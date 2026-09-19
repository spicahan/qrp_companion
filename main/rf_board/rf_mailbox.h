#pragma once
#include "rf_engine.h"
#include <cstdio>

namespace rf_board {
// Called while the ESP adapter holds its mailbox lock, before the worker
// performs tune(). A no-op tune MUST leave Receive published because the
// engine correctly returns without emitting any state transition for it.
inline bool claimTune(Snapshot &published, uint64_t frequency, bool tx_pending)
{
    if (!frequency || tx_pending || published.state != pal::radio::State::Receive ||
        frequency == published.frequency) return false;
    published.state = pal::radio::State::Starting;
    std::snprintf(published.status, sizeof(published.status), "Retuning receiver; TX OFF");
    return true;
}
} // namespace rf_board
