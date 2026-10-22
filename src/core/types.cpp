#include <wui/types.hpp>

#include <algorithm>
#include <cmath>

namespace wui {
namespace {

bool finite(float value) {
    return std::isfinite(value);
}

bool finite(Rect rect) {
    return finite(rect.x) && finite(rect.y) && finite(rect.w) && finite(rect.h);
}

bool finite(Vec2 point) {
    return finite(point.x) && finite(point.y);
}

float unit_channel(float value) {
    return finite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

bool valid_extent(Rect rect) {
    return finite(rect) && rect.w >= 0.0f && rect.h >= 0.0f &&
           finite(rect.x + rect.w) && finite(rect.y + rect.h);
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
