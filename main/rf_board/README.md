# Tab5 DX backend internals

`rf_engine.cpp` is the production radio sequencing engine. The ESP/M5 adapter
in `rf_board.cpp` owns the worker, ISR cutoff, persistent fault marker, shared
M5 internal-I2C transport, and I2S delivery. The engine is compiled unchanged
into the native `tests/rf_engine_test.cpp` failure-injection harness.

The low-level PCM1808, calibrated LPF sense/classifier and Si5351 code were
imported from the user's `/Users/spica/tab5_dx` bring-up project, firmware
commit `010e8e0`. They are local source, not a dependency on that directory.
The Si5351 transport has been adapted to callbacks so the existing M5 bus
owner/mutex is respected; its integer clock plan now supports 20 m RX as well
as 40 m. The PAL never calls the imported continuous-carrier/loopback helpers.

The worker runs on CPU1 because the ESP-IDF ADC oneshot conversion busy-waits
inside a critical section. The independent preparation and TX ISR guards run
on CPU0 and only lower G48/G47; no I2C, logging, NVS or allocation occurs there.
Normal TX shutdown is requested at 9.8 s so verified CLK0 disable and RX
restoration can complete before the independent 10 s emergency cutoff. An
actual emergency cutoff latches a persistent fault, not an automatic restart.

The shared `pa_bench/unverified` NVS key is never erased automatically. A
fault inherited from bring-up is preserved. The explicit UI acknowledgement
means the operator confirms a full physical RF-board power cycle; software
cannot sense it. Acknowledgement starts RX only, never TX.

See `Docs/TAB5_DX.md` for operator-facing build, wiring and safety limitations.
No new backend code has been flashed or electrically validated at this stage.
