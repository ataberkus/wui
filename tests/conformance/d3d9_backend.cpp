#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "conformance.hpp"

#include <wui/backends/d3d9.hpp>

#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
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

// D3D9 has no software rasterizer and its debug runtime is unavailable on
// Windows 10+, so this adapter always uses HAL and reports no debug layer.
class D3D9Backend final : public OffscreenBackend {
public:
    ~D3D9Backend() override {
        renderer_.reset();
        target_.Reset();
        system_.Reset();
        device_.Reset();
        api_.Reset();
        if (window_) DestroyWindow(window_);
    }

    Status initialize(const Options&) {
        window_ = CreateWindowExW(0, L"STATIC", L"wui D3D9 conformance", WS_OVERLAPPEDWINDOW, 0, 0, 256,
            256, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) return Error{ErrorCode::backend_failure, "CreateWindowExW failed"};
        api_.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if (!api_) return Error{ErrorCode::backend_failure, "Direct3DCreate9 failed"};
        presentation_.Windowed = TRUE;
        presentation_.SwapEffect = D3DSWAPEFFECT_DISCARD;
        presentation_.BackBufferFormat = D3DFMT_UNKNOWN;
        presentation_.BackBufferWidth = presentation_.BackBufferHeight = 256;
        presentation_.hDeviceWindow = window_;
        HRESULT hr = api_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &presentation_, &device_);
        if (FAILED(hr)) return hr_error("D3D9 HAL CreateDevice", hr);
        D3DADAPTER_IDENTIFIER9 id{};
        if (SUCCEEDED(api_->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id))) {
            char name[256]{};
            std::snprintf(name, sizeof(name), "%s (vendor 0x%04lX device 0x%04lX, HAL)", id.Description,
                id.VendorId, id.DeviceId);
            adapter_ = name;
        }
        auto created = d3d9::Renderer::create(device_.Get());
        if (auto* error = std::get_if<Error>(&created)) return *error;
        renderer_ = std::get<std::unique_ptr<d3d9::Renderer>>(std::move(created));
        return std::nullopt;
    }

    std::string_view name() const override { return "d3d9-hardware"; }
    std::string adapter_description() const override { return adapter_; }
    bool debug_layer() const override { return false; }

    Result<Image> render(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return *error;
        HRESULT hr = device_->ColorFill(target_.Get(), nullptr,
            D3DCOLOR_ARGB(kClear[3], kClear[0], kClear[1], kClear[2]));
        if (FAILED(hr)) return hr_error("ColorFill", hr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto status = submit(packet, w, h);
            if (!status) break;
            if (status->code != ErrorCode::busy) return *status;
            if (std::chrono::steady_clock::now() > deadline) return Error{ErrorCode::busy, "render busy for 10s"};
            if (auto error = collect_all()) return *error;
        }
        hr = device_->GetRenderTargetData(target_.Get(), system_.Get());
        if (FAILED(hr)) return hr_error("GetRenderTargetData", hr);
        if (auto error = collect_all()) return *error;
        D3DLOCKED_RECT locked{};
        hr = system_->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) return hr_error("LockRect(readback)", hr);
        Image image{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
        for (std::uint32_t y = 0; y < h; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(y) * locked.Pitch;
            auto* out = image.rgba.data() + static_cast<std::size_t>(y) * w * 4;
            for (std::uint32_t x = 0; x < w; ++x) {  // A8R8G8B8 is BGRA in memory
                out[4 * x + 0] = row[4 * x + 2];
                out[4 * x + 1] = row[4 * x + 1];
                out[4 * x + 2] = row[4 * x + 0];
                out[4 * x + 3] = row[4 * x + 3];
            }
        }
        system_->UnlockRect();
        return image;
    }

    Status submit_in_flight(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return error;
        return submit(packet, w, h);
    }

    Status collect_all() override {
        ComPtr<IDirect3DQuery9> done;
        HRESULT hr = device_->CreateQuery(D3DQUERYTYPE_EVENT, &done);
        if (FAILED(hr)) return hr_error("CreateQuery(EVENT)", hr);
        hr = done->Issue(D3DISSUE_END);
        if (FAILED(hr)) return hr_error("Issue(EVENT)", hr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            hr = done->GetData(nullptr, 0, D3DGETDATA_FLUSH);
            if (hr == S_OK) break;
            if (FAILED(hr)) return hr_error("GetData(EVENT)", hr);
            if (std::chrono::steady_clock::now() > deadline) return Error{ErrorCode::backend_failure, "GPU wait 10s"};
            std::this_thread::yield();
        }
        return renderer_->collect();
    }

    // A real device Reset, as a host does after D3DERR_DEVICENOTRESET: the
    // renderer drops its DEFAULT-pool resources and rebuilds them afterwards.
    Status recreate() override {
        if (auto error = collect_all()) return error;
        renderer_->before_reset();
        target_.Reset();
        system_.Reset();
        target_w_ = target_h_ = 0;
        presentation_.BackBufferWidth = presentation_.BackBufferWidth == 256 ? 320 : 256;
        const HRESULT hr = device_->Reset(&presentation_);
        if (FAILED(hr)) return hr_error("IDirect3DDevice9::Reset", hr);
        return renderer_->after_reset();
    }

    std::uint64_t debug_messages() override { return 0; }

private:
    Status submit(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) {
        HRESULT hr = device_->BeginScene();
        if (FAILED(hr)) return hr_error("BeginScene", hr);
        auto status = renderer_->render(packet, d3d9::Target{target_.Get(), w, h});
        hr = device_->EndScene();
        if (status) return status;
        if (FAILED(hr)) return hr_error("EndScene", hr);
        return std::nullopt;
    }

    Status ensure_target(std::uint32_t w, std::uint32_t h) {
        if (target_ && target_w_ == w && target_h_ == h) return std::nullopt;
        target_.Reset();
        system_.Reset();
        HRESULT hr = device_->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
            &target_, nullptr);
        if (FAILED(hr)) return hr_error("CreateRenderTarget(A8R8G8B8)", hr);
        hr = device_->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system_, nullptr);
        if (FAILED(hr)) return hr_error("CreateOffscreenPlainSurface(readback)", hr);
        target_w_ = w;
        target_h_ = h;
        return std::nullopt;
    }

    HWND window_ = nullptr;
    std::string adapter_ = "unknown";
    ComPtr<IDirect3D9> api_;
    ComPtr<IDirect3DDevice9> device_;
    D3DPRESENT_PARAMETERS presentation_{};
    std::unique_ptr<d3d9::Renderer> renderer_;
    ComPtr<IDirect3DSurface9> target_, system_;
    std::uint32_t target_w_ = 0, target_h_ = 0;
};

}  // namespace

Result<std::unique_ptr<OffscreenBackend>> make_backend(const Options& options) {
    auto backend = std::make_unique<D3D9Backend>();
    if (auto error = backend->initialize(options)) return *error;
    return std::unique_ptr<OffscreenBackend>(std::move(backend));
}

}  // namespace wui::conformance
