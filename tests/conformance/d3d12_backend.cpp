#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "conformance.hpp"

#include <wui/backends/d3d12.hpp>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wui::conformance {
namespace {

using Microsoft::WRL::ComPtr;

Error hr_error(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed hr=0x%08lx", what, static_cast<unsigned long>(hr));
    return Error{ErrorCode::backend_failure, text};
}

// Marks a device so recreate() can prove D3D12CreateDevice returned a new one
// rather than the per-adapter singleton.
constexpr GUID kDeviceMarker{0x5c1e7a3d, 0x2b9f, 0x4e61, {0x9a, 0x0d, 0x71, 0x3c, 0x55, 0xe2, 0x18, 0x4b}};

constexpr std::uint32_t kFrames = 3;

class D3D12Backend final : public OffscreenBackend {
public:
    ~D3D12Backend() override {
        if (renderer_) {
            (void)collect_all();
            (void)renderer_->shutdown();
        }
    }

    Status initialize(const Options& options) {
        warp_ = options.driver != "hardware";
        name_ = warp_ ? "d3d12-warp" : "d3d12-hardware";
        enable_debug_layer();
        if (auto error = create_device()) return error;
        return create_renderer();
    }

    std::string_view name() const override { return name_; }
    std::string adapter_description() const override { return adapter_; }
    bool debug_layer() const override { return debug_; }

    Result<Image> render(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return *error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto status = submit(packet, w, h, true);
            if (!status) break;
            if (status->code != ErrorCode::busy) return *status;
            if (std::chrono::steady_clock::now() > deadline) return Error{ErrorCode::busy, "render busy for 10s"};
            if (auto error = collect_all()) return *error;
        }
        if (auto error = collect_all()) return *error;
        void* mapped = nullptr;
        const D3D12_RANGE read{0, static_cast<SIZE_T>(readback_bytes_)};
        const HRESULT hr = readback_->Map(0, &read, &mapped);
        if (FAILED(hr)) return hr_error("Map(readback)", hr);
        Image image{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(image.rgba.data() + static_cast<std::size_t>(y) * w * 4,
                static_cast<const std::uint8_t*>(mapped) + footprint_.Offset +
                    static_cast<std::size_t>(y) * footprint_.Footprint.RowPitch,
                static_cast<std::size_t>(w) * 4);
        }
        const D3D12_RANGE written{0, 0};
        readback_->Unmap(0, &written);
        return image;
    }

    Status submit_in_flight(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return error;
        // Hold the queue behind a gate fence that only collect_all() signals, so
        // no submission can complete (and free its slot) before the renderer's
        // frames-in-flight capacity is reached. Nothing here waits on the CPU.
        if (!gated_) {
            const HRESULT hr = queue_->Wait(gate_.Get(), gate_value_ + 1);
            if (FAILED(hr)) return hr_error("ID3D12CommandQueue::Wait(gate)", hr);
            gated_ = true;
        }
        return submit(packet, w, h, false);
    }

    Status collect_all() override {
        if (gated_) {
            const HRESULT hr = gate_->Signal(++gate_value_);
            if (FAILED(hr)) return hr_error("ID3D12Fence::Signal(gate)", hr);
            gated_ = false;
        }
        if (auto error = wait_for(fence_value_)) return error;
        return renderer_->collect();
    }

    Status recreate() override {
        if (auto error = collect_all()) return error;
        if (auto error = renderer_->shutdown()) return error;
        renderer_.reset();
        drain_messages();
        // D3D12CreateDevice returns the existing device for an adapter while
        // any reference to it is alive, so release every device object first.
        release_device();
        // create_device() fails if the new device carries the old one's marker.
        if (auto error = create_device()) return error;
        return create_renderer();
    }

    std::uint64_t debug_messages() override {
        drain_messages();
        const auto total = messages_;
        messages_ = 0;
        return total;
    }

private:
    void enable_debug_layer() {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) return;
        debug->EnableDebugLayer();
        debug_ = true;
#ifdef _DEBUG
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) {
            debug1->SetEnableGPUBasedValidation(TRUE);
            gpu_validation_ = true;
        }
#endif
    }

    Status create_device() {
        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr_error("CreateDXGIFactory2", hr);
        ComPtr<IDXGIAdapter1> adapter;
        if (warp_) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        else hr = factory->EnumAdapters1(0, &adapter); // the default adapter
        if (FAILED(hr)) return hr_error(warp_ ? "EnumWarpAdapter" : "EnumAdapters1(0)", hr);
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (FAILED(hr)) return hr_error("D3D12CreateDevice", hr);
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[256]{};
            std::snprintf(name, sizeof(name), "%ls (vendor 0x%04X device 0x%04X, FL 11_0%s)",
                desc.Description, desc.VendorId, desc.DeviceId,
                gpu_validation_ ? ", GPU-based validation" : "");
            adapter_ = name;
        }
        if (debug_ && SUCCEEDED(device_.As(&info_))) {
            info_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            info_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            info_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        }
        UINT marker_size = 0;
        if (device_->GetPrivateData(kDeviceMarker, &marker_size, nullptr) != DXGI_ERROR_NOT_FOUND)
            return Error{ErrorCode::backend_failure, "D3D12CreateDevice returned a previously used device"};
        const std::uint32_t marker = 1;
        device_->SetPrivateData(kDeviceMarker, sizeof(marker), &marker);

        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) return hr_error("CreateCommandQueue", hr);
        for (std::uint32_t i = 0; i < kFrames; ++i) {
            hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]));
            if (FAILED(hr)) return hr_error("CreateCommandAllocator", hr);
            hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[i].Get(), nullptr,
                IID_PPV_ARGS(&lists_[i]));
            if (FAILED(hr)) return hr_error("CreateCommandList", hr);
            lists_[i]->Close();
            slot_values_[i] = 0;
        }
        // Open list handed to prepare() only when a slot is still in flight:
        // the renderer refuses with busy before recording anything into it.
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probe_allocator_));
        if (FAILED(hr)) return hr_error("CreateCommandAllocator(probe)", hr);
        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, probe_allocator_.Get(), nullptr,
            IID_PPV_ARGS(&probe_));
        if (FAILED(hr)) return hr_error("CreateCommandList(probe)", hr);
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) return hr_error("CreateFence", hr);
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_));
        if (FAILED(hr)) return hr_error("CreateFence(gate)", hr);
        fence_value_ = gate_value_ = 0;
        gated_ = false;
        next_slot_ = 0;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        hr = device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_));
        if (FAILED(hr)) return hr_error("CreateDescriptorHeap(RTV)", hr);
        return std::nullopt;
    }

    Status create_renderer() {
        d3d12::Config config;
        config.device = device_.Get();
        config.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        config.max_frames = kFrames;
        auto created = d3d12::Renderer::create(config);
        if (auto* error = std::get_if<Error>(&created)) return *error;
        renderer_ = std::get<std::unique_ptr<d3d12::Renderer>>(std::move(created));
        return std::nullopt;
    }

    void release_device() {
        target_.Reset();
        readback_.Reset();
        rtv_heap_.Reset();
        target_w_ = target_h_ = 0;
        probe_.Reset();
        probe_allocator_.Reset();
        for (auto& list : lists_) list.Reset();
        for (auto& allocator : allocators_) allocator.Reset();
        fence_.Reset();
        gate_.Reset();
        queue_.Reset();
        info_.Reset();
        device_.Reset();
    }

    Status ensure_target(std::uint32_t w, std::uint32_t h) {
        if (target_ && target_w_ == w && target_h_ == h) return std::nullopt;
        // The old target may still be referenced by queued work.
        if (target_) {
            if (auto error = collect_all()) return error;
        }
        target_.Reset();
        readback_.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int i = 0; i < 4; ++i) clear.Color[i] = kClear[i] / 255.0f;
        HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target_));
        if (FAILED(hr)) return hr_error("CreateCommittedResource(target)", hr);
        device_->CreateRenderTargetView(target_.Get(), nullptr, rtv_heap_->GetCPUDescriptorHandleForHeapStart());
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_));
        if (FAILED(hr)) return hr_error("CreateCommittedResource(readback)", hr);
        readback_bytes_ = bytes;
        target_w_ = w;
        target_h_ = h;
        return std::nullopt;
    }

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    // Records and executes one frame on the next slot. With `readback` the
    // target is cleared and copied to the readback buffer after drawing.
    Status submit(const DrawPacket& packet, std::uint32_t w, std::uint32_t h, bool readback) {
        const std::uint32_t index = next_slot_;
        const d3d12::FrameSlot slot{index, fence_.Get(), fence_value_ + 1};
        if (fence_->GetCompletedValue() < slot_values_[index]) {
            // The slot's allocator is still in use; the renderer must refuse it.
            auto refused = renderer_->prepare(slot, probe_.Get(), packet);
            if (auto* error = std::get_if<Error>(&refused)) return *error;
            renderer_->abandon(std::get<d3d12::PreparedFrame>(std::move(refused)));
            return Error{ErrorCode::backend_failure, "renderer accepted a slot whose fence has not completed"};
        }
        auto& allocator = allocators_[index];
        auto& list = lists_[index];
        HRESULT hr = allocator->Reset();
        if (FAILED(hr)) return hr_error("ID3D12CommandAllocator::Reset", hr);
        hr = list->Reset(allocator.Get(), nullptr);
        if (FAILED(hr)) return hr_error("ID3D12GraphicsCommandList::Reset", hr);
        auto prepared = renderer_->prepare(slot, list.Get(), packet);
        if (auto* error = std::get_if<Error>(&prepared)) {
            list->Close();
            return *error;
        }
        auto ticket = std::get<d3d12::PreparedFrame>(std::move(prepared));
        const auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        if (readback) {
            const float clear[4]{kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, 1.0f};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        }
        if (auto error = renderer_->record(ticket, list.Get(), d3d12::Target{rtv, w, h})) {
            list->Close();
            renderer_->abandon(std::move(ticket));
            return error;
        }
        if (readback) {
            auto barrier = transition(target_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback_.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprint_;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target_.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier = transition(target_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &barrier);
        }
        hr = list->Close();
        if (FAILED(hr)) {
            renderer_->abandon(std::move(ticket));
            return hr_error("ID3D12GraphicsCommandList::Close", hr);
        }
        ID3D12CommandList* lists[]{list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        hr = queue_->Signal(fence_.Get(), slot.completion_value);
        renderer_->submitted(std::move(ticket));
        if (FAILED(hr)) return hr_error("ID3D12CommandQueue::Signal", hr);
        fence_value_ = slot.completion_value;
        slot_values_[index] = slot.completion_value;
        next_slot_ = (index + 1) % kFrames;
        return std::nullopt;
    }

    Status wait_for(std::uint64_t value) {
        if (fence_->GetCompletedValue() >= value) return std::nullopt;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return Error{ErrorCode::backend_failure, "CreateEvent failed"};
        HRESULT hr = fence_->SetEventOnCompletion(value, event);
        DWORD waited = WAIT_OBJECT_0;
        if (SUCCEEDED(hr)) waited = WaitForSingleObject(event, 10000);
        CloseHandle(event);
        if (FAILED(hr)) return hr_error("SetEventOnCompletion", hr);
        if (waited != WAIT_OBJECT_0) return Error{ErrorCode::backend_failure, "GPU wait 10s"};
        return std::nullopt;
    }

    void drain_messages() {
        if (!info_) return;
        const UINT64 stored = info_->GetNumStoredMessages();
        for (UINT64 i = 0; i < stored; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info_->GetMessage(i, nullptr, &bytes)) || bytes < sizeof(D3D12_MESSAGE)) continue;
            std::vector<std::byte> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info_->GetMessage(i, message, &bytes))) continue;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                ++messages_;
                std::fprintf(stderr, "d3d12 debug: %.*s\n", static_cast<int>(message->DescriptionByteLength),
                    message->pDescription);
            }
        }
        info_->ClearStoredMessages();
    }

    bool warp_ = true;
    bool debug_ = false;
    bool gpu_validation_ = false;
    std::string name_, adapter_ = "unknown";
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12InfoQueue> info_;
    ComPtr<ID3D12CommandQueue> queue_;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrames> allocators_;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kFrames> lists_;
    std::array<std::uint64_t, kFrames> slot_values_{};
    ComPtr<ID3D12CommandAllocator> probe_allocator_;
    ComPtr<ID3D12GraphicsCommandList> probe_;
    ComPtr<ID3D12Fence> fence_, gate_;
    std::uint64_t fence_value_ = 0, gate_value_ = 0;
    bool gated_ = false;
    std::uint32_t next_slot_ = 0;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12Resource> target_, readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    std::uint64_t readback_bytes_ = 0;
    std::uint32_t target_w_ = 0, target_h_ = 0;
    std::uint64_t messages_ = 0;
    std::unique_ptr<d3d12::Renderer> renderer_;
};

}  // namespace

Result<std::unique_ptr<OffscreenBackend>> make_backend(const Options& options) {
    auto backend = std::make_unique<D3D12Backend>();
    if (auto error = backend->initialize(options)) return *error;
    return std::unique_ptr<OffscreenBackend>(std::move(backend));
}

}  // namespace wui::conformance
