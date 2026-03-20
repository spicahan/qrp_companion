#include "app.h"
#include "pal.h"
#include "draw.h"
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

// Pre-computed background frame
static uint16_t *bg_frame = nullptr;

// FPS
static uint32_t frame_count;
static float fps;
static int64_t fps_last_time;
static float t_draw_ms, t_commit_ms, t_total_ms;

// Touch
static int64_t touch_time;
static int touch_x, touch_y;
static char touch_text[64];

static uint16_t spectrum_color(float norm)
{
    if (norm > 1.0f) norm = 1.0f;
    uint8_t r, g;
    if (norm < 0.5f) { r = (uint8_t)(norm * 2 * 255); g = 255; }
    else             { r = 255; g = (uint8_t)((1.0f - norm) * 2 * 255); }
    return draw::rgb565(r, g, 0);
}

static uint16_t waterfall_color(int intensity)
{
    if (intensity > 255) intensity = 255;
    if (intensity < 0) intensity = 0;
    uint8_t r, g, b;
    if (intensity < 85)       { r = 0; g = 0; b = intensity * 3; }
    else if (intensity < 170) { r = (intensity-85)*3; g = (intensity-85)*3; b = 255-(intensity-85)*3; }
    else                      { r = 255; g = 255-(intensity-170)*3; b = 0; }
    return draw::rgb565(r, g, b);
}

static void precompute_background()
{
    float peaks[]     = {0.15f, 0.35f, 0.50f, 0.72f, 0.88f};
    float widths[]    = {0.02f, 0.01f, 0.03f, 0.015f, 0.01f};
    float strengths[] = {80, 120, 60, 140, 90};

    // Header
    draw::fillRect(bg_frame, log_w, 0, 0, log_w, HEADER_H, COL_NAVY);

    // Gap
    int spec_start = HEADER_H + GAP;
    draw::fillRect(bg_frame, log_w, 0, HEADER_H, log_w, GAP, COL_BLACK);

    // Spectrum
    for (int x = 0; x < log_w; x++) {
        float f = (float)x / log_w;
        float v = 10.0f;
        for (int p = 0; p < 5; p++) {
            float dist = (f - peaks[p]) / widths[p];
            v += strengths[p] / (1.0f + dist * dist);
        }
        if (v > SPEC_H) v = SPEC_H;
        int bar = (int)v;
        uint16_t color = spectrum_color(v / SPEC_H);
        for (int y = 0; y < SPEC_H; y++) {
            int fy = spec_start + y;
            bg_frame[fy * log_w + x] = (SPEC_H - 1 - y < bar) ? color : COL_BLACK;
        }
    }

    // Gap
    draw::fillRect(bg_frame, log_w, 0, spec_start + SPEC_H, log_w, GAP, COL_BLACK);

    // Waterfall
    for (int row = 0; row < wf_h; row++) {
        float fade = 1.0f - (float)row / wf_h;
        int fy = wf_y + row;
        for (int x = 0; x < log_w; x++) {
            float f = (float)x / log_w;
            float v = 10.0f;
            for (int p = 0; p < 5; p++) {
                float dist = (f - peaks[p]) / widths[p];
                v += strengths[p] / (1.0f + dist * dist);
            }
            int intensity = (int)(v / SPEC_H * 255 * fade);
            bg_frame[fy * log_w + x] = waterfall_color(intensity);
        }
    }

    // Info bar
    draw::fillRect(bg_frame, log_w, 0, log_h - INFO_H, log_w, INFO_H, COL_DGREY);
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

    bg_frame = new uint16_t[log_w * log_h];
    precompute_background();

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

    // Copy background to framebuffer
    uint16_t *fb = pal::getFramebuffer();
    memcpy(fb, bg_frame, log_w * log_h * sizeof(uint16_t));

    // Header text
    draw::drawText(fb, log_w, log_h, 8, 6, "QRP Companion", COL_WHITE, COL_NAVY, 2);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f FPS", fps);
    uint16_t fps_col = (fps >= 24) ? COL_GREEN : (fps >= 10) ? COL_YELLOW : COL_RED;
    int tw = draw::textWidth(buf, 2);
    draw::drawText(fb, log_w, log_h, log_w - tw - 8, 6, buf, fps_col, COL_NAVY, 2);

    // Labels
    draw::drawText(fb, log_w, log_h, 8, HEADER_H + GAP + 4, "SPECTRUM", COL_CYAN, COL_BLACK);
    draw::drawText(fb, log_w, log_h, 8, wf_y + 4, "WATERFALL", COL_CYAN, COL_BLACK);

    // Touch event tooltip
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

    // Info bar
    int info_y = log_h - INFO_H;
    snprintf(buf, sizeof(buf), "draw:%.1f commit:%.1f total:%.1fms",
             t_draw_ms, t_commit_ms, t_total_ms);
    draw::drawText(fb, log_w, log_h, 8, info_y + 4, buf, COL_WHITE, COL_DGREY);

    snprintf(buf, sizeof(buf), "%dx%d  heap:%dK  psram:%dK",
             log_w, log_h, pal::freeHeapKb(), pal::freePsramKb());
    draw::drawText(fb, log_w, log_h, 8, info_y + 16, buf, COL_WHITE, COL_DGREY);

    int64_t t1 = pal::micros();

    // Push to screen
    pal::commitFrame();

    int64_t t2 = pal::micros();

    t_draw_ms   = (t1 - t0) / 1000.0f;
    t_commit_ms = (t2 - t1) / 1000.0f;
    t_total_ms  = (t2 - t0) / 1000.0f;
}
