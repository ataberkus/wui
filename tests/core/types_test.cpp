#include <cmath>
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

TEST_CASE("every nonfinite channel or extent is rejected, including overflowing edges") {
    const auto inf = std::numeric_limits<float>::infinity();
    // Infinite channels become 0 rather than clamping to 1.
    const auto color = wui::premultiply({inf, -inf, 0.5f, 1.0f});
    CHECK(color.r == 0.0f);
    CHECK(color.g == 0.0f);
    CHECK(color.b == 0.5f);
    CHECK(color.a == 1.0f);
    CHECK(wui::premultiply({1.0f, 1.0f, 1.0f, inf}).a == 0.0f);
    // In-range channels multiply exactly.
    const auto exact = wui::premultiply({1.0f, 0.5f, 0.0f, 0.5f});
    CHECK(exact.r == 0.5f);
    CHECK(exact.g == 0.25f);
    CHECK(exact.b == 0.0f);
    CHECK(exact.a == 0.5f);

    const float big = std::numeric_limits<float>::max();
    const wui::Rect unit{0, 0, 10, 10};
    const wui::Rect invalid[] = {
        {inf, 0, 1, 1}, {0, -inf, 1, 1}, {0, 0, inf, 1}, {0, 0, 1, inf},
        {big, 0, big, 1}, {0, big, 1, big}, {0, 0, -1, 1}, {0, 0, 1, -1},
    };
    for (const auto rect : invalid) {
        const auto clipped = wui::intersect(rect, unit);
        CHECK(clipped.x == 0.0f);
        CHECK(clipped.y == 0.0f);
        CHECK(clipped.w == 0.0f);
        CHECK(clipped.h == 0.0f);
        CHECK(wui::intersect(unit, rect).w == 0.0f);
        CHECK_FALSE(wui::contains(rect, {0.5f, 0.5f}));
    }
    CHECK(wui::intersect({-big, -big, big, big}, unit).w == 0.0f);
    CHECK(wui::contains({-big, -big, big, big}, {-1.0f, -1.0f}));
}
