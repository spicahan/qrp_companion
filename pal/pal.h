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
uint16_t*    getFramebuffer();   // raw pixel buffer (physical or logical depending on platform)
int          getFramebufferStride(); // physical width of buffer
bool         isRotated();        // true if display uses 90° CW rotation
void         commitFrame();      // push framebuffer to screen

// Input
bool pollEvent(TouchEvent &evt); // returns false if no pending event

// Timing
int64_t micros();
void    delayMs(int ms);

// System info
int freeHeapKb();
int freePsramKb();  // 0 on desktop

// 2D block copy (hardware-accelerated on Tab5 via PPA, CPU fallback on desktop)
// Copies a rectangular block from src (with src_stride) to dst (with dst_stride).
// mirror_y: if true, source rows are read in reverse order.
// All sizes in pixels (uint16_t). Blocking call.
void blitBlock(const uint16_t *src, int src_stride, int src_x, int src_y,
               uint16_t *dst, int dst_stride, int dst_x, int dst_y,
               int width, int height, bool mirror_y = false);

// Audio input (stereo 24-bit 48kHz, delivered as I/Q float pairs)
// Callback is called from the audio thread.
// iq_samples: interleaved [I0, Q0, I1, Q1, ...], num_frames pairs.
using AudioInputCallback = void(*)(const float *iq_samples, int num_frames);
bool audioInputOpen(AudioInputCallback cb);
void audioInputClose();

} // namespace pal
