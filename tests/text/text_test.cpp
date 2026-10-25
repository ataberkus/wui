#include <doctest/doctest.h>
#include <wui/text.hpp>
#include <wui/canvas.hpp>
#include <wui/wui.hpp>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <utf8proc.h>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <vector>

namespace {
std::vector<std::byte> load_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    REQUIRE(stream.is_open());
    const auto size = stream.tellg();
    REQUIRE(size >= 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
    return bytes;
}
struct Fixture {
    std::shared_ptr<wui::TextureStore> textures;
    std::shared_ptr<wui::IText> service;
    explicit Fixture(std::size_t budget = 64 * 1024 * 1024)
        : textures(std::make_shared<wui::TextureStore>(budget)) {
        const auto regular = load_bytes(std::filesystem::path(WUI_FONT_DIR) / "Inter-Regular.ttf");
        const auto semibold = load_bytes(std::filesystem::path(WUI_FONT_DIR) / "Inter-SemiBold.ttf");
        auto result = wui::make_text_service(regular, semibold, textures);
        REQUIRE(std::holds_alternative<std::shared_ptr<wui::IText>>(result));
        service = std::get<std::shared_ptr<wui::IText>>(std::move(result));
    }
    std::shared_ptr<const wui::TextRun> shape(std::string_view text, float scale = 1) {
        auto result = service->shape({text, wui::FontRole::body, 14, scale, "tr"});
        REQUIRE(std::holds_alternative<std::shared_ptr<const wui::TextRun>>(result));
        return std::get<std::shared_ptr<const wui::TextRun>>(std::move(result));
    }
};
std::vector<std::byte> font_bytes(const char* name) {
    return load_bytes(std::filesystem::path(WUI_FONT_DIR) / name);
}
std::shared_ptr<wui::IText> make_service(wui::FontFaces faces) {
    auto result = wui::make_text_service(
        faces, std::make_shared<wui::TextureStore>(64 * 1024 * 1024));
    REQUIRE(std::holds_alternative<std::shared_ptr<wui::IText>>(result));
    return std::get<std::shared_ptr<wui::IText>>(std::move(result));
}
std::shared_ptr<const wui::TextRun> shape_role(wui::IText& service, std::string_view text,
                                               wui::FontRole role) {
    auto result = service.shape({text, role, 20, 1, "und"});
    REQUIRE(std::holds_alternative<std::shared_ptr<const wui::TextRun>>(result));
    return std::get<std::shared_ptr<const wui::TextRun>>(std::move(result));
}
bool same_quad(const wui::GlyphQuad& left, const wui::GlyphQuad& right) {
    return left.texture == right.texture && left.uv.x == right.uv.x &&
           left.uv.y == right.uv.y && left.uv.w == right.uv.w && left.uv.h == right.uv.h;
}
}

TEST_CASE("real typography shapes Turkish and combines accent graphemes") {
    Fixture f;
    const auto run = f.shape("İstanbul, ışık, şğçöü, e\xCC\x81");
    CHECK(run->width > 0);
    CHECK(run->height > 0);
    REQUIRE_FALSE(run->glyphs.empty());
    REQUIRE_FALSE(f.service->publish());
    for (const auto& glyph : run->glyphs) {
        CHECK(std::holds_alternative<std::shared_ptr<const wui::TextureSnapshot>>(
            f.textures->snapshot(glyph.texture)));
    }
    const auto combined = f.shape("e\xCC\x81");
    REQUIRE(combined->grapheme_starts.size() == 1);
    CHECK(combined->grapheme_starts[0] == 0);
}

TEST_CASE("text dependency runtime versions match the locked sources") {
    FT_Library library = nullptr;
    REQUIRE(FT_Init_FreeType(&library) == 0);
    int major = 0, minor = 0, patch = 0;
    FT_Library_Version(library, &major, &minor, &patch);
    FT_Done_FreeType(library);
    CHECK(major == 2);
    CHECK(minor == 14);
    CHECK(patch == 3);
    CHECK(std::string(hb_version_string()) == "14.4.0");
    CHECK(std::string(utf8proc_version()) == "2.11.3");
}

TEST_CASE("malformed display text is replaced without changing caller bytes") {
    Fixture f;
    const std::string source = std::string("A") + char(0xff) + "B";
    const auto before = source;
    const auto malformed = f.shape(source);
    const auto sanitized = f.shape("A?B");
    CHECK(source == before);
    CHECK(malformed->display == "A?B");
    CHECK(malformed->width == sanitized->width);
    REQUIRE(malformed->glyphs.size() == sanitized->glyphs.size());
    for (std::size_t i = 0; i < malformed->glyphs.size(); ++i) {
        CHECK(malformed->glyphs[i].uv.x == sanitized->glyphs[i].uv.x);
        CHECK(malformed->glyphs[i].uv.y == sanitized->glyphs[i].uv.y);
    }
}

TEST_CASE("narrow descriptions consume whole combining graphemes and respect newlines") {
    Fixture f;
    const auto combined = f.shape("e\xCC\x81");
    const auto x_run = f.shape("X");
    auto created = wui::Context::create(
        {wui::Theme::dark(), f.textures, f.service});
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::Context>>(created));
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    auto begun = context->begin_frame({{0, 0, 200, 120}});
    REQUIRE(std::holds_alternative<wui::Frame>(begun));
    auto frame = std::get<wui::Frame>(std::move(begun));
    const std::array columns{wui::Column{wui::ColumnKind::fixed, 4},
                             wui::Column{wui::ColumnKind::fixed, 80}};
    {
        auto row = frame.row("text", columns);
        { auto cell = row.cell();
          frame.description("e\xCC\x81" "X", {.color = wui::Color{1, 0, 0, 1}}); }
        { auto cell = row.cell(); frame.description("A\nB"); }
    }
    auto finished = frame.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
    const auto packet = std::get<wui::DrawPacket>(std::move(finished));
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    REQUIRE(packet->semantics.size() == 2);
    CHECK(packet->semantics[0].bounds.h == doctest::Approx(2 * combined->height));
    CHECK(packet->semantics[1].bounds.h == doctest::Approx(2 * combined->height));
    CHECK(packet->semantics[0].label == "e\xCC\x81" "X");
    CHECK(packet->semantics[1].label == "A\nB");
    std::size_t first_red = packet->vertices.size();
    for (std::size_t i = 0; i < packet->vertices.size(); ++i) {
        const auto color = packet->vertices[i].premultiplied;
        if (color.r > 0.9f && color.g == 0 && color.b == 0) {
            first_red = i;
            break;
        }
    }
    REQUIRE(first_red + (combined->glyphs.size() + 1) * 4 <= packet->vertices.size());
    CHECK(packet->vertices[first_red + 1].position.x > 4);
    const auto& next_line = packet->vertices[first_red + combined->glyphs.size() * 4];
    CHECK(next_line.uv.x == doctest::Approx(x_run->glyphs.front().uv.x));
    CHECK(next_line.position.y ==
          doctest::Approx(x_run->glyphs.front().bounds.y + combined->height));
    bool narrow_clip = false, wide_clip = false;
    for (const auto& batch : packet->batches) {
        narrow_clip |= batch.clip.x == 0 && batch.clip.w == 4;
        wide_clip |= batch.clip.x == 12 && batch.clip.w == 80;
    }
    CHECK(narrow_clip);
    CHECK(wide_clip);
}

TEST_CASE("description colors and clips follow resolved narrow and wide columns") {
    Fixture f;
    auto created = wui::Context::create({wui::Theme::dark(), f.textures, f.service});
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::Context>>(created));
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    auto begun = context->begin_frame({{0, 0, 360, 240}});
    REQUIRE(std::holds_alternative<wui::Frame>(begun));
    auto frame = std::get<wui::Frame>(std::move(begun));
    constexpr std::string_view words = "\xC4\xB0" "stanbul words break naturally across cells";
    const std::array columns{wui::Column{wui::ColumnKind::fixed, 90},
                             wui::Column{wui::ColumnKind::fixed, 240}};
    {
        auto row = frame.row("descriptions", columns);
        { auto cell = row.cell();
          frame.description(words, {.color = wui::Color{1, 0, 0, 1}}); }
        { auto cell = row.cell(); frame.description(words); }
    }
    auto finished = frame.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
    const auto packet = std::get<wui::DrawPacket>(std::move(finished));
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    REQUIRE(packet->semantics.size() == 2);
    CHECK(packet->semantics[0].bounds.h > packet->semantics[1].bounds.h);
    CHECK(packet->semantics[0].label == words);
    bool narrow_red = false, wide_secondary = false;
    const auto secondary = wui::Theme::dark().secondary;
    for (const auto& batch : packet->batches) {
        for (std::size_t i = batch.first_index; i < batch.first_index + batch.index_count; ++i) {
            const auto color = packet->vertices[packet->indices[i]].premultiplied;
            narrow_red |= batch.clip.x == 0 && batch.clip.w == 90 &&
                          color.r == 1 && color.g == 0 && color.b == 0;
            wide_secondary |= batch.clip.x == 98 && batch.clip.w == 240 &&
                              color.r == secondary.r && color.g == secondary.g &&
                              color.b == secondary.b;
        }
    }
    CHECK(narrow_red);
    CHECK(wide_secondary);
}

TEST_CASE("missing glyphs remain visible and shaping reuses owned cache entries") {
    Fixture f;
    std::string text = "Cache-owned text";
    const auto cached = f.shape(text);
    text.assign("changed caller bytes");
    CHECK(f.shape("Cache-owned text") == cached);
    const auto missing = f.shape("\xF4\x8F\xBF\xBF");
    const auto replacement = f.shape("\xEF\xBF\xBD");
    CHECK(missing->width == replacement->width);
    REQUIRE_FALSE(missing->glyphs.empty());
    REQUIRE_FALSE(f.service->publish());
    const auto snapshot = std::get<std::shared_ptr<const wui::TextureSnapshot>>(
        f.textures->snapshot(missing->glyphs.front().texture));
    bool visible = false;
    const auto uv = missing->glyphs.front().uv;
    const auto left = static_cast<unsigned>(std::lround(uv.x * snapshot->width));
    const auto top = static_cast<unsigned>(std::lround(uv.y * snapshot->height));
    const auto width = static_cast<unsigned>(std::lround(uv.w * snapshot->width));
    const auto height = static_cast<unsigned>(std::lround(uv.h * snapshot->height));
    for (unsigned y = top; y < top + height; ++y) {
        for (unsigned x = left; x < left + width; ++x) {
            visible |= snapshot->rgba[(y * snapshot->width + x) * 4 + 3] != std::byte{0};
        }
    }
    CHECK(visible);
}

TEST_CASE("DPI shaping preserves logical scale and separates physical atlas entries") {
    Fixture f;
    const auto reference = f.shape("Scaling");
    for (const float scale : {1.25f, 1.5f, 2.0f}) {
        const auto run = f.shape("Scaling", scale);
        CHECK(run->width == doctest::Approx(reference->width).epsilon(0.08));
        CHECK(run->baseline > 0);
        CHECK(run->height >= run->baseline);
        CHECK(f.shape("Scaling", scale) == run);
    }
}

TEST_CASE("new glyph publication cannot alter an old draw packet") {
    Fixture f;
    wui::Canvas canvas(f.textures);
    const auto first = f.shape("A");
    canvas.begin({0, 0, 100, 100}, 1);
    for (const auto& glyph : first->glyphs) {
        canvas.image(glyph.bounds, glyph.texture, glyph.uv, {1, 1, 1, 1});
    }
    REQUIRE_FALSE(f.service->publish());
    const auto old_packet = std::get<wui::DrawPacket>(canvas.finish());
    REQUIRE_FALSE(old_packet->textures.empty());
    const auto old_snapshot = old_packet->textures.front();
    const auto old_bytes = old_snapshot->rgba;
    const auto new_run = f.shape("ğ");
    REQUIRE_FALSE(f.service->publish());
    const auto current = std::get<std::shared_ptr<const wui::TextureSnapshot>>(
        f.textures->snapshot(new_run->glyphs.front().texture));
    CHECK(current->revision > old_snapshot->revision);
    CHECK(current->rgba != old_bytes);
    CHECK(old_snapshot->rgba == old_bytes);
    CHECK_FALSE(wui::validate_packet(*old_packet));
}

TEST_CASE("text rejects invalid font bytes and invalid shape requests") {
    const std::array invalid{std::byte{1}, std::byte{2}};
    const auto creation = wui::make_text_service(invalid, invalid,
        std::make_shared<wui::TextureStore>(64 * 1024 * 1024));
    REQUIRE(std::holds_alternative<wui::Error>(creation));
    CHECK(std::get<wui::Error>(creation).code == wui::ErrorCode::invalid_font);
    Fixture f;
    wui::ShapeRequest request{"A"};
    SUBCASE("nonfinite scale") { request.scale = std::numeric_limits<float>::infinity(); }
    SUBCASE("zero logical size") { request.logical_size = 0; }
    SUBCASE("unknown role") { request.role = static_cast<wui::FontRole>(9); }
    const auto result = f.service->shape(request);
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
}

TEST_CASE("glyph atlas page ceiling returns capacity without corrupting prior glyphs") {
    Fixture f;
    auto first = f.shape("A");
    bool exhausted = false;
    for (const char letter : std::string("MWNOQDHBAERSTUVXYZ")) {
        const std::string text(1, letter);
        const auto result = f.service->shape({text, wui::FontRole::heading, 900, 1, "und"});
        if (const auto* error = std::get_if<wui::Error>(&result)) {
            CHECK(error->code == wui::ErrorCode::capacity);
            exhausted = true;
            break;
        }
    }
    CHECK(exhausted);
    CHECK(f.shape("A") == first);
    REQUIRE_FALSE(f.service->publish());
    CHECK(std::holds_alternative<std::shared_ptr<const wui::TextureSnapshot>>(
        f.textures->snapshot(first->glyphs.front().texture)));
}

TEST_CASE("atlas publication retries capacity failures without losing staged glyphs") {
    Fixture f(8 * 1024 * 1024);
    const auto first = f.shape("A");
    const auto handle = first->glyphs.front().texture;
    REQUIRE_FALSE(f.service->publish());
    auto held = std::get<std::shared_ptr<const wui::TextureSnapshot>>(f.textures->snapshot(handle));
    const auto held_pixels = held->rgba;
    f.shape("B");
    REQUIRE_FALSE(f.service->publish());
    const auto before_failure = std::get<std::shared_ptr<const wui::TextureSnapshot>>(
        f.textures->snapshot(handle))->revision;
    f.shape("C");
    const auto failed = f.service->publish();
    REQUIRE(failed.has_value());
    CHECK(failed->code == wui::ErrorCode::capacity);
    CHECK(held->rgba == held_pixels);
    held.reset();
    REQUIRE_FALSE(f.service->publish());
    const auto published = std::get<std::shared_ptr<const wui::TextureSnapshot>>(
        f.textures->snapshot(handle));
    CHECK(published->revision == before_failure + 1);
    REQUIRE_FALSE(f.service->publish());
    CHECK(std::get<std::shared_ptr<const wui::TextureSnapshot>>(
        f.textures->snapshot(handle)) == published);
}

TEST_CASE("language interning rejects oversized process-lifetime tags") {
    Fixture f;
    const std::string oversized(64 * 1024, 'a');
    const auto result = f.service->shape({"A", wui::FontRole::body, 14, 1, oversized});
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::capacity);
    CHECK(f.shape("A")->width > 0);
}

TEST_CASE("display role shapes with a supplied display face without aliasing heading glyphs") {
    const auto regular = font_bytes("Inter-Regular.ttf");
    const auto semibold = font_bytes("Inter-SemiBold.ttf");
    const auto service = make_service({regular, semibold, regular});
    // W differs between Inter Regular and SemiBold.
    const auto display = shape_role(*service, "Wave", wui::FontRole::display);
    const auto body = shape_role(*service, "Wave", wui::FontRole::body);
    const auto heading = shape_role(*service, "Wave", wui::FontRole::heading);
    CHECK(display->width == body->width);
    CHECK(display->width != heading->width);
    REQUIRE(display->glyphs.size() == body->glyphs.size());
    REQUIRE_FALSE(display->glyphs.empty());
    REQUIRE_FALSE(heading->glyphs.empty());
    for (std::size_t i = 0; i < display->glyphs.size(); ++i) {
        CHECK(display->glyphs[i].bounds.x == body->glyphs[i].bounds.x);
        CHECK(display->glyphs[i].bounds.w == body->glyphs[i].bounds.w);
        CHECK(display->glyphs[i].bounds.h == body->glyphs[i].bounds.h);
    }
    CHECK_FALSE(same_quad(display->glyphs.front(), heading->glyphs.front()));
    CHECK(shape_role(*service, "Wave", wui::FontRole::display) == display);
    REQUIRE_FALSE(service->publish());
}

TEST_CASE("display role falls back to the semibold face when no display face is supplied") {
    const auto regular = font_bytes("Inter-Regular.ttf");
    const auto semibold = font_bytes("Inter-SemiBold.ttf");
    const auto service = make_service({regular, semibold, {}});
    const auto display = shape_role(*service, "Wave", wui::FontRole::display);
    const auto heading = shape_role(*service, "Wave", wui::FontRole::heading);
    const auto body = shape_role(*service, "Wave", wui::FontRole::body);
    CHECK(display->width == heading->width);
    CHECK(display->width != body->width);
    REQUIRE(display->glyphs.size() == heading->glyphs.size());
    for (std::size_t i = 0; i < display->glyphs.size(); ++i) {
        CHECK(same_quad(display->glyphs[i], heading->glyphs[i]));
    }
}

TEST_CASE("two-font overload still builds a service whose display role uses semibold") {
    Fixture f;
    const auto display = shape_role(*f.service, "Wave", wui::FontRole::display);
    const auto heading = shape_role(*f.service, "Wave", wui::FontRole::heading);
    CHECK(display->width == heading->width);
    REQUIRE_FALSE(display->glyphs.empty());
    REQUIRE(display->glyphs.size() == heading->glyphs.size());
    CHECK(same_quad(display->glyphs.front(), heading->glyphs.front()));
    CHECK(shape_role(*f.service, "Wave", wui::FontRole::body)->width < heading->width);
}

TEST_CASE("run cache evicts unheld runs so changing strings keep shaping") {
    Fixture f;
    bool all_shaped = true;
    for (std::size_t i = 0; i < 5000; ++i) {
        const auto text = "frame " + std::to_string(i);
        const auto result = f.service->shape({text, wui::FontRole::body, 14, 1, "und"});
        all_shaped &= std::holds_alternative<std::shared_ptr<const wui::TextRun>>(result);
    }
    CHECK(all_shaped);
}

TEST_CASE("run cache returns capacity only while every cached run is held") {
    Fixture f;
    std::vector<std::shared_ptr<const wui::TextRun>> held;
    held.reserve(4096);
    bool all_shaped = true;
    for (std::size_t i = 0; i < 4096; ++i) {
        const auto text = "held " + std::to_string(i);
        auto result = f.service->shape({text, wui::FontRole::body, 14, 1, "und"});
        if (auto* run = std::get_if<std::shared_ptr<const wui::TextRun>>(&result)) {
            held.push_back(std::move(*run));
        } else {
            all_shaped = false;
        }
    }
    REQUIRE(all_shaped);
    const auto full = f.service->shape({"one more", wui::FontRole::body, 14, 1, "und"});
    REQUIRE(std::holds_alternative<wui::Error>(full));
    CHECK(std::get<wui::Error>(full).code == wui::ErrorCode::capacity);
    // Held runs remain cached hits even when the cache is full.
    const auto hit = f.service->shape({"held 7", wui::FontRole::body, 14, 1, "und"});
    REQUIRE(std::holds_alternative<std::shared_ptr<const wui::TextRun>>(hit));
    CHECK(std::get<std::shared_ptr<const wui::TextRun>>(hit) == held[7]);
    held[0].reset();
    const auto freed = f.service->shape({"one more", wui::FontRole::body, 14, 1, "und"});
    CHECK(std::holds_alternative<std::shared_ptr<const wui::TextRun>>(freed));
}

TEST_CASE("run cache evicts the least recently used unheld run first") {
    Fixture f;
    const std::weak_ptr<const wui::TextRun> recent = f.shape("lru recent");
    const std::weak_ptr<const wui::TextRun> unused = f.shape("lru unused");
    for (std::size_t i = 0; i < 4094; ++i) {
        f.shape("filler " + std::to_string(i));
    }
    // A hit is a use: "lru recent" becomes the most recently used entry.
    CHECK(f.shape("lru recent") == recent.lock());
    f.shape("lru trigger");
    CHECK(unused.expired());
    REQUIRE_FALSE(recent.expired());
    CHECK(f.shape("lru recent") == recent.lock());
}

TEST_CASE("text and button labels honour start center and end alignment") {
    Fixture f;
    const auto run = f.shape("Wx");
    REQUIRE_FALSE(run->glyphs.empty());
    const float bearing = run->glyphs.front().bounds.x;
    auto created = wui::Context::create({wui::Theme::dark(), f.textures, f.service});
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::Context>>(created));
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    auto begun = context->begin_frame({{0, 0, 200, 300}});
    REQUIRE(std::holds_alternative<wui::Frame>(begun));
    auto frame = std::get<wui::Frame>(std::move(begun));
    const wui::Color start{1, 0, 0, 1}, center{0, 1, 0, 1}, end{0, 0, 1, 1}, label{1, 0, 1, 1};
    frame.text("Wx", {.color = start});
    frame.text("Wx", {.color = center, .align = wui::Align::center});
    frame.text("Wx", {.color = end, .align = wui::Align::end});
    frame.text("Wx", {.color = end, .wrap = true, .align = wui::Align::end});
    CHECK_FALSE(frame.button("left", "Wx", {.style = {.text = label}, .label_align = wui::Align::start}));
    auto finished = frame.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
    const auto packet = std::get<wui::DrawPacket>(std::move(finished));
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    // Leftmost vertex of the glyphs painted in one exact color, per text row.
    const auto left = [&](wui::Color color, float row_y) {
        float x = 1e9f;
        for (const auto& vertex : packet->vertices) {
            const auto c = vertex.premultiplied;
            if (c.r == color.r && c.g == color.g && c.b == color.b &&
                vertex.position.y >= row_y && vertex.position.y < row_y + 36)
                x = std::min(x, vertex.position.x);
        }
        return x;
    };
    const auto row = [&](std::size_t index) { return packet->semantics[index].bounds.y; };
    REQUIRE(packet->semantics.size() == 5);
    CHECK(left(start, row(0)) == doctest::Approx(bearing));
    CHECK(left(center, row(1)) == doctest::Approx((200 - run->width) * 0.5f + bearing));
    CHECK(left(end, row(2)) == doctest::Approx(200 - run->width + bearing));
    CHECK(left(end, row(3)) == doctest::Approx(200 - run->width + bearing));
    CHECK(left(label, row(4)) == doctest::Approx(wui::Theme::dark().gap + bearing));
}
