#pragma once

#include <wui/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace wui {

struct TextureHandle {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    bool operator==(const TextureHandle&) const = default;
};

enum class AlphaMode { straight, premultiplied };

struct TextureSnapshot {
    TextureHandle handle;
    std::uint64_t revision = 1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    AlphaMode alpha = AlphaMode::straight;
    std::vector<std::byte> rgba;
};

class TextureStore {
public:
    explicit TextureStore(std::size_t byte_limit);
    ~TextureStore();

    TextureStore(const TextureStore&) = delete;
    TextureStore& operator=(const TextureStore&) = delete;
    Result<TextureHandle> create(std::uint32_t width, std::uint32_t height,
                                 std::span<const std::byte> rgba, AlphaMode alpha);
    Status update(TextureHandle handle, std::span<const std::byte> full_rgba);
    Result<std::shared_ptr<const TextureSnapshot>> snapshot(TextureHandle handle) const;
    Status release(TextureHandle handle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wui
