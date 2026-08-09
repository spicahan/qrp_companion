#include "pal.h"
#include "uac_host.h"
#include "cat_host.h"
#include "pc_link.h"

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

// Call once per frame to read touch hardware and queue events
static void updateTouch()
{
    M5.update();

    auto count = M5.Touch.getCount();
    if (count > 0) {
        auto detail = M5.Touch.getDetail(0);
        int lx = log_w - 1 - detail.y;
        int ly = detail.x;
        if (detail.wasPressed()) {
            evt_push({lx, ly, TouchEvent::DOWN});
        } else if (detail.isPressed()) {
            evt_push({lx, ly, TouchEvent::MOVE});
        }
        if (detail.wasReleased()) {
            evt_push({lx, ly, TouchEvent::UP});
        }
    }
}

// First call per frame reads hardware; subsequent calls only drain queue
static bool s_touch_read_this_frame = false;

bool pollEvent(TouchEvent &evt)
{
    if (!s_touch_read_this_frame) {
        updateTouch();
        s_touch_read_this_frame = true;
    }

    if (evt_head != evt_tail) {
        evt = evt_queue[evt_tail];
        evt_tail = (evt_tail + 1) % EVT_QUEUE_SIZE;
        return true;
    }

    // Queue drained — reset flag for next frame
    s_touch_read_this_frame = false;
    return false;
}

void injectEvent(const TouchEvent &evt) { evt_push(evt); }

// Battery (M5Unified Tab5: INA226 bus voltage → 2S Li-Po SoC, CHG_STAT pin)
static int  s_battery_level = -1;
static bool s_battery_charging = false;

void pollBattery()
{
    int32_t lvl = M5.Power.getBatteryLevel();
    s_battery_level = (lvl < 0) ? -1 : (int)lvl;
    // Charging detection: use battery current sign rather than CHG_STAT pin.
    // The charger IC's STAT line can linger HIGH for a moment after unplug,
    // but the INA226 shunt current immediately reflects actual power flow.
    // Tab5 INA226 polarity: negative when charging, positive when discharging
    // (opposite of the M5Unified docstring; verified empirically).
    int32_t i_ma = M5.Power.getBatteryCurrent();
    s_battery_charging = (i_ma < -30);  // 30 mA threshold ignores topping-up noise
}

int  getBatteryLevel() { return s_battery_level; }
bool isCharging()      { return s_battery_charging; }

// PC link (USB CDC device on USB-C) — see main/pc_link.h
bool pcLinkSupported() { return true; }
bool pcLinkRunning()   { return pc_link_running(); }
void pcLinkStart()     { pc_link_start(); }

int64_t micros() { return esp_timer_get_time(); }
void delayMs(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
int freeHeapKb() { return (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024); }
int freePsramKb() { return (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024); }

bool catIsConnected() { return cat_host_is_connected(); }
int  catSend(const char *data, int len) { return cat_host_send(data, len); }
int  catRecv(char *buf, int max_len) { return cat_host_recv(buf, max_len); }

void blitBlock(const uint16_t *src, int src_stride, int src_x, int src_y,
               uint16_t *dst, int dst_stride, int dst_x, int dst_y,
               int width, int height, bool mirror_y)
{
    if (s_ppa_srm_client && width > 0 && height > 0) {
        // Flush source to PSRAM so PPA DMA can see it
        esp_cache_msync((void *)&src[src_y * src_stride],
                        (size_t)height * src_stride * sizeof(uint16_t),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        // Flush+invalidate destination so PPA DMA doesn't conflict with CPU cache
        // This prevents stale CPU cache lines from overwriting PPA output on later flush
        esp_cache_msync((void *)&dst[dst_y * dst_stride],
                        (size_t)height * dst_stride * sizeof(uint16_t),
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
        if (err == ESP_OK) {
            // Invalidate destination cache so CPU sees PPA's DMA writes
            // Note: M2C direction doesn't allow UNALIGNED flag on ESP32-P4
            esp_cache_msync((void *)&dst[dst_y * dst_stride],
                            (size_t)height * dst_stride * sizeof(uint16_t),
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            return;
        }
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

// Audio output via M5.Speaker
static int s_audio_out_rate = 0;
static bool s_audio_muted = false;

bool audioOutputOpen(int sample_rate)
{
    s_audio_out_rate = sample_rate;
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = 48000;
    M5.Speaker.config(spk_cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(128);
    return M5.Speaker.isEnabled();
}

// M5.Speaker.playRaw stores a POINTER to the data — the buffer must remain
// valid until the Speaker finishes reading it. Use 3 rotating buffers:
// one being played, one queued, one being written.
static constexpr int SPKR_BUF_SIZE = 256;
static constexpr int SPKR_NUM_BUFS = 3;
static int16_t s_spkr_bufs[SPKR_NUM_BUFS][SPKR_BUF_SIZE];
static int s_spkr_buf_idx = 0;

void audioOutputWrite(const float *samples, int num_frames)
{
    if (s_audio_out_rate == 0 || num_frames <= 0) return;
    int remaining = num_frames;
    const float *p = samples;
    while (remaining > 0) {
        int chunk = remaining > SPKR_BUF_SIZE ? SPKR_BUF_SIZE : remaining;
        int16_t *buf = s_spkr_bufs[s_spkr_buf_idx];
        for (int i = 0; i < chunk; i++) {
            float s = s_audio_muted ? 0.0f : p[i];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            buf[i] = (int16_t)(s * 32767.0f);
        }
        M5.Speaker.playRaw(buf, chunk, s_audio_out_rate, false, 1, 0, false);
        s_spkr_buf_idx = (s_spkr_buf_idx + 1) % SPKR_NUM_BUFS;
        p += chunk;
        remaining -= chunk;
    }
}

void audioSetMuted(bool muted) { s_audio_muted = muted; }
bool audioIsMuted() { return s_audio_muted; }

void audioOutputClose()
{
    M5.Speaker.stop();
    s_audio_out_rate = 0;
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
