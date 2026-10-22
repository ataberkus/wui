#include "packet_fixture.hpp"

#include <wui/canvas.hpp>
#include <wui/resources.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace wui::test {
namespace {

constexpr std::uint32_t kCheckerSize = 4;
constexpr std::uint32_t kPadVertices = 8;

std::vector<std::byte> checker_pixels(bool revised) {
    std::vector<std::byte> rgba(static_cast<std::size_t>(kCheckerSize) * kCheckerSize * 4);
    for (std::uint32_t y = 0; y < kCheckerSize; ++y) {
        for (std::uint32_t x = 0; x < kCheckerSize; ++x) {
            const bool white = ((x + y) % 2) == 0;
            const std::size_t base = (static_cast<std::size_t>(y) * kCheckerSize + x) * 4;
            // Revision 2 changes exactly the (0,0) texel from white to red.
            if (revised && x == 0 && y == 0) {
                rgba[base] = std::byte{255};
                rgba[base + 1] = std::byte{0};
                rgba[base + 2] = std::byte{0};
                rgba[base + 3] = std::byte{255};
            } else if (white) {
                rgba[base] = std::byte{255};
                rgba[base + 1] = std::byte{255};
                rgba[base + 2] = std::byte{255};
                rgba[base + 3] = std::byte{255};
            } else {
                rgba[base] = std::byte{0};
                rgba[base + 1] = std::byte{0};
                rgba[base + 2] = std::byte{0};
                rgba[base + 3] = std::byte{255};
            }
        }
    }
    return rgba;
}

} // namespace

DrawPacket reference_packet(std::uint64_t texture_revision, float scale) {
    if (texture_revision != 1 && texture_revision != 2) {
        throw std::invalid_argument("unknown reference revision");
    }
    if (!(scale > 0) || !(scale < 1000000)) {
        throw std::invalid_argument("reference packet scale must be positive and finite");
    }

    auto store = std::make_shared<TextureStore>(65536);
    Canvas canvas(store);

    const auto initial = checker_pixels(false);
    auto created = store->create(kCheckerSize, kCheckerSize, initial, AlphaMode::straight);
    if (std::holds_alternative<Error>(created)) {
        throw std::runtime_error("reference fixture cannot create checker texture");
    }
    const auto checker = std::get<TextureHandle>(created);
    if (texture_revision == 2) {
        const auto revised = checker_pixels(true);
        if (const auto error = store->update(checker, revised)) {
            throw std::runtime_error("reference fixture cannot revise checker texture");
        }
    }

    canvas.begin({0, 0, 128, 128}, scale);
    canvas.rounded_rect({0, 0, 128, 128}, 0, {0.1f, 0.1f, 0.1f, 1.0f});
    canvas.rounded_rect({8, 8, 40, 40}, 0, {1.0f, 0.0f, 0.0f, 1.0f});
    canvas.rounded_rect({24, 24, 40, 40}, 0, {0.0f, 1.0f, 0.0f, 0.5f});
    canvas.image({72, 8, 32, 32}, checker, {0, 0, 1, 1}, {1.0f, 1.0f, 1.0f, 1.0f});
    canvas.push_clip({80, 72, 16, 16});
    canvas.rounded_rect({64, 56, 48, 48}, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    canvas.pop_clip();

    auto finished = canvas.finish();
    if (std::holds_alternative<Error>(finished)) {
        throw std::runtime_error("reference fixture canvas failed");
    }
    DrawPacket frozen = std::get<DrawPacket>(std::move(finished));
    if (!frozen || frozen->batches.size() < 2) {
        throw std::runtime_error("reference fixture requires at least two batches");
    }

    // Canvas always emits base_vertex 0 with pre-offset absolute indices.
    // Prepend degenerate dummy vertices so the second disjoint batch can
    // carry a nonzero base_vertex while rendering identically.
    auto adjusted = std::make_shared<PacketData>(*frozen);
    adjusted->vertices.insert(adjusted->vertices.begin(), kPadVertices,
        Vertex{{0, 0}, {0, 0}, {0, 0, 0, 0}});
    for (std::size_t b = 0; b < adjusted->batches.size(); ++b) {
        auto& batch = adjusted->batches[b];
        if (b == 1) {
            batch.base_vertex += static_cast<std::int32_t>(kPadVertices);
        } else {
            const std::uint64_t end =
                static_cast<std::uint64_t>(batch.first_index) + batch.index_count;
            for (std::uint64_t i = batch.first_index; i < end; ++i) {
                adjusted->indices[static_cast<std::size_t>(i)] += kPadVertices;
            }
        }
    }
    if (adjusted->batches[1].base_vertex == 0) {
        throw std::runtime_error("reference fixture failed to offset base vertex");
    }
    if (const auto error = validate_packet(*adjusted)) {
        throw std::runtime_error("reference fixture adjustment is invalid");
    }
    return adjusted;
}

} // namespace wui::test
