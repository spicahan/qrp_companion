# WOLA Polyphase Filter Bank — Design Document

## Problem

The current spectrum/waterfall uses a plain 1024-point FFT with rectangular window.
This gives -13 dB sidelobe rejection — strong signals leak across the entire display,
masking weak signals nearby. Additionally, each span (48k, ±12k, ±6k, ±3k) runs at
a different frame rate because FPS = rate/N, giving 46.9, 23.4, 11.7, and 5.9 FPS
respectively.

## Goal

Replace the plain FFT with a Weighted Overlap-Add (WOLA) polyphase filter bank:
- Sidelobe rejection ≈ -92 dB (Blackman-Harris)
- Uniform FPS = 24000/1024 ≈ 23.44 Hz across all spans
- Preserve per-span frequency resolution (46.9, 23.4, 11.7, 5.9 Hz/bin)
- More efficient cascaded decimation

## Architecture Overview

```
QMX I/Q (48 kHz)
      │
  fs/4 mixer (VFO → DC)
      │
  CW NCO (CW offset → DC, when active)
      │
      ├──────────────────── WOLA 48k span (P=4, hop=2048)
      │
  half-band ↓2 ──────────── WOLA ±12k span (P=4, hop=1024)
      │
  half-band ↓2 ──────────── WOLA ±6k span (P=4, hop=512)
      │
  half-band ↓2 ──────────── WOLA ±3k span (P=4, hop=512)
      │                           │
      └─── audio pipeline ◄──────┘  (CW filter, APF, sidetone, SSB)
```

The fs/4 mixer brings the QMX VFO (at +fs/4 in digital I/Q) to DC.
All subsequent anti-alias filters and WOLA prototypes are centered at DC.

## WOLA Parameters

Fixed across all spans:
- **N** = 1024 (FFT size, number of output bins)
- **P** = 4 (polyphase segments)
- **M** = P × N = 4096 (prototype filter / window length)
- **FPS** = 23.4375 Hz (±3k: 11.72 Hz to reduce temporal smoothing)

Per-span:

| Span  | Rate   | Hop  | Hz/bin | Overlap | FPS   |
|-------|--------|------|--------|---------|-------|
| 48k   | 48000  | 2048 |  46.9  |  50.0%  | 23.44 |
| ±12k  | 24000  | 1024 |  23.4  |  75.0%  | 23.44 |
| ±6k   | 12000  |  512 |  11.7  |  87.5%  | 23.44 |
| ±3k   |  6000  |  512 |   5.9  |  87.5%  | 11.72 |

FPS derivation: `FPS = rate / hop`.
- 48000/2048 = 23.4375
- 24000/1024 = 23.4375
- 12000/512  = 23.4375
- 6000/512   = 11.71875

## Prototype Filter (Window)

The prototype filter is a **Blackman-Harris** window of length M+1 = 4097
(symmetric, Type I), periodic-truncated to M = 4096 samples:

```
w_sym[n] = a0 - a1·cos(2πn/M) + a2·cos(4πn/M) - a3·cos(6πn/M)
           for n = 0, 1, ..., M     (4097 samples, symmetric)

w[n] = w_sym[n]  for n = 0, 1, ..., M-1   (4096 samples, periodic)
```

Coefficients (4-term Blackman-Harris):
- a0 = 0.35875
- a1 = 0.48829
- a2 = 0.14128
- a3 = 0.01168

The window is then scaled by N:
```
w[n] *= N    (compensates for the P-fold summation in the fold step)
```

Properties:
- Sidelobe level: -92 dB
- Mainlobe width: ~4 bins of the M-point DFT = 1 bin of the N-point output
- Scalloping loss: ~0.83 dB
- Power-complementary flatness: < 0.1 dB ripple

The periodic (non-symmetric) form ensures correct DFT-domain behavior:
the fold-and-FFT treats data as periodic, so the window must match.

## WOLA Processing (per frame)

Each span maintains a circular buffer of M = 4096 complex (I/Q) samples.

When `hop_counter` reaches `hop_size`:

### Step 1: Window + Fold (combined)

```
folded[n] = 0   for n = 0..N-1  (complex)

for p = 0..P-1:
    for n = 0..N-1:
        idx = (write_pos - M + p*N + n) mod M     // oldest → newest
        folded[n] += circ_buf[idx] * w[p*N + n]
```

Cost: P×N = 4096 complex multiply-adds per frame.

### Step 2: N-point complex FFT

```
FFT(folded) → spectrum[0..N-1]
```

Cost: N·log₂(N)/2 = 5120 complex multiplies.

### Step 3: Magnitude (with fftshift)

```
for i = 0..N-1:
    bin = (i + N/2) mod N               // fftshift: DC at center
    mag_db[i] = 20·log10(|spectrum[bin]| / N)
```

Total cost per frame: ~9200 complex multiply-adds. At 23.44 FPS: ~215K ops/sec.
Negligible for ESP32-P4.

## Cascaded Half-Band Decimation

Replace 3 independent parallel decimators (each at 48 kHz) with a cascade:

```
Current (3N mults/sample):
  48k ──FIR→ ↓2   (independently)
  48k ──FIR→ ↓4   (independently)
  48k ──FIR→ ↓8   (independently)

New (≈0.875N mults/sample with half-band):
  48k ──HB→ ↓2 ──HB→ ↓2 ──HB→ ↓2
             24k       12k       6k
```

Half-band FIR properties:
- Cutoff at fs/4 (transition band symmetric about fs/4)
- Every other coefficient is zero (except center tap)
- For L-tap half-band: only ~L/4 non-trivial multiplies per output sample
- Each stage runs at its input rate (not the full 48 kHz)

Cost: L/4 per output sample at each rate.
- Stage 1 (48k→24k): runs at 48 kHz input, produces at 24 kHz
- Stage 2 (24k→12k): runs at 24 kHz input
- Stage 3 (12k→6k): runs at 12 kHz input
- Total per 48 kHz input sample: L/4 + L/8 + L/16 ≈ 0.44L

With L=63: ~28 mults per input sample (vs 3×65=195 currently).

The 6 kHz output feeds the audio pipeline (CW filter, APF, sidetone, SSB)
exactly as before.

## Circular Buffer Design

Each of the 4 spans has a WOLA state:

```c
struct WolaState {
    float buf_i[4096];    // circular I buffer
    float buf_q[4096];    // circular Q buffer
    int   write_pos;      // next write position (0..4095)
    int   hop_counter;    // samples since last frame
    int   hop_size;       // 2048, 1024, 512, or 256
    bool  ready;          // true when hop_counter >= hop_size
};
```

All 4 circular buffers are filled continuously (one write per sample at
each rate). Only the selected span triggers FFT processing when ready.

Memory: 4 × (4096 × 2 × 4 bytes) = 128 KB for circular buffers.
Plus 4096 × 4 = 16 KB for the shared prototype window.
Total: ~144 KB (fits in ESP32-P4 PSRAM).

## Display Mapping

The 48k span retains the `displayBin(idx) = (idx + 3N/4) % N` rotation
to center the VFO (which the fs/4 mixer placed at DC) in the display.

All narrow spans: `displayBin(idx) = idx` (DC = VFO already at center
after fftshift).

The VFO marker:
- 48k span: at 3/4 of display width
- Narrow spans: at center (1/2)

## What Changes

### dsp.cpp
- Replace `g_input_buf` double-buffer with 4 `WolaState` circular buffers
- Replace `compute_spectrum()` with `wola_process()` (window+fold+FFT)
- Replace 3 independent `FirDecimState` with cascaded half-band stages
- Add prototype window generation in `dsp::init()`
- `pushIQ()`: write to 48k circular buffer, cascade through half-bands,
  write decimated output to each span's circular buffer
- `processIfReady()`: check selected span's hop counter, run WOLA if ready

### dsp.h
- Remove `setSpan()` / `getSpan()` return values that assumed rate-dependent FFT fill
- Add span hop sizes as compile-time constants

### app.cpp
- No changes needed (span switching, display, touch-to-tune all unchanged)
- FPS counter will naturally show ~23.4 for all spans

## What Does NOT Change

- fs/4 mixer (stays, brings QMX VFO from +fs/4 to DC)
- CW NCO (stays, shifts CW offset to DC)
- Audio pipeline: CW filter, APF, sidetone NCO, SSB filter (all at 6 kHz)
- Goertzel detector (at 6 kHz, fed from final ↓2 stage)
- Waterfall rendering, spectrum drawing, touch-to-tune
- Widget system, property system, CAT protocol
