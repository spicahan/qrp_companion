#include "cat.h"
#include "pal.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

// Force a deterministic QMX CW filter configuration so APF toggling
// (50Hz↔250Hz) never shifts the CW offset:
//   - Choose filters: enable only 50Hz and 250Hz (disable the rest)
//   - CW center: 625 Hz (the lowest center common to both 50Hz and 250Hz
//     filter lists — switching passband preserves the center)
//   - CW offset: 625 Hz (matches center; explicit in case Auto-offset/tone
//     is disabled on the user's QMX)
//   - CW passband: 250 Hz (default, APF off)
// 50Hz centers : {625, 675, 725, 775, 825}
// 250Hz centers: {525, 575, 625, 675, 725, 775, 825, 875, 925}
// Note: MM set commands persist in EEPROM; this overwrites the user's
// CW filter configuration. Q* commands are volatile (Q91 for I/Q mode).
static constexpr int CW_OFFSET_HZ = 625;

static uint64_t s_vfo_freq = 0;
static int      s_mode = 0;

// RX accumulator
static char s_rx_buf[128];
static int  s_rx_pos = 0;

// Polling timer
static int64_t s_last_poll = 0;
static constexpr int64_t POLL_INTERVAL_US = 1000000; // 1 second
static bool s_polling_suppressed = false;

// Setup state machine. The setup is split across multiple poll cycles
// to avoid overflowing the QMX serial input buffer with one huge chain.
//   0: send Q91 + ensure 50/250 enabled (so subsequent passband=250 is valid)
//   1: send passband=250 + center=625 + offset=625
//   2: disable the other filters
//   3: done — normal polling
static int s_setup_phase = 0;

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
    // MM responses are not parsed — CW offset is forced to a known value
    // by our setup commands, so we don't need to query it back.
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
    s_rx_pos = 0;
    s_last_poll = 0;
    s_setup_phase = 0;
}

void cat::poll()
{
    int64_t now = pal::micros();

    // Periodic polling (suppressed during drag-to-tune)
    if (!s_polling_suppressed && now - s_last_poll >= POLL_INTERVAL_US) {
        s_last_poll = now;
        if (pal::catIsConnected()) {
            switch (s_setup_phase) {
                case 0:
                    // Enable I/Q + ensure both 50Hz and 250Hz filters are
                    // enabled BEFORE selecting passband=250 (so the selection
                    // is guaranteed to land on an enabled filter).
                    send_cmd(
                        "Q91;"
                        "MMCW|Choose filters|0=ENABLED;"   // 50 Hz
                        "MMCW|Choose filters|4=ENABLED;"   // 250 Hz
                    );
                    s_setup_phase = 1;
                    break;
                case 1:
                    // Now set passband, center, and offset to a deterministic
                    // configuration. Order matters: passband first because
                    // changing passband may auto-snap the center.
                    send_cmd(
                        "MMCW|CW passband=250;"
                        "MMCW|CW center=625;"
                        "MMCW|CW offset=625;"
                    );
                    s_setup_phase = 2;
                    break;
                case 2:
                    // Now safe to disable other filters (we're already on 250).
                    send_cmd(
                        "MMCW|Choose filters|1=DISABLED;"  // 100
                        "MMCW|Choose filters|2=DISABLED;"  // 150
                        "MMCW|Choose filters|3=DISABLED;"  // 200
                        "MMCW|Choose filters|5=DISABLED;"  // 300
                        "MMCW|Choose filters|6=DISABLED;"  // 400
                        "MMCW|Choose filters|7=DISABLED;"  // 500
                    );
                    s_setup_phase = 3;
                    printf("[cat] QMX configured: 50/250 only, center=625, offset=625\n");
                    break;
                default:
                    send_cmd("FA;MD;");
                    break;
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

// CW offset is forced by setup; always return 625 Hz in CW mode.
int cat::getCwOffset()
{
    return (s_mode == 3) ? CW_OFFSET_HZ : 0;
}

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

void cat::setCwFilter(const char *bandwidth)
{
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "MMCW|CW passband=%s;", bandwidth);
    send_cmd(cmd);
    // No need to update an offset cache — the QMX is configured so that
    // the center stays 625 Hz across both 50/250 filter selections.
}

void cat::setVfoFreq(uint64_t freq_hz)
{
    if (freq_hz == 0) return;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "FA%011llu;", (unsigned long long)freq_hz);
    send_cmd(cmd);
    s_vfo_freq = freq_hz;  // optimistic update
    s_last_poll = pal::micros();
}
