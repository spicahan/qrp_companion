#include "cat_host.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

static const char *TAG = "cat";

// QMX VID/PID — CDC-ACM is on interface 0
#define QMX_VID     0x0483
#define QMX_PID     0xA34C
#define QMX_CDC_IF  0

#define CAT_RX_BUF_SIZE  128
#define CAT_POLL_INTERVAL_MS 1000

static cdc_acm_dev_hdl_t s_cdc_handle = NULL;
static volatile uint64_t s_vfo_freq = 0;
static volatile int s_mode = 0;

// RX buffer for accumulating CAT response
static char s_rx_buf[CAT_RX_BUF_SIZE];
static int s_rx_pos = 0;

// Process one complete CAT response (terminated by ';')
static void process_cat_response(const char *resp, int len)
{
    // FA command response: "FA00014070000;"
    if (len >= 14 && resp[0] == 'F' && resp[1] == 'A') {
        char freq_str[12];
        int flen = len - 3;  // skip "FA" and ";"
        if (flen > 11) flen = 11;
        memcpy(freq_str, &resp[2], flen);
        freq_str[flen] = '\0';
        uint64_t freq = strtoull(freq_str, NULL, 10);
        if (freq > 0) {
            s_vfo_freq = freq;
        }
    }
    // MD command response: "MD6;"
    else if (len >= 3 && resp[0] == 'M' && resp[1] == 'D') {
        int mode = resp[2] - '0';
        if (mode >= 1 && mode <= 9) {
            s_mode = mode;
        }
    }
}

// CDC-ACM RX data callback — accumulate and parse responses
static bool cdc_rx_callback(const uint8_t *data, size_t data_len, void *user_ctx)
{
    for (size_t i = 0; i < data_len; i++) {
        char c = (char)data[i];
        if (c == ';') {
            s_rx_buf[s_rx_pos] = ';';
            s_rx_pos++;
            s_rx_buf[s_rx_pos] = '\0';
            process_cat_response(s_rx_buf, s_rx_pos);
            s_rx_pos = 0;
        } else if (s_rx_pos < CAT_RX_BUF_SIZE - 2) {
            s_rx_buf[s_rx_pos++] = c;
        }
    }
    return false;  // no task wakeup needed
}

// CDC-ACM event callback
static void cdc_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "CDC device disconnected");
        if (s_cdc_handle) {
            cdc_acm_host_close(s_cdc_handle);
            s_cdc_handle = NULL;
        }
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGW(TAG, "CDC error");
        break;
    default:
        break;
    }
}

// New device callback — detect CDC interfaces on composite devices
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

// Try to open the CDC-ACM device
static esp_err_t try_open_cdc(void)
{
    if (s_cdc_handle) return ESP_OK;  // already open

    cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 500,
        .out_buffer_size = 64,
        .in_buffer_size = 128,
        .event_cb = cdc_event_callback,
        .data_cb = cdc_rx_callback,
        .user_arg = NULL,
    };

    // Try QMX first
    esp_err_t err = cdc_acm_host_open(QMX_VID, QMX_PID, QMX_CDC_IF, &dev_cfg, &s_cdc_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CDC opened: QMX VID/PID iface %d", QMX_CDC_IF);
        return ESP_OK;
    }

    // Try hinted interface
    if (s_cdc_iface_hint >= 0) {
        err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                                (uint8_t)s_cdc_iface_hint, &dev_cfg, &s_cdc_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "CDC opened: hint iface %d", s_cdc_iface_hint);
            return ESP_OK;
        }
    }

    // Scan interfaces 0-5
    for (int iface = 0; iface <= 5; iface++) {
        err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                                (uint8_t)iface, &dev_cfg, &s_cdc_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "CDC opened: scan iface %d", iface);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t cat_send(const char *cmd)
{
    if (!s_cdc_handle) return ESP_ERR_INVALID_STATE;
    return cdc_acm_host_data_tx_blocking(s_cdc_handle, (const uint8_t *)cmd, strlen(cmd), 500);
}

// CAT polling task — periodically sends FA; and processes responses
static void cat_poll_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));  // wait for USB enumeration

    while (1) {
        if (!s_cdc_handle) {
            try_open_cdc();
        }

        if (s_cdc_handle) {
            esp_err_t err = cat_send("FA;MD;");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "CAT send failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CAT_POLL_INTERVAL_MS));
    }
}

void cat_host_init(void)
{
    // Install CDC-ACM driver (must happen before UAC driver on composite devices)
    const cdc_acm_host_driver_config_t cdc_cfg = {
        .driver_task_stack_size = 3072,
        .driver_task_priority = 4,
        .xCoreID = 0,
        .new_dev_cb = cdc_new_dev_cb,
    };

    esp_err_t err = cdc_acm_host_install(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ACM driver install failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CDC-ACM driver installed");
    }
}

void cat_host_start(void)
{
    xTaskCreatePinnedToCore(cat_poll_task, "cat_poll", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "CAT polling task started");
}

uint64_t cat_get_vfo_freq(void)
{
    return s_vfo_freq;
}

int cat_get_mode(void)
{
    return s_mode;
}
