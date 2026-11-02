#pragma once

#include <bit>
#include <cstdint>

namespace wui::detail {

// Branch-free finiteness test. A float is infinite or NaN exactly when its
// exponent field is all ones. Adding one exponent unit to the isolated
// exponent field carries into bit 31 only in that case ((0xff + 1) << 23 ==
// 1u << 31; every smaller exponent stays below it). The words of many values
// can therefore be OR-ed together and tested once against nonfinite_flag,
// and for every single value the test agrees exactly with std::isfinite.
inline constexpr std::uint32_t nonfinite_flag = 0x80000000u;

inline std::uint32_t nonfinite_bits(float value) {
    return (std::bit_cast<std::uint32_t>(value) & 0x7f800000u) + 0x00800000u;
}

} // namespace wui::detail
