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
static float  g_phase;          // sine phase accumulator for test tone

// Double-buffer for external sample input (lock-free SPSC)
static float *g_input_buf[2];
static volatile int g_write_buf;    // buffer index being written to (0 or 1)
static int          g_write_pos;    // write position in current buffer
static volatile int g_ready_buf;    // buffer index with complete data (-1 = none)

static void compute_fft()
{
    dsps_fft2r_fc32(g_fft_data, g_fft_size);
    dsps_bit_rev_fc32(g_fft_data, g_fft_size);

    const float scale = 1.0f / g_fft_size;
    for (int i = 0; i < g_num_bins; i++) {
        float re = g_fft_data[2 * i];
        float im = g_fft_data[2 * i + 1];
        float mag = sqrtf(re * re + im * im) * scale;
        g_magnitude_db[i] = 20.0f * log10f(mag + 1e-10f);
    }
}

void dsp::init(int sample_rate, int fft_size)
{
    g_sample_rate  = sample_rate;
    g_fft_size     = fft_size;
    g_num_bins     = fft_size / 2 + 1;
    g_fft_data     = new float[2 * fft_size];
    g_magnitude_db = new float[g_num_bins];
    g_phase        = 0.0f;

    g_input_buf[0] = new float[fft_size];
    g_input_buf[1] = new float[fft_size];
    g_write_buf    = 0;
    g_write_pos    = 0;
    g_ready_buf    = -1;

    dsps_fft2r_init_fc32(nullptr, fft_size);

    for (int i = 0; i < g_num_bins; i++)
        g_magnitude_db[i] = -120.0f;
}

void dsp::pushSamples(const float *samples, int count)
{
    for (int i = 0; i < count; i++) {
        g_input_buf[g_write_buf][g_write_pos++] = samples[i];
        if (g_write_pos >= g_fft_size) {
            g_ready_buf = g_write_buf;
            g_write_buf = 1 - g_write_buf;
            g_write_pos = 0;
        }
    }
}

bool dsp::processIfReady()
{
    int rb = g_ready_buf;
    if (rb < 0) return false;
    g_ready_buf = -1;

    // Pack into interleaved complex format
    const float *src = g_input_buf[rb];
    for (int i = 0; i < g_fft_size; i++) {
        g_fft_data[2 * i]     = src[i];
        g_fft_data[2 * i + 1] = 0.0f;
    }

    compute_fft();
    return true;
}

void dsp::processTestTone()
{
    const float freq = 1500.0f;
    const float phase_inc = 2.0f * (float)M_PI * freq / g_sample_rate;

    for (int i = 0; i < g_fft_size; i++) {
        g_fft_data[2 * i]     = sinf(g_phase);
        g_fft_data[2 * i + 1] = 0.0f;
        g_phase += phase_inc;
    }
    g_phase = fmodf(g_phase, 2.0f * (float)M_PI);

    compute_fft();
}

int dsp::getNumBins()             { return g_num_bins; }
const float* dsp::getMagnitudeDb(){ return g_magnitude_db; }
float dsp::getBinFrequency(int bin) { return (float)bin * g_sample_rate / g_fft_size; }
