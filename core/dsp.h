#pragma once

namespace dsp {

void init(int sample_rate, int fft_size);

// Audio output callback — receives decimated mono float samples at decimated_rate
using AudioOutCallback = void(*)(const float *samples, int count);
void setAudioOutCallback(AudioOutCallback cb);

int getDecimatedRate();  // returns sample_rate / DECIM_FACTOR
void setAudioGain(float gain);  // linear gain for audio output
float getAudioGain();
void setCwOffset(float offset_hz); // set CW offset NCO frequency (0 to disable)
void setModeKnown(bool known);     // enable audio output once mode is determined
void setSoftNcoCorrection(float hz); // sub-10Hz fine correction added to CW NCO
float getSoftNcoCorrection();

// APF (Audio Peaking Filter) — very narrow CW filter, toggle with 300Hz filter
void setApfEnabled(bool enabled);
bool isApfEnabled();

// Goertzel fine-tune: start 3-second energy collection across 31 bins (10Hz apart)
// centered at DC. Call after coarse touch-to-tune. When done, returns the
// peak bin offset in Hz via getGoertzelResult(). Returns 0 while collecting.
void startGoertzel();
bool isGoertzelRunning();
// Returns peak frequency offset in Hz (valid after collection completes), or 0
float getGoertzelResult();
void clearGoertzelResult();

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
