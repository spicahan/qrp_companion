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
static float g_cw_offset_hz = 0;

// CW audio filter (300Hz complex LPF at decimated rate) + sidetone NCO
static constexpr int CW_FIR_TAPS = 65;
static constexpr float CW_FIR_CUTOFF_HZ = 150.0f; // ±150Hz = 300Hz BW
static float g_cw_fir_coeffs[CW_FIR_TAPS];
static float g_cw_fir_delay_i[CW_FIR_TAPS];
static float g_cw_fir_delay_q[CW_FIR_TAPS];
static int   g_cw_fir_pos = 0;
static NcoState g_sidetone_nco;

// --- FIR decimation stages ---
static constexpr int FIR_TAPS = 65;

// Span configurations: decim factor, FIR cutoff, label
struct SpanDef {
    int decim;
    float cutoff_hz;
    const char *label;
    int display_rotation;  // displayBin rotation (in bins, relative to N)
};
static const SpanDef g_spans[dsp::NUM_SPANS] = {
    { 1,     0, "48k",  0 },   // no extra decimation, use 48kHz stream directly
    { 2, 10000, "+/-12k", 0 },
    { 4,  5000, "+/-6k",  0 },
    { 8,  2400, "+/-3k",  0 },
};
static int g_cur_span = 0;

// Per-stage FIR decimator state (stages for ↓2, ↓4, ↓8)
// Index 0=↓2, 1=↓4, 2=↓8. Stage 0 (48k) has no FIR.
static constexpr int NUM_DECIM_STAGES = 3;
struct FirDecimState {
    float coeffs[FIR_TAPS];
    float delay_i[FIR_TAPS];
    float delay_q[FIR_TAPS];
    int pos;
    int counter;
    int decim;
};
static FirDecimState g_decim[NUM_DECIM_STAGES];

// Audio output uses the ↓8 stage (index 2) regardless of display span
static constexpr int AUDIO_DECIM_IDX = 2;

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

// Process one sample through a FIR decimator stage, returns true if output ready
static bool fir_decimate_sample(FirDecimState &s, float in_i, float in_q,
                                float &out_i, float &out_q)
{
    s.delay_i[s.pos] = in_i;
    s.delay_q[s.pos] = in_q;
    s.pos = (s.pos + 1) % FIR_TAPS;

    s.counter++;
    if (s.counter < s.decim) return false;
    s.counter = 0;

    float acc_i = 0, acc_q = 0;
    int idx = s.pos;
    for (int k = 0; k < FIR_TAPS; k++) {
        acc_i += s.delay_i[idx] * s.coeffs[k];
        acc_q += s.delay_q[idx] * s.coeffs[k];
        idx = (idx + 1) % FIR_TAPS;
    }
    out_i = acc_i;
    out_q = acc_q;
    return true;
}

// Apply CW complex FIR to one I/Q sample (runs at decimated 6kHz rate)
static void cw_fir_sample(float in_i, float in_q, float &out_i, float &out_q)
{
    g_cw_fir_delay_i[g_cw_fir_pos] = in_i;
    g_cw_fir_delay_q[g_cw_fir_pos] = in_q;
    g_cw_fir_pos = (g_cw_fir_pos + 1) % CW_FIR_TAPS;

    float acc_i = 0, acc_q = 0;
    int idx = g_cw_fir_pos;
    for (int k = 0; k < CW_FIR_TAPS; k++) {
        acc_i += g_cw_fir_delay_i[idx] * g_cw_fir_coeffs[k];
        acc_q += g_cw_fir_delay_q[idx] * g_cw_fir_coeffs[k];
        idx = (idx + 1) % CW_FIR_TAPS;
    }
    out_i = acc_i;
    out_q = acc_q;
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

    // Initialize FIR decimation stages: ↓2, ↓4, ↓8
    int decim_factors[NUM_DECIM_STAGES] = {2, 4, 8};
    float cutoffs[NUM_DECIM_STAGES] = {10000.0f, 5000.0f, 2400.0f};
    for (int i = 0; i < NUM_DECIM_STAGES; i++) {
        g_decim[i].decim = decim_factors[i];
        g_decim[i].pos = 0;
        g_decim[i].counter = 0;
        memset(g_decim[i].delay_i, 0, sizeof(g_decim[i].delay_i));
        memset(g_decim[i].delay_q, 0, sizeof(g_decim[i].delay_q));
        design_fir_lowpass(g_decim[i].coeffs, FIR_TAPS, cutoffs[i], (float)sample_rate);
    }
    g_cur_span = 0;
    g_audio_out_pos = 0;

    // NCO init
    nco::init();
    g_cw_nco.phase = 0;
    g_cw_nco.inc = 0;
    g_cw_nco_active = false;
    g_cw_offset_hz = 0;

    // CW audio filter (runs at decimated rate)
    float dec_rate = (float)sample_rate / 8;
    design_fir_lowpass(g_cw_fir_coeffs, CW_FIR_TAPS, CW_FIR_CUTOFF_HZ, dec_rate);
    memset(g_cw_fir_delay_i, 0, sizeof(g_cw_fir_delay_i));
    memset(g_cw_fir_delay_q, 0, sizeof(g_cw_fir_delay_q));
    g_cw_fir_pos = 0;

    // Sidetone NCO (mix UP at decimated rate — set when CW offset is known)
    g_sidetone_nco.phase = 0;
    g_sidetone_nco.inc = 0;

    dsps_fft2r_init_fc32(nullptr, fft_size);

    for (int i = 0; i < g_num_bins; i++)
        g_magnitude_db[i] = -120.0f;
}

void dsp::setAudioOutCallback(AudioOutCallback cb) { g_audio_out_cb = cb; }
int  dsp::getDecimatedRate() { return g_sample_rate / 8; }  // audio always at ↓8
void dsp::setAudioGain(float gain) { g_audio_gain = gain; }

void dsp::setCwOffset(float offset_hz)
{
    if (offset_hz == 0.0f) {
        g_cw_nco_active = false;
        g_cw_nco.inc = 0;
        g_sidetone_nco.inc = 0;
        g_cw_offset_hz = 0;
    } else {
        // Down-conversion NCO at 48kHz rate
        nco::setFreq(g_cw_nco, offset_hz, (float)g_sample_rate);
        // Sidetone NCO at decimated rate (mix UP to create audible tone)
        float dec_rate = (float)g_sample_rate / 8;
        nco::setFreq(g_sidetone_nco, offset_hz, dec_rate);
        g_cw_offset_hz = offset_hz;
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

        // Feed to FFT buffer — either direct (48k) or via decimator
        if (g_cur_span == 0) {
            // 48kHz span: feed every sample directly
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

        // Run all decimation stages (each independently from 48kHz)
        for (int si = 0; si < NUM_DECIM_STAGES; si++) {
            float di, dq;
            if (fir_decimate_sample(g_decim[si], oI, oQ, di, dq)) {
                // Feed to FFT if this is the selected span's decimator
                // span 1=↓2(idx 0), span 2=↓4(idx 1), span 3=↓8(idx 2)
                if (g_cur_span == si + 1) {
                    int pos = g_write_pos;
                    buf[2 * pos]     = di;
                    buf[2 * pos + 1] = dq;
                    g_write_pos = pos + 1;
                    if (g_write_pos >= g_fft_size) {
                        __sync_synchronize();
                        g_ready_buf = g_write_buf;
                        g_write_buf = 1 - g_write_buf;
                        buf = g_input_buf[g_write_buf];
                        g_write_pos = 0;
                    }
                }

                // ↓8 stage (idx 2) always feeds audio output
                if (si == AUDIO_DECIM_IDX) {
                    float audio;
                    if (g_cw_nco_active) {
                        float filt_i, filt_q;
                        cw_fir_sample(di, dq, filt_i, filt_q);
                        float tone_i, tone_q;
                        nco::mixUp(g_sidetone_nco, filt_i, filt_q, tone_i, tone_q);
                        audio = tone_i;
                    } else {
                        audio = di;
                    }
                    audio *= g_audio_gain;
                    if (audio > 1.0f) audio = 1.0f;
                    if (audio < -1.0f) audio = -1.0f;
                    g_audio_out_buf[g_audio_out_pos++] = audio;
                    if (g_audio_out_pos >= AUDIO_OUT_BUF_SIZE)
                        flush_audio_out();
                }
            }
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
    if (g_cur_span == 0) {
        // 48kHz span: virtual rotation to center LO (shift by 3N/4 = 12kHz)
        return (display_idx + 3 * g_fft_size / 4) % g_fft_size;
    }
    // Narrow spans: DC (=VFO) already at center after fftshift, no rotation
    return display_idx;
}

void dsp::setSpan(int span_idx)
{
    if (span_idx < 0 || span_idx >= NUM_SPANS) return;
    if (span_idx == g_cur_span) return;
    g_cur_span = span_idx;
    // Reset FFT buffer to avoid mixing rates
    g_write_pos = 0;
    g_ready_buf = -1;
}

int dsp::getSpan() { return g_cur_span; }

int dsp::getSpanRate()
{
    if (g_cur_span == 0) return g_sample_rate;
    return g_sample_rate / g_spans[g_cur_span].decim;
}

const char* dsp::getSpanLabel()
{
    return g_spans[g_cur_span].label;
}
