#include "d3d12_upload.hpp"

#include <limits>

namespace wui::d3d12::detail {
namespace {
constexpr std::uint32_t kNoEntry = std::numeric_limits<std::uint32_t>::max();

bool power_of_two(std::uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

Error lost_error() { return Error{ErrorCode::device_lost, "D3D12 device is removed"}; }
Error slot_error() { return Error{ErrorCode::invalid_argument, "D3D12 frame slot is out of range"}; }
} // namespace

std::optional<std::uint64_t> align_up(std::uint64_t value, std::uint64_t alignment) noexcept {
    if (!power_of_two(alignment)) return std::nullopt;
    if (value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) return std::nullopt;
    return (value + alignment - 1) & ~(alignment - 1);
}

Result<Footprint> texture_footprint(std::uint32_t width, std::uint32_t height,
    std::uint32_t max_dimension) {
    if (width == 0 || height == 0)
        return Error{ErrorCode::invalid_argument, "D3D12 texture has no texels"};
    if (width > max_dimension || height > max_dimension)
        return Error{ErrorCode::unsupported, "D3D12 texture exceeds the 2D dimension limit"};
    // Both factors are below 2^32, so neither the pitch nor the product overflows.
    const auto pitch = align_up(std::uint64_t{width} * 4, kPitchAlignment);
    if (!pitch) return Error{ErrorCode::capacity, "D3D12 texture row pitch overflows"};
    return Footprint{*pitch, *pitch * height};
}

void UploadRing::reset(std::uint64_t capacity) noexcept {
    capacity_ = capacity;
    head_ = tail_ = 0;
}

Result<std::uint64_t> UploadRing::allocate(std::uint64_t bytes, std::uint64_t alignment) {
    if (bytes == 0 || !power_of_two(alignment))
        return Error{ErrorCode::invalid_argument, "Invalid D3D12 upload allocation"};
    if (bytes > capacity_)
        return Error{ErrorCode::capacity, "D3D12 upload exceeds the ring capacity"};
    const std::uint64_t physical = head_ % capacity_;
    const std::uint64_t lap = head_ - physical;
    const auto aligned = align_up(physical, alignment);
    std::uint64_t start = 0;
    if (aligned && *aligned <= capacity_ && bytes <= capacity_ - *aligned)
        start = lap + *aligned;
    else
        start = lap + capacity_; // Wrap: offset 0 of the next lap is always aligned.
    const std::uint64_t end = start + bytes;
    if (end - tail_ > capacity_)
        return Error{ErrorCode::capacity, "D3D12 upload ring is full"};
    head_ = end;
    return start % capacity_;
}

Status UploadRing::release_to(std::uint64_t marker) {
    if (marker < tail_ || marker > head_)
        return Error{ErrorCode::invalid_argument, "D3D12 upload release marker is out of order"};
    tail_ = marker;
    return std::nullopt;
}

FrameTracker::FrameTracker(std::uint32_t slots) : tickets_(slots) {}

TicketState FrameTracker::state(std::uint32_t slot) const noexcept {
    return slot < tickets_.size() ? tickets_[slot].state : TicketState::free;
}

std::uint64_t FrameTracker::completion_value(std::uint32_t slot) const noexcept {
    return slot < tickets_.size() ? tickets_[slot].completion : 0;
}

Status FrameTracker::prepare(std::uint32_t slot, std::uint64_t completion_value) {
    if (lost_) return lost_error();
    if (slot >= tickets_.size()) return slot_error();
    auto& ticket = tickets_[slot];
    if (ticket.state != TicketState::free)
        return Error{ErrorCode::busy, "D3D12 frame slot is in use"};
    ticket.state = TicketState::prepared;
    ticket.completion = completion_value;
    return std::nullopt;
}

Status FrameTracker::record(std::uint32_t slot) {
    if (lost_) return lost_error();
    if (slot >= tickets_.size()) return slot_error();
    auto& ticket = tickets_[slot];
    if (ticket.state != TicketState::prepared)
        return Error{ErrorCode::invalid_frame, "D3D12 frame is not prepared for recording"};
    ticket.state = TicketState::recorded;
    return std::nullopt;
}

Status FrameTracker::submit(std::uint32_t slot) {
    if (lost_) return lost_error();
    if (slot >= tickets_.size()) return slot_error();
    auto& ticket = tickets_[slot];
    if (ticket.state != TicketState::recorded)
        return Error{ErrorCode::invalid_frame, "D3D12 frame was not recorded"};
    ticket.state = TicketState::submitted;
    // Uploads recorded by this slot are now ordered on the host queue ahead
    // of any later submission, so later frames may share them.
    for (auto& entry : entries_)
        if (entry.live && entry.pending && entry.uploader == slot) entry.pending = false;
    return std::nullopt;
}

Status FrameTracker::abandon(std::uint32_t slot) {
    if (lost_) return lost_error();
    if (slot >= tickets_.size()) return slot_error();
    auto& ticket = tickets_[slot];
    if (ticket.state != TicketState::prepared && ticket.state != TicketState::recorded)
        return Error{ErrorCode::invalid_frame, "Only unsubmitted D3D12 frames can be abandoned"};
    for (auto& entry : entries_) {
        if (entry.live && entry.pending && entry.uploader == slot) {
            entry.pending = false;
            entry.orphaned = true;
        }
    }
    release(slot);
    ticket.state = TicketState::free;
    return std::nullopt;
}

Status FrameTracker::retire(std::uint32_t slot, std::uint64_t completed) {
    if (lost_) return lost_error();
    if (slot >= tickets_.size()) return slot_error();
    auto& ticket = tickets_[slot];
    if (ticket.state != TicketState::submitted)
        return Error{ErrorCode::invalid_frame, "D3D12 frame was not submitted"};
    if (completed == std::numeric_limits<std::uint64_t>::max()) {
        lose();
        return lost_error();
    }
    if (completed < ticket.completion)
        return Error{ErrorCode::busy, "D3D12 frame fence has not completed"};
    release(slot);
    ticket.state = TicketState::free;
    return std::nullopt;
}

void FrameTracker::lose() noexcept {
    lost_ = true;
    for (auto& ticket : tickets_)
        if (ticket.state != TicketState::free) ticket.state = TicketState::lost;
}

std::uint32_t FrameTracker::find(std::uint32_t slot, const TextureKey& key) const noexcept {
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        if (entry.live && !entry.orphaned && !entry.expired && entry.key == key &&
            (!entry.pending || entry.uploader == slot)) return i;
    }
    return kNoEntry;
}

void FrameTracker::supersede_others(const TextureKey& key, std::uint32_t keep) noexcept {
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        auto& entry = entries_[i];
        if (i != keep && entry.live && entry.key.slot == key.slot &&
            entry.key.generation == key.generation) entry.superseded = true;
    }
}

void FrameTracker::release(std::uint32_t slot) noexcept {
    const std::uint64_t bit = std::uint64_t{1} << slot;
    for (auto& entry : entries_) entry.references &= ~bit;
}

bool FrameTracker::resident(std::uint32_t slot, const TextureKey& key) const noexcept {
    return find(slot, key) != kNoEntry;
}

Result<FrameTracker::Use> FrameTracker::use(std::uint32_t slot, const TextureKey& key) {
    if (lost_) return lost_error();
    if (slot >= tickets_.size()) return slot_error();
    if (tickets_[slot].state != TicketState::prepared)
        return Error{ErrorCode::invalid_frame, "D3D12 textures are referenced while preparing"};
    const std::uint64_t bit = std::uint64_t{1} << slot;
    if (const auto found = find(slot, key); found != kNoEntry) {
        auto& entry = entries_[found];
        entry.references |= bit;
        entry.superseded = false;
        supersede_others(key, found);
        return Use{found, false};
    }
    std::uint32_t index = 0;
    if (!free_entries_.empty()) {
        index = free_entries_.back();
        free_entries_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(entries_.size());
        entries_.emplace_back();
    }
    auto& entry = entries_[index];
    entry = Entry{};
    entry.key = key;
    entry.references = bit;
    entry.uploader = slot;
    entry.live = true;
    entry.pending = true;
    supersede_others(key, index);
    return Use{index, true};
}

void FrameTracker::expire(std::uint32_t entry) noexcept {
    if (entry < entries_.size() && entries_[entry].live) entries_[entry].expired = true;
}

void FrameTracker::retire_unreferenced(std::vector<std::uint32_t>& retired) {
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        auto& entry = entries_[i];
        if (!entry.live || entry.references != 0 || entry.pending) continue;
        if (!entry.superseded && !entry.expired && !entry.orphaned) continue;
        entry = Entry{};
        free_entries_.push_back(i);
        retired.push_back(i);
    }
}

bool FrameTracker::live(std::uint32_t entry) const noexcept {
    return entry < entries_.size() && entries_[entry].live;
}

TextureKey FrameTracker::key(std::uint32_t entry) const noexcept {
    return entry < entries_.size() ? entries_[entry].key : TextureKey{};
}

std::uint64_t FrameTracker::references(std::uint32_t entry) const noexcept {
    return entry < entries_.size() ? entries_[entry].references : 0;
}

} // namespace wui::d3d12::detail
