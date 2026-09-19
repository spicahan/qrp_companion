#pragma once

#include "pal.h"
#include <cstddef>

// Keep the RF/ADC control worker independent of DSP and speaker backpressure.
// As on QMX, input DSP runs on CPU0; hardware sequencing/ADC stays on CPU1.
namespace rf_audio {
inline constexpr size_t BLOCK_FRAMES = 240;
inline constexpr size_t QUEUE_BLOCKS = 4;

bool initialize(); // Called once by the UI thread, before starting RF hardware.
bool open(pal::AudioInputCallback callback); // UI thread only.
void close(); // UI thread; waits for any already-running callback to finish.
void invalidate(); // Drop queued samples on RX stop; does not wait for DSP.
void submit(const float *iq, size_t frames); // Single RF producer; never waits.
} // namespace rf_audio
