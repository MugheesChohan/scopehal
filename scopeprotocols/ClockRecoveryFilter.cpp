/***********************************************************************************************************************
*                                                                                                                      *
* libscopeprotocols                                                                                                    *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg and contributors                                                         *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

#include "../scopehal/scopehal.h"
#include "scopeprotocols.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ClockRecoveryFilter::ClockRecoveryFilter(const string& color)
	: Filter(color, CAT_CLOCK)
{
	AddDigitalStream("data");
	CreateInput("IN");
	CreateInput("Gate");

	m_baudname = "Symbol rate";
	m_parameters[m_baudname] = FilterParameter(FilterParameter::TYPE_FLOAT, Unit(Unit::UNIT_HZ));
	m_parameters[m_baudname].SetFloatVal(1250000000);	//1.25 Gbps

	m_threshname = "Threshold";
	m_parameters[m_threshname] = FilterParameter(FilterParameter::TYPE_FLOAT, Unit(Unit::UNIT_VOLTS));
	m_parameters[m_threshname].SetFloatVal(0);
}

ClockRecoveryFilter::~ClockRecoveryFilter()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Factory methods

bool ClockRecoveryFilter::ValidateChannel(size_t i, StreamDescriptor stream)
{
	switch(i)
	{
		case 0:
			if(stream.m_channel == NULL)
				return false;
			return
				(stream.GetType() == Stream::STREAM_TYPE_ANALOG) ||
				(stream.GetType() == Stream::STREAM_TYPE_DIGITAL);

		case 1:
			if(stream.m_channel == NULL)	//null is legal for gate
				return true;

			return (stream.GetType() == Stream::STREAM_TYPE_DIGITAL);

		default:
			return false;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

string ClockRecoveryFilter::GetProtocolName()
{
	return "Clock Recovery (PLL)";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual decoder logic

void ClockRecoveryFilter::Refresh()
{
	//Require a data signal, but not necessarily a gate
	if(!VerifyInputOK(0))
	{
		SetData(NULL, 0);
		return;
	}

	auto din = GetInputWaveform(0);
	din->PrepareForCpuAccess();

	auto uadin = dynamic_cast<UniformAnalogWaveform*>(din);
	auto sadin = dynamic_cast<SparseAnalogWaveform*>(din);
	auto uddin = dynamic_cast<UniformDigitalWaveform*>(din);
	auto sddin = dynamic_cast<SparseDigitalWaveform*>(din);
	auto gate = GetInputWaveform(1);
	auto sgate = dynamic_cast<SparseDigitalWaveform*>(gate);
	auto ugate = dynamic_cast<UniformDigitalWaveform*>(gate);
	if(gate)
		gate->PrepareForCpuAccess();

	//Timestamps of the edges
	vector<int64_t> edges;
	if(uadin)
		FindZeroCrossings(uadin, m_parameters[m_threshname].GetFloatVal(), edges);
	else if(sadin)
		FindZeroCrossings(sadin, m_parameters[m_threshname].GetFloatVal(), edges);
	else if(uddin)
		FindZeroCrossings(uddin, edges);
	else if(sddin)
		FindZeroCrossings(sddin, edges);
	if(edges.empty())
	{
		SetData(NULL, 0);
		return;
	}

	//Get nominal period used for the first cycle of the NCO
	int64_t initialPeriod = round(FS_PER_SECOND / m_parameters[m_baudname].GetFloatVal());
	int64_t period = initialPeriod;

	//Disallow frequencies higher than Nyquist of the input
	auto fnyquist = 2*din->m_timescale;
	if( period < fnyquist)
	{
		SetData(NULL, 0);
		return;
	}

	//Create the output waveform and copy our timescales
	auto cap = SetupEmptySparseDigitalOutputWaveform(din, 0);
	cap->m_triggerPhase = 0;
	cap->m_timescale = 1;		//recovered clock time scale is single femtoseconds
	cap->PrepareForCpuAccess();

	int64_t tend;
	if(sadin || uadin)
		tend = GetOffsetScaled(sadin, uadin, din->size()-1);
	else
		tend = GetOffsetScaled(sddin, uddin, din->size()-1);

	//The actual PLL NCO
	//TODO: use the real fibre channel PLL.
	size_t nedge = 1;
	//LogDebug("n, delta, period, freq_ghz, cycles_open_loop\n");
	int64_t edgepos = edges[0];
	bool value = false;
	int64_t total_error = 0;
	cap->m_samples.reserve(edges.size());
	size_t igate = 0;

	//If gated at T=0, start with output stopped
	bool gating = false;
	if(gate && gate->size())
		gating = !GetValue(sgate, ugate, 0);

	int64_t tlast = 0;
	/*LogDebug("--START--\n");
	LogDebug("t,period,dphase,dperiod,uiLen\n");*/
	for(; (edgepos < tend) && (nedge < edges.size()-1); edgepos += period)
	{
		float center = period/2;

		//See if the current edge position is within a gating region
		bool was_gating = gating;
		if(gate != nullptr)
		{
			while(igate < edges.size()-1)
			{
				//See if this edge is within the region
				int64_t a = GetOffsetScaled(sgate, ugate, igate);
				int64_t b = a + GetDurationScaled(sgate, ugate, igate);

				//We went too far, stop
				if(edgepos < a)
					break;

				//Keep looking
				else if(edgepos > b)
					igate ++;

				//Good alignment
				else
				{
					gating = !GetValue(sgate, ugate, igate);
					break;
				}
			}
		}

		//See if the next edge occurred in this UI.
		//If not, just run the NCO open loop.
		//Allow multiple edges in the UI if the frequency is way off.
		int64_t tnext = edges[nedge];
		while( (tnext + center < edgepos) && (nedge+1 < edges.size()) )
		{
			if(!gating)
			{
				//Find phase error
				int64_t dphase = (edgepos - tnext) - period;
				total_error += fabs(dphase);

				//Find frequency error
				int64_t uiLen = (tnext - tlast);
				float numUIs = round(uiLen * 1.0 / initialPeriod);
				if(numUIs < 0.1)		//Sanity check: no correction if we have a glitch
					uiLen = period;
				else
					uiLen /= numUIs;
				int64_t dperiod = period - uiLen;

				/*LogDebug("%e, %.2f, %.2f, %.2f, %.2f\n",
					edgepos * SECONDS_PER_FS, period*1e-3f, dphase*1e-3f, dperiod*1e-3f, uiLen*1e-3f);*/

				//If the clock just got ungated, align exactly to the next edge
				if(was_gating)
					edgepos = tnext + period;

				else
				{
					//Proportional correction for frequency error
					period -= dperiod * 0.008;

					//Proportional correction for phase error
					period -= dphase * 0.002;
				}

				if(period < fnyquist)
				{
					LogWarning("PLL attempted to lock to frequency near or above Nyquist - invalid config or undersampled data?\n");
					nedge = edges.size();
					break;
				}
				if(period > 2*initialPeriod)
				{
					LogWarning("PLL attempted to go really slow, what's up? %s\n",
						Unit(Unit::UNIT_FS).PrettyPrint(period).c_str());
					nedge = edges.size();
					break;
				}
			}

			tlast = tnext;
			tnext = edges[++nedge];
		}

		//Add the sample (90 deg phase offset from the internal NCO)
		if(!gating)
		{
			value = !value;

			cap->m_offsets.push_back(edgepos + period/2);
			cap->m_durations.push_back(period);
			cap->m_samples.push_back(value);
		}
	}

	total_error /= edges.size();
	//LogTrace("average phase error %zu\n", total_error);

	SetData(cap, 0);

	cap->MarkModifiedFromCpu();
}
