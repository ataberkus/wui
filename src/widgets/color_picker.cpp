#include "painter.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>

namespace wui {
namespace {
struct Hsv { float h = 0, s = 0, v = 0; };

Color from_hsv(Hsv hsv, float alpha) {
    const float h = (hsv.h >= 1.0f ? 0.0f : hsv.h) * 6.0f;
    const float c = hsv.v * hsv.s;
    const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    const float m = hsv.v - c;
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    return {std::clamp(r + m, 0.0f, 1.0f), std::clamp(g + m, 0.0f, 1.0f),
            std::clamp(b + m, 0.0f, 1.0f), alpha};
}

// Achromatic colors keep the previous hue (and black keeps saturation), so
// dragging through white or black does not reset the other handles.
Hsv to_hsv(Color color, Hsv previous) {
    const float high = std::max({color.r, color.g, color.b});
    const float low = std::min({color.r, color.g, color.b});
    const float delta = high - low;
    Hsv hsv{previous.h, previous.s, high};
    if (high <= 0.0f) return hsv;
    hsv.s = delta / high;
    if (delta <= 0.0f) return hsv;
    float h = 0;
    if (high == color.r) h = (color.g - color.b) / delta;
    else if (high == color.g) h = 2.0f + (color.b - color.r) / delta;
    else h = 4.0f + (color.r - color.g) / delta;
    h /= 6.0f;
    if (h < 0) h += 1.0f;
    hsv.h = h;
    return hsv;
}

bool near_color(Color a, Color b) {
    constexpr float tolerance = 1.0f / 512.0f;
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance &&
           std::abs(a.b - b.b) <= tolerance;
}

bool differs(Color a, Color b) {
    return a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
}

float unit(float value, float origin, float extent) {
    return extent > 0 ? std::clamp((value - origin) / extent, 0.0f, 1.0f) : 0.0f;
}

// Applies one drag's ordered pointer positions and keyboard steps.
template<class Position>
bool apply(const DragResult& drag, float& target, Position position) {
    const float before = target;
    for (const auto& action : drag.actions) {
        switch (action.key) {
        case Key::none: target = position(action); break;
        case Key::left: target = std::max(0.0f, target - 0.01f); break;
        case Key::right: target = std::min(1.0f, target + 0.01f); break;
        case Key::home: target = 0.0f; break;
        case Key::end: target = 1.0f; break;
        default: break;
        }
    }
    return target != before;
}

void marker(detail::FrameState& frame, Rect bar, float y) {
    const Rect line{bar.x - 1, y - 2, bar.w + 2, 4};
    frame.canvas.rounded_rect(line, 1, Color{0, 0, 0, 1});
    frame.canvas.rounded_rect(detail::inset(line, 1), 0, Color{1, 1, 1, 1});
}
} // namespace

bool Frame::color_picker(std::string_view key, std::string_view label, Color& value,
                         ColorPickerOptions options) {
    if (!state_) {
#ifndef NDEBUG
        std::fputs("wui contract: Frame handle has been moved\n", stderr);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
#endif
        return false;
    }
    auto& frame = *state_;
    const Color previous = value;
    try {
        if (!frame.usable(generation_)) return false;
        for (float channel : {value.r, value.g, value.b, value.a}) {
            if (!std::isfinite(channel) || channel < 0 || channel > 1) {
                frame.report(ErrorCode::invalid_argument, "Color picker value must be normalized", true);
                return false;
            }
        }
        if (!std::isfinite(options.size) || options.size < 0) {
            frame.report(ErrorCode::invalid_argument, "Color picker size must be finite and nonnegative", true);
            return false;
        }
        if (label.empty()) {
            frame.report(ErrorCode::invalid_argument, "Color picker needs an accessible label", true);
            return false;
        }
        const WidgetOptions widget_options = frame.effective({});
        const bool enabled = widget_options.enabled;
        const Theme theme = detail::widget_theme(frame, widget_options);
        if (frame.error) return false;
        const Id id = frame.keyed_id(key);
        auto* state = frame.register_widget(id);
        if (!state || !frame.copy_label(label)) return false;
        const Id sv_id = child_id(id, "saturation-value");
        const Id hue_id = child_id(id, "hue");
        const Id alpha_id = child_id(id, "alpha");
        if (!frame.register_widget(sv_id) || !frame.register_widget(hue_id) ||
            (options.alpha && !frame.register_widget(alpha_id)))
            return false;

        const float bar = std::max(12.0f, theme.control_height * 0.5f);
        const float bars = (options.alpha ? 2.0f : 1.0f) * (bar + theme.gap);
        const Rect probe = next_rect(0);
        if (frame.error) return false;
        const float side = options.size > 0 ? options.size
            : std::clamp(probe.w - bars, 64.0f, 240.0f);
        const Rect row = frame.take(side);
        if (frame.error || !frame.check_canvas()) return false;
        const Rect sv{row.x, row.y, side, side};
        const Rect hue{sv.x + side + theme.gap, row.y, bar, side};
        const Rect alpha{hue.x + bar + theme.gap, row.y, bar, side};
        const Rect clip = frame.clip();

        Hsv hsv{state->hsv[0], state->hsv[1], state->hsv[2]};
        if (!state->hsv_valid || !near_color(from_hsv(hsv, value.a), value)) hsv = to_hsv(value, hsv);
        float a = value.a;

        const auto hit = [&](Rect area) { return intersect(area, clip); };
        const auto sv_drag = frame.interaction.drag(sv_id, hit(sv), enabled && detail::visible(hit(sv)));
        bool changed = false;
        {
            // Pointer positions set saturation (x) and value (y) together;
            // keyboard steps adjust saturation.
            const Hsv before = hsv;
            for (const auto& action : sv_drag.actions) {
                switch (action.key) {
                case Key::none:
                    hsv.s = unit(action.position_x, sv.x, sv.w);
                    hsv.v = 1.0f - unit(action.position_y, sv.y, sv.h);
                    break;
                case Key::left: hsv.s = std::max(0.0f, hsv.s - 0.01f); break;
                case Key::right: hsv.s = std::min(1.0f, hsv.s + 0.01f); break;
                case Key::home: hsv.s = 0.0f; break;
                case Key::end: hsv.s = 1.0f; break;
                default: break;
                }
            }
            changed = hsv.s != before.s || hsv.v != before.v;
        }
        const auto hue_drag = frame.interaction.drag(hue_id, hit(hue), enabled && detail::visible(hit(hue)));
        changed = apply(hue_drag, hsv.h, [&](const DragAction& action) {
            return unit(action.position_y, hue.y, hue.h);
        }) || changed;
        DragResult alpha_drag;
        if (options.alpha) {
            alpha_drag = frame.interaction.drag(alpha_id, hit(alpha), enabled && detail::visible(hit(alpha)));
            changed = apply(alpha_drag, a, [&](const DragAction& action) {
                return 1.0f - unit(action.position_y, alpha.y, alpha.h);
            }) || changed;
        }
        if (changed && enabled) value = from_hsv(hsv, options.alpha ? a : value.a);
        state->hsv[0] = hsv.h;
        state->hsv[1] = hsv.s;
        state->hsv[2] = hsv.v;
        state->hsv_valid = true;
        const bool focused = sv_drag.behavior.focused || hue_drag.behavior.focused ||
                             alpha_drag.behavior.focused;
        detail::track_feedback(*state, hit(row), {sv_drag.behavior.hovered || hue_drag.behavior.hovered ||
                                                  alpha_drag.behavior.hovered,
                                                  false, focused, false}, enabled);

        {
            detail::WidgetClip scope(frame, hit(row));
            if (!frame.check_canvas()) return differs(value, previous);
            const Color pure = from_hsv({hsv.h, 1, 1}, 1);
            frame.canvas.gradient(sv, Color{1, 1, 1, 1}, pure, pure, Color{1, 1, 1, 1});
            frame.canvas.gradient(sv, Color{0, 0, 0, 0}, Color{0, 0, 0, 1});
            const Vec2 point{sv.x + hsv.s * sv.w, sv.y + (1.0f - hsv.v) * sv.h};
            frame.canvas.border({point.x - 6, point.y - 6, 12, 12}, 6, 2, Color{0, 0, 0, 1});
            frame.canvas.border({point.x - 5, point.y - 5, 10, 10}, 5, 1.5f, Color{1, 1, 1, 1});
            for (int i = 0; i < 6; ++i) {
                const float top = hue.y + hue.h * static_cast<float>(i) / 6.0f;
                const float bottom = i == 5 ? hue.y + hue.h : hue.y + hue.h * static_cast<float>(i + 1) / 6.0f;
                frame.canvas.gradient({hue.x, top, hue.w, bottom - top},
                                      from_hsv({static_cast<float>(i) / 6.0f, 1, 1}, 1),
                                      from_hsv({static_cast<float>(i + 1) / 6.0f, 1, 1}, 1));
            }
            marker(frame, hue, hue.y + hsv.h * hue.h);
            if (options.alpha) {
                const float cell = alpha.w * 0.5f;
                int index = 0;
                for (float y = alpha.y; y < alpha.y + alpha.h; y += cell, ++index) {
                    const float h = std::min(cell, alpha.y + alpha.h - y);
                    const Color light{0.8f, 0.8f, 0.8f, 1}, dark{0.5f, 0.5f, 0.5f, 1};
                    frame.canvas.rounded_rect({alpha.x, y, cell, h}, 0, index % 2 ? dark : light);
                    frame.canvas.rounded_rect({alpha.x + cell, y, alpha.w - cell, h}, 0,
                                              index % 2 ? light : dark);
                }
                const Color shown = from_hsv(hsv, 1);
                frame.canvas.gradient(alpha, shown, Color{shown.r, shown.g, shown.b, 0});
                marker(frame, alpha, alpha.y + (1.0f - a) * alpha.h);
            }
            const auto ring = [&](Rect area, const DragResult& drag) {
                if (drag.behavior.focused) frame.canvas.border(area, 2, 2, theme.focus);
            };
            ring(sv, sv_drag);
            ring(hue, hue_drag);
            if (options.alpha) ring(alpha, alpha_drag);
            if (!enabled) {
                frame.canvas.rounded_rect({sv.x, sv.y, (options.alpha ? alpha.x + alpha.w : hue.x + hue.w) - sv.x,
                                           side}, 0, Color{theme.panel.r, theme.panel.g, theme.panel.b, 0.55f});
            }
        }
        if (!frame.check_canvas()) return differs(value, previous);
        std::array<char, 10> text{};
        const auto byte = [](float channel) {
            return static_cast<unsigned>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
        };
        std::snprintf(text.data(), text.size(), "#%02X%02X%02X%02X",
                      byte(value.r), byte(value.g), byte(value.b), byte(value.a));
        const std::string_view shown{text.data(), 9};
        if (frame.copy_label(shown))
            frame.canvas.semantic(id, Role::slider, label, row, enabled, focused, false, shown);
        frame.check_canvas();
        return differs(value, previous);
    } catch (const std::bad_alloc&) {
        frame.report(ErrorCode::capacity, "Color picker allocation failed");
    } catch (const std::length_error&) {
        frame.report(ErrorCode::capacity, "Color picker exceeds container limits");
    }
    return differs(value, previous);
}

} // namespace wui
