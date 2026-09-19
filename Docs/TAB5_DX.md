# Tab5 DX RF-board platform

This backend runs the companion UI directly against the Si5351, PCM1808 and
GPIO-controlled RF daughter board. It does not open a USB CAT transport, start
the QMX UAC host, or offer the PC CAT relay. The existing QMX and desktop builds
remain available through the same radio PAL contract.

## Build the correct platform

From the repository root, with ESP-IDF 5.5.1 activated:

```sh
idf.py -B build-tab5-dx -D SDKCONFIG=sdkconfig.tab5_dx \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.tab5_dx.defaults' build
```

Do not use a plain `idf.py flash` in the old default `build/` directory: that
is the QMX platform, not the RF-board backend. A full clean is not normally
needed; separate output/configuration paths prevent profile contamination.

Flashing is a separate operator-controlled step. Disconnect/power down the RF
board and close other serial monitors first. Reuse the exact build/profile
arguments above when invoking `flash`, adding the verified current Tab5 port.
Do not erase NVS to bypass a safety fault. Initial integration is build/native
test validated only until an explicit supervised hardware validation is done.

The dedicated `partitions.tab5_dx.csv` preserves the bring-up firmware's full
24 KiB NVS region at 0x9000 and PHY region at 0xf000. It uses a single factory
application, not the QMX profile's OTA metadata at 0xe000, which overlaps the
last page of that existing NVS region. Flash with the DX partition table;
do not substitute the QMX table or erase flash during this migration. An NVS
initialization error fails closed rather than automatically erasing the marker.

Original QMX build remains:

```sh
idf.py -B build build
```

Desktop compilation remains `cd desktop && ./build.sh`. Building does not open
ports. Running that original desktop application automatically probes serial
ports and must not be used as the offline regression harness.

## Radio PAL contract

The core now depends on `pal::radio`, not on `cat::*`. The backend publishes
confirmed frequency, stable band labels, capabilities, status text and an
explicit operating state:

`Offline / Starting / Receive / PreparingTx / Transmit / ReturningRx / Fault`

The QMX adapter uses `Unmanaged` while connected, because the old CAT polling
does not provide TX/RX telemetry. It does not expose the new TX diagnostic.
An accepted TX request is not confirmation of RF output. UI indication follows
the backend state. Error status is retained for the operator; no automatic TX
retry is allowed.

## RF-board interface

| Function | Tab5 GPIO |
| --- | --- |
| RF power enable, active high | G48 |
| RX switch, high for RX / low for TX isolation | G47 |
| LPF resistor ID, calibrated ADC2 channel 2 | G51 |
| Internal I2C SDA / SCL | G31 / G32 |
| PCM1808 MCLK / BCLK / LRCK / data | G16 / G45 / G3 / G4 |

PCM1808 uses I2S1, 48 kHz stereo, 24 significant bits in 32-bit slots,
12.288 MHz MCLK and 3.072 MHz BCLK. M5 speaker stays on I2S0. The synthesizer
uses the board's 26 MHz reference and the shared M5 internal I2C transport at
100 kHz; it does not allocate a competing IDF I2C controller on those pins.

The final supplied schematic connects VINR to I_OUT and VINL to Q_OUT, so PCM
right becomes I and PCM left becomes Q. The previous unsaturated +1/+2 kHz
bring-up measurements are consistent with that handedness. A clean external
receive signal is still needed to validate board-level frequency direction,
gain and I/Q balance with this new application.

## LPF-derived, read-only bands

Only a recognized, stable, calibrated installed LPF is listed. There is no
software band-selector override and no guessed default when identification
fails. Current resistor-ID windows are the established bring-up values:

| LPF | Nominal ID resistor | Accepted calibrated voltage |
| --- | --- | --- |
| 40 m | 47 kOhm | 900-1200 mV |
| 20 m | 100 kOhm | 1450-1850 mV |

Each batch checks all eight samples, rejects ADC rails and more than 80 mV
spread. Qualification requires consecutive stable batches with clocks quiet.
The initial TX diagnostic is restricted to the previously tested 40 m band;
20 m is receive-only in this first implementation. Other LPFs are unsupported
until their divider values and RF operation are added and verified.

LPFs are **not hot-swappable**. Band identification is performed during quiet
initialization and immediately before a TX job, not continuously while clocks
or RF are active. The result is latched during RX/TX. The ID resistor cannot
verify the filter components, antenna match or attached load.
Here "quiet" means the RF-board synthesizer and I2S1 receive clocks are off;
the Tab5's separate speaker subsystem may remain clocked. Board-level ADC
stability still needs to be checked with this application running.

## DR menu and TX test

The diagnostic menu exposes a deliberate tap-to-start/tap-to-stop TX test,
with the receive oscillator and PCM1808 clocks stopped during transmission.
Use a correctly assembled 40 m LPF and a suitably rated 50-ohm dummy load;
no antenna for this diagnostic. Load, PA current and temperature are not sensed.

Initial tuning is 7.074 MHz for a 40 m LPF or 14.074 MHz for a 20 m LPF.
The diagnostic carrier follows the confirmed displayed RF frequency, not the
experimental receiver LO offset. RX can be tuned within the recognized band's
implemented range. The current audio monitor retains the application's basic
real-channel audio path; the header says I/Q, not USB/LSB, because complete
sideband-selective demodulation is not implemented in this step.

The diagnostic has a maximum 10-second independent cutoff and no auto-repeat.
A normal timed stop starts at about 9.8 seconds, allowing time for clock-off
readback before the hard cutoff. A completed TX job has a 10-second cooldown.
Stopping an active carrier early also starts that cooldown. The same button
shows `Wait Ns`, then returns automatically to `TX Test 10s`; there is no need
to leave and reopen DR. Cancelling preparation before keying does not itself
start a TX cooldown.
A thin red outline marks TX and remains through the return-to-RX transition
until the backend reports a safe non-TX state. The stop control also cancels
pending TX preparation. Tuning is unavailable during TX and transitions.

The normal screen retains the QMX layout, without RF diagnostic banners.
The Band menu shows the detected band as a read-only highlighted tile. Detailed
status stays in the serial log; physical power-cycle instructions appear only
when that explicit fault-recovery confirmation is opened. Removing the banners
does not remove the TX cutoff or dummy-load requirement above.

TX entry stops receive capture, switches G47 low, verifies clocks off, and
rechecks the LPF before enabling CLK0 only. Return disables and verifies CLK0
before restoring G47/receive oscillator/capture. Initial ADC samples are
discarded after restart. No simultaneous PA/QSD leakage loopback is provided.

Power starts off and TX is never enabled at boot. An independent timer can
cut G48 without a working UI or blocked I2C transaction. If the hard cutoff
actually fires or shutdown cannot be verified, RF power stays off and a
persistent fault requires a physical RF-board power-cycle and explicit UI
acknowledgement. Acknowledgement only permits receive initialization; it
never starts a carrier. This carries the earlier `pa_bench/unverified` NVS
marker forward rather than silently clearing it during migration.

## Responsive receive tuning

Ordinary tuning within the installed band changes the receive PLL feedback
ratio only. It keeps PCM1808/I2S and the RX switch running, preserves continuous
DSP processing, and does not repeat the startup sequence, LPF detection, PLL
reset or one-second ADC discard on each movement. Changed synthesizer bytes,
lock state and the disabled TX clock are still checked. Failure cuts RF power.

Drag gestures remain active across asynchronous retunes. A latest-request-wins
mailbox coalesces movements to at most one update per 20 ms, while the displayed
frequency remains hardware-confirmed. TX cannot start during an in-flight
retune. Startup and return from TX still use the one-second capture settling
period; this tuning optimization does not relax that earlier bench finding.

Like QMX, the input DSP and speaker callback run on CPU0. The RF/ADC worker
stays on CPU1, separate from the CPU0 cutoff ISR. A four-block, nonblocking
audio queue holds at most 20 ms of waiting samples and drops the oldest block
if playback falls behind, keeping the RF control worker responsive. RX stop
invalidates queued samples. Actual touch-to-tune latency and RF settling still
require observation on the board; native tests cannot measure those timings.

## Why zero IF remains the current default

The user requested Fs/4 if the analog hardware permits it. The LM4562's speed
is not the limiting issue; the schematic's R19/R20=33 kOhm and C40/C41=1 nF
feedback networks have an impedance pole near 4.82 kHz. A simple single-pole
estimate gives -8.6 dB at 12 kHz relative to low frequency, before including
the QSD's 470 nF capacitors and the rest of the circuit. This is an estimate,
not a measured complete receiver transfer function or SNR penalty.

Zero IF therefore remains the default. For an intentional bandwidth experiment
or revised analog filter, use the dedicated DX profile's `menuconfig` and
select QRP Companion platform > RF board receive I/Q offset > Fs/4. The
backend places the LO 12 kHz below the RF tuning reference and reports the
offset to the DSP; the QMX path keeps its existing Fs/4 behavior. Do not infer
flat analog bandwidth from the digital spectrum width. Source: supplied
`TAB5 DX RF V1.11 CIRCUIT SCHEMATIC.pdf`, receiver section, and
[TI LM4562 datasheet](https://www.ti.com/lit/ds/symlink/lm4562.pdf).

## Validation boundaries

Native tests cover the QMX adapter, synthetic I/Q offsets and UI/hardware
sequencing through injected test interfaces. They cannot establish physical
RF safety, I2C coexistence timing, ADC input voltage, analog sensitivity or
PA mismatch tolerance. Prior bring-up measurements remain useful evidence,
but are not a hardware validation of this new firmware. Do not repeat the
clipped full-power receive-leakage test as a receiver quality check.
