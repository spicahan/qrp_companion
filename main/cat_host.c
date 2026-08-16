#include "cat_host.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "pc_link.h"

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
    // true = "consumed, flush the RX buffer". Returning false asks the driver to
    // APPEND the next transfer to this one, which ESP32-P4 cannot do
    // (SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE) -- it logged "RX buffer append is not
    // supported on this target!" once per CAT reply, flooding the console.
    return true;
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

// ---- PC pass-through port (QMX USB 2) -------------------------------------

static cdc_acm_dev_hdl_t s_pc_handle = NULL;

// QMX -> PC. Straight relay; the QMX already addressed this reply to the port
// the PC's command came in on, so there is nothing to demultiplex here.
static bool pc_rx_callback(const uint8_t *data, size_t len, void *user_ctx)
{
    pc_link_from_qmx(data, len);
    return true;   // consumed; see cdc_rx_callback on why this must not be false
}

static void pc_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "CDC(PC) disconnected");
        if (s_pc_handle) {
            cdc_acm_host_close(s_pc_handle);
            s_pc_handle = NULL;
        }
    }
}

int cat_host_pc_is_connected(void) { return s_pc_handle != NULL; }

int cat_host_pc_send(const char *data, int len)
{
    if (!s_pc_handle) return -1;
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_pc_handle, (const uint8_t *)data, len, 100);
    return (err == ESP_OK) ? len : -1;
}

static void try_open_pc_cdc(void)
{
    if (s_pc_handle || !s_cdc_handle) return;   // our own port comes first

    cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 500,
        .out_buffer_size = 64,
        .in_buffer_size = 128,
        .event_cb = pc_event_callback,
        .data_cb = pc_rx_callback,
        .user_arg = NULL,
    };

    esp_err_t err = cdc_acm_host_open(QMX_VID, QMX_PID, QMX_CDC_IF_PC, &cfg, &s_pc_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CDC(PC) opened: QMX itf %d", QMX_CDC_IF_PC);
        return;
    }
    // Only a warning: one serial port is the QMX default, and everything except
    // the pass-through works fine without the second one.
    static bool warned = false;
    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "PC pass-through port (itf %d) unavailable: %s -- set QMX "
                      "'USB serial ports' to 2 and restart the radio",
                 QMX_CDC_IF_PC, esp_err_to_name(err));
    }
}

// Every CDC-Comm interface the device exposes, in ascending order. With QMX
// "USB serial ports" = 2 there are two (0 and 5), so a single "last one wins"
// hint would point at USB 2 -- the port reserved for the PC pass-through.
#define MAX_COMM_IFACES 8
static uint8_t s_comm_ifaces[MAX_COMM_IFACES];
static int     s_comm_count = 0;

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
                bool known = false;
                for (int i = 0; i < s_comm_count; i++)
                    if (s_comm_ifaces[i] == intf->bInterfaceNumber) known = true;
                if (!known && s_comm_count < MAX_COMM_IFACES) {
                    s_comm_ifaces[s_comm_count++] = intf->bInterfaceNumber;
                    ESP_LOGI(TAG, "CDC interface %d detected", intf->bInterfaceNumber);
                }
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
        ESP_LOGI(TAG, "CDC opened: QMX itf %d", QMX_CDC_IF);
        // Dump the config descriptor so the interface layout is visible in the
        // log; this is what tells us where the PC pass-through port lives.
        cdc_acm_host_desc_print(s_cdc_handle);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "open QMX itf %d failed: %s", QMX_CDC_IF, esp_err_to_name(err));

    // Fall back only to interfaces we positively know are CDC-Comm, lowest
    // first, never the pass-through's. Opening a *data* interface (the old
    // blind 0..6 scan happily grabbed itf 6) silently steals half of USB 2.
    for (int i = 0; i < s_comm_count; i++) {
        uint8_t itf = s_comm_ifaces[i];
        if (itf == QMX_CDC_IF_PC) continue;     // reserved for the PC pass-through
        err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                                itf, &dev_cfg, &s_cdc_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "CDC opened: comm itf %d", itf);
            cdc_acm_host_desc_print(s_cdc_handle);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "open comm itf %d failed: %s", itf, esp_err_to_name(err));
    }

    // Only if we never saw a descriptor (non-QMX device, callback missed).
    for (int iface = 0; s_comm_count == 0 && iface <= 4; iface++) {
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
        // Opened lazily and independently: the pass-through port only exists
        // when the operator has enabled two USB serial ports on the QMX.
        if (s_cdc_handle && !s_pc_handle && pc_link_running())
            try_open_pc_cdc();
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
