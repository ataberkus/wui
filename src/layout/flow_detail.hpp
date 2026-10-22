#pragma once

#include <wui/layout.hpp>

#include <cstddef>
#include <memory_resource>

namespace wui::detail {
Result<std::size_t> append_row_cells(Rect area, std::span<const Column> columns,
                                     float gap, std::pmr::vector<Rect>& out);
}
