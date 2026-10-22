#pragma once
#include "../style/theme_detail.hpp"

#include "../core/context_detail.hpp"

#include <algorithm>
#include <cmath>

namespace wui::detail {

class WidgetClip {
public:
    WidgetClip(FrameState& frame, Rect bounds) : canvas_(frame.canvas) {
        canvas_.push_clip(bounds);
    }
    ~WidgetClip() { canvas_.pop_clip(); }
    WidgetClip(const WidgetClip&) = delete;
    WidgetClip& operator=(const WidgetClip&) = delete;

private:
    Canvas& canvas_;
};

inline bool visible(Rect bounds) {
    return bounds.w > 0.0f && bounds.h > 0.0f;
}
inline Theme widget_theme(FrameState& frame, const WidgetOptions& options) {
    Theme theme = frame.theme;
    for (const auto& scope : frame.style_stack) apply_style(theme, scope);
    switch (options.variant) {
    case Variant::normal: break;
    case Variant::prominent:
        theme.control = theme.accent;
        theme.accent = theme.focus;
        break;
    case Variant::quiet:
        theme.control = theme.panel;
        theme.text = theme.secondary;
        theme.accent = theme.secondary;
        break;
    default:
        frame.report(ErrorCode::invalid_argument, "Unknown widget variant", true);
        return theme;
    }
    apply_style(theme, options.style);
    if (auto error = validate_theme(theme))
        frame.report(error->code, error->message.c_str(), true);
    return theme;
}

inline Rect inset(Rect bounds, float amount) {
    const float x = std::min(amount, bounds.w * 0.5f);
    const float y = std::min(amount, bounds.h * 0.5f);
    return {bounds.x + x, bounds.y + y, bounds.w - x * 2.0f, bounds.h - y * 2.0f};
}

inline Color mix(Color from, Color to, float amount) {
    return {from.r + (to.r - from.r) * amount,
            from.g + (to.g - from.g) * amount,
            from.b + (to.b - from.b) * amount,
            from.a + (to.a - from.a) * amount};
}

inline void initialize_animation(WidgetState& state, bool selected) {
    if (state.initialized) return;
    state.hover.duration = 0.1f;
    state.selection.duration = 0.16f;
    state.selection.from = state.selection.target = selected ? 1.0f : 0.0f;
    state.selection.elapsed = state.selection.duration;
    state.initialized = true;
}

inline void animate(FrameState& frame, WidgetState& state, Behavior behavior, bool selected) {
    state.hover.retarget(behavior.hovered ? 1.0f : 0.0f);
    state.selection.retarget(selected ? 1.0f : 0.0f);
    state.hover.tick(frame.dt, frame.theme.reduced_motion);
    state.selection.tick(frame.dt, frame.theme.reduced_motion);
}
inline void track_feedback(WidgetState& state, Rect hit, Behavior behavior, bool enabled) {
    state.bounds = hit;
    state.hovered = behavior.hovered;
    state.enabled = enabled;
}
inline Color invalid_ink(const Theme& theme) {
    return theme.canvas.r > 0.5f ? Color{0.65f, 0.10f, 0.14f, 1.0f}
                                 : Color{1.0f, 0.42f, 0.43f, 1.0f};
}

inline Color button_fill(const Theme& theme, const WidgetState& state,
                         Behavior behavior, bool enabled) {
    return enabled ? mix(theme.control, theme.accent,
                         behavior.held ? 0.28f : state.hover.value() * 0.16f)
                   : mix(theme.panel, theme.control, 0.5f);
}
inline void paint_surface(FrameState& frame, Rect bounds, Color fill,
                          bool focused, const Theme& theme, bool invalid = false) {
    if (focused) {
        // Keep the focus ring inside the hit row, including at container edges.
        frame.canvas.rounded_rect(bounds, theme.control_radius, theme.focus);
        bounds = inset(bounds, 2.0f);
    }
    if (invalid) {
        frame.canvas.rounded_rect(bounds, theme.control_radius, invalid_ink(theme));
        bounds = inset(bounds, 2.0f);
    }
    frame.canvas.rounded_rect(bounds, theme.control_radius, fill);
}

inline Color check_ink(const Theme& theme, Color fill) {
    // The selection fill animates from dark to violet. Choose the readable
    // theme ink on every frame instead of fading the check through the fill.
    const auto luminance = [](Color color) {
        const auto linear = [](float channel) {
            return channel <= 0.04045f ? channel / 12.92f
                                      : std::pow((channel + 0.055f) / 1.055f, 2.4f);
        };
        return 0.2126f * linear(color.r) + 0.7152f * linear(color.g) +
               0.0722f * linear(color.b);
    };
    const float background = luminance(fill);
    const auto contrast = [background](float ink) {
        return (std::max(background, ink) + 0.05f) /
               (std::min(background, ink) + 0.05f);
    };
    return contrast(luminance(theme.text)) >= contrast(luminance(theme.canvas))
               ? theme.text : theme.canvas;
}

} // namespace wui::detail
