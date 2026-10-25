#include <doctest/doctest.h>

#include <wui/text.hpp>
#include <wui/wui.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <variant>
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

std::shared_ptr<wui::IText> real_text(std::shared_ptr<wui::TextureStore> textures) {
    const auto regular = load_bytes(std::filesystem::path(WUI_FONT_DIR) / "Inter-Regular.ttf");
    const auto semibold = load_bytes(std::filesystem::path(WUI_FONT_DIR) / "Inter-SemiBold.ttf");
    auto result = wui::make_text_service(regular, semibold, std::move(textures));
    REQUIRE(std::holds_alternative<std::shared_ptr<wui::IText>>(result));
    return std::get<std::shared_ptr<wui::IText>>(std::move(result));
}

std::unique_ptr<wui::Context> make_context(std::shared_ptr<wui::TextureStore> textures,
                                           std::shared_ptr<wui::IText> text,
                                           std::shared_ptr<wui::IClipboard> clipboard = {}) {
    wui::ContextDesc desc{wui::Theme::dark(), std::move(textures), std::move(text)};
    desc.clipboard = std::move(clipboard);
    auto result = wui::Context::create(std::move(desc));
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::Context>>(result));
    return std::get<std::unique_ptr<wui::Context>>(std::move(result));
}
class FakeClipboard final : public wui::IClipboard {
public:
    wui::Result<std::string> clipboard() const override {
        if (fail_read) return wui::Error{wui::ErrorCode::backend_failure, "clipboard read failed"};
        return contents;
    }
    wui::Status set_clipboard(std::string_view value) override {
        if (fail_write) return wui::Error{wui::ErrorCode::backend_failure, "clipboard write failed"};
        contents.assign(value.data(), value.size());
        return std::nullopt;
    }
    std::string contents;
    bool fail_read = false, fail_write = false;
};

wui::Frame begin(wui::Context& context, const wui::FrameInput& input = wui::FrameInput{{0, 0, 400, 300}}) {
    auto result = context.begin_frame(input);
    REQUIRE(std::holds_alternative<wui::Frame>(result));
    return std::get<wui::Frame>(std::move(result));
}

wui::DrawPacket finish(wui::Frame& frame) {
    auto result = frame.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
    auto packet = std::get<wui::DrawPacket>(std::move(result));
    REQUIRE(packet);
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    return packet;
}

wui::InputEvent key_event(wui::EventKind kind, wui::Key key, bool shift = false, bool control = false) {
    wui::InputEvent event{kind};
    event.key = key;
    event.shift = shift;
    event.control = control;
    return event;
}

wui::InputEvent text_event(std::string text) {
    wui::InputEvent event{wui::EventKind::text};
    event.text = std::move(text);
    return event;
}

wui::InputEvent composition_event(std::string text) {
    wui::InputEvent event{wui::EventKind::composition};
    event.text = std::move(text);
    return event;
}

const wui::Semantic& semantic(const wui::DrawPacket& packet, wui::Role role, std::string_view label) {
    const auto found = std::find_if(packet->semantics.begin(), packet->semantics.end(),
        [=](const wui::Semantic& item) { return item.role == role && item.label == label; });
    REQUIRE(found != packet->semantics.end());
    return *found;
}

} // namespace
TEST_CASE("real original-byte grapheme boundaries include malformed trailing bytes") {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    const auto service = real_text(textures);
    for (const auto source : {std::string("İst"), std::string("e\xCC\x81"),
                              std::string("A") + char(0xff) + "B"}) {
        const auto result = service->grapheme_offsets(source);
        REQUIRE(std::holds_alternative<std::vector<std::size_t>>(result));
        const auto offsets = std::get<std::vector<std::size_t>>(result);
        REQUIRE(offsets.size() >= 2);
        CHECK(offsets.front() == 0);
        CHECK(offsets.back() == source.size());
        for (const auto offset : offsets) CHECK(offset <= source.size());
    }
    const auto combined = service->grapheme_offsets("e\xCC\x81x");
    REQUIRE(std::holds_alternative<std::vector<std::size_t>>(combined));
    CHECK(std::get<std::vector<std::size_t>>(combined) == std::vector<std::size_t>{0, 3, 4});
}

TEST_CASE("single-line text input edits one combining grapheme and survives relabel") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto context = make_context(textures, real_text(textures));
    std::string value = "e";
    wui::FrameInput input{{0, 0, 400, 300}};
    auto unfocused = begin(*context, input);
    CHECK_FALSE(unfocused.text_input("name", "Name", value));
    finish(unfocused);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto focus = begin(*context, input);
    CHECK_FALSE(focus.text_input("name", "Name", value));
    auto focused = finish(focus);
    CHECK(semantic(focused, wui::Role::text_input, "Name").focused);
    focused.reset();

    input.events = {text_event("İstanbul ")};
    auto inserted = begin(*context, input);
    CHECK(inserted.text_input("name", "Name", value));
    finish(inserted);
    CHECK(value == "eİstanbul ");

    input.events = {key_event(wui::EventKind::key_down, wui::Key::end),
                    text_event(std::string("x") + "e\xCC\x81")};
    auto appended = begin(*context, input);
    CHECK(appended.text_input("name", "Name", value));
    finish(appended);
    CHECK(value == std::string("eİstanbul ") + "xe\xCC\x81");
    input.events = {key_event(wui::EventKind::key_down, wui::Key::left, true),
                    key_event(wui::EventKind::key_down, wui::Key::backspace)};
    auto combining = begin(*context, input);
    CHECK(combining.text_input("name", "Name", value));
    auto packet = finish(combining);
    CHECK(value == "eİstanbul x");
    CHECK(semantic(packet, wui::Role::text_input, "Name").value == "eİstanbul x");
    packet.reset();

    input.events = {text_event(std::string("e\xCC\x81")),
                    key_event(wui::EventKind::key_down, wui::Key::left, true),
                    key_event(wui::EventKind::key_down, wui::Key::backspace)};
    auto selected_grapheme = begin(*context, input);
    CHECK(selected_grapheme.text_input("name", "Name", value));
    auto selected_packet = finish(selected_grapheme);
    CHECK(value == "eİstanbul x");
    CHECK(selected_packet->semantics.back().value == "eİstanbul x");
    selected_packet.reset();

    input.events = {key_event(wui::EventKind::key_down, wui::Key::home),
                    key_event(wui::EventKind::key_down, wui::Key::end, true),
                    key_event(wui::EventKind::key_down, wui::Key::a, false, true)};
    auto selected = begin(*context, input);
    CHECK_FALSE(selected.text_input("name", "Name", value));
    finish(selected);

    input.events = {key_event(wui::EventKind::key_down, wui::Key::z, false, true)};
    auto undone = begin(*context, input);
    finish(undone);
    CHECK(value == "eİstanbul x");

    input.events.clear();
    auto relabeled = begin(*context, input);
    CHECK_FALSE(relabeled.text_input("name", "Renamed", value));
    const auto retained = finish(relabeled);
    CHECK(value == "eİstanbul x");
    CHECK(semantic(retained, wui::Role::text_input, "Renamed").value == "eİstanbul x");
}

TEST_CASE("text editors keep independent undo and honor fresh application models") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto context = make_context(textures, real_text(textures));
    std::string first = "one", second = "two";
    wui::FrameInput first_input{{0, 0, 400, 300}};
    auto unfocused = begin(*context, first_input);
    CHECK_FALSE(unfocused.text_input("first", "First", first));
    CHECK_FALSE(unfocused.text_input("second", "Second", second));
    finish(unfocused);
    first_input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto focused = begin(*context, first_input);
    CHECK_FALSE(focused.text_input("first", "First", first));
    CHECK_FALSE(focused.text_input("second", "Second", second));
    finish(focused);

    wui::FrameInput edit{{0, 0, 400, 300}};
    edit.events = {text_event("1"), text_event("2")};
    auto changed = begin(*context, edit);
    CHECK(changed.text_input("first", "First", first));
    CHECK_FALSE(changed.text_input("second", "Second", second));
    finish(changed);
    CHECK(first == "one12");
    CHECK(second == "two");

    edit.events = {key_event(wui::EventKind::key_down, wui::Key::z, false, true)};
    auto undone = begin(*context, edit);
    CHECK(undone.text_input("first", "First", first));
    CHECK_FALSE(undone.text_input("second", "Second", second));
    finish(undone);
    CHECK(first == "one1");
    CHECK(second == "two");

    first.assign("application replaced the focused draft while it was focused");
    edit.events.clear();
    auto replaced = begin(*context, edit);
    CHECK_FALSE(replaced.text_input("first", "First", first));
    finish(replaced);
    CHECK(first == "application replaced the focused draft while it was focused");

    edit.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto moved = begin(*context, edit);
    CHECK_FALSE(moved.text_input("hidden", "Transient", first));
    finish(moved);
    edit.events = {key_event(wui::EventKind::key_down, wui::Key::z, false, true)};
    auto separate = begin(*context, edit);
    CHECK_FALSE(separate.text_input("fresh", "Fresh", second));
    finish(separate);
    CHECK(second == "two");
}

TEST_CASE("clipboard shortcuts move whole graphemes and composition previews without committing") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto clipboard = std::make_shared<FakeClipboard>();
    auto context = make_context(textures, real_text(textures), clipboard);
    std::string value = std::string("e\xCC\x81x");
    wui::FrameInput input{{0, 0, 400, 300}};
    auto unfocused = begin(*context, input);
    CHECK_FALSE(unfocused.text_input("clip", "Clip", value));
    finish(unfocused);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto focused = begin(*context, input);
    CHECK_FALSE(focused.text_input("clip", "Clip", value));
    finish(focused);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::home),
                    key_event(wui::EventKind::key_down, wui::Key::right, true),
                    key_event(wui::EventKind::key_down, wui::Key::c, false, true)};
    auto copied = begin(*context, input);
    CHECK_FALSE(copied.text_input("clip", "Clip", value));
    finish(copied);
    CHECK(value == std::string("e\xCC\x81x"));
    CHECK(clipboard->contents == std::string("e\xCC\x81"));
    input.events = {key_event(wui::EventKind::key_down, wui::Key::end),
                    key_event(wui::EventKind::key_down, wui::Key::left, true),
                    key_event(wui::EventKind::key_down, wui::Key::x, false, true)};
    auto cut = begin(*context, input);
    CHECK(cut.text_input("clip", "Clip", value));
    finish(cut);
    CHECK(value == std::string("e\xCC\x81"));
    CHECK(clipboard->contents == "x");
    clipboard->fail_write = true;
    input.events = {key_event(wui::EventKind::key_down, wui::Key::a, false, true),
                    key_event(wui::EventKind::key_down, wui::Key::x, false, true)};
    auto failed_cut = begin(*context, input);
    CHECK_FALSE(failed_cut.text_input("clip", "Clip", value));
    finish(failed_cut);
    CHECK(value == std::string("e\xCC\x81"));
    CHECK(clipboard->contents == "x");
    clipboard->fail_write = false;
    input.events = {key_event(wui::EventKind::key_down, wui::Key::end),
                    key_event(wui::EventKind::key_down, wui::Key::v, false, true)};
    auto pasted = begin(*context, input);
    CHECK(pasted.text_input("clip", "Clip", value));
    finish(pasted);
    CHECK(value == std::string("e\xCC\x81x"));
    input.events = {composition_event("ş")};
    auto preview = begin(*context, input);
    CHECK_FALSE(preview.text_input("clip", "Clip", value));
    const auto preview_packet = finish(preview);
    CHECK(value == std::string("e\xCC\x81x"));
    CHECK(semantic(preview_packet, wui::Role::text_input, "Clip").bounds.w > 0);
    input.events = {composition_event("")};
    auto cancelled = begin(*context, input);
    CHECK_FALSE(cancelled.text_input("clip", "Clip", value));
    finish(cancelled);
    CHECK(value == std::string("e\xCC\x81x"));
    input.events = {composition_event(""), text_event("ş")};
    auto committed = begin(*context, input);
    CHECK(committed.text_input("clip", "Clip", value));
    const auto committed_packet = finish(committed);
    CHECK(value == std::string("e\xCC\x81xş"));
    CHECK(semantic(committed_packet, wui::Role::text_input, "Clip").bounds.w > 0);
}

TEST_CASE("focused text input keeps its focus ring and caret on every frame") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto context = make_context(textures, real_text(textures));
    std::string value = "abc";
    wui::FrameInput input{{0, 0, 400, 300}};
    auto warm = begin(*context, input);
    CHECK_FALSE(warm.text_input("field", "Field", value));
    finish(warm);
    auto unfocused = begin(*context, input);
    CHECK_FALSE(unfocused.text_input("field", "Field", value));
    const auto unfocused_packet = finish(unfocused);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto tabbed = begin(*context, input);
    CHECK_FALSE(tabbed.text_input("field", "Field", value));
    const auto tabbed_packet = finish(tabbed);
    CHECK(semantic(tabbed_packet, wui::Role::text_input, "Field").focused);
    CHECK(tabbed_packet->indices.size() > unfocused_packet->indices.size());
    input.events = {composition_event("ş")};
    auto composing = begin(*context, input);
    CHECK_FALSE(composing.text_input("field", "Field", value));
    const auto composing_packet = finish(composing);
    CHECK(composing_packet->indices.size() == tabbed_packet->indices.size());
    input.events.clear();
    for (int i = 0; i < 3; ++i) {
        auto idle = begin(*context, input);
        CHECK_FALSE(idle.text_input("field", "Field", value));
        const auto idle_packet = finish(idle);
        CHECK(semantic(idle_packet, wui::Role::text_input, "Field").focused);
        CHECK(idle_packet->indices.size() == tabbed_packet->indices.size());
    }
    CHECK(value == "abc");
}

TEST_CASE("absent or failing clipboard leaves the model unchanged with diagnostics") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto context = make_context(textures, real_text(textures));
    std::string value = "plain";
    wui::FrameInput input{{0, 0, 400, 300}};
    auto unfocused = begin(*context, input);
    CHECK_FALSE(unfocused.text_input("clip", "Clip", value));
    finish(unfocused);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto focused = begin(*context, input);
    CHECK_FALSE(focused.text_input("clip", "Clip", value));
    finish(focused);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::a, false, true),
                    key_event(wui::EventKind::key_down, wui::Key::c, false, true)};
    auto missing = begin(*context, input);
    CHECK_FALSE(missing.text_input("clip", "Clip", value));
    finish(missing);
    CHECK(value == "plain");
    REQUIRE(context->diagnostics().size() == 1);
    CHECK(context->diagnostics().front().code == wui::ErrorCode::unsupported);
    auto clipboard = std::make_shared<FakeClipboard>();
    clipboard->contents = "external ş";
    clipboard->fail_read = true;
    auto failing = make_context(std::make_shared<wui::TextureStore>(64 * 1024 * 1024),
                                real_text(std::make_shared<wui::TextureStore>(64 * 1024 * 1024)),
                                clipboard);
    std::string other = "local";
    auto other_unfocused = begin(*failing, input);
    CHECK_FALSE(other_unfocused.text_input("clip", "Clip", other));
    finish(other_unfocused);
    CHECK(other == "local");
}

TEST_CASE("finite numeric input commits on Enter and cancels on Escape") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto clipboard = std::make_shared<FakeClipboard>();
    auto context = make_context(textures, real_text(textures), clipboard);
    double value = 1.5;
    wui::FrameInput input{{0, 0, 400, 300}};
    auto unregistered = begin(*context, input);
    CHECK_FALSE(unregistered.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(unregistered);
    auto first = begin(*context, input);
    CHECK_FALSE(first.numeric_input("amount", "Amount", value, 0.0, 3.0));
    {
        const auto first_packet = finish(first);
        CHECK(semantic(first_packet, wui::Role::numeric_input, "Amount").value == "1.5");
    }
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto focused = begin(*context, input);
    CHECK_FALSE(focused.numeric_input("amount", "Amount", value, 0.0, 3.0));
    const auto focus_bounds = [&] {
        const auto focused_packet = finish(focused);
        return semantic(focused_packet, wui::Role::numeric_input, "Amount").bounds;
    }();
    input.events = {{wui::EventKind::pointer_down, {focus_bounds.x + 8, focus_bounds.y + 8}},
                    {wui::EventKind::pointer_up, {focus_bounds.x + 8, focus_bounds.y + 8}}};
    auto clicked = begin(*context, input);
    CHECK_FALSE(clicked.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(clicked);
    input.events.clear();
    auto click_settled = begin(*context, input);
    CHECK_FALSE(click_settled.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(click_settled);
    wui::InputEvent select{wui::EventKind::key_down};
    select.key = wui::Key::a; select.control = true;
    wui::InputEvent replace{wui::EventKind::text}; replace.text = "2.75";
    input.events = {select, replace};
    auto draft = begin(*context, input);
    CHECK_FALSE(draft.numeric_input("amount", "Amount", value, 0.0, 3.0));
    {
        const auto draft_packet = finish(draft);
        CHECK(value == 1.5);
        CHECK(semantic(draft_packet, wui::Role::numeric_input, "Amount").value == "2.75");
    }
    input.events = {key_event(wui::EventKind::key_down, wui::Key::enter)};
    auto committed = begin(*context, input);
    CHECK(committed.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(committed);
    CHECK(value == 2.75);
    for (const std::string invalid : {"", "-", "nan", "inf", "2x", "1e1000", "-1", "4"}) {
        input.events = {select, text_event(invalid)};
        auto rejected = begin(*context, input);
        CHECK_FALSE(rejected.numeric_input("amount", "Amount", value, 0.0, 3.0));
        const auto rejected_packet = finish(rejected);
        CHECK(value == 2.75);
        CHECK(rejected_packet->semantics.back().invalid);
        CHECK(rejected_packet->semantics.back().value == invalid);
    }
    int exact = 3;
    auto exact_frame = begin(*context, input);
    CHECK_FALSE(exact_frame.numeric_input("exact", "Exact", exact, 3, 3));
    finish(exact_frame);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto exact_focused = begin(*context, input);
    // Out-of-range "4" left by the last rejection restores on focus loss.
    CHECK_FALSE(exact_focused.numeric_input("amount", "Amount", value, 0.0, 3.0));
    CHECK_FALSE(exact_focused.numeric_input("exact", "Exact", exact, 3, 3));
    finish(exact_focused);
    input.events = {select, text_event("9999999999999999999999")};
    auto overflow = begin(*context, input);
    CHECK_FALSE(overflow.numeric_input("exact", "Exact", exact, 3, 3));
    finish(overflow);
    CHECK(exact == 3);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::enter)};
    auto overflow_committed = begin(*context, input);
    CHECK_FALSE(overflow_committed.numeric_input("exact", "Exact", exact, 3, 3));
    finish(overflow_committed);
    CHECK(exact == 3);
    // Tab traverses the previous frame's order, so both inputs must be present.
    input.events.clear();
    auto both_present = begin(*context, input);
    CHECK_FALSE(both_present.numeric_input("exact", "Exact", exact, 3, 3));
    CHECK_FALSE(both_present.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(both_present);
    CHECK(exact == 3);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto amount_refocused = begin(*context, input);
    CHECK_FALSE(amount_refocused.numeric_input("exact", "Exact", exact, 3, 3));
    CHECK_FALSE(amount_refocused.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(amount_refocused);
    input.events = {select, text_event("bad")};
    auto invalid_cancel_draft = begin(*context, input);
    CHECK_FALSE(invalid_cancel_draft.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(invalid_cancel_draft);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::escape)};
    auto cancelled = begin(*context, input);
    CHECK_FALSE(cancelled.numeric_input("amount", "Amount", value, 0.0, 3.0));
    {
        const auto cancelled_packet = finish(cancelled);
        CHECK(value == 2.75);
        CHECK(semantic(cancelled_packet, wui::Role::numeric_input, "Amount").value == "2.75");
    }
    input.events.clear();
    auto zero_focused = begin(*context, input);
    CHECK_FALSE(zero_focused.numeric_input("exact", "Exact", exact, 3, 3));
    CHECK_FALSE(zero_focused.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(zero_focused);
    input.events = {select, text_event("0")};
    auto focus_draft = begin(*context, input);
    CHECK_FALSE(focus_draft.numeric_input("exact", "Exact", exact, 3, 3));
    CHECK_FALSE(focus_draft.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(focus_draft);
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab)};
    auto focus_lost = begin(*context, input);
    CHECK_FALSE(focus_lost.numeric_input("exact", "Exact", exact, 3, 3));
    CHECK(focus_lost.numeric_input("amount", "Amount", value, 0.0, 3.0));
    const auto focus_packet = finish(focus_lost);
    CHECK(value == 0.0);
    CHECK(semantic(focus_packet, wui::Role::numeric_input, "Amount").value == "0");
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab), select, text_event("oops")};
    auto invalid_draft = begin(*context, input);
    CHECK_FALSE(invalid_draft.numeric_input("amount", "Amount", value, 0.0, 3.0));
    {
        const auto invalid_packet = finish(invalid_draft);
        CHECK(semantic(invalid_packet, wui::Role::numeric_input, "Amount").value == "oops");
        CHECK(semantic(invalid_packet, wui::Role::numeric_input, "Amount").invalid);
    }
    input.events = {{wui::EventKind::focus_lost}};
    auto restored = begin(*context, input);
    CHECK_FALSE(restored.numeric_input("amount", "Amount", value, 0.0, 3.0));
    {
        const auto restored_packet = finish(restored);
        CHECK(value == 0.0);
        CHECK(semantic(restored_packet, wui::Role::numeric_input, "Amount").value == "0");
    }
    clipboard->contents = "1.25";
    input.events = {key_event(wui::EventKind::key_down, wui::Key::tab), select,
                    key_event(wui::EventKind::key_down, wui::Key::v, false, true),
                    key_event(wui::EventKind::key_down, wui::Key::enter)};
    auto pasted = begin(*context, input);
    CHECK(pasted.numeric_input("amount", "Amount", value, 0.0, 3.0));
    finish(pasted);
    CHECK(value == 1.25);
    auto reduced = wui::Theme::dark();
    reduced.reduced_motion = true;
    REQUIRE_FALSE(context->set_theme(reduced).has_value());
    input.events.clear();
    auto settled = begin(*context, input);
    CHECK_FALSE(settled.numeric_input("amount", "Amount", value, 0.0, 3.0));
    {
        const auto settled_packet = finish(settled);
        CHECK(value == 1.25);
        CHECK(semantic(settled_packet, wui::Role::numeric_input, "Amount").value == "1.25");
    }
}

TEST_CASE("text area edits lines, moves by visual line, pastes line breaks and follows the caret") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto clipboard = std::make_shared<FakeClipboard>();
    auto context = make_context(textures, real_text(textures), clipboard);
    std::string value;
    float height = 120;
    wui::Rect bounds{};
    const auto step = [&](std::vector<wui::InputEvent> events, float width = 400) {
        wui::FrameInput input{{0, 0, width, 300}};
        input.events = std::move(events);
        auto frame = begin(*context, input);
        const bool changed = frame.text_area("notes", "Notes", value, height);
        const auto packet = finish(frame);
        const auto& area = semantic(packet, wui::Role::text_area, "Notes");
        CHECK(area.value == value);
        bounds = area.bounds;
        return changed;
    };
    const auto down = [](wui::Key key, bool shift = false, bool control = false) {
        return key_event(wui::EventKind::key_down, key, shift, control);
    };
    CHECK_FALSE(step({}));
    CHECK(bounds.h == 120);
    CHECK(step({down(wui::Key::tab), text_event("abc"), down(wui::Key::enter), text_event("abc")}));
    CHECK(value == "abc\nabc");

    // Up keeps the caret column; Ctrl+Home/End reach the whole text's edges.
    CHECK(step({down(wui::Key::left), down(wui::Key::up), text_event("|")}));
    CHECK(value == "ab|c\nabc");
    CHECK(step({down(wui::Key::home, false, true), text_event("<"), down(wui::Key::end, false, true),
                text_event(">")}));
    CHECK(value == "<ab|c\nabc>");
    CHECK(step({down(wui::Key::up), down(wui::Key::end), text_event("!")}));
    CHECK(value == "<ab|c!\nabc>");

    // Pasted CRLF and CR become '\n'.
    clipboard->contents = "x\r\ny\rz";
    CHECK(step({down(wui::Key::end, false, true), down(wui::Key::v, false, true)}));
    CHECK(value == "<ab|c!\nabc>x\ny\nz");

    // A press places the caret; dragging across the first line selects it.
    value = "abc\nabc";
    CHECK_FALSE(step({}));
    const float line_y = bounds.y + 8 + 4;
    CHECK(step({{wui::EventKind::pointer_down, {bounds.x + 9, line_y}},
                {wui::EventKind::pointer_move, {bounds.x + bounds.w - 12, line_y}},
                {wui::EventKind::pointer_up, {bounds.x + bounds.w - 12, line_y}}}) == false);
    CHECK(step({text_event("Z")}));
    CHECK(value == "Z\nabc");

    // In a narrow box End stops at the visual line, before the hanging space.
    value = "alpha beta gamma delta epsilon zeta eta theta iota kappa";
    CHECK_FALSE(step({}, 160));
    CHECK(step({down(wui::Key::home, false, true), down(wui::Key::end), text_event("|")}, 160));
    const auto mark = value.find('|');
    REQUIRE(mark != std::string::npos);
    CHECK(mark < 20);
    CHECK(value[mark + 1] == ' ');

    // Twenty lines in a short box: the caret's line scrolls into view.
    height = 80;
    value.clear();
    for (int i = 0; i < 20; ++i) value += "line " + std::to_string(i) + "\n";
    CHECK_FALSE(step({}));
    CHECK_FALSE(step({down(wui::Key::home, false, true)}));
    CHECK_FALSE(step({down(wui::Key::end, false, true)}));
    const auto caret = context->composition_caret();
    REQUIRE(caret.has_value());
    CHECK(caret->y > bounds.y);
    CHECK(caret->y <= bounds.y + bounds.h);
    CHECK_FALSE(step({down(wui::Key::page_up), down(wui::Key::home, false, true)}));
    const auto top = context->composition_caret();
    REQUIRE(top.has_value());
    CHECK(top->y < bounds.y + 8 + 30);
}

TEST_CASE("data grid virtualizes rows, selects by pointer and keys, sorts and resizes columns") {
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto context = make_context(textures, real_text(textures));
    const std::array<wui::GridColumn, 3> columns{{{"A", 120}, {"B", 120}, {"C", 120, 32, false}}};
    std::optional<std::size_t> selected;
    wui::GridSort sort;
    std::size_t calls = 0;
    const auto cell = [&](std::size_t row, std::size_t column) {
        ++calls;
        return "r" + std::to_string(row) + "c" + std::to_string(column);
    };
    wui::Rect grid{};
    std::vector<wui::Semantic> semantics;
    const auto step = [&](std::vector<wui::InputEvent> events) {
        wui::FrameInput input{{0, 0, 400, 300}};
        input.events = std::move(events);
        auto frame = begin(*context, input);
        calls = 0;
        const auto result = frame.data_grid("entries", "Entries", columns, 100, cell, selected, sort, 200);
        const auto packet = finish(frame);
        grid = semantic(packet, wui::Role::grid, "Entries").bounds;
        semantics = packet->semantics;
        return result;
    };
    const auto rows = [&] {
        std::vector<std::string> labels;
        for (const auto& item : semantics)
            if (item.role == wui::Role::grid_row) labels.push_back(item.label);
        return labels;
    };
    const auto header = [&](std::string_view title) {
        const auto found = std::find_if(semantics.begin(), semantics.end(), [&](const wui::Semantic& item) {
            return item.role == wui::Role::grid_header && item.label == title;
        });
        REQUIRE(found != semantics.end());
        return *found;
    };
    const auto click = [](wui::Vec2 at) {
        return std::vector<wui::InputEvent>{{wui::EventKind::pointer_down, at}, {wui::EventKind::pointer_up, at}};
    };
    const auto down = [](wui::Key key) { return key_event(wui::EventKind::key_down, key); };

    step({});
    const auto visible = rows();
    REQUIRE_FALSE(visible.empty());
    CHECK(visible.size() < 12);
    CHECK(visible.front() == "r0c0");
    CHECK(calls < 60); // only visible cells are queried
    const float row_height = header("A").bounds.h;

    auto result = step(click({grid.x + 20, grid.y + row_height * 2.5f}));
    CHECK(result.selection_changed);
    CHECK(selected == 1u);
    CHECK(step({down(wui::Key::down)}).selection_changed);
    CHECK(selected == 2u);
    step({down(wui::Key::end)});
    CHECK(selected == 99u);
    CHECK(rows().back() == "r99c0");
    step({down(wui::Key::page_up)});
    CHECK(selected < 99u);
    step({down(wui::Key::home)});
    CHECK(selected == 0u);
    CHECK(rows().front() == "r0c0");
    CHECK(step({down(wui::Key::enter)}).activated);

    const auto b = header("B").bounds;
    CHECK(step(click({b.x + 30, b.y + b.h * 0.5f})).sort_changed);
    CHECK((sort.column == 1 && sort.direction == wui::SortDirection::ascending));
    CHECK(step(click({b.x + 30, b.y + b.h * 0.5f})).sort_changed);
    CHECK(sort.direction == wui::SortDirection::descending);
    CHECK(header("B").value == "descending");
    CHECK_FALSE(step(click({header("C").bounds.x + 30, b.y + b.h * 0.5f})).sort_changed);

    // Dragging A's right edge widens A and shifts B without sorting.
    const float edge = header("A").bounds.x + header("A").bounds.w;
    result = step({{wui::EventKind::pointer_down, {edge, b.y + 4}},
                   {wui::EventKind::pointer_move, {edge + 40, b.y + 4}},
                   {wui::EventKind::pointer_up, {edge + 40, b.y + 4}}});
    CHECK_FALSE(result.sort_changed);
    step({});
    CHECK(header("B").bounds.x == doctest::Approx(b.x + 40));
    CHECK(sort.direction == wui::SortDirection::descending);
}
