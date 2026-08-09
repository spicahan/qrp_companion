#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// "PC link": exposes the Tab5's USB-C port to a PC as a virtual COM port
// (eventually a CAT pass-through to the QMX, for loggers such as N1MM).
//
// USB topology on the Tab5 (ESP32-P4 has 2 OTG peripherals + USB-Serial-JTAG,
// with 1 UTMI PHY and 1 internal FSLS PHY):
//
//   USB-A  -> OTG High-Speed (UTMI PHY)      : USB host, QMX audio + CAT
//   USB-C  -> USB-Serial-JTAG (dedicated pads): console + flashing + THIS link
//   n/a    -> OTG Full-Speed (internal PHY)   : NOT USABLE, see below
//
// We deliberately do NOT use TinyUSB on the OTG Full-Speed port, even though it
// installs cleanly and would give us a "real" CDC-ACM device. On ESP32-P4 the
// internal FSLS PHY is not wired to the USB-C connector at all -- it is muxed
// onto GPIO26/GPIO27 (see esp-idf soc/esp32p4/usb_dwc_periph.c), and on the
// Tab5 those two pins belong to the ES8388 audio codec. Bringing that PHY up
// silently reconfigures both pins as USB D-/D+ at 40mA drive, so the device
// never enumerates over USB-C *and* audio breaks. The USB-C data lines go to
// the USB-Serial-JTAG peripheral's dedicated pads, which is why flashing and
// the console work there.
//
// So the PC-facing port is USB-Serial-JTAG itself, which already enumerates as
// a CDC-ACM serial device. The cost: while the link is up, USB-C carries CAT
// bytes only, so log output is suppressed to avoid corrupting the stream
// (the primary console is UART0 and is unaffected). That is why this is not
// started at boot -- leaving it off by default keeps USB-C console and crash
// dumps working exactly as before.

// Control lines: USB-Serial-JTAG is a full hardware CDC-ACM device, not a
// TX/RX-only pipe -- it handles SET_CONTROL_LINE_STATE and exposes the host's
// DTR and RTS levels (chip_rst bits 1 and 0) plus change interrupts. That lets
// a logger's DTR keying be forwarded verbatim to the QMX's own virtual COM DTR
// via cdc_acm_host_set_control_line_state(), with no CAT transcoding.
//
// NOTE: by default the USJ hardware resets the chip on the DTR/RTS pattern
// esptool uses for auto-download, so a logger asserting RTS would reboot the
// Tab5. pc_link_start() disables that (chip_rst.usb_uart_chip_rst_dis) and
// latches it via config_update -- the latch is required, as the reset logic
// lives in the 48MHz USB clock domain. Verified against all four DTR/RTS
// combinations. Side effect: esptool auto-reset stops working while the link
// is up, so flashing then needs a manual reset.

esp_err_t pc_link_start(void);   // take over USB-C serial for the PC
bool      pc_link_running(void);

// Host control-line state, sampled every PC_LINK_POLL_MS.
bool      pc_link_dtr(void);
bool      pc_link_rts(void);
size_t    pc_link_line_changes(void);

// Loopback diagnostics (step 1): bytes echoed back to the PC.
size_t    pc_link_rx_count(void);
size_t    pc_link_tx_count(void);

#ifdef __cplusplus
}
#endif
