# QRP Companion

A spectrum analyzer and waterfall application for the [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5) (ESP32-P4), with separate radio PAL backends for the [QRP-Labs QMX](https://qrp-labs.com/qmx.html) and the Tab5 DX RF daughter board.

The `tab5_dx` branch adds the RF-board backend, explicit RX/TX/transition/fault
states, an LPF-derived read-only band menu, and a bounded diagnostic TX toggle.
See [Tab5 DX build and operation](Docs/TAB5_DX.md) before connecting a populated PA.
The default build remains the original QMX companion; the DX build uses its own
configuration and output directory. No CAT is started in the DX application.

Receives 48 kHz stereo I/Q from QMX USB Audio Class or RF-board PCM1808 I2S,
and displays a real-time spectrum and waterfall. The 48 kHz digital span does
not imply a flat 48 kHz RF-board analog passband.

## Features

- **I/Q spectrum display**: 1024-point complex FFT, 48kHz bandwidth centered at LO
- **Waterfall display**: scrolling history with blue-yellow-red colormap
- **USB Audio Class host**: 48kHz / 24-bit / stereo I/Q input from QMX
- **fs/4 down-conversion**: zero-multiply mixer shifts VFO to DC for DSP processing
- **Touch screen**: GT911 capacitive touch with landscape coordinate mapping
- **Desktop emulator**: Qt5 + PortAudio backend for development on macOS/Linux

## Architecture

```
┌──────────────────────────────────────┐
│          Core Application            │
│   (DSP, UI, spectrum/waterfall)      │
│      core/app.cpp, core/dsp.cpp      │
├──────────────────────────────────────┤
│        Board PAL + Radio PAL        │
│ Display · Audio · Tuning · RX/TX    │
├────────────────┬─────────────────────┤
│  Tab5 Backend  │  Desktop Backend    │
│  M5GFX/DSI,    │  Qt5 QWidget,      │
│  GT911 touch,  │  PortAudio input,   │
│  UAC or I2S    │  mouse events       │
└────────────────┴─────────────────────┘

Radio backend: QMX CAT adapter OR direct Tab5 DX Si5351/PCM1808
```

Core code (`core/`) uses the board PAL (`pal/pal.h`) for display/touch/audio and
the radio PAL (`pal/radio.h`) for tuning, bands, operating state and capabilities.
`pal/radio_qmx.cpp` adapts the existing CAT implementation; `main/rf_board/`
implements the direct RF hardware. Both Tab5 radio platforms share display,
touch, battery and speaker code. QMX state is explicitly `Unmanaged`: the
existing companion does not know whether the external radio is transmitting.

Input offset is explicit metadata. QMX retains +Fs/4; the current DX analog
filter defaults to zero IF, with an experimental Fs/4 build option. DSP mixing,
wide-span bin mapping and the tuning marker all use that convention.

## Building

### Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/) v5.5+
- Qt5 and PortAudio (desktop only): `brew install qt@5 portaudio`

### Clone

```bash
git clone --recursive https://github.com/<your-org>/qrp_companion.git
cd qrp_companion
```

### Tab5 with QMX (ESP32-P4)

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build flash monitor
```

For the RF daughter board, use the separate [DX build profile](Docs/TAB5_DX.md)
instead of these QMX commands. The default image does not enable RF-board power.

### Desktop (macOS)

```bash
cd desktop
./build.sh
./build/qrp_companion_desktop
```

## Project Structure

```
pal/pal.h                 Board/display/audio PAL
pal/radio.h               Radio state, control and capability contract
pal/radio_qmx.cpp         QMX CAT adapter
core/
  app.cpp                 Core application logic
  dsp.cpp                 DSP pipeline (I/Q, FFT, spectrum)
  draw.h                  Embedded 8x8 font + drawing primitives
main/
  main_tab5.cpp           ESP-IDF entry point
  pal_tab5.cpp            Tab5 PAL: display, touch, UAC audio
  uac_host.c              USB Audio Class host driver
  rf_board/              Direct Si5351/PCM1808 backend and tested RX/TX engine
desktop/
  build.sh                Desktop build script
  pal_desktop.cpp         Desktop PAL: Qt5 display, PortAudio
  fft_shim/               Portable FFT (desktop uses this, Tab5 uses esp-dsp)
components/
  M5GFX/                  M5Stack graphics library (git submodule)
  M5Unified/              M5Stack hardware abstraction (git submodule)
```

## License

See [LICENSE](LICENSE) for details.

## Offline regression tests

See [tests/README.md](tests/README.md). Native tests use fake transports and
framebuffers; they do not open serial ports or transmit. Do not launch the
ordinary desktop application as a hardware-free test: its original CAT backend
automatically probes USB modem ports.
