#include <doctest/doctest.h>

#include <wui/canvas.hpp>

#include "../../src/paint/canvas_access.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using wui::detail::CanvasAccess;

wui::DrawPacket finish(wui::Canvas& canvas) {
    auto result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
    auto packet = std::get<wui::DrawPacket>(std::move(result));
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    return packet;
}

void check_rect(wui::Rect actual, wui::Rect expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.w == doctest::Approx(expected.w));
    CHECK(actual.h == doctest::Approx(expected.h));
}

void check_color(wui::Color actual, wui::Color expected) {
    CHECK(actual.r == doctest::Approx(expected.r));
    CHECK(actual.g == doctest::Approx(expected.g));
    CHECK(actual.b == doctest::Approx(expected.b));
    CHECK(actual.a == doctest::Approx(expected.a));
}

const wui::Vertex& drawn_vertex(const wui::PacketData& packet,
                               const wui::Batch& batch, std::uint32_t offset) {
    return packet.vertices[static_cast<std::size_t>(
        std::int64_t{batch.base_vertex} + packet.indices[batch.first_index + offset])];
}

// Compare what a renderer consumes, independent of vertex/index storage order.
void check_draw(const wui::PacketData& actual, const wui::Batch& actual_batch,
                const wui::PacketData& expected, const wui::Batch& expected_batch) {
    REQUIRE(actual_batch.index_count == expected_batch.index_count);
    check_rect(actual_batch.clip, expected_batch.clip);
    for (std::uint32_t i = 0; i < actual_batch.index_count; ++i) {
        const auto& a = drawn_vertex(actual, actual_batch, i);
        const auto& e = drawn_vertex(expected, expected_batch, i);
        CHECK(a.position.x == doctest::Approx(e.position.x));
        CHECK(a.position.y == doctest::Approx(e.position.y));
        CHECK(a.uv.x == doctest::Approx(e.uv.x));
        CHECK(a.uv.y == doctest::Approx(e.uv.y));
        check_color(a.premultiplied, e.premultiplied);
    }
}

} // namespace

TEST_CASE("panel recording orders nested translucent backgrounds before their children") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    constexpr std::array pixel{
        std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
    const auto texture = std::get<wui::TextureHandle>(
        textures->create(1, 1, pixel, wui::AlphaMode::straight));
    wui::Canvas canvas(textures);
    constexpr wui::Rect viewport{0, 0, 200, 160};
    constexpr wui::Rect outer_bounds{10, 10, 180, 120};
    constexpr wui::Rect outer_interior{18, 18, 164, 104};
    constexpr wui::Rect inner_bounds{20, 20, 160, 60};
    constexpr wui::Rect inner_interior{28, 28, 144, 44};

    canvas.begin(viewport, 1);
    canvas.rounded_rect(viewport, 0, {0, 0, 0, 0.5f});
    std::string title = "Outer";
    const auto outer = CanvasAccess::open_panel(
        canvas, {1}, title, {10, 10, 180, 0}, 8, {1, 0, 0, 0.5f});
    title = "Changed after open";
    // Equal provisional clips deliberately make texture/clip compatibility
    // insufficient to distinguish panel boundaries from adjacent child paint.
    canvas.push_clip(viewport);
    canvas.rounded_rect({30, 30, 20, 20}, 0, {0.5f, 0.5f, 0.5f, 0.5f});
    canvas.image({30, 30, 20, 20}, texture, {0, 0, 1, 1}, {1, 1, 1, 0.5f});
    const auto inner = CanvasAccess::open_panel(
        canvas, {2}, "Inner", {20, 20, 160, 0}, 8, {0, 1, 0, 0.5f});
    canvas.push_clip(viewport);
    canvas.rounded_rect({30, 30, 20, 20}, 0, {1, 1, 0, 0.5f});
    canvas.pop_clip();
    CanvasAccess::close_panel(canvas, inner, inner_bounds, inner_interior);
    canvas.rounded_rect({30, 90, 20, 20}, 0, {0, 1, 1, 0.5f});
    canvas.pop_clip();
    CanvasAccess::close_panel(canvas, outer, outer_bounds, outer_interior);
    canvas.rounded_rect({0, 140, 20, 20}, 0, {1, 0, 1, 0.5f});
    const auto packet = finish(canvas);

    constexpr std::array colors{
        wui::Color{0, 0, 0, 0.5f}, wui::Color{0.5f, 0, 0, 0.5f},
        wui::Color{0.25f, 0.25f, 0.25f, 0.5f},
        wui::Color{0.5f, 0.5f, 0.5f, 0.5f}, wui::Color{0, 0.5f, 0, 0.5f},
        wui::Color{0.5f, 0.5f, 0, 0.5f}, wui::Color{0, 0.5f, 0.5f, 0.5f},
        wui::Color{0.5f, 0, 0.5f, 0.5f}};
    const std::array clips{viewport, viewport, outer_interior, outer_interior,
                           outer_interior, inner_interior, outer_interior, viewport};
    REQUIRE(packet->batches.size() == colors.size());
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const auto& batch = packet->batches[i];
        check_color(drawn_vertex(*packet, batch, 0).premultiplied, colors[i]);
        check_rect(batch.clip, clips[i]);
        const auto& snapshot = packet->textures[batch.texture_index];
        if (i == 3) {
            CHECK(snapshot->handle == texture);
            CHECK(snapshot->rgba[0] == std::byte{0});
        } else {
            CHECK_FALSE(snapshot->handle == texture);
            CHECK(snapshot->rgba[0] == std::byte{255});
        }
    }
    REQUIRE(packet->semantics.size() == 2);
    CHECK(packet->semantics[0].id == wui::Id{1});
    CHECK(packet->semantics[0].label == "Outer");
    CHECK(packet->semantics[1].id == wui::Id{2});
    CHECK(packet->semantics[1].label == "Inner");
    check_rect(packet->semantics[0].bounds, outer_bounds);
    check_rect(packet->semantics[1].bounds, inner_bounds);
}

TEST_CASE("panel final geometry uses the final rounded topology including degenerate panels") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures);
    wui::Canvas reference(textures);
    wui::Rect bounds{10, 10, 80, 64};
    wui::Rect clip{0, 0, 120, 120};
    SUBCASE("height grows from zero into a fully rounded panel") {}
    SUBCASE("narrow width clamps the corner radius") { bounds.w = 3; }
    SUBCASE("empty width emits no background") { bounds.w = 0; }
    SUBCASE("empty height emits no background") { bounds.h = 0; }
    SUBCASE("fully clipped panel still records its final semantic bounds") {
        clip = {200, 200, 20, 20};
    }
    constexpr wui::Color color{0.4f, 0.2f, 0.8f, 0.5f};
    for (auto* target : {&canvas, &reference}) {
        target->begin({0, 0, 120, 120}, 1.5f);
        target->push_clip(clip);
    }
    const auto mark = CanvasAccess::open_panel(
        canvas, {3}, "Panel", {bounds.x, bounds.y, bounds.w, 0}, 24, color);
    CanvasAccess::close_panel(canvas, mark, bounds, bounds);
    reference.rounded_rect(bounds, 24, color);
    canvas.pop_clip();
    reference.pop_clip();
    const auto packet = finish(canvas);
    const auto expected = finish(reference);

    REQUIRE(packet->semantics.size() == 1);
    check_rect(packet->semantics[0].bounds, bounds);
    REQUIRE(packet->batches.size() == expected->batches.size());
    for (std::size_t i = 0; i < expected->batches.size(); ++i) {
        check_draw(*packet, packet->batches[i], *expected, expected->batches[i]);
    }
    if (bounds.w == 0 || bounds.h == 0 || clip.x == 200) {
        CHECK(packet->vertices.empty());
        CHECK(packet->indices.empty());
        CHECK(packet->textures.empty());
    }
}

TEST_CASE("final panel interiors intersect existing clips even when no child area remains") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 100, 100}, 1);
    const auto mark = CanvasAccess::open_panel(
        canvas, {4}, "Narrow", {10, 10, 4, 0}, 8, {1, 0, 0, 1});
    canvas.push_clip({10, 15, 4, 80});
    canvas.rounded_rect({10, 15, 4, 60}, 0, {0, 1, 0, 1});
    canvas.pop_clip();
    CanvasAccess::close_panel(canvas, mark, {10, 10, 4, 20}, {12, 12, 0, 16});
    const auto packet = finish(canvas);
    REQUIRE(packet->batches.size() == 2);
    check_rect(packet->batches[0].clip, {0, 0, 100, 100});
    check_rect(packet->batches[1].clip, {12, 15, 0, 13});
}

TEST_CASE("a held panel packet remains unchanged after recording into another slot") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures, 2);
    canvas.begin({0, 0, 120, 120}, 1);
    const auto first_mark = CanvasAccess::open_panel(
        canvas, {5}, "First", {10, 10, 80, 0}, 8, {1, 0, 0, 0.5f});
    canvas.rounded_rect({30, 30, 20, 20}, 2, {0, 1, 0, 1});
    CanvasAccess::close_panel(canvas, first_mark, {10, 10, 80, 80}, {18, 18, 64, 64});
    const auto first = finish(canvas);
    const wui::PacketData saved = *first;

    canvas.begin({0, 0, 240, 240}, 2);
    const auto second_mark = CanvasAccess::open_panel(
        canvas, {6}, "Second", {20, 20, 160, 0}, 24, {0, 0, 1, 1});
    CanvasAccess::close_panel(canvas, second_mark, {20, 20, 160, 160}, {44, 44, 112, 112});
    const auto second = finish(canvas);
    CHECK(second->semantics[0].label == "Second");
    CHECK(second->semantics[0].bounds.h == 160);

    check_rect(first->viewport, saved.viewport);
    CHECK(first->scale == saved.scale);
    REQUIRE(first->batches.size() == saved.batches.size());
    for (std::size_t i = 0; i < saved.batches.size(); ++i) {
        check_draw(*first, first->batches[i], saved, saved.batches[i]);
        CHECK(first->textures[first->batches[i].texture_index] ==
              saved.textures[saved.batches[i].texture_index]);
    }
    REQUIRE(first->semantics.size() == 1);
    CHECK(first->semantics[0].id == wui::Id{5});
    CHECK(first->semantics[0].label == "First");
    check_rect(first->semantics[0].bounds, {10, 10, 80, 80});
}

TEST_CASE("canvas status exposes failures and abort frees temporary stroke resources") {
    SUBCASE("construction errors are available before beginning") {
        wui::Canvas invalid(nullptr);
        const auto status = CanvasAccess::status(invalid);
        REQUIRE(status.has_value());
        CHECK(status->code == wui::ErrorCode::invalid_argument);
        CanvasAccess::abort(invalid);
    }
    SUBCASE("frame error remains visible and a later begin recovers") {
        auto textures = std::make_shared<wui::TextureStore>(4096);
        wui::Canvas canvas(textures);
        canvas.begin({0, 0, 50, 50}, 1);
        canvas.pop_clip();
        const auto status = CanvasAccess::status(canvas);
        REQUIRE(status.has_value());
        CHECK(status->code == wui::ErrorCode::unbalanced_scope);
        CanvasAccess::abort(canvas);
        CHECK(std::holds_alternative<wui::Error>(canvas.finish()));
        canvas.begin({0, 0, 50, 50}, 1);
        CHECK_FALSE(CanvasAccess::status(canvas).has_value());
        CHECK(finish(canvas)->batches.empty());
    }
    SUBCASE("aborting a coverage stroke returns its texture budget without publishing") {
        auto textures = std::make_shared<wui::TextureStore>(8192);
        wui::Canvas canvas(textures, 1);
        constexpr std::array path{
            wui::Vec2{10, 25}, wui::Vec2{40, 25}, wui::Vec2{10, 25}};
        canvas.begin({0, 0, 50, 50}, 1);
        canvas.line(path, 4, {1, 1, 1, 1});
        REQUIRE_FALSE(CanvasAccess::status(canvas).has_value());
        CanvasAccess::abort(canvas);
        const std::vector<std::byte> pixels(45 * 45 * 4, std::byte{255});
        auto created = textures->create(45, 45, pixels, wui::AlphaMode::straight);
        REQUIRE(std::holds_alternative<wui::TextureHandle>(created));
        REQUIRE_FALSE(textures->release(std::get<wui::TextureHandle>(created)).has_value());
        canvas.begin({0, 0, 50, 50}, 1);
        CHECK(finish(canvas)->batches.empty());
    }
}

TEST_CASE("canvas focus synchronization updates only enabled interactive semantics") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 100, 100}, 1);
    canvas.semantic({1}, wui::Role::button, "Old", {0, 0, 20, 20}, true, true, false);
    canvas.semantic({2}, wui::Role::checkbox, "New", {0, 20, 20, 20}, true, false, true);
    canvas.semantic({2}, wui::Role::button, "Disabled", {0, 40, 20, 20}, false, true, false);
    canvas.semantic({2}, wui::Role::text, "Text", {0, 60, 20, 20}, true, false, false);
    CanvasAccess::sync_focus(canvas, {2});
    const auto packet = finish(canvas);
    REQUIRE(packet->semantics.size() == 4);
    CHECK_FALSE(packet->semantics[0].focused);
    CHECK(packet->semantics[1].focused);
    CHECK(packet->semantics[1].selected);
    CHECK_FALSE(packet->semantics[2].focused);
    CHECK_FALSE(packet->semantics[3].focused);
}

TEST_CASE("nested panel reuse preserves bounds and indexing") {
    auto textures = std::make_shared<wui::TextureStore>(8192);
    wui::Canvas canvas(textures, 1);
    const std::string long_title =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";
    canvas.begin({0, 0, 200, 160}, 1);
    const auto outer_long = CanvasAccess::open_panel(
        canvas, {11}, long_title, {10, 10, 180, 0}, 8, {1, 0, 0, 0.5f});
    const auto inner_long = CanvasAccess::open_panel(
        canvas, {12}, long_title, {20, 20, 160, 0}, 8, {0, 1, 0, 0.5f});
    canvas.rounded_rect({30, 30, 20, 20}, 0, {0, 0, 1, 1});
    CanvasAccess::close_panel(canvas, inner_long, {20, 20, 160, 60}, {28, 28, 144, 44});
    CanvasAccess::close_panel(canvas, outer_long, {10, 10, 180, 120}, {18, 18, 164, 104});
    auto long_packet = finish(canvas);
    REQUIRE(long_packet->semantics.size() == 2);
    CHECK(long_packet->semantics[0].label == long_title);
    CHECK(long_packet->semantics[1].label == long_title);
    long_packet.reset();
    canvas.begin({0, 0, 200, 160}, 1);
    const auto outer = CanvasAccess::open_panel(
        canvas, {11}, "Outer", {10, 10, 180, 0}, 8, {1, 0, 0, 0.5f});
    const auto inner = CanvasAccess::open_panel(
        canvas, {12}, "Inner", {20, 20, 160, 0}, 8, {0, 1, 0, 0.5f});
    canvas.rounded_rect({30, 30, 20, 20}, 0, {0, 0, 1, 1});
    CanvasAccess::close_panel(canvas, inner, {20, 20, 160, 60}, {28, 28, 144, 44});
    CanvasAccess::close_panel(canvas, outer, {10, 10, 180, 120}, {18, 18, 164, 104});
    const auto packet = finish(canvas);
    REQUIRE(packet->semantics.size() == 2);
    CHECK(packet->semantics[0].label == "Outer");
    CHECK(packet->semantics[1].label == "Inner");
    check_rect(packet->semantics[0].bounds, {10, 10, 180, 120});
    check_rect(packet->semantics[1].bounds, {20, 20, 160, 60});
    REQUIRE(packet->batches.size() == 3);
}
