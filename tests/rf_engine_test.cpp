// Tests the production engine used by the ESP adapter. Simulated peripherals
// prove sequencing, not electrical safety, IRQ latency, or I2S continuity.
// c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
//  -Ipal -Imain/rf_board tests/rf_engine_test.cpp main/rf_board/rf_engine.cpp \
//  -o /tmp/qrp_rf_engine_test && /tmp/qrp_rf_engine_test
#include "rf_engine.h"
#include "iq_format.h"
#include "rf_mailbox.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace rf_board;
using State = pal::radio::State;

struct Sim final : Hardware {
    Snapshot published;
    Band sensed = Band::B40;
    bool pending = false, inherited = false, power = false, receive = false;
    bool audio = false, prep = false, tx_guard = false, cancel = false, trip = false;
    bool cancel_at_sample = false, cancel_at_plan = false, cancel_at_handoff = false;
    bool fail_off_when_powerless = false;
    bool key_write_then_verify_failure = false;
    bool cancel_after_key = false;
    bool trip_at_audio = false, trip_at_prep_stop = false;
    uint8_t mask = 0;
    uint64_t now = 0;
    unsigned keys = 0, samples = 0, clear_markers = 0, plans = 0;
    std::string fail_operation;
    std::vector<std::string> operations;
    bool event(const char *name) {
        operations.emplace_back(name);
        if (fail_operation == name) { fail_operation.clear(); return false; }
        return true;
    }
    bool initialize(bool &fault) override { fault = inherited; pending = inherited; return event("initialize"); }
    void publish(const Snapshot &snapshot) override { published = snapshot; }
    uint64_t nowMs() override { return now; }
    bool marker(bool value) override {
        if (!event(value ? "marker-set" : "marker-clear")) return false;
        pending = value;
        if (!value) ++clear_markers;
        return true;
    }
    void powerOff() override { event("power-off"); power = false; receive = false; }
    bool powerOn() override {
        assert(prep && pending);
        if (!event("power-on") || cancel || trip) return false;
        power = true; return true;
    }
    bool rxSwitch(bool value) override {
        if (value) assert(mask == 2 && !trip && !cancel);
        if (!event(value ? "rx-on" : "rx-off")) return false;
        receive = value; return true;
    }
    bool clocksOff() override {
        if (!event("clocks-off") || (fail_off_when_powerless && !power)) return false;
        mask = 0; return true;
    }
    bool clockPlan(uint32_t frequency, Band band, bool rx) override {
        assert(prep && mask == 0 && !audio && !receive);
        assert(Engine::frequencyAllowed(band, frequency));
        ++plans;
        if (!event(rx ? "rx-plan" : "tx-plan")) return false;
        mask = 2;
        if (cancel_at_plan && !rx) cancel = true;
        return true;
    }
    bool transmitClockOn() override {
        assert(power && tx_guard && !prep && pending && mask == 0 && !receive && !audio);
        assert(!cancel && !trip);
        if (key_write_then_verify_failure) { mask = 1; ++keys; return false; }
        if (!event("key")) return false;
        mask = 1; ++keys;
        if (cancel_after_key) cancel = true;
        return true;
    }
    bool qualifyBand(Band &band) override {
        assert(prep && power && mask == 0 && !audio && !receive);
        ++samples; now += 110;
        if (!event("sample")) return false;
        band = sensed;
        if (cancel_at_sample) cancel = true;
        return band != Band::Unknown;
    }
    bool startAudio() override {
        assert(mask == 2 && receive && prep && !cancel && !trip);
        if (!event("audio-start")) return false;
        audio = true; now += 1000;
        if (trip_at_audio) hardCutoff();
        return true;
    }
    bool stopAudio() override { if (!event("audio-stop")) return false; audio = false; return true; }
    bool prepGuardStart() override {
        assert(!prep);
        if (!event("prep-start") || cancel || trip) return false;
        prep = true; return true;
    }
    void prepGuardStop() override {
        if (prep && trip_at_prep_stop) hardCutoff();
        event("prep-stop"); prep = false;
        if (cancel_at_handoff && tx_guard) cancel = true;
    }
    bool txGuardStart() override {
        assert(prep && pending && !audio && mask == 0);
        if (!event("tx-guard-start")) return false;
        tx_guard = true; return true;
    }
    void txGuardStop() override { event("tx-guard-stop"); tx_guard = false; }
    bool cancelled() override { return cancel || trip; }
    bool hardTripped() override { return trip; }
    void clearCancellation() override { event("clear-cancel"); cancel = trip = false; }
    bool waitMs(uint32_t ms) override { now += ms; return event("wait") && !cancel && !trip; }
    void hardCutoff() { assert(tx_guard || prep); trip = cancel = true; power = receive = false; }
};

static void normal_rx_tx_rx()
{
    Sim hw; Engine engine(hw);
    assert(engine.initialize());
    assert(hw.keys == 0 && hw.samples == 1 && hw.audio && hw.receive && hw.mask == 2);
    assert(!hw.pending && !hw.prep && !hw.tx_guard);
    assert(engine.snapshot().frequency == 7074000);
    assert(engine.beginTransmit());
    assert(hw.keys == 1 && hw.samples == 2 && hw.mask == 1 && hw.pending && !hw.audio && !hw.receive);
    const unsigned sampled_before_tx = hw.samples;
    hw.now += Engine::TX_NORMAL_LIMIT_MS;
    engine.serviceTransmit();
    assert(engine.snapshot().state == State::Receive && hw.mask == 2 && hw.audio && hw.receive);
    assert(hw.samples == sampled_before_tx && !hw.pending && !hw.tx_guard);
    assert(!engine.beginTransmit()); // Cooldown cannot be bypassed.
    hw.now += Engine::COOLDOWN_MS;
    assert(engine.beginTransmit());
    hw.cancel = true;
    engine.serviceTransmit();
    assert(engine.snapshot().state == State::Receive && !hw.pending && !hw.cancel);
    engine.shutdown();
    assert(!hw.power && !hw.audio && engine.snapshot().state == State::Offline);
}

static void band_and_tuning()
{
    Sim hw; hw.sensed = Band::B20; Engine engine(hw);
    assert(engine.initialize());
    assert(engine.snapshot().band == Band::B20 && engine.snapshot().frequency == 14074000);
    assert(!engine.beginTransmit() && hw.keys == 0);
    assert(!engine.tune(7074000));
    assert(engine.tune(14075000));
    assert(engine.snapshot().frequency == 14075000 && hw.samples == 1);
    hw.fail_operation = "rx-plan";
    assert(!engine.tune(14076000));
    assert(engine.snapshot().frequency == 14075000 && !hw.power);
    Sim absent; absent.sensed = Band::Unknown; Engine absent_engine(absent);
    assert(!absent_engine.initialize() && absent.keys == 0 && !absent.power);
    assert(absent_engine.snapshot().frequency == 0);
}

static void safety_faults()
{
    Sim inherited; inherited.inherited = true; Engine old(inherited);
    assert(!old.initialize());
    assert(old.snapshot().power_cycle_required && inherited.pending && inherited.clear_markers == 0);
    assert(old.acknowledgePowerCycle());
    assert(old.snapshot().state == State::Receive && !inherited.pending);

    Sim hw; Engine engine(hw); assert(engine.initialize());
    assert(engine.beginTransmit());
    const unsigned clears_before = hw.clear_markers;
    hw.hardCutoff();
    engine.serviceTransmit();
    assert(engine.snapshot().state == State::Fault && engine.snapshot().power_cycle_required);
    assert(hw.pending && hw.clear_markers == clears_before && !hw.power);
    assert(!engine.returnToReceive() && !engine.beginTransmit());
    assert(engine.acknowledgePowerCycle());
    assert(engine.snapshot().state == State::Receive && !hw.pending);

    Sim failed; Engine bad(failed); assert(bad.initialize()); assert(bad.beginTransmit());
    failed.fail_operation = "clocks-off"; failed.fail_off_when_powerless = true;
    assert(!bad.returnToReceive() && bad.snapshot().power_cycle_required && failed.pending && !failed.power);
}

static void cancellations()
{
    for (int stage = 0; stage < 4; ++stage) {
        Sim hw; Engine engine(hw); assert(engine.initialize());
        if (stage == 0) hw.cancel = true;
        if (stage == 1) hw.cancel_at_sample = true;
        if (stage == 2) hw.cancel_at_plan = true;
        if (stage == 3) hw.cancel_at_handoff = true;
        (void)engine.beginTransmit();
        assert(hw.keys == 0 && engine.snapshot().state == State::Receive);
        assert(!hw.pending && !hw.cancel && !hw.tx_guard && !hw.prep && hw.audio);
    }
    Sim hw; Engine engine(hw); assert(engine.initialize());
    hw.cancel = true; engine.serviceTransmit();
    assert(!hw.cancel && engine.snapshot().state == State::Receive && hw.keys == 0);
    hw.cancel_after_key = true;
    assert(engine.beginTransmit());
    assert(hw.keys == 1 && engine.snapshot().state == State::Receive && !hw.pending);
}

static void preparation_failures()
{
    const char *stages[] = {"prep-start", "audio-stop", "clocks-off", "wait", "sample",
                            "tx-plan", "marker-set", "tx-guard-start", "key"};
    for (const char *stage : stages) {
        Sim hw; Engine engine(hw); assert(engine.initialize());
        hw.fail_operation = stage;
        assert(!engine.beginTransmit());
        assert(engine.snapshot().state == State::Fault && !hw.power && hw.keys == 0);
        assert(!hw.prep && !hw.tx_guard);
    }
    Sim changed; Engine engine(changed); assert(engine.initialize());
    changed.sensed = Band::B20;
    assert(!engine.beginTransmit() && changed.keys == 0 && !changed.power);
}

static void uncertain_and_interrupted_shutdowns()
{
    Sim exiting; Engine exit_engine(exiting); assert(exit_engine.initialize());
    assert(exit_engine.beginTransmit());
    exit_engine.shutdown();
    assert(exiting.pending && !exiting.power && exit_engine.snapshot().power_cycle_required);

    Sim uncertain; Engine unclear(uncertain); assert(unclear.initialize());
    uncertain.key_write_then_verify_failure = true;
    uncertain.fail_off_when_powerless = true;
    assert(!unclear.beginTransmit());
    assert(uncertain.keys == 1 && uncertain.pending && !uncertain.power);
    assert(unclear.snapshot().power_cycle_required);

    Sim storage; Engine nvs(storage); assert(nvs.initialize()); assert(nvs.beginTransmit());
    storage.fail_operation = "marker-clear";
    assert(!nvs.returnToReceive());
    assert(storage.pending && !storage.power && nvs.snapshot().power_cycle_required);

    // The startup marker was cleared after proving CLK0 OFF. A later guard
    // trip must CREATE a fresh persistent marker, not merely preserve one.
    Sim rx_trip; rx_trip.trip_at_audio = true; Engine startup(rx_trip);
    assert(!startup.initialize());
    assert(rx_trip.pending && startup.snapshot().power_cycle_required && !rx_trip.power);

    Sim tune_trip; Engine tuner(tune_trip); assert(tuner.initialize());
    tune_trip.trip_at_prep_stop = true;
    assert(!tuner.tune(7075000));
    assert(tune_trip.pending && tuner.snapshot().power_cycle_required && !tune_trip.power);
}

static void actual_pcm_format()
{
    // Left is Q, right is I. All 24 significant bits are retained.
    const uint32_t words[] = {0x80000000U, 0x7FFFFF00U, 0x00000100U, 0xFFFFFF00U};
    float iq[4]{};
    assert(convertPcm1808IQ(words, iq, 2));
    assert(iq[0] == 8388607.0f / 8388608.0f && iq[1] == -1.0f);
    assert(iq[2] == -1.0f / 8388608.0f && iq[3] == 1.0f / 8388608.0f);
    const uint32_t invalid[] = {0, 0x00000001};
    iq[0] = iq[1] = 0.25f;
    assert(!convertPcm1808IQ(invalid, iq, 1));
    assert(iq[0] == 0.25f && iq[1] == 0.25f);
    assert(!convertPcm1808IQ(nullptr, iq, 1));
}

static void actual_tune_mailbox()
{
    Sim hw; Engine engine(hw); assert(engine.initialize());
    const uint32_t current = engine.snapshot().frequency;
    const unsigned plans = hw.plans;
    assert(!claimTune(hw.published, current, false));
    assert(engine.tune(current));
    assert(hw.published.state == State::Receive && hw.plans == plans);
    assert(!claimTune(hw.published, 0, false));
    assert(!claimTune(hw.published, current + 1000, true));
    assert(hw.published.state == State::Receive);
    assert(claimTune(hw.published, current + 1000, false));
    assert(hw.published.state == State::Starting);
    assert(engine.tune(current + 1000));
    assert(hw.published.state == State::Receive && hw.published.frequency == current + 1000);
}

int main()
{
    normal_rx_tx_rx(); band_and_tuning(); safety_faults(); cancellations(); preparation_failures();
    uncertain_and_interrupted_shutdowns(); actual_pcm_format(); actual_tune_mailbox();
    std::puts("RF production engine sequencing tests passed");
}
