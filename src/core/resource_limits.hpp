#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace wui::detail {

struct TextureExtent {
    std::uint32_t row_bytes;
    std::size_t byte_count;
};

inline std::optional<TextureExtent> texture_extent(std::uint32_t width,
                                                   std::uint32_t height) {
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    const auto row_bytes = std::uint64_t{width} * 4;
    if (row_bytes > std::numeric_limits<std::uint32_t>::max() ||
        row_bytes > std::numeric_limits<std::size_t>::max() / height) {
        return std::nullopt;
    }
    return TextureExtent{static_cast<std::uint32_t>(row_bytes),
                         static_cast<std::size_t>(row_bytes) * height};
}

inline std::optional<std::uint32_t> next_texture_generation(std::uint32_t current) {
    if (current == std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return current + 1;
}

} // namespace wui::detail
