#include "dsp.h"
#include "dsps_fft2r.h"
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int    g_sample_rate;
static int    g_fft_size;
static int    g_num_bins;       // = fft_size (full complex spectrum)
static float *g_fft_data;       // 2 * fft_size interleaved complex
static float *g_magnitude_db;   // fft_size elements (after fftshift)

// Double-buffer for I/Q input (lock-free SPSC)
// Each buffer holds fft_size frames × 2 floats (interleaved I/Q)
static float *g_input_buf[2];
static volatile int g_write_buf;
static int          g_write_pos;    // in frames (not floats)
static volatile int g_ready_buf;

// fs/4 down-conversion state (persists across buffers)
static int g_mix_phase;

static void compute_spectrum()
{
    dsps_fft2r_fc32(g_fft_data, g_fft_size);
    dsps_bit_rev_fc32(g_fft_data, g_fft_size);

    // Compute magnitude in dB with fftshift:
    // FFT output: bins [0..N/2-1] = DC to +fs/2, [N/2..N-1] = -fs/2 to DC
    // After shift: output[0] = -fs/2, output[N/2] = DC, output[N-1] ≈ +fs/2
    const float scale = 1.0f / g_fft_size;
    int half = g_fft_size / 2;
    for (int i = 0; i < g_fft_size; i++) {
        // fftshift: map output index i to FFT bin
        int bin = (i + half) % g_fft_size;
        float re = g_fft_data[2 * bin];
        float im = g_fft_data[2 * bin + 1];
        float mag = sqrtf(re * re + im * im) * scale;
        g_magnitude_db[i] = 20.0f * log10f(mag + 1e-10f);
    }
}

void dsp::init(int sample_rate, int fft_size)
{
    g_sample_rate  = sample_rate;
    g_fft_size     = fft_size;
    g_num_bins     = fft_size;  // full complex spectrum
    g_fft_data     = new float[2 * fft_size];
    g_magnitude_db = new float[fft_size];

    // Each input buffer holds fft_size I/Q frames = 2*fft_size floats
    g_input_buf[0] = new float[2 * fft_size];
    g_input_buf[1] = new float[2 * fft_size];
    memset(g_input_buf[0], 0, 2 * fft_size * sizeof(float));
    memset(g_input_buf[1], 0, 2 * fft_size * sizeof(float));
    g_write_buf = 0;
    g_write_pos = 0;
    g_ready_buf = -1;
    g_mix_phase = 0;

    dsps_fft2r_init_fc32(nullptr, fft_size);

    for (int i = 0; i < g_num_bins; i++)
        g_magnitude_db[i] = -120.0f;
}

void dsp::pushIQ(const float *iq, int num_frames)
{
    float *buf = g_input_buf[g_write_buf];

    for (int i = 0; i < num_frames; i++) {
        float I = iq[2 * i];
        float Q = iq[2 * i + 1];

        // fs/4 complex down-conversion: multiply by exp(-j*pi*n/2)
        // Phase 0: (I, Q) * 1     = ( I,  Q)
        // Phase 1: (I, Q) * (-j)  = ( Q, -I)
        // Phase 2: (I, Q) * (-1)  = (-I, -Q)
        // Phase 3: (I, Q) * (j)   = (-Q,  I)
        float oI, oQ;
        switch (g_mix_phase) {
            case 0: oI =  I; oQ =  Q; break;
            case 1: oI =  Q; oQ = -I; break;
            case 2: oI = -I; oQ = -Q; break;
            case 3: oI = -Q; oQ =  I; break;
        }
        g_mix_phase = (g_mix_phase + 1) & 3;

        int pos = g_write_pos;
        buf[2 * pos]     = oI;
        buf[2 * pos + 1] = oQ;
        g_write_pos = pos + 1;

        if (g_write_pos >= g_fft_size) {
            __sync_synchronize();
            g_ready_buf = g_write_buf;
            g_write_buf = 1 - g_write_buf;
            buf = g_input_buf[g_write_buf];
            g_write_pos = 0;
        }
    }
}

bool dsp::processIfReady()
{
    int rb = g_ready_buf;
    if (rb < 0) return false;
    g_ready_buf = -1;
    __sync_synchronize();

    // Copy I/Q data directly to FFT buffer (already interleaved complex)
    memcpy(g_fft_data, g_input_buf[rb], 2 * g_fft_size * sizeof(float));

    compute_spectrum();
    return true;
}

int dsp::getNumBins()              { return g_num_bins; }
const float* dsp::getMagnitudeDb() { return g_magnitude_db; }

float dsp::getBinFrequency(int bin)
{
    // After fftshift: bin 0 = -fs/2, bin N/2 = DC (0 Hz), bin N-1 ≈ +fs/2
    int half = g_fft_size / 2;
    return (float)(bin - half) * g_sample_rate / g_fft_size;
}
