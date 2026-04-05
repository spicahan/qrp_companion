#pragma once
// ── UI Property Definitions (X-macro DSL) ──
// This file is the single source of truth for all UI-visible state.
// Format: X(type, name, default_value)
// Change handlers are registered separately in app.cpp.

#define UI_PROPERTIES(X) \
    X(uint64_t, vfo_freq,     0)          \
    X(int,      mode,         0)          \
    X(bool,     apf_enabled,  false)      \
    X(float,    audio_gain,   1000.0f)    \
    X(int,      span_idx,     1)          \
    X(int,      cw_offset,    0)          \
    X(float,    db_floor,     -110.0f)    \
    X(float,    db_ceil,      -30.0f)     \
    X(float,    fps,          0.0f)       \
    X(bool,     mode_known,   false)      \
    X(bool,     goertzel_running, false)  \

