#include <wui/wui.hpp>
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#ifndef NDEBUG
#error "Contract diagnostics require a separately compiled NDEBUG core and test target"
#endif

namespace {
class FixtureText final : public wui::IText {
public:
    wui::Result<std::shared_ptr<const wui::TextRun>> shape(
        const wui::ShapeRequest& request) override {
        auto [it, inserted] = runs_.try_emplace(std::string(request.text));
        if (inserted) {
            auto run = std::make_shared<wui::TextRun>();
            run->width = static_cast<float>(request.text.size()) * 7;
            run->height = 18;
            run->baseline = 14;
            run->display = std::string(request.text);
            for (std::size_t i = 0; i < run->display.size(); ++i)
                run->grapheme_starts.push_back(i);
            it->second = std::move(run);
        }
        return it->second;
    }
    wui::Status publish() override { return std::nullopt; }
    wui::Result<std::vector<std::size_t>> grapheme_offsets(std::string_view source) override {
        std::vector<std::size_t> offsets{0};
        for (std::size_t i = 0; i < source.size(); ++i) offsets.push_back(i + 1);
        return offsets;
    }
private:
    std::unordered_map<std::string, std::shared_ptr<const wui::TextRun>> runs_;
};

std::unique_ptr<wui::Context> make_context() {
    wui::ContextDesc desc;
    desc.textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    desc.text = std::make_shared<FixtureText>();
    auto result = wui::Context::create(std::move(desc));
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::Context>>(result));
    return std::get<std::unique_ptr<wui::Context>>(std::move(result));
}

wui::Frame begin(wui::Context& context,
                 const wui::FrameInput& input = wui::FrameInput{{0, 0, 400, 300}}) {
    auto result = context.begin_frame(input);
    REQUIRE(std::holds_alternative<wui::Frame>(result));
    return std::get<wui::Frame>(std::move(result));
}

void check_error(wui::Result<wui::DrawPacket> result, wui::ErrorCode code) {
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == code);
}

void check_recovery(wui::Context& context) {
    auto frame = begin(context);
    frame.text("Fresh frame");
    auto result = frame.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
    auto packet = std::get<wui::DrawPacket>(std::move(result));
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    REQUIRE(packet->semantics.size() == 1);
    CHECK(packet->semantics.front().label == "Fresh frame");
}
}

TEST_CASE("release duplicate IDs invalidate the frame without undoing prior model updates") {
    auto context = make_context();
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 30}},
                    {wui::EventKind::pointer_up, {30, 30}},
                    {wui::EventKind::pointer_down, {30, 74}},
                    {wui::EventKind::pointer_up, {30, 74}}};
    auto frame = begin(*context, input);
    bool changed_before_error = false, later = false;
    {
        auto panel = frame.panel("panel");
        CHECK(frame.checkbox("same", "Initial", changed_before_error));
        CHECK(changed_before_error);
        CHECK_FALSE(frame.checkbox("same", "Renamed duplicate", changed_before_error));
        CHECK_FALSE(frame.checkbox("later", "Must not mutate", later));
    }
    CHECK(changed_before_error);
    CHECK_FALSE(later);
    check_error(frame.finish(), wui::ErrorCode::duplicate_id);
    check_recovery(*context);
}

TEST_CASE("release out of order scope closure returns unbalanced scope and stops mutation") {
    auto context = make_context();
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {40, 40}},
                    {wui::EventKind::pointer_up, {40, 40}}};
    auto frame = begin(*context, input);
    std::optional<wui::PanelScope> outer{frame.panel("outer")};
    std::optional<wui::PanelScope> inner{frame.panel("inner")};
    outer.reset();
    bool value = false;
    CHECK_FALSE(frame.checkbox("later", "Must not mutate", value));
    inner.reset();
    CHECK_FALSE(value);
    check_error(frame.finish(), wui::ErrorCode::unbalanced_scope);
    check_recovery(*context);
}

TEST_CASE("release finish with an open scope rejects the packet and permits recovery") {
    auto context = make_context();
    auto frame = begin(*context);
    std::optional<wui::PanelScope> stale{frame.panel("open")};
    frame.text("Unfinished panel");
    check_error(frame.finish(), wui::ErrorCode::unbalanced_scope);
    // Closing the old generation must not close the new frame's panel.
    auto recovered = begin(*context);
    {
        auto current = recovered.panel("current");
        stale.reset();
        recovered.text("Still inside current panel");
    }
    auto result = recovered.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
    const auto& packet = std::get<wui::DrawPacket>(result);
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    REQUIRE(packet->semantics.size() == 2);
    CHECK(packet->semantics.back().label == "Still inside current panel");
}

TEST_CASE("release row rejects overlapping, extra, and prematurely closed cells") {
    const std::array columns{wui::Column{wui::ColumnKind::fixed, 80}};
    for (int mode = 0; mode < 3; ++mode) {
        auto context = make_context();
        auto frame = begin(*context);
        std::optional<wui::RowScope> row{frame.row("row", columns)};
        if (mode == 0) {
            auto first = row->cell();
            auto overlap = row->cell();
        } else if (mode == 1) {
            { auto first = row->cell(); frame.text("First"); }
            auto extra = row->cell();
        } else {
            auto first = row->cell();
            row.reset();
        }
        row.reset();
        check_error(frame.finish(), wui::ErrorCode::unbalanced_scope);
        check_recovery(*context);
    }
}

TEST_CASE("release unfinished row rejects packet and stale scope cannot close next frame") {
    auto context = make_context();
    auto frame = begin(*context);
    const std::array columns{wui::Column{wui::ColumnKind::fixed, 80}};
    std::optional<wui::RowScope> stale{frame.row("row", columns)};
    check_error(frame.finish(), wui::ErrorCode::unbalanced_scope);
    auto recovered = begin(*context);
    {
        auto current = recovered.panel("current");
        stale.reset();
        recovered.text("Still here");
    }
    REQUIRE(std::holds_alternative<wui::DrawPacket>(recovered.finish()));
}

TEST_CASE("release rejects invalid spacer, scroll and custom heights without a packet") {
    for (float height : {-1.0f, std::numeric_limits<float>::infinity()}) {
        auto context = make_context();
        auto frame = begin(*context);
        frame.spacer(height);
        check_error(frame.finish(), wui::ErrorCode::invalid_argument);
        check_recovery(*context);

        auto scrolling = begin(*context);
        { auto scope = scrolling.scroll("invalid", height); }
        check_error(scrolling.finish(), wui::ErrorCode::invalid_argument);
        check_recovery(*context);

        auto custom = begin(*context);
        auto item = custom.custom("invalid", "Invalid", height);
        REQUIRE(std::holds_alternative<wui::Error>(item));
        CHECK(std::get<wui::Error>(item).code == wui::ErrorCode::invalid_argument);
        check_error(custom.finish(), wui::ErrorCode::invalid_argument);
        check_recovery(*context);
    }
}

TEST_CASE("release rejects invalid and out-of-order styles and unlabeled icons") {
    auto context = make_context();
    {
        auto frame = begin(*context);
        std::optional<wui::StyleScope> outer{frame.style({.accent = wui::Color{1, 0, 0, 1}})};
        std::optional<wui::StyleScope> inner{frame.style({.accent = wui::Color{0, 1, 0, 1}})};
        outer.reset();
        inner.reset();
        check_error(frame.finish(), wui::ErrorCode::unbalanced_scope);
        check_recovery(*context);
    }
    {
        auto frame = begin(*context);
        { auto invalid = frame.style({.focus = wui::Color{2, 0, 0, 1}}); }
        check_error(frame.finish(), wui::ErrorCode::invalid_argument);
        check_recovery(*context);
    }
    {
        auto frame = begin(*context);
        CHECK_FALSE(frame.icon_button("help", "", {}));
        check_error(frame.finish(), wui::ErrorCode::invalid_argument);
        check_recovery(*context);
    }
}

TEST_CASE("release a second frame invalidates the outstanding producer") {
    auto context = make_context();
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 10}},
                    {wui::EventKind::pointer_up, {30, 10}}};
    auto first = begin(*context, input);
    auto second = context->begin_frame(input);
    REQUIRE(std::holds_alternative<wui::Error>(second));
    CHECK(std::get<wui::Error>(second).code == wui::ErrorCode::invalid_frame);
    bool value = false;
    CHECK_FALSE(first.checkbox("later", "Must not mutate", value));
    CHECK_FALSE(value);
    check_error(first.finish(), wui::ErrorCode::invalid_frame);
    check_recovery(*context);
}

TEST_CASE("release moved and finished frame handles reject further use") {
    auto context = make_context();
    auto original = begin(*context);
    auto frame = std::move(original);
    check_error(original.finish(), wui::ErrorCode::invalid_frame);
    frame.text("Valid owner");
    auto result = frame.finish();
    REQUIRE(std::holds_alternative<wui::DrawPacket>(result));
    check_error(frame.finish(), wui::ErrorCode::invalid_frame);
    bool value = false;
    CHECK_FALSE(frame.checkbox("late", "After finish", value));
    CHECK_FALSE(value);
    const auto& packet = std::get<wui::DrawPacket>(result);
    REQUIRE(packet->semantics.size() == 1);
    CHECK(packet->semantics.front().label == "Valid owner");
    check_recovery(*context);
}

TEST_CASE("release rejects reversed or nonfinite slider ranges without changing the model") {
    auto context = make_context();
    const float initial = 0.25f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const std::array<float, 3> range : {
             std::array{1.0f, 0.0f, initial},
             std::array{0.0f, 0.0f, initial},
             std::array{nan, 1.0f, initial},
             std::array{0.0f, 1.0f, nan}}) {
        float float_value = range[2];
        auto frame = begin(*context);
        CHECK_FALSE(frame.slider("invalid", "Invalid", float_value,
                                 range[0], range[1]));
        check_error(frame.finish(), wui::ErrorCode::invalid_argument);
        if (std::isnan(range[2])) CHECK(std::isnan(float_value));
        else CHECK(float_value == initial);
        check_recovery(*context);
    }
    int integer = 3;
    auto equal = begin(*context);
    CHECK_FALSE(equal.slider("equal", "Equal", integer, 3, 3));
    check_error(equal.finish(), wui::ErrorCode::invalid_argument);
    CHECK(integer == 3);
}
