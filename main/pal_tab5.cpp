#include "pal.h"
#include "uac_host.h"
#include "cat_host.h"

#include <M5Unified.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_timer.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <driver/ppa.h>
#include <cstring>

static constexpr int PHYS_W = 720;
static constexpr int PHYS_H = 1280;

static int log_w, log_h;
static uint8_t *phys_fb = nullptr;

// PPA (Pixel Processing Accelerator) for hardware 2D copy
static ppa_client_handle_t s_ppa_srm_client = nullptr;

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

    // Initialize PPA for hardware 2D copy
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t ppa_err = ppa_register_client(&ppa_cfg, &s_ppa_srm_client);
    if (ppa_err != ESP_OK) s_ppa_srm_client = nullptr;

    return phys_fb != nullptr;
}

void shutdown() {
    if (s_ppa_srm_client) { ppa_unregister_client(s_ppa_srm_client); s_ppa_srm_client = nullptr; }
}

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

uint64_t getVfoFreq() { return cat_get_vfo_freq(); }

void blitBlock(const uint16_t *src, int src_stride, int src_x, int src_y,
               uint16_t *dst, int dst_stride, int dst_x, int dst_y,
               int width, int height, bool mirror_y)
{
    if (s_ppa_srm_client && width > 0 && height > 0) {
        // Flush source to PSRAM so PPA DMA can see it
        esp_cache_msync((void *)&src[src_y * src_stride],
                        (size_t)height * src_stride * sizeof(uint16_t),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        ppa_srm_oper_config_t srm_cfg = {};
        srm_cfg.in.buffer = src;
        srm_cfg.in.pic_w = src_stride;
        srm_cfg.in.pic_h = src_y + height;
        srm_cfg.in.block_w = width;
        srm_cfg.in.block_h = height;
        srm_cfg.in.block_offset_x = src_x;
        srm_cfg.in.block_offset_y = src_y;
        srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

        srm_cfg.out.buffer = dst;
        srm_cfg.out.buffer_size = (size_t)(dst_y + height) * dst_stride * sizeof(uint16_t);
        srm_cfg.out.pic_w = dst_stride;
        srm_cfg.out.pic_h = dst_y + height;
        srm_cfg.out.block_offset_x = dst_x;
        srm_cfg.out.block_offset_y = dst_y;
        srm_cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

        srm_cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        srm_cfg.scale_x = 1.0f;
        srm_cfg.scale_y = 1.0f;
        srm_cfg.mirror_y = mirror_y;
        srm_cfg.mode = PPA_TRANS_MODE_BLOCKING;

        esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm_client, &srm_cfg);
        if (err == ESP_OK) return;
        // Fall through to CPU copy on error
    }

    // CPU fallback
    for (int y = 0; y < height; y++) {
        int sy = mirror_y ? (src_y + height - 1 - y) : (src_y + y);
        memcpy(&dst[(dst_y + y) * dst_stride + dst_x],
               &src[sy * src_stride + src_x],
               width * sizeof(uint16_t));
    }
}

// Audio input via UAC host
static AudioInputCallback s_audio_cb = nullptr;

bool audioInputOpen(AudioInputCallback cb) { s_audio_cb = cb; uac_host_start(); return true; }
void audioInputClose() { s_audio_cb = nullptr; }

} // namespace pal

extern "C" void uac_push_audio_samples(const float *samples, int count)
{
    if (pal::s_audio_cb) pal::s_audio_cb(samples, count);
}
