#pragma once
#include <cstdint>

namespace web {

void init(int port);     // Start HTTP + WebSocket server
void poll();             // Call from main loop (non-blocking)

// Send spectrum frame to all connected WebSocket clients.
// bins: uint8 magnitude array [0-255], count: number of bins.
void sendSpectrum(const uint8_t *bins, int count,
                  int span_rate, uint64_t vfo_freq, int mode, bool apf);

// Touch event callback (fired when phone sends touch)
using TouchCallback = void(*)(int action, int x, int y, int display_w, int display_h);
void setTouchCallback(TouchCallback cb);

// Button callback (fired when phone sends a named button press)
using ButtonCallback = void(*)(const char *id);
void setButtonCallback(ButtonCallback cb);

bool hasClients();       // true if at least one WebSocket client is connected

} // namespace web
