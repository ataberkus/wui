#include <doctest/doctest.h>

#include <wui/input.hpp>

#include <cstdint>
#include <random>
#include <unordered_set>

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

TEST_CASE("id set claims each id once per generation and clears in O(1)") {
    wui::detail::IdSet set;
    CHECK_FALSE(set.contains(7));
    CHECK(set.capacity() == 0);
    CHECK(set.insert(7));
    CHECK_FALSE(set.insert(7));
    CHECK(set.contains(7));
    CHECK(set.size() == 1);
    set.clear();
    CHECK(set.size() == 0);
    CHECK_FALSE(set.contains(7));
    CHECK(set.insert(7));

    // Sequential, structured and hashed ids, well past several growth steps.
    set.clear();
    constexpr std::uint64_t count = 5000;
    for (std::uint64_t i = 1; i <= count; ++i) {
        REQUIRE(set.insert(i));
        REQUIRE(set.insert(i << 32));
        REQUIRE(set.insert(wui::child_id({i}, "k").value));
    }
    CHECK(set.size() == 3 * count);
    for (std::uint64_t i = 1; i <= count; ++i) {
        REQUIRE(set.contains(i));
        REQUIRE(set.contains(i << 32));
        REQUIRE_FALSE(set.insert(wui::child_id({i}, "k").value));
    }
    CHECK_FALSE(set.contains(count + 1));

    // Storage is retained: a generation of the same size never grows it.
    const auto retained = set.capacity();
    CHECK(retained >= 3 * count);
    set.clear();
    for (std::uint64_t i = 1; i <= 3 * count; ++i) REQUIRE(set.insert(i * 0x10001));
    CHECK(set.capacity() == retained);
    CHECK_FALSE(set.contains(1));
}

TEST_CASE("id set at capacity rejects duplicates without growing and keeps every id when it grows") {
    wui::detail::IdSet set;
    REQUIRE(set.insert(1));
    const auto initial = set.capacity();
    REQUIRE(initial > 1);
    for (std::uint64_t i = 2; i <= initial; ++i) REQUIRE(set.insert(i));
    REQUIRE(set.size() == initial);
    // Full: a duplicate is still recognized and does not reallocate.
    CHECK_FALSE(set.insert(initial));
    CHECK_FALSE(set.insert(1));
    CHECK(set.capacity() == initial);
    // The next new id grows the storage; nothing is dropped by the rehash.
    CHECK(set.insert(initial + 1));
    CHECK(set.capacity() > initial);
    for (std::uint64_t i = 1; i <= initial + 1; ++i) CHECK(set.contains(i));
    CHECK(set.size() == initial + 1);
}

TEST_CASE("duplicate claims are ignored in large frames and ids are reclaimable next frame") {
    wui::Interaction input;
    constexpr std::uint64_t count = 3000;
    const auto bounds = [](std::uint64_t i) {
        return wui::Rect{static_cast<float>(i % 50) * 10.0f, static_cast<float>(i / 50) * 10.0f, 10, 10};
    };
    for (int frame_index = 0; frame_index < 2; ++frame_index) {
        wui::FrameInput frame{{0, 0, 500, 600}};
        frame.events = {{wui::EventKind::pointer_move, {2005 % 50 * 10.0f + 5, 2005 / 50 * 10.0f + 5}}};
        input.begin(frame, {});
        for (std::uint64_t i = 1; i <= count; ++i) {
            const auto behavior = input.button({i}, bounds(i), i != 17);
            if (i == 2005) CHECK(behavior.hovered);
        }
        // Repeated ids, enabled or not, early or late, claim nothing.
        CHECK_FALSE(input.button({2005}, bounds(2005), true).hovered);
        CHECK_FALSE(input.drag({1}, bounds(1), true).behavior.hovered);
        bool edited = false;
        input.edit({count}, true, [&](const wui::InputEvent&) { edited = true; });
        CHECK_FALSE(edited);
        // A fresh id is still claimed after the duplicates.
        CHECK(input.button({count + 1}, bounds(2005), true).hovered);
        input.end();
    }
}

TEST_CASE("id set matches a reference set across generations of clustered ids") {
    // Ids that share low bits, differ only in high bits, or repeat often stress
    // the probe sequences; every generation must start empty.
    wui::detail::IdSet set;
    std::unordered_set<std::uint64_t> reference;
    std::mt19937_64 random(20261102);
    for (int generation = 0; generation < 200; ++generation) {
        set.clear();
        reference.clear();
        const auto count = generation % 25 == 0 ? 5000 : static_cast<int>(random() % 400);
        for (int i = 0; i < count; ++i) {
            std::uint64_t id = 0;
            switch (random() % 4) {
            case 0: id = 1 + random() % 64; break;
            case 1: id = (1 + random() % 512) << 55; break;
            case 2: id = (1 + random() % 4096) * 0x9E3779B97F4A7C15ULL; break;
            default: id = random() | 1; break;
            }
            REQUIRE(set.insert(id) == reference.insert(id).second);
        }
        REQUIRE(set.size() == reference.size());
        REQUIRE(set.capacity() >= set.size());
        for (const auto id : reference) REQUIRE(set.contains(id));
        for (std::uint64_t probe = 1; probe <= 128; ++probe)
            REQUIRE(set.contains(probe) == reference.contains(probe));
    }
}
