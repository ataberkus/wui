#include <wui/draw.hpp>

#include "../core/resource_limits.hpp"

#include <cmath>
#include <limits>

namespace wui {
namespace {

bool finite(float value) { return std::isfinite(value); }
bool finite(Vec2 value) { return finite(value.x) && finite(value.y); }
bool finite(Color value) {
    return finite(value.r) && finite(value.g) && finite(value.b) && finite(value.a);
}
bool finite(Rect value) {
    return finite(value.x) && finite(value.y) && finite(value.w) && finite(value.h);
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
    for (const auto& vertex : packet.vertices) {
        if (!finite(vertex.position) || !finite(vertex.uv) || !finite(vertex.premultiplied)) {
            return invalid("Nonfinite vertex");
        }
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
    for (const auto& semantic : packet.semantics) {
        if (!finite(semantic.bounds)) {
            return invalid("Nonfinite semantic bounds");
        }
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
        for (std::uint64_t i = batch.first_index; i < end; ++i) {
            const auto vertex = std::int64_t{batch.base_vertex} + packet.indices[i];
            if (vertex < 0 || static_cast<std::uint64_t>(vertex) >= packet.vertices.size()) {
                return invalid("Invalid vertex reference");
            }
        }
    }
    return std::nullopt;
}

} // namespace wui
