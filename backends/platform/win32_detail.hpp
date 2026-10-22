#pragma once

#include <wui/backends/win32.hpp>
#include <imm.h>
#include <string>
#include <string_view>

namespace wui::win32::detail {
inline constexpr std::size_t text_byte_limit = 16 * 1024 * 1024;
Result<std::wstring> to_utf16(std::string_view);
Result<std::string> to_utf8(std::wstring_view);
// Window scale and clipboard access accept a live Unicode HWND from any
// thread. IMM helpers require the calling thread to own the window.
Result<float> window_scale(HWND);
Status write_clipboard(HWND, std::string_view);
Result<std::string> read_clipboard(HWND);
// Client pixels are round(logical * scale) per axis.
Status position_composition(HWND, Vec2 logical_position, double scale_x, double scale_y);
// Narrow IMM boundary: production uses the real Win32 functions. Native tests
// can supply deterministic composition buffers without an installed language
// engine or changing the user's keyboard layout.
struct ImeApi {
    decltype(&ImmGetContext) context = &ImmGetContext;
    decltype(&ImmReleaseContext) release = &ImmReleaseContext;
    decltype(&ImmGetCompositionStringW) string = &ImmGetCompositionStringW;
};
Result<std::wstring> composition_string(HWND, DWORD kind, const ImeApi&);
struct AdapterAccess {
    static void ime_api(Adapter&, ImeApi);
};
} // namespace wui::win32::detail
