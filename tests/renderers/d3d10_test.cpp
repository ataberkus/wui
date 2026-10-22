#include <doctest/doctest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d10_1.h>
#include <d3d10sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <wui/backends/d3d10.hpp>

#include "../support/native_image.hpp"
#include "../support/packet_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

// Hardware is first-class; WARP runs every case as well.
constexpr D3D10_DRIVER_TYPE kDrivers[] = {
    D3D10_DRIVER_TYPE_HARDWARE, D3D10_DRIVER_TYPE_WARP};

// std::string, not const char*: doctest logs raw pointers as booleans.
std::string driver_name(D3D10_DRIVER_TYPE driver) {
    return driver == D3D10_DRIVER_TYPE_HARDWARE ? "hardware" : "warp";
}

// The debug layer is requested only in debug builds and only when the SDK
// layer DLL is installed; every debug device must then stay message-free.
bool debug_layer_requested() {
#ifdef _DEBUG
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    std::wstring path(system_dir, length);
    path += L"\\d3d10SDKLayers.dll";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return false;
#endif
}

std::string narrow(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

struct Device {
    ComPtr<ID3D10Device1> device;
    ComPtr<ID3D10InfoQueue> info;
    D3D10_DRIVER_TYPE driver = D3D10_DRIVER_TYPE_HARDWARE;
    std::string adapter;
};

HRESULT create_raw(D3D10_DRIVER_TYPE driver, D3D10_FEATURE_LEVEL1 level,
    UINT flags, ComPtr<ID3D10Device1>& out) {
    return D3D10CreateDevice1(nullptr, driver, nullptr, flags, level,
        D3D10_1_SDK_VERSION, out.ReleaseAndGetAddressOf());
}

Device create_device(D3D10_DRIVER_TYPE driver) {
    Device out;
    out.driver = driver;
    UINT flags = D3D10_CREATE_DEVICE_BGRA_SUPPORT;
    if (debug_layer_requested()) flags |= D3D10_CREATE_DEVICE_DEBUG;
    const HRESULT hr = create_raw(driver, D3D10_FEATURE_LEVEL_10_0, flags, out.device);
    if (FAILED(hr)) {
        throw std::runtime_error(std::string("D3D10CreateDevice1 ") +
            driver_name(driver) + " failed, hr=" + std::to_string(hr));
    }
    if (out.device->GetFeatureLevel() < D3D10_FEATURE_LEVEL_10_0) {
        throw std::runtime_error("device below feature level 10_0");
    }
    if ((flags & D3D10_CREATE_DEVICE_DEBUG) != 0) {
        if (FAILED(out.device.As(&out.info))) {
            throw std::runtime_error("debug device exposes no ID3D10InfoQueue");
        }
        // Record validation messages instead of breaking into the debugger.
        out.info->SetBreakOnSeverity(D3D10_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        out.info->SetBreakOnSeverity(D3D10_MESSAGE_SEVERITY_ERROR, FALSE);
        out.info->SetBreakOnSeverity(D3D10_MESSAGE_SEVERITY_WARNING, FALSE);
        out.info->ClearStoredMessages();
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(out.device.As(&dxgi)) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        out.adapter = narrow(desc.Description);
    }
    return out;
}

struct MessageCounts {
    UINT64 errors = 0, warnings = 0;
};

// Counts stored warnings and errors (corruption counts as an error),
// logging each one.
MessageCounts count_messages(const Device& dev, const char* stage) {
    MessageCounts counts;
    const UINT64 stored = dev.info->GetNumStoredMessages();
    for (UINT64 i = 0; i < stored; ++i) {
        SIZE_T bytes = 0;
        if (FAILED(dev.info->GetMessage(i, nullptr, &bytes)) || bytes == 0) continue;
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D10_MESSAGE*>(storage.data());
        if (FAILED(dev.info->GetMessage(i, message, &bytes))) continue;
        if (message->Severity == D3D10_MESSAGE_SEVERITY_CORRUPTION ||
            message->Severity == D3D10_MESSAGE_SEVERITY_ERROR) {
            ++counts.errors;
            MESSAGE(std::string(stage), " D3D10 error: ", std::string(message->pDescription));
        } else if (message->Severity == D3D10_MESSAGE_SEVERITY_WARNING) {
            ++counts.warnings;
            MESSAGE(std::string(stage), " D3D10 warning: ", std::string(message->pDescription));
        }
    }
    return counts;
}

// Requires zero warnings and errors since creation or the previous check.
// Release builds have no debug device and skip.
void check_clean(const Device& dev, const char* stage) {
    if (!dev.info) return;
    const auto counts = count_messages(dev, stage);
    INFO(std::string(stage), " on ", driver_name(dev.driver));
    CHECK(counts.errors == 0);
    CHECK(counts.warnings == 0);
    dev.info->ClearStoredMessages();
}

void wait_for_gpu(ID3D10Device* device, const char* stage) {
    D3D10_QUERY_DESC desc{};
    desc.Query = D3D10_QUERY_EVENT;
    ComPtr<ID3D10Query> completion;
    const HRESULT created = device->CreateQuery(&desc, &completion);
    if (FAILED(created)) {
        throw std::runtime_error(std::string(stage) +
            ": CreateQuery(EVENT) failed, hr=" + std::to_string(created));
    }
    completion->End();
    device->Flush(); // The host submits once; polling must never flush.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        BOOL complete = FALSE;
        const HRESULT hr = completion->GetData(
            &complete, sizeof(complete), D3D10_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && complete) return;
        if (FAILED(hr)) {
            throw std::runtime_error(std::string(stage) +
                ": GetData(EVENT) failed, hr=" + std::to_string(hr));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string(stage) +
                ": GPU retirement timed out after 5 seconds; device removal hr=" +
                std::to_string(device->GetDeviceRemovedReason()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct ReadbackTarget {
    ComPtr<ID3D10Texture2D> rt;
    ComPtr<ID3D10RenderTargetView> rtv;
    ComPtr<ID3D10Texture2D> staging;
    std::uint32_t width = 0, height = 0;
};

ReadbackTarget create_target(ID3D10Device* device, std::uint32_t width,
    std::uint32_t height, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    ReadbackTarget target;
    target.width = width;
    target.height = height;
    D3D10_TEXTURE2D_DESC color{};
    color.Width = width;
    color.Height = height;
    color.MipLevels = 1;
    color.ArraySize = 1;
    color.Format = format;
    color.SampleDesc.Count = 1;
    color.Usage = D3D10_USAGE_DEFAULT;
    color.BindFlags = D3D10_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&color, nullptr, &target.rt);
    if (FAILED(hr)) throw std::runtime_error("CreateTexture2D RT failed");
    hr = device->CreateRenderTargetView(target.rt.Get(), nullptr, &target.rtv);
    if (FAILED(hr)) throw std::runtime_error("CreateRenderTargetView failed");
    D3D10_TEXTURE2D_DESC staging = color;
    staging.Usage = D3D10_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&staging, nullptr, &target.staging);
    if (FAILED(hr)) throw std::runtime_error("CreateTexture2D staging failed");
    return target;
}

void clear(ID3D10Device* device, const ReadbackTarget& target, float r, float g,
    float b, float a) {
    const FLOAT color[4] = {r, g, b, a};
    device->ClearRenderTargetView(target.rtv.Get(), color);
}

wui::test::Image read_back(ID3D10Device* device, const ReadbackTarget& target) {
    device->CopyResource(target.staging.Get(), target.rt.Get());
    wait_for_gpu(device, "readback copy");
    D3D10_MAPPED_TEXTURE2D mapped{};
    const HRESULT hr = target.staging->Map(0, D3D10_MAP_READ, 0, &mapped);
    if (FAILED(hr)) throw std::runtime_error("Map staging failed");
    wui::test::Image image;
    image.width = target.width;
    image.height = target.height;
    image.pixels.resize(static_cast<std::size_t>(target.width) * target.height);
    const auto* rows = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        const auto* row = rows + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (std::uint32_t x = 0; x < target.width; ++x) {
            wui::test::Pixel pixel;
            pixel.r = row[x * 4];
            pixel.g = row[x * 4 + 1];
            pixel.b = row[x * 4 + 2];
            pixel.a = row[x * 4 + 3];
            image.pixels[static_cast<std::size_t>(y) * target.width + x] = pixel;
        }
    }
    target.staging->Unmap(0);
    return image;
}

void check_channel(std::uint8_t actual, std::uint8_t expected, int tolerance) {
    const int delta = static_cast<int>(actual) - static_cast<int>(expected);
    CHECK(std::abs(delta) <= tolerance);
}

void check_pixel(wui::test::Pixel actual, std::uint8_t r, std::uint8_t g, std::uint8_t b,
    std::uint8_t a, int tolerance = 2) {
    INFO("actual=(", int(actual.r), ",", int(actual.g), ",", int(actual.b), ",",
        int(actual.a), ")");
    check_channel(actual.r, r, tolerance);
    check_channel(actual.g, g, tolerance);
    check_channel(actual.b, b, tolerance);
    check_channel(actual.a, a, tolerance);
}

bool images_equal(const wui::test::Image& a, const wui::test::Image& b) {
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (a.pixels[i].r != b.pixels[i].r || a.pixels[i].g != b.pixels[i].g ||
            a.pixels[i].b != b.pixels[i].b || a.pixels[i].a != b.pixels[i].a) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<wui::d3d10::Renderer> make_renderer(ID3D10Device* device) {
    auto created = wui::d3d10::Renderer::create(device);
    if (auto* error = std::get_if<wui::Error>(&created)) {
        FAIL("D3D10 renderer creation failed: ", error->message);
    }
    return std::move(std::get<std::unique_ptr<wui::d3d10::Renderer>>(created));
}

// Renders on a target cleared to opaque black, drains the GPU and collects.
wui::test::Image render_and_read(Device& dev, wui::d3d10::Renderer& renderer,
    const wui::DrawPacket& packet, const ReadbackTarget& target) {
    clear(dev.device.Get(), target, 0, 0, 0, 1);
    const auto error = renderer.render(packet, {target.rtv.Get(), target.width, target.height});
    if (error) {
        FAIL("D3D10 render failed: code=", static_cast<int>(error->code), ", ",
            error->message);
    }
    auto image = read_back(dev.device.Get(), target);
    REQUIRE_FALSE(renderer.collect().has_value());
    return image;
}

template <class Fn>
void for_each_driver(Fn&& fn) {
    for (const auto driver : kDrivers) {
        INFO("driver: ", driver_name(driver));
        Device dev;
        try {
            dev = create_device(driver);
        } catch (const std::exception& error) {
            FAIL("D3D10 device creation failed: ", error.what());
        }
        fn(dev);
        check_clean(dev, "end of case");
    }
}

wui::DrawPacket alpha_packet(wui::AlphaMode alpha) {
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
    // Straight (255,0,0,128) and premultiplied (128,0,0,128) are both
    // half-alpha red and must resolve identically over black.
    texture->rgba = {
        alpha == wui::AlphaMode::straight ? std::byte{255} : std::byte{128},
        std::byte{0}, std::byte{0}, std::byte{128}};
    packet->textures.push_back(std::move(texture));
    packet->batches.push_back({0, 6, 0, 0, {0, 0, 8, 8}});
    return packet;
}

} // namespace

TEST_CASE("d3d10 rejects null and below-10_0 devices") {
    auto null_created = wui::d3d10::Renderer::create(nullptr);
    REQUIRE(std::holds_alternative<wui::Error>(null_created));
    CHECK(std::get<wui::Error>(null_created).code == wui::ErrorCode::invalid_argument);

    // A 10.1 runtime device at feature level 9_x is not 10_0-class. WARP
    // provides 9_x levels; when it cannot, the rejection is not exercised.
    ComPtr<ID3D10Device1> level9;
    const HRESULT hr = create_raw(D3D10_DRIVER_TYPE_WARP, D3D10_FEATURE_LEVEL_9_3,
        0, level9);
    if (FAILED(hr)) {
        MESSAGE("WARP feature level 9_3 unavailable (hr=", hr,
            "); below-10_0 rejection not exercised");
    } else {
        auto low = wui::d3d10::Renderer::create(level9.Get());
        REQUIRE(std::holds_alternative<wui::Error>(low));
        CHECK(std::get<wui::Error>(low).code == wui::ErrorCode::unsupported);
    }

    for_each_driver([&](Device& dev) {
        MESSAGE(driver_name(dev.driver), " adapter: ", dev.adapter,
            std::string(dev.info ? " (debug layer on)" : " (debug layer off)"));
        if (dev.info) {
            // Prove the info queue really records hazards: binding a
            // render target as a shader resource must warn. The queue is
            // then cleared so the renderer's own checks start from zero.
            check_clean(dev, "before liveness probe");
            auto hazard = create_target(dev.device.Get(), 4, 4);
            D3D10_TEXTURE2D_DESC desc{};
            hazard.rt->GetDesc(&desc);
            desc.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ComPtr<ID3D10Texture2D> both;
            REQUIRE(SUCCEEDED(dev.device->CreateTexture2D(&desc, nullptr, &both)));
            ComPtr<ID3D10RenderTargetView> both_rtv;
            ComPtr<ID3D10ShaderResourceView> both_srv;
            REQUIRE(SUCCEEDED(dev.device->CreateRenderTargetView(both.Get(), nullptr, &both_rtv)));
            REQUIRE(SUCCEEDED(
                dev.device->CreateShaderResourceView(both.Get(), nullptr, &both_srv)));
            dev.device->OMSetRenderTargets(1, both_rtv.GetAddressOf(), nullptr);
            dev.device->PSSetShaderResources(0, 1, both_srv.GetAddressOf());
            const auto probe = count_messages(dev, "liveness probe (expected)");
            CHECK(probe.errors + probe.warnings > 0);
            dev.device->ClearState();
            dev.info->ClearStoredMessages();
        }
        auto renderer = make_renderer(dev.device.Get());
        const auto null_recreate = renderer->recreate(nullptr);
        REQUIRE(null_recreate.has_value());
        CHECK(null_recreate->code == wui::ErrorCode::invalid_argument);
        if (level9) {
            // A rejected replacement leaves the renderer on its current device.
            const auto low_recreate = renderer->recreate(level9.Get());
            REQUIRE(low_recreate.has_value());
            CHECK(low_recreate->code == wui::ErrorCode::unsupported);
            auto target = create_target(dev.device.Get(), 128, 128);
            const auto image = render_and_read(
                dev, *renderer, wui::test::reference_packet(1), target);
            check_pixel(image.at(12, 12), 255, 0, 0, 255);
        }
        // A moved-from renderer reports invalid_frame instead of crashing.
        wui::d3d10::Renderer moved(std::move(*renderer));
        auto target = create_target(dev.device.Get(), 128, 128);
        const auto moved_error = renderer->render(
            wui::test::reference_packet(1), {target.rtv.Get(), 128, 128});
        REQUIRE(moved_error.has_value());
        CHECK(moved_error->code == wui::ErrorCode::invalid_frame);
        const auto moved_collect = renderer->collect();
        REQUIRE(moved_collect.has_value());
        CHECK(moved_collect->code == wui::ErrorCode::invalid_frame);
        REQUIRE_FALSE(moved.collect().has_value());
    });
}

TEST_CASE("d3d10 renders reference revisions 1 and 2") {
    for_each_driver([&](Device& dev) {
        auto renderer = make_renderer(dev.device.Get());
        auto target = create_target(dev.device.Get(), 128, 128);
        const auto first = wui::test::reference_packet(1, 1.0f);
        const auto image = render_and_read(dev, *renderer, first, target);
        // Opaque red rect interior, away from the green overlap.
        check_pixel(image.at(12, 12), 255, 0, 0, 255);
        // Half-alpha green over red resolves near (128,128,0).
        check_pixel(image.at(30, 30), 128, 128, 0, 255, 8);
        // Pixels outside the {80,72,16,16} scissor stay background (~26).
        check_pixel(image.at(70, 60), 26, 26, 26, 255, 8);
        check_pixel(image.at(100, 100), 26, 26, 26, 255, 8);
        check_pixel(image.at(84, 76), 0, 0, 255, 255, 8);
        check_pixel(image.at(12, 116), 26, 26, 26, 255, 8);
        check_pixel(image.at(75, 11), 255, 255, 255, 255, 8);
        check_pixel(image.at(76, 12), 225, 225, 225, 255, 3);

        // Revision 2 changes the checker's (0,0) texel to red.
        const auto second = wui::test::reference_packet(2, 1.0f);
        const auto revised = render_and_read(dev, *renderer, second, target);
        check_pixel(revised.at(75, 11), 255, 0, 0, 255, 8);
        check_pixel(revised.at(76, 12), 225, 1, 1, 255, 3);
        check_pixel(revised.at(84, 12), 30, 30, 30, 255, 3);
        CHECK_FALSE(images_equal(image, revised));
        const std::string prefix = std::string("d3d10-") + driver_name(dev.driver);
        CHECK_FALSE(wui::test::write_ppm(image, prefix + "-rev1.ppm").has_value());
        CHECK_FALSE(wui::test::write_ppm(revised, prefix + "-rev2.ppm").has_value());
    });
}

TEST_CASE("d3d10 does not share native textures between snapshots with equal handles") {
    // Two TextureStores can hand out the same handle slot, generation and
    // revision for different pixels; each packet must sample its own pixels.
    for_each_driver([&](Device& dev) {
        auto renderer = make_renderer(dev.device.Get());
        auto target = create_target(dev.device.Get(), 128, 128);
        const auto original = wui::test::reference_packet(1, 1.0f);
        auto copy = std::make_shared<wui::PacketData>(*original);
        bool replaced = false;
        for (auto& texture : copy->textures) {
            if (texture && texture->width == 4 && texture->height == 4) {
                auto blue = std::make_shared<wui::TextureSnapshot>(*texture);
                for (std::size_t i = 0; i < blue->rgba.size(); i += 4) {
                    blue->rgba[i + 0] = std::byte{0};
                    blue->rgba[i + 1] = std::byte{0};
                    blue->rgba[i + 2] = std::byte{255};
                    blue->rgba[i + 3] = std::byte{255};
                }
                texture = std::move(blue);
                replaced = true;
            }
        }
        REQUIRE(replaced);
        REQUIRE_FALSE(wui::validate_packet(*copy).has_value());
        const wui::DrawPacket other(std::move(copy));
        const auto first = render_and_read(dev, *renderer, original, target);
        const auto second = render_and_read(dev, *renderer, other, target);
        const auto third = render_and_read(dev, *renderer, original, target);
        check_pixel(first.at(75, 11), 255, 255, 255, 255, 8);
        check_pixel(second.at(75, 11), 0, 0, 255, 255, 8);
        CHECK(images_equal(first, third));
    });
}

TEST_CASE("d3d10 renders the reference packet at scale 2") {
    for_each_driver([&](Device& dev) {
        auto renderer = make_renderer(dev.device.Get());
        auto target = create_target(dev.device.Get(), 256, 256);
        const auto image = render_and_read(
            dev, *renderer, wui::test::reference_packet(1, 2.0f), target);
        check_pixel(image.at(24, 24), 255, 0, 0, 255);
        check_pixel(image.at(60, 60), 128, 128, 0, 255, 8);
        check_pixel(image.at(140, 120), 26, 26, 26, 255, 8);
        check_pixel(image.at(24, 232), 26, 26, 26, 255, 8);
        check_pixel(image.at(152, 24), 239, 239, 239, 255, 3);
        check_pixel(image.at(168, 24), 16, 16, 16, 255, 3);
    });
}

TEST_CASE("d3d10 converts straight and premultiplied sampled alpha exactly once") {
    for_each_driver([&](Device& dev) {
        auto renderer = make_renderer(dev.device.Get());
        auto target = create_target(dev.device.Get(), 8, 8);
        for (const auto alpha : {wui::AlphaMode::straight, wui::AlphaMode::premultiplied}) {
            INFO("alpha: ",
                std::string(alpha == wui::AlphaMode::straight ? "straight" : "premultiplied"));
            const auto packet = alpha_packet(alpha);
            REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
            const auto image = render_and_read(dev, *renderer, packet, target);
            check_pixel(image.at(4, 4), 128, 0, 0, 255, 3);
        }
    });
}

TEST_CASE("d3d10 clips fractional rectangles and ignores empty batches") {
    for_each_driver([&](Device& dev) {
        auto renderer = make_renderer(dev.device.Get());
        auto target = create_target(dev.device.Get(), 128, 128);
        auto packet = std::make_shared<wui::PacketData>(*wui::test::reference_packet(1));
        auto blue = std::find_if(packet->batches.begin(), packet->batches.end(),
            [](const wui::Batch& batch) {
                return batch.clip.x == 80 && batch.clip.y == 72 &&
                    batch.clip.w == 16 && batch.clip.h == 16;
            });
        REQUIRE(blue != packet->batches.end());
        blue->clip = {-10.25f, -10.25f, 106.0f, 98.0f};
        packet->batches.push_back({0, 0, 0, 0, {0, 0, 0, 0}});
        REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
        const auto image = render_and_read(dev, *renderer, packet, target);
        check_pixel(image.at(95, 87), 0, 0, 255, 255, 8);
        check_pixel(image.at(96, 87), 26, 26, 26, 255, 8);
        check_pixel(image.at(95, 88), 26, 26, 26, 255, 8);

        // A packet holding only an empty batch draws nothing.
        auto empty = std::make_shared<wui::PacketData>();
        empty->viewport = {0, 0, 128, 128};
        empty->textures = packet->textures;
        empty->batches.push_back({0, 0, 0, 0, {0, 0, 128, 128}});
        REQUIRE_FALSE(wui::validate_packet(*empty).has_value());
        const auto blank = render_and_read(dev, *renderer, empty, target);
        check_pixel(blank.at(64, 64), 0, 0, 0, 255, 0);
        check_pixel(blank.at(12, 12), 0, 0, 0, 255, 0);
    });
}

namespace {

ComPtr<ID3DBlob> compile_host(const char* source, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::char_traits<char>::length(source),
        "d3d10_host", nullptr, nullptr, entry, profile, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string message = std::string("host shader ") + entry + " failed";
        if (errors) {
            message.append(static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return code;
}

constexpr char kHostShaders[] = R"(
struct V { float4 p : POSITION; };
struct O { float4 p : SV_Position; };
O HostVs(V v) { O o; o.p = v.p; return o; }
[maxvertexcount(3)]
void HostGs(triangle O input[3], inout TriangleStream<O> stream) {
    for (int i = 0; i < 3; ++i) stream.Append(input[i]);
}
float4 HostPs(O o) : SV_Target { return float4(1, 0, 1, 1); }
)";

ComPtr<ID3D10Buffer> make_buffer(ID3D10Device* device, UINT bytes, UINT bind) {
    D3D10_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D10_USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    ComPtr<ID3D10Buffer> buffer;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer))) {
        throw std::runtime_error("test buffer creation failed");
    }
    return buffer;
}

// Every slot the renderer touches, read back through fresh Get calls.
struct HostState {
    ComPtr<ID3D10InputLayout> layout;
    D3D10_PRIMITIVE_TOPOLOGY topology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D10Buffer> vertex_buffer;
    UINT vertex_stride = 0, vertex_offset = 0;
    ComPtr<ID3D10Buffer> index_buffer;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0;
    ComPtr<ID3D10RasterizerState> rasterizer;
    UINT viewport_count = 0;
    D3D10_VIEWPORT viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT scissor_count = 0;
    D3D10_RECT scissors[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ComPtr<ID3D10BlendState> blend;
    FLOAT blend_factor[4]{};
    UINT blend_mask = 0;
    ComPtr<ID3D10DepthStencilState> depth_stencil;
    UINT stencil_ref = 0;
    ComPtr<ID3D10RenderTargetView> rtvs[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D10DepthStencilView> dsv;
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10GeometryShader> gs;
    ComPtr<ID3D10PixelShader> ps;
    ComPtr<ID3D10Buffer> vs_cbuf0;
    ComPtr<ID3D10Buffer> ps_cbuf1;
    ComPtr<ID3D10SamplerState> ps_sampler0;
    ComPtr<ID3D10ShaderResourceView> ps_srv0;

    void capture(ID3D10Device* device) {
        *this = HostState{};
        device->IAGetInputLayout(layout.GetAddressOf());
        device->IAGetPrimitiveTopology(&topology);
        device->IAGetVertexBuffers(
            0, 1, vertex_buffer.GetAddressOf(), &vertex_stride, &vertex_offset);
        device->IAGetIndexBuffer(index_buffer.GetAddressOf(), &index_format, &index_offset);
        device->RSGetState(rasterizer.GetAddressOf());
        viewport_count = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        device->RSGetViewports(&viewport_count, viewports);
        scissor_count = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        device->RSGetScissorRects(&scissor_count, scissors);
        device->OMGetBlendState(blend.GetAddressOf(), blend_factor, &blend_mask);
        device->OMGetDepthStencilState(depth_stencil.GetAddressOf(), &stencil_ref);
        ID3D10RenderTargetView* raw[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D10DepthStencilView* depth = nullptr;
        device->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, raw, &depth);
        for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            rtvs[i].Attach(raw[i]);
        }
        dsv.Attach(depth);
        device->VSGetShader(vs.GetAddressOf());
        device->GSGetShader(gs.GetAddressOf());
        device->PSGetShader(ps.GetAddressOf());
        device->VSGetConstantBuffers(0, 1, vs_cbuf0.GetAddressOf());
        device->PSGetConstantBuffers(1, 1, ps_cbuf1.GetAddressOf());
        device->PSGetSamplers(0, 1, ps_sampler0.GetAddressOf());
        device->PSGetShaderResources(0, 1, ps_srv0.GetAddressOf());
    }

    void check_equal(const HostState& expected) const {
        CHECK(layout.Get() == expected.layout.Get());
        CHECK(topology == expected.topology);
        CHECK(vertex_buffer.Get() == expected.vertex_buffer.Get());
        CHECK(vertex_stride == expected.vertex_stride);
        CHECK(vertex_offset == expected.vertex_offset);
        CHECK(index_buffer.Get() == expected.index_buffer.Get());
        CHECK(index_format == expected.index_format);
        CHECK(index_offset == expected.index_offset);
        CHECK(rasterizer.Get() == expected.rasterizer.Get());
        REQUIRE(viewport_count == expected.viewport_count);
        for (UINT i = 0; i < viewport_count; ++i) {
            CHECK(viewports[i].TopLeftX == expected.viewports[i].TopLeftX);
            CHECK(viewports[i].TopLeftY == expected.viewports[i].TopLeftY);
            CHECK(viewports[i].Width == expected.viewports[i].Width);
            CHECK(viewports[i].Height == expected.viewports[i].Height);
            CHECK(viewports[i].MinDepth == doctest::Approx(expected.viewports[i].MinDepth));
            CHECK(viewports[i].MaxDepth == doctest::Approx(expected.viewports[i].MaxDepth));
        }
        REQUIRE(scissor_count == expected.scissor_count);
        for (UINT i = 0; i < scissor_count; ++i) {
            CHECK(scissors[i].left == expected.scissors[i].left);
            CHECK(scissors[i].top == expected.scissors[i].top);
            CHECK(scissors[i].right == expected.scissors[i].right);
            CHECK(scissors[i].bottom == expected.scissors[i].bottom);
        }
        CHECK(blend.Get() == expected.blend.Get());
        for (int i = 0; i < 4; ++i) {
            CHECK(blend_factor[i] == doctest::Approx(expected.blend_factor[i]));
        }
        CHECK(blend_mask == expected.blend_mask);
        CHECK(depth_stencil.Get() == expected.depth_stencil.Get());
        CHECK(stencil_ref == expected.stencil_ref);
        for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            CHECK(rtvs[i].Get() == expected.rtvs[i].Get());
        }
        CHECK(dsv.Get() == expected.dsv.Get());
        CHECK(vs.Get() == expected.vs.Get());
        CHECK(gs.Get() == expected.gs.Get());
        CHECK(ps.Get() == expected.ps.Get());
        CHECK(vs_cbuf0.Get() == expected.vs_cbuf0.Get());
        CHECK(ps_cbuf1.Get() == expected.ps_cbuf1.Get());
        CHECK(ps_sampler0.Get() == expected.ps_sampler0.Get());
        CHECK(ps_srv0.Get() == expected.ps_srv0.Get());
    }
};

// Unusual, non-null host bindings for every slot the renderer touches.
struct HostBindings {
    ReadbackTarget color;
    ReadbackTarget color1;
    ComPtr<ID3D10DepthStencilView> dsv;
    ComPtr<ID3D10InputLayout> layout;
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10GeometryShader> gs;
    ComPtr<ID3D10PixelShader> ps;
    ComPtr<ID3D10Buffer> vertex_buffer, index_buffer, vs_cbuf, ps_cbuf;
    ComPtr<ID3D10BlendState> blend;
    ComPtr<ID3D10RasterizerState> rasterizer;
    ComPtr<ID3D10DepthStencilState> depth;
    ComPtr<ID3D10SamplerState> sampler;
    ComPtr<ID3D10ShaderResourceView> srv;

    explicit HostBindings(ID3D10Device* device) {
        color = create_target(device, 128, 128);
        color1 = create_target(device, 128, 128);
        {
            D3D10_TEXTURE2D_DESC desc{};
            desc.Width = 128;
            desc.Height = 128;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D10_USAGE_DEFAULT;
            desc.BindFlags = D3D10_BIND_DEPTH_STENCIL;
            ComPtr<ID3D10Texture2D> texture;
            REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
            REQUIRE(SUCCEEDED(device->CreateDepthStencilView(texture.Get(), nullptr, &dsv)));
        }
        const auto vs_blob = compile_host(kHostShaders, "HostVs", "vs_4_0");
        const auto gs_blob = compile_host(kHostShaders, "HostGs", "gs_4_0");
        const auto ps_blob = compile_host(kHostShaders, "HostPs", "ps_4_0");
        REQUIRE(SUCCEEDED(device->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &vs)));
        REQUIRE(SUCCEEDED(device->CreateGeometryShader(
            gs_blob->GetBufferPointer(), gs_blob->GetBufferSize(), &gs)));
        REQUIRE(SUCCEEDED(device->CreatePixelShader(
            ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), &ps)));
        const D3D10_INPUT_ELEMENT_DESC element{"POSITION", 0,
            DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0};
        REQUIRE(SUCCEEDED(device->CreateInputLayout(&element, 1,
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &layout)));
        vertex_buffer = make_buffer(device, 64, D3D10_BIND_VERTEX_BUFFER);
        index_buffer = make_buffer(device, 64, D3D10_BIND_INDEX_BUFFER);
        vs_cbuf = make_buffer(device, 16, D3D10_BIND_CONSTANT_BUFFER);
        ps_cbuf = make_buffer(device, 16, D3D10_BIND_CONSTANT_BUFFER);
        {
            D3D10_BLEND_DESC desc{};
            desc.BlendEnable[0] = TRUE;
            desc.SrcBlend = D3D10_BLEND_SRC_ALPHA;
            desc.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
            desc.BlendOp = D3D10_BLEND_OP_ADD;
            desc.SrcBlendAlpha = D3D10_BLEND_ONE;
            desc.DestBlendAlpha = D3D10_BLEND_ZERO;
            desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
            desc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_RED;
            REQUIRE(SUCCEEDED(device->CreateBlendState(&desc, &blend)));
        }
        {
            D3D10_RASTERIZER_DESC desc{};
            desc.FillMode = D3D10_FILL_WIREFRAME;
            desc.CullMode = D3D10_CULL_FRONT;
            desc.ScissorEnable = TRUE;
            desc.DepthClipEnable = TRUE;
            REQUIRE(SUCCEEDED(device->CreateRasterizerState(&desc, &rasterizer)));
        }
        {
            D3D10_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
            desc.DepthFunc = D3D10_COMPARISON_LESS;
            REQUIRE(SUCCEEDED(device->CreateDepthStencilState(&desc, &depth)));
        }
        {
            D3D10_SAMPLER_DESC desc{};
            desc.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
            desc.AddressU = D3D10_TEXTURE_ADDRESS_WRAP;
            desc.AddressV = D3D10_TEXTURE_ADDRESS_WRAP;
            desc.AddressW = D3D10_TEXTURE_ADDRESS_WRAP;
            desc.ComparisonFunc = D3D10_COMPARISON_NEVER;
            desc.MaxLOD = D3D10_FLOAT32_MAX;
            REQUIRE(SUCCEEDED(device->CreateSamplerState(&desc, &sampler)));
        }
        {
            D3D10_TEXTURE2D_DESC desc{};
            desc.Width = 2;
            desc.Height = 2;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D10_USAGE_DEFAULT;
            desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
            const std::uint32_t pixels[4] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};
            D3D10_SUBRESOURCE_DATA initial{};
            initial.pSysMem = pixels;
            initial.SysMemPitch = 8;
            ComPtr<ID3D10Texture2D> texture;
            REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture)));
            REQUIRE(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv)));
        }
    }

    void bind(ID3D10Device* device) const {
        // Two render targets so restoration must cover more than slot 0.
        ID3D10RenderTargetView* rtvs[2] = {color.rtv.Get(), color1.rtv.Get()};
        device->OMSetRenderTargets(2, rtvs, dsv.Get());
        const D3D10_VIEWPORT viewports[2] = {{8, 8, 64, 64, 0.2f, 0.9f},
            {1, 2, 30, 40, 0.0f, 0.5f}};
        device->RSSetViewports(2, viewports);
        const D3D10_RECT scissors[2] = {{4, 4, 20, 20}, {1, 1, 9, 9}};
        device->RSSetScissorRects(2, scissors);
        const FLOAT factor[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        device->OMSetBlendState(blend.Get(), factor, 0x00ffffff);
        device->OMSetDepthStencilState(depth.Get(), 7);
        device->RSSetState(rasterizer.Get());
        device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        const UINT stride = 16, offset = 16;
        device->IASetVertexBuffers(0, 1, vertex_buffer.GetAddressOf(), &stride, &offset);
        device->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R16_UINT, 2);
        device->IASetInputLayout(layout.Get());
        device->VSSetShader(vs.Get());
        device->GSSetShader(gs.Get());
        device->PSSetShader(ps.Get());
        device->VSSetConstantBuffers(0, 1, vs_cbuf.GetAddressOf());
        device->PSSetConstantBuffers(1, 1, ps_cbuf.GetAddressOf());
        device->PSSetSamplers(0, 1, sampler.GetAddressOf());
        device->PSSetShaderResources(0, 1, srv.GetAddressOf());
    }
};

} // namespace

TEST_CASE("d3d10 restores every touched host slot on success and error") {
    for_each_driver([&](Device& dev) {
        ID3D10Device* device = dev.device.Get();
        auto renderer = make_renderer(device);
        HostBindings host(device);
        auto draw_target = create_target(device, 128, 128);
        const auto packet = wui::test::reference_packet(1, 1.0f);
        const wui::d3d10::Target native{draw_target.rtv.Get(), 128, 128};

        // A host with nothing bound (zero viewports and scissors) gets
        // exactly that back.
        device->ClearState();
        HostState empty_before;
        empty_before.capture(device);
        REQUIRE(empty_before.viewport_count == 0);
        REQUIRE(empty_before.scissor_count == 0);
        REQUIRE_FALSE(renderer->render(packet, native).has_value());
        HostState empty_after;
        empty_after.capture(device);
        empty_after.check_equal(empty_before);
        wait_for_gpu(device, "after empty-host render");
        REQUIRE_FALSE(renderer->collect().has_value());

        clear(dev.device.Get(), draw_target, 0, 0, 0, 1);
        host.bind(device);
        HostState before;
        before.capture(device);
        // Every slot the renderer touches is non-null before rendering.
        REQUIRE(before.layout);
        REQUIRE(before.vertex_buffer);
        REQUIRE(before.index_buffer);
        REQUIRE(before.rasterizer);
        REQUIRE(before.viewport_count == 2);
        REQUIRE(before.scissor_count == 2);
        REQUIRE(before.blend);
        REQUIRE(before.depth_stencil);
        REQUIRE(before.rtvs[0].Get() == host.color.rtv.Get());
        REQUIRE(before.rtvs[1].Get() == host.color1.rtv.Get());
        REQUIRE(before.dsv);
        REQUIRE(before.vs);
        REQUIRE(before.gs);
        REQUIRE(before.ps);
        REQUIRE(before.vs_cbuf0);
        REQUIRE(before.ps_cbuf1);
        REQUIRE(before.ps_sampler0);
        REQUIRE(before.ps_srv0);

        REQUIRE_FALSE(renderer->render(packet, native).has_value());
        HostState after;
        after.capture(device);
        // Fresh Get calls on both sides; the backup struct is never
        // compared to itself.
        after.check_equal(before);
        // The UI really drew under the host state.
        const auto image = read_back(device, draw_target);
        check_pixel(image.at(12, 12), 255, 0, 0, 255);
        check_pixel(image.at(30, 30), 128, 128, 0, 255, 8);

        // Rejected calls leave the host state in place.
        host.bind(device);
        HostState error_before;
        error_before.capture(device);
        const auto bad = renderer->render(packet, {nullptr, 0, 0});
        REQUIRE(bad.has_value());
        HostState error_after;
        error_after.capture(device);
        error_after.check_equal(error_before);

        // caller_restores skips capture: the UI pipeline stays bound.
        wait_for_gpu(device, "before caller_restores");
        REQUIRE_FALSE(renderer->collect().has_value());
        host.bind(device);
        REQUIRE_FALSE(renderer->render(
            packet, native, wui::d3d10::StatePolicy::caller_restores).has_value());
        HostState ui;
        ui.capture(device);
        CHECK(ui.rtvs[0].Get() == draw_target.rtv.Get());
        for (UINT i = 1; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            CHECK(ui.rtvs[i].Get() == nullptr);
        }
        CHECK(ui.dsv.Get() == nullptr);
        CHECK(ui.layout);
        CHECK(ui.layout.Get() != before.layout.Get());
        CHECK(ui.topology == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CHECK(ui.vertex_buffer);
        CHECK(ui.vertex_buffer.Get() != before.vertex_buffer.Get());
        CHECK(ui.vertex_stride == sizeof(wui::Vertex));
        CHECK(ui.index_buffer);
        CHECK(ui.index_buffer.Get() != before.index_buffer.Get());
        CHECK(ui.index_format == DXGI_FORMAT_R32_UINT);
        CHECK(ui.vs);
        CHECK(ui.vs.Get() != before.vs.Get());
        CHECK(ui.gs.Get() == nullptr);
        CHECK(ui.ps);
        CHECK(ui.ps.Get() != before.ps.Get());
        CHECK(ui.vs_cbuf0);
        CHECK(ui.vs_cbuf0.Get() != before.vs_cbuf0.Get());
        CHECK(ui.ps_cbuf1);
        CHECK(ui.ps_cbuf1.Get() != before.ps_cbuf1.Get());
        CHECK(ui.ps_sampler0);
        CHECK(ui.ps_sampler0.Get() != before.ps_sampler0.Get());
        // The atlas SRV is unbound after the last draw.
        CHECK(ui.ps_srv0.Get() == nullptr);
        CHECK(ui.rasterizer);
        CHECK(ui.rasterizer.Get() != before.rasterizer.Get());
        CHECK(ui.blend);
        CHECK(ui.blend.Get() != before.blend.Get());
        CHECK(ui.blend_mask == 0xffffffffu);
        CHECK(ui.depth_stencil);
        CHECK(ui.depth_stencil.Get() != before.depth_stencil.Get());
        REQUIRE(ui.viewport_count == 1);
        CHECK(ui.viewports[0].TopLeftX == 0);
        CHECK(ui.viewports[0].TopLeftY == 0);
        CHECK(ui.viewports[0].Width == 128);
        CHECK(ui.viewports[0].Height == 128);
        CHECK(ui.scissor_count == 1);
        const auto ui_image = read_back(device, draw_target);
        check_pixel(ui_image.at(12, 12), 255, 0, 0, 255);
        // Leave the device in a neutral state for the debug-layer check.
        device->ClearState();
        wait_for_gpu(device, "after caller_restores");
        REQUIRE_FALSE(renderer->collect().has_value());
    });
}

TEST_CASE("d3d10 rejects invalid packets and targets without touching state") {
    for_each_driver([&](Device& dev) {
        ID3D10Device* device = dev.device.Get();
        auto renderer = make_renderer(device);
        HostBindings host(device);
        auto target = create_target(device, 128, 128);
        const wui::d3d10::Target native{target.rtv.Get(), 128, 128};
        const auto good = wui::test::reference_packet(1, 1.0f);

        auto expect = [&](const wui::DrawPacket& packet, wui::d3d10::Target to,
                          wui::ErrorCode code, const char* what) {
            INFO("case: ", std::string(what));
            host.bind(device);
            HostState before;
            before.capture(device);
            const auto error = renderer->render(packet, to);
            REQUIRE(error.has_value());
            CHECK(error->code == code);
            HostState after;
            after.capture(device);
            after.check_equal(before);
        };

        expect(nullptr, native, wui::ErrorCode::invalid_argument, "null packet");
        {
            auto bad = std::make_shared<wui::PacketData>(*good);
            bad->batches[0].texture_index = 99;
            expect(bad, native, wui::ErrorCode::invalid_packet, "texture index");
        }
        {
            auto bad = std::make_shared<wui::PacketData>(*good);
            bad->batches[0].index_count += 3000;
            expect(bad, native, wui::ErrorCode::invalid_packet, "index range");
        }
        {
            auto bad = std::make_shared<wui::PacketData>(*good);
            bad->indices[0] = 100000;
            expect(bad, native, wui::ErrorCode::invalid_packet, "vertex index");
        }
        expect(good, {target.rtv.Get(), 64, 64}, wui::ErrorCode::invalid_argument,
            "viewport mismatch");
        expect(good, {nullptr, 128, 128}, wui::ErrorCode::invalid_argument, "null view");
        {
            auto scaled = wui::test::reference_packet(1, 2.0f);
            expect(scaled, native, wui::ErrorCode::invalid_argument, "scale mismatch");
        }
        {
            auto wide = create_target(device, 128, 128, DXGI_FORMAT_R16G16B16A16_FLOAT);
            expect(good, {wide.rtv.Get(), 128, 128}, wui::ErrorCode::unsupported,
                "float target");
        }
        {
            // The packet matches the declared size but the view's texture
            // does not: the declared target is stale.
            auto undersized = create_target(device, 64, 64);
            expect(good, {undersized.rtv.Get(), 128, 128}, wui::ErrorCode::invalid_argument,
                "stale target size");
        }
        // The renderer still works after every rejection.
        device->ClearState();
        const auto image = render_and_read(dev, *renderer, good, target);
        check_pixel(image.at(12, 12), 255, 0, 0, 255);
        device->ClearState();
    });
}

TEST_CASE("d3d10 reports busy on the fourth in-flight render until collect") {
    for_each_driver([&](Device& dev) {
        ID3D10Device* device = dev.device.Get();
        auto renderer = make_renderer(device);
        auto target = create_target(device, 128, 128);
        const wui::d3d10::Target native{target.rtv.Get(), 128, 128};
        const auto first = wui::test::reference_packet(1, 1.0f);
        const auto second = wui::test::reference_packet(2, 1.0f);
        const auto expected = render_and_read(dev, *renderer, first, target);

        REQUIRE_FALSE(renderer->render(first, native).has_value());
        REQUIRE_FALSE(renderer->render(second, native).has_value());
        REQUIRE_FALSE(renderer->render(first, native).has_value());
        const auto busy = renderer->render(second, native);
        REQUIRE(busy.has_value());
        CHECK(busy->code == wui::ErrorCode::busy);
        // Busy stays until the host retires work; collect never blocks and
        // never flushes, so the host flushes and waits on its own event.
        wait_for_gpu(device, "drain three in-flight renders");
        REQUIRE_FALSE(renderer->collect().has_value());
        const auto drained = render_and_read(dev, *renderer, first, target);
        CHECK(images_equal(expected, drained));
    });
}

TEST_CASE("d3d10 keeps old revisions immutable and recreates on a replacement device") {
    for_each_driver([&](Device& dev) {
        auto renderer = make_renderer(dev.device.Get());
        auto target = create_target(dev.device.Get(), 128, 128);
        const auto first = wui::test::reference_packet(1, 1.0f);
        const auto second = wui::test::reference_packet(2, 1.0f);
        const auto image_v1 = render_and_read(dev, *renderer, first, target);
        const auto image_v2 = render_and_read(dev, *renderer, second, target);
        CHECK_FALSE(images_equal(image_v1, image_v2));
        // The retained revision-1 packet renders byte-identically after the
        // newer revision was uploaded: native revisions are immutable.
        CHECK(images_equal(image_v1, render_and_read(dev, *renderer, first, target)));

        // Leave one submission pending: recreate drops it with the old device.
        REQUIRE_FALSE(renderer->render(second, {target.rtv.Get(), 128, 128}).has_value());
        Device replacement;
        try {
            replacement = create_device(dev.driver);
        } catch (const std::exception& error) {
            FAIL("replacement device creation failed: ", error.what());
        }
        REQUIRE_FALSE(renderer->recreate(replacement.device.Get()).has_value());
        auto target2 = create_target(replacement.device.Get(), 128, 128);
        const auto recreated_v1 = render_and_read(replacement, *renderer, first, target2);
        CHECK(images_equal(image_v1, recreated_v1));
        const auto recreated_v2 = render_and_read(replacement, *renderer, second, target2);
        CHECK(images_equal(image_v2, recreated_v2));
        CHECK(images_equal(image_v1, render_and_read(replacement, *renderer, first, target2)));
        check_clean(replacement, "replacement device");
    });
}
