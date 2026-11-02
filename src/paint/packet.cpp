#include <wui/draw.hpp>

#include "../core/float_bits.hpp"
#include "../core/resource_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wui {
namespace {

// Finiteness flags (see float_bits.hpp) are OR-ed over whole arrays and
// tested once. The loops below have no data-dependent branches, which keeps
// them cheap and lets compilers vectorize them.
using detail::nonfinite_bits;
using detail::nonfinite_flag;

std::uint32_t nonfinite_bits(Rect value) {
    return nonfinite_bits(value.x) | nonfinite_bits(value.y) | nonfinite_bits(value.w) |
           nonfinite_bits(value.h);
}
bool finite(float value) { return (nonfinite_bits(value) & nonfinite_flag) == 0; }
bool finite(Rect value) { return (nonfinite_bits(value) & nonfinite_flag) == 0; }

// Eight independent accumulators, one per Vertex member, form a shape that
// compilers turn into a few SIMD operations per vertex.
bool finite_vertices(const std::vector<Vertex>& vertices) {
    std::array<std::uint32_t, 8> flags{};
    for (const auto& vertex : vertices) {
        flags[0] |= nonfinite_bits(vertex.position.x);
        flags[1] |= nonfinite_bits(vertex.position.y);
        flags[2] |= nonfinite_bits(vertex.uv.x);
        flags[3] |= nonfinite_bits(vertex.uv.y);
        flags[4] |= nonfinite_bits(vertex.premultiplied.r);
        flags[5] |= nonfinite_bits(vertex.premultiplied.g);
        flags[6] |= nonfinite_bits(vertex.premultiplied.b);
        flags[7] |= nonfinite_bits(vertex.premultiplied.a);
    }
    std::uint32_t any = 0;
    for (const auto flag : flags) any |= flag;
    return (any & nonfinite_flag) == 0;
}

// True when base + index lies in [0, vertex_count) for every index of the
// range, for any base and index values. vertex_count must not exceed 2^31
// (validate_packet has already bounded it far below that).
//
// v = index + base in modular 32-bit arithmetic equals the true sum s whenever
// s is in [0, 2^32). The flag word index & guard | v | (last - v) then has
// bit 31 clear exactly when s is in [0, last]: v > last yields last - v >= 2^31
// after wrapping, and v >= 2^31 sets the bit directly. A true sum s >= 2^32
// would wrap to a small v; it needs base >= 0 and index >= 2^31, so for a
// nonnegative base the index itself is included (guard), which is harmless
// because such an index can never be in range. A negative base keeps
// s < 2^32, and a negative s wraps to v >= 2^31. No step branches on data,
// so compilers can vectorize the loop.
bool indices_in_range(const std::uint32_t* indices, std::size_t count, std::int32_t base,
                      std::size_t vertex_count) {
    if (vertex_count == 0) {
        return count == 0;
    }
    const auto offset = static_cast<std::uint32_t>(base);
    const auto last = static_cast<std::uint32_t>(vertex_count - 1);
    const std::uint32_t guard = base >= 0 ? 0xffffffffu : 0u;
    std::uint32_t flags = 0;
    if (base == 0) {
        // The same flags with offset 0 and guard ~0: canvas batches all
        // use base vertex 0, and the simpler loop vectorizes more tightly.
        for (std::size_t i = 0; i < count; ++i) {
            flags |= indices[i] | (last - indices[i]);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t vertex = indices[i] + offset;
            flags |= (indices[i] & guard) | vertex | (last - vertex);
        }
    }
    return (flags & 0x80000000u) == 0;
}

Status invalid(const char* message) {
    return Error{ErrorCode::invalid_packet, message};
}

} // namespace

Status validate_packet(const PacketData& packet) {
    if (!finite(packet.scale) || packet.scale <= 0 || !finite(packet.viewport) ||
        packet.viewport.w < 0 || packet.viewport.h < 0) {
        return invalid("Invalid viewport or scale");
    }
    if (packet.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        packet.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid("Packet buffer count exceeds API range");
    }
    if (packet.vertices.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(Vertex) ||
        packet.indices.size() >
            std::numeric_limits<std::uint32_t>::max() / sizeof(std::uint32_t)) {
        return invalid("Packet buffer byte size exceeds API range");
    }
    if (!finite_vertices(packet.vertices)) {
        return invalid("Nonfinite vertex");
    }
    for (const auto& texture : packet.textures) {
        const auto extent = texture
                                ? detail::texture_extent(texture->width, texture->height)
                                : std::nullopt;
        if (!texture || !extent || texture->rgba.size() != extent->byte_count ||
            (texture->alpha != AlphaMode::straight &&
             texture->alpha != AlphaMode::premultiplied)) {
            return invalid("Invalid texture snapshot");
        }
    }
    std::uint32_t semantic_flags = 0;
    for (const auto& semantic : packet.semantics) {
        semantic_flags |= nonfinite_bits(semantic.bounds);
    }
    if (semantic_flags & nonfinite_flag) {
        return invalid("Nonfinite semantic bounds");
    }
    for (const auto& batch : packet.batches) {
        const auto end = std::uint64_t{batch.first_index} + batch.index_count;
        if (!finite(batch.clip) || batch.clip.w < 0 || batch.clip.h < 0 ||
            end > packet.indices.size() || batch.index_count % 3 != 0) {
            return invalid("Invalid batch range or clip");
        }
        if (batch.texture_index >= packet.textures.size()) {
            return invalid("Invalid texture index");
        }
        if (!indices_in_range(packet.indices.data() + batch.first_index, batch.index_count,
                              batch.base_vertex, packet.vertices.size())) {
            return invalid("Invalid vertex reference");
        }
    }
    return std::nullopt;
}

} // namespace wui
