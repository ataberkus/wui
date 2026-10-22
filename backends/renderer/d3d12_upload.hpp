#pragma once

// Device-free upload and lifetime bookkeeping for the D3D12 renderer. No SDK
// headers: also compiled into wui_d3d12_lifetime_tests. The native-only test
// hooks at the end refer to D3D12 types by forward declaration only.
#include <wui/types.hpp>

#include <cstdint>
#include <optional>
#include <vector>

struct ID3D12Fence;

namespace wui::d3d12 { class Renderer; }

namespace wui::d3d12::detail {

// D3D12_TEXTURE_DATA_PITCH_ALIGNMENT and D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT.
inline constexpr std::uint64_t kPitchAlignment = 256;
inline constexpr std::uint64_t kPlacementAlignment = 512;

// nullopt when rounding up overflows. `alignment` must be a power of two.
std::optional<std::uint64_t> align_up(std::uint64_t value, std::uint64_t alignment) noexcept;

// Placed RGBA8 footprint for a buffer-to-texture copy.
struct Footprint {
    std::uint64_t row_pitch = 0;
    std::uint64_t bytes = 0;
};
// invalid_argument for an empty texture, unsupported beyond `max_dimension`.
Result<Footprint> texture_footprint(std::uint32_t width, std::uint32_t height,
    std::uint32_t max_dimension);

// Ring allocator over one upload-heap buffer. Positions are monotonically
// increasing virtual byte offsets; the physical offset is position modulo
// capacity. A block that does not fit before the end of the buffer wraps to
// offset 0, and the skipped tail stays live until released. Allocations
// never overlap bytes that have not been released.
class UploadRing {
public:
    explicit UploadRing(std::uint64_t capacity = 0) noexcept : capacity_(capacity) {}
    // Empties the ring with a new capacity (after its buffer was replaced).
    void reset(std::uint64_t capacity) noexcept;
    std::uint64_t capacity() const noexcept { return capacity_; }
    std::uint64_t used() const noexcept { return head_ - tail_; }
    // Marker for release_to(): everything allocated so far.
    std::uint64_t position() const noexcept { return head_; }
    // Physical offset of a `bytes` block aligned to `alignment` (a power of
    // two). capacity when it would overlap live bytes; nothing is consumed.
    Result<std::uint64_t> allocate(std::uint64_t bytes, std::uint64_t alignment);
    // Releases every allocation made before `marker` (an earlier position()).
    Status release_to(std::uint64_t marker);

private:
    std::uint64_t capacity_ = 0;
    std::uint64_t head_ = 0;
    std::uint64_t tail_ = 0;
};

enum class TicketState { free, prepared, recorded, submitted, lost };

// Handle and revision alone are not unique: two TextureStores can issue the
// same (slot, generation, revision) for different pixels. A native texture
// is shared only with the same snapshot object (`identity`) of the same
// extent and alpha mode. Entries whose snapshot expired never match again,
// so a reused snapshot address cannot alias an old texture.
struct TextureKey {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    std::uint64_t revision = 0;
    std::uint32_t width = 0, height = 0;
    std::uint32_t alpha = 0;
    const void* identity = nullptr;
    bool operator==(const TextureKey&) const = default;
};

// Frame-slot tickets plus texture residency. A texture entry records which
// frame slots reference it; it retires only when no slot references it and it
// is superseded by another revision of the same handle, its CPU snapshot
// expired, or its upload was abandoned unexecuted. Completion values come
// from the caller (the host fence, or a fake in tests).
class FrameTracker {
public:
    static constexpr std::uint32_t kMaxSlots = 64;

    explicit FrameTracker(std::uint32_t slots = 0);

    std::uint32_t slots() const noexcept { return static_cast<std::uint32_t>(tickets_.size()); }
    TicketState state(std::uint32_t slot) const noexcept;
    std::uint64_t completion_value(std::uint32_t slot) const noexcept;
    bool lost() const noexcept { return lost_; }

    Status prepare(std::uint32_t slot, std::uint64_t completion_value);
    Status record(std::uint32_t slot);
    Status submit(std::uint32_t slot);
    // The slot's command lists were discarded unexecuted: its pending uploads
    // are orphaned and its references dropped.
    Status abandon(std::uint32_t slot);
    // busy until `completed` reaches the slot's completion value. UINT64_MAX
    // is device removal: every live ticket becomes lost and stays lost.
    Status retire(std::uint32_t slot, std::uint64_t completed);
    void lose() noexcept;

    struct Use {
        std::uint32_t entry = 0;
        bool created = false; // the caller must upload into this new entry
    };
    // References `key` from a prepared slot. An upload still pending in a
    // different, unsubmitted slot is never shared: a fresh entry is created.
    Result<Use> use(std::uint32_t slot, const TextureKey& key);
    // True when use() would reuse an existing entry without an upload.
    bool resident(std::uint32_t slot, const TextureKey& key) const noexcept;
    void expire(std::uint32_t entry) noexcept;
    // Frees retirable entries and appends their indices to `retired`.
    void retire_unreferenced(std::vector<std::uint32_t>& retired);

    std::uint32_t entry_count() const noexcept { return static_cast<std::uint32_t>(entries_.size()); }
    bool live(std::uint32_t entry) const noexcept;
    TextureKey key(std::uint32_t entry) const noexcept;
    std::uint64_t references(std::uint32_t entry) const noexcept;

private:
    struct Ticket {
        TicketState state = TicketState::free;
        std::uint64_t completion = 0;
    };
    struct Entry {
        TextureKey key;
        std::uint64_t references = 0;
        std::uint32_t uploader = 0;
        bool live = false, pending = false, superseded = false, expired = false, orphaned = false;
    };
    std::uint32_t find(std::uint32_t slot, const TextureKey& key) const noexcept;
    void supersede_others(const TextureKey& key, std::uint32_t keep) noexcept;
    void release(std::uint32_t slot) noexcept;

    std::vector<Ticket> tickets_;
    std::vector<Entry> entries_;
    std::vector<std::uint32_t> free_entries_;
    bool lost_ = false;
};

// Native-only test inspection, defined in d3d12.cpp. Never part of a packet.
struct ResidentTexture {
    TextureKey key;
    const void* resource = nullptr;
    std::uint64_t references = 0;
};
using CompletedValueFn = std::uint64_t (*)(ID3D12Fence*);
struct Inspection {
    static std::vector<ResidentTexture> resident(const Renderer& renderer);
    // Replaces the fence completed-value query (simulated device removal).
    static CompletedValueFn replace_completed_value(Renderer& renderer,
        CompletedValueFn replacement) noexcept;
};

} // namespace wui::d3d12::detail
