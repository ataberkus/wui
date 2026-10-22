#include <wui/backends/d3d9.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wui::d3d9 {

Result<std::vector<Chunk>> make_chunks(const PacketData& packet,
    std::uint32_t max_vertices, std::uint32_t max_primitives) {
    if (const auto error = validate_packet(packet)) return *error;
    const auto vertex_limit = std::min(max_vertices, std::uint32_t{65535});
    if (vertex_limit < 3 || max_primitives == 0) {
        return Error{ErrorCode::capacity, "D3D9 chunk needs at least 3 vertices and 1 primitive"};
    }

    try {
        std::vector<Chunk> output;
        output.reserve(packet.batches.size());
        std::vector<Vertex> vertices;
        std::vector<std::uint16_t> indices;
        std::unordered_map<std::uint32_t, std::uint16_t> remap;

        for (const auto& batch : packet.batches) {
            if (batch.index_count == 0) continue;
            const auto vertex_reserve = std::min<std::size_t>(vertex_limit, batch.index_count);
            const auto index_reserve = std::min<std::uint64_t>(batch.index_count,
                std::uint64_t{max_primitives} * 3);
            vertices.reserve(vertex_reserve);
            indices.reserve(static_cast<std::size_t>(index_reserve));
            remap.reserve(vertex_reserve);
            const auto end = std::uint64_t{batch.first_index} + batch.index_count;
            for (std::uint64_t i = batch.first_index; i < end; i += 3) {
                const std::uint32_t original[3] = {
                    static_cast<std::uint32_t>(std::int64_t{batch.base_vertex} + packet.indices[i]),
                    static_cast<std::uint32_t>(std::int64_t{batch.base_vertex} + packet.indices[i + 1]),
                    static_cast<std::uint32_t>(std::int64_t{batch.base_vertex} + packet.indices[i + 2])
                };
                unsigned new_vertices = 0;
                for (unsigned j = 0; j != 3; ++j) {
                    if (remap.contains(original[j])) continue;
                    bool seen = false;
                    for (unsigned k = 0; k < j; ++k) seen |= original[k] == original[j];
                    if (!seen) ++new_vertices;
                }
                if (vertices.size() + new_vertices > vertex_limit ||
                    indices.size() / 3 >= max_primitives) {
                    output.push_back(Chunk{std::move(vertices), std::move(indices),
                        batch.texture_index, batch.clip});
                    vertices.clear();
                    indices.clear();
                    remap.clear();
                    vertices.reserve(std::min<std::uint64_t>(vertex_limit, end - i));
                    indices.reserve(std::min<std::uint64_t>(
                        std::uint64_t{max_primitives} * 3, end - i));
                }
                for (const auto vertex : original) {
                    const auto [it, inserted] = remap.try_emplace(vertex,
                        static_cast<std::uint16_t>(vertices.size()));
                    if (inserted) vertices.push_back(packet.vertices[vertex]);
                    indices.push_back(it->second);
                }
            }
            output.push_back(Chunk{std::move(vertices), std::move(indices),
                batch.texture_index, batch.clip});
            vertices.clear();
            indices.clear();
            remap.clear();
        }
        return output;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, "D3D9 chunk allocation failed"};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, "D3D9 chunk capacity exceeded"};
    }
}

} // namespace wui::d3d9
