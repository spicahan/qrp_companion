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
uint16_t*    getFramebuffer();
int          getFramebufferStride();
bool         isRotated();
void         commitFrame();

// Extended framebuffer for ring buffer waterfall (portrait mode)
int  getExtendedHeight();
void setVisibleOffset(int row);

// 2D block copy (hardware-accelerated on Tab5 via PPA, CPU fallback on desktop)
void blitBlock(const uint16_t *src, int src_stride, int src_x, int src_y,
               uint16_t *dst, int dst_stride, int dst_x, int dst_y,
               int width, int height, bool mirror_y = false);

// Input
bool pollEvent(TouchEvent &evt);
void injectEvent(const TouchEvent &evt);  // push event from web UI

// Battery / power. pollBattery() refreshes cached values from hardware
// (call once per second from app::tick — I2C reads aren't free).
// Getters return cached values cheaply; safe to call every frame.
void pollBattery();
int  getBatteryLevel();    // 0-100, or -1 if unknown / unsupported
bool isCharging();

// Timing
int64_t micros();
void    delayMs(int ms);

// System info
int freeHeapKb();
int freePsramKb();

// CAT serial transport (raw byte send/receive)
// Returns true if a CAT-capable serial port is connected.
bool catIsConnected();
// Send raw bytes. Returns number of bytes sent, or -1 on error.
int  catSend(const char *data, int len);
// Receive up to max_len bytes. Non-blocking, returns bytes read (0 if none).
int  catRecv(char *buf, int max_len);

// Audio input (stereo 24-bit 48kHz, delivered as I/Q float pairs)
using AudioInputCallback = void(*)(const float *iq_samples, int num_frames);
bool audioInputOpen(AudioInputCallback cb);
void audioInputClose();

// Audio output (mono, 24-bit quality, configurable sample rate)
bool audioOutputOpen(int sample_rate);
void audioOutputWrite(const float *samples, int num_frames); // mono float [-1,1]
void audioOutputClose();

} // namespace pal
