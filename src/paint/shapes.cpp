#include <wui/canvas.hpp>

#include "canvas_access.hpp"
#include "shapes_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wui {
namespace {

constexpr float epsilon = 1.0e-6f;

bool finite(float value) { return std::isfinite(value); }
bool finite(Vec2 value) { return finite(value.x) && finite(value.y); }
bool finite(Rect value) {
    return finite(value.x) && finite(value.y) && finite(value.w) && finite(value.h) &&
           finite(value.x + value.w) && finite(value.y + value.h);
}
bool finite(Color value) {
    return finite(value.r) && finite(value.g) && finite(value.b) && finite(value.a);
}
bool valid_color(Color value) {
    return finite(value) && value.r >= 0.0f && value.r <= 1.0f &&
           value.g >= 0.0f && value.g <= 1.0f && value.b >= 0.0f &&
           value.b <= 1.0f && value.a >= 0.0f && value.a <= 1.0f;
}
bool valid_rect(Rect value) {
    return finite(value) && value.w >= 0.0f && value.h >= 0.0f;
}
bool equal(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 subtract(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 multiply(Vec2 value, float scalar) { return {value.x * scalar, value.y * scalar}; }
float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
float length(Vec2 value) { return std::hypot(value.x, value.y); }
Vec2 normalized(Vec2 value) {
    const auto magnitude = length(value);
    return magnitude > epsilon ? multiply(value, 1.0f / magnitude) : Vec2{};
}
bool near(Vec2 a, Vec2 b) {
    return length(subtract(a, b)) <= epsilon;
}

void append_unique(std::vector<Vec2>& points, Vec2 point) {
    if (points.empty() || !near(points.back(), point)) {
        points.push_back(point);
    }
}

void remove_duplicate_closure(std::vector<Vec2>& points) {
    if (points.size() > 1 && near(points.front(), points.back())) {
        points.pop_back();
    }
}

Error invalid_argument(const char* message) {
    return {ErrorCode::invalid_argument, message};
}

struct ShadowKey {
    // Both are finite validated physical values; exact float equality is
    // stable for repeated requests and does not quantize caller geometry.
    float radius_px = 0;
    float sigma_px = 0;
    bool operator==(const ShadowKey&) const = default;
};

struct ShadowEntry {
    ShadowKey key;
    TextureHandle handle;
    std::uint32_t texture_size = 0;
    std::uint32_t border_px = 0;
    std::uint64_t stamp = 0;
};

struct PendingBatch {
    TextureHandle texture;
};

void rounded_outline(Rect bounds, float radius, float scale, std::vector<Vec2>& out) {
    out.clear();
    radius = std::clamp(radius, 0.0f, 0.5f * std::min(bounds.w, bounds.h));
    if (radius <= epsilon) {
        out = {{bounds.x, bounds.y}, {bounds.x + bounds.w, bounds.y},
               {bounds.x + bounds.w, bounds.y + bounds.h},
               {bounds.x, bounds.y + bounds.h}};
        return;
    }

    const float physical_radius = radius * scale;
    const float ratio = std::clamp(1.0f - 0.25f / physical_radius, -1.0f, 1.0f);
    const float maximum_step = 2.0f * std::acos(ratio);
    const int segments = std::clamp(
        static_cast<int>(std::ceil((std::numbers::pi_v<float> * 0.5f) /
                                   std::max(maximum_step, epsilon))),
        3, 32);
    out.reserve(static_cast<std::size_t>((segments + 1) * 4));
    const std::array centers{
        Vec2{bounds.x + radius, bounds.y + radius},
        Vec2{bounds.x + bounds.w - radius, bounds.y + radius},
        Vec2{bounds.x + bounds.w - radius, bounds.y + bounds.h - radius},
        Vec2{bounds.x + radius, bounds.y + bounds.h - radius},
    };
    constexpr std::array starts{
        std::numbers::pi_v<float>,
        std::numbers::pi_v<float> * 1.5f,
        0.0f,
        std::numbers::pi_v<float> * 0.5f,
    };
    for (std::size_t corner = 0; corner < centers.size(); ++corner) {
        // Exact cardinal endpoints prevent near-duplicate edges when adjacent
        // corner centers coincide (circles and capsules).
        constexpr std::array cardinal{Vec2{-1, 0}, Vec2{0, -1},
                                       Vec2{1, 0}, Vec2{0, 1}};
        for (int segment = 0; segment <= segments; ++segment) {
            if (segment == 0 || segment == segments) {
                const auto axis = cardinal[(corner + (segment == segments)) % 4];
                append_unique(out, add(centers[corner], multiply(axis, radius)));
                continue;
            }
            const float angle = starts[corner] +
                                (std::numbers::pi_v<float> * 0.5f) *
                                    static_cast<float>(segment) /
                                    static_cast<float>(segments);
            append_unique(out,
                          {centers[corner].x + std::cos(angle) * radius,
                           centers[corner].y + std::sin(angle) * radius});
        }
    }
    remove_duplicate_closure(out);
}

double signed_area(std::span<const Vec2> points) {
    if (points.size() < 3) {
        return 0;
    }
    double area = 0.0;
    const auto origin = points.front();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto j = (i + 1) % points.size();
        area += (static_cast<double>(points[i].x) - origin.x) *
                    (static_cast<double>(points[j].y) - origin.y) -
                (static_cast<double>(points[i].y) - origin.y) *
                    (static_cast<double>(points[j].x) - origin.x);
    }
    return area * 0.5f;
}

bool is_convex(std::span<const Vec2> points) {
    float direction = 0.0f;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto turn = cross(subtract(points[(i + 1) % points.size()], points[i]),
                                subtract(points[(i + 2) % points.size()],
                                         points[(i + 1) % points.size()]));
        if (std::abs(turn) <= epsilon) {
            continue;
        }
        if (direction == 0.0f) {
            direction = turn;
        } else if ((direction > 0.0f) != (turn > 0.0f)) {
            return false;
        }
    }
    return direction != 0.0f;
}

double triangle_area(Vec2 a, Vec2 b, Vec2 c) {
    return (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
           (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
}

bool point_in_triangle(Vec2 point, Vec2 a, Vec2 b, Vec2 c, float orientation) {
    const double ab = triangle_area(a, b, point) * orientation;
    const double bc = triangle_area(b, c, point) * orientation;
    const double ca = triangle_area(c, a, point) * orientation;
    return ab >= -epsilon && bc >= -epsilon && ca >= -epsilon;
}

void triangulate_simple(std::span<const Vec2> points,
                        std::vector<std::uint32_t>& remaining,
                        std::vector<std::uint32_t>& out) {
    out.clear();
    const double area = signed_area(points);
    if (points.size() < 3 || !std::isfinite(area) || std::abs(area) <= epsilon) {
        return;
    }
    remaining.resize(points.size());
    for (std::uint32_t i = 0; i < remaining.size(); ++i) {
        remaining[i] = i;
    }
    out.reserve((points.size() - 2) * 3);
    const float orientation = area > 0.0 ? 1.0f : -1.0f;
    while (remaining.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const auto previous = remaining[(i + remaining.size() - 1) % remaining.size()];
            const auto current = remaining[i];
            const auto next = remaining[(i + 1) % remaining.size()];
            if (triangle_area(points[previous], points[current], points[next]) *
                    orientation <= epsilon) {
                continue;
            }
            bool contains_other = false;
            for (const auto candidate : remaining) {
                if (candidate != previous && candidate != current && candidate != next &&
                    point_in_triangle(points[candidate], points[previous], points[current],
                                      points[next], orientation)) {
                    contains_other = true;
                    break;
                }
            }
            if (contains_other) {
                continue;
            }
            out.insert(out.end(), {previous, current, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            out.clear();
            return;
        }
    }
    if (triangle_area(points[remaining[0]], points[remaining[1]], points[remaining[2]]) *
            orientation <= epsilon) {
        out.clear();
        return;
    }
    out.insert(out.end(), {remaining[0], remaining[1], remaining[2]});
}

Vec2 offset_intersection(Vec2 point, Vec2 previous_direction, Vec2 next_direction,
                         Vec2 previous_normal, Vec2 next_normal, float radius) {
    const auto first = add(point, multiply(previous_normal, radius));
    const auto second = add(point, multiply(next_normal, radius));
    const float denominator = cross(previous_direction, next_direction);
    if (std::abs(denominator) <= epsilon) {
        return second;
    }
    const float distance = cross(subtract(second, first), next_direction) / denominator;
    auto offset = subtract(add(first, multiply(previous_direction, distance)), point);
    const float magnitude = length(offset);
    if (magnitude > radius * 2.0f) {
        offset = multiply(offset, radius * 2.0f / magnitude);
    }
    return add(point, offset);
}

void append_arc(std::vector<Vec2>& output, Vec2 center, Vec2 from, float delta,
                float scale) {
    const float radius = length(from);
    const float physical_radius = radius * scale;
    float maximum_step = std::numbers::pi_v<float> * 0.5f;
    if (physical_radius > 0.25f) {
        maximum_step = 2.0f * std::acos(
            std::clamp(1.0f - 0.25f / physical_radius, -1.0f, 1.0f));
    }
    const int segments = std::clamp(
        static_cast<int>(std::ceil(std::abs(delta) /
                                   std::max(maximum_step, epsilon))),
        1, 32);
    append_unique(output, add(center, from));
    const float start = std::atan2(from.y, from.x);
    for (int i = 1; i <= segments; ++i) {
        const float angle = start + delta * static_cast<float>(i) /
                                        static_cast<float>(segments);
        append_unique(output, {center.x + std::cos(angle) * radius,
                               center.y + std::sin(angle) * radius});
    }
}

void stroke_outline(std::span<const Vec2> points, float radius, float scale,
                    std::vector<Vec2>& directions, std::vector<Vec2>& normals,
                    std::vector<Vec2>& plus, std::vector<Vec2>& minus,
                    std::vector<Vec2>& out) {
    const std::size_t segment_count = points.size() - 1;
    directions.resize(segment_count);
    normals.resize(segment_count);
    for (std::size_t i = 0; i < segment_count; ++i) {
        directions[i] = normalized(subtract(points[i + 1], points[i]));
        normals[i] = {-directions[i].y, directions[i].x};
    }

    plus.clear();
    minus.clear();
    append_unique(plus, add(points.front(), multiply(normals.front(), radius)));
    append_unique(minus, subtract(points.front(), multiply(normals.front(), radius)));
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const float turn = cross(directions[i - 1], directions[i]);
        if (std::abs(turn) <= epsilon) {
            append_unique(plus, add(points[i], multiply(normals[i], radius)));
            append_unique(minus, subtract(points[i], multiply(normals[i], radius)));
            continue;
        }
        if (turn < 0.0f) {
            append_arc(plus, points[i], multiply(normals[i - 1], radius),
                       std::atan2(turn, dot(directions[i - 1], directions[i])), scale);
            append_unique(minus,
                          offset_intersection(points[i], directions[i - 1], directions[i],
                                              multiply(normals[i - 1], -1.0f),
                                              multiply(normals[i], -1.0f), radius));
        } else {
            append_unique(plus,
                          offset_intersection(points[i], directions[i - 1], directions[i],
                                              normals[i - 1], normals[i], radius));
            append_arc(minus, points[i], multiply(normals[i - 1], -radius),
                       std::atan2(turn, dot(directions[i - 1], directions[i])), scale);
        }
    }
    append_unique(plus, add(points.back(), multiply(normals.back(), radius)));
    append_unique(minus, subtract(points.back(), multiply(normals.back(), radius)));

    out = plus;
    append_arc(out, points.back(), subtract(plus.back(), points.back()),
               -std::numbers::pi_v<float>, scale);
    for (std::size_t i = minus.size(); i-- > 1;) {
        append_unique(out, minus[i - 1]);
    }
    append_arc(out, points.front(), subtract(minus.front(), points.front()),
               -std::numbers::pi_v<float>, scale);
    out.back() = plus.front(); // The final semicircle ends exactly at the start.
    remove_duplicate_closure(out);
}

bool simple_outline(std::span<const Vec2> points) {
    if (points.size() < 3 || points.size() > detail::stroke_mesh_point_limit ||
        !std::all_of(points.begin(), points.end(), [](Vec2 p) { return finite(p); }) ||
        std::abs(signed_area(points)) <= epsilon) {
        return false;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto next = (i + 1) % points.size();
        const auto a = points[i];
        const auto b = points[next];
        if (near(a, b)) {
            return false;
        }
        for (std::size_t j = i + 1; j < points.size(); ++j) {
            const auto after = (j + 1) % points.size();
            if (j == next || after == i) {
                continue;
            }
            const auto c = points[j];
            const auto d = points[after];
            if (std::max(a.x, b.x) < std::min(c.x, d.x) ||
                std::max(c.x, d.x) < std::min(a.x, b.x) ||
                std::max(a.y, b.y) < std::min(c.y, d.y) ||
                std::max(c.y, d.y) < std::min(a.y, b.y)) {
                continue;
            }
            const double ab_c = triangle_area(a, b, c);
            const double ab_d = triangle_area(a, b, d);
            const double cd_a = triangle_area(c, d, a);
            const double cd_b = triangle_area(c, d, b);
            // Include tangencies and collinear overlap: both can duplicate
            // coverage in a nominally triangulatable outline or AA ring.
            if (((ab_c <= 0 && ab_d >= 0) || (ab_c >= 0 && ab_d <= 0)) &&
                ((cd_a <= 0 && cd_b >= 0) || (cd_a >= 0 && cd_b <= 0))) {
                return false;
            }
        }
    }
    return true;
}

void fringe_outline(std::span<const Vec2> points, float fringe, std::vector<Vec2>& out) {
    const float orientation = signed_area(points) > 0 ? 1.0f : -1.0f;
    out.clear();
    out.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto previous = points[(i + points.size() - 1) % points.size()];
        const auto current = points[i];
        const auto next = points[(i + 1) % points.size()];
        const auto previous_edge = normalized(subtract(current, previous));
        const auto next_edge = normalized(subtract(next, current));
        const auto previous_normal = multiply(
            Vec2{previous_edge.y, -previous_edge.x}, orientation);
        const auto next_normal = multiply(Vec2{next_edge.y, -next_edge.x}, orientation);
        auto miter = normalized(add(previous_normal, next_normal));
        float distance = fringe;
        const float denominator = dot(miter, next_normal);
        if (std::abs(denominator) > epsilon) {
            distance = fringe / denominator;
        } else {
            miter = next_normal;
        }
        distance = std::clamp(distance, -2.0f * fringe, 2.0f * fringe);
        out.push_back(add(current, multiply(miter, distance)));
    }
}

bool coherent_fringe(std::span<const Vec2> inner, std::span<const Vec2> outer) {
    if (!simple_outline(outer)) {
        return false;
    }
    const double orientation = signed_area(inner) > 0 ? 1.0 : -1.0;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const auto next = (i + 1) % inner.size();
        // Simple inner and outer contours alone do not prevent an offset
        // edge from folding back across its inner edge. With a simple outer
        // boundary and consistently oriented triangles, the complete mesh
        // covers its interior once rather than adding overlapping AA layers.
        if (triangle_area(inner[i], outer[next], inner[next]) * orientation <= 0 ||
            triangle_area(inner[i], outer[i], outer[next]) * orientation <= 0) {
            return false;
        }
    }
    return true;
}

struct StrokeRaster {
    Rect bounds;
    std::uint32_t width = 0, height = 0;
    std::vector<std::byte> rgba;
};

Result<StrokeRaster> raster_stroke(std::span<const Vec2> points, float width,
                                   float scale, Rect clip) {
    const double half = static_cast<double>(width) * 0.5;
    const double fringe = 1.0 / scale;
    double left = points.front().x, right = left;
    double top = points.front().y, bottom = top;
    for (const auto p : points) {
        left = std::min(left, static_cast<double>(p.x));
        right = std::max(right, static_cast<double>(p.x));
        top = std::min(top, static_cast<double>(p.y));
        bottom = std::max(bottom, static_cast<double>(p.y));
    }
    left = std::max(left - half - fringe, static_cast<double>(clip.x) - fringe);
    right = std::min(right + half + fringe,
                     static_cast<double>(clip.x) + clip.w + fringe);
    top = std::max(top - half - fringe, static_cast<double>(clip.y) - fringe);
    bottom = std::min(bottom + half + fringe,
                      static_cast<double>(clip.y) + clip.h + fringe);
    if (right <= left || bottom <= top) {
        return StrokeRaster{};
    }
    // Include a transparent border for bilinear sampling, and align texels to
    // physical pixels. The batch still uses the exact logical clip scope.
    left = std::floor(left * scale);
    right = std::ceil(right * scale);
    top = std::floor(top * scale);
    bottom = std::ceil(bottom * scale);
    const double raster_width = right - left;
    const double raster_height = bottom - top;
    if (!std::isfinite(raster_width) || !std::isfinite(raster_height) ||
        raster_width > detail::stroke_raster_dimension_limit ||
        raster_height > detail::stroke_raster_dimension_limit ||
        raster_width < 1 || raster_height < 1) {
        return Error{ErrorCode::capacity, "Stroke raster dimension limit exceeded"};
    }
    StrokeRaster result;
    result.width = static_cast<std::uint32_t>(raster_width);
    result.height = static_cast<std::uint32_t>(raster_height);
    const auto pixels = static_cast<std::size_t>(result.width) * result.height;
    const auto segments = std::max<std::size_t>(points.size() - 1, 1);
    constexpr std::size_t samples = 16;
    if (pixels > detail::stroke_raster_pixel_limit ||
        pixels * samples > detail::stroke_raster_work_limit / segments) {
        return Error{ErrorCode::capacity, "Stroke raster work or pixel limit exceeded"};
    }
    result.bounds = {static_cast<float>(left / scale), static_cast<float>(top / scale),
                     static_cast<float>(raster_width / scale),
                     static_cast<float>(raster_height / scale)};
    if (!valid_rect(result.bounds)) {
        return invalid_argument("Stroke raster exceeds finite logical coordinates");
    }
    struct Segment {
        double ax, ay, bx, by, ux, uy, length, line_offset;
    };
    std::vector<Segment> physical;
    physical.reserve(segments);
    for (std::size_t i = 0; i < segments; ++i) {
        const auto a = points[i];
        const auto b = points[std::min(i + 1, points.size() - 1)];
        const double ax = static_cast<double>(a.x) * scale;
        const double ay = static_cast<double>(a.y) * scale;
        const double bx = static_cast<double>(b.x) * scale;
        const double by = static_cast<double>(b.y) * scale;
        const double distance = std::hypot(bx - ax, by - ay);
        // Compensated endpoint determinant preserves the line's offset even
        // when products cancel. Subtracting a remote endpoint from each local
        // sample would discard that sample's perpendicular displacement.
        const double first = ax * by, second = ay * bx;
        const double determinant = (first - second) +
            (std::fma(ax, by, -first) - std::fma(ay, bx, -second));
        physical.push_back({ax, ay, bx, by,
                            distance > 0 ? (bx - ax) / distance : 0,
                            distance > 0 ? (by - ay) / distance : 0, distance,
                            distance > 0 ? determinant / distance : 0});
    }
    result.rgba.resize(pixels * 4, std::byte{255});
    const double radius_squared = half * scale * half * scale;
    constexpr std::array offsets{0.125, 0.375, 0.625, 0.875};
    for (std::uint32_t y = 0; y < result.height; ++y) {
        for (std::uint32_t x = 0; x < result.width; ++x) {
            unsigned covered = 0;
            for (const auto oy : offsets) {
                for (const auto ox : offsets) {
                    const double px = left + x + ox, py = top + y + oy;
                    for (const auto& segment : physical) {
                        const double ax = px - segment.ax, ay = py - segment.ay;
                        const double projection = ax * segment.ux + ay * segment.uy;
                        double distance_squared;
                        if (projection <= 0) {
                            distance_squared = ax * ax + ay * ay;
                        } else if ((px - segment.bx) * segment.ux +
                                   (py - segment.by) * segment.uy >= 0) {
                            const double bx = px - segment.bx, by = py - segment.by;
                            distance_squared = bx * bx + by * by;
                        } else {
                            const double perpendicular =
                                (px * segment.uy - py * segment.ux) - segment.line_offset;
                            distance_squared = perpendicular * perpendicular;
                        }
                        if (distance_squared <= radius_squared) {
                            ++covered;
                            break; // Union coverage: count a sample only once.
                        }
                    }
                }
            }
            result.rgba[(static_cast<std::size_t>(y) * result.width + x) * 4 + 3] =
                static_cast<std::byte>((covered * 255 + samples / 2) / samples);
        }
    }
    return result;
}

std::vector<std::byte> raster_shadow(float radius_px, float sigma,
                                     std::uint32_t& texture_size,
                                     std::uint32_t& border_px) {
    const auto blur_radius = static_cast<std::uint32_t>(std::ceil(3.0f * sigma));
    border_px = static_cast<std::uint32_t>(std::ceil(radius_px)) + blur_radius;
    const std::uint64_t size64 = std::uint64_t{border_px} * 2 + 1;
    if (size64 > 4096) {
        return {};
    }
    texture_size = static_cast<std::uint32_t>(size64);
    const auto count = static_cast<std::size_t>(texture_size) * texture_size;
    std::vector<float> source(count, 0.0f);
    const float shape_left = static_cast<float>(border_px) - radius_px;
    const float shape_size = radius_px * 2.0f + 1.0f;
    const float shape_right = shape_left + shape_size;
    const float shape_radius = static_cast<float>(radius_px);
    constexpr std::array offsets{0.125f, 0.375f, 0.625f, 0.875f};
    for (std::uint32_t y = 0; y < texture_size; ++y) {
        for (std::uint32_t x = 0; x < texture_size; ++x) {
            float covered = 0.0f;
            for (const float oy : offsets) {
                for (const float ox : offsets) {
                    const float px = static_cast<float>(x) + ox;
                    const float py = static_cast<float>(y) + oy;
                    const float center_x = std::clamp(px, shape_left + shape_radius,
                                                      shape_right - shape_radius);
                    const float center_y = std::clamp(py, shape_left + shape_radius,
                                                      shape_right - shape_radius);
                    const auto delta = Vec2{px - center_x, py - center_y};
                    const bool inside = shape_radius == 0.0f
                                            ? px >= shape_left && px <= shape_right &&
                                                  py >= shape_left && py <= shape_right
                                            : dot(delta, delta) <=
                                                  shape_radius * shape_radius;
                    if (inside) {
                        covered += 1.0f;
                    }
                }
            }
            source[static_cast<std::size_t>(y) * texture_size + x] = covered / 16.0f;
        }
    }

    const auto kernel = detail::gaussian_kernel(sigma);
    const int kernel_radius = static_cast<int>(kernel.size() / 2);
    std::vector<float> horizontal(count, 0.0f);
    std::vector<float> blurred(count, 0.0f);
    for (std::uint32_t y = 0; y < texture_size; ++y) {
        for (std::uint32_t x = 0; x < texture_size; ++x) {
            float total = 0.0f;
            for (int offset = -kernel_radius; offset <= kernel_radius; ++offset) {
                const int sx = static_cast<int>(x) + offset;
                if (sx >= 0 && sx < static_cast<int>(texture_size)) {
                    total += source[static_cast<std::size_t>(y) * texture_size +
                                    static_cast<std::size_t>(sx)] *
                             kernel[static_cast<std::size_t>(offset + kernel_radius)];
                }
            }
            horizontal[static_cast<std::size_t>(y) * texture_size + x] = total;
        }
    }
    for (std::uint32_t y = 0; y < texture_size; ++y) {
        for (std::uint32_t x = 0; x < texture_size; ++x) {
            float total = 0.0f;
            for (int offset = -kernel_radius; offset <= kernel_radius; ++offset) {
                const int sy = static_cast<int>(y) + offset;
                if (sy >= 0 && sy < static_cast<int>(texture_size)) {
                    total += horizontal[static_cast<std::size_t>(sy) * texture_size + x] *
                             kernel[static_cast<std::size_t>(offset + kernel_radius)];
                }
            }
            blurred[static_cast<std::size_t>(y) * texture_size + x] = total;
        }
    }

    std::vector<std::byte> rgba(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        rgba[i * 4 + 0] = std::byte{255};
        rgba[i * 4 + 1] = std::byte{255};
        rgba[i * 4 + 2] = std::byte{255};
        rgba[i * 4 + 3] = static_cast<std::byte>(
            std::clamp(static_cast<int>(std::lround(blurred[i] * 255.0f)), 0, 255));
    }
    return rgba;
}

} // namespace

namespace detail {

std::vector<float> gaussian_kernel(float sigma) {
    const int radius = static_cast<int>(std::ceil(3.0f * sigma));
    std::vector<float> kernel(static_cast<std::size_t>(radius * 2 + 1));
    float total = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float weight = std::exp(-0.5f * static_cast<float>(i * i) /
                                      (sigma * sigma));
        kernel[static_cast<std::size_t>(i + radius)] = weight;
        total += weight;
    }
    for (auto& weight : kernel) {
        weight /= total;
    }
    return kernel;
}

} // namespace detail

struct Canvas::Impl {
    explicit Impl(std::shared_ptr<TextureStore> texture_store, std::size_t packet_limit)
        : textures(std::move(texture_store)), max_packet_slots(packet_limit) {
        if (!textures) {
            construction_error = invalid_argument("Canvas requires a texture store");
            return;
        }
        if (max_packet_slots == 0) {
            construction_error = invalid_argument("Canvas packet slot limit must be positive");
            return;
        }
        constexpr std::array pixel{
            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        auto result = textures->create(1, 1, pixel, AlphaMode::straight);
        if (std::holds_alternative<Error>(result)) {
            construction_error = std::get<Error>(std::move(result));
        } else {
            white = std::get<TextureHandle>(result);
        }
    }

    ~Impl() {
        if (!textures) {
            return;
        }
        for (const auto handle : stroke_textures) {
            textures->release(handle);
        }
        for (const auto& entry : shadows) {
            textures->release(entry.handle);
        }
        if (white) {
            textures->release(*white);
        }
    }

    void fail(Error error) {
        if (!frame_error) {
            frame_error = std::move(error);
        }
    }

    bool ready() const { return active && current && !frame_error; }

    void reset_frame(bool published = false) noexcept {
        for (const auto handle : stroke_textures) {
            textures->release(handle);
        }
        stroke_textures.clear();
        if (current && !published) {
            current->textures.clear();
        }
        active = false;
        current.reset();
        clips.clear();
        pending_batches.clear();
        merge_barrier = false;
        semantic_count = 0;
        frame_handles.clear();
        layer_batches.clear();
        layer_pending.clear();
    }

    void flush_layers() {
        if (layer_batches.empty() || !current) return;
        current->batches.insert(current->batches.end(), layer_batches.begin(), layer_batches.end());
        pending_batches.insert(pending_batches.end(), layer_pending.begin(), layer_pending.end());
        layer_batches.clear();
        layer_pending.clear();
        merge_barrier = true;
    }

    bool append(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices,
                TextureHandle texture) {
        if (!ready() || clips.back().w == 0.0f || clips.back().h == 0.0f) {
            return false;
        }
        if (current->vertices.size() > std::numeric_limits<std::uint32_t>::max() -
                                           vertices.size() ||
            current->indices.size() > std::numeric_limits<std::uint32_t>::max() -
                                          indices.size()) {
            fail({ErrorCode::capacity, "Canvas geometry exceeds packet limits"});
            return false;
        }
        const auto base = static_cast<std::uint32_t>(current->vertices.size());
        const auto first_index = static_cast<std::uint32_t>(current->indices.size());
        current->vertices.insert(current->vertices.end(), vertices.begin(), vertices.end());
        for (const auto index : indices) {
            current->indices.push_back(base + index);
        }
        if (!merge_barrier && !current->batches.empty() &&
            pending_batches.back().texture == texture &&
            equal(current->batches.back().clip, clips.back()) &&
            current->batches.back().first_index + current->batches.back().index_count ==
                first_index) {
            current->batches.back().index_count += static_cast<std::uint32_t>(indices.size());
        } else {
            current->batches.push_back({first_index, static_cast<std::uint32_t>(indices.size()),
                                        0, 0, clips.back()});
            pending_batches.push_back({texture});
        }
        merge_barrier = false;
        return true;
    }

    void image_unchecked(Rect bounds, TextureHandle texture, Rect uv, Color tint) {
        const auto color = premultiply(tint);
        const std::array vertices{
            Vertex{{bounds.x, bounds.y}, {uv.x, uv.y}, color},
            Vertex{{bounds.x + bounds.w, bounds.y}, {uv.x + uv.w, uv.y}, color},
            Vertex{{bounds.x + bounds.w, bounds.y + bounds.h},
                   {uv.x + uv.w, uv.y + uv.h}, color},
            Vertex{{bounds.x, bounds.y + bounds.h}, {uv.x, uv.y + uv.h}, color},
        };
        constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
        append(vertices, indices, texture);
    }

    void polygon_unchecked(std::span<const Vec2> points, Color color,
                           std::span<const std::uint32_t> interior = {},
                           std::span<const Vec2> outer = {}) {
        if (points.size() < 3 || clips.back().w == 0.0f || clips.back().h == 0.0f) {
            return;
        }
        const auto inner_color = premultiply(color);
        vertex_scratch.clear();
        vertex_scratch.reserve(points.size() * 2);
        for (const auto point : points) {
            vertex_scratch.push_back({point, {0.5f, 0.5f}, inner_color});
        }
        if (outer.empty()) {
            fringe_outline(points, 1.0f / current->scale, fringe_scratch);
            outer = fringe_scratch;
        }
        for (const auto point : outer) {
            vertex_scratch.push_back({point, {0.5f, 0.5f}, {0, 0, 0, 0}});
        }

        index_scratch.clear();
        index_scratch.reserve((points.size() - 2) * 3 + points.size() * 6);
        if (interior.empty()) {
            for (std::uint32_t i = 1; i + 1 < points.size(); ++i) {
                index_scratch.insert(index_scratch.end(), {0, i, i + 1});
            }
        } else {
            index_scratch.insert(index_scratch.end(), interior.begin(), interior.end());
        }
        const auto outer_index = static_cast<std::uint32_t>(points.size());
        for (std::uint32_t i = 0; i < points.size(); ++i) {
            const auto next = (i + 1) % static_cast<std::uint32_t>(points.size());
            index_scratch.insert(index_scratch.end(), {i, outer_index + next, next,
                                                       i, outer_index + i, outer_index + next});
        }
        append(vertex_scratch, index_scratch, *white);
    }

    void stroke_coverage(std::span<const Vec2> points, float width, Color color) {
        try {
            auto raster = raster_stroke(points, width, current->scale, clips.back());
            if (const auto* error = std::get_if<Error>(&raster)) {
                fail(*error);
                return;
            }
            auto& coverage = std::get<StrokeRaster>(raster);
            if (coverage.rgba.empty()) {
                return;
            }
            auto created = textures->create(coverage.width, coverage.height,
                                             coverage.rgba, AlphaMode::straight);
            if (const auto* error = std::get_if<Error>(&created)) {
                fail(*error);
                return;
            }
            const auto handle = std::get<TextureHandle>(created);
            try {
                stroke_textures.push_back(handle);
            } catch (...) {
                textures->release(handle);
                throw;
            }
            image_unchecked(coverage.bounds, handle, {0, 0, 1, 1}, color);
        } catch (const std::bad_alloc&) {
            fail({ErrorCode::capacity, "Stroke coverage allocation failed"});
        }
    }

    Result<ShadowEntry*> shadow_texture(float radius_px, float sigma_px) {
        const ShadowKey key{radius_px, sigma_px};
        for (auto& entry : shadows) {
            if (entry.key == key) {
                entry.stamp = ++shadow_stamp;
                return &entry;
            }
        }
        if (shadows.size() >= detail::shadow_cache_capacity) {
            auto candidate = shadows.end();
            for (auto it = shadows.begin(); it != shadows.end(); ++it) {
                const bool recorded = std::any_of(
                    pending_batches.begin(), pending_batches.end(),
                    [handle = it->handle](const PendingBatch& batch) {
                        return batch.texture == handle;
                    });
                if (recorded) {
                    continue;
                }
                auto snapshot = textures->snapshot(it->handle);
                if (std::holds_alternative<std::shared_ptr<const TextureSnapshot>>(snapshot) &&
                    std::get<std::shared_ptr<const TextureSnapshot>>(snapshot).use_count() == 2 &&
                    (candidate == shadows.end() || it->stamp < candidate->stamp)) {
                    candidate = it;
                }
            }
            if (candidate == shadows.end()) {
                return Error{ErrorCode::capacity,
                             "Shadow cache is full and every entry is leased"};
            }
            textures->release(candidate->handle);
            shadows.erase(candidate);
        }

        std::uint32_t texture_size = 0;
        std::uint32_t border_px = 0;
        auto pixels = raster_shadow(radius_px, sigma_px, texture_size, border_px);
        if (pixels.empty()) {
            return Error{ErrorCode::capacity, "Shadow texture exceeds bounded dimensions"};
        }
        auto created = textures->create(texture_size, texture_size, pixels,
                                        AlphaMode::straight);
        if (std::holds_alternative<Error>(created)) {
            return std::get<Error>(std::move(created));
        }
        shadows.push_back({key, std::get<TextureHandle>(created), texture_size,
                           border_px, ++shadow_stamp});
        return &shadows.back();
    }

    std::shared_ptr<TextureStore> textures;
    std::size_t max_packet_slots = 0;
    std::optional<TextureHandle> white;
    Status construction_error;
    Status frame_error;
    bool active = false;
    bool merge_barrier = false;
    std::vector<std::shared_ptr<PacketData>> slots;
    std::shared_ptr<PacketData> current;
    std::vector<Rect> clips;
    std::vector<PendingBatch> pending_batches;
    std::vector<ShadowEntry> shadows;
    std::vector<TextureHandle> stroke_textures;
    std::uint64_t shadow_stamp = 0;
    std::size_t semantic_count = 0;
    std::vector<TextureHandle> frame_handles;
    std::vector<Vec2> outline_scratch, fringe_scratch, clean_scratch, direction_scratch,
                      normal_scratch, plus_scratch, minus_scratch;
    std::vector<Vertex> vertex_scratch;
    std::vector<std::uint32_t> index_scratch, remaining_scratch, triangle_scratch;
    // Detached overlay layers: batch descriptors moved out of the main order
    // and appended above it by flush_layers().
    std::vector<Batch> layer_batches;
    std::vector<PendingBatch> layer_pending;
};
Canvas::Canvas(std::shared_ptr<TextureStore> textures, std::size_t max_packet_slots)
    : impl_(std::make_unique<Impl>(std::move(textures), max_packet_slots)) {}

Canvas::~Canvas() = default;

void Canvas::begin(Rect viewport, float scale) {
    if (impl_->active) {
        impl_->fail({ErrorCode::invalid_frame, "Canvas frame is already active"});
        return;
    }
    impl_->frame_error = impl_->construction_error;
    impl_->pending_batches.clear();
    impl_->clips.clear();
    impl_->merge_barrier = false;
    impl_->current.reset();
    impl_->semantic_count = 0;
    impl_->frame_handles.clear();
    impl_->layer_batches.clear();
    impl_->layer_pending.clear();
    impl_->active = true;
    if (!valid_rect(viewport) || !finite(scale) || scale <= 0.0f) {
        impl_->fail(invalid_argument("Canvas viewport and scale must be finite and valid"));
        return;
    }
    for (auto& slot : impl_->slots) {
        if (slot.use_count() == 1) {
            slot->textures.clear();
            if (!impl_->current) {
                impl_->current = slot;
            }
        }
    }
    if (!impl_->current && impl_->slots.size() < impl_->max_packet_slots) {
        impl_->current = std::make_shared<PacketData>();
        impl_->slots.push_back(impl_->current);
    }
    if (!impl_->current) {
        impl_->fail({ErrorCode::busy, "All canvas packet slots are leased"});
        return;
    }
    impl_->current->viewport = viewport;
    impl_->current->scale = scale;
    impl_->current->vertices.clear();
    impl_->current->indices.clear();
    impl_->current->batches.clear();
    impl_->current->textures.clear();
    impl_->clips.push_back(viewport);
}

void Canvas::push_clip(Rect clip) {
    if (!impl_->ready()) {
        if (!impl_->active) {
            impl_->fail({ErrorCode::invalid_frame, "Canvas frame is not active"});
        }
        return;
    }
    if (!valid_rect(clip)) {
        impl_->fail(invalid_argument("Canvas clip must be finite and nonnegative"));
        return;
    }
    impl_->clips.push_back(intersect(impl_->clips.back(), clip));
}

void Canvas::pop_clip() {
    if (!impl_->ready()) {
        if (!impl_->active) {
            impl_->fail({ErrorCode::invalid_frame, "Canvas frame is not active"});
        }
        return;
    }
    if (impl_->clips.size() <= 1) {
        impl_->fail({ErrorCode::unbalanced_scope, "Cannot pop the root canvas clip"});
        return;
    }
    impl_->clips.pop_back();
}

void Canvas::rounded_rect(Rect bounds, float radius, Color color) {
    if (!impl_->ready()) {
        return;
    }
    if (!valid_rect(bounds) || !finite(radius) || radius < 0.0f || !valid_color(color)) {
        impl_->fail(invalid_argument("Rounded rectangle input must be finite and valid"));
        return;
    }
    if (bounds.w == 0.0f || bounds.h == 0.0f) {
        return;
    }
    rounded_outline(bounds, radius, impl_->current->scale, impl_->outline_scratch);
    impl_->polygon_unchecked(impl_->outline_scratch, color);
}
void Canvas::gradient(Rect bounds, Color top, Color bottom) {
    if (!impl_->ready()) return;
    if (!valid_rect(bounds) || !valid_color(top) || !valid_color(bottom)) {
        impl_->fail(invalid_argument("Gradient bounds and colors must be valid"));
        return;
    }
    if (bounds.w == 0 || bounds.h == 0) return;
    const std::array vertices{
        Vertex{{bounds.x, bounds.y}, {0.5f, 0.5f}, premultiply(top)},
        Vertex{{bounds.x + bounds.w, bounds.y}, {0.5f, 0.5f}, premultiply(top)},
        Vertex{{bounds.x + bounds.w, bounds.y + bounds.h}, {0.5f, 0.5f}, premultiply(bottom)},
        Vertex{{bounds.x, bounds.y + bounds.h}, {0.5f, 0.5f}, premultiply(bottom)},
    };
    constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    impl_->append(vertices, indices, *impl_->white);
}

void Canvas::gradient(Rect bounds, Color top_left, Color top_right,
                      Color bottom_right, Color bottom_left) {
    if (!impl_->ready()) return;
    if (!valid_rect(bounds) || !valid_color(top_left) || !valid_color(top_right) ||
        !valid_color(bottom_right) || !valid_color(bottom_left)) {
        impl_->fail(invalid_argument("Gradient bounds and colors must be valid"));
        return;
    }
    if (bounds.w == 0 || bounds.h == 0) return;
    constexpr std::uint32_t cells = 8;
    const Color corners[4]{premultiply(top_left), premultiply(top_right),
                           premultiply(bottom_right), premultiply(bottom_left)};
    const auto lerp = [](Color a, Color b, float t) {
        return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                     a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    };
    auto& vertices = impl_->vertex_scratch;
    auto& indices = impl_->index_scratch;
    vertices.clear();
    indices.clear();
    vertices.reserve((cells + 1) * (cells + 1));
    indices.reserve(cells * cells * 6);
    for (std::uint32_t y = 0; y <= cells; ++y) {
        const float v = static_cast<float>(y) / cells;
        const Color left = lerp(corners[0], corners[3], v);
        const Color right = lerp(corners[1], corners[2], v);
        // Exact edges: the last row/column uses the bound itself, not a sum.
        const float py = y == cells ? bounds.y + bounds.h : bounds.y + bounds.h * v;
        for (std::uint32_t x = 0; x <= cells; ++x) {
            const float u = static_cast<float>(x) / cells;
            const float px = x == cells ? bounds.x + bounds.w : bounds.x + bounds.w * u;
            vertices.push_back({{px, py}, {0.5f, 0.5f}, lerp(left, right, u)});
        }
    }
    for (std::uint32_t y = 0; y < cells; ++y) {
        for (std::uint32_t x = 0; x < cells; ++x) {
            const std::uint32_t a = y * (cells + 1) + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + cells + 2;
            const std::uint32_t d = a + cells + 1;
            indices.insert(indices.end(), {a, b, c, a, c, d});
        }
    }
    impl_->append(vertices, indices, *impl_->white);
}

void Canvas::border(Rect bounds, float radius, float width, Color color) {
    if (!impl_->ready()) return;
    if (!valid_rect(bounds) || !finite(radius) || radius < 0 ||
        !finite(width) || width <= 0 || !valid_color(color)) {
        impl_->fail(invalid_argument("Border input must be finite and valid"));
        return;
    }
    if (bounds.w == 0 || bounds.h == 0) return;
    if (width >= std::min(bounds.w, bounds.h) * 0.5f) {
        rounded_rect(bounds, radius, color);
        return;
    }
    const float outer_radius = std::clamp(radius, 0.0f, std::min(bounds.w, bounds.h) * 0.5f);
    const Rect inner{bounds.x + width, bounds.y + width,
                     bounds.w - width * 2, bounds.h - width * 2};
    const float inner_radius = std::max(0.0f, outer_radius - width);
    auto& outer = impl_->outline_scratch;
    auto& fringe = impl_->fringe_scratch;
    auto& vertices = impl_->vertex_scratch;
    auto& indices = impl_->index_scratch;
    rounded_outline(bounds, outer_radius, impl_->current->scale, outer);
    fringe_outline(outer, 1.0f / impl_->current->scale, fringe);
    const auto count = static_cast<std::uint32_t>(outer.size());
    const Color fill = premultiply(color);
    vertices.clear();
    vertices.reserve(count * 3);
    for (const auto point : outer)
        vertices.push_back({point, {0.5f, 0.5f}, fill});
    const float middle_x = bounds.x + bounds.w * 0.5f;
    const float middle_y = bounds.y + bounds.h * 0.5f;
    for (const auto point : outer) {
        const bool left = point.x <= middle_x;
        const bool top = point.y <= middle_y;
        const float center_x = (left ? bounds.x : bounds.x + bounds.w) +
                               (left ? outer_radius : -outer_radius);
        const float center_y = (top ? bounds.y : bounds.y + bounds.h) +
                               (top ? outer_radius : -outer_radius);
        const float inner_x = (left ? inner.x : inner.x + inner.w) +
                              (left ? inner_radius : -inner_radius);
        const float inner_y = (top ? inner.y : inner.y + inner.h) +
                              (top ? inner_radius : -inner_radius);
        const float ratio = outer_radius > 0 ? inner_radius / outer_radius : 0;
        vertices.push_back({{inner_x + (point.x - center_x) * ratio,
                             inner_y + (point.y - center_y) * ratio},
                            {0.5f, 0.5f}, fill});
    }
    for (const auto point : fringe)
        vertices.push_back({point, {0.5f, 0.5f}, {0, 0, 0, 0}});
    indices.clear();
    indices.reserve(count * 12);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto next = (i + 1) % count;
        indices.insert(indices.end(), {i, next, count + next, i, count + next, count + i,
                                       i, count * 2 + next, next,
                                       i, count * 2 + i, count * 2 + next});
    }
    impl_->append(vertices, indices, *impl_->white);
}

void Canvas::convex(std::span<const Vec2> points, Color color) {
    if (!impl_->ready()) {
        return;
    }
    if (points.size() < 3 || !valid_color(color) ||
        !std::all_of(points.begin(), points.end(), [](Vec2 point) { return finite(point); }) ||
        !is_convex(points)) {
        impl_->fail(invalid_argument("Convex path must be finite and convex"));
        return;
    }
    impl_->polygon_unchecked(points, color);
}

void Canvas::polygon(std::span<const Vec2> points, Color color) {
    if (!impl_->ready()) return;
    if (points.size() > detail::stroke_mesh_point_limit + 1) {
        impl_->fail({ErrorCode::capacity, "Polygon point limit exceeded"});
        return;
    }
    if (points.size() < 3 || !valid_color(color) ||
        !std::all_of(points.begin(), points.end(), [](Vec2 point) { return finite(point); })) {
        impl_->fail(invalid_argument("Polygon must have at least three finite points"));
        return;
    }
    try {
        auto& clean = impl_->clean_scratch;
        clean.assign(points.begin(), points.end());
        remove_duplicate_closure(clean);
        if (clean.size() > detail::stroke_mesh_point_limit) {
            impl_->fail({ErrorCode::capacity, "Polygon point limit exceeded"});
            return;
        }
        auto& interior = impl_->triangle_scratch;
        if (!simple_outline(clean) ||
            (triangulate_simple(clean, impl_->remaining_scratch, interior), interior.empty())) {
            impl_->fail(invalid_argument("Polygon must be simple and non-degenerate"));
            return;
        }
        if (impl_->clips.back().w == 0.0f || impl_->clips.back().h == 0.0f) return;
        auto& outer = impl_->fringe_scratch;
        fringe_outline(clean, 1.0f / impl_->current->scale, outer);
        // Sharp concave spikes can fold the offset ring; such outlines keep a
        // hard edge instead of overlapping translucent fringe triangles.
        if (!coherent_fringe(clean, outer)) outer.assign(clean.begin(), clean.end());
        impl_->polygon_unchecked(clean, color, interior, outer);
    } catch (const std::bad_alloc&) {
        impl_->fail({ErrorCode::capacity, "Polygon allocation failed"});
    }
}

void Canvas::line(std::span<const Vec2> points, float width, Color color) {
    if (!impl_->ready()) {
        return;
    }
    if (points.size() > detail::stroke_point_limit) {
        impl_->fail({ErrorCode::capacity, "Stroke point limit exceeded"});
        return;
    }
    if (points.size() < 2 || !finite(width) || width <= 0.0f || !valid_color(color) ||
        !std::all_of(points.begin(), points.end(), [](Vec2 point) { return finite(point); })) {
        impl_->fail(invalid_argument("Line input must be finite and have positive width"));
        return;
    }
    if (impl_->clips.back().w == 0 || impl_->clips.back().h == 0) {
        return;
    }
    try {
        const float half = width * 0.5f;
        auto& clean = impl_->clean_scratch;
        clean.clear();
        clean.reserve(points.size());
        for (const auto point : points) {
            if (clean.empty() || point.x != clean.back().x || point.y != clean.back().y) {
                clean.push_back(point);
            }
        }
        bool mesh_candidate = clean.size() >= 2 && half > epsilon &&
                              finite(half * impl_->current->scale) &&
                              clean.size() <= detail::stroke_mesh_point_limit;
        for (std::size_t i = 1; mesh_candidate && i < clean.size(); ++i) {
            const auto edge = subtract(clean[i], clean[i - 1]);
            const float magnitude = length(edge);
            mesh_candidate = finite(magnitude) && magnitude > epsilon;
            if (mesh_candidate && i + 1 < clean.size()) {
                // Sharp reversals have an unbounded inner join miter. The union
                // raster handles those without trimming or overlapping geometry.
                mesh_candidate = dot(normalized(edge),
                                     normalized(subtract(clean[i + 1], clean[i]))) > -0.5f;
            }
        }
        if (mesh_candidate) {
            auto& outline = impl_->outline_scratch;
            stroke_outline(clean, half, impl_->current->scale,
                           impl_->direction_scratch, impl_->normal_scratch,
                           impl_->plus_scratch, impl_->minus_scratch, outline);
            if (simple_outline(outline)) {
                auto& outer = impl_->fringe_scratch;
                fringe_outline(outline, 1.0f / impl_->current->scale, outer);
                if (coherent_fringe(outline, outer)) {
                    auto& interior = impl_->triangle_scratch;
                    triangulate_simple(outline, impl_->remaining_scratch, interior);
                    if (!interior.empty()) {
                        // One connected interior and exterior fringe avoid translucent
                        // overdraw at segment joins and round caps.
                        impl_->polygon_unchecked(outline, color, interior, outer);
                        return;
                    }
                }
            }
        }
        impl_->stroke_coverage(clean, width, color);
    } catch (const std::bad_alloc&) {
        impl_->fail({ErrorCode::capacity, "Stroke allocation failed"});
    }
}

void Canvas::image(Rect bounds, TextureHandle texture, Rect uv, Color tint) {
    if (!impl_->ready()) {
        return;
    }
    if (!valid_rect(bounds) || !valid_rect(uv) || !valid_color(tint)) {
        impl_->fail(invalid_argument("Image input must be finite and valid"));
        return;
    }
    if (bounds.w == 0.0f || bounds.h == 0.0f ||
        impl_->clips.back().w == 0.0f || impl_->clips.back().h == 0.0f) {
        return;
    }
    impl_->image_unchecked(bounds, texture, uv, tint);
}

void Canvas::shadow(Rect bounds, float radius, float sigma, Color color) {
    if (!impl_->ready()) {
        return;
    }
    if (!valid_rect(bounds) || !finite(radius) || radius < 0.0f || !finite(sigma) ||
        !valid_color(color)) {
        impl_->fail(invalid_argument("Shadow input must be finite and valid"));
        return;
    }
    const float physical_sigma = sigma * impl_->current->scale;
    if (!finite(physical_sigma) || physical_sigma < 0.5f || physical_sigma > 32.0f) {
        impl_->fail(invalid_argument("Shadow sigma must be between 0.5 and 32 physical pixels"));
        return;
    }
    if (bounds.w == 0.0f || bounds.h == 0.0f ||
        impl_->clips.back().w == 0.0f || impl_->clips.back().h == 0.0f) {
        return;
    }
    const float clamped_radius = std::min(radius, 0.5f * std::min(bounds.w, bounds.h));
    const float physical_radius = clamped_radius * impl_->current->scale;
    if (!finite(physical_radius) || physical_radius > 2047.0f) {
        impl_->fail({ErrorCode::capacity, "Shadow radius exceeds cache limits"});
        return;
    }
    auto cached = impl_->shadow_texture(physical_radius, physical_sigma);
    if (std::holds_alternative<Error>(cached)) {
        impl_->fail(std::get<Error>(std::move(cached)));
        return;
    }
    const auto* entry = std::get<ShadowEntry*>(cached);
    const float padding = static_cast<float>(entry->border_px - entry->key.radius_px) /
                          impl_->current->scale;
    const Rect destination{bounds.x - padding, bounds.y - padding,
                           bounds.w + padding * 2.0f, bounds.h + padding * 2.0f};
    const float nominal_border = static_cast<float>(entry->border_px) /
                                 impl_->current->scale;
    const float border_x = std::min(nominal_border, destination.w * 0.5f);
    const float border_y = std::min(nominal_border, destination.h * 0.5f);
    const float source_border = static_cast<float>(entry->border_px) /
                                static_cast<float>(entry->texture_size);
    const std::array dx{destination.x, destination.x + border_x,
                        destination.x + destination.w - border_x,
                        destination.x + destination.w};
    const std::array dy{destination.y, destination.y + border_y,
                        destination.y + destination.h - border_y,
                        destination.y + destination.h};
    const std::array su{0.0f, source_border, 1.0f - source_border, 1.0f};
    for (std::size_t y = 0; y < 3; ++y) {
        for (std::size_t x = 0; x < 3; ++x) {
            const Rect piece{dx[x], dy[y], dx[x + 1] - dx[x], dy[y + 1] - dy[y]};
            const Rect uv{su[x], su[y], su[x + 1] - su[x], su[y + 1] - su[y]};
            if (piece.w > 0.0f && piece.h > 0.0f) {
                impl_->image_unchecked(piece, entry->handle, uv, color);
            }
        }
    }
}

void Canvas::semantic(Id id, Role role, std::string_view label, Rect bounds,
                      bool enabled, bool focused, bool selected,
                      std::string_view value, bool invalid) {
    if (!impl_->ready()) {
        return;
    }
    if (!valid_rect(bounds)) {
        impl_->fail(invalid_argument("Semantic bounds must be finite and nonnegative"));
        return;
    }
    auto& semantics = impl_->current->semantics;
    Semantic* entry = nullptr;
    if (impl_->semantic_count < semantics.size()) {
        entry = &semantics[impl_->semantic_count];
    } else {
        semantics.emplace_back();
        entry = &semantics.back();
    }
    if (label.empty()) {
        entry->label.clear();
    } else {
        entry->label.assign(label.data(), label.size());
    }
    if (value.empty()) entry->value.clear();
    else entry->value.assign(value.data(), value.size());
    entry->id = id;
    entry->role = role;
    entry->enabled = enabled;
    entry->focused = focused;
    entry->selected = selected;
    entry->invalid = invalid;
    entry->bounds = bounds;
    ++impl_->semantic_count;
}

Result<DrawPacket> Canvas::finish() {
    if (!impl_->active) {
        return Error{ErrorCode::invalid_frame, "Canvas frame is not active"};
    }
    if (!impl_->frame_error && impl_->clips.size() != 1) {
        impl_->fail({ErrorCode::unbalanced_scope, "Canvas clip scopes are unbalanced"});
    }
    if (!impl_->frame_error) {
        try {
            impl_->flush_layers();
        } catch (const std::bad_alloc&) {
            impl_->fail({ErrorCode::capacity, "Canvas layer allocation failed"});
        }
    }
    if (impl_->frame_error) {
        const auto error = *impl_->frame_error;
        impl_->reset_frame();
        return error;
    }
    if (impl_->semantic_count < impl_->current->semantics.size()) {
        impl_->current->semantics.resize(impl_->semantic_count);
    }
    auto& handles = impl_->frame_handles;
    handles.clear();
    handles.reserve(impl_->pending_batches.size());
    for (std::size_t i = 0; i < impl_->pending_batches.size(); ++i) {
        const auto handle = impl_->pending_batches[i].texture;
        auto found = std::find(handles.begin(), handles.end(), handle);
        std::size_t texture_index = 0;
        if (found == handles.end()) {
            auto snapshot = impl_->textures->snapshot(handle);
            if (std::holds_alternative<Error>(snapshot)) {
                const auto error = std::get<Error>(std::move(snapshot));
                impl_->reset_frame();
                return error;
            }
            texture_index = handles.size();
            handles.push_back(handle);
            impl_->current->textures.push_back(
                std::get<std::shared_ptr<const TextureSnapshot>>(std::move(snapshot)));
        } else {
            texture_index = static_cast<std::size_t>(found - handles.begin());
        }
        impl_->current->batches[i].texture_index =
            static_cast<std::uint32_t>(texture_index);
    }
    if (const auto error = validate_packet(*impl_->current)) {
        impl_->reset_frame();
        return *error;
    }
    DrawPacket result = impl_->current;
    impl_->reset_frame(true);
    return result;
}

namespace detail {

Status CanvasAccess::status(const Canvas& canvas) {
    const auto& impl = *canvas.impl_;
    return impl.frame_error ? impl.frame_error : impl.construction_error;
}

void CanvasAccess::abort(Canvas& canvas) noexcept {
    canvas.impl_->reset_frame();
}

PanelMark CanvasAccess::open_panel(Canvas& canvas, Id id, std::string_view title,
                                  Rect provisional, float radius, Color color) {
    auto& impl = *canvas.impl_;
    if (!impl.ready()) {
        return {};
    }
    if (!valid_rect(provisional) || !finite(radius) || radius < 0.0f ||
        !valid_color(color)) {
        impl.fail(invalid_argument("Panel input must be finite and valid"));
        return {};
    }
    const PanelMark mark{impl.current->batches.size(), impl.semantic_count,
                         impl.clips.back(), radius, color};
    try {
        canvas.semantic(id, Role::panel, title, provisional, true, false, false);
    } catch (const std::bad_alloc&) {
        impl.fail({ErrorCode::capacity, {}});
        return {};
    }
    // Even matching child paint must start after the reserved background.
    impl.merge_barrier = true;
    return mark;
}

void CanvasAccess::close_panel(Canvas& canvas, PanelMark mark,
                              Rect bounds, Rect interior) {
    auto& impl = *canvas.impl_;
    if (!impl.ready()) {
        return;
    }
    if (!valid_rect(bounds) || !valid_rect(interior)) {
        impl.fail(invalid_argument("Panel bounds and interior must be finite and valid"));
        return;
    }
    auto& packet = *impl.current;
    if (mark.first_batch > packet.batches.size() ||
        mark.semantic_index >= impl.semantic_count ||
        mark.semantic_index >= packet.semantics.size() ||
        packet.semantics[mark.semantic_index].role != Role::panel ||
        !equal(mark.clip, impl.clips.back())) {
        impl.fail({ErrorCode::unbalanced_scope, "Invalid canvas panel boundary"});
        return;
    }
    const auto child_end = packet.batches.size();
    impl.merge_barrier = true;
    try {
        // Only the final outline is tessellated: its clamped radius and topology
        // can differ completely from those of the provisional zero-height box.
        canvas.rounded_rect(bounds, mark.radius, mark.color);
    } catch (const std::bad_alloc&) {
        impl.fail({ErrorCode::capacity, {}});
        return;
    }
    if (!impl.ready()) {
        return;
    }
    const auto child_clip = intersect(mark.clip, intersect(bounds, interior));
    for (std::size_t i = mark.first_batch; i < child_end; ++i) {
        packet.batches[i].clip = intersect(packet.batches[i].clip, child_clip);
    }
    packet.semantics[mark.semantic_index].bounds = bounds;

    // Move descriptors, not meshes. Global indices stay valid; nested panels
    // have already ordered their own descriptors inside this child range.
    const auto first = static_cast<std::ptrdiff_t>(mark.first_batch);
    const auto middle = static_cast<std::ptrdiff_t>(child_end);
    std::rotate(packet.batches.begin() + first, packet.batches.begin() + middle,
                packet.batches.end());
    std::rotate(impl.pending_batches.begin() + first,
                impl.pending_batches.begin() + middle, impl.pending_batches.end());
    // Subsequent sibling paint must not extend a child batch after reordering.
    impl.merge_barrier = true;
}
CanvasLayer CanvasAccess::begin_layer(Canvas& canvas, Rect clip) {
    auto& impl = *canvas.impl_;
    if (!impl.ready()) return {};
    if (!valid_rect(clip)) {
        impl.fail(invalid_argument("Layer clip must be finite and nonnegative"));
        return {};
    }
    const CanvasLayer layer{impl.current->batches.size(), impl.layer_batches.size(),
                            impl.clips.size(), true};
    // Overlay layers are bounded by the root surface, not the enclosing scope.
    impl.clips.push_back(intersect(impl.clips.front(), clip));
    impl.merge_barrier = true;
    return layer;
}

void CanvasAccess::end_layer(Canvas& canvas, CanvasLayer layer) {
    auto& impl = *canvas.impl_;
    if (!impl.ready() || !layer.open) return;
    auto& batches = impl.current->batches;
    if (layer.first_batch > batches.size() || layer.stash_position > impl.layer_batches.size() ||
        impl.clips.size() != layer.clip_depth + 1) {
        impl.fail({ErrorCode::unbalanced_scope, "Invalid canvas layer boundary"});
        return;
    }
    impl.clips.pop_back();
    const auto first = static_cast<std::ptrdiff_t>(layer.first_batch);
    const auto stash = static_cast<std::ptrdiff_t>(layer.stash_position);
    // Inner layers closed earlier were stashed after this position and stay
    // above this layer's own paint.
    impl.layer_batches.insert(impl.layer_batches.begin() + stash,
                              batches.begin() + first, batches.end());
    impl.layer_pending.insert(impl.layer_pending.begin() + stash,
                              impl.pending_batches.begin() + first, impl.pending_batches.end());
    batches.erase(batches.begin() + first, batches.end());
    impl.pending_batches.erase(impl.pending_batches.begin() + first, impl.pending_batches.end());
    impl.merge_barrier = true;
}

void CanvasAccess::flush_layers(Canvas& canvas) {
    auto& impl = *canvas.impl_;
    if (impl.ready()) impl.flush_layers();
}

void CanvasAccess::clear_choice_selection(Canvas& canvas, Id id) {
    auto& impl = *canvas.impl_;
    if (!impl.ready()) return;
    const auto active = std::min(impl.semantic_count, impl.current->semantics.size());
    for (std::size_t i = 0; i < active; ++i) {
        auto& entry = impl.current->semantics[i];
        if (entry.id == id) {
            entry.selected = false;
            entry.value.clear();
            break;
        }
    }
}
void CanvasAccess::sync_focus(Canvas& canvas, Id focused) {
    auto& impl = *canvas.impl_;
    if (!impl.ready()) {
        return;
    }
    const std::size_t active =
        std::min(impl.semantic_count, impl.current->semantics.size());
    for (std::size_t i = 0; i < active; ++i) {
        auto& semantic = impl.current->semantics[i];
        if (semantic.role == Role::button || semantic.role == Role::checkbox ||
            semantic.role == Role::tab || semantic.role == Role::radio ||
            semantic.role == Role::dropdown || semantic.role == Role::slider ||
            semantic.role == Role::text_input || semantic.role == Role::numeric_input ||
            semantic.role == Role::text_area || semantic.role == Role::grid ||
            semantic.role == Role::grid_header) {
            semantic.focused = focused.value != 0 && semantic.enabled &&
                               semantic.id == focused;
        }
    }
}

} // namespace detail

} // namespace wui
