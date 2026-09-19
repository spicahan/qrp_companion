// The actual application/widget sources run against a fake radio and display.
// No ESP hardware, serial ports, audio device, or window system is accessed.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pal.h"
#include "dsp.h"
#include "../core/app.cpp"

namespace fake {
bool rotated = false, qmx = false, muted = false, ack_required = false;
bool radio_initialized = false, refuse_tx = false;
bool tx_supported = true;
bool tx_ready = true;
bool complete_tunes_async = false;
int tune_pending_ticks = 0;
int band_writes = 0, tune_writes = 0, acknowledgements = 0;
int64_t now = 1000000;
uint64_t frequency = 7075000;
uint64_t cooldown_until_us = 0, requested_frequency = 0;
pal::radio::State state = pal::radio::State::Receive;
std::string status = "RX ready; LPF 40m, TX test maximum 10s";
std::string snapshot_directory;
std::vector<uint16_t> pixels;
std::deque<pal::TouchEvent> events;
std::vector<bool> tx_requests;
pal::radio::BandInfo bands[] = {{0, "40"}, {1, "20"}};
float magnitudes[512];
int span = 1, input_offset = 0;
}

namespace pal {
bool init(int, int) { return true; }
void shutdown() {}
DisplayInfo getDisplayInfo() { return {1280, 720}; }
uint16_t *getFramebuffer() { return fake::pixels.data() + 1; }
int getFramebufferStride() { return fake::rotated ? 720 : 1280; }
bool isRotated() { return fake::rotated; }
void commitFrame()
{
    assert(fake::pixels.front() == 0xabcd);
    assert(fake::pixels.back() == 0xabcd);
}
int getExtendedHeight() { return fake::rotated ? 1280 : 720; }
void setVisibleOffset(int) {}
void blitBlock(const uint16_t *src, int ss, int sx, int sy,
               uint16_t *dst, int ds, int dx, int dy, int w, int h, bool mirror)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            dst[(dy + y) * ds + dx + x] = src[(sy + (mirror ? h - y - 1 : y)) * ss + sx + x];
}
bool pollEvent(TouchEvent &event)
{
    if (fake::events.empty()) return false;
    event = fake::events.front();
    fake::events.pop_front();
    return true;
}
void injectEvent(const TouchEvent &event) { fake::events.push_back(event); }
void pollBattery() { assert(fake::events.empty()); }
int getBatteryLevel() { return 75; }
bool isCharging() { return false; }
bool pcLinkSupported() { return fake::qmx; }
bool pcLinkRunning() { return false; }
void pcLinkStart() {}
bool pcLinkRelayConnected() { return false; }
int64_t micros() { return fake::now; }
void delayMs(int ms) { fake::now += ms * 1000; }
int freeHeapKb() { return 100; }
int freePsramKb() { return 10000; }
bool catIsConnected() { assert(false); return false; }
int catSend(const char *, int) { assert(false); return -1; }
int catRecv(char *, int) { assert(false); return -1; }
bool audioInputOpen(AudioInputCallback)
{
    assert(fake::radio_initialized);
    assert(fake::input_offset == (fake::qmx ? 12000 : 0));
    return true;
}
void audioInputClose() {}
bool audioOutputOpen(int) { return true; }
void audioOutputWrite(const float *, int) {}
void audioSetMuted(bool muted) { fake::muted = muted; }
bool audioIsMuted() { return fake::muted; }
void audioOutputClose() {}
}

namespace pal::radio {
bool init() { fake::radio_initialized = true; return true; }
void shutdown() {}
void poll()
{
    if (fake::tune_pending_ticks > 0 && --fake::tune_pending_ticks == 0)
        fake::frequency = fake::requested_frequency;
}
void tick1Hz() {}
State getState() { return fake::state; }
const char *getStatusText() { return fake::status.c_str(); }
uint64_t getVfoFreq() { return fake::frequency; }
int getMode() { return 3; }
int getCwOffset() { return fake::qmx ? 625 : 0; }
const char *getModeStr() { return "CW"; }
void setVfoFreq(uint64_t frequency)
{
    ++fake::tune_writes;
    if (fake::qmx) fake::frequency = frequency;
    else if (fake::complete_tunes_async) {
        fake::requested_frequency = frequency;
        fake::tune_pending_ticks = 2;
    }
}
void suppressPolling(bool) {}
void setCwFilter(const char *) {}
int getInputOffsetHz() { return fake::qmx ? 12000 : 0; }
int getBandCount() { return fake::qmx ? 2 : 1; }
const BandInfo *getBand(int index) { return index >= 0 && index < getBandCount() ? &fake::bands[index] : nullptr; }
bool isBandEnumDone() { return true; }
int getBandIndex() { return 0; }
bool bandSelectionReadOnly() { return !fake::qmx; }
void setBandIndex(int) { ++fake::band_writes; }
bool txTestSupported() { return !fake::qmx && fake::tx_supported; }
uint32_t getTxTestLimitMs() { return 10000; }
uint32_t getTxCooldownRemainingMs()
{
    return fake::cooldown_until_us > static_cast<uint64_t>(fake::now)
         ? static_cast<uint32_t>((fake::cooldown_until_us - fake::now + 999) / 1000) : 0;
}
bool txTestReady()
{
    return !fake::qmx && fake::tx_supported && fake::tx_ready &&
           fake::state == State::Receive && getTxCooldownRemainingMs() == 0 &&
           fake::tune_pending_ticks == 0;
}
bool requestTransmit(bool enable)
{
    fake::tx_requests.push_back(enable);
    if (fake::refuse_tx || (enable && getTxCooldownRemainingMs() != 0)) {
        fake::status = "TX refused: wait for cooldown";
        return false;
    }
    fake::state = enable ? State::PreparingTx : State::ReturningRx;
    fake::status = enable ? "Preparing TX; QSD and ADC stopped" : "Returning RX; waiting for confirmed carrier off";
    return true;
}
bool powerCycleAcknowledgementRequired() { return fake::ack_required; }
bool acknowledgePowerCycle()
{
    ++fake::acknowledgements;
    fake::ack_required = false;
    fake::state = State::Starting;
    fake::status = "Starting RX after operator power-cycle confirmation";
    return true;
}
}

namespace dsp {
void init(int, int)
{
    for (int i = 0; i < 512; ++i) fake::magnitudes[i] = -108.0f + static_cast<float>(i % 17);
    fake::magnitudes[276] = -43.0f;
    fake::magnitudes[300] = -57.0f;
}
bool setInputOffsetHz(int hz) { fake::input_offset = hz; return true; }
float getVfoFraction() { return fake::span == 0 ? .5f + fake::input_offset / 48000.0f : .5f; }
void setAudioOutCallback(AudioOutCallback) {}
int getDecimatedRate() { return 8000; }
void setAudioGain(float) {}
float getAudioGain() { return 1000; }
void setCwOffset(float) {}
void setModeKnown(bool) {}
void setApfEnabled(bool) {}
bool isApfEnabled() { return false; }
void startGoertzel() {}
bool isGoertzelRunning() { return false; }
float getGoertzelResult() { return 0; }
void clearGoertzelResult() {}
void pushIQ(const float *, int) {}
bool processIfReady() { assert(fake::events.empty()); return true; }
int getNumBins() { return 512; }
const float *getMagnitudeDb() { return fake::magnitudes; }
int displayBin(int index) { return index; }
void setSpan(int span) { fake::span = span; }
int getSpan() { return fake::span; }
int getSpanRate() { const int rates[] = {48000, 24000, 8000}; return rates[fake::span]; }
const char *getSpanLabel() { return "+/-12k"; }
}

static void tick()
{
    fake::now += 20000;
    app::tick();
}

static void tap(ui::Button &button)
{
    const int x = button.x + button.w / 2;
    const int y = button.y + button.h / 2;
    pal::injectEvent({x, y, pal::TouchEvent::DOWN});
    pal::injectEvent({x, y, pal::TouchEvent::UP});
    tick();
}

static uint16_t logical_pixel(int x, int y)
{
    return fb.buf[fb.rotated ? (fb.log_w - 1 - x) * fb.phys_w + y : y * fb.phys_w + x];
}

static void assert_border(bool red)
{
    for (int y = 0; y < log_h; ++y)
        for (int x = 0; x < log_w; ++x)
            if (x < 2 || x >= log_w - 2 || y < 2 || y >= log_h - 2)
                assert((logical_pixel(x, y) == COL_RED) == red);
}

static void assert_plain_header_rows()
{
    for (int y0 : {8, HEADER_H - 24})
        for (int y = y0; y < y0 + 16; ++y)
            for (int x = 2; x < log_w - 2; ++x)
                assert(logical_pixel(x, y) == COL_DGREY);
}

static void assert_header_rows_not_rewritten()
{
    constexpr uint16_t sentinel = 0x1357;
    for (int y0 : {8, HEADER_H - 24})
        draw::fillRect(fb, 2, y0, log_w - 4, 16, sentinel);
    tick();
    tick();
    for (int y0 : {8, HEADER_H - 24})
        for (int y = y0; y < y0 + 16; ++y)
            for (int x = 2; x < log_w - 2; ++x)
                assert(logical_pixel(x, y) == sentinel);
    header_band.dirty = true;
    tick();
    assert_plain_header_rows();
}

static void snapshot(const char *name)
{
    if (fake::snapshot_directory.empty()) return;
    std::filesystem::create_directories(fake::snapshot_directory);
    std::string filename = fake::snapshot_directory + "/" + (fake::qmx ? "qmx-" : "rf-") +
                           (fake::rotated ? "rotated-" : "desktop-") + name + ".ppm";
    std::ofstream output(filename, std::ios::binary);
    assert(output.good());
    output << "P6\n" << log_w << ' ' << log_h << "\n255\n";
    for (int y = 0; y < log_h; ++y) {
        for (int x = 0; x < log_w; ++x) {
            const uint16_t pixel = logical_pixel(x, y);
            const char rgb[] = {static_cast<char>(((pixel >> 11) & 31) * 255 / 31),
                                static_cast<char>(((pixel >> 5) & 63) * 255 / 63),
                                static_cast<char>((pixel & 31) * 255 / 31)};
            output.write(rgb, sizeof(rgb));
        }
    }
    assert(output.good());
}

static void border_restore_test()
{
    for (bool rotated : {false, true}) {
        constexpr int w = 14, h = 9;
        const int stride = (rotated ? h : w) + 3;
        const int rows = rotated ? w : h;
        std::vector<uint16_t> storage(stride * rows + 2);
        for (size_t i = 0; i < storage.size(); ++i) storage[i] = static_cast<uint16_t>(i + 1);
        const auto original = storage;
        draw::Framebuf target{storage.data() + 1, stride, rows, w, h, rotated};
        FrameBorder overlay;
        for (int pass = 0; pass < 3; ++pass) {
            overlay.apply(target, 0xf800);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int pos = rotated ? (w - 1 - x) * stride + y : y * stride + x;
                    assert(target.buf[pos] == ((x < 2 || x >= w - 2 || y < 2 || y >= h - 2)
                           ? 0xf800 : original[pos + 1]));
                }
            }
            overlay.restore();
            assert(storage == original); // includes stride padding and canaries
        }
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rotated") == 0) fake::rotated = true;
        else if (strcmp(argv[i], "--qmx") == 0) fake::qmx = true;
        else if (strcmp(argv[i], "--snapshots") == 0 && i + 1 < argc) fake::snapshot_directory = argv[++i];
        else { fprintf(stderr, "Unknown test argument: %s\n", argv[i]); return 2; }
    }
    border_restore_test();
    fake::pixels.assign(1280 * 720 + 2, 0xabcd);
    if (fake::qmx) fake::state = pal::radio::State::Unmanaged;
    app::init();
    tick();
    assert(fake::tx_requests.empty());
    assert_border(false);
    assert_plain_header_rows();
    assert_header_rows_not_rewritten();
    snapshot("rx");

    tap(btn_band);
    if (fake::qmx) {
        assert(panel_band_select.count == 3);
        tap(btn_bands[1]);
        assert(fake::band_writes == 1);
        tap(btn_dr);
        assert(panel_dr_bottom.count == 3);
        assert(panel_dr_bottom.widgets[1] == &btn_pclink);
        assert(fake::tx_requests.empty());
        snapshot("diagnostics");
        puts("UI QMX compatibility PASS");
        return 0;
    }
    assert(panel_band_select.count == 2);
    assert(strcmp(btn_band.label, "Band") == 0);
    assert(panel_band_select.widgets[0] == &btn_bands[0]);
    assert(strcmp(btn_bands[0].label, "40") == 0);
    assert(btn_bands[0].highlight && !btn_bands[0].enabled && !btn_bands[0].on_press);
    tap(btn_bands[0]);
    assert(fake::band_writes == 0);
    assert(bottom_band.active() == &panel_band_select);
    btn_band_select_press(btn_bands[0]);
    assert(fake::band_writes == 0);
    snapshot("lpf-readonly");
    tap(btn_back);
    tap(btn_dr);
    assert(panel_dr_bottom.count == 3);
    assert(panel_dr_bottom.widgets[1] == &btn_tx_test);

    // Fast retuning deliberately keeps the public state Receive; readiness is
    // a separate backend gate. Never make a refused TX attempt look like a tap
    // vanished, and never retry on the user's behalf when tuning completes.
    fake::tx_ready = false;
    tick();
    assert(fake::state == pal::radio::State::Receive);
    assert(strcmp(btn_tx_test.label, "Tuning...") == 0 && !btn_tx_test.enabled);
    const auto pending_tune_requests = fake::tx_requests.size();
    tap(btn_tx_test);
    assert(fake::tx_requests.size() == pending_tune_requests);
    fake::tx_ready = true;
    tick();
    assert(strcmp(btn_tx_test.label, "TX Test 10s") == 0 && btn_tx_test.enabled);
    assert(fake::tx_requests.size() == pending_tune_requests);
    assert(strcmp(btn_tx_test.label, "TX Test 10s") == 0);
    assert_plain_header_rows();
    snapshot("diagnostics-rx");

    // LPF discovery is asynchronous: the DR panel can already be open when
    // the backend gains/loses the TX capability. No menu reopen is needed.
    fake::tx_supported = false;
    tick();
    assert(panel_dr_bottom.count == 2);
    fake::tx_supported = true;
    tick();
    assert(panel_dr_bottom.count == 3);
    assert(panel_dr_bottom.widgets[1] == &btn_tx_test);

    const int tx_x = btn_tx_test.x + btn_tx_test.w / 2;
    const int tx_y = btn_tx_test.y + btn_tx_test.h / 2;
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::UP});
    tick();
    assert(fake::tx_requests.empty()); // stray release cannot key TX
    pal::injectEvent({10, 200, pal::TouchEvent::DOWN});
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::UP});
    tick();
    assert(fake::tx_requests.empty()); // tuning drag cannot key TX
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::DOWN});
    pal::injectEvent({10, 200, pal::TouchEvent::MOVE});
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::UP});
    tick();
    assert(fake::tx_requests.empty()); // leaving/reentering cancels press
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::DOWN});
    pal::injectEvent({btn_dr_default.x + 20, tx_y, pal::TouchEvent::MOVE});
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::UP});
    tick();
    assert(fake::tx_requests.empty()); // earlier sibling must not hide cancellation

    const auto confirmed_frequency = ui::get_u64(ui::PROP_vfo_freq);
    ui::set_u64(ui::PROP_vfo_freq, confirmed_frequency + 1000);
    assert(fake::tune_writes == 1);
    assert(ui::get_u64(ui::PROP_vfo_freq) == confirmed_frequency);
    tap(btn_tx_test);
    assert(fake::tx_requests.size() == 1 && fake::tx_requests.back());
    assert(fake::state == pal::radio::State::PreparingTx);
    assert_border(false); // accepted request is not confirmed TX
    assert(strcmp(btn_tx_test.label, "Cancel TX prep") == 0);
    tap(btn_tx_test);
    assert(fake::tx_requests.size() == 2 && !fake::tx_requests.back());
    fake::state = pal::radio::State::Receive;
    tick();

    tap(btn_tx_test);
    fake::state = pal::radio::State::Transmit;
    fake::status = "TX test active; 40m carrier; maximum 10s";
    tick();
    assert_border(true);
    assert(strcmp(btn_tx_test.label, "Return RX") == 0);
    snapshot("diagnostics-tx");
    tick();
    assert_border(true); // animation/widget redraw cannot overwrite the border
    ui::set_u64(ui::PROP_vfo_freq, confirmed_frequency + 2000);
    assert(fake::tune_writes == 1);
    assert(ui::get_u64(ui::PROP_vfo_freq) == confirmed_frequency);
    tap(btn_tx_test);
    assert(!fake::tx_requests.back());
    assert_border(true); // keep warning until off is confirmed
    fake::state = pal::radio::State::Receive;
    fake::status = "RX ready; cooldown 10s";
    fake::cooldown_until_us = fake::now + 10000000;
    tick();
    assert_border(false); // no red residue on retained/background pixels
    assert(strcmp(btn_tx_test.label, "Wait 10s") == 0 && !btn_tx_test.enabled);
    assert(bottom_band.active() == &panel_dr_bottom);
    assert_plain_header_rows();
    snapshot("rx-restored");
    const auto during_cooldown_count = fake::tx_requests.size();
    tap(btn_tx_test);
    assert(fake::tx_requests.size() == during_cooldown_count);
    // Only a label/eligibility change invalidates the panel. Ordinary idle
    // frames do not rewrite the label or continually rebuild the touch target.
    bottom_band.dirty = false;
    update_tx_button();
    assert(!bottom_band.dirty);
    fake::now += 2000000;
    update_tx_button();
    assert(bottom_band.dirty && strcmp(btn_tx_test.label, "Wait 8s") == 0);
    bottom_band.dirty = false;
    update_tx_button();
    assert(!bottom_band.dirty);
    // Pressing while disabled and releasing after expiry is NOT a new TX tap.
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::DOWN});
    tick();
    fake::now = static_cast<int64_t>(fake::cooldown_until_us) + 1;
    pal::injectEvent({tx_x, tx_y, pal::TouchEvent::UP});
    tick();
    assert(fake::tx_requests.size() == during_cooldown_count);
    assert(strcmp(btn_tx_test.label, "TX Test 10s") == 0 && btn_tx_test.enabled);
    assert(bottom_band.active() == &panel_dr_bottom);
    snapshot("diagnostics-ready-again");
    tap(btn_tx_test); // retry without ever leaving/reopening DR
    assert(fake::tx_requests.size() == during_cooldown_count + 1 && fake::tx_requests.back());
    assert(fake::state == pal::radio::State::PreparingTx);
    tap(btn_tx_test); // preparation cancellation immediately remains usable
    assert(!fake::tx_requests.back());
    fake::state = pal::radio::State::Receive;
    tick();
    assert(bottom_band.active() == &panel_dr_bottom);
    assert(strcmp(btn_tx_test.label, "TX Test 10s") == 0);
    assert_header_rows_not_rewritten();
    const auto request_count = fake::tx_requests.size();
    fake::state = pal::radio::State::Transmit;
    tick();
    fake::state = pal::radio::State::Receive; // independent timeout completion
    tick();
    assert_border(false);
    assert(fake::tx_requests.size() == request_count); // never repeat TX
    fake::refuse_tx = true;
    tap(btn_tx_test);
    assert(fake::state == pal::radio::State::Receive);
    assert_border(false);
    assert(strstr(fake::status.c_str(), "refused") != nullptr);
    fake::refuse_tx = false;

    fake::state = pal::radio::State::Fault;
    fake::status = "Fault: power off RF board completely before recovery";
    fake::ack_required = true;
    tick();
    assert(panel_dr_bottom.count == 4);
    const auto fault_tx_requests = fake::tx_requests.size();
    tap(btn_tx_test);
    assert(fake::tx_requests.size() == fault_tx_requests);
    tap(btn_power_cycle);
    assert(power_cycle_confirmation);
    assert(fake::acknowledgements == 0);
    snapshot("fault-confirmation");
    // A legitimate header repaint must not erase an open confirmation.
    header_band.dirty = true;
    tick();
    bool confirmation_text_present = false;
    for (int y = 8; y < 24; ++y)
        for (int x = 16; x < log_w - 16; ++x)
            if (logical_pixel(x, y) == COL_YELLOW) confirmation_text_present = true;
    assert(confirmation_text_present);
    pal::injectEvent({btn_power_cycle.x + 20, tx_y, pal::TouchEvent::UP});
    tick();
    assert(fake::acknowledgements == 0);
    tap(btn_dr_back);
    assert(!power_cycle_confirmation);
    assert_plain_header_rows();
    tap(btn_dr);
    tap(btn_power_cycle);
    const auto before_ack_tx_requests = fake::tx_requests.size();
    tap(btn_power_cycle);
    assert(fake::acknowledgements == 1);
    assert(fake::state == pal::radio::State::Starting);
    assert(fake::tx_requests.size() == before_ack_tx_requests);
    tap(btn_tx_test);
    assert(fake::tx_requests.size() == before_ack_tx_requests);
    assert_border(false);

    // The asynchronous backend keeps Receive while a lightweight retune is
    // pending. Multiple MOVE samples must continue a single drag; the displayed
    // VFO follows acknowledgements rather than cancelling/restarting the gesture.
    fake::state = pal::radio::State::Receive;
    fake::complete_tunes_async = true;
    tick();
    const uint64_t drag_origin = fake::frequency;
    const int tune_count = fake::tune_writes;
    const int x0 = spec_x + spec_w / 2;
    const int y0 = spec_y + 40;
    pal::injectEvent({x0, y0, pal::TouchEvent::DOWN});
    tick();
    for (int delta : {10, 20, 30}) {
        pal::injectEvent({x0 + delta, y0, pal::TouchEvent::MOVE});
        tick();
        assert(dragging && fake::state == pal::radio::State::Receive);
        assert(strcmp(btn_tx_test.label, "Tuning...") == 0 && !btn_tx_test.enabled);
    }
    assert(fake::tune_writes >= tune_count + 3);
    pal::injectEvent({x0 + 30, y0, pal::TouchEvent::UP});
    tick();
    assert(!dragging);
    tick();
    tick();
    const uint64_t expected_tune = (static_cast<int64_t>(drag_origin) - 30 * dsp::getSpanRate() / spec_w) / 10 * 10;
    assert(fake::frequency == expected_tune);
    assert(ui::get_u64(ui::PROP_vfo_freq) == expected_tune);
    assert(strcmp(btn_tx_test.label, "TX Test 10s") == 0 && btn_tx_test.enabled);

    tx_border.restore();
    puts(fake::rotated ? "UI RF rotated framebuffer PASS" : "UI RF desktop framebuffer PASS");
}
