#include "painter.hpp"

#include <algorithm>
#include <array>
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
            const Rect bounds = frame.popup_bounds;
            const Rect clipped = intersect(frame.viewport, bounds);
            const float radius = theme.control_radius;
            const Color surface = raised_fill(theme);
            // Elevation: a soft cached shadow at the root clip, then a raised
            // sheet. Idle rows paint nothing, so the menu reads as one sheet.
            // The cached shadow blurs a (2r+1)px shape, so at sigma 8 its peak
            // coverage is about 0.7; alpha 0.8 gives a moderate edge shadow.
            // The shadow is decorative: without room for it the menu still
            // paints, just flat.
            CanvasAccess::decorative_shadow(frame.canvas,
                                            {bounds.x, bounds.y + 4.0f, bounds.w, bounds.h},
                                            radius, shadow_sigma(frame, 8.0f),
                                            {0.0f, 0.0f, 0.0f, 0.8f});
            if (!frame.check_canvas()) return;
            WidgetClip clip(frame, bounds);
            if (!frame.check_canvas()) return;
            frame.canvas.rounded_rect(bounds, radius, surface);
            if (!frame.check_canvas()) return;
            bool any_invalid = false;
            for (std::size_t i = 0; i < frame.overlay_count; ++i) {
                const auto& option = frame.overlays[i];
                if (option.kind != OverlayKind::popup_option) continue;
                any_invalid = any_invalid || option.invalid;
                const Rect row = option.bounds;
                const Rect band{row.x + 4.0f, row.y + 3.0f, std::max(0.0f, row.w - 8.0f),
                                std::max(0.0f, row.h - 6.0f)};
                // The focused row is a solid accent band: the keyboard indicator.
                const bool focused = option.behavior.focused;
                const bool tinted = focused || option.behavior.held || option.behavior.hovered;
                const Color fill = focused
                    ? (option.behavior.held ? mix(theme.accent, theme.focus, 0.28f) : theme.accent)
                    : mix(surface, theme.accent, option.behavior.held ? 0.28f : 0.16f);
                if (tinted) frame.canvas.rounded_rect(band, std::max(0.0f, radius - 2.0f), fill);
                if (!frame.check_canvas()) return;
                const Color ink = option.enabled ? readable_ink(theme, tinted ? fill : surface)
                                                 : disabled_ink(theme);
                if (option.run) {
                    const float text_x = band.x + theme.gap;
                    frame.glyphs(*option.run,
                                 {text_x, row.y + (row.h - option.run->height) * 0.5f}, ink);
                    if (!frame.check_canvas()) return;
                    // Check mark for the chosen value, only where it clears the label.
                    const float x0 = band.x + band.w - theme.gap - 10.0f;
                    if (option.selected && band.w >= 40.0f &&
                        text_x + option.run->width + 8.0f <= x0) {
                        const float cy = band.y + band.h * 0.5f;
                        const std::array<Vec2, 3> check{{{x0, cy}, {x0 + 3.5f, cy + 3.5f},
                                                         {x0 + 10.0f, cy - 3.5f}}};
                        frame.canvas.line(check, 1.75f, ink);
                        if (!frame.check_canvas()) return;
                    }
                }
                if (option.selected && !frame.copy_label(option.label)) return;
                frame.canvas.semantic(option.id, Role::dropdown, option.label,
                                      intersect(clipped, option.bounds),
                                      option.enabled, option.behavior.focused, option.selected,
                                      option.selected ? std::string_view{option.label} : std::string_view{},
                                      option.invalid);
                if (!frame.check_canvas()) return;
            }
            frame.canvas.border(bounds, radius, 1.0f, any_invalid ? invalid_ink(theme) : track_ink(theme));
            if (!frame.check_canvas()) return;
        }
    }
    for (std::size_t i = 0; i < frame.overlay_count; ++i) {
        const auto& tooltip = frame.overlays[i];
        if (tooltip.kind != OverlayKind::tooltip) continue;
        const auto& theme = tooltip.theme;
        const Rect bounds = tooltip.bounds;
        CanvasAccess::decorative_shadow(frame.canvas, {bounds.x, bounds.y + 2.0f, bounds.w, bounds.h},
                                        theme.control_radius, shadow_sigma(frame, 4.0f),
                                        {0.0f, 0.0f, 0.0f, 0.45f});
        if (!frame.check_canvas()) return;
        WidgetClip clip(frame, bounds);
        if (!frame.check_canvas()) return;
        frame.canvas.rounded_rect(bounds, theme.control_radius, raised_fill(theme));
        if (tooltip.run)
            frame.glyphs(*tooltip.run,
                         {bounds.x + theme.gap, bounds.y + (bounds.h - tooltip.run->height) * 0.5f},
                         theme.text);
        if (!frame.check_canvas()) return;
        frame.canvas.border(bounds, theme.control_radius, 1.0f, track_ink(theme));
        if (!frame.check_canvas()) return;
        frame.canvas.semantic(tooltip.id, Role::tooltip, tooltip.label,
                              intersect(frame.viewport, tooltip.bounds), false, false, false);
        if (!frame.check_canvas()) return;
    }
}

} // namespace wui::detail
