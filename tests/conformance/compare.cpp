#include "compare.hpp"

#include <algorithm>
#include <cstdlib>

namespace wui::conformance {

CompareResult compare(const Image& reference, const Image& candidate) {
    CompareResult result;
    result.same_size = reference.width == candidate.width && reference.height == candidate.height &&
        reference.rgba.size() == candidate.rgba.size() &&
        reference.rgba.size() == static_cast<std::size_t>(reference.width) * reference.height * 4;
    if (!result.same_size) {
        result.max_channel_diff = 255;
        return result;
    }
    bool found = false;
    const std::size_t pixels = static_cast<std::size_t>(reference.width) * reference.height;
    for (std::size_t i = 0; i < pixels; ++i) {
        unsigned pixel_max = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            const int a = reference.rgba[i * 4 + c];
            const int b = candidate.rgba[i * 4 + c];
            pixel_max = std::max(pixel_max, static_cast<unsigned>(std::abs(a - b)));
        }
        if (pixel_max != 0 && !found) {
            found = true;
            result.first_x = static_cast<std::uint32_t>(i % reference.width);
            result.first_y = static_cast<std::uint32_t>(i / reference.width);
        }
        result.max_channel_diff = std::max(result.max_channel_diff, pixel_max);
        result.pixels_over_8 += pixel_max > 8;
    }
    result.fraction_over_8 = pixels ? static_cast<double>(result.pixels_over_8) / pixels : 0;
    return result;
}

bool within_tolerance(const std::string& case_name, const CompareResult& result, std::string& reason) {
    if (!result.same_size) {
        reason = "size mismatch";
        return false;
    }
    if (case_name.rfind("fixture/", 0) == 0) {
        if (result.max_channel_diff <= 2) return true;
        reason = "fixture max channel diff " + std::to_string(result.max_channel_diff) + " > 2";
        return false;
    }
    if (result.fraction_over_8 <= 0.002 && result.max_channel_diff <= 48) return true;
    reason = "scene fraction over 8 = " + std::to_string(result.fraction_over_8) +
        " (limit 0.002), max = " + std::to_string(result.max_channel_diff) + " (limit 48)";
    return false;
}

}  // namespace wui::conformance
