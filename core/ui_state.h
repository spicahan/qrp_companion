#pragma once
// ── UI State Store ──
// Source-aware property storage with dirty flags and change callbacks.
// Generated from UI_PROPERTIES X-macro.

#include "ui_props.h"
#include <cstdint>

namespace ui {

// Who changed the property — callbacks can filter on this
enum Source { FROM_RADIO, FROM_UI, FROM_DSP, FROM_INIT };

// Property IDs (auto-generated from X-macro)
enum PropId {
    #define X(type, name, def) PROP_##name,
    UI_PROPERTIES(X)
    #undef X
    PROP_COUNT
};

// Change callback: called after value is stored
using ChangeCallback = void(*)(PropId id, Source src);

void init();

// ── Typed setters (source-tagged) ──
void set_u64(PropId id, uint64_t val, Source src = FROM_UI);
void set_i32(PropId id, int val, Source src = FROM_UI);
void set_f32(PropId id, float val, Source src = FROM_UI);
void set_bool(PropId id, bool val, Source src = FROM_UI);

// ── Typed getters ──
uint64_t get_u64(PropId id);
int      get_i32(PropId id);
float    get_f32(PropId id);
bool     get_bool(PropId id);

// ── Dirty flags (per-frame redraw tracking) ──
bool is_dirty(PropId id);
void clear_dirty(PropId id);
void clear_all_dirty();

// ── Register change callback per property ──
void on_change(PropId id, ChangeCallback cb);

} // namespace ui
