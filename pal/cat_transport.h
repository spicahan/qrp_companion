#pragma once

// Private transport contract for the QMX radio adapter, not a requirement of
// every platform. Direct RF-board backends do not implement or initialize CAT.
namespace pal {
bool catIsConnected();
int catSend(const char* data, int len); // bytes written, or -1 on error
int catRecv(char* buf, int max_len);   // nonblocking, 0 when empty
}
