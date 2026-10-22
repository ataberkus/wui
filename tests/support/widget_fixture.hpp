#pragma once

#include <wui/wui.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace wui::test {

class FixtureText final : public IText {
public:
    Result<std::shared_ptr<const TextRun>> shape(const ShapeRequest& request) override {
        auto [it, inserted] = runs.try_emplace(std::string(request.text));
        if (inserted) {
            auto run = std::make_shared<TextRun>();
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
    Status publish() override { return std::nullopt; }
    Result<std::vector<std::size_t>> grapheme_offsets(std::string_view source) override {
        std::vector<std::size_t> offsets{0};
        for (std::size_t i = 0; i < source.size(); ++i) offsets.push_back(i + 1);
        return offsets;
    }
private:
    std::unordered_map<std::string, std::shared_ptr<const TextRun>> runs;
};

inline ContextDesc fixture_desc() {
    ContextDesc desc;
    desc.textures = std::make_shared<TextureStore>(1024 * 1024);
    desc.text = std::make_shared<FixtureText>();
    return desc;
}

inline std::unique_ptr<Context> make_context(ContextDesc desc = fixture_desc()) {
    auto result = Context::create(std::move(desc));
    REQUIRE(std::holds_alternative<std::unique_ptr<Context>>(result));
    return std::get<std::unique_ptr<Context>>(std::move(result));
}

inline Frame begin(Context& context,
                   const FrameInput& input = FrameInput{{0, 0, 400, 300}}) {
    auto result = context.begin_frame(input);
    REQUIRE(std::holds_alternative<Frame>(result));
    return std::get<Frame>(std::move(result));
}

inline DrawPacket finish(Frame& frame) {
    auto result = frame.finish();
    REQUIRE(std::holds_alternative<DrawPacket>(result));
    auto packet = std::get<DrawPacket>(std::move(result));
    REQUIRE(packet);
    REQUIRE_FALSE(validate_packet(*packet).has_value());
    return packet;
}

inline const Semantic& semantic(const DrawPacket& packet, Role role,
                                std::string_view label) {
    const auto found = std::find_if(packet->semantics.begin(), packet->semantics.end(),
        [=](const Semantic& item) { return item.role == role && item.label == label; });
    REQUIRE(found != packet->semantics.end());
    return *found;
}

inline InputEvent key_event(EventKind kind, Key key) {
    InputEvent event{kind};
    event.key = key;
    return event;
}

} // namespace wui::test
