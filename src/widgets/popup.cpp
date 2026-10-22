#include "painter.hpp"

#include <algorithm>
#include <utility>

namespace wui::detail {

Behavior FrameState::route_popup_button(Id option, Rect bounds, bool enabled) {
    if (popup.owner == Id{} ||
        (interaction.popup_owner_at_frame_start_ != Id{} &&
         popup.owner != interaction.popup_owner_at_frame_start_))
        return {};
    interaction.popup_list_ = intersect(viewport, popup_bounds);
    const auto behavior = interaction.popup_button(option, bounds, enabled);
    if (behavior.focused && interaction.popup_navigated_) {
        const double top = popup_bounds.y;
        const double bottom = top + popup_bounds.h;
        const double requested = bounds.y < top
            ? static_cast<double>(popup_offset) + bounds.y - top
            : bounds.y + static_cast<double>(bounds.h) > bottom
                ? static_cast<double>(popup_offset) + bounds.y + bounds.h - bottom
                : popup_offset;
        popup_focus_offset = static_cast<float>(std::clamp(
            requested, 0.0, static_cast<double>(popup_max_offset)));
    }
    if (interaction.popup_dismissed_) close_popup(interaction.popup_escape_);
    return behavior;
}

float FrameState::route_popup_wheel(Rect list, float current_offset, float max_offset) {
    const float next = interaction.popup_wheel(list, popup.owner, current_offset, max_offset);
    return interaction.popup_navigated_ ? popup_focus_offset : next;
}
void FrameState::focus_popup_option(Id option) {
    interaction.popup_focus(option);
}

void FrameState::close_popup(bool restore_focus) {
    if (restore_focus && popup.previous_focus != Id{})
        interaction.focus_ = popup.previous_focus;
    popup = {};
}


void FrameState::record_popup_option(Id id, Rect bounds, std::string_view label,
                                     std::shared_ptr<const TextRun> run, Behavior behavior,
                                     bool selected, bool enabled, bool invalid) {
    if (!copy_label(label)) return;
    if (overlay_count == overlays.size()) overlays.emplace_back();
    auto& record = overlays[overlay_count++];
    record.kind = OverlayKind::popup_option;
    record.label.assign(label);
    record.run = std::move(run);
    record.bounds = bounds;
    record.id = id;
    record.behavior = behavior;
    record.selected = selected;
    record.enabled = enabled;
    record.invalid = invalid;
}

void paint_popup(FrameState& frame) {
    // Generic popups painted their own layer during emission.
    if (frame.popup.owner != Id{} && !frame.popup.generic) {
        const auto owner = frame.widgets.find(frame.popup.owner.value);
        if (owner == frame.widgets.end() || owner->second.seen != frame.generation) {
            frame.popup = {};
        } else {
            const auto& theme = frame.popup_theme;
            const Rect clipped = intersect(frame.viewport, frame.popup_bounds);
            frame.canvas.rounded_rect(frame.popup_bounds, theme.control_radius, theme.panel);
            if (!frame.check_canvas()) return;
            WidgetClip clip(frame, frame.popup_bounds);
            if (!frame.check_canvas()) return;
            for (std::size_t i = 0; i < frame.overlay_count; ++i) {
                const auto& option = frame.overlays[i];
                if (option.kind != OverlayKind::popup_option) continue;
                const auto fill = mix(theme.control, theme.accent,
                    option.selected ? 0.65f : option.behavior.held ? 0.35f :
                    option.behavior.hovered ? 0.18f : 0.0f);
                paint_surface(frame, option.bounds, fill, option.behavior.focused, theme,
                              option.invalid);
                if (!frame.check_canvas()) return;
                if (option.run) {
                    frame.glyphs(*option.run,
                                 {option.bounds.x + theme.gap,
                                  option.bounds.y + (option.bounds.h - option.run->height) * 0.5f},
                                 option.enabled ? theme.text : theme.secondary);
                    if (!frame.check_canvas()) return;
                }
                if (option.selected && !frame.copy_label(option.label)) return;
                frame.canvas.semantic(option.id, Role::dropdown, option.label,
                                      intersect(clipped, option.bounds),
                                      option.enabled, option.behavior.focused, option.selected,
                                      option.selected ? std::string_view{option.label} : std::string_view{},
                                      option.invalid);
                if (!frame.check_canvas()) return;
            }
        }
    }
    for (std::size_t i = 0; i < frame.overlay_count; ++i) {
        const auto& tooltip = frame.overlays[i];
        if (tooltip.kind != OverlayKind::tooltip) continue;
        const auto& theme = tooltip.theme;
        frame.canvas.rounded_rect(tooltip.bounds, theme.control_radius, theme.control);
        if (!frame.check_canvas()) return;
        WidgetClip clip(frame, tooltip.bounds);
        if (!frame.check_canvas()) return;
        if (tooltip.run)
            frame.glyphs(*tooltip.run,
                         {tooltip.bounds.x + theme.gap,
                          tooltip.bounds.y + (tooltip.bounds.h - tooltip.run->height) * 0.5f},
                         theme.text);
        if (!frame.check_canvas()) return;
        frame.canvas.semantic(tooltip.id, Role::tooltip, tooltip.label,
                              intersect(frame.viewport, tooltip.bounds), false, false, false);
        if (!frame.check_canvas()) return;
    }
}

} // namespace wui::detail
