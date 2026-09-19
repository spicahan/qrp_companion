# Native PAL/DSP regressions

Run from the repository root:

```sh
bash tests/run_native.sh
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
