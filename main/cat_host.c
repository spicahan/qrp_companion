#include "cat_host.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

static const char *TAG = "cat";

#define QMX_VID     0x0483
#define QMX_PID     0xA34C
#define QMX_CDC_IF  0

// When the QMX has "USB serial ports" set to 2 it exposes a second, fully
// equivalent CAT channel. Verified interface map on firmware 1_04_004:
//   0 CDC-comm / 1 CDC-data   -> USB 1 (ours)
//   2,3,4 audio               -> UAC stream
//   5 CDC-comm / 6 CDC-data   -> USB 2 (reserved for the PC pass-through)
// The blind scan below must never grab USB 2, or the pass-through loses its
// port to our own CAT layer.
#define QMX_CDC_IF_PC  5

static cdc_acm_dev_hdl_t s_cdc_handle = NULL;

// RX ring buffer filled by CDC callback, drained by cat_host_recv
#define RX_RING_SIZE 256
static uint8_t s_rx_ring[RX_RING_SIZE];
static volatile int s_rx_head = 0;
static volatile int s_rx_tail = 0;

static bool cdc_rx_callback(const uint8_t *data, size_t len, void *user_ctx)
{
    for (size_t i = 0; i < len; i++) {
        int next = (s_rx_head + 1) % RX_RING_SIZE;
        if (next != s_rx_tail) {
            s_rx_ring[s_rx_head] = data[i];
            s_rx_head = next;
        }
    }
    return false;
}

static void cdc_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "CDC disconnected");
        if (s_cdc_handle) {
            cdc_acm_host_close(s_cdc_handle);
            s_cdc_handle = NULL;
        }
    }
}

static int s_cdc_iface_hint = -1;

static void cdc_new_dev_cb(usb_device_handle_t usb_dev)
{
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(usb_dev, &cfg) != ESP_OK) return;

    const uint8_t *p = (const uint8_t *)cfg;
    for (int offset = 0; offset + 2 <= cfg->wTotalLength; ) {
        uint8_t len = p[offset];
        uint8_t dtype = p[offset + 1];
        if (len == 0) break;
        if (dtype == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)(p + offset);
            if (intf->bInterfaceClass == USB_CLASS_COMM) {
                s_cdc_iface_hint = intf->bInterfaceNumber;
                ESP_LOGI(TAG, "CDC interface %d detected", s_cdc_iface_hint);
            }
        }
        offset += len;
    }
}

static esp_err_t try_open_cdc(void)
{
    if (s_cdc_handle) return ESP_OK;

    cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 500,
        .out_buffer_size = 64,
        .in_buffer_size = 128,
        .event_cb = cdc_event_callback,
        .data_cb = cdc_rx_callback,
        .user_arg = NULL,
    };

    esp_err_t err = cdc_acm_host_open(QMX_VID, QMX_PID, QMX_CDC_IF, &dev_cfg, &s_cdc_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CDC opened: QMX");
        // Dump the config descriptor so we can see the interface layout. With
        // QMX "USB serial ports" set to 2 this reveals the second CDC function's
        // communication interface index, which the PC pass-through opens as its
        // own port (QMX then arbitrates the two CAT masters itself).
        cdc_acm_host_desc_print(s_cdc_handle);
        return ESP_OK;
    }

    if (s_cdc_iface_hint >= 0) {
        err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                                (uint8_t)s_cdc_iface_hint, &dev_cfg, &s_cdc_handle);
        if (err == ESP_OK) { ESP_LOGI(TAG, "CDC opened: hint iface %d", s_cdc_iface_hint); return ESP_OK; }
    }

    for (int iface = 0; iface <= 6; iface++) {
        if (iface == QMX_CDC_IF_PC) continue;   // reserved for the PC pass-through
        err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                                (uint8_t)iface, &dev_cfg, &s_cdc_handle);
        if (err == ESP_OK) { ESP_LOGI(TAG, "CDC opened: scan iface %d", iface); return ESP_OK; }
    }

    return ESP_ERR_NOT_FOUND;
}

// Background task: keep trying to open CDC device
static void cat_connect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    while (1) {
        if (!s_cdc_handle)
            try_open_cdc();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void cat_host_init(void)
{
    const cdc_acm_host_driver_config_t cdc_cfg = {
        .driver_task_stack_size = 3072,
        .driver_task_priority = 4,
        .xCoreID = 0,
        .new_dev_cb = cdc_new_dev_cb,
    };
    esp_err_t err = cdc_acm_host_install(&cdc_cfg);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "CDC-ACM install failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "CDC-ACM driver installed");
}

void cat_host_start(void)
{
    xTaskCreatePinnedToCore(cat_connect_task, "cat_conn", 4096, NULL, 2, NULL, 0);
}

int cat_host_is_connected(void)
{
    return s_cdc_handle != NULL;
}

int cat_host_send(const char *data, int len)
{
    if (!s_cdc_handle) return -1;
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_handle, (const uint8_t *)data, len, 10);
    return (err == ESP_OK) ? len : -1;
}

int cat_host_recv(char *buf, int max_len)
{
    int count = 0;
    while (count < max_len && s_rx_head != s_rx_tail) {
        buf[count++] = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1) % RX_RING_SIZE;
    }
    return count;
}
