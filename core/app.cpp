#include "app.h"
#include "pal.h"
#include "draw.h"
#include "dsp.h"
#include "cat.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// Layout constants (logical landscape)
static constexpr int HEADER_H = 28;
static constexpr int SPEC_H   = 180;
static constexpr int GAP      = 2;
static constexpr int INFO_H   = 28;

static int log_w, log_h;
static int spec_w;        // spectrum/waterfall width (1024 to match FFT)
static int slider_x;      // gain slider left edge
static constexpr int SLIDER_W = 128;
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

// Touch / drag-to-tune
static int64_t touch_time;
static int touch_x, touch_y;
static char touch_text[64];
static bool  dragging = false;
static int   drag_start_x;          // screen X at touch DOWN
static uint64_t drag_start_freq;    // VFO freq at touch DOWN
static int   drag_current_x;        // latest drag X
static bool  drag_vfo_dirty = false; // true if drag moved since last FFT sync

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
    return bin * spec_w / num_bins;
}

static int g_vfo_x;  // VFO marker x position
static uint16_t COL_MARKER;

static void draw_spectrum(const float *mag_db)
{
    int spec_y = HEADER_H + GAP;

    // Clear spectrum area (only spec_w, not full log_w)
    draw::fillRect(fb, 0, spec_y, spec_w, SPEC_H, COL_BLACK);

    // Draw spectrum bars with VFO marker
    for (int di = 0; di < num_bins; di++) {
        int x0 = bin_to_x(di);
        int x1 = bin_to_x(di + 1);
        if (x1 <= x0) x1 = x0 + 1;
        if (x0 >= spec_w) break;

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
    if (g_pixel_to_fftbin) delete[] g_pixel_to_fftbin;
    g_pixel_to_fftbin = new int[spec_w];
    for (int x = 0; x < spec_w; x++) {
        int di = x * num_bins / spec_w;
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
                           n, spec_w, true);
        } else {
            // Wrap around: two blitBlock calls
            int first = WF_MAX_LINES - start;
            int second = n - first;
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, start, 0,
                           fb.buf, fb.phys_w, wf_y, 0,
                           first, spec_w, true);
            pal::blitBlock(wf_pixels_t, WF_MAX_LINES, 0, 0,
                           fb.buf, fb.phys_w, wf_y + first, 0,
                           second, spec_w, true);
        }

        if (n < wf_h) {
            for (int py = 0; py < spec_w; py++)
                memset(&fb.buf[py * fb.phys_w + wf_y + n], 0, (wf_h - n) * sizeof(uint16_t));
        }
    } else {
        // Non-rotated (desktop): each logical row = one time step
        for (int row = 0; row < n; row++) {
            int fy = wf_y + row;
            int time_idx = (wf_head + row) % WF_MAX_LINES;
            uint16_t *dst = &fb.buf[fy * fb.phys_w];
            for (int x = 0; x < spec_w; x++)
                dst[x] = wf_pixels_t[x * WF_MAX_LINES + time_idx];
        }
        if (n < wf_h)
            draw::fillRect(fb, 0, wf_y + n, log_w, wf_h - n, COL_BLACK);
    }
}

// Gain slider
static constexpr float GAIN_MIN = 100.0f;
static constexpr float GAIN_MAX = 10000.0f;
static bool gain_dragging = false;

static float gain_from_y(int y)
{
    int sy = HEADER_H + GAP;
    int sh = SPEC_H + GAP + (log_h - (HEADER_H + GAP + SPEC_H + GAP) - INFO_H);
    float norm = 1.0f - (float)(y - sy) / sh;
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;
    float log_min = log10f(GAIN_MIN);
    float log_max = log10f(GAIN_MAX);
    return powf(10.0f, log_min + norm * (log_max - log_min));
}

static void draw_gain_slider()
{
    int sy = HEADER_H + GAP;
    int sh = SPEC_H + GAP + wf_h;
    int sx = slider_x;

    draw::fillRect(fb, sx, sy, SLIDER_W, sh, COL_DGREY);
    draw::drawVLine(fb, sx + SLIDER_W / 2, sy + 4, sh - 8, COL_BLACK);

    float gain = dsp::getAudioGain();
    float log_min = log10f(GAIN_MIN);
    float log_max = log10f(GAIN_MAX);
    float log_gain = log10f(gain > GAIN_MIN ? gain : GAIN_MIN);
    float norm = (log_gain - log_min) / (log_max - log_min);
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;

    int bar_y = sy + (int)((1.0f - norm) * (sh - 10));
    draw::fillRect(fb, sx + 8, bar_y, SLIDER_W - 16, 6, COL_WHITE);

    char lbl[16];
    int gain_db = (int)(20.0f * log10f(gain) + 0.5f);
    snprintf(lbl, sizeof(lbl), "%ddB", gain_db);
    draw::drawText(fb, sx + 4, bar_y - 12, lbl, COL_GREEN, COL_DGREY);
}

// Audio output callback — receives decimated mono samples from DSP, forwards to PAL
static void audio_output_cb(const float *samples, int count)
{
    pal::audioOutputWrite(samples, count);
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

    spec_w = 1024;  // match FFT size for 1:1 bin-to-pixel
    slider_x = log_w - SLIDER_W;

    wf_y = HEADER_H + GAP + SPEC_H + GAP;
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
    wf_pixels_t = new uint16_t[spec_w * WF_MAX_LINES]();  // transposed pixel cache
    last_dsp_time = pal::micros();

    g_vfo_x = log_w * 3 / 4;
    COL_MARKER = draw::rgb565(255, 255, 255);
    precompute_pixel_bin_map();

    // Open audio input (UAC on Tab5, PortAudio on desktop)
    // Audio output (decimated 6kHz mono from DSP)
    pal::audioOutputOpen(dsp::getDecimatedRate());
    dsp::setAudioOutCallback(audio_output_cb);

    pal::audioInputOpen(audio_input_cb);
    cat::init();

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

    // --- CAT ---
    cat::poll();

    // Update CW offset NCO when mode/offset changes
    static int last_mode = 0;
    static int last_cw_offset = 0;
    int cur_mode = cat::getMode();
    int cur_cw_offset = cat::getCwOffset();
    if (cur_mode != last_mode || cur_cw_offset != last_cw_offset) {
        dsp::setSoftNcoCorrection(0);  // reset on mode/offset change
        if (cur_mode == 3 && cur_cw_offset > 0) {
            dsp::setCwOffset((float)cur_cw_offset);
        } else {
            dsp::setCwOffset(0);
        }
        last_mode = cur_mode;
        last_cw_offset = cur_cw_offset;
    }

    // Update VFO marker position based on span (within spec_w area)
    if (dsp::getSpan() == 0) {
        g_vfo_x = spec_w * 3 / 4;  // 48kHz span: VFO at 3/4
    } else {
        g_vfo_x = spec_w / 2;      // narrow spans: VFO at center
    }

    // --- Goertzel fine-tune result ---
    if (!dsp::isGoertzelRunning() && dsp::getGoertzelResult() != 0) {
        float g_offset = dsp::getGoertzelResult();
        dsp::clearGoertzelResult();
        uint64_t vfo = cat::getVfoFreq();
        if (vfo > 0) {
            // Round to nearest 10Hz for VFO CAT command
            int rounded = ((int)(g_offset + (g_offset >= 0 ? 5.0f : -5.0f))) / 10 * 10;
            int vfo_delta = -rounded;
            // Soft NCO correction = the sub-10Hz residual
            float soft_hz = -(g_offset - (float)rounded);

            if (vfo_delta != 0) {
                uint64_t new_freq = (int64_t)vfo + vfo_delta;
                cat::setVfoFreq(new_freq);
            }
            dsp::setSoftNcoCorrection(soft_hz);

            snprintf(touch_text, sizeof(touch_text),
                     "Fine %+.0fHz VFO%+d NCO%+.1f",
                     g_offset, vfo_delta, soft_hz);
            touch_time = t0;
        }
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
        for (int lx = 0; lx < spec_w; lx++) {
            int fft_bin = g_pixel_to_fftbin[lx];
            uint16_t color = (lx == g_vfo_x) ? COL_MARKER
                           : waterfall_color_from_db(mag[fft_bin]);
            wf_pixels_t[lx * WF_MAX_LINES + wf_head] = color;
        }

        if (wf_count < WF_MAX_LINES) wf_count++;
    }
    int64_t t_dsp = pal::micros();

    // Poll touch events — tap-to-tune (on release) + drag-to-tune
    static constexpr int TAP_THRESHOLD = 5;  // pixels — below this is a tap, above is drag

    pal::TouchEvent evt;
    while (pal::pollEvent(evt)) {
        touch_x = evt.x;
        touch_y = evt.y;
        touch_time = t0;

        if (evt.action == pal::TouchEvent::DOWN) {
            // Check header buttons
            if (evt.y < HEADER_H) {
                if (evt.x < 120) {
                    // Span toggle button
                    dsp::setSpan((dsp::getSpan() + 1) % dsp::NUM_SPANS);
                    precompute_pixel_bin_map();
                    continue;
                }
                // APF toggle button (right side of header, CW mode only)
                if (evt.x > log_w - 80 && cat::getMode() == 3) {
                    dsp::setApfEnabled(!dsp::isApfEnabled());
                    continue;
                }
            }
            // Gain slider area
            if (evt.x >= slider_x) {
                gain_dragging = true;
                dsp::setAudioGain(gain_from_y(evt.y));
                continue;
            }
            // Reset soft NCO correction on new tune action
            dsp::setSoftNcoCorrection(0);
            dragging = true;
            drag_start_x = evt.x;
            drag_start_freq = cat::getVfoFreq();
            drag_current_x = evt.x;
            drag_vfo_dirty = false;
            cat::suppressPolling(true);
        }
        else if (evt.action == pal::TouchEvent::MOVE && gain_dragging) {
            dsp::setAudioGain(gain_from_y(evt.y));
        }
        else if (evt.action == pal::TouchEvent::MOVE && dragging) {
            drag_current_x = evt.x;
            drag_vfo_dirty = true;
        }
        else if (evt.action == pal::TouchEvent::UP && gain_dragging) {
            gain_dragging = false;
        }
        else if (evt.action == pal::TouchEvent::UP && dragging) {
            int total_drag = drag_current_x - drag_start_x;
            if (total_drag < 0) total_drag = -total_drag;

            if (total_drag < TAP_THRESHOLD && drag_start_freq > 0) {
                // Tap-to-tune: coarse jump VFO to tapped frequency
                int delta_px = evt.x - g_vfo_x;
                int delta_hz = delta_px * dsp::getSpanRate() / spec_w;
                uint64_t new_freq = (int64_t)drag_start_freq + delta_hz;
                if (new_freq > 0) {
                    cat::setVfoFreq(new_freq);
                    snprintf(touch_text, sizeof(touch_text),
                             "%+dHz -> %d.%03d.%02d",
                             delta_hz,
                             (int)(new_freq / 1000000),
                             (int)((new_freq % 1000000) / 1000),
                             (int)((new_freq % 1000) / 10));
                    // In CW mode, start Goertzel fine-tune
                    if (cat::getMode() == 3)
                        dsp::startGoertzel();
                }
            } else if (drag_vfo_dirty && drag_start_freq > 0) {
                // Final drag update
                int delta_px = drag_current_x - drag_start_x;
                int delta_hz = -delta_px * dsp::getSpanRate() / spec_w;
                uint64_t new_freq = (int64_t)drag_start_freq + delta_hz;
                if (new_freq > 0)
                    cat::setVfoFreq(new_freq);
            }
            dragging = false;
            drag_vfo_dirty = false;
            cat::suppressPolling(false);
        }
    }

    // Sync drag VFO update with FFT (throttle to FFT rate, not touch rate)
    if (dragging && drag_vfo_dirty && new_spectrum && drag_start_freq > 0) {
        int delta_px = drag_current_x - drag_start_x;
        int delta_hz = -delta_px * dsp::getSpanRate() / spec_w;
        uint64_t new_freq = (int64_t)drag_start_freq + delta_hz;
        if (new_freq > 0) {
            cat::setVfoFreq(new_freq);
            snprintf(touch_text, sizeof(touch_text),
                     "%+dHz -> %d.%03d.%03d",
                     delta_hz,
                     (int)(new_freq / 1000000),
                     (int)((new_freq % 1000000) / 1000),
                     (int)(new_freq % 1000));
        }
        drag_vfo_dirty = false;
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
    draw::fillRect(fb, 0, HEADER_H + GAP + SPEC_H, spec_w, GAP, COL_BLACK);
    draw_waterfall();

    // Gain slider (right side)
    draw_gain_slider();
    int64_t t_wf = pal::micros();

    // HUD text — span button on left (fixed width to avoid residuals)
    char span_btn[16];
    snprintf(span_btn, sizeof(span_btn), "%-9s", dsp::getSpanLabel());
    draw::drawText(fb, 8, 6, span_btn, COL_CYAN, COL_NAVY, 2);

    // Mode + VFO frequency centered in header (fixed width to avoid residuals)
    char buf[80];
    uint64_t vfo = cat::getVfoFreq();
    if (vfo > 0) {
        int mhz  = (int)(vfo / 1000000);
        int khz  = (int)((vfo % 1000000) / 1000);
        int hz10 = (int)((vfo % 1000) / 10);
        float soft = dsp::getSoftNcoCorrection();
        if (soft != 0.0f && cat::getMode() == 3) {
            snprintf(buf, sizeof(buf), "%-4s %d.%03d.%02d%+.1f",
                     cat::getModeStr(), mhz, khz, hz10, soft);
        } else {
            snprintf(buf, sizeof(buf), "%-4s %d.%03d.%02d",
                     cat::getModeStr(), mhz, khz, hz10);
        }
    } else {
        snprintf(buf, sizeof(buf), "%-4s ---.---.--", cat::getModeStr());
    }
    // Pad to fixed 24 chars to overwrite any residuals
    int blen = strlen(buf);
    while (blen < 24) buf[blen++] = ' ';
    buf[blen] = '\0';
    int freq_tw = draw::textWidth(buf, 2);
    draw::drawText(fb, (log_w - freq_tw) / 2, 6, buf, COL_GREEN, COL_NAVY, 2);

    // APF indicator + FPS right-aligned
    if (cat::getMode() == 3) {
        const char *filt_label = dsp::isApfEnabled() ? "APF" : "CW ";
        uint16_t filt_col = dsp::isApfEnabled() ? COL_YELLOW : COL_DGREY;
        snprintf(buf, sizeof(buf), "%s %4.0fFPS", filt_label, fps);
    } else {
        snprintf(buf, sizeof(buf), "%4.0fFPS", fps);
    }
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
