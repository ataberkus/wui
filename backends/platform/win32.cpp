#include "win32_detail.hpp"

#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace wui::win32 {
namespace {
constexpr std::size_t event_limit = 65536;
// Tracked modifier keys, in the index order of Adapter::Impl::modifiers.
constexpr std::array<int, 8> modifier_keys{
    VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};

Key key_code(WPARAM code) {
    if (code >= '0' && code <= '9')
        return static_cast<Key>(static_cast<int>(Key::digit0) + static_cast<int>(code - '0'));
    if (code >= 'A' && code <= 'Z') {
        static constexpr std::array<Key, 26> letters{
            Key::a, Key::b, Key::c, Key::d, Key::e, Key::f, Key::g, Key::h, Key::i,
            Key::j, Key::k, Key::l, Key::m, Key::n, Key::o, Key::p, Key::q, Key::r,
            Key::s, Key::t, Key::u, Key::v, Key::w, Key::x, Key::y, Key::z};
        return letters[static_cast<std::size_t>(code - 'A')];
    }
    if (code >= static_cast<WPARAM>(VK_F1) && code <= static_cast<WPARAM>(VK_F12))
        return static_cast<Key>(static_cast<int>(Key::f1) + static_cast<int>(code - VK_F1));
    switch (code) {
    case VK_TAB: return Key::tab;
    case VK_RETURN: return Key::enter;
    case VK_SPACE: return Key::space;
    case VK_ESCAPE: return Key::escape;
    case VK_LEFT: return Key::left;
    case VK_RIGHT: return Key::right;
    case VK_UP: return Key::up;
    case VK_DOWN: return Key::down;
    case VK_HOME: return Key::home;
    case VK_END: return Key::end;
    case VK_BACK: return Key::backspace;
    case VK_DELETE: return Key::del;
    case VK_INSERT: return Key::insert;
    case VK_PRIOR: return Key::page_up;
    case VK_NEXT: return Key::page_down;
    case VK_LSHIFT: return Key::left_shift;
    case VK_RSHIFT: return Key::right_shift;
    case VK_LCONTROL: return Key::left_control;
    case VK_RCONTROL: return Key::right_control;
    case VK_LMENU: return Key::left_alt;
    case VK_RMENU: return Key::right_alt;
    case VK_LWIN: return Key::left_super;
    case VK_RWIN: return Key::right_super;
    default: return Key::none;
    }
}

// Generic modifier codes become their left/right virtual key: Shift by its
// scan code (0x36 is right), Control and Alt by the extended-key bit.
WPARAM resolve_key(WPARAM key, LPARAM flags) {
    const bool extended = (flags & (LPARAM{1} << 24)) != 0;
    switch (key) {
    case VK_SHIFT:
        return ((static_cast<unsigned long long>(flags) >> 16) & 0xff) == 0x36
            ? static_cast<WPARAM>(VK_RSHIFT) : static_cast<WPARAM>(VK_LSHIFT);
    case VK_CONTROL:
        return extended ? static_cast<WPARAM>(VK_RCONTROL) : static_cast<WPARAM>(VK_LCONTROL);
    case VK_MENU:
        return extended ? static_cast<WPARAM>(VK_RMENU) : static_cast<WPARAM>(VK_LMENU);
    default:
        return key;
    }
}

int modifier_index(WPARAM key) {
    for (std::size_t index = 0; index < modifier_keys.size(); ++index) {
        if (key == static_cast<WPARAM>(modifier_keys[index])) return static_cast<int>(index);
    }
    return -1;
}

std::optional<PointerButton> message_button(UINT message, WPARAM wparam) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: return PointerButton::left;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: return PointerButton::right;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP: return PointerButton::middle;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
        switch (GET_XBUTTON_WPARAM(wparam)) {
        case XBUTTON1: return PointerButton::x1;
        case XBUTTON2: return PointerButton::x2;
        default: return std::nullopt;
        }
    default: return std::nullopt;
    }
}

bool button_release(UINT message) {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
           message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}

unsigned button_bit(PointerButton button) { return 1u << static_cast<unsigned>(button); }

// The thread that owns window, or 0 for a null, destroyed or foreign HWND.
DWORD owner_thread(HWND window) {
    if (!window) return 0;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() ? thread : 0;
}
}
struct Adapter::Impl {
    struct Target {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        float scale = 1;
    };
    // Immutable after construction: the HWND's owning thread.
    const DWORD thread;
    // Window-thread-only state; other threads never read or write these.
    bool releasing = false, tracking = false;

    // Everything below is shared with the frame thread and guarded by mutex.
    // The mutex is never held across calls that may send messages to the
    // window (capture, mouse tracking, IMM), because those re-enter process().
    std::mutex mutex;
    HWND window;
    float scale = 1;
    std::optional<Target> target;
    Status failure;
    std::vector<InputEvent> events;
    std::size_t queued_bytes = 0;
    wchar_t high_surrogate = 0;
    unsigned high_repeats = 0;
    std::wstring result_echo;
    std::size_t char_echo = 0, ime_echo = 0;
    std::array<bool, 8> modifiers{};
    unsigned buttons = 0;
    bool composing = false, has_caret = false;
    // Window-thread work requested elsewhere; applied by the next process().
    bool release_requested = false, caret_pending = false, sync_pending = false;
    Vec2 caret{};
    detail::ImeApi ime;

    explicit Impl(HWND handle) : thread(owner_thread(handle)), window(handle) {
        if (thread == 0 || !IsWindow(window) || !IsWindowUnicode(window)) {
            failure = Error{ErrorCode::invalid_argument,
                            "Adapter requires a live Unicode HWND owned by this process"};
            return;
        }
        auto dpi = detail::window_scale(window);
        if (auto* error = std::get_if<Error>(&dpi)) {
            failure = std::move(*error);
            return;
        }
        scale = std::get<float>(dpi);
        // Key state is per thread: only the window thread observes the
        // modifiers that accompany the window's messages.
        if (on_window_thread()) sync_modifiers();
        else sync_pending = true;
    }
    bool on_window_thread() const { return thread != 0 && GetCurrentThreadId() == thread; }
    void sync_modifiers() {
        for (std::size_t i = 0; i < modifier_keys.size(); ++i)
            modifiers[i] = (GetKeyState(modifier_keys[i]) & 0x8000) != 0;
    }
    HWND current_window() {
        std::lock_guard lock(mutex);
        return window;
    }
    // Window thread only, mutex not held: ReleaseCapture synchronously sends
    // WM_CAPTURECHANGED, which re-enters process().
    void release_capture(HWND handle) noexcept {
        if (handle == nullptr || releasing || GetCapture() != handle) return;
        releasing = true;
        ReleaseCapture();
        releasing = false;
    }
    void fail(Error error) noexcept {
        if (!failure) failure = std::move(error);
        events.clear();
        queued_bytes = 0;
        high_surrogate = 0;
        high_repeats = 0;
        result_echo.clear();
        composing = false;
        buttons = 0;
        release_requested = true;
        caret_pending = false;
    }
    void fail(ErrorCode code, const char* message) {
        fail(Error{code, message});
    }
    void fail_now(Error error) {
        std::lock_guard lock(mutex);
        fail(std::move(error));
    }
    bool live() {
        if (failure) return false;
        if (!window || !IsWindow(window)) {
            fail(ErrorCode::invalid_frame, "Adapter window is destroyed");
            return false;
        }
        return true;
    }
    float frame_scale() const { return target ? target->scale : scale; }
    bool shift() const { return modifiers[0] || modifiers[1]; }
    bool control() const { return modifiers[2] || modifiers[3]; }
    bool alt() const { return modifiers[4] || modifiers[5]; }
    bool super() const { return modifiers[6] || modifiers[7]; }
    void append(InputEvent event) {
        if (failure) return;
        if (events.size() == event_limit || sizeof(InputEvent) > detail::text_byte_limit - queued_bytes ||
            event.text.size() > detail::text_byte_limit - queued_bytes - sizeof(InputEvent)) {
            fail(ErrorCode::capacity, "Win32 input queue exceeds its event or byte limit");
            return;
        }
        queued_bytes += sizeof(InputEvent) + event.text.size();
        events.push_back(std::move(event));
    }
    void text_event(std::wstring_view value, EventKind kind = EventKind::text, unsigned repeats = 1) {
        auto converted = detail::to_utf8(value);
        if (auto* error = std::get_if<Error>(&converted)) { fail(std::move(*error)); return; }
        InputEvent event{kind};
        event.text = std::get<std::string>(std::move(converted));
        event.shift = shift(); event.control = control();
        event.alt = alt(); event.super = super();
        for (unsigned i = 1; i < repeats && !failure; ++i) append(event);
        if (repeats != 0) append(std::move(event));
    }
    void flush_surrogate() {
        if (high_surrogate) {
            high_surrogate = 0;
            const auto repeats = high_repeats;
            high_repeats = 0;
            text_event(L"\uFFFD", EventKind::text, repeats);
        }
    }
    void clear_echo() { result_echo.clear(); char_echo = ime_echo = 0; }
    bool is_echo(std::wstring_view value, bool from_ime) {
        auto& index = from_ime ? ime_echo : char_echo;
        if (index < result_echo.size() && result_echo.compare(index, value.size(), value) == 0) {
            index += value.size();
            return true;
        }
        if (index < result_echo.size()) clear_echo();
        return false;
    }
    void characters(std::wstring_view value, bool from_ime, unsigned repeats) {
        while (repeats != 0 && is_echo(value, from_ime)) --repeats;
        if (repeats != 0) text_event(value, EventKind::text, repeats);
    }
    void character(wchar_t unit, bool from_ime, unsigned repeats) {
        if (unit >= 0xd800 && unit <= 0xdbff) {
            flush_surrogate();
            high_surrogate = unit;
            high_repeats = repeats;
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
            if (high_surrogate) {
                const wchar_t pair[]{high_surrogate, unit};
                const auto paired = std::min(high_repeats, repeats);
                high_repeats -= paired;
                repeats -= paired;
                if (high_repeats != 0) flush_surrogate();
                else high_surrogate = 0;
                characters({pair, 2}, from_ime, paired);
            }
            if (repeats != 0) text_event(L"\uFFFD", EventKind::text, repeats);
        } else {
            flush_surrogate();
            // Control keys travel through key events, never duplicate text.
            if (unit >= 0x20 && unit != 0x7f) characters({&unit, 1}, from_ime, repeats);
        }
    }
    // Mutex held. Capture is released by the window thread afterwards.
    void cancel(bool lost_keyboard_focus) {
        buttons = 0;
        release_requested = true;
        high_surrogate = 0;
        high_repeats = 0;
        clear_echo();
        composing = false;
        if (lost_keyboard_focus) modifiers.fill(false);
        append(InputEvent{EventKind::focus_lost});
    }
    // Mutex held. Maps client pixels into the target extent while one is set.
    bool map_target(POINT point, Vec2& position) {
        position = {static_cast<float>(point.x), static_cast<float>(point.y)};
        if (!target) return true;
        RECT client{};
        if (!GetClientRect(window, &client)) {
            fail(ErrorCode::backend_failure, "GetClientRect failed for adapter window");
            return false;
        }
        const LONG width = client.right - client.left;
        const LONG height = client.bottom - client.top;
        if (width <= 0 || height <= 0) return true;
        position.x = static_cast<float>(static_cast<double>(point.x) * target->width / width);
        position.y = static_cast<float>(static_cast<double>(point.y) * target->height / height);
        return true;
    }
    bool pointer_event(InputEvent& event, WPARAM wparam, POINT point) {
        const auto keys = GET_KEYSTATE_WPARAM(wparam);
        event.shift = (keys & MK_SHIFT) != 0;
        event.control = (keys & MK_CONTROL) != 0;
        event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        event.super = super();
        return map_target(point, event.position);
    }
    bool pointer(std::unique_lock<std::mutex>& lock, UINT message, WPARAM wparam, LPARAM lparam) {
        const bool move = message == WM_MOUSEMOVE;
        const bool up = button_release(message);
        PointerButton button = PointerButton::left;
        if (!move) {
            const auto pressed = message_button(message, wparam);
            if (!pressed) return false;
            button = *pressed;
        }
        InputEvent event{move ? EventKind::pointer_move : up ? EventKind::pointer_up : EventKind::pointer_down};
        event.button = button;
        if (!pointer_event(event, wparam, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) return true;
        append(std::move(event));
        if (failure) return true;
        if (up) {
            // Capture stays while any button is held; the last release frees it.
            buttons &= ~button_bit(button);
            if (buttons == 0) release_requested = true;
            return true;
        }
        if (!move) buttons |= button_bit(button);
        const HWND handle = window;
        lock.unlock();
        if (move) {
            if (!tracking) {
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, handle, 0};
                if (TrackMouseEvent(&track)) tracking = true;
            }
        } else if (GetCapture() != handle) {
            SetCapture(handle);
            if (GetCapture() != handle) {
                lock.lock();
                fail(ErrorCode::backend_failure, "SetCapture failed for the adapter window");
            }
        }
        return true;
    }
    bool key(UINT message, WPARAM wparam, LPARAM lparam) {
        if (wparam == 0 || wparam > 0xff) return false;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool system = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
        if (down) clear_echo();
        const WPARAM code = resolve_key(wparam, lparam);
        const int index = modifier_index(code);
        if (index >= 0) modifiers[static_cast<std::size_t>(index)] = down;
        // WM_SYSKEY* context code: Alt is down even if its own key-down was
        // never seen. An Alt key's own event uses the tracked state instead.
        const bool context_alt = system && (lparam & (LPARAM{1} << 29)) != 0 &&
            code != static_cast<WPARAM>(VK_LMENU) && code != static_cast<WPARAM>(VK_RMENU);
        const Key mapped = key_code(code);
        const unsigned count = down ? std::max<unsigned>(1, LOWORD(lparam)) : 1;
        for (unsigned i = 0; i < count && !failure; ++i) {
            InputEvent event{down ? EventKind::key_down : EventKind::key_up};
            event.key = mapped;
            event.native_key = static_cast<std::uint32_t>(code);
            event.shift = shift(); event.control = control();
            event.alt = alt() || context_alt; event.super = super();
            event.repeat = down && (i != 0 || (lparam & (LPARAM{1} << 30)) != 0);
            append(std::move(event));
        }
        return true;
    }
    bool composition(std::unique_lock<std::mutex>& lock, LPARAM flags) {
        const bool result_flag = (flags & GCS_RESULTSTR) != 0;
        const bool preedit_flag = (flags & GCS_COMPSTR) != 0;
        std::wstring result, preedit;
        if (result_flag || preedit_flag) {
            const HWND handle = window;
            const detail::ImeApi api = ime;
            lock.unlock();
            const auto read = [&](DWORD kind, std::wstring& output) {
                auto value = detail::composition_string(handle, kind, api);
                if (auto* error = std::get_if<Error>(&value)) {
                    lock.lock();
                    fail(std::move(*error));
                    return false;
                }
                output = std::get<std::wstring>(std::move(value));
                return true;
            };
            if (result_flag && !read(GCS_RESULTSTR, result)) return true;
            if (preedit_flag && !read(GCS_COMPSTR, preedit)) return true;
            lock.lock();
            if (failure) return true;
        }
        if (result_flag) {
            flush_surrogate();
            if (failure) return true;
            result_echo = std::move(result);
            char_echo = ime_echo = 0;
            text_event({}, EventKind::composition);
            if (!result_echo.empty()) text_event(result_echo);
            composing = false;
        }
        if (preedit_flag) {
            text_event(preedit, EventKind::composition);
            composing = true;
        } else if (flags == 0) {
            text_event({}, EventKind::composition);
            composing = false;
        }
        return true;
    }
    // Window thread only, mutex not held. Applies capture release, deferred
    // modifier synchronization and composition-window positioning.
    void apply_window_work() {
        HWND handle = nullptr;
        bool release = false, position = false;
        Vec2 logical{};
        std::optional<Target> mapping;
        float dpi = 1;
        {
            std::lock_guard lock(mutex);
            handle = window;
            if (sync_pending) {
                sync_pending = false;
                sync_modifiers();
            }
            // A press that arrived after the request owns capture again.
            release = release_requested && buttons == 0;
            release_requested = false;
            position = caret_pending && has_caret && !failure;
            caret_pending = false;
            logical = caret;
            mapping = target;
            dpi = scale;
        }
        if (release) release_capture(handle);
        if (!position || handle == nullptr) return;
        double scale_x = dpi, scale_y = dpi;
        if (mapping) {
            // Invert map_target: logical -> target pixels -> client pixels.
            scale_x = scale_y = mapping->scale;
            RECT client{};
            if (!GetClientRect(handle, &client)) {
                fail_now(Error{ErrorCode::backend_failure, "GetClientRect failed for adapter window"});
                return;
            }
            const LONG width = client.right - client.left;
            const LONG height = client.bottom - client.top;
            if (width > 0 && height > 0) {
                scale_x = static_cast<double>(mapping->scale) * width / mapping->width;
                scale_y = static_cast<double>(mapping->scale) * height / mapping->height;
            }
        }
        if (auto error = detail::position_composition(handle, logical, scale_x, scale_y))
            fail_now(std::move(*error));
    }
    bool translate(UINT message, WPARAM wparam, LPARAM lparam) {
        std::unique_lock lock(mutex);
        if (!live()) return false;
        switch (message) {
        case WM_DPICHANGED: {
            const auto dpi = LOWORD(wparam);
            if (dpi == 0 || HIWORD(wparam) != dpi) {
                fail(ErrorCode::invalid_argument, "Window DPI must be positive and uniform");
                return false;
            }
            scale = static_cast<float>(dpi) / 96.0f;
            if (has_caret) caret_pending = true;
            return false; // Application owns the suggested rectangle and resize.
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
        case WM_MOUSEMOVE:
            return pointer(lock, message, wparam, lparam);
        case WM_MOUSELEAVE: {
            tracking = false;
            // A captured press keeps receiving real positions; a leave here
            // must not become a drag to (-1, -1).
            if (buttons != 0) return false;
            InputEvent event{EventKind::pointer_move}; event.position = {-1, -1};
            append(std::move(event));
            return true;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (!ScreenToClient(window, &point)) {
                fail(ErrorCode::backend_failure, "ScreenToClient failed for wheel input");
                return true;
            }
            InputEvent event{EventKind::wheel};
            if (!pointer_event(event, wparam, point)) return true;
            const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            if (message == WM_MOUSEHWHEEL) event.wheel_x = delta;
            else event.wheel = delta;
            append(std::move(event));
            return true;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return key(message, wparam, lparam);
        case WM_CHAR:
        case WM_IME_CHAR: {
            const auto count = std::max<unsigned>(1, LOWORD(lparam));
            if (wparam > 0xffff) {
                flush_surrogate();
                text_event(L"\uFFFD", EventKind::text, count);
                return true;
            }
            character(static_cast<wchar_t>(wparam), message == WM_IME_CHAR, count);
            return true;
        }
        case WM_IME_STARTCOMPOSITION:
            clear_echo(); flush_surrogate(); composing = true;
            text_event({}, EventKind::composition);
            return true;
        case WM_IME_COMPOSITION:
            return composition(lock, lparam);
        case WM_IME_ENDCOMPOSITION:
            if (composing) text_event({}, EventKind::composition);
            composing = false;
            return true;
        case WM_SETFOCUS:
            sync_modifiers(); return false;
        case WM_KILLFOCUS:
            cancel(true); return true;
        case WM_CANCELMODE:
            cancel(false); return true;
        case WM_CAPTURECHANGED:
            if (releasing || buttons == 0 || reinterpret_cast<HWND>(lparam) == window) return false;
            cancel(false);
            return true;
        default:
            return false;
        }
    }
    bool process(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCDESTROY) {
            if (on_window_thread()) {
                HWND handle = nullptr;
                {
                    std::lock_guard lock(mutex);
                    handle = window;
                    buttons = 0;
                    release_requested = false;
                }
                release_capture(handle);
                std::lock_guard lock(mutex);
                window = nullptr;
                fail(ErrorCode::invalid_frame, "Adapter's borrowed HWND was destroyed");
            }
            return false;
        }
        if (!on_window_thread()) {
            std::lock_guard lock(mutex);
            fail(ErrorCode::invalid_frame, "Adapter::process must run on the window's thread");
            return false;
        }
        apply_window_work();
        const bool translated = translate(message, wparam, lparam);
        apply_window_work();
        return translated;
    }
    FrameInput take_frame(double dt_seconds) {
        std::lock_guard lock(mutex);
        FrameInput frame{{0, 0, -1, -1}, frame_scale(), dt_seconds};
        if (!live()) return frame;
        if (!std::isfinite(dt_seconds) || dt_seconds < 0) {
            fail(ErrorCode::invalid_argument, "Frame elapsed time must be finite and nonnegative");
            return frame;
        }
        if (target) {
            frame.viewport = {0, 0, static_cast<float>(target->width) / frame.scale,
                              static_cast<float>(target->height) / frame.scale};
        } else {
            RECT rectangle{};
            if (!GetClientRect(window, &rectangle)) {
                fail(ErrorCode::backend_failure, "GetClientRect failed for adapter window");
                return frame;
            }
            frame.viewport = {0, 0, static_cast<float>(rectangle.right - rectangle.left) / frame.scale,
                              static_cast<float>(rectangle.bottom - rectangle.top) / frame.scale};
        }
        frame.events.swap(events);
        queued_bytes = 0;
        for (auto& event : frame.events) {
            if (event.kind == EventKind::pointer_move || event.kind == EventKind::pointer_down ||
                event.kind == EventKind::pointer_up || event.kind == EventKind::wheel) {
                event.position.x /= frame.scale; event.position.y /= frame.scale;
            }
        }
        return frame;
    }
    void retarget(std::optional<Target> value) {
        {
            std::lock_guard lock(mutex);
            target = value;
            if (has_caret) caret_pending = true;
        }
        if (on_window_thread()) apply_window_work();
    }
};

Adapter::Adapter(HWND window) : impl_(std::make_unique<Impl>(window)) {}
Adapter::~Adapter() {
    // ReleaseCapture may synchronously re-enter the host WndProc. Keep the
    // complete Adapter and Impl alive until that callback has returned.
    // Capture belongs to the window thread; elsewhere it is left untouched.
    if (impl_->on_window_thread()) impl_->release_capture(impl_->current_window());
}
bool Adapter::process(UINT message, WPARAM wparam, LPARAM lparam) {
    try { return impl_->process(message, wparam, lparam); }
    catch (const std::bad_alloc&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
    catch (const std::length_error&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
    return true;
}
FrameInput Adapter::take_frame(double dt_seconds) {
    try { return impl_->take_frame(dt_seconds); }
    catch (const std::bad_alloc&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
    catch (const std::length_error&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
    std::lock_guard lock(impl_->mutex);
    return FrameInput{{0, 0, -1, -1}, impl_->frame_scale(), dt_seconds};
}
Status Adapter::set_target(std::uint32_t width, std::uint32_t height, float scale) {
    try {
        if (width == 0 || height == 0 || !std::isfinite(scale) || scale <= 0) {
            return Error{ErrorCode::invalid_argument,
                         "Adapter target requires a positive size and a finite positive scale"};
        }
        impl_->retarget(Impl::Target{width, height, scale});
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}
void Adapter::clear_target() {
    try { impl_->retarget(std::nullopt); }
    catch (const std::bad_alloc&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
    catch (const std::length_error&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
}
void Adapter::cancel_input() {
    try {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->cancel(true);
        }
        if (impl_->on_window_thread()) impl_->apply_window_work();
    } catch (const std::bad_alloc&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
      catch (const std::length_error&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
}
Status Adapter::set_clipboard(std::string_view utf8) {
    return detail::write_clipboard(impl_->current_window(), utf8);
}
Result<std::string> Adapter::clipboard() const {
    return detail::read_clipboard(impl_->current_window());
}
void Adapter::set_composition_position(Vec2 logical_position) {
    try {
        {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->live()) return;
            if (!std::isfinite(logical_position.x) || !std::isfinite(logical_position.y)) {
                impl_->fail(ErrorCode::invalid_argument, "Invalid composition position or scale");
                return;
            }
            impl_->caret = logical_position;
            impl_->has_caret = true;
            impl_->caret_pending = true;
        }
        if (impl_->on_window_thread()) impl_->apply_window_work();
    } catch (const std::bad_alloc&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
      catch (const std::length_error&) { impl_->fail_now(Error{ErrorCode::capacity, {}}); }
}
Status Adapter::status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->failure;
}
void detail::AdapterAccess::ime_api(Adapter& adapter, ImeApi api) {
    std::lock_guard lock(adapter.impl_->mutex);
    adapter.impl_->ime = api;
}
} // namespace wui::win32
