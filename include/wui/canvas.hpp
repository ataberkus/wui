#pragma once

#include <wui/draw.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace wui {

namespace detail { struct CanvasAccess; }

class Canvas {
public:
    explicit Canvas(std::shared_ptr<TextureStore> textures,
                    std::size_t max_packet_slots = 4);
    ~Canvas();

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    void begin(Rect viewport, float scale);
    void push_clip(Rect clip);
    void pop_clip();
    void rounded_rect(Rect bounds, float radius, Color color);
    void gradient(Rect bounds, Color top, Color bottom);
    // Four-corner fill: colors are evaluated bilinearly on a 9x9 vertex grid
    // and interpolated linearly inside its cells (exact on the edges and for
    // gradients that vary along one axis).
    void gradient(Rect bounds, Color top_left, Color top_right,
                  Color bottom_right, Color bottom_left);
    void border(Rect bounds, float radius, float width, Color color);
    void convex(std::span<const Vec2> points, Color color);
    // Simple (non-self-intersecting) polygon, convex or concave, either
    // winding, 3..512 points; a repeated closing point is ignored. Edges get
    // the same one-physical-pixel coverage fringe as convex(). Self-intersecting
    // or degenerate outlines are invalid_argument.
    void polygon(std::span<const Vec2> points, Color color);
    // Round joins/caps; overlapping segments contribute one union coverage layer.
    // Duplicate points are accepted, including a point-only stroke. Complex
    // outlines use a 4x4-sampled physical-pixel coverage texture clipped to the
    // active surface. Per call: <=4096 input points, fallback <=2048 per axis,
    // <=1,048,576 pixels and <=67,108,864 sample/segment evaluations. Exceeding
    // these bounds or the TextureStore budget makes finish() return capacity.
    void line(std::span<const Vec2> points, float width, Color color);
    void image(Rect bounds, TextureHandle texture, Rect uv, Color tint);
    // sigma is logical; sigma*scale must be in [0.5,32] physical pixels.
    // Cache identity and rasterization use exact physical radius and sigma.
    // At most 32 entries; only unleased entries may be evicted. Physical radius
    // is <=2047 and texture axes <=4096; exceeding limits returns capacity.
    void shadow(Rect bounds, float radius, float sigma, Color color);
    void semantic(Id id, Role role, std::string_view label, Rect bounds,
                  bool enabled, bool focused, bool selected,
                  std::string_view value = {}, bool invalid = false);
    Result<DrawPacket> finish();

private:
    friend struct detail::CanvasAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wui
