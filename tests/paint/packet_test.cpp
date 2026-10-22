#include <doctest/doctest.h>

#include <wui/canvas.hpp>
#include <wui/draw.hpp>
#include <wui/resources.hpp>

#include "../../src/core/resource_limits.hpp"
#include "../../src/paint/canvas_access.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <variant>
namespace {

constexpr std::array red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
constexpr std::array blue{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};

wui::TextureHandle create_pixel(wui::TextureStore& store) {
    return std::get<wui::TextureHandle>(
        store.create(1, 1, red, wui::AlphaMode::straight));
}

wui::PacketData triangle_packet() {
    wui::PacketData packet;
    packet.viewport = {0, 0, 100, 100};
    packet.vertices = {
        {{0, 0}, {0, 0}, {1, 0, 0, 1}},
        {{1, 0}, {1, 0}, {0, 1, 0, 1}},
        {{0, 1}, {0, 1}, {0, 0, 1, 1}},
    };
    packet.indices = {0, 1, 2};
    packet.batches.push_back({0, 3, 0, 0, {0, 0, 100, 100}});
    auto texture = std::make_shared<wui::TextureSnapshot>();
    texture->width = 1;
    texture->height = 1;
    texture->rgba.assign(red.begin(), red.end());
    packet.textures.push_back(std::move(texture));
    return packet;
}

void check_invalid(const wui::PacketData& packet) {
    const auto error = wui::validate_packet(packet);
    REQUIRE(error.has_value());
    CHECK(error->code == wui::ErrorCode::invalid_packet);
}

} // namespace

static_assert(sizeof(wui::Vertex) == 32);
static_assert(offsetof(wui::Vertex, position) == 0);
static_assert(offsetof(wui::Vertex, uv) == 8);
static_assert(offsetof(wui::Vertex, premultiplied) == 16);

TEST_CASE("texture limits retire exhausted generations") {
    CHECK(wui::detail::next_texture_generation(1) == 2);
    CHECK(wui::detail::next_texture_generation(
              std::numeric_limits<std::uint32_t>::max() - 1) ==
          std::numeric_limits<std::uint32_t>::max());
    CHECK_FALSE(wui::detail::next_texture_generation(
                    std::numeric_limits<std::uint32_t>::max())
                    .has_value());
}

TEST_CASE("texture limits check widened row pitch before pixel storage") {
    constexpr auto max_width = std::numeric_limits<std::uint32_t>::max() / 4;
    const auto boundary = wui::detail::texture_extent(max_width, 1);
    REQUIRE(boundary.has_value());
    CHECK(boundary->row_bytes == max_width * 4);
    CHECK(boundary->byte_count == std::size_t{max_width} * 4);
    CHECK_FALSE(wui::detail::texture_extent(max_width + 1, 1).has_value());
}

TEST_CASE("an update cannot change pixels held by an older packet") {
    wui::TextureStore store(1024);
    const auto handle = create_pixel(store);
    const auto a = std::get<std::shared_ptr<const wui::TextureSnapshot>>(store.snapshot(handle));
    REQUIRE_FALSE(store.update(handle, blue).has_value());
    const auto b = std::get<std::shared_ptr<const wui::TextureSnapshot>>(store.snapshot(handle));
    const auto unchanged = std::get<std::shared_ptr<const wui::TextureSnapshot>>(store.snapshot(handle));
    CHECK(a->rgba[0] == std::byte{255});
    CHECK(b->rgba[2] == std::byte{255});
    CHECK(b->revision > a->revision);
    CHECK(unchanged == b);
    REQUIRE_FALSE(store.release(handle).has_value());
    CHECK(std::holds_alternative<wui::Error>(store.snapshot(handle)));
    CHECK(a->rgba[0] == std::byte{255});
}

TEST_CASE("released slots reject stale handles and advance their generation") {
    wui::TextureStore store(1024);
    const auto old_handle = create_pixel(store);
    REQUIRE_FALSE(store.release(old_handle).has_value());
    const auto new_handle = create_pixel(store);
    CHECK(new_handle.slot == old_handle.slot);
    CHECK(new_handle.generation != old_handle.generation);
    CHECK(std::get<wui::Error>(store.snapshot(old_handle)).code == wui::ErrorCode::stale_texture);
    const auto update_error = store.update(old_handle, blue);
    REQUIRE(update_error.has_value());
    CHECK(update_error->code == wui::ErrorCode::stale_texture);
    const auto release_error = store.release(old_handle);
    REQUIRE(release_error.has_value());
    CHECK(release_error->code == wui::ErrorCode::stale_texture);
}

TEST_CASE("leased revisions retain budget until the final lease is destroyed") {
    wui::TextureStore store(8);
    const auto handle = create_pixel(store);
    auto old = std::get<std::shared_ptr<const wui::TextureSnapshot>>(store.snapshot(handle));
    REQUIRE_FALSE(store.update(handle, blue).has_value());
    const auto capacity_error = store.update(handle, red);
    REQUIRE(capacity_error.has_value());
    CHECK(capacity_error->code == wui::ErrorCode::capacity);
    old.reset();
    REQUIRE_FALSE(store.update(handle, red).has_value());
}

TEST_CASE("snapshot leases can outlive and be destroyed after their store") {
    std::shared_ptr<const wui::TextureSnapshot> lease;
    {
        wui::TextureStore store(4);
        lease = std::get<std::shared_ptr<const wui::TextureSnapshot>>(store.snapshot(create_pixel(store)));
    }
    CHECK(lease->rgba == std::vector<std::byte>(red.begin(), red.end()));
    std::thread renderer([lease = std::move(lease)]() mutable { lease.reset(); });
    renderer.join();
}

TEST_CASE("texture creation validates dimensions byte counts overflow and capacity") {
    wui::TextureStore store(4);
    constexpr auto excessive_row_width =
        std::numeric_limits<std::uint32_t>::max() / 4 + 1;
    CHECK(std::get<wui::Error>(store.create(0, 1, {}, wui::AlphaMode::straight)).code ==
          wui::ErrorCode::invalid_argument);
    CHECK(std::get<wui::Error>(store.create(1, 1, std::span{red}.first(3),
                                            wui::AlphaMode::straight)).code ==
          wui::ErrorCode::invalid_argument);
    CHECK(std::get<wui::Error>(store.create(std::numeric_limits<std::uint32_t>::max(),
                                            std::numeric_limits<std::uint32_t>::max(), {},
                                            wui::AlphaMode::straight)).code ==
          wui::ErrorCode::invalid_argument);
    CHECK(std::get<wui::Error>(store.create(excessive_row_width, 1, {},
                                            wui::AlphaMode::straight)).code ==
          wui::ErrorCode::invalid_argument);
    CHECK(std::get<wui::Error>(store.create(1, 1, red, static_cast<wui::AlphaMode>(99))).code ==
          wui::ErrorCode::invalid_argument);
    create_pixel(store);
    CHECK(std::get<wui::Error>(store.create(1, 1, blue, wui::AlphaMode::straight)).code ==
          wui::ErrorCode::capacity);
}

TEST_CASE("a draw packet owns texture pixels after the producing store is gone") {
    wui::DrawPacket packet;
    {
        wui::TextureStore store(4);
        auto data = std::make_shared<wui::PacketData>();
        data->viewport = {0, 0, 0, 0};
        data->textures.push_back(std::get<std::shared_ptr<const wui::TextureSnapshot>>(
            store.snapshot(create_pixel(store))));
        packet = std::move(data);
    }
    CHECK(packet->textures[0]->rgba[0] == std::byte{255});
    CHECK_FALSE(wui::validate_packet(*packet).has_value());
}

TEST_CASE("a valid empty minimized frame is accepted") {
    wui::PacketData packet;
    packet.viewport = {0, 0, 0, 0};
    CHECK_FALSE(wui::validate_packet(packet).has_value());
}

TEST_CASE("packet validation rejects invalid scalar and vertex data") {
    auto packet = triangle_packet();
    SUBCASE("nonpositive scale") { packet.scale = 0; }
    SUBCASE("nonfinite scale") { packet.scale = std::numeric_limits<float>::infinity(); }
    SUBCASE("negative viewport") { packet.viewport.w = -1; }
    SUBCASE("nonfinite vertex") {
        packet.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    }
    SUBCASE("nonfinite clip") {
        packet.batches[0].clip.y = std::numeric_limits<float>::infinity();
    }
    check_invalid(packet);
}

TEST_CASE("packet validation rejects malformed batch ranges and references") {
    auto packet = triangle_packet();
    SUBCASE("nontriangular count") { packet.batches[0].index_count = 2; }
    SUBCASE("range addition beyond the index buffer") {
        packet.batches[0].first_index = std::numeric_limits<std::uint32_t>::max();
        packet.batches[0].index_count = 3;
    }
    SUBCASE("invalid texture index") { packet.batches[0].texture_index = 1; }
    SUBCASE("negative base vertex") { packet.batches[0].base_vertex = -1; }
    SUBCASE("vertex reference beyond the buffer") { packet.indices[2] = 3; }
    check_invalid(packet);
}

TEST_CASE("packet validation rejects malformed texture snapshots") {
    auto packet = triangle_packet();
    SUBCASE("null snapshot") { packet.textures[0].reset(); }
    SUBCASE("zero dimensions") {
        auto texture = std::make_shared<wui::TextureSnapshot>(*packet.textures[0]);
        texture->width = 0;
        packet.textures[0] = std::move(texture);
    }
    SUBCASE("wrong pixel byte count") {
        auto texture = std::make_shared<wui::TextureSnapshot>(*packet.textures[0]);
        texture->rgba.pop_back();
        packet.textures[0] = std::move(texture);
    }
    SUBCASE("invalid alpha mode") {
        auto texture = std::make_shared<wui::TextureSnapshot>(*packet.textures[0]);
        texture->alpha = static_cast<wui::AlphaMode>(99);
        packet.textures[0] = std::move(texture);
    }
    SUBCASE("row pitch exceeds API width") {
        auto texture = std::make_shared<wui::TextureSnapshot>();
        texture->width = std::numeric_limits<std::uint32_t>::max() / 4 + 1;
        texture->height = 1;
        packet.textures[0] = std::move(texture);
    }
    check_invalid(packet);
}

TEST_CASE("canvas semantic ownership survives slot reuse") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures, 4);
    const std::string long_label =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";
    canvas.begin({0, 0, 100, 100}, 1);
    canvas.semantic({1}, wui::Role::button, long_label, {0, 0, 20, 20},
                    true, false, false, long_label, true);
    wui::DrawPacket retained;
    {
        auto finished = canvas.finish();
        REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
        retained = std::get<wui::DrawPacket>(std::move(finished));
        REQUIRE(retained);
        REQUIRE(retained->semantics.size() == 1);
        CHECK(retained->semantics[0].label == long_label);
        CHECK(retained->semantics[0].value == long_label);
        CHECK(retained->semantics[0].invalid);
    }
    canvas.begin({0, 0, 100, 100}, 1);
    canvas.semantic({1}, wui::Role::button, "short", {0, 0, 20, 20}, true, false, false);
    {
        auto finished = canvas.finish();
        REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
        auto second = std::get<wui::DrawPacket>(std::move(finished));
        REQUIRE(second->semantics.size() == 1);
        CHECK(second->semantics[0].label == "short");
        CHECK(second->semantics[0].value.empty());
        CHECK_FALSE(second->semantics[0].invalid);
        CHECK(retained->semantics[0].label == long_label);
    }
    canvas.begin({0, 0, 100, 100}, 1);
    canvas.semantic({1}, wui::Role::button, "", {0, 0, 20, 20}, true, false, false);
    {
        auto finished = canvas.finish();
        REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
        auto third = std::get<wui::DrawPacket>(std::move(finished));
        REQUIRE(third->semantics.size() == 1);
        CHECK(third->semantics[0].label.empty());
        CHECK(third->semantics[0].value.empty());
        CHECK_FALSE(third->semantics[0].invalid);
        CHECK(retained->semantics[0].label == long_label);
    }
    retained.reset();
    // Reusing a previously longer slot must not leak stale labels.
    canvas.begin({0, 0, 100, 100}, 1);
    {
        auto finished = canvas.finish();
        REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
        auto empty = std::get<wui::DrawPacket>(std::move(finished));
        CHECK(empty->semantics.empty());
    }
    // Abort a partial frame, then build cleanly.
    canvas.begin({0, 0, 100, 100}, 1);
    canvas.semantic({9}, wui::Role::button, "partial", {0, 0, 5, 5}, true, false, false);
    wui::detail::CanvasAccess::abort(canvas);
    canvas.begin({0, 0, 100, 100}, 1);
    canvas.semantic({10}, wui::Role::button, "clean", {0, 0, 5, 5}, true, false, false);
    {
        auto finished = canvas.finish();
        REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
        auto clean = std::get<wui::DrawPacket>(std::move(finished));
        REQUIRE(clean->semantics.size() == 1);
        CHECK(clean->semantics[0].label == "clean");
    }
}
