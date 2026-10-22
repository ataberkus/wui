#include "painter.hpp"

#include <cassert>
#include <cstdio>
#include <cmath>
#include <new>
#include <optional>
#include <stdexcept>

namespace wui {
namespace {
Error moved_frame() {
#ifndef NDEBUG
    std::fputs("wui contract: Frame handle has been moved\n", stderr);
    std::fflush(stderr);
    assert(false && "wui programmer contract violation");
#endif
    return Error{ErrorCode::invalid_frame, "Frame handle has been moved"};
}

// Flow items pass no explicit rectangle; positioned items never advance flow.
Result<CustomItem> emit_custom(detail::FrameState& frame, std::string_view key,
                               std::string_view label, float height,
                               std::optional<Rect> placed, WidgetOptions options) {
    options = frame.effective(options);
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return *frame.error;
    const Rect bounds = placed ? *placed : frame.take(height);
    if (frame.error || !frame.check_canvas()) return *frame.error;
    const Rect clip = intersect(bounds, frame.clip());
    const Behavior behavior = frame.interaction.button(id, clip,
                                                         options.enabled && detail::visible(clip));
    detail::track_feedback(*state, clip, behavior, options.enabled);
    frame.canvas.semantic(id, Role::button, label, bounds, options.enabled,
                          behavior.focused, false, {}, options.invalid);
    if (!frame.check_canvas()) return *frame.error;
    return CustomItem{id, bounds, clip, behavior, options.enabled};
}
} // namespace

Result<CustomItem> Frame::custom(std::string_view key, std::string_view label,
                                 float height, WidgetOptions options) {
    if (!state_) return moved_frame();
    auto& frame = *state_;
    try {
        if (!frame.usable(generation_)) return *frame.error;
        if (!std::isfinite(height) || height < 0) {
            frame.report(ErrorCode::invalid_argument, "Custom item height must be finite and nonnegative", true);
            return *frame.error;
        }
        return emit_custom(frame, key, label, height, std::nullopt, options);
    } catch (const std::bad_alloc&) {
        frame.report(ErrorCode::capacity, "Custom item allocation failed");
    } catch (const std::length_error&) {
        frame.report(ErrorCode::capacity, "Custom item exceeds container limits");
    }
    return *frame.error;
}

Result<CustomItem> Frame::custom_at(std::string_view key, std::string_view label,
                                    Rect bounds, WidgetOptions options) {
    if (!state_) return moved_frame();
    auto& frame = *state_;
    try {
        if (!frame.usable(generation_)) return *frame.error;
        if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.w) ||
            !std::isfinite(bounds.h) || bounds.w < 0 || bounds.h < 0) {
            frame.report(ErrorCode::invalid_argument, "Custom item bounds must be finite and nonnegative", true);
            return *frame.error;
        }
        return emit_custom(frame, key, label, 0, bounds, options);
    } catch (const std::bad_alloc&) {
        frame.report(ErrorCode::capacity, "Custom item allocation failed");
    } catch (const std::length_error&) {
        frame.report(ErrorCode::capacity, "Custom item exceeds container limits");
    }
    return *frame.error;
}

} // namespace wui
