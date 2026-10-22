#include "painter.hpp"

#include <algorithm>
#include <array>

namespace wui::detail {

bool emit_checkbox(FrameState& frame, std::string_view key, std::string_view label,
                   bool& value, WidgetOptions options) {
    if (!frame.check_canvas()) return false;
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return false;
    const auto run = frame.shape(label, FontRole::body);
    if (!run) return false;
    const auto theme = widget_theme(frame, options);
    if (frame.error) return false;
    const auto& container = frame.stack.back();
    const float width = std::max(0.0f, container.bounds.w - 2.0f * container.inset);
    const float padding = std::min(theme.gap, width * 0.25f);
    const float inner_width = width - 2.0f * padding;
    const float preferred_size = std::max(theme.body_size + theme.gap * 0.5f,
                                          theme.control_height * 0.5f);
    const float box_size = std::min(preferred_size, inner_width);
    const bool stacked = !label.empty() &&
                         preferred_size + theme.gap + run->width > inner_width;
    const float content_height = stacked ? run->height + theme.gap + box_size
                                         : std::max(run->height, box_size);
    const Rect row = frame.take(std::max(theme.control_height, content_height + theme.gap));
    if (frame.error || !frame.check_canvas()) return false;
    const Rect hit = intersect(row, frame.clip());
    initialize_animation(*state, value);
    Behavior behavior;
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return false;
        behavior = frame.interaction.button(id, hit, options.enabled && visible(hit));
        track_feedback(*state, hit, behavior, options.enabled);
        if (behavior.activated) value = !value;
        animate(frame, *state, behavior, value);

        const float hover = options.enabled ? state->hover.value() : 0.0f;
        const Color row_fill = mix(theme.panel, theme.control, behavior.held ? 1.0f : hover);
        paint_surface(frame, row, row_fill, behavior.focused, theme, options.invalid);
        const float content_y = row.y + (row.h - content_height) * 0.5f;
        const Rect box{row.x + padding,
                       stacked ? content_y + run->height + theme.gap
                               : row.y + (row.h - box_size) * 0.5f,
                       box_size, box_size};
        const float selected = state->selection.value();
        const Color unselected = mix(theme.control, theme.accent, behavior.held ? 0.28f : hover * 0.16f);
        const Color fill = mix(unselected, options.enabled ? theme.accent : theme.secondary, selected);
        const Color border = value ? fill : (options.enabled ? theme.secondary
                                                             : mix(theme.control, theme.secondary, 0.5f));
        const float radius = std::min(theme.control_radius * 0.5f, box_size * 0.5f);
        frame.canvas.rounded_rect(box, radius, border);
        frame.canvas.rounded_rect(inset(box, 1.0f), std::max(0.0f, radius - 1.0f), fill);
        if (value && visible(box)) {
            const std::array points{
                Vec2{box.x + box.w * 0.23f, box.y + box.h * 0.51f},
                Vec2{box.x + box.w * 0.43f, box.y + box.h * 0.70f},
                Vec2{box.x + box.w * 0.78f, box.y + box.h * 0.30f},
            };
            frame.canvas.line(points, std::min(2.0f, box_size * 0.12f), check_ink(theme, fill));
        }
        if (frame.check_canvas()) {
            const float label_x = stacked ? row.x + padding : box.x + box.w + theme.gap;
            const Rect label_bounds{label_x,
                                    stacked ? content_y : row.y + (row.h - run->height) * 0.5f,
                                    std::max(0.0f, row.x + row.w - padding - label_x), run->height};
            WidgetClip label_clip(frame, label_bounds);
            if (frame.check_canvas()) {
                frame.glyphs(*run, {label_bounds.x, label_bounds.y},
                             options.enabled ? theme.text : theme.secondary);
            }
        }
        if (frame.check_canvas()) {
            frame.canvas.semantic(id, Role::checkbox, label, row, options.enabled,
                                  behavior.focused, value, {}, options.invalid);
        }
    }
    frame.check_canvas();
    return behavior.activated;
}

} // namespace wui::detail
