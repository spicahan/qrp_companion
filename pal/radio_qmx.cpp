#include "radio.h"
#include "pal.h"
#include "cat_transport.h"
#include "cat.h"
#include <cstring>

namespace pal::radio {
static BandInfo bands[MAX_BANDS]{};
bool init() { cat::init(); return true; }
void shutdown() {}
void poll() {
    cat::poll();
    for (int i = 0; i < MAX_BANDS; ++i) {
        const auto* band = cat::getBand(i);
        if (band) { bands[i].index = band->index; std::memcpy(bands[i].name, band->name, sizeof(bands[i].name)); }
        else { bands[i] = {}; }
    }
}
void tick1Hz() { cat::tick1Hz(); }
// The existing companion protocol does not query PTT. Do not invent RX state.
State getState() { return pal::catIsConnected() ? State::Unmanaged : State::Offline; }
const char* getStatusText() {
    return pal::catIsConnected() ? "QMX companion; TX/RX managed by radio" : "Waiting for QMX";
}
uint64_t getVfoFreq() { return cat::getVfoFreq(); }
int getMode() { return cat::getMode(); }
int getCwOffset() { return cat::getCwOffset(); }
const char* getModeStr() { return cat::getModeStr(); }
void setVfoFreq(uint64_t hz) { cat::setVfoFreq(hz); }
void suppressPolling(bool suppress) { cat::suppressPolling(suppress); }
void setCwFilter(const char* bandwidth) { cat::setCwFilter(bandwidth); }
int getInputOffsetHz() { return 12000; }
int getBandCount() { return cat::getBandCount(); }
const BandInfo* getBand(int i) { return i >= 0 && i < getBandCount() ? &bands[i] : nullptr; }
bool isBandEnumDone() { return cat::isBandEnumDone(); }
int getBandIndex() { return cat::getBandIndex(); }
bool bandSelectionReadOnly() { return false; }
void setBandIndex(int index) { cat::setBandIndex(index); }
bool txTestSupported() { return false; }
uint32_t getTxTestLimitMs() { return 0; }
bool requestTransmit(bool) { return false; }
bool powerCycleAcknowledgementRequired() { return false; }
bool acknowledgePowerCycle() { return false; }
} // namespace pal::radio
