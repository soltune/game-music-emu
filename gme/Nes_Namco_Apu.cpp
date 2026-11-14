// Nes_Snd_Emu 0.1.8. http://www.slack.net/~ant/

#include "Nes_Namco_Apu.h"

/* Copyright (C) 2003-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "blargg_source.h"
#include <string.h>

Nes_Namco_Apu::Nes_Namco_Apu()
{
	output( NULL );
	volume( 1.0 );
	reset();
}

void Nes_Namco_Apu::reset()
{
	last_time = 0;
	addr_reg = 0;
	active_delay = 0;
	current_osc = osc_count - 1;
	dac_amp = 0;
	dac_last_buf = NULL;

	int i;
	for ( i = 0; i < reg_count; i++ )
		reg [i] = 0;

#if NAMCO_DEZIPPER_WAVE
	memset( staged_wave, 0, sizeof(staged_wave) );
	wave_dirty = false;
	wave_burst_clocks = 0;
	wave_idle_clocks = 0;
#endif

	for ( i = 0; i < osc_count; i++ )
	{
		Namco_Osc& osc = oscs [i];
		osc.delay = 0;
		osc.last_amp = 0;
		osc.wave_pos = 0;
		osc.phase = 0;
		osc.sub_accum = 0;
		osc.last_period = 0;
#if NAMCO_DEZIPPER_WAVE && NAMCO_DEZIPPER_CYCLE_LATCH
		memset( osc.wave_snap, 0, sizeof(osc.wave_snap) );
		osc.wave_pending = false;
#endif
	}
}

void Nes_Namco_Apu::output( Blip_Buffer* buf )
{
	for ( int i = 0; i < osc_count; i++ )
		osc_output( i, buf );
}

/*
void Nes_Namco_Apu::reflect_state( Tagged_Data& data )
{
	reflect_int16( data, BLARGG_4CHAR('A','D','D','R'), &addr_reg );

	static const char hex [17] = "0123456789ABCDEF";
	int i;
	for ( i = 0; i < reg_count; i++ )
		reflect_int16( data, 'RG\0\0' + hex [i >> 4] * 0x100 + hex [i & 15], &reg [i] );

	for ( i = 0; i < osc_count; i++ )
	{
		reflect_int32( data, BLARGG_4CHAR('D','L','Y','0') + i, &oscs [i].delay );
		reflect_int16( data, BLARGG_4CHAR('P','O','S','0') + i, &oscs [i].wave_pos );
	}
}
*/

void Nes_Namco_Apu::end_frame( blip_time_t time )
{
	if ( time > last_time )
		run_until( time );

	assert( last_time >= time );
	last_time -= time;
}

#if !NAMCO_DIGITAL_MIXER || !NAMCO_SYNC_UPDATE || NAMCO_SYNC_MODE_TICK
static inline int advance_osc_phase( uint32_t& phase, int wave_size, uint32_t step )
{
	uint32_t wave_limit = (uint32_t)wave_size << 16;
	phase += step;
	if ( phase >= wave_limit )
		phase %= wave_limit;
	return (int)(phase >> 16);
}
#endif

void Nes_Namco_Apu::latch_osc_wave( int osc_idx )
{
#if NAMCO_DEZIPPER_WAVE && NAMCO_DEZIPPER_CYCLE_LATCH
	Namco_Osc& osc = oscs [osc_idx];
	if ( osc.wave_pending )
	{
		memcpy( osc.wave_snap, reg, sizeof(osc.wave_snap) );
		osc.wave_pending = false;
	}
#else
	(void)osc_idx;
#endif
}

void Nes_Namco_Apu::commit_staged_wave()
{
#if NAMCO_DEZIPPER_WAVE
	if ( !wave_dirty )
		return;
	memcpy( reg, staged_wave, sizeof(staged_wave) );
	wave_dirty = false;
	wave_burst_clocks = 0;
	wave_idle_clocks = 0;
	#if NAMCO_DEZIPPER_CYCLE_LATCH
	for ( int i = 0; i < osc_count; i++ )
		oscs [i].wave_pending = true;
	#endif
#endif
}

int Nes_Namco_Apu::read_wave_sample( int osc_idx, int wave_pos ) const
{
	const uint8_t* osc_reg = &reg [osc_idx * 8 + 0x40];
	int addr = (wave_pos + osc_reg [6]) & 0xFF;
#if NAMCO_DEZIPPER_WAVE && NAMCO_DEZIPPER_CYCLE_LATCH
	const uint8_t* wsrc = (addr < 0x80) ? oscs [osc_idx].wave_snap : reg;
	int sample = (wsrc [addr >> 1] >> ((addr & 1) << 2)) & 15;
#else
	int sample = (reg [addr >> 1] >> ((addr & 1) << 2)) & 15;
#endif
	return sample * (osc_reg [7] & 15);
}

void Nes_Namco_Apu::silence_osc( Namco_Osc& osc, blip_resampled_time_t res_time )
{
	if ( osc.last_amp )
	{
		if ( osc.output )
		{
			osc.output->set_modified();
			synth.offset_resampled( res_time, -osc.last_amp, osc.output );
		}
		osc.last_amp = 0;
	}
}

void Nes_Namco_Apu::silence_cpu_osc( Namco_Osc& osc, blip_time_t cpu_time )
{
	if ( osc.last_amp )
	{
		if ( osc.output )
		{
			osc.output->set_modified();
			synth.offset( cpu_time, -osc.last_amp, osc.output );
		}
		osc.last_amp = 0;
	}
}

void Nes_Namco_Apu::run_until( blip_time_t nes_end_time )
{
	if ( nes_end_time <= last_time )
		return;

#if NAMCO_DEZIPPER_WAVE
	if ( wave_dirty )
	{
		int dt = nes_end_time - last_time;
		int rem_idle = 1800 - wave_idle_clocks;
		int rem_max = 4500 - wave_burst_clocks;
		int rem = (rem_idle < rem_max) ? rem_idle : rem_max;
		if ( rem <= 0 )
		{
			commit_staged_wave();
		}
		else if ( rem < dt )
		{
			render_span( last_time + rem );
			commit_staged_wave();
		}
		else
		{
			wave_idle_clocks += dt;
			wave_burst_clocks += dt;
		}
	}
#endif

	render_span( nes_end_time );
}

void Nes_Namco_Apu::render_span( blip_time_t nes_end_time )
{
	if ( nes_end_time <= last_time )
		return;

	int active_oscs = (reg [0x7F] >> 4 & 7) + 1;
	int first_active = osc_count - active_oscs;

#if !NAMCO_DIGITAL_MIXER || !NAMCO_SYNC_UPDATE || NAMCO_SYNC_MODE_TICK
	for ( int i = 0; i < first_active; i++ )
	{
		silence_cpu_osc( oscs [i], last_time );
		latch_osc_wave( i );
	}

	blip_time_t t = last_time + active_delay;

#if !NAMCO_DIGITAL_MIXER
	// Hardware single-DAC 15-CPU-clock round-robin multiplexing
	Blip_Buffer* shared_buf = NULL;
	bool multi_buf = false;
	for ( int i = first_active; i < osc_count; i++ )
	{
		Blip_Buffer* b = oscs [i].output;
		if ( b )
		{
			if ( !shared_buf )
				shared_buf = b;
			else if ( shared_buf != b )
				multi_buf = true;
		}
	}
	if ( !shared_buf && dac_amp && dac_last_buf )
	{
		dac_last_buf->set_modified();
		synth.offset( last_time, -dac_amp, dac_last_buf );
		dac_amp = 0;
		dac_last_buf = NULL;
	}

	while ( t < nes_end_time )
	{
		if ( current_osc < first_active || current_osc >= osc_count )
			current_osc = osc_count - 1;

		const uint8_t* osc_reg = &reg [current_osc * 8 + 0x40];
		Namco_Osc& osc = oscs [current_osc];
		int wave_size = 256 - (osc_reg [4] & 0xFC);
		uint32_t freq = ((uint32_t)(osc_reg [4] & 3) << 16)
				| ((uint32_t)osc_reg [2] << 8)
				| osc_reg [0];

		int prev_pos = osc.wave_pos;
		int next_pos = advance_osc_phase( osc.phase, wave_size, freq );
		osc.wave_pos = (short)next_pos;
		if ( next_pos < prev_pos || !(osc_reg [7] & 15) )
			latch_osc_wave( current_osc );

		int sample = osc.output ? read_wave_sample( current_osc, osc.wave_pos ) : 0;

		if ( !multi_buf )
		{
			if ( shared_buf )
			{
				int delta = sample - dac_amp;
				if ( delta )
				{
					dac_amp = (short)sample;
					shared_buf->set_modified();
					synth.offset( t, delta, shared_buf );
				}
				dac_last_buf = shared_buf;
			}
		}
		else
		{
			for ( int i = first_active; i < osc_count; i++ )
			{
				int target = (i == current_osc) ? sample : 0;
				Namco_Osc& ch = oscs [i];
				if ( ch.output )
				{
					int delta = target - ch.last_amp;
					if ( delta )
					{
						ch.last_amp = (short)target;
						ch.output->set_modified();
						synth.offset( t, delta, ch.output );
					}
				}
			}
		}

		current_osc--;
		t += 15;
	}

#elif !NAMCO_SYNC_UPDATE
	// Fix 1 only: Digital mixer with 15-CPU-clock serial round-robin updates
	while ( t < nes_end_time )
	{
		if ( current_osc < first_active || current_osc >= osc_count )
			current_osc = osc_count - 1;

		const uint8_t* osc_reg = &reg [current_osc * 8 + 0x40];
		Namco_Osc& osc = oscs [current_osc];
		int wave_size = 256 - (osc_reg [4] & 0xFC);
		uint32_t freq = ((uint32_t)(osc_reg [4] & 3) << 16)
				| ((uint32_t)osc_reg [2] << 8)
				| osc_reg [0];

		int prev_pos = osc.wave_pos;
		int next_pos = advance_osc_phase( osc.phase, wave_size, freq );
		osc.wave_pos = (short)next_pos;
		if ( next_pos < prev_pos || osc.last_amp == 0 || !(osc_reg [7] & 15) )
			latch_osc_wave( current_osc );

		if ( osc.output )
		{
			int sample = read_wave_sample( current_osc, osc.wave_pos );
			int delta = sample - osc.last_amp;
			if ( delta )
			{
				osc.last_amp = (short)sample;
				osc.output->set_modified();
				synth.offset( t, delta, osc.output );
			}
		}

		current_osc--;
		t += 15;
	}

#else
	// Fix 1 + Fix 2 (Option A): Digital mixer with synchronized 15-CPU-clock stepping
	while ( t < nes_end_time )
	{
		for ( int i = first_active; i < osc_count; i++ )
		{
			const uint8_t* osc_reg = &reg [i * 8 + 0x40];
			Namco_Osc& osc = oscs [i];
			int wave_size = 256 - (osc_reg [4] & 0xFC);
			uint32_t freq = ((uint32_t)(osc_reg [4] & 3) << 16)
					| ((uint32_t)osc_reg [2] << 8)
					| osc_reg [0];

			osc.sub_accum += freq;
			uint32_t step = osc.sub_accum / (uint32_t)active_oscs;
			osc.sub_accum %= (uint32_t)active_oscs;

			int prev_pos = osc.wave_pos;
			int next_pos = advance_osc_phase( osc.phase, wave_size, step );
			osc.wave_pos = (short)next_pos;
			if ( next_pos < prev_pos || osc.last_amp == 0 || !(osc_reg [7] & 15) )
				latch_osc_wave( i );

			if ( osc.output )
			{
				int sample = read_wave_sample( i, osc.wave_pos );
				int delta = sample - osc.last_amp;
				if ( delta )
				{
					osc.last_amp = (short)sample;
					osc.output->set_modified();
					synth.offset( t, delta, osc.output );
				}
			}
		}
		t += 15;
	}
#endif

	active_delay = t - nes_end_time;

#else
	// Fix 1 + Fix 2 (Option B): Digital mixer with band-limited transition synthesis
	for ( int i = 0; i < first_active; i++ )
	{
		Namco_Osc& osc = oscs [i];
		if ( osc.output )
			silence_osc( osc, osc.output->resampled_time( last_time ) );
		osc.delay = 0;
		osc.last_period = 0;
		latch_osc_wave( i );
	}

	for ( int i = first_active; i < osc_count; i++ )
	{
		Namco_Osc& osc = oscs [i];
		Blip_Buffer* output = osc.output;
		if ( !output )
		{
			latch_osc_wave( i );
			continue;
		}
		output->set_modified();

		blip_resampled_time_t start_time = output->resampled_time( last_time );
		blip_resampled_time_t end_time = output->resampled_time( nes_end_time );

		const uint8_t* osc_reg = &reg [i * 8 + 0x40];
		int volume = osc_reg [7] & 15;
		uint32_t freq = ((uint32_t)(osc_reg [4] & 3) << 16)
				| ((uint32_t)osc_reg [2] << 8)
				| osc_reg [0];

		if ( !volume || freq < (uint32_t)(4 * active_oscs) )
		{
			silence_osc( osc, start_time );
			osc.delay = 0;
			osc.last_period = 0;
			latch_osc_wave( i );
			continue;
		}

		blip_resampled_time_t period = (blip_resampled_time_t)(
				((uint64_t)output->resampled_duration( 983040 ) * (uint64_t)active_oscs) / freq );
		if ( !period )
			period = 1;

		if ( osc.last_period && period != osc.last_period && osc.delay > 0 )
			osc.delay = (int32_t)( (uint64_t)osc.delay * period / osc.last_period );
		osc.last_period = period;

		blip_resampled_time_t time = start_time + osc.delay;
		osc.delay = 0;

		if ( time < end_time )
		{
			int wave_size = 256 - (osc_reg [4] & 0xFC);
			int last_amp = osc.last_amp;
			int wave_pos = osc.wave_pos;
			if ( wave_pos >= wave_size )
				wave_pos %= wave_size;

			do
			{
				if ( wave_pos == 0 || last_amp == 0 )
					latch_osc_wave( i );

				int sample = read_wave_sample( i, wave_pos );
				wave_pos++;
				if ( wave_pos >= wave_size )
					wave_pos = 0;

				int delta = sample - last_amp;
				if ( delta )
				{
					last_amp = sample;
					synth.offset_resampled( time, delta, output );
				}

				time += period;
			}
			while ( time < end_time );

			osc.wave_pos = (short)wave_pos;
			osc.last_amp = (short)last_amp;
		}
		osc.delay = (int32_t)(time - end_time);
	}
#endif

	last_time = nes_end_time;
}

