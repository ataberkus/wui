#include <wui/types.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wui {
namespace {

bool finite(float value) {
    return std::isfinite(value);
}

// Exactly `isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f`, with the
// common in-range case tested first. NaN fails every comparison and maps to
// zero, as does +infinity; -0.0f passes through unchanged as before.
float unit_channel(float value) {
    if (value >= 0.0f && value <= 1.0f) return value;
    return value > 1.0f && value <= std::numeric_limits<float>::max() ? 1.0f : 0.0f;
}

// An IEEE sum is finite only when both operands are finite (an infinite or
// NaN operand yields an infinite or NaN sum), so the two edge sums alone
// prove all four fields finite. Same result as testing each field as well.
// `right` and `bottom` are rect.x + rect.w and rect.y + rect.h.
bool valid_extent(Rect rect, float right, float bottom) {
    return rect.w >= 0.0f && rect.h >= 0.0f && finite(right) && finite(bottom);
}

} // namespace

Color premultiply(Color color) {
    const float alpha = unit_channel(color.a);
    return {
        unit_channel(color.r) * alpha,
        unit_channel(color.g) * alpha,
        unit_channel(color.b) * alpha,
        alpha,
    };
}

Rect intersect(Rect a, Rect b) {
    const float a_right = a.x + a.w, a_bottom = a.y + a.h;
    const float b_right = b.x + b.w, b_bottom = b.y + b.h;
    if (!valid_extent(a, a_right, a_bottom) || !valid_extent(b, b_right, b_bottom)) {
        return {};
    }

    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a_right, b_right);
    const float bottom = std::min(a_bottom, b_bottom);
    return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

bool contains(Rect rect, Vec2 point) {
    const float right = rect.x + rect.w, bottom = rect.y + rect.h;
    // With finite edges, an infinite or NaN point coordinate fails one of
    // the half-open comparisons below, so it needs no separate test.
    return valid_extent(rect, right, bottom) &&
           point.x >= rect.x && point.y >= rect.y && point.x < right && point.y < bottom;
}

} // namespace wui
