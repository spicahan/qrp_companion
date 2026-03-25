# QRP Companion

A spectrum analyzer and waterfall display companion device for the [QRP-Labs QMX](https://qrp-labs.com/qmx.html) transceiver, built on the [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5) (ESP32-P4).

Receives I/Q audio from QMX via USB Audio Class (UAC), displays a real-time spectrum and waterfall with 48kHz bandwidth. Designed for CW and digital modes.

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
│            PAL API (pal/pal.h)       │
│  Display · Touch · Audio · Timing   │
├────────────────┬─────────────────────┤
│  Tab5 Backend  │  Desktop Backend    │
│  M5GFX/DSI,    │  Qt5 QWidget,      │
│  GT911 touch,  │  PortAudio input,   │
│  UAC host      │  mouse events       │
└────────────────┴─────────────────────┘
```

Core code (`core/`) is platform-independent. Each PAL backend implements the same API with platform-specific drivers.

## Building

### Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/) v5.5+
- Qt5 and PortAudio (desktop only): `brew install qt@5 portaudio`

### Clone

```bash
git clone --recursive https://github.com/<your-org>/qrp_companion.git
cd qrp_companion
```

### Tab5 (ESP32-P4)

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build flash monitor
```

### Desktop (macOS)

```bash
cd desktop
./build.sh
./build/qrp_companion_desktop
```

## Project Structure

```
pal/pal.h                 PAL API (shared contract)
core/
  app.cpp                 Core application logic
  dsp.cpp                 DSP pipeline (I/Q, FFT, spectrum)
  draw.h                  Embedded 8x8 font + drawing primitives
main/
  main_tab5.cpp           ESP-IDF entry point
  pal_tab5.cpp            Tab5 PAL: display, touch, UAC audio
  uac_host.c              USB Audio Class host driver
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
