#include "dsp.h"
#include "dsps_fft2r.h"
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int    g_sample_rate;
static int    g_fft_size;
static int    g_num_bins;
static float *g_fft_data;       // 2 * fft_size interleaved complex
static float *g_magnitude_db;   // fft_size/2 + 1
static float  g_phase;          // sine phase accumulator

void dsp::init(int sample_rate, int fft_size)
{
    g_sample_rate = sample_rate;
    g_fft_size    = fft_size;
    g_num_bins    = fft_size / 2 + 1;
    g_fft_data    = new float[2 * fft_size];
    g_magnitude_db = new float[g_num_bins];
    g_phase       = 0.0f;

    dsps_fft2r_init_fc32(nullptr, fft_size);

    // Zero out magnitude
    for (int i = 0; i < g_num_bins; i++)
        g_magnitude_db[i] = -120.0f;
}

void dsp::process_frame()
{
    // Synthesize 1.5 kHz full-scale tone (24-bit precision irrelevant for float)
    const float freq = 1500.0f;
    const float phase_inc = 2.0f * (float)M_PI * freq / g_sample_rate;

    for (int i = 0; i < g_fft_size; i++) {
        g_fft_data[2 * i]     = sinf(g_phase);   // real
        g_fft_data[2 * i + 1] = 0.0f;            // imaginary
        g_phase += phase_inc;
    }
    // Keep phase bounded
    g_phase = fmodf(g_phase, 2.0f * (float)M_PI);

    // FFT (in-place)
    dsps_fft2r_fc32(g_fft_data, g_fft_size);
    dsps_bit_rev_fc32(g_fft_data, g_fft_size);

    // Compute magnitude in dB
    const float scale = 1.0f / g_fft_size;
    for (int i = 0; i < g_num_bins; i++) {
        float re = g_fft_data[2 * i];
        float im = g_fft_data[2 * i + 1];
        float mag = sqrtf(re * re + im * im) * scale;
        // Full-scale sine has peak at N/2, so 0 dB reference = 0.5 (half the FFT size)
        // Use 20*log10 with floor to avoid -inf
        g_magnitude_db[i] = 20.0f * log10f(mag + 1e-10f);
    }
}

int dsp::getNumBins()
{
    return g_num_bins;
}

const float* dsp::getMagnitudeDb()
{
    return g_magnitude_db;
}

float dsp::getBinFrequency(int bin)
{
    return (float)bin * g_sample_rate / g_fft_size;
}
