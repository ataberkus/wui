#pragma once

#include <cstdint>

namespace wui::example {

struct AllocationSample {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
};

void begin_allocation_sample() noexcept;
AllocationSample end_allocation_sample() noexcept;

}  // namespace wui::example
