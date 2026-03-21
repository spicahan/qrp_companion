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

// Optional platform debug info (2 lines). Returns empty strings if none.
const char* debugLine1();
const char* debugLine2();

} // namespace pal
