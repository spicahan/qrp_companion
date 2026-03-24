#pragma once
#include <cstdint>
#include <cstddef>

namespace pal {

struct TouchEvent {
    int x, y;
    enum Action { DOWN, MOVE, UP } action;
};

struct DisplayInfo {
    int width, height;
};

// Lifecycle
bool init(int width, int height);
void shutdown();

// Display
DisplayInfo  getDisplayInfo();
uint16_t*    getFramebuffer();   // logical landscape pixel buffer (RGB565)
void         commitFrame();      // push framebuffer to screen

// Input
bool pollEvent(TouchEvent &evt); // returns false if no pending event

// Timing
int64_t micros();
void    delayMs(int ms);

// System info
int freeHeapKb();
int freePsramKb();  // 0 on desktop

// Audio input (stereo 24-bit 48kHz, delivered as I/Q float pairs)
// Callback is called from the audio thread.
// iq_samples: interleaved [I0, Q0, I1, Q1, ...], num_frames pairs.
using AudioInputCallback = void(*)(const float *iq_samples, int num_frames);
bool audioInputOpen(AudioInputCallback cb);
void audioInputClose();

} // namespace pal
