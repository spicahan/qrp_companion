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

#ifdef __cplusplus
}
#endif
