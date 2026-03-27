#include "dsp.h"
#include "nco.h"
#include "dsps_fft2r.h"
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int    g_sample_rate;
static int    g_fft_size;
static int    g_num_bins;
static float *g_fft_data;
static float *g_magnitude_db;

// Double-buffer for I/Q input (lock-free SPSC)
static float *g_input_buf[2];
static volatile int g_write_buf;
static int          g_write_pos;
static volatile int g_ready_buf;

// fs/4 down-conversion mixer phase
static int g_mix_phase;

// CW offset NCO (shifts CW sidetone offset to DC after fs/4 mixer)
static NcoState g_cw_nco;
static bool g_cw_nco_active = false;

// --- FIR decimation: 48kHz → 6kHz ---
static constexpr int DECIM_FACTOR = 8;
static constexpr int FIR_TAPS = 65;
static constexpr float FIR_CUTOFF_HZ = 2400.0f; // anti-aliasing for 6kHz output

static float g_fir_coeffs[FIR_TAPS];
static float g_fir_delay_i[FIR_TAPS]; // delay line for I channel
static float g_fir_delay_q[FIR_TAPS]; // delay line for Q channel
static int   g_fir_pos = 0;           // circular delay line position
static int   g_decim_counter = 0;

// Audio output buffer
static constexpr int AUDIO_OUT_BUF_SIZE = 128;
static float g_audio_out_buf[AUDIO_OUT_BUF_SIZE];
static int   g_audio_out_pos = 0;
static dsp::AudioOutCallback g_audio_out_cb = nullptr;
static float g_audio_gain = 1000.0f;  // ~70 dB gain to bring noise floor to audible

// Design Hamming-windowed sinc lowpass FIR
static void design_fir_lowpass(float *h, int N, float cutoff_hz, float sample_rate)
{
    float fc = cutoff_hz / sample_rate;
    int M = (N - 1) / 2;
    for (int n = 0; n < N; n++) {
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * n / (N - 1)); // Hamming
        if (n == M) {
            h[n] = 2.0f * fc * w;
        } else {
            float x = (float)(n - M);
            h[n] = sinf(2.0f * (float)M_PI * fc * x) / ((float)M_PI * x) * w;
        }
    }
    // Normalize for unity gain at DC
    float sum = 0;
    for (int n = 0; n < N; n++) sum += h[n];
    if (sum > 0) for (int n = 0; n < N; n++) h[n] /= sum;
}

// Process one sample through FIR decimator, returns true if output is ready
static bool fir_decimate_sample(float in_i, float in_q, float &out_i, float &out_q)
{
    // Insert into circular delay line
    g_fir_delay_i[g_fir_pos] = in_i;
    g_fir_delay_q[g_fir_pos] = in_q;
    g_fir_pos = (g_fir_pos + 1) % FIR_TAPS;

    g_decim_counter++;
    if (g_decim_counter < DECIM_FACTOR)
        return false;
    g_decim_counter = 0;

    // Compute FIR output (dot product with delay line)
    float acc_i = 0, acc_q = 0;
    int idx = g_fir_pos; // oldest sample
    for (int k = 0; k < FIR_TAPS; k++) {
        acc_i += g_fir_delay_i[idx] * g_fir_coeffs[k];
        acc_q += g_fir_delay_q[idx] * g_fir_coeffs[k];
        idx = (idx + 1) % FIR_TAPS;
    }
    out_i = acc_i;
    out_q = acc_q;
    return true;
}

static void flush_audio_out()
{
    if (g_audio_out_pos > 0 && g_audio_out_cb) {
        g_audio_out_cb(g_audio_out_buf, g_audio_out_pos);
        g_audio_out_pos = 0;
    }
}

static void compute_spectrum()
{
    dsps_fft2r_fc32(g_fft_data, g_fft_size);
    dsps_bit_rev_fc32(g_fft_data, g_fft_size);

    const float scale = 1.0f / g_fft_size;
    int half = g_fft_size / 2;
    for (int i = 0; i < g_fft_size; i++) {
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
    g_num_bins     = fft_size;
    g_fft_data     = new float[2 * fft_size];
    g_magnitude_db = new float[fft_size];

    g_input_buf[0] = new float[2 * fft_size];
    g_input_buf[1] = new float[2 * fft_size];
    memset(g_input_buf[0], 0, 2 * fft_size * sizeof(float));
    memset(g_input_buf[1], 0, 2 * fft_size * sizeof(float));
    g_write_buf = 0;
    g_write_pos = 0;
    g_ready_buf = -1;
    g_mix_phase = 0;

    // Design anti-aliasing FIR for decimation
    design_fir_lowpass(g_fir_coeffs, FIR_TAPS, FIR_CUTOFF_HZ, (float)sample_rate);
    memset(g_fir_delay_i, 0, sizeof(g_fir_delay_i));
    memset(g_fir_delay_q, 0, sizeof(g_fir_delay_q));
    g_fir_pos = 0;
    g_decim_counter = 0;
    g_audio_out_pos = 0;

    // NCO init
    nco::init();
    g_cw_nco.phase = 0;
    g_cw_nco.inc = 0;
    g_cw_nco_active = false;

    dsps_fft2r_init_fc32(nullptr, fft_size);

    for (int i = 0; i < g_num_bins; i++)
        g_magnitude_db[i] = -120.0f;
}

void dsp::setAudioOutCallback(AudioOutCallback cb) { g_audio_out_cb = cb; }
int  dsp::getDecimatedRate() { return g_sample_rate / DECIM_FACTOR; }
void dsp::setAudioGain(float gain) { g_audio_gain = gain; }

void dsp::setCwOffset(float offset_hz)
{
    if (offset_hz == 0.0f) {
        g_cw_nco_active = false;
        g_cw_nco.inc = 0;
    } else {
        nco::setFreq(g_cw_nco, offset_hz, (float)g_sample_rate);
        g_cw_nco_active = true;
    }
}

void dsp::pushIQ(const float *iq, int num_frames)
{
    float *buf = g_input_buf[g_write_buf];

    for (int i = 0; i < num_frames; i++) {
        float I = iq[2 * i];
        float Q = iq[2 * i + 1];

        // fs/4 complex down-conversion
        float oI, oQ;
        switch (g_mix_phase) {
            case 0: oI =  I; oQ =  Q; break;
            case 1: oI =  Q; oQ = -I; break;
            case 2: oI = -I; oQ = -Q; break;
            case 3: oI = -Q; oQ =  I; break;
        }
        g_mix_phase = (g_mix_phase + 1) & 3;

        // CW offset NCO: shift CW sidetone offset to DC
        if (g_cw_nco_active) {
            float nI, nQ;
            nco::mixDown(g_cw_nco, oI, oQ, nI, nQ);
            oI = nI;
            oQ = nQ;
        }

        // Feed to FFT buffer (full 48kHz rate)
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

        // FIR decimate: 48kHz → 6kHz, output I stream to audio with gain
        float dec_i, dec_q;
        if (fir_decimate_sample(oI, oQ, dec_i, dec_q)) {
            float out = dec_i * g_audio_gain;
            // Soft clip to [-1, 1]
            if (out > 1.0f) out = 1.0f;
            if (out < -1.0f) out = -1.0f;
            g_audio_out_buf[g_audio_out_pos++] = out;
            if (g_audio_out_pos >= AUDIO_OUT_BUF_SIZE)
                flush_audio_out();
        }
    }
}

bool dsp::processIfReady()
{
    int rb = g_ready_buf;
    if (rb < 0) return false;
    g_ready_buf = -1;
    __sync_synchronize();

    memcpy(g_fft_data, g_input_buf[rb], 2 * g_fft_size * sizeof(float));
    compute_spectrum();
    return true;
}

int dsp::getNumBins()              { return g_num_bins; }
const float* dsp::getMagnitudeDb() { return g_magnitude_db; }

int dsp::displayBin(int display_idx)
{
    return (display_idx + 3 * g_fft_size / 4) % g_fft_size;
}
