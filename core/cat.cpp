#include "cat.h"
#include "pal.h"
#include <cstring>
#include <cstdlib>

static uint64_t s_vfo_freq = 0;
static int      s_mode = 0;

// RX accumulator
static char s_rx_buf[128];
static int  s_rx_pos = 0;

// Polling timer
static int64_t s_last_poll = 0;
static constexpr int64_t POLL_INTERVAL_US = 1000000; // 1 second

static void process_response(const char *resp, int len)
{
    // FA response: "FA00014070000"  (len >= 13, without trailing ;)
    if (len >= 13 && resp[0] == 'F' && resp[1] == 'A') {
        uint64_t freq = 0;
        for (int i = 2; i < len; i++) {
            if (resp[i] >= '0' && resp[i] <= '9')
                freq = freq * 10 + (resp[i] - '0');
        }
        if (freq > 0) s_vfo_freq = freq;
    }
    // MD response: "MD6"  (len >= 3)
    else if (len >= 3 && resp[0] == 'M' && resp[1] == 'D') {
        int mode = resp[2] - '0';
        if (mode >= 1 && mode <= 9) s_mode = mode;
    }
}

void cat::init()
{
    s_vfo_freq = 0;
    s_mode = 0;
    s_rx_pos = 0;
    s_last_poll = 0;
}

void cat::poll()
{
    int64_t now = pal::micros();

    // Send poll commands at interval
    if (now - s_last_poll >= POLL_INTERVAL_US) {
        s_last_poll = now;
        if (pal::catIsConnected()) {
            pal::catSend("FA;MD;", 6);
        }
    }

    // Read and parse any available responses
    char tmp[64];
    int n = pal::catRecv(tmp, sizeof(tmp));
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == ';') {
                s_rx_buf[s_rx_pos] = '\0';
                if (s_rx_pos > 0)
                    process_response(s_rx_buf, s_rx_pos);
                s_rx_pos = 0;
            } else if (s_rx_pos < (int)sizeof(s_rx_buf) - 2) {
                s_rx_buf[s_rx_pos++] = c;
            }
        }
    }
}

uint64_t cat::getVfoFreq() { return s_vfo_freq; }
int      cat::getMode()    { return s_mode; }

const char* cat::getModeStr()
{
    switch (s_mode) {
        case 1: return "LSB";
        case 2: return "USB";
        case 3: return "CW";
        case 6: return "DIGI";
        default: return "---";
    }
}
