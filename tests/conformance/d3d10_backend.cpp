#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "conformance.hpp"

// The SDK requires d3d10_1.h before d3d10.h (pulled in by the public header).
#include <d3d10_1.h>
#include <d3d10sdklayers.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <wui/backends/d3d10.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace wui::conformance {
namespace {

using Microsoft::WRL::ComPtr;

Error hr_error(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed hr=0x%08lx", what, static_cast<unsigned long>(hr));
    return Error{ErrorCode::backend_failure, text};
}

class D3D10Backend final : public OffscreenBackend {
public:
    Status initialize(const Options& options) {
        warp_ = options.driver != "hardware";
        name_ = warp_ ? "d3d10-warp" : "d3d10-hardware";
        if (auto error = create_device(device_)) return error;
        auto created = d3d10::Renderer::create(device_.Get());
        if (auto* error = std::get_if<Error>(&created)) return *error;
        renderer_ = std::get<std::unique_ptr<d3d10::Renderer>>(std::move(created));
        return std::nullopt;
    }

    std::string_view name() const override { return name_; }
    std::string adapter_description() const override { return adapter_; }
    bool debug_layer() const override { return debug_; }

    Result<Image> render(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return *error;
        const float clear[4]{kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, 1.0f};
        device_->ClearRenderTargetView(view_.Get(), clear);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto status = renderer_->render(packet, d3d10::Target{view_.Get(), w, h});
            if (!status) break;
            if (status->code != ErrorCode::busy) return *status;
            if (std::chrono::steady_clock::now() > deadline) return Error{ErrorCode::busy, "render busy for 10s"};
            if (auto error = collect_all()) return *error;
        }
        device_->CopyResource(staging_.Get(), target_.Get());
        if (auto error = collect_all()) return *error;
        D3D10_MAPPED_TEXTURE2D mapped{};
        const HRESULT hr = staging_->Map(0, D3D10_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return hr_error("Map(readback)", hr);
        Image image{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(image.rgba.data() + static_cast<std::size_t>(y) * w * 4,
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                static_cast<std::size_t>(w) * 4);
        }
        staging_->Unmap(0);
        return image;
    }

    Status submit_in_flight(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return error;
        // No Flush: the GPU cannot complete before the next submission is tried.
        return renderer_->render(packet, d3d10::Target{view_.Get(), w, h});
    }

    Status collect_all() override {
        D3D10_QUERY_DESC desc{D3D10_QUERY_EVENT, 0};
        ComPtr<ID3D10Query> done;
        HRESULT hr = device_->CreateQuery(&desc, &done);
        if (FAILED(hr)) return hr_error("CreateQuery(EVENT)", hr);
        done->End();
        device_->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            hr = done->GetData(nullptr, 0, 0);
            if (hr == S_OK) break;
            if (FAILED(hr)) return hr_error("GetData(EVENT)", hr);
            if (std::chrono::steady_clock::now() > deadline) return Error{ErrorCode::backend_failure, "GPU wait 10s"};
            std::this_thread::yield();
        }
        return renderer_->collect();
    }

    Status recreate() override {
        if (auto error = collect_all()) return error;
        drain_messages();
        ComPtr<ID3D10Device1> device;
        if (auto error = create_device(device)) return error;
        if (auto error = renderer_->recreate(device.Get())) return error;
        target_.Reset();
        view_.Reset();
        staging_.Reset();
        target_w_ = target_h_ = 0;
        device_ = std::move(device);
        return std::nullopt;
    }

    std::uint64_t debug_messages() override {
        drain_messages();
        const auto total = messages_;
        messages_ = 0;
        return total;
    }

private:
    Status create_device(ComPtr<ID3D10Device1>& device) {
        const D3D10_DRIVER_TYPE type = warp_ ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE;
        HRESULT hr = D3D10CreateDevice1(nullptr, type, nullptr, D3D10_CREATE_DEVICE_DEBUG,
            D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &device);
        debug_ = SUCCEEDED(hr);
        if (FAILED(hr)) {
            hr = D3D10CreateDevice1(nullptr, type, nullptr, 0, D3D10_FEATURE_LEVEL_10_0,
                D3D10_1_SDK_VERSION, &device);
        }
        if (FAILED(hr)) return hr_error("D3D10CreateDevice1", hr);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&desc))) {
            char name[256]{};
            std::snprintf(name, sizeof(name), "%ls (vendor 0x%04X device 0x%04X, FL 10_0)",
                desc.Description, desc.VendorId, desc.DeviceId);
            adapter_ = name;
        }
        info_.Reset();
        if (debug_) device.As(&info_);
        return std::nullopt;
    }

    Status ensure_target(std::uint32_t w, std::uint32_t h) {
        if (target_ && target_w_ == w && target_h_ == h) return std::nullopt;
        target_.Reset();
        view_.Reset();
        staging_.Reset();
        D3D10_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D10_BIND_RENDER_TARGET;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &target_);
        if (FAILED(hr)) return hr_error("CreateTexture2D(target)", hr);
        hr = device_->CreateRenderTargetView(target_.Get(), nullptr, &view_);
        if (FAILED(hr)) return hr_error("CreateRenderTargetView", hr);
        desc.Usage = D3D10_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
        if (FAILED(hr)) return hr_error("CreateTexture2D(staging)", hr);
        target_w_ = w;
        target_h_ = h;
        return std::nullopt;
    }

    void drain_messages() {
        if (!info_) return;
        const UINT64 stored = info_->GetNumStoredMessages();
        for (UINT64 i = 0; i < stored; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info_->GetMessage(i, nullptr, &bytes)) || bytes < sizeof(D3D10_MESSAGE)) continue;
            std::vector<std::byte> storage(bytes);
            auto* message = reinterpret_cast<D3D10_MESSAGE*>(storage.data());
            if (FAILED(info_->GetMessage(i, message, &bytes))) continue;
            if (message->Severity <= D3D10_MESSAGE_SEVERITY_WARNING) {
                ++messages_;
                std::fprintf(stderr, "d3d10 debug: %.*s\n", static_cast<int>(message->DescriptionByteLength),
                    message->pDescription);
            }
        }
        info_->ClearStoredMessages();
    }

    bool warp_ = true;
    bool debug_ = false;
    std::string name_, adapter_ = "unknown";
    ComPtr<ID3D10Device1> device_;
    ComPtr<ID3D10InfoQueue> info_;
    std::unique_ptr<d3d10::Renderer> renderer_;
    ComPtr<ID3D10Texture2D> target_, staging_;
    ComPtr<ID3D10RenderTargetView> view_;
    std::uint32_t target_w_ = 0, target_h_ = 0;
    std::uint64_t messages_ = 0;
};

}  // namespace

Result<std::unique_ptr<OffscreenBackend>> make_backend(const Options& options) {
    auto backend = std::make_unique<D3D10Backend>();
    if (auto error = backend->initialize(options)) return *error;
    return std::unique_ptr<OffscreenBackend>(std::move(backend));
}

}  // namespace wui::conformance
