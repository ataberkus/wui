#include <wui/types.hpp>

#include "float_bits.hpp"

#include <algorithm>
#include <cmath>

namespace wui {
namespace {

bool finite(float value) {
    return std::isfinite(value);
}

bool finite(Vec2 point) {
    return finite(point.x) && finite(point.y);
}

float unit_channel(float value) {
    return finite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

// Finite x, y, w, h, x + w and y + h with nonnegative size. An infinite or
// NaN operand always makes a sum infinite or NaN, so finite edge sums
// already imply finite components. Tested branch-free.
bool valid_extent(Rect rect) {
    const auto nonfinite =
        detail::nonfinite_bits(rect.x + rect.w) | detail::nonfinite_bits(rect.y + rect.h);
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

Rect intersect(Rect a, Rect b) {
    if (!valid_extent(a) || !valid_extent(b)) {
        return {};
    }

    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a.x + a.w, b.x + b.w);
    const float bottom = std::min(a.y + a.h, b.y + b.h);
    return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

bool contains(Rect rect, Vec2 point) {
    if (!valid_extent(rect) || !finite(point)) {
        return false;
    }
    return point.x >= rect.x && point.y >= rect.y &&
           point.x < rect.x + rect.w && point.y < rect.y + rect.h;
}

} // namespace wui
