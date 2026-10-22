// Consumer of the installed wui package: proves every requested component
// compiles, links and runs from a relocated prefix without the source tree.

#include <wui/version.hpp>
#include <wui/wui.hpp>

#ifdef WUI_HAS_WIN32
#include <wui/backends/win32.hpp>
#endif
#ifdef WUI_HAS_D3D9
#include <wui/backends/d3d9.hpp>
#endif
#ifdef WUI_HAS_D3D10
#include <d3d10_1.h>
#include <wui/backends/d3d10.hpp>
#endif
#ifdef WUI_HAS_D3D11
#include <wui/backends/d3d11.hpp>
#endif
#ifdef WUI_HAS_D3D12
#include <wui/backends/d3d12.hpp>
#endif
#ifdef WUI_HAS_VULKAN
#include <wui/backends/vulkan.hpp>
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

int fail(const char* what, const wui::Error& error) {
    std::printf("FAIL %s: %s\n", what, error.message.c_str());
    return 1;
}

template<class Created>
int expect_invalid_argument(const char* what, Created created) {
    auto* error = std::get_if<wui::Error>(&created);
    if (!error || error->code != wui::ErrorCode::invalid_argument) {
        std::printf("FAIL %s: expected invalid_argument\n", what);
        return 1;
    }
    return 0;
}

#ifdef WUI_HAS_TEXT
std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(bytes.size());
    if (!bytes.empty()) std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

int run_text(const std::filesystem::path& fonts) {
    auto regular = read_file(fonts / "Inter-Regular.ttf");
    auto semibold = read_file(fonts / "Inter-SemiBold.ttf");
    if (regular.empty() || semibold.empty() || !std::filesystem::exists(fonts / "LICENSE.txt")) {
        std::printf("FAIL fonts or font license missing under %s\n", fonts.string().c_str());
        return 1;
    }
    auto textures = std::make_shared<wui::TextureStore>(64u << 20);
    auto text = wui::make_text_service(regular, semibold, textures);
    if (auto* error = std::get_if<wui::Error>(&text)) return fail("make_text_service", *error);
    wui::ContextDesc desc;
    desc.textures = textures;
    desc.text = std::get<std::shared_ptr<wui::IText>>(text);
    auto context = wui::Context::create(desc);
    if (auto* error = std::get_if<wui::Error>(&context)) return fail("Context::create", *error);
    auto& ui = *std::get<std::unique_ptr<wui::Context>>(context);
    wui::FrameInput input;
    input.viewport = {0, 0, 640, 400};
    auto frame = ui.begin_frame(input);
    if (auto* error = std::get_if<wui::Error>(&frame)) return fail("begin_frame", *error);
    auto& f = std::get<wui::Frame>(frame);
    bool checked = true;
    {
        auto panel = f.panel("consumer", {.title = "Consumer"});
        f.heading("Installed package");
        f.button("ok", "OK");
        f.checkbox("checked", "Checked", checked);
    }
    auto packet = f.finish();
    if (auto* error = std::get_if<wui::Error>(&packet)) return fail("finish", *error);
    if (auto error = wui::validate_packet(*std::get<wui::DrawPacket>(packet))) return fail("validate_packet", *error);
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path fonts;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--fonts") == 0) fonts = argv[i + 1];
    if (auto error = wui::validate_packet(wui::PacketData{})) return fail("validate_packet(empty)", *error);
    int failures = 0;
#ifdef WUI_HAS_TEXT
    failures += run_text(fonts);
#endif
#ifdef WUI_HAS_WIN32
    {
        HWND window = CreateWindowExW(0, L"STATIC", L"consumer", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!window) { std::printf("FAIL CreateWindowExW\n"); ++failures; }
        else {
            { wui::win32::Adapter adapter(window); (void)adapter.take_frame(0); }
            DestroyWindow(window);
        }
    }
#endif
#ifdef WUI_HAS_D3D9
    failures += expect_invalid_argument("d3d9 create(nullptr)", wui::d3d9::Renderer::create(nullptr));
#endif
#ifdef WUI_HAS_D3D10
    failures += expect_invalid_argument("d3d10 create(nullptr)", wui::d3d10::Renderer::create(nullptr));
#endif
#ifdef WUI_HAS_D3D11
    failures += expect_invalid_argument("d3d11 create(nullptr)", wui::d3d11::Renderer::create(nullptr));
#endif
#ifdef WUI_HAS_D3D12
    failures += expect_invalid_argument("d3d12 create({})", wui::d3d12::Renderer::create(wui::d3d12::Config{}));
#endif
#ifdef WUI_HAS_VULKAN
    failures += expect_invalid_argument("vulkan create({})", wui::vulkan::Renderer::create(wui::vulkan::Config{}));
#endif
    if (failures) return 1;
    std::printf("wui %s ok\n", wui::version_string);
    return 0;
}
