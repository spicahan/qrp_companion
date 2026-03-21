#include "uac_host.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

static const char *TAG = "uac";

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define MIC_TASK_PRIORITY       3
// Buffer must be a multiple of 288 (USB packet size for 48kHz/24-bit/stereo)
// 4608 = 288 * 16. Non-aligned sizes cause frame boundary splits in the ring buffer.
#define MIC_BUFFER_SIZE         4608
#define FLOAT_BUF_SIZE          (MIC_BUFFER_SIZE / 6 + 1)  // max mono samples from stereo 24-bit

// Desired format
#define TARGET_FREQ   48000
#define TARGET_BITS   24
#define TARGET_CH     2

typedef enum { APP_EVENT = 0, UAC_DRIVER_EVENT, UAC_DEVICE_EVENT } event_group_t;

typedef struct {
    event_group_t event_group;
    union {
        struct { uint8_t addr; uint8_t iface_num; uac_host_driver_event_t event; } drv;
        struct { uac_host_device_handle_t handle; uac_host_driver_event_t event; } dev;
    };
} event_msg_t;

static QueueHandle_t s_event_queue = NULL;
static uac_host_device_handle_t s_mic_handle = NULL;
static volatile bool s_recording = false;
static uint32_t s_mic_freq = TARGET_FREQ;
static uint8_t  s_mic_bits = TARGET_BITS;
static uint8_t  s_mic_ch   = TARGET_CH;

// Debug info displayed on screen
static uac_debug_info_t s_debug_info = { "UAC: waiting...", "" };
static uint32_t s_debug_counter = 0;

const uac_debug_info_t* uac_get_debug_info(void) { return &s_debug_info; }

// Convert 24-bit stereo raw bytes to mono float [-1,1] and push to DSP
static void process_audio_buffer(const uint8_t *buf, uint32_t bytes)
{
    static float float_buf[FLOAT_BUF_SIZE];

    uint32_t bytes_per_frame = s_mic_ch * (s_mic_bits / 8);
    if (bytes_per_frame == 0) return;
    uint32_t num_frames = bytes / bytes_per_frame;
    uint32_t remainder = bytes % bytes_per_frame;

    // Update debug info periodically (every ~250 calls ≈ 1 second)
    if ((s_debug_counter % 250) == 0) {
        // Line 1: format + alignment
        snprintf(s_debug_info.line1, sizeof(s_debug_info.line1),
                 "UAC: ch=%d b=%d %luHz  bytes=%lu frames=%lu rem=%lu",
                 s_mic_ch, s_mic_bits, (unsigned long)s_mic_freq,
                 (unsigned long)bytes, (unsigned long)num_frames,
                 (unsigned long)remainder);

        // Line 2: first 2 raw frames + first 4 mono floats
        if (num_frames >= 2 && bytes_per_frame == 6) {
            int32_t l0 = buf[0] | (buf[1] << 8) | (buf[2] << 16);
            if (l0 & 0x800000) l0 |= (int32_t)0xFF000000;
            int32_t l1 = buf[6] | (buf[7] << 8) | (buf[8] << 16);
            if (l1 & 0x800000) l1 |= (int32_t)0xFF000000;

            // Also compute what mono[0..3] will be
            float m0 = 0, m1 = 0;
            for (int ch = 0; ch < s_mic_ch; ch++) {
                int32_t s0 = buf[ch*3] | (buf[ch*3+1] << 8) | (buf[ch*3+2] << 16);
                if (s0 & 0x800000) s0 |= (int32_t)0xFF000000;
                m0 += (float)s0 / 8388608.0f;
                uint32_t o1 = bytes_per_frame + ch*3;
                int32_t s1 = buf[o1] | (buf[o1+1] << 8) | (buf[o1+2] << 16);
                if (s1 & 0x800000) s1 |= (int32_t)0xFF000000;
                m1 += (float)s1 / 8388608.0f;
            }
            m0 /= s_mic_ch; m1 /= s_mic_ch;

            snprintf(s_debug_info.line2, sizeof(s_debug_info.line2),
                     "L0=%ld L1=%ld  mono=%.4f,%.4f  #%lu",
                     (long)l0, (long)l1, m0, m1,
                     (unsigned long)s_debug_counter);
        }
    }
    s_debug_counter++;

    for (uint32_t i = 0; i < num_frames; i++) {
        float mono = 0.0f;

        if (s_mic_bits == 24) {
            for (int ch = 0; ch < s_mic_ch; ch++) {
                uint32_t off = i * bytes_per_frame + ch * 3;
                int32_t s = buf[off] | (buf[off+1] << 8) | (buf[off+2] << 16);
                if (s & 0x800000) s |= (int32_t)0xFF000000;
                mono += (float)s / 8388608.0f;
            }
        } else if (s_mic_bits == 16) {
            const int16_t *buf16 = (const int16_t *)buf;
            for (int ch = 0; ch < s_mic_ch; ch++) {
                mono += (float)buf16[i * s_mic_ch + ch] / 32768.0f;
            }
        }

        float_buf[i] = mono / s_mic_ch;
    }

    uac_push_audio_samples(float_buf, num_frames);
}

static void mic_task(void *arg)
{
    // Extra room for leftover bytes from previous read
    uint8_t *audio_buf = malloc(MIC_BUFFER_SIZE + 8);
    if (!audio_buf) { vTaskDelete(NULL); return; }
    uint32_t leftover = 0;  // bytes carried over from previous read

    while (1) {
        if (s_mic_handle && s_recording) {
            uint32_t bytes_read = 0;
            esp_err_t ret = uac_host_device_read(s_mic_handle, audio_buf + leftover,
                                                  MIC_BUFFER_SIZE, &bytes_read, pdMS_TO_TICKS(200));
            bytes_read += leftover;
            if (ret == ESP_OK && bytes_read > 0) {
                // Only process complete frames; save leftover bytes
                uint32_t bpf = s_mic_ch * (s_mic_bits / 8);
                uint32_t aligned = (bpf > 0) ? (bytes_read / bpf) * bpf : bytes_read;
                leftover = bytes_read - aligned;

                if (aligned > 0)
                    process_audio_buffer(audio_buf, aligned);

                // Move leftover bytes to start of buffer for next read
                if (leftover > 0)
                    memmove(audio_buf, audio_buf + aligned, leftover);

                vTaskDelay(1);  // yield for watchdog
            } else {
                leftover = 0;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static esp_err_t try_start_mic(uac_host_device_handle_t handle)
{
    uint32_t rates[] = {48000, 44100, 32000, 16000};
    uint8_t bits[]   = {24, 16};
    uint8_t chs[]    = {2, 1};

    for (int r = 0; r < sizeof(rates)/sizeof(rates[0]); r++) {
        for (int b = 0; b < sizeof(bits)/sizeof(bits[0]); b++) {
            for (int c = 0; c < sizeof(chs)/sizeof(chs[0]); c++) {
                uac_host_stream_config_t cfg = {
                    .channels = chs[c],
                    .bit_resolution = bits[b],
                    .sample_freq = rates[r],
                };
                ESP_LOGI(TAG, "Trying ch:%d bits:%d rate:%lu", chs[c], bits[b], (unsigned long)rates[r]);
                if (uac_host_device_start(handle, &cfg) == ESP_OK) {
                    s_mic_ch   = chs[c];
                    s_mic_bits = bits[b];
                    s_mic_freq = rates[r];
                    ESP_LOGI(TAG, "Started: ch:%d bits:%d rate:%lu", s_mic_ch, s_mic_bits, (unsigned long)s_mic_freq);
                    return ESP_OK;
                }
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void device_callback(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Mic disconnected");
        s_recording = false;
        s_mic_handle = NULL;
        uac_host_device_close(handle);
        return;
    }
    event_msg_t msg = { .event_group = UAC_DEVICE_EVENT, .dev = { .handle = handle, .event = event } };
    xQueueSend(s_event_queue, &msg, 0);
}

static void driver_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    ESP_LOGI(TAG, "Driver event: addr:%d iface:%d event:%d", addr, iface_num, event);
    event_msg_t msg = { .event_group = UAC_DRIVER_EVENT, .drv = { .addr = addr, .iface_num = iface_num, .event = event } };
    xQueueSend(s_event_queue, &msg, 0);
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            break;
        }
    }
    usb_host_uninstall();
    vTaskDelete(NULL);
}

static void uac_event_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uac_host_driver_config_t uac_cfg = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_install(&uac_cfg));
    ESP_LOGI(TAG, "UAC driver installed");

    event_msg_t msg;
    while (xQueueReceive(s_event_queue, &msg, portMAX_DELAY)) {
        if (msg.event_group == UAC_DRIVER_EVENT) {
            if (msg.drv.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
                ESP_LOGI(TAG, "Mic connected addr:%d iface:%d", msg.drv.addr, msg.drv.iface_num);

                uac_host_device_handle_t handle = NULL;
                const uac_host_device_config_t dev_cfg = {
                    .addr = msg.drv.addr,
                    .iface_num = msg.drv.iface_num,
                    .buffer_size = 16000,
                    .buffer_threshold = 4000,
                    .callback = device_callback,
                    .callback_arg = NULL,
                };

                if (uac_host_device_open(&dev_cfg, &handle) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to open device");
                    continue;
                }
                uac_host_printf_device_param(handle);

                if (try_start_mic(handle) == ESP_OK) {
                    s_mic_handle = handle;
                    s_recording = true;
                } else {
                    ESP_LOGE(TAG, "No compatible config found");
                    uac_host_device_close(handle);
                }
            }
        } else if (msg.event_group == APP_EVENT) {
            break;
        }
    }

    uac_host_uninstall();
    vTaskDelete(NULL);
}

void uac_host_start(void)
{
    s_event_queue = xQueueCreate(10, sizeof(event_msg_t));

    xTaskCreatePinnedToCore(mic_task, "mic_task", 4096, NULL, MIC_TASK_PRIORITY, NULL, 0);

    static TaskHandle_t uac_task_handle = NULL;
    xTaskCreatePinnedToCore(uac_event_task, "uac_events", 4096, NULL,
                            UAC_TASK_PRIORITY, &uac_task_handle, 0);

    xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, (void *)uac_task_handle,
                            USB_HOST_TASK_PRIORITY, NULL, 0);

    ESP_LOGI(TAG, "UAC host tasks started");
}
