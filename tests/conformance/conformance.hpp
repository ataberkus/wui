#pragma once

#include "compare.hpp"

#include <wui/draw.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace wui::conformance {

// Every adapter clears its single-sample UNORM target to this opaque color
// before rendering a case (sRGB-encoded bytes, as the native gallery host).
inline constexpr std::uint8_t kClear[4]{0x10, 0x12, 0x18, 0xff};

// Test-only offscreen adapter around one renderer. The adapter owns the
// device, targets, submission and readback exactly as a host would; the
// renderer under test is used only through its public backend API.
class OffscreenBackend {
public:
    virtual ~OffscreenBackend() = default;
    virtual std::string_view name() const = 0;          // "d3d11-warp", "vulkan", ...
    virtual std::string adapter_description() const = 0;
    virtual bool debug_layer() const = 0;               // validation/debug layer active
    // Uploads, renders into a cleared w x h target, waits for completion and reads back RGBA8.
    virtual Result<Image> render(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) = 0;
    // Submits without waiting for the GPU. Must return busy once the renderer's
    // frames-in-flight capacity is reached and the host has not yet collected.
    virtual Status submit_in_flight(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) = 0;
    // Waits for all submitted work and lets the renderer retire it.
    virtual Status collect_all() = 0;
    // D3D9: real Reset; D3D10/11: recreate on a new device; D3D12/Vulkan: shutdown + create.
    virtual Status recreate() = 0;
    // Debug/validation warnings + errors since the previous call (each printed to stderr).
    virtual std::uint64_t debug_messages() = 0;
};

struct Options {
    std::filesystem::path out;
    std::string driver = "warp";   // backends without a software driver ignore it
};

// Provided by exactly one backend translation unit per conformance executable.
Result<std::unique_ptr<OffscreenBackend>> make_backend(const Options& options);

}  // namespace wui::conformance
