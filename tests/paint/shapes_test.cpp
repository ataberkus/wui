#include <doctest/doctest.h>

#include <wui/canvas.hpp>

#include "../../src/paint/canvas_access.hpp"
#include "../../src/paint/glyph_quad.hpp"
#include "../../src/paint/shapes_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
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

TEST_CASE("shadow cache does not evict handles referenced by a closed overlay layer") {
    using wui::detail::CanvasAccess;
    constexpr wui::Rect viewport{0, 0, 300, 300};
    auto textures = std::make_shared<wui::TextureStore>(32 * 1024 * 1024);
    // A shadow inside an overlay layer: end_layer parks the layer's batches
    // until finish() appends them, and the shadow stays in use meanwhile.
    bool elevated = false;
    SUBCASE("a shadow drawn in the layer") {}
    SUBCASE("the elevated sheet of a panel closed in the layer") { elevated = true; }
    const auto layered = [&](wui::Canvas& canvas) {
        canvas.begin(viewport, 1.0f);
        const auto layer = CanvasAccess::begin_layer(canvas, viewport);
        if (elevated) {
            const auto mark = CanvasAccess::open_panel(canvas, {9}, {}, {100, 100, 120, 0}, 8,
                                                       {0.2f, 0.2f, 0.25f, 1});
            CanvasAccess::close_panel(canvas, mark, {100, 100, 120, 80}, {108, 108, 104, 64},
                                      {{0, 0, 0, 0.8f}, 4, 4, {0, 0, 0, 0}});
        } else {
            canvas.shadow({100, 104, 120, 80}, 8, 4, {0, 0, 0, 0.8f});
        }
        CanvasAccess::end_layer(canvas, layer);
    };
    // Newer keys drawn after the layer closed, in the main order.
    const auto main_shadows = [](wui::Canvas& canvas, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            canvas.shadow({20, 20, 200, 160}, static_cast<float>(i + 1), 0.5f, {0, 0, 0, 0.4f});
        }
    };
    {
        // Filling the cache exactly evicts nothing; the layer paints last
        // (an elevated sheet above its shadow).
        wui::Canvas canvas(textures);
        layered(canvas);
        main_shadows(canvas, wui::detail::shadow_cache_capacity - 1);
        const auto full = packet(canvas);
        REQUIRE(full->textures.size() == wui::detail::shadow_cache_capacity + (elevated ? 1 : 0));
        const auto& shadow = full->batches[full->batches.size() - (elevated ? 2 : 1)];
        CHECK(full->textures[shadow.texture_index]->width > 1);
    }
    // One more key finds every entry in use by this frame, the layer's
    // included: the request reports the full cache instead of releasing the
    // layer's texture (which made finish() fail with stale_texture).
    wui::Canvas canvas(textures);
    layered(canvas);
    main_shadows(canvas, wui::detail::shadow_cache_capacity);
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

namespace {

bool same_geometry(const wui::PacketData& a, const wui::PacketData& b) {
    return a.vertices.size() == b.vertices.size() && a.indices == b.indices &&
           a.batches.size() == b.batches.size() &&
           std::equal(a.vertices.begin(), a.vertices.end(), b.vertices.begin(),
                      [](const wui::Vertex& u, const wui::Vertex& v) {
                          return std::bit_cast<std::array<std::uint32_t, 8>>(u) ==
                                 std::bit_cast<std::array<std::uint32_t, 8>>(v);
                      });
}

void draw_rounded_scene(wui::Canvas& canvas, float offset) {
    canvas.rounded_rect({10 + offset, 10, 120, 32}, 6, {0.2f, 0.3f, 0.4f, 1});
    canvas.rounded_rect({10 + offset, 50, 40, 40}, 20, {1, 1, 1, 0.5f}); // circle
    canvas.rounded_rect({60 + offset, 50, 90, 24}, 12, {1, 0, 0, 1});    // capsule
    canvas.rounded_rect({10 + offset, 100, 30, 30}, 0, {0, 1, 0, 1});    // square corners
    canvas.border({50 + offset, 100, 80, 40}, 8, 2, {0, 0, 1, 1});
    canvas.rounded_rect({10 + offset, 10, 120, 32}, 6, {0.5f, 0.5f, 0.5f, 1}); // repeat
}

} // namespace

TEST_CASE("rounded shapes reused from earlier frames match a fresh tessellation") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas reused(textures);
    // Repeats, moves and returns exercise hits in the current and previous
    // frame's records as well as misses.
    for (const float scale : {1.0f, 1.5f}) {
        for (const float offset : {0.0f, 0.0f, 3.5f, 0.0f, 3.5f, 3.5f, 7.25f, 0.0f}) {
            reused.begin({0, 0, 300, 200}, scale);
            draw_rounded_scene(reused, offset);
            const auto actual = packet(reused);
            wui::Canvas fresh(textures);
            fresh.begin({0, 0, 300, 200}, scale);
            draw_rounded_scene(fresh, offset);
            const auto expected = packet(fresh);
            CHECK(same_geometry(*actual, *expected));
        }
    }
}

TEST_CASE("rounded shapes beyond the outline cache bounds are still drawn every frame") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    // Square-cornered shapes are small, so the entry bound is reached first.
    const std::size_t count = wui::detail::outline_cache_entry_limit + 300;
    REQUIRE(count * 8 < wui::detail::outline_cache_point_limit);
    wui::DrawPacket first;
    for (int frame = 0; frame < 3; ++frame) {
        canvas.begin({0, 0, 4096, 4096}, 1.0f);
        for (std::size_t i = 0; i < count; ++i) {
            canvas.rounded_rect({static_cast<float>(i % 200) * 20.0f,
                                 static_cast<float>(i / 200) * 20.0f, 10, 10},
                                0, {1, 1, 1, 1});
        }
        auto result = packet(canvas);
        // Four outline and four fringe vertices; two interior triangles and
        // two per fringe edge.
        REQUIRE(result->vertices.size() == count * 8);
        REQUIRE(result->indices.size() == count * 30);
        if (frame == 0) {
            first = std::move(result);
        } else {
            CHECK(same_geometry(*result, *first));
        }
    }
}

TEST_CASE("rounded shapes recorded only in the previous frame are reused when the cache is full") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas reused(textures);
    const std::size_t count = wui::detail::outline_cache_entry_limit;
    // Square-cornered shapes keep the point bound out of the way. Set A is
    // recorded in frame 0. Frame 1 fills the cache with set B before drawing
    // A again, so A is found among the previous frame's records but cannot
    // be carried over into the full current generation. In frame 2, A is
    // in neither generation and is tessellated again.
    const auto draw = [&](wui::Canvas& canvas, float y) {
        for (std::size_t i = 0; i < count; ++i) {
            canvas.rounded_rect({static_cast<float>(i % 256) * 16.0f,
                                 y + static_cast<float>(i / 256) * 16.0f, 10, 10},
                                0, {1, 1, 1, 1});
        }
    };
    const auto frame = [&](wui::Canvas& canvas, std::initializer_list<float> sets) {
        canvas.begin({0, 0, 4096, 8192}, 1.0f);
        for (const float y : sets) draw(canvas, y);
        // Signed zeros have distinct bits and tessellate to distinct vertices.
        canvas.rounded_rect({-0.0f, 0, 10, 10}, 2, {1, 1, 1, 1});
        canvas.rounded_rect({0.0f, 0, 10, 10}, 2, {1, 1, 1, 1});
        return packet(canvas);
    };
    constexpr float set_a = 0.0f;
    constexpr float set_b = 4096.0f;
    for (const auto sets : {std::initializer_list<float>{set_a},
                            std::initializer_list<float>{set_b, set_a},
                            std::initializer_list<float>{set_a}}) {
        const auto actual = frame(reused, sets);
        wui::Canvas fresh(textures);
        const auto expected = frame(fresh, sets);
        CHECK(same_geometry(*actual, *expected));
    }
}

namespace {

bool same_packet(const wui::PacketData& a, const wui::PacketData& b) {
    const auto same_rect = [](wui::Rect u, wui::Rect v) {
        return std::bit_cast<std::array<std::uint32_t, 4>>(u) ==
               std::bit_cast<std::array<std::uint32_t, 4>>(v);
    };
    return same_geometry(a, b) &&
           std::equal(a.batches.begin(), a.batches.end(), b.batches.begin(),
                      [&](const wui::Batch& u, const wui::Batch& v) {
                          return u.first_index == v.first_index &&
                                 u.index_count == v.index_count &&
                                 u.texture_index == v.texture_index &&
                                 u.base_vertex == v.base_vertex && same_rect(u.clip, v.clip);
                      }) &&
           a.textures.size() == b.textures.size() &&
           // Each canvas owns its own white texture: compare contents, not handles.
           std::equal(a.textures.begin(), a.textures.end(), b.textures.begin(),
                      [](const auto& u, const auto& v) {
                          return u->width == v->width && u->height == v->height &&
                                 u->alpha == v->alpha && u->rgba == v->rgba;
                      });
}

// Every quad- and mesh-emitting primitive, `copies` times.
void draw_mixed_scene(wui::Canvas& canvas, wui::TextureHandle texture, int copies) {
    for (int i = 0; i < copies; ++i) {
        const float y = static_cast<float>(i) * 7.0f;
        canvas.rounded_rect({4, y, 60, 20}, 5, {0.2f, 0.3f, 0.4f, 1});
        canvas.image({70, y, 9, 11}, texture, {0.25f, 0.5f, 0.25f, 0.5f}, {1, 1, 1, 0.75f});
        canvas.gradient({90, y, 30, 6}, {1, 0, 0, 1}, {0, 0, 1, 0.5f});
        canvas.border({130, y, 40, 16}, 4, 1.5f, {1, 1, 1, 0.5f});
        const std::array triangle{wui::Vec2{180, y}, wui::Vec2{200, y + 5}, wui::Vec2{185, y + 9}};
        canvas.convex(triangle, {0, 1, 0, 1});
        const std::array path{wui::Vec2{210, y}, wui::Vec2{230, y + 4}, wui::Vec2{250, y}};
        canvas.line(path, 2, {1, 1, 0, 1});
    }
}

} // namespace

TEST_CASE("a reused packet slot publishes exactly the geometry of each frame") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    const auto texture = create_pixel(*textures, red);
    wui::Canvas reused(textures, 1);
    // Growing, shrinking, failed and aborted frames leave retained elements
    // beyond what the next frame writes; none of them may be published.
    for (const int copies : {3, 40, 1, -40, 0, -1, 5, 60, 2}) {
        reused.begin({0, 0, 400, 600}, 1.25f);
        draw_mixed_scene(reused, texture, copies < 0 ? -copies : copies);
        if (copies < -1) {
            reused.image({0, 0, std::numeric_limits<float>::quiet_NaN(), 1}, texture,
                         {0, 0, 1, 1}, {1, 1, 1, 1});
            const auto failed = reused.finish();
            REQUIRE(std::holds_alternative<wui::Error>(failed));
            CHECK(std::get<wui::Error>(failed).code == wui::ErrorCode::invalid_argument);
            continue;
        }
        if (copies == -1) {
            wui::detail::CanvasAccess::abort(reused);
            continue;
        }
        const auto actual = packet(reused);
        CHECK_FALSE(wui::validate_packet(*actual).has_value());
        wui::Canvas fresh(textures, 1);
        fresh.begin({0, 0, 400, 600}, 1.25f);
        draw_mixed_scene(fresh, texture, copies);
        const auto expected = packet(fresh);
        CHECK(same_packet(*actual, *expected));
    }
}

namespace {

struct GlyphRunCase {
    std::vector<wui::GlyphQuad> glyphs;
    wui::Vec2 offset;
    wui::Color tint{1, 1, 1, 1};
};

struct GlyphRunOutcome {
    wui::Status status;
    wui::DrawPacket packet; // when finish() succeeded
};

// Paints `before`, the run, then `after`, with either the batched run or one
// Canvas::image per glyph, and finishes the frame.
template <typename Before, typename After>
GlyphRunOutcome paint_glyph_run(const std::shared_ptr<wui::TextureStore>& textures,
                                const GlyphRunCase& run, bool batched, Before before,
                                After after) {
    wui::Canvas canvas(textures, 1);
    canvas.begin({0, 0, 200, 100}, 1.5f);
    before(canvas);
    if (batched) {
        wui::detail::CanvasAccess::glyph_run(canvas, run.glyphs, run.offset, run.tint);
    } else {
        for (const auto& glyph : run.glyphs) {
            wui::Rect bounds = glyph.bounds;
            bounds.x += run.offset.x;
            bounds.y += run.offset.y;
            canvas.image(bounds, glyph.texture, glyph.uv, run.tint);
        }
    }
    GlyphRunOutcome outcome;
    outcome.status = wui::detail::CanvasAccess::status(canvas);
    after(canvas);
    auto finished = canvas.finish();
    if (auto* packet = std::get_if<wui::DrawPacket>(&finished)) {
        outcome.packet = std::move(*packet);
    } else {
        CHECK(outcome.status.has_value());
    }
    return outcome;
}

template <typename Before, typename After>
void check_glyph_run(const std::shared_ptr<wui::TextureStore>& textures, const GlyphRunCase& run,
                     Before before, After after) {
    const auto expected = paint_glyph_run(textures, run, false, before, after);
    const auto actual = paint_glyph_run(textures, run, true, before, after);
    REQUIRE(actual.status.has_value() == expected.status.has_value());
    if (expected.status) {
        CHECK(actual.status->code == expected.status->code);
        CHECK(actual.status->message == expected.status->message);
    }
    REQUIRE(static_cast<bool>(actual.packet) == static_cast<bool>(expected.packet));
    if (expected.packet) {
        CHECK_FALSE(wui::validate_packet(*actual.packet).has_value());
        CHECK(same_packet(*actual.packet, *expected.packet));
    }
}

} // namespace

TEST_CASE("a batched glyph run matches one image per glyph") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    const auto a = create_pixel(*textures, red);
    const auto b = create_pixel(*textures, blue);
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    const auto glyph = [](wui::TextureHandle texture, float x, float w = 6, float h = 9) {
        return wui::GlyphQuad{texture, {x, -7.25f, w, h}, {x / 64, 0.125f, w / 64, h / 64}};
    };
    const auto nothing = [](wui::Canvas&) {};
    const auto image_a = [&](wui::Canvas& canvas) {
        canvas.image({1, 2, 3, 4}, a, {0, 0, 1, 1}, {1, 1, 1, 1});
    };
    const auto image_b = [&](wui::Canvas& canvas) {
        canvas.image({1, 2, 3, 4}, b, {0, 0, 1, 1}, {1, 1, 1, 1});
    };
    const auto shapes_after = [&](wui::Canvas& canvas) {
        canvas.rounded_rect({5, 50, 40, 20}, 4, {0.5f, 0.5f, 0.5f, 1});
        canvas.image({60, 50, 7, 7}, b, {0, 0, 1, 1}, {1, 1, 1, 0.5f});
    };
    const wui::Vec2 offset{10.5f, 20.25f};
    const wui::Color tint{0.9f, 0.6f, 0.3f, 0.8f};

    SUBCASE("one texture, joined to a preceding image of the same texture") {
        const GlyphRunCase run{{glyph(a, 0), glyph(a, 7), glyph(a, 14)}, offset, tint};
        check_glyph_run(textures, run, image_a, shapes_after);
        check_glyph_run(textures, run, image_b, shapes_after);
        check_glyph_run(textures, run, nothing, nothing);
    }
    SUBCASE("texture changes break batches") {
        const GlyphRunCase run{{glyph(a, 0), glyph(a, 7), glyph(b, 14), glyph(b, 21),
                                glyph(a, 28), glyph(b, 35)},
                               offset, tint};
        check_glyph_run(textures, run, image_a, shapes_after);
        check_glyph_run(textures, run, image_b, shapes_after);
    }
    SUBCASE("a different clip or a layer keeps the run out of the previous batch") {
        const GlyphRunCase run{{glyph(a, 0), glyph(a, 7)}, offset, tint};
        check_glyph_run(textures, run, [&](wui::Canvas& canvas) {
            image_a(canvas);
            canvas.push_clip({0, 0, 150, 90});
        }, [](wui::Canvas& canvas) { canvas.pop_clip(); });
        wui::detail::CanvasLayer layer;
        check_glyph_run(textures, run, [&](wui::Canvas& canvas) {
            image_a(canvas);
            layer = wui::detail::CanvasAccess::begin_layer(canvas, {0, 0, 120, 80});
        }, [&](wui::Canvas& canvas) {
            wui::detail::CanvasAccess::end_layer(canvas, layer);
            shapes_after(canvas);
        });
    }
    SUBCASE("empty glyphs are skipped wherever they are") {
        check_glyph_run(textures, {{glyph(b, 0, 0, 9), glyph(a, 7), glyph(a, 14, 6, 0),
                                    glyph(b, 21, 0, 0), glyph(a, 28), glyph(b, 35, 0)},
                                   offset, tint},
                        image_a, shapes_after);
        check_glyph_run(textures, {{glyph(b, 0, 0, 9), glyph(a, 7, 4, 0)}, offset, tint},
                        image_a, shapes_after);
        check_glyph_run(textures, {{glyph(a, 0), glyph(b, 7, 0, 3), glyph(a, 14)}, offset, tint},
                        image_b, shapes_after);
    }
    SUBCASE("an empty run draws nothing") {
        check_glyph_run(textures, {{}, offset, tint}, image_a, shapes_after);
        check_glyph_run(textures, {{}, offset, {nan, 0, 0, 1}}, image_a, shapes_after);
    }
    SUBCASE("an invalid glyph fails at its position") {
        for (const auto bad : {wui::GlyphQuad{a, {nan, 0, 6, 9}, {0, 0, 1, 1}},
                               wui::GlyphQuad{a, {0, 0, -1, 9}, {0, 0, 1, 1}},
                               wui::GlyphQuad{a, {0, 0, 6, inf}, {0, 0, 1, 1}},
                               wui::GlyphQuad{a, {3.0e38f, 0, 6, 9}, {0, 0, 1, 1}},
                               wui::GlyphQuad{a, {0, 0, 6, 9}, {0, nan, 1, 1}},
                               wui::GlyphQuad{a, {0, 0, 6, 9}, {0, 0, 1, -0.5f}},
                               wui::GlyphQuad{a, {nan, 0, 0, 0}, {0, 0, 1, 1}}}) {
            for (std::size_t at = 0; at < 4; ++at) {
                std::vector<wui::GlyphQuad> glyphs{glyph(a, 0), glyph(b, 7), glyph(b, 14, 0),
                                                   glyph(a, 21)};
                glyphs.insert(glyphs.begin() + static_cast<std::ptrdiff_t>(at), bad);
                check_glyph_run(textures, {glyphs, offset, tint}, image_a, shapes_after);
                // A later invalid glyph is never reached.
                glyphs.push_back(wui::GlyphQuad{a, {nan, nan, nan, nan}, {}});
                check_glyph_run(textures, {glyphs, {3.0e38f, 0}, tint}, image_a, shapes_after);
                check_glyph_run(textures, {glyphs, offset, tint}, image_a, shapes_after);
            }
        }
    }
    SUBCASE("an invalid tint fails at the first glyph") {
        for (const wui::Color bad : {wui::Color{nan, 0, 0, 1}, wui::Color{0, 0, 0, 1.5f},
                                     wui::Color{0, -0.25f, 0, 1}, wui::Color{0, 0, inf, 1}}) {
            check_glyph_run(textures, {{glyph(a, 0, 0, 0), glyph(a, 7)}, offset, bad}, image_a,
                            shapes_after);
        }
    }
    SUBCASE("an empty clip draws nothing but still validates") {
        const auto hidden = [&](wui::Canvas& canvas) {
            image_a(canvas);
            canvas.push_clip({300, 300, 10, 10});
        };
        const auto shown = [&](wui::Canvas& canvas) {
            canvas.pop_clip();
            shapes_after(canvas);
        };
        check_glyph_run(textures, {{glyph(a, 0), glyph(b, 7)}, offset, tint}, hidden, shown);
        check_glyph_run(textures,
                        {{glyph(a, 0), wui::GlyphQuad{a, {0, 0, 1, 1}, {nan, 0, 1, 1}}},
                         offset, tint},
                        hidden, shown);
        check_glyph_run(textures, {{glyph(a, 0)}, offset, {0, 0, 0, 2}}, hidden, shown);
    }
    SUBCASE("a failed canvas ignores the run") {
        const auto failing = [&](wui::Canvas& canvas) {
            image_a(canvas);
            canvas.rounded_rect({0, 0, 1, 1}, -1, {1, 1, 1, 1});
        };
        check_glyph_run(textures, {{glyph(a, 0)}, offset, tint}, failing, shapes_after);
        check_glyph_run(textures, {{glyph(a, 0)}, offset, {nan, 0, 0, 1}}, failing, shapes_after);
    }
}

TEST_CASE("a batched glyph run outside an active frame does nothing") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    const auto a = create_pixel(*textures, red);
    wui::Canvas canvas(textures, 1);
    const std::array glyphs{wui::GlyphQuad{a, {0, 0, 4, 4}, {0, 0, 1, 1}}};
    wui::detail::CanvasAccess::glyph_run(canvas, glyphs, {}, {1, 1, 1, 1});
    CHECK_FALSE(wui::detail::CanvasAccess::status(canvas).has_value());
    canvas.begin({0, 0, 10, 10}, 1);
    const auto result = packet(canvas);
    CHECK(result->vertices.empty());
}

TEST_CASE("convex and border meshes list their triangles in outline order for every size") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    const auto texture = create_pixel(*textures, red);
    wui::Canvas canvas(textures, 1);
    // A leading quad gives every mesh a nonzero base vertex.
    const auto leading = [&] { canvas.image({0, 0, 1, 1}, texture, {0, 0, 1, 1}, {1, 1, 1, 1}); };
    // Point counts across every remainder of the blocked index writers.
    for (std::uint32_t count = 3; count <= 41; ++count) {
        std::vector<wui::Vec2> points(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
            points[i] = {50 + 40 * std::cos(angle), 50 + 40 * std::sin(angle)};
        }
        canvas.begin({0, 0, 100, 100}, 1.0f);
        leading();
        canvas.convex(points, {0.5f, 1, 0, 0.5f});
        const auto result = packet(canvas);
        const std::uint32_t b = 4;
        const std::uint32_t o = b + count;
        REQUIRE(result->vertices.size() == b + 2 * count);
        std::vector<std::uint32_t> expected{0, 1, 2, 0, 2, 3};
        for (std::uint32_t t = 1; t + 1 < count; ++t) {
            expected.insert(expected.end(), {b, b + t, b + t + 1});
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto next = i + 1 == count ? 0 : i + 1;
            expected.insert(expected.end(), {b + i, o + next, b + next, b + i, o + i, o + next});
        }
        CHECK(result->indices == expected);
        for (std::uint32_t i = 0; i < 2 * count; ++i) {
            const auto& vertex = result->vertices[b + i];
            CHECK(vertex.uv.x == 0.5f);
            CHECK(vertex.uv.y == 0.5f);
            CHECK(vertex.premultiplied.a == (i < count ? 0.5f : 0.0f));
            CHECK(vertex.premultiplied.r == (i < count ? 0.25f : 0.0f));
            if (i < count) {
                CHECK(vertex.position.x == points[i].x);
                CHECK(vertex.position.y == points[i].y);
            }
        }
    }
    // Borders: outline, inner and fringe rings of `count` vertices each.
    for (const float radius : {0.0f, 1.0f, 2.5f, 4.0f, 7.0f, 12.0f, 30.0f}) {
        for (const float scale : {1.0f, 1.25f, 2.0f}) {
            canvas.begin({0, 0, 100, 100}, scale);
            leading();
            canvas.border({10, 10, 70, 60}, radius, 1.5f, {1, 1, 1, 1});
            const auto result = packet(canvas);
            const std::uint32_t b = 4;
            REQUIRE((result->vertices.size() - b) % 3 == 0);
            const auto count = static_cast<std::uint32_t>((result->vertices.size() - b) / 3);
            REQUIRE(count >= 4);
            const std::uint32_t n = b + count;
            const std::uint32_t f = b + 2 * count;
            std::vector<std::uint32_t> expected{0, 1, 2, 0, 2, 3};
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto next = i + 1 == count ? 0 : i + 1;
                expected.insert(expected.end(),
                                {b + i, b + next, n + next, b + i, n + next, n + i,
                                 b + i, f + next, b + next, b + i, f + i, f + next});
            }
            CHECK(result->indices == expected);
        }
    }
}

TEST_CASE("rounded shapes stay exact across steady, churning and returning frames") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas reused(textures, 1);
    // A pool of rounded shapes (squares, circles, capsules, plain corners)
    // drawn in changing subsets: long steady stretches keep one recording
    // generation, growing churn makes it stale and starts a new one, and
    // shapes come back after being absent for many frames.
    const auto shape = [](std::uint32_t i) {
        const float x = static_cast<float>(i % 8) * 37.0f + 0.25f * static_cast<float>(i / 8);
        const float y = static_cast<float>(i / 8) * 29.0f;
        const float w = 12.0f + static_cast<float>(i % 5) * 7.0f;
        const float h = (i % 3 == 0) ? w : 10.0f + static_cast<float>(i % 4) * 5.0f;
        const float radius = (i % 6 == 0) ? 0.0f : (i % 6 == 1) ? w : static_cast<float>(i % 7);
        return std::pair{wui::Rect{x, y, w, h}, radius};
    };
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::vector<std::uint32_t> scene;
    for (int frame = 0; frame < 160; ++frame) {
        const int phase = frame / 20;
        if (phase % 2 == 0 && frame % 20 != 0) {
            // Steady: the same scene, now and then one shape swapped.
            if (frame % 7 == 0 && !scene.empty()) scene[next() % scene.size()] = next() % 400;
        } else {
            // Churn: mostly new shapes from a wide pool, a few old ones.
            scene.clear();
            const std::uint32_t count = 10 + next() % 30;
            for (std::uint32_t i = 0; i < count; ++i) {
                scene.push_back(i < 6 ? i : next() % (phase % 4 == 1 ? 400u : 60u));
            }
        }
        const float scale = phase % 3 == 2 ? 1.5f : 1.0f;
        const auto draw = [&](wui::Canvas& canvas) {
            canvas.begin({0, 0, 400, 400}, scale);
            for (const auto index : scene) {
                const auto [bounds, radius] = shape(index);
                canvas.rounded_rect(bounds, radius, {0.3f, 0.6f, 0.9f, 1});
                if (index % 4 == 0) canvas.border(bounds, radius, 1.0f, {1, 1, 1, 0.5f});
            }
            return packet(canvas);
        };
        const auto actual = draw(reused);
        wui::Canvas fresh(textures, 1);
        const auto expected = draw(fresh);
        REQUIRE(same_packet(*actual, *expected));
    }
}

namespace {

// Strokes and polygons of every mesh-cache path: round joins and caps,
// carets, a concave polygon whose fringe folds (hard edge), a convex
// chevron, repeats within one frame, and a stroke that falls back to a
// coverage raster (never recorded).
void draw_mesh_scene(wui::Canvas& canvas, float offset, float width) {
    const std::array check{wui::Vec2{10 + offset, 20}, wui::Vec2{14 + offset, 25},
                           wui::Vec2{24 + offset, 12}};
    canvas.line(check, width, {1, 1, 1, 1});
    const std::array caret{wui::Vec2{40, 10 + offset}, wui::Vec2{40, 30 + offset}};
    canvas.line(caret, 1, {0.9f, 0.9f, 0.9f, 1});
    const std::array chevron{wui::Vec2{60 + offset, 10}, wui::Vec2{70 + offset, 10},
                             wui::Vec2{65 + offset, 16}};
    canvas.polygon(chevron, {0.5f, 0.5f, 0.5f, 1});
    const std::array spike{wui::Vec2{80, 40}, wui::Vec2{120, 40}, wui::Vec2{100.5f, 41},
                           wui::Vec2{100, 80}, wui::Vec2{99.5f, 41}};
    canvas.polygon(spike, {0.2f, 0.7f, 0.3f, 0.75f});
    const std::array closed{wui::Vec2{10, 60}, wui::Vec2{40, 60 + offset}, wui::Vec2{25, 90},
                            wui::Vec2{10, 60}};
    canvas.polygon(closed, {1, 0, 0, 0.5f});
    const std::array reversal{wui::Vec2{50, 60}, wui::Vec2{70, 60}, wui::Vec2{52, 61}};
    canvas.line(reversal, 2, {1, 1, 0, 1});
    canvas.line(check, width, {0, 0, 1, 0.5f}); // repeat, another color
}

} // namespace

TEST_CASE("line and polygon meshes reused across frames match a fresh tessellation") {
    auto textures = std::make_shared<wui::TextureStore>(4 * 1024 * 1024);
    wui::Canvas reused(textures, 1);
    for (const float scale : {1.0f, 1.25f}) {
        for (const auto& [offset, width] :
             {std::pair{0.0f, 2.0f}, std::pair{0.0f, 2.0f}, std::pair{3.5f, 2.0f},
              std::pair{0.0f, 2.0f}, std::pair{0.0f, 1.5f}, std::pair{3.5f, 2.0f},
              std::pair{0.0f, 2.0f}, std::pair{-0.0f, 2.0f}}) {
            reused.begin({0, 0, 200, 120}, scale);
            draw_mesh_scene(reused, offset, width);
            const auto actual = packet(reused);
            wui::Canvas fresh(textures, 1);
            fresh.begin({0, 0, 200, 120}, scale);
            draw_mesh_scene(fresh, offset, width);
            const auto expected = packet(fresh);
            CHECK(same_packet(*actual, *expected));
        }
    }
}

TEST_CASE("cached polygons keep their validation and clipping behavior") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas canvas(textures);
    const std::array valid{wui::Vec2{10, 10}, wui::Vec2{30, 10}, wui::Vec2{20, 25}};
    const std::array crossing{wui::Vec2{10, 10}, wui::Vec2{30, 30}, wui::Vec2{30, 10},
                              wui::Vec2{10, 30}};
    const std::array caret{wui::Vec2{50, 10}, wui::Vec2{50, 30}};
    for (int frame = 0; frame < 3; ++frame) {
        // Recorded or reused: drawn when visible, skipped (without error)
        // under an empty clip.
        canvas.begin({0, 0, 100, 100}, 1.0f);
        canvas.push_clip({200, 200, 5, 5});
        canvas.polygon(valid, {1, 1, 1, 1});
        canvas.line(caret, 1, {1, 1, 1, 1});
        canvas.pop_clip();
        const auto hidden = packet(canvas);
        CHECK(hidden->indices.empty());

        canvas.begin({0, 0, 100, 100}, 1.0f);
        canvas.polygon(valid, {1, 1, 1, 1});
        canvas.line(caret, 1, {1, 1, 1, 1});
        const auto shown = packet(canvas);
        CHECK(shown->indices.size() > 0);

        // Invalid outlines are never recorded and fail every time, visible or not.
        for (const bool clipped : {false, true}) {
            canvas.begin({0, 0, 100, 100}, 1.0f);
            canvas.polygon(valid, {1, 1, 1, 1});
            if (clipped) canvas.push_clip({200, 200, 5, 5});
            canvas.polygon(crossing, {1, 1, 1, 1});
            if (clipped) canvas.pop_clip();
            const auto failed = canvas.finish();
            REQUIRE(std::holds_alternative<wui::Error>(failed));
            CHECK(std::get<wui::Error>(failed).code == wui::ErrorCode::invalid_argument);
        }
    }
}

TEST_CASE("strokes beyond the mesh cache bounds are still drawn every frame") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    wui::Canvas reused(textures);
    const std::size_t count = wui::detail::mesh_cache_entry_limit;
    // Set A alone overflows the entry bound. Frame 1 fills a fresh
    // generation with set B before A, so A is found in the previous
    // generation but cannot be carried over; frame 2 meets A in neither.
    const auto draw = [&](wui::Canvas& canvas, float y, std::size_t extra) {
        for (std::size_t i = 0; i < count + extra; ++i) {
            const float x = static_cast<float>(i % 64) * 60.0f;
            const float top = y + static_cast<float>(i / 64) * 30.0f;
            const std::array caret{wui::Vec2{x, top}, wui::Vec2{x, top + 10}};
            canvas.line(caret, 1, {1, 1, 1, 1});
        }
    };
    const auto frame = [&](wui::Canvas& canvas, bool with_b) {
        canvas.begin({0, 0, 4096, 8192}, 1.0f);
        if (with_b) draw(canvas, 4096.0f, 0);
        draw(canvas, 0.0f, 50);
        return packet(canvas);
    };
    for (const bool with_b : {false, true, false, false}) {
        const auto actual = frame(reused, with_b);
        wui::Canvas fresh(textures);
        const auto expected = frame(fresh, with_b);
        CHECK(same_packet(*actual, *expected));
    }
}

namespace {

// Canvas::image's handling of one glyph, written out independently.
wui::detail::GlyphVerdict reference_glyph(const wui::GlyphQuad& glyph, wui::Vec2 offset,
                                          wui::Color color, std::array<wui::Vertex, 4>& out,
                                          std::array<std::uint32_t, 6>& indices,
                                          std::uint32_t base) {
    const wui::Rect bounds{glyph.bounds.x + offset.x, glyph.bounds.y + offset.y, glyph.bounds.w,
                           glyph.bounds.h};
    const auto valid = [](wui::Rect r) {
        return std::isfinite(r.x + r.w) && std::isfinite(r.y + r.h) && r.w >= 0 && r.h >= 0;
    };
    if (!valid(bounds) || !valid(glyph.uv)) return wui::detail::GlyphVerdict::invalid;
    if (bounds.w == 0 || bounds.h == 0) return wui::detail::GlyphVerdict::empty;
    const auto uv = glyph.uv;
    out[0] = {{bounds.x, bounds.y}, {uv.x, uv.y}, color};
    out[1] = {{bounds.x + bounds.w, bounds.y}, {uv.x + uv.w, uv.y}, color};
    out[2] = {{bounds.x + bounds.w, bounds.y + bounds.h}, {uv.x + uv.w, uv.y + uv.h}, color};
    out[3] = {{bounds.x, bounds.y + bounds.h}, {uv.x, uv.y + uv.h}, color};
    indices = {base, base + 1, base + 2, base, base + 2, base + 3};
    return wui::detail::GlyphVerdict::quad;
}

template <typename Write>
void check_glyph_corners(Write write, const wui::GlyphQuad& glyph, wui::Vec2 offset,
                         wui::Color color, std::uint32_t base) {
    std::array<wui::Vertex, 4> expected{};
    std::array<std::uint32_t, 6> expected_indices{};
    const auto verdict = reference_glyph(glyph, offset, color, expected, expected_indices, base);
    std::array<wui::Vertex, 4> actual{};
    std::array<std::uint32_t, 8> actual_indices{}; // two slack slots
    REQUIRE(write(glyph, actual.data(), actual_indices.data(), base) == verdict);
    if (verdict != wui::detail::GlyphVerdict::quad) return;
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(std::bit_cast<std::array<std::uint32_t, 8>>(actual[i]) ==
              std::bit_cast<std::array<std::uint32_t, 8>>(expected[i]));
    }
    CHECK(std::equal(expected_indices.begin(), expected_indices.end(), actual_indices.begin()));
}

} // namespace

TEST_CASE("scalar and packed glyph corners match image() for every value class") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float big = std::numeric_limits<float>::max();
    const float tiny = std::numeric_limits<float>::denorm_min();
    const std::array specials{0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 7.25f, -3.75f, 1.0e-30f, tiny,
                              -tiny, big, -big, 3.0e38f, 1.0e20f, nan, inf, -inf, 0.125f};
    std::uint32_t seed = 987654321;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    const auto value = [&] {
        const auto pick = next();
        if (pick % 4 == 0) return specials[(pick >> 8) % specials.size()];
        // Mostly ordinary glyph-sized values, both signs, many magnitudes.
        const float magnitude = static_cast<float>((pick >> 8) % 100000) / 64.0f;
        return (pick & 0x10) ? -magnitude * 0.01f : magnitude;
    };
    // Extents are usually nonnegative, so all three verdicts are common.
    const auto extent = [&] {
        const float v = value();
        return next() % 4 != 0 ? std::abs(v) : v;
    };
    const wui::Color color{0.25f, 0.5f, 0.125f, 0.5f};
    std::array<int, 3> verdicts{};
    for (int i = 0; i < 20000; ++i) {
        const wui::GlyphQuad glyph{{next(), next()}, {value(), value(), extent(), extent()},
                                   {value(), value(), extent(), extent()}};
        const wui::Vec2 offset{value(), value()};
        const wui::detail::GlyphCorners corners(offset, color);
        const auto base = next() >> 4;
        std::array<wui::Vertex, 4> unused{};
        std::array<std::uint32_t, 6> unused_indices{};
        ++verdicts[static_cast<std::size_t>(
            reference_glyph(glyph, offset, color, unused, unused_indices, base))];
        check_glyph_corners([&](const auto&... args) { return corners.scalar(args...); }, glyph,
                            offset, color, base);
#ifdef WUI_PAINT_SSE2
        check_glyph_corners([&](const auto&... args) { return corners.packed(args...); }, glyph,
                            offset, color, base);
#endif
    }
    for (const int count : verdicts) CHECK(count > 500);
}
