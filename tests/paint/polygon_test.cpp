#include <doctest/doctest.h>

#include <wui/canvas.hpp>

#include "../../src/paint/canvas_access.hpp"

#include <array>
#include <memory>
#include <variant>
#include <vector>

namespace {

wui::DrawPacket finish(wui::Canvas& canvas) {
    auto result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
    return std::get<wui::DrawPacket>(std::move(result));
}

// Fully opaque interior triangles of `batch` covering `point` (fringe
// triangles have a transparent vertex and are ignored).
int opaque_cover(const wui::PacketData& packet, const wui::Batch& batch, wui::Vec2 point) {
    int count = 0;
    for (std::uint32_t i = batch.first_index; i + 2 < batch.first_index + batch.index_count; i += 3) {
        const auto& a = packet.vertices[packet.indices[i]];
        const auto& b = packet.vertices[packet.indices[i + 1]];
        const auto& c = packet.vertices[packet.indices[i + 2]];
        if (a.premultiplied.a < 1 || b.premultiplied.a < 1 || c.premultiplied.a < 1) continue;
        const auto side = [&](wui::Vec2 u, wui::Vec2 v) {
            return (v.x - u.x) * (point.y - u.y) - (v.y - u.y) * (point.x - u.x);
        };
        const float ab = side(a.position, b.position);
        const float bc = side(b.position, c.position);
        const float ca = side(c.position, a.position);
        if ((ab > 0 && bc > 0 && ca > 0) || (ab < 0 && bc < 0 && ca < 0)) ++count;
    }
    return count;
}

} // namespace

TEST_CASE("concave polygons fill each interior point once and leave notches empty") {
    wui::Canvas canvas(std::make_shared<wui::TextureStore>(1024 * 1024));
    canvas.begin({0, 0, 100, 100}, 1);
    // An L shape in both windings, with an explicit closing point on the second.
    const std::array<wui::Vec2, 6> clockwise{{{10, 10}, {60, 10}, {60, 30}, {30, 30}, {30, 60}, {10, 60}}};
    const std::array<wui::Vec2, 7> counter{{{10, 10}, {10, 60}, {30, 60}, {30, 30}, {60, 30}, {60, 10}, {10, 10}}};
    canvas.polygon(clockwise, {1, 0, 0, 1});
    canvas.polygon(counter, {0, 1, 0, 1});
    const auto packet = finish(canvas);
    REQUIRE(packet->batches.size() == 1); // same texture and clip merge
    const auto& batch = packet->batches.front();
    // Interior: (n - 2) triangles per polygon; fringe: two per edge.
    CHECK(batch.index_count == (4 * 3 + 6 * 6) * 2);
    // Sample points avoid every possible triangulation diagonal.
    for (const wui::Vec2 inside : {wui::Vec2{17, 23}, wui::Vec2{47, 19}, wui::Vec2{19, 47}})
        CHECK(opaque_cover(*packet, batch, inside) == 2);
    for (const wui::Vec2 notch : {wui::Vec2{45, 41}, wui::Vec2{55, 57}, wui::Vec2{33, 41}})
        CHECK(opaque_cover(*packet, batch, notch) == 0);
}

TEST_CASE("self-intersecting, degenerate and oversized polygons are rejected") {
    wui::Canvas canvas(std::make_shared<wui::TextureStore>(1024 * 1024));
    canvas.begin({0, 0, 100, 100}, 1);
    const std::array<wui::Vec2, 4> bowtie{{{10, 10}, {50, 50}, {50, 10}, {10, 50}}};
    canvas.polygon(bowtie, {1, 1, 1, 1});
    auto result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);

    canvas.begin({0, 0, 100, 100}, 1);
    const std::array<wui::Vec2, 3> line{{{0, 0}, {10, 10}, {20, 20}}};
    canvas.polygon(line, {1, 1, 1, 1});
    result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);

    canvas.begin({0, 0, 100, 100}, 1);
    std::vector<wui::Vec2> many(600, wui::Vec2{1, 1});
    canvas.polygon(many, {1, 1, 1, 1});
    result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::capacity);
}

TEST_CASE("four-corner gradients interpolate bilinearly with exact edges") {
    wui::Canvas canvas(std::make_shared<wui::TextureStore>(1024 * 1024));
    canvas.begin({0, 0, 100, 100}, 1);
    const wui::Color top_left{1, 0, 0, 1}, top_right{0, 1, 0, 1},
        bottom_right{0, 0, 1, 1}, bottom_left{0, 0, 0, 0.5f};
    canvas.gradient({10, 20, 80, 40}, top_left, top_right, bottom_right, bottom_left);
    const auto packet = finish(canvas);
    REQUIRE(packet->vertices.size() == 81);
    const auto at = [&](std::size_t x, std::size_t y) { return packet->vertices[y * 9 + x]; };
    CHECK(at(0, 0).position.x == 10);
    CHECK(at(8, 8).position.x == 90);
    CHECK(at(8, 8).position.y == 60);
    CHECK(at(0, 0).premultiplied.r == doctest::Approx(1));
    CHECK(at(8, 0).premultiplied.g == doctest::Approx(1));
    CHECK(at(8, 8).premultiplied.b == doctest::Approx(1));
    // Premultiplied black at half alpha.
    CHECK(at(0, 8).premultiplied.a == doctest::Approx(0.5f));
    // Center: average of the premultiplied corners.
    const auto center = at(4, 4).premultiplied;
    CHECK(center.r == doctest::Approx(0.25f));
    CHECK(center.g == doctest::Approx(0.25f));
    CHECK(center.b == doctest::Approx(0.25f));
    CHECK(center.a == doctest::Approx(0.875f));
}

TEST_CASE("detached layers paint above later content with a root-bounded replacement clip") {
    wui::Canvas canvas(std::make_shared<wui::TextureStore>(1024 * 1024));
    canvas.begin({0, 0, 200, 100}, 1);
    canvas.push_clip({0, 0, 50, 50});
    const auto layer = wui::detail::CanvasAccess::begin_layer(canvas, {40, 40, 300, 30});
    canvas.rounded_rect({60, 45, 20, 10}, 0, {1, 0, 0, 1});
    const auto inner = wui::detail::CanvasAccess::begin_layer(canvas, {0, 0, 10, 10});
    canvas.rounded_rect({1, 1, 2, 2}, 0, {0, 0, 1, 1});
    wui::detail::CanvasAccess::end_layer(canvas, inner);
    wui::detail::CanvasAccess::end_layer(canvas, layer);
    canvas.rounded_rect({5, 5, 10, 10}, 0, {0, 1, 0, 1});
    canvas.pop_clip();
    const auto packet = finish(canvas);
    REQUIRE(packet->batches.size() == 3);
    // Main paint first; then the outer layer; the nested layer stays above it.
    CHECK(packet->batches[0].clip.w == 50);
    const auto outer_clip = packet->batches[1].clip;
    CHECK(outer_clip.x == 40);
    CHECK(outer_clip.w == 160); // replacement clip, bounded only by the viewport
    CHECK(outer_clip.h == 30);
    CHECK(packet->batches[2].clip.w == 10);
    const auto first_vertex = [&](std::size_t batch) {
        return packet->vertices[packet->indices[packet->batches[batch].first_index]];
    };
    CHECK(first_vertex(0).premultiplied.g == doctest::Approx(1));
    CHECK(first_vertex(1).premultiplied.r == doctest::Approx(1));
    CHECK(first_vertex(2).premultiplied.b == doctest::Approx(1));
}
