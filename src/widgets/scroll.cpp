#include "../core/context_detail.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui {
ScrollScope Frame::scroll(std::string_view key, float viewport_height) {
    if (!state_) {
#ifndef NDEBUG
        std::fputs("wui contract: Frame handle has been moved\n", stderr);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
#endif
        return ScrollScope({}, 0, 0, {}, 0, {});
    }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return ScrollScope({}, 0, 0, {}, 0, {});
        if (!std::isfinite(viewport_height) || viewport_height < 0) {
            s.report(ErrorCode::invalid_argument, "Scroll height must be finite and nonnegative", true);
            return ScrollScope({}, 0, 0, {}, 0, {});
        }
        const Id id = s.keyed_id(key);
        auto* widget = s.register_widget(id);
        if (!widget) return ScrollScope({}, 0, 0, {}, 0, {});
        auto probe = s.stack.back().flow;
        auto allocated = probe.take(viewport_height);
        if (auto* error = std::get_if<Error>(&allocated)) {
            s.report(error->code, error->message.c_str(), true);
            return ScrollScope({}, 0, 0, {}, 0, {});
        }
        const Rect viewport = std::get<Rect>(allocated);
        const float offset = widget->scroll_offset;
        const double origin = static_cast<double>(viewport.y) - offset;
        if (!std::isfinite(origin) ||
            origin < std::numeric_limits<float>::lowest() ||
            origin > std::numeric_limits<float>::max()) {
            s.report(ErrorCode::invalid_argument, "Scroll content origin overflow", true);
            return ScrollScope({}, 0, 0, {}, 0, {});
        }
        const Rect content{viewport.x, static_cast<float>(origin), viewport.w, viewport.h};
        const Rect clip = intersect(s.clip(), viewport);
        s.stack.push_back({id, viewport, clip, Flow(content, s.theme.gap), {}, 0, 0});
        s.canvas.push_clip(clip);
        if (!s.check_canvas()) return ScrollScope({}, 0, 0, {}, 0, {});
        return ScrollScope(state_, generation_, s.stack.size(), id, offset, viewport);
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Scroll allocation failed");
    } catch (const std::length_error&) {
        s.report(ErrorCode::capacity, "Scroll exceeds container limits");
    }
    return ScrollScope({}, 0, 0, {}, 0, {});
}

void Frame::spacer(float height) {
    if (!state_) {
#ifndef NDEBUG
        std::fputs("wui contract: Frame handle has been moved\n", stderr);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
#endif
        return;
    }
    auto& s = *state_;
    if (!s.usable(generation_)) return;
    if (!std::isfinite(height) || height < 0) {
        s.report(ErrorCode::invalid_argument, "Spacer height must be finite and nonnegative", true);
        return;
    }
    (void)s.take(height);
}
} // namespace wui

namespace wui::detail {
float FrameState::settle_scroll(Rect visible_viewport, float current_offset, float max_offset) {
    return interaction.take_wheel(visible_viewport, current_offset, max_offset);
}

void FrameState::close_scroll(std::uint64_t expected, std::size_t depth, Id id,
                              Rect viewport, float current_offset) noexcept {
    if (!active || generation != expected || !owner_alive || error) return;
    if (stack.size() != depth || depth <= 1 || stack.back().id != id) {
        report(ErrorCode::unbalanced_scope, "Scroll scopes must close in reverse opening order", true);
        return;
    }
    const float content_height = stack.back().flow.used_height();
    const Rect visible_viewport = stack.back().clip;
    canvas.pop_clip();
    stack.pop_back();
    if (!check_canvas()) return;
    (void)take(viewport.h);
    if (error) return;
    const float max_offset = std::max(0.0f, content_height - viewport.h);
    widgets.find(id.value)->second.scroll_offset =
        settle_scroll(visible_viewport, current_offset, max_offset);
}
} // namespace wui::detail
