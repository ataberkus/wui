// Common backend conformance suite. One executable per renderer links this
// file plus one adapter (tests/conformance/<backend>_backend.cpp).
//
//   wui_conformance_<backend> --out DIR [--driver warp|hardware]
//
// Writes DIR/<case>.ppm (RGB), DIR/manifest.txt and DIR/summary.txt; exits 0
// only if every case rendered, every lifetime check held and the available
// debug/validation layer reported nothing. Cross-backend tolerance comparison
// is tools/compare_conformance.py.
#include "conformance.hpp"

#include "../support/packet_fixture.hpp"
#include "gallery.hpp"
#include "scenario_replay.hpp"

#include <wui/canvas.hpp>
#include <wui/wui.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace wui::conformance {
namespace {

// A physical pixel with a known expected color, so a case that renders
// consistently but wrongly (e.g. the wrong texture) still fails.
struct Probe {
    std::uint32_t x = 0, y = 0;
    std::uint8_t r = 0, g = 0, b = 0;
    int tolerance = 2;
};

struct Case {
    std::string name;
    DrawPacket packet;
    std::uint32_t width = 0, height = 0;
    std::vector<Probe> probes;
};

Error fail(std::string message) { return Error{ErrorCode::invalid_frame, std::move(message)}; }

std::vector<std::byte> read_font(const char* name) {
    const auto path = std::filesystem::path(WUI_FONT_DIR) / name;
    std::ifstream file(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) bytes[i] = static_cast<std::byte>(raw[i]);
    return bytes;
}

Result<DrawPacket> finish(Canvas& canvas) {
    auto finished = canvas.finish();
    if (auto* error = std::get_if<Error>(&finished)) return *error;
    return std::make_shared<PacketData>(*std::get<DrawPacket>(finished));
}

// Fractional clip at 1.5x: the scissor must floor/ceil to whole physical pixels.
Result<DrawPacket> fractional_clip() {
    auto store = std::make_shared<TextureStore>(65536);
    Canvas canvas(store);
    canvas.begin({0, 0, 128, 128}, 1.5f);
    canvas.rounded_rect({0, 0, 128, 128}, 0, {0.1f, 0.1f, 0.1f, 1});
    canvas.push_clip({10.3f, 20.7f, 50.2f, 30.4f});
    canvas.rounded_rect({0, 0, 100, 100}, 0, {1, 0.5f, 0, 1});
    canvas.pop_clip();
    canvas.push_clip({70.5f, 70.5f, 0.4f, 40});
    canvas.rounded_rect({60, 60, 60, 60}, 0, {0, 1, 1, 1});
    canvas.pop_clip();
    return finish(canvas);
}

Result<DrawPacket> empty_packet() {
    auto store = std::make_shared<TextureStore>(65536);
    Canvas canvas(store);
    canvas.begin({0, 0, 64, 64}, 1);
    return finish(canvas);
}

// Straight and premultiplied half-alpha red textures must composite identically.
Result<DrawPacket> alpha_modes() {
    auto store = std::make_shared<TextureStore>(65536);
    const std::byte straight[] = {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{128}};
    const std::byte premultiplied[] = {std::byte{128}, std::byte{0}, std::byte{0}, std::byte{128}};
    auto a = store->create(1, 1, straight, AlphaMode::straight);
    auto b = store->create(1, 1, premultiplied, AlphaMode::premultiplied);
    if (auto* error = std::get_if<Error>(&a)) return *error;
    if (auto* error = std::get_if<Error>(&b)) return *error;
    Canvas canvas(store);
    canvas.begin({0, 0, 64, 32}, 1);
    canvas.rounded_rect({0, 0, 64, 32}, 0, {0, 0, 0, 1});
    canvas.image({0, 0, 32, 32}, std::get<TextureHandle>(a), {0, 0, 1, 1}, {1, 1, 1, 1});
    canvas.image({32, 0, 32, 32}, std::get<TextureHandle>(b), {0, 0, 1, 1}, {1, 1, 1, 1});
    return finish(canvas);
}

// More than 65,535 vertices in one packet: 16-bit index paths must chunk.
Result<DrawPacket> large_index() {
    auto store = std::make_shared<TextureStore>(65536);
    Canvas canvas(store);
    canvas.begin({0, 0, 256, 256}, 1);
    for (int i = 0; i < 17600; ++i) {
        const float x = static_cast<float>(i % 160) * 1.6f;
        const float y = static_cast<float>(i / 160) * 2.3f;
        canvas.rounded_rect({x, y, 1.2f, 1.8f}, 0,
            {static_cast<float>(i % 7) / 6.0f, static_cast<float>(i % 11) / 10.0f, 0.5f, 1});
    }
    return finish(canvas);
}

Result<DrawPacket> gallery_scene(const ReplayServices& services, TextureHandle icon, int section,
                                 std::optional<StressWorkload> workload) {
    ContextDesc desc;
    desc.textures = services.textures;
    desc.text = services.text;
    auto created = Context::create(std::move(desc));
    if (auto* error = std::get_if<Error>(&created)) return *error;
    auto context = std::get<std::unique_ptr<Context>>(std::move(created));
    GalleryState state;
    prepare_gallery_model(state);
    state.icon = icon;
    state.section = section;
    auto theme = workload ? stress_theme(state) : gallery_theme(state);
    if (auto* error = std::get_if<Error>(&theme)) return *error;
    if (auto error = context->set_theme(std::get<Theme>(theme))) return *error;
    DrawPacket last;
    for (int i = 0; i < 12; ++i) {
        state.viewport_height = 720;
        auto begun = context->begin_frame(FrameInput{{0, 0, 1280, 720}, 1, 1.0 / 60.0, {}});
        if (auto* error = std::get_if<Error>(&begun)) return *error;
        Frame frame = std::get<Frame>(std::move(begun));
        if (workload) build_stress_gallery(frame, state, *workload);
        else build_gallery(frame, state);
        auto finished = frame.finish();
        if (auto* error = std::get_if<Error>(&finished)) return *error;
        last = std::get<DrawPacket>(std::move(finished));
        observe_gallery_frame(state, *last, context->diagnostics());
    }
    return std::make_shared<PacketData>(*last);
}

Status add(std::vector<Case>& cases, std::string name, Result<DrawPacket> made,
           std::vector<Probe> probes = {}) {
    if (auto* error = std::get_if<Error>(&made)) {
        return fail(name + ": " + error->message);
    }
    auto packet = std::get<DrawPacket>(std::move(made));
    Case item{std::move(name), packet,
              static_cast<std::uint32_t>(std::lround(packet->viewport.w * packet->scale)),
              static_cast<std::uint32_t>(std::lround(packet->viewport.h * packet->scale)),
              std::move(probes)};
    cases.push_back(std::move(item));
    return std::nullopt;
}

Status build_cases(std::vector<Case>& cases, const ReplayServices& services, std::string& log) {
    try {
        // Opaque red interior, half-alpha green over red, background outside the scissor.
        if (auto e = add(cases, "fixture/rev1", test::reference_packet(1),
                {{12, 12, 255, 0, 0, 2}, {30, 30, 128, 128, 0, 8}, {100, 100, 26, 26, 26, 8}})) return e;
        if (auto e = add(cases, "fixture/rev2", test::reference_packet(2))) return e;
        if (auto e = add(cases, "fixture/scale2", test::reference_packet(1, 2))) return e;
    } catch (const std::exception& error) {
        return fail(std::string("reference fixture: ") + error.what());
    }
    if (auto e = add(cases, "fixture/fractional-clip", fractional_clip(),
            {{5, 5, 26, 26, 26, 2}, {40, 50, 255, 128, 0, 2}})) return e;
    if (auto e = add(cases, "fixture/empty", empty_packet(),
            {{32, 32, kClear[0], kClear[1], kClear[2], 0}})) return e;
    // Its own TextureStore: handles collide with every other store's, so a
    // renderer that caches by handle alone samples the wrong texture here.
    if (auto e = add(cases, "fixture/alpha-modes", alpha_modes(),
            {{16, 16, 128, 0, 0, 3}, {48, 16, 128, 0, 0, 3}})) return e;
    if (auto e = add(cases, "fixture/large-index", large_index())) return e;
    log += "large-index vertices=" + std::to_string(cases.back().packet->vertices.size()) + "\n";

    auto icon = make_gallery_icon(*services.textures);
    if (auto* error = std::get_if<Error>(&icon)) return *error;
    const auto handle = std::get<TextureHandle>(icon);
    const char* sections[] = {"appearance", "controls", "diagnostics"};
    for (int s = 0; s < 3; ++s) {
        if (auto e = add(cases, std::string("gallery/1280x720-") + sections[s],
                gallery_scene(services, handle, s, std::nullopt))) return e;
    }
    if (auto e = add(cases, "stress/controls100",
            gallery_scene(services, handle, 1, StressWorkload::controls_100))) return e;
    if (auto e = add(cases, "stress/controls1000",
            gallery_scene(services, handle, 1, StressWorkload::controls_1000))) return e;
    if (auto e = add(cases, "stress/virtual10000",
            gallery_scene(services, handle, 1, StressWorkload::virtual_10000))) return e;

    auto replayed = replay_gallery(services);
    if (auto* error = std::get_if<Error>(&replayed)) return *error;
    auto& result = std::get<ReplayResult>(replayed);
    log += result.log;
    for (auto& checkpoint : result.checkpoints) {
        cases.push_back(Case{"scenario/" + checkpoint.name, checkpoint.packet,
                             checkpoint.width, checkpoint.height});
    }
    return std::nullopt;
}

std::string file_name(const std::string& name) {
    std::string out;
    for (char c : name) out += c == '/' ? std::string("__") : std::string(1, c);
    return out;
}

Status write_ppm(const Image& image, const std::filesystem::path& path) {
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return fail("cannot open " + path.string());
    std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.width) * image.height * 3);
    for (std::size_t i = 0; i < rgb.size() / 3; ++i) {
        rgb[i * 3] = image.rgba[i * 4];
        rgb[i * 3 + 1] = image.rgba[i * 4 + 1];
        rgb[i * 3 + 2] = image.rgba[i * 4 + 2];
    }
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), file) == rgb.size();
    if (std::fclose(file) != 0 || !ok) return fail("cannot write " + path.string());
    return std::nullopt;
}

std::uint64_t fnv(const Image& image) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < image.rgba.size(); ++i) {
        if (i % 4 == 3) continue;
        hash = (hash ^ image.rgba[i]) * 1099511628211ull;
    }
    return hash;
}

class Suite {
public:
    Suite(OffscreenBackend& backend, std::filesystem::path out) : backend_(backend), out_(std::move(out)) {}

    int run(const std::vector<Case>& cases, const std::string& replay_log) {
        log_ += "backend: " + std::string(backend_.name()) + "\nadapter: " + backend_.adapter_description() +
            "\ndebug-layer: " + (backend_.debug_layer() ? "active" : "UNAVAILABLE (not a clean-output proof)") + "\n";
        log_ += replay_log;
        std::string manifest;
        for (const auto& item : cases) {
            auto rendered = backend_.render(item.packet, item.width, item.height);
            if (auto* error = std::get_if<Error>(&rendered)) {
                error_line("render " + item.name + ": " + error->message);
                continue;
            }
            auto& image = std::get<Image>(rendered);
            if (image.width != item.width || image.height != item.height) {
                error_line("render " + item.name + ": readback size mismatch");
                continue;
            }
            if (auto error = write_ppm(image, out_ / (file_name(item.name) + ".ppm"))) {
                error_line(error->message);
            }
            check_probes(item, image);
            char line[256];
            std::snprintf(line, sizeof(line), "%s %u %u %.3f %016llx\n", item.name.c_str(), item.width,
                item.height, static_cast<double>(item.packet->scale), static_cast<unsigned long long>(fnv(image)));
            manifest += line;
            images_.emplace(item.name, std::move(image));
            packets_.emplace(item.name, item);
        }
        check_debug("cases");
        log_ += "cases rendered: " + std::to_string(images_.size()) + "/" + std::to_string(cases.size()) + "\n";
        lifetime();
        write_text(out_ / "manifest.txt", manifest);
        log_ += failures_ == 0 ? "RESULT PASS\n" : "RESULT FAIL failures=" + std::to_string(failures_) + "\n";
        write_text(out_ / "summary.txt", log_);
        std::fputs(log_.c_str(), stdout);
        return failures_ == 0 ? 0 : 1;
    }

private:
    void error_line(const std::string& message) {
        ++failures_;
        log_ += "FAIL " + message + "\n";
    }
    void pass_line(const std::string& message) { log_ += "PASS " + message + "\n"; }

    void check_probes(const Case& item, const Image& image) {
        for (const auto& probe : item.probes) {
            if (probe.x >= image.width || probe.y >= image.height) {
                error_line("probe " + item.name + " outside the image");
                continue;
            }
            const std::uint8_t* px =
                image.rgba.data() + (static_cast<std::size_t>(probe.y) * image.width + probe.x) * 4;
            const int expected[3]{probe.r, probe.g, probe.b};
            bool ok = true;
            for (int c = 0; c < 3; ++c) ok = ok && std::abs(px[c] - expected[c]) <= probe.tolerance;
            char line[192];
            std::snprintf(line, sizeof(line), "probe %s (%u,%u) = (%d,%d,%d) expected (%d,%d,%d) +-%d",
                item.name.c_str(), probe.x, probe.y, px[0], px[1], px[2], expected[0], expected[1],
                expected[2], probe.tolerance);
            if (ok) pass_line(line);
            else error_line(line);
        }
    }

    void check_debug(const char* group) {
        const auto messages = backend_.debug_messages();
        if (!backend_.debug_layer()) {
            log_ += std::string("SKIP debug-clean ") + group + ": no debug layer (not a clean-output proof)\n";
            return;
        }
        if (messages != 0) error_line(std::string("debug layer reported ") + std::to_string(messages) +
                                      " warnings/errors during " + group);
        else pass_line(std::string("debug-clean ") + group);
    }

    // Renders a stored case again and requires byte-identical output.
    void rerender(const std::string& name, const char* when) {
        const auto found = packets_.find(name);
        if (found == packets_.end()) return error_line(std::string(when) + ": missing case " + name);
        auto rendered = backend_.render(found->second.packet, found->second.width, found->second.height);
        if (auto* error = std::get_if<Error>(&rendered)) {
            return error_line(std::string(when) + " " + name + ": " + error->message);
        }
        const auto result = compare(images_.at(name), std::get<Image>(rendered));
        if (!result.identical()) {
            return error_line(std::string(when) + " " + name + " differs (max " +
                std::to_string(result.max_channel_diff) + ")");
        }
        pass_line(std::string(when) + " " + name + " identical");
    }

    void lifetime() {
        // Frames in flight: submissions are refused (busy) once capacity is reached.
        const auto& rev1 = packets_.find("fixture/rev1");
        if (rev1 == packets_.end()) return error_line("lifetime: fixture/rev1 did not render");
        unsigned accepted = 0;
        bool busy = false;
        for (int i = 0; i < 16 && !busy; ++i) {
            auto status = backend_.submit_in_flight(rev1->second.packet, 128, 128);
            if (!status) ++accepted;
            else if (status->code == ErrorCode::busy) busy = true;
            else return error_line("in-flight submit: " + status->message);
        }
        if (!busy || accepted == 0) {
            error_line("in-flight: expected busy after >=1 accepted submissions, accepted=" +
                std::to_string(accepted) + " busy=" + std::to_string(busy));
        } else {
            pass_line("in-flight accepted=" + std::to_string(accepted) + " then busy");
        }
        if (auto status = backend_.collect_all()) return error_line("collect_all: " + status->message);
        rerender("fixture/rev1", "after-collect");
        // Revisions: the newer revision and then the older one again.
        rerender("fixture/rev2", "revision");
        rerender("fixture/rev1", "revision");
        // Atlas growth: an older packet keeps rendering its own atlas snapshot.
        const std::string before = "scenario/scenario-09-glyph-atlas-recreation/before-glyphs";
        const std::string after = "scenario/scenario-09-glyph-atlas-recreation/after-glyphs";
        rerender(before, "atlas");
        rerender(after, "atlas");
        rerender(before, "atlas-old-again");
        rerender("scenario/scenario-10-packets-in-flight/held", "held-packet");
        check_debug("lifetime");
        // Recreation (reset / new device / shutdown+create) restores everything from packets.
        if (auto status = backend_.recreate()) return error_line("recreate: " + status->message);
        pass_line("recreate");
        rerender("fixture/rev1", "after-recreate");
        rerender(before, "after-recreate");
        rerender(after, "after-recreate");
        rerender("stress/controls1000", "after-recreate");
        check_debug("recreate");
    }

    void write_text(const std::filesystem::path& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary);
        file << text;
        if (!file) error_line("cannot write " + path.string());
    }

    OffscreenBackend& backend_;
    std::filesystem::path out_;
    std::map<std::string, Image> images_;
    std::map<std::string, Case> packets_;
    std::string log_;
    unsigned failures_ = 0;
};

}  // namespace
}  // namespace wui::conformance

int main(int argc, char** argv) {
    using namespace wui::conformance;
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) options.out = argv[++i];
        else if (arg == "--driver" && i + 1 < argc) options.driver = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s --out DIR [--driver warp|hardware]\n", argv[0]);
            return 2;
        }
    }
    if (options.out.empty()) {
        std::fprintf(stderr, "--out is required\n");
        return 2;
    }
    std::error_code code;
    std::filesystem::create_directories(options.out, code);
    if (code) {
        std::fprintf(stderr, "cannot create %s\n", options.out.string().c_str());
        return 1;
    }
    ReplayServices services;
    services.textures = std::make_shared<wui::TextureStore>(256 * 1024 * 1024);
    auto text = wui::make_text_service(read_font("Inter-Regular.ttf"), read_font("Inter-SemiBold.ttf"),
                                       services.textures);
    if (auto* error = std::get_if<wui::Error>(&text)) {
        std::fprintf(stderr, "text service: %s\n", error->message.c_str());
        return 1;
    }
    services.text = std::get<std::shared_ptr<wui::IText>>(std::move(text));
    std::vector<Case> cases;
    std::string replay_log;
    if (auto error = build_cases(cases, services, replay_log)) {
        std::fprintf(stderr, "building cases: %s\n", error->message.c_str());
        return 1;
    }
    auto made = make_backend(options);
    if (auto* error = std::get_if<wui::Error>(&made)) {
        std::fprintf(stderr, "backend: %s\n", error->message.c_str());
        return 1;
    }
    auto backend = std::get<std::unique_ptr<OffscreenBackend>>(std::move(made));
    Suite suite(*backend, options.out);
    return suite.run(cases, replay_log);
}
