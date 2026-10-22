#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "win32_test_support.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {
using wui::win32::Adapter;
using wui::win32::test::HostWindow;

std::wstring object_name(HANDLE handle) {
    DWORD bytes = 0;
    GetUserObjectInformationW(handle, UOI_NAME, nullptr, 0, &bytes);
    if (bytes == 0 || bytes % sizeof(wchar_t) != 0) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!GetUserObjectInformationW(handle, UOI_NAME, value.data(), bytes, &bytes)) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

class PrivateDesktop {
public:
    bool enter() {
        original_station_ = GetProcessWindowStation();
        if (original_station_ == nullptr) return failure("get original window station");
        // CREATE_ONLY never attaches to an existing clipboard. Named stations
        // need administrator rights; a fresh OS-named station is also safe.
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            return failure("query current token");
        const std::unique_ptr<void, decltype(&CloseHandle)> token(raw_token, &CloseHandle);
        DWORD acl_bytes = 0;
        GetTokenInformation(token.get(), TokenDefaultDacl, nullptr, 0, &acl_bytes);
        if (acl_bytes == 0) return failure("size default object ACL");
        std::vector<std::byte> acl_storage(acl_bytes);
        if (!GetTokenInformation(token.get(), TokenDefaultDacl,
                acl_storage.data(), acl_bytes, &acl_bytes))
            return failure("read default object ACL");
        const auto acl = reinterpret_cast<TOKEN_DEFAULT_DACL*>(acl_storage.data())->DefaultDacl;
        SECURITY_DESCRIPTOR descriptor{};
        if (acl == nullptr || !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE))
            return failure("prepare private object ACL");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
        name_ = L"wui_A8_clipboard_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(GetTickCount64());
        station_ = CreateWindowStationW(name_.c_str(), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, &security);
        if (station_ == nullptr && GetLastError() == ERROR_ACCESS_DENIED)
            station_ = CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, &security);
        if (station_ == nullptr) return failure("create new private window station");
        name_ = object_name(station_);
        if (name_.empty() || name_ == object_name(original_station_))
            return failure("verify distinct private station");
        if (!SetProcessWindowStation(station_)) return failure("enter private window station");
        station_changed_ = true;
        desktop_ = CreateDesktopW(L"wui_A8_private", nullptr, nullptr, 0,
            GENERIC_ALL, &security);
        if (desktop_ == nullptr) return failure("create private desktop");
        USEROBJECTFLAGS flags{};
        DWORD bytes = 0;
        if (GetProcessWindowStation() == original_station_ ||
            object_name(GetProcessWindowStation()) != name_ ||
            object_name(desktop_) != L"wui_A8_private" ||
            !GetUserObjectInformationW(station_, UOI_FLAGS, &flags, sizeof(flags), &bytes) ||
            (flags.dwFlags & WSF_VISIBLE) != 0) {
            return failure("verify private noninteractive station and desktop");
        }
        return true;
    }
    bool restore() {
        bool success = true;
        // Only the joined GUI worker enters the private desktop. The main
        // thread retains its original desktop, including any runtime HWNDs.
        if (station_changed_) {
            if (SetProcessWindowStation(original_station_)) station_changed_ = false;
            else success = failure("restore original window station");
        }
        if (desktop_ != nullptr && !station_changed_) {
            if (CloseDesktop(desktop_)) desktop_ = nullptr;
            else success = failure("close private desktop");
        }
        if (station_ != nullptr && !station_changed_ && desktop_ == nullptr) {
            if (CloseWindowStation(station_)) station_ = nullptr;
            else success = failure("close private window station");
        }
        return success;
    }
    ~PrivateDesktop() { restore(); }
    HDESK desktop() const { return desktop_; }
    const std::wstring& name() const { return name_; }
private:
    static bool failure(const char* step) {
        std::fprintf(stderr, "A8 clipboard isolation gate failed: %s (Win32 error %lu)\n",
            step, GetLastError());
        return false;
    }
    HWINSTA original_station_ = nullptr;
    HWINSTA station_ = nullptr;
    HDESK desktop_ = nullptr;
    bool station_changed_ = false;
    std::wstring name_;
};

PrivateDesktop* isolated_desktop = nullptr;

void require_isolation() {
    REQUIRE(isolated_desktop != nullptr);
    REQUIRE(object_name(GetProcessWindowStation()) == isolated_desktop->name());
    REQUIRE(GetThreadDesktop(GetCurrentThreadId()) == isolated_desktop->desktop());
    USEROBJECTFLAGS flags{};
    DWORD bytes = 0;
    REQUIRE(GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS,
        &flags, sizeof(flags), &bytes));
    REQUIRE((flags.dwFlags & WSF_VISIBLE) == 0);
}

class ClipboardOpen {
public:
    explicit ClipboardOpen(HWND window) : opened(OpenClipboard(window) != FALSE) {}
    ~ClipboardOpen() { if (opened) CloseClipboard(); }
    bool opened;
};

class GlobalBlock {
public:
    explicit GlobalBlock(SIZE_T bytes) : handle(GlobalAlloc(GMEM_MOVEABLE, bytes)) {}
    ~GlobalBlock() { if (handle != nullptr) GlobalFree(handle); }
    HGLOBAL handle;
};

class GlobalLockGuard {
public:
    explicit GlobalLockGuard(HGLOBAL handle) : data(GlobalLock(handle)), handle_(handle) {}
    ~GlobalLockGuard() { if (data != nullptr) GlobalUnlock(handle_); }
    void* data;
private:
    HGLOBAL handle_;
};

void publish_unicode(HWND window, std::wstring_view value) {
    require_isolation();
    ClipboardOpen clipboard(window);
    REQUIRE(clipboard.opened);
    REQUIRE(EmptyClipboard());
    GlobalBlock block((value.size() + 1) * sizeof(wchar_t));
    REQUIRE(block.handle != nullptr);
    {
        GlobalLockGuard lock(block.handle);
        REQUIRE(lock.data != nullptr);
        std::memset(lock.data, 0, GlobalSize(block.handle));
        std::memcpy(lock.data, value.data(), value.size() * sizeof(wchar_t));
        static_cast<wchar_t*>(lock.data)[value.size()] = L'\0';
    }
    REQUIRE(SetClipboardData(CF_UNICODETEXT, block.handle) != nullptr);
    block.handle = nullptr;
}

std::wstring native_unicode(HWND window) {
    require_isolation();
    ClipboardOpen clipboard(window);
    REQUIRE(clipboard.opened);
    const auto handle = static_cast<HGLOBAL>(GetClipboardData(CF_UNICODETEXT));
    REQUIRE(handle != nullptr);
    GlobalLockGuard lock(handle);
    REQUIRE(lock.data != nullptr);
    const auto* value = static_cast<const wchar_t*>(lock.data);
    const auto capacity = GlobalSize(handle) / sizeof(wchar_t);
    std::size_t length = 0;
    while (length < capacity && value[length] != L'\0') ++length;
    REQUIRE(length < capacity);
    return {value, length};
}

class ClipboardHolder {
public:
    explicit ClipboardHolder(HDESK desktop)
        : ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          release_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (ready_ != nullptr && release_ != nullptr) {
            worker_ = std::thread([this, desktop] {
                if (!SetThreadDesktop(desktop)) {
                    error_ = GetLastError();
                    SetEvent(ready_);
                    return;
                }
                HostWindow host;
                if (host.get() == nullptr) {
                    error_ = GetLastError();
                    SetEvent(ready_);
                    return;
                }
                ClipboardOpen clipboard(host.get());
                acquired_ = clipboard.opened;
                if (!clipboard.opened) error_ = GetLastError();
                SetEvent(ready_);
                if (clipboard.opened) WaitForSingleObject(release_, 5000);
            });
        }
    }
    ~ClipboardHolder() {
        release();
        if (ready_ != nullptr) CloseHandle(ready_);
        if (release_ != nullptr) CloseHandle(release_);
    }
    bool wait() {
        return ready_ != nullptr && WaitForSingleObject(ready_, 5000) == WAIT_OBJECT_0 && acquired_;
    }
    DWORD error() const { return error_; }
    void release() {
        if (release_ != nullptr) SetEvent(release_);
        if (worker_.joinable()) worker_.join();
    }
private:
    HANDLE ready_ = nullptr;
    HANDLE release_ = nullptr;
    std::atomic<bool> acquired_ = false;
    std::atomic<DWORD> error_ = ERROR_SUCCESS;
    std::thread worker_;
};

const std::string sample_utf8 = "line 1\r\nGr\xC3\xBC\xC3\x9F" "e \xE6\xBC\xA2 \xF0\x9F\x99\x82";
const std::wstring sample_utf16 = L"line 1\r\nGr\xFC\xDF" L"e \x6F22 \xD83D\xDE42";
} // namespace

TEST_CASE("Win32 clipboard transfers real CF_UNICODETEXT only in an isolated station") {
    require_isolation();
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    REQUIRE_FALSE(adapter.set_clipboard(sample_utf8).has_value());
    CHECK(native_unicode(host.get()) == sample_utf16);
    auto value = adapter.clipboard();
    REQUIRE(std::holds_alternative<std::string>(value));
    CHECK(std::get<std::string>(value) == sample_utf8);
    CHECK_FALSE(adapter.status().has_value());

    HostWindow producer;
    REQUIRE(producer.get() != nullptr);
    publish_unicode(producer.get(), L"native \xD83D\xDE42 \x6F22");
    value = adapter.clipboard();
    REQUIRE(std::holds_alternative<std::string>(value));
    CHECK(std::get<std::string>(value) == "native \xF0\x9F\x99\x82 \xE6\xBC\xA2");
    REQUIRE_FALSE(adapter.set_clipboard("").has_value());
    CHECK(native_unicode(host.get()).empty());
    value = adapter.clipboard();
    REQUIRE(std::holds_alternative<std::string>(value));
    CHECK(std::get<std::string>(value).empty());
}

TEST_CASE("Win32 clipboard rejects invalid UTF8 and NUL before replacing contents") {
    require_isolation();
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    publish_unicode(host.get(), L"synthetic previous clipboard");
    wui::Status status;
    SUBCASE("malformed UTF8") { status = adapter.set_clipboard("bad \xED\xA0\x80"); }
    SUBCASE("embedded NUL") { status = adapter.set_clipboard(std::string_view("a\0b", 3)); }
    REQUIRE(status.has_value());
    CHECK(status->code == wui::ErrorCode::invalid_argument);
    CHECK(native_unicode(host.get()) == L"synthetic previous clipboard");
    CHECK_FALSE(adapter.status().has_value());
}

TEST_CASE("Win32 clipboard rejects invalid native UTF16 without latching adapter failure") {
    require_isolation();
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    publish_unicode(host.get(), L"\xD83D");
    const auto value = adapter.clipboard();
    REQUIRE(std::holds_alternative<wui::Error>(value));
    CHECK(std::get<wui::Error>(value).code == wui::ErrorCode::invalid_argument);
    CHECK_FALSE(adapter.status().has_value());
    // Another thread can open it immediately, proving the failed read closed it.
    ClipboardHolder holder(isolated_desktop->desktop());
    INFO("clipboard holder Win32 error: ", holder.error());
    REQUIRE(holder.wait());
    holder.release();
    REQUIRE_FALSE(adapter.set_clipboard(sample_utf8).has_value());
    CHECK(native_unicode(host.get()) == sample_utf16);
}

TEST_CASE("Win32 clipboard contention is a local busy error and preserves previous contents") {
    require_isolation();
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    publish_unicode(host.get(), L"preserved under contention");
    ClipboardHolder holder(isolated_desktop->desktop());
    INFO("clipboard holder Win32 error: ", holder.error());
    REQUIRE(holder.wait());
    const auto write_error = adapter.set_clipboard("must not replace clipboard");
    REQUIRE(write_error.has_value());
    CHECK(write_error->code == wui::ErrorCode::busy);
    const auto read_error = adapter.clipboard();
    REQUIRE(std::holds_alternative<wui::Error>(read_error));
    CHECK(std::get<wui::Error>(read_error).code == wui::ErrorCode::busy);
    CHECK_FALSE(adapter.status().has_value());
    holder.release();
    CHECK(native_unicode(host.get()) == L"preserved under contention");
    auto frame = adapter.take_frame(0.016);
    CHECK(frame.viewport.w >= 0);
    CHECK(frame.viewport.h >= 0);
    REQUIRE_FALSE(adapter.set_clipboard("recovered").has_value());
    CHECK(native_unicode(host.get()) == L"recovered");
}

TEST_CASE("Win32 missing Unicode format is reported and subsequent operations recover") {
    require_isolation();
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    {
        ClipboardOpen clipboard(host.get());
        REQUIRE(clipboard.opened);
        REQUIRE(EmptyClipboard());
    }
    const auto value = adapter.clipboard();
    REQUIRE(std::holds_alternative<wui::Error>(value));
    CHECK(std::get<wui::Error>(value).code == wui::ErrorCode::unsupported);
    CHECK_FALSE(adapter.status().has_value());
    REQUIRE_FALSE(adapter.set_clipboard("available now").has_value());
    CHECK(native_unicode(host.get()) == L"available now");
}

TEST_CASE("Win32 clipboard through the core service preserves Unicode and recovers") {
    require_isolation();
    HostWindow host;
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    wui::IClipboard& service = adapter;
    REQUIRE_FALSE(service.set_clipboard(sample_utf8).has_value());
    const auto stored = service.clipboard();
    REQUIRE(std::holds_alternative<std::string>(stored));
    CHECK(std::get<std::string>(stored) == sample_utf8);
    CHECK(native_unicode(host.get()) == sample_utf16);
    REQUIRE_FALSE(service.set_clipboard("").has_value());
    const auto cleared = service.clipboard();
    REQUIRE(std::holds_alternative<std::string>(cleared));
    CHECK(std::get<std::string>(cleared).empty());
}

TEST_CASE("Win32 destroyed clipboard owner returns an error without replacing private data") {
    require_isolation();
    HostWindow producer;
    HostWindow host;
    REQUIRE(producer.get() != nullptr);
    REQUIRE(host.get() != nullptr);
    Adapter adapter(host.get());
    host.destroy();
    REQUIRE(adapter.set_clipboard("not written").has_value());
    CHECK(std::holds_alternative<wui::Error>(adapter.clipboard()));
    CHECK(native_unicode(producer.get()) == L"private sentinel");
}

int main(int argc, char** argv) {
    // Elevated launches have a separate console. Preserve isolation failures
    // beside the explicitly requested doctest output, without sharing its file.
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument.starts_with("--out=")) {
            std::FILE* stream = nullptr;
            const auto path = std::string(argument.substr(6)) + ".isolation.log";
            if (freopen_s(&stream, path.c_str(), "a", stderr) != 0) return 2;
        }
    }
    PrivateDesktop desktop;
    if (!desktop.enter()) return 2;
    isolated_desktop = &desktop;
    int result = 2;
    try {
        // A fresh thread enters the private desktop before creating HWNDs.
        // Runtime helper windows can prevent moving the main thread.
        std::thread tests([&] {
            if (!SetThreadDesktop(desktop.desktop())) {
                std::fprintf(stderr, "A8 clipboard test thread could not enter private desktop (%lu)\n",
                    GetLastError());
                return;
            }
            try {
                doctest::Context context(argc, argv);
                result = context.run();
            } catch (const std::exception& error) {
                std::fprintf(stderr, "A8 isolated clipboard tests failed: %s\n", error.what());
            } catch (...) {
                std::fprintf(stderr, "A8 isolated clipboard tests failed with an unknown exception\n");
            }
        });
        tests.join();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A8 isolated clipboard tests failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "A8 isolated clipboard tests failed with an unknown exception\n");
    }
    isolated_desktop = nullptr;
    if (!desktop.restore()) return 2;
    return result;
}
