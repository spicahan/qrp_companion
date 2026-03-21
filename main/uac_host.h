#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start USB host and UAC driver tasks. Non-blocking — spawns FreeRTOS tasks.
void uac_host_start(void);

// Called from uac_host.c to push converted audio samples to the DSP pipeline.
void uac_push_audio_samples(const float *samples, int count);

// Debug info from UAC, displayed on screen
typedef struct {
    char line1[128];
    char line2[128];
} uac_debug_info_t;
const uac_debug_info_t* uac_get_debug_info(void);

#ifdef __cplusplus
}
#endif
