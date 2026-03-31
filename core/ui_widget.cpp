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
    draw::fillRect(fb, x, y, w, h, b);
    // Center label in button
    int tw = draw::textWidth(label, scale);
    int tx = x + (w - tw) / 2;
    int ty_c = y + (h - 8 * scale) / 2;
    draw::drawText(fb, tx, ty_c, label, fg, b, scale);
}

bool Button::onTouch(int tx, int ty, int action)
{
    if (action != 2 && contains(tx, ty)) {  // action 2 = UP
        return false;  // wait for release
    }
    if (action == 2 && contains(tx, ty)) {
        if (on_press) on_press(*this);
        return true;
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
    draw::drawText(fb, x + 4, bar_y > y + 12 ? bar_y - 12 : bar_y + 10,
                   lbl, draw::rgb565(0, 255, 0), draw::rgb565(32, 32, 32));
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
    // Check sliders first (for drag continuity)
    for (int i = 0; i < count; i++) {
        if (widgets[i]->type == W_SLIDER) {
            auto *s = static_cast<Slider*>(widgets[i]);
            if (s->dragging) return s->onTouch(tx, ty, action);
        }
    }
    for (int i = 0; i < count; i++)
        if (widgets[i]->visible && widgets[i]->onTouch(tx, ty, action))
            return true;
    return false;
}

// ── Band ──
void Band::setLayout(const char *panel_id)
{
    for (int i = 0; i < panel_count; i++) {
        if (strcmp(panels[i]->id, panel_id) == 0) {
            active_idx = i;
            return;
        }
    }
}

void Band::draw(const draw::Framebuf &fb)
{
    Panel *p = active();
    if (p) p->draw(fb);
}

bool Band::onTouch(int tx, int ty, int action)
{
    if (tx < x || tx >= x + w || ty < y || ty >= y + h) return false;
    Panel *p = active();
    return p ? p->onTouch(tx, ty, action) : false;
}

} // namespace ui
