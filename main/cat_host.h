#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize CDC-ACM driver for CAT control.
// Must be called BEFORE uac_host_start() so the CDC driver is installed
// before the UAC driver on composite USB devices.
void cat_host_init(void);

// Start periodic CAT polling (spawns its own task).
// Call after USB host is running.
void cat_host_start(void);

// Get the current VFO frequency in Hz. Returns 0 if not yet received.
uint64_t cat_get_vfo_freq(void);

#ifdef __cplusplus
}
#endif
