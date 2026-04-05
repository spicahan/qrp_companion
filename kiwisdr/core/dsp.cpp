#include "dsp.h"
#include "nco.h"
#include "dsps_fft2r.h"
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int    g_sample_rate;  // 12000 Hz from KiwiSDR
static int    g_fft_size;
static int    g_num_bins;
static float *g_fft_data;
static float *g_magnitude_db;

// ── WOLA filter bank (single span at 12kHz) ─────────────────
static constexpr int WOLA_P = 4;

struct WolaState {
    float *buf_i;
    float *buf_q;
    float *window;
    int   M;
    int   P;
    int   write_pos;
    volatile int hop_counter;
    int   hop_size;
};
static WolaState g_wola;

// ── Span ─────────────────────────────────────────────────────
static int g_cur_span = 0;

// ── CW filter (300 Hz BW at 12 kHz) ─────────────────────────
static constexpr int CW_FIR_TAPS = 65;
static constexpr float CW_FIR_CUTOFF_HZ = 150.0f;
static float g_cw_fir_coeffs[CW_FIR_TAPS];
static float g_cw_fir_delay_i[CW_FIR_TAPS];
static float g_cw_fir_delay_q[CW_FIR_TAPS];
static int   g_cw_fir_pos = 0;

// ── APF (40 Hz BW at 12 kHz) ────────────────────────────────
static constexpr int APF_FIR_TAPS = 257;
static constexpr float APF_FIR_CUTOFF_HZ = 20.0f;
static float g_apf_fir_coeffs[APF_FIR_TAPS];
static float g_apf_fir_delay_i[APF_FIR_TAPS];
static float g_apf_fir_delay_q[APF_FIR_TAPS];
static int   g_apf_fir_pos = 0;
static bool  g_apf_enabled = false;

// ── Sidetone NCO ────────────────────────────────────────────
static NcoState g_sidetone_nco;
static float g_cw_offset_hz = 0;

// ── SSB filter (3 kHz LPF at 12 kHz) ────────────────────────
static constexpr int SSB_FIR_TAPS = 65;
static constexpr float SSB_FIR_CUTOFF_HZ = 2800.0f;
static float g_ssb_fir_coeffs[SSB_FIR_TAPS];
static float g_ssb_fir_delay[SSB_FIR_TAPS];
static int   g_ssb_fir_pos = 0;

// ── Audio output ─────────────────────────────────────────────
static constexpr int AUDIO_OUT_BUF_SIZE = 64;
static float g_audio_out_buf[AUDIO_OUT_BUF_SIZE];
static int   g_audio_out_pos = 0;
static dsp::AudioOutCallback g_audio_out_cb = nullptr;
static float g_audio_gain = 1000.0f;
static bool  g_mode_known = false;

// ── Goertzel ─────────────────────────────────────────────────
static constexpr int GOERTZEL_BINS = 1001;
static constexpr float GOERTZEL_BIN_HZ = 1.0f;
static constexpr float GOERTZEL_DURATION_S = 1.0f;

struct GoertzelBin {
    float coeff, cos_k, sin_k;
    float s1_re, s1_im, s2_re, s2_im;
};
static GoertzelBin g_goertzel[GOERTZEL_BINS];
static int   g_goertzel_count = 0;
static int   g_goertzel_target = 0;
static bool  g_goertzel_running = false;
static float g_goertzel_result = 0;

// ═══════════════════════════════════════════════════════════
// Filter design
// ═══════════════════════════════════════════════════════════

static void design_fir_lowpass(float *h, int N, float cutoff_hz, float sample_rate)
{
    float fc = cutoff_hz / sample_rate;
    int M = (N - 1) / 2;
    for (int n = 0; n < N; n++) {
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * n / (N - 1));
        if (n == M) h[n] = 2.0f * fc * w;
        else { float x = (float)(n - M); h[n] = sinf(2.0f * (float)M_PI * fc * x) / ((float)M_PI * x) * w; }
    }
    float sum = 0;
    for (int n = 0; n < N; n++) sum += h[n];
    if (sum > 0) for (int n = 0; n < N; n++) h[n] /= sum;
}

static void generate_wola_prototype(float *w, int M, int fft_size)
{
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    double fc = 1.0 / fft_size;
    int center = M / 2;
    for (int n = 0; n < M; n++) {
        double x = 2.0 * M_PI * n / M;
        double bh = a0 - a1 * cos(x) + a2 * cos(2 * x) - a3 * cos(3 * x);
        double s;
        int k = n - center;
        if (k == 0) s = 2.0 * fc;
        else s = sin(2.0 * M_PI * fc * k) / (M_PI * k);
        w[n] = (float)(s * bh);
    }
    float sum = 0;
    for (int n = 0; n < M; n++) sum += w[n];
    float scale = (float)fft_size / sum;
    for (int n = 0; n < M; n++) w[n] *= scale;
}

// ═══════════════════════════════════════════════════════════
// Audio filters
// ═══════════════════════════════════════════════════════════

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
    out_i = acc_i; out_q = acc_q;
}

static void apf_fir_sample(float in_i, float in_q, float &out_i, float &out_q)
{
    g_apf_fir_delay_i[g_apf_fir_pos] = in_i;
    g_apf_fir_delay_q[g_apf_fir_pos] = in_q;
    g_apf_fir_pos = (g_apf_fir_pos + 1) % APF_FIR_TAPS;
    float acc_i = 0, acc_q = 0;
    int idx = g_apf_fir_pos;
    for (int k = 0; k < APF_FIR_TAPS; k++) {
        acc_i += g_apf_fir_delay_i[idx] * g_apf_fir_coeffs[k];
        acc_q += g_apf_fir_delay_q[idx] * g_apf_fir_coeffs[k];
        idx = (idx + 1) % APF_FIR_TAPS;
    }
    out_i = acc_i; out_q = acc_q;
}

static float ssb_fir_sample(float in)
{
    g_ssb_fir_delay[g_ssb_fir_pos] = in;
    g_ssb_fir_pos = (g_ssb_fir_pos + 1) % SSB_FIR_TAPS;
    float acc = 0;
    int idx = g_ssb_fir_pos;
    for (int k = 0; k < SSB_FIR_TAPS; k++) {
        acc += g_ssb_fir_delay[idx] * g_ssb_fir_coeffs[k];
        idx = (idx + 1) % SSB_FIR_TAPS;
    }
    return acc;
}

// ═══════════════════════════════════════════════════════════
// WOLA
// ═══════════════════════════════════════════════════════════

static inline void wola_write(WolaState &w, float i, float q)
{
    w.buf_i[w.write_pos] = i;
    w.buf_q[w.write_pos] = q;
    w.write_pos = (w.write_pos + 1) % w.M;
    w.hop_counter++;
}

static void wola_process(const WolaState &w)
{
    int wp = w.write_pos;
    int N = g_fft_size;
    int M = w.M;
    int P = w.P;

    memset(g_fft_data, 0, 2 * N * sizeof(float));
    for (int p = 0; p < P; p++) {
        for (int n = 0; n < N; n++) {
            int buf_idx = (wp + p * N + n) % M;
            float wv = w.window[p * N + n];
            g_fft_data[2 * n]     += w.buf_i[buf_idx] * wv;
            g_fft_data[2 * n + 1] += w.buf_q[buf_idx] * wv;
        }
    }

    dsps_fft2r_fc32(g_fft_data, N);
    dsps_bit_rev_fc32(g_fft_data, N);

    const float scale = 1.0f / N;
    int half = N / 2;
    for (int i = 0; i < N; i++) {
        int bin = (i + half) % N;
        float re = g_fft_data[2 * bin];
        float im = g_fft_data[2 * bin + 1];
        float mag = sqrtf(re * re + im * im) * scale;
        g_magnitude_db[i] = 20.0f * log10f(mag + 1e-10f);
    }
}

// ═══════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════

void dsp::init(int sample_rate, int fft_size)
{
    g_sample_rate = sample_rate;  // 12000
    g_fft_size    = fft_size;
    g_num_bins    = fft_size;
    g_fft_data     = new float[2 * fft_size];
    g_magnitude_db = new float[fft_size];

    // Single WOLA at 12kHz, P=4
    int M = WOLA_P * fft_size;
    g_wola.P = WOLA_P;
    g_wola.M = M;
    g_wola.window = new float[M];
    generate_wola_prototype(g_wola.window, M, fft_size);
    g_wola.buf_i = new float[M]();
    g_wola.buf_q = new float[M]();
    g_wola.write_pos = 0;
    g_wola.hop_counter = 0;
    g_wola.hop_size = 512;  // 12000/512 ≈ 23.4 FPS

    g_audio_out_pos = 0;

    // Audio filters at 12 kHz
    float sr = (float)sample_rate;
    design_fir_lowpass(g_cw_fir_coeffs, CW_FIR_TAPS, CW_FIR_CUTOFF_HZ, sr);
    memset(g_cw_fir_delay_i, 0, sizeof(g_cw_fir_delay_i));
    memset(g_cw_fir_delay_q, 0, sizeof(g_cw_fir_delay_q));
    g_cw_fir_pos = 0;

    design_fir_lowpass(g_apf_fir_coeffs, APF_FIR_TAPS, APF_FIR_CUTOFF_HZ, sr);
    memset(g_apf_fir_delay_i, 0, sizeof(g_apf_fir_delay_i));
    memset(g_apf_fir_delay_q, 0, sizeof(g_apf_fir_delay_q));
    g_apf_fir_pos = 0;

    design_fir_lowpass(g_ssb_fir_coeffs, SSB_FIR_TAPS, SSB_FIR_CUTOFF_HZ, sr);
    memset(g_ssb_fir_delay, 0, sizeof(g_ssb_fir_delay));
    g_ssb_fir_pos = 0;

    // Sidetone NCO
    nco::init();
    g_sidetone_nco.phase = 0;
    g_sidetone_nco.inc = 0;

    dsps_fft2r_init_fc32(nullptr, fft_size);

    for (int i = 0; i < g_num_bins; i++)
        g_magnitude_db[i] = -120.0f;
}

void dsp::setAudioOutCallback(AudioOutCallback cb) { g_audio_out_cb = cb; }
int  dsp::getDecimatedRate() { return g_sample_rate; }  // 12kHz, no decimation
void dsp::setAudioGain(float gain) { g_audio_gain = gain; }
float dsp::getAudioGain() { return g_audio_gain; }
void dsp::setCwOffset(float offset_hz) {
    g_cw_offset_hz = offset_hz;
    if (offset_hz > 0)
        nco::setFreq(g_sidetone_nco, offset_hz, (float)g_sample_rate);
}
void dsp::setModeKnown(bool known) { g_mode_known = known; }
void dsp::setApfEnabled(bool enabled) { g_apf_enabled = enabled; }
bool dsp::isApfEnabled() { return g_apf_enabled; }

void dsp::startGoertzel()
{
    g_goertzel_target = (int)(g_sample_rate * GOERTZEL_DURATION_S);
    g_goertzel_count = 0;
    g_goertzel_result = 0;
    for (int i = 0; i < GOERTZEL_BINS; i++) {
        float freq = (i - GOERTZEL_BINS / 2) * GOERTZEL_BIN_HZ;
        float w = 2.0f * (float)M_PI * freq / g_sample_rate;
        g_goertzel[i].coeff = 2.0f * cosf(w);
        g_goertzel[i].cos_k = cosf(w);
        g_goertzel[i].sin_k = sinf(w);
        g_goertzel[i].s1_re = g_goertzel[i].s1_im = 0;
        g_goertzel[i].s2_re = g_goertzel[i].s2_im = 0;
    }
    g_goertzel_running = true;
}

bool  dsp::isGoertzelRunning() { return g_goertzel_running; }
float dsp::getGoertzelResult() { return g_goertzel_result; }
void  dsp::clearGoertzelResult() { g_goertzel_result = 0; }

// ═══════════════════════════════════════════════════════════
// pushIQ — 12kHz I/Q from KiwiSDR, VFO already at DC
// ═══════════════════════════════════════════════════════════

void dsp::pushIQ(const float *iq, int num_frames)
{
    for (int i = 0; i < num_frames; i++) {
        float I = iq[2 * i];
        float Q = iq[2 * i + 1];

        // Direct to WOLA (no mixer, no decimation — 12kHz is our native rate)
        wola_write(g_wola, I, Q);

        // CW filter runs continuously (for Goertzel + CW audio)
        float filt_i = I, filt_q = Q;
        if (g_cw_offset_hz > 0) {
            if (g_apf_enabled)
                apf_fir_sample(I, Q, filt_i, filt_q);
            else
                cw_fir_sample(I, Q, filt_i, filt_q);
        }

        // Goertzel detector (on CW-filtered I/Q)
        if (g_goertzel_running && g_cw_offset_hz > 0) {
            for (int b = 0; b < GOERTZEL_BINS; b++) {
                float c = g_goertzel[b].coeff;
                float s0_re = filt_i + c * g_goertzel[b].s1_re - g_goertzel[b].s2_re;
                float s0_im = filt_q + c * g_goertzel[b].s1_im - g_goertzel[b].s2_im;
                g_goertzel[b].s2_re = g_goertzel[b].s1_re;
                g_goertzel[b].s2_im = g_goertzel[b].s1_im;
                g_goertzel[b].s1_re = s0_re;
                g_goertzel[b].s1_im = s0_im;
            }
            g_goertzel_count++;
            if (g_goertzel_count >= g_goertzel_target) {
                float max_power = -1;
                int max_bin = GOERTZEL_BINS / 2;
                for (int b = 0; b < GOERTZEL_BINS; b++) {
                    float ck = g_goertzel[b].cos_k;
                    float sk = g_goertzel[b].sin_k;
                    float xr = g_goertzel[b].s1_re - g_goertzel[b].s2_re * ck + g_goertzel[b].s2_im * sk;
                    float xi = g_goertzel[b].s1_im - g_goertzel[b].s2_im * ck - g_goertzel[b].s2_re * sk;
                    float power = xr * xr + xi * xi;
                    if (power > max_power) { max_power = power; max_bin = b; }
                }
                g_goertzel_result = (max_bin - GOERTZEL_BINS / 2) * GOERTZEL_BIN_HZ;
                g_goertzel_running = false;
            }
        }

        // Audio output
        float audio;
        if (g_cw_offset_hz > 0) {
            // CW mode: sidetone NCO mixes filtered I/Q up to audible frequency
            float tone_i, tone_q;
            nco::mixUp(g_sidetone_nco, filt_i, filt_q, tone_i, tone_q);
            audio = tone_i;
        } else {
            // SSB/DIGI: 3 kHz LPF on I stream
            audio = ssb_fir_sample(I);
        }
        audio *= g_audio_gain;
        audio = tanhf(audio);
        g_audio_out_buf[g_audio_out_pos++] = audio;
        if (g_audio_out_pos >= AUDIO_OUT_BUF_SIZE) {
            if (g_audio_out_cb) g_audio_out_cb(g_audio_out_buf, g_audio_out_pos);
            g_audio_out_pos = 0;
        }
    }
}

bool dsp::processIfReady()
{
    if (g_wola.hop_counter < g_wola.hop_size) return false;
    g_wola.hop_counter -= g_wola.hop_size;
    __sync_synchronize();
    wola_process(g_wola);
    return true;
}

int          dsp::getNumBins()       { return g_num_bins; }
const float* dsp::getMagnitudeDb()   { return g_magnitude_db; }

int dsp::displayBin(int display_idx)
{
    // KiwiSDR: VFO at DC, fftshift puts DC at center, no rotation needed
    return display_idx;
}

void dsp::setSpan(int span_idx)
{
    g_cur_span = 0;  // only one span
    g_wola.hop_counter = 0;
}

int dsp::getSpan() { return 0; }
int dsp::getSpanRate() { return g_sample_rate; }  // 12000
const char* dsp::getSpanLabel() { return "+/-6k"; }
