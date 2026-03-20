#include <M5Unified.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_cache.h>
#include <string.h>

// Physical display: 720×1280 portrait framebuffer
// Logical display: 1280×720 landscape (we rotate ourselves)
static constexpr int PHYS_W = 720;
static constexpr int PHYS_H = 1280;
static constexpr int LOG_W  = 1280;  // landscape width
static constexpr int LOG_H  = 720;   // landscape height

static uint32_t fb_size;
static uint8_t *framebuffer;      // Direct pointer to display framebuffer

// Pre-computed frame in logical landscape layout (1280×720)
static uint16_t *frame_logical;

// Layout constants (landscape: 1280 wide, 720 tall)
static constexpr int HEADER_H = 28;
static constexpr int SPEC_H   = 180;
static constexpr int GAP      = 2;
static constexpr int INFO_H   = 28;
static int wf_y, wf_h;

// FPS
static uint32_t frame_count;
static float fps;
static int64_t fps_last_time;
static float t_rot_ms, t_flush_ms, t_hud_ms, t_total_ms;

static uint16_t spectrum_color(float norm)
{
    if (norm > 1.0f) norm = 1.0f;
    uint8_t r, g;
    if (norm < 0.5f) { r = (uint8_t)(norm * 2 * 255); g = 255; }
    else             { r = 255; g = (uint8_t)((1.0f - norm) * 2 * 255); }
    return lgfx::color565(r, g, 0);
}

static uint16_t waterfall_color(int intensity)
{
    if (intensity > 255) intensity = 255;
    if (intensity < 0) intensity = 0;
    uint8_t r, g, b;
    if (intensity < 85)       { r = 0; g = 0; b = intensity * 3; }
    else if (intensity < 170) { r = (intensity-85)*3; g = (intensity-85)*3; b = 255-(intensity-85)*3; }
    else                      { r = 255; g = 255-(intensity-170)*3; b = 0; }
    return lgfx::color565(r, g, b);
}

static void precompute_frame()
{
    float peaks[]     = {0.15f, 0.35f, 0.50f, 0.72f, 0.88f};
    float widths[]    = {0.02f, 0.01f, 0.03f, 0.015f, 0.01f};
    float strengths[] = {80, 120, 60, 140, 90};

    // Header (navy)
    uint16_t navy = lgfx::color565(0, 0, 128);
    for (int y = 0; y < HEADER_H; y++)
        for (int x = 0; x < LOG_W; x++)
            frame_logical[y * LOG_W + x] = navy;

    // Gap
    int spec_start = HEADER_H + GAP;
    for (int y = HEADER_H; y < spec_start; y++)
        for (int x = 0; x < LOG_W; x++)
            frame_logical[y * LOG_W + x] = 0;

    // Spectrum
    for (int x = 0; x < LOG_W; x++) {
        float f = (float)x / LOG_W;
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
            frame_logical[fy * LOG_W + x] = (SPEC_H - 1 - y < bar) ? color : 0;
        }
    }

    // Gap
    for (int y = spec_start + SPEC_H; y < wf_y; y++)
        for (int x = 0; x < LOG_W; x++)
            frame_logical[y * LOG_W + x] = 0;

    // Waterfall
    for (int row = 0; row < wf_h; row++) {
        float fade = 1.0f - (float)row / wf_h;
        int fy = wf_y + row;
        for (int x = 0; x < LOG_W; x++) {
            float f = (float)x / LOG_W;
            float v = 10.0f;
            for (int p = 0; p < 5; p++) {
                float dist = (f - peaks[p]) / widths[p];
                v += strengths[p] / (1.0f + dist * dist);
            }
            int intensity = (int)(v / SPEC_H * 255 * fade);
            frame_logical[fy * LOG_W + x] = waterfall_color(intensity);
        }
    }

    // Info bar
    uint16_t dgrey = lgfx::color565(32, 32, 32);
    int info_start = LOG_H - INFO_H;
    for (int y = info_start; y < LOG_H; y++)
        for (int x = 0; x < LOG_W; x++)
            frame_logical[y * LOG_W + x] = dgrey;
}

// Rotate-blit from logical landscape (1280×720) to physical portrait (720×1280).
// 90° CW: phys_x = log_y, phys_y = LOG_W - 1 - log_x
//   log_y  ranges [0, LOG_H)  = [0, 720)  → phys_x [0, 720)  ✓ fits PHYS_W
//   log_x  ranges [0, LOG_W)  = [0, 1280) → phys_y [0, 1280) ✓ fits PHYS_H
//
// Tile-based for cache friendliness.
static constexpr int TILE = 32;

static void rotate_blit()
{
    const uint16_t *src = frame_logical;
    uint16_t *dst = (uint16_t *)framebuffer;

    for (int ty = 0; ty < LOG_H; ty += TILE) {
        int th = (ty + TILE <= LOG_H) ? TILE : LOG_H - ty;
        for (int tx = 0; tx < LOG_W; tx += TILE) {
            int tw = (tx + TILE <= LOG_W) ? TILE : LOG_W - tx;
            for (int i = 0; i < th; i++) {
                int log_y = ty + i;
                int phys_x = log_y;
                for (int j = 0; j < tw; j++) {
                    int log_x = tx + j;
                    int phys_y = LOG_W - 1 - log_x;
                    dst[phys_y * PHYS_W + phys_x] = src[log_y * LOG_W + log_x];
                }
            }
        }
    }
}

// HUD text: render into a small sprite, then rotate-blit it to framebuffer
static M5Canvas hud_sprite;

static void blit_hud_rotated(int log_x, int log_y, int w, int h)
{
    const uint16_t *src = (const uint16_t *)hud_sprite.getBuffer();
    uint16_t *dst = (uint16_t *)framebuffer;

    for (int sy = 0; sy < h; sy++) {
        int ly = log_y + sy;
        int px = ly;  // phys_x = log_y
        for (int sx = 0; sx < w; sx++) {
            int lx = log_x + sx;
            int py = LOG_W - 1 - lx;  // phys_y = LOG_W - 1 - log_x
            dst[py * PHYS_W + px] = src[sy * w + sx];
        }
    }
}

static void draw_frame()
{
    int64_t t0, t1, t2, t3;

    t0 = esp_timer_get_time();
    frame_count++;
    if (t0 - fps_last_time >= 1000000) {
        fps = (float)frame_count * 1000000.0f / (t0 - fps_last_time);
        frame_count = 0;
        fps_last_time = t0;
    }

    // --- Rotate-blit landscape frame to portrait framebuffer ---
    rotate_blit();
    t1 = esp_timer_get_time();

    // --- HUD text via sprite + rotated blit ---
    // Header text
    hud_sprite.createSprite(LOG_W, HEADER_H);
    hud_sprite.fillSprite(lgfx::color565(0, 0, 128));
    hud_sprite.setTextColor(TFT_WHITE);
    hud_sprite.setTextSize(2);
    hud_sprite.setCursor(8, 6);
    hud_sprite.print("QRP Companion - Landscape");

    uint16_t fps_color = (fps >= 24) ? TFT_GREEN : (fps >= 10) ? TFT_YELLOW : TFT_RED;
    hud_sprite.setTextColor(fps_color);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f FPS", fps);
    hud_sprite.setCursor(LOG_W - hud_sprite.textWidth(buf) - 8, 6);
    hud_sprite.print(buf);
    blit_hud_rotated(0, 0, LOG_W, HEADER_H);

    // Info bar
    hud_sprite.createSprite(LOG_W, INFO_H);
    hud_sprite.fillSprite(lgfx::color565(32, 32, 32));
    hud_sprite.setTextColor(TFT_WHITE);
    hud_sprite.setTextSize(1);
    hud_sprite.setCursor(8, 4);
    hud_sprite.printf("rot:%.1f flush:%.1f hud:%.1f total:%.1fms",
                      t_rot_ms, t_flush_ms, t_hud_ms, t_total_ms);
    hud_sprite.setCursor(8, 16);
    hud_sprite.printf("%dx%d(logical)  heap:%dK  psram:%dK",
                      LOG_W, LOG_H,
                      (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    blit_hud_rotated(0, LOG_H - INFO_H, LOG_W, INFO_H);

    // Labels
    hud_sprite.createSprite(80, 12);
    hud_sprite.fillSprite(TFT_BLACK);
    hud_sprite.setTextColor(TFT_CYAN);
    hud_sprite.setTextSize(1);
    hud_sprite.setCursor(0, 0);
    hud_sprite.print("SPECTRUM");
    blit_hud_rotated(8, HEADER_H + GAP + 4, 80, 12);

    hud_sprite.fillSprite(TFT_BLACK);
    hud_sprite.setCursor(0, 0);
    hud_sprite.print("WATERFALL");
    blit_hud_rotated(8, wf_y + 4, 80, 12);

    t2 = esp_timer_get_time();

    // --- Cache flush ---
    esp_cache_msync(framebuffer, fb_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    t3 = esp_timer_get_time();

    t_rot_ms   = (t1 - t0) / 1000.0f;
    t_hud_ms   = (t2 - t1) / 1000.0f;
    t_flush_ms = (t3 - t2) / 1000.0f;
    t_total_ms = (t3 - t0) / 1000.0f;
}

static void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    auto &dsp = M5.Display;
    // NO M5GFX rotation — we handle rotation ourselves
    dsp.setBrightness(128);

    fb_size = PHYS_W * PHYS_H * 2;

    // Layout (logical landscape)
    wf_y = HEADER_H + GAP + SPEC_H + GAP;
    wf_h = LOG_H - wf_y - INFO_H;

    // Get direct framebuffer pointer
    auto panel = static_cast<lgfx::Panel_DSI*>(dsp.getPanel());
    framebuffer = (uint8_t*)panel->config_detail().buffer;

    // Allocate logical landscape frame in PSRAM
    frame_logical = (uint16_t*)heap_caps_malloc(LOG_W * LOG_H * 2, MALLOC_CAP_SPIRAM);
    precompute_frame();

    // HUD sprite (reused, small)
    hud_sprite.setPsram(true);
    hud_sprite.setColorDepth(16);

    fps = 0;
    frame_count = 0;
    fps_last_time = esp_timer_get_time();
}

static void loop()
{
    M5.update();
    draw_frame();
}

extern "C" void app_main(void)
{
    xTaskCreatePinnedToCore(
        [](void *) {
            setup();
            for (;;) { loop(); }
        },
        "main_task", 32768, nullptr, 1, nullptr, 1);
}
