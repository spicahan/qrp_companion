#include "app.h"
#include "pal.h"
#include "draw.h"
#include "dsp.h"
#include "cat.h"
#include "web.h"
#include "ui_state.h"
#include "ui_widget.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// ═══════════════════════════════════════════════════════════════
// Layout constants
// ═══════════════════════════════════════════════════════════════
static constexpr int HEADER_H   = 104;
static constexpr int SPEC_H     = 254;
static constexpr int GAP        = 2;
static constexpr int WF_H_FIXED = 256;
static constexpr int BOTTOM_H   = 104;
static constexpr int SIDE_W     = 128;

static int log_w, log_h;
static int spec_w, spec_x, spec_y;
static int wf_y, wf_h;
static draw::Framebuf fb;

// Colors
static uint16_t COL_NAVY, COL_DGREY, COL_BLACK, COL_WHITE;
static uint16_t COL_CYAN, COL_GREEN, COL_YELLOW, COL_RED;

// DSP
static constexpr int SAMPLE_RATE = 48000;
static constexpr int FFT_SIZE    = 512;
static int64_t last_dsp_time;

// Waterfall
static constexpr int WF_MAX_LINES = 512;
static float *wf_db = nullptr;
static uint16_t *wf_pixels_t = nullptr;
static int wf_head = 0, wf_count = 0, num_bins;

// VFO marker
static int g_vfo_x;
static uint16_t COL_MARKER;
static int *g_pixel_to_fftbin = nullptr;

// Spectrum dB range — read from properties
#define SPEC_DB_FLOOR  -110.0f
#define SPEC_DB_CEIL    -30.0f

// Drag-to-tune state
static bool  dragging = false;
static int   drag_start_x;
static uint64_t drag_start_freq;
static int   drag_current_x;
static bool  drag_vfo_dirty = false;



// ═══════════════════════════════════════════════════════════════
// Spectrum / waterfall helpers (unchanged)
// ═══════════════════════════════════════════════════════════════
static uint16_t spectrum_color(float norm)
{
    if (norm > 1.0f) norm = 1.0f;
    if (norm < 0.0f) norm = 0.0f;
    uint8_t r, g;
    if (norm < 0.5f) { r = (uint8_t)(norm * 2 * 255); g = 255; }
    else             { r = 255; g = (uint8_t)((1.0f - norm) * 2 * 255); }
    return draw::rgb565(r, g, 0);
}

static uint16_t waterfall_color_from_db(float db)
{
    float floor = ui::get_f32(ui::PROP_db_floor);
    float ceil  = ui::get_f32(ui::PROP_db_ceil);
    float norm = (db - floor) / (ceil - floor);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    int intensity = (int)(norm * 255);
    uint8_t r, g, b;
    if (intensity < 85)       { r = 0; g = 0; b = intensity * 3; }
    else if (intensity < 170) { r = (intensity-85)*3; g = (intensity-85)*3; b = 255-(intensity-85)*3; }
    else                      { r = 255; g = 255-(intensity-170)*3; b = 0; }
    return draw::rgb565(r, g, b);
}

static int bin_to_x(int bin) { return bin * spec_w / num_bins; }

static void precompute_pixel_bin_map()
{
    if (g_pixel_to_fftbin) delete[] g_pixel_to_fftbin;
    g_pixel_to_fftbin = new int[spec_w];
    for (int x = 0; x < spec_w; x++) {
        int di = x * num_bins / spec_w;
        if (di >= num_bins) di = num_bins - 1;
        g_pixel_to_fftbin[x] = dsp::displayBin(di);
    }
}

static void draw_spectrum(const float *mag_db)
{
    float floor = ui::get_f32(ui::PROP_db_floor);
    float ceil  = ui::get_f32(ui::PROP_db_ceil);

    draw::fillRect(fb, spec_x, spec_y, spec_w, SPEC_H, COL_BLACK);
    draw::drawVLine(fb, spec_x + g_vfo_x, spec_y, SPEC_H, COL_MARKER);

    for (int di = 0; di < num_bins; di++) {
        int x0 = bin_to_x(di);
        int x1 = bin_to_x(di + 1);
        if (x1 <= x0) x1 = x0 + 1;
        if (x0 >= spec_w) break;

        int fft_bin = dsp::displayBin(di);
        float norm = (mag_db[fft_bin] - floor) / (ceil - floor);
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        int bar_h = (int)(norm * SPEC_H);
        if (bar_h < 1) bar_h = 1;

        int bar_w = x1 - x0;
        if (x0 + bar_w > spec_w) bar_w = spec_w - x0;

        draw::fillRect(fb, spec_x + x0, spec_y + SPEC_H - bar_h, bar_w, bar_h, spectrum_color(norm));
    }
}

static void draw_waterfall()
{
    if (wf_count == 0) return;
    int n = wf_count;
    if (n > wf_h) n = wf_h;

    if (fb.rotated) {
        int phys_dy = fb.log_w - spec_x - spec_w;
        int start = wf_head;
        if (start + n <= WF_MAX_LINES) {
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, start, 0,
                           fb.buf, fb.phys_w, wf_y, phys_dy, n, spec_w, true);
        } else {
            int first = WF_MAX_LINES - start;
            int second = n - first;
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, start, 0,
                           fb.buf, fb.phys_w, wf_y, phys_dy, first, spec_w, true);
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, 0, 0,
                           fb.buf, fb.phys_w, wf_y + first, phys_dy, second, spec_w, true);
        }
        if (n < wf_h) {
            for (int py = phys_dy; py < phys_dy + spec_w; py++)
                memset(&fb.buf[py * fb.phys_w + wf_y + n], 0, (wf_h - n) * sizeof(uint16_t));
        }
    } else {
        for (int row = 0; row < n; row++) {
            int fy = wf_y + row;
            int time_idx = (wf_head + row) % WF_MAX_LINES;
            uint16_t *dst = &fb.buf[fy * fb.phys_w + spec_x];
            for (int x = 0; x < spec_w; x++)
                dst[x] = wf_pixels_t[x * WF_MAX_LINES + time_idx];
        }
        if (n < wf_h)
            draw::fillRect(fb, spec_x, wf_y + n, spec_w, wf_h - n, COL_BLACK);
    }
}

// ═══════════════════════════════════════════════════════════════
// Property change callbacks (system integration)
// ═══════════════════════════════════════════════════════════════
static void on_vfo_change(ui::PropId, ui::Source src) {
    if (src == ui::FROM_UI)
        cat::setVfoFreq(ui::get_u64(ui::PROP_vfo_freq));
}

static void on_gain_change(ui::PropId, ui::Source) {
    dsp::setAudioGain(ui::get_f32(ui::PROP_audio_gain));
}

static void on_span_change(ui::PropId, ui::Source) {
    int span = ui::get_i32(ui::PROP_span_idx);
    dsp::setSpan(span);
    precompute_pixel_bin_map();
    cat::setIqMode(span != dsp::NON_IQ_SPAN);
}

static void on_apf_change(ui::PropId, ui::Source) {
    bool apf = ui::get_bool(ui::PROP_apf_enabled);
    dsp::setApfEnabled(apf);
    cat::setCwFilter(apf ? "50" : "250");
}

static void on_mode_change(ui::PropId, ui::Source src) {
    int mode = ui::get_i32(ui::PROP_mode);
    int offset = ui::get_i32(ui::PROP_cw_offset);
    if (mode == 3 && offset > 0) {
        dsp::setCwOffset((float)offset);
        dsp::setModeKnown(true);
        // CW mode: mute local playback (use QMX's own audio output)
        ui::set_f32(ui::PROP_audio_gain, 100.0f, ui::FROM_DSP);
    } else if (mode > 0) {
        dsp::setCwOffset(0);
        dsp::setModeKnown(true);
    }
}

static void on_cwofs_change(ui::PropId, ui::Source) {
    on_mode_change(ui::PROP_mode, ui::FROM_RADIO);  // re-evaluate
}

// ═══════════════════════════════════════════════════════════════
// Widget definitions
// ═══════════════════════════════════════════════════════════════

// --- Header labels ---
static void fmt_span(char *buf, int len) {
    snprintf(buf, len, "%-9s", dsp::getSpanLabel());
}

static void fmt_freq(char *buf, int len) {
    uint64_t vfo = ui::get_u64(ui::PROP_vfo_freq);
    if (vfo > 0) {
        int mhz = (int)(vfo / 1000000);
        int khz = (int)((vfo % 1000000) / 1000);
        int hz  = (int)(vfo % 1000);
        snprintf(buf, len, "%-4s %d.%03d.%03d", cat::getModeStr(), mhz, khz, hz);
    } else {
        snprintf(buf, len, "%-4s ---.---.---", cat::getModeStr());
    }
    int blen = strlen(buf);
    while (blen < 24) buf[blen++] = ' ';
    buf[blen] = '\0';
}

static void fmt_fps(char *buf, int len) {
    snprintf(buf, len, "%4.0fFPS", ui::get_f32(ui::PROP_fps));
}

static ui::Label lbl_span, lbl_freq, lbl_fps;

// --- Bands (declared before handlers so all handlers can reference them) ---
static ui::Band header_band, bottom_band, right_band;

static void btn_span_press(ui::Button &) {
    bottom_band.setLayout("span_select");
}

static void btn_filter_press(ui::Button &) {
    if (ui::get_i32(ui::PROP_mode) == 3)
        ui::set_bool(ui::PROP_apf_enabled, !ui::get_bool(ui::PROP_apf_enabled));
}

static void btn_zerobeat_press(ui::Button &) {
    if (ui::get_i32(ui::PROP_mode) == 3)
        dsp::startGoertzel();
}

static void btn_band_press(ui::Button &) {
    bottom_band.setLayout("band_select");
}

static void btn_back_press(ui::Button &) {
    bottom_band.setLayout("default");
}

// Span selection helpers
static void select_span(int idx) {
    ui::set_i32(ui::PROP_span_idx, idx);
    bottom_band.setLayout("default");
}
static void btn_48k_press(ui::Button &)   { select_span(0); }
static void btn_12k_press(ui::Button &)   { select_span(1); }
static void btn_4k_press(ui::Button &)    { select_span(2); }
static void btn_noniq_press(ui::Button &) { select_span(dsp::NON_IQ_SPAN); }

// Band selection helpers
static void tune_band(uint64_t freq) {
    ui::set_u64(ui::PROP_vfo_freq, freq);
    bottom_band.setLayout("default");
}
static void btn_40_press(ui::Button &) { tune_band(7074000); }
static void btn_30_press(ui::Button &) { tune_band(10136000); }
static void btn_20_press(ui::Button &) { tune_band(14074000); }
static void btn_17_press(ui::Button &) { tune_band(18100000); }
static void btn_15_press(ui::Button &) { tune_band(21074000); }
static void btn_12_press(ui::Button &) { tune_band(24915000); }
static void btn_10_press(ui::Button &) { tune_band(28074000); }

// Dynamic range (DR) mode
static void btn_dr_press(ui::Button &) {
    bottom_band.setLayout("dr_active");
    right_band.setLayout("dr_sliders");
}
static void btn_dr_default_press(ui::Button &) {
    ui::set_f32(ui::PROP_db_floor, SPEC_DB_FLOOR, ui::FROM_UI);
    ui::set_f32(ui::PROP_db_ceil, SPEC_DB_CEIL, ui::FROM_UI);
}
static void btn_dr_back_press(ui::Button &) {
    bottom_band.setLayout("default");
    right_band.setLayout("default");
}

static ui::Button btn_span, btn_filter, btn_zerobeat, btn_band, btn_dr;
static ui::Button btn_48k, btn_12k, btn_4k, btn_noniq, btn_span_back;
static ui::Button btn_40, btn_30, btn_20, btn_17, btn_15, btn_12, btn_10, btn_back;
static ui::Button btn_dr_default, btn_dr_back;

static ui::Panel panel_bottom_default = { "default" };
static ui::Panel panel_span_select    = { "span_select" };
static ui::Panel panel_band_select    = { "band_select" };
static ui::Panel panel_dr_bottom      = { "dr_active" };

// --- Right band ---
static ui::Slider slider_gain;
static ui::Slider slider_ceil, slider_floor;
static ui::Panel panel_right_default  = { "default" };
static ui::Panel panel_right_dr       = { "dr_sliders" };

static ui::Panel panel_header = { "default" };

// ═══════════════════════════════════════════════════════════════
// Web UI callbacks
// ═══════════════════════════════════════════════════════════════
static void web_touch_cb(int action, int x, int y, int dw, int dh)
{
    // Map phone display coordinates to spectrum pixel coordinates
    // Phone renders spectrum across full width, VFO marker at center or 3/4
    int mapped_x = spec_x + x * spec_w / dw;
    int mapped_y = spec_y + (y - 30) * (SPEC_H + GAP + wf_h) / (dh - 30 - 48);

    // Inject as touch event (reuses the same event processing)
    pal::TouchEvent evt;
    evt.x = mapped_x;
    evt.y = mapped_y;
    evt.action = (action == 0) ? pal::TouchEvent::DOWN
               : (action == 1) ? pal::TouchEvent::MOVE
               : pal::TouchEvent::UP;
    pal::injectEvent(evt);
}

static void web_button_cb(const char *id)
{
    if (strcmp(id, "span") == 0) {
        int cur = ui::get_i32(ui::PROP_span_idx);
        ui::set_i32(ui::PROP_span_idx, (cur + 1) % dsp::NUM_SPANS);
    } else if (strcmp(id, "filter") == 0) {
        if (ui::get_i32(ui::PROP_mode) == 3)
            ui::set_bool(ui::PROP_apf_enabled, !ui::get_bool(ui::PROP_apf_enabled));
    } else if (strcmp(id, "zerobt") == 0) {
        if (ui::get_i32(ui::PROP_mode) == 3)
            dsp::startGoertzel();
    } else if (strcmp(id, "band") == 0) {
        // Cycle through common bands
        static const uint64_t bands[] = {7074000,10136000,14074000,18100000,21074000,24915000,28074000};
        static int bidx = 0;
        bidx = (bidx + 1) % 7;
        ui::set_u64(ui::PROP_vfo_freq, bands[bidx], ui::FROM_UI);
    }
}

// ═══════════════════════════════════════════════════════════════
// Audio callbacks
// ═══════════════════════════════════════════════════════════════
static void audio_output_cb(const float *samples, int count) {
    pal::audioOutputWrite(samples, count);
}
static void audio_input_cb(const float *iq_samples, int num_frames) {
    dsp::pushIQ(iq_samples, num_frames);
}

// ═══════════════════════════════════════════════════════════════
// Init
// ═══════════════════════════════════════════════════════════════
void app::init()
{
    auto info = pal::getDisplayInfo();
    log_w = info.width;
    log_h = info.height;

    spec_w = 1024;
    spec_x = (log_w - spec_w) / 2;
    spec_y = HEADER_H;
    wf_y = spec_y + SPEC_H + GAP;   // 32+126+2 = 160, naturally 32-byte aligned
    wf_h = WF_H_FIXED;

    fb.buf    = pal::getFramebuffer();
    fb.phys_w = pal::getFramebufferStride();
    fb.phys_h = pal::isRotated() ? log_w : log_h;
    fb.log_w  = log_w;
    fb.log_h  = log_h;
    fb.rotated = pal::isRotated();

    COL_NAVY   = draw::rgb565(0, 0, 128);
    COL_DGREY  = draw::rgb565(32, 32, 32);
    COL_BLACK  = draw::rgb565(0, 0, 0);
    COL_WHITE  = draw::rgb565(255, 255, 255);
    COL_CYAN   = draw::rgb565(0, 255, 255);
    COL_GREEN  = draw::rgb565(0, 255, 0);
    COL_YELLOW = draw::rgb565(255, 255, 0);
    COL_RED    = draw::rgb565(255, 0, 0);
    COL_MARKER = COL_WHITE;

    // --- UI property system ---
    ui::init();
    ui::set_f32(ui::PROP_audio_gain, 1000.0f, ui::FROM_INIT);
    ui::set_f32(ui::PROP_db_floor, SPEC_DB_FLOOR, ui::FROM_INIT);
    ui::set_f32(ui::PROP_db_ceil, SPEC_DB_CEIL, ui::FROM_INIT);
    ui::set_i32(ui::PROP_span_idx, 1, ui::FROM_INIT);

    ui::on_change(ui::PROP_vfo_freq,    on_vfo_change);
    ui::on_change(ui::PROP_audio_gain,  on_gain_change);
    ui::on_change(ui::PROP_span_idx,    on_span_change);
    ui::on_change(ui::PROP_apf_enabled, on_apf_change);
    ui::on_change(ui::PROP_mode,        on_mode_change);
    ui::on_change(ui::PROP_cw_offset,   on_cwofs_change);

    // --- DSP ---
    dsp::init(SAMPLE_RATE, FFT_SIZE);
    num_bins = dsp::getNumBins();
    wf_db = new float[WF_MAX_LINES * num_bins]();
    wf_pixels_t = new uint16_t[spec_w * WF_MAX_LINES]();
    last_dsp_time = pal::micros();

    dsp::setSpan(1);
    g_vfo_x = spec_w / 2;
    precompute_pixel_bin_map();

    // --- Init widgets ---
    auto make_label = [](ui::Label &l, int x, int y, ui::Label::FormatFunc fmt,
                         uint16_t col, uint16_t bg, int scale, int pad) {
        l.type = ui::W_LABEL; l.x = x; l.y = y; l.w = 0; l.h = 0;
        l.format = fmt; l.color = col; l.bg = bg; l.scale = scale; l.pad_chars = pad;
    };
    auto make_button = [](ui::Button &b, const char *label, uint16_t col, uint16_t bg,
                          ui::Button::PressFunc press, ui::PropId bind = ui::PROP_COUNT, bool toggle = false) {
        b.type = ui::W_BUTTON; b.label = label; b.color = col; b.bg = bg;
        b.scale = 2; b.on_press = press; b.bind = bind; b.show_as_toggle = toggle;
    };

    int hdr_ty = (HEADER_H - 16) / 2;  // center scale-2 text (16px) in header
    make_label(lbl_span,  8, hdr_ty, fmt_span, COL_CYAN, COL_NAVY, 2, 9);
    make_label(lbl_fps,   log_w - 200, hdr_ty, fmt_fps, COL_GREEN, COL_NAVY, 2, 12);

    int freq_tw = draw::textWidth("CW  14.070.02+3.0       ", 2);
    make_label(lbl_freq, (log_w - freq_tw) / 2, hdr_ty, fmt_freq, COL_GREEN, COL_NAVY, 2, 24);

    panel_header.add(&lbl_span);
    panel_header.add(&lbl_freq);
    panel_header.add(&lbl_fps);
    header_band.x = 0; header_band.y = 0; header_band.w = log_w; header_band.h = HEADER_H;
    header_band.addPanel(&panel_header);

    // Init buttons
    make_button(btn_span,     "Span",   COL_CYAN,   COL_DGREY, btn_span_press);
    make_button(btn_filter,   "Filter", COL_YELLOW, COL_DGREY, btn_filter_press, ui::PROP_apf_enabled, true);
    make_button(btn_zerobeat, "ZeroBt", COL_GREEN,  COL_DGREY, btn_zerobeat_press);
    make_button(btn_band,     "Band",   COL_WHITE,  COL_DGREY, btn_band_press);
    make_button(btn_48k, "48k",    COL_CYAN, COL_NAVY, btn_48k_press);
    make_button(btn_12k, "+/-12k", COL_CYAN, COL_NAVY, btn_12k_press);
    make_button(btn_4k,  "+/-4k",  COL_CYAN, COL_NAVY, btn_4k_press);
    make_button(btn_noniq, "NonIQ", COL_CYAN, COL_NAVY, btn_noniq_press);
    make_button(btn_span_back, "Back", COL_RED, COL_DGREY, btn_back_press);
    make_button(btn_40,  "40",   COL_WHITE, COL_NAVY, btn_40_press);
    make_button(btn_30,  "30",   COL_WHITE, COL_NAVY, btn_30_press);
    make_button(btn_20,  "20",   COL_WHITE, COL_NAVY, btn_20_press);
    make_button(btn_17,  "17",   COL_WHITE, COL_NAVY, btn_17_press);
    make_button(btn_15,  "15",   COL_WHITE, COL_NAVY, btn_15_press);
    make_button(btn_12,  "12",   COL_WHITE, COL_NAVY, btn_12_press);
    make_button(btn_10,  "10",   COL_WHITE, COL_NAVY, btn_10_press);
    make_button(btn_back,"Back", COL_RED,   COL_DGREY, btn_back_press);
    make_button(btn_dr,  "DR",   COL_WHITE, COL_DGREY, btn_dr_press);
    make_button(btn_dr_default, "Default", COL_CYAN, COL_DGREY, btn_dr_default_press);
    make_button(btn_dr_back,    "Back",    COL_RED,  COL_DGREY, btn_dr_back_press);

    // Init sliders
    slider_gain.type = ui::W_SLIDER;
    slider_gain.bind = ui::PROP_audio_gain;
    slider_gain.min_val = 100.0f; slider_gain.max_val = 10000.0f;
    slider_gain.logarithmic = true; slider_gain.unit = "dB";

    slider_ceil.type = ui::W_SLIDER;
    slider_ceil.bind = ui::PROP_db_ceil;
    slider_ceil.min_val = -80.0f; slider_ceil.max_val = 0.0f;
    slider_ceil.logarithmic = false; slider_ceil.unit = nullptr;

    slider_floor.type = ui::W_SLIDER;
    slider_floor.bind = ui::PROP_db_floor;
    slider_floor.min_val = -150.0f; slider_floor.max_val = -40.0f;
    slider_floor.logarithmic = false; slider_floor.unit = nullptr;

    // --- Layout: Right band ---
    int slider_x = log_w - SIDE_W;
    int rh = SPEC_H + GAP + wf_h;

    slider_gain.x = slider_x; slider_gain.y = spec_y;
    slider_gain.w = SIDE_W; slider_gain.h = rh;
    panel_right_default.add(&slider_gain);

    slider_ceil.x = slider_x; slider_ceil.y = spec_y;
    slider_ceil.w = SIDE_W;   slider_ceil.h = rh / 2;
    slider_floor.x = slider_x; slider_floor.y = spec_y + rh / 2;
    slider_floor.w = SIDE_W;   slider_floor.h = rh / 2;
    panel_right_dr.add(&slider_ceil);
    panel_right_dr.add(&slider_floor);

    right_band.x = slider_x; right_band.y = spec_y;
    right_band.w = SIDE_W; right_band.h = rh;
    right_band.addPanel(&panel_right_default);
    right_band.addPanel(&panel_right_dr);

    // --- Layout: Bottom band ---
    int bot_y = log_h - BOTTOM_H;
    int bw = log_w / 5;

    btn_span.x = 0;       btn_span.y = bot_y;   btn_span.w = bw;   btn_span.h = BOTTOM_H;
    btn_filter.x = bw;    btn_filter.y = bot_y;  btn_filter.w = bw; btn_filter.h = BOTTOM_H;
    btn_zerobeat.x = 2*bw; btn_zerobeat.y = bot_y; btn_zerobeat.w = bw; btn_zerobeat.h = BOTTOM_H;
    btn_band.x = 3*bw;    btn_band.y = bot_y;    btn_band.w = bw;   btn_band.h = BOTTOM_H;
    btn_dr.x = 4*bw;      btn_dr.y = bot_y;      btn_dr.w = log_w - 4*bw; btn_dr.h = BOTTOM_H;

    panel_bottom_default.add(&btn_span);
    panel_bottom_default.add(&btn_filter);
    panel_bottom_default.add(&btn_zerobeat);
    panel_bottom_default.add(&btn_band);
    panel_bottom_default.add(&btn_dr);

    // Span select panel (5 buttons)
    int sbw = log_w / 5;
    btn_48k.x = 0;        btn_48k.y = bot_y;  btn_48k.w = sbw;           btn_48k.h = BOTTOM_H;
    btn_12k.x = sbw;      btn_12k.y = bot_y;  btn_12k.w = sbw;           btn_12k.h = BOTTOM_H;
    btn_4k.x = 2*sbw;     btn_4k.y = bot_y;   btn_4k.w = sbw;            btn_4k.h = BOTTOM_H;
    btn_noniq.x = 3*sbw;  btn_noniq.y = bot_y; btn_noniq.w = sbw;        btn_noniq.h = BOTTOM_H;
    btn_span_back.x = 4*sbw; btn_span_back.y = bot_y; btn_span_back.w = log_w - 4*sbw; btn_span_back.h = BOTTOM_H;

    panel_span_select.add(&btn_48k);
    panel_span_select.add(&btn_12k);
    panel_span_select.add(&btn_4k);
    panel_span_select.add(&btn_noniq);
    panel_span_select.add(&btn_span_back);

    // Band select panel (8 buttons)
    int bbw = log_w / 8;
    btn_40.x = 0;      btn_40.y = bot_y; btn_40.w = bbw; btn_40.h = BOTTOM_H;
    btn_30.x = bbw;    btn_30.y = bot_y; btn_30.w = bbw; btn_30.h = BOTTOM_H;
    btn_20.x = 2*bbw;  btn_20.y = bot_y; btn_20.w = bbw; btn_20.h = BOTTOM_H;
    btn_17.x = 3*bbw;  btn_17.y = bot_y; btn_17.w = bbw; btn_17.h = BOTTOM_H;
    btn_15.x = 4*bbw;  btn_15.y = bot_y; btn_15.w = bbw; btn_15.h = BOTTOM_H;
    btn_12.x = 5*bbw;  btn_12.y = bot_y; btn_12.w = bbw; btn_12.h = BOTTOM_H;
    btn_10.x = 6*bbw;  btn_10.y = bot_y; btn_10.w = bbw; btn_10.h = BOTTOM_H;
    btn_back.x = 7*bbw; btn_back.y = bot_y; btn_back.w = log_w - 7*bbw; btn_back.h = BOTTOM_H;

    panel_band_select.add(&btn_40);
    panel_band_select.add(&btn_30);
    panel_band_select.add(&btn_20);
    panel_band_select.add(&btn_17);
    panel_band_select.add(&btn_15);
    panel_band_select.add(&btn_12);
    panel_band_select.add(&btn_10);
    panel_band_select.add(&btn_back);

    // DR mode bottom panel (2 buttons)
    int drw = log_w / 2;
    btn_dr_default.x = 0;   btn_dr_default.y = bot_y; btn_dr_default.w = drw;          btn_dr_default.h = BOTTOM_H;
    btn_dr_back.x = drw;    btn_dr_back.y = bot_y;    btn_dr_back.w = log_w - drw;     btn_dr_back.h = BOTTOM_H;
    panel_dr_bottom.add(&btn_dr_default);
    panel_dr_bottom.add(&btn_dr_back);

    bottom_band.x = 0; bottom_band.y = bot_y; bottom_band.w = log_w; bottom_band.h = BOTTOM_H;
    bottom_band.addPanel(&panel_bottom_default);
    bottom_band.addPanel(&panel_span_select);
    bottom_band.addPanel(&panel_band_select);
    bottom_band.addPanel(&panel_dr_bottom);

    // --- Audio ---
    pal::audioOutputOpen(dsp::getDecimatedRate());
    dsp::setAudioOutCallback(audio_output_cb);
    pal::audioInputOpen(audio_input_cb);
    cat::init();
    cat::setIqMode(true);  // default span is I/Q

    // Web UI server
    web::init(8080);
    web::setTouchCallback(web_touch_cb);
    web::setButtonCallback(web_button_cb);
}

// ═══════════════════════════════════════════════════════════════
// Tick
// ═══════════════════════════════════════════════════════════════
void app::tick()
{
    int64_t t0 = pal::micros();

    // FPS
    static uint32_t frame_count = 0;
    static int64_t fps_last_time = 0;
    frame_count++;
    if (t0 - fps_last_time >= 1000000) {
        ui::set_f32(ui::PROP_fps, (float)frame_count * 1000000.0f / (t0 - fps_last_time), ui::FROM_DSP);
        frame_count = 0;
        fps_last_time = t0;
    }

    // --- CAT → properties ---
    cat::poll();
    ui::set_i32(ui::PROP_mode, cat::getMode(), ui::FROM_RADIO);
    ui::set_u64(ui::PROP_vfo_freq, cat::getVfoFreq(), ui::FROM_RADIO);
    ui::set_i32(ui::PROP_cw_offset, cat::getCwOffset(), ui::FROM_RADIO);

    // VFO marker position
    g_vfo_x = (dsp::getSpan() == 0) ? spec_w * 3 / 4 : spec_w / 2;

    // --- Goertzel result → VFO at 1Hz resolution ---
    if (!dsp::isGoertzelRunning() && dsp::getGoertzelResult() != 0) {
        float g_offset = dsp::getGoertzelResult();
        dsp::clearGoertzelResult();
        uint64_t vfo = ui::get_u64(ui::PROP_vfo_freq);
        if (vfo > 0) {
            int delta = -(int)roundf(g_offset);
            if (delta != 0)
                ui::set_u64(ui::PROP_vfo_freq, (int64_t)vfo + delta, ui::FROM_UI);
        }
    }

    // --- DSP ---
    bool new_spectrum = dsp::processIfReady();
    if (new_spectrum) {
        last_dsp_time = t0;
        const float *mag = dsp::getMagnitudeDb();
        memcpy(&wf_db[wf_head * num_bins], mag, num_bins * sizeof(float));
        wf_head = (wf_head - 1 + WF_MAX_LINES) % WF_MAX_LINES;
        for (int lx = 0; lx < spec_w; lx++) {
            int fft_bin = g_pixel_to_fftbin[lx];
            wf_pixels_t[lx * WF_MAX_LINES + wf_head] = waterfall_color_from_db(mag[fft_bin]);
        }
        if (wf_count < WF_MAX_LINES) wf_count++;

        // Stream spectrum to web clients
        if (web::hasClients()) {
            float floor = ui::get_f32(ui::PROP_db_floor);
            float ceil  = ui::get_f32(ui::PROP_db_ceil);
            float range = ceil - floor;
            uint8_t web_bins[512];
            for (int i = 0; i < num_bins && i < 512; i++) {
                int fft_bin = dsp::displayBin(i);
                float norm = (mag[fft_bin] - floor) / range;
                if (norm < 0) norm = 0;
                if (norm > 1) norm = 1;
                web_bins[i] = (uint8_t)(norm * 255);
            }
            web::sendSpectrum(web_bins, num_bins,
                              dsp::getSpanRate(),
                              ui::get_u64(ui::PROP_vfo_freq),
                              ui::get_i32(ui::PROP_mode),
                              ui::get_bool(ui::PROP_apf_enabled));
        }
    }

    // --- Web server poll ---
    web::poll();

    // --- Touch events ---
    static constexpr int TAP_THRESHOLD = 5;
    pal::TouchEvent evt;
    while (pal::pollEvent(evt)) {
        int action = (evt.action == pal::TouchEvent::DOWN) ? 0
                   : (evt.action == pal::TouchEvent::MOVE) ? 1 : 2;

        // Route to bands first
        if (bottom_band.onTouch(evt.x, evt.y, action)) continue;
        if (right_band.onTouch(evt.x, evt.y, action)) continue;

        // Spectrum touch-to-tune / drag-to-tune (only in spectrum/WF area)
        bool in_spec_wf = (evt.x >= spec_x && evt.x < spec_x + spec_w &&
                           evt.y >= spec_y && evt.y < wf_y + wf_h);
        if (evt.action == pal::TouchEvent::DOWN && in_spec_wf) {
            dragging = true;
            drag_start_x = evt.x;
            drag_start_freq = ui::get_u64(ui::PROP_vfo_freq);
            drag_current_x = evt.x;
            drag_vfo_dirty = false;
            cat::suppressPolling(true);
        }
        else if (evt.action == pal::TouchEvent::MOVE && dragging) {
            drag_current_x = evt.x;
            drag_vfo_dirty = true;
        }
        else if (evt.action == pal::TouchEvent::UP && dragging) {
            int total_drag = drag_current_x - drag_start_x;
            if (total_drag < 0) total_drag = -total_drag;

            if (total_drag < TAP_THRESHOLD && drag_start_freq > 0) {
                int delta_px = evt.x - spec_x - g_vfo_x;
                int delta_hz = delta_px * dsp::getSpanRate() / spec_w;
                uint64_t new_freq = ((int64_t)drag_start_freq + delta_hz) / 10 * 10;
                if (new_freq > 0) {
                    ui::set_u64(ui::PROP_vfo_freq, new_freq, ui::FROM_UI);
                    if (ui::get_i32(ui::PROP_mode) == 3)
                        dsp::startGoertzel();
                }
            } else if (drag_vfo_dirty && drag_start_freq > 0) {
                int delta_px = drag_current_x - drag_start_x;
                int delta_hz = -delta_px * dsp::getSpanRate() / spec_w;
                uint64_t new_freq = ((int64_t)drag_start_freq + delta_hz) / 10 * 10;
                if (new_freq > 0)
                    ui::set_u64(ui::PROP_vfo_freq, new_freq, ui::FROM_UI);
            }
            dragging = false;
            drag_vfo_dirty = false;
            cat::suppressPolling(false);
        }
    }

    // Sync drag — update VFO every tick (not gated on spectrum frame rate)
    if (dragging && drag_vfo_dirty && drag_start_freq > 0) {
        int delta_px = drag_current_x - drag_start_x;
        int delta_hz = -delta_px * dsp::getSpanRate() / spec_w;
        uint64_t new_freq = ((int64_t)drag_start_freq + delta_hz) / 10 * 10;
        if (new_freq > 0)
            ui::set_u64(ui::PROP_vfo_freq, new_freq, ui::FROM_UI);
        drag_vfo_dirty = false;
    }

    // --- Draw frame ---
    static bool first_frame = true;
    if (first_frame) {
        int total = fb.rotated ? (fb.phys_w * fb.phys_h) : (fb.log_w * fb.log_h);
        for (int i = 0; i < total; i++) fb.buf[i] = COL_BLACK;
        draw::fillRect(fb, 0, 0, log_w, HEADER_H, COL_NAVY);
        first_frame = false;
    }

    draw_spectrum(dsp::getMagnitudeDb());
    draw::fillRect(fb, spec_x, spec_y + SPEC_H, spec_w, GAP, COL_GREEN);
    draw_waterfall();

    header_band.draw(fb);
    right_band.draw(fb);
    bottom_band.draw(fb);

    pal::commitFrame();
    ui::clear_all_dirty();
}
