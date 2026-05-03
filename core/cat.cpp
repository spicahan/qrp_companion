#include "cat.h"
#include "pal.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

static uint64_t s_vfo_freq = 0;
static int      s_mode = 0;
static int      s_cw_offset = 0;       // most recent MMCW|CW OFFSET response

// CW offset cache per filter — QMX changes the offset when the filter
// changes if the previous center frequency isn't available in the new filter
// (see QMX operation manual, "CW Filters" section, pages 30–33).
// Probe both at startup and apply the matching value on APF toggle.
static int      s_cw_offset_50 = 0;    // offset under 50Hz APF filter
static int      s_cw_offset_250 = 0;   // offset under 250Hz CW filter
static bool     s_apf_filter_active = false;  // true = 50Hz, false = 250Hz

// RX accumulator
static char s_rx_buf[128];
static int  s_rx_pos = 0;

// Polling timer
static int64_t s_last_poll = 0;
static constexpr int64_t POLL_INTERVAL_US = 1000000; // 1 second
static bool s_polling_suppressed = false;
static bool s_iq_mode_sent = false;  // send Q91 once on first connected poll

// Probe state machine — runs once when CW mode is first detected.
// Phase progression (1 second per phase):
//   0: send query for current (250Hz) offset
//   1: capture 250Hz offset response, then idle
//   2: switch to 50Hz filter
//   3: query offset (will be 50Hz value)
//   4: capture 50Hz offset response, then restore 250Hz filter
//   5: probe complete — normal polling resumes
static int s_probe_phase = 0;

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
    s_cw_offset_50 = 0;
    s_cw_offset_250 = 0;
    s_apf_filter_active = false;
    s_rx_pos = 0;
    s_last_poll = 0;
    s_iq_mode_sent = false;
    s_probe_phase = 0;
}

// Run one step of the CW offset probe (called from poll() at 1Hz).
// Returns true if a command was sent.
static bool run_probe_step()
{
    switch (s_probe_phase) {
        case 0:
            // Initial state: query current offset (passband=250 was set at init)
            s_cw_offset = 0;
            send_cmd("FA;MD;MMCW|CW OFFSET;");
            s_probe_phase = 1;
            return true;
        case 1:
            // Wait one cycle for response, then capture as 250Hz offset
            if (s_cw_offset > 0) {
                s_cw_offset_250 = s_cw_offset;
                s_probe_phase = 2;
            }
            send_cmd("FA;MD;");
            return true;
        case 2:
            // Switch to 50Hz filter
            send_cmd("FA;MD;MMCW|CW passband=50;");
            s_probe_phase = 3;
            return true;
        case 3:
            // Query offset (now under 50Hz filter)
            s_cw_offset = 0;
            send_cmd("FA;MD;MMCW|CW OFFSET;");
            s_probe_phase = 4;
            return true;
        case 4:
            // Capture 50Hz offset, then restore default 250Hz filter
            if (s_cw_offset > 0) {
                s_cw_offset_50 = s_cw_offset;
            }
            send_cmd("FA;MD;MMCW|CW passband=250;");
            // After restoring filter, snap s_cw_offset back to 250Hz value
            // so any code reading s_cw_offset directly stays consistent.
            s_cw_offset = s_cw_offset_250;
            s_probe_phase = 5;
            return true;
        default:
            return false;  // probe done
    }
}

void cat::poll()
{
    int64_t now = pal::micros();

    // Periodic polling (suppressed during drag-to-tune)
    if (!s_polling_suppressed && now - s_last_poll >= POLL_INTERVAL_US) {
        s_last_poll = now;
        if (pal::catIsConnected()) {
            // Enable I/Q mode + set CW filter once on first connected poll
            if (!s_iq_mode_sent) {
                send_cmd("Q91;MMCW|CW passband=250;");
                s_iq_mode_sent = true;
            }
            // Run probe state machine when CW mode detected
            else if (s_mode == 3 && s_probe_phase < 5) {
                run_probe_step();
            }
            // Normal polling
            else {
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

// Returns the CW offset matching the currently active filter.
// Falls back to the last MMCW|CW OFFSET response if probe hasn't completed.
int cat::getCwOffset()
{
    if (s_apf_filter_active && s_cw_offset_50 > 0) return s_cw_offset_50;
    if (!s_apf_filter_active && s_cw_offset_250 > 0) return s_cw_offset_250;
    return s_cw_offset;
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
    // Track which filter is now active so getCwOffset() returns the right cached value
    s_apf_filter_active = (strcmp(bandwidth, "50") == 0);
}

void cat::setVfoFreq(uint64_t freq_hz)
{
    if (freq_hz == 0) return;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "FA%011llu;", (unsigned long long)freq_hz);
    send_cmd(cmd);
    s_vfo_freq = freq_hz;  // optimistic update
    // Reset poll timer to give QMX time to process before next poll
    s_last_poll = pal::micros();
}
