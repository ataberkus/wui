#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_app.hpp"

#include <climits>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct NarrowOutcome {
    bool ok = false;
    std::string value;
};

NarrowOutcome narrow_arg(const wchar_t* wide) {
    if (!wide) return {};
    const std::size_t src_len = std::wcslen(wide);
    if (src_len == 0) return {true, {}};
    if (src_len > static_cast<std::size_t>(INT_MAX)) return {};
    const int src_count = static_cast<int>(src_len);
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, src_count, nullptr, 0, nullptr, nullptr);
    if (needed <= 0 && ::GetLastError() == ERROR_INVALID_FLAGS) {
        needed = WideCharToMultiByte(CP_UTF8, 0, wide, src_count, nullptr, 0, nullptr, nullptr);
    }
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    int written =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, src_count, out.data(), needed, nullptr, nullptr);
    if (written == 0 && ::GetLastError() == ERROR_INVALID_FLAGS) {
        written = WideCharToMultiByte(CP_UTF8, 0, wide, src_count, out.data(), needed, nullptr, nullptr);
    }
    if (written != needed) return {};
    return {true, std::move(out)};
}

void print_usage() {
    std::fputs(
        "usage: wui_widget_gallery [--driver hardware|warp] [--validate <dir> | --measure <dir>\n"
        "                          [--workload gallery|controls100|controls1000|virtual10000]] [--help]\n",
        stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::optional<wui::example::Driver> driver;
        std::optional<std::filesystem::path> validate_dir;
        std::optional<std::filesystem::path> measure_dir;
        bool help = false;
        bool driver_seen = false;
        bool validate_seen = false;
        bool measure_seen = false;
        bool workload_seen = false;
        std::optional<StressWorkload> workload;

        for (int i = 1; i < argc; ++i) {
            NarrowOutcome arg_outcome = narrow_arg(argv[i]);
            if (!arg_outcome.ok) {
                std::fprintf(stderr, "error: invalid argument at position %d: cannot convert to UTF-8\n", i);
                return 2;
            }
            const std::string& arg = arg_outcome.value;
            if (arg == "--help") {
                if (help) {
                    std::fputs("error: duplicate --help\n", stderr);
                    return 2;
                }
                help = true;
            } else if (arg == "--driver") {
                if (driver_seen) {
                    std::fputs("error: duplicate --driver\n", stderr);
                    return 2;
                }
                driver_seen = true;
                if (i + 1 >= argc) {
                    std::fputs("error: --driver requires hardware|warp\n", stderr);
                    return 2;
                }
                NarrowOutcome value_outcome = narrow_arg(argv[++i]);
                if (!value_outcome.ok) {
                    std::fputs("error: invalid --driver value: cannot convert to UTF-8\n", stderr);
                    return 2;
                }
                const std::string& value = value_outcome.value;
                if (value == "hardware") {
                    driver = wui::example::Driver::hardware;
                } else if (value == "warp") {
                    driver = wui::example::Driver::warp;
                } else {
                    std::fputs("error: --driver must be hardware|warp\n", stderr);
                    return 2;
                }
            } else if (arg == "--validate") {
                if (validate_seen) {
                    std::fputs("error: duplicate --validate\n", stderr);
                    return 2;
                }
                validate_seen = true;
                if (i + 1 >= argc) {
                    std::fputs("error: --validate requires <directory>\n", stderr);
                    return 2;
                }
                validate_dir = std::filesystem::path(argv[++i]);
            } else if (arg == "--measure") {
                if (measure_seen) {
                    std::fputs("error: duplicate --measure\n", stderr);
                    return 2;
                }
                measure_seen = true;
                if (i + 1 >= argc) {
                    std::fputs("error: --measure requires <directory>\n", stderr);
                    return 2;
                }
                measure_dir = std::filesystem::path(argv[++i]);
            } else if (arg == "--workload") {
                if (workload_seen) {
                    std::fputs("error: duplicate --workload\n", stderr);
                    return 2;
                }
                workload_seen = true;
                if (i + 1 >= argc) {
                    std::fputs("error: --workload requires gallery|controls100|controls1000|virtual10000\n", stderr);
                    return 2;
                }
                NarrowOutcome value_outcome = narrow_arg(argv[++i]);
                const std::string& value = value_outcome.value;
                if (!value_outcome.ok) {
                    std::fputs("error: invalid --workload value: cannot convert to UTF-8\n", stderr);
                    return 2;
                } else if (value == "gallery") {
                    workload.reset();
                } else if (value == "controls100") {
                    workload = StressWorkload::controls_100;
                } else if (value == "controls1000") {
                    workload = StressWorkload::controls_1000;
                } else if (value == "virtual10000") {
                    workload = StressWorkload::virtual_10000;
                } else {
                    std::fputs("error: --workload must be gallery|controls100|controls1000|virtual10000\n", stderr);
                    return 2;
                }
            } else {
                std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
                return 2;
            }
        }

        if (validate_dir && measure_dir) {
            std::fputs("error: --validate and --measure conflict\n", stderr);
            return 2;
        }
        if (workload_seen && !measure_dir) {
            std::fputs("error: --workload requires --measure\n", stderr);
            return 2;
        }
        if (help) {
            print_usage();
            return 0;
        }
        if (!driver) driver = wui::example::Driver::hardware;

        auto ensure_dir = [](const std::filesystem::path& dir) -> bool {
            std::error_code code;
            std::filesystem::create_directories(dir, code);
            if (code) {
                std::fprintf(stderr, "error: cannot create directory '%s': %s\n",
                    dir.string().c_str(), code.message().c_str());
                return false;
            }
            return true;
        };

        wui::example::NativeApp app;
        if (auto error = app.initialize(*driver)) {
            std::fprintf(stderr, "error: initialize failed: %s\n", error->message.c_str());
            return 1;
        }
        if (validate_dir) {
            if (!ensure_dir(*validate_dir)) return 1;
            if (auto error = app.validate(*validate_dir)) {
                std::fprintf(stderr, "error: validate failed: %s\n", error->message.c_str());
                return 1;
            }
            return 0;
        }
        if (measure_dir) {
            if (!ensure_dir(*measure_dir)) return 1;
            if (auto error = app.measure(*measure_dir, workload)) {
                std::fprintf(stderr, "error: measure failed: %s\n", error->message.c_str());
                return 1;
            }
            return 0;
        }
        return app.run();
    } catch (const std::bad_alloc&) {
        std::fputs("error: out of memory\n", stderr);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("error: unknown failure\n", stderr);
        return 1;
    }
}
