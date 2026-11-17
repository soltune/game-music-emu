// Namco 106 sound chip emulator

// Nes_Snd_Emu 0.1.8
#ifndef NES_NAMCO_APU_H
#define NES_NAMCO_APU_H

#include "blargg_common.h"
#include "Blip_Buffer.h"

#ifndef NAMCO_DIGITAL_MIXER
	#define NAMCO_DIGITAL_MIXER 1
#endif
#ifndef NAMCO_SYNC_UPDATE
	#define NAMCO_SYNC_UPDATE 1
#endif
#ifndef NAMCO_SYNC_MODE_TICK
	#define NAMCO_SYNC_MODE_TICK 0
#endif
#ifndef NAMCO_DEZIPPER_WAVE
	#define NAMCO_DEZIPPER_WAVE 1
#endif
#ifndef NAMCO_DEZIPPER_CYCLE_LATCH
	#define NAMCO_DEZIPPER_CYCLE_LATCH 0
#endif
#ifndef NAMCO_VOLUME_SCALE
	#define NAMCO_VOLUME_SCALE 0.55
#endif

struct namco_state_t;

class Nes_Namco_Apu {
public:
	// See Nes_Apu.h for reference.
	void volume( double );
	void treble_eq( const blip_eq_t& );
	void output( Blip_Buffer* );
	static const int osc_count = 8;
	void osc_output( int index, Blip_Buffer* );
	void reset();
	void end_frame( blip_time_t );

	// Read/write data register is at 0x4800
	static const unsigned int data_reg_addr = 0x4800;
	void write_data( blip_time_t, int );
	int read_data();

	// Write-only address register is at 0xF800
	static const unsigned int addr_reg_addr = 0xF800;
	void write_addr( int );

	// Read the wave length register the way older emulators did, for NSFs written
	// against that behavior (see wave_length). Kept across reset().
	void legacy_wave_length( bool b ) { legacy_length = b; }

	// to do: implement save/restore
	void save_state( namco_state_t* out ) const;
	void load_state( namco_state_t const& );

public:
	Nes_Namco_Apu();
	BLARGG_DISABLE_NOTHROW
private:
	// noncopyable
	Nes_Namco_Apu( const Nes_Namco_Apu& );
	Nes_Namco_Apu& operator = ( const Nes_Namco_Apu& );

	struct Namco_Osc {
		int32_t delay;
		Blip_Buffer* output;
		short last_amp;
		short wave_pos;
		uint32_t phase;
		uint32_t sub_accum;
		blip_resampled_time_t last_period;
#if NAMCO_DEZIPPER_WAVE && NAMCO_DEZIPPER_CYCLE_LATCH
		uint8_t wave_snap [0x40];
		bool wave_pending;
#endif
	};

	Namco_Osc oscs [osc_count];

	blip_time_t last_time;
	int addr_reg;
	bool legacy_length;
	int active_delay;
	int current_osc;
	short dac_amp;
	Blip_Buffer* dac_last_buf;

	static const int reg_count = 0x80;
	uint8_t reg [reg_count];
#if NAMCO_DEZIPPER_WAVE
	uint8_t staged_wave [0x40];
	bool wave_dirty;
	int wave_burst_clocks;
	int wave_idle_clocks;
#endif
	Blip_Synth<blip_good_quality,15> synth;

	uint8_t& access();
	void run_until( blip_time_t );
	void render_span( blip_time_t );
	void commit_staged_wave();
	void latch_osc_wave( int osc_idx );
	void silence_osc( Namco_Osc&, blip_resampled_time_t );
	void silence_cpu_osc( Namco_Osc&, blip_time_t );
	int read_wave_sample( int osc_idx, int wave_pos ) const;
	int wave_length( const uint8_t* osc_reg ) const;
};
/*
struct namco_state_t
{
	uint8_t regs [0x80];
	uint8_t addr;
	uint8_t unused;
	uint8_t positions [8];
	uint32_t delays [8];
};
*/

inline uint8_t& Nes_Namco_Apu::access()
{
	int addr = addr_reg & 0x7F;
	if ( addr_reg & 0x80 )
		addr_reg = (addr + 1) | 0x80;
	return reg [addr];
}

// Wave length in samples, or 0 when the channel is muted.
// The length field is 6 bits: 256 - (reg & 0xFC) samples (4..256), as on hardware.
// Older emulators instead took only bits 2-4 as the length (32 - n * 4 samples)
// and muted the channel when bits 5-7 were zero. NSFs written against that
// behavior store short waves close together, so with the 6-bit length their
// waves become far longer, run over other waves and into the channel registers,
// and play as noise. Those keep the old reading.
inline int Nes_Namco_Apu::wave_length( const uint8_t* osc_reg ) const
{
	if ( legacy_length )
		return (osc_reg [4] & 0xE0) ? 32 - (osc_reg [4] >> 2 & 7) * 4 : 0;
	return 256 - (osc_reg [4] & 0xFC);
}

inline void Nes_Namco_Apu::volume( double v ) { synth.volume( (0.10 * NAMCO_VOLUME_SCALE) / osc_count * v ); }

inline void Nes_Namco_Apu::treble_eq( const blip_eq_t& eq ) { synth.treble_eq( eq ); }

inline void Nes_Namco_Apu::write_addr( int v ) { addr_reg = v; }

inline int Nes_Namco_Apu::read_data()
{
#if NAMCO_DEZIPPER_WAVE
	int addr = addr_reg & 0x7F;
	if ( addr_reg & 0x80 )
		addr_reg = (addr + 1) | 0x80;
	if ( addr < 0x40 )
		return staged_wave [addr];
	return reg [addr];
#else
	return access();
#endif
}

inline void Nes_Namco_Apu::osc_output( int i, Blip_Buffer* buf )
{
	assert( (unsigned) i < osc_count );
	oscs [i].output = buf;
}

inline void Nes_Namco_Apu::write_data( blip_time_t time, int data )
{
	run_until( time );
#if NAMCO_DEZIPPER_WAVE
	int addr = addr_reg & 0x7F;
	bool auto_inc = (addr_reg & 0x80) != 0;
	if ( auto_inc )
		addr_reg = (addr + 1) | 0x80;
	if ( addr < 0x40 )
	{
		staged_wave [addr] = (uint8_t)data;
		if ( auto_inc )
		{
			if ( !wave_dirty )
			{
				wave_dirty = true;
				wave_burst_clocks = 0;
			}
			wave_idle_clocks = 0;
			return;
		}
		reg [addr] = (uint8_t)data;
		#if NAMCO_DEZIPPER_CYCLE_LATCH
		for ( int i = 0; i < osc_count; i++ )
			oscs [i].wave_snap [addr] = (uint8_t)data;
		#endif
		return;
	}
	reg [addr] = (uint8_t)data;
#else
	access() = data;
#endif
}

#endif
