#include "../support/widget_fixture.hpp"

#include <doctest/doctest.h>

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
        // Popup paint follows all main paint: the last geometry is its background.
        const auto& last = packet->batches.back();
        CHECK(packet->vertices[packet->indices[last.first_index] + last.base_vertex].position.y >= 29);
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
        wui::test::finish(frame);
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
