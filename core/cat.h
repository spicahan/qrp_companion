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

// ── Bands ───────────────────────────────────────────────────────────
// The QMX "Band config." table has 16 columns; the column index is the value
// the BN command takes. We enumerate the table at startup (a few columns per
// 1 Hz tick) so the UI reflects the bands this particular radio actually has —
// QMX and QMX+ differ, and users can customise the table.
// Requires firmware with BN + the Band config. grid; there is no fallback.
static constexpr int MAX_BANDS = 16;

struct BandInfo {
    int  index;      // BN index (column in the Band config. table)
    char name[8];    // band name in metres, e.g. "40", "160"
};

int  getBandCount();              // configured bands found (0 until enumerated)
const BandInfo* getBand(int i);   // i in [0, getBandCount()), else nullptr
bool isBandEnumDone();            // true once the table has been swept
int  getBandIndex();              // current BN index, -1 if unknown

// Switch band by BN index. The QMX restores that band's last-used frequency.
void setBandIndex(int index);

} // namespace cat
