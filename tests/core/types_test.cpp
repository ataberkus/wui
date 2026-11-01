#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include <doctest/doctest.h>

#include <wui/types.hpp>

TEST_CASE("geometry uses half-open edges and alpha is premultiplied") {
    const wui::Rect r{10, 20, 40, 30};
    CHECK(wui::contains(r, {10, 20}));
    CHECK_FALSE(wui::contains(r, {50, 30}));
    const auto c = wui::premultiply({1, 0.5f, 0, 0.25f});
    CHECK(c.r == doctest::Approx(0.25f));
    CHECK(c.g == doctest::Approx(0.125f));
    const auto clipped = wui::intersect(r, {30, 0, 100, 35});
    CHECK(clipped.x == 30);
    CHECK(clipped.w == 20);
    CHECK(clipped.h == 15);
}

TEST_CASE("nonfinite primitive input produces safe geometry and color") {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto color = wui::premultiply({nan, 0.5f, 1.0f, 0.5f});
    CHECK(color.r == 0.0f);
    CHECK(color.g == doctest::Approx(0.25f));
    CHECK(color.b == doctest::Approx(0.5f));
    CHECK(color.a == doctest::Approx(0.5f));

    CHECK_FALSE(wui::contains({0, 0, nan, 10}, {1, 1}));
    CHECK(wui::intersect({0, 0, nan, 10}, {0, 0, 10, 10}).w == 0.0f);
}

TEST_CASE("premultiply clamps channels before applying alpha") {
    const auto color = wui::premultiply({2.0f, -0.5f, 0.5f, 2.0f});
    CHECK(color.r == 1.0f);
    CHECK(color.g == 0.0f);
    CHECK(color.b == doctest::Approx(0.5f));
    CHECK(color.a == 1.0f);
}

namespace {
// The original, field-by-field formulations; the library versions must match
// them bit for bit on every input.
bool reference_finite(wui::Rect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h);
}
bool reference_extent(wui::Rect r) {
    return reference_finite(r) && r.w >= 0.0f && r.h >= 0.0f &&
           std::isfinite(r.x + r.w) && std::isfinite(r.y + r.h);
}
float reference_channel(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }
wui::Rect reference_intersect(wui::Rect a, wui::Rect b) {
    if (!reference_extent(a) || !reference_extent(b)) return {};
    const float left = std::max(a.x, b.x), top = std::max(a.y, b.y);
    const float right = std::min(a.x + a.w, b.x + b.w), bottom = std::min(a.y + a.h, b.y + b.h);
    return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}
bool reference_contains(wui::Rect r, wui::Vec2 p) {
    if (!reference_extent(r) || !std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}
bool same_bits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}
} // namespace

TEST_CASE("fast geometry and color checks match the field-by-field definitions bit for bit") {
    constexpr float big = std::numeric_limits<float>::max();
    const float values[] = {0.0f, -0.0f, 0.25f, 1.0f, 2.0f, -1.0f, 1e-40f, -1e-40f,
                            big, -big, 3e38f, std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()};
    for (const float v : values) {
        for (const float alpha : values) {
            const auto c = wui::premultiply({v, alpha, v, alpha});
            const float a = reference_channel(alpha);
            CHECK(same_bits(c.r, reference_channel(v) * a));
            CHECK(same_bits(c.g, reference_channel(alpha) * a));
            CHECK(same_bits(c.a, a));
        }
    }
    // Rect pairs drawn from the grid (x and w vary, y and h vary separately).
    for (const float x : values) {
        for (const float w : values) {
            for (const float y : {0.0f, -1.0f, 3e38f, std::numeric_limits<float>::quiet_NaN()}) {
                for (const float h : {0.0f, 2.0f, big, -1.0f}) {
                    const wui::Rect r{x, y, w, h};
                    for (const wui::Rect other : {wui::Rect{0, 0, 1, 1}, wui::Rect{-1, -1, 3e38f, 4},
                                                  wui::Rect{x, x, w, w}}) {
                        const auto got = wui::intersect(r, other);
                        const auto want = reference_intersect(r, other);
                        CHECK((same_bits(got.x, want.x) && same_bits(got.y, want.y) &&
                               same_bits(got.w, want.w) && same_bits(got.h, want.h)));
                        const auto swapped = wui::intersect(other, r);
                        const auto want_swapped = reference_intersect(other, r);
                        CHECK((same_bits(swapped.x, want_swapped.x) &&
                               same_bits(swapped.w, want_swapped.w) &&
                               same_bits(swapped.h, want_swapped.h)));
                    }
                    for (const float px : values) {
                        const wui::Vec2 point{px, 0.5f};
                        CHECK(wui::contains(r, point) == reference_contains(r, point));
                        const wui::Vec2 flipped{0.5f, px};
                        CHECK(wui::contains(r, flipped) == reference_contains(r, flipped));
                    }
                }
            }
        }
    }
}
