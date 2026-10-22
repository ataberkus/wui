#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_app.hpp"
#include "allocation_metrics.hpp"

#include <wui/wui.hpp>
#include <wui/text.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace wui::example {
namespace {

using Microsoft::WRL::ComPtr;

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string hr_string(HRESULT hr, const char* what) {
    char buffer[160] = {};
    std::snprintf(buffer, sizeof(buffer), "%s (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
    return std::string(buffer);
}

bool load_bytes_checked(const std::filesystem::path& path, std::vector<std::byte>& out,
    std::string& detail) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        detail = "cannot open " + path.string();
        return false;
    }
    const std::streamoff size = stream.tellg();
    if (size <= 0 || static_cast<std::uintmax_t>(size) > out.max_size() ||
        static_cast<std::uintmax_t>(size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        detail = "empty, unreadable, or oversized " + path.string();
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!stream) {
        detail = "cannot seek " + path.string();
        return false;
    }
    const auto expected = static_cast<std::streamsize>(out.size());
    stream.read(reinterpret_cast<char*>(out.data()), expected);
    if (stream.gcount() != expected || !stream) {
        detail = "short or failed read " + path.string();
        return false;
    }
    return true;
}

Error win32_error(const char* operation, DWORD code = GetLastError()) {
    return make_error(ErrorCode::backend_failure,
        hr_string(HRESULT_FROM_WIN32(code ? code : ERROR_GEN_FAILURE), operation));
}

Status executable_dir(std::filesystem::path& directory) {
    std::vector<wchar_t> buffer(260);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) return win32_error("GetModuleFileNameW");
        if (length < buffer.size()) {
            directory = std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
            return std::nullopt;
        }
        if (buffer.size() >= 32768) {
            return make_error(ErrorCode::capacity, "executable path exceeds Windows path limit");
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
using GetDpiWindowFn = UINT(WINAPI*)(HWND);
using GetDpiSystemFn = UINT(WINAPI*)();
using AdjustRectDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
using GetWindowDpiContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
using GetThreadDpiContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)();
using EqualDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
using GetAwarenessFn = DPI_AWARENESS(WINAPI*)(DPI_AWARENESS_CONTEXT);

Status read_dpi(HWND window, UINT& dpi) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return win32_error("GetModuleHandleW(user32)");
    if (window) {
        if (auto fn = reinterpret_cast<GetDpiWindowFn>(GetProcAddress(user32, "GetDpiForWindow"))) {
            dpi = fn(window);
            if (dpi == 0) return win32_error("GetDpiForWindow");
            return std::nullopt;
        }
    }
    if (auto fn = reinterpret_cast<GetDpiSystemFn>(GetProcAddress(user32, "GetDpiForSystem"))) {
        dpi = fn();
        if (dpi == 0) return win32_error("GetDpiForSystem");
        return std::nullopt;
    }
    const HDC dc = GetDC(nullptr);
    if (!dc) return win32_error("GetDC for DPI");
    const int pixels = GetDeviceCaps(dc, LOGPIXELSX);
    if (!ReleaseDC(nullptr, dc)) return win32_error("ReleaseDC for DPI");
    if (pixels <= 0) return win32_error("GetDeviceCaps(LOGPIXELSX)");
    dpi = static_cast<UINT>(pixels);
    return std::nullopt;
}

Status adjust_client_rect(RECT& rect, UINT dpi) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return win32_error("GetModuleHandleW(user32)");
    if (auto fn = reinterpret_cast<AdjustRectDpiFn>(
            GetProcAddress(user32, "AdjustWindowRectExForDpi"))) {
        if (!fn(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
            return win32_error("AdjustWindowRectExForDpi");
        }
    } else if (!AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0)) {
        return win32_error("AdjustWindowRectEx");
    }
    return std::nullopt;
}

Status actual_awareness(HWND window, std::string& description) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return win32_error("GetModuleHandleW(user32)");
    const auto window_context = reinterpret_cast<GetWindowDpiContextFn>(
        GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
    const auto thread_context = reinterpret_cast<GetThreadDpiContextFn>(
        GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
    const auto equal = reinterpret_cast<EqualDpiContextFn>(
        GetProcAddress(user32, "AreDpiAwarenessContextsEqual"));
    const auto awareness = reinterpret_cast<GetAwarenessFn>(
        GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext"));
    if ((window ? window_context != nullptr : thread_context != nullptr) && equal && awareness) {
        const auto context = window ? window_context(window) : thread_context();
        if (!context) return win32_error("Get DPI awareness context");
        if (equal(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            description = "per-monitor-v2";
        } else {
            switch (awareness(context)) {
            case DPI_AWARENESS_UNAWARE: description = "unaware"; break;
            case DPI_AWARENESS_SYSTEM_AWARE: description = "system-aware"; break;
            case DPI_AWARENESS_PER_MONITOR_AWARE: description = "per-monitor-v1"; break;
            default: return win32_error("GetAwarenessFromDpiAwarenessContext");
            }
        }
    } else {
        description = IsProcessDPIAware() ? "aware (legacy query; subtype unavailable)" : "unaware";
    }
    return std::nullopt;
}

bool removal_hresult(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

class FrameMeasurement {
public:
    explicit FrameMeasurement(bool enabled) : active_(enabled) {
        if (active_) {
            started_ = std::chrono::steady_clock::now();
            begin_allocation_sample();
        }
    }
    ~FrameMeasurement() {
        if (active_) end_allocation_sample();
    }
    FrameSample finish() {
        FrameSample result;
        if (active_) {
            const auto allocations = end_allocation_sample();
            const auto stopped = std::chrono::steady_clock::now();
            active_ = false;
            result.cpu_us = std::chrono::duration<double, std::micro>(stopped - started_).count();
            result.calls = allocations.calls;
            result.bytes = allocations.bytes;
        }
        return result;
    }
private:
    bool active_;
    std::chrono::steady_clock::time_point started_{};
};

}  // namespace

struct NativeApp::Impl {
    Driver driver = Driver::hardware;
    HWND window = nullptr;
    ATOM window_class = 0;
    std::shared_ptr<wui::win32::Adapter> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    std::uint32_t target_w = 0;
    std::uint32_t target_h = 0;
    std::unique_ptr<wui::d3d11::Renderer> renderer;
    std::shared_ptr<TextureStore> textures;
    std::shared_ptr<IText> text;
    std::unique_ptr<wui::Context> ui;
    GalleryState gallery;
    // Model theme inputs last applied with Context::set_theme.
    std::optional<std::array<int, 4>> applied_theme;
    std::optional<StressWorkload> workload;
    struct GpuQuery {
        ComPtr<ID3D11Query> disjoint, begin, end;
        std::uint64_t frame = 0;
        bool pending = false;
    };
    struct GpuTiming {
        std::uint64_t frame = 0;
        double us = -1;
        const char* status = "unmeasured";
    };
    std::array<GpuQuery, 16> gpu_queries;
    ID3D11Device* gpu_query_device = nullptr;
    bool gpu_queries_unavailable = false;
    std::array<GpuTiming, 1024> gpu_results;
    std::uint64_t gpu_frame = 0;
    std::optional<wui::FrameInput> pending_input;
    wui::DrawPacket pending_packet;
    std::vector<wui::Semantic> last_semantics;
    wui::DrawPacket last_packet;
    std::optional<std::filesystem::path> next_capture;
    std::optional<float> validation_scale;
    bool disabled_mode = false;
    bool evidence_enabled = false;
    unsigned submits = 0;
    unsigned presents = 0;
    unsigned dpi_changes = 0;
    bool close_requested = false;
    bool minimized = false;
    bool occluded = false;
    bool reset_clock = false;
    std::chrono::steady_clock::time_point next_occlusion_test{};
    bool resize_pending = false;
    std::uint64_t epoch = 0;
    std::uint64_t sampled_epoch = 0;
    std::optional<FrameSample> pending_sample;
    const char* host_error_operation = nullptr;
    DWORD host_error_code = ERROR_SUCCESS;
    ErrorCode host_error_kind = ErrorCode::backend_failure;
    std::string dpi_mode = "unknown";
    float current_dpi_scale = 1.0f;
    bool com_initialized = false;
    bool debug_enabled = false;
    std::string adapter_desc = "unknown";
    bool in_busy_wait = false;
    ComPtr<ID3D11Texture2D> staging;
    std::uint32_t staging_w = 0;
    std::uint32_t staging_h = 0;

    void latch(const char* operation, DWORD code = GetLastError(),
        ErrorCode kind = ErrorCode::backend_failure) noexcept {
        if (!host_error_operation) {
            host_error_operation = operation;
            host_error_code = code ? code : ERROR_GEN_FAILURE;
            host_error_kind = kind;
        }
    }
    Status status() const {
        if (!host_error_operation) return std::nullopt;
        return make_error(host_error_kind,
            hr_string(HRESULT_FROM_WIN32(host_error_code), host_error_operation));
    }
    Error graphics_error(HRESULT hr, const char* operation) const {
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
        std::string message = hr_string(hr, operation);
        if (FAILED(reason)) message += "; " + hr_string(reason, "GetDeviceRemovedReason");
        return make_error(removal_hresult(hr) || FAILED(reason) ?
            ErrorCode::device_lost : ErrorCode::backend_failure, std::move(message));
    }
    void release_target() {
        if (context) context->OMSetRenderTargets(0, nullptr, nullptr);
        rtv.Reset();
        staging.Reset();
        staging_w = staging_h = 0;
    }
};

NativeApp::NativeApp() : impl_(new Impl()) {}
NativeApp::~NativeApp() {
    if (!impl_) return;
    const bool uninitialize_com = impl_->com_initialized;
    // Detach before destruction: ReleaseCapture can re-enter our WndProc.
    // The context borrows the clipboard adapter, so it dies before the adapter.
    impl_->ui.reset();
    auto adapter = std::move(impl_->adapter);
    adapter.reset();
    impl_->pending_input.reset();
    impl_->pending_packet.reset();
    impl_->last_packet.reset();
    impl_->renderer.reset();
    impl_->release_target();
    if (impl_->context) {
        impl_->context->ClearState();
        impl_->context->Flush();
    }
    impl_->swapchain.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
    if (impl_->window && !DestroyWindow(impl_->window)) {
        std::fprintf(stderr, "DestroyWindow failed (win32=%lu)\n", GetLastError());
    }
    if (impl_->window_class &&
        !UnregisterClassW(L"wui.A10.WidgetGallery", GetModuleHandleW(nullptr))) {
        std::fprintf(stderr, "UnregisterClassW failed (win32=%lu)\n", GetLastError());
    }
    // Text services and every COM-owning member die before the apartment.
    impl_.reset();
    if (uninitialize_com) CoUninitialize();
}

HWND NativeApp::window() const { return impl_->window; }
ID3D11Device* NativeApp::device() const { return impl_->device.Get(); }
ID3D11DeviceContext* NativeApp::device_context() const { return impl_->context.Get(); }
wui::d3d11::Renderer* NativeApp::renderer() const { return impl_->renderer.get(); }
wui::Context* NativeApp::ui_context() const { return impl_->ui.get(); }
std::pair<std::uint32_t, std::uint32_t> NativeApp::target_size() const {
    return {impl_->target_w, impl_->target_h};
}
const GalleryState& NativeApp::gallery_state() const { return impl_->gallery; }
GalleryState& NativeApp::gallery_state() { return impl_->gallery; }
const std::vector<wui::Semantic>& NativeApp::last_semantics() const { return impl_->last_semantics; }
wui::DrawPacket NativeApp::last_packet() const { return impl_->last_packet; }
unsigned NativeApp::submit_count() const { return impl_->submits; }
unsigned NativeApp::present_count() const { return impl_->presents; }
unsigned NativeApp::dpi_change_count() const { return impl_->dpi_changes; }
float NativeApp::adapter_scale() const { return impl_->current_dpi_scale; }
bool NativeApp::debug_enabled() const { return impl_->debug_enabled; }
void NativeApp::set_validation_scale(std::optional<float> scale) {
    if (impl_->validation_scale != scale) {
        impl_->validation_scale = scale;
        impl_->resize_pending = true;
        ++impl_->epoch;
    }
}
void NativeApp::set_disabled_mode(bool disabled) {
    if (impl_->disabled_mode != disabled) ++impl_->epoch;
    impl_->disabled_mode = disabled;
}
void NativeApp::set_evidence_enabled(bool enabled) {
    impl_->evidence_enabled = enabled;
    if (!enabled) {
        impl_->last_packet.reset();
        impl_->last_semantics.clear();
    }
}
void NativeApp::set_workload(std::optional<StressWorkload> workload) {
    if (impl_->workload != workload) ++impl_->epoch;
    impl_->workload = workload;
    impl_->applied_theme.reset();
}
std::optional<StressWorkload> NativeApp::workload() const { return impl_->workload; }
Status NativeApp::apply_model_theme() {
    Impl& impl = *impl_;
    if (!impl.ui) return make_error(ErrorCode::invalid_frame, "theme requires a context");
    const std::array<int, 4> key{impl.gallery.density, impl.gallery.accent_choice,
                                 impl.gallery.reduced_motion ? 1 : 0,
                                 impl.workload ? static_cast<int>(*impl.workload) : -1};
    if (impl.applied_theme == key) return std::nullopt;
    auto theme = impl.workload ? stress_theme(impl.gallery) : gallery_theme(impl.gallery);
    if (auto* error = std::get_if<Error>(&theme)) return *error;
    if (auto error = impl.ui->set_theme(std::get<Theme>(theme))) return error;
    impl.applied_theme = key;
    return std::nullopt;
}
void NativeApp::collect_gpu_timings(bool wait) {
    Impl& impl = *impl_;
    if (!impl.context) return;
    if (wait) impl.context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (auto& query : impl.gpu_queries) {
        while (query.pending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const HRESULT hr = impl.context->GetData(query.disjoint.Get(), &disjoint,
                sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) {
                if (!wait || std::chrono::steady_clock::now() >= deadline) break;
                SwitchToThread();
                continue;
            }
            query.pending = false;
            if (query.frame == 0) break; // discarded submission
            auto& result = impl.gpu_results[query.frame % impl.gpu_results.size()];
            result = {query.frame, -1, "unavailable: query data failed"};
            if (FAILED(hr)) break;
            if (disjoint.Disjoint || disjoint.Frequency == 0) {
                result.status = "unavailable: disjoint";
                break;
            }
            UINT64 begin = 0, end = 0;
            if (impl.context->GetData(query.begin.Get(), &begin, sizeof(begin),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                impl.context->GetData(query.end.Get(), &end, sizeof(end),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || end < begin) {
                result.status = "unavailable: timestamp data";
                break;
            }
            result.us = static_cast<double>(end - begin) * 1e6 /
                static_cast<double>(disjoint.Frequency);
            result.status = "ok";
        }
    }
}
void NativeApp::resolve_gpu_timing(FrameSample& sample) const {
    if (sample.gpu_frame == 0) {
        if (std::string_view(sample.gpu_status) == "unmeasured")
            sample.gpu_status = "unavailable: no query issued";
        return;
    }
    const auto& result = impl_->gpu_results[sample.gpu_frame % impl_->gpu_results.size()];
    if (result.frame != sample.gpu_frame) {
        sample.gpu_status = "unavailable: result not ready";
        return;
    }
    sample.gpu_us = result.us;
    sample.gpu_status = result.status;
}
void NativeApp::schedule_capture(const std::filesystem::path& path) { impl_->next_capture = path; }
std::string NativeApp::driver_description() const { return impl_->adapter_desc; }
std::string NativeApp::dpi_description() const {
    char buffer[160] = {};
    std::snprintf(buffer, sizeof(buffer), "mode=%s scale=%.3f", impl_->dpi_mode.c_str(),
        static_cast<double>(impl_->current_dpi_scale));
    return std::string(buffer);
}

Status NativeApp::reset_validation_scenario() {
    if (!impl_->textures || !impl_->text) {
        return make_error(ErrorCode::invalid_frame, "validation scenario requires initialized text services");
    }
    SendMessageW(impl_->window, WM_CANCELMODE, 0, 0);
    if (GetCapture() == impl_->window && !ReleaseCapture()) return win32_error("ReleaseCapture");
    pump_messages();
    if (auto error = impl_->status()) return error;
    if (impl_->close_requested) {
        return make_error(ErrorCode::invalid_frame, "window closed during scenario reset");
    }
    if (impl_->adapter) {
        auto discarded = impl_->adapter->take_frame(1.0 / 60.0);
        if (auto error = impl_->adapter->status()) return error;
    }
    ContextDesc desc;
    desc.theme = Theme::dark();
    desc.textures = impl_->textures;
    desc.text = impl_->text;
    desc.clipboard = impl_->adapter;
    auto created = Context::create(std::move(desc));
    if (auto* error = std::get_if<Error>(&created)) return *error;
    impl_->ui = std::get<std::unique_ptr<Context>>(std::move(created));
    const auto icon = impl_->gallery.icon;
    impl_->gallery = GalleryState{};
    prepare_gallery_model(impl_->gallery);
    impl_->gallery.icon = icon;
    impl_->applied_theme.reset();
    impl_->pending_input.reset();
    impl_->pending_packet.reset();
    impl_->pending_sample.reset();
    impl_->last_semantics.clear();
    impl_->last_packet.reset();
    impl_->next_capture.reset();
    impl_->in_busy_wait = false;
    ++impl_->epoch;
    impl_->sampled_epoch = impl_->epoch;
    return std::nullopt;
}

LRESULT CALLBACK NativeApp::wnd_proc(HWND window, UINT message, WPARAM wparam,
    LPARAM lparam) noexcept {
    auto* self = reinterpret_cast<NativeApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam) {
        self = static_cast<NativeApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)) == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            if (self && self->impl_) self->impl_->latch("SetWindowLongPtrW(GWLP_USERDATA)");
            return FALSE;
        }
    }
    if (!self || !self->impl_) return DefWindowProcW(window, message, wparam, lparam);
    Impl& impl = *self->impl_;
    if (message == WM_NCDESTROY) {
        impl.window = nullptr;
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(window, GWLP_USERDATA, 0) == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            impl.latch("clear GWLP_USERDATA");
        }
    }
    try {
        if (message == WM_DPICHANGED) ++impl.dpi_changes;
        if ((message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_SETFOCUS) {
            ++impl.epoch;
        }
        if (impl.adapter && impl.adapter->process(message, wparam, lparam)) {
            ++impl.epoch;
            // System keys stay default-handled (Alt+F4, Alt+Space, F10);
            // handled X buttons report TRUE.
            if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
                return DefWindowProcW(window, message, wparam, lparam);
            if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK)
                return TRUE;
            return 0;
        }
        switch (message) {
        case WM_DPICHANGED: {
            ++impl.epoch;
            impl.resize_pending = true;
            if (!lparam) {
                impl.latch("WM_DPICHANGED missing suggested RECT", ERROR_INVALID_PARAMETER);
                return 0;
            }
            const auto& rect = *reinterpret_cast<const RECT*>(lparam);
            if (!SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left,
                    rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER)) {
                impl.latch("SetWindowPos(WM_DPICHANGED)");
            }
            return 0;
        }
        case WM_SIZE: {
            ++impl.epoch;
            impl.reset_clock = true;
            RECT client{};
            if (!GetClientRect(window, &client)) {
                impl.latch("GetClientRect(WM_SIZE)");
                return 0;
            }
            impl.minimized = wparam == SIZE_MINIMIZED ||
                client.right <= client.left || client.bottom <= client.top;
            impl.resize_pending = true;
            return 0;
        }
        case WM_GETMINMAXINFO: {
            // The size matrix needs physical clients up to 1920x1080 even on
            // monitors whose default tracking limit is smaller.
            DefWindowProcW(window, message, wparam, lparam);
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lparam);
            limits.ptMaxTrackSize.x = std::max<LONG>(limits.ptMaxTrackSize.x, 4096);
            limits.ptMaxTrackSize.y = std::max<LONG>(limits.ptMaxTrackSize.y, 4096);
            return 0;
        }
        case WM_CLOSE:
            impl.close_requested = true;
            ++impl.epoch;
            return 0;
        case WM_DESTROY:
            impl.close_requested = true;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    } catch (const std::bad_alloc&) {
        impl.latch("allocation failed in WndProc", ERROR_NOT_ENOUGH_MEMORY, ErrorCode::capacity);
    } catch (...) {
        impl.latch("exception in WndProc", ERROR_UNHANDLED_EXCEPTION);
    }
    return message == WM_NCCREATE ? FALSE : 0;
}

Status NativeApp::create_window_and_adapter() {
    Impl& impl = *impl_;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return win32_error("GetModuleHandleW(user32)");
    if (auto fn = reinterpret_cast<SetDpiContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
        if (!fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            const DWORD code = GetLastError();
            if (code != ERROR_ACCESS_DENIED) return win32_error("SetProcessDpiAwarenessContext", code);
        }
    } else if (!SetProcessDPIAware()) {
        const DWORD code = GetLastError();
        if (code != ERROR_ACCESS_DENIED) return win32_error("SetProcessDPIAware", code);
    }
    if (auto error = actual_awareness(nullptr, impl.dpi_mode)) return error;

    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = &NativeApp::wnd_proc;
    definition.hInstance = GetModuleHandleW(nullptr);
    if (!definition.hInstance) return win32_error("GetModuleHandleW(executable)");
    definition.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (!definition.hCursor) return win32_error("LoadCursorW");
    definition.lpszClassName = L"wui.A10.WidgetGallery";
    impl.window_class = RegisterClassExW(&definition);
    if (!impl.window_class) return win32_error("RegisterClassExW");

    UINT dpi = 96;
    if (auto error = read_dpi(nullptr, dpi)) return error;
    RECT rect{0, 0, static_cast<LONG>(std::lround(640.0 * dpi / 96.0)),
        static_cast<LONG>(std::lround(400.0 * dpi / 96.0))};
    if (auto error = adjust_client_rect(rect, dpi)) return error;
    impl.window = CreateWindowExW(0, definition.lpszClassName, L"wui \u2014 Widget foundation",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
        rect.bottom - rect.top, nullptr, nullptr, definition.hInstance, this);
    if (auto error = impl.status()) return error;
    if (!impl.window) return win32_error("CreateWindowExW");
    if (auto error = read_dpi(impl.window, dpi)) return error;
    if (auto error = actual_awareness(impl.window, impl.dpi_mode)) return error;
    impl.current_dpi_scale = static_cast<float>(dpi) / 96.0f;
    rect = {0, 0, static_cast<LONG>(std::lround(640.0 * dpi / 96.0)),
        static_cast<LONG>(std::lround(400.0 * dpi / 96.0))};
    if (auto error = adjust_client_rect(rect, dpi)) return error;
    if (!SetWindowPos(impl.window, nullptr, 0, 0, rect.right - rect.left,
            rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return win32_error("SetWindowPos(initial actual DPI)");
    }
    if (auto error = impl.status()) return error;
    impl.adapter = std::make_shared<wui::win32::Adapter>(impl.window);
    return impl.adapter->status();
}

Status NativeApp::create_device_resources(Driver driver) {
    Impl& impl = *impl_;
    impl.driver = driver;
    const D3D_DRIVER_TYPE type =
        driver == Driver::warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    DXGI_SWAP_CHAIN_DESC swap{};
    swap.BufferCount = 2;
    swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap.BufferDesc.RefreshRate.Numerator = 60;
    swap.BufferDesc.RefreshRate.Denominator = 1;
    swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.OutputWindow = impl.window;
    swap.SampleDesc.Count = 1;
    swap.Windowed = TRUE;
    swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, flags, levels, 1,
        D3D11_SDK_VERSION, &swap, impl.swapchain.GetAddressOf(), impl.device.GetAddressOf(),
        nullptr, impl.context.GetAddressOf());
    impl.debug_enabled = SUCCEEDED(hr);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        impl.swapchain.Reset();
        impl.context.Reset();
        impl.device.Reset();
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, flags, levels, 1,
            D3D11_SDK_VERSION, &swap, impl.swapchain.GetAddressOf(), impl.device.GetAddressOf(),
            nullptr, impl.context.GetAddressOf());
    }
    if (FAILED(hr)) return impl.graphics_error(hr, "D3D11CreateDeviceAndSwapChain");
    if (impl.device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        return make_error(ErrorCode::unsupported, "D3D11 requires feature level 11_0");
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = impl.device.As(&dxgi_device);
    if (FAILED(hr)) return impl.graphics_error(hr, "QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) return impl.graphics_error(hr, "GetAdapter");
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) return impl.graphics_error(hr, "GetAdapterDesc");
    char narrow[512] = {};
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, desc.Description, -1,
            narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr)) {
        return win32_error("WideCharToMultiByte(adapter description)");
    }
    char identity[768] = {};
    std::snprintf(identity, sizeof(identity),
        "%s vendor=0x%04X device=0x%04X luid=%08lX:%08lX feature=11_0 debug=%s",
        narrow, desc.VendorId, desc.DeviceId,
        static_cast<unsigned long>(desc.AdapterLuid.HighPart),
        static_cast<unsigned long>(desc.AdapterLuid.LowPart), impl.debug_enabled ? "yes" : "no");
    impl.adapter_desc = identity;
    LARGE_INTEGER version{};
    const HRESULT version_hr = adapter->CheckInterfaceSupport(__uuidof(ID3D11Device), &version);
    char driver_info[128] = {};
    if (SUCCEEDED(version_hr)) {
        std::snprintf(driver_info, sizeof(driver_info), " driver=%08lX:%08lX driver_hr=0x%08lX",
            static_cast<unsigned long>(version.HighPart), static_cast<unsigned long>(version.LowPart),
            static_cast<unsigned long>(version_hr));
    } else {
        std::snprintf(driver_info, sizeof(driver_info), " driver=unavailable driver_hr=0x%08lX",
            static_cast<unsigned long>(version_hr));
    }
    impl.adapter_desc += driver_info;
    ComPtr<IDXGIFactory> factory;
    hr = impl.swapchain->GetParent(__uuidof(IDXGIFactory),
        reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) return impl.graphics_error(hr, "GetParent(IDXGIFactory)");
    hr = factory->MakeWindowAssociation(impl.window, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) return impl.graphics_error(hr, "MakeWindowAssociation(NO_ALT_ENTER)");
    if (auto error = create_target()) return error;
    if (impl.renderer) return impl.renderer->recreate(impl.device.Get());
    auto created = wui::d3d11::Renderer::create(impl.device.Get());
    if (auto* error = std::get_if<Error>(&created)) return *error;
    impl.renderer = std::get<std::unique_ptr<wui::d3d11::Renderer>>(std::move(created));
    return std::nullopt;
}

Status NativeApp::create_target() {
    Impl& impl = *impl_;
    ComPtr<ID3D11Texture2D> backbuffer;
    HRESULT hr = impl.swapchain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()));
    if (FAILED(hr)) return impl.graphics_error(hr, "GetBuffer");
    hr = impl.device->CreateRenderTargetView(backbuffer.Get(), nullptr, impl.rtv.GetAddressOf());
    if (FAILED(hr)) return impl.graphics_error(hr, "CreateRenderTargetView");
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    impl.target_w = desc.Width;
    impl.target_h = desc.Height;
    return std::nullopt;
}

Status NativeApp::create_ui_services() {
    Impl& impl = *impl_;
    std::filesystem::path executable;
    if (auto error = executable_dir(executable)) return error;
    const auto base = executable / "assets" / "fonts";
    std::vector<std::byte> regular;
    std::vector<std::byte> semibold;
    std::string detail;
    if (!load_bytes_checked(base / "Inter-Regular.ttf", regular, detail)) {
        return make_error(ErrorCode::invalid_argument, "font load failed: " + detail);
    }
    if (!load_bytes_checked(base / "Inter-SemiBold.ttf", semibold, detail)) {
        return make_error(ErrorCode::invalid_argument, "font load failed: " + detail);
    }
    impl.textures = std::make_shared<TextureStore>(64 * 1024 * 1024);
    auto made = make_text_service(regular, semibold, impl.textures);
    if (auto* error = std::get_if<Error>(&made)) return *error;
    impl.text = std::get<std::shared_ptr<IText>>(std::move(made));
    ContextDesc desc;
    desc.theme = Theme::dark();
    desc.textures = impl.textures;
    desc.text = impl.text;
    desc.clipboard = impl.adapter;
    auto created = Context::create(std::move(desc));
    if (auto* error = std::get_if<Error>(&created)) return *error;
    impl.ui = std::get<std::unique_ptr<Context>>(std::move(created));
    prepare_gallery_model(impl.gallery);
    auto icon = make_gallery_icon(*impl.textures);
    if (auto* error = std::get_if<Error>(&icon)) return *error;
    impl.gallery.icon = std::get<TextureHandle>(icon);
    impl.applied_theme.reset();
    return std::nullopt;
}

Status NativeApp::initialize(Driver driver) {
    Impl& impl = *impl_;
    if (impl.window || impl.com_initialized) {
        return make_error(ErrorCode::invalid_frame, "host is already initialized");
    }
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(com_hr) || com_hr == RPC_E_CHANGED_MODE) {
        impl.com_initialized = SUCCEEDED(com_hr);
    } else {
        return make_error(ErrorCode::backend_failure, hr_string(com_hr, "CoInitializeEx failed"));
    }
    if (auto error = create_window_and_adapter()) return *error;
    if (auto error = create_device_resources(driver)) return *error;
    if (auto error = create_ui_services()) return *error;
    impl.resize_pending = false;
    ShowWindow(impl.window, SW_SHOW);
    return impl.status();
}

void NativeApp::pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            impl_->close_requested = true;
            ++impl_->epoch;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

Status NativeApp::update_pending_input() {
    Impl& impl = *impl_;
    UINT dpi = 96;
    if (auto error = read_dpi(impl.window, dpi)) return error;
    impl.current_dpi_scale = static_cast<float>(dpi) / 96.0f;
    const float scale = impl.validation_scale.value_or(impl.current_dpi_scale);
    if (!(scale > 0) || !std::isfinite(scale)) {
        return make_error(ErrorCode::invalid_argument, "validation scale must be finite and positive");
    }
    if (impl.pending_input) {
        const float old_scale = impl.pending_input->scale;
        if (!(old_scale > 0) || !std::isfinite(old_scale)) {
            return make_error(ErrorCode::invalid_frame, "input scale must be finite and positive");
        }
        const float factor = old_scale / scale;
        for (auto& event : impl.pending_input->events) {
            event.position.x *= factor;
            event.position.y *= factor;
        }
        impl.pending_input->viewport = {0, 0, static_cast<float>(impl.target_w) / scale,
            static_cast<float>(impl.target_h) / scale};
        impl.pending_input->scale = scale;
    }
    return std::nullopt;
}

Status NativeApp::apply_pending_resize() {
    Impl& impl = *impl_;
    if (!impl.resize_pending || impl.minimized) return std::nullopt;
    RECT client{};
    if (!GetClientRect(impl.window, &client)) return win32_error("GetClientRect(resize)");
    if (client.right <= client.left || client.bottom <= client.top) {
        impl.minimized = true;
        return std::nullopt;
    }
    const auto width = static_cast<std::uint32_t>(client.right - client.left);
    const auto height = static_cast<std::uint32_t>(client.bottom - client.top);
    impl.pending_packet.reset();
    impl.pending_sample.reset();
    if (width != impl.target_w || height != impl.target_h || !impl.rtv) {
        impl.release_target();
        const HRESULT hr = impl.swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return impl.graphics_error(hr, "ResizeBuffers");
        if (auto error = create_target()) return error;
    }
    if (auto error = update_pending_input()) return error;
    impl.resize_pending = false;
    impl.in_busy_wait = false;
    return std::nullopt;
}

Status NativeApp::resize(std::uint32_t width, std::uint32_t height) {
    Impl& impl = *impl_;
    if (!impl.window) return make_error(ErrorCode::invalid_frame, "resize requires a window");
    if (width == 0 || height == 0 || width > 32767 || height > 32767) {
        return make_error(ErrorCode::invalid_argument, "resize dimensions must be in [1,32767]");
    }
    UINT dpi = 96;
    if (auto error = read_dpi(impl.window, dpi)) return error;
    RECT desired{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (auto error = adjust_client_rect(desired, dpi)) return error;
    if (!SetWindowPos(impl.window, nullptr, 0, 0, desired.right - desired.left,
            desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return win32_error("SetWindowPos(resize)");
    }
    impl.resize_pending = true;
    ++impl.epoch;
    pump_messages();
    if (auto error = impl.status()) return error;
    if (auto error = apply_pending_resize()) {
        if (error->code == ErrorCode::device_lost) return recreate_device();
        return error;
    }
    return std::nullopt;
}

Status NativeApp::recreate_device() {
    Impl& impl = *impl_;
    if (!impl.device || !impl.swapchain || !impl.renderer) {
        return make_error(ErrorCode::invalid_frame, "recreate requires a device");
    }
    ++impl.epoch;
    impl.reset_clock = true;
    impl.pending_sample.reset();
    bool removed = FAILED(impl.device->GetDeviceRemovedReason());
    if (!removed) {
        ComPtr<ID3D11Query> fence;
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_EVENT;
        const HRESULT created = impl.device->CreateQuery(&desc, fence.GetAddressOf());
        if (FAILED(created)) {
            auto error = impl.graphics_error(created, "CreateQuery(device quiescence)");
            if (error.code != ErrorCode::device_lost) return error;
            removed = true;
        } else {
            impl.context->End(fence.Get());
            impl.context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                const HRESULT hr = impl.context->GetData(
                    fence.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr == S_OK) break;
                if (hr != S_FALSE) {
                    auto error = impl.graphics_error(hr, "GetData(device quiescence)");
                    if (error.code != ErrorCode::device_lost) return error;
                    removed = true;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    return make_error(ErrorCode::backend_failure, "device quiescence timed out after 5s");
                }
                if (auto error = wait_for_messages(1)) return error;
                if (impl.close_requested) {
                    return make_error(ErrorCode::invalid_frame, "window closed during device quiescence");
                }
            }
        }
    }
    if (!removed) {
        if (auto error = impl.renderer->collect(impl.context.Get())) {
            if (error->code != ErrorCode::device_lost &&
                SUCCEEDED(impl.device->GetDeviceRemovedReason())) return error;
        }
    }
    impl.pending_packet.reset();
    impl.release_target();
    impl.context->ClearState();
    impl.context->Flush();
    impl.swapchain.Reset();
    impl.context.Reset();
    impl.device.Reset();
    impl.in_busy_wait = false;
    impl.occluded = false;
    if (auto error = create_device_resources(impl.driver)) return error;
    impl.resize_pending = false;
    return update_pending_input();
}

Status NativeApp::capture(const std::filesystem::path& path) {
    Impl& impl = *impl_;
    if (!impl.device || !impl.context || !impl.swapchain || !impl.rtv) {
        return make_error(ErrorCode::invalid_frame, "capture requires a swap chain target");
    }
    ComPtr<ID3D11Texture2D> backbuffer;
    HRESULT hr = impl.swapchain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()));
    if (FAILED(hr)) return impl.graphics_error(hr, "capture GetBuffer");
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        return make_error(ErrorCode::invalid_argument, "capture target has zero extent");
    }
    const std::uint64_t stride64 = static_cast<std::uint64_t>(desc.Width) * 4;
    const std::uint64_t size64 = stride64 * desc.Height;
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || size64 > std::numeric_limits<UINT>::max() ||
        size64 > std::numeric_limits<std::size_t>::max()) {
        return make_error(ErrorCode::capacity, "capture format or extent exceeds PNG buffer limits");
    }
    const UINT stride = static_cast<UINT>(stride64);
    const UINT buffer_size = static_cast<UINT>(size64);
    if (!impl.staging || impl.staging_w != desc.Width || impl.staging_h != desc.Height) {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        impl.staging.Reset();
        hr = impl.device->CreateTexture2D(&staging, nullptr, impl.staging.GetAddressOf());
        if (FAILED(hr)) return impl.graphics_error(hr, "capture CreateTexture2D");
        impl.staging_w = desc.Width;
        impl.staging_h = desc.Height;
    }
    impl.context->CopyResource(impl.staging.Get(), backbuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = impl.context->Map(impl.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return impl.graphics_error(hr, "capture Map");
    struct UnmapGuard {
        ID3D11DeviceContext* context = nullptr;
        ID3D11Texture2D* texture = nullptr;
        ~UnmapGuard() {
            if (context && texture) context->Unmap(texture, 0);
        }
    } guard{impl.context.Get(), impl.staging.Get()};
    if (!mapped.pData || mapped.RowPitch < stride) {
        return make_error(ErrorCode::backend_failure, "capture Map returned invalid row pitch");
    }

    ComPtr<IWICImagingFactory> factory;
    hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC factory failed"));
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC stream failed"));
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC stream open failed"));
    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC PNG encoder failed"));
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC encoder init failed"));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr = encoder->CreateNewFrame(&frame, &options);
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC frame failed"));
    hr = frame->Initialize(options.Get());
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC frame init failed"));
    hr = frame->SetSize(desc.Width, desc.Height);
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC SetSize failed"));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC SetPixelFormat failed"));
    if (format != GUID_WICPixelFormat32bppBGRA) {
        return make_error(ErrorCode::unsupported, "WIC refused 32bpp BGRA");
    }
    // Backbuffer is R8G8B8A8_UNORM (RGBA byte order); WIC PNG expects BGRA.
    // Swizzle into a contiguous buffer to keep stride simple and deterministic.
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(size64));
    const auto* rows = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t y = 0; y < desc.Height; ++y) {
        const auto* row = rows + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* out = bgra.data() + static_cast<std::size_t>(y) * desc.Width * 4;
        for (std::uint32_t x = 0; x < desc.Width; ++x) {
            out[x * 4 + 0] = row[x * 4 + 2];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + 0];
            out[x * 4 + 3] = row[x * 4 + 3];
        }
    }
    hr = frame->WritePixels(desc.Height, stride, buffer_size, bgra.data());
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC WritePixels failed"));
    hr = frame->Commit();
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC frame commit failed"));
    hr = encoder->Commit();
    if (FAILED(hr)) return make_error(ErrorCode::backend_failure, hr_string(hr, "WIC encoder commit failed"));
    return std::nullopt;
}

Status NativeApp::wait_for_messages(DWORD milliseconds) {
    const DWORD result = MsgWaitForMultipleObjectsEx(
        0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED) return win32_error("MsgWaitForMultipleObjectsEx");
    if (result == WAIT_OBJECT_0) pump_messages();
    if (auto error = impl_->status()) return error;
    if (impl_->adapter) return impl_->adapter->status();
    return std::nullopt;
}

Status NativeApp::wait_busy() {
    if (!impl_->in_busy_wait) {
        impl_->context->Flush();
        impl_->in_busy_wait = true;
    }
    return wait_for_messages(1);
}

Status NativeApp::drain_or_submit(double dt_seconds, bool& did_present,
    FrameSample* sample, bool* interrupted) {
    Impl& impl = *impl_;
    did_present = false;
    pump_messages();
    if (auto error = impl.status()) return error;
    if (impl.close_requested) {
        if (sample || impl.evidence_enabled) {
            return make_error(ErrorCode::invalid_frame, "window closed before completing the frame");
        }
        return std::nullopt;
    }
    if (!impl.adapter || !impl.ui || !impl.renderer || !impl.device || !impl.swapchain) {
        return make_error(ErrorCode::invalid_frame, "host is not initialized");
    }
    if (auto error = impl.adapter->status()) return error;
    auto interrupt_sample = [&] {
        if (interrupted) *interrupted = true;
        impl.pending_sample.reset();
    };
    auto handle_graphics_error = [&](Error error) -> Status {
        const HRESULT reason = impl.device->GetDeviceRemovedReason();
        if (error.code == ErrorCode::device_lost ||
            (error.code == ErrorCode::backend_failure && FAILED(reason))) {
            interrupt_sample();
            std::fprintf(stderr, "recovering device: %s\n", error.message.c_str());
            return recreate_device();
        }
        return error;
    };
    if (impl.resize_pending) {
        interrupt_sample();
        if (auto error = apply_pending_resize()) return handle_graphics_error(std::move(*error));
    }
    if (auto error = impl.renderer->collect(impl.context.Get())) {
        return handle_graphics_error(std::move(*error));
    }
    if (impl.minimized || impl.target_w == 0 || impl.target_h == 0) {
        interrupt_sample();
        impl.reset_clock = true;
        impl.in_busy_wait = false;
        return wait_for_messages(16);
    }
    if (impl.occluded) {
        interrupt_sample();
        impl.reset_clock = true;
        const auto now = std::chrono::steady_clock::now();
        if (now < impl.next_occlusion_test) return wait_for_messages(16);
        const HRESULT hr = impl.swapchain->Present(0, DXGI_PRESENT_TEST);
        impl.next_occlusion_test = now + std::chrono::milliseconds(100);
        if (hr == DXGI_STATUS_OCCLUDED) return wait_for_messages(16);
        if (FAILED(hr)) return handle_graphics_error(impl.graphics_error(hr, "Present(TEST)"));
        impl.occluded = false;
        ++impl.epoch;
        return std::nullopt;
    }

    if (!impl.pending_input && !impl.pending_packet) {
        impl.pending_input = impl.adapter->take_frame(dt_seconds);
        if (auto error = impl.adapter->status()) return error;
        impl.current_dpi_scale = impl.pending_input->scale;
        if (impl.validation_scale) {
            if (auto error = update_pending_input()) return error;
        }
    }
    if (sample && (impl.epoch != impl.sampled_epoch ||
            (impl.pending_input && !impl.pending_input->events.empty()) ||
            !impl.gallery.notifications || impl.gallery.restore_count != 0 || impl.disabled_mode)) {
        interrupt_sample();
    }

    if (!impl.pending_packet) {
        if (!impl.pending_input) return std::nullopt;
        // The model selects the theme; apply it between frames, before begin.
        if (auto error = apply_model_theme()) return error;
        impl.gallery.viewport_height = impl.pending_input->viewport.h;
        FrameMeasurement measurement(sample != nullptr);
        auto begun = impl.ui->begin_frame(*impl.pending_input);
        if (auto* error = std::get_if<Error>(&begun)) {
            measurement.finish();
            if (error->code == ErrorCode::busy) return wait_busy();
            return *error;
        }
        wui::Frame frame = std::get<wui::Frame>(std::move(begun));
        wui::WidgetOptions options;
        options.enabled = !impl.disabled_mode;
        if (impl.workload) build_stress_gallery(frame, impl.gallery, *impl.workload);
        else build_gallery(frame, impl.gallery, options);
        auto finished = frame.finish();
        auto measured = measurement.finish();
        // begin consumed this input exactly once, including a model mutation.
        impl.pending_input.reset();
        if (auto* error = std::get_if<Error>(&finished)) return *error;
        if (auto caret = impl.ui->composition_caret())
            impl.adapter->set_composition_position(*caret);
        impl.pending_packet = std::get<wui::DrawPacket>(std::move(finished));
        if (!impl.pending_packet) {
            return make_error(ErrorCode::invalid_packet, "frame produced a null packet");
        }
        observe_gallery_frame(impl.gallery, *impl.pending_packet, impl.ui->diagnostics());
        if (sample && interrupted && !*interrupted) {
            measured.scale = impl.pending_packet->scale;
            measured.vertices = impl.pending_packet->vertices.size();
            measured.indices = impl.pending_packet->indices.size();
            measured.batches = impl.pending_packet->batches.size();
            measured.textures = impl.pending_packet->textures.size();
            measured.semantics = impl.pending_packet->semantics.size();
            impl.pending_sample = measured;
        }
    }

    const float clear[4] = {0x10 / 255.0f, 0x12 / 255.0f, 0x18 / 255.0f, 1.0f};
    impl.context->ClearRenderTargetView(impl.rtv.Get(), clear);
    wui::d3d11::Target target{impl.rtv.Get(), impl.target_w, impl.target_h};
    // Timestamp only measured frames' UI renderer submission; read asynchronously later.
    Impl::GpuQuery* gpu = nullptr;
    if (sample && impl.pending_sample) {
        collect_gpu_timings(false);
        if (impl.gpu_query_device != impl.device.Get()) {
            impl.gpu_query_device = impl.device.Get();
            impl.gpu_queries_unavailable = false;
            for (auto& query : impl.gpu_queries) {
                query = {};
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
                if (FAILED(impl.device->CreateQuery(&desc, query.disjoint.GetAddressOf())) ||
                    FAILED(impl.device->CreateQuery(&stamp, query.begin.GetAddressOf())) ||
                    FAILED(impl.device->CreateQuery(&stamp, query.end.GetAddressOf()))) {
                    impl.gpu_queries_unavailable = true;
                }
            }
        }
        if (impl.gpu_queries_unavailable) {
            impl.pending_sample->gpu_status = "unavailable: CreateQuery failed";
        } else {
            for (auto& query : impl.gpu_queries) {
                if (!query.pending) { gpu = &query; break; }
            }
            if (!gpu) impl.pending_sample->gpu_status = "unavailable: query ring full";
        }
        if (gpu) {
            impl.context->Begin(gpu->disjoint.Get());
            impl.context->End(gpu->begin.Get());
        }
    }
    auto end_gpu = [&](bool keep) {
        if (!gpu) return;
        impl.context->End(gpu->end.Get());
        impl.context->End(gpu->disjoint.Get());
        gpu->pending = true;
        gpu->frame = keep ? ++impl.gpu_frame : 0;
        if (keep) impl.pending_sample->gpu_frame = gpu->frame;
        gpu = nullptr;
    };
    if (auto error = impl.renderer->render(impl.context.Get(), impl.pending_packet, target)) {
        end_gpu(false);
        if (error->code == ErrorCode::busy) return wait_busy();
        impl.in_busy_wait = false;
        return handle_graphics_error(std::move(*error));
    }
    end_gpu(true);
    ++impl.submits;
    impl.in_busy_wait = false;
    if (impl.evidence_enabled) {
        impl.last_packet = impl.pending_packet;
        impl.last_semantics.assign(
            impl.pending_packet->semantics.begin(), impl.pending_packet->semantics.end());
    }
    impl.pending_packet.reset();
    if (auto error = impl.renderer->collect(impl.context.Get())) {
        return handle_graphics_error(std::move(*error));
    }
    if (impl.next_capture) {
        if (auto error = capture(*impl.next_capture)) {
            return handle_graphics_error(std::move(*error));
        }
        impl.next_capture.reset();
    }
    const HRESULT hr = impl.swapchain->Present(1, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        impl.occluded = true;
        impl.reset_clock = true;
        impl.next_occlusion_test = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        ++impl.epoch;
        interrupt_sample();
        return wait_for_messages(16);
    }
    if (FAILED(hr)) return handle_graphics_error(impl.graphics_error(hr, "Present"));
    ++impl.presents;
    did_present = sample == nullptr || impl.pending_sample.has_value();
    if (sample && impl.pending_sample) *sample = *impl.pending_sample;
    impl.pending_sample.reset();
    return std::nullopt;
}

Status NativeApp::step(double dt_seconds) {
    if (!(dt_seconds >= 0) || !(dt_seconds < 10)) {
        return make_error(ErrorCode::invalid_argument, "step dt must be finite nonnegative");
    }
    bool did_present = false;
    if (auto error = drain_or_submit(dt_seconds, did_present)) return error;
    return std::nullopt;
}

Status NativeApp::sample_step(FrameSample& sample, bool& completed, bool& interrupted) {
    Impl& impl = *impl_;
    set_evidence_enabled(false);
    completed = false;
    interrupted = impl.epoch != impl.sampled_epoch;
    const auto result = drain_or_submit(1.0 / 60.0, completed, &sample, &interrupted);
    interrupted = interrupted || impl.epoch != impl.sampled_epoch ||
        !impl.gallery.notifications || impl.gallery.restore_count != 0 || impl.disabled_mode;
    impl.sampled_epoch = impl.epoch;
    if (interrupted) {
        completed = false;
        impl.pending_sample.reset();
        // A finished frame's input/model effects are never replayed.
        impl.pending_packet.reset();
    }
    return result;
}

int NativeApp::run() {
    Impl& impl = *impl_;
    auto previous = std::chrono::steady_clock::now();
    while (!impl.close_requested) {
        const auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;
        if (impl.reset_clock || !(dt >= 0) || dt > 0.25) dt = 1.0 / 60.0;
        impl.reset_clock = false;
        bool did_present = false;
        if (auto error = drain_or_submit(dt, did_present)) {
            std::fprintf(stderr, "gallery error: %s\n", error->message.c_str());
            return 1;
        }
        if (impl.reset_clock) previous = std::chrono::steady_clock::now();
    }
    return 0;
}

}  // namespace wui::example
