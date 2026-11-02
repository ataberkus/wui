#pragma once

#include <wui/layout.hpp>

#include <cstddef>
#include <memory_resource>

namespace wui::detail {
Result<std::size_t> append_row_cells(Rect area, std::span<const Column> columns,
                                     float gap, std::pmr::vector<Rect>& out);

struct FlowAccess {
    // Flow::take without building a Result<Rect>: stores the row and returns
    // true exactly when Flow::take would succeed; otherwise returns false and
    // leaves the flow unchanged, so Flow::take then reports the same error.
    static bool take(Flow& flow, float height, Rect& out) noexcept;
};
}
