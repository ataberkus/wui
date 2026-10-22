#pragma once

#include <wui/types.hpp>
#include <cstdint>
#include <optional>

namespace wui::vulkan::detail {

enum class TicketState { free, prepared, recorded, submitted, lost };

struct SlotState {
    TicketState state = TicketState::free;
    Status prepare();
    Status record();
    Status submit();
    Status retire(bool fence_signaled);
    Status abandon(bool command_buffer_discarded);
    void lose() noexcept;
};

// A mapped-memory range for vkFlushMappedMemoryRanges/vkInvalidateMappedMemoryRanges:
// [offset, offset + size) widened to nonCoherentAtomSize boundaries and clamped so
// that a range ending past the allocation ends exactly at `allocation_size`.
struct MappedRange {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
MappedRange atom_aligned_range(std::uint64_t offset, std::uint64_t size,
    std::uint64_t atom, std::uint64_t allocation_size) noexcept;

// Arguments are bitsets of physical memory type indices, not Vulkan flag values.
std::optional<std::uint32_t> select_memory_type(std::uint32_t compatible,
    std::uint32_t required_types, std::uint32_t preferred_types) noexcept;

} // namespace wui::vulkan::detail
