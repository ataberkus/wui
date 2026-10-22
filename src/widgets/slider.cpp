#include "painter.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>

namespace wui::detail {
namespace {

template<class T>
bool emit_slider_impl(FrameState& frame, std::string_view key, std::string_view label,
                      T& value, T min, T max, WidgetOptions options) {
    if (!frame.check_canvas()) return false;
    if (min >= max ||
        (std::is_floating_point_v<T> &&
         (!std::isfinite(static_cast<double>(min)) ||
          !std::isfinite(static_cast<double>(max)) ||
          !std::isfinite(static_cast<double>(value))))) {
        frame.report(ErrorCode::invalid_argument, "Slider bounds and value must be finite with min < max", true);
        return false;
    }
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return false;
    const auto run = frame.shape(label, FontRole::body);
    if (!run) return false;
    const Theme theme = widget_theme(frame, options);
    if (frame.error) return false;
    const Rect row = frame.take(run->height + theme.gap + theme.control_height);
    if (frame.error || !frame.check_canvas()) return false;
    const Rect control{row.x, row.y + run->height + theme.gap, row.w, theme.control_height};
    const Rect hit = intersect(control, frame.clip());
    const float pad = std::min(theme.gap, control.w * 0.25f);
    const Rect track{control.x + pad, control.y + control.h * 0.5f - 3.0f,
                     control.w - pad * 2, 6};
    initialize_animation(*state, false);
    DragResult drag;
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return false;
        drag = frame.interaction.drag(id, hit, options.enabled && visible(hit));
    }
    track_feedback(*state, hit, drag.behavior, options.enabled);
    bool changed = false;
    const double lower = static_cast<double>(min), upper = static_cast<double>(max);
    for (const auto action : drag.actions) {
        double next = static_cast<double>(value);
        switch (action.key) {
        case Key::none: {
            const double fraction = track.w > 0
                ? std::clamp((static_cast<double>(action.position_x) - track.x) / track.w, 0.0, 1.0)
                : 0.0;
            next = lower + fraction * (upper - lower);
            break;
        }
        case Key::left: next -= std::is_integral_v<T> ? 1.0 : (upper - lower) / 100.0; break;
        case Key::right: next += std::is_integral_v<T> ? 1.0 : (upper - lower) / 100.0; break;
        case Key::home: next = lower; break;
        case Key::end: next = upper; break;
        default: continue;
        }
        next = std::clamp(next, lower, upper);
        if constexpr (std::is_integral_v<T>) next = std::round(next);
        const auto updated = static_cast<T>(next);
        if (updated != value) {
            value = updated;
            changed = true;
        }
    }
    animate(frame, *state, drag.behavior, drag.behavior.held);
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return changed;
        paint_surface(frame, control, button_fill(theme, *state, drag.behavior, options.enabled),
                      drag.behavior.focused, theme, options.invalid);
        frame.canvas.rounded_rect(track, track.h * 0.5f, theme.secondary);
        const double fraction = std::clamp((static_cast<double>(value) - lower) / (upper - lower), 0.0, 1.0);
        const Rect progress{track.x, track.y, static_cast<float>(track.w * fraction), track.h};
        frame.canvas.rounded_rect(progress, progress.h * 0.5f, theme.accent);
        const float radius = std::max(0.0f, std::min(8.0f, control.h * 0.5f - 3.0f));
        const Rect thumb{track.x + progress.w - radius, control.y + control.h * 0.5f - radius,
                         radius * 2, radius * 2};
        frame.canvas.rounded_rect(thumb, radius,
                                  mix(theme.control, theme.accent, state->selection.value()));
        frame.canvas.border(thumb, radius, 1, theme.text);
    }
    if (!frame.check_canvas()) return changed;
    {
        WidgetClip clip(frame, intersect({row.x, row.y, row.w, run->height}, frame.clip()));
        if (frame.check_canvas())
            frame.glyphs(*run, {row.x + pad, row.y}, options.enabled ? theme.text : theme.secondary);
    }
    if (!frame.check_canvas()) return changed;
    char formatted[64];
    const auto [end, error] = std::to_chars(formatted, formatted + sizeof(formatted), value);
    if (error != std::errc{}) {
        frame.report(ErrorCode::capacity, "Slider value could not be formatted");
        return changed;
    }
    const std::string_view display{formatted, static_cast<std::size_t>(end - formatted)};
    if (frame.copy_label(display))
        frame.canvas.semantic(id, Role::slider, label, control, options.enabled,
                              drag.behavior.focused, drag.behavior.held, display, options.invalid);
    frame.check_canvas();
    return changed;
}

} // namespace

bool emit_slider(FrameState& frame, std::string_view key, std::string_view label,
                 float& value, float min, float max, WidgetOptions options) {
    return emit_slider_impl(frame, key, label, value, min, max, options);
}
bool emit_slider(FrameState& frame, std::string_view key, std::string_view label,
                 int& value, int min, int max, WidgetOptions options) {
    return emit_slider_impl(frame, key, label, value, min, max, options);
}

} // namespace wui::detail
