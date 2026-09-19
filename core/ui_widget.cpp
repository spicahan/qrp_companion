#include "ui_widget.h"
#include <cstring>
#include <cstdio>
#include <cmath>

namespace ui {

// ── Label ──
void Label::draw(const draw::Framebuf &fb)
{
    if (!visible || !format) return;
    char buf[64];
    format(buf, sizeof(buf));
    int len = strlen(buf);
    while (len < pad_chars) buf[len++] = ' ';
    buf[len] = '\0';
    draw::drawText(fb, x, y, buf, color, bg, scale);
}

// ── Button ──
void Button::draw(const draw::Framebuf &fb)
{
    if (!visible) return;
    uint16_t fg = color;
    uint16_t b = bg;
    // Toggle display: highlight when bound bool is true
    if (show_as_toggle && bind < PROP_COUNT) {
        if (get_bool(bind)) {
            fg = draw::rgb565(0, 0, 0);
            b = color;  // inverted
        }
    }
    // Explicit highlight (e.g. the band the radio is currently on)
    if (highlight) {
        fg = draw::rgb565(0, 0, 0);
        b = color;  // inverted
    }
    draw::fillRect(fb, x, y, w, h, b);
    draw::drawRect(fb, x, y, w, h, draw::rgb565(80, 80, 80));
    // Center label in button
    int tw = draw::textWidth(label, scale);
    int tx = x + (w - tw) / 2;
    int ty_c = y + (h - 8 * scale) / 2;
    draw::drawText(fb, tx, ty_c, label, fg, b, scale);
}

bool Button::onTouch(int tx, int ty, int action)
{
    const bool inside = contains(tx, ty);
    if (action == 0) {
        pressed = inside && enabled;
        return inside;
    }
    if (action == 1) {
        if (!inside) pressed = false;
        return inside;
    }
    if (action == 2) {
        const bool activate = pressed && inside && enabled;
        pressed = false;
        if (activate && on_press) on_press(*this);
        return inside;
    }
    return false;
}

// ── Slider ──
float Slider::val_from_y(int ty) const
{
    float norm = 1.0f - (float)(ty - y) / h;
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;
    if (logarithmic) {
        float log_min = log10f(min_val);
        float log_max = log10f(max_val);
        return powf(10.0f, log_min + norm * (log_max - log_min));
    }
    return min_val + norm * (max_val - min_val);
}

void Slider::draw(const draw::Framebuf &fb)
{
    if (!visible || bind >= PROP_COUNT) return;

    // Background
    draw::fillRect(fb, x, y, w, h, draw::rgb565(32, 32, 32));
    // Track
    draw::drawVLine(fb, x + w / 2, y + 4, h - 8, draw::rgb565(0, 0, 0));

    // Current value → position
    float val = get_f32(bind);
    float norm;
    if (logarithmic) {
        float log_min = log10f(min_val > 0 ? min_val : 1.0f);
        float log_max = log10f(max_val);
        float log_val = log10f(val > min_val ? val : min_val);
        norm = (log_val - log_min) / (log_max - log_min);
    } else {
        norm = (val - min_val) / (max_val - min_val);
    }
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;

    int bar_y = y + (int)((1.0f - norm) * (h - 10));
    draw::fillRect(fb, x + 8, bar_y, w - 16, 6, draw::rgb565(255, 255, 255));

    // Label
    char lbl[16];
    if (unit && strcmp(unit, "dB") == 0)
        snprintf(lbl, sizeof(lbl), "%ddB", (int)(20.0f * log10f(val) + 0.5f));
    else
        snprintf(lbl, sizeof(lbl), "%.0f", val);
    draw::drawText(fb, x + 4, bar_y > y + 20 ? bar_y - 20 : bar_y + 10,
                   lbl, draw::rgb565(0, 255, 0), draw::rgb565(32, 32, 32), 2);
}

bool Slider::onTouch(int tx, int ty, int action)
{
    if (action == 0 && contains(tx, ty)) {  // DOWN
        dragging = true;
        set_f32(bind, val_from_y(ty));
        return true;
    }
    if (action == 1 && dragging) {  // MOVE
        set_f32(bind, val_from_y(ty));
        return true;
    }
    if (action == 2) {  // UP
        dragging = false;
        return true;
    }
    return false;
}

// ── Panel ──
void Panel::draw(const draw::Framebuf &fb)
{
    for (int i = 0; i < count; i++)
        if (widgets[i]->visible) widgets[i]->draw(fb);
}

bool Panel::onTouch(int tx, int ty, int action)
{
    auto clear_button_presses = [&]() {
        for (int i = 0; i < count; ++i)
            if (widgets[i]->type == W_BUTTON)
                static_cast<Button*>(widgets[i])->pressed = false;
    };
    if (action == 0) clear_button_presses();
    if (action == 1) {
        // Do this before dispatch: another button earlier in the panel may
        // consume the move before the originally pressed button sees it.
        for (int i = 0; i < count; ++i)
            if (widgets[i]->type == W_BUTTON && !widgets[i]->contains(tx, ty))
                static_cast<Button*>(widgets[i])->pressed = false;
    }

    bool consumed = false;
    // Check sliders first (for drag continuity)
    for (int i = 0; i < count; i++) {
        if (widgets[i]->type == W_SLIDER) {
            auto *s = static_cast<Slider*>(widgets[i]);
            if (s->dragging) {
                consumed = s->onTouch(tx, ty, action);
                break;
            }
        }
    }
    if (!consumed) {
        for (int i = 0; i < count; i++) {
            if (widgets[i]->visible && widgets[i]->onTouch(tx, ty, action)) {
                consumed = true;
                break;
            }
        }
    }
    if (action == 2) clear_button_presses();
    return consumed;
}

// ── Band ──
void Band::setLayout(const char *panel_id)
{
    for (int i = 0; i < panel_count; i++) {
        if (strcmp(panels[i]->id, panel_id) == 0) {
            if (Panel *old = active()) {
                for (int j = 0; j < old->count; ++j)
                    if (old->widgets[j]->type == W_BUTTON)
                        static_cast<Button*>(old->widgets[j])->pressed = false;
            }
            active_idx = i;
            dirty = true;
            return;
        }
    }
}

void Band::draw(const draw::Framebuf &fb)
{
    // Only clear background on layout switch (dirty flag), not every frame
    if (dirty) {
        draw::fillRect(fb, x, y, w, h, draw::rgb565(32, 32, 32));
        dirty = false;
    }
    Panel *p = active();
    if (p) p->draw(fb);
}

bool Band::onTouch(int tx, int ty, int action)
{
    Panel *p = active();
    if (tx < x || tx >= x + w || ty < y || ty >= y + h) {
        if (p) {
            for (int i = 0; i < p->count; ++i)
                if (p->widgets[i]->type == W_BUTTON)
                    static_cast<Button*>(p->widgets[i])->pressed = false;
        }
        return false;
    }
    return p ? p->onTouch(tx, ty, action) : false;
}

} // namespace ui
