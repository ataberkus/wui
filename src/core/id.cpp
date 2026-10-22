#include <wui/id.hpp>

namespace wui {

Id child_id(Id parent, std::string_view key) {
    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;

    std::uint64_t hash = offset_basis;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        hash ^= (parent.value >> shift) & 0xffU;
        hash *= prime;
    }
    for (const unsigned char byte : key) {
        hash ^= byte;
        hash *= prime;
    }
    return {hash};
}

} // namespace wui
