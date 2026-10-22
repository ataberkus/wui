#pragma once

#include <wui/backends/d3d9.hpp>

namespace wui::d3d9::detail {

// Pure part of native device validation, exercised with synthetic D3DCAPS9.
Status validate_caps(const D3DCAPS9& caps);

} // namespace wui::d3d9::detail
