#pragma once
#include <cstdint>

namespace cat {

void init();
void poll();           // RX accumulation. Call every frame from app::tick.
void tick1Hz();        // Periodic CAT command sends. Call once per second.

uint64_t getVfoFreq(); // Current VFO frequency in Hz, 0 if unknown
int      getMode();    // 1=LSB 2=USB 3=CW 6=DIGI, 0=unknown
int      getCwOffset();// CW sidetone offset in Hz, 0 if unknown

const char* getModeStr(); // "LSB", "USB", "CW", "DIGI", or "---"

// Set VFO frequency (sends FA command)
void setVfoFreq(uint64_t freq_hz);

// Suppress periodic polling (e.g. during drag-to-tune)
void suppressPolling(bool suppress);

// Set QMX CW filter passband via MMCW|CW passband=<value>;
// Valid values: "None", "50", "100", "150", "200", "250", "300", "400", "500"
void setCwFilter(const char *bandwidth);

} // namespace cat
