#include "painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace wui::detail {

Behavior begin_dropdown(FrameState& frame, WidgetState& state, Id group, Rect trigger,
                        std::string_view group_label, std::string_view selected_label,
                        std::size_t count, const Theme& theme, bool enabled, bool invalid) {
    const Rect hit = intersect(trigger, frame.clip());
    initialize_animation(state, false);
    Behavior behavior;
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return {};
        behavior = frame.interaction.button(group, hit, enabled && visible(hit));
        track_feedback(state, hit, behavior, enabled);
        animate(frame, state, behavior, false);
        paint_surface(frame, trigger, button_fill(theme, state, behavior, enabled),
                      behavior.focused, theme, invalid);
        if (!frame.check_canvas()) return behavior;
        const auto shown = selected_label.empty() ? group_label : selected_label;
        const auto run = frame.shape(shown, FontRole::body);
        if (run) {
            WidgetClip content(frame, inset(trigger, std::min(theme.gap, trigger.w * 0.25f)));
            if (frame.check_canvas())
                frame.glyphs(*run, {trigger.x + theme.gap,
                                   trigger.y + (trigger.h - run->height) * 0.5f},
                             enabled ? theme.text : theme.secondary);
        }
        if (frame.check_canvas()) {
            const float x = trigger.x + trigger.w - theme.gap - 10;
            const float y = trigger.y + trigger.h * 0.5f;
            const std::array<Vec2, 3> arrow{{{x, y - 3}, {x + 10, y - 3}, {x + 5, y + 3}}};
            frame.canvas.convex(arrow, enabled ? theme.text : theme.secondary);
        }
    }
    if (!frame.check_canvas()) return behavior;

    if (frame.popup.owner == group &&
        (!enabled || frame.popup_anchor.x != trigger.x || frame.popup_anchor.y != trigger.y ||
         frame.popup_anchor.w != trigger.w || frame.popup_anchor.h != trigger.h))
        frame.close_popup(false);
    if (behavior.activated && enabled && frame.popup.owner == Id{}) {
        frame.popup = {group, frame.interaction.focused()};
        frame.popup_anchor = trigger;
        state.scroll_offset = 0;
    }
    if (frame.popup.owner != group) return behavior;

    const Rect viewport = frame.viewport;
    const double desired = static_cast<double>(count) * theme.control_height +
                           static_cast<double>(count - 1) * theme.gap;
    const double below = static_cast<double>(viewport.y) + viewport.h -
                         (static_cast<double>(trigger.y) + trigger.h);
    const double above = static_cast<double>(trigger.y) - viewport.y;
    const bool flip = below < desired && above > below;
    const double available = flip ? above : below;
    const float height = static_cast<float>(std::min(desired, std::max(0.0, available)));
    const float width = std::min(trigger.w, viewport.w);
    if (height <= 0 || width <= 0 || !std::isfinite(height)) {
        frame.close_popup(false);
        return behavior;
    }
    const double top = flip ? static_cast<double>(trigger.y) - height :
                              static_cast<double>(trigger.y) + trigger.h;
    const Rect list{std::clamp(trigger.x, viewport.x, viewport.x + viewport.w - width),
                    static_cast<float>(std::clamp(top, static_cast<double>(viewport.y),
                                      static_cast<double>(viewport.y) + viewport.h - height)),
                    width, height};
    frame.popup_bounds = list;
    frame.popup_theme = theme;
    frame.popup_max_offset = static_cast<float>(std::min(
        std::max(0.0, desired - height), static_cast<double>(std::numeric_limits<float>::max())));
    frame.popup_offset = std::clamp(state.scroll_offset, 0.0f, frame.popup_max_offset);
    frame.popup_focus_offset = frame.popup_offset;
    return behavior;
}

} // namespace wui::detail
