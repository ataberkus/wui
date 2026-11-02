#pragma once

#include <wui/id.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wui::detail {

// child_id(parent, key) is 64-bit FNV-1a over the parent value's eight bytes,
// least significant first, followed by the key bytes:
//     child_id(parent, key) == Id{id_append(id_seed(parent), key)}.
// Inline so constant keys ("key", "text", "cell") unroll, and so a caller that
// derives many ids from one parent can hash the parent once and keep the seed.
inline constexpr std::uint64_t id_prime = 1099511628211ULL;

inline std::uint64_t id_seed(Id parent) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        hash ^= (parent.value >> shift) & 0xffU;
        hash *= id_prime;
    }
    return hash;
}

inline std::uint64_t id_append(std::uint64_t hash, std::string_view key) noexcept {
    const char* byte = key.data();
    std::size_t left = key.size();
    // Four bytes per iteration; FNV-1a is a strict byte chain either way.
    for (; left >= 4; left -= 4, byte += 4) {
        hash = (hash ^ static_cast<unsigned char>(byte[0])) * id_prime;
        hash = (hash ^ static_cast<unsigned char>(byte[1])) * id_prime;
        hash = (hash ^ static_cast<unsigned char>(byte[2])) * id_prime;
        hash = (hash ^ static_cast<unsigned char>(byte[3])) * id_prime;
    }
    for (; left > 0; --left, ++byte) hash = (hash ^ static_cast<unsigned char>(*byte)) * id_prime;
    return hash;
}

inline Id hash_child_id(Id parent, std::string_view key) noexcept {
    return {id_append(id_seed(parent), key)};
}

} // namespace wui::detail
