#include "painter.hpp"

#include <algorithm>

namespace wui::detail {

bool emit_icon_button(FrameState& frame, std::string_view key, std::string_view label,
                      TextureHandle image, WidgetOptions options) {
    if (!frame.check_canvas()) return false;
    if (label.empty()) {
        frame.report(ErrorCode::invalid_argument, "Icon buttons need an accessible label", true);
        return false;
    }
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return false;
    const auto theme = widget_theme(frame, options);
    if (frame.error) return false;
    const Rect row = frame.take(theme.control_height);
    if (frame.error || !frame.check_canvas()) return false;
    const Rect hit = intersect(row, frame.clip());
    initialize_animation(*state, false);
    Behavior behavior;
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return false;
        behavior = frame.interaction.button(id, hit, options.enabled && visible(hit));
        track_feedback(*state, hit, behavior, options.enabled);
        animate(frame, *state, behavior, false);
        paint_surface(frame, row, button_fill(theme, *state, behavior, options.enabled),
                      behavior.focused, theme, options.invalid);
        if (frame.check_canvas()) {
            const float padding = std::min(theme.gap, row.w * 0.25f);
            const float side = std::max(0.0f, std::min(row.h - 2 * padding,
                                                       row.w - 2 * padding));
            if (side > 0) frame.canvas.image({row.x + (row.w - side) * 0.5f,
                                              row.y + (row.h - side) * 0.5f, side, side},
                                             image, {0, 0, 1, 1}, theme.text);
        }
        if (frame.check_canvas())
            frame.canvas.semantic(id, Role::button, label, row, options.enabled,
                                  behavior.focused, false, {}, options.invalid);
    }
    frame.check_canvas();
    return behavior.activated;
}

} // namespace wui::detail
