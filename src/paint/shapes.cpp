#include <wui/canvas.hpp>

#include "../core/float_bits.hpp"
#include "canvas_access.hpp"
#include "glyph_quad.hpp"
#include "shapes_detail.hpp"
#include "simd.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
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
// Requires x, y, w, h, x + w and y + h to be finite. An infinite or NaN
// operand always makes a sum infinite or NaN, so the two edge sums being
// finite already implies that all four components are. Tested branch-free.
bool finite(Rect value) {
    return ((detail::nonfinite_bits(value.x + value.w) |
             detail::nonfinite_bits(value.y + value.h)) &
            detail::nonfinite_flag) == 0;
}
// Each channel in [0, 1]. These comparisons are false for NaN and for both
// infinities, so they also establish finiteness. Bitwise & instead of &&
// evaluates all of them without a branch per comparison.
bool valid_color(Color value) {
    return (value.r >= 0.0f) & (value.r <= 1.0f) & (value.g >= 0.0f) & (value.g <= 1.0f) &
           (value.b >= 0.0f) & (value.b <= 1.0f) & (value.a >= 0.0f) & (value.a <= 1.0f);
}
// premultiply() for a color that passed valid_color(): its clamping is then
// the identity, so this is the same product without the call and re-check.
// Every canvas entry point validates its colors before tessellating.
Color premultiply_valid(Color value) {
    assert(valid_color(value));
    return {value.r * value.a, value.g * value.a, value.b * value.a, value.a};
}
bool valid_rect(Rect value) {
    return finite(value) && ((value.w >= 0.0f) & (value.h >= 0.0f));
}
bool equal(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 subtract(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 multiply(Vec2 value, float scalar) { return {value.x * scalar, value.y * scalar}; }
float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
// Euclidean length without a libm call. Float squares are exact in double and
// cannot overflow there, so one correctly rounded double square root narrowed
// to float equals std::hypot (glibc's hypotf computes exactly this; verified
// bit for bit on 3e8 inputs) for finite components and for the infinities an
// overflowing subtraction of finite coordinates can produce.
float length(Vec2 value) {
    const double x = value.x;
    const double y = value.y;
    return static_cast<float>(std::sqrt(x * x + y * y));
}
Vec2 normalized(Vec2 value) {
    const auto magnitude = length(value);
    return magnitude > epsilon ? multiply(value, 1.0f / magnitude) : Vec2{};
}
bool near(Vec2 a, Vec2 b) {
    const auto delta = subtract(a, b);
    // length() never rounds below the magnitude of either component, so this
    // exact early-out skips the square root for all but coincident points.
    if (std::abs(delta.x) > epsilon || std::abs(delta.y) > epsilon) {
        return false;
    }
    return length(delta) <= epsilon;
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

constexpr int min_corner_segments = 3;
constexpr int max_corner_segments = 32;

// Unit-circle points of the corner arcs for every segment count that
// rounded_outline can choose: for n segments, the n - 1 interior points of
// corners 0..3 (in that order) start at offset[n]. Entries use the float
// angle expression rounded_outline used to evaluate per point, so sin and
// cos no longer run for every corner point of every rounded shape. The table
// is built once; compilers that evaluate the initializer at compile time
// (GCC does) store correctly rounded values, which can differ from a
// runtime sinf/cosf by one ulp (37 of the 3960 coordinates with glibc).
struct CornerArcs {
    static constexpr std::size_t point_count = [] {
        std::size_t count = 0;
        for (int segments = min_corner_segments; segments <= max_corner_segments; ++segments) {
            count += 4 * static_cast<std::size_t>(segments - 1);
        }
        return count;
    }();
    std::array<std::size_t, max_corner_segments + 1> offset{};
    std::array<Vec2, point_count> points{};
};

const CornerArcs& corner_arcs() {
    static const CornerArcs arcs = [] {
        CornerArcs result;
        constexpr std::array starts{
            std::numbers::pi_v<float>,
            std::numbers::pi_v<float> * 1.5f,
            0.0f,
            std::numbers::pi_v<float> * 0.5f,
        };
        std::size_t next = 0;
        for (int segments = min_corner_segments; segments <= max_corner_segments; ++segments) {
            result.offset[static_cast<std::size_t>(segments)] = next;
            for (std::size_t corner = 0; corner < starts.size(); ++corner) {
                for (int segment = 1; segment < segments; ++segment) {
                    const float angle = starts[corner] +
                                        (std::numbers::pi_v<float> * 0.5f) *
                                            static_cast<float>(segment) /
                                            static_cast<float>(segments);
                    result.points[next++] = {std::cos(angle), std::sin(angle)};
                }
            }
        }
        return result;
    }();
    return arcs;
}

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
        min_corner_segments, max_corner_segments);
    // Room for every candidate point, written through a cursor (no capacity
    // check per point) and trimmed afterwards. Consecutive near-duplicates
    // are skipped exactly as append_unique would skip them.
    out.resize(static_cast<std::size_t>((segments + 1) * 4));
    Vec2* const first = out.data();
    Vec2* last = first; // one past the most recently kept point
    const auto keep = [first, &last](Vec2 point) {
        if (last == first || !near(last[-1], point)) {
            *last++ = point;
        }
    };
    const std::array centers{
        Vec2{bounds.x + radius, bounds.y + radius},
        Vec2{bounds.x + bounds.w - radius, bounds.y + radius},
        Vec2{bounds.x + bounds.w - radius, bounds.y + bounds.h - radius},
        Vec2{bounds.x + radius, bounds.y + bounds.h - radius},
    };
    // Exact cardinal endpoints prevent near-duplicate edges when adjacent
    // corner centers coincide (circles and capsules).
    constexpr std::array cardinal{Vec2{-1, 0}, Vec2{0, -1}, Vec2{1, 0}, Vec2{0, 1}};
    const auto& arcs = corner_arcs();
    const Vec2* arc = arcs.points.data() + arcs.offset[static_cast<std::size_t>(segments)];
    for (std::size_t corner = 0; corner < centers.size(); ++corner) {
        const auto center = centers[corner];
        keep(add(center, multiply(cardinal[corner], radius)));
        for (int segment = 1; segment < segments; ++segment, ++arc) {
            keep({center.x + arc->x * radius, center.y + arc->y * radius});
        }
        keep(add(center, multiply(cardinal[(corner + 1) % 4], radius)));
    }
    out.resize(static_cast<std::size_t>(last - first));
    remove_duplicate_closure(out);
}

double signed_area(std::span<const Vec2> points) {
    if (points.size() < 3) {
        return 0;
    }
    double area = 0.0;
    const auto origin = points.front();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto j = i + 1 == points.size() ? 0 : i + 1;
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
    if (points.empty()) {
        return;
    }
    out.resize(points.size());
    const auto edge_normal = [orientation](Vec2 from, Vec2 to) {
        const auto edge = normalized(subtract(to, from));
        return multiply(Vec2{edge.y, -edge.x}, orientation);
    };
    // Each edge normal serves the corners at both of its ends: the outgoing
    // normal of one corner is carried over as the incoming one of the next.
    auto previous_normal = edge_normal(points.back(), points.front());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto current = points[i];
        const auto next = points[i + 1 == points.size() ? 0 : i + 1];
        const auto next_normal = edge_normal(current, next);
        auto miter = normalized(add(previous_normal, next_normal));
        float distance = fringe;
        const float denominator = dot(miter, next_normal);
        if (std::abs(denominator) > epsilon) {
            distance = fringe / denominator;
        } else {
            miter = next_normal;
        }
        distance = std::clamp(distance, -2.0f * fringe, 2.0f * fringe);
        out[i] = add(current, multiply(miter, distance));
        previous_normal = next_normal;
    }
}

// Cross-frame memo of rounded outlines and their antialiasing fringes.
// rounded_outline and fringe_outline are pure functions of the exact
// bounds, radius and scale, and immediate-mode UIs repaint mostly the same
// rectangles every frame, so a recorded shape is reused bit for bit instead
// of being tessellated again. Two generations alternate: lookups see the
// recording generation and the previous one, and a shape found only in the
// previous generation is (re)recorded into the recording one. rotate(),
// called at the start of each frame, starts a new recording generation
// (reusing the storage of the one before the previous) only once the
// recording generation is mostly stale: when shapes used in the frame that
// just ended account for less than half of its points, or when it refused a
// shape for lack of room. A UI that repaints the same shapes therefore keeps
// finding them in the recording generation and copies nothing, while
// shapes that stop being drawn are dropped within two rotations. Storage
// grows only when a generation records more than it held before (warm-up);
// beyond the per-generation bounds in shapes_detail.hpp shapes are simply
// not recorded and are tessellated every time (and the next frame starts a
// fresh generation). Storage is at most 8 MiB of points and 2.4 MiB of slots
// per generation.
class OutlineCache {
public:
    static constexpr std::size_t max_entries = detail::outline_cache_entry_limit;
    static constexpr std::size_t max_points = detail::outline_cache_point_limit; // outline + fringe

    struct Key {
        std::array<std::uint32_t, 6> bits{};
        bool operator==(const Key&) const = default;
    };
    struct Shape {
        std::span<const Vec2> outline;
        std::span<const Vec2> fringe; // same length as outline
    };

    static Key key(Rect bounds, float radius, float scale) {
        // Exact bits: equal keys mean identical tessellation inputs.
        return {{std::bit_cast<std::uint32_t>(bounds.x), std::bit_cast<std::uint32_t>(bounds.y),
                 std::bit_cast<std::uint32_t>(bounds.w), std::bit_cast<std::uint32_t>(bounds.h),
                 std::bit_cast<std::uint32_t>(radius), std::bit_cast<std::uint32_t>(scale)}};
    }
    static std::uint64_t hash(const Key& key) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < key.bits.size(); i += 2) {
            const auto word = key.bits[i] | (std::uint64_t{key.bits[i + 1]} << 32);
            value = (value ^ word) * 0x9e3779b97f4a7c15ull;
            value ^= value >> 32;
        }
        return value;
    }

    // Called at the start of each frame.
    void rotate() noexcept {
        const auto& recording = generations_[recording_];
        const bool stale = recording.stamp == 0 || recording.refused ||
                           recording.used > 2 * live_points_;
        live_points_ = 0;
        // A new frame needs a value no slot's `frame` holds. After 2^32
        // frames one can repeat; that shape then goes uncounted in
        // live_points_ for a frame, which at most starts a generation early.
        ++frame_;
        if (!stale) {
            return;
        }
        std::swap(recording_, previous_);
        auto& next = generations_[recording_];
        next.used = 0;
        next.entries = 0;
        next.refused = false;
        if (++stamp_ == 0) { // after 2^32 frames: forget every slot explicitly
            for (auto& generation : generations_) {
                for (auto& slot : generation.slots) slot.stamp = 0;
            }
            generations_[previous_].stamp = 0;
            stamp_ = 1;
        }
        next.stamp = stamp_;
    }

    // The shape recorded for `key` in the recording or the previous
    // generation. A shape from the previous generation is carried into the
    // recording one (when within bounds) so that it stays available.
    std::optional<Shape> find(const Key& key, std::uint64_t key_hash) {
        auto& current = generations_[recording_];
        if (auto* slot = lookup(current, key, key_hash)) {
            if (slot->frame != frame_) {
                slot->frame = frame_;
                live_points_ += std::size_t{slot->count} * 2;
            }
            return view(current, *slot);
        }
        auto& previous = generations_[previous_];
        if (const auto* slot = lookup(previous, key, key_hash)) {
            const auto shape = view(previous, *slot);
            if (const auto carried = record(key, key_hash, shape.outline, shape.fringe)) {
                return carried;
            }
            return shape; // stays valid until the next rotate()
        }
        return std::nullopt;
    }

    // Copies a freshly tessellated shape into the recording generation and
    // returns the stored copy, or nothing beyond the per-generation bounds.
    std::optional<Shape> record(const Key& key, std::uint64_t key_hash,
                                std::span<const Vec2> outline, std::span<const Vec2> fringe) {
        auto& current = generations_[recording_];
        const auto needed = outline.size() * 2;
        if (outline.size() != fringe.size()) {
            return std::nullopt;
        }
        if (current.entries >= max_entries || current.used + needed > max_points) {
            current.refused = true;
            return std::nullopt;
        }
        if ((current.entries + 1) * 2 > current.slots.size()) {
            grow_slots(current);
        }
        if (current.used + needed > current.capacity) {
            grow_points(current, current.used + needed);
        }
        const auto offset = current.used;
        std::copy(fringe.begin(), fringe.end(),
                  std::copy(outline.begin(), outline.end(), current.points.get() + offset));
        current.used += needed;
        auto* slot = probe(current, key, key_hash);
        *slot = {key, static_cast<std::uint32_t>(offset),
                 static_cast<std::uint32_t>(outline.size()), current.stamp, frame_};
        ++current.entries;
        live_points_ += needed;
        return view(current, *slot);
    }

private:
    struct Slot {
        Key key;
        std::uint32_t offset = 0, count = 0; // count outline points, then as many fringe points
        std::uint32_t stamp = 0;             // live while equal to its generation's stamp
        std::uint32_t frame = 0;             // the last frame that used the shape
    };
    struct Generation {
        // Raw storage rather than a vector: appending is a plain copy, with
        // no per-append construction of Vec2's default member initializers.
        std::unique_ptr<Vec2[]> points;
        std::size_t capacity = 0, used = 0;
        std::vector<Slot> slots; // open addressing; size is zero or a power of two
        std::size_t entries = 0;
        std::uint32_t stamp = 0;
        bool refused = false; // a shape did not fit since the generation started
    };

    static Shape view(const Generation& generation, const Slot& slot) {
        const auto* first = generation.points.get() + slot.offset;
        return {{first, slot.count}, {first + slot.count, slot.count}};
    }
    // The live slot holding `key`, or the first free slot of its probe run.
    static Slot* probe(Generation& generation, const Key& key, std::uint64_t key_hash) {
        const auto mask = generation.slots.size() - 1;
        for (auto index = static_cast<std::size_t>(key_hash) & mask;;
             index = (index + 1) & mask) {
            auto& slot = generation.slots[index];
            if (slot.stamp != generation.stamp || slot.key == key) return &slot;
        }
    }
    static Slot* lookup(Generation& generation, const Key& key, std::uint64_t key_hash) {
        if (generation.entries == 0 || generation.stamp == 0) return nullptr;
        auto* slot = probe(generation, key, key_hash);
        return slot->stamp == generation.stamp ? slot : nullptr;
    }
    // Doubles the table (kept at most half full) and re-inserts live slots.
    static void grow_slots(Generation& generation) {
        std::vector<Slot> slots(std::max<std::size_t>(64, generation.slots.size() * 2));
        std::swap(slots, generation.slots);
        for (const auto& slot : slots) {
            if (slot.stamp == generation.stamp) {
                *probe(generation, slot.key, hash(slot.key)) = slot;
            }
        }
    }
    static void grow_points(Generation& generation, std::size_t needed) {
        const auto capacity =
            std::min(max_points, std::max({needed, generation.capacity * 2, std::size_t{1024}}));
        auto points = std::make_unique<Vec2[]>(capacity);
        std::copy(generation.points.get(), generation.points.get() + generation.used,
                  points.get());
        generation.points = std::move(points);
        generation.capacity = capacity;
    }

    std::array<Generation, 2> generations_;
    std::size_t recording_ = 0, previous_ = 1;
    std::uint32_t stamp_ = 0;
    std::uint32_t frame_ = 0;
    // Points of the recording generation's shapes used in the current frame.
    std::size_t live_points_ = 0;
};

// Cross-frame memo of the meshes Canvas::line and Canvas::polygon build.
// Their outline, fringe and interior triangles are pure functions of the
// exact deduplicated input points, the stroke half width (lines) and the
// scale, and the validation and tessellation that produce them (simple
// outline tests, ear clipping, round joins and caps) are quadratic in the
// outline size, so a mesh that was accepted earlier is reused bit for bit.
// Only accepted meshes are recorded: invalid polygons and strokes that fall
// back to coverage rasters take their full path every time. Generations
// work as in OutlineCache; requests are compared by their exact bits.
class MeshCache {
public:
    static constexpr std::size_t max_entries = detail::mesh_cache_entry_limit;
    static constexpr std::size_t max_points = detail::mesh_cache_point_limit;
    static constexpr std::size_t max_indices = detail::mesh_cache_index_limit;

    enum class Kind : std::uint32_t { polygon, line };
    struct Request {
        Kind kind = Kind::polygon;
        std::span<const Vec2> points;
        float half_width = 0; // lines only
        float scale = 1;
    };
    struct Mesh {
        std::span<const Vec2> outline;
        std::span<const Vec2> fringe; // same length as outline
        std::span<const std::uint32_t> interior;
    };

    static std::uint64_t hash(const Request& request) {
        std::uint64_t value = (std::uint64_t{static_cast<std::uint32_t>(request.kind)} << 32) ^
                              request.points.size();
        const auto mix = [&value](std::uint64_t word) {
            value = (value ^ word) * 0x9e3779b97f4a7c15ull;
            value ^= value >> 32;
        };
        mix(std::bit_cast<std::uint32_t>(request.half_width) |
            (std::uint64_t{std::bit_cast<std::uint32_t>(request.scale)} << 32));
        for (const auto point : request.points) {
            mix(std::bit_cast<std::uint64_t>(point));
        }
        return value;
    }

    // Called at the start of each frame; same policy as OutlineCache.
    void rotate() noexcept {
        const auto& recording = generations_[recording_];
        const bool stale = recording.stamp == 0 || recording.refused ||
                           recording.used_points > 2 * live_points_;
        live_points_ = 0;
        ++frame_;
        if (!stale) {
            return;
        }
        std::swap(recording_, previous_);
        auto& next = generations_[recording_];
        next.used_points = next.used_indices = next.entries = 0;
        next.refused = false;
        if (++stamp_ == 0) { // after 2^32 rotations: forget every slot explicitly
            for (auto& generation : generations_) {
                for (auto& slot : generation.slots) slot.stamp = 0;
            }
            generations_[previous_].stamp = 0;
            stamp_ = 1;
        }
        next.stamp = stamp_;
    }

    // The mesh recorded for `request` in the recording or the previous
    // generation; one from the previous generation is carried over.
    std::optional<Mesh> find(const Request& request, std::uint64_t request_hash) {
        auto& current = generations_[recording_];
        if (auto* slot = lookup(current, request, request_hash)) {
            if (slot->frame != frame_) {
                slot->frame = frame_;
                live_points_ += slot->key_count + std::size_t{slot->outline_count} * 2;
            }
            return view(current, *slot);
        }
        auto& previous = generations_[previous_];
        if (const auto* slot = lookup(previous, request, request_hash)) {
            const auto mesh = view(previous, *slot);
            if (const auto carried =
                    record(request, request_hash, mesh.outline, mesh.fringe, mesh.interior)) {
                return carried;
            }
            return mesh; // stays valid until the next rotate()
        }
        return std::nullopt;
    }

    // Copies an accepted mesh into the recording generation and returns the
    // stored copy, or nothing beyond the per-generation bounds.
    std::optional<Mesh> record(const Request& request, std::uint64_t request_hash,
                               std::span<const Vec2> outline, std::span<const Vec2> fringe,
                               std::span<const std::uint32_t> interior) {
        auto& current = generations_[recording_];
        const auto points = request.points.size() + outline.size() * 2;
        if (outline.size() != fringe.size()) {
            return std::nullopt;
        }
        if (current.entries >= max_entries || current.used_points + points > max_points ||
            current.used_indices + interior.size() > max_indices) {
            current.refused = true;
            return std::nullopt;
        }
        if ((current.entries + 1) * 2 > current.slots.size()) {
            grow_slots(current);
        }
        grow(current.points, current.point_capacity, current.used_points,
             current.used_points + points, max_points);
        grow(current.indices, current.index_capacity, current.used_indices,
             current.used_indices + interior.size(), max_indices);
        Slot slot;
        slot.hash = request_hash;
        slot.kind = static_cast<std::uint32_t>(request.kind);
        slot.half_width = std::bit_cast<std::uint32_t>(request.half_width);
        slot.scale = std::bit_cast<std::uint32_t>(request.scale);
        slot.point_offset = static_cast<std::uint32_t>(current.used_points);
        slot.key_count = static_cast<std::uint32_t>(request.points.size());
        slot.outline_count = static_cast<std::uint32_t>(outline.size());
        slot.index_offset = static_cast<std::uint32_t>(current.used_indices);
        slot.index_count = static_cast<std::uint32_t>(interior.size());
        slot.stamp = current.stamp;
        slot.frame = frame_;
        Vec2* out = current.points.get() + current.used_points;
        out = std::copy(request.points.begin(), request.points.end(), out);
        out = std::copy(outline.begin(), outline.end(), out);
        std::copy(fringe.begin(), fringe.end(), out);
        std::copy(interior.begin(), interior.end(), current.indices.get() + current.used_indices);
        current.used_points += points;
        current.used_indices += interior.size();
        auto* stored = probe(current, request, request_hash);
        *stored = slot;
        ++current.entries;
        live_points_ += points;
        return view(current, *stored);
    }

private:
    struct Slot {
        std::uint64_t hash = 0;
        std::uint32_t kind = 0, half_width = 0, scale = 0; // request bits
        // The request's points, then outline_count outline and fringe points.
        std::uint32_t point_offset = 0, key_count = 0, outline_count = 0;
        std::uint32_t index_offset = 0, index_count = 0;
        std::uint32_t stamp = 0; // live while equal to its generation's stamp
        std::uint32_t frame = 0; // the last frame that used the mesh
    };
    struct Generation {
        std::unique_ptr<Vec2[]> points;
        std::unique_ptr<std::uint32_t[]> indices;
        std::size_t point_capacity = 0, used_points = 0;
        std::size_t index_capacity = 0, used_indices = 0;
        std::vector<Slot> slots; // open addressing; size is zero or a power of two
        std::size_t entries = 0;
        std::uint32_t stamp = 0;
        bool refused = false; // a mesh did not fit since the generation started
    };

    static Mesh view(const Generation& generation, const Slot& slot) {
        const auto* outline = generation.points.get() + slot.point_offset + slot.key_count;
        return {{outline, slot.outline_count},
                {outline + slot.outline_count, slot.outline_count},
                {generation.indices.get() + slot.index_offset, slot.index_count}};
    }
    static bool matches(const Generation& generation, const Slot& slot, const Request& request,
                        std::uint64_t request_hash) {
        if (slot.hash != request_hash || slot.kind != static_cast<std::uint32_t>(request.kind) ||
            slot.half_width != std::bit_cast<std::uint32_t>(request.half_width) ||
            slot.scale != std::bit_cast<std::uint32_t>(request.scale) ||
            slot.key_count != request.points.size()) {
            return false;
        }
        const auto* key = generation.points.get() + slot.point_offset;
        return std::equal(request.points.begin(), request.points.end(), key,
                          [](Vec2 a, Vec2 b) {
                              return std::bit_cast<std::uint64_t>(a) ==
                                     std::bit_cast<std::uint64_t>(b);
                          });
    }
    // The live slot holding `request`, or the first free slot of its probe run.
    static Slot* probe(Generation& generation, const Request& request,
                       std::uint64_t request_hash) {
        const auto mask = generation.slots.size() - 1;
        for (auto index = static_cast<std::size_t>(request_hash) & mask;;
             index = (index + 1) & mask) {
            auto& slot = generation.slots[index];
            if (slot.stamp != generation.stamp || matches(generation, slot, request, request_hash)) {
                return &slot;
            }
        }
    }
    static Slot* lookup(Generation& generation, const Request& request,
                        std::uint64_t request_hash) {
        if (generation.entries == 0 || generation.stamp == 0) return nullptr;
        auto* slot = probe(generation, request, request_hash);
        return slot->stamp == generation.stamp ? slot : nullptr;
    }
    // Doubles the table (kept at most half full) and re-inserts live slots,
    // which are distinct requests: each goes to the first free slot of its run.
    static void grow_slots(Generation& generation) {
        std::vector<Slot> slots(std::max<std::size_t>(16, generation.slots.size() * 2));
        std::swap(slots, generation.slots);
        const auto mask = generation.slots.size() - 1;
        for (const auto& slot : slots) {
            if (slot.stamp != generation.stamp) continue;
            auto index = static_cast<std::size_t>(slot.hash) & mask;
            while (generation.slots[index].stamp == generation.stamp) index = (index + 1) & mask;
            generation.slots[index] = slot;
        }
    }
    template <typename T>
    static void grow(std::unique_ptr<T[]>& storage, std::size_t& capacity, std::size_t used,
                     std::size_t needed, std::size_t limit) {
        if (needed <= capacity) return;
        const auto next = std::min(limit, std::max({needed, capacity * 2, std::size_t{256}}));
        auto grown = std::make_unique<T[]>(next);
        std::copy(storage.get(), storage.get() + used, grown.get());
        storage = std::move(grown);
        capacity = next;
    }

    std::array<Generation, 2> generations_;
    std::size_t recording_ = 0, previous_ = 1;
    std::uint32_t stamp_ = 0;
    std::uint32_t frame_ = 0;
    // Points of the recording generation's meshes used in the current frame.
    std::size_t live_points_ = 0;
};

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

namespace {

// Vertices at `points`, each with uv (0.5, 0.5) and `color`; returns the
// cursor past them. Four per iteration share the loop overhead, which is
// otherwise as large as the stores themselves.
Vertex* put_vertices(Vertex* out, std::span<const Vec2> points, Color color) {
    const Vertex pattern{{}, {0.5f, 0.5f}, color};
    const Vec2* point = points.data();
    const Vec2* const end = point + points.size();
    for (; end - point >= 4; point += 4, out += 4) {
        for (std::size_t k = 0; k < 4; ++k) {
            out[k] = pattern;
            out[k].position = point[k];
        }
    }
    for (; point != end; ++point, ++out) {
        *out = pattern;
        out->position = *point;
    }
    return out;
}

// Index sequences that repeat with a fixed per-lane step, a block at a time:
// `blocks` copies of `values`, every lane advancing by its `step` between
// copies. Twelve lanes are three 128-bit vectors kept in registers, so a
// block costs three additions and three stores instead of recomputing every
// index (explicitly with SSE2, see simd.hpp, and otherwise as compilers
// vectorize the lane loops).
using IndexBlock = std::array<std::uint32_t, 12>;
std::uint32_t* put_blocks(std::uint32_t* out, const IndexBlock& values, const IndexBlock& step,
                          std::size_t blocks) {
#ifdef WUI_PAINT_SSE2
    const auto load = [](const IndexBlock& lanes, std::size_t at) {
        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(lanes.data() + at));
    };
    __m128i first = load(values, 0), second = load(values, 4), third = load(values, 8);
    const __m128i first_step = load(step, 0), second_step = load(step, 4),
                  third_step = load(step, 8);
    for (std::size_t block = 0; block < blocks; ++block, out += 12) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out), first);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 4), second);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 8), third);
        first = _mm_add_epi32(first, first_step);
        second = _mm_add_epi32(second, second_step);
        third = _mm_add_epi32(third, third_step);
    }
#else
    IndexBlock lanes = values;
    for (std::size_t block = 0; block < blocks; ++block, out += 12) {
        for (std::size_t k = 0; k < lanes.size(); ++k) {
            out[k] = lanes[k];
        }
        for (std::size_t k = 0; k < lanes.size(); ++k) {
            lanes[k] += step[k];
        }
    }
#endif
    return out;
}

} // namespace

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
        vertex_size = index_size = 0;
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

    // Shapes write their geometry straight into the retained packet buffers
    // rather than building it in scratch storage and copying it over.
    // begin_mesh() checks the frame, makes room for exactly the requested
    // counts after the frame's vertices and indices and returns write cursors
    // into that room; the shape must fill every slot (indices offset by
    // `base`) and then call end_mesh() with the final cursors, which records
    // the new index range as a batch or extends the previous one. Writing
    // through cursors avoids a capacity check per element.
    //
    // During a frame the packet vectors are retained storage: vertex_size and
    // index_size are the frame's logical sizes, and the vectors' own sizes
    // only ever grow, so every element past the logical size is already
    // constructed and is simply overwritten. A slot reused for a frame no
    // larger than the one it last published therefore constructs (zeroes)
    // nothing; only growth beyond that constructs new elements. finish()
    // trims both vectors to the logical sizes before publishing.
    struct Mesh {
        Vertex* vertex = nullptr;       // next vertex slot to write
        std::uint32_t* index = nullptr; // next index slot to write
        std::uint32_t base = 0;         // packet index of the mesh's first vertex
        std::uint32_t first_index = 0;  // packet index of the mesh's first index
    };

    // Constructed room for `count` elements after the first `used` ones of
    // `values` (whose size is at least `used`). Only reserve can throw, and
    // it does so before anything changes; resizing within capacity cannot.
    template <typename T>
    static T* writable(std::vector<T>& values, std::size_t used, std::size_t count) {
        if (values.size() - used < count) {
            if (values.capacity() - used < count) {
                // Geometric growth, as insert() would apply, keeps warm-up amortized.
                values.reserve(used + std::max(used, count));
            }
            values.resize(used + count);
        }
        return values.data() + used;
    }

    // The frame checks shared by every mesh. False, having appended nothing,
    // when the canvas cannot take paint or the counts would overflow the
    // packet's 32-bit index space (which fails the frame).
    bool mesh_allowed(std::size_t vertex_count, std::size_t index_count) {
        if (!ready() || clips.back().w == 0.0f || clips.back().h == 0.0f) {
            return false;
        }
        if (vertex_size > std::numeric_limits<std::uint32_t>::max() - vertex_count ||
            index_size > std::numeric_limits<std::uint32_t>::max() - index_count) {
            fail({ErrorCode::capacity, "Canvas geometry exceeds packet limits"});
            return false;
        }
        return true;
    }

    // Room for a mesh whose counts mesh_allowed() accepted; the logical sizes
    // advance only once both buffers have room, so a throwing reserve leaves
    // the frame unchanged.
    Mesh claim_mesh(std::size_t vertex_count, std::size_t index_count) {
        Vertex* const vertex = writable(current->vertices, vertex_size, vertex_count);
        std::uint32_t* const index = writable(current->indices, index_size, index_count);
        const Mesh mesh{vertex, index, static_cast<std::uint32_t>(vertex_size),
                        static_cast<std::uint32_t>(index_size)};
        vertex_size += vertex_count;
        index_size += index_count;
        return mesh;
    }

    std::optional<Mesh> begin_mesh(std::size_t vertex_count, std::size_t index_count) {
        if (!mesh_allowed(vertex_count, index_count)) {
            return std::nullopt;
        }
        return claim_mesh(vertex_count, index_count);
    }

    // Records indices [first_index, index_size) as a batch or extends the previous one.
    void end_mesh(std::uint32_t first_index, TextureHandle texture) {
        const auto index_count = static_cast<std::uint32_t>(index_size - first_index);
        auto& batches = current->batches;
        if (!merge_barrier && !batches.empty() &&
            pending_batches.back().texture == texture &&
            equal(batches.back().clip, clips.back()) &&
            batches.back().first_index + batches.back().index_count == first_index) {
            batches.back().index_count += index_count;
        } else {
            batches.push_back({first_index, index_count, 0, 0, clips.back()});
            pending_batches.push_back({texture});
        }
        merge_barrier = false;
    }

    // `filled` is the mesh with its cursors advanced past the written slots.
    void end_mesh(const Mesh& filled, TextureHandle texture) {
        assert(filled.vertex == current->vertices.data() + vertex_size &&
               filled.index == current->indices.data() + index_size &&
               "a shape must write every vertex and index slot it requested");
        end_mesh(filled.first_index, texture);
    }

    template <std::size_t N>
    static void put(std::uint32_t*& cursor, const std::array<std::uint32_t, N>& values) {
        for (const auto value : values) {
            *cursor++ = value;
        }
    }

    // Two triangles over four corners given in winding order.
    void quad_unchecked(const std::array<Vertex, 4>& corners, TextureHandle texture) {
        auto mesh = begin_mesh(4, 6);
        if (!mesh) {
            return;
        }
        std::copy(corners.begin(), corners.end(), mesh->vertex);
        const auto b = mesh->base;
        put(mesh->index, std::array{b, b + 1, b + 2, b, b + 2, b + 3});
        end_mesh(mesh->first_index, texture);
    }

    void image_unchecked(Rect bounds, TextureHandle texture, Rect uv, Color tint) {
        const auto color = premultiply_valid(tint);
        quad_unchecked({
                           Vertex{{bounds.x, bounds.y}, {uv.x, uv.y}, color},
                           Vertex{{bounds.x + bounds.w, bounds.y}, {uv.x + uv.w, uv.y}, color},
                           Vertex{{bounds.x + bounds.w, bounds.y + bounds.h},
                                  {uv.x + uv.w, uv.y + uv.h}, color},
                           Vertex{{bounds.x, bounds.y + bounds.h}, {uv.x, uv.y + uv.h}, color},
                       },
                       texture);
    }

    // Canvas::image(glyph.bounds moved by offset, glyph.texture, glyph.uv,
    // tint) for every glyph in order, in one pass: the tint is validated and
    // premultiplied once, room for every glyph is made once, and each
    // texture run is recorded with one end_mesh() call. Returns false,
    // having done nothing, when the run's worst case would overflow the
    // packet's index space; the caller then takes the per-glyph path, which
    // fails at exactly the glyph where the overflow happens. The caller does
    // the same when making room throws, which happens before anything
    // changes (see CanvasAccess::glyph_run).
    //
    // Equivalence with the per-glyph path, which validates the bounds, uv
    // and tint of each glyph (one shared error), then skips empty glyphs
    // and empty clips, then appends one quad merged into the previous batch
    // when the texture, clip and index range allow:
    //  - an invalid tint fails at the first glyph, like an invalid rect;
    //  - glyphs are validated in order; the first invalid one fails the
    //    canvas after the glyphs before it have been appended, and the
    //    failed canvas ignores the rest;
    //  - under an empty clip glyphs are still validated but none is drawn;
    //  - within a run the clip is fixed, the index range is contiguous and
    //    no merge barrier arises, so consecutive glyphs with equal textures
    //    land in one batch, which joins the batch before the run exactly
    //    when the first appended quad would have.
    // Elements written for the room of glyphs that end up skipped stay
    // beyond the logical size, as retained storage.
    bool glyph_run(std::span<const GlyphQuad> glyphs, Vec2 offset, Color tint) {
        if (!ready() || glyphs.empty()) {
            return true;
        }
        if (!valid_color(tint)) {
            fail(invalid_argument("Image input must be finite and valid"));
            return true;
        }
        if (clips.back().w == 0.0f || clips.back().h == 0.0f) {
            for (const auto& glyph : glyphs) {
                const Rect bounds{glyph.bounds.x + offset.x, glyph.bounds.y + offset.y,
                                  glyph.bounds.w, glyph.bounds.h};
                if (!valid_rect(bounds) || !valid_rect(glyph.uv)) {
                    fail(invalid_argument("Image input must be finite and valid"));
                    break;
                }
            }
            return true;
        }
        constexpr std::size_t limit = std::numeric_limits<std::uint32_t>::max();
        if (glyphs.size() > (limit - vertex_size) / 4 || limit - index_size < 2 ||
            glyphs.size() > (limit - index_size - 2) / 6) {
            return false;
        }
        Vertex* const first_vertex =
            writable(current->vertices, vertex_size, glyphs.size() * 4);
        std::uint32_t* const first_slot =
            writable(current->indices, index_size, glyphs.size() * 6 + 2);
        const auto color = premultiply_valid(tint);
        Vertex* vertex = first_vertex;
        std::uint32_t* index = first_slot;
        // The texture run being written starts at the logical sizes, which
        // advance as each run is recorded (as they would quad by quad).
        TextureHandle texture = glyphs.front().texture;
        const std::size_t vertex_start = vertex_size;
        const std::size_t index_start = index_size;
        const auto record_run = [&] {
            const std::size_t run_end = index_start + static_cast<std::size_t>(index - first_slot);
            if (run_end != index_size) {
                const auto run_first = static_cast<std::uint32_t>(index_size);
                vertex_size = vertex_start + static_cast<std::size_t>(vertex - first_vertex);
                index_size = run_end;
                end_mesh(run_first, texture);
            }
        };
        const detail::GlyphCorners corners(offset, color);
        auto b = static_cast<std::uint32_t>(vertex_size); // the next quad's first vertex
        bool invalid = false;
        for (const auto& glyph : glyphs) {
            const auto verdict = corners(glyph, vertex, index, b);
            if (verdict != detail::GlyphVerdict::quad) {
                if (verdict == detail::GlyphVerdict::invalid) {
                    invalid = true;
                    break;
                }
                continue;
            }
            if (!(glyph.texture == texture)) {
                record_run(); // the glyph's quad is not counted yet
                texture = glyph.texture;
            }
            vertex += 4;
            index += 6;
            b += 4;
        }
        record_run();
        if (invalid) {
            fail(invalid_argument("Image input must be finite and valid"));
        }
        return true;
    }

    void polygon_unchecked(std::span<const Vec2> points, Color color,
                           std::span<const std::uint32_t> interior = {},
                           std::span<const Vec2> outer = {}) {
        if (points.size() < 3 || clips.back().w == 0.0f || clips.back().h == 0.0f) {
            return;
        }
        if (outer.empty()) {
            fringe_outline(points, 1.0f / current->scale, fringe_scratch);
            outer = fringe_scratch;
        }
        const auto count = static_cast<std::uint32_t>(points.size());
        const std::size_t interior_count =
            interior.empty() ? (points.size() - 2) * 3 : interior.size();
        auto mesh =
            begin_mesh(points.size() + outer.size(), interior_count + points.size() * 6);
        if (!mesh) {
            return;
        }
        mesh->vertex = put_vertices(mesh->vertex, points, premultiply_valid(color));
        mesh->vertex = put_vertices(mesh->vertex, outer, {0, 0, 0, 0});
        std::uint32_t* index = mesh->index;
        const auto b = mesh->base;
        // Whole blocks only: a final partial block writes up to three extra
        // fan triangles over the start of the fringe ring, or one extra ring
        // edge over the closing edge's slots, and the writes that follow
        // (the ring, the closing edge) replace them. Nothing is written past
        // the mesh.
        if (interior.empty()) {
            // Fan triangles (b, b + t, b + t + 1) for t in [1, count - 1),
            // four per block.
            put_blocks(index,
                       {b, b + 1, b + 2, b, b + 2, b + 3, b, b + 3, b + 4, b, b + 4, b + 5},
                       {0, 4, 4, 0, 4, 4, 0, 4, 4, 0, 4, 4}, (count - 2 + 3) / 4);
            index += std::size_t{count - 2} * 3;
        } else {
            for (const auto local : interior) {
                *index++ = b + local;
            }
        }
        // Two fringe triangles per edge (i, next): (b + i, o + next, b + next)
        // and (b + i, o + i, o + next). Edges that do not wrap go two per
        // block; the closing edge (count - 1, 0) comes last.
        const auto o = b + count; // first outer (fringe) vertex
        put_blocks(index,
                   {b, o + 1, b + 1, b, o, o + 1, b + 1, o + 2, b + 2, b + 1, o + 1, o + 2},
                   {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, (count - 1 + 1) / 2);
        const auto last = count - 1;
        index += std::size_t{last} * 6;
        put(index, std::array{b + last, o, b, b + last, o + last, o});
        mesh->index = index;
        end_mesh(*mesh, *white);
    }

    // rounded_outline(bounds, radius, scale) and its fringe_outline, from the
    // outline cache when an identical shape was recorded earlier. The spans
    // stay valid until the next call.
    OutlineCache::Shape rounded_shape(Rect bounds, float radius) {
        const float scale = current->scale;
        const auto key = OutlineCache::key(bounds, radius, scale);
        const auto key_hash = OutlineCache::hash(key);
        if (const auto cached = outline_cache.find(key, key_hash)) {
            return *cached;
        }
        rounded_outline(bounds, radius, scale, outline_scratch);
        fringe_outline(outline_scratch, 1.0f / scale, fringe_scratch);
        if (const auto recorded =
                outline_cache.record(key, key_hash, outline_scratch, fringe_scratch)) {
            return *recorded;
        }
        return {outline_scratch, fringe_scratch};
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

    // Whether a batch recorded in this frame draws with `handle`: one in the
    // main order, or one of a closed overlay layer that end_layer parked in
    // layer_pending until flush_layers() appends it.
    bool recorded(TextureHandle handle) const {
        const auto uses = [handle](const PendingBatch& batch) { return batch.texture == handle; };
        return std::any_of(pending_batches.begin(), pending_batches.end(), uses) ||
               std::any_of(layer_pending.begin(), layer_pending.end(), uses);
    }

    Result<ShadowEntry*> shadow_texture(float radius_px, float sigma_px) {
        const ShadowKey key{radius_px, sigma_px};
        for (auto& entry : shadows) {
            if (entry.key == key) {
                entry.stamp = ++shadow_stamp;
                return &entry;
            }
        }
        // Room for a full cache before any texture exists, so recording the
        // new entry below cannot throw: a throwing insert would lose the only
        // copy of the created handle and leak its texture in the shared store.
        shadows.reserve(detail::shadow_cache_capacity);
        if (shadows.size() >= detail::shadow_cache_capacity) {
            auto candidate = shadows.end();
            for (auto it = shadows.begin(); it != shadows.end(); ++it) {
                if (recorded(it->handle)) {
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
        assert(shadows.size() < shadows.capacity() && "the shadow insert must not allocate");
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
    std::size_t vertex_size = 0, index_size = 0; // logical sizes of current's buffers
    std::vector<Rect> clips;
    std::vector<PendingBatch> pending_batches;
    std::vector<ShadowEntry> shadows;
    std::vector<TextureHandle> stroke_textures;
    std::uint64_t shadow_stamp = 0;
    std::size_t semantic_count = 0;
    std::vector<TextureHandle> frame_handles;
    std::vector<Vec2> outline_scratch, fringe_scratch, clean_scratch, direction_scratch,
                      normal_scratch, plus_scratch, minus_scratch;
    OutlineCache outline_cache;
    MeshCache mesh_cache;
    std::vector<std::uint32_t> remaining_scratch, triangle_scratch;
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
    impl_->outline_cache.rotate();
    impl_->mesh_cache.rotate();
    impl_->pending_batches.clear();
    impl_->clips.clear();
    impl_->merge_barrier = false;
    impl_->current.reset();
    impl_->vertex_size = impl_->index_size = 0;
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
    // The vertex and index buffers keep their elements as retained storage
    // (see Impl::Mesh); vertex_size and index_size start the frame empty.
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
    if (impl_->clips.back().w == 0.0f || impl_->clips.back().h == 0.0f) {
        return; // polygon_unchecked would draw nothing either
    }
    const auto shape = impl_->rounded_shape(bounds, radius);
    impl_->polygon_unchecked(shape.outline, color, {}, shape.fringe);
}
void Canvas::gradient(Rect bounds, Color top, Color bottom) {
    if (!impl_->ready()) return;
    if (!valid_rect(bounds) || !valid_color(top) || !valid_color(bottom)) {
        impl_->fail(invalid_argument("Gradient bounds and colors must be valid"));
        return;
    }
    if (bounds.w == 0 || bounds.h == 0) return;
    impl_->quad_unchecked(
        {
            Vertex{{bounds.x, bounds.y}, {0.5f, 0.5f}, premultiply_valid(top)},
            Vertex{{bounds.x + bounds.w, bounds.y}, {0.5f, 0.5f}, premultiply_valid(top)},
            Vertex{{bounds.x + bounds.w, bounds.y + bounds.h}, {0.5f, 0.5f}, premultiply_valid(bottom)},
            Vertex{{bounds.x, bounds.y + bounds.h}, {0.5f, 0.5f}, premultiply_valid(bottom)},
        },
        *impl_->white);
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
    const Color corners[4]{premultiply_valid(top_left), premultiply_valid(top_right),
                           premultiply_valid(bottom_right), premultiply_valid(bottom_left)};
    const auto lerp = [](Color a, Color b, float t) {
        return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                     a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    };
    auto mesh = impl_->begin_mesh((cells + 1) * (cells + 1), cells * cells * 6);
    if (!mesh) return;
    for (std::uint32_t y = 0; y <= cells; ++y) {
        const float v = static_cast<float>(y) / cells;
        const Color left = lerp(corners[0], corners[3], v);
        const Color right = lerp(corners[1], corners[2], v);
        // Exact edges: the last row/column uses the bound itself, not a sum.
        const float py = y == cells ? bounds.y + bounds.h : bounds.y + bounds.h * v;
        for (std::uint32_t x = 0; x <= cells; ++x) {
            const float u = static_cast<float>(x) / cells;
            const float px = x == cells ? bounds.x + bounds.w : bounds.x + bounds.w * u;
            *mesh->vertex++ = {{px, py}, {0.5f, 0.5f}, lerp(left, right, u)};
        }
    }
    for (std::uint32_t y = 0; y < cells; ++y) {
        for (std::uint32_t x = 0; x < cells; ++x) {
            const std::uint32_t a = mesh->base + y * (cells + 1) + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + cells + 2;
            const std::uint32_t d = a + cells + 1;
            Impl::put(mesh->index, std::array{a, b, c, a, c, d});
        }
    }
    impl_->end_mesh(*mesh, *impl_->white);
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
    if (impl_->clips.back().w == 0.0f || impl_->clips.back().h == 0.0f) {
        return; // begin_mesh would refuse the mesh either way
    }
    const auto shape = impl_->rounded_shape(bounds, outer_radius);
    const auto outer = shape.outline;
    const auto fringe = shape.fringe;
    const auto count = static_cast<std::uint32_t>(outer.size());
    auto mesh = impl_->begin_mesh(std::size_t{count} * 3, std::size_t{count} * 12);
    if (!mesh) return;
    const Color fill = premultiply_valid(color);
    mesh->vertex = put_vertices(mesh->vertex, outer, fill);
    // Each outer point maps into the inner outline from the corner centers of
    // its quadrant; index 0 is the left (top) side, 1 the right (bottom).
    const float middle_x = bounds.x + bounds.w * 0.5f;
    const float middle_y = bounds.y + bounds.h * 0.5f;
    const std::array center_x{bounds.x + outer_radius, (bounds.x + bounds.w) + -outer_radius};
    const std::array center_y{bounds.y + outer_radius, (bounds.y + bounds.h) + -outer_radius};
    const std::array inner_x{inner.x + inner_radius, (inner.x + inner.w) + -inner_radius};
    const std::array inner_y{inner.y + inner_radius, (inner.y + inner.h) + -inner_radius};
    const float ratio = outer_radius > 0 ? inner_radius / outer_radius : 0;
    const Vertex pattern{{}, {0.5f, 0.5f}, fill};
    for (const auto point : outer) {
        const std::size_t side_x = point.x <= middle_x ? 0 : 1;
        const std::size_t side_y = point.y <= middle_y ? 0 : 1;
        Vertex vertex = pattern;
        vertex.position = {inner_x[side_x] + (point.x - center_x[side_x]) * ratio,
                           inner_y[side_y] + (point.y - center_y[side_y]) * ratio};
        *mesh->vertex++ = vertex;
    }
    mesh->vertex = put_vertices(mesh->vertex, fringe, {0, 0, 0, 0});
    // Rings of `count` vertices each: outer edge, inner edge, outer fringe.
    // Each edge (i, next) has a band quad and a fringe quad; edges that do
    // not wrap advance every index by one, the closing edge comes last.
    const auto o = mesh->base;
    const auto n = o + count;
    const auto f = o + count * 2;
    if (count > 0) { // rounded outlines always have points; keep the loop's bounds
        mesh->index = put_blocks(mesh->index,
                                 {o, o + 1, n + 1, o, n + 1, n, o, f + 1, o + 1, o, f, f + 1},
                                 {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, count - 1);
        const auto last = count - 1;
        Impl::put(mesh->index, std::array{o + last, o, n, o + last, n, n + last,
                                          o + last, f, o, o + last, f + last, f});
    }
    impl_->end_mesh(*mesh, *impl_->white);
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
        // A recorded mesh is one this outline passed validation with before.
        const MeshCache::Request request{MeshCache::Kind::polygon, clean, 0.0f,
                                         impl_->current->scale};
        const auto request_hash = MeshCache::hash(request);
        if (const auto cached = impl_->mesh_cache.find(request, request_hash)) {
            if (impl_->clips.back().w == 0.0f || impl_->clips.back().h == 0.0f) return;
            impl_->polygon_unchecked(cached->outline, color, cached->interior, cached->fringe);
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
        if (const auto recorded =
                impl_->mesh_cache.record(request, request_hash, clean, outer, interior)) {
            impl_->polygon_unchecked(recorded->outline, color, recorded->interior,
                                     recorded->fringe);
            return;
        }
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
        // A recorded mesh is the one this exact stroke produced before.
        const MeshCache::Request request{MeshCache::Kind::line, clean, half,
                                         impl_->current->scale};
        const auto request_hash = MeshCache::hash(request);
        if (const auto cached = impl_->mesh_cache.find(request, request_hash)) {
            impl_->polygon_unchecked(cached->outline, color, cached->interior, cached->fringe);
            return;
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
                        if (const auto recorded = impl_->mesh_cache.record(
                                request, request_hash, outline, outer, interior)) {
                            impl_->polygon_unchecked(recorded->outline, color,
                                                     recorded->interior, recorded->fringe);
                            return;
                        }
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
    // Shrinking only moves the end of these trivially destructible elements.
    impl_->current->vertices.resize(impl_->vertex_size);
    impl_->current->indices.resize(impl_->index_size);
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

bool CanvasAccess::failed(const Canvas& canvas) noexcept {
    const auto& impl = *canvas.impl_;
    return impl.frame_error.has_value() || impl.construction_error.has_value();
}
Status CanvasAccess::status(const Canvas& canvas) {
    const auto& impl = *canvas.impl_;
    // Polled after nearly every widget and glyph: return without copying an
    // optional Error in the common healthy case.
    if (!impl.frame_error && !impl.construction_error) {
        return std::nullopt;
    }
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
                              Rect bounds, Rect interior, const PanelElevation& elevation) {
    auto& impl = *canvas.impl_;
    if (!impl.ready()) {
        return;
    }
    if (!valid_rect(bounds) || !valid_rect(interior)) {
        impl.fail(invalid_argument("Panel bounds and interior must be finite and valid"));
        return;
    }
    // Canvas::shadow checks the physical sigma range.
    if (!valid_color(elevation.shadow) || !valid_color(elevation.hairline) ||
        !finite(elevation.shadow_sigma) || !finite(elevation.shadow_offset)) {
        impl.fail(invalid_argument("Panel elevation must be finite and valid"));
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
    const bool elevated = elevation.shadow.a > 0.0f || elevation.hairline.a > 0.0f;
    impl.merge_barrier = true;
    try {
        if (elevation.shadow.a > 0.0f) {
            canvas.shadow({bounds.x, bounds.y + elevation.shadow_offset, bounds.w, bounds.h},
                          mark.radius, elevation.shadow_sigma, elevation.shadow);
        }
        // Only the final outline is tessellated: its clamped radius and topology
        // can differ completely from those of the provisional zero-height box.
        if (elevated) impl.clips.push_back(intersect(mark.clip, bounds));
        canvas.rounded_rect(bounds, mark.radius, mark.color);
        if (elevated) impl.clips.pop_back();
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
    if (elevation.hairline.a > 0.0f) {
        // Above the children, so no child paint covers the sheet's edge.
        try {
            impl.clips.push_back(intersect(mark.clip, bounds));
            canvas.border(bounds, mark.radius, 1.0f, elevation.hairline);
            impl.clips.pop_back();
        } catch (const std::bad_alloc&) {
            impl.fail({ErrorCode::capacity, {}});
            return;
        }
        impl.merge_barrier = true;
    }
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

void CanvasAccess::glyph_run(Canvas& canvas, std::span<const GlyphQuad> glyphs, Vec2 offset,
                             Color tint) {
    auto& impl = *canvas.impl_;
    const auto index_size = impl.index_size;
    try {
        if (impl.glyph_run(glyphs, offset, tint)) {
            return;
        }
    } catch (const std::bad_alloc&) {
        // The run makes room for all of its glyphs, empty and invalid ones
        // included, before it changes anything. When that room (or an error
        // message) cannot be allocated while nothing is recorded yet, the
        // per-glyph path below starts over from the same state and fails,
        // or not, exactly where one image per glyph would. Once a texture
        // run is recorded, the exception comes from adding a batch or from
        // an error message after recorded quads, where the per-glyph path
        // throws as well, and it propagates.
        if (impl.index_size != index_size) {
            throw;
        }
    }
    for (const auto& glyph : glyphs) {
        Rect bounds = glyph.bounds;
        bounds.x += offset.x;
        bounds.y += offset.y;
        canvas.image(bounds, glyph.texture, glyph.uv, tint);
    }
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
