#include "pc_link.h"
#include "cat_host.h"

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
static size_t          s_dropped_bytes = 0;

// While the link is up the USB-C port carries CAT bytes, so anything the log
// subsystem emits would be injected straight into the PC's serial stream. Drop
// it. Logs stay available on the primary UART0 console.
static int quiet_vprintf(const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    return 0;
}

// QMX -> PC. Called from the CAT host's RX callback for the USB 2 port.
void pc_link_from_qmx(const uint8_t *data, size_t len)
{
    if (!s_running || !data || len == 0) return;
    size_t sent = 0;
    for (int spin = 0; sent < len && spin < 64; ++spin) {
        int w = usb_serial_jtag_write_bytes(data + sent, len - sent, pdMS_TO_TICKS(20));
        if (w > 0) sent += (size_t)w;
    }
    s_tx_bytes += sent;
}

// PC <-> QMX relay. Bytes are moved verbatim: the PC owns the QMX's USB 2 CAT
// port and the QMX answers it directly, so nothing here has to parse CAT,
// match responses to requests, or arbitrate against our own polling on USB 1.
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

        // PC -> QMX. If the pass-through port isn't open (QMX still set to one
        // USB serial port), drop the bytes rather than echoing them: a logger
        // must never mistake its own command for a reply from the radio.
        if (cat_host_pc_is_connected())
            cat_host_pc_send((const char *)buf, rx);
        else
            s_dropped_bytes += (size_t)rx;
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

    ESP_LOGI(TAG, "PC link up: USB-C serial relays to QMX USB 2. Console logs "
                  "suppressed on USB-C until reboot.");

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
