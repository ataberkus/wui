#include <wui/layout.hpp>
#include "flow_detail.hpp"

#include <cmath>
#include <new>
#include <stdexcept>
#include <utility>
#include <limits>

namespace wui {
namespace {

bool valid_area(Rect area) {
    return std::isfinite(area.x) && std::isfinite(area.y) &&
           std::isfinite(area.w) && std::isfinite(area.h) &&
           area.w >= 0.0f && area.h >= 0.0f;
}

Error invalid_layout(const char* message) {
    return {ErrorCode::invalid_argument, message};
}

bool representable_as_float(double value) {
    constexpr double lowest = std::numeric_limits<float>::lowest();
    constexpr double maximum = std::numeric_limits<float>::max();
    return std::isfinite(value) && value >= lowest && value <= maximum;
}

template<class Vector>
Result<std::size_t> append_cells(Rect area, std::span<const Column> columns,
                                 float gap, Vector& out) {
    const auto first = out.size();
    const auto invalid = [&](const char* message) -> Result<std::size_t> {
        out.resize(first);
        return invalid_layout(message);
    };
    try {
        if (!valid_area(area) || !std::isfinite(gap) || gap < 0.0f)
            return invalid("row dimensions and gap must be finite and nonnegative");
        if (columns.empty()) return std::size_t{0};

        double fixed_sum = 0.0, weight_sum = 0.0;
        for (const Column column : columns) {
            if (!std::isfinite(column.amount)) return invalid("column amounts must be finite");
            switch (column.kind) {
            case ColumnKind::fixed:
                if (column.amount < 0.0f)
                    return invalid("fixed column widths must be nonnegative");
                fixed_sum += column.amount;
                break;
            case ColumnKind::share:
                if (column.amount <= 0.0f)
                    return invalid("share weights must be positive");
                weight_sum += column.amount;
                break;
            default:
                return invalid("column kind is invalid");
            }
        }

        const double gap_sum = static_cast<double>(gap) * (columns.size() - 1);
        const double remaining = static_cast<double>(area.w) - fixed_sum - gap_sum;
        if (!std::isfinite(fixed_sum) || !std::isfinite(weight_sum) ||
            !std::isfinite(gap_sum) || remaining < 0.0)
            return invalid("fixed columns and gaps exceed the available width");
        const double bottom = static_cast<double>(area.y) + area.h;
        if (!representable_as_float(bottom))
            return invalid("resolved row geometry overflowed");

        out.reserve(first + columns.size());
        double x = area.x;
        for (const Column column : columns) {
            const double width = column.kind == ColumnKind::fixed
                                     ? static_cast<double>(column.amount)
                                     : remaining * column.amount / weight_sum;
            const double right = x + width;
            if (!representable_as_float(width) || !representable_as_float(x) ||
                !representable_as_float(right))
                return invalid("resolved row geometry overflowed");
            out.push_back({static_cast<float>(x), area.y,
                           static_cast<float>(width), area.h});
            x = right + gap;
        }
        return out.size() - first;
    } catch (const std::bad_alloc&) {
        out.resize(first);
        throw;
    } catch (const std::length_error&) {
        out.resize(first);
        throw;
    }
}

} // namespace

Flow::Flow(Rect area, float gap)
    : area_(area), gap_(gap),
      valid_(valid_area(area) && std::isfinite(gap) && gap >= 0.0f) {}

Result<Rect> Flow::take(float height) {
    if (!valid_ || !std::isfinite(height) || height < 0.0f) {
        return invalid_layout("flow dimensions and gap must be finite and nonnegative");
    }

    const float row_gap = has_rows_ ? gap_ : 0.0f;
    const float y_offset = used_height_ + row_gap;
    const float next_used_height = y_offset + height;
    const float y = area_.y + y_offset;
    if (!std::isfinite(y_offset) || !std::isfinite(next_used_height) ||
        !std::isfinite(y) || !std::isfinite(y + height)) {
        return invalid_layout("flow dimensions overflow");
    }

    used_height_ = next_used_height;
    has_rows_ = true;
    return Rect{area_.x, y, area_.w, height};
}

float Flow::used_height() const {
    return used_height_;
}

Result<std::vector<Rect>> row_cells(
    Rect area, std::span<const Column> columns, float gap) {
    std::vector<Rect> cells;
    auto result = append_cells(area, columns, gap, cells);
    if (auto* error = std::get_if<Error>(&result)) return std::move(*error);
    return cells;
}

namespace detail {
Result<std::size_t> append_row_cells(Rect area, std::span<const Column> columns,
                                     float gap, std::pmr::vector<Rect>& out) {
    return append_cells(area, columns, gap, out);
}
} // namespace detail

Result<VisibleRows> visible_rows(std::size_t count, float row_height,
                                 float offset, float viewport_height) {
    if (!std::isfinite(row_height) || row_height <= 0 ||
        !std::isfinite(offset) || offset < 0 ||
        !std::isfinite(viewport_height) || viewport_height < 0)
        return invalid_layout("Visible row dimensions must be finite and nonnegative");
    const double total = static_cast<double>(count) * row_height;
    if (!std::isfinite(total) || total > std::numeric_limits<float>::max())
        return Error{ErrorCode::capacity, "Visible row content exceeds float range"};
    const double first = std::floor(static_cast<double>(offset) / row_height);
    if (first >= static_cast<double>(count)) return VisibleRows{count, count};
    const double end = std::ceil(
        (static_cast<double>(offset) + viewport_height) / row_height);
    return VisibleRows{static_cast<std::size_t>(first),
        end >= static_cast<double>(count) ? count : static_cast<std::size_t>(end)};
}

} // namespace wui
