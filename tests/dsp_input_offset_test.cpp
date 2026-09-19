// Native regression test: no GUI, audio device, USB, or serial access.
// Run as a fresh process per input-offset setting; dsp::init owns static buffers.
// Example build/run commands live in tests/run_native.sh.
#include "dsp.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr int kRate = 48000;
constexpr int kFft = 512;
constexpr double kPi = 3.14159265358979323846;
size_t audio_frames = 0;

void discardAudio(const float *samples, int count)
{
    assert(samples && count > 0 && count <= 32);
    for (int n = 0; n < count; ++n) assert(std::isfinite(samples[n]));
    audio_frames += static_cast<size_t>(count);
}

void feedTone(int frequency_hz)
{
    // A non-power-of-two chunk checks mixer continuity across callback edges.
    constexpr int chunk_frames = 137;
    std::vector<float> iq(chunk_frames * 2);
    for (int start = 0; start < kRate / 2; start += chunk_frames) {
        const int count = std::min(chunk_frames, kRate / 2 - start);
        for (int n = 0; n < count; ++n) {
            const double phase = 2.0 * kPi * frequency_hz * (start + n) / kRate;
            iq[n * 2] = static_cast<float>(0.5 * std::cos(phase));
            iq[n * 2 + 1] = static_cast<float>(0.5 * std::sin(phase));
        }
        dsp::pushIQ(iq.data(), count);
        (void)dsp::processIfReady();
    }
}

void checkTone(int input_offset_hz, int span, int relative_hz)
{
    dsp::setSpan(span);
    const float wanted_vfo = span == 0 && input_offset_hz == kRate / 4
                                 ? 0.75f : 0.5f;
    assert(std::fabs(dsp::getVfoFraction() - wanted_vfo) < 0.00001f);
    feedTone(input_offset_hz + relative_hz);

    const float *magnitude = dsp::getMagnitudeDb();
    int peak_display = 0;
    for (int d = 1; d < kFft; ++d) {
        const int b = dsp::displayBin(d);
        assert(b >= 0 && b < kFft);
        if (magnitude[b] > magnitude[dsp::displayBin(peak_display)])
            peak_display = d;
    }
    const int expected = static_cast<int>(std::lround(
        wanted_vfo * kFft + relative_hz * static_cast<double>(kFft) /
                                  dsp::getSpanRate()));
    const float peak_db = magnitude[dsp::displayBin(peak_display)];
    std::printf("offset=%d span=%d relative=%+d expected=%d actual=%d peak=%.2f dB\n",
                input_offset_hz, span, relative_hz, expected, peak_display,
                static_cast<double>(peak_db));
    assert(std::abs(peak_display - expected) <= 1);
    // A half-scale complex sinusoid should be about -6 dB. This also catches
    // accidental down-conversion of zero-IF into the decimator stopband.
    assert(peak_db > -7.0f && peak_db < -5.0f);
    if (relative_hz > 0) assert(peak_display > wanted_vfo * kFft);
    if (relative_hz < 0) assert(peak_display < wanted_vfo * kFft);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    const int offset = std::atoi(argv[1]);
    assert(offset == 0 || offset == kRate / 4);
    dsp::init(kRate, kFft);
    assert(dsp::setInputOffsetHz(offset));
    assert(!dsp::setInputOffsetHz(-12000));
    assert(!dsp::setInputOffsetHz(1000));
    dsp::setAudioGain(1.0f);
    dsp::setAudioOutCallback(discardAudio);
    dsp::setCwOffset(0.0f);
    assert(dsp::getNumBins() == kFft);
    assert(dsp::getDecimatedRate() == 8000);

    for (int span = 0; span < dsp::NUM_SPANS; ++span) {
        checkTone(offset, span, -1500);
        checkTone(offset, span, 0);
        checkTone(offset, span, 1500);
    }
    assert(audio_frames > 0);

    // CW adds an independent pitch correction after the input-IF conversion.
    // The wanted +625 Hz CW note must land at the narrow-spectrum center.
    dsp::setSpan(2);
    dsp::setCwOffset(625.0f);
    feedTone(offset + 625);
    const float *cw_magnitude = dsp::getMagnitudeDb();
    const int cw_peak = static_cast<int>(
        std::max_element(cw_magnitude, cw_magnitude + kFft) - cw_magnitude);
    assert(cw_peak == kFft / 2);

    // Audio is optional. Removing its sink must not let the staging buffer
    // overflow while spectrum processing continues (checked by ASan/UBSan).
    dsp::setAudioOutCallback(nullptr);
    feedTone(offset + 625);
    dsp::setAudioOutCallback(discardAudio);
    const size_t before_resume = audio_frames;
    feedTone(offset + 625);
    assert(audio_frames > before_resume);
    std::printf("DSP input-offset regression PASS (%d Hz)\n", offset);
}
