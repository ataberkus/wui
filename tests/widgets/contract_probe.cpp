#include <wui/wui.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#endif

#ifdef NDEBUG
#error "The assertion probe and its independently compiled core must retain assertions"
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

void suppress_abort_dialogs() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
#endif
}

void arm(std::string_view mode) {
    std::fprintf(stdout, "WUI_ASSERT_ARMED:%.*s\n", static_cast<int>(mode.size()), mode.data());
    std::fflush(stdout);
}

int run(std::string_view mode) {
    if (mode == "ordinary-error") {
        arm(mode);
        std::fputs("probe ordinary error, not a contract assertion\n", stderr);
        return 23;
    }
    if (mode == "abort-control") {
        arm(mode);
        std::fputs("probe arbitrary abort, not a contract assertion\n", stderr);
        std::fflush(stderr);
        std::abort();
    }
    wui::ContextDesc desc;
    desc.textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    desc.text = std::make_shared<FixtureText>();
    auto created = wui::Context::create(std::move(desc));
    if (!std::holds_alternative<std::unique_ptr<wui::Context>>(created))
        throw std::runtime_error("context setup failed");
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    const wui::FrameInput input{{0, 0, 400, 300}};
    auto begun = context->begin_frame(input);
    if (!std::holds_alternative<wui::Frame>(begun))
        throw std::runtime_error("frame setup failed");
    auto frame = std::get<wui::Frame>(std::move(begun));
    if (mode == "control") {
        {
            auto panel = frame.panel("panel", {"Control"});
            frame.text("Valid scoped frame");
            frame.button("button", "Button");
        }
        auto packet = frame.finish();
        if (!std::holds_alternative<wui::DrawPacket>(packet) ||
            wui::validate_packet(*std::get<wui::DrawPacket>(packet)))
            throw std::runtime_error("control packet failed");
        std::fputs("WUI_CONTROL_OK\n", stdout);
        return 0;
    }
    if (mode == "duplicate") {
        frame.button("duplicate", "First");
        arm(mode);
        frame.button("duplicate", "Second");
    } else if (mode == "out-of-order") {
        std::optional<wui::PanelScope> outer{frame.panel("outer")};
        std::optional<wui::PanelScope> inner{frame.panel("inner")};
        arm(mode);
        outer.reset();
        inner.reset();
    } else if (mode == "open-scopes") {
        auto panel = frame.panel("open");
        arm(mode);
        frame.finish();
    } else if (mode == "row-overlap") {
        const std::array columns{wui::Column{wui::ColumnKind::fixed, 80}};
        auto row = frame.row("row", columns);
        auto first = row.cell();
        arm(mode);
        row.cell();
    } else if (mode == "spacer-negative") {
        arm(mode);
        frame.spacer(-1);
    } else if (mode == "custom-height") {
        arm(mode);
        frame.custom("invalid", "Invalid", -1);
    } else if (mode == "custom-moved") {
        auto moved = std::move(frame);
        arm(mode);
        frame.custom("invalid", "Invalid", 10);
    } else if (mode == "style-overlap") {
        std::optional<wui::StyleScope> outer{frame.style({})};
        std::optional<wui::StyleScope> inner{frame.style({})};
        arm(mode);
        outer.reset();
        inner.reset();
    } else if (mode == "style-color") {
        arm(mode);
        frame.style({.focus = wui::Color{2, 0, 0, 1}});
    } else if (mode == "icon-label") {
        arm(mode);
        frame.icon_button("help", "", {});
    } else if (mode == "second-frame") {
        arm(mode);
        context->begin_frame(input);
    } else {
        std::fputs("unknown probe mode\n", stderr);
        return 64;
    }
    std::fputs("contract operation unexpectedly returned\n", stderr);
    return 72;
}
}

int main(int argc, char** argv) {
    suppress_abort_dialogs();
    if (argc != 2) {
        std::fputs("expected exactly one probe mode\n", stderr);
        return 64;
    }
    try {
        return run(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "probe setup error: %s\n", error.what());
        return 70;
    } catch (...) {
        std::fputs("probe setup error: unknown exception\n", stderr);
        return 71;
    }
}
