#include <doctest/doctest.h>

#include <wui/backends/d3d9.hpp>
#include "../support/native_image.hpp"
#include "../support/packet_fixture.hpp"
#include "../../backends/renderer/d3d9_caps.hpp"
#ifdef WUI_PROBE_TEXT
#include <wui/context.hpp>
#include <wui/text.hpp>
#include "gallery.hpp"
#endif

#include <wrl/client.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#ifdef WUI_PROBE_TEXT
#include <filesystem>
#include <fstream>
#include <vector>
#endif

namespace {

D3DCAPS9 supported_caps() {
    D3DCAPS9 caps{};
    caps.DeviceType = D3DDEVTYPE_HAL;
    caps.VertexShaderVersion = D3DVS_VERSION(3, 0);
    caps.PixelShaderVersion = D3DPS_VERSION(3, 0);
    caps.MaxVertexIndex = 2;
    caps.MaxPrimitiveCount = 1;
    caps.MaxTextureWidth = 4;
    caps.MaxTextureHeight = 4;
    caps.RasterCaps = D3DPRASTERCAPS_SCISSORTEST;
    caps.SrcBlendCaps = D3DPBLENDCAPS_ONE;
    caps.DestBlendCaps = D3DPBLENDCAPS_INVSRCALPHA;
    caps.PrimitiveMiscCaps = D3DPMISCCAPS_SEPARATEALPHABLEND;
    caps.TextureCaps = D3DPTEXTURECAPS_ALPHA;
    caps.TextureFilterCaps = D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFLINEAR;
    return caps;
}

void rejects(D3DCAPS9 caps, const char* capability) {
    const auto error = wui::d3d9::detail::validate_caps(caps);
    REQUIRE(error.has_value());
    CHECK(error->code == wui::ErrorCode::unsupported);
    CHECK(error->message.find(capability) != std::string::npos);
}

using Microsoft::WRL::ComPtr;

struct Native {
    HWND window = nullptr;
    ComPtr<IDirect3D9> api;
    ComPtr<IDirect3DDevice9> device;
    D3DPRESENT_PARAMETERS presentation{};

    Native() {
        try {
            window = CreateWindowExW(0, L"STATIC", L"wui D3D9 render probe",
                WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (!window) throw std::runtime_error("CreateWindowExW failed");
            api.Attach(Direct3DCreate9(D3D_SDK_VERSION));
            if (!api) throw std::runtime_error("Direct3DCreate9 failed");
            presentation.Windowed = TRUE;
            presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
            presentation.BackBufferFormat = D3DFMT_UNKNOWN;
            presentation.BackBufferWidth = presentation.BackBufferHeight = 256;
            presentation.hDeviceWindow = window;
            if (FAILED(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                    window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                    &presentation, &device))) {
                throw std::runtime_error("D3D9 HAL CreateDevice failed");
            }
        } catch (...) {
            device.Reset();
            api.Reset();
            if (window) DestroyWindow(window);
            throw;
        }
    }
    ~Native() {
        device.Reset();
        api.Reset();
        if (window) DestroyWindow(window);
    }
    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;
};

struct Readback {
    ComPtr<IDirect3DSurface9> color;
    ComPtr<IDirect3DSurface9> system;
    std::uint32_t width = 0, height = 0;
};

Readback create_target(IDirect3DDevice9* device, std::uint32_t width,
    std::uint32_t height) {
    Readback result;
    result.width = width;
    result.height = height;
    if (FAILED(device->CreateRenderTarget(width, height, D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE, 0, FALSE, &result.color, nullptr)) ||
        FAILED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &result.system, nullptr))) {
        throw std::runtime_error("D3D9 A8R8G8B8 readback surfaces unavailable");
    }
    return result;
}

wui::test::Image read_back(IDirect3DDevice9* device, const Readback& target) {
    if (FAILED(device->GetRenderTargetData(target.color.Get(), target.system.Get()))) {
        throw std::runtime_error("GetRenderTargetData failed");
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(target.system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        throw std::runtime_error("D3D9 staging LockRect failed");
    }
    wui::test::Image image;
    image.width = target.width;
    image.height = target.height;
    image.pixels.resize(static_cast<std::size_t>(target.width) * target.height);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(locked.pBits) +
            static_cast<std::size_t>(y) * locked.Pitch;
        for (std::uint32_t x = 0; x < target.width; ++x) {
            image.pixels[static_cast<std::size_t>(y) * target.width + x] = {
                row[4 * x + 2], row[4 * x + 1], row[4 * x], row[4 * x + 3]};
        }
    }
    target.system->UnlockRect();
    return image;
}

std::unique_ptr<wui::d3d9::Renderer> renderer_for(IDirect3DDevice9* device) {
    auto created = wui::d3d9::Renderer::create(device);
    if (auto* error = std::get_if<wui::Error>(&created)) {
        throw std::runtime_error("D3D9 Renderer::create: " + error->message);
    }
    return std::move(std::get<std::unique_ptr<wui::d3d9::Renderer>>(created));
}

wui::test::Image render_and_read(IDirect3DDevice9* device,
    wui::d3d9::Renderer& renderer, const wui::DrawPacket& packet,
    const Readback& target) {
    if (FAILED(device->BeginScene())) throw std::runtime_error("BeginScene failed");
    const auto error = renderer.render(packet,
        {target.color.Get(), target.width, target.height});
    const HRESULT ended = device->EndScene();
    if (error) throw std::runtime_error("D3D9 render: " + error->message);
    if (FAILED(ended)) throw std::runtime_error("EndScene failed");
    auto image = read_back(device, target);
    if (const auto retirement = renderer.collect()) {
        throw std::runtime_error("D3D9 collect after readback: " + retirement->message);
    }
    return image;
}

void check_pixel(wui::test::Pixel pixel, int red, int green, int blue, int alpha,
    int tolerance = 8) {
    CHECK(std::abs(static_cast<int>(pixel.r) - red) <= tolerance);
    CHECK(std::abs(static_cast<int>(pixel.g) - green) <= tolerance);
    CHECK(std::abs(static_cast<int>(pixel.b) - blue) <= tolerance);
    CHECK(std::abs(static_cast<int>(pixel.a) - alpha) <= tolerance);
}
struct HostState {
    ComPtr<IDirect3DSurface9> target;
    ComPtr<IDirect3DSurface9> target1;
    ComPtr<IDirect3DSurface9> depth;
    ComPtr<IDirect3DBaseTexture9> texture;
    ComPtr<IDirect3DVertexShader9> vertex_shader;
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    ComPtr<IDirect3DVertexBuffer9> stream;
    ComPtr<IDirect3DIndexBuffer9> indices;
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    UINT offset = 0, stride = 0;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    D3DMATRIX world{};
    DWORD cull = 0, blend = 0, address = 0;
    float vertex_constants[4]{};
    float pixel_constants[4]{};

    void capture(IDirect3DDevice9* device) {
        REQUIRE(SUCCEEDED(device->GetRenderTarget(0, &target)));
        D3DCAPS9 caps{};
        REQUIRE(SUCCEEDED(device->GetDeviceCaps(&caps)));
        if (caps.NumSimultaneousRTs > 1) {
            const HRESULT extra_result = device->GetRenderTarget(1, &target1);
            const bool extra_available_or_unbound =
                extra_result == D3DERR_NOTFOUND || SUCCEEDED(extra_result);
            REQUIRE(extra_available_or_unbound);
        }
        const HRESULT depth_result = device->GetDepthStencilSurface(&depth);
        const bool depth_available_or_unbound =
            depth_result == D3DERR_NOTFOUND || SUCCEEDED(depth_result);
        REQUIRE(depth_available_or_unbound);
        REQUIRE(SUCCEEDED(device->GetTexture(0, &texture)));
        REQUIRE(SUCCEEDED(device->GetVertexShader(&vertex_shader)));
        REQUIRE(SUCCEEDED(device->GetPixelShader(&pixel_shader)));
        REQUIRE(SUCCEEDED(device->GetStreamSource(0, &stream, &offset, &stride)));
        REQUIRE(SUCCEEDED(device->GetIndices(&indices)));
        REQUIRE(SUCCEEDED(device->GetVertexDeclaration(&declaration)));
        REQUIRE(SUCCEEDED(device->GetViewport(&viewport)));
        REQUIRE(SUCCEEDED(device->GetScissorRect(&scissor)));
        REQUIRE(SUCCEEDED(device->GetTransform(D3DTS_WORLD, &world)));
        REQUIRE(SUCCEEDED(device->GetRenderState(D3DRS_CULLMODE, &cull)));
        REQUIRE(SUCCEEDED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)));
        REQUIRE(SUCCEEDED(device->GetSamplerState(0, D3DSAMP_ADDRESSU, &address)));
        REQUIRE(SUCCEEDED(device->GetVertexShaderConstantF(0, vertex_constants, 1)));
        REQUIRE(SUCCEEDED(device->GetPixelShaderConstantF(2, pixel_constants, 1)));
    }
    void check_equal(const HostState& expected) const {
        CHECK(target.Get() == expected.target.Get());
        CHECK(target1.Get() == expected.target1.Get());
        CHECK(depth.Get() == expected.depth.Get());
        CHECK(texture.Get() == expected.texture.Get());
        CHECK(vertex_shader.Get() == expected.vertex_shader.Get());
        CHECK(pixel_shader.Get() == expected.pixel_shader.Get());
        CHECK(stream.Get() == expected.stream.Get());
        CHECK(indices.Get() == expected.indices.Get());
        CHECK(declaration.Get() == expected.declaration.Get());
        CHECK(offset == expected.offset);
        CHECK(stride == expected.stride);
        CHECK(viewport.X == expected.viewport.X);
        CHECK(viewport.Y == expected.viewport.Y);
        CHECK(viewport.Width == expected.viewport.Width);
        CHECK(viewport.Height == expected.viewport.Height);
        CHECK(viewport.MinZ == expected.viewport.MinZ);
        CHECK(viewport.MaxZ == expected.viewport.MaxZ);
        CHECK(scissor.left == expected.scissor.left);
        CHECK(scissor.top == expected.scissor.top);
        CHECK(scissor.right == expected.scissor.right);
        CHECK(scissor.bottom == expected.scissor.bottom);
        CHECK(std::memcmp(&world, &expected.world, sizeof(world)) == 0);
        CHECK(cull == expected.cull);
        CHECK(blend == expected.blend);
        CHECK(address == expected.address);
        for (int i = 0; i < 4; ++i) {
            CHECK(vertex_constants[i] == expected.vertex_constants[i]);
            CHECK(pixel_constants[i] == expected.pixel_constants[i]);
        }
    }
};
#ifdef WUI_PROBE_TEXT
std::vector<std::byte> font_bytes(const char* filename) {
    const auto path = std::filesystem::path(WUI_FONT_DIR) / filename;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open font: " + path.string());
    const auto end = stream.tellg();
    if (end <= 0) throw std::runtime_error("Empty font: " + path.string());
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("Cannot read font: " + path.string());
    return bytes;
}

struct GalleryFixture {
    std::shared_ptr<wui::TextureStore> textures =
        std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    std::unique_ptr<wui::Context> context;
    GalleryState model;

    GalleryFixture() {
        auto text = wui::make_text_service(font_bytes("Inter-Regular.ttf"),
            font_bytes("Inter-SemiBold.ttf"), textures);
        if (auto* error = std::get_if<wui::Error>(&text)) {
            throw std::runtime_error("make_text_service: " + error->message);
        }
        wui::ContextDesc description;
        description.textures = textures;
        description.text = std::get<std::shared_ptr<wui::IText>>(std::move(text));
        auto created = wui::Context::create(std::move(description));
        if (auto* error = std::get_if<wui::Error>(&created)) {
            throw std::runtime_error("Context::create: " + error->message);
        }
        context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
        prepare_gallery_model(model);
        auto icon = make_gallery_icon(*textures);
        if (auto* error = std::get_if<wui::Error>(&icon)) {
            throw std::runtime_error("make_gallery_icon: " + error->message);
        }
        model.icon = std::get<wui::TextureHandle>(icon);
        model.restore_count = 7;
    }

    wui::DrawPacket packet(bool new_glyph, std::vector<wui::InputEvent> events = {}) {
        wui::FrameInput input;
        input.viewport = {0, 0, 640, 400};
        input.events = std::move(events);
        auto begun = context->begin_frame(input);
        if (auto* error = std::get_if<wui::Error>(&begun)) {
            throw std::runtime_error("begin_frame: " + error->message);
        }
        auto frame = std::get<wui::Frame>(std::move(begun));
        // Emit the new glyph first so it stays visible above the full gallery.
        if (new_glyph) frame.text("Yeni glif: ğ");
        build_gallery(frame, model);
        auto finished = frame.finish();
        if (auto* error = std::get_if<wui::Error>(&finished)) {
            throw std::runtime_error("finish gallery: " + error->message);
        }
        return std::get<wui::DrawPacket>(std::move(finished));
    }
};
#endif

} // namespace

TEST_CASE("synthetic caps reject missing shader index primitive raster and blend features") {
    auto caps = supported_caps();
    CHECK_FALSE(wui::d3d9::detail::validate_caps(caps).has_value());
    caps.DeviceType = D3DDEVTYPE_REF;
    rejects(caps, "HAL");
    caps = supported_caps();
    caps.VertexShaderVersion = D3DVS_VERSION(2, 0);
    rejects(caps, "VertexShaderVersion");
    caps = supported_caps();
    caps.PixelShaderVersion = D3DPS_VERSION(2, 0);
    rejects(caps, "PixelShaderVersion");
    caps = supported_caps();
    caps.MaxVertexIndex = 1;
    rejects(caps, "MaxVertexIndex");
    caps = supported_caps();
    caps.MaxPrimitiveCount = 0;
    rejects(caps, "MaxPrimitiveCount");
    caps = supported_caps();
    caps.RasterCaps = 0;
    rejects(caps, "SCISSORTEST");
    caps = supported_caps();
    caps.SrcBlendCaps = 0;
    rejects(caps, "SrcBlendCaps ONE");
    caps = supported_caps();
    caps.DestBlendCaps = 0;
    rejects(caps, "DestBlendCaps INVSRCALPHA");
    caps = supported_caps();
    caps.PrimitiveMiscCaps = 0;
    rejects(caps, "SEPARATEALPHABLEND");
}

TEST_CASE("synthetic caps reject unsupported texture dimensions alpha and filtering") {
    auto caps = supported_caps();
    caps.MaxTextureWidth = 0;
    rejects(caps, "MaxTextureWidth");
    caps = supported_caps();
    caps.MaxTextureHeight = 0;
    rejects(caps, "MaxTextureHeight");
    caps = supported_caps();
    caps.TextureCaps = 0;
    rejects(caps, "TEXTURECAPS_ALPHA");
    caps = supported_caps();
    caps.TextureCaps |= D3DPTEXTURECAPS_POW2;
    rejects(caps, "NONPOW2CONDITIONAL");
    caps.TextureCaps |= D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
    CHECK_FALSE(wui::d3d9::detail::validate_caps(caps).has_value());
    caps.TextureCaps |= D3DPTEXTURECAPS_SQUAREONLY;
    rejects(caps, "SQUAREONLY");
    caps = supported_caps();
    caps.TextureFilterCaps = D3DPTFILTERCAPS_MINFLINEAR;
    rejects(caps, "MAGFLINEAR");
    caps = supported_caps();
    caps.TextureFilterCaps = D3DPTFILTERCAPS_MAGFLINEAR;
    rejects(caps, "MINFLINEAR");
}

TEST_CASE("null D3D9 device is a typed invalid argument") {
    const auto result = wui::d3d9::Renderer::create(nullptr);
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
}

TEST_CASE("native HAL reports capabilities and initializes renderer") {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"wui d3d9 capability probe",
            WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    REQUIRE(window.handle != nullptr);
    Microsoft::WRL::ComPtr<IDirect3D9> api;
    api.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    REQUIRE(api != nullptr);

    D3DPRESENT_PARAMETERS presentation{};
    presentation.Windowed = TRUE;
    presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
    presentation.BackBufferFormat = D3DFMT_UNKNOWN;
    presentation.BackBufferWidth = 128;
    presentation.BackBufferHeight = 128;
    presentation.hDeviceWindow = window.handle;
    Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
    const auto created = api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        window.handle, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &presentation, &device);
    INFO("D3D9 HAL CreateDevice hr=" << static_cast<unsigned long>(created));
    REQUIRE(SUCCEEDED(created));
    D3DADAPTER_IDENTIFIER9 adapter{};
    REQUIRE(SUCCEEDED(api->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapter)));
    D3DCAPS9 caps{};
    REQUIRE(SUCCEEDED(device->GetDeviceCaps(&caps)));
    std::cout << "D3D9 native HAL: " << adapter.Description
              << ", VS=" << D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion) << '.'
              << D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion)
              << ", PS=" << D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion) << '.'
              << D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion)
              << ", MaxVertexIndex=" << caps.MaxVertexIndex
              << ", MaxPrimitiveCount=" << caps.MaxPrimitiveCount
              << ", MaxTexture=" << caps.MaxTextureWidth << 'x' << caps.MaxTextureHeight
              << std::endl;
    std::cout << "D3D9 texture/blend caps: alpha="
              << !!(caps.TextureCaps & D3DPTEXTURECAPS_ALPHA)
              << ", linear min/mag="
              << !!(caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) << '/'
              << !!(caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR)
              << ", scissor=" << !!(caps.RasterCaps & D3DPRASTERCAPS_SCISSORTEST)
              << ", ONE/INVSRCALPHA="
              << !!(caps.SrcBlendCaps & D3DPBLENDCAPS_ONE) << '/'
              << !!(caps.DestBlendCaps & D3DPBLENDCAPS_INVSRCALPHA)
              << ", separate alpha="
              << !!(caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND)
              << std::endl;

    auto result = wui::d3d9::Renderer::create(device.Get());
    if (const auto* error = std::get_if<wui::Error>(&result)) {
        std::cout << "D3D9 renderer creation failed: " << error->message << std::endl;
    }
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d9::Renderer>>(result));
    CHECK(std::get<std::unique_ptr<wui::d3d9::Renderer>>(result) != nullptr);
    std::cout << "D3D9 A8R8G8B8 RGBA upload and EVENT query accepted" << std::endl;
}

TEST_CASE("D3D9 renders shared packet at scales 1 and 2 with pixel-center alignment") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    for (const auto scale : {1.0f, 2.0f}) {
        const auto extent = static_cast<std::uint32_t>(128 * scale);
        const auto target = create_target(native.device.Get(), extent, extent);
        const auto original = wui::test::reference_packet(1, scale);
        const auto first = render_and_read(native.device.Get(), *renderer, original, target);
        const auto factor = static_cast<std::uint32_t>(scale);
        // Bilinear filtering mixes adjacent checker texels at these pixel centers.
        const int bright = factor == 1 ? 225 : 239;
        const int dark = factor == 1 ? 30 : 16;
        check_pixel(first.at(12 * factor, 12 * factor), 255, 0, 0, 255);
        check_pixel(first.at(30 * factor, 30 * factor), 128, 128, 0, 255);
        check_pixel(first.at(70 * factor, 60 * factor), 26, 26, 26, 255);
        check_pixel(first.at(100 * factor, 100 * factor), 26, 26, 26, 255);
        check_pixel(first.at(85 * factor, 77 * factor), 0, 0, 255, 255);
        check_pixel(first.at(96 * factor - 1, 80 * factor), 0, 0, 255, 255);
        check_pixel(first.at(96 * factor, 80 * factor), 26, 26, 26, 255);
        check_pixel(first.at(76 * factor, 12 * factor),
            bright, bright, bright, 255, 3);
        check_pixel(first.at(84 * factor, 12 * factor), dark, dark, dark, 255, 3);

        const auto revised = render_and_read(native.device.Get(), *renderer,
            wui::test::reference_packet(2, scale), target);
        check_pixel(revised.at(76 * factor, 12 * factor), bright, 1, 1, 255, 3);
        check_pixel(revised.at(84 * factor, 12 * factor), dark, dark, dark, 255, 3);
        check_pixel(revised.at(30 * factor, 30 * factor), 128, 128, 0, 255);
        if (factor == 1) {
            REQUIRE_FALSE(wui::test::write_ppm(first, "d3d9-reference-rev1.ppm").has_value());
            REQUIRE_FALSE(wui::test::write_ppm(revised, "d3d9-reference-rev2.ppm").has_value());
        }
        const auto retained = render_and_read(native.device.Get(), *renderer,
            original, target);
        check_pixel(retained.at(76 * factor, 12 * factor),
            bright, bright, bright, 255, 3);
    }
}

TEST_CASE("D3D9 converts straight and premultiplied sampled alpha exactly once") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    const auto target = create_target(native.device.Get(), 8, 8);
    for (const auto alpha : {wui::AlphaMode::straight, wui::AlphaMode::premultiplied}) {
        auto packet = std::make_shared<wui::PacketData>();
        packet->viewport = {0, 0, 8, 8};
        packet->vertices = {
            {{0, 0}, {0, 0}, {1, 1, 1, 1}},
            {{8, 0}, {1, 0}, {1, 1, 1, 1}},
            {{0, 8}, {0, 1}, {1, 1, 1, 1}},
            {{8, 8}, {1, 1}, {1, 1, 1, 1}},
        };
        packet->indices = {0, 1, 2, 2, 1, 3};
        auto texture = std::make_shared<wui::TextureSnapshot>();
        texture->width = texture->height = 1;
        texture->alpha = alpha;
        texture->handle.slot = alpha == wui::AlphaMode::straight ? 1 : 2;
        texture->rgba = {
            alpha == wui::AlphaMode::straight ? std::byte{255} : std::byte{128},
            std::byte{0}, std::byte{0}, std::byte{128}
        };
        packet->textures.push_back(std::move(texture));
        packet->batches.push_back({0, 6, 0, 0, {0, 0, 8, 8}});
        REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
        REQUIRE(SUCCEEDED(native.device->ColorFill(
            target.color.Get(), nullptr, D3DCOLOR_ARGB(255, 0, 0, 0))));
        const auto image = render_and_read(native.device.Get(), *renderer, packet, target);
        check_pixel(image.at(4, 4), 128, 0, 0, 255, 3);
    }
}

TEST_CASE("D3D9 renders triangles beyond the 16-bit vertex boundary") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    auto packet = std::make_shared<wui::PacketData>();
    packet->viewport = {0, 0, 128, 128};
    packet->vertices.reserve(70002);
    packet->indices.reserve(70002);
    for (std::uint32_t i = 0; i < 70002; ++i) {
        const bool last = i >= 69999;
        const auto corner = i % 3;
        const wui::Vec2 position = last
            ? (corner == 0 ? wui::Vec2{80, 80} :
               corner == 1 ? wui::Vec2{104, 80} : wui::Vec2{80, 104})
            : (corner == 0 ? wui::Vec2{8, 8} :
               corner == 1 ? wui::Vec2{40, 8} : wui::Vec2{8, 40});
        packet->vertices.push_back({position, {0, 0},
            last ? wui::Color{0, 0, 1, 1} : wui::Color{1, 0, 0, 1}});
        packet->indices.push_back(i);
    }
    auto white = std::make_shared<wui::TextureSnapshot>();
    white->width = white->height = 1;
    white->rgba = {std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    packet->textures.push_back(white);
    packet->batches.push_back({0, 70002, 0, 0, {0, 0, 128, 128}});
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    auto target = create_target(native.device.Get(), 128, 128);
    const auto image = render_and_read(native.device.Get(), *renderer, packet, target);
    check_pixel(image.at(12, 12), 255, 0, 0, 255);
    check_pixel(image.at(84, 84), 0, 0, 255, 255);
}

TEST_CASE("D3D9 restores host bindings viewport and transforms on success and late failure") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    auto host = create_target(native.device.Get(), 128, 128);
    auto draw = create_target(native.device.Get(), 128, 128);
    ComPtr<IDirect3DTexture9> host_texture;
    REQUIRE(SUCCEEDED(native.device->CreateTexture(1, 1, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &host_texture, nullptr)));
    REQUIRE(SUCCEEDED(native.device->SetRenderTarget(0, host.color.Get())));
    D3DCAPS9 mrt_caps{};
    REQUIRE(SUCCEEDED(native.device->GetDeviceCaps(&mrt_caps)));
    Readback extra_target;
    if (mrt_caps.NumSimultaneousRTs > 1) {
        extra_target = create_target(native.device.Get(), 128, 128);
        REQUIRE(SUCCEEDED(native.device->SetRenderTarget(1, extra_target.color.Get())));
    }
    ComPtr<IDirect3DSurface9> host_depth;
    REQUIRE(SUCCEEDED(native.device->CreateDepthStencilSurface(128, 128,
        D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &host_depth, nullptr)));
    REQUIRE(SUCCEEDED(native.device->SetDepthStencilSurface(host_depth.Get())));
    ComPtr<IDirect3DVertexBuffer9> host_stream;
    ComPtr<IDirect3DIndexBuffer9> host_indices;
    ComPtr<IDirect3DVertexDeclaration9> host_declaration;
    REQUIRE(SUCCEEDED(native.device->CreateVertexBuffer(256,
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
        &host_stream, nullptr)));
    REQUIRE(SUCCEEDED(native.device->CreateIndexBuffer(64,
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
        D3DPOOL_DEFAULT, &host_indices, nullptr)));
    const D3DVERTEXELEMENT9 host_elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    REQUIRE(SUCCEEDED(native.device->CreateVertexDeclaration(
        host_elements, &host_declaration)));
    REQUIRE(SUCCEEDED(native.device->SetStreamSource(
        0, host_stream.Get(), 16, sizeof(wui::Vertex))));
    REQUIRE(SUCCEEDED(native.device->SetIndices(host_indices.Get())));
    REQUIRE(SUCCEEDED(native.device->SetVertexDeclaration(host_declaration.Get())));
    auto compile_host_shader = [](const char* source, const char* profile) {
        ComPtr<ID3DBlob> code;
        REQUIRE(SUCCEEDED(D3DCompile(source, std::strlen(source), nullptr, nullptr,
            nullptr, "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, nullptr)));
        return code;
    };
    const auto vertex_code = compile_host_shader(
        "float4 main(float2 p : POSITION0) : POSITION0 { return float4(p, 0, 1); }",
        "vs_3_0");
    const auto pixel_code = compile_host_shader(
        "float4 main() : COLOR0 { return float4(0, 1, 0, 1); }", "ps_3_0");
    ComPtr<IDirect3DVertexShader9> host_vs;
    ComPtr<IDirect3DPixelShader9> host_ps;
    REQUIRE(SUCCEEDED(native.device->CreateVertexShader(
        static_cast<const DWORD*>(vertex_code->GetBufferPointer()), &host_vs)));
    REQUIRE(SUCCEEDED(native.device->CreatePixelShader(
        static_cast<const DWORD*>(pixel_code->GetBufferPointer()), &host_ps)));
    REQUIRE(SUCCEEDED(native.device->SetVertexShader(host_vs.Get())));
    REQUIRE(SUCCEEDED(native.device->SetPixelShader(host_ps.Get())));
    REQUIRE(SUCCEEDED(native.device->SetTexture(0, host_texture.Get())));
    REQUIRE(SUCCEEDED(native.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW)));
    REQUIRE(SUCCEEDED(native.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE)));
    REQUIRE(SUCCEEDED(native.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP)));
    const D3DVIEWPORT9 viewport{8, 9, 40, 50, 0.2f, 0.9f};
    REQUIRE(SUCCEEDED(native.device->SetViewport(&viewport)));
    const RECT scissor{11, 12, 41, 42};
    REQUIRE(SUCCEEDED(native.device->SetScissorRect(&scissor)));
    D3DMATRIX world{};
    world._11 = world._22 = world._33 = world._44 = 1.0f;
    world._41 = 5.0f;
    REQUIRE(SUCCEEDED(native.device->SetTransform(D3DTS_WORLD, &world)));
    const float host_vertex_constants[4] = {5, 6, 7, 8};
    const float host_pixel_constants[4] = {9, 10, 11, 12};
    REQUIRE(SUCCEEDED(native.device->SetVertexShaderConstantF(
        0, host_vertex_constants, 1)));
    REQUIRE(SUCCEEDED(native.device->SetPixelShaderConstantF(
        2, host_pixel_constants, 1)));
    HostState before;
    before.capture(native.device.Get());

    const auto packet = wui::test::reference_packet(1);
    REQUIRE(SUCCEEDED(native.device->BeginScene()));
    const auto status = renderer->render(packet, {draw.color.Get(), 128, 128});
    REQUIRE(SUCCEEDED(native.device->EndScene()));
    REQUIRE_FALSE(status.has_value());
    HostState after;
    after.capture(native.device.Get());
    after.check_equal(before);

    D3DCAPS9 caps{};
    REQUIRE(SUCCEEDED(native.device->GetDeviceCaps(&caps)));
    auto oversized = std::make_shared<wui::PacketData>(*packet);
    for (auto& texture : oversized->textures) {
        auto huge = std::make_shared<wui::TextureSnapshot>(*texture);
        huge->width = caps.MaxTextureWidth + 1;
        huge->height = 1;
        huge->rgba.resize(static_cast<std::size_t>(huge->width) * 4, std::byte{255});
        texture = std::move(huge);
    }
    REQUIRE_FALSE(wui::validate_packet(*oversized).has_value());
    REQUIRE(SUCCEEDED(native.device->BeginScene()));
    const auto failed = renderer->render(oversized, {draw.color.Get(), 128, 128});
    REQUIRE(SUCCEEDED(native.device->EndScene()));
    REQUIRE(failed.has_value());
    CHECK(failed->code == wui::ErrorCode::unsupported);
    HostState after_failure;
    after_failure.capture(native.device.Get());
    after_failure.check_equal(before);

    REQUIRE(SUCCEEDED(native.device->BeginScene()));
    const auto opted_out = renderer->render(packet, {draw.color.Get(), 128, 128},
        wui::d3d9::StatePolicy::caller_restores);
    REQUIRE(SUCCEEDED(native.device->EndScene()));
    REQUIRE_FALSE(opted_out.has_value());
    ComPtr<IDirect3DSurface9> bound;
    REQUIRE(SUCCEEDED(native.device->GetRenderTarget(0, &bound)));
    CHECK(bound.Get() == draw.color.Get());
}

TEST_CASE("D3D9 busy threshold retains packets until nonflushing event retirement") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    const auto target = create_target(native.device.Get(), 128, 128);
    auto first = wui::test::reference_packet(1);
    const std::weak_ptr<const wui::PacketData> lease = first;
    const auto later = wui::test::reference_packet(2);
    REQUIRE(SUCCEEDED(native.device->BeginScene()));
    for (int i = 0; i < 3; ++i) {
        const auto error = renderer->render(i == 0 ? first : later,
            {target.color.Get(), 128, 128});
        REQUIRE_FALSE(error.has_value());
    }
    first.reset();
    CHECK_FALSE(lease.expired());
    const auto blocked = renderer->render(later, {target.color.Get(), 128, 128});
    REQUIRE(blocked.has_value());
    CHECK(blocked->code == wui::ErrorCode::busy);
    REQUIRE(SUCCEEDED(native.device->EndScene()));

    const auto start = std::chrono::steady_clock::now();
    const auto polling = renderer->collect();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(2));
    if (polling) CHECK(polling->code == wui::ErrorCode::busy);
    const auto image = read_back(native.device.Get(), target);
    check_pixel(image.at(76, 12), 225, 1, 1, 255, 3);
    REQUIRE_FALSE(renderer->collect().has_value());
    CHECK(lease.expired());
    const auto recovered = render_and_read(native.device.Get(), *renderer, later, target);
    check_pixel(recovered.at(76, 12), 225, 1, 1, 255, 3);
}

TEST_CASE("D3D9 reset hooks gate rendering and recreate resources through ten real Reset cycles") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    Readback target = create_target(native.device.Get(), 128, 128);
    auto packet = wui::test::reference_packet(1);
    for (int cycle = 0; cycle < 10; ++cycle) {
        const auto before = render_and_read(native.device.Get(), *renderer, packet, target);
        check_pixel(before.at(12 * (target.width / 128), 12 * (target.width / 128)),
            255, 0, 0, 255);
        renderer->before_reset();
        const auto gated = renderer->render(packet,
            {target.color.Get(), target.width, target.height});
        REQUIRE(gated.has_value());
        CHECK(gated->code == wui::ErrorCode::device_lost);
        target = {};
        native.presentation.BackBufferWidth =
            native.presentation.BackBufferHeight = cycle % 2 ? 128 : 256;
        const HRESULT reset = native.device->Reset(&native.presentation);
        INFO("D3D9 Reset cycle=" << cycle << " hr=" << static_cast<unsigned long>(reset));
        REQUIRE(SUCCEEDED(reset));
        REQUIRE_FALSE(renderer->after_reset().has_value());
        const auto scale = cycle % 2 ? 1.0f : 2.0f;
        packet = wui::test::reference_packet(1, scale);
        const auto extent = static_cast<std::uint32_t>(128 * scale);
        target = create_target(native.device.Get(), extent, extent);
        const auto restored = render_and_read(native.device.Get(), *renderer, packet, target);
        check_pixel(restored.at(12 * static_cast<unsigned>(scale),
            12 * static_cast<unsigned>(scale)), 255, 0, 0, 255);
    }
    target = {};
    renderer.reset(); // UI objects die before the host device.
}

#ifdef WUI_PROBE_TEXT
TEST_CASE("D3D9 retains real gallery atlas revisions and text across reset") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    GalleryFixture gallery;
    wui::Vec2 checkbox_point{};
    bool found_checkbox = false;
    {
        const auto baseline = gallery.packet(false);
        for (const auto& semantic : baseline->semantics) {
            if (semantic.role == wui::Role::checkbox &&
                semantic.label == "Notifications") {
                checkbox_point = {semantic.bounds.x + semantic.bounds.w / 2,
                    semantic.bounds.y + semantic.bounds.h / 2};
                found_checkbox = true;
            }
        }
    }
    REQUIRE(found_checkbox);
    gallery.packet(false, {{wui::EventKind::pointer_down, checkbox_point}});
    gallery.packet(false, {{wui::EventKind::pointer_up, checkbox_point}});
    REQUIRE_FALSE(gallery.model.notifications);
    const auto original = gallery.packet(false);
    const auto newer = gallery.packet(true); // frame.text("Yeni glif: ğ")
    REQUIRE_FALSE(wui::validate_packet(*original).has_value());
    REQUIRE_FALSE(wui::validate_packet(*newer).has_value());
    bool atlas_updated = false;
    for (const auto& old_texture : original->textures) {
        for (const auto& new_texture : newer->textures) {
            if (old_texture->handle == new_texture->handle &&
                old_texture->revision < new_texture->revision) atlas_updated = true;
        }
    }
    CHECK(atlas_updated);
    bool checkbox_found = false;
    for (const auto& semantic : original->semantics) {
        if (semantic.role == wui::Role::checkbox && semantic.label == "Notifications") {
            checkbox_found = true;
            CHECK_FALSE(semantic.selected);
        }
    }
    CHECK(checkbox_found);

    Readback target = create_target(native.device.Get(), 640, 400);
    const auto old_image = render_and_read(native.device.Get(), *renderer, original, target);
    const auto new_image = render_and_read(native.device.Get(), *renderer, newer, target);
    REQUIRE_FALSE(wui::test::write_ppm(old_image, "d3d9-gallery-old.ppm").has_value());
    REQUIRE_FALSE(wui::test::write_ppm(new_image, "d3d9-gallery-new.ppm").has_value());
    auto differing_pixels = [](const wui::test::Image& a, const wui::test::Image& b) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < a.pixels.size(); ++i) {
            const auto left = a.pixels[i], right = b.pixels[i];
            if (left.r != right.r || left.g != right.g || left.b != right.b ||
                left.a != right.a) ++count;
        }
        return count;
    };
    CHECK(differing_pixels(old_image, new_image) > 0);
    const auto retained = render_and_read(native.device.Get(), *renderer, original, target);
    CHECK(differing_pixels(old_image, retained) == 0);

    for (int cycle = 0; cycle < 10; ++cycle) {
        renderer->before_reset();
        target = {};
        native.presentation.BackBufferWidth = cycle % 2 ? 320 : 256;
        native.presentation.BackBufferHeight = cycle % 2 ? 240 : 256;
        const HRESULT reset = native.device->Reset(&native.presentation);
        INFO("D3D9 gallery Reset cycle=" << cycle <<
            " hr=" << static_cast<unsigned long>(reset));
        REQUIRE(SUCCEEDED(reset));
        REQUIRE_FALSE(renderer->after_reset().has_value());
        target = create_target(native.device.Get(), 640, 400);
        const auto old_recreated = render_and_read(native.device.Get(), *renderer,
            original, target);
        const auto new_recreated = render_and_read(native.device.Get(), *renderer,
            newer, target);
        CHECK(differing_pixels(old_image, old_recreated) == 0);
        CHECK(differing_pixels(new_image, new_recreated) == 0);
        CHECK_FALSE(gallery.model.notifications);
        CHECK(gallery.model.restore_count == 7);
    }
    const auto rebuilt = gallery.packet(false);
    bool checkbox_remains_off = false;
    for (const auto& semantic : rebuilt->semantics) {
        if (semantic.role == wui::Role::checkbox && semantic.label == "Notifications") {
            checkbox_remains_off = !semantic.selected;
        }
    }
    CHECK(checkbox_remains_off);
}
#endif

TEST_CASE("D3D9 distinguishes distinct immutable snapshots with identical metadata") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    const auto target = create_target(native.device.Get(), 8, 8);
    auto white = std::make_shared<wui::PacketData>();
    white->viewport = {0, 0, 8, 8};
    white->vertices = {
        {{0, 0}, {0, 0}, {1, 1, 1, 1}},
        {{8, 0}, {1, 0}, {1, 1, 1, 1}},
        {{0, 8}, {0, 1}, {1, 1, 1, 1}},
        {{8, 8}, {1, 1}, {1, 1, 1, 1}}
    };
    white->indices = {0, 1, 2, 2, 1, 3};
    auto first_texture = std::make_shared<wui::TextureSnapshot>();
    first_texture->width = first_texture->height = 1;
    first_texture->rgba = {
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    white->textures.push_back(first_texture);
    white->batches.push_back({0, 6, 0, 0, {0, 0, 8, 8}});
    auto red = std::make_shared<wui::PacketData>(*white);
    auto second_texture = std::make_shared<wui::TextureSnapshot>(*first_texture);
    second_texture->rgba = {
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
    red->textures[0] = second_texture;
    REQUIRE(first_texture != second_texture);
    CHECK(first_texture->handle == second_texture->handle);
    CHECK(first_texture->revision == second_texture->revision);
    REQUIRE_FALSE(wui::validate_packet(*white).has_value());
    REQUIRE_FALSE(wui::validate_packet(*red).has_value());

    check_pixel(render_and_read(native.device.Get(), *renderer, white, target).at(4, 4),
        255, 255, 255, 255, 3);
    check_pixel(render_and_read(native.device.Get(), *renderer, red, target).at(4, 4),
        255, 0, 0, 255, 3);
    check_pixel(render_and_read(native.device.Get(), *renderer, white, target).at(4, 4),
        255, 255, 255, 255, 3);
}

TEST_CASE("D3D9 Reset releases an old bound UI stream after buffer growth") {
    Native native;
    auto renderer = renderer_for(native.device.Get());
    Readback target = create_target(native.device.Get(), 128, 128);
    auto small_packet = std::make_shared<wui::PacketData>();
    small_packet->viewport = {0, 0, 128, 128};
    small_packet->vertices = {
        {{8, 8}, {0, 0}, {1, 0, 0, 1}},
        {{40, 8}, {0, 0}, {1, 0, 0, 1}},
        {{8, 40}, {0, 0}, {1, 0, 0, 1}}
    };
    small_packet->indices = {0, 1, 2};
    auto white = std::make_shared<wui::TextureSnapshot>();
    white->width = white->height = 1;
    white->rgba = {
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    small_packet->textures.push_back(white);
    small_packet->batches.push_back({0, 3, 0, 0, {0, 0, 128, 128}});
    const auto large = wui::test::reference_packet(1);
    REQUIRE_FALSE(wui::validate_packet(*small_packet).has_value());
    REQUIRE(large->vertices.size() > small_packet->vertices.size());
    REQUIRE(large->indices.size() > small_packet->indices.size());

    REQUIRE(SUCCEEDED(native.device->BeginScene()));
    const auto first = renderer->render(small_packet, {target.color.Get(), 128, 128},
        wui::d3d9::StatePolicy::caller_restores);
    REQUIRE(SUCCEEDED(native.device->EndScene()));
    REQUIRE_FALSE(first.has_value());
    ComPtr<IDirect3DVertexBuffer9> old_stream;
    ComPtr<IDirect3DIndexBuffer9> old_indices;
    UINT offset = 0, stride = 0;
    REQUIRE(SUCCEEDED(native.device->GetStreamSource(0, &old_stream, &offset, &stride)));
    REQUIRE(SUCCEEDED(native.device->GetIndices(&old_indices)));
    REQUIRE(old_stream != nullptr);
    REQUIRE(old_indices != nullptr);
    read_back(native.device.Get(), target);
    REQUIRE_FALSE(renderer->collect().has_value());

    const auto second = render_and_read(native.device.Get(), *renderer, large, target);
    check_pixel(second.at(12, 12), 255, 0, 0, 255);
    ComPtr<IDirect3DVertexBuffer9> still_bound;
    ComPtr<IDirect3DIndexBuffer9> still_bound_indices;
    REQUIRE(SUCCEEDED(native.device->GetStreamSource(0, &still_bound, &offset, &stride)));
    REQUIRE(SUCCEEDED(native.device->GetIndices(&still_bound_indices)));
    CHECK(still_bound.Get() == old_stream.Get());
    CHECK(still_bound_indices.Get() == old_indices.Get());
    renderer->before_reset();
    ComPtr<IDirect3DSurface9> backbuffer;
    REQUIRE(SUCCEEDED(native.device->GetBackBuffer(
        0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)));
    REQUIRE(SUCCEEDED(native.device->SetRenderTarget(0, backbuffer.Get())));
    backbuffer.Reset();
    target = {};
    still_bound.Reset();
    still_bound_indices.Reset();
    old_stream.Reset();
    old_indices.Reset();
    const HRESULT reset = native.device->Reset(&native.presentation);
    INFO("D3D9 Reset after old bound UI buffers hr=" << static_cast<unsigned long>(reset));
    REQUIRE(SUCCEEDED(reset));
    REQUIRE_FALSE(renderer->after_reset().has_value());
    target = create_target(native.device.Get(), 128, 128);
    check_pixel(render_and_read(native.device.Get(), *renderer, large, target).at(12, 12),
        255, 0, 0, 255);
}
