#include "rf_board.h"
#include "rf_engine.h"
#include "rf_mailbox.h"
#include "rf_audio.h"
#include "iq_format.h"
#include "pal.h"
#include "lpf_sense.h"
#include "pcm1808_i2s.h"
#include "si5351.h"
#include "sdkconfig.h"
#include <M5Unified.h>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#if !CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD || !CONFIG_GPIO_CTRL_FUNC_IN_IRAM
#error "Tab5 DX requires ISR timer dispatch and IRAM GPIO control"
#endif
#if !CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0 || CONFIG_FREERTOS_UNICORE
#error "Tab5 DX ADC/worker must run on CPU1, independent of the CPU0 cutoff ISR"
#endif
#ifndef CONFIG_QRP_DX_IQ_OFFSET_HZ
#define CONFIG_QRP_DX_IQ_OFFSET_HZ 0
#endif
static_assert(CONFIG_QRP_DX_IQ_OFFSET_HZ == 0 || CONFIG_QRP_DX_IQ_OFFSET_HZ == 12000,
              "Only zero-IF and experimental Fs/4 are implemented");

namespace {
using rf_board::Band;
using rf_board::Snapshot;
using State = pal::radio::State;
constexpr gpio_num_t POWER = GPIO_NUM_48;
constexpr gpio_num_t RXSW = GPIO_NUM_47;
constexpr uint32_t BUS_HZ = 100000;
constexpr size_t FRAMES = rf_audio::BLOCK_FRAMES;
constexpr uint32_t RATE = 48000;
constexpr uint64_t PREP_US = 3000000;
const char *const TAG = "tab5_dx";

// All data touched by the ISR is statically allocated in internal DRAM.
portMUX_TYPE guard_mux = portMUX_INITIALIZER_UNLOCKED;
bool guard_tripped = false;
bool stop_requested = false;
bool guard_armed = false;
bool early_safe_ok = false;
Snapshot published;
Snapshot ui_snapshot;
TaskHandle_t worker_handle = nullptr;
bool start_tx_requested = false;
bool acknowledge_requested = false;
bool shutdown_requested = false;
rf_board::TuneMailbox tune_mailbox;

// Call only while holding guard_mux; the UI hint and actual request share the
// same authoritative predicate, but requestTransmit always checks it again.
bool tx_ready_locked()
{
    return worker_handle && published.state == State::Receive && published.band == Band::B40 &&
           !start_tx_requested && !stop_requested && !guard_tripped && !shutdown_requested &&
           !tune_mailbox.busyOrPending() &&
           static_cast<uint64_t>(esp_timer_get_time()) / 1000 >= published.cooldown_until_ms;
}

void IRAM_ATTR cutoff(void *)
{
    portENTER_CRITICAL_ISR(&guard_mux);
    gpio_set_level(POWER, 0);
    gpio_set_level(RXSW, 0);
    guard_tripped = true;
    stop_requested = true;
    portEXIT_CRITICAL_ISR(&guard_mux);
}

// M5GFX owns the bus and serializes start..stop transactions with its mutex.
// Always call stop, including on an intermediate failure, to release that lock.
esp_err_t bus_write(void *, uint8_t address, const uint8_t *bytes, size_t length)
{
    bool ok = M5.In_I2C.start(address, false, BUS_HZ);
    if (ok) ok = M5.In_I2C.write(bytes, length);
    const bool stopped = M5.In_I2C.stop();
    return ok && stopped ? ESP_OK : ESP_FAIL;
}
esp_err_t bus_read(void *, uint8_t address, uint8_t reg, uint8_t *value)
{
    bool ok = M5.In_I2C.start(address, false, BUS_HZ);
    if (ok) ok = M5.In_I2C.write(reg);
    if (ok) ok = M5.In_I2C.restart(address, true, BUS_HZ);
    if (ok) ok = M5.In_I2C.read(value, 1, true);
    const bool stopped = M5.In_I2C.stop();
    return ok && stopped ? ESP_OK : ESP_FAIL;
}
esp_err_t bus_probe(void *, uint8_t address)
{
    const bool ok = M5.In_I2C.start(address, false, BUS_HZ);
    const bool stopped = M5.In_I2C.stop();
    return ok && stopped ? ESP_OK : ESP_ERR_NOT_FOUND;
}

class EspHardware final : public rf_board::Hardware {
    si5351_t synthesizer{};
    pcm1808_i2s_t adc{};
    lpf_sense_t sense{};
    nvs_handle_t store = 0;
    esp_timer_handle_t preparation = nullptr;
    esp_timer_handle_t transmitter = nullptr;
    uint32_t words[FRAMES * 2]{};
    float iq[FRAMES * 2]{};
    bool initialized = false;
public:
    bool initialize(bool &pending) override
    {
        if (!early_safe_ok || M5.In_I2C.getSDA() != 31 || M5.In_I2C.getSCL() != 32)
            return false;
        // Never erase NVS automatically: it contains the interrupted-TX marker.
        if (nvs_flash_init() != ESP_OK ||
            nvs_open("pa_bench", NVS_READWRITE, &store) != ESP_OK) return false;
        uint8_t value = 0;
        const esp_err_t stored = nvs_get_u8(store, "unverified", &value);
        if (stored != ESP_OK && stored != ESP_ERR_NVS_NOT_FOUND) return false;
        pending = value != 0;
        const si5351_transport_t transport{nullptr, bus_write, bus_read, bus_probe};
        if (si5351_init(&synthesizer, &transport, SI5351_DEFAULT_I2C_ADDRESS,
                       26000000, SI5351_REFERENCE_EXTERNAL_CLOCK, BUS_HZ) != ESP_OK ||
            lpf_sense_init(&sense) != ESP_OK) return false;
        esp_timer_create_args_t config{};
        config.callback = cutoff;
        config.dispatch_method = ESP_TIMER_ISR;
        config.name = "dx_prep_guard";
        if (esp_timer_create(&config, &preparation) != ESP_OK) return false;
        config.name = "dx_tx_guard";
        if (esp_timer_create(&config, &transmitter) != ESP_OK) return false;
        // Prove that the independent ISR is live before allowing any RF power.
        if (esp_timer_start_once(preparation, 1000) != ESP_OK) return false;
        const int64_t deadline = esp_timer_get_time() + 200000;
        while (!hardTripped() && esp_timer_get_time() < deadline) vTaskDelay(1);
        if (!hardTripped() || gpio_get_level(POWER) != 0 || gpio_get_level(RXSW) != 0)
            return false;
        prepGuardStop();
        clearCancellation();
        initialized = true;
        ESP_LOGI(TAG, "ISR guard self-check passed with RF power held OFF");
        return true;
    }

    void publish(const Snapshot &snapshot) override
    {
        portENTER_CRITICAL(&guard_mux);
        if (snapshot.state != State::Receive) tune_mailbox.cancelPending();
        const bool routine_rx_update = published.state == State::Receive && snapshot.state == State::Receive;
        published = snapshot;
        portEXIT_CRITICAL(&guard_mux);
        if (!routine_rx_update) ESP_LOGI(TAG, "%s", snapshot.status);
    }
    uint64_t nowMs() override { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
    bool marker(bool pending) override
    {
        return store != 0 && nvs_set_u8(store, "unverified", pending ? 1 : 0) == ESP_OK &&
               nvs_commit(store) == ESP_OK;
    }
    void powerOff() override
    {
        portENTER_CRITICAL(&guard_mux);
        gpio_set_level(POWER, 0);
        gpio_set_level(RXSW, 0);
        portEXIT_CRITICAL(&guard_mux);
    }
    bool powerOn() override
    {
        portENTER_CRITICAL(&guard_mux);
        const bool permitted = initialized && guard_armed && !guard_tripped && !stop_requested;
        const bool ok = permitted && gpio_set_level(POWER, 1) == ESP_OK;
        portEXIT_CRITICAL(&guard_mux);
        return ok;
    }
    bool rxSwitch(bool receive) override
    {
        portENTER_CRITICAL(&guard_mux);
        const bool permitted = !receive || (!guard_tripped && !stop_requested);
        const bool ok = permitted && gpio_set_level(RXSW, receive ? 1 : 0) == ESP_OK;
        portEXIT_CRITICAL(&guard_mux);
        return ok;
    }
    bool clocksOff() override
    {
        return synthesizer.initialized &&
               si5351_set_enabled_outputs(&synthesizer, 0, true) == ESP_OK;
    }
    bool clockPlan(uint32_t frequency, Band band, bool receive) override
    {
        if (cancelled()) return false;
        const uint32_t lo = frequency - (receive ? CONFIG_QRP_DX_IQ_OFFSET_HZ : 0);
        return si5351_configure_radio_clock_plan(&synthesizer, lo, band == Band::B20, true) == ESP_OK &&
               !cancelled();
    }
    bool retuneReceive(uint32_t frequency, Band band) override
    {
        if (cancelled() || !adc.enabled) return false;
        const uint32_t lo = frequency - CONFIG_QRP_DX_IQ_OFFSET_HZ;
        return si5351_retune_receive(&synthesizer, lo, band == Band::B20) == ESP_OK &&
               !cancelled();
    }
    bool transmitClockOn() override
    {
        if (cancelled() || adc.rx_channel != nullptr) return false;
        // Preserve successful readback even if cancellation arrives during the
        // transaction: the engine can then perform normal guarded TX shutdown.
        return si5351_set_enabled_outputs(&synthesizer, SI5351_OUTPUT_CLK0, true) == ESP_OK;
    }
    bool qualifyBand(Band &result) override
    {
        // Worker is pinned to CPU1: a stuck ADC critical section cannot mask
        // the CPU0 cutoff. No sampling occurs once RF or I2S clocks run.
        if (adc.rx_channel != nullptr || cancelled()) return false;
        lpf_classification_t previous = LPF_CLASS_UNKNOWN;
        unsigned consecutive = 0;
        int64_t first_us = 0;
        int64_t previous_us = 0;
        const int64_t deadline = esp_timer_get_time() + 600000;
        while (esp_timer_get_time() < deadline && !cancelled()) {
            lpf_sense_result_t reading{};
            const int64_t start = esp_timer_get_time();
            const esp_err_t error = lpf_sense_read_batch(&sense, &reading);
            const int64_t finish = esp_timer_get_time();
            const bool valid = error == ESP_OK && finish >= start && finish - start <= 50000 &&
                (reading.classification == LPF_CLASS_40M || reading.classification == LPF_CLASS_20M);
            if (!valid || reading.classification != previous ||
                (previous_us != 0 && (start <= previous_us || start - previous_us > 100000))) {
                consecutive = 0;
                first_us = start;
            }
            previous = reading.classification;
            previous_us = start;
            if (valid && ++consecutive >= 5 && start - first_us >= 100000 && !cancelled()) {
                result = previous == LPF_CLASS_40M ? Band::B40 : Band::B20;
                ESP_LOGI(TAG, "Quiet LPF: %s, %d mV, range %d..%d, latched (no runtime ADC)",
                         lpf_classification_name(previous), reading.mv_mean, reading.mv_min, reading.mv_max);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(25) + 1);
        }
        return false;
    }
    bool startAudio() override
    {
        pcm1808_i2s_config_t config{};
        config.port = I2S_NUM_1;
        config.mclk_gpio = GPIO_NUM_16;
        config.bclk_gpio = GPIO_NUM_45;
        config.lrck_gpio = GPIO_NUM_3;
        config.din_gpio = GPIO_NUM_4;
        config.sample_rate_hz = RATE;
        if (cancelled() || pcm1808_i2s_init(&adc, &config) != ESP_OK ||
            pcm1808_i2s_enable(&adc) != ESP_OK) return false;
        size_t remaining = RATE;
        while (remaining > 0 && !cancelled()) {
            size_t received = 0;
            if (pcm1808_i2s_read(&adc, words, remaining < FRAMES ? remaining : FRAMES,
                                &received, 30) != ESP_OK || received == 0) return false;
            remaining -= received;
        }
        return remaining == 0 && !cancelled();
    }
    bool stopAudio() override
    {
        rf_audio::invalidate();
        const bool ok = pcm1808_i2s_deinit(&adc) == ESP_OK;
        // Deleting the channel plus resetting routing removes P4's MCLK.
        if (ok) {
            const gpio_num_t pins[] = {GPIO_NUM_16, GPIO_NUM_45, GPIO_NUM_3, GPIO_NUM_4};
            for (gpio_num_t pin : pins) {
                gpio_reset_pin(pin);
                gpio_set_pull_mode(pin, GPIO_FLOATING);
            }
        }
        return ok;
    }
    bool deliverAudio()
    {
        size_t received = 0;
        if (pcm1808_i2s_read(&adc, words, FRAMES, &received, 30) != ESP_OK || received == 0)
            return false;
        if (!rf_board::convertPcm1808IQ(words, iq, received)) return false;
        rf_audio::submit(iq, received);
        return true;
    }
    bool prepGuardStart() override
    {
        if (!preparation || cancelled()) return false;
        if (esp_timer_start_once(preparation, PREP_US) != ESP_OK) return false;
        portENTER_CRITICAL(&guard_mux);
        guard_armed = true;
        portEXIT_CRITICAL(&guard_mux);
        return !cancelled();
    }
    void prepGuardStop() override
    {
        if (preparation) (void)esp_timer_stop(preparation);
        portENTER_CRITICAL(&guard_mux);
        guard_armed = false;
        portEXIT_CRITICAL(&guard_mux);
    }
    bool txGuardStart() override
    {
        return transmitter && !cancelled() &&
            esp_timer_start_once(transmitter,
                static_cast<uint64_t>(rf_board::Engine::TX_HARD_LIMIT_MS) * 1000) == ESP_OK;
    }
    void txGuardStop() override { if (transmitter) (void)esp_timer_stop(transmitter); }
    bool cancelled() override
    {
        portENTER_CRITICAL(&guard_mux);
        const bool value = stop_requested || guard_tripped;
        portEXIT_CRITICAL(&guard_mux);
        return value;
    }
    bool hardTripped() override
    {
        portENTER_CRITICAL(&guard_mux);
        const bool value = guard_tripped;
        portEXIT_CRITICAL(&guard_mux);
        return value;
    }
    void clearCancellation() override
    {
        portENTER_CRITICAL(&guard_mux);
        if (!shutdown_requested) {
            stop_requested = false;
            guard_tripped = false;
        }
        portEXIT_CRITICAL(&guard_mux);
    }
    bool waitMs(uint32_t milliseconds) override
    {
        const uint64_t until = nowMs() + milliseconds;
        while (nowMs() < until && !cancelled()) vTaskDelay(1);
        return !cancelled();
    }
};

EspHardware hardware;
rf_board::Engine engine(hardware);

void worker(void *)
{
    (void)engine.initialize();
    for (;;) {
        portENTER_CRITICAL(&guard_mux);
        const bool stop = shutdown_requested;
        const bool tx = start_tx_requested;
        const bool ack = acknowledge_requested;
        const uint64_t frequency = tune_mailbox.claim(published, tx,
            static_cast<uint64_t>(esp_timer_get_time()) / 1000);
        start_tx_requested = false;
        acknowledge_requested = false;
        portEXIT_CRITICAL(&guard_mux);
        if (stop) { engine.shutdown(); break; }
        if (ack) (void)engine.acknowledgePowerCycle();
        if (tx) (void)engine.beginTransmit();
        if (frequency) {
            (void)engine.tune(frequency);
            portENTER_CRITICAL(&guard_mux);
            tune_mailbox.complete();
            portEXIT_CRITICAL(&guard_mux);
        }
        engine.serviceTransmit();
        if (engine.snapshot().state == State::Receive) {
            if (!hardware.deliverAudio()) engine.streamFailed();
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    portENTER_CRITICAL(&guard_mux);
    worker_handle = nullptr;
    portEXIT_CRITICAL(&guard_mux);
    vTaskDelete(nullptr);
}
} // namespace

bool rf_board::earlySafe()
{
    // Set output latches LOW before changing direction, avoiding a high glitch.
    bool ok = gpio_set_level(POWER, 0) == ESP_OK && gpio_set_level(RXSW, 0) == ESP_OK;
    gpio_config_t config{};
    config.pin_bit_mask = (1ULL << POWER) | (1ULL << RXSW);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ok = gpio_config(&config) == ESP_OK && ok;
    ok = gpio_set_level(POWER, 0) == ESP_OK && gpio_set_level(RXSW, 0) == ESP_OK && ok;
    early_safe_ok = ok;
    return ok;
}

namespace pal {
bool audioInputOpen(AudioInputCallback callback)
{
    return rf_audio::open(callback);
}
void audioInputClose() { rf_audio::close(); }
} // namespace pal

namespace pal::radio {
bool init()
{
    if (!early_safe_ok || worker_handle) return false;
    if (!rf_audio::initialize()) {
        published.state = State::Fault;
        std::strcpy(published.status, "Audio dispatcher initialization failed; RF power OFF");
        ui_snapshot = published;
        return false;
    }
    published.state = State::Starting;
    std::strcpy(published.status, "Starting RF board; no boot TX");
    ui_snapshot = published;
    return xTaskCreatePinnedToCore(worker, "dx_rf", 8192, nullptr, 3, &worker_handle, 1) == pdPASS;
}
void shutdown()
{
    // App exit is an immediate power-off request; interrupted TX marker remains.
    portENTER_CRITICAL(&guard_mux);
    stop_requested = true;
    shutdown_requested = true;
    gpio_set_level(POWER, 0);
    gpio_set_level(RXSW, 0);
    portEXIT_CRITICAL(&guard_mux);
    pal::audioInputClose();
}
void poll()
{
    portENTER_CRITICAL(&guard_mux);
    ui_snapshot = published;
    portEXIT_CRITICAL(&guard_mux);
}
void tick1Hz() {} // LPF ADC is intentionally NOT polled during RX or TX.
State getState() { return ui_snapshot.state; }
const char *getStatusText() { return ui_snapshot.status; }
uint64_t getVfoFreq() { return ui_snapshot.frequency; }
int getMode() { return ui_snapshot.frequency ? 6 : 0; }
int getCwOffset() { return 0; }
const char *getModeStr() { return ui_snapshot.frequency ? "I/Q" : "---"; }
void setVfoFreq(uint64_t frequency)
{
    portENTER_CRITICAL(&guard_mux);
    if (published.state == State::Receive && !start_tx_requested && !stop_requested && !guard_tripped &&
        rf_board::Engine::frequencyAllowed(published.band, frequency)) tune_mailbox.request(frequency);
    portEXIT_CRITICAL(&guard_mux);
}
void suppressPolling(bool) {}
void setCwFilter(const char *) {} // Local DSP APF only; this board has no CAT.
int getInputOffsetHz() { return CONFIG_QRP_DX_IQ_OFFSET_HZ; }
int getBandCount() { return ui_snapshot.band == Band::Unknown ? 0 : 1; }
const BandInfo *getBand(int index)
{
    static const BandInfo band40{40, "40"};
    static const BandInfo band20{20, "20"};
    if (index != 0) return nullptr;
    return ui_snapshot.band == Band::B40 ? &band40 : ui_snapshot.band == Band::B20 ? &band20 : nullptr;
}
bool isBandEnumDone() { return ui_snapshot.band != Band::Unknown || ui_snapshot.state == State::Fault; }
int getBandIndex() { const auto *band = getBand(0); return band ? band->index : -1; }
bool bandSelectionReadOnly() { return true; }
void setBandIndex(int) {}
bool txTestSupported() { return ui_snapshot.band == Band::B40; }
bool txTestReady()
{
    portENTER_CRITICAL(&guard_mux);
    const bool ready = tx_ready_locked();
    portEXIT_CRITICAL(&guard_mux);
    return ready;
}
uint32_t getTxTestLimitMs() { return rf_board::Engine::TX_HARD_LIMIT_MS; }
uint32_t getTxCooldownRemainingMs()
{
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
    return now < ui_snapshot.cooldown_until_ms
        ? static_cast<uint32_t>(ui_snapshot.cooldown_until_ms - now) : 0;
}
bool requestTransmit(bool enable)
{
    bool accepted = false;
    portENTER_CRITICAL(&guard_mux);
    if (!enable) {
        if (published.state == State::PreparingTx || published.state == State::Transmit || start_tx_requested) {
            stop_requested = true;
            start_tx_requested = false;
            accepted = true;
        } else accepted = published.state == State::Receive;
    } else if (tx_ready_locked()) {
        start_tx_requested = true;
        published.state = State::PreparingTx;
        std::strcpy(published.status, "TX test queued; tap again to cancel");
        accepted = true;
    }
    if (!accepted) {
        if (published.state == State::Fault) {
            // Preserve the useful underlying fault reason.
        } else if (published.band != Band::B40) {
            std::strcpy(published.status, "TX test is commissioned only for a detected 40m LPF");
        } else if (tune_mailbox.busyOrPending()) {
            std::strcpy(published.status, "Finishing RX tuning; retry TX test");
        } else {
            const uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
            if (now < published.cooldown_until_ms) {
                std::snprintf(published.status, sizeof(published.status),
                              "TX cooldown: %u seconds remaining",
                              static_cast<unsigned>((published.cooldown_until_ms - now + 999) / 1000));
            } else std::strcpy(published.status, "TX request refused: receiver not ready");
        }
    }
    ui_snapshot = published;
    portEXIT_CRITICAL(&guard_mux);
    return accepted;
}
bool powerCycleAcknowledgementRequired() { return ui_snapshot.power_cycle_required; }
bool acknowledgePowerCycle()
{
    portENTER_CRITICAL(&guard_mux);
    const bool ok = worker_handle && published.state == State::Fault && published.power_cycle_required;
    if (ok) acknowledge_requested = true;
    portEXIT_CRITICAL(&guard_mux);
    return ok;
}
} // namespace pal::radio
