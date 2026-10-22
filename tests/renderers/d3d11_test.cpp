#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <wui/backends/d3d11.hpp>
#include <wui/canvas.hpp>
#include <wui/resources.hpp>
#include "../../backends/renderer/d3d11_state.hpp"

#include "../support/native_image.hpp"
#include "../support/packet_fixture.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

Device create_warp() {
    Device out;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_9_1;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
        levels, 1, D3D11_SDK_VERSION, &out.device, &selected, &out.context);
    if (FAILED(hr)) {
        throw std::runtime_error("D3D11CreateDevice WARP failed, hr=" + std::to_string(hr));
    }
    if (selected < D3D_FEATURE_LEVEL_11_0) {
        throw std::runtime_error("WARP device below feature level 11_0");
    }
    return out;
}

void wait_for_gpu(ID3D11DeviceContext* context, const char* stage) {
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_QUERY_DESC desc{};
    desc.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> completion;
    const HRESULT created = device->CreateQuery(&desc, &completion);
    if (FAILED(created)) {
        throw std::runtime_error(std::string(stage) +
            ": CreateQuery(EVENT) failed, hr=" + std::to_string(created));
    }
    context->End(completion.Get());
    context->Flush(); // The host submits once; polling must never flush.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        BOOL complete = FALSE;
        const HRESULT hr = context->GetData(completion.Get(), &complete,
            sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && complete) return;
        if (FAILED(hr)) {
            throw std::runtime_error(std::string(stage) +
                ": GetData(EVENT) failed, hr=" + std::to_string(hr));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string(stage) +
                ": GPU retirement timed out after 5 seconds; GetData hr=" +
                std::to_string(hr) + ", device removal hr=" +
                std::to_string(device->GetDeviceRemovedReason()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void retire_submissions(wui::d3d11::Renderer& renderer,
    ID3D11DeviceContext* context, const char* stage) {
    wait_for_gpu(context, stage);
    if (const auto error = renderer.collect(context)) {
        throw std::runtime_error(std::string(stage) + ": collect failed, code=" +
            std::to_string(static_cast<int>(error->code)) + ", " + error->message);
    }
}

void render_with_retirement(wui::d3d11::Renderer& renderer,
    ID3D11DeviceContext* context, const wui::DrawPacket& packet,
    const wui::d3d11::Target& target, const char* stage) {
    auto error = renderer.render(context, packet, target);
    if (error && error->code == wui::ErrorCode::busy) {
        retire_submissions(renderer, context, stage);
        // The event follows every prior submission, so all three slots must
        // now be reusable. A second busy result is a failure, not another spin.
        error = renderer.render(context, packet, target);
    }
    if (error) {
        throw std::runtime_error(std::string(stage) + ": render failed, code=" +
            std::to_string(static_cast<int>(error->code)) + ", " + error->message);
    }
}

struct ReadbackTarget {
    ComPtr<ID3D11Texture2D> rt;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> staging;
    std::uint32_t width = 0, height = 0;
};

ReadbackTarget create_target(ID3D11Device* device, std::uint32_t width, std::uint32_t height) {
    ReadbackTarget target;
    target.width = width;
    target.height = height;
    D3D11_TEXTURE2D_DESC color{};
    color.Width = width;
    color.Height = height;
    color.MipLevels = 1;
    color.ArraySize = 1;
    color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color.SampleDesc.Count = 1;
    color.Usage = D3D11_USAGE_DEFAULT;
    color.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&color, nullptr, &target.rt);
    if (FAILED(hr)) throw std::runtime_error("CreateTexture2D RT failed");
    hr = device->CreateRenderTargetView(target.rt.Get(), nullptr, &target.rtv);
    if (FAILED(hr)) throw std::runtime_error("CreateRenderTargetView failed");
    D3D11_TEXTURE2D_DESC staging = color;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&staging, nullptr, &target.staging);
    if (FAILED(hr)) throw std::runtime_error("CreateTexture2D staging failed");
    return target;
}

wui::test::Image read_back(
    ID3D11DeviceContext* context, const ReadbackTarget& target) {
    context->CopyResource(target.staging.Get(), target.rt.Get());
    wait_for_gpu(context, "readback copy");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
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
    context->Unmap(target.staging.Get(), 0);
    return image;
}

void check_channel(std::uint8_t actual, std::uint8_t expected, int tolerance = 2) {
    const int delta = static_cast<int>(actual) - static_cast<int>(expected);
    CHECK(std::abs(delta) <= tolerance);
}

void check_pixel(wui::test::Pixel actual, std::uint8_t r, std::uint8_t g, std::uint8_t b,
    std::uint8_t a, int tolerance = 2) {
    check_channel(actual.r, r, tolerance);
    check_channel(actual.g, g, tolerance);
    check_channel(actual.b, b, tolerance);
    check_channel(actual.a, a, tolerance);
}

} // namespace

TEST_CASE("reference fixture is valid and revisioned") {
    for (const auto scale : {1.0f, 2.0f}) {
        const auto one = wui::test::reference_packet(1, scale);
        const auto two = wui::test::reference_packet(2, scale);
        REQUIRE(one != nullptr);
        REQUIRE(two != nullptr);
        CHECK_FALSE(wui::validate_packet(*one).has_value());
        CHECK_FALSE(wui::validate_packet(*two).has_value());
        REQUIRE(one->batches.size() >= 2);
        CHECK(one->batches[1].base_vertex != 0);
        CHECK(one->viewport.w == 128);
        CHECK(one->viewport.h == 128);
        CHECK(one->scale == doctest::Approx(scale));
        // Same identity across revisions: checker handle and dimensions match,
        // exactly one texel differs.
        REQUIRE(one->textures.size() == two->textures.size());
        bool saw_checker = false;
        for (std::size_t i = 0; i < one->textures.size(); ++i) {
            const auto& a = one->textures[i];
            const auto& b = two->textures[i];
            REQUIRE(a != nullptr);
            REQUIRE(b != nullptr);
            if (a->width == 4 && a->height == 4) {
                saw_checker = true;
                CHECK(a->handle == b->handle);
                CHECK(b->width == 4);
                CHECK(b->height == 4);
                std::size_t diffs = 0;
                for (std::size_t p = 0; p < a->rgba.size(); ++p) {
                    if (a->rgba[p] != b->rgba[p]) ++diffs;
                }
                CHECK(diffs == 2); // one RGB texel white->red, R and alpha unchanged
            }
        }
        CHECK(saw_checker);
    }
    CHECK_THROWS_AS(wui::test::reference_packet(3), std::invalid_argument);
    const auto ppm_error = wui::test::write_ppm({}, "unused.ppm");
    REQUIRE(ppm_error.has_value());
    CHECK(ppm_error->code == wui::ErrorCode::invalid_argument);
}

TEST_CASE("d3d11 renders the reference packet through WARP") {
    Device dev;
    try {
        dev = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    auto created = wui::d3d11::Renderer::create(dev.device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer =
        std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);

    const auto packet = wui::test::reference_packet(1, 1.0f);
    auto target = create_target(dev.device.Get(), 128, 128);
    wui::d3d11::Target native{target.rtv.Get(), 128, 128};
    const auto error = renderer->render(dev.context.Get(), packet, native);
    REQUIRE_FALSE(error.has_value());
    const auto image = read_back(dev.context.Get(), target);

    // Opaque red rect interior, away from the green overlap.
    check_pixel(image.at(12, 12), 255, 0, 0, 255);
    // Half-alpha green over red over {0.1,0.1,0.1} background under
    // premultiplied ONE/INV_SRC_ALPHA resolves near (128,128,0).
    check_pixel(image.at(30, 30), 128, 128, 0, 255, 8);
    // Pixels outside the {80,72,16,16} scissor stay background (~26).
    check_pixel(image.at(70, 60), 26, 26, 26, 255, 8);
    check_pixel(image.at(100, 100), 26, 26, 26, 255, 8);

    // Revision 2 changes the expected checker location.
    const auto packet2 = wui::test::reference_packet(2, 1.0f);
    const auto error2 = renderer->render(dev.context.Get(), packet2, native);
    REQUIRE_FALSE(error2.has_value());
    const auto image2 = read_back(dev.context.Get(), target);
    bool changed = false;
    for (std::uint32_t y = 8; y < 40; ++y) {
        for (std::uint32_t x = 72; x < 104; ++x) {
            const auto a = image.at(x, y);
            const auto b = image2.at(x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b) changed = true;
        }
    }
    CHECK(changed);
    REQUIRE_FALSE(
        wui::test::write_ppm(image, "a9-warp-rev1.ppm").has_value());
    REQUIRE_FALSE(
        wui::test::write_ppm(image2, "a9-warp-rev2.ppm").has_value());
}
TEST_CASE("d3d11 renders the reference packet at scale 2") {
    Device dev;
    try {
        dev = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    auto created = wui::d3d11::Renderer::create(dev.device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer =
        std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);
    const auto packet = wui::test::reference_packet(1, 2.0f);
    auto target = create_target(dev.device.Get(), 256, 256);
    wui::d3d11::Target native{target.rtv.Get(), 256, 256};
    const auto error = renderer->render(dev.context.Get(), packet, native);
    REQUIRE_FALSE(error.has_value());
    const auto image = read_back(dev.context.Get(), target);
    check_pixel(image.at(24, 24), 255, 0, 0, 255);
    check_pixel(image.at(60, 60), 128, 128, 0, 255, 8);
    check_pixel(image.at(140, 120), 26, 26, 26, 255, 8);
}

namespace {

struct HostState {
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11Buffer> vertex_buffer;
    UINT vertex_stride = 0, vertex_offset = 0;
    ComPtr<ID3D11Buffer> index_buffer;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0;
    ComPtr<ID3D11RasterizerState> rasterizer;
    UINT viewport_count = 0;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT scissor_count = 0;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ComPtr<ID3D11BlendState> blend;
    FLOAT blend_factor[4]{};
    UINT blend_mask = 0;
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    UINT stencil_ref = 0;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    UINT vs_instances = 0;
    ComPtr<ID3D11PixelShader> ps;
    UINT ps_instances = 0;
    ComPtr<ID3D11Buffer> vs_cbuf0;
    ComPtr<ID3D11Buffer> ps_cbuf1;
    ComPtr<ID3D11SamplerState> ps_sampler0;
    ComPtr<ID3D11ShaderResourceView> ps_srv0;
    ComPtr<ID3D11UnorderedAccessView> om_uavs[D3D11_PS_CS_UAV_REGISTER_COUNT];
    ComPtr<ID3D11UnorderedAccessView> cs_uavs[D3D11_PS_CS_UAV_REGISTER_COUNT];

    void capture(ID3D11DeviceContext* context) {
        *this = HostState{};
        context->IAGetInputLayout(layout.GetAddressOf());
        context->IAGetPrimitiveTopology(&topology);
        context->IAGetVertexBuffers(
            0, 1, vertex_buffer.GetAddressOf(), &vertex_stride, &vertex_offset);
        context->IAGetIndexBuffer(
            index_buffer.GetAddressOf(), &index_format, &index_offset);
        context->RSGetState(rasterizer.GetAddressOf());
        viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context->RSGetViewports(&viewport_count, viewports);
        scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context->RSGetScissorRects(&scissor_count, scissors);
        context->OMGetBlendState(blend.GetAddressOf(), blend_factor, &blend_mask);
        context->OMGetDepthStencilState(depth_stencil.GetAddressOf(), &stencil_ref);
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth = nullptr;
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &depth);
        rtv.Attach(rtvs[0]);
        for (UINT i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (rtvs[i] != nullptr) rtvs[i]->Release();
        }
        dsv.Attach(depth);
        ID3D11RenderTargetView* again[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth_again = nullptr;
        ID3D11UnorderedAccessView* om[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, again, &depth_again,
            0, D3D11_PS_CS_UAV_REGISTER_COUNT, om);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (again[i] != nullptr) again[i]->Release();
        }
        if (depth_again != nullptr) depth_again->Release();
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
            om_uavs[i].Attach(om[i]);
        }
        context->VSGetShader(vs.GetAddressOf(), nullptr, &vs_instances);
        context->PSGetShader(ps.GetAddressOf(), nullptr, &ps_instances);
        context->VSGetConstantBuffers(0, 1, vs_cbuf0.GetAddressOf());
        context->PSGetConstantBuffers(1, 1, ps_cbuf1.GetAddressOf());
        context->PSGetSamplers(0, 1, ps_sampler0.GetAddressOf());
        context->PSGetShaderResources(0, 1, ps_srv0.GetAddressOf());
        ID3D11UnorderedAccessView* cs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        context->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, cs);
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
            cs_uavs[i].Attach(cs[i]);
        }
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
        CHECK(viewport_count == expected.viewport_count);
        for (UINT i = 0; i < viewport_count; ++i) {
            CHECK(viewports[i].TopLeftX == doctest::Approx(expected.viewports[i].TopLeftX));
            CHECK(viewports[i].TopLeftY == doctest::Approx(expected.viewports[i].TopLeftY));
            CHECK(viewports[i].Width == doctest::Approx(expected.viewports[i].Width));
            CHECK(viewports[i].Height == doctest::Approx(expected.viewports[i].Height));
            CHECK(viewports[i].MinDepth == doctest::Approx(expected.viewports[i].MinDepth));
            CHECK(viewports[i].MaxDepth == doctest::Approx(expected.viewports[i].MaxDepth));
        }
        CHECK(scissor_count == expected.scissor_count);
        for (UINT i = 0; i < scissor_count; ++i) {
            CHECK(scissors[i].left == expected.scissors[i].left);
            CHECK(scissors[i].top == expected.scissors[i].top);
            CHECK(scissors[i].right == expected.scissors[i].right);
            CHECK(scissors[i].bottom == expected.scissors[i].bottom);
        }
        CHECK(blend.Get() == expected.blend.Get());
        CHECK(ps_srv0.Get() == expected.ps_srv0.Get());
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
            CHECK(om_uavs[i].Get() == expected.om_uavs[i].Get());
            CHECK(cs_uavs[i].Get() == expected.cs_uavs[i].Get());
        }
        CHECK(blend_mask == expected.blend_mask);
        CHECK(depth_stencil.Get() == expected.depth_stencil.Get());
        CHECK(stencil_ref == expected.stencil_ref);
        CHECK(rtv.Get() == expected.rtv.Get());
        CHECK(dsv.Get() == expected.dsv.Get());
        CHECK(vs.Get() == expected.vs.Get());
        CHECK(vs_instances == expected.vs_instances);
        CHECK(ps.Get() == expected.ps.Get());
        CHECK(ps_instances == expected.ps_instances);
        CHECK(vs_cbuf0.Get() == expected.vs_cbuf0.Get());
        CHECK(ps_cbuf1.Get() == expected.ps_cbuf1.Get());
        CHECK(ps_sampler0.Get() == expected.ps_sampler0.Get());
        CHECK(ps_srv0.Get() == expected.ps_srv0.Get());
    }
};

ComPtr<ID3D11Buffer> make_dynamic_buffer(
    ID3D11Device* device, UINT bytes, D3D11_BIND_FLAG bind) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> buffer;
    const HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr)) throw std::runtime_error("test buffer creation failed");
    return buffer;
}

} // namespace

TEST_CASE("d3d11 restores host state on success and error") {
    Device dev;
    try {
        dev = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    auto created = wui::d3d11::Renderer::create(dev.device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer =
        std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);
    auto host_target = create_target(dev.device.Get(), 128, 128);
    auto draw_target = create_target(dev.device.Get(), 128, 128);
    ID3D11DeviceContext* context = dev.context.Get();

    ComPtr<ID3D11BlendState> blend;
    {
        D3D11_BLEND_DESC desc{};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        REQUIRE(SUCCEEDED(dev.device->CreateBlendState(&desc, &blend)));
    }
    ComPtr<ID3D11RasterizerState> rasterizer;
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_WIREFRAME;
        desc.CullMode = D3D11_CULL_FRONT;
        desc.ScissorEnable = TRUE;
        desc.DepthClipEnable = TRUE;
        REQUIRE(SUCCEEDED(dev.device->CreateRasterizerState(&desc, &rasterizer)));
    }
    ComPtr<ID3D11DepthStencilState> depth;
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = TRUE;
        desc.DepthFunc = D3D11_COMPARISON_LESS;
        REQUIRE(SUCCEEDED(dev.device->CreateDepthStencilState(&desc, &depth)));
    }
    ComPtr<ID3D11SamplerState> sampler;
    {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        REQUIRE(SUCCEEDED(dev.device->CreateSamplerState(&desc, &sampler)));
    }
    auto vertex_buffer = make_dynamic_buffer(
        dev.device.Get(), 64, D3D11_BIND_VERTEX_BUFFER);
    auto index_buffer = make_dynamic_buffer(
        dev.device.Get(), 64, D3D11_BIND_INDEX_BUFFER);
    auto vs_cbuf = make_dynamic_buffer(
        dev.device.Get(), 16, D3D11_BIND_CONSTANT_BUFFER);
    auto ps_cbuf = make_dynamic_buffer(
        dev.device.Get(), 16, D3D11_BIND_CONSTANT_BUFFER);
    ComPtr<ID3D11ShaderResourceView> srv;
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 2;
        desc.Height = 2;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::uint32_t pixels[4] = {
            0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = pixels;
        initial.SysMemPitch = 8;
        ComPtr<ID3D11Texture2D> texture;
        REQUIRE(SUCCEEDED(
            dev.device->CreateTexture2D(&desc, &initial, &texture)));
        REQUIRE(SUCCEEDED(dev.device->CreateShaderResourceView(
            texture.Get(), nullptr, &srv)));
    }
    ComPtr<ID3D11UnorderedAccessView> om_uav;
    ComPtr<ID3D11UnorderedAccessView> om_uav_outer;
    ComPtr<ID3D11UnorderedAccessView> cs_uav;
    {
        auto make_uav = [&](ComPtr<ID3D11UnorderedAccessView>& out) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = 64;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = 16;
            ComPtr<ID3D11Buffer> buffer;
            REQUIRE(SUCCEEDED(
                dev.device->CreateBuffer(&desc, nullptr, &buffer)));
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_UNKNOWN;
            view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements = 4;
            REQUIRE(SUCCEEDED(dev.device->CreateUnorderedAccessView(
                buffer.Get(), &view, &out)));
        };
        make_uav(om_uav);
        make_uav(om_uav_outer);
        make_uav(cs_uav);
    }

    auto bind_host_state = [&] {
        ID3D11RenderTargetView* rtv = host_target.rtv.Get();
        // Two output-merger UAVs with an unbound gap at slot 2 exercise the
        // narrowed-span restore path including zero-count gaps.
        ID3D11UnorderedAccessView* oms[3] = {
            om_uav_outer.Get(), nullptr, om_uav.Get()};
        const UINT om_counts[3] = {0, 0, 0};
        context->OMSetRenderTargetsAndUnorderedAccessViews(
            1, &rtv, nullptr, 1, 3, oms, om_counts);
        ID3D11UnorderedAccessView* cs = cs_uav.Get();
        const UINT cs_zero = 0;
        context->CSSetUnorderedAccessViews(2, 1, &cs, &cs_zero);
        const D3D11_VIEWPORT viewport{8, 8, 64, 64, 0.2f, 0.9f};
        context->RSSetViewports(1, &viewport);
        const D3D11_RECT scissor{4, 4, 20, 20};
        context->RSSetScissorRects(1, &scissor);
        const FLOAT factor[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        context->OMSetBlendState(blend.Get(), factor, 0x00ffffff);
        context->OMSetDepthStencilState(depth.Get(), 7);
        context->RSSetState(rasterizer.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        const UINT stride = 16, offset = 16;
        context->IASetVertexBuffers(
            0, 1, vertex_buffer.GetAddressOf(), &stride, &offset);
        context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R16_UINT, 2);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* vcb = vs_cbuf.Get();
        context->VSSetConstantBuffers(0, 1, &vcb);
        ID3D11Buffer* pcb = ps_cbuf.Get();
        context->PSSetConstantBuffers(1, 1, &pcb);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetShaderResources(0, 1, srv.GetAddressOf());
    };

    bind_host_state();
    HostState before;
    before.capture(context);
    REQUIRE(before.rtv.Get() == host_target.rtv.Get());

    const auto packet = wui::test::reference_packet(1, 1.0f);
    wui::d3d11::Target native{draw_target.rtv.Get(), 128, 128};
    const auto error = renderer->render(context, packet, native);
    REQUIRE_FALSE(error.has_value());
    HostState after;
    after.capture(context);
    // Fresh Get APIs on both sides: the renderer's backup struct is never
    // compared to itself.
    after.check_equal(before);

    // An error return also leaves the unusual host state in place: invalid
    // packets are rejected before any device state changes.
    bind_host_state();
    HostState error_before;
    error_before.capture(context);
    wui::d3d11::Target bad{nullptr, 0, 0};
    const auto bad_error = renderer->render(context, packet, bad);
    REQUIRE(bad_error.has_value());
    HostState error_after;
    error_after.capture(context);
    error_after.check_equal(error_before);

    // Explicit opt-out skips the capture: the caller's render target remains
    // bound on return by design, so only verify the call succeeds.
    const auto caller_error = renderer->render(
        context, packet, native, wui::d3d11::StatePolicy::caller_restores);
    REQUIRE_FALSE(caller_error.has_value());
}

TEST_CASE("d3d11 state save and restore roundtrips bound targets") {
    Device dev;
    try {
        dev = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    ID3D11DeviceContext* context = dev.context.Get();
    auto first = create_target(dev.device.Get(), 128, 128);
    auto second = create_target(dev.device.Get(), 128, 128);
    ID3D11RenderTargetView* rtv = first.rtv.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT viewport{8, 8, 64, 64, 0.2f, 0.9f};
    context->RSSetViewports(1, &viewport);
    wui::d3d11::detail::Saved saved;
    wui::d3d11::detail::save_state(context, saved);
    // Diagnostic: is the snapshot itself populated?
    CHECK(saved.rtvs[0].Get() == first.rtv.Get());
    CHECK(saved.viewport_count == 1);
    HostState before;
    before.capture(context);
    REQUIRE(before.rtv.Get() == first.rtv.Get());
    ID3D11RenderTargetView* other = second.rtv.Get();
    context->OMSetRenderTargets(1, &other, nullptr);
    const D3D11_VIEWPORT changed{0, 0, 128, 128, 0, 1};
    context->RSSetViewports(1, &changed);
    wui::d3d11::detail::restore_state(context, saved);
    HostState after;
    after.capture(context);
    // Fresh Get APIs on both sides, never the backup struct itself.
    after.check_equal(before);
}

TEST_CASE("d3d11 retires revisions across packets and recreates devices") {
    Device dev1;
    try {
        dev1 = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    auto created = wui::d3d11::Renderer::create(dev1.device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer =
        std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);
    auto target1 = create_target(dev1.device.Get(), 128, 128);
    wui::d3d11::Target native1{target1.rtv.Get(), 128, 128};

    auto render_and_read = [&](const wui::DrawPacket& packet) {
        const auto error =
            renderer->render(dev1.context.Get(), packet, native1);
        REQUIRE_FALSE(error.has_value());
        auto image = read_back(dev1.context.Get(), target1);
        REQUIRE_FALSE(renderer->collect(dev1.context.Get()).has_value());
        return image;
    };
    const auto images_equal = [](const wui::test::Image& a,
                                  const wui::test::Image& b) {
        if (a.width != b.width || a.height != b.height ||
            a.pixels.size() != b.pixels.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.pixels.size(); ++i) {
            if (a.pixels[i].r != b.pixels[i].r ||
                a.pixels[i].g != b.pixels[i].g ||
                a.pixels[i].b != b.pixels[i].b ||
                a.pixels[i].a != b.pixels[i].a) {
                return false;
            }
        }
        return true;
    };

    const auto retained_v1 = wui::test::reference_packet(1, 1.0f);
    const auto image_v1 = render_and_read(retained_v1);
    const auto packet_v2 = wui::test::reference_packet(2, 1.0f);
    const auto image_v2 = render_and_read(packet_v2);
    CHECK_FALSE(images_equal(image_v1, image_v2));
    // The retained revision-1 packet renders byte-identically after the
    // newer revision was submitted: native revisions stay immutable.
    const auto image_v1_again = render_and_read(retained_v1);
    CHECK(images_equal(image_v1, image_v1_again));

    // A dropped unsubmitted packet releases its leases without disturbing
    // later submissions.
    {
        const auto dropped = wui::test::reference_packet(2, 1.0f);
        CHECK_FALSE(wui::validate_packet(*dropped).has_value());
    }
    const auto image_after_drop = render_and_read(retained_v1);
    CHECK(images_equal(image_v1, image_after_drop));

    // Fill the bounded in-flight window without collecting: the next render
    // reports busy and the host decides whether to wait.
    REQUIRE_FALSE(
        renderer->render(dev1.context.Get(), retained_v1, native1).has_value());
    REQUIRE_FALSE(
        renderer->render(dev1.context.Get(), packet_v2, native1).has_value());
    REQUIRE_FALSE(
        renderer->render(dev1.context.Get(), retained_v1, native1).has_value());
    const auto busy = renderer->render(dev1.context.Get(), packet_v2, native1);
    REQUIRE(busy.has_value());
    CHECK(busy->code == wui::ErrorCode::busy);
    // The held packets stay intact while busy: drain with host-driven waits
    // and prove submission resumes.
    render_with_retirement(*renderer, dev1.context.Get(), retained_v1, native1,
        "resume after three in-flight submissions");
    const auto image_drained = read_back(dev1.context.Get(), target1);
    REQUIRE_FALSE(renderer->collect(dev1.context.Get()).has_value());
    CHECK(images_equal(image_v1, image_drained));

    // Recreation on a second device renders retained packets without reusing
    // stale natives.
    Device dev2;
    try {
        dev2 = create_warp();
    } catch (const std::exception& error) {
        FAIL("second WARP device creation failed: ", error.what());
    }
    REQUIRE_FALSE(renderer->recreate(dev2.device.Get()).has_value());
    auto target2 = create_target(dev2.device.Get(), 128, 128);
    wui::d3d11::Target native2{target2.rtv.Get(), 128, 128};
    const auto recreated_error =
        renderer->render(dev2.context.Get(), retained_v1, native2);
    REQUIRE_FALSE(recreated_error.has_value());
    const auto image_recreated = read_back(dev2.context.Get(), target2);
    REQUIRE_FALSE(renderer->collect(dev2.context.Get()).has_value());
    CHECK(images_equal(image_v1, image_recreated));

    // Only the exact missing-SDK-layer result permits skipping this diagnostic.
    // When the debug layer is available, the same submission must produce no
    // error or corruption messages; warnings are reported for review.
    {
        ComPtr<ID3D11Device> debug_device;
        ComPtr<ID3D11DeviceContext> debug_context;
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_9_1;
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_DEBUG, levels, 1, D3D11_SDK_VERSION,
            &debug_device, &selected, &debug_context);
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            MESSAGE("D3D11 SDK debug layer missing; clean-output check skipped");
        } else {
            INFO("D3D11CreateDevice debug WARP hr=", hr);
            REQUIRE(SUCCEEDED(hr));
            MESSAGE("D3D11 debug layer available");
            ComPtr<ID3D11InfoQueue> info;
            REQUIRE(SUCCEEDED(debug_device.As(&info)));
            // Record validation messages instead of breaking: a default
            // debug device raises breakpoint exceptions on errors.
            REQUIRE(SUCCEEDED(info->SetBreakOnSeverity(
                D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE)));
            REQUIRE(SUCCEEDED(info->SetBreakOnSeverity(
                D3D11_MESSAGE_SEVERITY_ERROR, FALSE)));
            info->ClearStoredMessages();
            auto debug_created =
                wui::d3d11::Renderer::create(debug_device.Get());
            REQUIRE(std::holds_alternative<
                std::unique_ptr<wui::d3d11::Renderer>>(debug_created));
            auto& debug_renderer =
                std::get<std::unique_ptr<wui::d3d11::Renderer>>(debug_created);
            auto debug_target = create_target(debug_device.Get(), 128, 128);
            wui::d3d11::Target debug_native{
                debug_target.rtv.Get(), 128, 128};
            // The restore-policy render exercises save and restore on the
            // debug device; the info queue must stay free of errors.
            REQUIRE_FALSE(debug_renderer->render(debug_context.Get(),
                retained_v1, debug_native).has_value());
            retire_submissions(*debug_renderer, debug_context.Get(),
                "debug clean-output submission");
            UINT64 errors = 0;
            UINT64 warnings = 0;
            const UINT64 stored = info->GetNumStoredMessages();
            for (UINT64 i = 0; i < stored; ++i) {
                SIZE_T bytes = 0;
                if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
                if (bytes == 0) continue;
                std::vector<std::byte> storage(bytes);
                auto* message =
                    reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                if (FAILED(info->GetMessage(i, message, &bytes))) continue;
                if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
                    message->Severity == D3D11_MESSAGE_SEVERITY_ERROR) {
                    ++errors;
                    MESSAGE("D3D11 error: ", message->pDescription);
                } else if (
                    message->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
                    ++warnings;
                }
            }
            MESSAGE("D3D11 info-queue warnings: ", warnings);
            CHECK(errors == 0);
        }
    }
}

TEST_CASE("d3d11 evicts retired texture revisions after collect") {
    ComPtr<ID3D11Device> debug_device;
    ComPtr<ID3D11DeviceContext> debug_context;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_9_1;
    const HRESULT create_hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, levels, 1,
        D3D11_SDK_VERSION, &debug_device, &selected, &debug_context);
    if (create_hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        MESSAGE("D3D11 SDK debug layer missing; retirement-eviction check skipped");
        return;
    }
    INFO("D3D11CreateDevice debug WARP hr=", create_hr);
    REQUIRE(SUCCEEDED(create_hr));
    ComPtr<ID3D11InfoQueue> info;
    REQUIRE(SUCCEEDED(debug_device.As(&info)));
    REQUIRE(SUCCEEDED(
        info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE)));
    REQUIRE(SUCCEEDED(info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE)));
    ComPtr<ID3D11Debug> debug;
    REQUIRE(SUCCEEDED(debug_device.As(&debug)));

    auto count_live_textures = [&](const char* stage) {
        info->ClearStoredMessages();
        REQUIRE(SUCCEEDED(debug->ReportLiveDeviceObjects(
            D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL)));
        const UINT64 stored = info->GetNumStoredMessages();
        UINT64 live = 0;
        for (UINT64 i = 0; i < stored; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
            if (bytes == 0) continue;
            std::vector<std::byte> storage(bytes);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (FAILED(info->GetMessage(i, message, &bytes))) continue;
            if (message->ID == D3D11_MESSAGE_ID_LIVE_TEXTURE2D) ++live;
        }
        MESSAGE(stage, " live Texture2D: ", live);
        return live;
    };

    auto created = wui::d3d11::Renderer::create(debug_device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer = std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);
    auto target = create_target(debug_device.Get(), 128, 128);
    wui::d3d11::Target native{target.rtv.Get(), 128, 128};

    const UINT64 baseline = count_live_textures("baseline");
    info->ClearStoredMessages();

    const auto images_equal = [](const wui::test::Image& a, const wui::test::Image& b) {
        if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
            return false;
        for (std::size_t i = 0; i < a.pixels.size(); ++i) {
            if (a.pixels[i].r != b.pixels[i].r || a.pixels[i].g != b.pixels[i].g ||
                a.pixels[i].b != b.pixels[i].b || a.pixels[i].a != b.pixels[i].a)
                return false;
        }
        return true;
    };

    auto clone_with_revision = [](const wui::DrawPacket& base, std::uint64_t revision) {
        auto copy = std::make_shared<wui::PacketData>(*base);
        for (auto& texture : copy->textures) {
            if (texture && texture->width == 4 && texture->height == 4) {
                auto revised = std::make_shared<wui::TextureSnapshot>(*texture);
                revised->revision = revision;
                texture = std::move(revised);
            }
        }
        if (wui::validate_packet(*copy).has_value())
            throw std::runtime_error("cloned packet with revised checker is invalid");
        return wui::DrawPacket(std::move(copy));
    };

    auto render_drain_read = [&](const wui::DrawPacket& packet) {
        render_with_retirement(*renderer, debug_context.Get(), packet, native,
            "eviction readback submission");
        auto image = read_back(debug_context.Get(), target);
        REQUIRE_FALSE(renderer->collect(debug_context.Get()).has_value());
        return image;
    };

    wui::test::Image first_image{};
    {
        const auto retained = wui::test::reference_packet(1, 1.0f);
        first_image = render_drain_read(retained);
        std::vector<wui::DrawPacket> churn;
        churn.reserve(32);
        for (std::uint64_t i = 0; i < 32; ++i) churn.push_back(clone_with_revision(retained, 1000 + i));
        for (const auto& packet : churn) {
            render_with_retirement(*renderer, debug_context.Get(), packet, native,
                "texture revision churn");
        }
        const auto again = render_drain_read(retained);
        CHECK(images_equal(first_image, again));
    }

    debug_context->ClearState();
    retire_submissions(*renderer, debug_context.Get(), "final texture retirement");
    const UINT64 final_live = count_live_textures("final");
    info->ClearStoredMessages();
    CHECK(final_live == baseline);
}

TEST_CASE("d3d11 does not share native textures between snapshots with equal handles") {
    // Two TextureStores can hand out the same handle slot, generation and
    // revision for different pixels. The renderer must key its native
    // textures by snapshot identity so each packet samples its own pixels.
    Device dev;
    try {
        dev = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    auto created = wui::d3d11::Renderer::create(dev.device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer = std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);
    auto target = create_target(dev.device.Get(), 128, 128);
    wui::d3d11::Target native{target.rtv.Get(), 128, 128};

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

    auto render_read = [&](const wui::DrawPacket& packet) {
        render_with_retirement(*renderer, dev.context.Get(), packet, native,
            "identity readback submission");
        return read_back(dev.context.Get(), target);
    };
    const auto first = render_read(original);
    const auto second = render_read(other);
    const auto third = render_read(original);

    bool differs = false;
    bool restored = true;
    for (std::uint32_t y = 8; y < 40; ++y) {
        for (std::uint32_t x = 72; x < 104; ++x) {
            const auto a = first.at(x, y);
            const auto b = second.at(x, y);
            const auto c = third.at(x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b) differs = true;
            if (a.r != c.r || a.g != c.g || a.b != c.b) restored = false;
        }
    }
    CHECK(differs);
    CHECK(restored);
}

namespace {

// Typeless textures are read back through a staging copy in the UNORM member
// of the same format group; typed textures keep their own format.
DXGI_FORMAT staging_format(DXGI_FORMAT texture_format) {
    switch (texture_format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return texture_format;
    }
}

struct FormatTarget {
    ComPtr<ID3D11Texture2D> rt;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> staging; // null for multisampled targets
    DXGI_FORMAT stored = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0, height = 0;
};

// view_format UNKNOWN creates the view with a NULL desc, as a Present hook
// does over a typed back buffer (view format = buffer format).
FormatTarget create_format_target(ID3D11Device* device, std::uint32_t width,
    std::uint32_t height, DXGI_FORMAT texture_format, DXGI_FORMAT view_format,
    UINT samples = 1) {
    FormatTarget target;
    target.width = width;
    target.height = height;
    target.stored = staging_format(texture_format);
    D3D11_TEXTURE2D_DESC color{};
    color.Width = width;
    color.Height = height;
    color.MipLevels = 1;
    color.ArraySize = 1;
    color.Format = texture_format;
    color.SampleDesc.Count = samples;
    color.Usage = D3D11_USAGE_DEFAULT;
    color.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&color, nullptr, &target.rt);
    if (FAILED(hr)) {
        throw std::runtime_error("CreateTexture2D RT failed for DXGI_FORMAT " +
            std::to_string(static_cast<int>(texture_format)) + ", hr=" +
            std::to_string(hr));
    }
    if (view_format == DXGI_FORMAT_UNKNOWN) {
        hr = device->CreateRenderTargetView(target.rt.Get(), nullptr, &target.rtv);
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = view_format;
        view.ViewDimension = samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                         : D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = device->CreateRenderTargetView(target.rt.Get(), &view, &target.rtv);
    }
    if (FAILED(hr)) {
        throw std::runtime_error("CreateRenderTargetView failed for view DXGI_FORMAT " +
            std::to_string(static_cast<int>(view_format)) + ", hr=" +
            std::to_string(hr));
    }
    if (samples == 1) {
        D3D11_TEXTURE2D_DESC staging = color;
        staging.Format = target.stored;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&staging, nullptr, &target.staging);
        if (FAILED(hr)) throw std::runtime_error("CreateTexture2D staging failed");
    }
    return target;
}

// Reads stored values back as RGBA8: BGRA is reordered, 10-bit channels are
// scaled to 8 bits with rounding and 2-bit alpha is expanded.
wui::test::Image read_back_format(
    ID3D11DeviceContext* context, const FormatTarget& target) {
    if (!target.staging) throw std::runtime_error("target has no staging copy");
    context->CopyResource(target.staging.Get(), target.rt.Get());
    wait_for_gpu(context, "format readback copy");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) throw std::runtime_error("Map staging failed");
    wui::test::Image image;
    image.width = target.width;
    image.height = target.height;
    image.pixels.resize(static_cast<std::size_t>(target.width) * target.height);
    const auto* rows = static_cast<const std::uint8_t*>(mapped.pData);
    bool known = true;
    for (std::uint32_t y = 0; y < target.height && known; ++y) {
        const auto* row = rows + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const auto* texel = row + static_cast<std::size_t>(x) * 4;
            wui::test::Pixel pixel;
            switch (target.stored) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                pixel = {texel[0], texel[1], texel[2], texel[3]};
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                pixel = {texel[2], texel[1], texel[0], texel[3]};
                break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: {
                std::uint32_t packed = 0;
                std::memcpy(&packed, texel, sizeof(packed));
                const auto to8 = [](std::uint32_t v) {
                    return static_cast<std::uint8_t>((v * 510u + 1023u) / 2046u);
                };
                pixel.r = to8(packed & 0x3FFu);
                pixel.g = to8((packed >> 10) & 0x3FFu);
                pixel.b = to8((packed >> 20) & 0x3FFu);
                pixel.a = static_cast<std::uint8_t>(((packed >> 30) & 0x3u) * 85u);
                break;
            }
            default:
                known = false;
                break;
            }
            if (!known) break;
            image.pixels[static_cast<std::size_t>(y) * target.width + x] = pixel;
        }
    }
    context->Unmap(target.staging.Get(), 0);
    if (!known) {
        throw std::runtime_error("no readback decoder for DXGI_FORMAT " +
            std::to_string(static_cast<int>(target.stored)));
    }
    return image;
}

struct Region {
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // half-open
};

// Opaque 64x64 packet: dark background and four solid rects with distinct
// mid-range colors, so the sRGB decode/re-encode round trip is exercised
// away from 0 and 1. Interiors keep two pixels clear of every rect edge.
constexpr Region kOpaqueRegions[] = {
    {6, 6, 26, 26}, {38, 6, 58, 26}, {6, 38, 26, 58}, {38, 38, 58, 58},
    {30, 0, 34, 64}, {0, 30, 64, 34},
};

wui::DrawPacket opaque_format_packet() {
    auto store = std::make_shared<wui::TextureStore>(65536);
    wui::Canvas canvas(store);
    canvas.begin({0, 0, 64, 64}, 1.0f);
    canvas.rounded_rect({0, 0, 64, 64}, 0, {0.1f, 0.1f, 0.1f, 1.0f});
    canvas.rounded_rect({4, 4, 24, 24}, 0, {0.2f, 0.4f, 0.6f, 1.0f});
    canvas.rounded_rect({36, 4, 24, 24}, 0, {0.9f, 0.5f, 0.1f, 1.0f});
    canvas.rounded_rect({4, 36, 24, 24}, 0, {0.05f, 0.75f, 0.3f, 1.0f});
    canvas.rounded_rect({36, 36, 24, 24}, 0, {0.5f, 0.02f, 0.98f, 1.0f});
    auto finished = canvas.finish();
    if (std::holds_alternative<wui::Error>(finished)) {
        throw std::runtime_error("opaque format packet canvas failed: " +
            std::get<wui::Error>(finished).message);
    }
    return std::get<wui::DrawPacket>(std::move(finished));
}

// Counts pixels inside `region` whose channels differ by more than
// `tolerance`; the first offender is described in `first`.
std::size_t count_mismatches(const wui::test::Image& actual,
    const wui::test::Image& expected, Region region, int tolerance,
    std::string& first) {
    std::size_t mismatches = 0;
    for (std::uint32_t y = region.y0; y < region.y1; ++y) {
        for (std::uint32_t x = region.x0; x < region.x1; ++x) {
            const auto a = actual.at(x, y);
            const auto e = expected.at(x, y);
            const auto off = [&](std::uint8_t l, std::uint8_t r) {
                return std::abs(static_cast<int>(l) - static_cast<int>(r)) > tolerance;
            };
            if (off(a.r, e.r) || off(a.g, e.g) || off(a.b, e.b) || off(a.a, e.a)) {
                if (mismatches == 0) {
                    first = "(" + std::to_string(x) + "," + std::to_string(y) +
                        ") actual " + std::to_string(a.r) + "," + std::to_string(a.g) +
                        "," + std::to_string(a.b) + "," + std::to_string(a.a) +
                        " expected " + std::to_string(e.r) + "," + std::to_string(e.g) +
                        "," + std::to_string(e.b) + "," + std::to_string(e.a);
                }
                ++mismatches;
            }
        }
    }
    return mismatches;
}

} // namespace

TEST_CASE("d3d11 sRGB, 10-bit and typeless target views store the UNORM reference values") {
    Device dev;
    try {
        dev = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    auto created = wui::d3d11::Renderer::create(dev.device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer = std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);
    const auto packet = opaque_format_packet();

    auto render_read = [&](const FormatTarget& target, const char* stage) {
        const wui::d3d11::Target native{target.rtv.Get(), target.width, target.height};
        render_with_retirement(*renderer, dev.context.Get(), packet, native, stage);
        auto image = read_back_format(dev.context.Get(), target);
        REQUIRE_FALSE(renderer->collect(dev.context.Get()).has_value());
        return image;
    };

    const auto reference_target = create_format_target(dev.device.Get(), 64, 64,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN);
    const auto reference = render_read(reference_target, "R8G8B8A8_UNORM reference");
    // The reference itself stores the encoded colors (round(c * 255)).
    check_pixel(reference.at(16, 16), 51, 102, 153, 255, 1);
    check_pixel(reference.at(48, 16), 230, 128, 26, 255, 1);
    check_pixel(reference.at(16, 48), 13, 191, 77, 255, 1);
    check_pixel(reference.at(48, 48), 128, 5, 250, 255, 1);
    check_pixel(reference.at(32, 2), 26, 26, 26, 255, 1);

    struct Case {
        const char* name;
        DXGI_FORMAT texture;
        DXGI_FORMAT view; // UNKNOWN: NULL view desc
        bool srgb;
    };
    constexpr Case cases[] = {
        {"R8G8B8A8_TYPELESS texture, R8G8B8A8_UNORM view",
            DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, false},
        {"B8G8R8A8_UNORM texture, NULL view desc",
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_UNKNOWN, false},
        {"R10G10B10A2_UNORM texture, NULL view desc",
            DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, false},
        {"R10G10B10A2_TYPELESS texture, R10G10B10A2_UNORM view",
            DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, false},
        {"R8G8B8A8_TYPELESS texture, R8G8B8A8_UNORM_SRGB view",
            DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true},
        {"R8G8B8A8_UNORM_SRGB texture, NULL view desc",
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_UNKNOWN, true},
        {"B8G8R8A8_UNORM_SRGB texture, NULL view desc",
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_UNKNOWN, true},
        {"B8G8R8A8_TYPELESS texture, B8G8R8A8_UNORM_SRGB view",
            DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, true},
    };
    for (const auto& test : cases) {
        CAPTURE(test.name);
        const auto target = create_format_target(
            dev.device.Get(), 64, 64, test.texture, test.view);
        const auto image = render_read(target, test.name);
        // Opaque interiors store the reference values within one 8-bit step
        // (10-bit channels are compared after rounding to 8 bits).
        for (const auto& region : kOpaqueRegions) {
            std::string first;
            const auto mismatches = count_mismatches(image, reference, region, 1, first);
            INFO("first mismatch: ", first);
            CHECK(mismatches == 0);
        }
        // Non-sRGB views run the reference shader unchanged, so even the
        // translucent rect edges match. sRGB views blend edges in linear light.
        if (!test.srgb) {
            std::string first;
            const auto mismatches = count_mismatches(
                image, reference, Region{0, 0, 64, 64}, 1, first);
            INFO("first full-image mismatch: ", first);
            CHECK(mismatches == 0);
        }
    }
}

TEST_CASE("d3d11 rejects FP16, MSAA and array targets as unsupported without touching state") {
    Device dev;
    try {
        dev = create_warp();
    } catch (const std::exception& error) {
        FAIL("WARP device creation failed: ", error.what());
    }
    auto created = wui::d3d11::Renderer::create(dev.device.Get());
    REQUIRE(std::holds_alternative<std::unique_ptr<wui::d3d11::Renderer>>(created));
    auto& renderer = std::get<std::unique_ptr<wui::d3d11::Renderer>>(created);
    ID3D11DeviceContext* context = dev.context.Get();
    const auto packet = opaque_format_packet();

    auto host_target = create_target(dev.device.Get(), 64, 64);
    ID3D11RenderTargetView* host_rtv = host_target.rtv.Get();
    context->OMSetRenderTargets(1, &host_rtv, nullptr);
    const D3D11_VIEWPORT viewport{4, 4, 32, 32, 0.25f, 0.75f};
    context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{2, 2, 30, 30};
    context->RSSetScissorRects(1, &scissor);
    HostState before;
    before.capture(context);
    REQUIRE(before.rtv.Get() == host_rtv);

    auto expect_unsupported = [&](ID3D11RenderTargetView* view,
        const std::string& message_part) {
        const wui::d3d11::Target native{view, 64, 64};
        for (const auto policy : {wui::d3d11::StatePolicy::restore,
                 wui::d3d11::StatePolicy::caller_restores}) {
            const auto error = renderer->render(context, packet, native, policy);
            REQUIRE(error.has_value());
            CHECK(error->code == wui::ErrorCode::unsupported);
            INFO("message: ", error->message);
            CHECK(error->message.find(message_part) != std::string::npos);
            HostState after;
            after.capture(context);
            after.check_equal(before);
        }
    };

    {
        INFO("R16G16B16A16_FLOAT scRGB/HDR target");
        const auto fp16 = create_format_target(dev.device.Get(), 64, 64,
            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN);
        expect_unsupported(fp16.rtv.Get(), "DXGI_FORMAT " +
            std::to_string(static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT)) + ")");
    }
    {
        INFO("4x MSAA R8G8B8A8_UNORM target");
        UINT quality = 0;
        REQUIRE(SUCCEEDED(dev.device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_R8G8B8A8_UNORM, 4, &quality)));
        REQUIRE(quality > 0);
        const auto msaa = create_format_target(dev.device.Get(), 64, 64,
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, 4);
        expect_unsupported(msaa.rtv.Get(), "4 samples");
    }
    {
        INFO("Texture2DArray view of a two-slice R8G8B8A8_UNORM texture");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 64;
        desc.Height = 64;
        desc.MipLevels = 1;
        desc.ArraySize = 2;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> array;
        REQUIRE(SUCCEEDED(dev.device->CreateTexture2D(&desc, nullptr, &array)));
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.FirstArraySlice = 0;
        view.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11RenderTargetView> array_rtv;
        REQUIRE(SUCCEEDED(dev.device->CreateRenderTargetView(
            array.Get(), &view, &array_rtv)));
        expect_unsupported(array_rtv.Get(), "view dimension");
    }

    // The rejected calls queued nothing: an accepted target still renders.
    const auto accepted = create_format_target(dev.device.Get(), 64, 64,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN);
    const wui::d3d11::Target native{accepted.rtv.Get(), 64, 64};
    REQUIRE_FALSE(renderer->render(context, packet, native).has_value());
    const auto image = read_back_format(context, accepted);
    check_pixel(image.at(16, 16), 51, 102, 153, 255, 1);
    HostState after;
    after.capture(context);
    after.check_equal(before);
}
