#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static void draw_test(M5GFX &display)
{
    int w = display.width();
    int h = display.height();

    // Clear to black
    display.fillScreen(TFT_BLACK);

    // Draw a header bar
    display.fillRect(0, 0, w, 40, TFT_NAVY);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.print("QRP Companion - Tab5 Graphics Test");

    // Draw color gradient bars (simulating a spectrum display)
    int bar_y = 60;
    int bar_h = 80;
    for (int x = 0; x < w; x++) {
        // HSV-like rainbow gradient
        uint8_t r, g, b;
        int hue = (x * 360) / w;
        if (hue < 60)       { r = 255; g = hue * 255 / 60;       b = 0; }
        else if (hue < 120) { r = (120 - hue) * 255 / 60; g = 255; b = 0; }
        else if (hue < 180) { r = 0; g = 255; b = (hue - 120) * 255 / 60; }
        else if (hue < 240) { r = 0; g = (240 - hue) * 255 / 60; b = 255; }
        else if (hue < 300) { r = (hue - 240) * 255 / 60; g = 0;  b = 255; }
        else                { r = 255; g = 0; b = (360 - hue) * 255 / 60; }
        uint16_t color = display.color565(r, g, b);
        display.drawFastVLine(x, bar_y, bar_h, color);
    }

    // Label for gradient
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(1);
    display.setCursor(10, bar_y + bar_h + 5);
    display.print("Rainbow gradient (spectrum color palette test)");

    // Draw simulated spectrum bars
    int spec_y = bar_y + bar_h + 30;
    int spec_h = 150;
    display.drawRect(0, spec_y, w, spec_h, TFT_DARKGREY);

    // Fake spectrum data
    for (int x = 0; x < w; x += 2) {
        // Generate a "signal" with peaks
        float f = (float)x / w;
        int amplitude = 10;
        // Add some peaks to simulate signals
        float peaks[] = {0.15f, 0.35f, 0.5f, 0.72f, 0.88f};
        float widths[] = {0.02f, 0.01f, 0.03f, 0.015f, 0.01f};
        int strengths[] = {80, 120, 60, 140, 90};
        for (int p = 0; p < 5; p++) {
            float dist = (f - peaks[p]) / widths[p];
            amplitude += (int)(strengths[p] / (1.0f + dist * dist));
        }
        if (amplitude > spec_h - 2) amplitude = spec_h - 2;

        // Color based on intensity (green -> yellow -> red)
        uint8_t r, g;
        if (amplitude < spec_h / 2) {
            r = amplitude * 255 / (spec_h / 2);
            g = 255;
        } else {
            r = 255;
            g = 255 - (amplitude - spec_h / 2) * 255 / (spec_h / 2);
        }
        uint16_t color = display.color565(r, g, 0);
        display.drawFastVLine(x, spec_y + spec_h - 1 - amplitude, amplitude, color);
    }

    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(10, spec_y + spec_h + 5);
    display.print("Simulated RF spectrum with signal peaks");

    // Draw waterfall preview (horizontal color bands fading)
    int wf_y = spec_y + spec_h + 30;
    int wf_h = 120;
    display.drawRect(0, wf_y, w, wf_h, TFT_DARKGREY);

    for (int row = 0; row < wf_h - 2; row++) {
        for (int x = 1; x < w - 1; x += 2) {
            float f = (float)x / w;
            int intensity = 0;
            float peaks[] = {0.15f, 0.35f, 0.5f, 0.72f, 0.88f};
            float widths[] = {0.02f, 0.01f, 0.03f, 0.015f, 0.01f};
            int strengths[] = {80, 120, 60, 140, 90};
            for (int p = 0; p < 5; p++) {
                float dist = (f - peaks[p]) / widths[p];
                intensity += (int)(strengths[p] / (1.0f + dist * dist));
            }
            // Fade with row (simulate waterfall scrolling)
            intensity = intensity * (wf_h - row) / wf_h;
            if (intensity > 255) intensity = 255;

            // Blue-to-yellow-to-red colormap
            uint8_t r, g, b;
            if (intensity < 85) {
                r = 0; g = 0; b = intensity * 3;
            } else if (intensity < 170) {
                r = (intensity - 85) * 3; g = (intensity - 85) * 3; b = 255 - (intensity - 85) * 3;
            } else {
                r = 255; g = 255 - (intensity - 170) * 3; b = 0;
            }
            uint16_t color = display.color565(r, g, b);
            display.drawFastVLine(x, wf_y + 1 + row, 1, color);
        }
    }

    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(10, wf_y + wf_h + 5);
    display.print("Simulated waterfall display");

    // Draw basic shapes test
    int shape_y = wf_y + wf_h + 30;
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(1);
    display.setCursor(10, shape_y);
    display.print("Shape primitives:");

    int sx = 10;
    shape_y += 15;
    display.drawRect(sx, shape_y, 60, 40, TFT_RED);
    display.fillRect(sx + 5, shape_y + 5, 50, 30, TFT_DARKGREEN);
    sx += 80;

    display.drawCircle(sx + 20, shape_y + 20, 20, TFT_YELLOW);
    display.fillCircle(sx + 20, shape_y + 20, 10, TFT_ORANGE);
    sx += 60;

    display.drawTriangle(sx, shape_y + 40, sx + 20, shape_y, sx + 40, shape_y + 40, TFT_MAGENTA);
    sx += 60;

    display.drawRoundRect(sx, shape_y, 60, 40, 8, TFT_CYAN);
    sx += 80;

    display.drawLine(sx, shape_y, sx + 60, shape_y + 40, TFT_GREEN);
    display.drawLine(sx, shape_y + 40, sx + 60, shape_y, TFT_GREEN);

    // Display info at bottom
    int info_y = h - 50;
    display.fillRect(0, info_y, w, 50, TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.setTextSize(1);
    display.setCursor(10, info_y + 5);
    display.printf("Display: %d x %d  Board: %d  Color depth: %d bit",
                   w, h, (int)M5.getBoard(), display.getColorDepth());
    display.setCursor(10, info_y + 25);
    display.print("Touch the screen to continue...");
}

static void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    auto &display = M5.Display;

    // Use landscape orientation
    if (display.width() < display.height()) {
        display.setRotation(1);
    }

    display.setBrightness(128);
    display.startWrite();
    draw_test(display);
    display.endWrite();
}

static void loop()
{
    M5.update();
    vTaskDelay(pdMS_TO_TICKS(10));
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
