#include "pal.h"
#include "uac_host.h"

#include <M5Unified.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_timer.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <cstring>

static constexpr int PHYS_W = 720;
static constexpr int PHYS_H = 1280;

static int log_w, log_h;
static uint8_t *phys_fb = nullptr;

// Touch event queue
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
    dsp.setBrightness(128);

    log_w = width;
    log_h = height;

    // Get physical framebuffer — draw directly to it (no intermediate buffer)
    auto panel = static_cast<lgfx::Panel_DSI*>(dsp.getPanel());
    phys_fb = (uint8_t*)panel->config_detail().buffer;

    return phys_fb != nullptr;
}

void shutdown() {}

DisplayInfo getDisplayInfo() { return { log_w, log_h }; }

uint16_t* getFramebuffer() { return (uint16_t*)phys_fb; }

int getFramebufferStride() { return PHYS_W; }

bool isRotated() { return true; }

void commitFrame()
{
    // Just cache flush — no rotate_blit needed, drawing was done directly
    esp_cache_msync(phys_fb, PHYS_W * PHYS_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

bool pollEvent(TouchEvent &evt)
{
    M5.update();

    auto count = M5.Touch.getCount();
    if (count > 0) {
        auto detail = M5.Touch.getDetail(0);
        if (detail.wasPressed()) {
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

    if (evt_head != evt_tail) {
        evt = evt_queue[evt_tail];
        evt_tail = (evt_tail + 1) % EVT_QUEUE_SIZE;
        return true;
    }
    return false;
}

int64_t micros() { return esp_timer_get_time(); }
void delayMs(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
int freeHeapKb() { return (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024); }
int freePsramKb() { return (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024); }

// Audio input via UAC host
static AudioInputCallback s_audio_cb = nullptr;

bool audioInputOpen(AudioInputCallback cb) { s_audio_cb = cb; uac_host_start(); return true; }
void audioInputClose() { s_audio_cb = nullptr; }

} // namespace pal

extern "C" void uac_push_audio_samples(const float *samples, int count)
{
    if (pal::s_audio_cb) pal::s_audio_cb(samples, count);
}
