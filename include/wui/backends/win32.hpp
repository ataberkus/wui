#pragma once

#ifndef _WIN32
#error "wui::win32 requires Windows"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wui/input.hpp>
#include <wui/text.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace wui::win32 {
namespace detail { struct AdapterAccess; }
// Borrowed Unicode HWND. The adapter never creates, subclasses or destroys the
// window and never calls DefWindowProc or the previous window procedure.
//
// Threads: process() runs on the window's owning thread, inside the host's
// WndProc or inside a hooked (subclassed) WndProc that the host installed with
// SetWindowLongPtrW/CallWindowProcW. The adapter may be constructed on any
// thread, and one other thread (typically the frame/render thread) may call
// take_frame, status, set_target, clear_target, cancel_input,
// set_composition_position and the clipboard members concurrently with
// process(); the queue is internally synchronized. Capture, mouse tracking and
// IMM positioning only ever happen on the window thread: work requested from
// another thread is applied by the next process() call.
//
// process() returns true when the message was translated into queued input
// (pointer, wheel, key, text, IME composition or a focus cancellation) and
// false when it was ignored or only observed (for example WM_DPICHANGED,
// WM_SETFOCUS, WM_SIZE, WM_SETCURSOR). true never obliges the caller to do
// anything: a standalone host returns 0 for consumed messages, while a hooked
// WndProc decides itself whether to swallow the message or also forward it to
// the previous procedure (for example from Context::statistics()).
//
// Represented input: left/right/middle/X1/X2 buttons (double clicks are
// presses), vertical and horizontal wheel, Key plus left/right modifier keys,
// WM_SYSKEY* (Alt and F10 combinations), every virtual-key code through
// InputEvent::native_key, UTF-16 text and IME composition.
//
// Coordinates: positions stay in client pixels until take_frame divides every
// pointer position and the current extent by one common, latest DPI scale.
// set_target replaces that space for hosts whose swap chain size differs from
// the client rectangle: client pixels are mapped proportionally into the
// target, and take_frame reports the target extent divided by its scale.
// WM_DPICHANGED is observed but returns false: the HOST applies its suggested
// RECT. Opt into per-monitor DPI awareness before creating the HWND; on older
// Windows 10, scale falls back to system LOGPIXELSX if GetDpiForWindow is absent.
// Queue ceiling: 65,536 events / 16 MiB including text payload. High UTF-16
// surrogates and repeat counts may span frames; repeats apply to complete
// scalars, and malformed sequences display U+FFFD. IME result echoes remain
// suppressed until the next fresh key-down/composition/focus loss.
class Adapter : public IClipboard {
public:
    explicit Adapter(HWND window);
    ~Adapter() override;
    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;
    bool process(UINT message, WPARAM wparam, LPARAM lparam);
    FrameInput take_frame(double dt_seconds);
    // Width and height must be positive; scale must be finite and positive.
    Status set_target(std::uint32_t width, std::uint32_t height, float scale);
    void clear_target();
    // Queues a focus_lost cancellation and forgets held buttons, modifiers and
    // partial text, as when the host stops routing input to the UI (for
    // example when an overlay is hidden). Capture is released by the window
    // thread on its next process() call.
    void cancel_input();
    Status set_clipboard(std::string_view utf8) override;
    Result<std::string> clipboard() const override;
    void set_composition_position(Vec2 logical_position);
    // Native/queue failures latch here; failed adapters produce invalid frame
    // extents instead of a partial valid input frame. Recreate to recover.
    // Clipboard failures are operation-local and returned directly above.
    Status status() const;
private:
    friend struct detail::AdapterAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace wui::win32
