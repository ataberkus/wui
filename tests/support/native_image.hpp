#pragma once

#include <wui/types.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace wui::test {

struct Pixel {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<Pixel> pixels;
    Pixel at(std::uint32_t x, std::uint32_t y) const;
};

// Writes a P6 RGB image in row-major order; alpha is checked directly by
// tests and never written. Test-only; never enters the shipped library.
Status write_ppm(const Image& image, const std::filesystem::path& path);

} // namespace wui::test
