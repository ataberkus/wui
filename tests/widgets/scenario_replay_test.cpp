#include "../../examples/widget_gallery/scenario_replay.hpp"

#include <wui/wui.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> font_bytes(const char* name) {
    const std::filesystem::path path = std::filesystem::path(WUI_FONT_DIR) / name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) bytes[i] = static_cast<std::byte>(raw[i]);
    return bytes;
}

ReplayServices services() {
    ReplayServices result;
    result.textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    auto made = wui::make_text_service(font_bytes("Inter-Regular.ttf"),
        font_bytes("Inter-SemiBold.ttf"), result.textures);
    REQUIRE(std::holds_alternative<std::shared_ptr<wui::IText>>(made));
    result.text = std::get<std::shared_ptr<wui::IText>>(std::move(made));
    return result;
}

bool has_prefix(const std::vector<ReplayCheckpoint>& checkpoints, const std::string& prefix) {
    for (const auto& checkpoint : checkpoints) {
        if (checkpoint.name.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("gallery scenarios replay without a window and yield renderable checkpoints") {
    auto replayed = replay_gallery(services());
    if (auto* error = std::get_if<wui::Error>(&replayed)) FAIL(error->message);
    const auto& result = std::get<ReplayResult>(replayed);
    INFO(result.log);

    const char* scenarios[] = {"scenario-01-tabs-hide-show", "scenario-02-choice-values",
        "scenario-03-dropdown-edges", "scenario-04-slider-capture", "scenario-05-keyboard",
        "scenario-06-text-editing", "scenario-07-resize-matrix", "scenario-08-visual-states",
        "scenario-09-glyph-atlas-recreation", "scenario-10-packets-in-flight"};
    for (const char* scenario : scenarios) {
        CHECK(result.log.find(std::string("PASS ") + scenario + " ") != std::string::npos);
        CHECK(has_prefix(result.checkpoints, std::string(scenario) + "/"));
    }
    for (const char* scale : {"scale-100/", "scale-125/", "scale-150/", "scale-200/"}) {
        CHECK(has_prefix(result.checkpoints, scale));
    }

    MESSAGE("checkpoints=" << result.checkpoints.size());
    std::set<std::string> names;
    std::size_t matrix = 0;
    for (const auto& checkpoint : result.checkpoints) {
        CHECK(names.insert(checkpoint.name).second);
        REQUIRE(checkpoint.packet);
        CHECK_FALSE(wui::validate_packet(*checkpoint.packet).has_value());
        CHECK_FALSE(checkpoint.packet->vertices.empty());
        CHECK(checkpoint.width > 0);
        CHECK(checkpoint.height > 0);
        CHECK(checkpoint.width <= 1920);
        CHECK(checkpoint.height <= 1080);
        matrix += checkpoint.name.rfind("scenario-07-resize-matrix/matrix-", 0) == 0;
    }
    CHECK(matrix == 12);
    // The physical extent of each matrix checkpoint is exactly its named size.
    for (const auto& checkpoint : result.checkpoints) {
        if (checkpoint.name == "scenario-07-resize-matrix/matrix-scale-150-1920x1080") {
            CHECK(checkpoint.width == 1920);
            CHECK(checkpoint.height == 1080);
            CHECK(checkpoint.scale == doctest::Approx(1.5f));
        }
    }

    // The atlas grew between the two scenario-9 checkpoints.
    const ReplayCheckpoint* before = nullptr;
    const ReplayCheckpoint* after = nullptr;
    for (const auto& checkpoint : result.checkpoints) {
        if (checkpoint.name == "scenario-09-glyph-atlas-recreation/before-glyphs") before = &checkpoint;
        if (checkpoint.name == "scenario-09-glyph-atlas-recreation/after-glyphs") after = &checkpoint;
    }
    REQUIRE(before);
    REQUIRE(after);
    CHECK(before->packet->textures != after->packet->textures);
}
