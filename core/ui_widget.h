#pragma once
// ── UI Widget System ──
// Lightweight widgets that draw to a raw framebuffer and bind to properties.

#include "draw.h"
#include "ui_state.h"

namespace ui {

// ── Base Widget ──
enum WidgetType { W_LABEL, W_BUTTON, W_SLIDER };

struct Widget {
    WidgetType type;
    int x, y, w, h;
    bool visible = true;

    virtual void draw(const draw::Framebuf &fb) = 0;
    virtual bool onTouch(int tx, int ty, int action) { return false; }

    bool contains(int tx, int ty) const {
        return visible && tx >= x && tx < x + w && ty >= y && ty < y + h;
    }
};

// ── Label: displays formatted property value(s) ──
struct Label : Widget {
    using FormatFunc = void(*)(char *buf, int buflen);
    FormatFunc format;
    uint16_t color;
    uint16_t bg;
    int scale;
    int pad_chars;  // fixed width to avoid residuals

    void draw(const draw::Framebuf &fb) override;
    // No touch handling for labels
};

// ── Button: triggers an action on tap ──
struct Button : Widget {
    const char *label;
    uint16_t color;
    uint16_t bg;
    int scale;
    using PressFunc = void(*)(Button &self);
    PressFunc on_press;

    // Optional: bind to a bool property for toggle display
    PropId bind;           // PROP_COUNT = no binding
    bool show_as_toggle;   // if true, color changes based on bound bool

    void draw(const draw::Framebuf &fb) override;
    bool onTouch(int tx, int ty, int action) override;
};

// ── Slider: controls a float property ──
struct Slider : Widget {
    PropId bind;
    float min_val, max_val;
    bool logarithmic;
    const char *unit;       // e.g., "dB"
    bool dragging = false;

    void draw(const draw::Framebuf &fb) override;
    bool onTouch(int tx, int ty, int action) override;

private:
    float val_from_y(int ty) const;
};

// ── Panel: a named layout of widgets ──
struct Panel {
    const char *id;          // unique name for state machine transitions
    Widget *widgets[16];
    int count = 0;

    void add(Widget *w) { if (count < 16) widgets[count++] = w; }
    void draw(const draw::Framebuf &fb);
    bool onTouch(int tx, int ty, int action);
};

// ── Band: a screen region with layout state machine ──
struct Band {
    int x, y, w, h;          // screen region
    Panel *panels[8];         // all possible layouts
    int panel_count = 0;
    int active_idx = 0;       // currently shown layout
    bool dirty = true;        // clear background on next draw (set by setLayout)

    void addPanel(Panel *p) { if (panel_count < 8) panels[panel_count++] = p; }

    // State machine transition: switch to panel with given id
    void setLayout(const char *panel_id);

    Panel *active() { return (active_idx < panel_count) ? panels[active_idx] : nullptr; }

    void draw(const draw::Framebuf &fb);
    bool onTouch(int tx, int ty, int action);
};

} // namespace ui
