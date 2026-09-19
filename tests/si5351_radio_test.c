// Actual synthesizer driver, fake shared-bus transport; no hardware access.
#include "si5351.h"
#include "freertos/task.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { TRACE_CAPACITY = 4096, ADDRESS = 0x60, REFERENCE_HZ = 26000000 };
typedef struct { char kind; uint8_t reg, value; size_t length; } operation_t;
typedef struct {
    uint8_t registers[256];
    operation_t trace[TRACE_CAPACITY];
    unsigned transactions;
    unsigned fail_nth;
    bool sticky_failure;
    bool fail_after_commit;
    unsigned corrupt_read_nth;
    bool clear_reset_strobe;
    bool lock_fault_after_reset;
    bool reset_written;
    unsigned transient_lock_reads;
    uint8_t last_requested_disable_mask;
    bool requested_clk0_on;
    unsigned probes;
} fake_bus_t;

static fake_bus_t bus;
static TickType_t ticks;

TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay)
{
    assert(delay <= 1000);
    ticks += delay;
    assert(ticks < 10000); // a broken polling loop must not hang this suite
}

static bool record(fake_bus_t *target, char kind, uint8_t reg, uint8_t value, size_t length)
{
    assert(target == &bus);
    assert(target->transactions < TRACE_CAPACITY);
    target->trace[target->transactions++] = (operation_t){kind, reg, value, length};
    return target->fail_nth != 0 &&
           (target->transactions == target->fail_nth ||
            (target->sticky_failure && target->transactions > target->fail_nth));
}

static esp_err_t fake_write(void *context, uint8_t address, const uint8_t *data, size_t length)
{
    fake_bus_t *target = context;
    assert(address == ADDRESS);
    assert(data && length >= 2 && length <= 17);
    assert((size_t)data[0] + length - 1 <= sizeof(target->registers));
    const bool failing = record(target, 'W', data[0], data[1], length);
    if (data[0] == 3) {
        target->last_requested_disable_mask = data[1];
        if ((data[1] & 1U) == 0) target->requested_clk0_on = true;
    }
    if (failing && !target->fail_after_commit) return ESP_FAIL;
    memcpy(&target->registers[data[0]], &data[1], length - 1);
    if (data[0] == 177) target->reset_written = true;
    return failing ? ESP_FAIL : ESP_OK;
}

static esp_err_t fake_read(void *context, uint8_t address, uint8_t reg, uint8_t *value)
{
    fake_bus_t *target = context;
    assert(address == ADDRESS && value);
    const bool failing = record(target, 'R', reg, 0, 1);
    if (failing) return ESP_FAIL;
    if (reg == 177 && target->clear_reset_strobe)
        target->registers[reg] &= (uint8_t)~0xa0U;
    *value = target->registers[reg];
    if (reg == 0 && target->reset_written && target->lock_fault_after_reset) *value |= 0x60;
    if (reg == 0 && target->transient_lock_reads) {
        --target->transient_lock_reads;
        *value |= 0x40;
    }
    if (target->transactions == target->corrupt_read_nth) *value ^= 1;
    return ESP_OK;
}

static esp_err_t fake_probe(void *context, uint8_t address)
{
    fake_bus_t *target = context;
    assert(address == ADDRESS);
    ++target->probes;
    return record(target, 'P', 0, 0, 0) ? ESP_ERR_NOT_FOUND : ESP_OK;
}

static si5351_transport_t transport(void)
{
    return (si5351_transport_t){&bus, fake_write, fake_read, fake_probe};
}

static si5351_t fixture(void)
{
    memset(&bus, 0, sizeof(bus));
    ticks = 0;
    bus.clear_reset_strobe = true;
    // Start from a deliberately unsafe/stale predecessor, not ideal reset
    // defaults. A radio-plan call must actively remove the previous carrier.
    bus.registers[3] = 0xfe;
    bus.registers[149] = 0xa5;
    bus.registers[165] = 7;
    bus.registers[166] = 9;
    bus.last_requested_disable_mask = 0xfe;
    si5351_t device;
    const si5351_transport_t callbacks = transport();
    assert(si5351_init(&device, &callbacks, ADDRESS, REFERENCE_HZ,
                       SI5351_REFERENCE_EXTERNAL_CLOCK, 100000) == ESP_OK);
    assert(bus.transactions == 0); // attaching must not create/reconfigure a bus
    return device;
}

// Independently decode AN619 P1/P2/P3 bytes, instead of repeating the driver's
// numerator-to-register algorithm or relying only on its canned vectors.
static long double ratio_at(unsigned base)
{
    const uint8_t *p = &bus.registers[base];
    const uint32_t p1 = ((uint32_t)(p[2] & 3) << 16) | ((uint32_t)p[3] << 8) | p[4];
    const uint32_t p2 = ((uint32_t)(p[5] & 15) << 16) | ((uint32_t)p[6] << 8) | p[7];
    const uint32_t p3 = ((uint32_t)(p[5] >> 4) << 16) | ((uint32_t)p[0] << 8) | p[1];
    assert(p3 != 0);
    return ((long double)p1 + 512.0L + (long double)p2 / p3) / 128.0L;
}

static void assert_disabled_after_failure(bool physical_off_known)
{
    assert(bus.trace[0].kind == 'W' && bus.trace[0].reg == 3 && bus.trace[0].value == 0xff);
    assert(bus.last_requested_disable_mask == 0xff);
    assert(!bus.requested_clk0_on);
    if (physical_off_known) assert(bus.registers[3] == 0xff);
}

static unsigned test_plan(uint32_t lo_hz, bool band20, bool verify, long double tolerance)
{
    si5351_t device = fixture();
    assert(si5351_configure_radio_clock_plan(&device, lo_hz, band20, verify) == ESP_OK);
    assert(bus.trace[0].kind == 'W' && bus.trace[0].reg == 3 && bus.trace[0].value == 0xff);
    assert(!bus.requested_clk0_on);
    assert(bus.registers[3] == 0xfd); // only CLK1, never any PA clock
    assert(bus.last_requested_disable_mask == 0xfd);
    assert(bus.registers[16] == 0x4f && bus.registers[17] == 0x6f);
    for (unsigned reg = 18; reg < 24; ++reg) assert(bus.registers[reg] == 0x80);
    assert(bus.registers[24] == 0 && bus.registers[25] == 0);
    assert(bus.registers[15] == 0 && bus.registers[183] == SI5351_REFERENCE_EXTERNAL_CLOCK);
    assert(bus.registers[149] == 0x25); // preserve unrelated bits, disable SSC
    assert(bus.registers[165] == 0 && bus.registers[166] == 0);
    const long double plla = REFERENCE_HZ * ratio_at(26);
    const long double pllb = REFERENCE_HZ * ratio_at(34);
    const long double tx_divider = band20 ? 56 : 112;
    assert(plla >= 600000000 && plla <= 900000000);
    assert(fabsl(plla - pllb) < .000001L);
    assert(ratio_at(42) == tx_divider && ratio_at(50) == tx_divider / 4);
    assert(fabsl(plla / ratio_at(42) - lo_hz) < tolerance);
    assert(fabsl(pllb / ratio_at(50) - (long double)lo_hz * 4) < tolerance * 4);
    // Both R dividers remain /1, no divide-by-four special mode.
    assert((bus.registers[44] & 0xfc) == 0 && (bus.registers[52] & 0xfc) == 0);
    return bus.transactions;
}

static void test_nominal_and_offsets(void)
{
    for (unsigned verify = 0; verify <= 1; ++verify) {
        test_plan(7074000, false, verify, .000001L);
        test_plan(7075000, false, verify, .000001L);
        test_plan(14074000, true, verify, .000001L);
        test_plan(14075000, true, verify, .000001L);
        // Experimental +Fs/4 input: the RF adapter requests LO=VFO-12k for RX.
        // These correspond to displayed 7.075/14.075MHz and CLK1=28.252/56.252MHz.
        test_plan(7075000 - 12000, false, verify, .000001L);
        test_plan(14075000 - 12000, true, verify, .000001L);
        test_plan(7075001, false, verify, .5L); // bounded fractional approximation
        test_plan(14075001, true, verify, .5L);
    }
    si5351_t device = fixture();
    assert(si5351_configure_40m_clock_plan(&device, 7074000, true) == ESP_OK);
    assert(bus.registers[3] == 0xfd);
}

static void test_input_ranges(void)
{
    const si5351_transport_t valid = transport();
    si5351_t device = {0};
    assert(si5351_init(NULL, &valid, ADDRESS, REFERENCE_HZ, SI5351_REFERENCE_EXTERNAL_CLOCK, 100000) == ESP_ERR_INVALID_ARG);
    assert(si5351_init(&device, NULL, ADDRESS, REFERENCE_HZ, SI5351_REFERENCE_EXTERNAL_CLOCK, 100000) == ESP_ERR_INVALID_ARG);
    si5351_transport_t missing = valid;
    missing.read = NULL;
    assert(si5351_init(&device, &missing, ADDRESS, REFERENCE_HZ, SI5351_REFERENCE_EXTERNAL_CLOCK, 100000) == ESP_ERR_INVALID_ARG);
    for (unsigned address = 0; address <= 0xff; ++address) {
        const esp_err_t expected = address >= 8 && address <= 0x77 ? ESP_OK : ESP_ERR_INVALID_ARG;
        assert(si5351_init(&device, &valid, address, REFERENCE_HZ, SI5351_REFERENCE_EXTERNAL_CLOCK, 100000) == expected);
    }
    assert(si5351_init(&device, &valid, ADDRESS, 9999999, SI5351_REFERENCE_EXTERNAL_CLOCK, 100000) == ESP_ERR_INVALID_ARG);
    assert(si5351_init(&device, &valid, ADDRESS, 40000001, SI5351_REFERENCE_EXTERNAL_CLOCK, 100000) == ESP_ERR_INVALID_ARG);
    assert(si5351_init(&device, &valid, ADDRESS, REFERENCE_HZ, (si5351_reference_mode_t)0, 100000) == ESP_ERR_INVALID_ARG);
    assert(si5351_init(&device, &valid, ADDRESS, REFERENCE_HZ, SI5351_REFERENCE_EXTERNAL_CLOCK, 0) == ESP_ERR_INVALID_ARG);
    assert(si5351_init(&device, &valid, ADDRESS, REFERENCE_HZ, SI5351_REFERENCE_EXTERNAL_CLOCK, 400001) == ESP_ERR_INVALID_ARG);
    assert(si5351_configure_radio_clock_plan(NULL, 7075000, false, true) == ESP_ERR_INVALID_ARG);
    const uint32_t invalid40[] = {0, 1, 5357142, 8035715, UINT32_MAX};
    const uint32_t invalid20[] = {0, 1, 10714285, 16071429, UINT32_MAX};
    for (unsigned band20 = 0; band20 <= 1; ++band20) {
        const uint32_t *values = band20 ? invalid20 : invalid40;
        for (unsigned i = 0; i < sizeof(invalid40) / sizeof(invalid40[0]); ++i) {
            device = fixture();
            assert(si5351_configure_radio_clock_plan(&device, values[i], band20, true) == ESP_ERR_INVALID_ARG);
            assert_disabled_after_failure(true);
        }
    }
    // Hardware-synth limits are deliberately wider than amateur bands; the RF
    // engine, tested separately, enforces LPF-band policy before invoking this.
    test_plan(5357143, false, true, .5L);
    test_plan(8035714, false, true, .5L);
    test_plan(10714286, true, true, .5L);
    test_plan(16071428, true, true, .5L);
}

static void test_transport_contract(void)
{
    si5351_t device = fixture();
    assert(si5351_probe(&device, 250) == ESP_OK && bus.probes == 1);
    const unsigned before = bus.transactions;
    uint8_t value = 0;
    bus.registers[42] = 0x5a;
    assert(si5351_read_register(&device, 42, &value) == ESP_OK && value == 0x5a);
    assert(bus.transactions == before + 1);
    assert(bus.trace[before].kind == 'R' && bus.trace[before].reg == 42);
    // One callback owns pointer-write+read atomicity; the driver must not split
    // that operation into an exposed write followed by a second bus callback.
    uint8_t data[17] = {1, 2, 3};
    assert(si5351_write_registers(&device, 42, data, 3) == ESP_OK);
    assert(bus.trace[bus.transactions - 1].kind == 'W');
    assert(bus.trace[bus.transactions - 1].length == 4); // register pointer + payload
    assert(memcmp(&bus.registers[42], data, 3) == 0);
    const unsigned count = bus.transactions;
    assert(si5351_write_registers(&device, 42, data, 0) == ESP_ERR_INVALID_ARG);
    assert(si5351_write_registers(&device, 42, data, 17) == ESP_ERR_INVALID_ARG);
    assert(si5351_write_registers(&device, 42, NULL, 3) == ESP_ERR_INVALID_ARG);
    assert(si5351_read_register(&device, 42, NULL) == ESP_ERR_INVALID_ARG);
    assert(bus.transactions == count);
    assert(si5351_deinit(&device) == ESP_OK && !device.initialized);
    assert(bus.transactions == count); // detach doesn't tear down shared bus
    assert(si5351_probe(&device, 250) == ESP_ERR_INVALID_ARG);
    assert(si5351_read_register(&device, 42, &value) == ESP_ERR_INVALID_ARG);
    assert(si5351_write_register(&device, 42, 0) == ESP_ERR_INVALID_ARG);
    assert(si5351_configure_radio_clock_plan(&device, 7075000, false, true) == ESP_ERR_INVALID_ARG);
    assert(bus.transactions == count);
}

static void test_fault_injection(void)
{
    unsigned failures_exercised = 0;
    for (unsigned band20 = 0; band20 <= 1; ++band20) {
        const uint32_t frequency = band20 ? 14075000 : 7075000;
        const unsigned operations = test_plan(frequency, band20, true, .000001L);
        for (unsigned after_commit = 0; after_commit <= 1; ++after_commit) {
            for (unsigned nth = 1; nth <= operations; ++nth) {
                si5351_t device = fixture();
                bus.fail_nth = nth;
                bus.fail_after_commit = after_commit;
                assert(si5351_configure_radio_clock_plan(&device, frequency, band20, true) != ESP_OK);
                assert_disabled_after_failure(true);
                ++failures_exercised;
            }
        }
        test_plan(frequency, band20, true, .000001L);
        operation_t success_trace[TRACE_CAPACITY];
        memcpy(success_trace, bus.trace, sizeof(success_trace));
        for (unsigned i = 0; i < operations; ++i) {
            if (success_trace[i].kind != 'R' || success_trace[i].reg == 0 || success_trace[i].reg == 177) continue;
            // The first SSC read supplies preserved reserved bits; it is not a
            // readback comparison and cannot detect a single corrupted bit.
            if (success_trace[i].reg == 149 && (i == 0 || success_trace[i - 1].kind != 'W')) continue;
            si5351_t device = fixture();
            bus.corrupt_read_nth = i + 1;
            const esp_err_t error = si5351_configure_radio_clock_plan(&device, frequency, band20, true);
            if (error != ESP_ERR_INVALID_RESPONSE)
                fprintf(stderr, "Unexpected corrupt-read result %d at operation %u reg %u\n",
                        error, i + 1, success_trace[i].reg);
            assert(error == ESP_ERR_INVALID_RESPONSE);
            assert_disabled_after_failure(true);
            ++failures_exercised;
        }
    }
    si5351_t device = fixture();
    bus.registers[0] = 0x88;
    assert(si5351_configure_radio_clock_plan(&device, 7075000, false, true) == ESP_ERR_TIMEOUT);
    assert_disabled_after_failure(true);
    device = fixture();
    bus.lock_fault_after_reset = true;
    assert(si5351_configure_radio_clock_plan(&device, 7075000, false, true) == ESP_ERR_TIMEOUT);
    assert_disabled_after_failure(true);
    device = fixture();
    bus.clear_reset_strobe = false;
    assert(si5351_configure_radio_clock_plan(&device, 7075000, false, true) == ESP_ERR_TIMEOUT);
    assert_disabled_after_failure(true);
    device = fixture();
    bus.fail_nth = 1;
    bus.sticky_failure = true;
    assert(si5351_configure_radio_clock_plan(&device, 7075000, false, true) != ESP_OK);
    assert_disabled_after_failure(false);
    assert(bus.registers[3] == 0xfe); // cannot claim RF off when the bus is dead
    assert(bus.transactions >= 2);   // nevertheless attempts best-effort OFF
    printf("Si5351 radio plans PASS (%u injected transaction/readback failures)\n", failures_exercised);
}

static void reset_trace(void)
{
    bus.transactions = 0;
    bus.fail_nth = bus.corrupt_read_nth = 0;
    bus.sticky_failure = bus.fail_after_commit = bus.requested_clk0_on = false;
    bus.last_requested_disable_mask = bus.registers[3];
    ticks = 0;
}

static si5351_t rx_fixture(bool band20)
{
    si5351_t device = fixture();
    const uint32_t frequency = band20 ? 14075000 : 7075000;
    assert(si5351_configure_radio_clock_plan(&device, frequency, band20, true) == ESP_OK);
    assert(device.radio_rx_ready && device.radio_band_20m == band20 && device.radio_rx_hz == frequency);
    reset_trace();
    return device;
}

static void assert_fast_failure(const si5351_t *device, uint32_t confirmed, bool physical_off_known)
{
    assert(!device->radio_rx_ready && device->radio_rx_hz == confirmed);
    assert(bus.last_requested_disable_mask == 0xff && !bus.requested_clk0_on);
    if (physical_off_known) assert(bus.registers[3] == 0xff);
    for (unsigned i = 0; i < bus.transactions; ++i) {
        if (bus.trace[i].kind != 'W') continue;
        assert(bus.trace[i].reg == 34 || (bus.trace[i].reg == 3 && bus.trace[i].value == 0xff));
    }
}

static void test_fast_retune_work_budget(void)
{
    for (unsigned band20 = 0; band20 <= 1; ++band20) {
        si5351_t device = rx_fixture(band20);
        const uint32_t frequencies40[] = {7075001, 7074000, 7063000, 7000000, 7300000};
        const uint32_t frequencies20[] = {14075001, 14074000, 14063000, 14000000, 14350000};
        const uint32_t *frequencies = band20 ? frequencies20 : frequencies40;
        for (unsigned j = 0; j < sizeof(frequencies40) / sizeof(frequencies40[0]); ++j) {
            uint8_t previous[256];
            memcpy(previous, bus.registers, sizeof(previous));
            reset_trace();
            const uint32_t frequency = frequencies[j];
            assert(si5351_retune_receive(&device, frequency, band20) == ESP_OK);
            assert(device.radio_rx_ready && device.radio_rx_hz == frequency);
            assert(bus.transactions == 13 && ticks == 0 && !bus.requested_clk0_on);
            unsigned writes = 0;
            unsigned clocked_bits = 0;
            for (unsigned i = 0; i < bus.transactions; ++i) {
                const operation_t *op = &bus.trace[i];
                if (op->kind == 'W') {
                    ++writes;
                    assert(op->reg == 34 && op->length == 9);
                    clocked_bits += (1 + op->length) * 9; // address + register/payload + ACKs
                } else {
                    assert(op->kind == 'R');
                    clocked_bits += 4 * 9; // addrW, pointer, addrR, data (+ACK/NACK)
                }
            }
            // 5.22ms of clocked bus bits at100kHz, not measured wall latency:
            // excludes START/STOP, scheduler, mutex, stretch, and lock retries.
            assert(writes == 1 && clocked_bits == 522);
            for (unsigned reg = 0; reg < sizeof(previous); ++reg)
                if (reg < 34 || reg > 41) assert(bus.registers[reg] == previous[reg]);
            const long double received = REFERENCE_HZ * ratio_at(34) / ratio_at(50);
            assert(fabsl(received - (long double)frequency * 4) < 2.0L);
            assert(bus.registers[3] == 0xfd);
        }
    }
    puts("Si5351 fast RX work budget PASS: 13 transactions, one PLLB burst, 522 clocked I2C bits, no fixed waits");
}

static void test_fast_retune_faults(void)
{
    unsigned failures = 0;
    for (unsigned band20 = 0; band20 <= 1; ++band20) {
        const uint32_t initial = band20 ? 14075000 : 7075000;
        const uint32_t next = initial + 100;
        si5351_t device = rx_fixture(band20);
        assert(si5351_retune_receive(&device, next, band20) == ESP_OK);
        const unsigned operations = bus.transactions;
        operation_t success[16];
        assert(operations <= sizeof(success) / sizeof(success[0]));
        memcpy(success, bus.trace, operations * sizeof(success[0]));
        for (unsigned after = 0; after <= 1; ++after) {
            for (unsigned nth = 1; nth <= operations; ++nth) {
                device = rx_fixture(band20);
                bus.fail_nth = nth;
                bus.fail_after_commit = after;
                assert(si5351_retune_receive(&device, next, band20) != ESP_OK);
                assert_fast_failure(&device, initial, true);
                ++failures;
            }
        }
        for (unsigned i = 0; i < operations; ++i) {
            if (success[i].kind != 'R' || success[i].reg == 0) continue;
            device = rx_fixture(band20);
            bus.corrupt_read_nth = i + 1;
            assert(si5351_retune_receive(&device, next, band20) != ESP_OK);
            assert_fast_failure(&device, initial, true);
            ++failures;
        }
        const uint8_t unhealthy_statuses[] = {0x80, 0x08, 0x40};
        for (unsigned i = 0; i < sizeof(unhealthy_statuses); ++i) {
            device = rx_fixture(band20);
            bus.registers[0] = unhealthy_statuses[i];
            assert(si5351_retune_receive(&device, next, band20) == ESP_ERR_TIMEOUT);
            assert(ticks == 21); //20ms + one scheduler tick, fake ticks are 1ms
            assert_fast_failure(&device, initial, true);
        }
        device = rx_fixture(band20);
        bus.registers[0] = 0x20; // unused PLLA lock does not block receiving
        bus.transient_lock_reads = 3;
        assert(si5351_retune_receive(&device, next, band20) == ESP_OK && ticks == 3);
        device = rx_fixture(band20);
        bus.fail_nth = 2;
        bus.sticky_failure = true;
        assert(si5351_retune_receive(&device, next, band20) != ESP_OK);
        assert_fast_failure(&device, initial, false);
        assert(bus.registers[3] == 0xfd); // best effort OFF is not physical proof
    }
    printf("Si5351 fast RX failures PASS (%u injected transaction/readback failures; lock timeout/recovery and dead bus)\n", failures);
}

static void test_fast_retune_preconditions(void)
{
    si5351_t device = {0};
    assert(si5351_retune_receive(NULL, 7075000, false) == ESP_ERR_INVALID_ARG);
    assert(si5351_retune_receive(&device, 7075000, false) == ESP_ERR_INVALID_ARG);
    device = rx_fixture(false);
    assert(si5351_retune_receive(&device, 14075000, true) == ESP_ERR_INVALID_STATE);
    assert_fast_failure(&device, 7075000, true);
    device = rx_fixture(false);
    assert(si5351_retune_receive(&device, UINT32_MAX, false) == ESP_ERR_INVALID_ARG);
    assert_fast_failure(&device, 7075000, true);
    device = fixture();
    assert(si5351_configure_radio_clock_plan(&device, 7075000, false, false) == ESP_OK);
    reset_trace();
    assert(!device.radio_rx_ready);
    assert(si5351_retune_receive(&device, 7075100, false) == ESP_ERR_INVALID_STATE);
    assert_fast_failure(&device, 7075000, true);
    device = rx_fixture(false);
    assert(si5351_write_register(&device, 16, 0x4f) == ESP_OK);
    assert(!device.radio_rx_ready); // even identical arbitrary writes invalidate cache
    reset_trace();
    assert(si5351_retune_receive(&device, 7075100, false) == ESP_ERR_INVALID_STATE);
    assert_fast_failure(&device, 7075000, true);
    device = rx_fixture(false);
    bus.registers[3] = 0xfe; // stale/external mutation unexpectedly enabled CLK0
    assert(si5351_retune_receive(&device, 7075100, false) == ESP_ERR_INVALID_STATE);
    assert_fast_failure(&device, 7075000, true);
    assert(bus.transactions == 2); // mask check then OFF, no PLL write
}

int main(void)
{
    test_nominal_and_offsets();
    test_input_ranges();
    test_transport_contract();
    test_fault_injection();
    test_fast_retune_work_budget();
    test_fast_retune_faults();
    test_fast_retune_preconditions();
    puts("40m/20m zero-IF and experimental Fs/4 plans; shared transport contract PASS");
    return 0;
}
