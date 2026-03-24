#pragma once

namespace dsp {

void init(int sample_rate, int fft_size);

// Push I/Q sample pairs. iq is interleaved [I0,Q0,I1,Q1,...], num_frames pairs.
// Thread-safe: can be called from a different task/thread than processIfReady().
void pushIQ(const float *iq, int num_frames);

// Process FFT from buffered I/Q data. Returns true if new spectrum available.
// Applies fs/4 complex down-conversion then complex FFT.
bool processIfReady();

int          getNumBins();           // fft_size (full complex spectrum)
const float* getMagnitudeDb();       // dB values after fftshift, getNumBins() elements
                                     // index 0 = -fs/2, index N/2 = DC, index N-1 = +fs/2
float        getBinFrequency(int bin); // frequency relative to LO (after shift)

} // namespace dsp
