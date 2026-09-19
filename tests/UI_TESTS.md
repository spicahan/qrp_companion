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
- The Band menu uses the original band tiles, with only the detected LPF shown
  and its tile disabled/read-only. QMX band switching and PC Link remain available.
- Normal RX/TX headers have no extra diagnostic/status/caution rows. Sentinel
  framebuffer pixels prove the vacated rows are not rewritten each frame.
- A TX request requires a complete down/up gesture on the button. Stray release,
  tuning drag, or leaving/reentering the button does not request TX.
  Pending touch events are consumed before battery I2C housekeeping and DSP work.
- Preparation can be cancelled; only confirmed TX/returning-RX states show red.
- Cancel/TX/RX/cooldown/retry works while DR stays open. The button counts down
  `Wait Ns` and automatically returns to `TX Test 10s`; only label/eligibility
  changes invalidate it. Pressing while disabled and releasing after cooldown
  expiry cannot start a transmission.
- Fast tuning retains Receive while the separate TX-readiness gate displays
  `Tuning...` on the disabled button. It becomes ready in place and never retries
  an earlier press automatically.
- A 2-pixel border covers all four screen edges after widget/waterfall drawing,
  is restored before the next frame, and leaves no residue when RX resumes.
  Standalone border checks also verify rotated mapping, padded strides, unchanged
  interior pixels and exact restoration over repeated applications.
- Tuning uses backend-confirmed frequency and is rejected while TX is active.
  A multi-MOVE drag remains active through asynchronous RX retunes and submits
  its final UP target without resetting DSP or restarting the gesture.
- Backend refusal is not optimistically treated as TX, and an autonomous return
  to RX never triggers another request.
- Fault/starting states do not request TX. Recovery requires two deliberate
  gestures with contextual physical power-cycle instructions rendered only on
  entry or a required header repaint. Back restores the original header, and
  acknowledgement never requests TX.

Snapshots are logical landscape views of each physical framebuffer, suitable
for direct visual comparison. They include RX, read-only LPF menu, RX/TX
diagnostics, restored RX, fault confirmation, and QMX diagnostics. A fake spectrum
is deliberate: these tests validate rendering and interaction, not analog signal
quality, real-time RF shutdown, or physical touch timing.
