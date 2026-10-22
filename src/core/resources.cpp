#include <wui/resources.hpp>

#include "resource_limits.hpp"

#include <atomic>
#include <limits>
#include <utility>

namespace wui {
namespace {

struct Budget {
    explicit Budget(std::size_t limit_value) : limit(limit_value) {}

    bool reserve(std::size_t bytes) {
        auto current = used.load(std::memory_order_relaxed);
        while (bytes <= limit && current <= limit - bytes) {
            if (used.compare_exchange_weak(current, current + bytes,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void free(std::size_t bytes) { used.fetch_sub(bytes, std::memory_order_acq_rel); }

    const std::size_t limit;
    std::atomic<std::size_t> used{0};
};

std::shared_ptr<const TextureSnapshot> make_snapshot(
    const std::shared_ptr<Budget>& budget, TextureHandle handle, std::uint64_t revision,
    std::uint32_t width, std::uint32_t height, AlphaMode alpha,
    std::span<const std::byte> pixels) {
    if (!budget->reserve(pixels.size())) {
        return {};
    }
    TextureSnapshot* value = nullptr;
    try {
        value = new TextureSnapshot{handle, revision, width, height, alpha,
                                    {pixels.begin(), pixels.end()}};
    } catch (...) {
        budget->free(pixels.size());
        throw;
    }
    return {value, [budget, bytes = pixels.size()](const TextureSnapshot* snapshot) {
                delete snapshot;
                budget->free(bytes);
            }};
}

Error stale() { return {ErrorCode::stale_texture, "Stale texture handle"}; }

} // namespace

struct TextureStore::Impl {
    struct Slot {
        std::uint32_t generation = 1;
        std::shared_ptr<const TextureSnapshot> current;
    };

    explicit Impl(std::size_t limit) : budget(std::make_shared<Budget>(limit)) {}

    Slot* find(TextureHandle handle) {
        if (handle.slot >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[handle.slot];
        return slot.current && slot.generation == handle.generation ? &slot : nullptr;
    }

    const Slot* find(TextureHandle handle) const {
        if (handle.slot >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[handle.slot];
        return slot.current && slot.generation == handle.generation ? &slot : nullptr;
    }

    std::shared_ptr<Budget> budget;
    std::vector<Slot> slots;
};

TextureStore::TextureStore(std::size_t byte_limit) : impl_(std::make_unique<Impl>(byte_limit)) {}
TextureStore::~TextureStore() = default;

Result<TextureHandle> TextureStore::create(std::uint32_t width, std::uint32_t height,
                                           std::span<const std::byte> rgba,
                                           AlphaMode alpha) {
    const auto extent = detail::texture_extent(width, height);
    if (!extent || rgba.size() != extent->byte_count ||
        (alpha != AlphaMode::straight && alpha != AlphaMode::premultiplied)) {
        return Error{ErrorCode::invalid_argument, "Invalid texture dimensions or byte count"};
    }

    std::uint32_t index = 0;
    std::optional<std::uint32_t> next_generation;
    for (; index < impl_->slots.size(); ++index) {
        if (!impl_->slots[index].current) {
            next_generation = detail::next_texture_generation(
                impl_->slots[index].generation);
            if (next_generation) {
                break;
            }
        }
    }
    if (index == impl_->slots.size()) {
        if (impl_->slots.size() >= std::numeric_limits<std::uint32_t>::max()) {
            return Error{ErrorCode::capacity, "Texture slot capacity exceeded"};
        }
        impl_->slots.emplace_back();
    } else {
        impl_->slots[index].generation = *next_generation;
    }

    auto& slot = impl_->slots[index];
    const TextureHandle handle{index, slot.generation};
    auto value = make_snapshot(impl_->budget, handle, 1, width, height, alpha, rgba);
    if (!value) {
        return Error{ErrorCode::capacity, "Texture byte limit exceeded"};
    }
    slot.current = std::move(value);
    return handle;
}

Status TextureStore::update(TextureHandle handle, std::span<const std::byte> full_rgba) {
    auto* slot = impl_->find(handle);
    if (!slot) {
        return stale();
    }
    if (full_rgba.size() != slot->current->rgba.size()) {
        return Error{ErrorCode::invalid_argument, "Invalid texture byte count"};
    }
    const auto revision = slot->current->revision;
    if (revision == std::numeric_limits<std::uint64_t>::max()) {
        return Error{ErrorCode::capacity, "Texture revision capacity exceeded"};
    }
    auto next = make_snapshot(impl_->budget, handle, revision + 1, slot->current->width,
                              slot->current->height, slot->current->alpha, full_rgba);
    if (!next) {
        return Error{ErrorCode::capacity, "Texture byte limit exceeded"};
    }
    slot->current = std::move(next);
    return std::nullopt;
}

Result<std::shared_ptr<const TextureSnapshot>> TextureStore::snapshot(TextureHandle handle) const {
    const auto* slot = impl_->find(handle);
    if (!slot) {
        return stale();
    }
    return slot->current;
}

Status TextureStore::release(TextureHandle handle) {
    auto* slot = impl_->find(handle);
    if (!slot) {
        return stale();
    }
    slot->current.reset();
    return std::nullopt;
}

} // namespace wui
