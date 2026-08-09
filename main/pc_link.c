#include "pc_link.h"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "pc_link";

#define PC_LINK_BUF_SIZE  1024

static bool         s_running  = false;
static size_t       s_rx_bytes = 0;
static size_t       s_tx_bytes = 0;
static TaskHandle_t s_task     = NULL;

// While the link is up the USB-C port carries CAT bytes, so anything the log
// subsystem emits would be injected straight into the PC's serial stream. Drop
// it. Logs stay available on the primary UART0 console.
static int quiet_vprintf(const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    return 0;
}

// Step 1: pure loopback. Whatever the PC sends is echoed straight back, which
// proves the USB-C serial path works end to end while the QMX stays connected
// on the USB-A host port. The CAT pass-through replaces this echo later.
static void pc_link_task(void *arg)
{
    (void)arg;
    static uint8_t buf[PC_LINK_BUF_SIZE];

    for (;;) {
        int rx = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (rx <= 0) continue;
        s_rx_bytes += (size_t)rx;

        // write_bytes can accept less than requested when the TX ring is full,
        // so loop until the whole chunk is away. The spin cap stops us wedging
        // here forever if the host stops draining.
        int sent = 0;
        for (int spin = 0; sent < rx && spin < 64; ++spin) {
            int w = usb_serial_jtag_write_bytes(buf + sent, (size_t)(rx - sent),
                                                pdMS_TO_TICKS(20));
            if (w > 0) sent += w;
        }
        s_tx_bytes += (size_t)sent;
    }
}

esp_err_t pc_link_start(void)
{
    if (s_running) return ESP_OK;

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = PC_LINK_BUF_SIZE,
        .rx_buffer_size = PC_LINK_BUF_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "PC link up: USB-C serial (loopback). Console logs suppressed "
                  "on USB-C until reboot.");

    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_set_vprintf(quiet_vprintf);

    if (xTaskCreate(pc_link_task, "pc_link", 4096, NULL, 6, &s_task) != pdPASS) {
        usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    return ESP_OK;
}

bool   pc_link_running(void)  { return s_running; }
size_t pc_link_rx_count(void) { return s_rx_bytes; }
size_t pc_link_tx_count(void) { return s_tx_bytes; }
