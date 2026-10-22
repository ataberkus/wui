#pragma once

#include <cstdint>
#include <string_view>

namespace wui {

struct Id {
    std::uint64_t value = 0;
    bool operator==(const Id&) const = default;
};

Id child_id(Id parent, std::string_view key);

} // namespace wui
