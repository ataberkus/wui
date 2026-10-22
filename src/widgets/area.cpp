#include "painter.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui {
namespace {
void moved_frame() {
#ifndef NDEBUG
    std::fputs("wui contract: Frame handle has been moved\n", stderr);
    std::fflush(stderr);
    assert(false && "wui programmer contract violation");
#endif
}
bool finite_rect(Rect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) &&
           r.w >= 0 && r.h >= 0 && std::isfinite(r.x + r.w) && std::isfinite(r.y + r.h);
}
bool valid_color(Color c) {
    for (float channel : {c.r, c.g, c.b, c.a})
        if (!std::isfinite(channel) || channel < 0 || channel > 1) return false;
    return true;
}
} // namespace

AreaScope::AreaScope(std::shared_ptr<detail::FrameState> state, std::uint64_t generation,
                     std::size_t depth, Rect bounds) noexcept
    : state_(std::move(state)), generation_(generation), depth_(depth), bounds_(bounds) {}
AreaScope::AreaScope(AreaScope&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_),
      depth_(other.depth_), bounds_(other.bounds_) {}
AreaScope& AreaScope::operator=(AreaScope&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
        depth_ = other.depth_;
        bounds_ = other.bounds_;
    }
    return *this;
}
AreaScope::~AreaScope() { close(); }
void AreaScope::close() noexcept {
    if (state_) {
        state_->close_area(generation_, depth_);
        state_.reset();
    }
}

AreaScope Frame::region(std::string_view key, Rect bounds) {
    if (!state_) { moved_frame(); return {}; }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return {};
        if (!finite_rect(bounds)) {
            s.report(ErrorCode::invalid_argument, "Region bounds must be finite and nonnegative", true);
            return {};
        }
        const Id id = s.keyed_id(key);
        if (!s.register_widget(id)) return {};
        const Rect child_clip = intersect(s.clip(), bounds);
        s.canvas.push_clip(child_clip);
        if (!s.check_canvas()) return {};
        s.stack.push_back({id, bounds, child_clip, Flow(bounds, s.theme.gap), {}, 0, 0});
        s.areas.push_back({detail::AreaKind::region, id, {}, s.stack.size(), s.style_stack.size(),
                           bounds});
        return AreaScope(state_, generation_, s.areas.size(), bounds);
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Region allocation failed");
    } catch (const std::length_error&) {
        s.report(ErrorCode::capacity, "Region exceeds container limits");
    }
    return {};
}

AreaScope Frame::overlay(std::string_view key, Rect bounds) {
    if (!state_) { moved_frame(); return {}; }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return {};
        if (!finite_rect(bounds)) {
            s.report(ErrorCode::invalid_argument, "Overlay bounds must be finite and nonnegative", true);
            return {};
        }
        const Id id = s.keyed_id(key);
        if (!s.register_widget(id)) return {};
        const auto layer = detail::CanvasAccess::begin_layer(s.canvas, bounds);
        if (!s.check_canvas()) return {};
        s.stack.push_back({id, bounds, intersect(s.viewport, bounds),
                           Flow(bounds, s.theme.gap), {}, 0, 0});
        s.areas.push_back({detail::AreaKind::overlay, id, layer, s.stack.size(),
                           s.style_stack.size(), bounds});
        return AreaScope(state_, generation_, s.areas.size(), bounds);
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Overlay allocation failed");
    } catch (const std::length_error&) {
        s.report(ErrorCode::capacity, "Overlay exceeds container limits");
    }
    return {};
}

void Frame::open_popup(std::string_view key) {
    if (!state_) { moved_frame(); return; }
    auto& s = *state_;
    if (!s.usable(generation_)) return;
    const Id id = s.keyed_id(key);
    if (s.error || (s.popup.generic && s.popup.owner == id)) return;
    // Opening replaces any open dropdown list or popup. The opening frame
    // counts as emitted, so the popup survives being opened after popup().
    s.popup = {id, s.interaction.focused(), true, true};
}

bool Frame::popup_open(std::string_view key) {
    if (!state_) { moved_frame(); return false; }
    auto& s = *state_;
    if (!s.usable(generation_)) return false;
    const Id id = s.keyed_id(key);
    return !s.error && s.popup.generic && s.popup.owner == id;
}

void Frame::close_popup() {
    if (!state_) { moved_frame(); return; }
    auto& s = *state_;
    if (s.usable(generation_) && s.popup.generic) s.close_popup(false);
}

AreaScope Frame::popup(std::string_view key, PopupOptions options) {
    if (!state_) { moved_frame(); return {}; }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return {};
        if (!finite_rect(options.anchor) || !std::isfinite(options.width) || options.width < 0) {
            s.report(ErrorCode::invalid_argument, "Popup anchor and width must be finite and nonnegative", true);
            return {};
        }
        const Id id = s.keyed_id(key);
        auto* widget = s.register_widget(id);
        if (!widget || !s.popup.generic || s.popup.owner != id) return {};
        s.popup.emitted = true;
        const Theme theme = detail::widget_theme(s, {});
        if (s.error) return {};
        const Rect vp = s.viewport;
        const float width = std::min(vp.w, options.width > 0 ? options.width
            : std::max(options.anchor.w, theme.control_height * 6));
        // The previous frame's measured height positions the popup; the
        // background always uses the height measured at close.
        const float height = std::min(widget->measured_height, vp.h);
        float x = 0, y = 0;
        if (options.modal) {
            x = vp.x + (vp.w - width) * 0.5f;
            y = vp.y + (vp.h - height) * 0.5f;
        } else {
            const Rect a = options.anchor;
            x = std::clamp(a.x, vp.x, vp.x + vp.w - width);
            y = a.y + a.h;
            if (y + height > vp.y + vp.h && a.y - height >= vp.y) y = a.y - height;
            y = std::clamp(y, vp.y, std::max(vp.y, vp.y + vp.h - height));
        }
        const Rect bounds{x, y, width, height};
        const auto layer = detail::CanvasAccess::begin_layer(s.canvas, vp);
        if (!s.check_canvas()) return {};
        if (options.modal) s.canvas.rounded_rect(vp, 0, Color{0, 0, 0, 0.45f});
        const auto mark = detail::CanvasAccess::open_panel(s.canvas, id, {}, bounds,
                                                           theme.control_radius, theme.panel);
        if (!s.check_canvas()) return {};
        const float inset = std::min(theme.gap, width * 0.5f);
        const Rect interior{x + inset, y + inset, std::max(0.0f, width - 2 * inset),
                            std::max(0.0f, vp.y + vp.h - (y + inset))};
        const Rect child_clip = intersect(vp, interior);
        s.canvas.push_clip(child_clip);
        if (!s.check_canvas()) return {};
        s.stack.push_back({id, bounds, child_clip, Flow(interior, theme.gap), mark, inset, 0});
        const bool live = s.popup_live_at_frame_start(id);
        s.areas.push_back({detail::AreaKind::popup, id, layer, s.stack.size(), s.style_stack.size(),
                           bounds, options.modal, live, s.enter_layer()});
        return AreaScope(state_, generation_, s.areas.size(), bounds);
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Popup allocation failed");
    } catch (const std::length_error&) {
        s.report(ErrorCode::capacity, "Popup exceeds container limits");
    }
    return {};
}

bool Frame::tooltip_due(std::string_view owner_key) {
    if (!state_) { moved_frame(); return false; }
    auto& s = *state_;
    try {
        return s.usable(generation_) && s.tooltip_for_owner(s.keyed_id(owner_key), {});
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Tooltip allocation failed");
    }
    return false;
}

Rect Frame::next_rect(float height) {
    if (!state_) { moved_frame(); return {}; }
    auto& s = *state_;
    if (!s.usable(generation_)) return {};
    auto probe = s.stack.back().flow;
    auto allocated = probe.take(height);
    if (auto* failed = std::get_if<Error>(&allocated)) {
        s.report(failed->code, failed->message.c_str(), true);
        return {};
    }
    return std::get<Rect>(allocated);
}

Rect Frame::viewport() const {
    if (!state_) { moved_frame(); return {}; }
    return state_->viewport;
}

Vec2 Frame::measure_text(std::string_view text, TextStyle style) {
    if (!state_) { moved_frame(); return {}; }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return {};
        if (!std::isfinite(style.size) || style.size < 0) {
            s.report(ErrorCode::invalid_argument, "Text size must be finite and nonnegative", true);
            return {};
        }
        const auto run = s.shape(text, style.role, style.size);
        return run ? Vec2{run->width, run->height} : Vec2{};
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Text measurement allocation failed");
    }
    return {};
}

void Frame::draw_text(Vec2 top_left, std::string_view text, TextStyle style) {
    if (!state_) { moved_frame(); return; }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return;
        if (!std::isfinite(top_left.x) || !std::isfinite(top_left.y) ||
            !std::isfinite(style.size) || style.size < 0 ||
            (style.color && !valid_color(*style.color))) {
            s.report(ErrorCode::invalid_argument, "Text position, size and color must be valid", true);
            return;
        }
        const Color color = style.color ? *style.color : detail::widget_theme(s, {}).text;
        const auto run = s.shape(text, style.role, style.size);
        if (run) s.glyphs(*run, top_left, color);
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Text drawing allocation failed");
    }
}

std::span<const InputEvent> Frame::events() const {
    if (!state_) { moved_frame(); return {}; }
    return state_->interaction.events();
}

std::optional<Vec2> Frame::pointer() const {
    if (!state_) { moved_frame(); return std::nullopt; }
    return state_->interaction.pointer();
}

Vec2 Frame::take_wheel(Rect area) {
    if (!state_) { moved_frame(); return {}; }
    auto& s = *state_;
    if (!s.usable(generation_)) return {};
    return s.take_wheel_steps(area);
}
} // namespace wui

namespace wui::detail {
void FrameState::close_area(std::uint64_t expected, std::size_t depth) noexcept {
    if (!active || generation != expected || !owner_alive || error) return;
    if (areas.size() != depth || stack.size() != areas.back().container_depth ||
        style_stack.size() != areas.back().style_depth) {
        report(ErrorCode::unbalanced_scope, "Area scopes must close in reverse opening order", true);
        return;
    }
    try {
        const AreaRecord record = areas.back();
        switch (record.kind) {
        case AreaKind::region:
            canvas.pop_clip();
            stack.pop_back();
            break;
        case AreaKind::overlay:
            stack.pop_back();
            CanvasAccess::end_layer(canvas, record.layer);
            break;
        case AreaKind::popup: {
            const auto& container = stack.back();
            const float measured = container.flow.used_height() + 2 * container.inset;
            if (!std::isfinite(measured)) {
                report(ErrorCode::invalid_argument, "Measured popup height overflow", true);
                return;
            }
            Rect bounds = container.bounds;
            bounds.h = measured;
            const Rect interior{bounds.x + container.inset, bounds.y + container.inset,
                                std::max(0.0f, bounds.w - 2 * container.inset),
                                std::max(0.0f, bounds.h - 2 * container.inset)};
            const auto mark = container.mark;
            const auto found = widgets.find(record.id.value);
            if (found != widgets.end()) found->second.measured_height = measured;
            canvas.pop_clip();
            CanvasAccess::close_panel(canvas, mark, bounds, interior);
            stack.pop_back();
            CanvasAccess::end_layer(canvas, record.layer);
            interaction.layer_open_ = record.previous_layer_open;
            // Input of this frame belonged to the popup only if it was open
            // when the frame began; the opening frame's input stays below.
            if (record.live && interaction.finish_layer(intersect(bounds, viewport), record.modal) &&
                popup.generic && popup.owner == record.id)
                close_popup(false);
            break;
        }
        }
        areas.pop_back();
        check_canvas();
    } catch (const std::bad_alloc&) {
        report(ErrorCode::capacity, "Area finalization allocation failed");
    } catch (...) {
        report(ErrorCode::invalid_frame, "Area cleanup failed");
    }
}
} // namespace wui::detail
