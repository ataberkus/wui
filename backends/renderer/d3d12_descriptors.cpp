#include "d3d12_descriptors.hpp"

#include <limits>

namespace wui::d3d12::detail {

Result<DescriptorRanges> DescriptorRanges::create(std::uint32_t slots, std::uint32_t per_slot,
    std::uint32_t heap_limit) {
    if (slots == 0 || per_slot == 0)
        return Error{ErrorCode::invalid_argument, "D3D12 descriptor ranges need slots and size"};
    const std::uint64_t total = std::uint64_t{slots} * per_slot;
    if (total > std::numeric_limits<std::uint32_t>::max() || total > heap_limit)
        return Error{ErrorCode::capacity,
            "D3D12 shader-visible descriptor heap exceeds the binding limit"};
    DescriptorRanges ranges;
    ranges.slots_ = slots;
    ranges.per_slot_ = per_slot;
    ranges.used_.assign(slots, 0);
    return ranges;
}

std::uint32_t DescriptorRanges::used(std::uint32_t slot) const noexcept {
    return slot < used_.size() ? used_[slot] : 0;
}

Status DescriptorRanges::reset(std::uint32_t slot) {
    if (slot >= slots_)
        return Error{ErrorCode::invalid_argument, "D3D12 descriptor slot is out of range"};
    used_[slot] = 0;
    return std::nullopt;
}

Result<std::uint32_t> DescriptorRanges::allocate(std::uint32_t slot, std::uint32_t count) {
    if (slot >= slots_ || count == 0)
        return Error{ErrorCode::invalid_argument, "Invalid D3D12 descriptor allocation"};
    if (count > per_slot_ - used_[slot])
        return Error{ErrorCode::capacity, "D3D12 frame slot descriptor range is full"};
    const std::uint32_t index = first(slot) + used_[slot];
    used_[slot] += count;
    return index;
}

} // namespace wui::d3d12::detail
