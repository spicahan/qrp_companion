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
static draw::Framebuf fb;

// Colors
static uint16_t COL_NAVY, COL_DGREY, COL_BLACK, COL_WHITE;
static uint16_t COL_CYAN, COL_GREEN, COL_YELLOW, COL_RED;

// FPS
static uint32_t frame_count;
static float fps;
static int64_t fps_last_time;

// Per-phase timing (ms)
static float t_dsp_ms, t_spec_ms, t_wf_ms, t_hud_ms, t_commit_ms, t_total_ms;

// Touch
static int64_t touch_time;
static int touch_x, touch_y;
static char touch_text[64];

// DSP
static constexpr int SAMPLE_RATE = 48000;
static constexpr int FFT_SIZE    = 1024;
static int64_t last_dsp_time;

// Waterfall ring buffer
static constexpr int WF_MAX_LINES = 512;
static float *wf_db = nullptr;     // WF_MAX_LINES * num_bins (dB values)
static uint16_t *wf_pixels_t = nullptr; // transposed pixel cache: [log_w][WF_MAX_LINES]
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

static int g_vfo_x;  // VFO marker x position
static uint16_t COL_MARKER;

static void draw_spectrum(const float *mag_db)
{
    int spec_y = HEADER_H + GAP;

    // Clear entire spectrum area first to avoid residuals
    draw::fillRect(fb, 0, spec_y, log_w, SPEC_H, COL_BLACK);

    // Draw spectrum bars with VFO marker
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

        int bar_w = x1 - x0;
        if (x0 + bar_w > log_w) bar_w = log_w - x0;

        uint16_t color = spectrum_color(norm);
        draw::fillRect(fb, x0, spec_y + SPEC_H - bar_h, bar_w, bar_h, color);
    }

    // VFO marker (single column overwrite — fast, minimal flicker)
    draw::drawVLine(fb, g_vfo_x, spec_y, SPEC_H, COL_MARKER);

}

// Pre-computed bin index for each logical x pixel (avoids repeated bin_to_x + displayBin)
static int *g_pixel_to_fftbin = nullptr;

static void precompute_pixel_bin_map()
{
    g_pixel_to_fftbin = new int[log_w];
    for (int x = 0; x < log_w; x++) {
        // Which display bin does this pixel belong to?
        int di = x * num_bins / log_w;
        if (di >= num_bins) di = num_bins - 1;
        g_pixel_to_fftbin[x] = dsp::displayBin(di);
    }
}

static void draw_waterfall()
{
    if (wf_count == 0) return;

    int n = wf_count;
    if (n > wf_h) n = wf_h;

    if (fb.rotated) {
        // wf_pixels_t layout: [log_w rows][WF_MAX_LINES cols], one row per freq bin.
        // Physical row py reads from src row (log_w-1-py) → need mirror_y.
        // Ring buffer: forward from wf_head = newest first.
        // PPA blitBlock with mirror_y handles the Y-flip in hardware.

        int start = wf_head;
        if (start + n <= WF_MAX_LINES) {
            // Single contiguous region — one blitBlock call
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, start, 0,
                           fb.buf, fb.phys_w, wf_y, 0,
                           n, log_w, true);
        } else {
            // Wrap around: two blitBlock calls
            int first = WF_MAX_LINES - start;
            int second = n - first;
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, start, 0,
                           fb.buf, fb.phys_w, wf_y, 0,
                           first, log_w, true);
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, 0, 0,
                           fb.buf, fb.phys_w, wf_y + first, 0,
                           second, log_w, true);
        }

        if (n < wf_h) {
            for (int py = 0; py < log_w; py++)
                memset(&fb.buf[py * fb.phys_w + wf_y + n], 0, (wf_h - n) * sizeof(uint16_t));
        }
    } else {
        // Non-rotated (desktop): each logical row = one time step
        for (int row = 0; row < n; row++) {
            int fy = wf_y + row;
            int time_idx = (wf_head + row) % WF_MAX_LINES;
            uint16_t *dst = &fb.buf[fy * fb.phys_w];
            for (int x = 0; x < log_w; x++)
                dst[x] = wf_pixels_t[x * WF_MAX_LINES + time_idx];
        }
        if (n < wf_h)
            draw::fillRect(fb, 0, wf_y + n, log_w, wf_h - n, COL_BLACK);
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
    // Align wf_y to 32-pixel (cache line) boundary to prevent PPA DMA
    // cache invalidation from clobbering CPU-written spectrum data
    wf_y = (wf_y + 31) & ~31;
    wf_h = log_h - wf_y - INFO_H;

    // Set up framebuffer context
    fb.buf    = pal::getFramebuffer();
    fb.phys_w = pal::getFramebufferStride();
    fb.phys_h = pal::isRotated() ? log_w : log_h;  // physical height
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

    // DSP init
    dsp::init(SAMPLE_RATE, FFT_SIZE);
    num_bins = dsp::getNumBins();
    wf_db = new float[WF_MAX_LINES * num_bins]();
    wf_pixels_t = new uint16_t[log_w * WF_MAX_LINES]();  // transposed pixel cache
    last_dsp_time = pal::micros();

    g_vfo_x = log_w * 3 / 4;
    COL_MARKER = draw::rgb565(255, 255, 255);
    precompute_pixel_bin_map();

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

    // --- DSP ---
    bool new_spectrum = dsp::processIfReady();
    if (new_spectrum) {
        last_dsp_time = t0;
        const float *mag = dsp::getMagnitudeDb();
        memcpy(&wf_db[wf_head * num_bins], mag, num_bins * sizeof(float));

        // Decrement head FIRST, then write — forward reads from head give newest-first
        wf_head = (wf_head - 1 + WF_MAX_LINES) % WF_MAX_LINES;

        // Pre-render into transposed pixel cache [lx][WF_MAX_LINES]
        for (int lx = 0; lx < log_w; lx++) {
            int fft_bin = g_pixel_to_fftbin[lx];
            uint16_t color = (lx == g_vfo_x) ? COL_MARKER
                           : waterfall_color_from_db(mag[fft_bin]);
            wf_pixels_t[lx * WF_MAX_LINES + wf_head] = color;
        }

        if (wf_count < WF_MAX_LINES) wf_count++;
    }
    int64_t t_dsp = pal::micros();

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
    static bool first_frame = true;
    if (first_frame) {
        int total = fb.rotated ? (fb.phys_w * fb.phys_h) : (fb.log_w * fb.log_h);
        for (int i = 0; i < total; i++) fb.buf[i] = COL_BLACK;
        draw::fillRect(fb, 0, 0, log_w, HEADER_H, COL_NAVY);
        draw::fillRect(fb, 0, HEADER_H, log_w, GAP, COL_BLACK);
        draw::fillRect(fb, 0, log_h - INFO_H, log_w, INFO_H, COL_DGREY);
        first_frame = false;
    }

    // Spectrum (includes black fill above bars + VFO marker)
    draw_spectrum(dsp::getMagnitudeDb());
    int64_t t_spec = pal::micros();

    // Gap + Waterfall (includes VFO marker per-row)
    draw::fillRect(fb, 0, HEADER_H + GAP + SPEC_H, log_w, GAP, COL_BLACK);
    draw_waterfall();
    int64_t t_wf = pal::micros();

    // HUD text
    draw::drawText(fb, 8, 6, "QRP", COL_WHITE, COL_NAVY, 2);

    // Mode + VFO frequency centered in header
    char buf[80];
    int mode = pal::getMode();
    const char *mode_str = "---";
    switch (mode) {
        case 1: mode_str = "LSB";  break;
        case 2: mode_str = "USB";  break;
        case 3: mode_str = "CW";   break;
        case 6: mode_str = "DIGI"; break;
    }

    uint64_t vfo = pal::getVfoFreq();
    if (vfo > 0) {
        int mhz = (int)(vfo / 1000000);
        int khz = (int)((vfo % 1000000) / 1000);
        int hz  = (int)(vfo % 1000);
        snprintf(buf, sizeof(buf), "%s %d.%03d.%03d", mode_str, mhz, khz, hz);
    } else {
        snprintf(buf, sizeof(buf), "%s ---.---.---", mode_str);
    }
    int freq_tw = draw::textWidth(buf, 2);
    draw::drawText(fb, (log_w - freq_tw) / 2, 6, buf, COL_GREEN, COL_NAVY, 2);

    // FPS right-aligned
    snprintf(buf, sizeof(buf), "%5.1f FPS", fps);
    uint16_t fps_col = (fps >= 24) ? COL_GREEN : (fps >= 10) ? COL_YELLOW : COL_RED;
    int tw = draw::textWidth(buf, 2);
    draw::drawText(fb, log_w - tw - 8, 6, buf, fps_col, COL_NAVY, 2);


    if (touch_time > 0 && (t0 - touch_time) < 1000000) {
        int ttw = draw::textWidth(touch_text) + 8;
        int tx = touch_x - ttw / 2;
        int ty = touch_y - 20;
        if (tx < 0) tx = 0;
        if (tx + ttw > log_w) tx = log_w - ttw;
        if (ty < 0) ty = touch_y + 8;
        draw::fillRect(fb, tx, ty, ttw, 14, COL_DGREY);
        draw::drawRect(fb, tx, ty, ttw, 14, COL_WHITE);
        draw::drawText(fb, tx + 4, ty + 3, touch_text, COL_YELLOW, COL_DGREY);
    }

    // Info bar text — pad to fixed width, bg overwrites old text
    int info_y = log_h - INFO_H;
    snprintf(buf, sizeof(buf), "dsp:%4.1f spec:%4.1f wf:%5.1f hud:%4.1f commit:%4.1f total:%5.1fms",
             t_dsp_ms, t_spec_ms, t_wf_ms, t_hud_ms, t_commit_ms, t_total_ms);
    draw::drawText(fb, 8, info_y + 4, buf, COL_WHITE, COL_DGREY);

    snprintf(buf, sizeof(buf), "%dx%d  heap:%dK  psram:%dK   ",
             log_w, log_h, pal::freeHeapKb(), pal::freePsramKb());
    draw::drawText(fb, 8, info_y + 16, buf, COL_WHITE, COL_DGREY);

    int64_t t_hud = pal::micros();

    pal::commitFrame();
    int64_t t_end = pal::micros();

    t_dsp_ms    = (t_dsp  - t0)    / 1000.0f;
    t_spec_ms   = (t_spec - t_dsp) / 1000.0f;
    t_wf_ms     = (t_wf   - t_spec)/ 1000.0f;
    t_hud_ms    = (t_hud  - t_wf)  / 1000.0f;
    t_commit_ms = (t_end  - t_hud) / 1000.0f;
    t_total_ms  = (t_end  - t0)    / 1000.0f;
}
