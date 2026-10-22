#pragma once

#include <wui/style.hpp>

namespace wui::detail {

Status validate_theme(const Theme& theme);
void apply_style(Theme& theme, const StyleOverride& override);

} // namespace wui::detail
