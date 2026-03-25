#include "pal.h"
#include "uac_host.h"

#include <M5Unified.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_timer.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <cstring>
#include <cstdio>

// ESP-IDF private includes for DMA link list patching
#include "esp_lcd_panel_interface.h"
#include "esp_private/dw_gdma.h"
#include "soc/reg_base.h"

static constexpr int PHYS_W = 720;
static constexpr int PHYS_H = 1280;
static constexpr int WF_EXTRA = 512;
static constexpr int EXT_H = PHYS_H + WF_EXTRA;

// Minimal mirror of esp_lcd_dpi_panel_t (ESP-IDF v5.5.1)
// Validated at runtime by checking fbs[0] matches known framebuffer.
struct dpi_panel_compat_t {
    esp_lcd_panel_t base;
    esp_lcd_dsi_bus_handle_t bus;
    uint8_t virtual_channel;
    uint8_t cur_fb_index;
    uint8_t num_fbs;
    uint8_t *fbs[3];
    uint32_t h_pixels;
    uint32_t v_pixels;
    size_t fb_size;
    size_t bits_per_pixel;
    uint32_t in_color_format;
    uint32_t out_color_format;
    dw_gdma_channel_handle_t dma_chan;
    dw_gdma_link_list_handle_t link_lists[3];
};

// Access Panel_DSI's protected _disp_panel_handle
struct Panel_DSI_Accessor : lgfx::Panel_DSI {
    esp_lcd_panel_handle_t getHandle() const { return _disp_panel_handle; }
};

static int log_w, log_h;
static uint8_t *ext_fb = nullptr;
static uint8_t *orig_dpi_fb = nullptr;
static int visible_offset = WF_EXTRA;

// DMA patching
static dw_gdma_link_list_handle_t s_dma_link_list = nullptr;
static dw_gdma_block_transfer_config_t s_dma_cfg;
static bool s_dma_patched = false;

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

static void update_dma_source(int row_offset)
{
    if (!s_dma_link_list) return;
    s_dma_cfg.src.addr = (uint32_t)(ext_fb + row_offset * PHYS_W * sizeof(uint16_t));
    dw_gdma_lli_config_transfer(
        dw_gdma_link_list_get_item(s_dma_link_list, 0), &s_dma_cfg);
    dw_gdma_block_markers_t markers = {};
    markers.is_valid = true;
    markers.is_last = true;
    dw_gdma_lli_set_block_markers(
        dw_gdma_link_list_get_item(s_dma_link_list, 0), markers);
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

    // Get DPI panel handle and original framebuffer
    auto panel = static_cast<lgfx::Panel_DSI*>(dsp.getPanel());
    orig_dpi_fb = (uint8_t*)panel->config_detail().buffer;

    // Allocate extended framebuffer in PSRAM (cache-line aligned)
    size_t ext_size = PHYS_W * EXT_H * sizeof(uint16_t);
    ext_fb = (uint8_t*)heap_caps_aligned_calloc(64, 1, ext_size,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ext_fb || !orig_dpi_fb) return false;

    visible_offset = WF_EXTRA;
    s_dma_patched = false;

    // Get the esp_lcd_panel_handle_t from Panel_DSI (protected member)
    auto *acc = static_cast<Panel_DSI_Accessor*>(panel);
    esp_lcd_panel_handle_t lcd_handle = acc->getHandle();

    if (lcd_handle) {
        // Cast to compat struct — esp_lcd_panel_t base is the first member
        dpi_panel_compat_t *dpi = (dpi_panel_compat_t *)lcd_handle;

        // Validate struct layout
        bool valid = (dpi->fbs[0] == (uint8_t*)orig_dpi_fb &&
                      dpi->h_pixels == PHYS_W &&
                      dpi->v_pixels == PHYS_H &&
                      dpi->num_fbs >= 1 && dpi->num_fbs <= 3);

        printf("[pal] DMA patch: handle=%p fbs[0]=%p expect=%p valid=%d\n",
               lcd_handle, dpi->fbs[0], orig_dpi_fb, valid);

        if (valid) {
            s_dma_link_list = dpi->link_lists[dpi->cur_fb_index];
            printf("[pal] DMA patch: fb_size=%d link_list=%p\n",
                   (int)dpi->fb_size, s_dma_link_list);

            // Build DMA transfer config matching dpi_panel_init()
            memset(&s_dma_cfg, 0, sizeof(s_dma_cfg));
            s_dma_cfg.src.addr = (uint32_t)(ext_fb + visible_offset * PHYS_W * sizeof(uint16_t));
            s_dma_cfg.src.burst_mode = DW_GDMA_BURST_MODE_INCREMENT;
            s_dma_cfg.src.burst_items = DW_GDMA_BURST_ITEMS_512;
            s_dma_cfg.src.burst_len = 16;
            s_dma_cfg.src.width = DW_GDMA_TRANS_WIDTH_64;
            s_dma_cfg.dst.addr = MIPI_DSI_BRG_MEM_BASE;
            s_dma_cfg.dst.burst_mode = DW_GDMA_BURST_MODE_FIXED;
            s_dma_cfg.dst.burst_items = DW_GDMA_BURST_ITEMS_256;
            s_dma_cfg.dst.burst_len = 16;
            s_dma_cfg.dst.width = DW_GDMA_TRANS_WIDTH_64;
            s_dma_cfg.size = dpi->fb_size * 8 / 64;

            update_dma_source(visible_offset);
            s_dma_patched = true;
            printf("[pal] DMA patch: SUCCESS src=%p size=%d\n",
                   (void*)(uintptr_t)s_dma_cfg.src.addr, (int)s_dma_cfg.size);
        }
    }

    if (!s_dma_patched)
        printf("[pal] DMA patch: FAILED, using memcpy fallback\n");

    return true;
}

void shutdown() {}

DisplayInfo getDisplayInfo() { return { log_w, log_h }; }

uint16_t* getFramebuffer()
{
    return (uint16_t*)(ext_fb + visible_offset * PHYS_W * sizeof(uint16_t));
}

int getFramebufferStride() { return PHYS_W; }
bool isRotated() { return false; }
int getExtendedHeight() { return EXT_H; }

void setVisibleOffset(int row)
{
    if (row < 0) row = 0;
    if (row > WF_EXTRA) row = WF_EXTRA;
    visible_offset = row;
    if (s_dma_patched)
        update_dma_source(visible_offset);
}

void commitFrame()
{
    // Flush the visible region of ext_fb so DMA sees current pixels
    uint8_t *vis = ext_fb + visible_offset * PHYS_W * sizeof(uint16_t);
    esp_cache_msync(vis, PHYS_W * PHYS_H * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    if (!s_dma_patched) {
        // Fallback: copy to DPI framebuffer
        memcpy(orig_dpi_fb, vis, PHYS_W * PHYS_H * sizeof(uint16_t));
        esp_cache_msync(orig_dpi_fb, PHYS_W * PHYS_H * sizeof(uint16_t),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}

bool pollEvent(TouchEvent &evt)
{
    M5.update();
    auto count = M5.Touch.getCount();
    if (count > 0) {
        auto detail = M5.Touch.getDetail(0);
        if (detail.wasPressed())
            evt_push({detail.x, detail.y, TouchEvent::DOWN});
        if (detail.wasReleased())
            evt_push({detail.x, detail.y, TouchEvent::UP});
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
