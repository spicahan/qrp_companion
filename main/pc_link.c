#include "pc_link.h"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "soc/usb_serial_jtag_struct.h"
#include <stdio.h>
#include <string.h>

// Set to 1 to echo every DTR/RTS transition back over the data channel. Handy
// when bringing up a logger's keying config; off in normal builds.
#define PC_LINK_PROBE_LINE_STATE 0

// Poll period for the control lines. DTR carries CW keying, so this bounds the
// keying jitter we add; 5ms is well inside a dit at any practical speed.
#define PC_LINK_POLL_MS 5

static const char *TAG = "pc_link";

#define PC_LINK_BUF_SIZE  1024

static bool         s_running  = false;
static size_t       s_rx_bytes = 0;
static size_t       s_tx_bytes = 0;
static TaskHandle_t s_task     = NULL;
static volatile bool   s_dtr = false;
static volatile bool   s_rts = false;
static volatile size_t s_line_changes = 0;

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
        // Sample the host's control lines. chip_rst carries the live levels in
        // bits 0 (RTS) and 1 (DTR) despite the register's name -- verified on
        // hardware against every DTR/RTS combination.
        uint32_t rst = USB_SERIAL_JTAG.chip_rst.val;
        bool rts = rst & 0x1u;
        bool dtr = (rst >> 1) & 0x1u;
        if (dtr != s_dtr || rts != s_rts) {
            s_dtr = dtr;
            s_rts = rts;
            s_line_changes++;
#if PC_LINK_PROBE_LINE_STATE
            char msg[48];
            int n = snprintf(msg, sizeof(msg), "\r\n[LINE dtr=%d rts=%d]\r\n",
                             (int)dtr, (int)rts);
            usb_serial_jtag_write_bytes(msg, (size_t)n, pdMS_TO_TICKS(20));
#endif
        }

        int rx = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(PC_LINK_POLL_MS));
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

    // The USJ hardware resets the chip on the DTR/RTS pattern esptool uses for
    // auto-download. A logger keying PTT on DTR would therefore reboot the Tab5,
    // so disable USB-driven chip reset while the link is up. Cost: esptool's
    // auto-reset stops working until the next manual reset, which is fine
    // because the link is opt-in and off at boot.
    // The disable lives in the APB domain but the reset logic runs off the 48MHz
    // USB clock, so it only takes effect after a config_update latch.
    USB_SERIAL_JTAG.chip_rst.usb_uart_chip_rst_dis = 1;
    USB_SERIAL_JTAG.config_update.config_update = 1;

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
bool   pc_link_dtr(void)      { return s_dtr; }
bool   pc_link_rts(void)      { return s_rts; }
size_t pc_link_line_changes(void) { return s_line_changes; }
size_t pc_link_rx_count(void) { return s_rx_bytes; }
size_t pc_link_tx_count(void) { return s_tx_bytes; }
