#pragma once

namespace dsp {

void init(int sample_rate, int fft_size);
void process_frame();               // compute one FFT frame from test tone

int          getNumBins();           // fft_size / 2 + 1
const float* getMagnitudeDb();       // dB values, getNumBins() elements
float        getBinFrequency(int bin);

} // namespace dsp
