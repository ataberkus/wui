#include <doctest/doctest.h>

#include "d3d12_descriptors.hpp"
#include "d3d12_upload.hpp"

#include <cstdint>
#include <limits>
#include <variant>
#include <vector>

// Device-free D3D12 bookkeeping: descriptor ranges, upload ring arithmetic,
// frame-slot retirement and texture residency. Completion is driven by fake
// fence values supplied by the test, never by a GPU.
namespace {
using wui::ErrorCode;
using wui::d3d12::detail::DescriptorRanges;
using wui::d3d12::detail::FrameTracker;
using wui::d3d12::detail::TextureKey;
using wui::d3d12::detail::TicketState;
using wui::d3d12::detail::UploadRing;

void rejects(const wui::Status& status, ErrorCode code) {
    REQUIRE(status.has_value());
    CHECK(status->code == code);
}

template<class T>
void rejects(const wui::Result<T>& result, ErrorCode code) {
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == code);
}

template<class T>
T value(const wui::Result<T>& result) {
    REQUIRE(std::holds_alternative<T>(result));
    return std::get<T>(result);
}

constexpr std::uint32_t kHeapLimit = 1'000'000;

// A fake fence: the host "signals" by assigning completed.
struct FakeFence {
    std::uint64_t completed = 0;
};

void submit(FrameTracker& tracker, std::uint32_t slot) {
    REQUIRE_FALSE(tracker.record(slot).has_value());
    REQUIRE_FALSE(tracker.submit(slot).has_value());
}

std::vector<std::uint32_t> retire(FrameTracker& tracker) {
    std::vector<std::uint32_t> retired;
    tracker.retire_unreferenced(retired);
    return retired;
}
} // namespace

TEST_CASE("D3D12 descriptor ranges partition the heap per frame slot and wrap on reuse") {
    auto created = DescriptorRanges::create(3, 256, kHeapLimit);
    auto ranges = value(created);
    CHECK(ranges.slots() == 3);
    CHECK(ranges.per_slot() == 256);
    CHECK(ranges.total() == 768);
    CHECK(ranges.first(0) == 0);
    CHECK(ranges.first(1) == 256);
    CHECK(ranges.first(2) == 512);

    CHECK(value(ranges.allocate(1)) == 256);
    CHECK(value(ranges.allocate(1)) == 257);
    CHECK(ranges.used(1) == 2);
    CHECK(ranges.used(0) == 0);
    // Slot 0 is independent of slot 1.
    CHECK(value(ranges.allocate(0)) == 0);

    // Reusing a retired slot wraps its cursor back to the slot's first descriptor.
    REQUIRE_FALSE(ranges.reset(1).has_value());
    CHECK(ranges.used(1) == 0);
    CHECK(value(ranges.allocate(1)) == 256);

    // Filling the range exactly succeeds; one more is a capacity error that
    // consumes nothing and never spills into the next slot's range.
    CHECK(value(ranges.allocate(1, 255)) == 257);
    CHECK(ranges.used(1) == 256);
    rejects(ranges.allocate(1), ErrorCode::capacity);
    CHECK(ranges.used(1) == 256);
    CHECK(ranges.used(2) == 0);
    rejects(ranges.allocate(2, 257), ErrorCode::capacity);
    CHECK(ranges.used(2) == 0);

    rejects(ranges.allocate(3), ErrorCode::invalid_argument);
    rejects(ranges.allocate(0, 0), ErrorCode::invalid_argument);
    rejects(ranges.reset(3), ErrorCode::invalid_argument);
}

TEST_CASE("D3D12 descriptor heap sizing rejects zero, overflow and the binding limit") {
    rejects(DescriptorRanges::create(0, 256, kHeapLimit), ErrorCode::invalid_argument);
    rejects(DescriptorRanges::create(3, 0, kHeapLimit), ErrorCode::invalid_argument);
    // 32-bit multiplication overflow is reported, not wrapped.
    rejects(DescriptorRanges::create(std::numeric_limits<std::uint32_t>::max(), 2, kHeapLimit),
        ErrorCode::capacity);
    rejects(DescriptorRanges::create(4, 300'000, kHeapLimit), ErrorCode::capacity);
    CHECK(value(DescriptorRanges::create(4, 250'000, kHeapLimit)).total() == kHeapLimit);
}

TEST_CASE("D3D12 texture footprints align rows and reject oversized textures") {
    using wui::d3d12::detail::Footprint;
    using wui::d3d12::detail::texture_footprint;
    const auto small = value(texture_footprint(4, 4, 16384));
    CHECK(small.row_pitch == 256);
    CHECK(small.bytes == 1024);
    const auto exact = value(texture_footprint(64, 1, 16384));
    CHECK(exact.row_pitch == 256);
    CHECK(exact.bytes == 256);
    const auto wide = value(texture_footprint(65, 2, 16384));
    CHECK(wide.row_pitch == 512);
    CHECK(wide.bytes == 1024);
    const auto atlas = value(texture_footprint(1024, 1024, 16384));
    CHECK(atlas.row_pitch == 4096);
    CHECK(atlas.bytes == 4096u * 1024u);
    rejects(texture_footprint(0, 4, 16384), ErrorCode::invalid_argument);
    rejects(texture_footprint(4, 0, 16384), ErrorCode::invalid_argument);
    rejects(texture_footprint(16385, 1, 16384), ErrorCode::unsupported);
    rejects(texture_footprint(1, 16385, 16384), ErrorCode::unsupported);

    using wui::d3d12::detail::align_up;
    CHECK(align_up(0, 512) == 0u);
    CHECK(align_up(1, 512) == 512u);
    CHECK(align_up(512, 512) == 512u);
    CHECK_FALSE(align_up(std::numeric_limits<std::uint64_t>::max() - 3, 512).has_value());
}

TEST_CASE("D3D12 upload ring allocates aligned, wraps and rejects overflow without consuming") {
    UploadRing ring(1024);
    CHECK(ring.capacity() == 1024);
    CHECK(ring.used() == 0);
    CHECK(value(ring.allocate(300, 256)) == 0);
    const auto first_end = ring.position();
    CHECK(value(ring.allocate(300, 256)) == 512);
    const auto second_end = ring.position();
    CHECK(ring.used() == 812);

    // The ring is full enough that a 400-byte block cannot fit until the
    // first allocation is released; the failure consumes nothing.
    rejects(ring.allocate(400, 256), ErrorCode::capacity);
    CHECK(ring.position() == second_end);
    REQUIRE_FALSE(ring.release_to(first_end).has_value());
    CHECK(ring.used() == 512);
    // Still blocked: the tail would wrap over the second, live allocation.
    rejects(ring.allocate(400, 256), ErrorCode::capacity);
    REQUIRE_FALSE(ring.release_to(second_end).has_value());
    CHECK(ring.used() == 0);

    // The tail of the buffer (812..1024) is too short: the block wraps to 0.
    CHECK(value(ring.allocate(400, 256)) == 0);
    CHECK(ring.used() == (1024 - 812) + 400);
    const auto wrapped_end = ring.position();
    // 400 -> aligned 512; 512 + 700 > 1024 would wrap over live bytes.
    rejects(ring.allocate(700, 256), ErrorCode::capacity);
    CHECK(ring.position() == wrapped_end);
    REQUIRE_FALSE(ring.release_to(wrapped_end).has_value());
    CHECK(value(ring.allocate(512, 512)) == 512);
    CHECK(ring.used() == 624); // 112 alignment padding + 512 bytes

    rejects(ring.allocate(1025, 1), ErrorCode::capacity);
    rejects(ring.allocate(0, 256), ErrorCode::invalid_argument);
    rejects(ring.allocate(16, 3), ErrorCode::invalid_argument);
    // Release markers must lie between the current tail and head.
    rejects(ring.release_to(ring.position() + 1), ErrorCode::invalid_argument);
    rejects(ring.release_to(second_end), ErrorCode::invalid_argument);

    ring.reset(2048);
    CHECK(ring.capacity() == 2048);
    CHECK(ring.used() == 0);
    CHECK(value(ring.allocate(2048, 512)) == 0);
    UploadRing empty;
    rejects(empty.allocate(1, 1), ErrorCode::capacity);
}

TEST_CASE("D3D12 frame slots retire only when the fake fence reaches their value") {
    FrameTracker tracker(3);
    FakeFence fence;
    CHECK(tracker.slots() == 3);
    rejects(tracker.prepare(3, 1), ErrorCode::invalid_argument);
    rejects(tracker.record(0), ErrorCode::invalid_frame);
    rejects(tracker.submit(0), ErrorCode::invalid_frame);
    rejects(tracker.retire(0, fence.completed), ErrorCode::invalid_frame);
    rejects(tracker.abandon(0), ErrorCode::invalid_frame);

    REQUIRE_FALSE(tracker.prepare(0, 5).has_value());
    CHECK(tracker.completion_value(0) == 5);
    rejects(tracker.prepare(0, 6), ErrorCode::busy);
    rejects(tracker.submit(0), ErrorCode::invalid_frame);
    submit(tracker, 0);
    CHECK(tracker.state(0) == TicketState::submitted);
    rejects(tracker.prepare(0, 6), ErrorCode::busy);
    rejects(tracker.abandon(0), ErrorCode::invalid_frame);

    fence.completed = 4;
    rejects(tracker.retire(0, fence.completed), ErrorCode::busy);
    CHECK(tracker.state(0) == TicketState::submitted);
    fence.completed = 5;
    REQUIRE_FALSE(tracker.retire(0, fence.completed).has_value());
    CHECK(tracker.state(0) == TicketState::free);

    // Three slots can be in flight at once; each retires independently.
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE_FALSE(tracker.prepare(i, 10 + i).has_value());
        submit(tracker, i);
    }
    fence.completed = 11;
    REQUIRE_FALSE(tracker.retire(0, fence.completed).has_value());
    REQUIRE_FALSE(tracker.retire(1, fence.completed).has_value());
    rejects(tracker.retire(2, fence.completed), ErrorCode::busy);
    CHECK(tracker.state(2) == TicketState::submitted);
}

TEST_CASE("D3D12 textures retire only after every referencing slot completes") {
    FrameTracker tracker(3);
    FakeFence fence;
    const TextureKey rev1{7, 1, 1};
    const TextureKey rev2{7, 1, 2};
    const TextureKey other{8, 1, 1};

    REQUIRE_FALSE(tracker.prepare(0, 1).has_value());
    rejects(tracker.use(1, rev1), ErrorCode::invalid_frame);
    const auto a = value(tracker.use(0, rev1));
    CHECK(a.created);
    const auto o = value(tracker.use(0, other));
    CHECK(o.created);
    // A duplicate reference in the same frame reuses the pending upload.
    const auto again = value(tracker.use(0, rev1));
    CHECK_FALSE(again.created);
    CHECK(again.entry == a.entry);
    submit(tracker, 0);

    REQUIRE_FALSE(tracker.prepare(1, 2).has_value());
    const auto b = value(tracker.use(1, rev1));
    CHECK_FALSE(b.created);
    CHECK(b.entry == a.entry);
    submit(tracker, 1);

    REQUIRE_FALSE(tracker.prepare(2, 3).has_value());
    const auto c = value(tracker.use(2, rev2));
    CHECK(c.created);
    CHECK(c.entry != a.entry);
    submit(tracker, 2);
    CHECK(tracker.references(a.entry) == 0b011);
    CHECK(tracker.references(c.entry) == 0b100);

    // Revision 1 is superseded but slots 0 and 1 still reference it.
    CHECK(retire(tracker).empty());
    fence.completed = 1;
    REQUIRE_FALSE(tracker.retire(0, fence.completed).has_value());
    rejects(tracker.retire(1, fence.completed), ErrorCode::busy);
    CHECK(retire(tracker).empty());
    CHECK(tracker.live(a.entry));
    fence.completed = 2;
    REQUIRE_FALSE(tracker.retire(1, fence.completed).has_value());
    const auto retired = retire(tracker);
    REQUIRE(retired.size() == 1);
    CHECK(retired[0] == a.entry);
    CHECK_FALSE(tracker.live(a.entry));
    // The current revision and the untouched texture stay resident when idle.
    fence.completed = 3;
    REQUIRE_FALSE(tracker.retire(2, fence.completed).has_value());
    CHECK(retire(tracker).empty());
    CHECK(tracker.live(c.entry));
    CHECK(tracker.live(o.entry));
    CHECK(tracker.key(c.entry) == rev2);

    // Revision 1 renders again after retirement: it is a fresh upload that
    // reuses the freed entry, and revision 2 now waits for slot 0.
    REQUIRE_FALSE(tracker.prepare(0, 4).has_value());
    const auto d = value(tracker.use(0, rev1));
    CHECK(d.created);
    CHECK(d.entry == a.entry);
    submit(tracker, 0);
    CHECK(retire(tracker) == std::vector<std::uint32_t>{c.entry});
    fence.completed = 4;
    REQUIRE_FALSE(tracker.retire(0, fence.completed).has_value());
    CHECK(retire(tracker).empty());

    // An expired CPU snapshot retires once no slot references it.
    tracker.expire(o.entry);
    CHECK(retire(tracker) == std::vector<std::uint32_t>{o.entry});
}

TEST_CASE("D3D12 texture keys include extent, alpha and snapshot identity") {
    FrameTracker tracker(3);
    int store_a = 0, store_b = 0; // stand-ins for two distinct snapshots
    const TextureKey a{1, 1, 1, 4, 4, 0, &store_a};
    REQUIRE_FALSE(tracker.prepare(0, 1).has_value());
    const auto first = value(tracker.use(0, a));
    CHECK(first.created);
    submit(tracker, 0);
    REQUIRE_FALSE(tracker.prepare(1, 2).has_value());
    // Same handle and revision from another store: never shared.
    TextureKey other = a;
    other.identity = &store_b;
    CHECK_FALSE(tracker.resident(1, other));
    CHECK(value(tracker.use(1, other)).created);
    TextureKey wider = a;
    wider.width = 8;
    CHECK(value(tracker.use(1, wider)).created);
    TextureKey premultiplied = a;
    premultiplied.alpha = 1;
    CHECK(value(tracker.use(1, premultiplied)).created);
    // The original snapshot is still matched by identity.
    CHECK(tracker.resident(1, a));
    // Once its snapshot expired, an address-equal key never matches it again.
    tracker.expire(first.entry);
    CHECK_FALSE(tracker.resident(1, a));
}

TEST_CASE("D3D12 abandoned uploads are orphaned and never shared before submission") {
    FrameTracker tracker(3);
    const TextureKey key{3, 2, 9};
    REQUIRE_FALSE(tracker.prepare(0, 1).has_value());
    const auto first = value(tracker.use(0, key));
    CHECK(first.created);
    // Slot 1 cannot rely on slot 0's unexecuted copy: it uploads its own.
    REQUIRE_FALSE(tracker.prepare(1, 2).has_value());
    const auto second = value(tracker.use(1, key));
    CHECK(second.created);
    CHECK(second.entry != first.entry);
    REQUIRE_FALSE(tracker.record(0).has_value());
    REQUIRE_FALSE(tracker.abandon(0).has_value());
    CHECK(tracker.state(0) == TicketState::free);
    CHECK(retire(tracker) == std::vector<std::uint32_t>{first.entry});
    submit(tracker, 1);
    REQUIRE_FALSE(tracker.retire(1, 2).has_value());
    CHECK(retire(tracker).empty());
    CHECK(tracker.live(second.entry));

    // A prepared-only frame can be abandoned too.
    REQUIRE_FALSE(tracker.prepare(2, 3).has_value());
    const auto third = value(tracker.use(2, TextureKey{4, 1, 1}));
    REQUIRE_FALSE(tracker.abandon(2).has_value());
    CHECK(retire(tracker) == std::vector<std::uint32_t>{third.entry});
}

TEST_CASE("D3D12 completed value UINT64_MAX is terminal device loss") {
    FrameTracker tracker(3);
    REQUIRE_FALSE(tracker.prepare(0, 1).has_value());
    submit(tracker, 0);
    REQUIRE_FALSE(tracker.prepare(1, 2).has_value());
    rejects(tracker.retire(0, std::numeric_limits<std::uint64_t>::max()),
        ErrorCode::device_lost);
    CHECK(tracker.lost());
    CHECK(tracker.state(0) == TicketState::lost);
    CHECK(tracker.state(1) == TicketState::lost);
    CHECK(tracker.state(2) == TicketState::free);
    rejects(tracker.prepare(2, 3), ErrorCode::device_lost);
    rejects(tracker.record(1), ErrorCode::device_lost);
    rejects(tracker.retire(0, 1), ErrorCode::device_lost);
    rejects(tracker.abandon(1), ErrorCode::device_lost);
    rejects(tracker.use(1, TextureKey{1, 1, 1}), ErrorCode::device_lost);
}
