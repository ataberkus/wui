#include <wui/style.hpp>

#include "theme_detail.hpp"

#include <array>
#include <cmath>

namespace wui {
namespace {

constexpr float channel(unsigned int value) {
    return static_cast<float>(value) / 255.0f;
}

constexpr Color rgb(unsigned int red, unsigned int green, unsigned int blue) {
    return {channel(red), channel(green), channel(blue), 1.0f};
}

bool valid_channel(float value) {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool valid_color(Color color) {
    return valid_channel(color.r) && valid_channel(color.g) &&
           valid_channel(color.b) && valid_channel(color.a);
}

bool positive(float value) {
    return std::isfinite(value) && value > 0.0f;
}

bool nonnegative(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

} // namespace

Theme Theme::dark() {
    return {
        rgb(0x10, 0x12, 0x18),
        rgb(0x19, 0x1c, 0x25),
        rgb(0x23, 0x27, 0x35),
        rgb(0xf3, 0xf4, 0xf8),
        rgb(0xb7, 0xbd, 0xcc),
        rgb(0x9c, 0x8c, 0xff),
        rgb(0xc2, 0xb8, 0xff),
    };
}

Result<Theme> resolve_theme(Theme source, Density density) {
    if (density != Density::comfortable && density != Density::compact) {
        return Error{ErrorCode::invalid_argument, "Unknown theme density"};
    }
    if (const auto error = detail::validate_theme(source)) {
        return *error;
    }
    if (density == Density::compact) {
        source.control_height *= 7.0f / 9.0f;
        source.padding *= 0.75f;
        source.gap *= 0.5f;
    }
    if (const auto error = detail::validate_theme(source)) {
        return *error;
    }
    return source;
}

namespace detail {

void apply_style(Theme& theme, const StyleOverride& override) {
    if (override.control) theme.control = *override.control;
    if (override.text) theme.text = *override.text;
    if (override.accent) theme.accent = *override.accent;
    if (override.focus) theme.focus = *override.focus;
}
Status validate_theme(const Theme& theme) {
    const std::array colors{
        theme.canvas, theme.panel, theme.control, theme.text,
        theme.secondary, theme.accent, theme.focus,
    };
    for (const auto color : colors) {
        if (!valid_color(color)) {
            return Error{ErrorCode::invalid_argument, "Theme colors must be finite encoded SDR values"};
        }
    }
    if (!positive(theme.control_height) || !positive(theme.body_size) ||
        !positive(theme.heading_size) || !nonnegative(theme.padding) ||
        !nonnegative(theme.gap) || !nonnegative(theme.control_radius) ||
        !nonnegative(theme.panel_radius)) {
        return Error{ErrorCode::invalid_argument, "Theme metrics must be finite and nonnegative"};
    }
    return std::nullopt;
}

} // namespace detail
} // namespace wui
