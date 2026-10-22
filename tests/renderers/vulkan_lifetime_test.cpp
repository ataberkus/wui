#include <doctest/doctest.h>

#include "../../backends/renderer/vulkan_memory.hpp"

#include <initializer_list>

namespace {
using wui::ErrorCode;
using wui::vulkan::detail::SlotState;
using wui::vulkan::detail::TicketState;

void rejects(const wui::Status& status, ErrorCode code) {
    REQUIRE(status.has_value());
    CHECK(status->code == code);
}
} // namespace

TEST_CASE("a submitted slot cannot be reused before completion") {
    SlotState slot;
    REQUIRE_FALSE(slot.prepare().has_value());
    REQUIRE_FALSE(slot.record().has_value());
    REQUIRE_FALSE(slot.submit().has_value());
    rejects(slot.prepare(), ErrorCode::busy);
    rejects(slot.retire(false), ErrorCode::busy);
    CHECK(slot.state == TicketState::submitted);
    REQUIRE_FALSE(slot.retire(true).has_value());
    REQUIRE_FALSE(slot.prepare().has_value());
    rejects(slot.abandon(false), ErrorCode::invalid_frame);
    REQUIRE_FALSE(slot.abandon(true).has_value());
    CHECK(slot.state == TicketState::free);
}

TEST_CASE("invalid slot transitions never consume an active ticket") {
    SlotState slot;
    rejects(slot.record(), ErrorCode::invalid_frame);
    rejects(slot.submit(), ErrorCode::invalid_frame);
    rejects(slot.retire(true), ErrorCode::invalid_frame);
    rejects(slot.abandon(true), ErrorCode::invalid_frame);
    CHECK(slot.state == TicketState::free);

    REQUIRE_FALSE(slot.prepare().has_value());
    rejects(slot.submit(), ErrorCode::invalid_frame);
    rejects(slot.abandon(false), ErrorCode::invalid_frame);
    CHECK(slot.state == TicketState::prepared);
    REQUIRE_FALSE(slot.record().has_value());
    rejects(slot.record(), ErrorCode::invalid_frame);
    rejects(slot.abandon(false), ErrorCode::invalid_frame);
    CHECK(slot.state == TicketState::recorded);
    REQUIRE_FALSE(slot.abandon(true).has_value());
    CHECK(slot.state == TicketState::free);
    REQUIRE_FALSE(slot.prepare().has_value());
}

TEST_CASE("device loss is terminal for every live slot state") {
    for (const auto at : {TicketState::prepared, TicketState::recorded,
                          TicketState::submitted}) {
        SlotState slot;
        REQUIRE_FALSE(slot.prepare().has_value());
        if (at == TicketState::recorded || at == TicketState::submitted)
            REQUIRE_FALSE(slot.record().has_value());
        if (at == TicketState::submitted) REQUIRE_FALSE(slot.submit().has_value());
        slot.lose();
        CHECK(slot.state == TicketState::lost);
        rejects(slot.prepare(), ErrorCode::device_lost);
        rejects(slot.record(), ErrorCode::device_lost);
        rejects(slot.submit(), ErrorCode::device_lost);
        rejects(slot.retire(true), ErrorCode::device_lost);
        rejects(slot.abandon(true), ErrorCode::device_lost);
        CHECK(slot.state == TicketState::lost);
    }
    SlotState free;
    free.lose();
    CHECK(free.state == TicketState::free);
}

TEST_CASE("Vulkan upload memory favors coherent compatible types and falls back to visible") {
    using wui::vulkan::detail::select_memory_type;
    // Bits 0 and 2 are host-visible; only 2 is coherent; bit 2 is incompatible.
    CHECK(select_memory_type(0b0011, 0b0101, 0b0100) == 0);
    CHECK(select_memory_type(0b0101, 0b0101, 0b0100) == 2);
    CHECK_FALSE(select_memory_type(0b1000, 0b0101, 0b0100).has_value());
}

TEST_CASE("Vulkan explicit flush/invalidate ranges are nonCoherentAtomSize aligned") {
    using wui::vulkan::detail::atom_aligned_range;
    // Grows to atom boundaries inside the allocation.
    auto range = atom_aligned_range(0, 24, 64, 1024);
    CHECK(range.offset == 0);
    CHECK(range.size == 64);
    range = atom_aligned_range(70, 10, 64, 1024);
    CHECK(range.offset == 64);
    CHECK(range.size == 64);
    range = atom_aligned_range(0, 128, 64, 1024);
    CHECK(range.offset == 0);
    CHECK(range.size == 128);
    range = atom_aligned_range(100, 100, 64, 1024);
    CHECK(range.offset == 64);
    CHECK(range.size == 192);
    // Rounding past the allocation end is clamped so offset + size == allocation size.
    range = atom_aligned_range(0, 1000, 256, 1000);
    CHECK(range.offset == 0);
    CHECK(range.size == 1000);
    range = atom_aligned_range(900, 50, 256, 1000);
    CHECK(range.offset == 768);
    CHECK(range.size == 232);
    // Empty and degenerate inputs.
    CHECK(atom_aligned_range(0, 0, 64, 1024).size == 0);
    range = atom_aligned_range(3, 5, 0, 1024);
    CHECK(range.offset == 3);
    CHECK(range.size == 5);
    range = atom_aligned_range(3, 5, 1, 1024);
    CHECK(range.offset == 3);
    CHECK(range.size == 5);
}
