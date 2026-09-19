#pragma once
#include "rf_engine.h"

namespace rf_board {
// Owned by the ESP adapter's mailbox lock. Latest wins, including while a
// previous tune is in flight. The public radio stays Receive throughout;
// this private busy flag gates TX without cancelling an active touch gesture.
struct TuneMailbox {
    static constexpr uint64_t MIN_INTERVAL_MS = 20;
    uint64_t requested = 0;
    uint64_t next_allowed_ms = 0;
    bool in_flight = false;

    void request(uint64_t frequency) { requested = frequency; }
    // A fault, shutdown, or recovery must not replay a previous gesture. Do
    // not clear in_flight here: its worker still owns completion/cleanup.
    void cancelPending() { requested = 0; }
    bool busyOrPending() const { return in_flight || requested != 0; }
    uint64_t claim(const Snapshot &published, bool tx_pending, uint64_t now_ms)
    {
        if (in_flight || tx_pending || published.state != pal::radio::State::Receive ||
            requested == 0) return 0;
        if (requested == published.frequency) { requested = 0; return 0; }
        if (now_ms < next_allowed_ms) return 0;
        const uint64_t result = requested;
        requested = 0;
        in_flight = true;
        next_allowed_ms = now_ms + MIN_INTERVAL_MS;
        return result;
    }
    void complete() { in_flight = false; }
};
} // namespace rf_board
