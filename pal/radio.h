#pragma once
#include <cstdint>

// Radio control is separate from the display/audio board PAL. Core UI and DSP
// must not depend on CAT, USB, or a particular synthesizer/LPF implementation.
// Calls are made by the UI thread; asynchronous backends publish snapshots in
// poll(). Commands request changes, never imply that hardware already changed.
namespace pal::radio {

enum class State {
    Offline, Unmanaged, Starting, Receive, PreparingTx, Transmit, ReturningRx, Fault
};

bool init();
void shutdown();
void poll();
void tick1Hz();
State getState();
const char* getStatusText(); // Stable until the next UI-thread poll/command.

uint64_t getVfoFreq(); // Backend-confirmed RF tuning reference, Hz; 0 if unknown.
int getMode();        // Existing mode IDs: 1 LSB, 2 USB, 3 CW, 6 DIGI, 0 unknown.
int getCwOffset();
const char* getModeStr();
void setVfoFreq(uint64_t hz);
void suppressPolling(bool suppress);
void setCwFilter(const char* bandwidth);

// Location of the tuning reference in the incoming complex 48 kHz stream.
// QMX is +Fs/4; zero-IF receivers use 0. Set before starting audio delivery.
int getInputOffsetHz();

inline constexpr int MAX_BANDS = 16;
struct BandInfo { int index; char name[8]; };
int getBandCount();
const BandInfo* getBand(int i); // Backend-owned stable storage, nullptr if absent.
bool isBandEnumDone();
int getBandIndex();
bool bandSelectionReadOnly();
void setBandIndex(int index); // Must reject/no-op when selection is read-only.

// Diagnostic carrier only, not a general modulation/PTT API. False requests
// cancellation/return to RX, including while preparation is still in progress.
// Hardware backends must enforce their cutoff independently of UI progress.
bool txTestSupported();
uint32_t getTxTestLimitMs();
uint32_t getTxCooldownRemainingMs(); // 0 when ready; does not itself authorize TX.
bool txTestReady(); // Current preconditions, including retune exclusion; requests recheck.
bool requestTransmit(bool enable);

// Fault recovery requires the operator's explicit physical power-cycle
// acknowledgement. Rebooting, navigating menus, or requesting RX is not consent.
bool powerCycleAcknowledgementRequired();
bool acknowledgePowerCycle();

} // namespace pal::radio
