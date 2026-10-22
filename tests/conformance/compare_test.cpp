#include "../conformance/compare.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

wui::conformance::Image solid(std::uint32_t w, std::uint32_t h, std::uint8_t value) {
    wui::conformance::Image image{w, h, {}};
    image.rgba.assign(static_cast<std::size_t>(w) * h * 4, value);
    return image;
}

}  // namespace

TEST_CASE("conformance compare reports identity, channel diffs and size mismatch") {
    using namespace wui::conformance;
    const auto a = solid(10, 10, 100);
    std::string reason;

    const auto same = compare(a, a);
    CHECK(same.identical());
    CHECK(within_tolerance("fixture/rev1", same, reason));

    auto small = a;
    small.rgba[(3 * 10 + 4) * 4 + 1] = 103;
    const auto three = compare(a, small);
    CHECK_FALSE(three.identical());
    CHECK(three.max_channel_diff == 3);
    CHECK(three.pixels_over_8 == 0);
    CHECK(three.first_x == 4);
    CHECK(three.first_y == 3);
    CHECK_FALSE(within_tolerance("fixture/rev1", three, reason));
    CHECK(within_tolerance("gallery/controls", three, reason));

    auto alpha_only = a;
    alpha_only.rgba[3] = 0;
    CHECK(compare(a, alpha_only).identical());

    auto one_big = a;
    one_big.rgba[0] = 120;
    const auto big = compare(a, one_big);
    CHECK(big.pixels_over_8 == 1);
    CHECK(big.fraction_over_8 == doctest::Approx(0.01));
    // 1% of pixels over 8 exceeds the 0.2% scene limit.
    CHECK_FALSE(within_tolerance("gallery/controls", big, reason));
    CHECK(reason.find("fraction") != std::string::npos);

    const auto other = solid(10, 11, 100);
    const auto mismatch = compare(a, other);
    CHECK_FALSE(mismatch.same_size);
    CHECK_FALSE(within_tolerance("gallery/controls", mismatch, reason));
}
