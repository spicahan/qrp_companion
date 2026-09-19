# Native PAL/DSP regressions

Run from the repository root:

```sh
bash tests/run_native.sh
bash tests/run_ui.sh
bash tests/run_rf_engine.sh
bash tests/run_synth.sh
```

The runner builds into a new temporary directory with address and undefined-
behavior sanitizers. It does not launch the desktop application, initialize
PortAudio, probe serial ports, or access either RF board.

Coverage:

- QMX radio PAL capability/state contract, using the actual adapter and CAT
  parser with an in-memory byte transport. Checks setup commands, fragmented
  responses/ANSI escapes, band enumeration with gaps, stable band metadata,
  tuning/poll suppression, CW mode/filter behavior, unsupported TX requests,
  and disconnected state. QMX is `Unmanaged`, not an invented RX indication.
- DSP zero-IF and +Fs/4 input conventions, each in a fresh process. Checks
  negative, zero, and positive offsets on all three spans, exact FFT/display
  bin placement, expected amplitude, VFO-marker fraction, callback chunk
  continuity, unsupported-offset rejection, CW pitch translation, and safe
  removal/reinstallation of an optional audio sink.

These are deterministic native software checks, not proof of analog bandwidth,
RF image rejection, hardware timing, or correct physical I/Q wiring. The RF
backend still needs a controlled weak-signal bench check after safe flashing.

`run_ui.sh` exercises the actual application/widgets with fake PAL/DSP inputs:
read-only bands, deliberate TX gestures, state-driven red border and restoration,
asynchronous capability discovery, cancellation, fault recovery and QMX controls.
Both physical rotated and ordinary desktop framebuffer layouts are covered.
See [UI_TESTS.md](UI_TESTS.md) for optional framebuffer snapshots.

`run_rf_engine.sh` exercises the production RX/TX sequencing engine with
injected peripherals: quiet LPF prechecks, normal stop, independent-cutoff fault,
cooldown, cancellation, inherited shutdown markers, readback failures and
40 m TX / 20 m RX-only restrictions. ESP hardware adapter, electrical timing,
interrupt delivery and actual bus/ADC faults still need hardware validation.
It also checks the actual PCM1808 word-to-I/Q conversion and LPF classifier
boundaries, including individual outliers that an average would hide.

`run_synth.sh` compiles the actual adapted Si5351 driver against a fake bus and
clock. It independently decodes 40 m / 20 m register plans, fractional tuning
and Fs/4 LO offsets, checks CLK1-only receive completion, and injects 404
transaction/readback failures plus readiness, PLL and reset timeouts. A dead
bus cannot prove clocks off; the separate GPIO cutoff remains essential.
