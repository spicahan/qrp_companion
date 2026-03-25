#pragma once
#include <cstdint>

namespace cat {

void init();           // Start CAT polling (called from app::init)
void poll();           // Called from app::tick — handles send/receive timing

uint64_t getVfoFreq(); // Current VFO frequency in Hz, 0 if unknown
int      getMode();    // 1=LSB 2=USB 3=CW 6=DIGI, 0=unknown

const char* getModeStr(); // "LSB", "USB", "CW", "DIGI", or "---"

} // namespace cat
