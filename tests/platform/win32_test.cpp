#include <wui/backends/win32.hpp>
#include <doctest/doctest.h>

#include "win32_test_support.hpp"
#include "../../backends/platform/win32_detail.hpp"
#include "../support/widget_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using wui::win32::Adapter;
using wui::win32::test::HostWindow;
using wui::win32::test::WindowBinding;
using wui::win32::test::run_off_thread;

std::string text(const wui::FrameInput& frame) {
    std::string result;
    for (const auto& event : frame.events) {
        if (event.kind == wui::EventKind::text) result += event.text;
    }
    return result;
}

std::vector<std::string> compositions(const wui::FrameInput& frame) {
    std::vector<std::string> result;
    for (const auto& event : frame.events) {
        if (event.kind == wui::EventKind::composition) result.push_back(event.text);
    }
    return result;
}

std::size_t count(const wui::FrameInput& frame, wui::EventKind kind) {
    return static_cast<std::size_t>(std::count_if(frame.events.begin(), frame.events.end(),
        [kind](const auto& event) { return event.kind == kind; }));
}

bool modifier_key(wui::Key key) {
    return key >= wui::Key::left_shift && key <= wui::Key::right_super;
}

struct ImeReader {
    HWND window = nullptr;
    std::wstring composition;
    std::wstring result;
    bool fail = false;
    bool odd_size = false;
    inline static thread_local ImeReader* active = nullptr;

    static HIMC WINAPI context(HWND window) {
        return active != nullptr && window == active->window
            ? reinterpret_cast<HIMC>(active) : nullptr;
    }
    static BOOL WINAPI release(HWND window, HIMC context) {
        return active != nullptr && window == active->window &&
            context == reinterpret_cast<HIMC>(active);
    }
    static LONG WINAPI string(HIMC context, DWORD kind, LPVOID buffer, DWORD capacity) {
        if (active == nullptr || context != reinterpret_cast<HIMC>(active) || active->fail)
            return IMM_ERROR_GENERAL;
        if (active->odd_size) return 1;
        const auto& value = kind == GCS_RESULTSTR ? active->result : active->composition;
        const auto bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        if (buffer != nullptr) {
            if (capacity < bytes) return IMM_ERROR_GENERAL;
            if (bytes != 0) std::memcpy(buffer, value.data(), bytes);
        }
        return static_cast<LONG>(bytes);
    }
};

class ImeBinding {
public:
    ImeBinding(Adapter& adapter, ImeReader& reader) : adapter_(adapter), previous_(ImeReader::active) {
        ImeReader::active = &reader;
        wui::win32::detail::AdapterAccess::ime_api(adapter_,
            {&ImeReader::context, &ImeReader::release, &ImeReader::string});
    }
    ~ImeBinding() {
        wui::win32::detail::AdapterAccess::ime_api(adapter_, {});
        ImeReader::active = previous_;
    }
private:
    Adapter& adapter_;
    ImeReader* previous_;
};

class NativeImeContext {
public:
    explicit NativeImeContext(HWND window) : context(ImmCreateContext()), window_(window) {
        if (context != nullptr) previous_ = ImmAssociateContext(window_, context);
    }
    ~NativeImeContext() {
        if (context != nullptr) {
            ImmAssociateContext(window_, previous_);
            ImmDestroyContext(context);
        }
    }
    HIMC context = nullptr;
private:
    HWND window_;
    HIMC previous_ = nullptr;
};

// A host-installed subclass, as an overlay hook would install it: the hook
// feeds process() and decides itself whether the previous procedure also
// receives the message.
struct Hook {
    Adapter* adapter = nullptr;
    WNDPROC previous = nullptr;
    bool swallow_translated = false;
    UINT watched = 0;
    bool watched_translated = false;
    inline static thread_local Hook* active = nullptr;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Hook* hook = active;
        if (hook == nullptr || hook->adapter == nullptr || hook->previous == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);
        const bool translated = hook->adapter->process(message, wparam, lparam);
        if (message == hook->watched) hook->watched_translated = translated;
        if (translated && hook->swallow_translated) return 0;
        return CallWindowProcW(hook->previous, window, message, wparam, lparam);
    }
};

class Subclass {
public:
    Subclass(HWND window, Hook& hook) : window_(window) {
        Hook::active = &hook;
        previous_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window_, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&Hook::procedure)));
        hook.previous = previous_;
    }
    ~Subclass() {
        if (previous_ != nullptr)
            SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_));
        Hook::active = nullptr;
    }
    Subclass(const Subclass&) = delete;
    Subclass& operator=(const Subclass&) = delete;
private:
    HWND window_;
    WNDPROC previous_ = nullptr;
};
} // namespace

TEST_CASE("Win32 adapter preserves mouse edges and complete UTF16 scalars") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    REQUIRE(IsWindowUnicode(host.get()));
    CHECK_FALSE(IsWindowVisible(host.get()));
    const auto original_proc = GetWindowLongPtrW(host.get(), GWLP_WNDPROC);
    {
        Adapter adapter(host.get());
        WindowBinding binding(host, adapter);
        REQUIRE_FALSE(adapter.status().has_value());
        host.send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 30));
        CHECK(GetCapture() == host.get());
        host.send(WM_LBUTTONUP, 0, MAKELPARAM(30, 30));
        CHECK(GetCapture() != host.get());
        CHECK(host.capture_changes == 1);
        auto frame = adapter.take_frame(0.016);
        REQUIRE(frame.events.size() == 2);
        CHECK(frame.events[0].kind == wui::EventKind::pointer_down);
        CHECK(frame.events[1].kind == wui::EventKind::pointer_up);
        CHECK(adapter.take_frame(0.016).events.empty());
        host.send(WM_CHAR, 0xD83D, 1);
        host.send(WM_CHAR, 0xDE42, 1);
        frame = adapter.take_frame(0.016);
        REQUIRE(frame.events.size() == 1);
        CHECK(frame.events[0].text == "\xF0\x9F\x99\x82");
    }
    CHECK(IsWindow(host.get()));
    CHECK(GetWindowLongPtrW(host.get(), GWLP_WNDPROC) == original_proc);
}

TEST_CASE("Win32 signed client and wheel screen coordinates share frame scale") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    POINT wheel{-14, -9};
    REQUIRE(ClientToScreen(host.get(), &wheel));
    host.send(WM_MOUSEMOVE, MK_SHIFT | MK_CONTROL, MAKELPARAM(-37, -19));
    host.send(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
        MAKELPARAM(wheel.x, wheel.y));
    const auto frame = adapter.take_frame(0.125);
    REQUIRE_FALSE(adapter.status().has_value());
    REQUIRE(frame.events.size() == 2);
    CHECK(frame.dt_seconds == 0.125);
    CHECK(frame.events[0].kind == wui::EventKind::pointer_move);
    CHECK(frame.events[0].position.x == doctest::Approx(-37.0f / frame.scale));
    CHECK(frame.events[0].position.y == doctest::Approx(-19.0f / frame.scale));
    CHECK(frame.events[0].shift);
    CHECK(frame.events[0].control);
    CHECK(frame.events[1].kind == wui::EventKind::wheel);
    CHECK(frame.events[1].position.x == doctest::Approx(-14.0f / frame.scale));
    CHECK(frame.events[1].position.y == doctest::Approx(-9.0f / frame.scale));
    CHECK(frame.events[1].wheel == -1.0f);
}

TEST_CASE("Win32 pending positions use latest DPI once and resize stays host owned") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    RECT before{};
    REQUIRE(GetWindowRect(host.get(), &before));
    RECT suggested{31, 43, 631, 443};
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(60, 90));
    host.send(WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
    host.send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(300, 210));
    host.send(WM_DPICHANGED, MAKEWPARAM(192, 192), reinterpret_cast<LPARAM>(&suggested));
    host.send(WM_LBUTTONUP, 0, MAKELPARAM(300, 210));
    RECT after{};
    REQUIRE(GetWindowRect(host.get(), &after));
    CHECK(EqualRect(&before, &after));
    CHECK(host.unconsumed_dpi_changes == 2);
    RECT client{};
    REQUIRE(GetClientRect(host.get(), &client));
    auto frame = adapter.take_frame(0.016);
    REQUIRE(frame.events.size() == 3);
    CHECK(frame.scale == 2.0f);
    CHECK(frame.events[0].position.x == 30.0f);
    CHECK(frame.events[0].position.y == 45.0f);
    CHECK(frame.events[1].position.x == 150.0f);
    CHECK(frame.events[1].position.y == 105.0f);
    CHECK(frame.events[2].position.x == 150.0f);
    CHECK(frame.viewport.w == doctest::Approx((client.right - client.left) / 2.0f));
    CHECK(frame.viewport.h == doctest::Approx((client.bottom - client.top) / 2.0f));

    host.apply_suggested_rect = true;
    host.send(WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
    REQUIRE(host.applied_suggested_rect);
    REQUIRE(GetWindowRect(host.get(), &after));
    CHECK(EqualRect(&suggested, &after));
    REQUIRE(GetClientRect(host.get(), &client));
    // A stale/truncated WM_SIZE payload must not replace the real client extent.
    host.send(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1, 2));
    frame = adapter.take_frame(0.016);
    CHECK(frame.scale == 1.5f);
    CHECK(frame.viewport.w == doctest::Approx((client.right - client.left) / 1.5f));
    CHECK(frame.viewport.h == doctest::Approx((client.bottom - client.top) / 1.5f));
    CHECK(frame.events.empty());
}

TEST_CASE("Win32 key repeat counts and message modifiers retain ordered transitions") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_KEYDOWN, VK_SHIFT, 1);
    host.send(WM_KEYDOWN, VK_CONTROL, 1);
    host.send(WM_KEYDOWN, 'C', 1);
    host.send(WM_KEYUP, 'C', static_cast<LPARAM>(0xC0000001u));
    host.send(WM_KEYUP, VK_CONTROL, static_cast<LPARAM>(0xC0000001u));
    host.send(WM_KEYDOWN, VK_TAB, 3);
    host.send(WM_KEYUP, VK_SHIFT, static_cast<LPARAM>(0xC0000001u));
    host.send(WM_KEYDOWN, VK_SPACE, static_cast<LPARAM>(0x40000001u));
    host.send(WM_KEYUP, VK_SPACE, static_cast<LPARAM>(0xC0000001u));
    const auto frame = adapter.take_frame(0.016);
    std::vector<wui::InputEvent> keys;
    std::vector<wui::InputEvent> modifiers;
    for (const auto& event : frame.events) {
        if (modifier_key(event.key)) modifiers.push_back(event);
        else if (event.key != wui::Key::none) keys.push_back(event);
    }
    // Modifier transitions are queued key events of their own.
    REQUIRE(modifiers.size() == 4);
    CHECK(modifiers[0].key == wui::Key::left_shift);
    CHECK(modifiers[0].kind == wui::EventKind::key_down);
    CHECK(modifiers[1].key == wui::Key::left_control);
    CHECK(modifiers[2].key == wui::Key::left_control);
    CHECK(modifiers[2].kind == wui::EventKind::key_up);
    CHECK(modifiers[3].key == wui::Key::left_shift);
    CHECK(modifiers[3].kind == wui::EventKind::key_up);
    REQUIRE(keys.size() == 7);
    CHECK(keys[0].key == wui::Key::c);
    CHECK(keys[0].kind == wui::EventKind::key_down);
    CHECK(keys[0].shift);
    CHECK(keys[0].control);
    CHECK_FALSE(keys[0].repeat);
    CHECK(keys[1].kind == wui::EventKind::key_up);
    CHECK_FALSE(keys[1].repeat);
    for (std::size_t index = 2; index < 5; ++index) {
        CHECK(keys[index].key == wui::Key::tab);
        CHECK(keys[index].shift);
        CHECK_FALSE(keys[index].control);
        CHECK(keys[index].repeat == (index != 2));
    }
    CHECK(keys[5].key == wui::Key::space);
    CHECK(keys[5].repeat);
    CHECK_FALSE(keys[5].shift);
    CHECK_FALSE(keys[5].control);
    CHECK(keys[6].kind == wui::EventKind::key_up);
    CHECK_FALSE(keys[6].repeat);
}

TEST_CASE("Win32 up and down arrow messages route as ordered navigation keys") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_KEYDOWN, VK_UP, 1);
    host.send(WM_KEYUP, VK_UP, 1);
    host.send(WM_KEYDOWN, VK_DOWN, 1);
    host.send(WM_KEYUP, VK_DOWN, 1);
    const auto frame = adapter.take_frame(0.016);
    REQUIRE(frame.events.size() == 4);
    CHECK(frame.events[0].key == wui::Key::up);
    CHECK(frame.events[0].kind == wui::EventKind::key_down);
    CHECK(frame.events[1].key == wui::Key::up);
    CHECK(frame.events[1].kind == wui::EventKind::key_up);
    CHECK(frame.events[2].key == wui::Key::down);
    CHECK(frame.events[2].kind == wui::EventKind::key_down);
    CHECK(frame.events[3].key == wui::Key::down);
    CHECK(frame.events[3].kind == wui::EventKind::key_up);
}

TEST_CASE("Win32 loss of focus or capture cancels an active press") {
    HostWindow host;
    HostWindow other;
    REQUIRE(host.get() != nullptr);
    REQUIRE(other.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
    REQUIRE(GetCapture() == host.get());
    SUBCASE("focus loss releases capture without duplicate cancellation") {
        host.send(WM_KILLFOCUS);
        CHECK(GetCapture() != host.get());
    }
    SUBCASE("capture transfers synchronously to another owned hidden window") {
        SetCapture(other.get());
        REQUIRE(GetCapture() == other.get());
        ReleaseCapture();
    }
    auto frame = adapter.take_frame(0.016);
    REQUIRE(frame.events.size() == 2);
    CHECK(frame.events[0].kind == wui::EventKind::pointer_down);
    CHECK(frame.events[1].kind == wui::EventKind::focus_lost);
    host.send(WM_LBUTTONUP, 0, MAKELPARAM(20, 20));
    frame = adapter.take_frame(0.016);
    CHECK(count(frame, wui::EventKind::focus_lost) == 0);
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 text preserves incomplete pairs across drains and replaces malformed UTF16") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_CHAR, 0xD83D, 1);
    CHECK(adapter.take_frame(0.016).events.empty());
    host.send(WM_CHAR, 0xDE42, 1);
    CHECK(text(adapter.take_frame(0.016)) == "\xF0\x9F\x99\x82");
    host.send(WM_CHAR, 0xDE42, 1);
    host.send(WM_CHAR, 0xD83D, 1);
    host.send(WM_CHAR, 'A', 1);
    host.send(WM_CHAR, 0xD83D, 1);
    host.send(WM_CHAR, 0xD83D, 1);
    host.send(WM_CHAR, 0xDE42, 1);
    CHECK(text(adapter.take_frame(0.016)) ==
        "\xEF\xBF\xBD\xEF\xBF\xBD" "A" "\xEF\xBF\xBD\xF0\x9F\x99\x82");
    host.send(WM_CHAR, 'x', 3);
    CHECK(text(adapter.take_frame(0.016)) == "xxx");
    host.send(WM_CHAR, VK_TAB, 1);
    host.send(WM_CHAR, VK_RETURN, 1);
    host.send(WM_CHAR, 0x7F, 1);
    CHECK(text(adapter.take_frame(0.016)).empty());
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 focus loss drops incomplete UTF16 and modifier state") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_KEYDOWN, VK_SHIFT, 1);
    host.send(WM_KEYDOWN, VK_CONTROL, 1);
    host.send(WM_CHAR, 0xD83D, 1);
    adapter.take_frame(0.016);
    host.send(WM_KILLFOCUS);
    CHECK(count(adapter.take_frame(0.016), wui::EventKind::focus_lost) == 1);
    host.send(WM_CHAR, 0xDE42, 1);
    host.send(WM_KEYDOWN, 'A', 1);
    const auto frame = adapter.take_frame(0.016);
    CHECK(text(frame) == "\xEF\xBF\xBD");
    const auto key = std::find_if(frame.events.begin(), frame.events.end(),
        [](const auto& event) { return event.key == wui::Key::a; });
    REQUIRE(key != frame.events.end());
    CHECK_FALSE(key->shift);
    CHECK_FALSE(key->control);
}

TEST_CASE("Win32 IME updates clear on empty buffers end and cancellation") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    ImeReader reader{host.get(), L"preedit \xD83D\xDE42"};
    ImeBinding ime(adapter, reader);
    host.send(WM_IME_STARTCOMPOSITION);
    adapter.take_frame(0.016);
    host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    auto updates = compositions(adapter.take_frame(0.016));
    REQUIRE(updates.size() == 1);
    CHECK(updates[0] == "preedit \xF0\x9F\x99\x82");
    SUBCASE("empty composition buffer") {
        reader.composition.clear();
        host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    }
    SUBCASE("IME end") { host.send(WM_IME_ENDCOMPOSITION); }
    SUBCASE("composition cancellation has no GCS flags") { host.send(WM_IME_COMPOSITION); }
    updates = compositions(adapter.take_frame(0.016));
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].empty());
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 IME commits once across end and both echo channels") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    ImeReader reader{host.get(), L"preedit", L"\xD83D\xDE42\x6F22"};
    ImeBinding ime(adapter, reader);
    host.send(WM_IME_STARTCOMPOSITION);
    host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    adapter.take_frame(0.016);
    host.send(WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    auto frame = adapter.take_frame(0.016);
    CHECK(text(frame) == "\xF0\x9F\x99\x82\xE6\xBC\xA2");
    const auto updates = compositions(frame);
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].empty());
    host.send(WM_IME_ENDCOMPOSITION);
    for (const wchar_t unit : reader.result) host.send(WM_IME_CHAR, unit, 1);
    CHECK(text(adapter.take_frame(0.016)).empty());
    for (const wchar_t unit : reader.result) host.send(WM_CHAR, unit, 1);
    CHECK(text(adapter.take_frame(0.016)).empty());
    host.send(WM_KEYDOWN, 'A', 1);
    for (const wchar_t unit : reader.result) host.send(WM_CHAR, unit, 1);
    CHECK(text(adapter.take_frame(0.016)) == "\xF0\x9F\x99\x82\xE6\xBC\xA2");
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 new composition and focus loss discard stale IME echoes") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    ImeReader reader{host.get(), L"preedit", L"q"};
    ImeBinding ime(adapter, reader);
    host.send(WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    CHECK(text(adapter.take_frame(0.016)) == "q");
    SUBCASE("fresh composition resets echoes") { host.send(WM_IME_STARTCOMPOSITION); }
    SUBCASE("focus loss clears composition and echoes") {
        host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
        host.send(WM_KILLFOCUS);
        CHECK(count(adapter.take_frame(0.016), wui::EventKind::focus_lost) == 1);
    }
    host.send(WM_CHAR, 'q', 1);
    CHECK(text(adapter.take_frame(0.016)) == "q");
}

TEST_CASE("Win32 native IME reader failures latch and prevent partial valid frames") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    ImeReader reader{host.get(), L"preedit"};
    ImeBinding ime(adapter, reader);
    SUBCASE("IMM reports a failure") { reader.fail = true; }
    SUBCASE("IMM reports a partial UTF16 unit") { reader.odd_size = true; }
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(10, 20));
    host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    REQUIRE(adapter.status().has_value());
    const auto error = adapter.status()->code;
    auto frame = adapter.take_frame(0.016);
    CHECK(frame.viewport.w < 0);
    CHECK(frame.viewport.h < 0);
    CHECK(frame.events.empty());
    reader.fail = false;
    reader.odd_size = false;
    host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    frame = adapter.take_frame(0.016);
    REQUIRE(adapter.status().has_value());
    CHECK(adapter.status()->code == error);
    CHECK(frame.viewport.w < 0);
    CHECK(frame.events.empty());
}

TEST_CASE("Win32 composition position uses native IMM and physical client pixels") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    NativeImeContext ime(host.get());
    REQUIRE_MESSAGE(ime.context != nullptr, "ImmCreateContext unavailable; native IMM is required");
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    RECT suggested{0, 0, 420, 300};
    host.send(WM_DPICHANGED, MAKEWPARAM(192, 192), reinterpret_cast<LPARAM>(&suggested));
    adapter.set_composition_position({12.5f, 23.5f});
    REQUIRE_FALSE(adapter.status().has_value());
    COMPOSITIONFORM composition{};
    REQUIRE_MESSAGE(ImmGetCompositionWindow(ime.context, &composition),
        "Native IMM composition window query unavailable; no keyboard layout is changed");
    CHECK(composition.dwStyle == CFS_POINT);
    CHECK(composition.ptCurrentPos.x == 25);
    CHECK(composition.ptCurrentPos.y == 47);
}

TEST_CASE("Win32 destroyed borrowed HWND fails closed and keeps errors sticky") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(10, 20));
    host.destroy();
    auto frame = adapter.take_frame(0.016);
    REQUIRE(adapter.status().has_value());
    CHECK(frame.viewport.w < 0);
    CHECK(frame.viewport.h < 0);
    CHECK(frame.events.empty());
    CHECK_FALSE(adapter.process(WM_CHAR, 'x', 1));
    frame = adapter.take_frame(0.016);
    CHECK(frame.viewport.w < 0);
    CHECK(frame.events.empty());
    CHECK(adapter.status().has_value());
}

TEST_CASE("Win32 adapter clipboard service and composition caret stay scale-correct") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    wui::IClipboard& service = adapter;
    REQUIRE_FALSE(service.set_clipboard("clip \xC5\x9F offline").has_value());
    const auto stored = service.clipboard();
    REQUIRE(std::holds_alternative<std::string>(stored));
    CHECK(std::get<std::string>(stored) == "clip \xC5\x9F offline");
    RECT suggested{0, 0, 400, 300};
    host.send(WM_DPICHANGED, MAKEWPARAM(160, 160), reinterpret_cast<LPARAM>(&suggested));
    adapter.set_composition_position({10.0f, 20.0f});
    REQUIRE_FALSE(adapter.status().has_value());
    suggested = {0, 0, 200, 150};
    host.send(WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&suggested));
    adapter.set_composition_position({10.0f, 20.0f});
    REQUIRE_FALSE(adapter.status().has_value());
}
TEST_CASE("Win32 null HWND is rejected without creating a replacement window") {
    Adapter adapter(nullptr);
    REQUIRE(adapter.status().has_value());
    const auto frame = adapter.take_frame(0.016);
    CHECK(frame.viewport.w < 0);
    CHECK(frame.viewport.h < 0);
    CHECK(frame.events.empty());
    CHECK_FALSE(adapter.process(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 2)));
}

TEST_CASE("Win32 capture cancellation preserves still held keyboard modifiers") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_KEYDOWN, VK_SHIFT, 1);
    host.send(WM_KEYDOWN, VK_CONTROL, 1);
    host.send(WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT | MK_CONTROL, MAKELPARAM(12, 12));
    adapter.take_frame(0.016);
    SUBCASE("capture moved") { host.send(WM_CAPTURECHANGED); }
    SUBCASE("modal cancellation") { host.send(WM_CANCELMODE); }
    host.send(WM_KEYDOWN, 'A', 1);
    auto frame = adapter.take_frame(0.016);
    CHECK(count(frame, wui::EventKind::focus_lost) == 1);
    const auto key = std::find_if(frame.events.begin(), frame.events.end(),
        [](const auto& event) { return event.key == wui::Key::a; });
    REQUIRE(key != frame.events.end());
    CHECK(key->shift);
    CHECK(key->control);
}

TEST_CASE("Win32 repeated surrogate messages repeat complete scalars") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_CHAR, 0xD83D, 2);
    CHECK(adapter.take_frame(0.016).events.empty());
    host.send(WM_CHAR, 0xDE42, 2);
    CHECK(text(adapter.take_frame(0.016)) == "\xF0\x9F\x99\x82\xF0\x9F\x99\x82");
    ImeReader reader{host.get(), {}, L"\xD83D\xDE42\xD83D\xDE42"};
    ImeBinding ime(adapter, reader);
    host.send(WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    CHECK(text(adapter.take_frame(0.016)) == "\xF0\x9F\x99\x82\xF0\x9F\x99\x82");
    for (const auto message : {WM_IME_CHAR, WM_CHAR}) {
        host.send(message, 0xD83D, 2);
        host.send(message, 0xDE42, 2);
        CHECK(text(adapter.take_frame(0.016)).empty());
    }
}

TEST_CASE("Win32 focus regain resynchronizes modifiers without synthetic OS input") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    const bool shift = ((GetKeyState(VK_LSHIFT) | GetKeyState(VK_RSHIFT)) & 0x8000) != 0;
    const bool control = ((GetKeyState(VK_LCONTROL) | GetKeyState(VK_RCONTROL)) & 0x8000) != 0;
    for (const auto key : {VK_LSHIFT, VK_RSHIFT})
        host.send(shift ? WM_KEYUP : WM_KEYDOWN, key, 1);
    for (const auto key : {VK_LCONTROL, VK_RCONTROL})
        host.send(control ? WM_KEYUP : WM_KEYDOWN, key, 1);
    host.send(WM_SETFOCUS);
    host.send(WM_KEYDOWN, 'A', 1);
    const auto frame = adapter.take_frame(0.016);
    // The four synthetic modifier transitions are queued before the letter.
    REQUIRE(frame.events.size() == 5);
    const auto& key = frame.events.back();
    CHECK(key.key == wui::Key::a);
    CHECK(key.shift == shift);
    CHECK(key.control == control);
}

TEST_CASE("Win32 IME preedit, result and cancellation drive a focused text input") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    ImeReader reader{host.get(), L"\x0131", L"\x011F"};
    ImeBinding ime(adapter, reader);
    auto context = wui::test::make_context();
    std::string name = "a";
    auto build = [&] {
        auto frame = wui::test::begin(*context, adapter.take_frame(0.016));
        frame.text_input("name", "Name", name);
        return wui::test::finish(frame);
    };
    auto packet = build();
    const auto bounds = wui::test::semantic(packet, wui::Role::text_input, "Name").bounds;
    const auto x = static_cast<int>((bounds.x + bounds.w * 0.5f) * packet->scale);
    const auto y = static_cast<int>((bounds.y + bounds.h * 0.5f) * packet->scale);
    host.send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    host.send(WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    packet = build();
    REQUIRE(wui::test::semantic(packet, wui::Role::text_input, "Name").focused);

    // Preedit is shown to the IME at the caret but never committed to the model.
    host.send(WM_IME_STARTCOMPOSITION);
    host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    packet = build();
    CHECK(name == "a");
    CHECK(context->composition_caret().has_value());

    host.send(WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    host.send(WM_IME_ENDCOMPOSITION);
    packet = build();
    CHECK(name == "a\xC4\x9F");
    CHECK(wui::test::semantic(packet, wui::Role::text_input, "Name").value == "a\xC4\x9F");

    // Cancellation (no GCS flags) discards the preedit without touching the model.
    host.send(WM_IME_STARTCOMPOSITION);
    host.send(WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    packet = build();
    host.send(WM_IME_COMPOSITION);
    host.send(WM_IME_ENDCOMPOSITION);
    packet = build();
    CHECK(name == "a\xC4\x9F");
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 mouse leave during a captured press does not move the pointer") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 30));
    host.send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(500, 30));
    host.send(WM_MOUSELEAVE);
    auto frame = adapter.take_frame(0.016);
    REQUIRE(frame.events.size() == 2);
    CHECK(frame.events[1].kind == wui::EventKind::pointer_move);
    CHECK(frame.events[1].position.x == 500.0f);
    host.send(WM_LBUTTONUP, 0, MAKELPARAM(500, 30));
    host.send(WM_MOUSELEAVE);
    frame = adapter.take_frame(0.016);
    REQUIRE(frame.events.size() == 2);
    CHECK(frame.events[1].kind == wui::EventKind::pointer_move);
    CHECK(frame.events[1].position.x == -1.0f);
}

TEST_CASE("Win32 right middle and X buttons queue pointer edges with their button") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    struct Press {
        UINT down;
        UINT up;
        WPARAM down_wparam;
        WPARAM up_wparam;
        wui::PointerButton button;
    };
    const Press presses[]{
        {WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON | MK_SHIFT, MK_SHIFT, wui::PointerButton::right},
        {WM_MBUTTONDBLCLK, WM_MBUTTONUP, MK_MBUTTON | MK_CONTROL, 0, wui::PointerButton::middle},
        {WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), MAKEWPARAM(0, XBUTTON1),
         wui::PointerButton::x1},
        {WM_XBUTTONDBLCLK, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), MAKEWPARAM(0, XBUTTON2),
         wui::PointerButton::x2},
    };
    int x = 30;
    for (const auto& press : presses) {
        INFO("button message ", press.down);
        CHECK(adapter.process(press.down, press.down_wparam, MAKELPARAM(x, 40)));
        CHECK(GetCapture() == host.get());
        CHECK(adapter.process(press.up, press.up_wparam, MAKELPARAM(x + 5, 45)));
        CHECK(GetCapture() != host.get());
        x += 10;
    }
    // An X button message that names no X button is not input.
    CHECK_FALSE(adapter.process(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, 0), MAKELPARAM(1, 1)));
    CHECK(GetCapture() != host.get());
    const auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    REQUIRE(frame.events.size() == 2 * std::size(presses));
    for (std::size_t index = 0; index < std::size(presses); ++index) {
        INFO("press ", index);
        const auto& down = frame.events[2 * index];
        const auto& up = frame.events[2 * index + 1];
        const float left = 30.0f + 10.0f * static_cast<float>(index);
        CHECK(down.kind == wui::EventKind::pointer_down);
        CHECK(up.kind == wui::EventKind::pointer_up);
        CHECK(down.button == presses[index].button);
        CHECK(up.button == presses[index].button);
        CHECK(down.position.x == doctest::Approx(left / frame.scale));
        CHECK(down.position.y == doctest::Approx(40.0f / frame.scale));
        CHECK(up.position.x == doctest::Approx((left + 5.0f) / frame.scale));
        CHECK(up.position.y == doctest::Approx(45.0f / frame.scale));
    }
    CHECK(frame.events[0].shift);
    CHECK_FALSE(frame.events[0].control);
    CHECK(frame.events[2].control);
    CHECK_FALSE(frame.events[4].shift);
}

TEST_CASE("Win32 capture stays while any button is held and ends with the last release") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(20, 20));
    CHECK(GetCapture() == host.get());
    host.send(WM_LBUTTONDOWN, MK_LBUTTON | MK_RBUTTON, MAKELPARAM(21, 20));
    host.send(WM_LBUTTONUP, MK_RBUTTON, MAKELPARAM(22, 20));
    CHECK(GetCapture() == host.get());
    // While a button is held a leave is only observed, never a pointer move.
    CHECK_FALSE(adapter.process(WM_MOUSELEAVE, 0, 0));
    host.send(WM_RBUTTONUP, 0, MAKELPARAM(23, 20));
    CHECK(GetCapture() != host.get());
    CHECK(host.capture_changes == 1);
    const auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    REQUIRE(frame.events.size() == 4);
    CHECK(frame.events[0].kind == wui::EventKind::pointer_down);
    CHECK(frame.events[0].button == wui::PointerButton::right);
    CHECK(frame.events[1].kind == wui::EventKind::pointer_down);
    CHECK(frame.events[1].button == wui::PointerButton::left);
    CHECK(frame.events[2].kind == wui::EventKind::pointer_up);
    CHECK(frame.events[2].button == wui::PointerButton::left);
    CHECK(frame.events[3].kind == wui::EventKind::pointer_up);
    CHECK(frame.events[3].button == wui::PointerButton::right);
}

TEST_CASE("Win32 horizontal wheel reports signed notches apart from the vertical wheel") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    POINT point{25, 35};
    REQUIRE(ClientToScreen(host.get(), &point));
    CHECK(adapter.process(WM_MOUSEHWHEEL, MAKEWPARAM(MK_CONTROL, static_cast<WORD>(2 * WHEEL_DELTA)),
        MAKELPARAM(point.x, point.y)));
    CHECK(adapter.process(WM_MOUSEHWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA / 2)),
        MAKELPARAM(point.x, point.y)));
    const auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    REQUIRE(frame.events.size() == 2);
    for (const auto& event : frame.events) {
        CHECK(event.kind == wui::EventKind::wheel);
        CHECK(event.wheel == 0.0f);
        CHECK(event.position.x == doctest::Approx(25.0f / frame.scale));
        CHECK(event.position.y == doctest::Approx(35.0f / frame.scale));
    }
    CHECK(frame.events[0].wheel_x == 2.0f);
    CHECK(frame.events[0].control);
    CHECK(frame.events[1].wheel_x == -0.5f);
    CHECK_FALSE(frame.events[1].control);
}

TEST_CASE("Win32 virtual keys map to Key and always carry the native code") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    struct Mapping {
        WPARAM code;
        wui::Key key;
    };
    std::vector<Mapping> mappings{
        {'A', wui::Key::a}, {'B', wui::Key::b}, {'C', wui::Key::c}, {'D', wui::Key::d},
        {'E', wui::Key::e}, {'F', wui::Key::f}, {'G', wui::Key::g}, {'H', wui::Key::h},
        {'I', wui::Key::i}, {'J', wui::Key::j}, {'K', wui::Key::k}, {'L', wui::Key::l},
        {'M', wui::Key::m}, {'N', wui::Key::n}, {'O', wui::Key::o}, {'P', wui::Key::p},
        {'Q', wui::Key::q}, {'R', wui::Key::r}, {'S', wui::Key::s}, {'T', wui::Key::t},
        {'U', wui::Key::u}, {'V', wui::Key::v}, {'W', wui::Key::w}, {'X', wui::Key::x},
        {'Y', wui::Key::y}, {'Z', wui::Key::z},
        {VK_TAB, wui::Key::tab}, {VK_RETURN, wui::Key::enter}, {VK_SPACE, wui::Key::space},
        {VK_ESCAPE, wui::Key::escape}, {VK_LEFT, wui::Key::left}, {VK_RIGHT, wui::Key::right},
        {VK_UP, wui::Key::up}, {VK_DOWN, wui::Key::down}, {VK_HOME, wui::Key::home},
        {VK_END, wui::Key::end}, {VK_BACK, wui::Key::backspace}, {VK_DELETE, wui::Key::del},
        {VK_INSERT, wui::Key::insert}, {VK_PRIOR, wui::Key::page_up}, {VK_NEXT, wui::Key::page_down},
        // Keys without a Key name stay bindable through native_key.
        {VK_OEM_3, wui::Key::none}, {VK_NUMPAD5, wui::Key::none}, {VK_MULTIPLY, wui::Key::none}};
    const wui::Key digits[]{wui::Key::digit0, wui::Key::digit1, wui::Key::digit2, wui::Key::digit3,
        wui::Key::digit4, wui::Key::digit5, wui::Key::digit6, wui::Key::digit7, wui::Key::digit8,
        wui::Key::digit9};
    for (std::size_t index = 0; index < std::size(digits); ++index)
        mappings.push_back({'0' + index, digits[index]});
    const wui::Key functions[]{wui::Key::f1, wui::Key::f2, wui::Key::f3, wui::Key::f4, wui::Key::f5,
        wui::Key::f6, wui::Key::f7, wui::Key::f8, wui::Key::f9, wui::Key::f10, wui::Key::f11, wui::Key::f12};
    for (std::size_t index = 0; index < std::size(functions); ++index)
        mappings.push_back({VK_F1 + index, functions[index]});
    for (const auto& mapping : mappings) {
        INFO("virtual key ", mapping.code);
        CHECK(adapter.process(WM_KEYDOWN, mapping.code, 1));
        CHECK(adapter.process(WM_KEYUP, mapping.code, static_cast<LPARAM>(0xC0000001u)));
    }
    const auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    REQUIRE(frame.events.size() == 2 * mappings.size());
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        INFO("virtual key ", mappings[index].code);
        const auto& down = frame.events[2 * index];
        const auto& up = frame.events[2 * index + 1];
        CHECK(down.kind == wui::EventKind::key_down);
        CHECK(up.kind == wui::EventKind::key_up);
        CHECK(down.key == mappings[index].key);
        CHECK(up.key == mappings[index].key);
        CHECK(down.native_key == mappings[index].code);
        CHECK(up.native_key == mappings[index].code);
    }
}

TEST_CASE("Win32 left and right modifiers and system keys queue resolved key events") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    // Keystroke flags: scan code, extended bit 24, context (Alt) bit 29 and
    // the previous-state/transition bits of a release.
    const auto flags = [](unsigned scan, bool extended, bool context_alt, bool up) {
        unsigned value = 1u | (scan << 16);
        if (extended) value |= 1u << 24;
        if (context_alt) value |= 1u << 29;
        if (up) value |= 0xC0000000u;
        return static_cast<LPARAM>(value);
    };
    using wui::Key;
    constexpr auto down = wui::EventKind::key_down;
    constexpr auto up = wui::EventKind::key_up;
    struct Step {
        UINT message;
        WPARAM code;
        LPARAM lparam;
        wui::EventKind kind;
        Key key;
        std::uint32_t native;
        bool shift, control, alt, super;
    };
    const Step steps[]{
        {WM_KEYDOWN, VK_SHIFT, flags(0x2A, false, false, false), down, Key::left_shift, VK_LSHIFT, true, false, false, false},
        {WM_KEYDOWN, VK_SHIFT, flags(0x36, false, false, false), down, Key::right_shift, VK_RSHIFT, true, false, false, false},
        {WM_KEYUP, VK_SHIFT, flags(0x2A, false, false, true), up, Key::left_shift, VK_LSHIFT, true, false, false, false},
        {WM_KEYUP, VK_SHIFT, flags(0x36, false, false, true), up, Key::right_shift, VK_RSHIFT, false, false, false, false},
        {WM_KEYDOWN, VK_CONTROL, flags(0x1D, false, false, false), down, Key::left_control, VK_LCONTROL, false, true, false, false},
        {WM_KEYDOWN, VK_CONTROL, flags(0x1D, true, false, false), down, Key::right_control, VK_RCONTROL, false, true, false, false},
        {WM_KEYUP, VK_CONTROL, flags(0x1D, false, false, true), up, Key::left_control, VK_LCONTROL, false, true, false, false},
        {WM_KEYUP, VK_CONTROL, flags(0x1D, true, false, true), up, Key::right_control, VK_RCONTROL, false, false, false, false},
        {WM_SYSKEYDOWN, VK_MENU, flags(0x38, false, true, false), down, Key::left_alt, VK_LMENU, false, false, true, false},
        {WM_SYSKEYDOWN, 'F', flags(0x21, false, true, false), down, Key::f, 'F', false, false, true, false},
        {WM_SYSKEYUP, 'F', flags(0x21, false, true, true), up, Key::f, 'F', false, false, true, false},
        {WM_KEYUP, VK_MENU, flags(0x38, false, false, true), up, Key::left_alt, VK_LMENU, false, false, false, false},
        {WM_SYSKEYDOWN, VK_MENU, flags(0x38, true, true, false), down, Key::right_alt, VK_RMENU, false, false, true, false},
        {WM_SYSKEYUP, VK_MENU, flags(0x38, true, true, true), up, Key::right_alt, VK_RMENU, false, false, false, false},
        {WM_KEYDOWN, VK_LWIN, flags(0x5B, true, false, false), down, Key::left_super, VK_LWIN, false, false, false, true},
        {WM_KEYDOWN, VK_RWIN, flags(0x5C, true, false, false), down, Key::right_super, VK_RWIN, false, false, false, true},
        {WM_KEYUP, VK_LWIN, flags(0x5B, true, false, true), up, Key::left_super, VK_LWIN, false, false, false, true},
        {WM_KEYUP, VK_RWIN, flags(0x5C, true, false, true), up, Key::right_super, VK_RWIN, false, false, false, false},
        // F10 is a system key without Alt. The context bit reports Alt even
        // when the Alt key-down itself was never routed to the adapter.
        {WM_SYSKEYDOWN, VK_F10, flags(0x44, false, false, false), down, Key::f10, VK_F10, false, false, false, false},
        {WM_SYSKEYDOWN, 'G', flags(0x22, false, true, false), down, Key::g, 'G', false, false, true, false},
    };
    for (const auto& step : steps) {
        INFO("virtual key ", step.code);
        CHECK(adapter.process(step.message, step.code, step.lparam));
    }
    const auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    REQUIRE(frame.events.size() == std::size(steps));
    for (std::size_t index = 0; index < std::size(steps); ++index) {
        INFO("step ", index);
        const auto& event = frame.events[index];
        const auto& step = steps[index];
        CHECK(event.kind == step.kind);
        CHECK(event.key == step.key);
        CHECK(event.native_key == step.native);
        CHECK(event.shift == step.shift);
        CHECK(event.control == step.control);
        CHECK(event.alt == step.alt);
        CHECK(event.super == step.super);
        CHECK_FALSE(event.repeat);
    }
}

TEST_CASE("Win32 hooked WndProc decides forwarding while process reports translation") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    Hook hook;
    hook.adapter = &adapter;
    Subclass subclass(host.get(), hook);
    REQUIRE(hook.previous != nullptr);
    RECT suggested{0, 0, 420, 300};
    struct Message {
        UINT id;
        WPARAM wparam;
        LPARAM lparam;
        bool translated;
    };
    const Message messages[]{
        {WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(15, 25), true},
        {WM_LBUTTONUP, 0, MAKELPARAM(15, 25), true},
        {WM_KEYDOWN, 'K', 1, true},
        {WM_SIZE, SIZE_RESTORED, MAKELPARAM(300, 200), false},
        {WM_SETCURSOR, reinterpret_cast<WPARAM>(host.get()), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE), false},
        {WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&suggested), false},
    };
    for (const bool swallow : {false, true}) {
        hook.swallow_translated = swallow;
        for (const auto& message : messages) {
            INFO("message ", message.id, " swallow ", swallow);
            hook.watched = message.id;
            hook.watched_translated = !message.translated;
            host.observed.clear();
            SendMessageW(host.get(), message.id, message.wparam, message.lparam);
            CHECK(hook.watched_translated == message.translated);
            // The previous procedure sees exactly what the hook forwards: all
            // messages when forwarding, only untranslated ones when swallowing.
            const auto seen = std::count(host.observed.begin(), host.observed.end(), message.id);
            CHECK(seen == (swallow && message.translated ? 0 : 1));
        }
    }
    const auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    CHECK(count(frame, wui::EventKind::pointer_down) == 2);
    CHECK(count(frame, wui::EventKind::pointer_up) == 2);
    CHECK(count(frame, wui::EventKind::key_down) == 2);
    CHECK(frame.events.size() == 6);
}

TEST_CASE("Win32 adapter built off thread serves frames to a frame thread") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    std::unique_ptr<Adapter> adapter;
    run_off_thread([&] { adapter = std::make_unique<Adapter>(host.get()); });
    REQUIRE(adapter != nullptr);
    REQUIRE_FALSE(adapter->status().has_value());
    WindowBinding binding(host, *adapter);
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(12, 18));
    host.send(WM_KEYDOWN, 'K', 1);
    host.send(WM_CHAR, 'k', 1);
    host.send(WM_KEYUP, 'K', static_cast<LPARAM>(0xC0000001u));
    RECT client{};
    REQUIRE(GetClientRect(host.get(), &client));
    wui::FrameInput frame;
    run_off_thread([&] { frame = adapter->take_frame(0.016); });
    REQUIRE_FALSE(adapter->status().has_value());
    CHECK(frame.viewport.w == doctest::Approx((client.right - client.left) / frame.scale));
    CHECK(frame.viewport.h == doctest::Approx((client.bottom - client.top) / frame.scale));
    REQUIRE(frame.events.size() == 4);
    CHECK(frame.events[0].kind == wui::EventKind::pointer_move);
    CHECK(frame.events[0].position.x == doctest::Approx(12.0f / frame.scale));
    CHECK(frame.events[1].kind == wui::EventKind::key_down);
    CHECK(frame.events[1].key == wui::Key::k);
    CHECK(frame.events[2].text == "k");
    CHECK(frame.events[3].kind == wui::EventKind::key_up);

    // A frame thread draining concurrently receives every event exactly once,
    // in order, always with a valid extent.
    constexpr int moves = 512;
    std::atomic<bool> done{false};
    std::vector<long> drained;
    bool valid = true;
    std::thread drainer([&] {
        for (;;) {
            const bool last = done.load();
            const auto taken = adapter->take_frame(0.016);
            if (taken.viewport.w < 0 || taken.viewport.h < 0) valid = false;
            for (const auto& event : taken.events)
                drained.push_back(std::lround(event.position.x * taken.scale));
            if (last) break;
            std::this_thread::yield();
        }
    });
    for (int x = 0; x < moves; ++x) host.send(WM_MOUSEMOVE, 0, MAKELPARAM(x, 7));
    done = true;
    drainer.join();
    CHECK(valid);
    REQUIRE(drained.size() == static_cast<std::size_t>(moves));
    bool ordered = true;
    for (std::size_t index = 0; index < drained.size(); ++index)
        ordered = ordered && drained[index] == static_cast<long>(index);
    CHECK(ordered);

    // process() belongs to the window thread; elsewhere it latches a failure.
    bool translated = true;
    run_off_thread([&] { translated = adapter->process(WM_MOUSEMOVE, 0, MAKELPARAM(1, 2)); });
    CHECK_FALSE(translated);
    REQUIRE(adapter->status().has_value());
    CHECK(adapter->status()->code == wui::ErrorCode::invalid_frame);
    const auto failed = adapter->take_frame(0.016);
    CHECK(failed.viewport.w < 0);
    CHECK(failed.events.empty());
}

TEST_CASE("Win32 target extent maps client pixels and clears back to the client") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    const float dpi_scale = adapter.take_frame(0).scale;
    RECT client{};
    REQUIRE(GetClientRect(host.get(), &client));
    const auto width = static_cast<std::uint32_t>(client.right - client.left);
    const auto height = static_cast<std::uint32_t>(client.bottom - client.top);
    REQUIRE(width > 0);
    REQUIRE(height > 0);

    struct Invalid {
        std::uint32_t width;
        std::uint32_t height;
        float scale;
    };
    const Invalid invalid[]{
        {0, height, 1.0f}, {width, 0, 1.0f}, {width, height, 0.0f}, {width, height, -1.0f},
        {width, height, std::numeric_limits<float>::quiet_NaN()},
        {width, height, std::numeric_limits<float>::infinity()},
    };
    for (const auto& target : invalid) {
        const auto status = adapter.set_target(target.width, target.height, target.scale);
        REQUIRE(status.has_value());
        CHECK(status->code == wui::ErrorCode::invalid_argument);
    }
    CHECK_FALSE(adapter.status().has_value());

    REQUIRE_FALSE(adapter.set_target(2 * width, 2 * height, 1.0f).has_value());
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(10, 20));
    POINT wheel{30, 5};
    REQUIRE(ClientToScreen(host.get(), &wheel));
    host.send(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(wheel.x, wheel.y));
    auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    CHECK(frame.scale == 1.0f);
    CHECK(frame.viewport.x == 0.0f);
    CHECK(frame.viewport.y == 0.0f);
    CHECK(frame.viewport.w == static_cast<float>(2 * width));
    CHECK(frame.viewport.h == static_cast<float>(2 * height));
    REQUIRE(frame.events.size() == 2);
    CHECK(frame.events[0].position.x == 20.0f);
    CHECK(frame.events[0].position.y == 40.0f);
    CHECK(frame.events[1].position.x == 60.0f);
    CHECK(frame.events[1].position.y == 10.0f);

    // The target scale replaces the window DPI for positions and extent.
    REQUIRE_FALSE(adapter.set_target(2 * width, 2 * height, 2.0f).has_value());
    host.send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
    host.send(WM_LBUTTONUP, 0, MAKELPARAM(10, 20));
    frame = adapter.take_frame(0.016);
    CHECK(frame.scale == 2.0f);
    CHECK(frame.viewport.w == static_cast<float>(width));
    CHECK(frame.viewport.h == static_cast<float>(height));
    REQUIRE(frame.events.size() == 2);
    CHECK(frame.events[0].position.x == 10.0f);
    CHECK(frame.events[0].position.y == 20.0f);

    adapter.clear_target();
    host.send(WM_MOUSEMOVE, 0, MAKELPARAM(10, 20));
    frame = adapter.take_frame(0.016);
    CHECK(frame.scale == dpi_scale);
    CHECK(frame.viewport.w == doctest::Approx(static_cast<float>(width) / dpi_scale));
    CHECK(frame.viewport.h == doctest::Approx(static_cast<float>(height) / dpi_scale));
    REQUIRE(frame.events.size() == 1);
    CHECK(frame.events[0].position.x == doctest::Approx(10.0f / dpi_scale));
    CHECK(frame.events[0].position.y == doctest::Approx(20.0f / dpi_scale));
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 composition caret inverts the target mapping and defers off-thread requests") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    NativeImeContext ime(host.get());
    REQUIRE_MESSAGE(ime.context != nullptr, "ImmCreateContext unavailable; native IMM is required");
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    RECT client{};
    REQUIRE(GetClientRect(host.get(), &client));
    const auto width = static_cast<std::uint32_t>(client.right - client.left);
    const auto height = static_cast<std::uint32_t>(client.bottom - client.top);
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    REQUIRE_FALSE(adapter.set_target(2 * width, 2 * height, 1.0f).has_value());
    // Logical (20, 40) is target pixel (20, 40), which is client pixel (10, 20).
    adapter.set_composition_position({20.0f, 40.0f});
    REQUIRE_FALSE(adapter.status().has_value());
    COMPOSITIONFORM composition{};
    REQUIRE(ImmGetCompositionWindow(ime.context, &composition));
    CHECK(composition.dwStyle == CFS_POINT);
    CHECK(composition.ptCurrentPos.x == 10);
    CHECK(composition.ptCurrentPos.y == 20);

    run_off_thread([&] { adapter.set_composition_position({60.0f, 80.0f}); });
    REQUIRE(ImmGetCompositionWindow(ime.context, &composition));
    CHECK(composition.ptCurrentPos.x == 10);
    CHECK(composition.ptCurrentPos.y == 20);
    host.send(WM_NULL);
    REQUIRE(ImmGetCompositionWindow(ime.context, &composition));
    CHECK(composition.ptCurrentPos.x == 30);
    CHECK(composition.ptCurrentPos.y == 40);
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 cancel_input forgets held input and releases capture on the window thread") {
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    WindowBinding binding(host, adapter);
    host.send(WM_KEYDOWN, VK_SHIFT, 1);
    host.send(WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, MAKELPARAM(12, 12));
    REQUIRE(GetCapture() == host.get());
    adapter.take_frame(0.016);
    SUBCASE("from another thread, capture is released by the next message") {
        run_off_thread([&] { adapter.cancel_input(); });
        CHECK(GetCapture() == host.get());
        CHECK(host.capture_changes == 0);
        host.send(WM_NULL);
    }
    SUBCASE("on the window thread, capture is released immediately") {
        adapter.cancel_input();
    }
    CHECK(GetCapture() != host.get());
    CHECK(host.capture_changes == 1);
    host.send(WM_KEYDOWN, 'A', 1);
    const auto frame = adapter.take_frame(0.016);
    REQUIRE_FALSE(adapter.status().has_value());
    REQUIRE(frame.events.size() == 2);
    CHECK(frame.events[0].kind == wui::EventKind::focus_lost);
    CHECK(frame.events[1].key == wui::Key::a);
    CHECK_FALSE(frame.events[1].shift);
    CHECK_FALSE(frame.events[1].control);
}
