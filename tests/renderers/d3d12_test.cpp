#include <doctest/doctest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <wui/backends/d3d12.hpp>
#include "../../backends/renderer/d3d12_upload.hpp"

#include "../support/native_image.hpp"
#include "../support/packet_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Native D3D12 coverage. The test harness owns the queue, per-slot command
// allocators and lists, the fence, waits and readback. Every case runs on
// each feature-level 11_0 hardware adapter and on WARP, with the D3D12 debug
// layer (plus GPU-based validation in Debug builds) and asserts that the info
// queue holds zero warnings, errors and corruption messages.
namespace {

using Microsoft::WRL::ComPtr;
using wui::d3d12::Renderer;

void checked(HRESULT hr, const char* what) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + " failed, hr=" +
            std::to_string(static_cast<unsigned long>(hr)));
}

template<class T>
T success(wui::Result<T>& result, const char* what = "operation") {
    if (auto* error = std::get_if<wui::Error>(&result))
        throw std::runtime_error(std::string(what) + " failed: code=" +
            std::to_string(static_cast<int>(error->code)) + ", " + error->message);
    return std::get<T>(std::move(result));
}

struct DebugLayer {
    bool enabled = false;
    bool gpu_validation = false;
};

// The debug layer must be enabled before the first device is created and
// stays enabled for the process.
const DebugLayer& debug_layer() {
    static const DebugLayer layer = [] {
        DebugLayer out;
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) return out;
        debug->EnableDebugLayer();
        out.enabled = true;
#ifdef _DEBUG
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) {
            debug1->SetEnableGPUBasedValidation(TRUE);
            out.gpu_validation = true;
        }
#endif
        return out;
    }();
    return layer;
}

struct Driver {
    std::string name;
    ComPtr<IDXGIAdapter1> adapter;
    bool warp = false;
};

std::string narrow(const wchar_t* text) {
    std::string out;
    for (; *text; ++text) out.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    return out;
}

// Every hardware adapter that creates a feature-level 11_0 device, then WARP.
const std::vector<Driver>& drivers() {
    static const std::vector<Driver> list = [] {
        std::vector<Driver> out;
        (void)debug_layer();
        ComPtr<IDXGIFactory4> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            checked(adapter->GetDesc1(&desc), "GetDesc1");
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                    __uuidof(ID3D12Device), nullptr))) continue;
            out.push_back({"hardware " + narrow(desc.Description), adapter, false});
        }
        ComPtr<IDXGIAdapter1> warp;
        checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        DXGI_ADAPTER_DESC1 desc{};
        checked(warp->GetDesc1(&desc), "GetDesc1 WARP");
        out.push_back({"WARP " + narrow(desc.Description), warp, true});
        return out;
    }();
    return list;
}

constexpr float kClear[4] = {0, 0, 0, 0};

struct Harness {
    static constexpr std::uint32_t kSlots = 3;
    const Driver& driver;
    std::uint32_t width, height;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t next_value = 0;
    ComPtr<ID3D12Fence> gate;
    std::uint64_t gate_value = 0;
    HANDLE event = nullptr;
    std::array<ComPtr<ID3D12CommandAllocator>, kSlots> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kSlots> lists;
    std::array<std::uint64_t, kSlots> values{};
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    std::array<ComPtr<ID3D12Resource>, kSlots> readback;
    UINT64 readback_bytes = 0; // the last row is not padded to the pitch
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};

    explicit Harness(const Driver& selected, std::uint32_t size = 128)
        : driver(selected), width(size), height(size) {
        REQUIRE(debug_layer().enabled);
        checked(D3D12CreateDevice(driver.adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        checked(device.As(&info), "query ID3D12InfoQueue");
        checked(info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE), "break corruption");
        checked(info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE), "break error");
        checked(info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE), "break warning");
        info->ClearStoredMessages();

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        checked(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "CreateFence gate");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("CreateEventW failed");
        for (std::uint32_t i = 0; i < kSlots; ++i) {
            checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&allocators[i])), "CreateCommandAllocator");
            checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])), "CreateCommandList");
            checked(lists[i]->Close(), "Close initial list");
        }

        D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC color{};
        color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color.Width = width;
        color.Height = height;
        color.DepthOrArraySize = 1;
        color.MipLevels = 1;
        color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        color.SampleDesc.Count = 1;
        color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = color.Format;
        std::copy(std::begin(kClear), std::end(kClear), clear.Color);
        checked(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &color,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)),
            "CreateCommittedResource target");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        checked(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap RTV");
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);

        UINT64 total = 0;
        device->GetCopyableFootprints(&color, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        readback_bytes = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (auto& resource : readback)
            checked(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
                "CreateCommittedResource readback");
    }

    ~Harness() {
        try { idle(); } catch (...) {}
        if (event) CloseHandle(event);
    }

    wui::d3d12::Config config() const {
        wui::d3d12::Config out;
        out.device = device.Get();
        return out;
    }

    wui::d3d12::Target target_view() const { return {rtv, width, height}; }

    void begin(std::uint32_t i) {
        checked(allocators[i]->Reset(), "Reset allocator");
        checked(lists[i]->Reset(allocators[i].Get(), nullptr), "Reset list");
        values[i] = ++next_value;
    }

    wui::d3d12::FrameSlot slot(std::uint32_t i) const { return {i, fence.Get(), values[i]}; }

    void clear(std::uint32_t i) { lists[i]->ClearRenderTargetView(rtv, kClear, 0, nullptr); }

    void transition(std::uint32_t i, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        lists[i]->ResourceBarrier(1, &barrier);
    }

    // Copies the target into this slot's readback buffer and closes the list.
    void finish(std::uint32_t i) {
        transition(i, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback[i].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        lists[i]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(i, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        checked(lists[i]->Close(), "Close list");
    }

    void execute(std::uint32_t i) {
        ID3D12CommandList* submitted[] = {lists[i].Get()};
        queue->ExecuteCommandLists(1, submitted);
        checked(queue->Signal(fence.Get(), values[i]), "Signal fence");
    }

    // Real GPU-side hold: later submissions wait on a gate fence the CPU
    // signals only in release_hold().
    void hold() { checked(queue->Wait(gate.Get(), ++gate_value), "Wait gate"); }
    void release_hold() { checked(gate->Signal(gate_value), "Signal gate"); }

    void wait_value(std::uint64_t value) {
        if (fence->GetCompletedValue() >= value) return;
        checked(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU completion timed out after 10 seconds; removal hr=" +
                std::to_string(static_cast<unsigned long>(device->GetDeviceRemovedReason())));
    }
    void wait(std::uint32_t i) { wait_value(values[i]); }
    void idle() {
        if (gate_value) release_hold();
        const auto value = ++next_value;
        checked(queue->Signal(fence.Get(), value), "Signal idle");
        wait_value(value);
    }

    wui::test::Image pixels(std::uint32_t i) const {
        void* mapped = nullptr;
        const D3D12_RANGE read{0, static_cast<SIZE_T>(readback_bytes)};
        checked(readback[i]->Map(0, &read, &mapped), "Map readback");
        wui::test::Image image;
        image.width = width;
        image.height = height;
        image.pixels.resize(static_cast<std::size_t>(width) * height);
        const auto* rows = static_cast<const std::uint8_t*>(mapped);
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* row = rows + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
            for (std::uint32_t x = 0; x < width; ++x)
                image.pixels[static_cast<std::size_t>(y) * width + x] =
                    {row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]};
        }
        const D3D12_RANGE written{0, 0};
        readback[i]->Unmap(0, &written);
        return image;
    }

    // Waits for the queue to drain so GPU-based validation reports arrive,
    // then requires zero warnings, errors and corruption messages.
    void check_messages() {
        idle();
        std::uint64_t counted = 0;
        const UINT64 stored = info->GetNumStoredMessages();
        for (UINT64 i = 0; i < stored; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes)) || bytes == 0) continue;
            std::vector<std::byte> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info->GetMessage(i, message, &bytes))) continue;
            if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
                ++counted;
                MESSAGE(driver.name, " D3D12 debug message ", static_cast<int>(message->ID), ": ",
                    std::string(message->pDescription, message->DescriptionByteLength));
            }
        }
        MESSAGE(driver.name, ": D3D12 debug layer on, GPU-based validation ",
            std::string(debug_layer().gpu_validation ? "on" : "off"), ", warnings+errors=", counted);
        CHECK(counted == 0);
        info->ClearStoredMessages();
    }
};

std::unique_ptr<Renderer> make_renderer(const wui::d3d12::Config& config) {
    auto created = Renderer::create(config);
    return success(created, "Renderer::create");
}

wui::test::Image render(Harness& native, Renderer& renderer, const wui::DrawPacket& packet,
    std::uint32_t slot = 0) {
    native.begin(slot);
    auto prepared = renderer.prepare(native.slot(slot), native.lists[slot].Get(), packet);
    auto ticket = success(prepared, "prepare");
    native.clear(slot);
    const auto recorded = renderer.record(ticket, native.lists[slot].Get(), native.target_view());
    REQUIRE_FALSE(recorded.has_value());
    native.finish(slot);
    native.execute(slot);
    renderer.submitted(std::move(ticket));
    native.wait(slot);
    REQUIRE_FALSE(renderer.collect().has_value());
    return native.pixels(slot);
}

void check_channel(std::uint8_t actual, std::uint8_t expected, int tolerance) {
    CHECK(std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= tolerance);
}

void check_pixel(wui::test::Pixel actual, std::uint8_t r, std::uint8_t g, std::uint8_t b,
    std::uint8_t a, int tolerance = 2) {
    INFO("pixel ", int(actual.r), ",", int(actual.g), ",", int(actual.b), ",", int(actual.a));
    check_channel(actual.r, r, tolerance);
    check_channel(actual.g, g, tolerance);
    check_channel(actual.b, b, tolerance);
    check_channel(actual.a, a, tolerance);
}

bool same_pixels(const wui::test::Image& a, const wui::test::Image& b) {
    return a.width == b.width && a.height == b.height &&
        std::equal(a.pixels.begin(), a.pixels.end(), b.pixels.begin(), b.pixels.end(),
            [](const wui::test::Pixel& x, const wui::test::Pixel& y) {
                return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
            });
}

std::string file_tag(const Driver& driver, std::size_t index) {
    return driver.warp ? std::string("warp") : "hw" + std::to_string(index);
}

std::size_t checker_index(const wui::DrawPacket& packet) {
    for (std::size_t i = 0; i < packet->textures.size(); ++i)
        if (packet->textures[i]->width == 4 && packet->textures[i]->height == 4) return i;
    throw std::runtime_error("reference packet has no checker texture");
}

bool resident(const Renderer& renderer, std::uint64_t revision, std::uint32_t handle_slot,
    std::uint64_t* references = nullptr) {
    for (const auto& texture : wui::d3d12::detail::Inspection::resident(renderer)) {
        if (texture.key.slot == handle_slot && texture.key.revision == revision) {
            if (references) *references = texture.references;
            return true;
        }
    }
    return false;
}

std::uint64_t simulated_device_removed(ID3D12Fence*) {
    return std::numeric_limits<std::uint64_t>::max();
}

} // namespace

TEST_CASE("D3D12 finds feature-level 11_0 hardware and WARP adapters with the debug layer") {
    REQUIRE(debug_layer().enabled);
    MESSAGE("D3D12 debug layer enabled; GPU-based validation ",
        std::string(debug_layer().gpu_validation ? "enabled (Debug build)" : "not enabled (Release build)"));
    std::size_t hardware = 0, warp = 0;
    for (const auto& driver : drivers()) {
        MESSAGE("D3D12 adapter: ", driver.name);
        (driver.warp ? warp : hardware) += 1;
    }
    CHECK(hardware >= 1);
    CHECK(warp == 1);
}

TEST_CASE("D3D12 rejects invalid configuration with specific statuses") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto expect = [](wui::d3d12::Config config, wui::ErrorCode code) {
            auto created = Renderer::create(config);
            REQUIRE(std::holds_alternative<wui::Error>(created));
            CHECK(std::get<wui::Error>(created).code == code);
        };
        auto config = native.config();
        config.device = nullptr;
        expect(config, wui::ErrorCode::invalid_argument);
        config = native.config();
        config.max_frames = 0;
        expect(config, wui::ErrorCode::invalid_argument);
        config.max_frames = 65;
        expect(config, wui::ErrorCode::invalid_argument);
        config = native.config();
        config.max_textures = 0;
        expect(config, wui::ErrorCode::invalid_argument);
        config = native.config();
        config.sample_count = 0;
        expect(config, wui::ErrorCode::invalid_argument);
        config = native.config();
        config.rtv_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        expect(config, wui::ErrorCode::unsupported);
        config = native.config();
        config.max_frames = 4;
        config.max_textures = 300'000; // 1.2M exceeds the shader-visible binding limit
        expect(config, wui::ErrorCode::capacity);

        // A packet with more textures than the slot's SRV range is rejected
        // before anything is recorded or any slot is consumed.
        config = native.config();
        config.max_textures = 1;
        auto renderer = make_renderer(config);
        const auto packet = wui::test::reference_packet(1);
        REQUIRE(packet->textures.size() > 1);
        native.begin(0);
        auto prepared = renderer->prepare(native.slot(0), native.lists[0].Get(), packet);
        REQUIRE(std::holds_alternative<wui::Error>(prepared));
        CHECK(std::get<wui::Error>(prepared).code == wui::ErrorCode::capacity);
        // Invalid slot, list and fence arguments.
        auto bad_index = renderer->prepare({3, native.fence.Get(), 1}, native.lists[0].Get(), packet);
        REQUIRE(std::holds_alternative<wui::Error>(bad_index));
        CHECK(std::get<wui::Error>(bad_index).code == wui::ErrorCode::invalid_argument);
        auto no_list = renderer->prepare(native.slot(0), nullptr, packet);
        REQUIRE(std::holds_alternative<wui::Error>(no_list));
        CHECK(std::get<wui::Error>(no_list).code == wui::ErrorCode::invalid_argument);
        auto reached = renderer->prepare({0, native.fence.Get(), 0}, native.lists[0].Get(), packet);
        REQUIRE(std::holds_alternative<wui::Error>(reached));
        CHECK(std::get<wui::Error>(reached).code == wui::ErrorCode::invalid_frame);
        checked(native.lists[0]->Close(), "Close unused list");
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 renders the reference fixture revisions 1 and 2") {
    for (std::size_t index = 0; index < drivers().size(); ++index) {
        const auto& driver = drivers()[index];
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto first = wui::test::reference_packet(1);
        REQUIRE(first->batches.size() >= 2);
        REQUIRE(first->batches[1].base_vertex != 0);
        const auto image = render(native, *renderer, first);
        // Same expectations as the D3D11 reference test.
        check_pixel(image.at(12, 12), 255, 0, 0, 255);
        check_pixel(image.at(30, 30), 128, 128, 0, 255, 8);
        check_pixel(image.at(70, 60), 26, 26, 26, 255, 8);
        check_pixel(image.at(100, 100), 26, 26, 26, 255, 8);
        // Clip, background and checker texels.
        check_pixel(image.at(84, 76), 0, 0, 255, 255, 8);
        check_pixel(image.at(12, 116), 26, 26, 26, 255, 8);
        check_pixel(image.at(75, 11), 255, 255, 255, 255, 8);
        check_pixel(image.at(76, 12), 225, 225, 225, 255, 3);

        const auto second = wui::test::reference_packet(2);
        const auto revised = render(native, *renderer, second);
        check_pixel(revised.at(75, 11), 255, 0, 0, 255, 8);
        check_pixel(revised.at(76, 12), 225, 1, 1, 255, 3);
        check_pixel(revised.at(84, 12), 30, 30, 30, 255, 3);
        bool changed = false;
        for (std::uint32_t y = 8; y < 40; ++y)
            for (std::uint32_t x = 72; x < 104; ++x) {
                const auto a = image.at(x, y), b = revised.at(x, y);
                if (a.r != b.r || a.g != b.g || a.b != b.b) changed = true;
            }
        CHECK(changed);
        const auto tag = file_tag(driver, index);
        CHECK_FALSE(wui::test::write_ppm(image, "d3d12-" + tag + "-rev1.ppm").has_value());
        CHECK_FALSE(wui::test::write_ppm(revised, "d3d12-" + tag + "-rev2.ppm").has_value());
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 renders scale 2 and rejects incompatible targets without consuming tickets") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver, 256);
        auto renderer = make_renderer(native.config());
        const auto packet = wui::test::reference_packet(1, 2.0f);
        native.begin(0);
        auto prepared = renderer->prepare(native.slot(0), native.lists[0].Get(), packet);
        auto ticket = success(prepared, "prepare scale 2");
        for (const auto bad : {
                wui::d3d12::Target{{0}, 256, 256},
                wui::d3d12::Target{native.rtv, 128, 128},
                wui::d3d12::Target{native.rtv, 256, 0}}) {
            const auto error = renderer->record(ticket, native.lists[0].Get(), bad);
            REQUIRE(error.has_value());
            CHECK(error->code == wui::ErrorCode::invalid_argument);
        }
        const auto no_list = renderer->record(ticket, nullptr, native.target_view());
        REQUIRE(no_list.has_value());
        CHECK(no_list->code == wui::ErrorCode::invalid_argument);
        native.clear(0);
        REQUIRE_FALSE(renderer->record(ticket, native.lists[0].Get(), native.target_view()).has_value());
        const auto twice = renderer->record(ticket, native.lists[0].Get(), native.target_view());
        REQUIRE(twice.has_value());
        CHECK(twice->code == wui::ErrorCode::invalid_frame);
        native.finish(0);
        native.execute(0);
        renderer->submitted(std::move(ticket));
        native.wait(0);
        CHECK_FALSE(renderer->collect().has_value());
        const auto image = native.pixels(0);
        check_pixel(image.at(24, 24), 255, 0, 0, 255);
        check_pixel(image.at(60, 60), 128, 128, 0, 255, 8);
        check_pixel(image.at(140, 120), 26, 26, 26, 255, 8);
        check_pixel(image.at(24, 232), 26, 26, 26, 255, 8);
        check_pixel(image.at(152, 24), 239, 239, 239, 255, 3);
        check_pixel(image.at(168, 24), 16, 16, 16, 255, 3);
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 distinguishes straight and premultiplied texture alpha") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto base = wui::test::reference_packet(1);
        const auto checker = checker_index(base);
        auto variant = [&](wui::AlphaMode alpha, std::uint64_t revision) {
            auto packet = std::make_shared<wui::PacketData>(*base);
            auto texture = std::make_shared<wui::TextureSnapshot>(*base->textures[checker]);
            texture->revision = revision;
            texture->alpha = alpha;
            for (std::size_t i = 0; i < texture->rgba.size(); i += 4) {
                texture->rgba[i] = std::byte{255};
                texture->rgba[i + 1] = std::byte{0};
                texture->rgba[i + 2] = std::byte{0};
                texture->rgba[i + 3] = std::byte{128};
            }
            packet->textures[checker] = std::move(texture);
            REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
            return wui::DrawPacket(std::move(packet));
        };
        // Straight (255,0,0,128) is premultiplied in the shader to 128/255 red
        // over the 0.1 background: 0.502 + 0.1 * 0.498 -> 141.
        const auto straight = render(native, *renderer, variant(wui::AlphaMode::straight, 10));
        check_pixel(straight.at(88, 24), 141, 13, 13, 255, 3);
        // The same bytes declared premultiplied add full red: saturates at 255.
        const auto premultiplied =
            render(native, *renderer, variant(wui::AlphaMode::premultiplied, 11));
        check_pixel(premultiplied.at(88, 24), 255, 13, 13, 255, 3);
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 clips fractional rectangles and ignores empty batches") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        auto packet = std::make_shared<wui::PacketData>(*wui::test::reference_packet(1));
        auto blue = std::find_if(packet->batches.begin(), packet->batches.end(),
            [](const wui::Batch& batch) {
                return batch.clip.x == 80 && batch.clip.y == 72 &&
                    batch.clip.w == 16 && batch.clip.h == 16;
            });
        REQUIRE(blue != packet->batches.end());
        blue->clip = {-10.25f, -10.25f, 106.0f, 98.0f};
        packet->batches.push_back({0, 0, 0, 0, {0, 0, 0, 0}});
        REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
        const auto image = render(native, *renderer, packet);
        check_pixel(image.at(95, 87), 0, 0, 255, 255, 8);
        check_pixel(image.at(96, 87), 26, 26, 26, 255, 8);
        check_pixel(image.at(95, 88), 26, 26, 26, 255, 8);

        // A packet with no batches records nothing but still completes a slot.
        auto empty = std::make_shared<wui::PacketData>(*wui::test::reference_packet(1));
        empty->batches.clear();
        REQUIRE_FALSE(wui::validate_packet(*empty).has_value());
        const auto cleared = render(native, *renderer, empty);
        check_pixel(cleared.at(12, 12), 0, 0, 0, 0, 0);
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 snaps fractional-scale clip edges that land a few ulps short of a pixel") {
    // At scale 1.25, 81.6f and 77.6f map to 101.9999981 and 96.9999981. An
    // outward floor of those would add a whole fringe column and row.
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver, 160);
        auto renderer = make_renderer(native.config());
        auto packet = std::make_shared<wui::PacketData>(*wui::test::reference_packet(1, 1.25f));
        auto blue = std::find_if(packet->batches.begin(), packet->batches.end(),
            [](const wui::Batch& batch) {
                return batch.clip.x == 80 && batch.clip.y == 72 &&
                    batch.clip.w == 16 && batch.clip.h == 16;
            });
        REQUIRE(blue != packet->batches.end());
        blue->clip = {81.6f, 77.6f, 50.0f, 50.0f};
        REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
        const auto image = render(native, *renderer, packet);
        check_pixel(image.at(102, 97), 0, 0, 255, 255, 8);
        check_pixel(image.at(105, 96), 26, 26, 26, 255, 8);
        check_pixel(image.at(101, 100), 26, 26, 26, 255, 8);
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 does not share native textures between snapshots with equal handles") {
    // Two TextureStores can hand out the same handle slot, generation and
    // revision for different pixels. The renderer must key its native
    // textures by snapshot identity so each packet samples its own pixels.
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto original = wui::test::reference_packet(1);
        auto copy = std::make_shared<wui::PacketData>(*original);
        const auto checker = checker_index(original);
        auto blue = std::make_shared<wui::TextureSnapshot>(*copy->textures[checker]);
        for (std::size_t i = 0; i < blue->rgba.size(); i += 4) {
            blue->rgba[i + 0] = std::byte{0};
            blue->rgba[i + 1] = std::byte{0};
            blue->rgba[i + 2] = std::byte{255};
            blue->rgba[i + 3] = std::byte{255};
        }
        copy->textures[checker] = std::move(blue);
        REQUIRE_FALSE(wui::validate_packet(*copy).has_value());
        const wui::DrawPacket other(std::move(copy));

        const auto first = render(native, *renderer, original);
        const auto second = render(native, *renderer, other);
        const auto third = render(native, *renderer, original);
        bool differs = false, restored = true;
        for (std::uint32_t y = 8; y < 40; ++y)
            for (std::uint32_t x = 72; x < 104; ++x) {
                const auto a = first.at(x, y), b = second.at(x, y), c = third.at(x, y);
                if (a.r != b.r || a.g != b.g || a.b != b.b) differs = true;
                if (a.r != c.r || a.g != c.g || a.b != c.b || a.a != c.a) restored = false;
            }
        CHECK(differs);
        CHECK(restored);
        check_pixel(second.at(75, 11), 0, 0, 255, 255, 8);
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 grows a slot upload buffer for a 1024x1024 texture") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto base = wui::test::reference_packet(1);
        const auto expected = render(native, *renderer, base);
        // 4 MiB of texel data exceeds the 256 KiB initial slot upload buffer.
        auto large = std::make_shared<wui::TextureSnapshot>();
        large->handle = {999, 1};
        large->revision = 1;
        large->width = large->height = 1024;
        large->alpha = wui::AlphaMode::premultiplied;
        large->rgba.assign(std::size_t{1024} * 1024 * 4, std::byte{0x40});
        auto packet = std::make_shared<wui::PacketData>(*base);
        packet->textures.push_back(large);
        REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
        const auto image = render(native, *renderer, packet);
        CHECK(same_pixels(expected, image));
        CHECK(resident(*renderer, 1, 999));
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 holds three slots in flight, reports busy and retires revisions after fences") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto old = wui::test::reference_packet(1);
        const auto newer = wui::test::reference_packet(2);
        const auto checker = checker_index(old);
        const auto checker_slot = old->textures[checker]->handle.slot;
        auto latest = std::make_shared<wui::PacketData>(*newer);
        auto changed = std::make_shared<wui::TextureSnapshot>(*newer->textures[checker]);
        changed->revision = 3;
        changed->rgba[0] = std::byte{0};
        changed->rgba[1] = std::byte{255};
        latest->textures[checker] = changed;
        REQUIRE_FALSE(wui::validate_packet(*latest).has_value());
        const std::array<wui::DrawPacket, 3> packets{old, newer, latest};

        // The GPU cannot start until the host releases the gate, so all three
        // fences are genuinely pending (no simulated completion values).
        native.hold();
        for (std::uint32_t i = 0; i < 3; ++i) {
            native.begin(i);
            auto prepared = renderer->prepare(native.slot(i), native.lists[i].Get(), packets[i]);
            auto ticket = success(prepared, "prepare in flight");
            native.clear(i);
            REQUIRE_FALSE(renderer->record(ticket, native.lists[i].Get(), native.target_view()).has_value());
            native.finish(i);
            native.execute(i);
            renderer->submitted(std::move(ticket));
        }
        for (std::uint64_t revision = 1; revision <= 3; ++revision) {
            std::uint64_t references = 0;
            CHECK(resident(*renderer, revision, checker_slot, &references));
            CHECK(references == (1ull << (revision - 1)));
        }
        CHECK(native.fence->GetCompletedValue() < native.values[0]);
        auto fourth = renderer->prepare({0, native.fence.Get(), native.next_value + 1},
            native.lists[0].Get(), old);
        REQUIRE(std::holds_alternative<wui::Error>(fourth));
        CHECK(std::get<wui::Error>(fourth).code == wui::ErrorCode::busy);
        CHECK_FALSE(renderer->collect().has_value());
        CHECK(resident(*renderer, 1, checker_slot));
        CHECK(resident(*renderer, 2, checker_slot));
        const auto pending = renderer->shutdown();
        REQUIRE(pending.has_value());
        CHECK(pending->code == wui::ErrorCode::busy);

        native.release_hold();
        for (std::uint32_t i = 0; i < 3; ++i) native.wait(i);
        CHECK_FALSE(renderer->collect().has_value());
        // Superseded revisions retire once every referencing fence completed.
        CHECK_FALSE(resident(*renderer, 1, checker_slot));
        CHECK_FALSE(resident(*renderer, 2, checker_slot));
        CHECK(resident(*renderer, 3, checker_slot));
        check_pixel(native.pixels(2).at(75, 11), 0, 255, 0, 255, 8);
        check_pixel(native.pixels(0).at(75, 11), 255, 255, 255, 255, 8);
        check_pixel(native.pixels(1).at(75, 11), 255, 0, 0, 255, 8);

        // Revision 1 renders again after its native texture was retired.
        const auto rerendered = render(native, *renderer, old);
        check_pixel(rerendered.at(75, 11), 255, 255, 255, 255, 8);
        check_pixel(rerendered.at(12, 12), 255, 0, 0, 255);
        CHECK(resident(*renderer, 1, checker_slot));
        CHECK_FALSE(resident(*renderer, 3, checker_slot));
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 abandons recorded but unexecuted lists and recovers") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto packet = wui::test::reference_packet(1);
        for (const bool recorded : {false, true}) {
            native.begin(0);
            auto prepared = renderer->prepare(native.slot(0), native.lists[0].Get(), packet);
            auto ticket = success(prepared, "prepare abandoned frame");
            CHECK_FALSE(wui::d3d12::detail::Inspection::resident(*renderer).empty());
            if (recorded) {
                native.clear(0);
                REQUIRE_FALSE(renderer->record(ticket, native.lists[0].Get(), native.target_view()).has_value());
            }
            // The host discards the list without executing it.
            checked(native.lists[0]->Close(), "Close discarded list");
            renderer->abandon(std::move(ticket));
            // Its never-executed uploads are released at once.
            CHECK(wui::d3d12::detail::Inspection::resident(*renderer).empty());
        }
        // A ticket dropped without submission is abandoned by its destructor.
        {
            native.begin(1);
            auto prepared = renderer->prepare(native.slot(1), native.lists[1].Get(), packet);
            auto ticket = success(prepared, "prepare dropped frame");
            checked(native.lists[1]->Close(), "Close dropped list");
        }
        CHECK(wui::d3d12::detail::Inspection::resident(*renderer).empty());
        const auto recovered = render(native, *renderer, packet);
        check_pixel(recovered.at(12, 12), 255, 0, 0, 255);
        check_pixel(recovered.at(75, 11), 255, 255, 255, 255, 8);
        CHECK_FALSE(renderer->shutdown().has_value());
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("D3D12 shutdown reports busy while a submitted fence is pending") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto packet = wui::test::reference_packet(1);
        native.hold();
        native.begin(0);
        auto prepared = renderer->prepare(native.slot(0), native.lists[0].Get(), packet);
        auto ticket = success(prepared, "prepare pending frame");
        native.clear(0);
        REQUIRE_FALSE(renderer->record(ticket, native.lists[0].Get(), native.target_view()).has_value());
        native.finish(0);
        native.execute(0);
        renderer->submitted(std::move(ticket));
        const auto busy = renderer->shutdown();
        REQUIRE(busy.has_value());
        CHECK(busy->code == wui::ErrorCode::busy);
        native.release_hold();
        native.wait(0);
        CHECK_FALSE(renderer->shutdown().has_value());
        // Shutdown is idempotent and later frames are refused.
        CHECK_FALSE(renderer->shutdown().has_value());
        native.begin(1);
        auto refused = renderer->prepare(native.slot(1), native.lists[1].Get(), packet);
        REQUIRE(std::holds_alternative<wui::Error>(refused));
        CHECK(std::get<wui::Error>(refused).code == wui::ErrorCode::invalid_frame);
        checked(native.lists[1]->Close(), "Close refused list");
        renderer.reset();
        native.check_messages();
    }
}

TEST_CASE("simulated D3D12 device removal through UINT64_MAX completion is terminal") {
    for (const auto& driver : drivers()) {
        INFO(driver.name);
        MESSAGE(driver.name, ": device removal is SIMULATED by an injected completed-value "
            "source; the physical device is never removed");
        Harness native(driver);
        auto renderer = make_renderer(native.config());
        const auto packet = wui::test::reference_packet(1);
        native.begin(0);
        auto prepared = renderer->prepare(native.slot(0), native.lists[0].Get(), packet);
        auto ticket = success(prepared, "prepare before simulated loss");
        native.clear(0);
        REQUIRE_FALSE(renderer->record(ticket, native.lists[0].Get(), native.target_view()).has_value());
        native.finish(0);
        native.execute(0);
        renderer->submitted(std::move(ticket));
        native.wait(0); // The real device is healthy; only the next query is injected.
        const auto previous = wui::d3d12::detail::Inspection::replace_completed_value(
            *renderer, &simulated_device_removed);
        REQUIRE(previous != nullptr);
        const auto failed = renderer->collect();
        REQUIRE(failed.has_value());
        CHECK(failed->code == wui::ErrorCode::device_lost);
        // Terminal: restoring the genuine source does not revive the renderer.
        wui::d3d12::detail::Inspection::replace_completed_value(*renderer, previous);
        native.begin(1);
        auto retry = renderer->prepare(native.slot(1), native.lists[1].Get(), packet);
        REQUIRE(std::holds_alternative<wui::Error>(retry));
        CHECK(std::get<wui::Error>(retry).code == wui::ErrorCode::device_lost);
        checked(native.lists[1]->Close(), "Close refused list");
        const auto again = renderer->collect();
        REQUIRE(again.has_value());
        CHECK(again->code == wui::ErrorCode::device_lost);
        const auto terminal = renderer->shutdown();
        REQUIRE(terminal.has_value());
        CHECK(terminal->code == wui::ErrorCode::device_lost);
        renderer.reset();

        // The host reconstructs a renderer; the retained packet renders again.
        auto replacement = make_renderer(native.config());
        const auto image = render(native, *replacement, packet, 2);
        check_pixel(image.at(12, 12), 255, 0, 0, 255);
        CHECK_FALSE(replacement->shutdown().has_value());
        replacement.reset();
        native.check_messages();
    }
}
