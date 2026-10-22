#include "../core/context_detail.hpp"
#include "../layout/flow_detail.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui {
RowScope Frame::row(std::string_view key, std::span<const Column> columns) {
    if (!state_) {
#ifndef NDEBUG
        std::fputs("wui contract: Frame handle has been moved\n", stderr);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
#endif
        return RowScope({}, 0, 0);
    }
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return RowScope({}, 0, 0);
        const auto id = s.keyed_id(key);
        if (!s.register_widget(id)) return RowScope({}, 0, 0);
        auto probe = s.stack.back().flow;
        auto allocated = probe.take(0);
        if (auto* error = std::get_if<Error>(&allocated)) {
            s.report(error->code, error->message.c_str(), true);
            return RowScope({}, 0, 0);
        }
        auto area = std::get<Rect>(allocated);
        const auto clip = s.clip();
        area.h = std::max(0.0f, clip.y + clip.h - area.y);
        if (!std::isfinite(area.h)) {
            s.report(ErrorCode::invalid_argument, "Row viewport dimensions overflow", true);
            return RowScope({}, 0, 0);
        }
        const auto first = s.row_rects.size();
        auto count = detail::append_row_cells(area, columns, s.theme.gap, s.row_rects);
        if (auto* error = std::get_if<Error>(&count)) {
            s.report(error->code, error->message.c_str(), true);
            return RowScope({}, 0, 0);
        }
        const auto row_index = s.rows.size();
        s.rows.push_back({id, first, std::get<std::size_t>(count), 0, s.stack.size()});
        return RowScope(state_, generation_, row_index);
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Row allocation failed");
    } catch (const std::length_error&) {
        s.report(ErrorCode::capacity, "Row exceeds container limits");
    }
    return RowScope({}, 0, 0);
}

float Frame::available_width() const {
    if (!state_) {
#ifndef NDEBUG
        std::fputs("wui contract: Frame handle has been moved\n", stderr);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
#endif
        return 0;
    }
    auto& s = *state_;
    if (!s.usable(generation_)) return 0;
    auto probe = s.stack.back().flow;
    auto allocated = probe.take(0);
    if (auto* error = std::get_if<Error>(&allocated)) {
        s.report(error->code, error->message.c_str(), true);
        return 0;
    }
    return std::get<Rect>(allocated).w;
}

CellScope RowScope::cell() {
    if (!state_) return CellScope({}, 0, 0, 0);
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) return CellScope({}, 0, 0, 0);
        if (s.rows.size() != row_index_ + 1 ||
            s.stack.size() != s.rows[row_index_].parent_depth ||
            s.rows[row_index_].cell_open ||
            s.rows[row_index_].next >= s.rows[row_index_].count) {
            s.report(ErrorCode::unbalanced_scope, "Row cells must open one at a time within declared columns", true);
            return CellScope({}, 0, 0, 0);
        }
        auto& row = s.rows[row_index_];
        const std::uint64_t index = row.next;
        const auto id = child_id(child_id(row.id, "cell"),
            {reinterpret_cast<const char*>(&index), sizeof(index)});
        const auto bounds = s.row_rects[row.first + row.next];
        const auto clip = intersect(s.clip(), bounds);
        s.stack.push_back({id, bounds, clip, Flow(bounds, s.theme.gap), {}, 0, 0});
        s.canvas.push_clip(clip);
        if (!s.check_canvas()) return CellScope({}, 0, 0, 0);
        ++row.next;
        row.cell_open = true;
        return CellScope(state_, generation_, row_index_, s.stack.size());
    } catch (const std::bad_alloc&) {
        s.report(ErrorCode::capacity, "Row cell allocation failed");
    } catch (const std::length_error&) {
        s.report(ErrorCode::capacity, "Row cell exceeds container limits");
    }
    return CellScope({}, 0, 0, 0);
}
} // namespace wui

namespace wui::detail {
void FrameState::close_cell(std::uint64_t expected, std::size_t row_index,
                            std::size_t depth) noexcept {
    if (!active || generation != expected || !owner_alive || error) return;
    if (rows.size() != row_index + 1 || !rows[row_index].cell_open ||
        stack.size() != depth || depth != rows[row_index].parent_depth + 1) {
        report(ErrorCode::unbalanced_scope, "Cell scopes must close in reverse opening order", true);
        return;
    }
    rows[row_index].tallest = std::max(rows[row_index].tallest, stack.back().flow.used_height());
    canvas.pop_clip();
    stack.pop_back();
    rows[row_index].cell_open = false;
    check_canvas();
}

void FrameState::close_row(std::uint64_t expected, std::size_t row_index) noexcept {
    if (!active || generation != expected || !owner_alive || error) return;
    if (rows.size() != row_index + 1 || rows[row_index].cell_open ||
        stack.size() != rows[row_index].parent_depth) {
        report(ErrorCode::unbalanced_scope, "Row scopes must close after their cells", true);
        return;
    }
    const auto height = rows.back().tallest;
    rows.pop_back();
    (void)take(height);
}
} // namespace wui::detail
