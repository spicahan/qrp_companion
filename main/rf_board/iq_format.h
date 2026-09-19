#pragma once
#include <cstddef>
#include <cstdint>

namespace rf_board {
inline float pcm1808SignedSample(uint32_t word)
{
    const int32_t magnitude = static_cast<int32_t>(word >> 8);
    const int32_t signed_value = (magnitude & 0x800000) ? magnitude - 0x1000000 : magnitude;
    return signed_value / 8388608.0f;
}

// V1.11 VINR=I_OUT, VINL=Q_OUT: convert L/R words to I/Q float pairs.
// Reject nonzero padding before accepting the block into the DSP.
inline bool convertPcm1808IQ(const uint32_t *words, float *iq, size_t frames)
{
    if (!words || !iq) return false;
    for (size_t i = 0; i < frames * 2; ++i) if (words[i] & 0xFFU) return false;
    for (size_t frame = 0; frame < frames; ++frame) {
        iq[frame * 2] = pcm1808SignedSample(words[frame * 2 + 1]);
        iq[frame * 2 + 1] = pcm1808SignedSample(words[frame * 2]);
    }
    return true;
}
} // namespace rf_board
