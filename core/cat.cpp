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
static int      s_band_index = -1;   // current BN index, -1 = unknown

// ── Band enumeration ────────────────────────────────────────────────
// The QMX "Band config." screen is a 16-column grid; column N is the band
// index used by the BN command. Row "Band name (m)" holds the band name in
// metres, or "0" for an unconfigured slot. Hans Summers confirmed BN indexes
// into this table, so enumerating it is the only way to learn which bands a
// particular QMX/QMX+ (or custom build) actually has.
// Queries are chained a few per 1 Hz tick so the UI never blocks.
// Names are stored BY COLUMN, not appended, so a retried or duplicated reply
// overwrites the same slot instead of shifting every later band. Getting this
// wrong would be silent and dangerous: a shifted table means the "20" button
// switches the radio to some other band.
// One persistent record per column; name[0]=='\0' means unconfigured/not read.
// Persistent (not a shared temporary) because the UI keeps `name` as a label
// pointer for the lifetime of the button.
static cat::BandInfo s_band_slots[cat::MAX_BANDS];
static int  s_enum_col   = 0;      // first column of the batch in flight
static int  s_enum_pending = 0;    // responses still outstanding for it
static int  s_enum_next_slot = 0;  // column the next reply maps to
static int  s_enum_retries = 0;
static bool s_enum_inflight = false;
static bool s_enum_done  = false;
static constexpr int ENUM_BATCH = 4;   // columns queried per tick
static constexpr int ENUM_MAX_RETRIES = 2;

// RX accumulator
static char s_rx_buf[128];
static int  s_rx_pos = 0;
static int  s_esc_state = 0;   // ANSI CSI filter: 0=text, 1=saw ESC, 2=in CSI

static bool s_polling_suppressed = false;
static int  s_skip_ticks = 0;  // delay next tick1Hz (set by setVfoFreq)

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
    // BN response: "BN05" (zero-padded current band index)
    else if (len >= 3 && resp[0] == 'B' && resp[1] == 'N') {
        int idx = 0; bool any = false;
        for (int i = 2; i < len; i++) {
            if (resp[i] >= '0' && resp[i] <= '9') { idx = idx * 10 + (resp[i] - '0'); any = true; }
        }
        if (any) s_band_index = idx;
    }
    // MM response during band enumeration: "MM160" / "MM0" (unconfigured).
    // MM replies carry no column info, so we rely on the QMX answering a
    // chained request in order and consume them against the pending slots.
    else if (len >= 3 && resp[0] == 'M' && resp[1] == 'M' && s_enum_pending > 0) {
        int col = s_enum_next_slot++;
        s_enum_pending--;
        int name = 0;
        for (int i = 2; i < len; i++) {
            if (resp[i] >= '0' && resp[i] <= '9') name = name * 10 + (resp[i] - '0');
        }
        // Band names are metres: 6..2200. Clamp so the formatted name always
        // fits the buffer (lets the compiler prove no truncation).
        if (col >= 0 && col < cat::MAX_BANDS) {
            if (name > 9999) name = 9999;
            s_band_slots[col].index = col;
            if (name > 0)
                snprintf(s_band_slots[col].name, sizeof(s_band_slots[col].name),
                         "%u", (unsigned)name);
            else
                s_band_slots[col].name[0] = '\0';   // unconfigured slot
        }
    }
    // Other MM responses are ignored — CW offset is forced to a known value
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
    s_setup_phase = 0;
    s_skip_ticks = 0;
    s_band_index = -1;
    for (int c = 0; c < MAX_BANDS; c++) { s_band_slots[c].index = c; s_band_slots[c].name[0] = '\0'; }
    s_enum_col = 0;
    s_enum_pending = 0;
    s_enum_next_slot = 0;
    s_enum_retries = 0;
    s_enum_inflight = false;
    s_enum_done = false;
    s_esc_state = 0;
}

// Drain any pending RX bytes — called every frame from app::tick.
void cat::poll()
{
    char tmp[64];
    int n = pal::catRecv(tmp, sizeof(tmp));
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        char c = tmp[i];

        // MM *set* commands make the QMX repaint its terminal screen, emitting
        // ANSI escape sequences such as "\e[1;254H\e[2;254H\e[0;0H". Those
        // contain semicolons, so feeding them to the ';' framing below splits
        // them into junk AND leaves a trailing fragment ("0H") in the buffer
        // that then prefixes the next real response ("0HMM160"), silently
        // eating it. That is why 160m (the first band queried) went missing.
        // Strip CSI sequences here so every parser downstream sees clean text.
        if (s_esc_state == 1) {                     // saw ESC
            s_esc_state = (c == '[') ? 2 : 0;       // CSI intro, else 2-char esc
            continue;
        }
        if (s_esc_state == 2) {                     // inside CSI parameters
            if (c >= '@' && c <= '~') s_esc_state = 0;   // final byte ends it
            continue;
        }
        if (c == 0x1b) { s_esc_state = 1; continue; }

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

// Periodic CAT command sends — called once per second from app::tick.
// Honors suppressPolling() (set during drag-to-tune) and a short
// post-setVfoFreq cooldown to avoid racing FA-set with FA-query.
void cat::tick1Hz()
{
    if (s_polling_suppressed || !pal::catIsConnected()) return;
    if (s_skip_ticks > 0) { s_skip_ticks--; return; }

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
            // Enumerate the band table once, a few columns per tick, before
            // settling into normal polling. Chained MM queries are answered in
            // order, so a whole batch costs one round trip.
            if (!s_enum_done) {
                // Only advance once the batch in flight was fully answered.
                // A short reply would otherwise mis-align the remaining
                // columns; retry the same batch instead (replies are written
                // by column, so a retry is idempotent).
                if (s_enum_inflight) {
                    if (s_enum_pending > 0 && s_enum_retries < ENUM_MAX_RETRIES) {
                        s_enum_retries++;           // short reply — re-ask
                    } else {
                        if (s_enum_pending > 0)
                            printf("[cat] band enum: giving up on cols %d..%d\n",
                                   s_enum_col, s_enum_col + ENUM_BATCH - 1);
                        s_enum_col += ENUM_BATCH;   // batch settled, move on
                        s_enum_retries = 0;
                    }
                }

                if (s_enum_col >= cat::MAX_BANDS) {
                    s_enum_done = true;
                    printf("[cat] band enumeration done: %d bands\n", getBandCount());
                    break;
                }

                char cmd[256];
                int n = 0;
                s_enum_next_slot = s_enum_col;
                s_enum_pending = 0;
                for (int i = 0; i < ENUM_BATCH && (s_enum_col + i) < cat::MAX_BANDS; i++) {
                    n += snprintf(cmd + n, sizeof(cmd) - n,
                                  "MMBand config.|Band name (m)[%d];", s_enum_col + i);
                    s_enum_pending++;
                }
                if (n > 0) { send_cmd(cmd); s_enum_inflight = true; }
                break;
            }
            send_cmd("FA;MD;BN;");
            break;
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

// ── Band table / selection ──────────────────────────────────────────
bool cat::isBandEnumDone() { return s_enum_done; }
int  cat::getBandIndex() { return s_band_index; }

int cat::getBandCount()
{
    int n = 0;
    for (int c = 0; c < MAX_BANDS; c++)
        if (s_band_slots[c].name[0]) n++;
    return n;
}

// Returns the i-th *configured* band, carrying its real column (= BN index),
// so gaps in the table can never shift what a button switches to.
const cat::BandInfo* cat::getBand(int i)
{
    if (i < 0) return nullptr;
    int n = 0;
    for (int c = 0; c < MAX_BANDS; c++) {
        if (!s_band_slots[c].name[0]) continue;
        if (n == i) return &s_band_slots[c];
        n++;
    }
    return nullptr;
}

void cat::setBandIndex(int index)
{
    if (index < 0 || index >= MAX_BANDS) return;
    // Chain FA;MD; so the header shows the new band's frequency immediately
    // rather than up to a second later. The QMX processes the chain in order,
    // so FA already reports the band-stack frequency BN just restored
    // (verified: "BN3;FA;MD;" -> "FA00007074000;MD6;").
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "BN%d;FA;MD;", index);
    send_cmd(cmd);
    s_band_index = index;   // optimistic; corrected by the next BN; poll
}

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
    s_skip_ticks = 1;      // give QMX time to apply before next FA query
}
