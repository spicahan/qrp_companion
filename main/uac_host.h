#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start USB host and UAC driver tasks. Non-blocking — spawns FreeRTOS tasks.
void uac_host_start(void);

// Called from uac_host.c to push converted audio samples to the DSP pipeline.
// Implemented in C++ (pal_tab5.cpp) as a bridge to dsp::pushSamples().
void uac_push_audio_samples(const float *samples, int count);

#ifdef __cplusplus
}
#endif
