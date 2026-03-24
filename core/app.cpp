#include "app.h"
#include "pal.h"
#include "draw.h"
#include "dsp.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// Layout constants (logical landscape)
static constexpr int HEADER_H = 28;
static constexpr int SPEC_H   = 180;
static constexpr int GAP      = 2;
static constexpr int INFO_H   = 28;

static int log_w, log_h;
static int wf_y, wf_h;

// Colors
static uint16_t COL_NAVY, COL_DGREY, COL_BLACK, COL_WHITE;
static uint16_t COL_CYAN, COL_GREEN, COL_YELLOW, COL_RED;

// FPS
static uint32_t frame_count;
static float fps;
static int64_t fps_last_time;
static float t_draw_ms, t_commit_ms, t_total_ms;

// Touch
static int64_t touch_time;
static int touch_x, touch_y;
static char touch_text[64];

// DSP
static constexpr int SAMPLE_RATE = 48000;
static constexpr int FFT_SIZE    = 1024;
static int64_t last_dsp_time;

// Waterfall ring buffer (stores dB values)
static constexpr int WF_MAX_LINES = 512;
static float *wf_db = nullptr;     // WF_MAX_LINES * num_bins
static int wf_head = 0;
static int wf_count = 0;
static int num_bins;

// Spectrum dB range for display mapping
static constexpr float DB_MIN = -100.0f;
static constexpr float DB_MAX = 0.0f;

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
    float norm = (db - DB_MIN) / (DB_MAX - DB_MIN);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    int intensity = (int)(norm * 255);
    uint8_t r, g, b;
    if (intensity < 85)       { r = 0; g = 0; b = intensity * 3; }
    else if (intensity < 170) { r = (intensity-85)*3; g = (intensity-85)*3; b = 255-(intensity-85)*3; }
    else                      { r = 255; g = 255-(intensity-170)*3; b = 0; }
    return draw::rgb565(r, g, b);
}

// Map FFT bin index to display x coordinate
static int bin_to_x(int bin)
{
    return bin * log_w / num_bins;
}

static void draw_spectrum(uint16_t *fb, const float *mag_db)
{
    int spec_y = HEADER_H + GAP;

    // Clear spectrum area
    draw::fillRect(fb, log_w, 0, spec_y, log_w, SPEC_H, COL_BLACK);

    // Draw spectrum bars (virtual rotation: display centered at LO, not VFO)
    for (int di = 0; di < num_bins; di++) {
        int x0 = bin_to_x(di);
        int x1 = bin_to_x(di + 1);
        if (x1 <= x0) x1 = x0 + 1;
        if (x0 >= log_w) break;

        int fft_bin = dsp::displayBin(di);
        float norm = (mag_db[fft_bin] - DB_MIN) / (DB_MAX - DB_MIN);
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        int bar_h = (int)(norm * SPEC_H);
        if (bar_h < 1) bar_h = 1;

        uint16_t color = spectrum_color(norm);
        int bar_w = x1 - x0;
        if (x0 + bar_w > log_w) bar_w = log_w - x0;
        draw::fillRect(fb, log_w, x0, spec_y + SPEC_H - bar_h, bar_w, bar_h, color);
    }
}

static void draw_waterfall(uint16_t *fb)
{
    int rows_to_draw = wf_count;
    if (rows_to_draw > wf_h) rows_to_draw = wf_h;

    // Clear waterfall area
    draw::fillRect(fb, log_w, 0, wf_y, log_w, wf_h, COL_BLACK);

    // Draw from newest (top) to oldest (bottom)
    for (int row = 0; row < rows_to_draw; row++) {
        int buf_idx = (wf_head - 1 - row + WF_MAX_LINES) % WF_MAX_LINES;
        const float *line = &wf_db[buf_idx * num_bins];
        int fy = wf_y + row;

        for (int di = 0; di < num_bins; di++) {
            int x0 = bin_to_x(di);
            int x1 = bin_to_x(di + 1);
            if (x1 <= x0) x1 = x0 + 1;
            if (x0 >= log_w) break;

            int fft_bin = dsp::displayBin(di);
            uint16_t color = waterfall_color_from_db(line[fft_bin]);
            for (int x = x0; x < x1 && x < log_w; x++)
                fb[fy * log_w + x] = color;
        }
    }
}

// Audio input callback — called from audio thread, pushes I/Q to DSP
static void audio_input_cb(const float *iq_samples, int num_frames)
{
    dsp::pushIQ(iq_samples, num_frames);
}

void app::init()
{
    auto info = pal::getDisplayInfo();
    log_w = info.width;
    log_h = info.height;

    wf_y = HEADER_H + GAP + SPEC_H + GAP;
    wf_h = log_h - wf_y - INFO_H;

    COL_NAVY   = draw::rgb565(0, 0, 128);
    COL_DGREY  = draw::rgb565(32, 32, 32);
    COL_BLACK  = draw::rgb565(0, 0, 0);
    COL_WHITE  = draw::rgb565(255, 255, 255);
    COL_CYAN   = draw::rgb565(0, 255, 255);
    COL_GREEN  = draw::rgb565(0, 255, 0);
    COL_YELLOW = draw::rgb565(255, 255, 0);
    COL_RED    = draw::rgb565(255, 0, 0);

    // DSP init
    dsp::init(SAMPLE_RATE, FFT_SIZE);
    num_bins = dsp::getNumBins();
    wf_db = new float[WF_MAX_LINES * num_bins]();
    last_dsp_time = pal::micros();

    // Open audio input (UAC on Tab5, PortAudio on desktop)
    pal::audioInputOpen(audio_input_cb);

    fps = 0;
    frame_count = 0;
    fps_last_time = pal::micros();
    touch_time = 0;
}

void app::tick()
{
    int64_t t0 = pal::micros();

    // FPS
    frame_count++;
    if (t0 - fps_last_time >= 1000000) {
        fps = (float)frame_count * 1000000.0f / (t0 - fps_last_time);
        frame_count = 0;
        fps_last_time = t0;
    }

    // DSP: process external input only (UAC audio)
    bool new_spectrum = dsp::processIfReady();
    if (new_spectrum) {
        last_dsp_time = t0;
        const float *mag = dsp::getMagnitudeDb();
        memcpy(&wf_db[wf_head * num_bins], mag, num_bins * sizeof(float));
        wf_head = (wf_head + 1) % WF_MAX_LINES;
        if (wf_count < WF_MAX_LINES) wf_count++;
    }

    // Poll touch events
    pal::TouchEvent evt;
    while (pal::pollEvent(evt)) {
        if (evt.action == pal::TouchEvent::DOWN) {
            touch_x = evt.x;
            touch_y = evt.y;
            snprintf(touch_text, sizeof(touch_text), "Touch @(%d,%d)", touch_x, touch_y);
            touch_time = t0;
        }
    }

    // --- Draw frame ---
    uint16_t *fb = pal::getFramebuffer();

    // Header
    draw::fillRect(fb, log_w, 0, 0, log_w, HEADER_H, COL_NAVY);
    // Gap
    draw::fillRect(fb, log_w, 0, HEADER_H, log_w, GAP, COL_BLACK);
    // Info bar
    draw::fillRect(fb, log_w, 0, log_h - INFO_H, log_w, INFO_H, COL_DGREY);

    // Spectrum (from live DSP data)
    draw_spectrum(fb, dsp::getMagnitudeDb());

    // VFO marker at +12kHz (LO offset) = 3/4 of display
    int vfo_x = log_w * 3 / 4;
    uint16_t COL_MARKER = draw::rgb565(255, 255, 255);
    draw::drawVLine(fb, log_w, vfo_x, HEADER_H + GAP, SPEC_H, COL_MARKER);

    // Gap
    draw::fillRect(fb, log_w, 0, HEADER_H + GAP + SPEC_H, log_w, GAP, COL_BLACK);

    // Waterfall
    draw_waterfall(fb);

    // VFO marker on waterfall too
    draw::drawVLine(fb, log_w, vfo_x, wf_y, wf_h, COL_MARKER);

    // Header text
    draw::drawText(fb, log_w, log_h, 8, 6, "QRP Companion", COL_WHITE, COL_NAVY, 2);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f FPS", fps);
    uint16_t fps_col = (fps >= 24) ? COL_GREEN : (fps >= 10) ? COL_YELLOW : COL_RED;
    int tw = draw::textWidth(buf, 2);
    draw::drawText(fb, log_w, log_h, log_w - tw - 8, 6, buf, fps_col, COL_NAVY, 2);

    // Labels
    draw::drawText(fb, log_w, log_h, 8, HEADER_H + GAP + 4, "SPECTRUM", COL_CYAN, COL_BLACK);
    snprintf(buf, sizeof(buf), "I/Q 48kHz/%d-pt CFFT  LO-24k..LO+24k",
             FFT_SIZE);
    tw = draw::textWidth(buf);
    draw::drawText(fb, log_w, log_h, log_w - tw - 8, HEADER_H + GAP + 4, buf, COL_DGREY, COL_BLACK);

    draw::drawText(fb, log_w, log_h, 8, wf_y + 4, "WATERFALL", COL_CYAN, COL_BLACK);

    // Touch tooltip
    if (touch_time > 0 && (t0 - touch_time) < 1000000) {
        int ttw = draw::textWidth(touch_text) + 8;
        int tx = touch_x - ttw / 2;
        int ty = touch_y - 20;
        if (tx < 0) tx = 0;
        if (tx + ttw > log_w) tx = log_w - ttw;
        if (ty < 0) ty = touch_y + 8;
        draw::fillRect(fb, log_w, tx, ty, ttw, 14, COL_DGREY);
        draw::drawRect(fb, log_w, tx, ty, ttw, 14, COL_WHITE);
        draw::drawText(fb, log_w, log_h, tx + 4, ty + 3, touch_text, COL_YELLOW, COL_DGREY);
    }

    // Info bar text
    int info_y = log_h - INFO_H;
    snprintf(buf, sizeof(buf), "draw:%.1f commit:%.1f total:%.1fms  bins:%d",
             t_draw_ms, t_commit_ms, t_total_ms, num_bins);
    draw::drawText(fb, log_w, log_h, 8, info_y + 4, buf, COL_WHITE, COL_DGREY);

    snprintf(buf, sizeof(buf), "%dx%d  heap:%dK  psram:%dK",
             log_w, log_h, pal::freeHeapKb(), pal::freePsramKb());
    draw::drawText(fb, log_w, log_h, 8, info_y + 16, buf, COL_WHITE, COL_DGREY);

    int64_t t1 = pal::micros();
    pal::commitFrame();
    int64_t t2 = pal::micros();

    t_draw_ms   = (t1 - t0) / 1000.0f;
    t_commit_ms = (t2 - t1) / 1000.0f;
    t_total_ms  = (t2 - t0) / 1000.0f;
}
