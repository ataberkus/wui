#include <wui/wui.hpp>
#include "../support/widget_fixture.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <cmath>
#include <map>
#include <set>

#include "gallery.hpp"

namespace {
using namespace wui::test;
bool painted(const wui::DrawPacket& packet, wui::Rect bounds, wui::Color expected) {
    for (const auto& vertex : packet->vertices) {
        const auto p = vertex.position;
        const auto c = vertex.premultiplied;
        if (p.x >= bounds.x && p.x <= bounds.x + bounds.w &&
            p.y >= bounds.y && p.y < bounds.y + bounds.h &&
            std::abs(c.r - expected.r) < 1e-5f &&
            std::abs(c.g - expected.g) < 1e-5f &&
            std::abs(c.b - expected.b) < 1e-5f &&
            std::abs(c.a - expected.a) < 1e-5f) return true;
    }
    return false;
}


class FailingText final : public wui::IText {
public:
    bool fail_shape = false;
    bool fail_publish = false;
    wui::Result<std::shared_ptr<const wui::TextRun>> shape(
        const wui::ShapeRequest& request) override {
        if (fail_shape)
            return wui::Error{wui::ErrorCode::invalid_font, "fixture shape failure"};
        return text_.shape(request);
    }
    wui::Status publish() override {
        if (fail_publish)
            return wui::Error{wui::ErrorCode::backend_failure, "fixture publish failure"};
        return std::nullopt;
    }
    wui::Result<std::vector<std::size_t>> grapheme_offsets(std::string_view source) override {
        return text_.grapheme_offsets(source);
    }
private:
    FixtureText text_;
};
}

TEST_CASE("checkbox updates model synchronously on captured release") {
    wui::ContextDesc desc;
    desc.textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    desc.text = std::make_shared<FixtureText>();
    auto created = wui::Context::create(desc);
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::Context>>(created));
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    bool checked = false;
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 30}}};
    {
        auto frame = std::get<wui::Frame>(context->begin_frame(input));
        { auto panel = frame.panel("settings");
          CHECK_FALSE(frame.checkbox("check", "Enable", checked)); }
        REQUIRE(std::holds_alternative<wui::DrawPacket>(frame.finish()));
    }
    input.events = {{wui::EventKind::pointer_up, {30, 30}}};
    {
        auto frame = std::get<wui::Frame>(context->begin_frame(input));
        { auto panel = frame.panel("settings");
          CHECK(frame.checkbox("check", "Enable", checked));
          CHECK(checked); }
        REQUIRE(std::holds_alternative<wui::DrawPacket>(frame.finish()));
    }
}

TEST_CASE("identical keys in independent contexts cannot share capture") {
    auto first = make_context();
    auto second = make_context();
    bool first_value = false, second_value = false;
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 30}}};
    auto first_frame = begin(*first, input);
    {
        auto panel = first_frame.panel("settings");
        CHECK_FALSE(first_frame.checkbox("check", "Enable", first_value));
    }
    auto first_packet = finish(first_frame);
    input.events = {{wui::EventKind::pointer_up, {30, 30}}};
    auto second_frame = begin(*second, input);
    {
        auto panel = second_frame.panel("settings");
        CHECK_FALSE(second_frame.checkbox("check", "Enable", second_value));
    }
    auto second_packet = finish(second_frame);
    CHECK_FALSE(second_value);
    CHECK(semantic(first_packet, wui::Role::checkbox, "Enable").id ==
          semantic(second_packet, wui::Role::checkbox, "Enable").id);
    auto released = begin(*first, input);
    {
        auto panel = released.panel("settings");
        CHECK(released.checkbox("check", "Enable", first_value));
        CHECK(first_value);
    }
    finish(released);
}

TEST_CASE("display label changes preserve the explicit key and pointer capture") {
    auto context = make_context();
    bool value = false;
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 30}}};
    auto down = begin(*context, input);
    {
        auto panel = down.panel("settings", {"Old title"});
        CHECK_FALSE(down.checkbox("check", "Old label", value));
    }
    auto before = finish(down);
    // A title precedes the row, so establish capture using its actual bounds.
    const auto old_bounds = semantic(before, wui::Role::checkbox, "Old label").bounds;
    const wui::Vec2 point{old_bounds.x + 8, old_bounds.y + 8};
    input.events = {{wui::EventKind::pointer_down, point}};
    auto pressed = begin(*context, input);
    {
        auto panel = pressed.panel("settings", {"Old title"});
        CHECK_FALSE(pressed.checkbox("check", "Old label", value));
    }
    finish(pressed);
    input.events = {{wui::EventKind::pointer_up, point}};
    auto up = begin(*context, input);
    {
        auto panel = up.panel("settings", {"New title"});
        CHECK(up.checkbox("check", "New display label", value));
        CHECK(value);
    }
    auto after = finish(up);
    CHECK(semantic(before, wui::Role::checkbox, "Old label").id ==
          semantic(after, wui::Role::checkbox, "New display label").id);
    CHECK(semantic(before, wui::Role::panel, "Old title").id ==
          semantic(after, wui::Role::panel, "New title").id);
    CHECK(semantic(after, wui::Role::checkbox, "New display label").selected);
}

TEST_CASE("nested early returns and moved scopes close each panel exactly once") {
    auto context = make_context();
    auto frame = begin(*context);
    const auto emit_nested = [&] {
        auto outer = frame.panel("outer", {"Outer"});
        {
            auto inner = frame.panel("inner", {"Inner"});
            auto moved = std::move(inner);
            inner = std::move(moved);
            frame.text("Nested");
            return;
        }
    };
    emit_nested();
    {
        auto sibling = frame.panel("sibling", {"Sibling"});
        frame.text("Following");
    }
    auto packet = finish(frame);
    const auto& outer = semantic(packet, wui::Role::panel, "Outer");
    const auto& inner = semantic(packet, wui::Role::panel, "Inner");
    const auto& nested = semantic(packet, wui::Role::text, "Nested");
    const auto& sibling = semantic(packet, wui::Role::panel, "Sibling");
    CHECK(inner.bounds.x > outer.bounds.x);
    CHECK(inner.bounds.y > outer.bounds.y);
    CHECK(nested.bounds.y >= inner.bounds.y);
    CHECK(nested.bounds.y + nested.bounds.h <= inner.bounds.y + inner.bounds.h);
    CHECK(inner.bounds.y + inner.bounds.h <= outer.bounds.y + outer.bounds.h);
    CHECK(sibling.bounds.y >= outer.bounds.y + outer.bounds.h);
    CHECK(std::count_if(packet->semantics.begin(), packet->semantics.end(),
        [](const wui::Semantic& item) { return item.role == wui::Role::panel; }) == 3);
}

TEST_CASE("moving an active scope over another closes the destination responsibility") {
    auto first = make_context();
    auto second = make_context();
    auto first_frame = begin(*first);
    auto second_frame = begin(*second);
    {
        auto first_panel = first_frame.panel("panel", {"First"});
        first_frame.text("First child");
        auto second_panel = second_frame.panel("panel", {"Second"});
        second_frame.text("Second child");
        first_panel = std::move(second_panel);
        auto packet = finish(first_frame);
        const auto& panel = semantic(packet, wui::Role::panel, "First");
        const auto& child = semantic(packet, wui::Role::text, "First child");
        CHECK(panel.bounds.y + panel.bounds.h >= child.bounds.y + child.bounds.h + 16);
    }
    auto packet = finish(second_frame);
    const auto& panel = semantic(packet, wui::Role::panel, "Second");
    const auto& child = semantic(packet, wui::Role::text, "Second child");
    CHECK(panel.bounds.y + panel.bounds.h >= child.bounds.y + child.bounds.h + 16);
}

TEST_CASE("frame moves transfer finalization and abort an overwritten unfinished frame") {
    auto first = make_context();
    auto second = make_context();
    {
        auto original = begin(*first);
        original.text("Abandoned");
        auto moved = std::move(original);
        auto replacement = begin(*second);
        replacement.text("Published");
        moved = std::move(replacement);
        auto packet = finish(moved);
        REQUIRE(packet->semantics.size() == 1);
        CHECK(packet->semantics.front().label == "Published");
    }
    auto next = begin(*first);
    next.text("Fresh");
    auto packet = finish(next);
    CHECK(packet->semantics.size() == 1);
    CHECK(packet->semantics.front().label == "Fresh");
}

TEST_CASE("an unfinished frame releases its slot without publishing partial contents") {
    auto desc = fixture_desc();
    desc.max_packet_slots = 1;
    auto context = make_context(std::move(desc));
    {
        auto abandoned = begin(*context);
        auto panel = abandoned.panel("panel");
        abandoned.text("Discarded");
    }
    auto frame = begin(*context);
    frame.text("Replacement");
    auto packet = finish(frame);
    REQUIRE(packet->semantics.size() == 1);
    CHECK(packet->semantics.front().label == "Replacement");
}

TEST_CASE("packets own semantic labels and resources after context destruction") {
    wui::DrawPacket packet;
    {
        auto context = make_context();
        auto frame = begin(*context);
        std::string title = "Settings", body = "Body", heading = "Heading";
        std::string button = "Apply", checkbox = "Enable";
        bool selected = true;
        {
            auto panel = frame.panel("panel", {title});
            title.assign("Overwritten title");
            frame.text(body);
            body.assign("Overwritten body");
            frame.heading(heading);
            heading.assign("Overwritten heading");
            CHECK_FALSE(frame.button("button", button));
            button.assign("Overwritten button");
            CHECK_FALSE(frame.checkbox("checkbox", checkbox, selected));
            checkbox.assign("Overwritten checkbox");
        }
        packet = finish(frame);
    }
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    const auto& panel = semantic(packet, wui::Role::panel, "Settings");
    const auto& last = semantic(packet, wui::Role::checkbox, "Enable");
    CHECK(panel.bounds.y + panel.bounds.h >= last.bounds.y + last.bounds.h + 16);
    CHECK(semantic(packet, wui::Role::text, "Body").bounds.h == doctest::Approx(18));
    CHECK(semantic(packet, wui::Role::text, "Heading").bounds.h == doctest::Approx(18));
    CHECK(semantic(packet, wui::Role::button, "Apply").enabled);
    CHECK(semantic(packet, wui::Role::checkbox, "Enable").selected);
    REQUIRE_FALSE(packet->textures.empty());
    for (const auto& texture : packet->textures) {
        REQUIRE(texture);
        CHECK(texture->rgba.size() == static_cast<std::size_t>(texture->width) *
                                      texture->height * 4);
    }
}

TEST_CASE("disabled and omitted controls clear capture and keyboard focus") {
    auto context = make_context();
    bool value = false;
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 30}}};
    auto pressed = begin(*context, input);
    {
        auto panel = pressed.panel("settings");
        CHECK_FALSE(pressed.checkbox("check", "Enable", value));
    }
    auto before = finish(pressed);
    REQUIRE(semantic(before, wui::Role::checkbox, "Enable").focused);
    input.events.clear();
    auto absent = begin(*context, input);
    {
        auto panel = absent.panel("settings");
        SUBCASE("disabled controls are not focusable or activatable") {
            CHECK_FALSE(absent.checkbox("check", "Enable", value, {false}));
        }
        SUBCASE("hidden controls are omitted from the frame") {}
    }
    auto absent_packet = finish(absent);
    for (const auto& item : absent_packet->semantics) {
        if (item.role == wui::Role::checkbox) {
            CHECK_FALSE(item.enabled);
            CHECK_FALSE(item.focused);
        }
    }
    input.events = {{wui::EventKind::pointer_up, {30, 30}},
                    key_event(wui::EventKind::key_down, wui::Key::space),
                    key_event(wui::EventKind::key_up, wui::Key::space)};
    auto returned = begin(*context, input);
    {
        auto panel = returned.panel("settings");
        CHECK_FALSE(returned.checkbox("check", "Enable", value));
    }
    auto after = finish(returned);
    CHECK_FALSE(value);
    CHECK_FALSE(semantic(after, wui::Role::checkbox, "Enable").focused);
    CHECK(semantic(before, wui::Role::checkbox, "Enable").id ==
          semantic(after, wui::Role::checkbox, "Enable").id);
}

TEST_CASE("theme and density changes preserve model values identity and capture") {
    auto context = make_context();
    bool value = false;
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 30}}};
    auto down = begin(*context, input);
    {
        auto panel = down.panel("settings");
        CHECK_FALSE(down.checkbox("check", "Enable", value));
    }
    auto before = finish(down);
    auto tokens = wui::Theme::dark();
    tokens.accent = {0.3f, 0.2f, 0.7f, 1};
    tokens.reduced_motion = true;
    auto resolved = wui::resolve_theme(tokens, wui::Density::compact);
    REQUIRE(std::holds_alternative<wui::Theme>(resolved));
    REQUIRE_FALSE(context->set_theme(std::get<wui::Theme>(resolved)).has_value());
    CHECK_FALSE(value);
    input.events = {{wui::EventKind::pointer_up, {30, 30}}};
    auto up = begin(*context, input);
    {
        auto panel = up.panel("settings");
        CHECK(up.checkbox("check", "Enable", value));
        CHECK(value);
    }
    auto after = finish(up);
    const auto& old_row = semantic(before, wui::Role::checkbox, "Enable");
    const auto& new_row = semantic(after, wui::Role::checkbox, "Enable");
    CHECK(old_row.id == new_row.id);
    CHECK(old_row.bounds.h == doctest::Approx(36));
    CHECK(new_row.bounds.h == doctest::Approx(28));
    CHECK(new_row.bounds.x == doctest::Approx(12));
    CHECK(new_row.selected);
    REQUIRE_FALSE(context->set_theme(wui::Theme::dark()).has_value());
    input.events.clear();
    auto restored = begin(*context, input);
    {
        auto panel = restored.panel("settings");
        CHECK_FALSE(restored.checkbox("check", "Enable", value));
    }
    auto restored_packet = finish(restored);
    CHECK(value);
    CHECK(semantic(restored_packet, wui::Role::checkbox, "Enable").id == old_row.id);
}

TEST_CASE("text and heading flow uses measured text height rather than control height") {
    auto context = make_context();
    auto frame = begin(*context);
    frame.text("Body");
    frame.heading("Heading");
    CHECK_FALSE(frame.button("button", "Apply"));
    auto packet = finish(frame);
    const auto& body = semantic(packet, wui::Role::text, "Body");
    const auto& heading = semantic(packet, wui::Role::text, "Heading");
    const auto& button = semantic(packet, wui::Role::button, "Apply");
    CHECK(body.bounds.h == doctest::Approx(18));
    CHECK(heading.bounds.h == doctest::Approx(18));
    CHECK(heading.bounds.y == doctest::Approx(body.bounds.y + 18 + 8));
    CHECK(button.bounds.y == doctest::Approx(heading.bounds.y + 18 + 8));
    CHECK(button.bounds.h == doctest::Approx(36));
    CHECK(body.id != heading.id);
}

TEST_CASE("row cells use resolved widths and tallest cell advances following sibling") {
    auto context = make_context();
    auto frame = begin(*context);
    const std::array<wui::Column, 2> widths{{
        {wui::ColumnKind::fixed, 80}, {wui::ColumnKind::share, 1}}};
    CHECK(frame.available_width() == doctest::Approx(400));
    {
        auto row = frame.row("identity", widths);
        auto moved_row = std::move(row);
        {
            auto first = moved_row.cell();
            auto moved_cell = std::move(first);
            CHECK(frame.available_width() == doctest::Approx(80));
            frame.text("Name");
            frame.button("action", "First");
        }
        {
            auto second = moved_row.cell();
            CHECK(frame.available_width() == doctest::Approx(312));
            frame.text("Longer");
            frame.text("Second line");
            frame.button("action", "Second");
        }
    }
    frame.button("next", "Continue");
    const auto packet = finish(frame);
    const auto& first = semantic(packet, wui::Role::button, "First");
    const auto& second = semantic(packet, wui::Role::button, "Second");
    const auto& next = semantic(packet, wui::Role::button, "Continue");
    CHECK(first.bounds.x == doctest::Approx(0));
    CHECK(second.bounds.x == doctest::Approx(88));
    CHECK(second.bounds.w == doctest::Approx(312));
    CHECK(first.bounds.y == doctest::Approx(26));
    CHECK(second.bounds.y == doctest::Approx(52));
    CHECK(first.id != second.id);
    CHECK(next.bounds.y == doctest::Approx(96));
}

TEST_CASE("row scope closes on early return even with unused columns") {
    auto context = make_context();
    auto frame = begin(*context);
    const std::array<wui::Column, 2> widths{{
        {wui::ColumnKind::fixed, 80}, {wui::ColumnKind::share, 1}}};
    const auto emit = [&] {
        auto row = frame.row("partial", widths);
        auto cell = row.cell();
        frame.text("One line");
        return;
    };
    emit();
    frame.button("next", "Continue");
    const auto packet = finish(frame);
    CHECK(semantic(packet, wui::Role::button, "Continue").bounds.y == doctest::Approx(26));
}

TEST_CASE("nested rows keep outer cell geometry stable") {
    auto context = make_context();
    auto frame = begin(*context);
    const std::array outer_columns{wui::Column{wui::ColumnKind::fixed, 80},
                                    wui::Column{wui::ColumnKind::share, 1}};
    const std::array nested_columns{wui::Column{wui::ColumnKind::fixed, 40},
                                     wui::Column{wui::ColumnKind::share, 1}};
    {
        auto outer = frame.row("outer", outer_columns);
        {
            auto first = outer.cell();
            auto nested = frame.row("nested", nested_columns);
            { auto left = nested.cell(); frame.text("Left"); }
            { auto right = nested.cell(); frame.text("Right"); }
        }
        { auto second = outer.cell(); frame.text("Outer second"); }
    }
    const auto packet = finish(frame);
    CHECK(semantic(packet, wui::Role::text, "Right").bounds.x == doctest::Approx(48));
    CHECK(semantic(packet, wui::Role::text, "Outer second").bounds.x == doctest::Approx(88));
}

TEST_CASE("nested scroll claims wheel before its parent and paints within its clip") {
    auto context = make_context();
    wui::FrameInput input{{0, 0, 240, 200}};
    bool activated = false;
    const auto emit = [&](wui::Frame& frame, bool short_content = false) {
        auto outer = frame.scroll("outer", 120);
        const auto outer_offset = outer.offset();
        float inner_offset = 0;
        {
            auto inner = frame.scroll("inner", 80);
            inner_offset = inner.offset();
            if (!short_content) frame.spacer(56);
            activated = frame.button("partly-visible", "Edge");
            if (!short_content) frame.spacer(32);
        }
        if (!short_content) frame.spacer(120);
        return std::pair{outer_offset, inner_offset};
    };
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == std::pair{0.0f, 0.0f});
        auto packet = finish(frame);
        CHECK(semantic(packet, wui::Role::button, "Edge").bounds.y == doctest::Approx(64));
        CHECK(std::any_of(packet->batches.begin(), packet->batches.end(),
            [](const wui::Batch& batch) {
                return batch.clip.y == 64 && batch.clip.y + batch.clip.h <= 80;
            }));
    }
    wui::InputEvent down{wui::EventKind::wheel, {20, 20}};
    down.wheel = -1;
    input.events = {down};
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == std::pair{0.0f, 0.0f});
        finish(frame);
    }
    input.events.clear();
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == std::pair{0.0f, 32.0f});
        finish(frame);
    }
    input.events = {down};
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == std::pair{0.0f, 32.0f});
        finish(frame);
    }
    input.events.clear();
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == std::pair{4.0f, 60.0f});
        finish(frame);
    }
    wui::InputEvent up = down;
    up.wheel = 1;
    input.events = {down, up};
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == std::pair{4.0f, 60.0f});
        finish(frame);
    }
    input.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame, true) == std::pair{36.0f, 28.0f});
        CHECK_FALSE(activated);
        const auto packet = finish(frame);
        CHECK(semantic(packet, wui::Role::button, "Edge").bounds.y == doctest::Approx(-64));
    }
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame, true) == std::pair{0.0f, 0.0f});
        CHECK(activated);
        const auto packet = finish(frame);
        CHECK(semantic(packet, wui::Role::button, "Edge").bounds.y == doctest::Approx(0));
    }
}

TEST_CASE("offscreen nested scroll leaves wheel to parent and expired offsets reset") {
    auto context = make_context();
    wui::FrameInput input{{0, 0, 240, 200}};
    const auto emit = [&](wui::Frame& frame) {
        auto outer = frame.scroll("outer", 120);
        const float offset = outer.offset();
        frame.spacer(60);
        {
            auto inner = frame.scroll("inner", 80);
            CHECK(inner.viewport().y == doctest::Approx(68 - offset));
            CHECK(inner.offset() == 0);
            frame.spacer(100);
        }
        frame.spacer(120);
        return offset;
    };
    wui::InputEvent down{wui::EventKind::wheel, {20, 20}};
    down.wheel = -1;
    input.events = {down};
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == 0);
        finish(frame);
    }
    input.events.clear();
    {
        auto frame = begin(*context, input);
        CHECK(emit(frame) == 32);
        finish(frame);
    }
    for (int i = 0; i < 120; ++i) {
        auto frame = begin(*context, input);
        finish(frame);
    }
    auto frame = begin(*context, input);
    CHECK(emit(frame) == 0);
    finish(frame);
}

TEST_CASE("widget capacity includes panels and text and resets each frame") {
    auto desc = fixture_desc();
    desc.max_widgets = 2;
    auto context = make_context(std::move(desc));
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 60}},
                    {wui::EventKind::pointer_up, {30, 60}}};
    bool value = false;
    auto full = begin(*context, input);
    {
        auto panel = full.panel("panel");
        full.text("Occupies second slot");
        CHECK_FALSE(full.checkbox("overflow", "Must not mutate", value));
    }
    auto failed = full.finish();
    REQUIRE(std::holds_alternative<wui::Error>(failed));
    CHECK(std::get<wui::Error>(failed).code == wui::ErrorCode::capacity);
    CHECK_FALSE(value);
    auto fresh = begin(*context);
    {
        auto panel = fresh.panel("panel");
        CHECK_FALSE(fresh.checkbox("valid", "Fits", value));
    }
    auto packet = finish(fresh);
    CHECK(packet->semantics.size() == 2);
}

TEST_CASE("retained allocation capacity becomes available after 120 absent completed frames") {
    auto desc = fixture_desc();
    desc.max_retained_bytes = 4096;
    desc.max_widgets = 1024;
    std::unique_ptr<wui::Context> full;
    std::size_t fitted = 0;
    bool reached_capacity = false;
    // Calibrate against the public byte ceiling, not implementation-specific
    // node sizes, bucket counts, or a test-only retained-state accessor.
    for (std::size_t count = 1; count <= desc.max_widgets; ++count) {
        auto candidate = make_context(desc);
        auto frame = begin(*candidate);
        for (std::size_t index = 0; index < count; ++index)
            frame.button("retained-" + std::to_string(index), "");
        auto result = frame.finish();
        if (std::holds_alternative<wui::Error>(result)) {
            CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::capacity);
            reached_capacity = true;
            break;
        }
        fitted = count;
        full = std::move(candidate);
    }
    REQUIRE(reached_capacity);
    REQUIRE(fitted > 0);
    REQUIRE(full);
    for (int absent = 0; absent < 119; ++absent) {
        auto frame = begin(*full);
        finish(frame);
    }
    {
        auto frame = begin(*full);
        frame.button("replacement", "");
        auto result = frame.finish();
        REQUIRE(std::holds_alternative<wui::Error>(result));
        CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::capacity);
    }
    {
        auto frame = begin(*full);
        finish(frame);
    }
    auto reused = begin(*full);
    reused.button("replacement", "Admitted after expiry");
    auto packet = finish(reused);
    CHECK(semantic(packet, wui::Role::button, "Admitted after expiry").enabled);
}

TEST_CASE("retained byte errors stop later model mutation") {
    auto desc = fixture_desc();
    desc.max_retained_bytes = 4096;
    auto context = make_context(std::move(desc));
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 10}},
                    {wui::EventKind::pointer_up, {30, 10}}};
    auto frame = begin(*context, input);
    frame.text(std::string(4097, 'x'));
    bool value = false;
    CHECK_FALSE(frame.checkbox("later", "Must remain unchanged", value));
    CHECK_FALSE(value);
    auto result = frame.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::capacity);
}

TEST_CASE("held packet slots reject begin before any input can mutate the model") {
    auto desc = fixture_desc();
    desc.max_packet_slots = 2;
    auto context = make_context(std::move(desc));
    auto first = begin(*context);
    first.text("First packet");
    auto first_packet = finish(first);
    auto second = begin(*context);
    second.text("Second packet");
    auto second_packet = finish(second);
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 10}},
                    {wui::EventKind::pointer_up, {30, 10}}};
    auto busy = context->begin_frame(input);
    REQUIRE(std::holds_alternative<wui::Error>(busy));
    CHECK(std::get<wui::Error>(busy).code == wui::ErrorCode::busy);
    first_packet.reset();
    bool value = false;
    auto resumed = begin(*context);
    CHECK_FALSE(resumed.checkbox("check", "No replay", value));
    auto third_packet = finish(resumed);
    CHECK_FALSE(value);
    REQUIRE(second_packet->semantics.size() == 1);
    CHECK(second_packet->semantics.front().label == "Second packet");
    CHECK(semantic(third_packet, wui::Role::checkbox, "No replay").selected == value);
    REQUIRE_FALSE(wui::validate_packet(*second_packet).has_value());
}

TEST_CASE("text publication failure rejects the packet without rolling back synchronous updates") {
    auto service = std::make_shared<FailingText>();
    auto desc = fixture_desc();
    desc.text = service;
    desc.max_packet_slots = 1;
    auto context = make_context(std::move(desc));
    service->fail_publish = true;
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 10}},
                    {wui::EventKind::pointer_up, {30, 10}}};
    auto frame = begin(*context, input);
    bool value = false;
    CHECK(frame.checkbox("check", "Enable", value));
    CHECK(value);
    auto result = frame.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::backend_failure);
    CHECK(value);
    service->fail_publish = false;
    auto recovered = begin(*context);
    CHECK_FALSE(recovered.checkbox("check", "Enable", value));
    auto packet = finish(recovered);
    CHECK(semantic(packet, wui::Role::checkbox, "Enable").selected);
}

TEST_CASE("a text shape error wins over publication errors and prevents later mutation") {
    auto service = std::make_shared<FailingText>();
    auto desc = fixture_desc();
    desc.text = service;
    auto context = make_context(std::move(desc));
    service->fail_shape = true;
    service->fail_publish = true;
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 10}},
                    {wui::EventKind::pointer_up, {30, 10}}};
    auto frame = begin(*context, input);
    frame.text("Cannot shape");
    service->fail_shape = false;
    bool value = false;
    CHECK_FALSE(frame.checkbox("later", "Must not mutate", value));
    CHECK_FALSE(value);
    auto result = frame.finish();
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_font);
}

TEST_CASE("context creation rejects absent services instead of substituting defaults") {
    auto desc = fixture_desc();
    SUBCASE("text service is required") { desc.text.reset(); }
    SUBCASE("texture store is required") { desc.textures.reset(); }
    auto result = wui::Context::create(std::move(desc));
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
}

TEST_CASE("sibling panels scope identical button keys and return only the activated button") {
    auto context = make_context();
    auto measure = begin(*context);
    {
        auto panel = measure.panel("first");
        CHECK_FALSE(measure.button("action", "First action"));
    }
    {
        auto panel = measure.panel("second");
        CHECK_FALSE(measure.button("action", "Second action"));
    }
    auto before = finish(measure);
    const auto& first = semantic(before, wui::Role::button, "First action");
    const auto& second = semantic(before, wui::Role::button, "Second action");
    CHECK(first.id != second.id);
    const wui::Vec2 point{second.bounds.x + 8, second.bounds.y + 8};
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, point},
                    {wui::EventKind::pointer_up, point}};
    auto clicked = begin(*context, input);
    {
        auto panel = clicked.panel("first");
        CHECK_FALSE(clicked.button("action", "First action"));
    }
    {
        auto panel = clicked.panel("second");
        CHECK(clicked.button("action", "Renamed second action"));
    }
    auto after = finish(clicked);
    CHECK(semantic(after, wui::Role::button, "Renamed second action").id == second.id);
}

TEST_CASE("capture updates the current model reference rather than a prior caller variable") {
    auto context = make_context();
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 10}}};
    bool original = false;
    auto down = begin(*context, input);
    CHECK_FALSE(down.checkbox("check", "Enable", original));
    finish(down);
    bool replacement = false;
    input.events = {{wui::EventKind::pointer_up, {30, 10}}};
    auto up = begin(*context, input);
    CHECK(up.checkbox("check", "Enable", replacement));
    CHECK(replacement);
    CHECK_FALSE(original);
    auto packet = finish(up);
    CHECK(semantic(packet, wui::Role::checkbox, "Enable").selected);
}

TEST_CASE("descriptions measure wrapped text using each resolved cell width") {
    auto context = make_context();
    auto frame = begin(*context);
    const std::array columns{wui::Column{wui::ColumnKind::fixed, 90},
                             wui::Column{wui::ColumnKind::fixed, 240}};
    {
        auto row = frame.row("descriptions", columns);
        { auto cell = row.cell();
          frame.description("Words break naturally across cells", {.color = wui::Color{1, 0, 0, 1}}); }
        { auto cell = row.cell();
          frame.description("Words break naturally across cells"); }
    }
    const auto packet = finish(frame);
    REQUIRE(packet->semantics.size() == 2);
    CHECK(packet->semantics[0].bounds.x == 0);
    CHECK(packet->semantics[1].bounds.x == 98);
    CHECK(packet->semantics[0].bounds.h > packet->semantics[1].bounds.h);
    CHECK(packet->semantics[0].label == "Words break naturally across cells");
}

TEST_CASE("custom item owns its label, shares button activation and permits clipped paint") {
    auto context = make_context();
    wui::FrameInput input{{0, 0, 200, 100}};
    std::string label = "Custom action";
    {
        auto frame = begin(*context, input);
        auto created = frame.custom("action", label, 40);
        REQUIRE(std::holds_alternative<wui::CustomItem>(created));
        const auto item = std::get<wui::CustomItem>(created);
        CHECK_FALSE(item.behavior.activated);
        frame.canvas().push_clip(item.clip);
        frame.canvas().gradient(item.bounds, {1, 0, 0, 1}, {0, 0, 1, 1});
        frame.canvas().border(item.bounds, 4, 1, {1, 1, 1, 1});
        frame.canvas().pop_clip();
        label.assign("Changed caller storage");
        auto packet = finish(frame);
        REQUIRE_FALSE(wui::validate_packet(*packet));
        REQUIRE(packet->semantics.size() == 1);
        CHECK(packet->semantics.front().label == "Custom action");
        CHECK(packet->semantics.front().id == item.id);
    }
    input.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    auto frame = begin(*context, input);
    auto created = frame.custom("action", "Updated action", 40);
    REQUIRE(std::holds_alternative<wui::CustomItem>(created));
    CHECK(std::get<wui::CustomItem>(created).behavior.activated);
    auto packet = finish(frame);
    CHECK(packet->semantics.front().label == "Updated action");
}

TEST_CASE("nested styles resolve scopes before variant before widget overrides") {
    auto desc = fixture_desc();
    desc.theme.reduced_motion = true;
    auto context = make_context(std::move(desc));
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_move, {20, 100}}};
    auto frame = begin(*context, input);
    bool checked = true;
    const wui::Color outer{0.8f, 0.2f, 0.2f, 1};
    const wui::Color inner{0.2f, 0.8f, 0.2f, 1};
    const wui::Color final{0.9f, 0.7f, 0.1f, 1};
    {
        auto scope = frame.style({.accent = outer});
        frame.button("outer", "Outer", {.variant = wui::Variant::prominent});
        {
            auto nested = frame.style({.accent = inner});
            frame.button("inner", "Inner", {.variant = wui::Variant::prominent});
            frame.button("override", "Override", {.variant = wui::Variant::prominent,
                .style = {.accent = final}});
            frame.checkbox("persist", "Persist", checked);
        }
        frame.button("after-inner", "After inner", {.variant = wui::Variant::prominent});
    }
    frame.button("default", "Default");
    const auto packet = finish(frame);
    CHECK(painted(packet, semantic(packet, wui::Role::button, "Outer").bounds, outer));
    CHECK(painted(packet, semantic(packet, wui::Role::button, "Inner").bounds, inner));
    CHECK(painted(packet, semantic(packet, wui::Role::button, "Override").bounds,
                  {inner.r + (final.r - inner.r) * 0.16f,
                   inner.g + (final.g - inner.g) * 0.16f,
                   inner.b + (final.b - inner.b) * 0.16f, 1}));
    CHECK(painted(packet, semantic(packet, wui::Role::button, "After inner").bounds, outer));
    CHECK(painted(packet, semantic(packet, wui::Role::button, "Default").bounds,
                  wui::Theme::dark().control));
    const auto id = semantic(packet, wui::Role::checkbox, "Persist").id;
    auto next = begin(*context);
    next.checkbox("persist", "Persist", checked);
    const auto restored = finish(next);
    CHECK(checked);
    CHECK(semantic(restored, wui::Role::checkbox, "Persist").id == id);
}

TEST_CASE("button and checkbox states paint distinct hovered held focused disabled selected colors") {
    auto desc = fixture_desc();
    desc.theme.reduced_motion = true;
    auto context = make_context(desc);
    const auto tokens = wui::Theme::dark();
    auto emit = [&](wui::FrameInput input, bool enabled = true) {
        auto frame = begin(*context, input);
        const auto activated = frame.button("action", "Action", {.enabled = enabled});
        auto packet = finish(frame);
        return std::pair{activated, packet};
    };
    wui::FrameInput input{{0, 0, 400, 300}};
    auto [normal_click, normal] = emit(input);
    const auto bounds = semantic(normal, wui::Role::button, "Action").bounds;
    CHECK_FALSE(normal_click);
    CHECK(painted(normal, bounds, tokens.control));
    normal.reset();
    input.events = {{wui::EventKind::pointer_move, {20, 20}}};
    auto [hover_click, hover] = emit(input);
    CHECK_FALSE(hover_click);
    CHECK(painted(hover, bounds, {tokens.control.r + (tokens.accent.r - tokens.control.r) * 0.16f,
                                  tokens.control.g + (tokens.accent.g - tokens.control.g) * 0.16f,
                                  tokens.control.b + (tokens.accent.b - tokens.control.b) * 0.16f, 1}));
    hover.reset();
    input.events = {{wui::EventKind::pointer_down, {20, 20}}};
    auto [down_click, down] = emit(input);
    CHECK_FALSE(down_click);
    CHECK(semantic(down, wui::Role::button, "Action").focused);
    CHECK(semantic(down, wui::Role::button, "Action").bounds.h == bounds.h);
    CHECK(painted(down, bounds, tokens.focus));
    CHECK(painted(down, bounds, {tokens.control.r + (tokens.accent.r - tokens.control.r) * 0.28f,
                                 tokens.control.g + (tokens.accent.g - tokens.control.g) * 0.28f,
                                 tokens.control.b + (tokens.accent.b - tokens.control.b) * 0.28f, 1}));
    down.reset();
    input.events = {{wui::EventKind::pointer_up, {20, 20}}};
    auto [up_click, up] = emit(input);
    CHECK(up_click);
    CHECK(semantic(up, wui::Role::button, "Action").focused);
    up.reset();
    input.events.clear();
    auto [disabled_click, disabled] = emit(input, false);
    CHECK_FALSE(disabled_click);
    CHECK_FALSE(semantic(disabled, wui::Role::button, "Action").focused);
    CHECK(painted(disabled, bounds, {(tokens.panel.r + tokens.control.r) * 0.5f,
                                     (tokens.panel.g + tokens.control.g) * 0.5f,
                                     (tokens.panel.b + tokens.control.b) * 0.5f, 1}));
    disabled.reset();
    bool selected = true;
    auto frame = begin(*context);
    frame.checkbox("selected", "Selected", selected);
    auto selected_packet = finish(frame);
    CHECK(selected);
    CHECK(semantic(selected_packet, wui::Role::checkbox, "Selected").selected);
    CHECK(painted(selected_packet, semantic(selected_packet, wui::Role::checkbox, "Selected").bounds,
                  tokens.accent));
}

TEST_CASE("reduced motion snaps widget hover without changing its hit rectangle") {
    auto context = make_context();
    const auto tokens = wui::Theme::dark();
    wui::FrameInput input{{0, 0, 400, 300}};
    input.dt_seconds = 0.05;
    input.events = {{wui::EventKind::pointer_move, {20, 20}}};
    auto before = begin(*context, input);
    before.button("hover", "Hover");
    auto animating = finish(before);
    const auto bounds = semantic(animating, wui::Role::button, "Hover").bounds;
    CHECK(painted(animating, bounds,
                  {tokens.control.r + (tokens.accent.r - tokens.control.r) * 0.08f,
                   tokens.control.g + (tokens.accent.g - tokens.control.g) * 0.08f,
                   tokens.control.b + (tokens.accent.b - tokens.control.b) * 0.08f, 1}));
    animating.reset();
    auto instant = tokens;
    instant.reduced_motion = true;
    REQUIRE_FALSE(context->set_theme(instant).has_value());
    auto after = begin(*context, input);
    after.button("hover", "Hover");
    const auto snapped = finish(after);
    CHECK(semantic(snapped, wui::Role::button, "Hover").bounds.x == bounds.x);
    CHECK(semantic(snapped, wui::Role::button, "Hover").bounds.h == bounds.h);
    CHECK(painted(snapped, bounds,
                  {tokens.control.r + (tokens.accent.r - tokens.control.r) * 0.16f,
                   tokens.control.g + (tokens.accent.g - tokens.control.g) * 0.16f,
                   tokens.control.b + (tokens.accent.b - tokens.control.b) * 0.16f, 1}));
}

TEST_CASE("icon button leases its texture and activates with an accessible label") {
    auto desc = fixture_desc();
    const auto textures = desc.textures;
    constexpr std::array<std::byte, 16> rgba{
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    const auto created = textures->create(2, 2, rgba, wui::AlphaMode::straight);
    REQUIRE(std::holds_alternative<wui::TextureHandle>(created));
    const auto icon = std::get<wui::TextureHandle>(created);
    auto context = make_context(std::move(desc));
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {20, 20}},
                    {wui::EventKind::pointer_up, {20, 20}}};
    auto frame = begin(*context, input);
    CHECK(frame.icon_button("help", "Help", icon));
    const auto packet = finish(frame);
    CHECK(semantic(packet, wui::Role::button, "Help").bounds.h == 36);
    CHECK(std::any_of(packet->textures.begin(), packet->textures.end(),
        [icon](const auto& texture) { return texture->handle == icon; }));
}

TEST_CASE("tab selection tracks stable values across keyboard activation and reorder") {
    auto context = make_context();
    std::array<wui::Choice<int>, 2> choices{{{10, "First"}, {20, "Second"}}};
    int selected = 10;
    auto initial = begin(*context);
    CHECK_FALSE(initial.tabs("section", selected, choices));
    auto before = finish(initial);
    const auto first_id = semantic(before, wui::Role::tab, "Second").id;
    before.reset();

    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab),
                    key_event(wui::EventKind::key_down, wui::Key::tab),
                    key_event(wui::EventKind::key_down, wui::Key::enter),
                    key_event(wui::EventKind::key_up, wui::Key::enter)};
    auto activated = begin(*context, input);
    CHECK(activated.tabs("section", selected, choices));
    auto chosen = finish(activated);
    CHECK(selected == 20);
    CHECK(semantic(chosen, wui::Role::tab, "Second").selected);
    CHECK(semantic(chosen, wui::Role::tab, "Second").value == "Second");
    chosen.reset();

    std::swap(choices[0], choices[1]);
    input.events.clear();
    auto reordered = begin(*context, input);
    CHECK_FALSE(reordered.tabs("section", selected, choices));
    const auto after = finish(reordered);
    CHECK(selected == 20);
    CHECK(semantic(after, wui::Role::tab, "Second").id == first_id);
    CHECK(context->diagnostics().empty());
}

TEST_CASE("float slider drags beyond its bounds and releases outside without losing focus") {
    auto context = make_context();
    float value = 0.25f;
    auto initial = begin(*context);
    CHECK_FALSE(initial.slider("scale", "Scale", value, 0.0f, 1.0f));
    auto first = finish(initial);
    const auto bounds = semantic(first, wui::Role::slider, "Scale").bounds;
    const auto id = semantic(first, wui::Role::slider, "Scale").id;
    first.reset();
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down,
                     {bounds.x + bounds.w * 0.25f, bounds.y + bounds.h * 0.5f}}};
    auto pressed = begin(*context, input);
    pressed.slider("scale", "Scale", value, 0.0f, 1.0f);
    auto holding = finish(pressed);
    CHECK(semantic(holding, wui::Role::slider, "Scale").selected);
    holding.reset();
    input.events = {{wui::EventKind::pointer_move,
                     {bounds.x + bounds.w * 1.5f, bounds.y + bounds.h * 0.5f}}};
    auto moved = begin(*context, input);
    CHECK(moved.slider("scale", "Scale", value, 0.0f, 1.0f));
    const auto outside = finish(moved);
    CHECK(value == doctest::Approx(1.0f));
    CHECK(semantic(outside, wui::Role::slider, "Scale").selected);
    CHECK(semantic(outside, wui::Role::slider, "Scale").focused);
    CHECK(semantic(outside, wui::Role::slider, "Scale").id == id);
    input.events = {{wui::EventKind::pointer_up,
                     {bounds.x + bounds.w * 1.5f, bounds.y + bounds.h * 0.5f}}};
    auto released = begin(*context, input);
    CHECK_FALSE(released.slider("scale", "Scale", value, 0.0f, 1.0f));
    const auto after = finish(released);
    CHECK_FALSE(semantic(after, wui::Role::slider, "Scale").selected);
    CHECK(value == doctest::Approx(1.0f));
}

TEST_CASE("integer slider routes ordered keyboard steps and clamps extreme bounds") {
    auto context = make_context();
    int value = 5;
    auto initial = begin(*context);
    initial.slider("count", "Count", value, 0, 10, {.invalid = true});
    auto first = finish(initial);
    const auto id = semantic(first, wui::Role::slider, "Count").id;
    first.reset();
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab),
                    key_event(wui::EventKind::key_down, wui::Key::right)};
    auto step = begin(*context, input);
    CHECK(step.slider("count", "Count", value, 0, 10, {.invalid = true}));
    auto focused = finish(step);
    const auto& semantic = wui::test::semantic(focused, wui::Role::slider, "Count");
    CHECK(value == 6);
    CHECK(semantic.value == "6");
    CHECK(semantic.invalid);
    CHECK(semantic.focused);
    CHECK(semantic.id == id);
    CHECK(painted(focused, semantic.bounds, wui::Theme::dark().focus));
    CHECK(painted(focused, semantic.bounds, {1.0f, 0.42f, 0.43f, 1.0f}));
    focused.reset();
    input.events = {key_event(wui::EventKind::key_down, wui::Key::home),
                    key_event(wui::EventKind::key_down, wui::Key::end),
                    key_event(wui::EventKind::key_down, wui::Key::left)};
    auto ordered = begin(*context, input);
    CHECK(ordered.slider("count", "Count", value, 0, 10, {.invalid = true}));
    auto last = finish(ordered);
    CHECK(value == 9);
    CHECK(wui::test::semantic(last, wui::Role::slider, "Count").value == "9");
    last.reset();
    int extreme = 0;
    auto huge = begin(*context);
    huge.slider("extreme", "Extreme", extreme, std::numeric_limits<int>::min(),
                std::numeric_limits<int>::max());
    const auto huge_bounds = wui::test::semantic(finish(huge), wui::Role::slider, "Extreme").bounds;
    input.events = {{wui::EventKind::pointer_down,
                     {huge_bounds.x + huge_bounds.w * 0.5f, huge_bounds.y + 8}}};
    auto pressed = begin(*context, input);
    pressed.slider("extreme", "Extreme", extreme, std::numeric_limits<int>::min(),
                   std::numeric_limits<int>::max());
    finish(pressed);
    input.events = {{wui::EventKind::pointer_move,
                     {huge_bounds.x + huge_bounds.w * 1.5f, huge_bounds.y + 8}}};
    auto high = begin(*context, input);
    CHECK(high.slider("extreme", "Extreme", extreme, std::numeric_limits<int>::min(),
                      std::numeric_limits<int>::max()));
    finish(high);
    CHECK(extreme == std::numeric_limits<int>::max());
}

TEST_CASE("disabled removed or focus-lost sliders discard capture before later release") {
    for (int cause = 0; cause < 3; ++cause) {
        auto context = make_context();
        float value = 0.5f;
        auto initial = begin(*context);
        initial.slider("scale", "Scale", value, 0.0f, 1.0f);
        const auto bounds = semantic(finish(initial), wui::Role::slider, "Scale").bounds;
        wui::FrameInput input{{0, 0, 400, 300}};
        input.events = {{wui::EventKind::pointer_down,
                         {bounds.x + bounds.w * 0.5f, bounds.y + 8}}};
        auto pressed = begin(*context, input);
        pressed.slider("scale", "Scale", value, 0.0f, 1.0f);
        finish(pressed);
        input.events = cause == 2 ? std::vector<wui::InputEvent>{{wui::EventKind::focus_lost}}
                                  : std::vector<wui::InputEvent>{};
        auto cancelled = begin(*context, input);
        if (cause != 1)
            CHECK_FALSE(cancelled.slider("scale", "Scale", value, 0.0f, 1.0f,
                                         {.enabled = cause != 0}));
        finish(cancelled);
        const float before_release = value;
        input.events = {{wui::EventKind::pointer_up,
                         {bounds.x + bounds.w * 1.5f, bounds.y + 8}}};
        auto released = begin(*context, input);
        CHECK_FALSE(released.slider("scale", "Scale", value, 0.0f, 1.0f));
        const auto packet = finish(released);
        CHECK(value == before_release);
        CHECK_FALSE(semantic(packet, wui::Role::slider, "Scale").selected);
        CHECK_FALSE(semantic(packet, wui::Role::slider, "Scale").focused);
    }
}

namespace {
wui::Vec2 center(const wui::Rect& bounds) {
    return {bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f};
}
std::vector<wui::InputEvent> click(const wui::Rect& bounds) {
    return {{wui::EventKind::pointer_down, center(bounds)},
            {wui::EventKind::pointer_up, center(bounds)}};
}
bool has_semantic(const wui::DrawPacket& packet, wui::Role role, std::string_view label) {
    return std::any_of(packet->semantics.begin(), packet->semantics.end(),
        [&](const wui::Semantic& item) { return item.role == role && item.label == label; });
}
} // namespace

TEST_CASE("complete gallery exposes every control across three stable sections") {
    auto desc = fixture_desc();
    auto store = desc.textures;
    auto context = make_context(std::move(desc));
    GalleryState state;
    prepare_gallery_model(state);
    REQUIRE(state.row_ids.size() == 10'000);
    CHECK(state.row_ids[1] != state.row_ids[0]);
    auto icon = make_gallery_icon(*store);
    REQUIRE(std::holds_alternative<wui::TextureHandle>(icon));
    state.icon = std::get<wui::TextureHandle>(icon);
    CHECK(state.section == 1);
    state.viewport_height = 600;

    std::set<wui::Role> roles;
    std::map<std::pair<wui::Role, std::string>, std::uint64_t> ids;
    const auto frame_with = [&](wui::FrameInput input) {
        auto frame = begin(*context, input);
        build_gallery(frame, state);
        auto packet = finish(frame);
        for (const auto& item : packet->semantics) {
            roles.insert(item.role);
            // Text and validation IDs are positional; keyed controls must stay stable.
            if (item.role == wui::Role::text || item.role == wui::Role::validation ||
                item.role == wui::Role::tooltip) continue;
            const auto [found, inserted] = ids.try_emplace({item.role, item.label}, item.id.value);
            if (!inserted) CHECK_MESSAGE(found->second == item.id.value, item.label);
        }
        return packet;
    };
    const wui::Rect viewport{0, 0, 800, 600};
    const auto run = [&](std::vector<wui::InputEvent> events = {}, double dt = 1.0 / 60.0) {
        return frame_with(wui::FrameInput{viewport, 1.0f, dt, std::move(events)});
    };

    // A context leases at most four packets, so keep bounds rather than packets.
    auto startup = run();
    CHECK(semantic(startup, wui::Role::checkbox, "Notifications").selected);
    CHECK(has_semantic(startup, wui::Role::button, "Restore defaults"));
    const auto restore = semantic(startup, wui::Role::button, "Restore defaults").bounds;
    const auto appearance_tab = semantic(startup, wui::Role::tab, "Appearance").bounds;
    startup.reset();
    for (auto role : {wui::Role::panel, wui::Role::text, wui::Role::tab, wui::Role::radio,
                      wui::Role::dropdown, wui::Role::slider, wui::Role::text_input,
                      wui::Role::numeric_input})
        CHECK_MESSAGE(roles.count(role) == 1, static_cast<int>(role));

    // The B probes render the same source at 256x256; the original controls stay visible.
    auto probe = frame_with(wui::FrameInput{{0, 0, 256, 256}});
    for (auto [role, label] : {std::pair{wui::Role::checkbox, "Notifications"},
                               std::pair{wui::Role::button, "Restore defaults"}}) {
        const auto bounds = semantic(probe, role, label).bounds;
        CHECK(bounds.y >= 0);
        CHECK(bounds.y + bounds.h <= 256);
        CHECK(bounds.x + bounds.w <= 256);
    }
    probe.reset();

    const wui::InputEvent hover{wui::EventKind::pointer_move, center(restore)};
    run({hover}, 0.25);
    auto hovered = run({}, 0.25);
    CHECK(roles.count(wui::Role::tooltip) == 1);
    CHECK(std::any_of(hovered->semantics.begin(), hovered->semantics.end(),
        [](const wui::Semantic& item) { return item.role == wui::Role::tooltip; }));
    hovered.reset();

    state.name.clear();
    state.quantity = 9;
    auto invalid = run();
    CHECK(semantic(invalid, wui::Role::text_input, "Name").invalid);
    CHECK(roles.count(wui::Role::validation) == 1);
    invalid.reset();
    state.name = "İstanbul";
    state.quantity = 3;
    CHECK_FALSE(has_semantic(run(), wui::Role::validation, "Name is required"));

    state.notifications = false;
    run(click(appearance_tab));
    CHECK(state.section == 0);
    auto shown = run();
    CHECK_FALSE(has_semantic(shown, wui::Role::checkbox, "Notifications"));
    CHECK(semantic(shown, wui::Role::tab, "Appearance").selected);
    CHECK(has_semantic(shown, wui::Role::slider, "UI scale"));
    CHECK(has_semantic(shown, wui::Role::button, "Add item"));
    CHECK_FALSE(state.notifications);

    const auto diagnostics_tab = semantic(shown, wui::Role::tab, "Diagnostics").bounds;
    shown.reset();
    run(click(diagnostics_tab));
    CHECK(state.section == 2);
    auto diagnostics = run();
    std::size_t rows = 0;
    for (const auto& item : diagnostics->semantics)
        if (item.role == wui::Role::button && item.label.starts_with("Row ")) ++rows;
    CHECK(rows > 0);
    CHECK(rows < 100);
    CHECK(has_semantic(diagnostics, wui::Role::button, "Row 1"));
    CHECK_FALSE(has_semantic(diagnostics, wui::Role::button, "Row 10000"));

    const auto controls_tab = semantic(diagnostics, wui::Role::tab, "Controls").bounds;
    diagnostics.reset();
    run(click(controls_tab));
    CHECK(state.section == 1);
    auto back = run();
    CHECK_FALSE(semantic(back, wui::Role::checkbox, "Notifications").selected);
    CHECK(state.notifications == false);
    const auto restore_again = semantic(back, wui::Role::button, "Restore defaults").bounds;
    back.reset();
    run(click(restore_again));
    CHECK(state.notifications);
    CHECK(state.restore_count == 1);
    CHECK(semantic(run(), wui::Role::checkbox, "Notifications").selected);
    for (auto role : {wui::Role::panel, wui::Role::text, wui::Role::button, wui::Role::checkbox,
                      wui::Role::tab, wui::Role::radio, wui::Role::dropdown, wui::Role::slider,
                      wui::Role::text_input, wui::Role::numeric_input, wui::Role::validation,
                      wui::Role::tooltip})
        CHECK_MESSAGE(roles.count(role) == 1, static_cast<int>(role));
}

TEST_CASE("stress workloads emit exactly the visible controls with persistent keys") {
    auto desc = fixture_desc();
    auto store = desc.textures;
    auto context = make_context(std::move(desc));
    GalleryState state;
    prepare_gallery_model(state);
    auto icon = make_gallery_icon(*store);
    REQUIRE(std::holds_alternative<wui::TextureHandle>(icon));
    state.icon = std::get<wui::TextureHandle>(icon);
    auto theme = stress_theme(state);
    REQUIRE(std::holds_alternative<wui::Theme>(theme));
    CHECK(std::get<wui::Theme>(theme).gap == 0);
    CHECK(std::get<wui::Theme>(theme).control_height == doctest::Approx(28));
    REQUIRE_FALSE(context->set_theme(std::get<wui::Theme>(theme)).has_value());

    const wui::Rect viewport{0, 0, 1280, 720};
    state.viewport_height = viewport.h;
    std::size_t peak = 0;
    const auto run = [&](StressWorkload workload, std::vector<wui::InputEvent> events = {}) {
        auto frame = begin(*context, wui::FrameInput{viewport, 1.0f, 1.0 / 60.0, std::move(events)});
        build_stress_gallery(frame, state, workload);
        auto packet = finish(frame);
        const auto stats = context->statistics();
        CHECK(stats.retained_bytes <= stats.peak_retained_bytes);
        CHECK(stats.peak_retained_bytes <= wui::ContextDesc{}.max_retained_bytes);
        CHECK(stats.peak_retained_bytes >= peak);
        peak = stats.peak_retained_bytes;
        return packet;
    };
    const auto visible_buttons = [&](const wui::DrawPacket& packet) {
        std::set<std::string> labels;
        std::size_t count = 0;
        for (const auto& s : packet->semantics) {
            if (s.role != wui::Role::button) continue;
            ++count;
            labels.insert(s.label);
            CHECK(s.bounds.w > 0);
            CHECK(s.bounds.h > 0);
            CHECK(s.bounds.x >= viewport.x - 0.5f);
            CHECK(s.bounds.y >= viewport.y - 0.5f);
            CHECK(s.bounds.x + s.bounds.w <= viewport.w + 0.5f);
            CHECK(s.bounds.y + s.bounds.h <= viewport.h + 0.5f);
        }
        CHECK(labels.size() == count);
        return count;
    };

    CHECK(visible_buttons(run(StressWorkload::controls_100)) == 100);
    {
        auto packet = run(StressWorkload::controls_1000);
        const auto buttons = std::count_if(packet->semantics.begin(), packet->semantics.end(),
            [](const wui::Semantic& s) { return s.role == wui::Role::button; });
        CHECK(buttons == 1'000);
        CHECK(visible_buttons(packet) == 1'000);
        const auto& first = semantic(packet, wui::Role::button, "Item 1");
        CHECK(first.bounds.w == doctest::Approx(32));
        CHECK(first.bounds.h == doctest::Approx(28));
        CHECK(has_semantic(packet, wui::Role::button, "Item 1000"));
    }

    std::map<std::string, std::uint64_t> row_ids;
    const auto rows_in = [&](const wui::DrawPacket& packet, float offset) {
        auto range = wui::visible_rows(10'000, 28, offset, 720);
        REQUIRE(std::holds_alternative<wui::VisibleRows>(range));
        const auto [first, last] = std::get<wui::VisibleRows>(range);
        std::size_t rows = 0;
        for (const auto& s : packet->semantics) {
            if (s.role != wui::Role::button) continue;
            ++rows;
            const auto [found, inserted] = row_ids.try_emplace(s.label, s.id.value);
            if (!inserted) CHECK_MESSAGE(found->second == s.id.value, s.label);
        }
        CHECK(rows == last - first);
        CHECK(rows < 100);
        return rows;
    };
    CHECK(rows_in(run(StressWorkload::virtual_10000), 0) > 0);
    wui::InputEvent wheel{wui::EventKind::wheel, {640, 360}};
    wheel.wheel = -1;
    run(StressWorkload::virtual_10000, {wheel});
    auto scrolled = run(StressWorkload::virtual_10000);
    // The first emitted row and its painted top recover the scroll offset.
    const auto first_row = std::find_if(scrolled->semantics.begin(), scrolled->semantics.end(),
        [](const wui::Semantic& s) { return s.role == wui::Role::button; });
    REQUIRE(first_row != scrolled->semantics.end());
    const auto index = std::stoul(std::string(first_row->label.substr(4)));
    const float offset = static_cast<float>(index - 1) * 28.0f - first_row->bounds.y;
    CHECK(offset > 0);
    CHECK(rows_in(scrolled, offset) > 0);
    CHECK_FALSE(has_semantic(scrolled, wui::Role::button, "Row 10000"));
}
