#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_app.hpp"

#include <wui/wui.hpp>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <io.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wui::example {
namespace {

using Microsoft::WRL::ComPtr;

Error make_validation_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

Error native_error(const char* operation, unsigned long code) {
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s failed (code=0x%08lx)", operation, code);
    return make_validation_error(ErrorCode::backend_failure, message);
}

Status write_report(const std::filesystem::path& path, std::string_view text) {
    std::FILE* file = nullptr;
    const auto opened = _wfopen_s(&file, path.c_str(), L"wb");
    if (opened != 0 || !file) {
        return make_validation_error(ErrorCode::backend_failure,
            "cannot open " + path.string() + ": errno=" + std::to_string(opened));
    }
    int failure = 0;
    if (std::fwrite(text.data(), 1, text.size(), file) != text.size()) failure = errno ? errno : EIO;
    if (std::ferror(file)) failure = failure ? failure : EIO;
    if (std::fflush(file) != 0) failure = errno ? errno : EIO;
    if (_commit(_fileno(file)) != 0) failure = errno ? errno : EIO;
    if (std::fclose(file) != 0) failure = errno ? errno : EIO;
    if (failure != 0) {
        return make_validation_error(ErrorCode::backend_failure,
            "cannot write/flush/close " + path.string() + ": errno=" + std::to_string(failure));
    }
    return std::nullopt;
}

Status make_directory(const std::filesystem::path& path) {
    std::error_code code;
    std::filesystem::create_directories(path, code);
    if (code) {
        return make_validation_error(ErrorCode::backend_failure,
            "cannot create " + path.string() + ": " + code.message());
    }
    return std::nullopt;
}

Status run_presents(NativeApp& app, int count, double dt = 1.0 / 60.0) {
    const unsigned start = app.present_count();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    unsigned previous = start;
    while (app.present_count() - start < static_cast<unsigned>(count)) {
        if (std::chrono::steady_clock::now() > deadline) {
            return make_validation_error(ErrorCode::backend_failure,
                "scenario exceeded 5s without a completed Present");
        }
        if (auto error = app.step(dt)) return error;
        if (app.present_count() != previous) {
            previous = app.present_count();
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
    }
    return std::nullopt;
}

Status settle(NativeApp& app, int frames = 12) {
    return run_presents(app, frames);
}

const Semantic* find_one(const std::vector<Semantic>& semantics, Role role,
    const std::string& label, Status& error_out) {
    const Semantic* found = nullptr;
    unsigned matches = 0;
    for (const auto& item : semantics) {
        if (item.role == role && item.label == label) {
            ++matches;
            found = &item;
        }
    }
    if (matches != 1) {
        error_out = make_validation_error(ErrorCode::invalid_frame,
            "expected exactly one role=" + std::to_string(static_cast<int>(role)) +
            " label='" + label + "', found " + std::to_string(matches));
        return nullptr;
    }
    return found;
}

POINT physical_center(const Semantic& semantic, float scale) {
    return {
        static_cast<LONG>(std::lround((semantic.bounds.x + semantic.bounds.w * 0.5f) * scale)),
        static_cast<LONG>(std::lround((semantic.bounds.y + semantic.bounds.h * 0.5f) * scale))};
}

Status control_center(NativeApp& app, Role role, float scale, POINT& center) {
    Status error;
    const auto* item = find_one(app.last_semantics(), role,
        role == Role::checkbox ? "Notifications" : "Restore defaults", error);
    if (!item) return error;
    center = physical_center(*item, scale);
    return std::nullopt;
}

void send_move(HWND window, LONG x, LONG y) {
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    // The real cursor is elsewhere, so the system would post WM_MOUSELEAVE at an
    // arbitrary frame. Cancel that tracking so the synthetic pointer stays put.
    TRACKMOUSEEVENT cancel{sizeof(TRACKMOUSEEVENT), TME_CANCEL | TME_LEAVE, window, 0};
    TrackMouseEvent(&cancel);
    MSG pending{};
    while (PeekMessageW(&pending, window, WM_MOUSELEAVE, WM_MOUSELEAVE, PM_REMOVE)) {}
}

void send_down(HWND window, LONG x, LONG y) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
}

void send_up(HWND window, LONG x, LONG y) {
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

LPARAM key_lparam(int repeat, int scan, bool extended, bool prev_down, bool up) {
    LPARAM value = (repeat & 0xffff);
    value |= static_cast<LPARAM>(scan & 0xff) << 16;
    if (extended) value |= LPARAM{1} << 24;
    if (prev_down) value |= LPARAM{1} << 30;
    if (up) value |= LPARAM{1} << 31;
    return value;
}

Status send_key(HWND window, UINT vk, bool down, int repeat = 1, bool prev_down = false) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scan == 0) return native_error("MapVirtualKeyW", GetLastError());
    SendMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, vk,
        key_lparam(repeat, static_cast<int>(scan & 0xff), false, prev_down, !down));
    return std::nullopt;
}

Status key_frame(NativeApp& app, UINT vk, bool down, bool repeat = false) {
    if (auto error = send_key(app.window(), vk, down, repeat ? 2 : 1, repeat)) return error;
    return run_presents(app, 1);
}

Status focus_window(HWND window) {
    SetLastError(ERROR_SUCCESS);
    SetFocus(window);
    if (GetFocus() != window) return native_error("SetFocus", GetLastError());
    return std::nullopt;
}

Status check_model(NativeApp& app, bool notifications, unsigned restores, const char* operation) {
    if (app.gallery_state().notifications != notifications ||
        app.gallery_state().restore_count != restores) {
        return make_validation_error(ErrorCode::invalid_frame, std::string(operation) +
            ": expected notifications=" + std::to_string(notifications) +
            " restore_count=" + std::to_string(restores) + ", observed " +
            std::to_string(app.gallery_state().notifications) + "/" +
            std::to_string(app.gallery_state().restore_count));
    }
    return std::nullopt;
}

Status check_controls(NativeApp& app, bool enabled, Role focused = Role::panel) {
    Status error;
    const auto* checkbox = find_one(app.last_semantics(), Role::checkbox, "Notifications", error);
    if (!checkbox) return error;
    const auto* button = find_one(app.last_semantics(), Role::button, "Restore defaults", error);
    if (!button) return error;
    if (checkbox->enabled != enabled || button->enabled != enabled ||
        checkbox->focused != (focused == Role::checkbox) ||
        button->focused != (focused == Role::button) ||
        checkbox->selected != app.gallery_state().notifications) {
        return make_validation_error(ErrorCode::invalid_frame,
            "control enabled/focused/selected semantics disagree with scenario");
    }
    return std::nullopt;
}

Status check_baseline(NativeApp& app) {
    // Startup shows Controls; only the original pair is pinned, never totals or order.
    if (app.gallery_state().section != 1) {
        return make_validation_error(ErrorCode::invalid_frame, "gallery did not start on Controls");
    }
    if (auto failure = check_controls(app, true)) return failure;
    return check_model(app, true, 0, "baseline");
}

Status expect(bool condition, const std::string& message) {
    if (condition) return std::nullopt;
    return make_validation_error(ErrorCode::invalid_frame, message);
}

float packet_scale(const NativeApp& app) {
    const auto packet = app.last_packet();
    return packet ? packet->scale : 1.0f;
}

Rect logical_viewport(const NativeApp& app) {
    const auto packet = app.last_packet();
    return packet ? packet->viewport : Rect{};
}

std::size_t count_named(const NativeApp& app, Role role, std::string_view label) {
    return static_cast<std::size_t>(std::count_if(app.last_semantics().begin(),
        app.last_semantics().end(),
        [&](const Semantic& item) { return item.role == role && item.label == label; }));
}

const Semantic* named(const NativeApp& app, Role role, const std::string& label, Status& error) {
    return find_one(app.last_semantics(), role, label, error);
}

const Semantic* focused_semantic(const NativeApp& app) {
    for (const auto& item : app.last_semantics()) {
        if (item.focused) return &item;
    }
    return nullptr;
}

bool inside(Rect item, Rect bounds) {
    constexpr float slack = 0.5f;
    return item.x >= bounds.x - slack && item.y >= bounds.y - slack &&
        item.x + item.w <= bounds.x + bounds.w + slack &&
        item.y + item.h <= bounds.y + bounds.h + slack;
}

Status reveal(NativeApp& app, Role role, const std::string& label);

Status point_of(NativeApp& app, Role role, const std::string& label, POINT& point) {
    if (auto failed = reveal(app, role, label)) return failed;
    Status error;
    const auto* item = named(app, role, label, error);
    if (!item) return error;
    point = physical_center(*item, packet_scale(app));
    return std::nullopt;
}

// Every scenario interaction targets the current semantic bounds, not fixed pixels.
Status click_named(NativeApp& app, Role role, const std::string& label) {
    POINT point{};
    if (auto error = point_of(app, role, label, point)) return error;
    send_move(app.window(), point.x, point.y);
    send_down(app.window(), point.x, point.y);
    if (auto error = run_presents(app, 1)) return error;
    send_up(app.window(), point.x, point.y);
    if (auto error = run_presents(app, 2)) return error;
    if (GetCapture() == app.window()) {
        return make_validation_error(ErrorCode::invalid_frame, "click did not release native capture");
    }
    return std::nullopt;
}

Status press_key(NativeApp& app, UINT vk) {
    if (auto error = key_frame(app, vk, true)) return error;
    return key_frame(app, vk, false);
}

// Modifier and key arrive in one frame through real WM_KEYDOWN/WM_KEYUP messages.
Status chord(NativeApp& app, UINT modifier, UINT vk) {
    if (auto error = send_key(app.window(), modifier, true)) return error;
    if (auto error = send_key(app.window(), vk, true)) return error;
    if (auto error = send_key(app.window(), vk, false)) return error;
    if (auto error = send_key(app.window(), modifier, false)) return error;
    return run_presents(app, 1);
}

Status tab_to(NativeApp& app, Role role, const std::string& label, bool backwards = false) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (auto error = backwards ? chord(app, VK_SHIFT, VK_TAB) : press_key(app, VK_TAB)) {
            return error;
        }
        const auto* focused = focused_semantic(app);
        if (focused && focused->role == role && focused->label == label) return std::nullopt;
    }
    return make_validation_error(ErrorCode::invalid_frame,
        "keyboard traversal never focused '" + label + "'");
}

Status type_text(NativeApp& app, std::wstring_view text) {
    for (const wchar_t unit : text) SendMessageW(app.window(), WM_CHAR, unit, 1);
    return run_presents(app, 1);
}

Status send_wheel(NativeApp& app, POINT client, int notches) {
    POINT screen = client;
    if (!ClientToScreen(app.window(), &screen)) return native_error("ClientToScreen(wheel)", GetLastError());
    SendMessageW(app.window(), WM_MOUSEWHEEL,
        MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(notches * WHEEL_DELTA))),
        MAKELPARAM(screen.x, screen.y));
    return run_presents(app, 2);
}

// Scrolls the page with real wheel messages until the control is fully visible.
Status reveal(NativeApp& app, Role role, const std::string& label) {
    for (int notch = 0; notch < 80; ++notch) {
        Status error;
        const auto* item = named(app, role, label, error);
        if (!item) return error;
        const Rect view = logical_viewport(app);
        if (inside(item->bounds, view)) return std::nullopt;
        const float scale = packet_scale(app);
        const POINT middle{static_cast<LONG>(std::lround(view.w * 0.5f * scale)),
                           static_cast<LONG>(std::lround(view.h * 0.5f * scale))};
        if (auto failed = send_wheel(app, middle, item->bounds.y < view.y ? 1 : -1)) return failed;
    }
    return make_validation_error(ErrorCode::invalid_frame, "could not scroll '" + label + "' into view");
}

std::size_t popup_options(const NativeApp& app, std::span<const char* const> labels) {
    std::size_t found = 0;
    for (const char* label : labels) found += count_named(app, Role::dropdown, label);
    return found;
}

Status check_extent(NativeApp& app) {
    RECT client{};
    if (!GetClientRect(app.window(), &client)) return native_error("GetClientRect", GetLastError());
    const auto [width, height] = app.target_size();
    const auto packet = app.last_packet();
    if (!packet || width != static_cast<unsigned>(client.right - client.left) ||
        height != static_cast<unsigned>(client.bottom - client.top) ||
        std::llround(packet->viewport.w * packet->scale) != static_cast<std::int64_t>(width) ||
        std::llround(packet->viewport.h * packet->scale) != static_cast<std::int64_t>(height)) {
        return make_validation_error(ErrorCode::invalid_frame,
            "packet physical extent, target and current client extent differ");
    }
    return std::nullopt;
}

Status click_control(NativeApp& app, Role role, float scale) {
    POINT center{};
    if (auto error = control_center(app, role, scale, center)) return error;
    send_move(app.window(), center.x, center.y);
    send_down(app.window(), center.x, center.y);
    if (auto error = run_presents(app, 1)) return error;
    send_up(app.window(), center.x, center.y);
    if (auto error = run_presents(app, 2)) return error;
    if (GetCapture() == app.window()) {
        return make_validation_error(ErrorCode::invalid_frame, "click did not release native capture");
    }
    return std::nullopt;
}

Status pump_and_wait(DWORD timeout = 1) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            return make_validation_error(ErrorCode::backend_failure, "validation interrupted by WM_QUIT");
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
        WAIT_FAILED) {
        return native_error("MsgWaitForMultipleObjectsEx", GetLastError());
    }
    return std::nullopt;
}

Status wait_for_gpu(NativeApp& app) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> done;
    HRESULT hr = app.device()->CreateQuery(&desc, &done);
    if (FAILED(hr)) return native_error("CreateQuery(readback)", hr);
    app.device_context()->End(done.Get());
    app.device_context()->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        hr = app.device_context()->GetData(done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) return app.renderer()->collect(app.device_context());
        if (hr != S_FALSE) return native_error("GetData(readback)", hr);
        if (std::chrono::steady_clock::now() >= deadline) {
            return make_validation_error(ErrorCode::backend_failure, "readback GPU wait exceeded 5s");
        }
        if (auto error = pump_and_wait()) return error;
    }
}

Status render_and_read_pixels(NativeApp& app, const DrawPacket& packet,
    std::vector<std::uint8_t>& rgba, std::uint32_t& w, std::uint32_t& h) {
    if (!packet) return make_validation_error(ErrorCode::invalid_argument, "null packet for readback");
    const auto pw = std::llround(packet->viewport.w * packet->scale);
    const auto ph = std::llround(packet->viewport.h * packet->scale);
    if (pw <= 0 || ph <= 0 || pw > 4096 || ph > 4096) {
        return make_validation_error(ErrorCode::invalid_argument, "packet extent out of range");
    }
    w = static_cast<std::uint32_t>(pw);
    h = static_cast<std::uint32_t>(ph);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = app.device()->CreateTexture2D(&desc, nullptr, &target);
    if (FAILED(hr)) return native_error("CreateTexture2D(isolated target)", hr);
    hr = app.device()->CreateRenderTargetView(target.Get(), nullptr, &view);
    if (FAILED(hr)) return native_error("CreateRenderTargetView(isolated target)", hr);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = app.device()->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) return native_error("CreateTexture2D(isolated staging)", hr);
    const float clear[4]{0x10 / 255.0f, 0x12 / 255.0f, 0x18 / 255.0f, 1.0f};
    app.device_context()->ClearRenderTargetView(view.Get(), clear);
    const wui::d3d11::Target native{view.Get(), w, h};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool flushed = false;
    for (;;) {
        auto error = app.renderer()->render(app.device_context(), packet, native);
        if (!error) break;
        if (error->code != ErrorCode::busy) return error;
        if (std::chrono::steady_clock::now() >= deadline) {
            return make_validation_error(ErrorCode::backend_failure, "isolated render busy for 5s");
        }
        if (!flushed) {
            app.device_context()->Flush();
            flushed = true;
        }
        if (auto failure = app.renderer()->collect(app.device_context())) return failure;
        if (auto failure = pump_and_wait()) return failure;
    }
    app.device_context()->CopyResource(staging.Get(), target.Get());
    if (auto error = wait_for_gpu(app)) return error;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = app.device_context()->Map(staging.Get(), 0, D3D11_MAP_READ,
        D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (FAILED(hr)) return native_error("Map(isolated readback)", hr);
    struct UnmapGuard {
        ID3D11DeviceContext* context;
        ID3D11Texture2D* texture;
        ~UnmapGuard() { context->Unmap(texture, 0); }
    } guard{app.device_context(), staging.Get()};
    if (!mapped.pData || mapped.RowPitch < static_cast<std::size_t>(w) * 4) {
        return make_validation_error(ErrorCode::backend_failure, "invalid isolated readback row pitch");
    }
    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(y) * w * 4,
            static_cast<const std::uint8_t*>(mapped.pData) +
                static_cast<std::size_t>(y) * mapped.RowPitch,
            static_cast<std::size_t>(w) * 4);
    }
    return std::nullopt;
}

Status check_info_queue(NativeApp& app, std::string& log) {
    if (!app.debug_enabled()) {
        log += "debug-layer: unavailable (not a clean-debug-output proof)\n";
        return std::nullopt;
    }
    if (!app.device()) {
        return make_validation_error(ErrorCode::backend_failure,
            "debug InfoQueue unavailable: no current device after failed recreation");
    }
    ComPtr<ID3D11InfoQueue> info;
    HRESULT hr = app.device()->QueryInterface(__uuidof(ID3D11InfoQueue),
        reinterpret_cast<void**>(info.GetAddressOf()));
    if (FAILED(hr) || !info) {
        return native_error("QueryInterface(ID3D11InfoQueue) on debug-enabled device",
            FAILED(hr) ? hr : E_NOINTERFACE);
    }
    UINT64 errors = 0;
    UINT64 warnings = 0;
    const UINT64 stored = info->GetNumStoredMessages();
    for (UINT64 i = 0; i < stored; ++i) {
        SIZE_T bytes = 0;
        hr = info->GetMessage(i, nullptr, &bytes);
        if (FAILED(hr)) return native_error("InfoQueue::GetMessage(size)", hr);
        if (bytes < sizeof(D3D11_MESSAGE)) {
            return make_validation_error(ErrorCode::backend_failure, "InfoQueue message is truncated");
        }
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        hr = info->GetMessage(i, message, &bytes);
        if (FAILED(hr)) return native_error("InfoQueue::GetMessage", hr);
        const bool is_error = message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
            message->Severity == D3D11_MESSAGE_SEVERITY_ERROR;
        const bool is_warning = message->Severity == D3D11_MESSAGE_SEVERITY_WARNING;
        errors += is_error;
        warnings += is_warning;
        if (is_error || is_warning) {
            log += std::string(is_error ? "debug-error" : "debug-warning") +
                " id=" + std::to_string(static_cast<unsigned>(message->ID)) +
                " category=" + std::to_string(static_cast<unsigned>(message->Category)) + ": ";
            if (message->pDescription && message->DescriptionByteLength != 0) {
                std::size_t size = message->DescriptionByteLength;
                if (message->pDescription[size - 1] == '\0') --size;
                log.append(message->pDescription, size);
            }
            log += "\n";
        }
    }
    log += "debug-layer: available errors=" + std::to_string(errors) +
        " warnings=" + std::to_string(warnings) + "\n";
    info->ClearStoredMessages();
    if (errors != 0 || warnings != 0) {
        return make_validation_error(ErrorCode::backend_failure,
            "debug layer reported errors or warnings; descriptions retained for review");
    }
    return std::nullopt;
}

struct OwnedWindow {
    HWND handle = nullptr;
    ~OwnedWindow() { if (handle) DestroyWindow(handle); }
    Status close() {
        if (handle && !DestroyWindow(handle)) return native_error("DestroyWindow(peer)", GetLastError());
        handle = nullptr;
        return std::nullopt;
    }
};

struct WindowPosition {
    HWND window;
    RECT original{};
    bool changed = false;
    ~WindowPosition() {
        if (changed) SetWindowPos(window, nullptr, original.left, original.top,
            original.right - original.left, original.bottom - original.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
    Status restore() {
        if (changed && !SetWindowPos(window, nullptr, original.left, original.top,
            original.right - original.left, original.bottom - original.top,
            SWP_NOACTIVATE | SWP_NOZORDER)) {
            return native_error("SetWindowPos(restore)", GetLastError());
        }
        changed = false;
        return std::nullopt;
    }
};

Status window_dpi(HWND window, UINT& dpi) {
    const auto user = GetModuleHandleW(L"user32.dll");
    if (!user) return native_error("GetModuleHandleW(user32)", GetLastError());
    using GetDpi = UINT(WINAPI*)(HWND);
    if (auto fn = reinterpret_cast<GetDpi>(GetProcAddress(user, "GetDpiForWindow"))) {
        dpi = fn(window);
        if (dpi == 0) return native_error("GetDpiForWindow", GetLastError());
        return std::nullopt;
    }
    HDC dc = GetDC(window);
    if (!dc) return native_error("GetDC(DPI)", GetLastError());
    const int value = GetDeviceCaps(dc, LOGPIXELSX);
    const int released = ReleaseDC(window, dc);
    if (released == 0) return native_error("ReleaseDC(DPI)", GetLastError());
    if (value <= 0) return make_validation_error(ErrorCode::backend_failure, "GetDeviceCaps(DPI) failed");
    dpi = static_cast<UINT>(value);
    return std::nullopt;
}

struct Monitor {
    HMONITOR handle = nullptr;
    RECT work{};
    UINT dpi_x = 0;
    UINT dpi_y = 0;
};

struct MonitorEnumeration {
    std::vector<Monitor> monitors;
    DWORD error = ERROR_SUCCESS;
    bool allocation_failed = false;
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) noexcept {
    auto& result = *reinterpret_cast<MonitorEnumeration*>(data);
    MONITORINFO info{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(monitor, &info)) {
        result.error = GetLastError();
        return FALSE;
    }
    try {
        result.monitors.push_back({monitor, info.rcWork, 0, 0});
    } catch (...) {
        result.allocation_failed = true;
        return FALSE;
    }
    return TRUE;
}

Status exercise_monitor_dpi(NativeApp& app, std::string& log) {
    MonitorEnumeration enumeration;
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&enumeration))) {
        if (enumeration.allocation_failed) {
            return make_validation_error(ErrorCode::capacity, "monitor enumeration allocation failed");
        }
        return native_error("EnumDisplayMonitors/GetMonitorInfoW",
            enumeration.error ? enumeration.error : GetLastError());
    }
    struct Module {
        HMODULE handle;
        ~Module() { if (handle) FreeLibrary(handle); }
    } shcore{LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    using GetMonitorDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    const auto get_dpi = shcore.handle
        ? reinterpret_cast<GetMonitorDpi>(GetProcAddress(shcore.handle, "GetDpiForMonitor")) : nullptr;
    log += "monitors: " + std::to_string(enumeration.monitors.size()) + "\n";
    if (!get_dpi) {
        log += "cross-monitor-dpi: not exercised (GetDpiForMonitor unavailable; Win32 code=" +
            std::to_string(GetLastError()) + ")\n";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < enumeration.monitors.size(); ++i) {
        auto& monitor = enumeration.monitors[i];
        const HRESULT hr = get_dpi(monitor.handle, 0, &monitor.dpi_x, &monitor.dpi_y);
        if (FAILED(hr)) return native_error("GetDpiForMonitor(effective DPI)", hr);
        if (monitor.dpi_x == 0 || monitor.dpi_y == 0) {
            return make_validation_error(ErrorCode::backend_failure, "GetDpiForMonitor returned zero DPI");
        }
        log += "monitor=" + std::to_string(i) + " effective-dpi=" +
            std::to_string(monitor.dpi_x) + "x" + std::to_string(monitor.dpi_y) +
            " work=" + std::to_string(monitor.work.left) + "," +
            std::to_string(monitor.work.top) + ".." + std::to_string(monitor.work.right) +
            "," + std::to_string(monitor.work.bottom) + "\n";
    }
    const Monitor* first = nullptr;
    const Monitor* second = nullptr;
    for (const auto& monitor : enumeration.monitors) {
        if (!first) first = &monitor;
        else if (monitor.dpi_x != first->dpi_x || monitor.dpi_y != first->dpi_y) {
            second = &monitor;
            break;
        }
    }
    if (!second) {
        log += "cross-monitor-dpi: not exercised (no distinct effective-DPI monitors in current awareness)\n";
        return std::nullopt;
    }
    WindowPosition position{app.window()};
    if (!GetWindowRect(app.window(), &position.original)) {
        return native_error("GetWindowRect(before monitor moves)", GetLastError());
    }
    unsigned observed = 0;
    for (const Monitor* monitor : {first, second}) {
        UINT before = 0;
        if (auto error = window_dpi(app.window(), before)) return error;
        const unsigned messages_before = app.dpi_change_count();
        const LONG width = std::min<LONG>(640, monitor->work.right - monitor->work.left);
        const LONG height = std::min<LONG>(400, monitor->work.bottom - monitor->work.top);
        position.changed = true;
        if (!SetWindowPos(app.window(), nullptr, monitor->work.left, monitor->work.top,
            width, height, SWP_NOACTIVATE | SWP_NOZORDER)) {
            return native_error("SetWindowPos(real monitor move)", GetLastError());
        }
        if (auto error = settle(app)) return error;
        if (MonitorFromWindow(app.window(), MONITOR_DEFAULTTONULL) != monitor->handle) {
            return make_validation_error(ErrorCode::backend_failure, "window did not reach requested monitor");
        }
        UINT after = 0;
        if (auto error = window_dpi(app.window(), after)) return error;
        const unsigned messages = app.dpi_change_count() - messages_before;
        observed += messages;
        log += "real-monitor-move: window-dpi=" + std::to_string(before) + "->" +
            std::to_string(after) + " WM_DPICHANGED=" + std::to_string(messages) +
            " adapter-scale=" + std::to_string(app.adapter_scale()) + "\n";
        if (before != after && messages == 0) {
            return make_validation_error(ErrorCode::invalid_frame,
                "effective window DPI changed without an observed WM_DPICHANGED");
        }
        if (std::abs(app.adapter_scale() - static_cast<float>(after) / 96.0f) > 0.001f) {
            return make_validation_error(ErrorCode::invalid_frame, "actual DPI and adapter scale disagree");
        }
        if (auto error = check_extent(app)) return error;
        const bool notifications = app.gallery_state().notifications;
        const unsigned restores = app.gallery_state().restore_count;
        if (auto error = click_control(app, Role::checkbox, app.adapter_scale())) return error;
        if (auto error = check_model(app, !notifications, restores, "real-DPI pointer alignment")) return error;
    }
    log += observed != 0
        ? "cross-monitor-dpi: exercised via actual own-window moves and observed WM_DPICHANGED\n"
        : "cross-monitor-dpi: not exercised (moves completed but current awareness produced no DPI messages)\n";
    if (auto error = position.restore()) return error;
    return settle(app);
}

}  // namespace

Status NativeApp::validate(const std::filesystem::path& directory) {
    set_evidence_enabled(true);
    struct RestoreMode {
        NativeApp& app;
        ~RestoreMode() {
            app.set_validation_scale(std::nullopt);
            app.set_disabled_mode(false);
            app.set_evidence_enabled(false);
        }
    } restore_mode{*this};
    if (auto error = make_directory(directory)) return error;
    const auto report_path = directory / "validation.txt";
    std::string log = "c4 validation (complete gallery)\n"
        "input: programmatic, scoped to application-owned HWNDs; not manual typing evidence\n"
        "scale matrix: simulated render/input scales, not OS DPI changes\n"
        "captures: 12 successful settling frames before every native pre-Present capture\n"
        "manual mouse/keyboard and visual PNG/interactive review: not performed by this command\n";
    log += "driver: " + driver_description() + "\ndpi: " + dpi_description() + "\n";
    std::string scenario = "initialization";
    auto checkpoint = [&](const std::string& name) -> Status {
        scenario = name;
        log += "BEGIN " + scenario + "\n";
        return write_report(report_path, log);
    };
    auto finish_scenario = [&]() -> Status {
        if (auto error = check_extent(*this)) return error;
        if (auto error = check_info_queue(*this, log)) return error;
        log += "PASS " + scenario + " notifications=" +
            std::to_string(gallery_state().notifications) + " restore_count=" +
            std::to_string(gallery_state().restore_count) + "\n";
        return write_report(report_path, log);
    };
    auto fresh = [&](const std::string& name, float scale, unsigned width = 640,
                     unsigned height = 400, bool disabled = false,
                     bool notifications = true) -> Status {
        if (auto error = checkpoint(name)) return error;
        set_validation_scale(scale);
        if (auto error = resize(static_cast<unsigned>(std::lround(width * scale)),
                static_cast<unsigned>(std::lround(height * scale)))) return error;
        send_up(window(), 4, 4);
        SendMessageW(window(), WM_KILLFOCUS, 0, 0);
        send_move(window(), 4, 4);
        if (auto error = reset_validation_scenario()) return error;
        set_disabled_mode(disabled);
        gallery_state().notifications = notifications;
        if (auto error = settle(*this)) return error;
        return check_controls(*this, !disabled);
    };
    unsigned captures = 0;
    // Synthetic hover ends when Windows posts WM_MOUSELEAVE for the real cursor,
    // so hover-dependent frames re-send the pointer position every frame.
    auto hover_frames = [&](POINT point, int frames) -> Status {
        for (int i = 0; i < frames; ++i) {
            send_move(window(), point.x, point.y);
            if (auto error = run_presents(*this, 1)) return error;
        }
        return std::nullopt;
    };
    auto capture_one = [&](const std::filesystem::path& path,
                           std::optional<POINT> hover = std::nullopt) -> Status {
        if (auto error = hover ? hover_frames(*hover, 12) : settle(*this)) return error;
        schedule_capture(path);
        if (hover) send_move(window(), hover->x, hover->y);
        if (auto error = run_presents(*this, 1)) return error;
        if (auto error = check_extent(*this)) return error;
        std::error_code code;
        const auto bytes = std::filesystem::file_size(path, code);
        if (code || bytes == 0) {
            return make_validation_error(ErrorCode::backend_failure,
                "native capture missing/empty: " + path.string() +
                (code ? ": " + code.message() : ""));
        }
        ++captures;
        const auto [width, height] = target_size();
        log += "capture=" + path.string() + " scale=" + std::to_string(last_packet()->scale) +
            " physical=" + std::to_string(width) + "x" + std::to_string(height) +
            " notifications=" + std::to_string(gallery_state().notifications) +
            " restore_count=" + std::to_string(gallery_state().restore_count) + "\n";
        for (const auto& semantic : last_semantics()) {
            log += " semantic role=" + std::to_string(static_cast<int>(semantic.role)) +
                " label='" + semantic.label + "' enabled=" + std::to_string(semantic.enabled) +
                " selected=" + std::to_string(semantic.selected) +
                " focused=" + std::to_string(semantic.focused) +
                " bounds=" + std::to_string(semantic.bounds.x) + "," +
                std::to_string(semantic.bounds.y) + "," + std::to_string(semantic.bounds.w) +
                "," + std::to_string(semantic.bounds.h) + "\n";
        }
        return write_report(report_path, log);
    };
    auto assert_capture = [&](bool held, const char* operation) -> Status {
        if ((GetCapture() == window()) != held) {
            return make_validation_error(ErrorCode::invalid_frame,
                std::string(operation) + ": native capture mismatch");
        }
        return std::nullopt;
    };
    if (auto error = write_report(report_path, log)) return error;
    Status result;
    try {
        result = [&]() -> Status {
            UINT actual_dpi = 0;
            if (auto error = window_dpi(window(), actual_dpi)) return error;
            RECT client{};
            if (!GetClientRect(window(), &client)) return native_error("GetClientRect(startup)", GetLastError());
            log += "actual-startup-window-dpi=" + std::to_string(actual_dpi) +
                " client=" + std::to_string(client.right - client.left) + "x" +
                std::to_string(client.bottom - client.top) + "\n";
            if (auto error = check_info_queue(*this, log)) return error;
            constexpr std::array<float, 4> scales{1.0f, 1.25f, 1.5f, 2.0f};
            constexpr std::array<const char*, 4> names{"scale-100", "scale-125", "scale-150", "scale-200"};
            for (std::size_t si = 0; si < scales.size(); ++si) {
                const float scale = scales[si];
                const auto scale_dir = directory / names[si];
                const std::string prefix = std::string(names[si]) + "/";
                if (auto error = make_directory(scale_dir)) return error;
                if (auto error = fresh(prefix + "normal", scale)) return error;
                if (auto error = check_baseline(*this)) return error;
                if (auto error = capture_one(scale_dir / "normal-checked.png")) return error;
                if (auto error = check_baseline(*this)) return error;
                if (auto error = finish_scenario()) return error;

                // One transaction sequence: hover, held, matching release, settled unchecked.
                if (auto error = fresh(prefix + "mouse-checkbox", scale)) return error;
                POINT checkbox{};
                if (auto error = control_center(*this, Role::checkbox, scale, checkbox)) return error;
                send_move(window(), checkbox.x, checkbox.y);
                if (auto error = capture_one(scale_dir / "checkbox-hover.png", checkbox)) return error;
                send_down(window(), checkbox.x, checkbox.y);
                if (auto error = run_presents(*this, 1)) return error;
                if (auto error = assert_capture(true, "checkbox down")) return error;
                if (auto error = check_model(*this, true, 0, "checkbox down")) return error;
                if (auto error = capture_one(scale_dir / "checkbox-pressed.png")) return error;
                if (auto error = assert_capture(true, "checkbox pressed capture")) return error;
                if (auto error = check_model(*this, true, 0, "checkbox held")) return error;
                send_up(window(), checkbox.x, checkbox.y);
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = assert_capture(false, "checkbox release")) return error;
                if (auto error = check_model(*this, false, 0, "checkbox release")) return error;
                send_move(window(), 4, 4);
                SendMessageW(window(), WM_KILLFOCUS, 0, 0);
                if (auto error = capture_one(scale_dir / "unchecked.png")) return error;
                if (auto error = check_controls(*this, true)) return error;
                if (auto error = check_model(*this, false, 0, "checkbox settled after release")) return error;
                if (auto error = finish_scenario()) return error;

                if (auto error = fresh(prefix + "mouse-button", scale)) return error;
                if (auto error = click_control(*this, Role::checkbox, scale)) return error;
                if (auto error = check_model(*this, false, 0, "button prerequisite checkbox toggle")) return error;
                SendMessageW(window(), WM_KILLFOCUS, 0, 0);
                POINT button{};
                if (auto error = control_center(*this, Role::button, scale, button)) return error;
                send_move(window(), button.x, button.y);
                if (auto error = capture_one(scale_dir / "button-hover.png", button)) return error;
                send_down(window(), button.x, button.y);
                if (auto error = run_presents(*this, 1)) return error;
                if (auto error = assert_capture(true, "button down")) return error;
                if (auto error = check_model(*this, false, 0, "button down")) return error;
                if (auto error = capture_one(scale_dir / "button-pressed.png")) return error;
                if (auto error = assert_capture(true, "button pressed capture")) return error;
                if (auto error = check_model(*this, false, 0, "button held")) return error;
                send_up(window(), button.x, button.y);
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = check_model(*this, true, 1, "button matching release")) return error;
                if (auto error = assert_capture(false, "button matching release")) return error;
                if (auto error = check_controls(*this, true, Role::button)) return error;
                // Actual down INSIDE, held across repeated frames, move/up OUTSIDE.
                send_down(window(), button.x, button.y);
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, true, 1, "button held again")) return error;
                if (auto error = assert_capture(true, "button held again")) return error;
                send_move(window(), 4, 4);
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, true, 1, "button dragged outside")) return error;
                send_up(window(), 4, 4);
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, true, 1, "button outside release")) return error;
                if (auto error = assert_capture(false, "button outside release")) return error;
                if (auto error = finish_scenario()) return error;

                if (auto error = fresh(prefix + "keyboard", scale)) return error;
                if (auto error = focus_window(window())) return error;
                SendMessageW(window(), WM_SETFOCUS, 0, 0);
                if (auto error = run_presents(*this, 1)) return error;
                // The section tabs precede the checkbox in the complete gallery's order.
                if (auto error = tab_to(*this, Role::checkbox, "Notifications")) return error;
                if (auto error = check_controls(*this, true, Role::checkbox)) return error;
                if (auto error = capture_one(scale_dir / "checkbox-focus.png")) return error;
                if (auto error = check_controls(*this, true, Role::checkbox)) return error;
                if (auto error = key_frame(*this, VK_SPACE, true)) return error;
                if (auto error = check_model(*this, true, 0, "Space down")) return error;
                for (int repeat = 0; repeat < 3; ++repeat) {
                    if (auto error = key_frame(*this, VK_SPACE, true, true)) return error;
                    if (auto error = check_model(*this, true, 0, "Space repeated down")) return error;
                }
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, true, 0, "Space held")) return error;
                if (auto error = key_frame(*this, VK_SPACE, false)) return error;
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, false, 0, "Space release")) return error;
                if (auto error = check_controls(*this, true, Role::checkbox)) return error;
                if (auto error = key_frame(*this, VK_TAB, true)) return error;
                if (auto error = key_frame(*this, VK_TAB, false)) return error;
                if (auto error = check_controls(*this, true, Role::button)) return error;
                if (auto error = capture_one(scale_dir / "button-focus.png")) return error;
                if (auto error = check_controls(*this, true, Role::button)) return error;
                if (auto error = key_frame(*this, VK_RETURN, true)) return error;
                if (auto error = check_model(*this, false, 0, "Enter down")) return error;
                for (int repeat = 0; repeat < 3; ++repeat) {
                    if (auto error = key_frame(*this, VK_RETURN, true, true)) return error;
                    if (auto error = check_model(*this, false, 0, "Enter repeated down")) return error;
                }
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, false, 0, "Enter held")) return error;
                if (auto error = key_frame(*this, VK_RETURN, false)) return error;
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, true, 1, "Enter release")) return error;
                if (auto error = check_controls(*this, true, Role::button)) return error;
                if (auto error = finish_scenario()) return error;

                // Independent checked/unchecked disabled scenarios exercise both controls.
                for (bool checked : {true, false}) {
                    const char* image = checked ? "disabled-checked.png" : "disabled-unchecked.png";
                    if (auto error = fresh(prefix + image, scale, 640, 400, true, checked)) return error;
                    for (Role role : {Role::checkbox, Role::button}) {
                        POINT point{};
                        if (auto error = control_center(*this, role, scale, point)) return error;
                        send_move(window(), point.x, point.y);
                        send_down(window(), point.x, point.y);
                        if (auto error = settle(*this)) return error;
                        if (auto error = check_model(*this, checked, 0, "disabled pointer held")) return error;
                        if (auto error = check_controls(*this, false)) return error;
                        send_up(window(), point.x, point.y);
                        if (auto error = run_presents(*this, 2)) return error;
                        if (auto error = assert_capture(false, "disabled pointer release")) return error;
                        if (auto error = check_model(*this, checked, 0, "disabled pointer release")) return error;
                        if (auto error = check_controls(*this, false)) return error;
                    }
                    if (auto error = focus_window(window())) return error;
                    SendMessageW(window(), WM_SETFOCUS, 0, 0);
                    for (int traversal = 0; traversal < 3; ++traversal) {
                        if (auto error = key_frame(*this, VK_TAB, true)) return error;
                        if (auto error = key_frame(*this, VK_TAB, false)) return error;
                        if (auto error = check_controls(*this, false)) return error;
                        for (UINT key : {UINT{VK_SPACE}, UINT{VK_RETURN}}) {
                            if (auto error = key_frame(*this, key, true)) return error;
                            if (auto error = key_frame(*this, key, true, true)) return error;
                            if (auto error = check_model(*this, checked, 0, "disabled key held")) return error;
                            if (auto error = check_controls(*this, false)) return error;
                            if (auto error = key_frame(*this, key, false)) return error;
                            if (auto error = check_model(*this, checked, 0, "disabled key release")) return error;
                            if (auto error = check_controls(*this, false)) return error;
                        }
                    }
                    send_move(window(), 4, 4);
                    if (auto error = capture_one(scale_dir / image)) return error;
                    if (auto error = check_model(*this, checked, 0, "disabled settled capture")) return error;
                    if (auto error = check_controls(*this, false)) return error;
                    if (auto error = finish_scenario()) return error;
                }
                log += "scale-result=" + std::string(names[si]) + " PASS prescribed-images=10\n";
            }

            const auto scenes = directory / "scenarios";
            if (auto error = make_directory(scenes)) return error;
            const HINSTANCE peer_module = GetModuleHandleW(nullptr);
            if (!peer_module) return native_error("GetModuleHandleW(peer)", GetLastError());
            auto make_peer = [&](OwnedWindow& peer) -> Status {
                peer.handle = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"wui C4 focus peer",
                    WS_POPUP | WS_VISIBLE, 0, 0, 8, 8, window(), nullptr, peer_module, nullptr);
                if (!peer.handle) return native_error("CreateWindowExW(focus peer)", GetLastError());
                return focus_window(window());
            };
            auto keyboard_focus = [&]() -> Status {
                if (auto error = focus_window(window())) return error;
                SendMessageW(window(), WM_SETFOCUS, 0, 0);
                return run_presents(*this, 1);
            };
            constexpr std::array<const char*, 3> section_labels{"Appearance", "Controls", "Diagnostics"};
            constexpr std::array<const char*, 3> menu_labels{"Low", "Balanced", "High"};

            // (1) Tab hide/show keeps model values and keyed IDs; hidden controls emit nothing.
            if (auto error = fresh("scenario-01-tabs-hide-show", 1.0f)) return error;
            {
                Status lookup;
                const auto* box = named(*this, Role::checkbox, "Notifications", lookup);
                if (!box) return lookup;
                const auto notifications_id = box->id.value;
                const auto* restore = named(*this, Role::button, "Restore defaults", lookup);
                if (!restore) return lookup;
                const auto restore_id = restore->id.value;
                std::array<std::uint64_t, 3> tab_ids{};
                for (std::size_t i = 0; i < section_labels.size(); ++i) {
                    const auto* tab = named(*this, Role::tab, section_labels[i], lookup);
                    if (!tab) return lookup;
                    tab_ids[i] = tab->id.value;
                }
                if (auto error = click_named(*this, Role::checkbox, "Notifications")) return error;
                if (auto error = check_model(*this, false, 0, "tabs model edit")) return error;
                if (auto error = click_named(*this, Role::tab, "Appearance")) return error;
                if (auto error = expect(gallery_state().section == 0, "Appearance tab not selected")) return error;
                if (auto error = expect(count_named(*this, Role::checkbox, "Notifications") == 0 &&
                        count_named(*this, Role::button, "Restore defaults") == 0,
                        "hidden Controls still emitted semantics")) return error;
                if (auto error = capture_one(scenes / "section-appearance.png")) return error;
                if (auto error = click_named(*this, Role::tab, "Diagnostics")) return error;
                if (auto error = expect(gallery_state().section == 2, "Diagnostics tab not selected")) return error;
                std::size_t rows = 0;
                for (const auto& item : last_semantics()) {
                    rows += item.role == Role::button && item.label.rfind("Row ", 0) == 0;
                }
                if (auto error = expect(count_named(*this, Role::button, "Row 1") == 1 &&
                        count_named(*this, Role::button, "Row 10000") == 0 && rows > 0 && rows < 100,
                        "virtual list emitted invisible rows")) return error;
                if (auto error = capture_one(scenes / "section-diagnostics.png")) return error;
                if (auto error = click_named(*this, Role::tab, "Controls")) return error;
                if (auto error = check_model(*this, false, 0, "tabs model after switching")) return error;
                box = named(*this, Role::checkbox, "Notifications", lookup);
                if (!box) return lookup;
                restore = named(*this, Role::button, "Restore defaults", lookup);
                if (!restore) return lookup;
                bool stable = box->id.value == notifications_id && restore->id.value == restore_id &&
                    !box->selected;
                for (std::size_t i = 0; i < section_labels.size(); ++i) {
                    const auto* tab = named(*this, Role::tab, section_labels[i], lookup);
                    if (!tab) return lookup;
                    stable = stable && tab->id.value == tab_ids[i];
                }
                if (auto error = expect(stable, "IDs or model changed across tab hide/show")) return error;
                log += "tabs: section switches via semantic clicks; hidden semantics absent; "
                    "keyed IDs and unchecked model survived hide/show; virtual rows emitted=" +
                    std::to_string(rows) + " of 10000\n";
            }
            if (auto error = finish_scenario()) return error;

            // (2) Reordered values select by value; a missing value selects nothing and reports.
            if (auto error = fresh("scenario-02-choice-values", 1.0f)) return error;
            {
                Status lookup;
                std::array<std::uint64_t, 3> radio_ids{};
                for (std::size_t i = 0; i < menu_labels.size(); ++i) {
                    const auto* radio = named(*this, Role::radio, menu_labels[i], lookup);
                    if (!radio) return lookup;
                    radio_ids[i] = radio->id.value;
                }
                for (const auto& [label, value] : {std::pair{"High", 2}, std::pair{"Low", 0}}) {
                    if (auto error = click_named(*this, Role::radio, label)) return error;
                    const auto* menu = named(*this, Role::dropdown, "Quality menu", lookup);
                    if (!menu) return lookup;
                    if (auto error = expect(gallery_state().quality == value && menu->value == label &&
                            menu->selected, std::string("reordered choice did not select ") + label)) {
                        return error;
                    }
                }
                gallery_state().quality = 7;
                if (auto error = settle(*this)) return error;
                bool none_selected = true;
                for (std::size_t i = 0; i < menu_labels.size(); ++i) {
                    const auto* radio = named(*this, Role::radio, menu_labels[i], lookup);
                    if (!radio) return lookup;
                    none_selected = none_selected && !radio->selected && radio->id.value == radio_ids[i];
                }
                const auto* menu = named(*this, Role::dropdown, "Quality menu", lookup);
                if (!menu) return lookup;
                std::size_t missing = 0;
                for (const auto& diagnostic : ui_context()->diagnostics()) {
                    missing += diagnostic.message == "Selected value is missing from choices";
                }
                if (auto error = expect(none_selected && !menu->selected && missing == 2,
                        "missing value selected an option or was not diagnosed by both groups")) {
                    return error;
                }
                gallery_state().quality = 1;
                if (auto error = settle(*this)) return error;
                if (auto error = expect(ui_context()->diagnostics().empty(),
                        "diagnostics persisted after the model value was restored")) return error;
                log += "choices: radio order High/Low/Balanced and menu order Low/Balanced/High "
                    "select by value; missing value -> no selection + 2 nonfatal diagnostics, IDs "
                    "unchanged; duplicate values are not expressible in the fixed gallery source and "
                    "remain covered by core choice tests\n";
            }
            if (auto error = finish_scenario()) return error;

            // (3) Dropdown popups stay inside the root viewport at every edge; outside clicks are consumed.
            if (auto error = fresh("scenario-03-dropdown-edges", 1.0f)) return error;
            {
                auto open_menu = [&](const char* image, bool expect_above) -> Status {
                    Status lookup;
                    const auto* trigger = named(*this, Role::dropdown, "Quality menu", lookup);
                    if (!trigger) return lookup;
                    const Rect anchor = trigger->bounds;
                    const Rect view = logical_viewport(*this);
                    if (auto error = expect(inside(anchor, view), "dropdown trigger not fully visible")) {
                        return error;
                    }
                    if (auto error = click_named(*this, Role::dropdown, "Quality menu")) return error;
                    if (auto error = expect(popup_options(*this, menu_labels) == 3,
                            "dropdown did not open three options: " + [&] {
                                std::string seen;
                                for (const auto& item : last_semantics()) {
                                    if (item.role == Role::dropdown || item.focused) {
                                        seen += " [" + std::to_string(static_cast<int>(item.role)) + " " +
                                            item.label + " y=" + std::to_string(item.bounds.y) + "]";
                                    }
                                }
                                return seen;
                            }())) return error;
                    for (const char* label : menu_labels) {
                        const auto* option = named(*this, Role::dropdown, label, lookup);
                        if (!option) return lookup;
                        const bool placed = expect_above
                            ? option->bounds.y + option->bounds.h <= anchor.y + 0.5f
                            : option->bounds.y >= anchor.y + anchor.h - 0.5f;
                        if (auto error = expect(inside(option->bounds, view) && placed,
                                std::string("popup option outside viewport or on wrong side: ") + label)) {
                            return error;
                        }
                    }
                    return capture_one(scenes / image);
                };
                // Near the bottom edge the list flips above its trigger.
                if (auto error = open_menu("popup-bottom-edge.png", true)) return error;
                const auto quality = gallery_state().quality;
                if (auto error = click_named(*this, Role::checkbox, "Notifications")) return error;
                if (auto error = expect(popup_options(*this, menu_labels) == 0,
                        "outside click did not close popup")) return error;
                if (auto error = check_model(*this, true, 0, "outside click consumed")) return error;
                if (auto error = expect(gallery_state().quality == quality,
                        "outside click changed selection")) return error;
                // Scroll the page until the trigger reaches the top edge; the list opens below.
                if (auto error = resize(640, 240)) return error;
                if (auto error = settle(*this)) return error;
                for (int notch = 0; notch < 40; ++notch) {
                    Status lookup;
                    const auto* trigger = named(*this, Role::dropdown, "Quality menu", lookup);
                    if (!trigger) return lookup;
                    if (trigger->bounds.y <= 40) break;
                    if (auto error = send_wheel(*this, {320, 120}, -1)) return error;
                }
                if (auto error = open_menu("popup-top-edge.png", false)) return error;
                if (auto error = click_named(*this, Role::dropdown, "High")) return error;
                if (auto error = expect(gallery_state().quality == 2 &&
                        popup_options(*this, menu_labels) == 0,
                        "choosing a popup option did not select and close")) return error;
                // Narrow left/right edges: the list is clamped horizontally inside the root.
                if (auto error = resize(200, 400)) return error;
                if (auto error = settle(*this)) return error;
                for (int notch = 0; notch < 60; ++notch) {
                    Status lookup;
                    const auto* trigger = named(*this, Role::dropdown, "Quality menu", lookup);
                    if (!trigger) return lookup;
                    if (trigger->bounds.y >= 0 && trigger->bounds.y + trigger->bounds.h <= 360) break;
                    if (auto error = send_wheel(*this, {100, 200},
                            trigger->bounds.y < 0 ? 1 : -1)) return error;
                }
                Status lookup;
                const auto* trigger = named(*this, Role::dropdown, "Quality menu", lookup);
                if (!trigger) return lookup;
                const float below = 400 - (trigger->bounds.y + trigger->bounds.h);
                if (auto error = open_menu("popup-narrow.png", below < 124)) return error;
                if (auto error = key_frame(*this, VK_ESCAPE, true)) return error;
                if (auto error = key_frame(*this, VK_ESCAPE, false)) return error;
                if (auto error = expect(popup_options(*this, menu_labels) == 0,
                        "Escape did not close popup")) return error;
                log += "dropdown: bottom edge flips above, scrolled top edge opens below, 200px root "
                    "clamps horizontally; outside click closes without activating; Escape closes\n";
            }
            if (auto error = resize(640, 400)) return error;
            if (auto error = settle(*this)) return error;
            if (auto error = finish_scenario()) return error;

            // (4) Slider capture: drag outside, release, focus loss, removal and disabled state.
            if (auto error = fresh("scenario-04-slider-capture", 1.0f)) return error;
            {
                POINT center{};
                if (auto error = point_of(*this, Role::slider, "Volume", center)) return error;
                const LONG right = static_cast<LONG>(target_size().first) + 200;
                send_move(window(), center.x, center.y);
                send_down(window(), center.x, center.y);
                if (auto error = run_presents(*this, 1)) return error;
                if (auto error = assert_capture(true, "slider press")) return error;
                const float pressed = gallery_state().volume;
                if (auto error = expect(pressed > 0.3f && pressed < 0.7f,
                        "slider press did not move value toward pointer: " + std::to_string(pressed) + " at " + std::to_string(center.x) + "," + std::to_string(center.y))) return error;
                if (auto error = capture_one(scenes / "slider-held.png")) return error;
                send_move(window(), right, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = expect(gallery_state().volume == 1.0f, "drag right did not clamp to max: " + std::to_string(gallery_state().volume) + " captured=" + std::to_string(GetCapture() == window()))) {
                    return error;
                }
                send_move(window(), -200, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = expect(gallery_state().volume == 0.0f, "drag left did not clamp to min")) {
                    return error;
                }
                send_up(window(), -200, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = assert_capture(false, "slider outside release")) return error;
                send_move(window(), center.x, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = expect(gallery_state().volume == 0.0f, "released slider still dragged")) {
                    return error;
                }
                // Real Win32 focus loss to a peer window cancels the drag.
                OwnedWindow peer;
                if (auto error = make_peer(peer)) return error;
                send_down(window(), center.x, center.y);
                if (auto error = run_presents(*this, 1)) return error;
                const float before_loss = gallery_state().volume;
                if (auto error = focus_window(peer.handle)) return error;
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = assert_capture(false, "slider focus loss")) return error;
                send_move(window(), right, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = expect(gallery_state().volume == before_loss,
                        "slider moved after focus loss")) return error;
                send_up(window(), right, center.y);
                if (auto error = focus_window(window())) return error;
                if (auto error = peer.close()) return error;
                if (auto error = run_presents(*this, 2)) return error;
                // Removing the held slider (host hides its section) ends the drag.
                send_move(window(), center.x, center.y);
                send_down(window(), center.x, center.y);
                if (auto error = run_presents(*this, 1)) return error;
                const float before_removal = gallery_state().volume;
                gallery_state().section = 0;
                if (auto error = run_presents(*this, 2)) return error;
                send_move(window(), right, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                gallery_state().section = 1;
                if (auto error = run_presents(*this, 2)) return error;
                if (auto error = expect(gallery_state().volume == before_removal,
                        "removed slider kept dragging")) return error;
                send_up(window(), right, center.y);
                if (auto error = settle(*this)) return error;
                if (auto error = assert_capture(false, "slider removal release")) return error;
                if (auto error = point_of(*this, Role::slider, "Volume", center)) return error;
                // Disabled sliders ignore presses and drags.
                set_disabled_mode(true);
                if (auto error = settle(*this)) return error;
                const float disabled_value = gallery_state().volume;
                send_move(window(), center.x, center.y);
                send_down(window(), center.x, center.y);
                if (auto error = run_presents(*this, 1)) return error;
                send_move(window(), right, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                send_up(window(), right, center.y);
                if (auto error = run_presents(*this, 2)) return error;
                Status lookup;
                const auto* slider = named(*this, Role::slider, "Volume", lookup);
                if (!slider) return lookup;
                if (auto error = expect(gallery_state().volume == disabled_value && !slider->enabled,
                        "disabled slider changed")) return error;
                set_disabled_mode(false);
                log += "slider: press=" + std::to_string(pressed) + " clamps 1/0 outside the client, "
                    "release ends drag, real focus loss and removal cancel, disabled ignores input\n";
            }
            if (auto error = finish_scenario()) return error;

            // (5) Keyboard: Tab/Shift+Tab, disabled skip, missing focus, Enter/Escape.
            if (auto error = fresh("scenario-05-keyboard", 1.0f)) return error;
            {
                if (auto error = keyboard_focus()) return error;
                if (auto error = tab_to(*this, Role::checkbox, "Notifications")) return error;
                if (auto error = press_key(*this, VK_TAB)) return error;
                if (auto error = check_controls(*this, true, Role::button)) return error;
                if (auto error = chord(*this, VK_SHIFT, VK_TAB)) return error;
                if (auto error = check_controls(*this, true, Role::checkbox)) return error;
                if (auto error = press_key(*this, VK_SPACE)) return error;
                if (auto error = check_model(*this, false, 0, "keyboard Space")) return error;
                if (auto error = press_key(*this, VK_TAB)) return error;
                if (auto error = press_key(*this, VK_RETURN)) return error;
                if (auto error = check_model(*this, true, 1, "keyboard Enter")) return error;
                if (auto error = tab_to(*this, Role::dropdown, "Quality menu")) return error;
                if (auto error = press_key(*this, VK_RETURN)) return error;
                if (auto error = expect(popup_options(*this, menu_labels) == 3,
                        "Enter did not open focused dropdown")) return error;
                if (auto error = capture_one(scenes / "popup-keyboard.png")) return error;
                if (auto error = press_key(*this, VK_ESCAPE)) return error;
                const auto* focused = focused_semantic(*this);
                if (auto error = expect(popup_options(*this, menu_labels) == 0 && focused &&
                        focused->role == Role::dropdown && focused->label == "Quality menu",
                        "Escape did not close popup and restore trigger focus")) return error;
                if (auto error = click_named(*this, Role::tab, "Appearance")) return error;
                bool saw_prominent = false, saw_quiet = false, saw_disabled = false;
                for (int step = 0; step < 40; ++step) {
                    if (auto error = press_key(*this, VK_TAB)) return error;
                    if (const auto* item = focused_semantic(*this)) {
                        saw_prominent |= item->label == "Prominent";
                        saw_quiet |= item->label == "Quiet";
                        saw_disabled |= item->label == "Disabled";
                    }
                }
                if (auto error = expect(saw_prominent && saw_quiet && !saw_disabled,
                        "Tab traversal did not skip the disabled button")) return error;
                if (auto error = tab_to(*this, Role::button, "Quiet")) return error;
                gallery_state().section = 1;
                if (auto error = settle(*this)) return error;
                if (auto error = expect(count_named(*this, Role::button, "Quiet") == 0 &&
                        focused_semantic(*this) == nullptr, "missing focused widget still reported focus")) {
                    return error;
                }
                if (auto error = press_key(*this, VK_TAB)) return error;
                if (auto error = expect(focused_semantic(*this) != nullptr,
                        "Tab after missing focus did not recover")) return error;
                log += "keyboard: Tab/Shift+Tab between Notifications and Restore, Space/Enter activate, "
                    "Enter opens and Escape closes the dropdown restoring focus, disabled skipped, "
                    "missing focus recovers on Tab\n";
            }
            if (auto error = finish_scenario()) return error;

            // (6) Turkish and combining text, selection, undo; numeric commit/cancel.
            if (auto error = fresh("scenario-06-text-editing", 1.0f)) return error;
            {
                Status lookup;
                if (auto error = keyboard_focus()) return error;
                if (auto error = click_named(*this, Role::text_input, "Name")) return error;
                auto name_is = [&](const std::string& expected, const char* step) -> Status {
                    Status missing;
                    const auto* input = named(*this, Role::text_input, "Name", missing);
                    if (!input) return missing;
                    return expect(gallery_state().name == expected && input->value == expected &&
                        input->focused, std::string("text editing step failed: ") + step);
                };
                if (auto error = chord(*this, VK_CONTROL, 'A')) return error;
                if (auto error = type_text(*this, L"Iğdır")) return error;
                if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r", "replace selection")) return error;
                if (auto error = type_text(*this, L"é")) return error;
                if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1re\xCC\x81", "combining mark")) return error;
                if (auto error = press_key(*this, VK_BACK)) return error;
                if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r", "grapheme backspace")) return error;
                if (auto error = chord(*this, VK_SHIFT, VK_LEFT)) return error;
                if (auto error = chord(*this, VK_SHIFT, VK_LEFT)) return error;
                if (auto error = capture_one(scenes / "text-selection.png")) return error;
                if (auto error = type_text(*this, L"z")) return error;
                if (auto error = name_is("I\xC4\x9F" "dz", "type over selection")) return error;
                if (auto error = chord(*this, VK_CONTROL, 'Z')) return error;
                if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r", "undo")) return error;
                if (auto error = press_key(*this, VK_END)) return error;
                if (auto error = type_text(*this, L"İ")) return error;
                if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r\xC4\xB0", "dotted capital I")) return error;
                if (auto error = capture_one(scenes / "text-caret.png")) return error;
                if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r\xC4\xB0", "caret capture")) return error;
                // Numeric input commits on Enter, cancels on Escape, restores on focus loss.
                if (auto error = click_named(*this, Role::numeric_input, "Amount")) return error;
                if (auto error = chord(*this, VK_CONTROL, 'A')) return error;
                if (auto error = type_text(*this, L"42")) return error;
                if (auto error = press_key(*this, VK_RETURN)) return error;
                if (auto error = expect(gallery_state().amount == 42.0, "Enter did not commit 42")) return error;
                if (auto error = chord(*this, VK_CONTROL, 'A')) return error;
                if (auto error = type_text(*this, L"abc")) return error;
                if (auto error = press_key(*this, VK_ESCAPE)) return error;
                const auto* amount = named(*this, Role::numeric_input, "Amount", lookup);
                if (!amount) return lookup;
                if (auto error = expect(gallery_state().amount == 42.0 && amount->value == "42",
                        "Escape did not restore committed amount")) return error;
                if (auto error = chord(*this, VK_CONTROL, 'A')) return error;
                if (auto error = type_text(*this, L"5000")) return error;
                if (auto error = press_key(*this, VK_TAB)) return error;
                amount = named(*this, Role::numeric_input, "Amount", lookup);
                if (!amount) return lookup;
                if (auto error = expect(gallery_state().amount == 42.0 && amount->value == "42" &&
                        !amount->focused, "out-of-range draft was not restored on focus loss")) {
                    return error;
                }
                log += "text: real WM_CHAR/WM_KEYDOWN input; Turkish i/I forms, combining acute, "
                    "grapheme Backspace, Shift+Left selection, overtype and Ctrl+Z verified; numeric "
                    "Enter/Escape/focus-loss verified\n"
                    "text-clipboard: not exercised natively (interactive clipboard is never touched; "
                    "wui-win32-clipboard needs an elevated private window station)\n"
                    "text-ime: no IME is driven natively; preedit/result/cancel through Adapter and "
                    "Context are covered by the wui-win32 IME text-input test\n";
            }
            if (auto error = finish_scenario()) return error;

            // (7) Resize while interacting across four simulated scales and three physical extents.
            if (auto error = checkpoint("scenario-07-resize-matrix")) return error;
            {
                constexpr std::array<std::pair<unsigned, unsigned>, 3> extents{
                    std::pair{800u, 600u}, std::pair{1280u, 720u}, std::pair{1920u, 1080u}};
                for (std::size_t si = 0; si < scales.size(); ++si) {
                    for (std::size_t ei = 0; ei < extents.size(); ++ei) {
                        const auto [w, h] = extents[ei];
                        const auto [other_w, other_h] = extents[(ei + 1) % extents.size()];
                        set_validation_scale(scales[si]);
                        if (auto error = resize(w, h)) return error;
                        send_up(window(), 4, 4);
                        SendMessageW(window(), WM_KILLFOCUS, 0, 0);
                        if (auto error = reset_validation_scenario()) return error;
                        if (auto error = settle(*this)) return error;
                        if (auto error = expect(target_size() == std::pair<std::uint32_t, std::uint32_t>{w, h},
                                "resize matrix extent " + std::to_string(w) + "x" +
                                std::to_string(h) + " was not applied")) return error;
                        if (auto error = check_extent(*this)) return error;
                        Status lookup;
                        const Rect view = logical_viewport(*this);
                        for (const auto& [role, label] : {std::pair{Role::checkbox, "Notifications"},
                                                          std::pair{Role::button, "Restore defaults"}}) {
                            const auto* item = named(*this, role, label, lookup);
                            if (!item) return lookup;
                            if (auto error = expect(inside(item->bounds, view),
                                    std::string(label) + " outside the logical viewport")) return error;
                        }
                        POINT slider{};
                        if (auto error = point_of(*this, Role::slider, "Volume", slider)) return error;
                        send_move(window(), slider.x, slider.y);
                        send_down(window(), slider.x, slider.y);
                        if (auto error = run_presents(*this, 1)) return error;
                        if (auto error = assert_capture(true, "matrix press")) return error;
                        if (auto error = resize(other_w, other_h)) return error;
                        if (auto error = settle(*this)) return error;
                        if (auto error = check_extent(*this)) return error;
                        send_up(window(), 4, 4);
                        if (auto error = settle(*this)) return error;
                        if (auto error = assert_capture(false, "matrix release after resize")) return error;
                        const float volume = gallery_state().volume;
                        if (auto error = expect(std::isfinite(volume) && volume >= 0 && volume <= 1,
                                "slider value invalid after resize")) return error;
                        if (auto error = resize(w, h)) return error;
                        char image[64]{};
                        std::snprintf(image, sizeof(image), "matrix-%s-%ux%u.png", names[si], w, h);
                        if (auto error = capture_one(scenes / image)) return error;
                        if (auto error = check_info_queue(*this, log)) return error;
                        log += "matrix " + std::string(names[si]) + " simulated-scale physical=" +
                            std::to_string(w) + "x" + std::to_string(h) + " logical=" +
                            std::to_string(view.w) + "x" + std::to_string(view.h) +
                            " resized-while-held-to=" + std::to_string(other_w) + "x" +
                            std::to_string(other_h) + " volume=" + std::to_string(volume) + "\n";
                    }
                }
                set_validation_scale(1.0f);
                if (auto error = resize(640, 400)) return error;
                if (auto error = settle(*this)) return error;
            }
            if (auto error = finish_scenario()) return error;

            // (8) Visual states, tooltip and reduced motion with fixed dt.
            if (auto error = fresh("scenario-08-visual-states", 1.0f)) return error;
            {
                Status lookup;
                if (auto error = capture_one(scenes / "state-normal.png")) return error;
                POINT restore{};
                if (auto error = point_of(*this, Role::button, "Restore defaults", restore)) return error;
                if (auto error = capture_one(scenes / "state-hover.png", restore)) return error;
                if (auto error = hover_frames(restore, 30)) return error;
                const auto* tip = named(*this, Role::tooltip, "Turns notifications back on", lookup);
                if (!tip) return lookup;
                if (auto error = expect(inside(tip->bounds, logical_viewport(*this)),
                        "tooltip outside the viewport")) return error;
                if (auto error = capture_one(scenes / "state-tooltip.png", restore)) return error;
                send_down(window(), restore.x, restore.y);
                if (auto error = run_presents(*this, 1)) return error;
                if (auto error = capture_one(scenes / "state-pressed.png")) return error;
                send_move(window(), 4, 4);
                send_up(window(), 4, 4);
                if (auto error = settle(*this)) return error;
                if (auto error = check_model(*this, true, 0, "pressed state released outside")) return error;
                if (auto error = keyboard_focus()) return error;
                if (auto error = tab_to(*this, Role::checkbox, "Notifications")) return error;
                const auto* balanced = named(*this, Role::radio, "Balanced", lookup);
                if (!balanced) return lookup;
                const auto* controls = named(*this, Role::tab, "Controls", lookup);
                if (!controls) return lookup;
                if (auto error = expect(balanced->selected && controls->selected,
                        "selected radio/tab state not reported")) return error;
                if (auto error = capture_one(scenes / "state-focused-selected.png")) return error;
                SendMessageW(window(), WM_KILLFOCUS, 0, 0);
                if (auto error = settle(*this)) return error;
                // Hover animation progresses over fixed 1/60 s frames unless motion is reduced.
                auto hover_pixels = [&](std::vector<std::uint8_t>& first,
                                        std::vector<std::uint8_t>& settled) -> Status {
                    send_move(window(), 4, 4);
                    if (auto error = settle(*this)) return error;
                    if (auto error = hover_frames(restore, 1)) return error;
                    std::uint32_t w = 0, h = 0;
                    if (auto error = render_and_read_pixels(*this, last_packet(), first, w, h)) return error;
                    if (auto error = hover_frames(restore, 12)) return error;
                    return render_and_read_pixels(*this, last_packet(), settled, w, h);
                };
                std::vector<std::uint8_t> animated_first, animated_settled, reduced_first, reduced_settled;
                if (auto error = hover_pixels(animated_first, animated_settled)) return error;
                gallery_state().reduced_motion = true;
                if (auto error = hover_pixels(reduced_first, reduced_settled)) return error;
                if (auto error = expect(animated_first != animated_settled,
                        "hover transition not observable at normal motion")) return error;
                if (auto error = expect(reduced_first == reduced_settled,
                        "reduced motion still animated hover")) return error;
                gallery_state().reduced_motion = false;
                send_move(window(), 4, 4);
                set_disabled_mode(true);
                if (auto error = settle(*this)) return error;
                for (const auto& [role, label] : {std::pair{Role::checkbox, "Notifications"},
                                                  std::pair{Role::button, "Restore defaults"},
                                                  std::pair{Role::slider, "Volume"},
                                                  std::pair{Role::dropdown, "Quality menu"}}) {
                    const auto* item = named(*this, role, label, lookup);
                    if (!item) return lookup;
                    if (auto error = expect(!item->enabled, std::string(label) + " not disabled")) return error;
                }
                if (auto error = capture_one(scenes / "state-disabled.png")) return error;
                set_disabled_mode(false);
                if (auto error = settle(*this)) return error;
                if (auto error = click_named(*this, Role::text_input, "Name")) return error;
                if (auto error = chord(*this, VK_CONTROL, 'A')) return error;
                if (auto error = press_key(*this, VK_BACK)) return error;
                gallery_state().quantity = 9;
                if (auto error = settle(*this)) return error;
                const auto* name = named(*this, Role::text_input, "Name", lookup);
                if (!name) return lookup;
                const auto* quantity = named(*this, Role::slider, "Quantity", lookup);
                if (!quantity) return lookup;
                if (auto error = expect(gallery_state().name.empty() && name->invalid && quantity->invalid &&
                        count_named(*this, Role::validation, "Name is required") == 1 &&
                        count_named(*this, Role::validation, "Quantities above 8 need approval") == 1,
                        "invalid state or validation message missing")) return error;
                if (auto error = send_wheel(*this, {320, 200}, -10)) return error;
                if (auto error = capture_one(scenes / "state-invalid.png")) return error;
                log += "states: normal/hover/tooltip/pressed/focused/selected/disabled/invalid captured; "
                    "hover animates at fixed dt and is immediate with reduced motion\n";
            }
            if (auto error = finish_scenario()) return error;

            // (9) New glyphs grow the atlas; device recreation keeps the model and re-uploads.
            if (auto error = fresh("scenario-09-glyph-atlas-recreation", 1.0f)) return error;
            {
                const DrawPacket before = last_packet();
                if (auto error = keyboard_focus()) return error;
                if (auto error = click_named(*this, Role::text_input, "Name")) return error;
                if (auto error = chord(*this, VK_CONTROL, 'A')) return error;
                if (auto error = type_text(*this, L"ЖЯΩΨšđ")) return error;
                if (auto error = settle(*this)) return error;
                const std::string glyphs = "\xD0\x96\xD0\xAF\xCE\xA9\xCE\xA8\xC5\xA1\xC4\x91";
                const DrawPacket after = last_packet();
                bool new_snapshot = false;
                for (const auto& snapshot : after->textures) {
                    new_snapshot |= std::find(before->textures.begin(), before->textures.end(),
                        snapshot) == before->textures.end();
                }
                if (auto error = expect(gallery_state().name == glyphs && new_snapshot,
                        "new glyphs did not publish a new atlas snapshot")) return error;
                std::vector<std::uint8_t> old_pixels, new_pixels;
                std::uint32_t w = 0, h = 0;
                if (auto error = render_and_read_pixels(*this, before, old_pixels, w, h)) return error;
                if (auto error = recreate_device()) return error;
                if (auto error = settle(*this)) return error;
                Status lookup;
                const auto* input = named(*this, Role::text_input, "Name", lookup);
                if (!input) return lookup;
                if (auto error = expect(gallery_state().name == glyphs && input->value == glyphs,
                        "model lost across device recreation")) return error;
                std::vector<std::uint8_t> replay;
                if (auto error = render_and_read_pixels(*this, before, replay, w, h)) return error;
                if (auto error = expect(replay == old_pixels,
                        "retained pre-glyph packet differs after recreation")) return error;
                if (auto error = render_and_read_pixels(*this, last_packet(), new_pixels, w, h)) return error;
                if (auto error = expect(new_pixels != old_pixels,
                        "new glyph frame identical to old frame after recreation")) return error;
                if (auto error = capture_one(scenes / "atlas-after-recreation.png")) return error;
                log += "atlas: 6 new Cyrillic/Greek/Latin glyphs published a new snapshot; recreated device "
                    "replays the retained old packet byte-identically and renders the new glyphs\n";
            }
            if (auto error = finish_scenario()) return error;

            // (10) Four packets in flight: held packets stay renderable; a fifth frame is refused.
            if (auto error = fresh("scenario-10-packets-in-flight", 1.0f)) return error;
            {
                DrawPacket held = last_packet();
                std::vector<std::uint8_t> original, replay;
                std::uint32_t w = 0, h = 0;
                if (auto error = render_and_read_pixels(*this, held, original, w, h)) return error;
                // The host keeps presenting while an application retains an older packet.
                if (auto error = run_presents(*this, 6)) return error;
                // Fill the remaining slots directly; the context refuses a fifth live packet.
                std::vector<DrawPacket> direct;
                bool refused = false;
                for (int attempt = 0; attempt < 4 && !refused; ++attempt) {
                    auto begun = ui_context()->begin_frame(FrameInput{logical_viewport(*this), 1.0f,
                                                                      1.0 / 60.0, {}});
                    if (const auto* error = std::get_if<Error>(&begun)) {
                        if (error->code != ErrorCode::busy) return *error;
                        refused = true;
                        break;
                    }
                    Frame frame = std::get<Frame>(std::move(begun));
                    build_gallery(frame, gallery_state());
                    auto finished = frame.finish();
                    if (auto* error = std::get_if<Error>(&finished)) return *error;
                    direct.push_back(std::get<DrawPacket>(std::move(finished)));
                }
                const std::size_t filled = direct.size();
                if (auto error = expect(refused && filled >= 1 && filled <= 3,
                        "context did not refuse a frame once four packets were live")) return error;
                direct.clear();
                if (auto error = keyboard_focus()) return error;
                if (auto error = click_named(*this, Role::text_input, "Name")) return error;
                if (auto error = type_text(*this, L"щλ")) return error;
                if (auto error = settle(*this)) return error;
                if (auto error = render_and_read_pixels(*this, held, replay, w, h)) return error;
                if (auto error = expect(replay == original,
                        "held packet changed after newer atlas snapshots")) return error;
                held.reset();
                if (auto error = settle(*this)) return error;
                log += "packets: 1 application-held packet while the host presented; " +
                    std::to_string(filled) + " direct frames filled the remaining slots, the next "
                    "begin_frame returned busy; held packet replays byte-identically after atlas "
                    "growth; host resumes after release\n";
            }
            if (auto error = finish_scenario()) return error;

            if (auto error = fresh("resize-during-checkbox-press", 1.0f)) return error;
            POINT checkbox{};
            if (auto error = control_center(*this, Role::checkbox, 1.0f, checkbox)) return error;
            send_down(window(), checkbox.x, checkbox.y);
            if (auto error = run_presents(*this, 1)) return error;
            if (auto error = assert_capture(true, "resize press")) return error;
            if (auto error = check_model(*this, true, 0, "resize press")) return error;
            if (auto error = resize(320, 240)) return error;
            if (auto error = settle(*this)) return error;
            if (auto error = check_model(*this, true, 0, "resize while held")) return error;
            if (auto error = check_extent(*this)) return error;
            send_up(window(), 4, 4);
            if (auto error = settle(*this)) return error;
            if (auto error = check_model(*this, true, 0, "resized outside release")) return error;
            if (auto error = assert_capture(false, "resized outside release")) return error;
            if (target_size() != std::pair<std::uint32_t, std::uint32_t>{320, 240}) {
                return make_validation_error(ErrorCode::invalid_frame, "requested 320x240 resize not applied");
            }
            if (auto error = finish_scenario()) return error;

            if (auto error = fresh("narrow-checkbox-clipping", 1.0f, 180, 400)) return error;
            {
                Status lookup_error;
                const auto* narrow = find_one(last_semantics(), Role::checkbox, "Notifications", lookup_error);
                if (!narrow) return lookup_error;
                const auto* button = find_one(last_semantics(), Role::button, "Restore defaults", lookup_error);
                if (!button) return lookup_error;
                // Stacking is text-width driven; real Inter fits horizontally at 180px.
                if (narrow->bounds.h < Theme::dark().control_height || narrow->bounds.x < 0 ||
                    narrow->bounds.x + narrow->bounds.w > 180 ||
                    narrow->bounds.y + narrow->bounds.h > button->bounds.y ||
                    button->bounds.x < 0 || button->bounds.x + button->bounds.w > 180) {
                    return make_validation_error(ErrorCode::invalid_frame,
                        "180px controls violate minimum height, viewport bounds or non-overlap");
                }
            }
            if (auto error = capture_one(directory / "scale-100" / "narrow-180.png")) return error;
            if (auto error = finish_scenario()) return error;

            if (auto error = fresh("real-focus-loss-while-captured", 1.0f)) return error;
            if (auto error = focus_window(window())) return error;
            const HINSTANCE module = GetModuleHandleW(nullptr);
            if (!module) return native_error("GetModuleHandleW(peer)", GetLastError());
            OwnedWindow peer{CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"wui A10 focus peer",
                WS_POPUP | WS_VISIBLE, 0, 0, 8, 8, window(), nullptr, module, nullptr)};
            if (!peer.handle) return native_error("CreateWindowExW(focus peer)", GetLastError());
            // Creation may have activated the popup: establish the actual focus-loss source again.
            if (auto error = focus_window(window())) return error;
            if (auto error = control_center(*this, Role::checkbox, 1.0f, checkbox)) return error;
            send_down(window(), checkbox.x, checkbox.y);
            if (auto error = run_presents(*this, 1)) return error;
            if (auto error = assert_capture(true, "focus-loss prerequisite")) return error;
            if (auto error = check_model(*this, true, 0, "focus-loss prerequisite")) return error;
            if (auto error = focus_window(peer.handle)) return error;
            if (auto error = run_presents(*this, 2)) return error;
            if (auto error = assert_capture(false, "actual focus loss")) return error;
            if (auto error = check_model(*this, true, 0, "actual focus loss")) return error;
            send_up(window(), 4, 4);
            if (auto error = run_presents(*this, 2)) return error;
            if (auto error = check_model(*this, true, 0, "release after focus loss")) return error;
            if (auto error = focus_window(window())) return error;
            if (auto error = peer.close()) return error;
            if (auto error = click_control(*this, Role::checkbox, 1.0f)) return error;
            if (auto error = check_model(*this, false, 0, "ordinary click after focus restoration")) return error;
            log += "focus-loss: actual SetFocus to RAII-owned same-thread native window; programmatic input\n";
            if (auto error = finish_scenario()) return error;

            if (auto error = fresh("minimize-no-submit-no-present", 1.0f)) return error;
            if (auto error = click_control(*this, Role::checkbox, 1.0f)) return error;
            if (auto error = check_model(*this, false, 0, "pre-minimize")) return error;
            const unsigned submits = submit_count();
            const unsigned presents = present_count();
            ShowWindow(window(), SW_MINIMIZE);
            if (auto error = pump_and_wait(0)) return error;
            if (!IsIconic(window())) {
                return make_validation_error(ErrorCode::backend_failure, "ShowWindow did not minimize window");
            }
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (auto error = step(1.0 / 60.0)) return error;
                if (submit_count() != submits || present_count() != presents) {
                    return make_validation_error(ErrorCode::invalid_frame, "minimized window submitted or presented");
                }
                if (auto error = check_model(*this, false, 0, "minimized attempted step")) return error;
            }
            ShowWindow(window(), SW_RESTORE);
            if (IsIconic(window())) {
                return make_validation_error(ErrorCode::backend_failure, "ShowWindow did not restore window");
            }
            if (auto error = resize(640, 400)) return error;
            if (auto error = settle(*this)) return error;
            if (auto error = check_model(*this, false, 0, "restored after minimization")) return error;
            log += "minimize: attempted-steps=10 submit-delta=0 present-delta=0 state-preserved\n";
            if (auto error = finish_scenario()) return error;

            if (auto error = fresh("controlled-device-recreation", 1.0f)) return error;
            if (auto error = click_control(*this, Role::checkbox, 1.0f)) return error;
            if (auto error = check_model(*this, false, 0, "recreation first toggle")) return error;
            if (auto error = click_control(*this, Role::button, 1.0f)) return error;
            if (auto error = check_model(*this, true, 1, "recreation Restore")) return error;
            if (auto error = click_control(*this, Role::checkbox, 1.0f)) return error;
            if (auto error = check_model(*this, false, 1, "recreation second toggle")) return error;
            send_move(window(), 4, 4);
            SendMessageW(window(), WM_KILLFOCUS, 0, 0);
            if (auto error = settle(*this)) return error;
            DrawPacket retained = last_packet();
            std::vector<std::uint8_t> before_pixels;
            std::uint32_t before_w = 0, before_h = 0;
            if (auto error = render_and_read_pixels(*this, retained, before_pixels, before_w, before_h)) return error;
            log += "controlled-recreation old-device diagnostics:\n";
            if (auto error = check_info_queue(*this, log)) return error;
            if (auto error = write_report(report_path, log)) return error;
            if (auto error = recreate_device()) return error;
            if (auto error = settle(*this)) return error;
            if (auto error = check_model(*this, false, 1, "controlled recreation model")) return error;
            if (auto error = check_controls(*this, true)) return error;
            std::vector<std::uint8_t> after_pixels;
            std::uint32_t after_w = 0, after_h = 0;
            if (auto error = render_and_read_pixels(*this, retained, after_pixels, after_w, after_h)) return error;
            if (before_w != after_w || before_h != after_h || before_pixels != after_pixels) {
                return make_validation_error(ErrorCode::backend_failure,
                    "retained packet differs after controlled device recreation");
            }
            retained.reset();
            log += "controlled-recreation: quiesced healthy device, retained packet byte-identical on "
                "cleared isolated targets; not hardware-removal proof\n";
            if (auto error = finish_scenario()) return error;

            if (auto error = fresh("synthetic-WM_DPICHANGED-message-path", 1.0f)) return error;
            set_validation_scale(std::nullopt);
            if (auto error = reset_validation_scenario()) return error;
            if (auto error = settle(*this)) return error;
            if (auto error = window_dpi(window(), actual_dpi)) return error;
            RECT original{};
            if (!GetWindowRect(window(), &original)) return native_error("GetWindowRect(synthetic DPI)", GetLastError());
            RECT suggested{original.left, original.top, original.right + 40, original.bottom + 40};
            const unsigned dpi_messages = dpi_change_count();
            SendMessageW(window(), WM_DPICHANGED, MAKEWPARAM(actual_dpi, actual_dpi),
                reinterpret_cast<LPARAM>(&suggested));
            if (auto error = settle(*this)) return error;
            RECT applied{};
            if (!GetWindowRect(window(), &applied)) return native_error("GetWindowRect(applied DPI)", GetLastError());
            if (!EqualRect(&suggested, &applied) || dpi_change_count() != dpi_messages + 1) {
                return make_validation_error(ErrorCode::invalid_frame,
                    "synthetic WM_DPICHANGED suggested rectangle/message not applied exactly");
            }
            if (std::abs(adapter_scale() - static_cast<float>(actual_dpi) / 96.0f) > 0.001f) {
                return make_validation_error(ErrorCode::invalid_frame, "synthetic DPI input scale mismatch");
            }
            if (auto error = click_control(*this, Role::checkbox, adapter_scale())) return error;
            if (auto error = check_model(*this, false, 0, "synthetic DPI pointer alignment")) return error;
            log += "synthetic-WM_DPICHANGED: actual-dpi=" + std::to_string(actual_dpi) +
                " suggested/applied=" + std::to_string(applied.left) + "," +
                std::to_string(applied.top) + ".." + std::to_string(applied.right) + "," +
                std::to_string(applied.bottom) + "; message-path check only, no OS DPI change\n";
            if (auto error = finish_scenario()) return error;

            if (auto error = checkpoint("real-monitor-DPI-enumeration-and-moves")) return error;
            if (auto error = reset_validation_scenario()) return error;
            if (auto error = settle(*this)) return error;
            if (auto error = exercise_monitor_dpi(*this, log)) return error;
            if (auto error = finish_scenario()) return error;
            log += "native-captures=" + std::to_string(captures) + "\n";
            return std::nullopt;
        }();
    } catch (const std::exception& error) {
        result = make_validation_error(ErrorCode::backend_failure,
            std::string("validation exception: ") + error.what());
    }
    if (result) {
        log += "FAIL " + scenario + ": " + result->message + "\n";
        // Preserve any queued diagnostics even when a scenario assertion failed first.
        if (auto queue_error = check_info_queue(*this, log)) {
            log += "failure-diagnostics: " + queue_error->message + "\n";
        }
    } else {
        log += "PASS programmatic validation; manual/visual review remains separate\n";
    }
    if (auto report_error = write_report(report_path, log)) {
        if (result) report_error->message += "; original failure: " + result->message;
        return report_error;
    }
    return result;
}

Status NativeApp::measure(const std::filesystem::path& directory,
                          std::optional<StressWorkload> workload) {
    set_evidence_enabled(false);
    struct RestoreHost {
        NativeApp& app;
        ~RestoreHost() {
            app.set_validation_scale(std::nullopt);
            app.set_workload(std::nullopt);
        }
    } restore_host{*this};
    if (auto error = make_directory(directory)) return error;
    const char* workload_name = !workload ? "gallery"
        : *workload == StressWorkload::controls_100 ? "controls100"
        : *workload == StressWorkload::controls_1000 ? "controls1000" : "virtual10000";
    const bool fixed_gallery = !workload;
    const auto report_path = directory / (std::string("measurement-") + workload_name + ".txt");
#ifdef NDEBUG
    constexpr const char* build = "Release";
#else
    constexpr const char* build = "Debug";
#endif
#ifdef WUI_GALLERY_CXX_FLAGS
    constexpr const char* flags = WUI_GALLERY_CXX_FLAGS;
#else
    constexpr const char* flags = "unrecorded";
#endif
    std::string summary = std::string("c4 measurement workload=") + workload_name + "\n" +
        "boundary: UI-thread C++ allocation requests (plain/array/aligned/nothrow new), "
        "not direct malloc, other threads, driver allocations or whole-process heap freedom\n"
        "cpu scope: actual host Context::begin_frame + build_gallery/build_stress_gallery + "
        "Frame::finish only; actual Adapter input drain, renderer/collect and successful "
        "swap-chain Present outside count\n"
        "gpu scope: D3D11 TIMESTAMP_DISJOINT plus two TIMESTAMP queries around the UI "
        "renderer submission only, read asynchronously outside the measured build; "
        "Present time is never reported as GPU time\n"
        "memory: retained = Context PMR used/peak after each frame; process = "
        "PeakWorkingSetSize from GetProcessMemoryInfo, sampled outside the count\n"
        "prewarm: all four real Context packet slots retained together then released, outside "
        "count, building this same workload with the same viewport, scale, dt, theme and model\n"
        "warmup: 120 actual successfully presented host frames per accepted batch\n"
        "samples: 300 actual successfully presented host frames, preallocated storage\n"
        "input/resize/DPI/suspension/recreation/model interruptions discard the entire batch "
        "and restart four-slot prewarm plus 120-frame warmup (maximum five attempts)\n"
        "scales are simulated render/input scales, not changed OS DPI\n";
    summary += fixed_gallery
        ? "gate: fixed gallery requires cpp_alloc_calls == 0 and cpp_alloc_bytes == 0 in "
          "every warmed sample of every batch (frame-indexed FAIL otherwise)\n"
          "theme: dark, comfortable density, violet accent, reduced motion off; section "
          "Controls (no gradient/border/shadow effects on that section)\n"
        : "gate: none; stress workloads are variable-path measurements and report their "
          "allocation counts without claiming the fixed-gallery gate\n"
          "theme: dark, compact density with gap 0 (28px controls, 28px pitch); no effects\n";
    summary += "build=" + std::string(build) + " msvc=" + std::to_string(_MSC_FULL_VER) +
        " flags=\"" + flags + "\" driver=" + driver_description() +
        " debug=" + (debug_enabled() ? "yes" : "unavailable") + "\n";
    if (auto error = write_report(report_path, summary)) return error;

    struct Batch {
        const char* name;
        float scale;
        std::uint32_t logical_w, logical_h;
    };
    constexpr std::array<Batch, 5> gallery_batches{
        Batch{"gallery-1280x720", 1.0f, 1280, 720},
        Batch{"gallery-scale-100", 1.0f, 800, 600},
        Batch{"gallery-scale-125", 1.25f, 800, 600},
        Batch{"gallery-scale-150", 1.5f, 800, 600},
        Batch{"gallery-scale-200", 2.0f, 800, 600}};
    const Batch stress_batch{workload_name, 1.0f, 1280, 720};
    const std::span<const Batch> batches = fixed_gallery
        ? std::span<const Batch>(gallery_batches) : std::span<const Batch>(&stress_batch, 1);

    Status result;
    try {
        result = [&]() -> Status {
            for (const auto& batch : batches) {
                const float scale = batch.scale;
                const auto width = static_cast<std::uint32_t>(std::lround(batch.logical_w * scale));
                const auto height = static_cast<std::uint32_t>(std::lround(batch.logical_h * scale));
                const Rect viewport{0, 0, static_cast<float>(batch.logical_w),
                                    static_cast<float>(batch.logical_h)};
                std::array<FrameSample, 300> rows{};
                unsigned accepted_attempt = 0;
                unsigned batch_presents = 0;
                std::size_t expected_semantics = 0, expected_buttons = 0;
                for (unsigned attempt = 1; attempt <= 5; ++attempt) {
                    summary += "BEGIN " + std::string(batch.name) + " attempt=" +
                        std::to_string(attempt) + "\n";
                    if (auto error = write_report(report_path, summary)) return error;
                    set_validation_scale(scale);
                    set_disabled_mode(false);
                    set_workload(workload);
                    if (auto error = resize(width, height)) return error;
                    send_up(window(), 4, 4);
                    SendMessageW(window(), WM_KILLFOCUS, 0, 0);
                    send_move(window(), 4, 4);
                    if (auto error = reset_validation_scenario()) return error;
                    if (auto error = check_model(*this, true, 0, "measurement reset")) return error;
                    if (target_size() != std::pair{width, height}) {
                        return make_validation_error(ErrorCode::invalid_frame,
                            "measurement requested physical extent was not applied");
                    }
                    Context* context = ui_context();
                    if (!context) {
                        return make_validation_error(ErrorCode::invalid_frame, "measurement context missing");
                    }
                    // The host applies this same theme before its first frame.
                    if (auto error = apply_model_theme()) return error;
                    gallery_state().viewport_height = viewport.h;
                    // Synthetic input is used only to lease/warm every slot, never to replace sampled host input.
                    {
                        std::array<DrawPacket, 4> held{};
                        for (std::size_t slot = 0; slot < held.size(); ++slot) {
                            auto& packet = held[slot];
                            FrameInput input{viewport, scale, 1.0 / 60.0, {}};
                            auto begun = context->begin_frame(input);
                            if (auto* error = std::get_if<Error>(&begun)) return *error;
                            Frame frame = std::get<Frame>(std::move(begun));
                            if (workload) build_stress_gallery(frame, gallery_state(), *workload);
                            else build_gallery(frame, gallery_state());
                            auto finished = frame.finish();
                            if (auto* error = std::get_if<Error>(&finished)) return *error;
                            packet = std::get<DrawPacket>(std::move(finished));
                            if (!packet) {
                                return make_validation_error(ErrorCode::invalid_packet,
                                    "four-slot prewarm produced a null packet");
                            }
                            std::size_t buttons = 0;
                            std::uint64_t first_id = 0;
                            for (const auto& item : packet->semantics) {
                                if (item.role != Role::button) continue;
                                ++buttons;
                                if (!first_id) first_id = item.id.value;
                                // Grid controls lie fully inside; the last virtual row may be clipped.
                                const bool placed = workload == StressWorkload::virtual_10000
                                    ? item.bounds.y < viewport.y + viewport.h &&
                                      item.bounds.y + item.bounds.h > viewport.y
                                    : inside(item.bounds, viewport);
                                if (workload && !placed) {
                                    return make_validation_error(ErrorCode::invalid_packet,
                                        std::string(batch.name) + ": stress control outside viewport: " +
                                        item.label);
                                }
                            }
                            if (slot == 0) {
                                expected_semantics = packet->semantics.size();
                                expected_buttons = buttons;
                            } else if (packet->semantics.size() != expected_semantics ||
                                       buttons != expected_buttons) {
                                return make_validation_error(ErrorCode::invalid_packet,
                                    std::string(batch.name) + ": prewarm packets disagree");
                            }
                        }
                        std::size_t required_buttons = 0;
                        if (!workload) {
                            const auto in_packet = [&](Role role, std::string_view label) {
                                return std::count_if(held[0]->semantics.begin(),
                                    held[0]->semantics.end(), [&](const Semantic& item) {
                                        return item.role == role && item.label == label;
                                    });
                            };
                            if (in_packet(Role::checkbox, "Notifications") != 1 ||
                                in_packet(Role::button, "Restore defaults") != 1 ||
                                gallery_state().section != 1) {
                                return make_validation_error(ErrorCode::invalid_packet,
                                    std::string(batch.name) + ": prewarm is not the Controls gallery");
                            }
                            required_buttons = expected_buttons;
                        } else if (*workload == StressWorkload::controls_100) {
                            required_buttons = 100;
                        } else if (*workload == StressWorkload::controls_1000) {
                            required_buttons = 1000;
                        } else {
                            auto range = visible_rows(10'000, 28, 0, viewport.h);
                            if (auto* error = std::get_if<Error>(&range)) return *error;
                            const auto rows_visible = std::get<VisibleRows>(range);
                            required_buttons = rows_visible.last - rows_visible.first;
                        }
                        if (expected_buttons != required_buttons) {
                            return make_validation_error(ErrorCode::invalid_packet,
                                std::string(batch.name) + ": expected " +
                                std::to_string(required_buttons) + " buttons, packet has " +
                                std::to_string(expected_buttons));
                        }
                    }
                    unsigned completed_frames = 0;
                    const unsigned presents_before = present_count();
                    bool restart = false;
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (completed_frames < 120 + rows.size()) {
                        if (std::chrono::steady_clock::now() >= deadline) {
                            return make_validation_error(ErrorCode::backend_failure,
                                "measurement waited 5s without a completed Present");
                        }
                        FrameSample sample{};
                        bool completed = false;
                        bool interrupted = false;
                        if (auto error = sample_step(sample, completed, interrupted)) return error;
                        if (interrupted || target_size() != std::pair{width, height} ||
                            gallery_state().notifications != true ||
                            gallery_state().restore_count != 0 || gallery_state().section != 1) {
                            summary += "DISCARD " + std::string(batch.name) +
                                " attempt=" + std::to_string(attempt) + " completed=" +
                                std::to_string(completed_frames) +
                                " cause=host-input/resize/DPI/suspension/recovery/model-interruption\n";
                            restart = true;
                            break;
                        }
                        if (!completed) continue;
                        if (!std::isfinite(sample.cpu_us) || sample.cpu_us < 0 ||
                            sample.semantics != expected_semantics ||
                            !std::isfinite(sample.scale) || std::abs(sample.scale - scale) > 0.001f) {
                            return make_validation_error(ErrorCode::invalid_frame,
                                std::string(batch.name) + ": completed sample " +
                                std::to_string(completed_frames) + " has invalid timing/semantics/scale"
                                " (semantics=" + std::to_string(sample.semantics) + " expected=" +
                                std::to_string(expected_semantics) + ")");
                        }
                        const auto stats = context->statistics();
                        sample.retained_bytes = stats.retained_bytes;
                        sample.peak_retained_bytes = stats.peak_retained_bytes;
                        PROCESS_MEMORY_COUNTERS memory{};
                        memory.cb = sizeof(memory);
                        if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
                            sample.process_peak_bytes = memory.PeakWorkingSetSize;
                        if (completed_frames >= 120) rows[completed_frames - 120] = sample;
                        ++completed_frames;
                        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    }
                    if (restart) continue;
                    batch_presents = present_count() - presents_before;
                    if (batch_presents != 420) {
                        return make_validation_error(ErrorCode::invalid_frame,
                            "accepted measurement batch did not correspond to exactly 420 successful Presents");
                    }
                    accepted_attempt = attempt;
                    break;
                }
                if (accepted_attempt == 0) {
                    return make_validation_error(ErrorCode::backend_failure,
                        std::string(batch.name) + ": repeated interference prevented a stable batch in five attempts");
                }
                // GPU readback, serialization, sorting, native diagnostics and writes are outside all sampled scopes.
                collect_gpu_timings(true);
                std::string csv =
                    "frame,cpu_us,cpp_alloc_calls,cpp_alloc_bytes,vertices,indices,batches,textures,"
                    "semantics,gpu_us,gpu_status,retained_bytes,peak_retained_bytes,"
                    "process_peak_working_set_bytes\n";
                csv.reserve(64000);
                std::array<double, 300> cpu{};
                std::vector<double> gpu;
                gpu.reserve(rows.size());
                std::string gpu_unavailable;
                std::uint64_t sum_calls = 0, min_calls = rows.front().calls, max_calls = 0;
                std::uint64_t sum_bytes = 0, min_bytes = rows.front().bytes, max_bytes = 0;
                std::size_t min_vertices = rows.front().vertices, max_vertices = 0;
                std::size_t min_batches = rows.front().batches, max_batches = 0;
                std::size_t peak_retained = 0, max_retained = 0, process_peak = 0;
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    auto& row = rows[i];
                    resolve_gpu_timing(row);
                    char buffer[512]{};
                    const int length = std::snprintf(buffer, sizeof(buffer),
                        "%zu,%.6f,%llu,%llu,%zu,%zu,%zu,%zu,%zu,%.3f,%s,%zu,%zu,%zu\n", i, row.cpu_us,
                        static_cast<unsigned long long>(row.calls),
                        static_cast<unsigned long long>(row.bytes), row.vertices, row.indices,
                        row.batches, row.textures, row.semantics, row.gpu_us, row.gpu_status,
                        row.retained_bytes, row.peak_retained_bytes, row.process_peak_bytes);
                    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(buffer)) {
                        return make_validation_error(ErrorCode::backend_failure, "CSV row formatting failed");
                    }
                    csv.append(buffer, static_cast<std::size_t>(length));
                    sum_calls += row.calls;
                    min_calls = std::min(min_calls, row.calls);
                    max_calls = std::max(max_calls, row.calls);
                    sum_bytes += row.bytes;
                    min_bytes = std::min(min_bytes, row.bytes);
                    max_bytes = std::max(max_bytes, row.bytes);
                    min_vertices = std::min(min_vertices, row.vertices);
                    max_vertices = std::max(max_vertices, row.vertices);
                    min_batches = std::min(min_batches, row.batches);
                    max_batches = std::max(max_batches, row.batches);
                    if (row.peak_retained_bytes < peak_retained) {
                        return make_validation_error(ErrorCode::invalid_frame,
                            std::string(batch.name) + ": retained peak decreased at frame " +
                            std::to_string(i));
                    }
                    peak_retained = row.peak_retained_bytes;
                    max_retained = std::max(max_retained, row.retained_bytes);
                    process_peak = std::max(process_peak, row.process_peak_bytes);
                    cpu[i] = row.cpu_us;
                    if (std::string_view(row.gpu_status) == "ok") gpu.push_back(row.gpu_us);
                    else if (gpu_unavailable.empty()) gpu_unavailable = row.gpu_status;
                }
                const auto csv_path = directory / (std::string(batch.name) + ".csv");
                if (auto error = write_report(csv_path, csv)) return error;
                std::sort(cpu.begin(), cpu.end());
                const double median = (cpu[149] + cpu[150]) / 2;
                const double p95 = cpu[284]; // Nearest-rank 95th percentile of 300 samples.
                summary += "ACCEPT " + std::string(batch.name) + " csv=" + csv_path.string() +
                    " attempt=" + std::to_string(accepted_attempt) +
                    " prewarm-slots=4 warmup-presents=120 sampled-presents=300 total-presents=" +
                    std::to_string(batch_presents) + " scale=" + std::to_string(scale) +
                    " logical=" + std::to_string(batch.logical_w) + "x" +
                    std::to_string(batch.logical_h) +
                    " physical=" + std::to_string(width) + "x" + std::to_string(height) +
                    " semantics=" + std::to_string(expected_semantics) +
                    " buttons=" + std::to_string(expected_buttons) + "\n";
                summary += " cpp_alloc_calls sum=" + std::to_string(sum_calls) +
                    " min=" + std::to_string(min_calls) + " max=" + std::to_string(max_calls) +
                    " cpp_alloc_bytes sum=" + std::to_string(sum_bytes) +
                    " min=" + std::to_string(min_bytes) + " max=" + std::to_string(max_bytes) + "\n";
                summary += " cpu_us min=" + std::to_string(cpu.front()) +
                    " median=" + std::to_string(median) + " p95-nearest-rank=" +
                    std::to_string(p95) + " max=" + std::to_string(cpu.back()) + "\n";
                if (gpu.empty()) {
                    summary += " gpu_us unavailable: " + gpu_unavailable + "\n";
                } else {
                    std::sort(gpu.begin(), gpu.end());
                    summary += " gpu_us samples=" + std::to_string(gpu.size()) + "/300 min=" +
                        std::to_string(gpu.front()) + " median=" +
                        std::to_string(gpu[gpu.size() / 2]) + " max=" + std::to_string(gpu.back());
                    if (!gpu_unavailable.empty()) summary += " first-unavailable=" + gpu_unavailable;
                    summary += "\n";
                }
                summary += " vertices min=" + std::to_string(min_vertices) + " max=" +
                    std::to_string(max_vertices) + " batches min=" + std::to_string(min_batches) +
                    " max=" + std::to_string(max_batches) + " retained_bytes max=" +
                    std::to_string(max_retained) + " peak=" + std::to_string(peak_retained) +
                    " ceiling=" + std::to_string(ContextDesc{}.max_retained_bytes) +
                    " process_peak_working_set_bytes=" + std::to_string(process_peak) + "\n";
                if (fixed_gallery) {
                    for (std::size_t i = 0; i < rows.size(); ++i) {
                        if (rows[i].calls != 0 || rows[i].bytes != 0) {
                            summary += "FAIL gallery zero-new batch=" + std::string(batch.name) +
                                " frame=" + std::to_string(i) +
                                " calls=" + std::to_string(rows[i].calls) +
                                " bytes=" + std::to_string(rows[i].bytes) + "\n";
                            if (auto error = write_report(report_path, summary)) return error;
                            return make_validation_error(ErrorCode::invalid_frame,
                                "warmed fixed gallery allocated C++ storage");
                        }
                    }
                    summary += "PASS gallery zero-new 300/300 presented frames batch=" +
                        std::string(batch.name) + "\n";
                } else {
                    summary += "MEASURED " + std::string(batch.name) +
                        " variable-path allocations reported above; not gated\n";
                }
                if (auto error = check_info_queue(*this, summary)) return error;
                if (auto error = write_report(report_path, summary)) return error;
            }
            return std::nullopt;
        }();
    } catch (const std::exception& error) {
        result = make_validation_error(ErrorCode::backend_failure,
            std::string("measurement exception: ") + error.what());
    }
    summary += result ? "FAIL measurement: " + result->message + "\n" : "PASS measurement\n";
    if (auto error = write_report(report_path, summary)) {
        if (result) error->message += "; original failure: " + result->message;
        return error;
    }
    return result;
}

}  // namespace wui::example
