#include <doctest/doctest.h>

#include <wui/canvas.hpp>

#include "../../src/paint/shapes_detail.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <ostream>
#include <variant>
#include <vector>

namespace {

constexpr std::array white{
    std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
constexpr std::array red{
    std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
constexpr std::array blue{
    std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};

wui::TextureHandle create_pixel(wui::TextureStore& store,
                                std::span<const std::byte> pixel) {
    return std::get<wui::TextureHandle>(
        store.create(1, 1, pixel, wui::AlphaMode::straight));
}

wui::DrawPacket packet(wui::Canvas& canvas) {
    auto result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
    return std::get<wui::DrawPacket>(std::move(result));
}

void check_rect(wui::Rect actual, wui::Rect expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.w == doctest::Approx(expected.w));
    CHECK(actual.h == doctest::Approx(expected.h));
}

bool strictly_inside_triangle(wui::Vec2 point, wui::Vec2 a, wui::Vec2 b,
                              wui::Vec2 c) {
    const auto cross = [](wui::Vec2 u, wui::Vec2 v, wui::Vec2 w) {
        return (v.x - u.x) * (w.y - u.y) - (v.y - u.y) * (w.x - u.x);
    };
    const float ab = cross(a, b, point);
    const float bc = cross(b, c, point);
    const float ca = cross(c, a, point);
    constexpr float edge_epsilon = 1.0e-4f;
    return (ab > edge_epsilon && bc > edge_epsilon && ca > edge_epsilon) ||
           (ab < -edge_epsilon && bc < -edge_epsilon && ca < -edge_epsilon);
}

} // namespace

TEST_CASE("canvas preserves transparent submission order and only merges adjacent draws") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    const auto first = create_pixel(*textures, red);
    const auto second = create_pixel(*textures, blue);
    wui::Canvas canvas(textures);

    canvas.begin({0, 0, 100, 100}, 1.0f);
    canvas.image({0, 0, 50, 50}, first, {0, 0, 1, 1}, {1, 1, 1, 0.4f});
    canvas.image({20, 20, 50, 50}, second, {0, 0, 1, 1}, {1, 1, 1, 0.5f});
    canvas.image({40, 40, 50, 50}, first, {0, 0, 1, 1}, {1, 1, 1, 0.6f});
    const auto result = packet(canvas);

    REQUIRE(result->batches.size() == 3);
    REQUIRE(result->textures.size() == 2);
    CHECK(result->textures[0]->handle == first);
    CHECK(result->textures[1]->handle == second);
    CHECK(result->batches[0].texture_index == 0);
    CHECK(result->batches[1].texture_index == 1);
    CHECK(result->batches[2].texture_index == 0);
    CHECK(result->batches[0].first_index < result->batches[1].first_index);
    CHECK(result->batches[1].first_index < result->batches[2].first_index);
    CHECK_FALSE(wui::validate_packet(*result).has_value());
}

TEST_CASE("adjacent compatible images merge and nested clips intersect") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    const auto texture = create_pixel(*textures, white);
    wui::Canvas canvas(textures);

    canvas.begin({0, 0, 100, 80}, 1.0f);
    canvas.push_clip({10, 5, 40, 20});
    canvas.push_clip({20, 0, 40, 20});
    canvas.image({0, 0, 20, 20}, texture, {0, 0, 1, 1}, {1, 1, 1, 1});
    canvas.image({20, 0, 20, 20}, texture, {0, 0, 1, 1}, {1, 1, 1, 1});
    canvas.pop_clip();
    canvas.pop_clip();
    const auto result = packet(canvas);

    REQUIRE(result->batches.size() == 1);
    CHECK(result->batches[0].index_count == 12);
    check_rect(result->batches[0].clip, {20, 5, 30, 15});
}

TEST_CASE("an empty clip emits no geometry") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 100, 100}, 1.0f);
    canvas.push_clip({200, 200, 10, 10});
    canvas.rounded_rect({10, 10, 40, 30}, 6.0f, {1, 1, 1, 0.5f});
    canvas.pop_clip();
    const auto result = packet(canvas);

    CHECK(result->vertices.empty());
    CHECK(result->indices.empty());
    CHECK(result->batches.empty());
    CHECK(result->textures.empty());
    CHECK_FALSE(wui::validate_packet(*result).has_value());
}

TEST_CASE("rounded rectangles add a one-physical-pixel transparent outer ring") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 100, 100}, 2.0f);
    canvas.rounded_rect({10, 10, 20, 20}, 10.0f, {0.8f, 0.6f, 0.4f, 0.5f});
    const auto result = packet(canvas);

    REQUIRE_FALSE(result->vertices.empty());
    float outer_left = std::numeric_limits<float>::infinity();
    float inner_left = std::numeric_limits<float>::infinity();
    std::size_t outer_count = 0;
    for (const auto& vertex : result->vertices) {
        if (vertex.premultiplied.a == 0.0f) {
            ++outer_count;
            outer_left = std::min(outer_left, vertex.position.x);
            CHECK(vertex.premultiplied.r == 0.0f);
            CHECK(vertex.premultiplied.g == 0.0f);
            CHECK(vertex.premultiplied.b == 0.0f);
        } else {
            inner_left = std::min(inner_left, vertex.position.x);
            CHECK(vertex.premultiplied.a == doctest::Approx(0.5f));
        }
    }
    CHECK(outer_count * 2 == result->vertices.size());
    CHECK(inner_left == doctest::Approx(10.0f));
    CHECK(outer_left == doctest::Approx(9.5f).epsilon(0.001));
    CHECK_FALSE(wui::validate_packet(*result).has_value());
}

TEST_CASE("circle-like rounded rectangle fringe has no coincident cardinal vertices") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    wui::Canvas canvas(textures);
    for (const float scale : {1.0f, 2.0f}) {
        canvas.begin({0, 0, 640, 480}, scale);
        canvas.rounded_rect({504, 316, 56, 56}, 28.0f, {1, 1, 1, 1});
        const auto result = packet(canvas);
        REQUIRE_FALSE(result->vertices.empty());
        for (const auto& vertex : result->vertices) {
            if (vertex.premultiplied.a == 0.0f) {
                const float dx = vertex.position.x - 532.0f;
                const float dy = vertex.position.y - 344.0f;
                const float outward = std::sqrt(dx * dx + dy * dy) - 28.0f;
                CHECK(outward >= 0.99f / scale);
                CHECK(outward <= 1.02f / scale);
            }
        }
    }
}

TEST_CASE("convex fills and round polylines use valid antialiased meshes") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures);
    constexpr std::array polygon{
        wui::Vec2{10, 10}, wui::Vec2{80, 10}, wui::Vec2{60, 60}, wui::Vec2{20, 70}};
    constexpr std::array path{
        wui::Vec2{10, 80}, wui::Vec2{40, 30}, wui::Vec2{80, 80}};

    canvas.begin({0, 0, 100, 100}, 1.5f);
    canvas.convex(polygon, {1, 0, 0, 0.35f});
    canvas.line(path, 5.0f, {0, 1, 0, 0.35f});
    const auto result = packet(canvas);

    CHECK(result->batches.size() == 1);
    CHECK(result->vertices.size() > polygon.size() * 2);
    CHECK_FALSE(wui::validate_packet(*result).has_value());
    // Each connected mesh keeps its interior and fringe winding consistent.
    for (std::size_t i = 0; i < result->indices.size(); i += 3) {
        const auto ia = result->indices[i];
        const auto ib = result->indices[i + 1];
        const auto ic = result->indices[i + 2];
        const auto a = result->vertices[ia].position;
        const auto b = result->vertices[ib].position;
        const auto c = result->vertices[ic].position;
        const auto area = (b.x - a.x) * (c.y - a.y) -
                          (b.y - a.y) * (c.x - a.x);
        CHECK((ia < polygon.size() * 2 ? area : -area) >= 0.0f);
    }
}

TEST_CASE("translucent polyline interior has one coherent coverage layer") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures);
    constexpr std::array path{
        wui::Vec2{76, 174}, wui::Vec2{124, 128},
        wui::Vec2{186, 178}, wui::Vec2{250, 130}};
    canvas.begin({0, 0, 300, 220}, 1.0f);
    canvas.line(path, 10.0f, {0.61f, 0.55f, 1.0f, 0.5f});
    const auto result = packet(canvas);
    REQUIRE_FALSE(result->indices.empty());

    for (float y = 120.25f; y < 185.0f; y += 0.5f) {
        for (float x = 68.25f; x < 258.0f; x += 0.5f) {
            std::size_t layers = 0;
            for (std::size_t i = 0; i < result->indices.size(); i += 3) {
                const auto& a = result->vertices[result->indices[i]];
                const auto& b = result->vertices[result->indices[i + 1]];
                const auto& c = result->vertices[result->indices[i + 2]];
                // Count fringe triangles too: a shared boundary must not
                // introduce a second coverage layer around a join or cap.
                if (strictly_inside_triangle({x, y}, a.position, b.position, c.position)) {
                    ++layers;
                }
            }
            CHECK(layers <= 1);
        }
    }
}

TEST_CASE("canvas resolves the current immutable texture revision at finish") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    const auto texture = create_pixel(*textures, red);
    wui::Canvas canvas(textures);

    canvas.begin({0, 0, 10, 10}, 1.0f);
    canvas.image({0, 0, 10, 10}, texture, {0, 0, 1, 1}, {1, 1, 1, 1});
    REQUIRE_FALSE(textures->update(texture, blue).has_value());
    const auto result = packet(canvas);

    REQUIRE(result->textures.size() == 1);
    CHECK(result->textures[0]->revision == 2);
    CHECK(result->textures[0]->rgba[2] == std::byte{255});
}

TEST_CASE("finished packet storage remains immutable and is bounded") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    wui::Canvas canvas(textures, 1);

    canvas.begin({0, 0, 20, 20}, 1.0f);
    canvas.rounded_rect({0, 0, 10, 10}, 2.0f, {1, 1, 1, 1});
    auto first = packet(canvas);
    const auto original_vertices = first->vertices.size();
    const auto original_position = first->vertices.front().position;

    canvas.begin({0, 0, 30, 30}, 1.0f);
    canvas.rounded_rect({5, 5, 20, 20}, 4.0f, {1, 0, 0, 1});
    const auto busy = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(busy));
    CHECK(std::get<wui::Error>(busy).code == wui::ErrorCode::busy);
    CHECK(first->vertices.size() == original_vertices);
    CHECK(first->vertices.front().position.x == original_position.x);
    CHECK(first->vertices.front().position.y == original_position.y);

    first.reset();
    canvas.begin({0, 0, 30, 30}, 1.0f);
    canvas.rounded_rect({5, 5, 20, 20}, 4.0f, {1, 0, 0, 1});
    CHECK(std::holds_alternative<wui::DrawPacket>(canvas.finish()));
}

TEST_CASE("unbalanced and nonfinite canvas input fails finalization") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    wui::Canvas canvas(textures);

    SUBCASE("popping the root clip") {
        canvas.begin({0, 0, 10, 10}, 1.0f);
        canvas.pop_clip();
        const auto result = canvas.finish();
        REQUIRE(std::holds_alternative<wui::Error>(result));
        CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::unbalanced_scope);
    }
    SUBCASE("leaving a clip pushed") {
        canvas.begin({0, 0, 10, 10}, 1.0f);
        canvas.push_clip({0, 0, 5, 5});
        const auto result = canvas.finish();
        REQUIRE(std::holds_alternative<wui::Error>(result));
        CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::unbalanced_scope);
    }
    SUBCASE("nonfinite viewport") {
        canvas.begin({0, 0, std::numeric_limits<float>::infinity(), 10}, 1.0f);
        const auto result = canvas.finish();
        REQUIRE(std::holds_alternative<wui::Error>(result));
        CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
    }
    SUBCASE("nonfinite shape before premultiplication") {
        canvas.begin({0, 0, 10, 10}, 1.0f);
        canvas.rounded_rect({0, 0, 5, 5}, 1,
                            {1, 1, 1, std::numeric_limits<float>::quiet_NaN()});
        const auto result = canvas.finish();
        REQUIRE(std::holds_alternative<wui::Error>(result));
        CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
    }
}

TEST_CASE("canvas copies semantic labels and state") {
    auto textures = std::make_shared<wui::TextureStore>(1024);
    wui::Canvas canvas(textures);
    std::string label = "Apply";
    canvas.begin({0, 0, 100, 100}, 1.0f);
    canvas.semantic({42}, wui::Role::button, label, {10, 20, 30, 40},
                    false, true, true);
    label = "changed";
    const auto result = packet(canvas);

    REQUIRE(result->semantics.size() == 1);
    CHECK(result->semantics[0].id.value == 42);
    CHECK(result->semantics[0].label == "Apply");
    CHECK_FALSE(result->semantics[0].enabled);
    CHECK(result->semantics[0].focused);
    CHECK(result->semantics[0].selected);
}

TEST_CASE("gaussian weights are normalized and symmetric") {
    const auto weights = wui::detail::gaussian_kernel(2.0f);
    REQUIRE(weights.size() == 13);
    CHECK(std::accumulate(weights.begin(), weights.end(), 0.0f) ==
          doctest::Approx(1.0f).epsilon(0.00001));
    for (std::size_t i = 0; i < weights.size(); ++i) {
        CHECK(weights[i] == doctest::Approx(weights[weights.size() - 1 - i]));
    }
}

TEST_CASE("shadow cache reuses matching physical radius and sigma") {
    auto textures = std::make_shared<wui::TextureStore>(8 * 1024 * 1024);
    wui::Canvas canvas(textures);

    canvas.begin({0, 0, 200, 200}, 2.0f);
    canvas.shadow({20, 20, 80, 60}, 8.0f, 2.0f, {0, 0, 0, 0.5f});
    const auto first = packet(canvas);
    REQUIRE(first->textures.size() == 1);

    canvas.begin({0, 0, 200, 200}, 2.0f);
    canvas.shadow({40, 40, 100, 80}, 8.0f, 2.0f, {0, 0, 0, 0.5f});
    const auto second = packet(canvas);
    REQUIRE(second->textures.size() == 1);
    CHECK(second->textures[0]->handle == first->textures[0]->handle);
    CHECK(second->textures[0] == first->textures[0]);
    CHECK_FALSE(wui::validate_packet(*second).has_value());
}

TEST_CASE("a full shadow cache evicts only entries without packet leases") {
    auto textures = std::make_shared<wui::TextureStore>(32 * 1024 * 1024);
    wui::Canvas canvas(textures, 2);
    canvas.begin({0, 0, 300, 300}, 1.0f);
    for (std::size_t i = 0; i < wui::detail::shadow_cache_capacity; ++i) {
        canvas.shadow({20, 20, 200, 160}, static_cast<float>(i + 1), 0.5f,
                      {0, 0, 0, 0.4f});
    }
    auto leased = packet(canvas);
    REQUIRE(leased->textures.size() == wui::detail::shadow_cache_capacity);

    canvas.begin({0, 0, 300, 300}, 1.0f);
    canvas.shadow({20, 20, 200, 160},
                  static_cast<float>(wui::detail::shadow_cache_capacity + 1),
                  0.5f, {0, 0, 0, 0.4f});
    const auto full = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(full));
    CHECK(std::get<wui::Error>(full).code == wui::ErrorCode::capacity);

    leased.reset();
    canvas.begin({0, 0, 300, 300}, 1.0f);
    canvas.shadow({20, 20, 200, 160},
                  static_cast<float>(wui::detail::shadow_cache_capacity + 1),
                  0.5f, {0, 0, 0, 0.4f});
    CHECK(std::holds_alternative<wui::DrawPacket>(canvas.finish()));
}

TEST_CASE("shadow cache does not evict handles referenced by the active recording") {
    auto textures = std::make_shared<wui::TextureStore>(32 * 1024 * 1024);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 300, 300}, 1.0f);
    for (std::size_t i = 0; i <= wui::detail::shadow_cache_capacity; ++i) {
        canvas.shadow({20, 20, 200, 160}, static_cast<float>(i + 1), 0.5f,
                      {0, 0, 0, 0.4f});
    }
    const auto result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::capacity);
}

TEST_CASE("shadow validates physical sigma before kernel generation") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);

    canvas.begin({0, 0, 100, 100}, 2.0f);
    canvas.shadow({10, 10, 40, 40}, 4.0f, 0.2f, {0, 0, 0, 0.5f});
    auto result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);

    canvas.begin({0, 0, 100, 100}, 2.0f);
    canvas.shadow({10, 10, 40, 40}, 4.0f, 16.0f, {0, 0, 0, 0.5f});
    CHECK(std::holds_alternative<wui::DrawPacket>(canvas.finish()));

    canvas.begin({0, 0, 100, 100}, 2.0f);
    canvas.shadow({10, 10, 40, 40}, 4.0f, 16.1f, {0, 0, 0, 0.5f});
    result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
}

TEST_CASE("zero-radius shadow rasterizes only its center shape before blur") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 100, 100}, 1.0f);
    canvas.shadow({10, 10, 40, 30}, 0.0f, 0.5f, {0, 0, 0, 0.5f});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 1);
    const auto& texture = *result->textures[0];
    REQUIRE(texture.width == texture.height);
    const auto center = (static_cast<std::size_t>(texture.height / 2) * texture.width +
                         texture.width / 2) * 4 + 3;
    const auto corner = std::size_t{3};
    CHECK(std::to_integer<unsigned int>(texture.rgba[corner]) * 4 <
          std::to_integer<unsigned int>(texture.rgba[center]));
}

TEST_CASE("tiny positive strokes never index a collapsed outline") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    for (const float width : {std::numeric_limits<float>::denorm_min(), 1.0e-7f, 3.0e-6f}) {
        for (const float origin : {0.0f, 1000000.0f}) {
            const std::array path{wui::Vec2{origin + 10, 10}, wui::Vec2{origin + 30, 10}};
            canvas.begin({origin, 0, 50, 50}, 1.0f);
            canvas.line(path, width, {1, 1, 1, 0.5f});
            const auto result = packet(canvas);
            CHECK_FALSE(wui::validate_packet(*result).has_value());
        }
    }
}

TEST_CASE("sub-epsilon logical strokes retain visible physical coverage") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    constexpr std::array path{wui::Vec2{1.0e-6f, 2.0e-6f}, wui::Vec2{4.0e-6f, 2.0e-6f}};
    constexpr float scale = 1.0e7f;
    canvas.begin({0, 0, 5.0e-6f, 5.0e-6f}, scale);
    canvas.line(path, 1.0e-7f, {1, 1, 1, 0.5f});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 1);
    const auto& texture = *result->textures[0];
    const auto bounds = result->vertices.front().position;
    const auto x = static_cast<std::uint32_t>(25.5f - bounds.x * scale);
    const auto y = static_cast<std::uint32_t>(20.5f - bounds.y * scale);
    REQUIRE(x < texture.width);
    REQUIRE(y < texture.height);
    CHECK(texture.rgba[(y * texture.width + x) * 4 + 3] == std::byte{128});
    CHECK_FALSE(wui::validate_packet(*result).has_value());
}

TEST_CASE("crossing reversing and duplicate strokes render a single coverage union") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    std::vector<wui::Vec2> path;
    SUBCASE("crossing") { path = {{10, 10}, {40, 40}, {10, 40}, {40, 10}}; }
    SUBCASE("reversing") { path = {{10, 25}, {40, 25}, {10, 25}}; }
    SUBCASE("duplicate reversing") { path = {{10, 25}, {10, 25}, {40, 25}, {10, 25}}; }
    SUBCASE("tight reversal") { path = {{10, 25}, {40, 25}, {15, 26}}; }
    SUBCASE("short crossing segments") {
        path = {{20, 20}, {25, 20}, {25, 21}, {20, 21}, {20, 19}, {26, 19}};
    }
    SUBCASE("all points identical") { path = {{25, 25}, {25, 25}, {25, 25}}; }
    SUBCASE("closed tangent and endpoint") {
        path = {{10, 10}, {40, 10}, {40, 40}, {10, 40}, {10, 10}, {25, 25}};
    }
    canvas.begin({0, 0, 50, 50}, 2.0f);
    canvas.line(path, 6.0f, {0.6f, 0.5f, 1, 0.5f});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 1);
    REQUIRE(result->textures[0]->width > 1);
    CHECK_FALSE(wui::validate_packet(*result).has_value());
    const auto& texture = *result->textures[0];
    const auto bounds = result->vertices.front().position;
    constexpr std::array offsets{0.125, 0.375, 0.625, 0.875};
    for (std::uint32_t y = 0; y < texture.height; ++y) {
        for (std::uint32_t x = 0; x < texture.width; ++x) {
            unsigned covered = 0;
            for (const double oy : offsets) {
                for (const double ox : offsets) {
                    const double px = bounds.x + (x + ox) / 2.0;
                    const double py = bounds.y + (y + oy) / 2.0;
                    bool inside = false;
                    for (std::size_t i = 1; i < path.size(); ++i) {
                        const auto a = path[i - 1], b = path[i];
                        const double dx = b.x - a.x, dy = b.y - a.y;
                        const double length_squared = dx * dx + dy * dy;
                        const double t = length_squared == 0 ? 0 :
                            std::clamp(((px - a.x) * dx + (py - a.y) * dy) /
                                           length_squared, 0.0, 1.0);
                        const double ex = px - a.x - t * dx, ey = py - a.y - t * dy;
                        inside |= ex * ex + ey * ey <= 9.0;
                    }
                    covered += inside;
                }
            }
            CHECK(std::to_integer<unsigned>(texture.rgba[(y * texture.width + x) * 4 + 3]) ==
                  (covered * 255 + 8) / 16);
        }
    }
    for (const auto& vertex : result->vertices) {
        CHECK(vertex.premultiplied.a == 0.5f);
    }
}

TEST_CASE("stroke union textures preserve order clip and immutable packet lifetime") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::DrawPacket retained;
    wui::TextureHandle handle;
    {
        wui::Canvas canvas(textures);
        constexpr std::array path{wui::Vec2{10, 25}, wui::Vec2{40, 25}, wui::Vec2{10, 25}};
        canvas.begin({0, 0, 50, 50}, 2.0f);
        canvas.rounded_rect({0, 0, 50, 50}, 0, {1, 0, 0, 0.5f});
        canvas.push_clip({20, 20, 10, 10});
        canvas.line(path, 6, {0, 1, 0, 0.5f});
        canvas.pop_clip();
        canvas.rounded_rect({0, 0, 50, 50}, 0, {0, 0, 1, 0.5f});
        retained = packet(canvas);
        REQUIRE(retained->batches.size() == 3);
        check_rect(retained->batches[1].clip, {20, 20, 10, 10});
        CHECK(retained->batches[0].texture_index == retained->batches[2].texture_index);
        CHECK(retained->batches[1].texture_index != retained->batches[0].texture_index);
        handle = retained->textures[retained->batches[1].texture_index]->handle;
    }
    CHECK_FALSE(wui::validate_packet(*retained).has_value());
    // Recording ownership ends after publication; the immutable packet owns pixels.
    CHECK(std::holds_alternative<wui::Error>(textures->snapshot(handle)));
}

TEST_CASE("stroke union raster work and texture storage have typed capacity limits") {
    std::size_t budget = 1024 * 1024;
    std::vector<wui::Vec2> path{{10, 25}, {40, 25}, {10, 25}};
    wui::Rect viewport{0, 0, 50, 50};
    SUBCASE("texture store budget") { budget = 4; }
    SUBCASE("physical raster dimensions") {
        path = {{10, 25}, {4000, 25}, {10, 25}};
        viewport.w = 5000;
    }
    SUBCASE("pixel budget") {
        path = {{10, 10}, {1100, 1100}, {10, 10}};
        viewport = {0, 0, 1200, 1200};
    }
    SUBCASE("sample segment work budget") {
        path.clear();
        for (std::size_t i = 0; i < 129; ++i) {
            path.push_back(i % 2 ? wui::Vec2{190, 190} : wui::Vec2{10, 10});
        }
        viewport = {0, 0, 200, 200};
    }
    SUBCASE("input point budget") {
        path.clear();
        for (std::size_t i = 0; i <= wui::detail::stroke_point_limit; ++i) {
            path.push_back(i % 2 ? wui::Vec2{1, 1} : wui::Vec2{0, 0});
        }
        viewport = {0, 0, 1, 1};
    }
    auto textures = std::make_shared<wui::TextureStore>(budget);
    wui::Canvas canvas(textures);
    canvas.begin(viewport, 1.0f);
    canvas.line(path, 6, {1, 1, 1, 0.5f});
    const auto result = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::capacity);
}

TEST_CASE("shadow cache distinguishes exact physical sigma and radius") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 100, 100}, 1.0f);
    canvas.shadow({10, 10, 40, 40}, 4.0f, 2.01f, {0, 0, 0, 0.5f});
    canvas.shadow({10, 10, 40, 40}, 4.0f, 2.02f, {0, 0, 0, 0.5f});
    canvas.shadow({10, 10, 40, 40}, 4.01f, 2.02f, {0, 0, 0, 0.5f});
    canvas.shadow({10, 10, 40, 40}, 4.02f, 2.02f, {0, 0, 0, 0.5f});
    canvas.shadow({10, 10, 40, 40}, 4.0f, 2.01f, {0, 0, 0, 0.5f});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 4);
    REQUIRE(result->batches.size() == 5);
    CHECK(result->batches.front().texture_index == result->batches.back().texture_index);
    CHECK(result->textures[0]->rgba != result->textures[1]->rgba);
}

TEST_CASE("failed stroke recording releases its transient texture budget") {
    auto textures = std::make_shared<wui::TextureStore>(5000);
    wui::Canvas canvas(textures);
    constexpr std::array path{wui::Vec2{10, 25}, wui::Vec2{40, 25}, wui::Vec2{10, 25}};
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 0.5f});
    canvas.pop_clip();
    REQUIRE(std::holds_alternative<wui::Error>(canvas.finish()));
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 0.5f});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 1);
    CHECK(result->textures.front()->width > 1);
}

TEST_CASE("failed snapshot finalization releases unpublished stroke leases immediately") {
    auto textures = std::make_shared<wui::TextureStore>(1220);
    wui::Canvas canvas(textures);
    constexpr std::array path{wui::Vec2{10, 25}, wui::Vec2{40, 25}, wui::Vec2{10, 25}};
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 1});
    canvas.image({0, 0, 1, 1}, {std::numeric_limits<std::uint32_t>::max(), 1},
                 {0, 0, 1, 1}, {1, 1, 1, 1});
    const auto failed = canvas.finish();
    REQUIRE(std::holds_alternative<wui::Error>(failed));
    CHECK(std::get<wui::Error>(failed).code == wui::ErrorCode::stale_texture);
    CHECK(std::holds_alternative<wui::TextureHandle>(
        textures->create(1, 1, white, wui::AlphaMode::straight)));
}

TEST_CASE("reusing one packet slot retires dropped textures in other free slots") {
    auto textures = std::make_shared<wui::TextureStore>(2436);
    wui::Canvas canvas(textures);
    constexpr std::array path{wui::Vec2{10, 25}, wui::Vec2{40, 25}, wui::Vec2{10, 25}};
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 1});
    auto first = packet(canvas);
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 1});
    auto second = packet(canvas);
    first.reset();
    second.reset();
    constexpr std::array larger{wui::Vec2{10, 21}, wui::Vec2{40, 21},
                                wui::Vec2{10, 29}, wui::Vec2{10, 21}};
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(larger, 6, {1, 1, 1, 1});
    const auto result = packet(canvas);
    CHECK_FALSE(wui::validate_packet(*result).has_value());
}

TEST_CASE("clipped reversing strokes preserve coverage with distant endpoints") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    constexpr std::array path{
        wui::Vec2{-1.0e20f, 25}, wui::Vec2{1.0e20f, 25}, wui::Vec2{-1.0e20f, 25}};
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 0.5f});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 1);
    const auto& texture = *result->textures[0];
    const auto bounds = result->vertices.front().position;
    for (std::uint32_t y = 0; y < texture.height; ++y) {
        for (std::uint32_t x = 0; x < texture.width; ++x) {
            const float py = bounds.y + y + 0.5f;
            const auto expected = py > 22 && py < 28 ? std::byte{255} : std::byte{0};
            CHECK(texture.rgba[(y * texture.width + x) * 4 + 3] == expected);
        }
    }
    CHECK_FALSE(wui::validate_packet(*result).has_value());
}

TEST_CASE("a finite stroke endpoint clips a remote segment without erasing its body") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    constexpr std::array path{wui::Vec2{-1.0e20f, 25}, wui::Vec2{40, 25}};
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 1});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 1);
    const auto& texture = *result->textures[0];
    const auto origin = result->vertices.front().position;
    constexpr std::array offsets{0.125, 0.375, 0.625, 0.875};
    for (std::uint32_t y = 0; y < texture.height; ++y) {
        for (std::uint32_t x = 0; x < texture.width; ++x) {
            unsigned covered = 0;
            for (const auto oy : offsets) {
                for (const auto ox : offsets) {
                    const double dx = std::max(0.0, origin.x + x + ox - 40);
                    const double dy = origin.y + y + oy - 25;
                    covered += dx * dx + dy * dy <= 9;
                }
            }
            CHECK(texture.rgba[(y * texture.width + x) * 4 + 3] ==
                  static_cast<std::byte>((covered * 255 + 8) / 16));
        }
    }
}

TEST_CASE("distant diagonal stroke retains viewport-local perpendicular distance") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    constexpr std::array path{
        wui::Vec2{-1.0e20f, -1.0e20f}, wui::Vec2{1.0e20f, 1.0e20f},
        wui::Vec2{-1.0e20f, -1.0e20f}};
    canvas.begin({0, 0, 50, 50}, 1);
    canvas.line(path, 6, {1, 1, 1, 0.5f});
    const auto result = packet(canvas);
    REQUIRE(result->textures.size() == 1);
    const auto& texture = *result->textures[0];
    const auto origin = result->vertices.front().position;
    constexpr std::array offsets{0.125, 0.375, 0.625, 0.875};
    for (std::uint32_t y = 0; y < texture.height; ++y) {
        for (std::uint32_t x = 0; x < texture.width; ++x) {
            unsigned covered = 0;
            for (const auto oy : offsets) {
                for (const auto ox : offsets) {
                    const double delta = (origin.x + x + ox) - (origin.y + y + oy);
                    covered += delta * delta <= 18.0;
                }
            }
            CHECK(texture.rgba[(y * texture.width + x) * 4 + 3] ==
                  static_cast<std::byte>((covered * 255 + 8) / 16));
        }
    }
}

TEST_CASE("shadow raster uses the validated sigma without quantization") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    wui::DrawPacket first;
    for (const float scale : {1.0f, 2.0f}) {
        canvas.begin({0, 0, 100, 100}, scale);
        canvas.shadow({10, 10, 40, 40}, 0, 0.51f / scale, {0, 0, 0, 0.5f});
        const auto result = packet(canvas);
        REQUIRE(result->textures.size() == 1);
        const auto& texture = *result->textures[0];
        if (first) {
            CHECK(result->textures[0] == first->textures[0]);
        } else {
            first = result;
        }
        // Radius zero produces a single covered center texel. Its separable
        // Gaussian convolution is the product of the two normalized weights.
        const double sigma = 0.51f;
        const int radius = static_cast<int>(std::ceil(3 * sigma));
        REQUIRE(texture.width == static_cast<unsigned>(2 * radius + 1));
        REQUIRE(texture.height == texture.width);
        double total = 0;
        for (int i = -radius; i <= radius; ++i) {
            total += std::exp(-0.5 * i * i / (sigma * sigma));
        }
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                const auto expected = std::lround(
                    255 * std::exp(-0.5 * (x * x + y * y) / (sigma * sigma)) /
                    (total * total));
                const auto index = ((y + radius) * texture.width + x + radius) * 4 + 3;
                CHECK(std::to_integer<long>(texture.rgba[index]) == expected);
            }
        }
    }
}

TEST_CASE("gradient vertices encode premultiplied endpoints and border leaves a hole") {
    auto textures = std::make_shared<wui::TextureStore>(4096);
    wui::Canvas canvas(textures);
    canvas.begin({0, 0, 100, 100}, 1);
    canvas.gradient({10, 10, 30, 20}, {1, 0, 0, 0.5f}, {0, 0, 1, 0.25f});
    canvas.border({10, 40, 30, 20}, 4, 2, {0, 1, 0, 0.5f});
    const auto result = packet(canvas);
    REQUIRE_FALSE(wui::validate_packet(*result).has_value());
    REQUIRE(result->vertices.size() > 4);
    CHECK(result->vertices[0].premultiplied.r == doctest::Approx(0.5f));
    CHECK(result->vertices[0].premultiplied.a == doctest::Approx(0.5f));
    CHECK(result->vertices[2].premultiplied.b == doctest::Approx(0.25f));
    CHECK(result->vertices[2].premultiplied.a == doctest::Approx(0.25f));
    const wui::Vec2 center{25, 50};
    for (std::size_t i = 6; i < result->indices.size(); i += 3) {
        const auto a = result->vertices[result->indices[i]].position;
        const auto b = result->vertices[result->indices[i + 1]].position;
        const auto c = result->vertices[result->indices[i + 2]].position;
        CHECK_FALSE(strictly_inside_triangle(center, a, b, c));
    }
    for (std::size_t i = 4; i < result->vertices.size(); ++i) {
        const auto& vertex = result->vertices[i];
        if (vertex.premultiplied.a > 0) {
            CHECK(vertex.position.x >= 10);
            CHECK(vertex.position.x <= 40);
            CHECK(vertex.position.y >= 40);
            CHECK(vertex.position.y <= 60);
        }
    }
}
