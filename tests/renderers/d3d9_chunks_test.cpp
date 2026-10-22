#include <doctest/doctest.h>

#include <wui/backends/d3d9.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace {

wui::PacketData packet_with_triangles(std::uint32_t vertex_count) {
    wui::PacketData packet;
    packet.viewport = {0, 0, 128, 128};
    packet.vertices.reserve(vertex_count);
    packet.indices.reserve(vertex_count);
    for (std::uint32_t i = 0; i < vertex_count; ++i) {
        packet.vertices.push_back({{static_cast<float>(i % 128), static_cast<float>((i / 128) % 128)},
            {0, 0}, {0.25f, 0.125f, 0.0625f, 0.5f}});
        packet.indices.push_back(i);
    }
    auto white = std::make_shared<wui::TextureSnapshot>();
    white->width = white->height = 1;
    white->alpha = wui::AlphaMode::straight;
    white->rgba = {std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    packet.textures.push_back(std::move(white));
    packet.batches.push_back({0, vertex_count, 0, 0, {0, 0, 128, 128}});
    return packet;
}

} // namespace

TEST_CASE("70002 unique vertices split at 65535 without dropping triangles") {
    auto packet = packet_with_triangles(70002);
    REQUIRE_FALSE(wui::validate_packet(packet).has_value());

    const auto converted = wui::d3d9::make_chunks(packet, 65535, 21845);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(converted));
    const auto& chunks = std::get<std::vector<wui::d3d9::Chunk>>(converted);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0].vertices.size() == 65535);
    CHECK(chunks[1].vertices.size() == 4467);
    std::size_t triangle_count = 0;
    for (const auto& chunk : chunks) {
        CHECK(chunk.vertices.size() <= 65535);
        triangle_count += chunk.indices.size() / 3;
        for (const auto index : chunk.indices) CHECK(index < chunk.vertices.size());
    }
    CHECK(triangle_count == 23334);
    CHECK(chunks[1].vertices[0].position.x == packet.vertices[65535].position.x);
    CHECK(chunks[1].vertices[0].position.y == packet.vertices[65535].position.y);

    const auto oversized_caps = wui::d3d9::make_chunks(packet, 100000, 100000);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(oversized_caps));
    const auto& clamped = std::get<std::vector<wui::d3d9::Chunk>>(oversized_caps);
    REQUIRE(clamped.size() == 2);
    CHECK(clamped[0].vertices.size() == 65535);
}

TEST_CASE("capacity limits split on a triangle boundary and deduplicate repeated vertices") {
    auto packet = packet_with_triangles(9);
    packet.indices = {0, 1, 2, 2, 1, 0, 3, 4, 5, 6, 7, 8};
    packet.batches[0].index_count = 12;
    REQUIRE_FALSE(wui::validate_packet(packet).has_value());

    const auto converted = wui::d3d9::make_chunks(packet, 6, 2);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(converted));
    const auto& chunks = std::get<std::vector<wui::d3d9::Chunk>>(converted);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0].vertices.size() == 3);
    CHECK(chunks[0].indices == std::vector<std::uint16_t>{0, 1, 2, 2, 1, 0});
    CHECK(chunks[1].indices == std::vector<std::uint16_t>{0, 1, 2, 3, 4, 5});
    CHECK(chunks[1].vertices[0].position.x == packet.vertices[3].position.x);

    const auto vertex_limited = wui::d3d9::make_chunks(packet, 3, 4);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(vertex_limited));
    const auto& by_vertices = std::get<std::vector<wui::d3d9::Chunk>>(vertex_limited);
    REQUIRE(by_vertices.size() == 3);
    CHECK(by_vertices[0].indices.size() == 6);
    CHECK(by_vertices[1].indices.size() == 3);
    CHECK(by_vertices[2].indices.size() == 3);
}

TEST_CASE("degenerate triangle counts distinct new vertices only") {
    auto packet = packet_with_triangles(3);
    packet.indices = {0, 1, 1, 2, 2, 2};
    packet.batches[0].index_count = 6;
    const auto converted = wui::d3d9::make_chunks(packet, 3, 2);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(converted));
    const auto& chunks = std::get<std::vector<wui::d3d9::Chunk>>(converted);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].indices == std::vector<std::uint16_t>{0, 1, 1, 2, 2, 2});
    CHECK(chunks[0].vertices.size() == 3);
}

TEST_CASE("invalid capacity and malformed packet yield typed errors") {
    const auto packet = packet_with_triangles(3);
    for (const auto [vertices, primitives] : {
             std::pair{0u, 1u}, std::pair{2u, 1u}, std::pair{3u, 0u}}) {
        const auto converted = wui::d3d9::make_chunks(packet, vertices, primitives);
        REQUIRE(std::holds_alternative<wui::Error>(converted));
        CHECK(std::get<wui::Error>(converted).code == wui::ErrorCode::capacity);
    }
    auto invalid = packet;
    invalid.batches[0].base_vertex = -1;
    const auto converted = wui::d3d9::make_chunks(invalid, 3, 1);
    REQUIRE(std::holds_alternative<wui::Error>(converted));
    CHECK(std::get<wui::Error>(converted).code == wui::ErrorCode::invalid_packet);
}

TEST_CASE("batch boundaries retain submission order texture clip and vertex alpha") {
    auto packet = packet_with_triangles(9);
    packet.indices = {0, 1, 2, 0, 1, 2, 0, 1, 2};
    packet.batches = {
        {0, 3, 0, 0, {0, 0, 20, 20}},
        {3, 3, 1, 3, {8, 9, 10, 11}},
        {6, 3, 0, 6, {1, 2, 3, 4}},
    };
    auto second = std::make_shared<wui::TextureSnapshot>(*packet.textures.front());
    second->alpha = wui::AlphaMode::premultiplied;
    packet.textures.push_back(std::move(second));
    packet.vertices[3].premultiplied.a = 0.25f;
    REQUIRE_FALSE(wui::validate_packet(packet).has_value());

    const auto converted = wui::d3d9::make_chunks(packet, 65535, 21845);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(converted));
    const auto& chunks = std::get<std::vector<wui::d3d9::Chunk>>(converted);
    REQUIRE(chunks.size() == 3);
    CHECK(chunks[0].texture_index == 0);
    CHECK(chunks[1].texture_index == 1);
    CHECK(chunks[2].texture_index == 0);
    CHECK(chunks[0].clip.w == 20);
    CHECK(chunks[1].clip.x == 8);
    CHECK(chunks[1].clip.y == 9);
    CHECK(chunks[1].clip.w == 10);
    CHECK(chunks[1].clip.h == 11);
    CHECK(chunks[2].clip.x == 1);
    CHECK(chunks[1].vertices[0].position.x == packet.vertices[3].position.x);
    CHECK(chunks[1].vertices[0].premultiplied.a == 0.25f);
    CHECK(chunks[2].vertices[0].position.x == packet.vertices[6].position.x);
}

TEST_CASE("adjacent compatible original batches remain distinct") {
    auto packet = packet_with_triangles(6);
    packet.batches = {
        {0, 3, 0, 0, {0, 0, 128, 128}},
        {3, 3, 0, 0, {0, 0, 128, 128}},
    };
    const auto converted = wui::d3d9::make_chunks(packet, 65535, 21845);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(converted));
    const auto& chunks = std::get<std::vector<wui::d3d9::Chunk>>(converted);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0].vertices[0].position.x == packet.vertices[0].position.x);
    CHECK(chunks[1].vertices[0].position.x == packet.vertices[3].position.x);
}

TEST_CASE("negative base vertex resolves validated index references") {
    auto packet = packet_with_triangles(6);
    packet.indices = {3, 4, 5};
    packet.batches = {{0, 3, 0, -3, {0, 0, 128, 128}}};
    REQUIRE_FALSE(wui::validate_packet(packet).has_value());
    const auto converted = wui::d3d9::make_chunks(packet, 3, 1);
    REQUIRE(std::holds_alternative<std::vector<wui::d3d9::Chunk>>(converted));
    const auto& chunks = std::get<std::vector<wui::d3d9::Chunk>>(converted);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].indices == std::vector<std::uint16_t>{0, 1, 2});
    CHECK(chunks[0].vertices[0].position.x == packet.vertices[0].position.x);
}
