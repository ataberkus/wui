#include <wui/types.hpp>

#include "float_bits.hpp"
#include "rect_ops.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wui {
namespace {

// Exactly `isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f`, with the
// common in-range case tested first. NaN fails every comparison and maps to
// zero, as does +infinity; -0.0f passes through unchanged as before.
float unit_channel(float value) {
    if (value >= 0.0f && value <= 1.0f) return value;
    return value > 1.0f && value <= std::numeric_limits<float>::max() ? 1.0f : 0.0f;
}

// Finite x, y, w, h, x + w and y + h with nonnegative size. An infinite or
// NaN operand always makes a sum infinite or NaN, so finite edge sums
// already imply finite components. Tested branch-free. `right` and `bottom`
// are rect.x + rect.w and rect.y + rect.h, computed once by the caller.
bool valid_extent(Rect rect, float right, float bottom) {
    const auto nonfinite = detail::nonfinite_bits(right) | detail::nonfinite_bits(bottom);
    return ((nonfinite & detail::nonfinite_flag) == 0) & (rect.w >= 0.0f) & (rect.h >= 0.0f);
}

} // namespace

Color premultiply(Color color) {
    // Fast path: every channel already in [0, 1] (false for NaN and the
    // infinities), where unit_channel is the identity. Validated canvas
    // colors always take it.
    if ((color.r >= 0.0f) & (color.r <= 1.0f) & (color.g >= 0.0f) & (color.g <= 1.0f) &
        (color.b >= 0.0f) & (color.b <= 1.0f) & (color.a >= 0.0f) & (color.a <= 1.0f)) {
        return {color.r * color.a, color.g * color.a, color.b * color.a, color.a};
    }
    const float alpha = unit_channel(color.a);
    return {
        unit_channel(color.r) * alpha,
        unit_channel(color.g) * alpha,
        unit_channel(color.b) * alpha,
        alpha,
    };
}

Rect intersect(Rect a, Rect b) { return detail::intersect_rects(a, b); }

bool contains(Rect rect, Vec2 point) {
    const float right = rect.x + rect.w, bottom = rect.y + rect.h;
    // With finite edges, an infinite or NaN point coordinate fails one of
    // the half-open comparisons below, so it needs no separate test.
    return valid_extent(rect, right, bottom) &&
           point.x >= rect.x && point.y >= rect.y && point.x < right && point.y < bottom;
}

} // namespace wui
