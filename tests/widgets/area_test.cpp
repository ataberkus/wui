#include "../support/widget_fixture.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace {
using wui::EventKind;
using wui::PointerButton;

wui::InputEvent pointer(EventKind kind, float x, float y,
                        PointerButton button = PointerButton::left) {
    wui::InputEvent event{kind, {x, y}};
    event.button = button;
    return event;
}
std::vector<wui::InputEvent> click(float x, float y, PointerButton button = PointerButton::left) {
    return {pointer(EventKind::pointer_down, x, y, button), pointer(EventKind::pointer_up, x, y, button)};
}
wui::FrameInput input(std::vector<wui::InputEvent> events = {},
                      wui::Rect viewport = {0, 0, 400, 300}) {
    wui::FrameInput result{viewport};
    result.events = std::move(events);
    return result;
}
wui::CustomItem custom(wui::Frame& frame, std::string_view key, float height,
                       wui::WidgetOptions options = {}) {
    auto item = frame.custom(key, key, height, options);
    REQUIRE(std::holds_alternative<wui::CustomItem>(item));
    return std::get<wui::CustomItem>(item);
}
const wui::Vertex& vertex(const wui::DrawPacket& packet, const wui::Batch& batch) {
    return packet->vertices[static_cast<std::size_t>(
        std::int64_t{batch.base_vertex} + packet->indices[batch.first_index])];
}
void check_same(wui::Rect actual, wui::Rect expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.w == doctest::Approx(expected.w));
    CHECK(actual.h == doctest::Approx(expected.h));
}
void check_rgb(wui::Color actual, wui::Color expected) {
    CHECK(actual.r == doctest::Approx(expected.r));
    CHECK(actual.g == doctest::Approx(expected.g));
    CHECK(actual.b == doctest::Approx(expected.b));
    CHECK(actual.a == doctest::Approx(expected.a));
}
// Generic popups share the dropdown menu's raised look: a cached shadow (a
// texture other than the 1x1 white one) in the popup layer's clip, the sheet
// (control mixed 5% toward text) and, last, a 1px hairline (control mixed 30%
// toward secondary), both clipped to the popup bounds. The batches start at
// `first`; `content` batches of popup content lie between sheet and hairline.
void check_raised_sheet(const wui::DrawPacket& packet, std::size_t first, wui::Rect bounds,
                        wui::Rect layer_clip, std::size_t content = 0) {
    REQUIRE(first + 3 + content == packet->batches.size());
    const auto theme = wui::Theme::dark();
    const auto mix = [](wui::Color a, wui::Color b, float t) {
        return wui::Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
    };
    const auto& shadow = packet->batches[first];
    const auto& sheet = packet->batches[first + 1];
    const auto& hairline = packet->batches[first + 2 + content];
    CHECK(packet->textures[shadow.texture_index]->width > 1);
    check_same(shadow.clip, layer_clip);
    check_rgb(vertex(packet, shadow).premultiplied, {0, 0, 0, 0.8f});
    check_same(sheet.clip, bounds);
    check_rgb(vertex(packet, sheet).premultiplied, mix(theme.control, theme.text, 0.05f));
    check_same(hairline.clip, bounds);
    check_rgb(vertex(packet, hairline).premultiplied, mix(theme.control, theme.secondary, 0.3f));
}
wui::CustomItem custom_at(wui::Frame& frame, std::string_view key, wui::Rect bounds) {
    auto item = frame.custom_at(key, key, bounds);
    REQUIRE(std::holds_alternative<wui::CustomItem>(item));
    return std::get<wui::CustomItem>(item);
}
} // namespace

TEST_CASE("right release on the pressed target is a secondary activation that keeps focus") {
    auto context = wui::test::make_context();
    wui::Id first_id;
    {
        auto frame = wui::test::begin(*context, input(click(10, 10)));
        const auto first = custom(frame, "first", 30);
        CHECK(first.behavior.activated);
        first_id = first.id;
        custom(frame, "second", 30);
        wui::test::finish(frame);
    }
    CHECK(context->statistics().focused == first_id);
    {
        // Press on second, release on second: secondary only. A right press
        // released elsewhere is not an activation.
        auto events = click(10, 50, PointerButton::right);
        events.push_back(pointer(EventKind::pointer_down, 10, 50, PointerButton::right));
        events.push_back(pointer(EventKind::pointer_up, 10, 10, PointerButton::right));
        auto frame = wui::test::begin(*context, input(std::move(events)));
        const auto first = custom(frame, "first", 30);
        const auto second = custom(frame, "second", 30);
        CHECK_FALSE(first.behavior.secondary_activated);
        CHECK(second.behavior.secondary_activated);
        CHECK_FALSE(second.behavior.activated);
        wui::test::finish(frame);
    }
    CHECK(context->statistics().focused == first_id);
    {
        auto events = click(10, 50, PointerButton::middle);
        auto x1 = click(10, 50, PointerButton::x1);
        events.insert(events.end(), x1.begin(), x1.end());
        auto frame = wui::test::begin(*context, input(std::move(events)));
        custom(frame, "first", 30);
        const auto second = custom(frame, "second", 30);
        CHECK_FALSE(second.behavior.activated);
        CHECK_FALSE(second.behavior.secondary_activated);
        // Other buttons stay visible as raw input for bindings.
        REQUIRE(frame.events().size() == 4);
        CHECK(frame.events()[2].button == PointerButton::x1);
        wui::test::finish(frame);
    }
    CHECK(context->statistics().focused == first_id);
}

TEST_CASE("raw events, pointer and wheel steps are available to custom items") {
    auto context = wui::test::make_context();
    wui::InputEvent wheel{EventKind::wheel, {20, 20}};
    wheel.wheel = -2;
    wheel.wheel_x = 1;
    auto frame = wui::test::begin(*context, input({pointer(EventKind::pointer_move, 5, 5), wheel}));
    REQUIRE(frame.events().size() == 2);
    REQUIRE(frame.pointer().has_value());
    CHECK(frame.pointer()->x == 20);
    {
        auto list = frame.scroll("list", 100);
        const auto preview = custom(frame, "preview", 60);
        const auto steps = frame.take_wheel(preview.clip);
        CHECK(steps.x == doctest::Approx(1));
        CHECK(steps.y == doctest::Approx(-2));
        CHECK(frame.take_wheel(preview.clip).y == 0);
        custom(frame, "filler", 400);
    }
    wui::test::finish(frame);
    // The consumed wheel did not scroll the enclosing list.
    auto next = wui::test::begin(*context, input());
    {
        auto list = next.scroll("list", 100);
        CHECK(list.offset() == 0);
        custom(next, "preview", 60);
        custom(next, "filler", 400);
    }
    wui::test::finish(next);
}

TEST_CASE("explicit placement and regions leave the parent flow untouched") {
    auto context = wui::test::make_context();
    auto frame = wui::test::begin(*context, input(click(260, 120), {100, 50, 400, 300}));
    const auto start = frame.next_rect(10);
    CHECK(start.x == 100);
    CHECK(start.y == 50);
    const auto placed = custom_at(frame, "placed", {300, 200, 40, 40});
    CHECK(placed.bounds.x == 300);
    CHECK(frame.next_rect(10).y == start.y);
    {
        auto region = frame.region("side", {250, 110, 100, 60});
        REQUIRE(region);
        const auto inside = custom(frame, "inside", 30);
        CHECK(inside.bounds.x == 250);
        CHECK(inside.bounds.y == 110);
        CHECK(inside.bounds.w == 100);
        CHECK(inside.behavior.activated);
        // Clipped to the region: the second item is cut at its bottom.
        const auto clipped = custom(frame, "clipped", 30);
        CHECK(clipped.clip.y + clipped.clip.h == doctest::Approx(170));
    }
    CHECK(frame.next_rect(10).y == start.y);
    const auto flowing = custom(frame, "flowing", 20);
    CHECK(flowing.bounds.y == start.y);
    wui::test::finish(frame);
}

TEST_CASE("popups gate underlying widgets, route their own content and dismiss outside") {
    auto context = wui::test::make_context();
    const wui::PopupOptions options{{0, 0, 100, 30}};
    auto emit = [&](wui::Frame& frame, bool& under, bool& item) {
        under = custom(frame, "under", 120).behavior.activated;
        if (auto popup = frame.popup("menu", options)) {
            CHECK(popup.bounds().y == 30);
            CHECK(popup.bounds().w == doctest::Approx(216));
            item = custom(frame, "item", 20).behavior.activated;
        }
    };
    bool under = false, item = false;
    {
        auto frame = wui::test::begin(*context, input());
        frame.open_popup("menu");
        CHECK(frame.popup_open("menu"));
        emit(frame, under, item);
        const auto packet = wui::test::finish(frame);
        // The popup background is measured from its content: 20 + 2 * gap.
        const auto& panel = wui::test::semantic(packet, wui::Role::panel, "");
        CHECK(panel.bounds.h == doctest::Approx(36));
        // Popup paint follows all main paint: the last geometry is its hairline.
        const auto& last = packet->batches.back();
        CHECK(packet->vertices[packet->indices[last.first_index] + last.base_vertex].position.y >= 29);
        check_raised_sheet(packet, packet->batches.size() - 3, {0, 30, 216, 36}, {0, 0, 400, 300});
    }
    {
        // Item inside the popup at (8, 38); the same point is over `under`.
        auto frame = wui::test::begin(*context, input(click(20, 45)));
        emit(frame, under, item);
        CHECK_FALSE(under);
        CHECK(item);
        wui::test::finish(frame);
    }
    {
        auto frame = wui::test::begin(*context, input(click(20, 100)));
        under = item = false;
        emit(frame, under, item);
        CHECK_FALSE(under); // the dismissing press never reaches the widget below
        CHECK_FALSE(frame.popup_open("menu"));
        wui::test::finish(frame);
    }
    {
        auto frame = wui::test::begin(*context, input(click(20, 100)));
        emit(frame, under, item);
        CHECK(under);
        frame.open_popup("menu");
        wui::test::finish(frame);
    }
    {
        auto frame = wui::test::begin(*context, input({wui::test::key_event(EventKind::key_down, wui::Key::escape)}));
        emit(frame, under, item);
        CHECK_FALSE(frame.popup_open("menu"));
        frame.open_popup("menu");
        wui::test::finish(frame);
    }
    {
        // A popup that is no longer emitted closes when the frame finishes.
        auto frame = wui::test::begin(*context, input());
        custom(frame, "under", 120);
        CHECK(frame.popup_open("menu"));
        wui::test::finish(frame);
    }
    auto frame = wui::test::begin(*context, input());
    CHECK_FALSE(frame.popup_open("menu"));
    wui::test::finish(frame);
}

TEST_CASE("modal popups center, swallow outside input and close only on request") {
    auto context = wui::test::make_context();
    const wui::PopupOptions options{{}, 200, true};
    bool under = false;
    auto emit = [&](wui::Frame& frame) {
        under = custom(frame, "under", 300).behavior.activated;
        if (auto popup = frame.popup("confirm", options)) {
            CHECK(popup.bounds().x == doctest::Approx(100));
            custom(frame, "ok", 20);
        }
    };
    {
        auto frame = wui::test::begin(*context, input());
        frame.open_popup("confirm");
        emit(frame);
        const auto packet = wui::test::finish(frame);
        // The scrim stays below the raised sheet (its shadow included). The
        // opening frame centers a popup of the not yet measured height 0.
        REQUIRE(packet->batches.size() >= 4);
        const auto first = packet->batches.size() - 3;
        const auto& scrim = packet->batches[first - 1];
        CHECK(vertex(packet, scrim).premultiplied.a == doctest::Approx(0.45f));
        check_raised_sheet(packet, first, {100, 150, 200, 36}, {0, 0, 400, 300});
    }
    auto events = click(5, 5);
    events.push_back(wui::test::key_event(EventKind::key_down, wui::Key::escape));
    {
        auto frame = wui::test::begin(*context, input(std::move(events)));
        emit(frame);
        CHECK_FALSE(under);
        CHECK(frame.popup_open("confirm"));
        frame.close_popup();
        CHECK_FALSE(frame.popup_open("confirm"));
        wui::test::finish(frame);
    }
    auto frame = wui::test::begin(*context, input(click(5, 5)));
    emit(frame);
    CHECK(under);
    wui::test::finish(frame);
}

TEST_CASE("raised popup sheets enclose nested panels, also when taller than the viewport") {
    auto context = wui::test::make_context();
    for (int i = 0; i < 2; ++i) {
        auto frame = wui::test::begin(*context, input());
        if (i == 0) frame.open_popup("tall");
        if (auto popup = frame.popup("tall", {{0, 0, 100, 30}})) {
            {
                auto inner = frame.panel("inner");
                custom(frame, "row", 20);
            }
            custom(frame, "rest", 400);
        }
        const auto packet = wui::test::finish(frame);
        if (i == 0) continue;
        // Measured 8 + (20 + 2 * 16) + 8 + 400 + 8 = 476: the second frame
        // clamps the popup to the viewport top and it overflows the bottom.
        const wui::Semantic* sheet = nullptr;
        for (const auto& entry : packet->semantics)
            if (entry.role == wui::Role::panel && entry.bounds.w == doctest::Approx(216)) sheet = &entry;
        REQUIRE(sheet != nullptr);
        check_same(sheet->bounds, {0, 0, 216, 476});
        // Shadow, sheet and hairline follow the full height; sheet and
        // hairline are clipped to the visible part. The nested panel's flat
        // background lies between them, clipped to the popup interior.
        check_raised_sheet(packet, packet->batches.size() - 4, {0, 0, 216, 300}, {0, 0, 400, 300}, 1);
        const auto& inner = packet->batches[packet->batches.size() - 2];
        check_same(inner.clip, {8, 8, 200, 292});
        check_rgb(vertex(packet, inner).premultiplied, wui::Theme::dark().panel);
    }
}

namespace {
// The popup shadow is decorative: when a capacity limit keeps it from being
// produced, the popup paints flat (its sheet, then content, then the hairline
// last) and its frames succeed, so it stays open.
void check_flat_sheet(const wui::DrawPacket& packet, wui::Rect sheet_clip) {
    REQUIRE(packet->batches.size() >= 2);
    const auto theme = wui::Theme::dark();
    const auto mix = [](wui::Color a, wui::Color b, float t) {
        return wui::Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
    };
    const auto& sheet = packet->batches[packet->batches.size() - 2];
    const auto& hairline = packet->batches.back();
    check_same(sheet.clip, sheet_clip);
    check_rgb(vertex(packet, sheet).premultiplied, mix(theme.control, theme.text, 0.05f));
    check_same(hairline.clip, sheet_clip);
    check_rgb(vertex(packet, hairline).premultiplied, mix(theme.control, theme.secondary, 0.3f));
}
} // namespace

TEST_CASE("generic popups paint flat and stay open when their shadow cannot be produced") {
    auto desc = wui::test::fixture_desc();
    auto frame_input = input();
    wui::PopupOptions options{{0, 0, 100, 30}};
    float content = 20;
    wui::Rect sheet{0, 30, 216, 36};
    SUBCASE("no room for the shadow texture in the store") {
        desc.textures = std::make_shared<wui::TextureStore>(4096);
    }
    SUBCASE("a physical shadow radius beyond the cache limits") {
        // The radius clamps to half the 2100 x 2116 sheet: 1050 logical and
        // 2100 physical pixels. Themes bound no radius.
        desc.theme.control_radius = 4000;
        frame_input = input({}, {0, 0, 2400, 2400});
        frame_input.scale = 2;
        options.width = 2100;
        content = 2100;
        sheet = {0, 30, 2100, 2116};
    }
    auto context = wui::test::make_context(std::move(desc));
    for (int i = 0; i < 2; ++i) {
        auto frame = wui::test::begin(*context, frame_input);
        if (i == 0) frame.open_popup("menu");
        bool emitted = false;
        if (auto popup = frame.popup("menu", options)) {
            custom(frame, "item", content);
            emitted = true;
        }
        CHECK(emitted);
        CHECK(frame.popup_open("menu"));
        const auto packet = wui::test::finish(frame);
        for (const auto& texture : packet->textures) CHECK(texture->width == 1); // no shadow
        check_same(wui::test::semantic(packet, wui::Role::panel, "").bounds, sheet);
        check_flat_sheet(packet, sheet);
    }
}

TEST_CASE("a generic popup opened after caller shadows fill the shadow cache paints flat") {
    // An overlay's shadow waits with its closed layer until the frame ends,
    // and 31 newer keys fill the 32-entry cache, every entry in use by this
    // frame. The popup's shadow finds nothing to evict: it is skipped, and
    // the overlay's shadow keeps its texture.
    auto context = wui::test::make_context();
    for (int i = 0; i < 2; ++i) {
        auto frame = wui::test::begin(*context, input());
        if (auto overlay = frame.overlay("card", {200, 200, 150, 80}))
            frame.canvas().shadow({210, 210, 120, 50}, 6, 4, {0, 0, 0, 0.6f});
        for (int k = 0; k < 31; ++k)
            frame.canvas().shadow({10, 150, 100, 100}, 1.0f + static_cast<float>(k), 2, {0, 0, 0, 0.5f});
        if (i == 0) frame.open_popup("menu");
        if (auto popup = frame.popup("menu", {{0, 0, 100, 30}})) custom(frame, "item", 20);
        CHECK(frame.popup_open("menu"));
        const auto packet = wui::test::finish(frame);
        CHECK(packet->textures.size() == 33); // 32 shadows and the white texture
        check_flat_sheet(packet, {0, 30, 216, 36});
        // The overlay layer paints above the main content, below the popup.
        const auto& card = packet->batches[packet->batches.size() - 3];
        CHECK(packet->textures[card.texture_index]->width > 1);
        check_rgb(vertex(packet, card).premultiplied, {0, 0, 0, 0.6f});
    }
}

TEST_CASE("tooltip_due follows the shared hover delay") {
    auto context = wui::test::make_context();
    std::vector<bool> due;
    for (int i = 0; i < 3; ++i) {
        auto frame_input = input({pointer(EventKind::pointer_move, 10, 10)});
        frame_input.dt_seconds = 0.2;
        auto frame = wui::test::begin(*context, frame_input);
        custom(frame, "owner", 30);
        due.push_back(frame.tooltip_due("owner"));
        if (due.back()) {
            auto tip = frame.overlay("tip", {10, 40, 120, 30});
            CHECK(tip);
        }
        wui::test::finish(frame);
    }
    CHECK(due == std::vector<bool>{false, false, true});
}

TEST_CASE("disabled style scopes disable widgets and custom items inside them") {
    auto context = wui::test::make_context();
    { auto frame = wui::test::begin(*context, input()); frame.button("go", "Go"); wui::test::finish(frame); }
    auto frame = wui::test::begin(*context, input(click(10, 10)));
    {
        auto off = frame.style({.disabled = true});
        CHECK_FALSE(frame.button("go", "Go"));
        CHECK_FALSE(custom(frame, "item", 20).enabled);
    }
    CHECK(custom(frame, "after", 20).enabled);
    const auto packet = wui::test::finish(frame);
    CHECK_FALSE(wui::test::semantic(packet, wui::Role::button, "Go").enabled);
}

TEST_CASE("a transparent canvas token leaves the target untouched") {
    auto desc = wui::test::fixture_desc();
    desc.theme.canvas.a = 0;
    auto context = wui::test::make_context(std::move(desc));
    auto frame = wui::test::begin(*context, input());
    const auto packet = wui::test::finish(frame);
    CHECK(packet->batches.empty());
}

TEST_CASE("color picker maps saturation/value, hue and alpha drags") {
    auto context = wui::test::make_context();
    wui::Color color{1, 0, 0, 1};
    const wui::ColorPickerOptions options{true, 100};
    { auto frame = wui::test::begin(*context, input()); frame.color_picker("c", "Color", color, options); wui::test::finish(frame); }
    {
        auto frame = wui::test::begin(*context, input(click(50, 25)));
        CHECK(frame.color_picker("c", "Color", color, options));
        wui::test::finish(frame);
    }
    CHECK(color.r == doctest::Approx(0.75f));
    CHECK(color.g == doctest::Approx(0.375f));
    CHECK(color.b == doctest::Approx(0.375f));
    {
        // Hue bar starts at 100 + gap; alpha bar follows it.
        auto events = click(110, 50);
        auto alpha = click(140, 75);
        events.insert(events.end(), alpha.begin(), alpha.end());
        auto frame = wui::test::begin(*context, input(std::move(events)));
        CHECK(frame.color_picker("c", "Color", color, options));
        const auto packet = wui::test::finish(frame);
        CHECK(wui::test::semantic(packet, wui::Role::slider, "Color").value == "#60BFBF40");
    }
    CHECK(color.r == doctest::Approx(0.375f));
    CHECK(color.g == doctest::Approx(0.75f));
    CHECK(color.b == doctest::Approx(0.75f));
    CHECK(color.a == doctest::Approx(0.25f));
}

TEST_CASE("input events with unknown pointer buttons are rejected") {
    auto context = wui::test::make_context();
    auto bad = input({pointer(EventKind::pointer_down, 1, 1, static_cast<PointerButton>(9))});
    auto result = context->begin_frame(bad);
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
}
