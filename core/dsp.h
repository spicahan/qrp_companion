#pragma once

namespace dsp {

void init(int sample_rate, int fft_size);

// Audio output callback — receives decimated mono float samples at decimated_rate
using AudioOutCallback = void(*)(const float *samples, int count);
void setAudioOutCallback(AudioOutCallback cb);

int getDecimatedRate();  // returns sample_rate / DECIM_FACTOR
void setAudioGain(float gain);  // linear gain for audio output (default 1.0)
void setCwOffset(float offset_hz); // set CW offset NCO frequency (0 to disable)

// Push I/Q sample pairs. iq is interleaved [I0,Q0,I1,Q1,...], num_frames pairs.
// Thread-safe: can be called from a different task/thread than processIfReady().
void pushIQ(const float *iq, int num_frames);

// Process complex FFT from buffered I/Q data. Returns true if new spectrum available.
// Data is fs/4 down-converted (VFO at DC) before FFT.
bool processIfReady();

int          getNumBins();           // fft_size (full complex spectrum)
const float* getMagnitudeDb();       // dB values after fftshift

// Display bin mapping (depends on span — includes virtual rotation for 48k span)
int  displayBin(int display_idx);

// Span control
static constexpr int NUM_SPANS = 4;
void setSpan(int span_idx);          // 0=48k, 1=±12k, 2=±6k, 3=±3k
int  getSpan();
int  getSpanRate();                  // effective sample rate of current span
const char* getSpanLabel();

} // namespace dsp
