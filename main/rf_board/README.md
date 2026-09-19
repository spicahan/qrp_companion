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
`rf_audio.cpp` dispatches the input DSP/speaker callback separately on CPU0,
matching QMX's original audio-task affinity. A static four-block queue cannot
block the RF worker; generation tags invalidate queued samples on RX stop.
Normal TX shutdown is requested at 9.8 s so verified CLK0 disable can
complete before the independent 10 s emergency cutoff. An
actual emergency cutoff latches a persistent fault, not an automatic restart.

The shared `pa_bench/unverified` NVS key is never erased automatically. A
fault inherited from bring-up is preserved. The explicit UI acknowledgement
means the operator confirms a full physical RF-board power cycle; software
cannot sense it. Acknowledgement starts RX only, never TX.

Same-band RX tuning has a separate fast path: one PLLB feedback burst, changed
register readback, CLK1-only enable checks before/after, and bounded lock checks.
It keeps I2S and G47 running, leaves PLLA/MS0 and the output dividers untouched,
and does not reset the PLLs or discard another second of ADC data. A latest-wins
mailbox coalesces requests at a 20 ms minimum start interval; an internal busy
flag inhibits TX until the confirmed tuning operation and any queued request
finish. Preparation guards and fail-closed shutdown remain active. Startup and
TX-to-RX still use the full verified plan and one-second ADC settling period.

The register split follows [Skyworks AN619](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/AN619.pdf).
[Skyworks AN1423, Q8](https://www.skyworksinc.com/-/media/SkyWorks/SL/documents/public/application-notes/AN1423_Si5350_Si5351_FAQs.pdf)
describes the loss of inter-output skew guarantees after dynamic frequency
changes until reset. Only CLK1 runs during RX, and the QSD derives quadrature
externally from that one clock, so no inter-output phase guarantee is required.
Reset-free feedback adjustment is also implemented by the
[upstream Linux Si5351 driver](https://github.com/torvalds/linux/commit/b2adbc9cea752539f6421e9d4642408f666c1251).
The two clear status reads and 20 ms-plus-one-tick lock timeout are software
qualification, not a measured analog settling guarantee. The native test
enforces 13 transport callbacks / 522 clocked I2C bits for an immediately-locked
tune (about 5.22 ms at 100 kHz, excluding bus overhead and scheduling).
This is not a claim of glitch-free PLL updates.

RF capture remains on CPU1; a bounded four-block dispatcher invokes the DSP
callback on CPU0, independently of UI/control work. It drops old blocks when a
consumer falls behind rather than blocking capture. Stream shutdown invalidates
queued blocks, and callback close waits for an in-flight callback to return.

See `Docs/TAB5_DX.md` for operator-facing build, wiring and safety limitations.
The latest fast-tuning/audio-dispatch changes require on-board validation.
