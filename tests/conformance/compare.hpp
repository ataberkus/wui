#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wui::conformance {

// Tightly packed RGBA8, row-major, top-left origin.
struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

struct CompareResult {
    bool same_size = false;
    unsigned max_channel_diff = 0;   // over R, G, B (alpha is not compared)
    std::size_t pixels_over_8 = 0;   // pixels with any RGB channel differing by > 8
    double fraction_over_8 = 0;
    std::uint32_t first_x = 0, first_y = 0; // first differing pixel, if any
    bool identical() const { return same_size && max_channel_diff == 0; }
};

CompareResult compare(const Image& reference, const Image& candidate);

// Fixture cases must stay within 2; widget scenes within 0.2% of pixels over 8
// and a maximum of 48. Returns a readable reason when outside tolerance.
bool within_tolerance(const std::string& case_name, const CompareResult& result, std::string& reason);

}  // namespace wui::conformance
