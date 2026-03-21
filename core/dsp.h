#pragma once

namespace dsp {

void init(int sample_rate, int fft_size);

// Push mono float samples from an external source (e.g. UAC audio)
// Thread-safe: can be called from a different task/thread than processIfReady()
void pushSamples(const float *samples, int count);

// Process FFT from externally pushed samples. Returns true if new spectrum available.
bool processIfReady();

// Fallback: generate test tone internally and process FFT.
void processTestTone();

int          getNumBins();           // fft_size / 2 + 1
const float* getMagnitudeDb();       // dB values, getNumBins() elements
float        getBinFrequency(int bin);

} // namespace dsp
