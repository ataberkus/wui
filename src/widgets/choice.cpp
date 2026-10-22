#include "painter.hpp"
#include "../layout/flow_detail.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui {

Frame::ChoiceScope::ChoiceScope(std::shared_ptr<detail::FrameState> state,
                                std::uint64_t generation, Id group, Rect bounds,
                                std::size_t count, std::size_t selected,
                                std::size_t first, ChoiceKind kind,
                                Orientation orientation, Theme theme,
                                bool enabled, bool invalid, std::string_view group_label,
                                std::string_view selected_label, Behavior trigger) noexcept
    : state_(std::move(state)), generation_(generation), group_(group), bounds_(bounds),
      count_(count), selected_(selected), first_(first), kind_(kind),
      orientation_(orientation), theme_(theme), enabled_(enabled), invalid_(invalid),
      group_label_(group_label), selected_label_(selected_label), trigger_(trigger) {}

Frame::ChoiceScope::ChoiceScope(ChoiceScope&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_), group_(other.group_),
      selected_id_(other.selected_id_), bounds_(other.bounds_), count_(other.count_),
      selected_(other.selected_), first_(other.first_), next_(other.next_),
      kind_(other.kind_), orientation_(other.orientation_), theme_(other.theme_),
      enabled_(other.enabled_), invalid_(other.invalid_),
      group_label_(other.group_label_), selected_label_(other.selected_label_),
      trigger_(other.trigger_) {}
Frame::ChoiceScope& Frame::ChoiceScope::operator=(ChoiceScope&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
        group_ = other.group_;
        selected_id_ = other.selected_id_;
        bounds_ = other.bounds_;
        count_ = other.count_;
        selected_ = other.selected_;
        first_ = other.first_;
        next_ = other.next_;
        kind_ = other.kind_;
        orientation_ = other.orientation_;
        theme_ = other.theme_;
        enabled_ = other.enabled_;
        invalid_ = other.invalid_;
        group_label_ = other.group_label_;
        selected_label_ = other.selected_label_;
        trigger_ = other.trigger_;
    }
    return *this;
}
Frame::ChoiceScope::~ChoiceScope() { close(); }
void Frame::ChoiceScope::close() noexcept {
    if (!state_) return;
    auto& frame = *state_;
    if (frame.active && frame.owner_alive && frame.generation == generation_ && !frame.error) {
        if (next_ != count_) {
            frame.report(ErrorCode::unbalanced_scope, "Choice group did not emit every option", true);
        } else if (kind_ == ChoiceKind::dropdown) {
            try {
                if (frame.popup.owner == group_) {
                    auto& widget = frame.widgets.find(group_.value)->second;
                    widget.scroll_offset = frame.route_popup_wheel(
                        frame.popup_bounds, frame.popup_offset, frame.popup_max_offset);
                }
                if (frame.copy_label(selected_label_)) {
                    frame.canvas.semantic(group_, Role::dropdown, group_label_, bounds_,
                                          enabled_, frame.interaction.focused() == group_,
                                          selected_ != std::numeric_limits<std::size_t>::max(),
                                          selected_label_, invalid_);
                    frame.check_canvas();
                }
            } catch (const std::bad_alloc&) {
                frame.report(ErrorCode::capacity, "Dropdown semantic allocation failed");
            } catch (const std::length_error&) {
                frame.report(ErrorCode::capacity, "Dropdown semantic exceeds container limits");
            }
        }
    }
    state_.reset();
}

Frame::ChoiceScope Frame::begin_choices(std::string_view key, std::string_view label,
                                        std::size_t count, std::size_t selected_index,
                                        ChoiceKind kind, Orientation orientation,
                                        WidgetOptions options, std::string_view selected_label) {
    if (!state_) {
#ifndef NDEBUG
        std::fputs("wui contract: Frame handle has been moved\n", stderr);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
#endif
        return ChoiceScope{};
    }
    auto& frame = *state_;
    options = frame.effective(options);
    try {
        if (!frame.usable(generation_)) return ChoiceScope{};
        if (orientation != Orientation::horizontal && orientation != Orientation::vertical) {
            frame.report(ErrorCode::invalid_argument, "Unknown choice orientation", true);
            return ChoiceScope{};
        }
        if (frame.count >= frame.max_widgets || count > frame.max_widgets - frame.count - 1) {
            frame.report(ErrorCode::capacity, "Choice group exceeds widget limit");
            return ChoiceScope{};
        }
        if (kind == ChoiceKind::radio && !label.empty()) {
            text(label);
            if (frame.error) return ChoiceScope{};
        }
        const Id group = frame.keyed_id(key);
        auto* widget = frame.register_widget(group);
        if (!widget || !frame.copy_label(label)) return ChoiceScope{};
        const Theme theme = detail::widget_theme(frame, options);
        if (frame.error) return ChoiceScope{};
        const bool horizontal = kind == ChoiceKind::tabs && orientation == Orientation::horizontal;
        const double height = horizontal || kind == ChoiceKind::dropdown
            ? theme.control_height
            : static_cast<double>(count) * theme.control_height +
              static_cast<double>(count - 1) * theme.gap;
        if (!std::isfinite(height) || height > std::numeric_limits<float>::max()) {
            frame.report(ErrorCode::capacity, "Choice group height exceeds layout limits");
            return ChoiceScope{};
        }
        const Rect bounds = frame.take(static_cast<float>(height));
        widget->bounds = intersect(bounds, frame.clip());
        widget->enabled = options.enabled;
        if (frame.error) return ChoiceScope{};
        const auto first = frame.row_rects.size();
        if (horizontal) {
            frame.choice_columns.assign(count, Column{ColumnKind::share, 1});
            auto resolved = detail::append_row_cells(bounds, frame.choice_columns,
                                                      theme.gap, frame.row_rects);
            if (auto* error = std::get_if<Error>(&resolved)) {
                frame.report(error->code, error->message.c_str(), true);
                return ChoiceScope{};
            }
        }
        Behavior trigger;
        if (kind == ChoiceKind::dropdown)
            trigger = detail::begin_dropdown(frame, *widget, group, bounds, label,
                                             selected_label, count, theme, options.enabled,
                                             options.invalid);
        if (frame.error) return ChoiceScope{};
        return ChoiceScope(state_, generation_, group, bounds, count, selected_index,
                           first, kind, orientation, theme, options.enabled, options.invalid,
                           label, selected_label, trigger);
    } catch (const std::bad_alloc&) {
        frame.report(ErrorCode::capacity, "Choice group allocation failed");
    } catch (const std::length_error&) {
        frame.report(ErrorCode::capacity, "Choice group exceeds container limits");
    }
    return ChoiceScope{};
}

bool Frame::ChoiceScope::option(std::size_t index, std::string_view label,
                                std::size_t value_hash) {
    if (!state_) return false;
    auto& frame = *state_;
    try {
        if (!frame.usable(generation_)) return false;
        if (index != next_ || index >= count_) {
            frame.report(ErrorCode::unbalanced_scope, "Choice options must emit in order", true);
            return false;
        }
        ++next_;
        if (kind_ == ChoiceKind::dropdown && frame.popup.owner != group_) return false;
        const Id id = child_id(child_id(group_, "option"),
            {reinterpret_cast<const char*>(&value_hash), sizeof(value_hash)});
        auto* state = frame.register_widget(id);
        if (!state) return false;
        if (kind_ == ChoiceKind::dropdown) {
            const double y = static_cast<double>(frame.popup_bounds.y) +
                static_cast<double>(index) * (theme_.control_height + theme_.gap) -
                frame.popup_offset;
            if (!std::isfinite(y) || y < std::numeric_limits<float>::lowest() ||
                y > std::numeric_limits<float>::max()) {
                frame.report(ErrorCode::capacity, "Dropdown option geometry exceeds layout limits");
                return false;
            }
            const Rect row{frame.popup_bounds.x, static_cast<float>(y),
                           frame.popup_bounds.w, theme_.control_height};
            const Rect hit = intersect(intersect(row, frame.popup_bounds), frame.viewport);
            auto behavior = frame.route_popup_button(id, row, enabled_);
            detail::track_feedback(*state, hit, behavior, enabled_);
            if (trigger_.activated &&
                (selected_ == index || (selected_ == std::numeric_limits<std::size_t>::max() &&
                                        index == 0))) {
                frame.focus_popup_option(id);
                behavior.focused = true;
            }
            if (behavior.activated) {
                selected_ = index;
                selected_label_ = label;
                frame.close_popup(true);
            }
            if (frame.popup.owner == group_ && detail::visible(hit)) {
                detail::initialize_animation(*state, selected_ == index);
                detail::animate(frame, *state, behavior, selected_ == index);
                auto run = frame.shape(label, FontRole::body);
                if (run) frame.record_popup_option(id, row, label, std::move(run),
                                                   behavior, selected_ == index,
                                                   enabled_, invalid_);
            }
            return behavior.activated;
        }
        if (!frame.copy_label(label)) return false;
        const auto run = frame.shape(label, FontRole::body);
        if (!run) return false;
        const Rect row = kind_ == ChoiceKind::tabs && orientation_ == Orientation::horizontal
            ? frame.row_rects[first_ + index]
            : Rect{bounds_.x, bounds_.y + static_cast<float>(index) *
                       (theme_.control_height + theme_.gap),
                   bounds_.w, theme_.control_height};
        const Rect hit = intersect(row, frame.clip());
        detail::initialize_animation(*state, selected_ == index);
        Behavior behavior;
        {
            detail::WidgetClip clip(frame, hit);
            if (!frame.check_canvas()) return false;
            behavior = frame.interaction.button(id, hit, enabled_ && detail::visible(hit));
            detail::track_feedback(*state, hit, behavior, enabled_);
            if (behavior.hovered) frame.widgets.find(group_.value)->second.hovered = true;
            if (behavior.activated) {
                if (selected_id_ != Id{} && selected_id_ != id)
                    detail::CanvasAccess::clear_choice_selection(frame.canvas, selected_id_);
                selected_ = index;
            }
            const bool selected = selected_ == index;
            if (selected) selected_id_ = id;
            detail::animate(frame, *state, behavior, selected);
            const Color fill = kind_ == ChoiceKind::radio
                ? detail::mix(theme_.panel, theme_.control,
                              behavior.held ? 1.0f : state->hover.value())
                : detail::mix(theme_.control, theme_.accent, state->selection.value());
            detail::paint_surface(frame, row, fill, behavior.focused, theme_, invalid_);
            if (!frame.check_canvas()) return behavior.activated;
            float label_x = row.x + theme_.gap;
            if (kind_ == ChoiceKind::radio) {
                const float size = std::min(theme_.control_height * 0.5f,
                                            std::min(row.h, row.w) * 0.5f);
                const Rect circle{label_x, row.y + (row.h - size) * 0.5f, size, size};
                frame.canvas.rounded_rect(circle, size * 0.5f,
                                          selected ? theme_.accent : theme_.secondary);
                const auto inner = detail::inset(circle, 2);
                frame.canvas.rounded_rect(inner, inner.w * 0.5f, theme_.control);
                if (selected) {
                    const auto dot = detail::inset(circle, 5);
                    frame.canvas.rounded_rect(dot, dot.w * 0.5f, theme_.accent);
                }
                label_x += size + theme_.gap;
            } else {
                label_x = row.x + std::max(0.0f, (row.w - run->width) * 0.5f);
            }
            if (frame.check_canvas()) {
                detail::WidgetClip content(frame, row);
                if (frame.check_canvas())
                    frame.glyphs(*run, {label_x, row.y + (row.h - run->height) * 0.5f},
                                 enabled_ ? theme_.text : theme_.secondary);
            }
            if (frame.check_canvas()) {
                if (selected && !frame.copy_label(label)) return behavior.activated;
                frame.canvas.semantic(id, kind_ == ChoiceKind::radio ? Role::radio : Role::tab,
                                      label, row, enabled_, behavior.focused, selected,
                                      selected ? label : std::string_view{}, invalid_);
            }
        }
        frame.check_canvas();
        return behavior.activated;
    } catch (const std::bad_alloc&) {
        frame.report(ErrorCode::capacity, "Choice option allocation failed");
    } catch (const std::length_error&) {
        frame.report(ErrorCode::capacity, "Choice option exceeds container limits");
    }
    return false;
}

} // namespace wui
