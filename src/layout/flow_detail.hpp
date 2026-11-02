#pragma once

#include <wui/layout.hpp>

#include <cmath>
#include <cstddef>
#include <memory_resource>

namespace wui::detail {
Result<std::size_t> append_row_cells(Rect area, std::span<const Column> columns,
                                     float gap, std::pmr::vector<Rect>& out);

struct FlowAccess {
    // Flow::take without building a Result<Rect>: stores the row and returns
    // true exactly when Flow::take would succeed; otherwise returns false and
    // leaves the flow unchanged, so Flow::take then reports the same error.
    // Inline: it runs once per widget.
    static bool take(Flow& flow, float height, Rect& out) noexcept {
        // NaN and negative heights stop here; an infinite one fails the sum test.
        if (!flow.valid_ || !(height >= 0.0f)) return false;

        const float row_gap = flow.has_rows_ ? flow.gap_ : 0.0f;
        const float y_offset = flow.used_height_ + row_gap;
        const float next_used_height = y_offset + height;
        const float y = flow.area_.y + y_offset;
        // An IEEE sum is finite only when both operands are, so these two tests
        // also prove y_offset, y and height finite.
        if (!std::isfinite(next_used_height) || !std::isfinite(y + height)) return false;

        flow.used_height_ = next_used_height;
        flow.has_rows_ = true;
        out = Rect{flow.area_.x, y, flow.area_.w, height};
        return true;
    }
    // Flow::used_height, inline.
    static float used_height(const Flow& flow) noexcept { return flow.used_height_; }
};
}
