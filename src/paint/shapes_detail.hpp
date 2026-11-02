#pragma once

#include <cstddef>
#include <vector>

namespace wui::detail {

inline constexpr std::size_t shadow_cache_capacity = 32;

// Per-frame bounds of the cross-frame rounded-outline cache: shapes beyond
// them are tessellated every frame instead of being recorded.
inline constexpr std::size_t outline_cache_entry_limit = std::size_t{1} << 15;
inline constexpr std::size_t outline_cache_point_limit = std::size_t{1} << 20;

// Per-generation bounds of the cross-frame cache of line and polygon meshes
// (points count the request's own points as well as outline and fringe).
inline constexpr std::size_t mesh_cache_entry_limit = 4096;
inline constexpr std::size_t mesh_cache_point_limit = std::size_t{1} << 18;
inline constexpr std::size_t mesh_cache_index_limit = std::size_t{1} << 18;

// Per-stroke fallback limits; total live pixel storage also obeys TextureStore.
inline constexpr std::size_t stroke_point_limit = 4096;
inline constexpr std::size_t stroke_mesh_point_limit = 512;
inline constexpr std::size_t stroke_raster_dimension_limit = 2048;
inline constexpr std::size_t stroke_raster_pixel_limit = 1024 * 1024;
inline constexpr std::size_t stroke_raster_work_limit = 64 * 1024 * 1024;

std::vector<float> gaussian_kernel(float sigma);

} // namespace wui::detail
