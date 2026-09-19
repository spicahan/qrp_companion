# Radio-control UI regression tests

Run `bash tests/run_ui.sh` from any directory. An optional output directory
argument saves PPM framebuffer snapshots, for example:

```sh
bash tests/run_ui.sh /tmp/qrp-ui-review
```

The test compiles the actual `core/app.cpp`, UI property store and widgets with
fake PAL/radio/DSP implementations. It opens no serial port, GUI, audio device
or hardware driver. AddressSanitizer and UndefinedBehaviorSanitizer are enabled;
the temporary binary is retained at the printed path. The two disabled warning
categories are existing unused virtual/callback parameters and partial aggregate
initializers in the application, not sanitizer exclusions.

Four variants cover RF-board and QMX capabilities in both unrotated desktop and
90-degree rotated Tab5 physical framebuffers. They check:

- Radio and I/Q offset initialization precede audio callbacks.
- LPF-derived RF bands are labels, not writable band buttons; QMX band switching
  and its PC Link diagnostics remain available.
- A TX request requires a complete down/up gesture on the button. Stray release,
  tuning drag, or leaving/reentering the button does not request TX.
- Preparation can be cancelled; only confirmed TX/returning-RX states show red.
- A 2-pixel border covers all four screen edges after widget/waterfall drawing,
  is restored before the next frame, and leaves no residue when RX resumes.
  Standalone border checks also verify rotated mapping, padded strides, unchanged
  interior pixels and exact restoration over repeated applications.
- Tuning uses backend-confirmed frequency and is rejected while TX is active.
- Backend refusal is not optimistically treated as TX, and an autonomous return
  to RX never triggers another request.
- Fault/starting states do not request TX. Recovery requires two deliberate
  gestures with visible physical power-cycle instructions; Back cancels, and
  acknowledgement never requests transmission.

Snapshots are logical landscape views of each physical framebuffer, suitable
for direct visual comparison. They include RX, read-only LPF menu, RX/TX
diagnostics, restored RX, fault confirmation, and QMX diagnostics. A fake spectrum
is deliberate: these tests validate rendering and interaction, not analog signal
quality, real-time RF shutdown, or physical touch timing.
