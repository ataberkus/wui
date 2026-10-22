#include <doctest/doctest.h>

#include <wui/style.hpp>

#include <cmath>
#include <limits>

namespace {

void check_channel(float actual, int encoded) {
    CHECK(actual == doctest::Approx(static_cast<float>(encoded) / 255.0f));
}

} // namespace

TEST_CASE("dark theme uses the approved visual tokens") {
    const auto theme = wui::Theme::dark();

    check_channel(theme.canvas.r, 0x10);
    check_channel(theme.canvas.g, 0x12);
    check_channel(theme.canvas.b, 0x18);
    check_channel(theme.panel.r, 0x19);
    check_channel(theme.panel.g, 0x1c);
    check_channel(theme.panel.b, 0x25);
    check_channel(theme.control.r, 0x23);
    check_channel(theme.control.g, 0x27);
    check_channel(theme.control.b, 0x35);
    check_channel(theme.text.r, 0xf3);
    check_channel(theme.text.g, 0xf4);
    check_channel(theme.text.b, 0xf8);
    check_channel(theme.secondary.r, 0xb7);
    check_channel(theme.secondary.g, 0xbd);
    check_channel(theme.secondary.b, 0xcc);
    check_channel(theme.accent.r, 0x9c);
    check_channel(theme.accent.g, 0x8c);
    check_channel(theme.accent.b, 0xff);
    check_channel(theme.focus.r, 0xc2);
    check_channel(theme.focus.g, 0xb8);
    check_channel(theme.focus.b, 0xff);
    CHECK(theme.canvas.a == 1.0f);
    CHECK(theme.panel.a == 1.0f);
    CHECK(theme.control.a == 1.0f);
    CHECK(theme.text.a == 1.0f);
    CHECK(theme.secondary.a == 1.0f);
    CHECK(theme.accent.a == 1.0f);
    CHECK(theme.focus.a == 1.0f);

    CHECK(theme.control_height == 36.0f);
    CHECK(theme.body_size == 14.0f);
    CHECK(theme.heading_size == 20.0f);
    CHECK(theme.padding == 16.0f);
    CHECK(theme.gap == 8.0f);
    CHECK(theme.control_radius == 8.0f);
    CHECK(theme.panel_radius == 12.0f);
    CHECK_FALSE(theme.reduced_motion);
    CHECK(std::holds_alternative<wui::Theme>(wui::resolve_theme(theme)));
}

TEST_CASE("theme validation rejects nonfinite out of range and invalid geometry") {
    auto theme = wui::Theme::dark();

    SUBCASE("nonfinite color") {
        theme.accent.r = std::numeric_limits<float>::quiet_NaN();
    }
    SUBCASE("color below encoded range") { theme.text.g = -0.01f; }
    SUBCASE("color above encoded range") { theme.focus.a = 1.01f; }
    SUBCASE("nonpositive control height") { theme.control_height = 0.0f; }
    SUBCASE("nonpositive body size") { theme.body_size = -1.0f; }
    SUBCASE("nonfinite heading size") {
        theme.heading_size = std::numeric_limits<float>::infinity();
    }
    SUBCASE("negative padding") { theme.padding = -1.0f; }
    SUBCASE("negative gap") { theme.gap = -1.0f; }
    SUBCASE("negative control radius") { theme.control_radius = -1.0f; }
    SUBCASE("negative panel radius") { theme.panel_radius = -1.0f; }

    const auto result = wui::resolve_theme(theme);
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
}

TEST_CASE("retargeting continues from the current visual value") {
    wui::Transition t;
    t.retarget(1);
    t.tick(0.05f, false);
    CHECK(t.value() == doctest::Approx(0.5f));
    t.retarget(0);
    CHECK(t.value() == doctest::Approx(0.5f));
    t.tick(0.05f, false);
    CHECK(t.value() == doctest::Approx(0.25f));
    t.tick(0, true);
    CHECK(t.value() == 0);
}

TEST_CASE("transition time clamps stalls and ignores negative time") {
    wui::Transition t;
    t.duration = 0.16f;
    t.retarget(1.0f);
    t.tick(2.0f, false);
    CHECK(t.value() == doctest::Approx(0.3125f));
    const auto before = t.value();
    t.tick(-1.0f, false);
    CHECK(t.value() == before);
    t.tick(2.0f, false);
    CHECK(t.value() == doctest::Approx(0.625f));
    t.tick(0.0f, true);
    CHECK(t.value() == 1.0f);
}

TEST_CASE("zero-duration transitions resolve to their target") {
    wui::Transition t;
    t.duration = 0.0f;
    t.retarget(1.0f);
    CHECK(t.value() == 1.0f);
    t.retarget(0.25f);
    CHECK(t.value() == doctest::Approx(0.25f));
}

TEST_CASE("public theme resolution validates and preserves custom source tokens") {
    auto source = wui::Theme::dark();
    source.control_height = 45;
    source.padding = 20;
    source.gap = 10;
    source.body_size = 15;
    source.heading_size = 23;
    source.control_radius = 7;
    source.panel_radius = 11;
    source.accent = {0.1f, 0.2f, 0.3f, 0.4f};
    source.reduced_motion = true;
    const auto comfortable_result = wui::resolve_theme(source);
    const auto compact_result = wui::resolve_theme(source, wui::Density::compact);
    REQUIRE(std::holds_alternative<wui::Theme>(comfortable_result));
    REQUIRE(std::holds_alternative<wui::Theme>(compact_result));
    const auto comfortable = std::get<wui::Theme>(comfortable_result);
    const auto compact = std::get<wui::Theme>(compact_result);
    CHECK(comfortable.control_height == 45);
    CHECK(comfortable.padding == 20);
    CHECK(comfortable.gap == 10);
    CHECK(compact.control_height == doctest::Approx(35));
    CHECK(compact.padding == 15);
    CHECK(compact.gap == 5);
    for (const auto& resolved : {comfortable, compact}) {
        CHECK(resolved.body_size == source.body_size);
        CHECK(resolved.heading_size == source.heading_size);
        CHECK(resolved.control_radius == source.control_radius);
        CHECK(resolved.panel_radius == source.panel_radius);
        CHECK(resolved.accent.r == source.accent.r);
        CHECK(resolved.accent.g == source.accent.g);
        CHECK(resolved.accent.b == source.accent.b);
        CHECK(resolved.accent.a == source.accent.a);
        CHECK(resolved.reduced_motion == source.reduced_motion);
    }
    CHECK(source.control_height == 45);
    CHECK(source.padding == 20);
    CHECK(source.gap == 10);
    const auto defaults = std::get<wui::Theme>(
        wui::resolve_theme(wui::Theme::dark(), wui::Density::compact));
    CHECK(defaults.control_height == 28);
    CHECK(defaults.padding == 12);
    CHECK(defaults.gap == 4);
}

TEST_CASE("public theme resolution rejects invalid input and density") {
    auto source = wui::Theme::dark();
    auto density = wui::Density::comfortable;
    SUBCASE("nonfinite color") { source.text.r = std::numeric_limits<float>::quiet_NaN(); }
    SUBCASE("negative metric") { source.padding = -1; }
    SUBCASE("invalid enum") { density = static_cast<wui::Density>(1234); }
    const auto result = wui::resolve_theme(source, density);
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
}
