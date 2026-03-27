// nco.h — Single-tone f32 NCO for complex mixing
// Adapted from dds_two_tone_q15 (64-bit phase accumulator, quarter-wave LUT + LERP)
#pragma once
#include <cstdint>

struct NcoState {
    uint64_t phase;
    uint64_t inc;
};

namespace nco {

// Initialize quarter-wave sine LUT (call once at startup)
void init();

// Set NCO frequency. Phase is continuous across frequency changes.
void setFreq(NcoState &s, float freq_hz, float sample_rate);

// Complex mix: multiply (in_i + j*in_q) by exp(-j*2π*f*n/fs)
// Shifts frequency DOWN by freq_hz. Advances phase by one sample.
void mixDown(NcoState &s, float in_i, float in_q, float &out_i, float &out_q);

// Get sin/cos pair at current phase (for external use). Advances phase.
void sincos(NcoState &s, float &sin_val, float &cos_val);

} // namespace nco
