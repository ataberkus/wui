#include "../../src/core/context_detail.hpp"
#include "../support/widget_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <utility>
#include <variant>

namespace {
constexpr wui::Id owner{51};
constexpr wui::Id option{52};
constexpr wui::Id under{53};
constexpr wui::Rect root{0, 0, 240, 180};
constexpr wui::Rect list{90, 35, 110, 90};
constexpr wui::Rect underlying{20, 30, 60, 32};
bool same(wui::Rect a, wui::Rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

wui::InputEvent wheel(float amount, wui::Vec2 position) {
    wui::InputEvent event{wui::EventKind::wheel};
    event.position = position;
    event.wheel = amount;
    return event;
}

wui::detail::FrameState state() { return wui::detail::FrameState(wui::test::fixture_desc()); }
}

TEST_CASE("a root popup consumes wheel records in order without scrolling its parent") {
    auto s = state();
    s.popup.owner = owner;
    wui::FrameInput input{root};
    input.events = {wheel(-1, {100, 50}), wheel(1, {100, 50})};
    s.interaction.begin(input, s.popup.owner);
    CHECK(s.route_popup_wheel(list, 50, 60) == doctest::Approx(28));
    CHECK(s.settle_scroll(root, 50, 60) == doctest::Approx(50));
    s.interaction.end();

    input.events = {wheel(-1, {100, 50})};
    s.interaction.begin(input, s.popup.owner);
    CHECK(s.route_popup_wheel(list, 50, 60) == doctest::Approx(60));
    CHECK(s.settle_scroll(root, 50, 60) == doctest::Approx(50));
    s.interaction.end();
    input.events.clear();
    s.interaction.begin(input, s.popup.owner);
    CHECK(s.route_popup_wheel(list, 60, 60) == doctest::Approx(60));
    s.interaction.end();

    input.events = {wheel(-1, {30, 150})};
    s.interaction.begin(input, s.popup.owner);
    CHECK(s.route_popup_wheel(list, 50, 60) == doctest::Approx(50));
    CHECK(s.settle_scroll(root, 50, 60) == doctest::Approx(50));
    s.interaction.end();
    input.events = {wheel(-1, {100, 50})};
    s.interaction.begin(input, s.popup.owner);
    s.popup.owner = {};
    CHECK(s.route_popup_wheel(list, 50, 60) == doctest::Approx(50));
    CHECK(s.settle_scroll(root, 50, 60) == doctest::Approx(50));
    s.interaction.end();
}

TEST_CASE("an outside close never replays input into earlier or later widgets") {
    auto s = state();
    s.popup.owner = owner;
    s.popup_bounds = list;
    s.viewport = root;
    wui::FrameInput input{root};
    input.events = {{wui::EventKind::pointer_down, {30, 45}},
                    {wui::EventKind::pointer_up, {30, 45}},
                    wheel(-1, {30, 45})};
    s.interaction.begin(input, s.popup.owner);
    const auto blocked = s.interaction.button(under, underlying, true);
    CHECK_FALSE(blocked.activated);
    CHECK_FALSE(blocked.hovered);
    CHECK_FALSE(s.route_popup_button(option, {95, 40, 90, 32}, true).activated);
    CHECK(s.popup.owner == wui::Id{});
    CHECK(s.settle_scroll(root, 10, 60) == doctest::Approx(10));
    s.interaction.end();

    input.events.clear();
    s.interaction.begin(input, s.popup.owner);
    CHECK(s.settle_scroll(root, 10, 60) == doctest::Approx(10));
    s.interaction.end();
    input.events = {wheel(-1, {30, 45})};
    s.interaction.begin(input, s.popup.owner);
    CHECK(s.settle_scroll(root, 10, 60) == doctest::Approx(42));
    s.interaction.end();

    wui::Interaction neighbor;
    input.events = {{wui::EventKind::pointer_down, {30, 45}},
                    {wui::EventKind::pointer_up, {30, 45}}};
    neighbor.begin(input, {});
    CHECK(neighbor.button(under, underlying, true).activated);
    neighbor.end();
}

TEST_CASE("popup options activate only after a matching release") {
    auto s = state();
    s.popup.owner = owner;
    s.popup_bounds = list;
    s.viewport = root;
    wui::FrameInput input{root};
    input.events = {{wui::EventKind::pointer_down, {100, 50}}};
    s.interaction.begin(input, s.popup.owner);
    CHECK_FALSE(s.route_popup_button(option, {95, 40, 90, 32}, true).activated);
    CHECK(s.interaction.captured() == option);
    s.interaction.end();

    input.events = {{wui::EventKind::pointer_up, {100, 50}}};
    s.interaction.begin(input, s.popup.owner);
    CHECK(s.route_popup_button(option, {95, 40, 90, 32}, true).activated);
    CHECK(s.interaction.captured() == wui::Id{});
    s.interaction.end();
}

TEST_CASE("focus loss and missing owners close the popup without replay") {
    auto s = state();
    s.popup.owner = owner;
    s.popup_bounds = list;
    s.viewport = root;
    wui::FrameInput input{root};
    input.events = {{wui::EventKind::pointer_down, {100, 50}},
                    {wui::EventKind::focus_lost},
                    {wui::EventKind::pointer_up, {100, 50}}};
    s.interaction.begin(input, s.popup.owner);
    CHECK_FALSE(s.route_popup_button(option, {95, 40, 90, 32}, true).activated);
    CHECK(s.popup.owner == wui::Id{});
    CHECK(s.interaction.captured() == wui::Id{});
    s.interaction.end();

    s.popup.owner = owner;
    s.generation = 1;
    s.canvas.begin(root, 1);
    wui::detail::paint_popup(s); // owner not emitted in this frame
    CHECK(s.popup.owner == wui::Id{});
    auto result = s.canvas.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
}

TEST_CASE("Escape closes an open popup before a later underlying pointer gesture") {
    auto s = state();
    s.popup.owner = owner;
    s.popup_bounds = list;
    s.viewport = root;
    wui::FrameInput input{root};
    input.events = {wui::test::key_event(wui::EventKind::key_down, wui::Key::escape),
                    wui::test::key_event(wui::EventKind::key_up, wui::Key::escape),
                    {wui::EventKind::pointer_down, {30, 45}},
                    {wui::EventKind::pointer_up, {30, 45}}};
    s.interaction.begin(input, s.popup.owner);
    CHECK_FALSE(s.interaction.button(under, underlying, true).activated);
    CHECK_FALSE(s.route_popup_button(option, {95, 40, 90, 32}, true).activated);
    CHECK(s.popup.owner == wui::Id{});
    s.interaction.end();
}

TEST_CASE("popup paint is last, root-clipped and owns its label after the caller changes it") {
    auto s = state();
    s.generation = 1;
    s.viewport = root;
    s.popup.owner = owner;
    s.widgets[owner.value].seen = 1;
    s.popup_bounds = {170, 80, 110, 70};
    s.popup_theme = wui::Theme::dark();
    s.canvas.begin(root, 1);
    s.canvas.rounded_rect(root, 0, s.theme.canvas);
    s.canvas.push_clip({20, 20, 60, 70});
    s.canvas.rounded_rect({20, 30, 60, 32}, 2, s.theme.control);
    s.canvas.pop_clip();
    std::string label = "Very long copied option label";
    auto run = s.shape(label, wui::FontRole::body);
    REQUIRE(run);
    s.record_popup_option(option, {175, 85, 90, 32}, label, std::move(run),
                          {}, true, true, false);
    label.assign("changed");
    wui::detail::paint_popup(s);
    auto finished = s.canvas.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(finished));
    auto packet = std::get<wui::DrawPacket>(std::move(finished));
    REQUIRE(packet);
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    REQUIRE(packet->batches.size() >= 4);
    CHECK(same(packet->batches.front().clip, root));
    CHECK(same(packet->batches[1].clip, {20, 20, 60, 70}));
    CHECK(same(packet->batches[2].clip, root));
    CHECK(same(packet->batches.back().clip, wui::intersect(root, s.popup_bounds)));
    const auto& selected = wui::test::semantic(packet, wui::Role::dropdown,
                                                "Very long copied option label");
    CHECK(selected.selected);
    CHECK(same(selected.bounds, {175, 85, 65, 32}));
}

TEST_CASE("dropdown overlay escapes parent scissor while modal wheel and outside click stay gated") {
    auto context = wui::test::make_context();
    const std::array<wui::Choice<int>, 8> choices{{
        {0, "Low"}, {1, "Medium"}, {2, "High"}, {3, "Ultra"},
        {4, "Mode 4"}, {5, "Mode 5"}, {6, "Mode 6"}, {7, "Mode 7"}}};
    int selected = 1;
    float parent_offset = 0;
    wui::Rect parent_view{};
    auto draw = [&](wui::FrameInput input, bool emit = true, float shift = 0) {
        auto frame = wui::test::begin(*context, input);
        const bool behind = frame.button("behind", "Behind");
        {
            auto panel = frame.panel("settings");
            auto scroll = frame.scroll("parent", 64);
            parent_offset = scroll.offset();
            parent_view = scroll.viewport();
            frame.spacer(shift);
            if (emit) frame.dropdown("quality", "Quality", selected, choices);
            frame.spacer(300);
        }
        return std::pair{wui::test::finish(frame), behind};
    };
    wui::FrameInput input{root};
    auto [base, _] = draw(input);
    const auto trigger = wui::test::semantic(base, wui::Role::dropdown, "Quality").bounds;
    const auto behind = wui::test::semantic(base, wui::Role::button, "Behind").bounds;
    base.reset();
    const wui::Vec2 trigger_point{trigger.x + 8, trigger.y + 8};
    input.events = {{wui::EventKind::pointer_down, trigger_point},
                    {wui::EventKind::pointer_up, trigger_point}};
    auto [opened, no_behind] = draw(input);
    CHECK_FALSE(no_behind);
    const auto first = wui::test::semantic(opened, wui::Role::dropdown, "Low").bounds;
    const auto medium = wui::test::semantic(opened, wui::Role::dropdown, "Medium").bounds;
    CHECK(medium.y + medium.h > parent_view.y + parent_view.h);
    REQUIRE_FALSE(opened->batches.empty());
    const auto overlay_clip = opened->batches.back().clip;
    CHECK(overlay_clip.y + overlay_clip.h > parent_view.y + parent_view.h);
    CHECK(overlay_clip.y + overlay_clip.h <= root.y + root.h);
    opened.reset();

    input.events = {wheel(-1, {first.x + 8, first.y + 8})};
    auto [scrolled, ignored] = draw(input);
    CHECK_FALSE(ignored);
    CHECK(parent_offset == doctest::Approx(0));
    CHECK(wui::test::semantic(scrolled, wui::Role::dropdown, "Medium").bounds.y ==
          doctest::Approx(medium.y));
    scrolled.reset();
    input.events.clear();
    auto [shifted, still_ignored] = draw(input);
    CHECK_FALSE(still_ignored);
    CHECK(parent_offset == doctest::Approx(0));
    CHECK(wui::test::semantic(shifted, wui::Role::dropdown, "Medium").bounds.y < medium.y);
    shifted.reset();

    const wui::Vec2 behind_point{behind.x + 8, behind.y + 8};
    input.events = {{wui::EventKind::pointer_down, behind_point},
                    {wui::EventKind::pointer_up, behind_point},
                    wheel(-1, {first.x + 8, first.y + 8})};
    auto [closed, blocked] = draw(input);
    CHECK_FALSE(blocked);
    CHECK(parent_offset == doctest::Approx(0));
    CHECK(std::none_of(closed->semantics.begin(), closed->semantics.end(),
          [](const wui::Semantic& entry) {
              return entry.role == wui::Role::dropdown && entry.label == "Low";
          }));
    closed.reset();

    input.events = {{wui::EventKind::pointer_down, trigger_point},
                    {wui::EventKind::pointer_up, trigger_point}};
    auto [reopened, another] = draw(input);
    CHECK_FALSE(another);
    CHECK(wui::test::semantic(reopened, wui::Role::dropdown, "Low").enabled);
    reopened.reset();
    input.events.clear();
    auto [moved, ignored_move] = draw(input, true, 12);
    CHECK_FALSE(ignored_move);
    CHECK(std::none_of(moved->semantics.begin(), moved->semantics.end(),
          [](const wui::Semantic& entry) {
              return entry.role == wui::Role::dropdown && entry.label == "Low";
          }));
    moved.reset();

    input.events = {{wui::EventKind::pointer_down, trigger_point},
                    {wui::EventKind::pointer_up, trigger_point}};
    auto [again, ignored_open] = draw(input);
    CHECK_FALSE(ignored_open);
    again.reset();
    input.events.clear();
    auto [removed, ignored_remove] = draw(input, false);
    CHECK_FALSE(ignored_remove);
    CHECK(std::none_of(removed->semantics.begin(), removed->semantics.end(),
          [](const wui::Semantic& entry) {
              return entry.role == wui::Role::dropdown && entry.label == "Low";
          }));
    removed.reset();
    input.events = {wheel(-1, {trigger.x + 8, trigger.y + 8})};
    auto [normal, ignored_wheel] = draw(input);
    CHECK_FALSE(ignored_wheel);
    CHECK(parent_offset == doctest::Approx(0));
    normal.reset();
    input.events.clear();
    auto [next, ignored_next] = draw(input);
    CHECK_FALSE(ignored_next);
    CHECK(parent_offset > 0);
}

TEST_CASE("keyboard navigation scrolls long dropdown lists to reveal the focused option") {
    auto context = wui::test::make_context();
    const std::array<wui::Choice<int>, 8> choices{{
        {0, "Low"}, {1, "Medium"}, {2, "High"}, {3, "Ultra"},
        {4, "Mode 4"}, {5, "Mode 5"}, {6, "Mode 6"}, {7, "Mode 7"}}};
    int selected = 1;
    auto draw = [&](wui::FrameInput input) {
        auto frame = wui::test::begin(*context, input);
        frame.dropdown("quality", "Quality", selected, choices);
        return wui::test::finish(frame);
    };
    wui::FrameInput input{root};
    auto initial = draw(input);
    const auto trigger = wui::test::semantic(initial, wui::Role::dropdown, "Quality").bounds;
    initial.reset();
    input.events = {{wui::EventKind::pointer_down, {trigger.x + 8, trigger.y + 8}},
                    {wui::EventKind::pointer_up, {trigger.x + 8, trigger.y + 8}}};
    auto open = draw(input);
    open.reset();
    input.events = {wui::test::key_event(wui::EventKind::key_down, wui::Key::down),
                    wui::test::key_event(wui::EventKind::key_down, wui::Key::down),
                    wui::test::key_event(wui::EventKind::key_down, wui::Key::down)};
    auto navigate = draw(input);
    CHECK(selected == 1);
    navigate.reset();
    input.events.clear();
    auto revealed = draw(input);
    const auto& focused = wui::test::semantic(revealed, wui::Role::dropdown, "Mode 4");
    CHECK(focused.focused);
    CHECK(focused.bounds.h > 0);
    CHECK(focused.bounds.y + focused.bounds.h <= root.y + root.h);
}

TEST_CASE("delayed tooltips stay root-clamped, copied, and noninteractive") {
    auto context = wui::test::make_context();
    std::string message = "Save preferences";
    auto draw = [&](wui::FrameInput input, bool owner_present = true, bool enabled = true) {
        auto frame = wui::test::begin(*context, input);
        if (owner_present) frame.button("save", "Save", {.enabled = enabled});
        frame.tooltip("save", message);
        const bool behind = frame.button("behind", "Behind");
        return std::pair{wui::test::finish(frame), behind};
    };
    wui::FrameInput input{root};
    input.dt_seconds = 0.1;
    auto [first, _] = draw(input);
    const auto owner_rect = wui::test::semantic(first, wui::Role::button, "Save").bounds;
    const auto behind_rect = wui::test::semantic(first, wui::Role::button, "Behind").bounds;
    first.reset();
    input.events = {{wui::EventKind::pointer_move, {owner_rect.x + 8, owner_rect.y + 8}}};
    for (int i = 0; i < 4; ++i) {
        auto [not_yet, activated] = draw(input);
        CHECK_FALSE(activated);
        CHECK(std::none_of(not_yet->semantics.begin(), not_yet->semantics.end(),
              [](const wui::Semantic& entry) { return entry.role == wui::Role::tooltip; }));
    }
    auto [shown, not_clicked] = draw(input);
    CHECK_FALSE(not_clicked);
    const auto tooltip = wui::test::semantic(shown, wui::Role::tooltip, message);
    CHECK(tooltip.bounds.x >= root.x);
    CHECK(tooltip.bounds.y >= root.y);
    CHECK(tooltip.bounds.x + tooltip.bounds.w <= root.x + root.w);
    CHECK(tooltip.bounds.y + tooltip.bounds.h <= root.y + root.h);
    CHECK_FALSE(tooltip.focused);
    CHECK_FALSE(tooltip.selected);
    message.assign("Changed caller storage");
    CHECK(tooltip.label == "Save preferences");
    shown.reset();

    input.events = {{wui::EventKind::pointer_down, {behind_rect.x + 8, behind_rect.y + 8}},
                    {wui::EventKind::pointer_up, {behind_rect.x + 8, behind_rect.y + 8}}};
    auto [pressed, behind_activated] = draw(input);
    CHECK(behind_activated);
    CHECK_FALSE(wui::test::semantic(pressed, wui::Role::button, "Save").focused);
    pressed.reset();
    input.events = {{wui::EventKind::pointer_move, {owner_rect.x + 8, owner_rect.y + 8}}};
    auto [disabled, not_activated] = draw(input, true, false);
    CHECK_FALSE(not_activated);
    CHECK(std::none_of(disabled->semantics.begin(), disabled->semantics.end(),
          [](const wui::Semantic& entry) { return entry.role == wui::Role::tooltip; }));
    disabled.reset();
    auto [missing, still_not_activated] = draw(input, false);
    CHECK_FALSE(still_not_activated);
    REQUIRE(context->diagnostics().size() == 1);
    CHECK(std::none_of(missing->semantics.begin(), missing->semantics.end(),
          [](const wui::Semantic& entry) { return entry.role == wui::Role::tooltip; }));
    missing.reset();
    auto [returned, no_click] = draw(input);
    CHECK_FALSE(no_click);
    CHECK(std::none_of(returned->semantics.begin(), returned->semantics.end(),
          [](const wui::Semantic& entry) { return entry.role == wui::Role::tooltip; }));
}

TEST_CASE("tooltip delay uses full elapsed time and paints above an open dropdown") {
    auto context = wui::test::make_context();
    const std::array<wui::Choice<int>, 2> choices{{{0, "Low"}, {1, "High"}}};
    int selected = 0;
    auto draw = [&](wui::FrameInput input) {
        auto frame = wui::test::begin(*context, input);
        frame.dropdown("quality", "Quality", selected, choices);
        frame.tooltip("quality", "Dropdown help");
        return wui::test::finish(frame);
    };
    wui::FrameInput input{root};
    auto initial = draw(input);
    const auto trigger = wui::test::semantic(initial, wui::Role::dropdown, "Quality").bounds;
    initial.reset();
    input.events = {{wui::EventKind::pointer_down, {trigger.x + 8, trigger.y + 8}},
                    {wui::EventKind::pointer_up, {trigger.x + 8, trigger.y + 8}}};
    auto opened = draw(input);
    CHECK(wui::test::semantic(opened, wui::Role::dropdown, "Low").selected);
    opened.reset();
    input.events = {{wui::EventKind::pointer_move, {trigger.x + 8, trigger.y + 8}}};
    input.dt_seconds = 0.3;
    auto early = draw(input);
    CHECK(std::none_of(early->semantics.begin(), early->semantics.end(),
          [](const wui::Semantic& entry) { return entry.role == wui::Role::tooltip; }));
    early.reset();
    input.dt_seconds = 0.2;
    auto late = draw(input);
    const auto tooltip = wui::test::semantic(late, wui::Role::tooltip, "Dropdown help");
    CHECK(tooltip.bounds.w > 0);
    REQUIRE(late->semantics.size() >= 3);
    CHECK(late->semantics.back().role == wui::Role::tooltip);
}

TEST_CASE("inline validation wraps in flow, marks invalid controls, and preserves model IDs across themes") {
    auto context = wui::test::make_context();
    bool value = true;
    const std::string message = "Enter a finite number before saving a value";
    auto draw = [&] {
        auto frame = wui::test::begin(*context, wui::FrameInput{{0, 0, 160, 220}});
        frame.checkbox("number", "Number", value, {.invalid = true});
        frame.validation(message);
        frame.button("after", "After", {.invalid = true});
        return wui::test::finish(frame);
    };
    auto first = draw();
    const auto& control = wui::test::semantic(first, wui::Role::checkbox, "Number");
    const auto& validation = wui::test::semantic(first, wui::Role::validation, message);
    const auto& after = wui::test::semantic(first, wui::Role::button, "After");
    const auto control_id = control.id;
    CHECK(control.invalid);
    CHECK(after.invalid);
    CHECK(validation.invalid);
    CHECK(validation.bounds.h > 18);
    CHECK(after.bounds.y >= validation.bounds.y + validation.bounds.h);
    bool red_ink = false;
    for (const auto& vertex : first->vertices) {
        const auto p = vertex.position;
        const auto c = vertex.premultiplied;
        red_ink |= p.y >= control.bounds.y && p.y <= control.bounds.y + control.bounds.h &&
                   c.r > 0.9f && c.g < 0.6f;
    }
    CHECK(red_ink);
    first.reset();
    auto theme = wui::Theme::dark();
    theme.accent = {0.1f, 0.7f, 0.6f, 1};
    REQUIRE_FALSE(context->set_theme(theme).has_value());
    auto changed_theme = draw();
    CHECK(value);
    CHECK(wui::test::semantic(changed_theme, wui::Role::checkbox, "Number").id == control_id);
    CHECK(wui::test::semantic(changed_theme, wui::Role::validation, message).invalid);
}
