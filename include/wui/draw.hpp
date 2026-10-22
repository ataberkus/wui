#pragma once

#include <wui/id.hpp>
#include <wui/resources.hpp>
#include <wui/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace wui {

struct Vertex {
    Vec2 position;
    Vec2 uv;
    Color premultiplied;
};

static_assert(std::is_standard_layout_v<Vertex>);
static_assert(sizeof(Vertex) == 32);
static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, uv) == 8);
static_assert(offsetof(Vertex, premultiplied) == 16);

struct Batch {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t texture_index = 0;
    std::int32_t base_vertex = 0;
    Rect clip;
};

enum class Role { panel, text, button, checkbox, tab, radio, dropdown, tooltip, validation,
                  slider, text_input, numeric_input };

struct Semantic {
    Id id;
    Role role;
    std::string label;
    std::string value;
    bool enabled = true;
    bool focused = false;
    bool selected = false;
    bool invalid = false;
    Rect bounds;
};

struct PacketData {
    Rect viewport;
    float scale = 1;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<std::shared_ptr<const TextureSnapshot>> textures;
    std::vector<Semantic> semantics;
};

using DrawPacket = std::shared_ptr<const PacketData>;

Status validate_packet(const PacketData& packet);

} // namespace wui
