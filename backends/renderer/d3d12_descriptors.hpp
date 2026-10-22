#pragma once

// Device-free descriptor bookkeeping for the D3D12 renderer. No SDK types:
// also compiled into wui_d3d12_lifetime_tests.
#include <wui/types.hpp>

#include <cstdint>
#include <vector>

namespace wui::d3d12::detail {

// One shader-visible CBV/SRV/UAV heap partitioned into a fixed, contiguous
// range per frame slot: slot i owns [i * per_slot, (i + 1) * per_slot).
// A slot's range is only rewritten after that slot's previous frame retired,
// so descriptors referenced by in-flight command lists are never overwritten.
class DescriptorRanges {
public:
    DescriptorRanges() = default;
    // invalid_argument for zero slots or range size; capacity when the total
    // overflows 32 bits or exceeds `heap_limit`.
    static Result<DescriptorRanges> create(std::uint32_t slots, std::uint32_t per_slot,
        std::uint32_t heap_limit);

    std::uint32_t slots() const noexcept { return slots_; }
    std::uint32_t per_slot() const noexcept { return per_slot_; }
    std::uint32_t total() const noexcept { return slots_ * per_slot_; }
    std::uint32_t first(std::uint32_t slot) const noexcept { return slot * per_slot_; }
    std::uint32_t used(std::uint32_t slot) const noexcept;

    // Restarts the slot's cursor at its first descriptor (wrap on reuse).
    Status reset(std::uint32_t slot);
    // Returns the absolute heap index of the first of `count` descriptors.
    // capacity when the slot's range is exhausted; nothing is consumed then.
    Result<std::uint32_t> allocate(std::uint32_t slot, std::uint32_t count = 1);

private:
    std::uint32_t slots_ = 0;
    std::uint32_t per_slot_ = 0;
    std::vector<std::uint32_t> used_;
};

} // namespace wui::d3d12::detail
