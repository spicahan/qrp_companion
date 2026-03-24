#pragma once

namespace dsp {

void init(int sample_rate, int fft_size);

// Push I/Q sample pairs. iq is interleaved [I0,Q0,I1,Q1,...], num_frames pairs.
// Thread-safe: can be called from a different task/thread than processIfReady().
void pushIQ(const float *iq, int num_frames);

// Process complex FFT from buffered I/Q data. Returns true if new spectrum available.
// Data is fs/4 down-converted (VFO at DC) before FFT.
bool processIfReady();

int          getNumBins();           // fft_size (full complex spectrum)
const float* getMagnitudeDb();       // dB values after fftshift, centered at VFO (DC)

// For display: virtual rotation to show LO-centered spectrum.
// Use displayBin(i) to map display position i to magnitude_db index.
// Display: 0 = LO-24k, N/4 = LO-12k, N/2 = LO, 3N/4 = VFO, N-1 = LO+24k
int  displayBin(int display_idx);    // returns (display_idx + 3*N/4) % N

} // namespace dsp
