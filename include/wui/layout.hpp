#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <wui/types.hpp>

namespace wui {

enum class ColumnKind { fixed, share };

struct Column {
    ColumnKind kind;
    float amount;
};

class Flow {
public:
    Flow(Rect area, float gap);
    Result<Rect> take(float height);
    float used_height() const;

private:
    Rect area_;
    float gap_ = 0;
    float used_height_ = 0;
    bool has_rows_ = false;
    bool valid_ = false;
};

Result<std::vector<Rect>> row_cells(
    Rect area, std::span<const Column> columns, float gap);
struct VisibleRows { std::size_t first, last; };
Result<VisibleRows> visible_rows(std::size_t count, float row_height,
                                 float offset, float viewport_height);

} // namespace wui
