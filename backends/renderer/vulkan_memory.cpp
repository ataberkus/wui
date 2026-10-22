#include "vulkan_memory.hpp"

#include <bit>

namespace wui::vulkan::detail {
namespace {
Status invalid() { return Error{ErrorCode::invalid_frame, "Invalid Vulkan frame transition"}; }
Status busy() { return Error{ErrorCode::busy, "Vulkan frame is still in flight"}; }
Status lost() { return Error{ErrorCode::device_lost, "Vulkan device is lost"}; }
} // namespace

Status SlotState::prepare() {
    if (state == TicketState::lost) return lost();
    if (state != TicketState::free) return busy();
    state = TicketState::prepared;
    return std::nullopt;
}

Status SlotState::record() {
    if (state == TicketState::lost) return lost();
    if (state != TicketState::prepared) return invalid();
    state = TicketState::recorded;
    return std::nullopt;
}

Status SlotState::submit() {
    if (state == TicketState::lost) return lost();
    if (state != TicketState::recorded) return invalid();
    state = TicketState::submitted;
    return std::nullopt;
}

Status SlotState::retire(bool fence_signaled) {
    if (state == TicketState::lost) return lost();
    if (state != TicketState::submitted) return invalid();
    if (!fence_signaled) return busy();
    state = TicketState::free;
    return std::nullopt;
}

Status SlotState::abandon(bool command_buffer_discarded) {
    if (state == TicketState::lost) return lost();
    if (state != TicketState::prepared && state != TicketState::recorded) return invalid();
    if (!command_buffer_discarded) return invalid();
    state = TicketState::free;
    return std::nullopt;
}

void SlotState::lose() noexcept {
    if (state != TicketState::free) state = TicketState::lost;
}

std::optional<std::uint32_t> select_memory_type(std::uint32_t compatible,
    std::uint32_t required_types, std::uint32_t preferred_types) noexcept {
    const auto required = compatible & required_types;
    if (!required) return std::nullopt;
    const auto preferred = required & preferred_types;
    return static_cast<std::uint32_t>(std::countr_zero(preferred ? preferred : required));
}

MappedRange atom_aligned_range(std::uint64_t offset, std::uint64_t size,
    std::uint64_t atom, std::uint64_t allocation_size) noexcept {
    if (size == 0) return {offset, 0};
    if (atom <= 1) return {offset, size};
    const std::uint64_t begin = offset - offset % atom;
    const std::uint64_t end = offset + size;
    const std::uint64_t remainder = end % atom;
    const std::uint64_t aligned_end = remainder ? end + (atom - remainder) : end;
    // VkMappedMemoryRange: size is a multiple of the atom, or the range ends at the allocation end.
    if (aligned_end >= allocation_size) return {begin, allocation_size - begin};
    return {begin, aligned_end - begin};
}

} // namespace wui::vulkan::detail
