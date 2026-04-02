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

// ── WOLA filter bank ─────────────────────────────────────────
// Per-span P: wide spans (48k,±12k) use P=4 for sidelobe rejection,
// narrow spans (±4k,NonIQ) use P=2 for lower spectral latency.
static const int g_wola_p[dsp::NUM_SPANS] = { 4, 4, 2, 2 };

struct WolaState {
    float *buf_i;                   // circular buffer I [M]
    float *buf_q;                   // circular buffer Q [M]
    float *window;                  // prototype filter [M]
    int   M;                        // = P * fft_size
    int   P;                        // polyphase segments
    int   write_pos;
    volatile int hop_counter;
    int   hop_size;
};
static WolaState g_wola[dsp::NUM_SPANS];

// ── Independent decimation (no cascade — minimal latency) ────
//  ↓2: 48k → 24k for ±12k span
//  ↓6: 48k → 8k  for ±4k span + audio output
static constexpr int D2_TAPS = 65;    // ↓2 anti-alias FIR
static constexpr int D6_TAPS = 193;   // ↓6 anti-alias FIR (divisible-ish, odd)
static constexpr int AUDIO_DECIM = 6; // decimation factor for audio path

struct DecimState {
    float *delay_i;
    float *delay_q;
    int   taps;
    int   pos;
    int   counter;
    int   decim;
};
static DecimState g_decim2;   // ↓2
static DecimState g_decim6;   // ↓6
static float g_d2_coeffs[D2_TAPS];
static float g_d6_coeffs[D6_TAPS];

// ── fs/4 down-conversion mixer ───────────────────────────────
static int g_mix_phase;

// ── CW offset NCO ───────────────────────────────────────────
static NcoState g_cw_nco;
static bool  g_cw_nco_active = false;
static float g_cw_offset_hz = 0;
static float g_soft_nco_hz = 0;

// ── CW audio filter (300 Hz BW at 8 kHz) + sidetone NCO ────
static constexpr int CW_FIR_TAPS = 65;
static constexpr float CW_FIR_CUTOFF_HZ = 150.0f;
static float g_cw_fir_coeffs[CW_FIR_TAPS];
static float g_cw_fir_delay_i[CW_FIR_TAPS];
static float g_cw_fir_delay_q[CW_FIR_TAPS];
static int   g_cw_fir_pos = 0;
static NcoState g_sidetone_nco;

// ── APF (40 Hz BW at 8 kHz) ─────────────────────────────────
static constexpr int APF_FIR_TAPS = 257;
static constexpr float APF_FIR_CUTOFF_HZ = 20.0f;
static float g_apf_fir_coeffs[APF_FIR_TAPS];
static float g_apf_fir_delay_i[APF_FIR_TAPS];
static float g_apf_fir_delay_q[APF_FIR_TAPS];
static int   g_apf_fir_pos = 0;
static bool  g_apf_enabled = false;

// ── SSB/DIGI audio filter (3 kHz LPF at 8 kHz) ─────────────
static constexpr int SSB_FIR_TAPS = 65;
static constexpr float SSB_FIR_CUTOFF_HZ = 2800.0f;
static float g_ssb_fir_coeffs[SSB_FIR_TAPS];
static float g_ssb_fir_delay[SSB_FIR_TAPS];
static int   g_ssb_fir_pos = 0;

static bool  g_mode_known = false;

// ── Goertzel fine-tune detector ─────────────────────────────
static constexpr int GOERTZEL_BINS = 301;
static constexpr float GOERTZEL_BIN_HZ = 1.0f;
static constexpr float GOERTZEL_DURATION_S = 1.0f;

struct GoertzelBin {
    float coeff;
    float cos_k, sin_k;
    float s1_re, s1_im;
    float s2_re, s2_im;
};
static GoertzelBin g_goertzel[GOERTZEL_BINS];
static int   g_goertzel_count = 0;
static int   g_goertzel_target = 0;
static bool  g_goertzel_running = false;
static float g_goertzel_result = 0;

// ── Hilbert transform (real → analytic for Non-I/Q mode) ────
static constexpr int HILBERT_TAPS = 255;
static float g_hilbert_coeffs[HILBERT_TAPS];
static float g_hilbert_delay[HILBERT_TAPS];
static int   g_hilbert_pos = 0;
static int   g_hard_decim_counter = 0;   // hard ↓6 counter for Non-I/Q

// ── Span definitions ────────────────────────────────────────
// 0=48k, 1=±12k, 2=±4k, 3=NonIQ
static const int g_span_rates[dsp::NUM_SPANS]  = { 48000, 24000,  8000,  8000 };
static const int g_hop_sizes[dsp::NUM_SPANS]   = {  2048,  1024,   320,   320 };  // N=512
static const char *g_span_labels[dsp::NUM_SPANS] = { "48k", "+/-12k", "+/-4k", "NonIQ" };
static int g_cur_span = 0;

// ── Audio output ────────────────────────────────────────────
static constexpr int AUDIO_OUT_BUF_SIZE = 32;   // smaller for lower latency
static float g_audio_out_buf[AUDIO_OUT_BUF_SIZE];
static int   g_audio_out_pos = 0;
static dsp::AudioOutCallback g_audio_out_cb = nullptr;
static float g_audio_gain = 1000.0f;

// ═════════════════════════════════════════════════════════════
// Filter design
// ═════════════════════════════════════════════════════════════

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
    float sum = 0;
    for (int n = 0; n < N; n++) sum += h[n];
    if (sum > 0) for (int n = 0; n < N; n++) h[n] /= sum;
}

// WOLA prototype filter: windowed-sinc lowpass with Blackman-Harris envelope.
// Cutoff at fs/N (full bin spacing) for flat-top per-bin response.
// Reference: Lyons, "Understanding DSP", Ch.13 — WOLA spectrum analyzer.
static void generate_wola_prototype(float *w, int M, int fft_size)
{
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    double fc = 1.0 / fft_size;  // normalized cutoff = 1/N (full bin spacing)
    int center = M / 2;

    for (int n = 0; n < M; n++) {
        double x = 2.0 * M_PI * n / M;
        double bh = a0 - a1 * cos(x) + a2 * cos(2 * x) - a3 * cos(3 * x);

        double s;
        int k = n - center;
        if (k == 0)
            s = 2.0 * fc;
        else
            s = sin(2.0 * M_PI * fc * k) / (M_PI * k);

        w[n] = (float)(s * bh);
    }

    float sum = 0;
    for (int n = 0; n < M; n++) sum += w[n];
    float scale = (float)fft_size / sum;
    for (int n = 0; n < M; n++) w[n] *= scale;
}

// ═════════════════════════════════════════════════════════════
// Hilbert transform (Non-I/Q mode)
// ═════════════════════════════════════════════════════════════

static void design_hilbert(float *h, int N)
{
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    int center = (N - 1) / 2;
    for (int n = 0; n < N; n++) {
        int k = n - center;
        if (k == 0 || (k & 1) == 0) {
            h[n] = 0.0f;
        } else {
            double x = 2.0 * M_PI * n / (N - 1);
            double win = a0 - a1 * cos(x) + a2 * cos(2 * x) - a3 * cos(3 * x);
            h[n] = (float)((2.0 / (M_PI * k)) * win);
        }
    }
}

static void hilbert_process(float in, float &out_i, float &out_q)
{
    g_hilbert_delay[g_hilbert_pos] = in;
    g_hilbert_pos = (g_hilbert_pos + 1) % HILBERT_TAPS;

    float acc = 0;
    int start = ((HILBERT_TAPS - 1) / 2) & 1 ? 0 : 1;
    for (int k = start; k < HILBERT_TAPS; k += 2) {
        int idx = (g_hilbert_pos + k) % HILBERT_TAPS;
        acc += g_hilbert_delay[idx] * g_hilbert_coeffs[k];
    }
    out_q = -acc;

    int delayed_idx = (g_hilbert_pos + (HILBERT_TAPS - 1) / 2) % HILBERT_TAPS;
    out_i = g_hilbert_delay[delayed_idx];
}

// ═════════════════════════════════════════════════════════════
// Audio filters
// ═════════════════════════════════════════════════════════════

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
    out_i = acc_i;
    out_q = acc_q;
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

static void flush_audio_out()
{
    if (g_audio_out_pos > 0 && g_audio_out_cb) {
        g_audio_out_cb(g_audio_out_buf, g_audio_out_pos);
        g_audio_out_pos = 0;
    }
}

// ═════════════════════════════════════════════════════════════
// Independent decimation (no cascade — minimal group delay)
// ═════════════════════════════════════════════════════════════

static void decim_init(DecimState &s, int taps, int decim)
{
    s.delay_i = new float[taps]();
    s.delay_q = new float[taps]();
    s.taps = taps;
    s.pos = 0;
    s.counter = 0;
    s.decim = decim;
}

static bool decim_process(DecimState &s, const float *coeffs,
                          float in_i, float in_q, float &out_i, float &out_q)
{
    s.delay_i[s.pos] = in_i;
    s.delay_q[s.pos] = in_q;
    s.pos = (s.pos + 1) % s.taps;

    s.counter++;
    if (s.counter < s.decim) return false;
    s.counter = 0;

    float acc_i = 0, acc_q = 0;
    int idx = s.pos;
    for (int k = 0; k < s.taps; k++) {
        acc_i += s.delay_i[idx] * coeffs[k];
        acc_q += s.delay_q[idx] * coeffs[k];
        idx = (idx + 1) % s.taps;
    }
    out_i = acc_i;
    out_q = acc_q;
    return true;
}

// ═════════════════════════════════════════════════════════════
// WOLA processing
// ═════════════════════════════════════════════════════════════

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
    int N  = g_fft_size;
    int M  = w.M;
    int P  = w.P;

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

// ═════════════════════════════════════════════════════════════
// NCO helpers
// ═════════════════════════════════════════════════════════════

static void update_cw_ncos()
{
    if (g_cw_offset_hz == 0.0f) {
        g_cw_nco_active = false;
        g_cw_nco.inc = 0;
        g_sidetone_nco.inc = 0;
        return;
    }
    float total_shift = g_cw_offset_hz + g_soft_nco_hz;
    nco::setFreq(g_cw_nco, total_shift, (float)g_sample_rate);
    float dec_rate = (float)g_sample_rate / AUDIO_DECIM;
    nco::setFreq(g_sidetone_nco, g_cw_offset_hz, dec_rate);
    g_cw_nco_active = true;
}

// ═════════════════════════════════════════════════════════════
// Public API
// ═════════════════════════════════════════════════════════════

void dsp::init(int sample_rate, int fft_size)
{
    g_sample_rate = sample_rate;
    g_fft_size    = fft_size;
    g_num_bins    = fft_size;
    g_fft_data     = new float[2 * fft_size];
    g_magnitude_db = new float[fft_size];
    g_mix_phase    = 0;

    // WOLA per-span: each span has its own P, M, window, and buffers
    for (int i = 0; i < NUM_SPANS; i++) {
        int P = g_wola_p[i];
        int M = P * fft_size;
        g_wola[i].P = P;
        g_wola[i].M = M;
        g_wola[i].window = new float[M];
        generate_wola_prototype(g_wola[i].window, M, fft_size);
        g_wola[i].buf_i = new float[M]();
        g_wola[i].buf_q = new float[M]();
        g_wola[i].write_pos = 0;
        g_wola[i].hop_counter = 0;
        g_wola[i].hop_size = g_hop_sizes[i];
    }

    // Independent decimators (no cascade)
    // ↓2: 48k → 24k, cutoff 10 kHz
    design_fir_lowpass(g_d2_coeffs, D2_TAPS, 10000.0f, (float)sample_rate);
    decim_init(g_decim2, D2_TAPS, 2);

    // ↓6: 48k → 8k, cutoff 3.5 kHz (passband 3kHz + margin, stopband at 4kHz Nyquist)
    design_fir_lowpass(g_d6_coeffs, D6_TAPS, 3500.0f, (float)sample_rate);
    decim_init(g_decim6, D6_TAPS, AUDIO_DECIM);

    // Hilbert transform (Non-I/Q mode)
    design_hilbert(g_hilbert_coeffs, HILBERT_TAPS);
    memset(g_hilbert_delay, 0, sizeof(g_hilbert_delay));
    g_hilbert_pos = 0;
    g_hard_decim_counter = 0;

    g_cur_span = 0;
    g_audio_out_pos = 0;

    // NCO init
    nco::init();
    g_cw_nco.phase = 0;
    g_cw_nco.inc = 0;
    g_cw_nco_active = false;
    g_cw_offset_hz = 0;

    // Audio filters (at ↓6 rate = 8 kHz)
    float dec_rate = (float)sample_rate / AUDIO_DECIM;
    design_fir_lowpass(g_cw_fir_coeffs, CW_FIR_TAPS, CW_FIR_CUTOFF_HZ, dec_rate);
    memset(g_cw_fir_delay_i, 0, sizeof(g_cw_fir_delay_i));
    memset(g_cw_fir_delay_q, 0, sizeof(g_cw_fir_delay_q));
    g_cw_fir_pos = 0;

    // Sidetone NCO
    g_sidetone_nco.phase = 0;
    g_sidetone_nco.inc = 0;

    // APF
    design_fir_lowpass(g_apf_fir_coeffs, APF_FIR_TAPS, APF_FIR_CUTOFF_HZ, dec_rate);
    memset(g_apf_fir_delay_i, 0, sizeof(g_apf_fir_delay_i));
    memset(g_apf_fir_delay_q, 0, sizeof(g_apf_fir_delay_q));
    g_apf_fir_pos = 0;
    g_apf_enabled = false;

    // SSB/DIGI filter
    design_fir_lowpass(g_ssb_fir_coeffs, SSB_FIR_TAPS, SSB_FIR_CUTOFF_HZ, dec_rate);
    memset(g_ssb_fir_delay, 0, sizeof(g_ssb_fir_delay));
    g_ssb_fir_pos = 0;
    g_mode_known = false;

    dsps_fft2r_init_fc32(nullptr, fft_size);

    for (int i = 0; i < g_num_bins; i++)
        g_magnitude_db[i] = -120.0f;
}

void dsp::setAudioOutCallback(AudioOutCallback cb) { g_audio_out_cb = cb; }
int  dsp::getDecimatedRate() { return g_sample_rate / AUDIO_DECIM; }
void dsp::setAudioGain(float gain) { g_audio_gain = gain; }
float dsp::getAudioGain() { return g_audio_gain; }

void dsp::startGoertzel()
{
    float dec_rate = (float)g_sample_rate / AUDIO_DECIM;
    g_goertzel_target = (int)(dec_rate * GOERTZEL_DURATION_S);
    g_goertzel_count = 0;
    g_goertzel_result = 0;

    for (int i = 0; i < GOERTZEL_BINS; i++) {
        float freq = (i - GOERTZEL_BINS / 2) * GOERTZEL_BIN_HZ;
        float w = 2.0f * (float)M_PI * freq / dec_rate;
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

void dsp::setCwOffset(float offset_hz)
{
    g_cw_offset_hz = offset_hz;
    update_cw_ncos();
}

void dsp::setSoftNcoCorrection(float hz)
{
    g_soft_nco_hz = hz;
    if (g_cw_nco_active) update_cw_ncos();
}

float dsp::getSoftNcoCorrection() { return g_soft_nco_hz; }

void dsp::setModeKnown(bool known) { g_mode_known = known; }
void dsp::setApfEnabled(bool enabled) { g_apf_enabled = enabled; }
bool dsp::isApfEnabled() { return g_apf_enabled; }

// ═════════════════════════════════════════════════════════════
// Audio pipeline helper (called at 8 kHz)
// ═════════════════════════════════════════════════════════════

static void process_audio_8k(float di, float dq)
{
    float audio = 0;
    if (!g_mode_known) {
        // Mute until mode is determined
    } else if (g_cw_nco_active) {
        float filt_i, filt_q;
        if (g_apf_enabled)
            apf_fir_sample(di, dq, filt_i, filt_q);
        else
            cw_fir_sample(di, dq, filt_i, filt_q);

        // Goertzel detector
        if (g_goertzel_running) {
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
                    if (power > max_power) {
                        max_power = power;
                        max_bin = b;
                    }
                }
                g_goertzel_result = (max_bin - GOERTZEL_BINS / 2) * GOERTZEL_BIN_HZ;
                g_goertzel_running = false;
            }
        }

        float tone_i, tone_q;
        nco::mixUp(g_sidetone_nco, filt_i, filt_q, tone_i, tone_q);
        audio = tone_i;
    } else {
        // Non-CW: 3 kHz LPF on I stream
        audio = ssb_fir_sample(di);
    }
    audio *= g_audio_gain;
    audio = tanhf(audio);
    g_audio_out_buf[g_audio_out_pos++] = audio;
    if (g_audio_out_pos >= AUDIO_OUT_BUF_SIZE)
        flush_audio_out();
}

// ═════════════════════════════════════════════════════════════
// pushIQ — sample-by-sample processing
// ═════════════════════════════════════════════════════════════

void dsp::pushIQ(const float *iq, int num_frames)
{
    for (int i = 0; i < num_frames; i++) {
        float I = iq[2 * i];
        float Q = iq[2 * i + 1];

        if (g_cur_span == NON_IQ_SPAN) {
            // ── Non-I/Q path: mono baseband → Hilbert → CW NCO → hard ↓6 ──
            float hI, hQ;
            hilbert_process(I, hI, hQ);

            if (g_cw_nco_active) {
                float nI, nQ;
                nco::mixDown(g_cw_nco, hI, hQ, nI, nQ);
                hI = nI; hQ = nQ;
            }

            g_hard_decim_counter++;
            if (g_hard_decim_counter >= AUDIO_DECIM) {
                g_hard_decim_counter = 0;
                wola_write(g_wola[NON_IQ_SPAN], hI, hQ);
                process_audio_8k(hI, hQ);
            }
        } else {
            // ── Normal I/Q path ──

            // fs/4 complex down-conversion
            float oI, oQ;
            switch (g_mix_phase) {
                case 0: oI =  I; oQ =  Q; break;
                case 1: oI =  Q; oQ = -I; break;
                case 2: oI = -I; oQ = -Q; break;
                case 3: oI = -Q; oQ =  I; break;
            }
            g_mix_phase = (g_mix_phase + 1) & 3;

            // CW offset NCO: shift CW sidetone to DC
            if (g_cw_nco_active) {
                float nI, nQ;
                nco::mixDown(g_cw_nco, oI, oQ, nI, nQ);
                oI = nI; oQ = nQ;
            }

            // Fill 48k WOLA buffer (span 0)
            wola_write(g_wola[0], oI, oQ);

            // ↓2: 48k → 24k (independent, for ±12k span)
            float d2i, d2q;
            if (decim_process(g_decim2, g_d2_coeffs, oI, oQ, d2i, d2q)) {
                wola_write(g_wola[1], d2i, d2q);
            }

            // ↓6: 48k → 8k (independent, for ±4k span + audio)
            float d6i, d6q;
            if (decim_process(g_decim6, g_d6_coeffs, oI, oQ, d6i, d6q)) {
                wola_write(g_wola[2], d6i, d6q);
                process_audio_8k(d6i, d6q);
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════
// processIfReady — called from main loop
// ═════════════════════════════════════════════════════════════

bool dsp::processIfReady()
{
    WolaState &w = g_wola[g_cur_span];
    if (w.hop_counter < w.hop_size) return false;
    w.hop_counter -= w.hop_size;
    __sync_synchronize();

    wola_process(w);
    return true;
}

int          dsp::getNumBins()       { return g_num_bins; }
const float* dsp::getMagnitudeDb()   { return g_magnitude_db; }

int dsp::displayBin(int display_idx)
{
    if (g_cur_span == 0) {
        // 48k span: rotate by 3N/4 to center VFO (placed at DC by fs/4 mixer)
        return (display_idx + 3 * g_fft_size / 4) % g_fft_size;
    }
    // Narrow spans + NonIQ: DC (= VFO) already at center after fftshift
    return display_idx;
}

void dsp::setSpan(int span_idx)
{
    if (span_idx < 0 || span_idx >= NUM_SPANS) return;
    if (span_idx == g_cur_span) return;
    g_cur_span = span_idx;
    g_wola[span_idx].hop_counter = 0;
}

int dsp::getSpan() { return g_cur_span; }

int dsp::getSpanRate()
{
    return g_span_rates[g_cur_span];
}

const char* dsp::getSpanLabel()
{
    return g_span_labels[g_cur_span];
}
