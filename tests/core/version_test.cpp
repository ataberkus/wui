#include <doctest/doctest.h>
#include <wui/version.hpp>

#include <ostream>
#include <string_view>

TEST_CASE("version macros and constants agree with the CMake project version") {
    CHECK(WUI_VERSION_MAJOR == 0);
    CHECK(WUI_VERSION_MINOR == 1);
    CHECK(WUI_VERSION_PATCH == 0);
    CHECK(std::string_view(wui::version_string) == WUI_PROJECT_VERSION);
    CHECK(wui::version_major == WUI_VERSION_MAJOR);
    CHECK(wui::version_minor == WUI_VERSION_MINOR);
    CHECK(wui::version_patch == WUI_VERSION_PATCH);
}
