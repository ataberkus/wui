#include "../../backends/renderer/scissor.hpp"

#include <doctest/doctest.h>

TEST_CASE("physical scissor rounds outward but snaps float noise to pixel edges") {
    using wui::detail::physical_scissor;
    const wui::Rect viewport{0, 0, 640, 400};

    // 49.6f * 1.25 is 61.9999981 in double; it must select row 62, not 61.
    auto snapped = physical_scissor({10, 49.6f, 100, 168.8f}, viewport, 1.25f, 800, 500);
    CHECK(snapped.left == 12);
    CHECK(snapped.top == 62);
    CHECK(snapped.right == 138);
    CHECK(snapped.bottom == 273);

    // Genuinely fractional edges still floor and ceil outward.
    auto fractional = physical_scissor({10.3f, 20.7f, 50.2f, 30.4f}, viewport, 1.5f, 960, 600);
    CHECK(fractional.left == 15);
    CHECK(fractional.top == 31);
    CHECK(fractional.right == 91);
    CHECK(fractional.bottom == 77);

    // Clamped to the target, and empty when fully outside it.
    auto clamped = physical_scissor({-20, -5, 1000, 1000}, viewport, 1.0f, 640, 400);
    CHECK(clamped.left == 0);
    CHECK(clamped.top == 0);
    CHECK(clamped.right == 640);
    CHECK(clamped.bottom == 400);
    CHECK(physical_scissor({700, 0, 10, 10}, viewport, 1.0f, 640, 400).empty());
    CHECK(physical_scissor({0, 0, 0.0f, 10}, viewport, 1.0f, 640, 400).empty());
}
