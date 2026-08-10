#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize CDC-ACM driver for CAT control.
// Must be called BEFORE uac_host_start() so the CDC driver is installed
// before the UAC driver on composite USB devices.
void cat_host_init(void);

// Start CDC device discovery (spawns background task).
void cat_host_start(void);

// Raw serial transport for PAL
int  cat_host_is_connected(void);
int  cat_host_send(const char *data, int len);
int  cat_host_recv(char *buf, int max_len);

// PC pass-through channel: the QMX's *second* virtual COM port (USB 2), opened
// independently of our own CAT port (USB 1). Because QMX treats the two ports
// as equivalent CAT channels and always answers on the port a command arrived
// on, the radio arbitrates the two masters itself -- we need no response
// dispatch, no shared command queue and no cached view for correctness.
// Requires the QMX "USB serial ports" setting to be 2 (needs a QMX restart).
// Bytes arriving from the QMX on this port are handed to pc_link_from_qmx().
int  cat_host_pc_is_connected(void);
int  cat_host_pc_send(const char *data, int len);

#ifdef __cplusplus
}
#endif
