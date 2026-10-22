#pragma once

#ifndef _WIN32
#error "wui::example requires Windows"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wui/backends/d3d11.hpp>
#include <wui/backends/win32.hpp>
#include <wui/context.hpp>
#include <wui/types.hpp>

#include "gallery.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wui::example {

enum class Driver { hardware, warp };

struct FrameSample {
    double cpu_us = 0;
    float scale = 0;
    std::uint64_t calls = 0, bytes = 0;
    std::size_t vertices = 0, indices = 0, batches = 0, textures = 0, semantics = 0;
    // D3D11 timestamp query id for this frame's UI renderer submission; 0 = none.
    std::uint64_t gpu_frame = 0;
    // Filled after the batch from the asynchronous query results.
    double gpu_us = -1;
    const char* gpu_status = "unmeasured";
    // Sampled after the frame, outside the allocation counter.
    std::size_t retained_bytes = 0, peak_retained_bytes = 0, process_peak_bytes = 0;
};

class NativeApp {
public:
    NativeApp();
    ~NativeApp();
    NativeApp(const NativeApp&) = delete;
    NativeApp& operator=(const NativeApp&) = delete;

    Status initialize(Driver driver);
    int run();
    Status step(double dt_seconds);
    Status sample_step(FrameSample& sample, bool& completed, bool& interrupted);
    Status resize(std::uint32_t width, std::uint32_t height);
    Status recreate_device();
    Status capture(const std::filesystem::path& path);
    Status validate(const std::filesystem::path& directory);
    // nullopt measures the fixed gallery (allocation gate); otherwise one stress workload.
    Status measure(const std::filesystem::path& directory,
                   std::optional<StressWorkload> workload = std::nullopt);

    // Validation and measurement support (example-local, not public library API).
    HWND window() const;
    ID3D11Device* device() const;
    ID3D11DeviceContext* device_context() const;
    wui::d3d11::Renderer* renderer() const;
    wui::Context* ui_context() const;
    std::pair<std::uint32_t, std::uint32_t> target_size() const;
    const GalleryState& gallery_state() const;
    GalleryState& gallery_state();
    const std::vector<wui::Semantic>& last_semantics() const;
    wui::DrawPacket last_packet() const;
    unsigned submit_count() const;
    unsigned present_count() const;
    unsigned dpi_change_count() const;
    float adapter_scale() const;
    bool debug_enabled() const;
    void set_validation_scale(std::optional<float> scale);
    void set_disabled_mode(bool disabled);
    void set_evidence_enabled(bool enabled);
    void schedule_capture(const std::filesystem::path& path);
    Status reset_validation_scenario();
    // Selects what the host builds each frame; nullopt is the fixed gallery.
    void set_workload(std::optional<StressWorkload> workload);
    std::optional<StressWorkload> workload() const;
    // Applies the theme the current model/workload selects (between frames).
    Status apply_model_theme();
    // Reads finished timestamp queries; with wait, flushes and waits up to 1s.
    void collect_gpu_timings(bool wait);
    // Fills the sample's gpu_us/gpu_status from collected query results.
    void resolve_gpu_timing(FrameSample& sample) const;
    std::string driver_description() const;
    std::string dpi_description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    static LRESULT CALLBACK wnd_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    Status create_device_resources(Driver driver);
    Status create_target();
    Status create_window_and_adapter();
    Status create_ui_services();
    Status apply_pending_resize();
    Status update_pending_input();
    Status drain_or_submit(double dt_seconds, bool& did_present,
        FrameSample* sample = nullptr, bool* interrupted = nullptr);
    Status wait_for_messages(DWORD milliseconds);
    Status wait_busy();
    void pump_messages();
};

}  // namespace wui::example
