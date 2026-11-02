#include <doctest/doctest.h>

#include <wui/id.hpp>
#include "../../src/core/id_hash.hpp"

#include <cstdint>
#include <string>
#include <string_view>

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

namespace {
// The original byte-at-a-time child_id, kept as the reference definition.
wui::Id reference_child_id(wui::Id parent, std::string_view key) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        hash ^= (parent.value >> shift) & 0xffU;
        hash *= 1099511628211ULL;
    }
    for (const unsigned char byte : key) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return {hash};
}
}

TEST_CASE("unrolled and seeded identity hashing matches the byte-at-a-time definition") {
    std::string key;
    for (int length = 0; length <= 19; ++length) {
        // High-bit bytes check that key bytes are taken as unsigned.
        for (const wui::Id parent : {wui::Id{}, wui::Id{1}, wui::Id{0x0102030405060708ULL},
                                     wui::Id{0xfedcba9876543210ULL}}) {
            const auto want = reference_child_id(parent, key);
            CHECK(wui::child_id(parent, key) == want);
            CHECK(wui::detail::hash_child_id(parent, key) == want);
            CHECK(wui::detail::id_append(wui::detail::id_seed(parent), key) == want.value);
            // A seed extended in pieces equals one pass over the whole key.
            for (std::size_t split = 0; split <= key.size(); ++split) {
                const auto head = wui::detail::id_append(wui::detail::id_seed(parent),
                                                         std::string_view(key).substr(0, split));
                CHECK(wui::detail::id_append(head, std::string_view(key).substr(split)) == want.value);
            }
        }
        key.push_back(static_cast<char>(length % 2 ? 0x80 + length * 5 : 'a' + length));
    }
    // A cell index hashes as the eight bytes of its 64-bit object representation.
    const wui::Id scope = wui::child_id({7}, "cell");
    for (const std::uint64_t index : {0ULL, 1ULL, 39ULL, 0x0123456789abcdefULL}) {
        const std::string_view bytes{reinterpret_cast<const char*>(&index), sizeof(index)};
        CHECK(wui::detail::id_append(wui::detail::id_seed(scope), bytes) ==
              reference_child_id(scope, bytes).value);
    }
}
