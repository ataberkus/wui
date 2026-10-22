#include "win32_detail.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui::win32::detail {
namespace {
static_assert(sizeof(wchar_t) == 2);

Error native_error(DWORD code, const char* message) {
    return {code == ERROR_NOT_ENOUGH_MEMORY || code == ERROR_OUTOFMEMORY
                ? ErrorCode::capacity : ErrorCode::backend_failure,
            message};
}

Status valid_window(HWND window, bool owner_thread) {
    if (!window || !IsWindow(window) || !IsWindowUnicode(window)) {
        return Error{ErrorCode::invalid_argument, "Expected a live Unicode window"};
    }
    if (owner_thread && GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) {
        return Error{ErrorCode::invalid_argument,
                     "Expected a live Unicode window owned by the calling thread"};
    }
    return std::nullopt;
}

Error conversion_error(const char* message) {
    const DWORD code = GetLastError();
    if (code == ERROR_NO_UNICODE_TRANSLATION) {
        return {ErrorCode::invalid_argument, message};
    }
    return native_error(code, message);
}

// Querying the strict conversion also validates UTF-16 without allocating an
// otherwise unused UTF-8 copy of an IMM buffer.
Result<int> utf8_size(std::wstring_view text) {
    if (text.size() > text_byte_limit / sizeof(wchar_t)) {
        return Error{ErrorCode::capacity, "UTF-16 input exceeds byte limit"};
    }
    if (text.empty()) return 0;
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count == 0) return conversion_error("Cannot convert UTF-16 to UTF-8");
    if (static_cast<std::size_t>(count) > text_byte_limit) {
        return Error{ErrorCode::capacity, "UTF-8 output exceeds byte limit"};
    }
    return count;
}

struct ClipboardScope {
    bool opened = false;
    ~ClipboardScope() { if (opened) CloseClipboard(); }
    bool close() {
        if (!CloseClipboard()) return false;
        opened = false;
        return true;
    }
};

struct GlobalMemory {
    HGLOBAL handle = nullptr;
    ~GlobalMemory() { if (handle) GlobalFree(handle); }
};

struct GlobalLockScope {
    HGLOBAL handle;
    void* data;
    explicit GlobalLockScope(HGLOBAL memory) : handle(memory), data(GlobalLock(memory)) {}
    ~GlobalLockScope() { if (data) GlobalUnlock(handle); }
    bool unlock() {
        SetLastError(ERROR_SUCCESS);
        const BOOL still_locked = GlobalUnlock(handle);
        if (!still_locked && GetLastError() != ERROR_SUCCESS) return false;
        // A nonzero result means other locks remain, not that this lock failed
        // to release. Never release someone else's lock reference.
        data = nullptr;
        return true;
    }
};

struct ImeContextScope {
    HWND window;
    HIMC context;
    decltype(&ImmReleaseContext) release;
    ~ImeContextScope() { if (context) release(window, context); }
    bool close() {
        if (!release(window, context)) return false;
        context = nullptr;
        return true;
    }
};
} // namespace

Result<std::wstring> to_utf16(std::string_view text) {
    try {
        if (text.size() > text_byte_limit) {
            return Error{ErrorCode::capacity, "UTF-8 input exceeds byte limit"};
        }
        if (text.empty()) return std::wstring{};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (count == 0) return conversion_error("Cannot convert UTF-8 to UTF-16");
        if (static_cast<std::size_t>(count) > text_byte_limit / sizeof(wchar_t)) {
            return Error{ErrorCode::capacity, "UTF-16 output exceeds byte limit"};
        }
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), result.data(), count);
        if (written == 0) return conversion_error("Cannot convert UTF-8 to UTF-16");
        if (written != count) {
            return Error{ErrorCode::backend_failure, "UTF-16 conversion length changed"};
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Result<std::string> to_utf8(std::wstring_view text) {
    try {
        auto size = utf8_size(text);
        if (auto* error = std::get_if<Error>(&size)) return std::move(*error);
        const int count = std::get<int>(size);
        if (count == 0) return std::string{};
        std::string result(static_cast<std::size_t>(count), '\0');
        const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
        if (written == 0) return conversion_error("Cannot convert UTF-16 to UTF-8");
        if (written != count) {
            return Error{ErrorCode::backend_failure, "UTF-8 conversion length changed"};
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Result<float> window_scale(HWND window) {
    try {
        if (auto error = valid_window(window, false)) return std::move(*error);
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return native_error(GetLastError(), "Cannot locate user32");
        using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
        const auto get_dpi = reinterpret_cast<GetDpiForWindowFunction>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi) {
            const UINT dpi = get_dpi(window);
            if (dpi == 0) return Error{ErrorCode::backend_failure, "GetDpiForWindow failed"};
            return static_cast<float>(dpi) / 96.0f;
        }
        // Older Windows 10 versions have no GetDpiForWindow. LOGPIXELSX of
        // the screen DC is system DPI, not a per-monitor approximation.
        const HDC dc = GetDC(nullptr);
        if (!dc) return native_error(GetLastError(), "Cannot acquire system DPI DC");
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        const int released = ReleaseDC(nullptr, dc);
        if (released == 0) return Error{ErrorCode::backend_failure, "Cannot release system DPI DC"};
        if (dpi <= 0) return Error{ErrorCode::backend_failure, "Cannot read system DPI"};
        return static_cast<float>(dpi) / 96.0f;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Status write_clipboard(HWND window, std::string_view text) {
    try {
        if (auto error = valid_window(window, false)) return error;
        if (text.size() > text_byte_limit) {
            return Error{ErrorCode::capacity, "Clipboard input exceeds byte limit"};
        }
        if (text.find('\0') != std::string_view::npos) {
            return Error{ErrorCode::invalid_argument, "Clipboard text contains an embedded NUL"};
        }
        auto converted = to_utf16(text);
        if (auto* error = std::get_if<Error>(&converted)) return std::move(*error);
        const auto& wide = std::get<std::wstring>(converted);
        if (wide.size() >= text_byte_limit / sizeof(wchar_t)) {
            return Error{ErrorCode::capacity, "Clipboard text and terminator exceed byte limit"};
        }
        const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        GlobalMemory memory{GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes)};
        if (!memory.handle) return native_error(GetLastError(), "Cannot allocate clipboard text");
        GlobalLockScope lock(memory.handle);
        if (!lock.data) return native_error(GetLastError(), "Cannot lock clipboard allocation");
        std::memcpy(lock.data, wide.c_str(), bytes);
        if (!lock.unlock()) return native_error(GetLastError(), "Cannot unlock clipboard allocation");

        ClipboardScope clipboard;
        if (!OpenClipboard(window)) return Error{ErrorCode::busy, "Cannot open clipboard"};
        clipboard.opened = true;
        if (!EmptyClipboard()) return native_error(GetLastError(), "Cannot empty clipboard");
        if (!SetClipboardData(CF_UNICODETEXT, memory.handle)) {
            return native_error(GetLastError(), "Cannot set clipboard text");
        }
        memory.handle = nullptr; // Only successful SetClipboardData transfers ownership.
        if (!clipboard.close()) return native_error(GetLastError(), "Cannot close clipboard");
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Result<std::string> read_clipboard(HWND window) {
    try {
        if (auto error = valid_window(window, false)) return std::move(*error);
        ClipboardScope clipboard;
        if (!OpenClipboard(window)) return Error{ErrorCode::busy, "Cannot open clipboard"};
        clipboard.opened = true;
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            return Error{ErrorCode::unsupported, "Clipboard has no Unicode text"};
        }
        const HGLOBAL memory = GetClipboardData(CF_UNICODETEXT);
        if (!memory) return native_error(GetLastError(), "Cannot read clipboard text");
        const SIZE_T bytes = GlobalSize(memory);
        if (bytes == 0) return native_error(GetLastError(), "Cannot size clipboard text");
        if (bytes > text_byte_limit) {
            return Error{ErrorCode::capacity, "Clipboard allocation exceeds byte limit"};
        }
        if (bytes % sizeof(wchar_t) != 0) {
            return Error{ErrorCode::invalid_argument, "Clipboard UTF-16 allocation has an odd byte count"};
        }
        GlobalLockScope lock(memory);
        if (!lock.data) return native_error(GetLastError(), "Cannot lock clipboard text");
        const auto* text = static_cast<const wchar_t*>(lock.data);
        const std::size_t bound = bytes / sizeof(wchar_t);
        std::size_t length = 0;
        while (length < bound && text[length] != L'\0') ++length;
        if (length == bound) {
            return Error{ErrorCode::invalid_argument, "Clipboard text has no bounded terminator"};
        }
        auto converted = to_utf8(std::wstring_view(text, length));
        if (auto* error = std::get_if<Error>(&converted)) return std::move(*error);
        if (!lock.unlock()) return native_error(GetLastError(), "Cannot unlock clipboard text");
        if (!clipboard.close()) return native_error(GetLastError(), "Cannot close clipboard");
        return converted;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Status position_composition(HWND window, Vec2 position, double scale_x, double scale_y) {
    try {
        if (auto error = valid_window(window, true)) return error;
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(scale_x) || scale_x <= 0 || !std::isfinite(scale_y) || scale_y <= 0) {
            return Error{ErrorCode::invalid_argument, "Invalid composition position or scale"};
        }
        const double x = std::round(static_cast<double>(position.x) * scale_x);
        const double y = std::round(static_cast<double>(position.y) * scale_y);
        constexpr double minimum = static_cast<double>(std::numeric_limits<LONG>::min());
        constexpr double maximum = static_cast<double>(std::numeric_limits<LONG>::max());
        if (x < minimum || x > maximum || y < minimum || y > maximum) {
            return Error{ErrorCode::invalid_argument, "Composition position exceeds native coordinate range"};
        }
        ImeContextScope context{window, ImmGetContext(window), &ImmReleaseContext};
        if (!context.context) return Error{ErrorCode::backend_failure, "Cannot acquire IME context"};
        COMPOSITIONFORM form{};
        form.dwStyle = CFS_POINT;
        form.ptCurrentPos = {static_cast<LONG>(x), static_cast<LONG>(y)};
        if (!ImmSetCompositionWindow(context.context, &form)) {
            return Error{ErrorCode::backend_failure, "Cannot position IME composition window"};
        }
        if (!context.close()) return Error{ErrorCode::backend_failure, "Cannot release IME context"};
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Result<std::wstring> composition_string(HWND window, DWORD kind, const ImeApi& api) {
    try {
        if (auto error = valid_window(window, true)) return std::move(*error);
        if (!api.context || !api.release || !api.string ||
            (kind != GCS_COMPSTR && kind != GCS_RESULTSTR &&
             kind != GCS_COMPREADSTR && kind != GCS_RESULTREADSTR)) {
            return Error{ErrorCode::invalid_argument, "Invalid IME string request"};
        }
        ImeContextScope context{window, api.context(window), api.release};
        if (!context.context) return Error{ErrorCode::backend_failure, "Cannot acquire IME context"};
        const LONG bytes = api.string(context.context, kind, nullptr, 0);
        if (bytes < 0) return Error{ErrorCode::backend_failure, "Cannot query IME string"};
        if (static_cast<std::size_t>(bytes) > text_byte_limit) {
            return Error{ErrorCode::capacity, "IME string exceeds byte limit"};
        }
        if (bytes % sizeof(wchar_t) != 0) {
            return Error{ErrorCode::invalid_argument, "IME UTF-16 string has an odd byte count"};
        }
        std::wstring result(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
        const LONG written = api.string(context.context, kind,
            bytes == 0 ? nullptr : result.data(), static_cast<DWORD>(bytes));
        if (written < 0) return Error{ErrorCode::backend_failure, "Cannot read IME string"};
        // A truncated read can return the supplied capacity. A fresh size query
        // catches growth too; never turn an unstable read into a partial commit.
        if (written != bytes || api.string(context.context, kind, nullptr, 0) != bytes) {
            return Error{ErrorCode::backend_failure, "IME string length changed while reading"};
        }
        auto validated = utf8_size(result);
        if (auto* error = std::get_if<Error>(&validated)) return std::move(*error);
        if (!context.close()) return Error{ErrorCode::backend_failure, "Cannot release IME context"};
        return result;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}
} // namespace wui::win32::detail
