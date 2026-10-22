#pragma once

#include <wui/types.hpp>
#include <optional>

namespace wui {

enum class Density { comfortable, compact };
enum class Variant { normal, prominent, quiet };
struct StyleOverride {
    std::optional<Color> control, text, accent, focus;
    // In a Frame::style scope, disables every widget emitted inside it (as if
    // WidgetOptions::enabled were false); per-widget overrides ignore it.
    bool disabled = false;
};

struct Theme {
    Color canvas;
    Color panel;
    Color control;
    Color text;
    Color secondary;
    Color accent;
    Color focus;
    float control_height = 36.0f;
    float body_size = 14.0f;
    float heading_size = 20.0f;
    float padding = 16.0f;
    float gap = 8.0f;
    float control_radius = 8.0f;
    float panel_radius = 12.0f;
    bool reduced_motion = false;

    static Theme dark();
};

// Resolves a validated copy; the caller's source tokens remain unchanged.
// Comfortable preserves every token. Compact scales control_height by 7/9,
// padding by 3/4, and gap by 1/2 (dark defaults: 28, 12, 4).
// Font sizes, colors, radii and reduced_motion are preserved in both modes.
Result<Theme> resolve_theme(Theme source, Density density = Density::comfortable);

struct Transition {
    float from = 0.0f;
    float target = 0.0f;
    float elapsed = 0.0f;
    float duration = 0.1f;

    float value() const;
    void retarget(float next);
    void tick(float dt, bool reduced_motion);
};

} // namespace wui
