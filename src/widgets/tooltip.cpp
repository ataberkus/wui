#include "painter.hpp"

#include <algorithm>

namespace wui::detail {

bool FrameState::tooltip_for_owner(Id owner, std::string_view message) {
    if (error) return false;
    const auto found = widgets.find(owner.value);
    if (found == widgets.end() || found->second.seen != generation) {
        if (found != widgets.end()) found->second.tooltip_seconds = 0;
        note_choice_error("Tooltip owner must be an earlier widget in the current scope");
        return false;
    }
    auto& widget = found->second;
    const bool hovered = widget.hovered ||
        (popup.owner == owner && interaction.popup_owner_at_frame_start_ == owner &&
         interaction.has_pointer_ &&
         contains(intersect(widget.bounds, viewport), interaction.pointer_));
    if (!widget.enabled || !hovered) {
        widget.tooltip_seconds = 0;
        return false;
    }
    if (widget.tooltip_seen != generation) {
        widget.tooltip_seconds = std::min(0.45, widget.tooltip_seconds + elapsed_seconds);
        widget.tooltip_seen = generation;
    }
    const bool due = widget.tooltip_seconds >= 0.45;
    if (due && !message.empty()) queue_noninteractive_tooltip(widget.bounds, message);
    return due;
}

void FrameState::queue_noninteractive_tooltip(Rect owner, std::string_view message) {
    if (!copy_label(message)) return;
    const auto run = shape(message, FontRole::body);
    if (!run) return;
    const Theme theme = widget_theme(*this, {});
    if (error) return;
    const float pad = std::min(theme.gap, viewport.w * 0.25f);
    const float width = std::min(viewport.w, run->width + 2 * pad);
    const float height = std::min(viewport.h, run->height + 2 * pad);
    const float x = std::clamp(owner.x, viewport.x, viewport.x + viewport.w - width);
    float y = owner.y + owner.h + 4;
    if (y + height > viewport.y + viewport.h) y = owner.y - height - 4;
    y = std::clamp(y, viewport.y, viewport.y + viewport.h - height);
    if (overlay_count == overlays.size()) overlays.emplace_back();
    auto& record = overlays[overlay_count++];
    record.kind = OverlayKind::tooltip;
    record.label.assign(message);
    record.run = run;
    record.bounds = {x, y, width, height};
    record.id = child_id(text_id(), "tooltip");
    record.theme = theme;
}

} // namespace wui::detail
