#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

static M5Canvas canvas;      // Off-screen sprite for double-buffered drawing
static uint32_t frame_count;
static float fps;
static int64_t fps_last_time;

// Fake spectrum signal generator with slowly drifting peaks
static void calc_spectrum(float *out, int bins, int64_t time_us)
{
    float t = time_us / 1000000.0f;
    float peaks[]    = {0.15f, 0.35f, 0.50f, 0.72f, 0.88f};
    float widths[]   = {0.02f, 0.01f, 0.03f, 0.015f, 0.01f};
    float strengths[] = {80, 120, 60, 140, 90};
    int n_peaks = 5;

    for (int i = 0; i < bins; i++) {
        float f = (float)i / bins;
        float v = 10.0f;
        for (int p = 0; p < n_peaks; p++) {
            // Drift peaks slowly over time
            float center = peaks[p] + 0.02f * sinf(t * (0.3f + p * 0.17f));
            float dist = (f - center) / widths[p];
            v += strengths[p] / (1.0f + dist * dist);
        }
        // Add some noise
        uint32_t hash = (uint32_t)(i * 2654435761u + (uint32_t)(t * 1000));
        v += (float)(hash & 0xFF) / 64.0f;
        out[i] = v;
    }
}

static uint16_t spectrum_color(float amplitude, float max_val)
{
    float norm = amplitude / max_val;
    if (norm > 1.0f) norm = 1.0f;
    uint8_t r, g;
    if (norm < 0.5f) {
        r = (uint8_t)(norm * 2 * 255);
        g = 255;
    } else {
        r = 255;
        g = (uint8_t)((1.0f - norm) * 2 * 255);
    }
    return lgfx::color565(r, g, 0);
}

static uint16_t waterfall_color(float amplitude, float max_val)
{
    int intensity = (int)(amplitude / max_val * 255);
    if (intensity > 255) intensity = 255;
    if (intensity < 0) intensity = 0;
    uint8_t r, g, b;
    if (intensity < 85) {
        r = 0; g = 0; b = intensity * 3;
    } else if (intensity < 170) {
        r = (intensity - 85) * 3; g = (intensity - 85) * 3; b = 255 - (intensity - 85) * 3;
    } else {
        r = 255; g = 255 - (intensity - 170) * 3; b = 0;
    }
    return lgfx::color565(r, g, b);
}

// Waterfall history buffer
static constexpr int WF_MAX_ROWS = 200;
static uint16_t *wf_linebuf = nullptr;  // WF_MAX_ROWS * screen_width, ring buffer
static int wf_head = 0;
static int wf_width = 0;

static void draw_frame(M5GFX &display)
{
    int64_t now = esp_timer_get_time();
    int w = canvas.width();
    int h = canvas.height();

    // --- FPS calculation (update once per second) ---
    frame_count++;
    int64_t elapsed = now - fps_last_time;
    if (elapsed >= 1000000) {
        fps = (float)frame_count * 1000000.0f / elapsed;
        frame_count = 0;
        fps_last_time = now;
    }

    canvas.fillScreen(TFT_BLACK);

    // Header
    canvas.fillRect(0, 0, w, 32, TFT_NAVY);
    canvas.setTextColor(TFT_WHITE, TFT_NAVY);
    canvas.setTextSize(2);
    canvas.setCursor(8, 8);
    canvas.print("QRP Companion - Graphics Test");

    // FPS display (right-aligned in header)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f FPS", fps);
        int tw = canvas.textWidth(buf);
        canvas.setCursor(w - tw - 8, 8);
        // Color-code: green > 30, yellow > 15, red otherwise
        uint16_t fps_color = (fps > 30) ? TFT_GREEN : (fps > 15) ? TFT_YELLOW : TFT_RED;
        canvas.setTextColor(fps_color, TFT_NAVY);
        canvas.print(buf);
    }

    // --- Spectrum ---
    int spec_y = 40;
    int spec_h = 180;
    canvas.drawRect(0, spec_y, w, spec_h, TFT_DARKGREY);

    float spectrum[1280];
    int bins = w;
    calc_spectrum(spectrum, bins, now);

    float max_val = (float)spec_h;
    for (int x = 0; x < bins; x++) {
        float amp = spectrum[x];
        if (amp > max_val) amp = max_val;
        int bar = (int)amp;
        if (bar < 1) bar = 1;
        uint16_t color = spectrum_color(amp, max_val);
        canvas.drawFastVLine(x, spec_y + spec_h - 1 - bar, bar, color);
    }

    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setTextSize(1);
    canvas.setCursor(8, spec_y + 4);
    canvas.print("SPECTRUM");

    // --- Waterfall ---
    int wf_y = spec_y + spec_h + 4;
    int wf_h = h - wf_y - 40;
    if (wf_h > WF_MAX_ROWS) wf_h = WF_MAX_ROWS;

    // Initialize waterfall buffer on first call
    if (wf_linebuf == nullptr || wf_width != w) {
        free(wf_linebuf);
        wf_width = w;
        wf_linebuf = (uint16_t *)heap_caps_calloc(WF_MAX_ROWS * w, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        wf_head = 0;
    }

    // Write new spectrum line into ring buffer
    uint16_t *new_line = &wf_linebuf[wf_head * w];
    for (int x = 0; x < bins; x++) {
        new_line[x] = waterfall_color(spectrum[x], max_val);
    }
    wf_head = (wf_head + 1) % wf_h;

    // Draw waterfall from ring buffer (newest at top)
    for (int row = 0; row < wf_h; row++) {
        int buf_idx = (wf_head - 1 - row + wf_h) % wf_h;
        canvas.pushImage(0, wf_y + row, w, 1, &wf_linebuf[buf_idx * w]);
    }

    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setCursor(8, wf_y + 4);
    canvas.print("WATERFALL");

    // --- Info bar ---
    int info_y = h - 32;
    canvas.fillRect(0, info_y, w, 32, 0x2104); // dark grey
    canvas.setTextColor(TFT_WHITE, 0x2104);
    canvas.setTextSize(1);
    canvas.setCursor(8, info_y + 4);
    canvas.printf("Display: %dx%d  Board: %d  Depth: %dbit  Heap: %dK free",
                  w, h, (int)M5.getBoard(), display.getColorDepth(),
                  (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    // Frame time stats
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Frame: %.1fms", fps > 0 ? 1000.0f / fps : 0);
        int tw = canvas.textWidth(buf);
        canvas.setCursor(w - tw - 8, info_y + 4);
        canvas.print(buf);
    }

    canvas.setCursor(8, info_y + 18);
    canvas.printf("PSRAM: %dK free  Uptime: %ds",
                  (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  (int)(now / 1000000));

    // Push canvas to display
    canvas.pushSprite(&display, 0, 0);
}

static void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    auto &display = M5.Display;

    if (display.width() < display.height()) {
        display.setRotation(1);
    }

    display.setBrightness(128);

    // Create off-screen canvas in PSRAM for double-buffered rendering
    canvas.setPsram(true);
    canvas.createSprite(display.width(), display.height());

    fps = 0;
    frame_count = 0;
    fps_last_time = esp_timer_get_time();
}

static void loop()
{
    M5.update();
    M5.Display.startWrite();
    draw_frame(M5.Display);
    M5.Display.endWrite();
}

extern "C" void app_main(void)
{
    xTaskCreatePinnedToCore(
        [](void *) {
            setup();
            for (;;) { loop(); }
        },
        "main_task", 8192, nullptr, 1, nullptr, 1);
}
