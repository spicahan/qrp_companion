# Tab5 DX software validation

Validated source: `cd5f8f9` on branch `tab5_dx`, 2026-09-19.
This record is a documentation-only follow-up to that source commit.

## Result

All four build variants and all five offline regression runners passed.
No firmware was flashed, no serial port was opened, and no RF transmission
was requested. These results do not substitute for supervised board testing.

| Build | Result | Application size |
| --- | --- | --- |
| Tab5 DX, default zero IF | PASS | 619136 bytes (`0x97280`) |
| Tab5 DX, experimental Fs/4 | PASS | 619168 bytes (`0x972a0`) |
| Original Tab5 QMX platform | PASS | 673552 bytes (`0xa4710`) |
| Desktop Qt/PortAudio application, compile only | PASS | Not an embedded image |

ESP-IDF reported version 5.5.1. Existing SDK/library warnings remain; builds
completed without errors. The desktop application was not launched because
its original CAT transport automatically probes serial devices.

## Reproduction

Activate ESP-IDF, then use the dedicated build command in
[TAB5_DX.md](TAB5_DX.md). The verified default image is
`build-tab5-dx/qrp_companion.bin`. Do not use the QMX `build/` image on the RF
board or substitute its partition table.

The Fs/4 variant was built in an isolated temporary directory using the same
defaults plus a final override file:

```ini
# CONFIG_QRP_DX_IF_ZERO is not set
CONFIG_QRP_DX_IF_FS4=y
```

Its generated configuration and linked `getInputOffsetHz()` both return
12000 Hz. The regular DX profile remains zero IF. QMX retains its own
12000 Hz adapter metadata regardless of the unused DX configuration value.

Run the hardware-free regressions from the repository root:

```sh
bash tests/run_native.sh
bash tests/run_ui.sh
bash tests/run_rf_engine.sh
bash tests/run_synth.sh
bash tests/run_rf_audio.sh
```

All run with address/undefined-behavior sanitizers. Coverage includes:

- Actual QMX adapter/CAT parser, band metadata and existing tuning behavior.
- Actual DSP with zero and 12 kHz offsets, signed test tones on all spans,
  VFO marker placement, CW pitch and absent/reinstalled audio output callbacks.
- Actual UI/widgets, both display orientations and radio capabilities, deliberate
  start/stop gestures, state-driven red border, cancellation, recovery and
  non-editable LPF bands.
- Actual RX/TX engine with injected hardware failures, persistent marker
  handling, normal/hard cutoff, cooldown, no-op retuning, preparation
  cancellation, shutdown and confirmed-frequency behavior.
- Actual PCM word conversion and LPF classifier, including single-sample
  outliers, ADC rails and incomplete/overflowed batches.
- Actual Si5351 driver, independently decoded frequency vectors and 404
  injected transaction/readback failures, plus readiness/PLL/reset timeouts.
- Fast receive retuning: 72 additional injected transaction/readback failures,
  lock timeout/transient-lock/dead-bus cases. An immediately locked retune uses
  one PLLB write burst and 13 total transport calls, with no fixed sleep.
- Production engine: 100 successive retunes leave I2S/G47 running, perform no
  LPF reads, NVS writes or full clock plans, and retain guarded TX sequencing.
- Latest-wins mailbox: 20 ms coalescing, in-flight TX exclusion, final gesture
  target, no-op requests, and cancellation of pending intent across faults.
- Actual audio dispatcher with native queue/task adapters: bounded fresh-data
  buffering, copied producer data, stale-after-dequeue invalidation, callback
  close/reopen barriers, initialization failure and CPU0 task affinity.
- UI: cancel/cooldown/retry without leaving DR, automatic button readiness,
  disabled tuning/cooldown gestures, continuous asynchronous drag tuning,
  touch dispatch before housekeeping/FFT, and no diagnostic header writes.

Framebuffer snapshots from the UI harness were visually inspected for RX,
TX controls, read-only band tile and the power-cycle confirmation screen. The
normal screen uses the QMX layout without the former RF diagnostic rows. The
red outline fits the physical screen edges and is removed on return to RX.

## Linked-image and migration checks

- RF image has no linked `cat::`, `cat_host_*`, `uac_host_*` or
  `usb_host_install` symbols.
- Cutoff callback and `gpio_set_level` are in `.iram0.text`; the ISR lock is
  in `.dram0.data`, and the audio queue storage is in `.dram0.bss`. Timer ISR
  affinity is CPU0, RF/ADC worker is CPU1, and input DSP runs on CPU0 like QMX.
- Decoded DX partition table preserves NVS at `0x9000`, size `0x6000`, and
  PHY at `0xf000`, size `0x1000`.
- Generated DX `flash_args` writes only bootloader at `0x2000`, partition table
  at `0x8000`, and application at `0x10000`. There is **no write at `0xe000`**,
  avoiding the QMX OTA metadata collision with the existing NVS safety marker.
- Default DX image checksum/validation hash verified by the image parser.

Default DX binary SHA-256:

```text
aa0e3c135f229db6d8620bbce192fb17575b86061c8fa3368403e9fea8050a33
```

## Hardware work still required

The user reported that the initial backend worked on their board. This update's
fast tuning and audio-dispatch changes have not yet been flashed by this task;
actual touch latency, audio continuity and PLL settling require another check.

After explicitly arranging a safe flash, first validate boot with no carrier,
LPF sensing in the full application, shared internal I2C, and PCM stream startup.
Use a controlled weak external signal to check frequency direction, sensitivity,
gain and I/Q balance. Do not use the previously clipped PA-leakage loopback as
a receiver-quality measurement.

Only after receive checks, validate the diagnostic TX toggle into a rated
dummy load, its stop/timeout behavior, and RX recovery. Hardware cutoff latency,
physical power removal, RF input stress, PA current/temperature and mismatch
tolerance have not been established by these software tests.
