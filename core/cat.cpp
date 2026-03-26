#include "cat.h"
#include "pal.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

static uint64_t s_vfo_freq = 0;
static int      s_mode = 0;
static int      s_cw_offset = 0;       // CW sidetone offset in Hz
static bool     s_cw_offset_queried = false;

// RX accumulator
static char s_rx_buf[128];
static int  s_rx_pos = 0;

// Polling timer
static int64_t s_last_poll = 0;
static constexpr int64_t POLL_INTERVAL_US = 1000000; // 1 second
static bool s_polling_suppressed = false;

static void process_response(const char *resp, int len)
{
    // FA response: "FA00014070000"
    if (len >= 13 && resp[0] == 'F' && resp[1] == 'A') {
        uint64_t freq = 0;
        for (int i = 2; i < len; i++) {
            if (resp[i] >= '0' && resp[i] <= '9')
                freq = freq * 10 + (resp[i] - '0');
        }
        if (freq > 0) s_vfo_freq = freq;
    }
    // MD response: "MD6"
    else if (len >= 3 && resp[0] == 'M' && resp[1] == 'D') {
        int mode = resp[2] - '0';
        if (mode >= 1 && mode <= 9) s_mode = mode;
    }
    // MM response: "MM650" (CW offset, always 3 digits)
    else if (len >= 5 && resp[0] == 'M' && resp[1] == 'M') {
        int offset = 0;
        for (int i = 2; i < len; i++) {
            if (resp[i] >= '0' && resp[i] <= '9')
                offset = offset * 10 + (resp[i] - '0');
        }
        if (offset > 0) s_cw_offset = offset;
    }
}

static void send_cmd(const char *cmd)
{
    if (pal::catIsConnected())
        pal::catSend(cmd, strlen(cmd));
}

void cat::init()
{
    s_vfo_freq = 0;
    s_mode = 0;
    s_cw_offset = 0;
    s_cw_offset_queried = false;
    s_rx_pos = 0;
    s_last_poll = 0;
}

void cat::poll()
{
    int64_t now = pal::micros();

    // Periodic polling (suppressed during drag-to-tune)
    if (!s_polling_suppressed && now - s_last_poll >= POLL_INTERVAL_US) {
        s_last_poll = now;
        if (pal::catIsConnected()) {
            // Query CW offset once when mode is CW and offset is unknown
            if (s_mode == 3 && s_cw_offset == 0 && !s_cw_offset_queried) {
                send_cmd("FA;MD;MMCW|CW OFFSET;");
                s_cw_offset_queried = true;
            } else {
                send_cmd("FA;MD;");
            }
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
int      cat::getCwOffset(){ return s_cw_offset; }

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

void cat::suppressPolling(bool suppress) { s_polling_suppressed = suppress; }

void cat::setVfoFreq(uint64_t freq_hz)
{
    if (freq_hz == 0) return;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "FA%011llu;", (unsigned long long)freq_hz);
    send_cmd(cmd);
    s_vfo_freq = freq_hz;  // optimistic update for immediate display
}
