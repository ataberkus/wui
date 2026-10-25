#include "painter.hpp"

#include <algorithm>

namespace wui::detail {

bool emit_button(FrameState& frame, std::string_view key, std::string_view label,
                 WidgetOptions options) {
    if (!frame.check_canvas()) return false;
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return false;
    const auto run = frame.shape(label, FontRole::body);
    if (!run) return false;
    const auto theme = widget_theme(frame, options);
    if (frame.error) return false;
    const Rect row = frame.take(std::max(theme.control_height, run->height + theme.gap));
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
        const Color fill = button_fill(theme, *state, behavior, options.enabled);
        paint_surface(frame, row, fill, behavior.focused, theme, options.invalid);
        if (frame.check_canvas()) {
            const float padding = std::min(theme.gap, row.w * 0.25f);
            const Rect content{row.x + padding, row.y, row.w - 2.0f * padding, row.h};
            WidgetClip label_clip(frame, content);
            if (frame.check_canvas()) {
                const float x = content.x + align_offset(options.label_align, content.w, run->width);
                frame.glyphs(*run, {x, row.y + (row.h - run->height) * 0.5f},
                             options.enabled ? theme.text : theme.secondary);
            }
        }
        if (frame.check_canvas()) {
            frame.canvas.semantic(id, Role::button, label, row, options.enabled,
                                  behavior.focused, false, {}, options.invalid);
        }
    }
    frame.check_canvas();
    return behavior.activated;
}

} // namespace wui::detail
