#include "../core/context_detail.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui {
PanelScope Frame::panel(std::string_view key, PanelOptions options) {
    if (!state_) {
#ifndef NDEBUG
        std::fputs("wui contract: Frame handle has been moved\n", stderr);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
#endif
        return PanelScope({}, 0, 0);
    }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return PanelScope({}, 0, 0);
        const Id id = s.keyed_id(key);
        if (!s.register_widget(id) || !s.copy_label(options.title)) return PanelScope({}, 0, 0);
        // Probe a copy. The actual parent cursor advances once, on close, by
        // the height measured from children. Application code is never replayed.
        auto probe = s.stack.back().flow;
        auto allocated = probe.take(0);
        if (auto* error = std::get_if<Error>(&allocated)) {
            s.report(error->code, error->message.c_str(), true);
            return PanelScope({}, 0, 0);
        }
        Rect bounds = std::get<Rect>(allocated);
        const float inset = std::min(bounds.w * 0.5f,
            std::max(s.theme.padding, std::min(s.theme.panel_radius, bounds.w * 0.5f)));
        Rect interior{bounds.x + inset, bounds.y + inset,
                      std::max(0.0f, bounds.w - 2 * inset),
                      std::max(0.0f, s.clip().y + s.clip().h - (bounds.y + inset))};
        if (!std::isfinite(interior.x) || !std::isfinite(interior.y) ||
            !std::isfinite(interior.h)) {
            s.report(ErrorCode::invalid_argument, "Panel interior dimensions overflow", true);
            return PanelScope({}, 0, 0);
        }
        const Rect child_clip = intersect(s.clip(), interior);
        const auto mark = detail::CanvasAccess::open_panel(s.canvas, id, options.title,
            bounds, s.theme.panel_radius, s.theme.panel);
        if (!s.check_canvas()) return PanelScope({}, 0, 0);
        s.stack.push_back({id, bounds, child_clip, Flow(interior, s.theme.gap), mark, inset, 0});
        s.canvas.push_clip(child_clip);
        if (!options.title.empty() && s.check_canvas()) {
            // Title belongs to panel semantics, not a duplicate text widget.
            auto run = s.shape(options.title, FontRole::heading);
            if (run) {
                const Rect row = s.take(run->height);
                s.canvas.push_clip(intersect(child_clip, row));
                if (s.check_canvas()) s.glyphs(*run, {row.x, row.y}, s.theme.text);
                s.canvas.pop_clip();
            }
        }
        s.check_canvas();
        return PanelScope(state_, generation_, s.stack.size());
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Panel allocation failed");
    } catch (const std::length_error&) {
        s.report(ErrorCode::capacity, "Panel exceeds container limits");
    }
    return PanelScope({}, 0, 0);
}
} // namespace wui

namespace wui::detail {
void FrameState::close_panel(std::uint64_t expected, std::size_t depth) noexcept {
    // Old scopes from an aborted frame never close a new frame's stack.
    if (!active || generation != expected || !owner_alive) return;
    if (error) return;
    if (stack.size() != depth || depth <= 1) {
        report(ErrorCode::unbalanced_scope, "Panel scopes must close in reverse opening order", true);
        return;
    }
    try {
        auto& panel = stack.back();
        Rect bounds = panel.bounds;
        bounds.h = panel.flow.used_height() + 2 * panel.inset;
        if (!std::isfinite(bounds.h) || !std::isfinite(bounds.y + bounds.h)) {
            report(ErrorCode::invalid_argument, "Measured panel height overflow", true);
            return;
        }
        const Rect interior{bounds.x + panel.inset, bounds.y + panel.inset,
            std::max(0.0f, bounds.w - 2 * panel.inset),
            std::max(0.0f, bounds.h - 2 * panel.inset)};
        const auto mark = panel.mark;
        canvas.pop_clip();
        CanvasAccess::close_panel(canvas, mark, bounds, interior);
        stack.pop_back();
        if (!check_canvas()) return;
        (void)take(bounds.h);
    } catch (const std::bad_alloc&) {
        report(ErrorCode::capacity, "Panel finalization allocation failed");
    } catch (const std::length_error&) {
        report(ErrorCode::capacity, "Panel finalization exceeds container limits");
    } catch (...) {
        // Stack unwinding cannot throw; an unexpected cleanup failure still
        // invalidates the frame rather than submitting a partial packet.
        report(ErrorCode::invalid_frame, "Panel cleanup failed");
    }
}
} // namespace wui::detail
