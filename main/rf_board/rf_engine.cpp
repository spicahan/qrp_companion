#include "rf_engine.h"
#include <cstdio>

namespace rf_board {
using State = pal::radio::State;

void Engine::state(State next, const char *message)
{
    s.state = next;
    std::snprintf(s.status, sizeof(s.status), "%s", message);
    hw.publish(s);
}

bool Engine::frequencyAllowed(Band band, uint64_t hz)
{
    // Hardware/LPF envelope only, not an operator licence or regional band plan.
    return (band == Band::B40 && hz >= 7000000 && hz <= 7300000) ||
           (band == Band::B20 && hz >= 14000000 && hz <= 14350000);
}

bool Engine::setMarker(bool pending)
{
    if (!hw.marker(pending)) {
        s.power_cycle_required = true;
        return false;
    }
    marker_pending = pending;
    return true;
}

bool Engine::quiet()
{
    if (!hw.stopAudio()) return false;
    if (!hw.clocksOff()) return false;
    all_clocks_off = true;
    return hw.rxSwitch(false);
}

bool Engine::fail(const char *reason)
{
    // GPIO shutdown is FIRST; cleanup may block or fail on a dead bus.
    hw.powerOff();
    hw.prepGuardStop();
    hw.txGuardStop();
    // A physical cutoff is a fault even if a later readback happens to work.
    // Persist it BEFORE considering any ordinary verified-cleanup marker clear.
    if (hw.hardTripped()) {
        s.power_cycle_required = true;
        if (!marker_pending) (void)setMarker(true);
    }
    (void)hw.stopAudio();
    (void)hw.rxSwitch(false);
    if (!all_clocks_off && power_started) all_clocks_off = hw.clocksOff();
    if (marker_pending && !inherited_fault && !s.power_cycle_required &&
        (!power_started || all_clocks_off)) {
        (void)setMarker(false);
    }
    if (marker_pending || hw.hardTripped()) s.power_cycle_required = true;
    power_started = false;
    if (!frequency_confirmed) s.frequency = 0;
    state(State::Fault, reason);
    return false;
}

bool Engine::initialize()
{
    hw.powerOff();
    bool pending = false;
    if (!hw.initialize(pending)) {
        s.power_cycle_required = true;
        return fail("RF initialization failed; power OFF");
    }
    marker_pending = pending;
    inherited_fault = pending;
    if (pending) {
        s.power_cycle_required = true;
        return fail("Unverified RF shutdown: full RF-board power-cycle, then acknowledge");
    }
    hw.clearCancellation();
    return startReceive(true);
}

bool Engine::startReceive(bool inspect_band)
{
    state(State::Starting, "RF startup: quiet LPF inspection, no TX");
    if (!hw.prepGuardStart()) return fail("RX preparation guard failed");
    if (!setMarker(true)) return fail("Cannot persist RF startup guard");
    all_clocks_off = false;
    if (hw.cancelled() || !hw.powerOn()) return fail("RF startup cancelled");
    power_started = true;
    if (!hw.waitMs(200) || !quiet() || hw.cancelled())
        return fail("RF startup: cannot verify clocks OFF");
    if (!setMarker(false)) return fail("Cannot clear verified startup marker");
    if (inspect_band) {
        Band detected = Band::Unknown;
        if (!hw.qualifyBand(detected) || hw.cancelled())
            return fail("LPF missing, unstable or unknown; RF power OFF");
        s.band = detected;
        if (!frequencyAllowed(s.band, s.frequency))
            s.frequency = s.band == Band::B40 ? 7074000 : 14074000;
    }
    return restoreReceive();
}

bool Engine::restoreReceive()
{
    if (hw.cancelled()) return fail("RX restoration cancelled; RF power OFF");
    // CLK0 remains disabled throughout the RX plan. Never close Q1 until the
    // carrier shutdown was verified, and never deliver pre-settling samples.
    all_clocks_off = false;
    if (!hw.clockPlan(s.frequency, s.band, true) || hw.cancelled())
        return fail("RX synthesizer configuration failed");
    frequency_confirmed = true;
    if (!hw.rxSwitch(true) || !hw.startAudio() || hw.cancelled())
        return fail("RX I2S startup failed; RF power OFF");
    hw.prepGuardStop();
    if (hw.hardTripped()) return fail("RX startup exceeded hardware guard");
    state(State::Receive, s.band == Band::B40
          ? "RX 40m; LPF ID latched; TX test requires dummy load"
          : "RX 20m; LPF ID latched; TX test not commissioned");
    return true;
}

bool Engine::tune(uint64_t frequency)
{
    if (s.state != State::Receive || !frequencyAllowed(s.band, frequency))
        return false;
    if (frequency == s.frequency) return true;
    state(State::Starting, "Retuning receiver; TX OFF");
    if (!hw.prepGuardStart() || !quiet() || hw.cancelled())
        return fail("Retune could not establish quiet state");
    const uint32_t previous = s.frequency;
    // Publish actual/confirmed frequency, not the GUI's optimistic request.
    all_clocks_off = false;
    if (!hw.clockPlan(static_cast<uint32_t>(frequency), s.band, true) || hw.cancelled()) {
        s.frequency = previous;
        return fail("Retune synthesizer failed");
    }
    all_clocks_off = false;
    s.frequency = static_cast<uint32_t>(frequency);
    if (!hw.rxSwitch(true) || !hw.startAudio() || hw.cancelled())
        return fail("Retune I2S startup failed");
    hw.prepGuardStop();
    if (hw.hardTripped()) return fail("Retune exceeded hardware guard");
    state(State::Receive, "RX; tuning confirmed, LPF ID latched");
    return true;
}

bool Engine::beginTransmit()
{
    if (s.state != State::Receive || s.band != Band::B40 ||
        !frequencyAllowed(s.band, s.frequency) ||
        hw.nowMs() < s.cooldown_until_ms) return false;
    if (hw.cancelled()) {
        if (hw.hardTripped()) return fail("TX request encountered hard cutoff");
        hw.clearCancellation();
        state(State::Receive, "TX cancelled before preparation; RX unchanged");
        return false;
    }
    state(State::PreparingTx, "Preparing TX test: receiver clocks OFF, fresh LPF check");
    if (!hw.prepGuardStart() || !quiet() || !hw.waitMs(50) || hw.cancelled()) {
        if (hw.cancelled() && !hw.hardTripped()) return cancelPreparation();
        return fail("TX preparation could not stop receive clocks");
    }
    Band current = Band::Unknown;
    if (!hw.qualifyBand(current) || current != s.band || hw.cancelled()) {
        if (hw.cancelled() && !hw.hardTripped()) return cancelPreparation();
        return fail("TX rejected: fresh quiet LPF ID does not match 40m");
    }
    // Program at the displayed RF frequency (NOT any experimental RX offset).
    all_clocks_off = false;
    if (!hw.clockPlan(s.frequency, s.band, false) || !quiet() || hw.cancelled()) {
        if (hw.cancelled() && !hw.hardTripped()) return cancelPreparation();
        return fail("TX clock programming failed");
    }
    if (!setMarker(true)) return fail("TX rejected: cannot persist shutdown marker");
    // The two guards overlap; there is no unguarded handoff to the carrier.
    if (!hw.txGuardStart()) {
        if (hw.cancelled() && !hw.hardTripped()) return cancelPreparation();
        return fail("TX hard cutoff could not be armed");
    }
    tx_stop_ms = hw.nowMs() + TX_NORMAL_LIMIT_MS;
    hw.prepGuardStop();
    if (hw.cancelled() && !hw.hardTripped()) return cancelPreparation();
    if (hw.cancelled()) return fail("TX cutoff before keying");
    all_clocks_off = false; // A write may succeed even if its readback fails.
    if (!hw.transmitClockOn())
        return fail("TX enable failed or cancelled; RF power OFF");
    if (hw.hardTripped()) return fail("TX cutoff during carrier enable");
    state(State::Transmit, "TX TEST: dummy load only; RX/QSD/I2S OFF; 10s hard limit");
    if (hw.cancelled()) return returnToReceive();
    return true;
}

bool Engine::cancelPreparation()
{
    // No carrier-enable write has happened. Nevertheless verify the mask again
    // before clearing cancellation and restoring G47/receiver clocks.
    state(State::ReturningRx, "TX preparation cancelled; restoring receiver");
    if (hw.hardTripped() || !quiet()) return fail("Cancelled TX could not verify clocks OFF");
    hw.prepGuardStop();
    hw.txGuardStop();
    if (hw.hardTripped()) return fail("Cutoff during cancelled TX cleanup");
    if (marker_pending && !setMarker(false)) return fail("Cancelled TX marker cleanup failed");
    hw.clearCancellation();
    if (!hw.prepGuardStart()) return fail("Cancelled TX: RX guard failed");
    return restoreReceive();
}

bool Engine::returnToReceive()
{
    if (s.state != State::Transmit) return false;
    state(State::ReturningRx, "Stopping carrier before restoring receiver");
    if (hw.hardTripped()) return fail("TX hard cutoff fired; power-cycle acknowledgement required");
    // Keep the independent guard armed across this potentially blocking I2C.
    if (!hw.clocksOff()) return fail("Cannot verify carrier OFF; RF power OFF");
    all_clocks_off = true;
    hw.txGuardStop();
    if (hw.hardTripped()) return fail("TX cutoff raced shutdown; RF power OFF");
    if (!setMarker(false)) return fail("Cannot persist verified carrier shutdown");
    s.cooldown_until_ms = hw.nowMs() + COOLDOWN_MS;
    hw.clearCancellation();
    if (!hw.prepGuardStart()) return fail("RX return guard failed");
    return restoreReceive();
}

void Engine::serviceTransmit()
{
    if (s.state == State::Receive && hw.cancelled()) {
        if (hw.hardTripped()) { (void)fail("Hardware cutoff; RF power OFF"); return; }
        // A queued TX request may have been cancelled before the worker took it.
        hw.clearCancellation();
        state(State::Receive, "Queued TX cancelled; RX unchanged");
    }
    if (s.state != State::Transmit) return;
    if (hw.hardTripped()) { (void)fail("TX hard cutoff fired; RF power OFF"); return; }
    if (hw.cancelled() || hw.nowMs() >= tx_stop_ms) (void)returnToReceive();
}

void Engine::streamFailed() { (void)fail("RX I2S stream failed; RF power OFF"); }

bool Engine::acknowledgePowerCycle()
{
    if (s.state != State::Fault || !s.power_cycle_required) return false;
    hw.powerOff();
    if (!hw.marker(false)) return false;
    marker_pending = false;
    inherited_fault = false;
    s.power_cycle_required = false;
    hw.clearCancellation();
    return startReceive(true);
}

void Engine::shutdown()
{
    hw.powerOff();
    hw.prepGuardStop();
    hw.txGuardStop();
    (void)hw.stopAudio();
    (void)hw.rxSwitch(false);
    // A TX interrupted by app exit retains its marker. Never invent readback.
    if (marker_pending) s.power_cycle_required = true;
    state(State::Offline, "RF power OFF");
}
} // namespace rf_board
