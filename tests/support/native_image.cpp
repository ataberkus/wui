#include "native_image.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace wui::test {

Pixel Image::at(std::uint32_t x, std::uint32_t y) const {
    if (x >= width || y >= height) throw std::out_of_range("Image coordinates out of range");
    return pixels[static_cast<std::size_t>(y) * width + x];
}

Status write_ppm(const Image& image, const std::filesystem::path& path) {
    if (image.width == 0 || image.height == 0) {
        return Error{ErrorCode::invalid_argument, "Cannot write an empty image"};
    }
    if (image.pixels.size() != static_cast<std::size_t>(image.width) * image.height) {
        return Error{ErrorCode::invalid_argument, "Image pixel count does not match dimensions"};
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Error{ErrorCode::invalid_argument, "Cannot open PPM output path"};
    const std::string header =
        "P6\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (const auto& pixel : image.pixels) {
        const char rgb[3] = {
            static_cast<char>(pixel.r), static_cast<char>(pixel.g), static_cast<char>(pixel.b)};
        out.write(rgb, 3);
    }
    out.flush();
    if (!out) return Error{ErrorCode::backend_failure, "Failed to write PPM image"};
    return std::nullopt;
}

} // namespace wui::test
