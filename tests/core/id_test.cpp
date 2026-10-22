#include <doctest/doctest.h>

#include <wui/id.hpp>

TEST_CASE("identity uses a scoped key instead of a display label") {
    const auto root = wui::child_id({}, "preferences");
    CHECK(wui::child_id(root, "sound") == wui::child_id(root, "sound"));
    CHECK_FALSE(wui::child_id(root, "sound") == wui::child_id({}, "sound"));
}

TEST_CASE("identity hashes UTF-8 key bytes after the little-endian parent") {
    CHECK(wui::child_id({}, "caf\xC3\xA9").value == 0x86ab05a127c4f069ULL);

    const wui::Id parent{0x0102030405060708ULL};
    CHECK(wui::child_id(parent, "x").value == 0x960539611f8060f7ULL);
}
