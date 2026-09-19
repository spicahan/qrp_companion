#pragma once

#include "draw.h"
#include <algorithm>
#include <vector>

// An overlay must never become part of the waterfall's retained image. Restore
// the previous physical pixels before drawing or moving any framebuffer rows,
// then apply the next overlay after all widgets and waterfall copies finish.
// Retaining the old mapping also handles rotated framebuffers and a changed
// visible window when the caller restores before changing that window.
class FrameBorder {
public:
    void restore()
    {
        if (!active_) return;
        size_t i = 0;
        visitEdges(previous_, thickness_, [&](int x, int y) {
            previous_.setPixel(x, y, saved_[i++]);
        });
        active_ = false;
    }

    void apply(const draw::Framebuf &fb, uint16_t color, int thickness = 2)
    {
        // The owner must restore before beginning the next frame. Do not
        // silently overwrite a still-active backup with already-red pixels.
        if (active_ || !fb.buf || fb.log_w <= 0 || fb.log_h <= 0) return;
        thickness_ = std::max(1, std::min(thickness, std::min(fb.log_w, fb.log_h)));
        previous_ = fb;
        saved_.clear();
        saved_.reserve(static_cast<size_t>(2 * thickness_ * (fb.log_w + fb.log_h)));
        visitEdges(fb, thickness_, [&](int x, int y) {
            const int index = fb.rotated ? (fb.log_w - 1 - x) * fb.phys_w + y
                                         : y * fb.phys_w + x;
            saved_.push_back(fb.buf[index]);
            fb.setPixel(x, y, color);
        });
        active_ = true;
    }

private:
    template <typename Fn>
    static void visitEdges(const draw::Framebuf &fb, int t, Fn fn)
    {
        for (int y = 0; y < fb.log_h; ++y) {
            if (y < t || y >= fb.log_h - t) {
                for (int x = 0; x < fb.log_w; ++x) fn(x, y);
            } else {
                const int left_end = std::min(t, fb.log_w);
                for (int x = 0; x < left_end; ++x) fn(x, y);
                for (int x = std::max(left_end, fb.log_w - t); x < fb.log_w; ++x) fn(x, y);
            }
        }
    }

    draw::Framebuf previous_{};
    std::vector<uint16_t> saved_;
    int thickness_ = 0;
    bool active_ = false;
};
