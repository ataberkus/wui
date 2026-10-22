#include "../support/widget_fixture.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

struct CollidingChoiceValue {
    int id;
    bool operator==(const CollidingChoiceValue&) const = default;
};
template<> struct std::hash<CollidingChoiceValue> {
    std::size_t operator()(const CollidingChoiceValue&) const noexcept { return 7; }
};

TEST_CASE("vertical tabs and radio choices expose owned selected values and a selected mark") {
    auto context = wui::test::make_context();
    std::string label = "Second";
    const std::array<wui::Choice<int>, 2> options{{{10, "First"}, {20, label}}};
    int tab = 20, radio = 20;
    auto frame = wui::test::begin(*context);
    CHECK_FALSE(frame.tabs("pages", tab, options, wui::Orientation::vertical));
    CHECK_FALSE(frame.radio("mode", "Mode", radio, options));
    auto packet = wui::test::finish(frame);
    const auto& first = wui::test::semantic(packet, wui::Role::tab, "First");
    const auto& second = wui::test::semantic(packet, wui::Role::tab, "Second");
    CHECK(second.bounds.x == first.bounds.x);
    CHECK(second.bounds.y > first.bounds.y);
    CHECK(second.selected);
    CHECK(second.value == "Second");
    const auto& selected = wui::test::semantic(packet, wui::Role::radio, "Second");
    CHECK(selected.selected);
    CHECK(selected.value == "Second");
    CHECK_FALSE(wui::test::semantic(packet, wui::Role::radio, "First").selected);
    CHECK(wui::test::semantic(packet, wui::Role::text, "Mode").bounds.y < selected.bounds.y);
    const auto accent = wui::Theme::dark().accent;
    bool marked = false;
    for (const auto& vertex : packet->vertices) {
        const auto p = vertex.position;
        const auto c = vertex.premultiplied;
        marked |= p.x > selected.bounds.x && p.x < selected.bounds.x + 40 &&
                  p.y >= selected.bounds.y && p.y < selected.bounds.y + selected.bounds.h &&
                  c.r == accent.r && c.g == accent.g && c.b == accent.b;
    }
    CHECK(marked);
    label.assign("Changed caller storage");
    CHECK(selected.label == "Second");
    CHECK(selected.value == "Second");
}

TEST_CASE("missing duplicate colliding and empty choices warn without replacing a model") {
    auto context = wui::test::make_context();
    int selected = 99;
    const std::array<wui::Choice<int>, 2> options{{{10, "First"}, {20, "Second"}}};
    auto frame = wui::test::begin(*context);
    CHECK_FALSE(frame.tabs("section", selected, options));
    auto packet = wui::test::finish(frame);
    CHECK(selected == 99);
    CHECK_FALSE(wui::test::semantic(packet, wui::Role::tab, "First").selected);
    CHECK_FALSE(wui::test::semantic(packet, wui::Role::tab, "Second").selected);
    REQUIRE(context->diagnostics().size() == 1);
    CHECK(context->diagnostics().front().code == wui::ErrorCode::invalid_argument);
    packet.reset();

    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    const std::array<wui::Choice<int>, 2> duplicate{{{20, "One"}, {20, "Two"}}};
    auto duplicates = wui::test::begin(*context, input);
    CHECK(context->diagnostics().empty());
    CHECK_FALSE(duplicates.tabs("section", selected, duplicate));
    packet = wui::test::finish(duplicates);
    CHECK(selected == 99);
    REQUIRE(context->diagnostics().size() == 1);
    CHECK(context->diagnostics().front().code == wui::ErrorCode::invalid_argument);
    packet.reset();

    CollidingChoiceValue collision_selected{1};
    const std::array<wui::Choice<CollidingChoiceValue>, 2> collisions{{
        {CollidingChoiceValue{1}, "One"}, {CollidingChoiceValue{2}, "Two"}}};
    auto colliding = wui::test::begin(*context, input);
    CHECK_FALSE(colliding.radio("collision", "Collision", collision_selected, collisions));
    packet = wui::test::finish(colliding);
    CHECK(collision_selected.id == 1);
    REQUIRE(context->diagnostics().size() == 1);
    packet.reset();

    auto empty = wui::test::begin(*context);
    const std::span<const wui::Choice<int>> no_options;
    for (int i = 0; i < 70; ++i) CHECK_FALSE(empty.tabs("empty", selected, no_options));
    packet = wui::test::finish(empty);
    CHECK(selected == 99);
    CHECK(context->diagnostics().size() == 64);
    packet.reset();
    auto valid = wui::test::begin(*context);
    CHECK(context->diagnostics().empty());
    selected = 20;
    CHECK_FALSE(valid.tabs("section", selected, options));
    packet = wui::test::finish(valid);
    CHECK(context->diagnostics().empty());
}

TEST_CASE("disabled choices do not enter keyboard traversal or change selected value") {
    auto context = wui::test::make_context();
    const std::array<wui::Choice<int>, 2> options{{{10, "First"}, {20, "Second"}}};
    int selected = 10;
    auto first = wui::test::begin(*context);
    CHECK_FALSE(first.tabs("section", selected, options, wui::Orientation::horizontal,
                           {.enabled = false}));
    first.button("following", "Following");
    auto packet = wui::test::finish(first);
    packet.reset();

    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {wui::test::key_event(wui::EventKind::key_down, wui::Key::tab),
                    wui::test::key_event(wui::EventKind::key_down, wui::Key::enter),
                    wui::test::key_event(wui::EventKind::key_up, wui::Key::enter)};
    auto second = wui::test::begin(*context, input);
    CHECK_FALSE(second.tabs("section", selected, options, wui::Orientation::horizontal,
                            {.enabled = false}));
    CHECK(second.button("following", "Following"));
    packet = wui::test::finish(second);
    CHECK(selected == 10);
    CHECK_FALSE(wui::test::semantic(packet, wui::Role::tab, "First").enabled);
    CHECK_FALSE(wui::test::semantic(packet, wui::Role::tab, "Second").focused);
}

TEST_CASE("tab content can disappear without transferring focus or model value") {
    auto context = wui::test::make_context();
    const std::array<wui::Choice<int>, 2> options{{{10, "Appearance"}, {20, "Controls"}}};
    int selected = 10;
    bool enabled = true;
    auto draw = [&](wui::FrameInput input) {
        auto frame = wui::test::begin(*context, input);
        frame.tabs("section", selected, options);
        if (selected == 10) {
            auto panel = frame.panel("appearance");
            frame.checkbox("explicit-child", "Persist", enabled);
        } else {
            auto panel = frame.panel("controls");
            frame.button("explicit-child", "Other control");
        }
        return wui::test::finish(frame);
    };
    wui::FrameInput input{{0, 0, 400, 300}};
    auto first = draw(input);
    const auto checkbox = wui::test::semantic(first, wui::Role::checkbox, "Persist");
    const auto other_tab = wui::test::semantic(first, wui::Role::tab, "Controls").bounds;
    first.reset();
    const wui::Vec2 point{checkbox.bounds.x + 8, checkbox.bounds.y + 8};
    input.events = {{wui::EventKind::pointer_down, point},
                    {wui::EventKind::pointer_up, point}};
    auto focused = draw(input);
    CHECK(wui::test::semantic(focused, wui::Role::checkbox, "Persist").focused);
    CHECK_FALSE(enabled);
    focused.reset();
    const wui::Vec2 switch_to{other_tab.x + 8, other_tab.y + 8};
    input.events = {{wui::EventKind::pointer_down, switch_to},
                    {wui::EventKind::pointer_up, switch_to}};
    auto hidden = draw(input);
    CHECK(selected == 20);
    CHECK_FALSE(wui::test::semantic(hidden, wui::Role::button, "Other control").focused);
    CHECK_FALSE(enabled);
    const auto first_tab = wui::test::semantic(hidden, wui::Role::tab, "Appearance").bounds;
    hidden.reset();
    const wui::Vec2 switch_back{first_tab.x + 8, first_tab.y + 8};
    input.events = {{wui::EventKind::pointer_down, switch_back},
                    {wui::EventKind::pointer_up, switch_back}};
    auto restored = draw(input);
    CHECK(selected == 10);
    CHECK_FALSE(enabled);
    CHECK(wui::test::semantic(restored, wui::Role::checkbox, "Persist").id == checkbox.id);
}

TEST_CASE("dropdown commits only on matching release and tracks reordered values") {
    auto context = wui::test::make_context();
    std::array<wui::Choice<int>, 3> choices{{{0, "Low"}, {1, "Medium"}, {2, "High"}}};
    int selected = 1;
    auto draw = [&](wui::FrameInput input) {
        auto frame = wui::test::begin(*context, input);
        const bool changed = frame.dropdown("quality", "Quality", selected, choices);
        return std::pair{wui::test::finish(frame), changed};
    };
    wui::FrameInput input{{0, 0, 400, 300}};
    auto [baseline, initial_change] = draw(input);
    CHECK_FALSE(initial_change);
    const auto trigger = wui::test::semantic(baseline, wui::Role::dropdown, "Quality").bounds;
    CHECK(wui::test::semantic(baseline, wui::Role::dropdown, "Quality").value == "Medium");
    baseline.reset();
    input.events = {{wui::EventKind::pointer_down, {trigger.x + 12, trigger.y + 12}},
                    {wui::EventKind::pointer_up, {trigger.x + 12, trigger.y + 12}}};
    auto [opened, open_change] = draw(input);
    CHECK_FALSE(open_change);
    const auto high = wui::test::semantic(opened, wui::Role::dropdown, "High");
    const auto high_id = high.id;
    opened.reset();
    std::swap(choices[0], choices[2]);
    input.events.clear();
    auto [reordered, reorder_change] = draw(input);
    CHECK_FALSE(reorder_change);
    const auto& new_high = wui::test::semantic(reordered, wui::Role::dropdown, "High");
    CHECK(new_high.id == high_id);
    const wui::Vec2 point{new_high.bounds.x + 12, new_high.bounds.y + 12};
    reordered.reset();
    input.events = {{wui::EventKind::pointer_down, point}};
    auto [pressed, press_change] = draw(input);
    CHECK_FALSE(press_change);
    CHECK(selected == 1);
    pressed.reset();
    input.events = {{wui::EventKind::pointer_up, point}};
    auto [committed, release_change] = draw(input);
    CHECK(release_change);
    CHECK(selected == 2);
    CHECK(wui::test::semantic(committed, wui::Role::dropdown, "Quality").value == "High");
    committed.reset();
    input.events.clear();
    auto [reopened, unchanged] = draw(input);
    CHECK_FALSE(unchanged);
    CHECK(wui::test::semantic(reopened, wui::Role::dropdown, "Quality").value == "High");
    reopened.reset();
    input.events = {{wui::EventKind::pointer_down, {trigger.x + 12, trigger.y + 12}},
                    {wui::EventKind::pointer_up, {trigger.x + 12, trigger.y + 12}}};
    auto [after_reorder, no_change] = draw(input);
    CHECK_FALSE(no_change);
    CHECK(wui::test::semantic(after_reorder, wui::Role::dropdown, "High").id == high_id);
}

TEST_CASE("dropdown keyboard selection and cancellation preserve value and focus") {
    auto context = wui::test::make_context();
    const std::array<wui::Choice<int>, 3> choices{{{0, "Low"}, {1, "Medium"}, {2, "High"}}};
    int selected = 1;
    auto draw = [&](wui::FrameInput input) {
        auto frame = wui::test::begin(*context, input);
        const bool changed = frame.dropdown("quality", "Quality", selected, choices);
        return std::pair{wui::test::finish(frame), changed};
    };
    wui::FrameInput input{{0, 0, 400, 300}};
    auto [initial, _] = draw(input);
    const auto trigger = wui::test::semantic(initial, wui::Role::dropdown, "Quality").bounds;
    initial.reset();
    input.events = {{wui::EventKind::pointer_down, {trigger.x + 12, trigger.y + 12}},
                    {wui::EventKind::pointer_up, {trigger.x + 12, trigger.y + 12}}};
    auto [opened, opening] = draw(input);
    CHECK_FALSE(opening);
    CHECK(wui::test::semantic(opened, wui::Role::dropdown, "Medium").focused);
    opened.reset();
    input.events = {wui::test::key_event(wui::EventKind::key_down, wui::Key::down)};
    auto [navigated, navigation] = draw(input);
    CHECK_FALSE(navigation);
    CHECK(selected == 1);
    CHECK(wui::test::semantic(navigated, wui::Role::dropdown, "High").focused);
    navigated.reset();
    input.events = {wui::test::key_event(wui::EventKind::key_down, wui::Key::enter),
                    wui::test::key_event(wui::EventKind::key_up, wui::Key::enter)};
    auto [committed, changed] = draw(input);
    CHECK(changed);
    CHECK(selected == 2);
    committed.reset();
    input.events = {{wui::EventKind::pointer_down, {trigger.x + 12, trigger.y + 12}},
                    {wui::EventKind::pointer_up, {trigger.x + 12, trigger.y + 12}}};
    auto [reopened, opening_again] = draw(input);
    CHECK_FALSE(opening_again);
    reopened.reset();
    input.events = {wui::test::key_event(wui::EventKind::key_down, wui::Key::up),
                    wui::test::key_event(wui::EventKind::key_down, wui::Key::escape)};
    auto [cancelled, cancellation] = draw(input);
    CHECK_FALSE(cancellation);
    CHECK(selected == 2);
    CHECK(wui::test::semantic(cancelled, wui::Role::dropdown, "Quality").focused);
    CHECK(std::none_of(cancelled->semantics.begin(), cancelled->semantics.end(),
          [](const wui::Semantic& entry) {
              return entry.role == wui::Role::dropdown && entry.label == "Low";
          }));
    cancelled.reset();
    input.events = {{wui::EventKind::pointer_down, {trigger.x + 12, trigger.y + 12}},
                    {wui::EventKind::pointer_up, {trigger.x + 12, trigger.y + 12}}};
    auto [another_open, opening_third] = draw(input);
    CHECK_FALSE(opening_third);
    another_open.reset();
    input.events = {wui::test::key_event(wui::EventKind::key_down, wui::Key::up),
                    wui::test::key_event(wui::EventKind::key_down, wui::Key::enter),
                    wui::test::key_event(wui::EventKind::key_up, wui::Key::enter)};
    auto [keyboard_commit, sequential] = draw(input);
    CHECK(sequential);
    CHECK(selected == 1);
}

TEST_CASE("dropdown flips and clamps popup options at all viewport edges") {
    const std::array<wui::Choice<int>, 3> choices{{{0, "Low"}, {1, "Medium"}, {2, "High"}}};
    for (bool bottom : {false, true}) for (bool right : {false, true}) {
        auto context = wui::test::make_context();
        int selected = 1;
        auto draw = [&](wui::FrameInput input) {
            auto frame = wui::test::begin(*context, input);
            {
                auto panel = frame.panel("settings");
                if (bottom) frame.spacer(180);
                if (right) {
                    const std::array<wui::Column, 2> columns{{
                        {wui::ColumnKind::fixed, 250}, {wui::ColumnKind::share, 1}}};
                    auto row = frame.row("right", columns);
                    { auto cell = row.cell(); frame.spacer(0); }
                    { auto cell = row.cell(); frame.dropdown("quality", "Quality", selected, choices); }
                } else {
                    frame.dropdown("quality", "Quality", selected, choices);
                }
            }
            return wui::test::finish(frame);
        };
        wui::FrameInput input{{0, 0, 400, 300}};
        auto initial = draw(input);
        const auto trigger = wui::test::semantic(initial, wui::Role::dropdown, "Quality").bounds;
        CHECK((trigger.x > 200) == right);
        initial.reset();
        input.events = {{wui::EventKind::pointer_down, {trigger.x + 8, trigger.y + 8}},
                        {wui::EventKind::pointer_up, {trigger.x + 8, trigger.y + 8}}};
        auto opened = draw(input);
        const auto& first = wui::test::semantic(opened, wui::Role::dropdown, "Low");
        CHECK((first.bounds.y < trigger.y) == bottom);
        for (const auto& entry : opened->semantics) {
            if (entry.role != wui::Role::dropdown || entry.label == "Quality") continue;
            CHECK(entry.bounds.x >= input.viewport.x);
            CHECK(entry.bounds.y >= input.viewport.y);
            CHECK(entry.bounds.x + entry.bounds.w <= input.viewport.x + input.viewport.w);
            CHECK(entry.bounds.y + entry.bounds.h <= input.viewport.y + input.viewport.h);
        }
    }
}

TEST_CASE("missing or duplicate dropdown values cannot replace caller selection") {
    auto context = wui::test::make_context();
    int selected = 99;
    const std::array<wui::Choice<int>, 2> choices{{{1, "One"}, {2, "Two"}}};
    auto frame = wui::test::begin(*context);
    CHECK_FALSE(frame.dropdown("quality", "Quality", selected, choices));
    auto packet = wui::test::finish(frame);
    CHECK(selected == 99);
    CHECK(wui::test::semantic(packet, wui::Role::dropdown, "Quality").value.empty());
    REQUIRE(context->diagnostics().size() == 1);
    packet.reset();
    const std::array<wui::Choice<int>, 2> duplicate{{{1, "One"}, {1, "Again"}}};
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    auto invalid = wui::test::begin(*context, input);
    CHECK_FALSE(invalid.dropdown("quality", "Quality", selected, duplicate));
    packet = wui::test::finish(invalid);
    CHECK(selected == 99);
    REQUIRE(context->diagnostics().size() == 1);
}
