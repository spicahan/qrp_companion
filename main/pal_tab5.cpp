#include "pal.h"

#include <M5Unified.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_timer.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <cstring>

static constexpr int PHYS_W = 720;
static constexpr int PHYS_H = 1280;

static int log_w, log_h;
static uint16_t *logical_fb = nullptr;   // landscape buffer (core draws here)
static uint8_t  *phys_fb    = nullptr;   // physical DSI framebuffer

static constexpr int TILE = 32;

// 90° CW rotation: logical landscape → physical portrait
static void rotate_blit()
{
    const uint16_t *src = logical_fb;
    uint16_t *dst = (uint16_t *)phys_fb;

    for (int ty = 0; ty < log_h; ty += TILE) {
        int th = (ty + TILE <= log_h) ? TILE : log_h - ty;
        for (int tx = 0; tx < log_w; tx += TILE) {
            int tw = (tx + TILE <= log_w) ? TILE : log_w - tx;
            for (int i = 0; i < th; i++) {
                int log_y = ty + i;
                int phys_x = log_y;
                for (int j = 0; j < tw; j++) {
                    int log_x = tx + j;
                    int phys_y = log_w - 1 - log_x;
                    dst[phys_y * PHYS_W + phys_x] = src[log_y * log_w + log_x];
                }
            }
        }
    }
}

// Touch event queue (simple ring buffer)
static constexpr int EVT_QUEUE_SIZE = 16;
static pal::TouchEvent evt_queue[EVT_QUEUE_SIZE];
static int evt_head = 0, evt_tail = 0;

static void evt_push(const pal::TouchEvent &e)
{
    int next = (evt_head + 1) % EVT_QUEUE_SIZE;
    if (next != evt_tail) {
        evt_queue[evt_head] = e;
        evt_head = next;
    }
}

namespace pal {

bool init(int width, int height)
{
    auto cfg = M5.config();
    M5.begin(cfg);

    auto &dsp = M5.Display;
    // No M5GFX rotation — we rotate ourselves
    dsp.setBrightness(128);

    log_w = width;
    log_h = height;

    // Get physical framebuffer
    auto panel = static_cast<lgfx::Panel_DSI*>(dsp.getPanel());
    phys_fb = (uint8_t*)panel->config_detail().buffer;

    // Allocate logical landscape framebuffer in PSRAM
    logical_fb = (uint16_t*)heap_caps_malloc(log_w * log_h * 2, MALLOC_CAP_SPIRAM);

    return phys_fb && logical_fb;
}

void shutdown() {}

DisplayInfo getDisplayInfo()
{
    return { log_w, log_h };
}

uint16_t* getFramebuffer()
{
    return logical_fb;
}

void commitFrame()
{
    rotate_blit();
    esp_cache_msync(phys_fb, PHYS_W * PHYS_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

bool pollEvent(TouchEvent &evt)
{
    // Update M5 state (reads touch hardware)
    M5.update();

    // Check for new touch events
    auto count = M5.Touch.getCount();
    if (count > 0) {
        auto detail = M5.Touch.getDetail(0);
        if (detail.wasPressed()) {
            // Physical portrait → logical landscape
            int lx = log_w - 1 - detail.y;
            int ly = detail.x;
            evt_push({lx, ly, TouchEvent::DOWN});
        }
        if (detail.wasReleased()) {
            int lx = log_w - 1 - detail.y;
            int ly = detail.x;
            evt_push({lx, ly, TouchEvent::UP});
        }
    }

    // Dequeue
    if (evt_head != evt_tail) {
        evt = evt_queue[evt_tail];
        evt_tail = (evt_tail + 1) % EVT_QUEUE_SIZE;
        return true;
    }
    return false;
}

int64_t micros()
{
    return esp_timer_get_time();
}

void delayMs(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

int freeHeapKb()
{
    return (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}

int freePsramKb()
{
    return (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

} // namespace pal
