// nco.cpp — Single-tone f32 NCO
// Adapted from dds_two_tone_q15: 64-bit phase accumulator, quarter-wave LUT + LERP
// Output is float [-1, 1] for direct use in f32 DSP pipeline.

#include "nco.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Quarter-wave LUT configuration
static constexpr unsigned QTABLE_BITS = 8;   // 256 entries per quarter
static constexpr unsigned FRAC_BITS   = 10;  // interpolation fractional bits
static constexpr unsigned QTABLE_SIZE = 1u << QTABLE_BITS;
static constexpr unsigned VISIBLE_BITS = 2 + QTABLE_BITS + FRAC_BITS; // 20 bits

// Quarter-wave sine LUT stored as float [0, 1]
static float g_sin_quarter[QTABLE_SIZE + 1];

void nco::init()
{
    const double step = (0.5 * M_PI) / (double)QTABLE_SIZE;
    for (unsigned n = 0; n <= QTABLE_SIZE; n++) {
        g_sin_quarter[n] = (float)sin(step * (double)n);
    }
}

void nco::setFreq(NcoState &s, float freq_hz, float sample_rate)
{
    // phase_inc = round((f / fs) * 2^64)
    long double num = (long double)freq_hz * (long double)(1ULL << 63) * 2.0L;
    long double den = (long double)sample_rate;
    s.inc = (uint64_t)llroundl(num / den);
}

// Quarter-wave LUT + LERP: returns sin(phase) as float [-1, 1]
static inline float sin_from_phase(uint64_t phase)
{
    uint32_t v = (uint32_t)(phase >> (64 - VISIBLE_BITS));

    uint32_t quad = v >> (QTABLE_BITS + FRAC_BITS);         // 0..3
    uint32_t xf   = v & ((1u << (QTABLE_BITS + FRAC_BITS)) - 1u);

    uint32_t idx  = xf >> FRAC_BITS;
    uint32_t frac = xf & ((1u << FRAC_BITS) - 1u);

    // Mirror in odd quadrants (Q1, Q3)
    if (quad & 1u) {
        idx  = QTABLE_SIZE - ((xf + ((1u << FRAC_BITS) - 1u)) >> FRAC_BITS);
        frac = (-frac) & ((1u << FRAC_BITS) - 1u);
    }

    // Linear interpolation
    float y;
    if (idx >= QTABLE_SIZE) {
        y = g_sin_quarter[QTABLE_SIZE];
    } else {
        float y0 = g_sin_quarter[idx];
        float y1 = g_sin_quarter[idx + 1];
        float t  = (float)frac / (float)(1u << FRAC_BITS);
        y = y0 + (y1 - y0) * t;
    }

    // Negate for Q2/Q3
    if (quad >= 2u) y = -y;
    return y;
}

void nco::mixDown(NcoState &s, float in_i, float in_q, float &out_i, float &out_q)
{
    // Get sin/cos at current phase
    // cos(phase) = sin(phase + pi/2) = sin(phase + 2^62)
    float sin_val = sin_from_phase(s.phase);
    float cos_val = sin_from_phase(s.phase + (1ULL << 62)); // +pi/2

    // Complex multiply by exp(-j*phase): (I + jQ) * (cos - j*sin)
    //   out_I = I*cos + Q*sin
    //   out_Q = Q*cos - I*sin
    out_i = in_i * cos_val + in_q * sin_val;
    out_q = in_q * cos_val - in_i * sin_val;

    // Advance phase
    s.phase += s.inc;
}

void nco::sincos(NcoState &s, float &sin_val, float &cos_val)
{
    sin_val = sin_from_phase(s.phase);
    cos_val = sin_from_phase(s.phase + (1ULL << 62));
    s.phase += s.inc;
}
