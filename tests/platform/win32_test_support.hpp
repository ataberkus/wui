#pragma once

#include <wui/backends/win32.hpp>

#include <thread>
#include <utility>
#include <vector>

namespace wui::win32::test {
class HostWindow {
public:
    HostWindow() {
        static const WindowClass window_class;
        if (window_class.atom != 0) {
            window_ = CreateWindowExW(0, class_name, L"wui hidden native test",
                WS_OVERLAPPEDWINDOW, 200, 150, 420, 300, nullptr, nullptr,
                GetModuleHandleW(nullptr), this);
        }
    }
    ~HostWindow() {
        adapter_ = nullptr;
        destroy();
    }
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    HWND get() const { return window_; }
    void attach(Adapter* adapter) { adapter_ = adapter; }
    void send(UINT message, WPARAM wparam = 0, LPARAM lparam = 0) const {
        SendMessageW(window_, message, wparam, lparam);
    }
    void destroy() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    unsigned capture_changes = 0;
    unsigned unconsumed_dpi_changes = 0;
    bool apply_suggested_rect = false;
    bool applied_suggested_rect = false;
    // Every message this class procedure received, in order.
    std::vector<UINT> observed;

private:
    inline static constexpr wchar_t class_name[] = L"wui.A8.NativeTest.Host";
    struct WindowClass {
        ATOM atom = 0;
        WindowClass() {
            WNDCLASSEXW definition{};
            definition.cbSize = sizeof(definition);
            definition.lpfnWndProc = &HostWindow::procedure;
            definition.hInstance = GetModuleHandleW(nullptr);
            definition.lpszClassName = class_name;
            atom = RegisterClassExW(&definition);
        }
        ~WindowClass() {
            if (atom != 0) UnregisterClassW(class_name, GetModuleHandleW(nullptr));
        }
    };
    static LRESULT CALLBACK procedure(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
        auto* host = reinterpret_cast<HostWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            host = static_cast<HostWindow*>(
                reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(host));
        }
        if (host != nullptr) {
            host->observed.push_back(message);
            if (message == WM_CAPTURECHANGED) ++host->capture_changes;
            if (host->adapter_ != nullptr &&
                host->adapter_->process(message, wparam, lparam)) {
                return 0;
            }
            if (message == WM_DPICHANGED) {
                ++host->unconsumed_dpi_changes;
                if (host->apply_suggested_rect && lparam != 0) {
                    const auto& rect = *reinterpret_cast<const RECT*>(lparam);
                    host->applied_suggested_rect = SetWindowPos(window, nullptr,
                        rect.left, rect.top, rect.right - rect.left,
                        rect.bottom - rect.top,
                        SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
                }
                return 0;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    HWND window_ = nullptr;
    Adapter* adapter_ = nullptr;
};

class WindowBinding {
public:
    WindowBinding(HostWindow& host, Adapter& adapter) : host_(host) {
        host_.attach(&adapter);
    }
    ~WindowBinding() { host_.attach(nullptr); }
    WindowBinding(const WindowBinding&) = delete;
    WindowBinding& operator=(const WindowBinding&) = delete;
private:
    HostWindow& host_;
};

// Runs work on a fresh thread. The calling (window) thread keeps dispatching
// messages sent to its windows while it waits, so the worker never deadlocks
// on a synchronous cross-thread SendMessage.
template<class Work>
void run_off_thread(Work&& work) {
    std::thread worker(std::forward<Work>(work));
    const HANDLE handle = worker.native_handle();
    while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    worker.join();
}
} // namespace wui::win32::test
