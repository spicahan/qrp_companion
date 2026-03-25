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
uint16_t*    getFramebuffer();       // pixel buffer (may be within an extended buffer)
int          getFramebufferStride(); // physical row width in pixels
bool         isRotated();            // true if 90° CW rotation active
void         commitFrame();          // push framebuffer to screen

// Extended framebuffer for ring buffer waterfall (portrait mode)
int  getExtendedHeight();            // total buffer height (>= display height if ring buffer)
void setVisibleOffset(int row);      // set DMA visible window start row

// Input
bool pollEvent(TouchEvent &evt);

// Timing
int64_t micros();
void    delayMs(int ms);

// System info
int freeHeapKb();
int freePsramKb();

// Audio input (stereo 24-bit 48kHz, delivered as I/Q float pairs)
using AudioInputCallback = void(*)(const float *iq_samples, int num_frames);
bool audioInputOpen(AudioInputCallback cb);
void audioInputClose();

} // namespace pal
