#include <M5Unified.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_cache.h>
#include <string.h>

// Physical display: 720×1280 portrait (no rotation!)
static int phys_w, phys_h;
static uint32_t fb_size;
static uint8_t *framebuffer;      // Direct pointer to display framebuffer in PSRAM

// Pre-computed full frame
static uint16_t *frame_data;      // 720×1280 pre-rendered pixels in PSRAM

// Layout constants (portrait: 720 wide, 1280 tall)
static constexpr int HEADER_H = 28;
static constexpr int SPEC_H   = 240;
static constexpr int GAP      = 2;
static constexpr int INFO_H   = 28;
static int wf_y, wf_h;

// FPS
static uint32_t frame_count;
static float fps;
static int64_t fps_last_time;
static float t_copy_ms, t_flush_ms, t_hud_ms, t_total_ms;

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
        for (int x = 0; x < phys_w; x++)
            frame_data[y * phys_w + x] = navy;

    // Gap (black)
    int spec_start = HEADER_H + GAP;
    for (int y = HEADER_H; y < spec_start; y++)
        for (int x = 0; x < phys_w; x++)
            frame_data[y * phys_w + x] = 0;

    // Spectrum
    for (int x = 0; x < phys_w; x++) {
        float f = (float)x / phys_w;
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
            frame_data[fy * phys_w + x] = (SPEC_H - 1 - y < bar) ? color : 0;
        }
    }

    // Gap
    for (int y = spec_start + SPEC_H; y < wf_y; y++)
        for (int x = 0; x < phys_w; x++)
            frame_data[y * phys_w + x] = 0;

    // Waterfall
    for (int row = 0; row < wf_h; row++) {
        float fade = 1.0f - (float)row / wf_h;
        int fy = wf_y + row;
        for (int x = 0; x < phys_w; x++) {
            float f = (float)x / phys_w;
            float v = 10.0f;
            for (int p = 0; p < 5; p++) {
                float dist = (f - peaks[p]) / widths[p];
                v += strengths[p] / (1.0f + dist * dist);
            }
            int intensity = (int)(v / SPEC_H * 255 * fade);
            frame_data[fy * phys_w + x] = waterfall_color(intensity);
        }
    }

    // Info bar (dark grey)
    uint16_t dgrey = lgfx::color565(32, 32, 32);
    int info_start = phys_h - INFO_H;
    for (int y = info_start; y < phys_h; y++)
        for (int x = 0; x < phys_w; x++)
            frame_data[y * phys_w + x] = dgrey;
}

static void draw_frame()
{
    int64_t t0, t1, t2, t3;

    // FPS
    t0 = esp_timer_get_time();
    frame_count++;
    if (t0 - fps_last_time >= 1000000) {
        fps = (float)frame_count * 1000000.0f / (t0 - fps_last_time);
        frame_count = 0;
        fps_last_time = t0;
    }

    // --- Blit entire pre-computed frame to framebuffer ---
    memcpy(framebuffer, frame_data, fb_size);
    t1 = esp_timer_get_time();

    // --- Cache flush so DMA sees the new data ---
    esp_cache_msync(framebuffer, fb_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    t2 = esp_timer_get_time();

    // --- HUD text overlay via M5GFX (no rotation = fast memcpy path) ---
    auto &dsp = M5.Display;
    dsp.startWrite();

    // Header text
    dsp.setTextColor(TFT_WHITE, lgfx::color565(0, 0, 128));
    dsp.setTextSize(2);
    dsp.setCursor(8, 6);
    dsp.print("QRP Companion");

    uint16_t fps_color = (fps >= 24) ? TFT_GREEN : (fps >= 10) ? TFT_YELLOW : TFT_RED;
    dsp.setTextColor(fps_color, lgfx::color565(0, 0, 128));
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f FPS", fps);
    dsp.setCursor(phys_w - dsp.textWidth(buf) - 8, 6);
    dsp.print(buf);

    // Labels
    dsp.setTextColor(TFT_CYAN, TFT_BLACK);
    dsp.setTextSize(1);
    dsp.setCursor(8, HEADER_H + GAP + 4);
    dsp.print("SPECTRUM");
    dsp.setCursor(8, wf_y + 4);
    dsp.print("WATERFALL");

    // Info bar text
    int info_y = phys_h - INFO_H;
    dsp.setTextColor(TFT_WHITE, lgfx::color565(32, 32, 32));
    dsp.setCursor(8, info_y + 4);
    dsp.printf("copy:%.1f flush:%.1f hud:%.1f total:%.1fms",
               t_copy_ms, t_flush_ms, t_hud_ms, t_total_ms);
    dsp.setCursor(8, info_y + 16);
    dsp.printf("%dx%d  heap:%dK  psram:%dK",
               phys_w, phys_h,
               (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
               (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    dsp.endWrite();
    t3 = esp_timer_get_time();

    t_copy_ms  = (t1 - t0) / 1000.0f;
    t_flush_ms = (t2 - t1) / 1000.0f;
    t_hud_ms   = (t3 - t2) / 1000.0f;
    t_total_ms = (t3 - t0) / 1000.0f;
}

static void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    auto &dsp = M5.Display;
    // NO rotation — use native portrait 720×1280
    dsp.setBrightness(128);

    phys_w = dsp.width();    // 720
    phys_h = dsp.height();   // 1280
    fb_size = phys_w * phys_h * 2;  // RGB565

    // Layout
    wf_y = HEADER_H + GAP + SPEC_H + GAP;
    wf_h = phys_h - wf_y - INFO_H;

    // Get direct framebuffer pointer from Panel_DSI
    auto panel = static_cast<lgfx::Panel_DSI*>(dsp.getPanel());
    framebuffer = (uint8_t*)panel->config_detail().buffer;

    // Pre-compute full frame in PSRAM
    frame_data = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    precompute_frame();

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
