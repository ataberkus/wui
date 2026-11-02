#include "painter.hpp"
#include "../core/rect_ops.hpp"

#include <algorithm>

namespace wui::detail {

// Canvas checks: the caller verified usable() (so the canvas had not failed),
// and the canvas can only fail inside a canvas call, so before the first
// paint `frame.error` alone decides. Paint on a failed canvas is ignored and
// keeps the first error, so a check is only needed where it guards something
// other than paint (interaction routing) and once at the end to report.
bool emit_icon_button(FrameState& frame, std::string_view key, std::string_view label,
                      TextureHandle image, WidgetOptions options) {
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
    if (frame.error) return false;
    const Rect hit = intersect_rects(row, frame.clip());
    initialize_animation(*state, false);
    Behavior behavior;
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return false;
        behavior = frame.interaction.button(id, hit, options.enabled && visible(hit));
        track_feedback(*state, hit, behavior, options.enabled);
        animate(frame, *state, behavior, false);
        const Color fill = button_fill(theme, *state, behavior, options.enabled);
        paint_surface(frame, row, fill, behavior.focused, theme, options.invalid);
        const float padding = std::min(theme.gap, row.w * 0.25f);
        const float side = std::max(0.0f, std::min(row.h - 2 * padding, row.w - 2 * padding));
        if (side > 0) frame.canvas.image({row.x + (row.w - side) * 0.5f,
                                          row.y + (row.h - side) * 0.5f, side, side},
                                         image, {0, 0, 1, 1},
                                         options.enabled ? readable_ink(theme, fill)
                                                         : disabled_ink(theme));
        frame.canvas.semantic(id, Role::button, label, row, options.enabled,
                              behavior.focused, false, {}, options.invalid);
    }
    frame.check_canvas();
    return behavior.activated;
}

} // namespace wui::detail
