#pragma once

// Shared clip-to-scissor conversion for every renderer, so all backends
// select exactly the same physical pixels for a batch clip.

#include <wui/draw.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wui::detail {

struct PixelRect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const noexcept { return left >= right || top >= bottom; }
};

// Logical coordinates are floats, so a clip edge that lands exactly on a
// physical pixel boundary can come out a few ulps short or long of it
// (49.6f * 1.25 = 61.9999981). Edges within kSnap of a boundary snap to
// it before the outward floor/ceil, so precision noise never adds a row
// or column of anti-aliasing fringe.
inline constexpr double kScissorSnap = 1.0 / 1024.0;

inline PixelRect physical_scissor(const Rect& clip, const Rect& viewport, float scale,
                                  std::uint32_t width, std::uint32_t height) noexcept {
    const auto edge = [&](double logical, double origin, bool upper, std::uint32_t limit) {
        const double physical = (logical - origin) * static_cast<double>(scale);
        const double snapped = upper ? std::ceil(physical - kScissorSnap) : std::floor(physical + kScissorSnap);
        return static_cast<std::int32_t>(std::clamp(snapped, 0.0, static_cast<double>(limit)));
    };
    const double x = clip.x, y = clip.y;
    return PixelRect{
        edge(x, viewport.x, false, width),
        edge(y, viewport.y, false, height),
        edge(x + static_cast<double>(clip.w), viewport.x, true, width),
        edge(y + static_cast<double>(clip.h), viewport.y, true, height),
    };
}

}  // namespace wui::detail
