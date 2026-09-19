#pragma once
#include "radio.h"
#include <cstdint>

// This is the production sequencing engine, not a second model for the tests.
// Only the worker calls Engine; stop/cutoff flags live in Hardware and may be
// set concurrently. All clock changes, ADC reads and I2S changes are serialized.
namespace rf_board {
enum class Band { Unknown, B40, B20 };
struct Snapshot {
    pal::radio::State state = pal::radio::State::Offline;
    Band band = Band::Unknown;
    uint32_t frequency = 0;
    bool power_cycle_required = false;
    uint64_t cooldown_until_ms = 0;
    char status[144] = "RF board offline";
};

class Hardware {
public:
    virtual ~Hardware() = default;
    virtual bool initialize(bool &pending_fault) = 0;
    virtual void publish(const Snapshot &) = 0;
    virtual uint64_t nowMs() = 0;
    virtual bool marker(bool pending) = 0;
    virtual void powerOff() = 0;
    virtual bool powerOn() = 0;
    virtual bool rxSwitch(bool receive) = 0;
    virtual bool clocksOff() = 0; // Must write AND read back ALL outputs OFF.
    virtual bool clockPlan(uint32_t frequency, Band, bool receive) = 0;
    virtual bool transmitClockOn() = 0; // CLK0 only, with readback.
    virtual bool qualifyBand(Band &) = 0; // Quiet, calibrated, bounded/debounced.
    virtual bool startAudio() = 0; // 48k I/Q; discard one second before delivery.
    virtual bool stopAudio() = 0; // Delete channel; disable MCLK too.
    virtual bool prepGuardStart() = 0;
    virtual void prepGuardStop() = 0;
    virtual bool txGuardStart() = 0;
    virtual void txGuardStop() = 0;
    virtual bool cancelled() = 0;
    virtual bool hardTripped() = 0;
    virtual void clearCancellation() = 0;
    virtual bool waitMs(uint32_t) = 0;
};

class Engine {
public:
    static constexpr uint32_t TX_HARD_LIMIT_MS = 10000;
    // Leave room for a verified normal shutdown before the independent ISR.
    static constexpr uint32_t TX_NORMAL_LIMIT_MS = 9800;
    static constexpr uint32_t COOLDOWN_MS = 10000;
    explicit Engine(Hardware &hardware) : hw(hardware) {}
    bool initialize();
    bool acknowledgePowerCycle();
    bool tune(uint64_t frequency);
    bool beginTransmit();
    bool returnToReceive();
    void serviceTransmit();
    void streamFailed();
    void shutdown();
    const Snapshot &snapshot() const { return s; }
    static bool frequencyAllowed(Band, uint64_t);
private:
    Hardware &hw;
    Snapshot s;
    bool marker_pending = false;
    bool power_started = false;
    bool all_clocks_off = false;
    bool inherited_fault = false;
    bool frequency_confirmed = false;
    uint64_t tx_stop_ms = 0;
    bool startReceive(bool inspect_band);
    bool restoreReceive();
    bool fail(const char *reason);
    bool setMarker(bool pending);
    bool quiet();
    bool cancelPreparation();
    void state(pal::radio::State, const char *message);
};
} // namespace rf_board
