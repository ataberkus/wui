#pragma once

#include <wui/types.hpp>

#include "float_bits.hpp"

#include <algorithm>

namespace wui::detail {

// wui::intersect, inline for hot widget and core paths (types.cpp defines
// the public function with it). Invalid input (a nonfinite edge or a negative
// size in either rectangle) yields the empty rect at the origin; otherwise the
// overlap, with a nonnegative size. An infinite or NaN operand always makes an
// edge sum infinite or NaN, so finite edge sums already imply finite
// components: the four sums are tested together first, and the four then
// finite sizes are all nonnegative exactly when their minimum is.
inline Rect intersect_rects(Rect a, Rect b) noexcept {
    const float a_right = a.x + a.w, a_bottom = a.y + a.h;
    const float b_right = b.x + b.w, b_bottom = b.y + b.h;
    const auto nonfinite = nonfinite_bits(a_right) | nonfinite_bits(a_bottom) |
                           nonfinite_bits(b_right) | nonfinite_bits(b_bottom);
    if ((nonfinite & nonfinite_flag) != 0) return {};
    if (!(std::min(std::min(a.w, a.h), std::min(b.w, b.h)) >= 0.0f)) return {};
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a_right, b_right);
    const float bottom = std::min(a_bottom, b_bottom);
    return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

} // namespace wui::detail
