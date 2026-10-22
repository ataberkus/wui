#include <doctest/doctest.h>

#include <wui/input.hpp>

namespace {
constexpr wui::Id first{1};
constexpr wui::Id second{2};
constexpr wui::Rect first_bounds{10, 10, 60, 36};
constexpr wui::Rect second_bounds{90, 10, 60, 36};

wui::InputEvent key(wui::EventKind kind, wui::Key value,
                    bool shift = false, bool repeat = false) {
    wui::InputEvent event{kind};
    event.key = value;
    event.shift = shift;
    event.repeat = repeat;
    return event;
}

void seed_focus(wui::Interaction& input) {
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, {});
    input.button(first, first_bounds, true);
    input.button(second, second_bounds, true);
    input.end();
    REQUIRE(input.focused() == first);
}
} // namespace

TEST_CASE("a press released outside does not activate") {
    wui::Interaction input;
    const wui::Id id{1};
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}}};
    input.begin(frame, {});
    CHECK_FALSE(input.button(id, {10, 10, 60, 36}, true).activated);
    input.end();
    frame.events = {{wui::EventKind::pointer_up, {100, 100}}};
    input.begin(frame, {});
    CHECK_FALSE(input.button(id, {10, 10, 60, 36}, true).activated);
    input.end();
    CHECK(input.captured() == wui::Id{});
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, {});
    CHECK(input.button(id, {10, 10, 60, 36}, true).activated);
    input.end();
}

TEST_CASE("release on another control does not activate either") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {100, 20}}};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    CHECK_FALSE(input.button(second, second_bounds, true).activated);
    input.end();
    CHECK(input.captured() == wui::Id{});
}

TEST_CASE("disabled and missing capture owners are cancelled") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}}};
    input.begin(frame, {});
    CHECK(input.button(first, first_bounds, true).held);
    input.end();
    REQUIRE(input.captured() == first);
    frame.events.clear();
    input.begin(frame, {});
    SUBCASE("disabled owner clears immediately at emission") {
        auto behavior = input.button(first, first_bounds, false);
        CHECK_FALSE(behavior.activated);
        CHECK_FALSE(behavior.held);
        CHECK_FALSE(behavior.focused);
        CHECK(input.captured() == wui::Id{});
    }
    SUBCASE("missing owner clears at end") {}
    input.end();
    CHECK(input.captured() == wui::Id{});
    CHECK(input.focused() == wui::Id{});
    frame.events = {{wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    input.end();
}

TEST_CASE("focus loss cancels capture and a pending keyboard press") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    SUBCASE("pointer capture") {
        frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                        {wui::EventKind::focus_lost},
                        {wui::EventKind::pointer_up, {20, 20}}};
    }
    SUBCASE("keyboard press") {
        frame.events = {key(wui::EventKind::key_down, wui::Key::enter),
                        {wui::EventKind::focus_lost},
                        key(wui::EventKind::key_up, wui::Key::enter)};
    }
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    input.button(second, second_bounds, true);
    input.end();
    CHECK(input.focused() == wui::Id{});
    CHECK(input.captured() == wui::Id{});
}

TEST_CASE("Tab traverses the previous eligible order and wraps") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    for (const auto expected : {second, first}) {
        frame.events = {key(wui::EventKind::key_down, wui::Key::tab)};
        input.begin(frame, {});
        input.button(first, first_bounds, true);
        input.button(second, second_bounds, true);
        input.end();
        CHECK(input.focused() == expected);
    }
    frame.events = {key(wui::EventKind::key_down, wui::Key::tab, true)};
    input.begin(frame, {});
    input.button(first, first_bounds, true);
    CHECK(input.button(second, second_bounds, true).focused);
    input.end();
    CHECK(input.focused() == second);
}

TEST_CASE("new controls join keyboard traversal after their first frame") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {key(wui::EventKind::key_down, wui::Key::tab)};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).focused);
    input.end();
    CHECK(input.focused() == wui::Id{});
    input.begin(frame, {});
    CHECK(input.button(first, first_bounds, true).focused);
    input.end();
}

TEST_CASE("a missing or disabled traversal target cannot activate") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {key(wui::EventKind::key_down, wui::Key::tab),
                    key(wui::EventKind::key_down, wui::Key::space),
                    key(wui::EventKind::key_up, wui::Key::space)};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    SUBCASE("disabled target") {
        CHECK_FALSE(input.button(second, second_bounds, false).activated);
    }
    SUBCASE("missing target") {}
    input.end();
    CHECK(input.focused() == wui::Id{});
}

TEST_CASE("keyboard activation needs a matching nonrepeated down and up") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    for (const auto activation_key : {wui::Key::enter, wui::Key::space}) {
        frame.events = {key(wui::EventKind::key_down, activation_key)};
        input.begin(frame, {});
        CHECK_FALSE(input.button(first, first_bounds, true).activated);
        input.end();
        frame.events = {key(wui::EventKind::key_down, activation_key, false, true)};
        input.begin(frame, {});
        CHECK_FALSE(input.button(first, first_bounds, true).activated);
        input.end();
        frame.events = {key(wui::EventKind::key_up, activation_key)};
        input.begin(frame, {});
        CHECK(input.button(first, first_bounds, true).activated);
        input.end();
        input.begin(frame, {});
        CHECK_FALSE(input.button(first, first_bounds, true).activated);
        input.end();
    }
}

TEST_CASE("an isolated repeated keydown cannot arm activation") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {key(wui::EventKind::key_down, wui::Key::enter, false, true),
                    key(wui::EventKind::key_up, wui::Key::enter)};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    input.end();
}

TEST_CASE("pointer focus follows event time rather than widget emission order") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {100, 20}},
                    {wui::EventKind::pointer_up, {100, 20}},
                    {wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, {});
    CHECK(input.button(first, first_bounds, true).activated);
    CHECK(input.button(second, second_bounds, true).activated);
    input.end();
    CHECK(input.focused() == first);
    CHECK(input.captured() == wui::Id{});
}

TEST_CASE("pointer input uses current bounds") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {100, 20}},
                    {wui::EventKind::pointer_up, {100, 20}}};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    CHECK_FALSE(input.button(second, {90, 100, 60, 36}, true).activated);
    input.end();
    CHECK(input.focused() != second);
}

TEST_CASE("blank pointer presses clear focus") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {180, 180}}};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).focused);
    input.end();
    CHECK(input.focused() == wui::Id{});
}

TEST_CASE("keyboard after pointer down is deferred ahead of new input") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {100, 20}},
                    {wui::EventKind::pointer_up, {100, 20}},
                    key(wui::EventKind::key_down, wui::Key::enter),
                    key(wui::EventKind::key_up, wui::Key::enter)};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    CHECK(input.button(second, second_bounds, true).activated);
    input.end();
    frame.events = {key(wui::EventKind::key_down, wui::Key::tab)};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    CHECK(input.button(second, second_bounds, true).activated);
    input.end();
    CHECK(input.focused() == first);
}

TEST_CASE("deferred pointer and keyboard suffixes retain their order") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {100, 20}},
                    {wui::EventKind::pointer_up, {100, 20}},
                    key(wui::EventKind::key_down, wui::Key::tab),
                    {wui::EventKind::pointer_down, {100, 20}},
                    {wui::EventKind::pointer_up, {100, 20}},
                    key(wui::EventKind::key_down, wui::Key::space),
                    key(wui::EventKind::key_up, wui::Key::space)};
    for (int iteration = 0; iteration < 3; ++iteration) {
        input.begin(frame, {});
        CHECK_FALSE(input.button(first, first_bounds, true).activated);
        CHECK(input.button(second, second_bounds, true).activated);
        input.end();
        CHECK(input.focused() == second);
        frame.events.clear();
    }
    input.begin(frame, {});
    CHECK_FALSE(input.button(second, second_bounds, true).activated);
    input.end();
}

TEST_CASE("focus loss inside a deferred suffix cancels immediately") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {100, 20}},
                    key(wui::EventKind::key_down, wui::Key::space),
                    {wui::EventKind::focus_lost},
                    {wui::EventKind::pointer_up, {100, 20}},
                    key(wui::EventKind::key_up, wui::Key::space)};
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    CHECK_FALSE(input.button(second, second_bounds, true).held);
    input.end();
    CHECK(input.focused() == wui::Id{});
    CHECK(input.captured() == wui::Id{});
    frame.events.clear();
    input.begin(frame, {});
    CHECK_FALSE(input.button(second, second_bounds, true).activated);
    input.end();
}

TEST_CASE("final behavior follows the last pointer gesture in either emission order") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}},
                    {wui::EventKind::pointer_down, {100, 20}}};
    input.begin(frame, {});
    wui::Behavior a, b;
    SUBCASE("first then second") {
        a = input.button(first, first_bounds, true);
        b = input.button(second, second_bounds, true);
    }
    SUBCASE("second then first") {
        b = input.button(second, second_bounds, true);
        a = input.button(first, first_bounds, true);
    }
    CHECK(a.activated);
    CHECK_FALSE(a.focused);
    CHECK_FALSE(a.held);
    CHECK_FALSE(b.activated);
    CHECK(b.focused);
    CHECK(b.held);
    CHECK(b.hovered);
    input.end();
    CHECK(input.focused() == second);
    CHECK(input.captured() == second);
}

TEST_CASE("overlapping buttons consume each gesture only once") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, {});
    CHECK(input.button(first, first_bounds, true).activated);
    CHECK_FALSE(input.button(second, first_bounds, true).activated);
    input.end();
    CHECK(input.focused() == first);
}

TEST_CASE("deferred pointer releases use the next frames current bounds") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    key(wui::EventKind::key_down, wui::Key::enter),
                    {wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, {});
    CHECK(input.button(first, first_bounds, true).held);
    input.end();
    frame.events.clear();
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, {10, 100, 60, 36}, true).activated);
    input.end();
    CHECK(input.captured() == wui::Id{});
}

TEST_CASE("focus loss preserves completed pointer activation but drops deferred gestures") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}},
                    key(wui::EventKind::key_down, wui::Key::enter),
                    {wui::EventKind::pointer_down, {100, 20}},
                    {wui::EventKind::pointer_up, {100, 20}},
                    {wui::EventKind::focus_lost}};
    input.begin(frame, {});
    const auto behavior = input.button(first, first_bounds, true);
    CHECK(behavior.activated);
    CHECK_FALSE(behavior.focused);
    CHECK_FALSE(behavior.held);
    CHECK_FALSE(input.button(second, second_bounds, true).activated);
    input.end();
    frame.events.clear();
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    CHECK_FALSE(input.button(second, second_bounds, true).activated);
    input.end();
}

TEST_CASE("modal snapshot gates earlier buttons even when the popup later disappears") {
    wui::Interaction input;
    seed_focus(input);
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {key(wui::EventKind::key_down, wui::Key::enter),
                    key(wui::EventKind::key_up, wui::Key::enter),
                    {wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, wui::Id{99});
    const auto blocked = input.button(first, first_bounds, true);
    CHECK_FALSE(blocked.activated);
    CHECK_FALSE(blocked.hovered);
    CHECK_FALSE(blocked.held);
    input.end();

    frame.events.clear();
    input.begin(frame, {});
    CHECK_FALSE(input.button(first, first_bounds, true).activated);
    input.end();
    frame.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    input.begin(frame, {});
    CHECK(input.button(first, first_bounds, true).activated);
    input.end();
}

TEST_CASE("drag retains capture beyond bounds and consumes release before a sibling button") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    frame.events = {{wui::EventKind::pointer_down, {25, 25}}};
    input.begin(frame, {});
    const auto down = input.drag(first, first_bounds, true);
    CHECK(down.moved);
    CHECK(down.position_x == 25);
    CHECK(down.behavior.held);
    input.button(second, second_bounds, true);
    input.end();
    CHECK(input.captured() == first);

    frame.events = {{wui::EventKind::pointer_move, {180, 25}}};
    input.begin(frame, {});
    const auto outside = input.drag(first, first_bounds, true);
    CHECK(outside.moved);
    CHECK(outside.position_x == 180);
    CHECK(outside.behavior.held);
    input.end();
    frame.events = {{wui::EventKind::pointer_up, {100, 25}}};
    input.begin(frame, {});
    const auto release = input.drag(first, first_bounds, true);
    CHECK(release.moved);
    CHECK(release.position_x == 100);
    CHECK_FALSE(input.button(second, second_bounds, true).activated);
    input.end();
    CHECK(input.captured() == wui::Id{});
}

TEST_CASE("drag respects focus-loss barrier and event-time keyboard ownership") {
    wui::Interaction input;
    wui::FrameInput frame{{0, 0, 200, 200}};
    input.begin(frame, {});
    input.drag(first, first_bounds, true);
    input.button(second, second_bounds, true);
    input.end();
    frame.events = {key(wui::EventKind::key_down, wui::Key::tab),
                    key(wui::EventKind::key_down, wui::Key::right),
                    {wui::EventKind::pointer_down, {100, 25}}};
    input.begin(frame, {});
    const auto from_previous_focus = input.drag(first, first_bounds, true);
    CHECK_FALSE(from_previous_focus.moved);
    CHECK_FALSE(from_previous_focus.behavior.focused);
    REQUIRE(from_previous_focus.actions.size() == 1);
    CHECK(from_previous_focus.actions.front().key == wui::Key::right);
    CHECK(input.button(second, second_bounds, true).held);
    input.end();
    frame.events = {{wui::EventKind::pointer_down, {20, 25}},
                    {wui::EventKind::pointer_move, {130, 25}},
                    {wui::EventKind::focus_lost},
                    {wui::EventKind::pointer_up, {180, 25}}};
    input.begin(frame, {});
    const auto cancelled = input.drag(first, first_bounds, true);
    CHECK(cancelled.moved);
    CHECK(cancelled.position_x == 130);
    CHECK_FALSE(cancelled.behavior.held);
    input.end();
    CHECK(input.captured() == wui::Id{});
    CHECK(input.focused() == wui::Id{});
}
