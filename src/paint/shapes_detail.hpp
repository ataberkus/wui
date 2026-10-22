#pragma once

#include <cstddef>
#include <vector>

namespace wui::detail {

inline constexpr std::size_t shadow_cache_capacity = 32;

// Per-stroke fallback limits; total live pixel storage also obeys TextureStore.
inline constexpr std::size_t stroke_point_limit = 4096;
inline constexpr std::size_t stroke_mesh_point_limit = 512;
inline constexpr std::size_t stroke_raster_dimension_limit = 2048;
inline constexpr std::size_t stroke_raster_pixel_limit = 1024 * 1024;
inline constexpr std::size_t stroke_raster_work_limit = 64 * 1024 * 1024;

std::vector<float> gaussian_kernel(float sigma);

} // namespace wui::detail
