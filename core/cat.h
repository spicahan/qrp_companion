#pragma once
#include <cstdint>

namespace cat {

void init();
void poll();           // Called from app::tick — handles send/receive timing

uint64_t getVfoFreq(); // Current VFO frequency in Hz, 0 if unknown
int      getMode();    // 1=LSB 2=USB 3=CW 6=DIGI, 0=unknown
int      getCwOffset();// CW sidetone offset in Hz, 0 if unknown

const char* getModeStr(); // "LSB", "USB", "CW", "DIGI", or "---"

// Set VFO frequency (sends FA command)
void setVfoFreq(uint64_t freq_hz);

// Suppress periodic polling (e.g. during drag-to-tune)
void suppressPolling(bool suppress);

} // namespace cat
