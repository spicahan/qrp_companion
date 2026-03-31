#include "ui_state.h"
#include <cstring>

// Storage: union for all property types
union PropValue {
    uint64_t u64;
    int      i32;
    float    f32;
    bool     b;
};

static PropValue  g_props[ui::PROP_COUNT];
static bool       g_dirty[ui::PROP_COUNT];
static ui::ChangeCallback g_callbacks[ui::PROP_COUNT];

void ui::init()
{
    memset(g_dirty, 0, sizeof(g_dirty));
    memset(g_callbacks, 0, sizeof(g_callbacks));

    // Set defaults from X-macro
    #define X(type, name, def) g_props[PROP_##name] = {}; \
        { type tmp = def; memcpy(&g_props[PROP_##name], &tmp, sizeof(tmp)); }
    UI_PROPERTIES(X)
    #undef X
}

// ── Setters ──
static void notify(ui::PropId id, ui::Source src)
{
    g_dirty[id] = true;
    if (g_callbacks[id]) g_callbacks[id](id, src);
}

void ui::set_u64(PropId id, uint64_t val, Source src)
{
    if (g_props[id].u64 == val) return;
    g_props[id].u64 = val;
    notify(id, src);
}

void ui::set_i32(PropId id, int val, Source src)
{
    if (g_props[id].i32 == val) return;
    g_props[id].i32 = val;
    notify(id, src);
}

void ui::set_f32(PropId id, float val, Source src)
{
    if (g_props[id].f32 == val) return;
    g_props[id].f32 = val;
    notify(id, src);
}

void ui::set_bool(PropId id, bool val, Source src)
{
    if (g_props[id].b == val) return;
    g_props[id].b = val;
    notify(id, src);
}

// ── Getters ──
uint64_t ui::get_u64(PropId id)  { return g_props[id].u64; }
int      ui::get_i32(PropId id)  { return g_props[id].i32; }
float    ui::get_f32(PropId id)  { return g_props[id].f32; }
bool     ui::get_bool(PropId id) { return g_props[id].b; }

// ── Dirty flags ──
bool ui::is_dirty(PropId id)     { return g_dirty[id]; }
void ui::clear_dirty(PropId id)  { g_dirty[id] = false; }
void ui::clear_all_dirty()       { memset(g_dirty, 0, sizeof(g_dirty)); }

// ── Callbacks ──
void ui::on_change(PropId id, ChangeCallback cb) { g_callbacks[id] = cb; }
