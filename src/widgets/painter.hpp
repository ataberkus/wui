#pragma once
#include "../style/theme_detail.hpp"

#include "../core/context_detail.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace wui::detail {

class WidgetClip {
public:
    WidgetClip(FrameState& frame, Rect bounds) : canvas_(frame.canvas) {
        canvas_.push_clip(bounds);
    }
    ~WidgetClip() { canvas_.pop_clip(); }
    WidgetClip(const WidgetClip&) = delete;
    WidgetClip& operator=(const WidgetClip&) = delete;

private:
    Canvas& canvas_;
};

inline bool visible(Rect bounds) {
    return bounds.w > 0.0f && bounds.h > 0.0f;
}
// Left offset of a line `width` wide placed in `room` (never negative, so an
// overflowing line keeps its start visible).
inline float align_offset(Align align, float room, float width) {
    const float slack = std::max(0.0f, room - width);
    if (align == Align::center) return slack * 0.5f;
    return align == Align::end ? slack : 0.0f;
}
inline Color mix(Color from, Color to, float amount) {
    // Exact endpoints: settled states paint the token itself.
    if (amount <= 0.0f) return from;
    if (amount >= 1.0f) return to;
    return {from.r + (to.r - from.r) * amount,
            from.g + (to.g - from.g) * amount,
            from.b + (to.b - from.b) * amount,
            from.a + (to.a - from.a) * amount};
}
inline bool same_color(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
// Disabled widgets replace the accent with a neutral grey, so no state of a
// disabled control reads as active.
inline Color disabled_accent(const Theme& theme) { return mix(theme.panel, theme.secondary, 0.35f); }

inline Theme widget_theme(FrameState& frame, const WidgetOptions& options) {
    Theme theme = frame.theme;
    for (const auto& scope : frame.style_stack) apply_style(theme, scope);
    switch (options.variant) {
    case Variant::normal: break;
    case Variant::prominent:
        if (!options.enabled) break; // disabled prominent buttons use the neutral disabled look
        theme.control = theme.accent;
        theme.accent = theme.focus;
        break;
    case Variant::quiet:
        theme.control = theme.panel;
        theme.text = theme.secondary;
        theme.accent = theme.secondary;
        break;
    default:
        frame.report(ErrorCode::invalid_argument, "Unknown widget variant", true);
        return theme;
    }
    apply_style(theme, options.style);
    if (!options.enabled) theme.accent = disabled_accent(theme);
    // frame.theme is validated by Context::create and set_theme, and every
    // style scope by Frame::style before it is pushed; variants and the
    // disabled accent only copy or blend those valid colors. validate_theme
    // checks each field on its own, so only per-widget overrides can fail.
    const auto& style = options.style;
    if (style.control || style.text || style.accent || style.focus) {
        if (auto error = validate_theme(theme))
            frame.report(error->code, error->message.c_str(), true);
    }
    return theme;
}

inline Rect inset(Rect bounds, float amount) {
    const float x = std::min(amount, bounds.w * 0.5f);
    const float y = std::min(amount, bounds.h * 0.5f);
    return {bounds.x + x, bounds.y + y, bounds.w - x * 2.0f, bounds.h - y * 2.0f};
}

inline void initialize_animation(WidgetState& state, bool selected) {
    if (state.initialized) return;
    state.hover.duration = 0.1f;
    state.selection.duration = 0.16f;
    state.selection.from = state.selection.target = selected ? 1.0f : 0.0f;
    state.selection.elapsed = state.selection.duration;
    state.initialized = true;
}

inline void animate(FrameState& frame, WidgetState& state, Behavior behavior, bool selected) {
    state.hover.retarget(behavior.hovered ? 1.0f : 0.0f);
    state.selection.retarget(selected ? 1.0f : 0.0f);
    state.hover.tick(frame.dt, frame.theme.reduced_motion);
    state.selection.tick(frame.dt, frame.theme.reduced_motion);
}
inline void track_feedback(WidgetState& state, Rect hit, Behavior behavior, bool enabled) {
    state.bounds = hit;
    state.hovered = behavior.hovered;
    state.enabled = enabled;
}
inline Color invalid_ink(const Theme& theme) {
    return theme.canvas.r > 0.5f ? Color{0.65f, 0.10f, 0.14f, 1.0f}
                                 : Color{1.0f, 0.42f, 0.43f, 1.0f};
}

inline Color button_fill(const Theme& theme, const WidgetState& state,
                         Behavior behavior, bool enabled) {
    return enabled ? mix(theme.control, theme.accent,
                         behavior.held ? 0.28f : state.hover.value() * 0.16f)
                   : mix(theme.panel, theme.control, 0.5f);
}
// Ink choice. The cubic matches the sRGB transfer curve to within 0.002
// without pow(), so it is cheap enough to evaluate for every label.
inline float approximate_luminance(Color c) {
    const auto lin = [](float v) {
        return v * (v * (v * 0.305306011f + 0.682171111f) + 0.012522878f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}
// The ink with the higher contrast against `fill`; ties go to `first`.
inline Color pick_ink(Color first, Color second, Color fill) {
    const float lf = approximate_luminance(fill);
    const auto contrast = [lf](Color ink) {
        const float li = approximate_luminance(ink);
        return (std::max(li, lf) + 0.05f) / (std::min(li, lf) + 0.05f);
    };
    return contrast(first) >= contrast(second) ? first : second;
}
// Text or canvas ink, whichever reads better on `fill` (dark ink on light accents).
inline Color readable_ink(const Theme& theme, Color fill) { return pick_ink(theme.text, theme.canvas, fill); }

// Derived tokens: every colour below comes from existing theme tokens.
// Recessed well for fields and toggles.
inline Color field_fill(const Theme& theme, bool enabled) {
    return mix(theme.panel, theme.canvas, enabled ? 0.6f : 0.3f);
}
// Field and toggle boundary (3:1 against the panel and the well).
inline Color outline_ink(const Theme& theme) { return mix(theme.control, theme.secondary, 0.45f); }
inline Color field_outline(const Theme& theme, float hover, bool enabled, bool open) {
    if (!enabled) return mix(theme.panel, outline_ink(theme), 0.5f);
    if (open) return theme.focus;
    return mix(outline_ink(theme), theme.secondary, 0.5f * hover);
}
// Slider rail and the hairline around raised surfaces.
inline Color track_ink(const Theme& theme) { return mix(theme.control, theme.secondary, 0.3f); }
// Menu and tooltip surface.
inline Color raised_fill(const Theme& theme) { return mix(theme.control, theme.text, 0.05f); }
// Segmented-control track behind a tab group.
inline Color tab_track(const Theme& theme, bool enabled) {
    return enabled ? theme.control : mix(theme.panel, theme.control, 0.5f);
}
inline Color disabled_ink(const Theme& theme) { return mix(theme.panel, theme.secondary, 0.6f); }
inline Color selection_fill(const Theme& theme) { return mix(field_fill(theme, true), theme.accent, 0.4f); }
// Canvas::shadow needs sigma*scale in [0.5, 32] physical pixels.
inline float shadow_sigma(const FrameState& frame, float sigma) {
    const float scale = frame.scale > 0.0f ? frame.scale : 1.0f;
    return std::clamp(sigma, 0.5f / scale, 32.0f / scale);
}

// Elevation of a raised popup sheet, matching the dropdown menu: a cached soft
// shadow 4px down (sigma 8, alpha 0.8: the cached shadow blurs a (2r+1)px
// shape, so its peak coverage is only about 0.7) and the track_ink hairline.
inline PanelElevation popup_elevation(const FrameState& frame, const Theme& theme) {
    return {{0.0f, 0.0f, 0.0f, 0.8f}, shadow_sigma(frame, 8.0f), 4.0f, track_ink(theme)};
}

// Area left inside the bands paint_rings() draws for the same arguments.
inline Rect ring_inner(Rect bounds, bool focused, bool invalid, bool outlined) {
    if (focused) {
        if (std::min(bounds.w, bounds.h) < 12.0f) return inset(bounds, 2.0f);
        return inset(bounds, invalid ? 5.0f : 3.0f);
    }
    if (invalid) return inset(bounds, 2.0f);
    return outlined ? inset(bounds, 1.0f) : bounds;
}
// Rings over an already painted surface, always inside `bounds` (the hit row).
// Every band is a Canvas::border drawn after the fill: borders keep a hard
// inner edge, while a stack of inset fills lets each fringe eat half of the
// band below it. Order: invalid, separator, ring, so no fringe softens a band.
// ring_inner() gives the area the bands leave.
inline void paint_rings(FrameState& frame, Rect bounds, bool focused, bool invalid,
                        const Theme& theme, Color outline = Color{0, 0, 0, 0}) {
    auto& canvas = frame.canvas;
    const float radius = theme.control_radius;
    const auto concentric = [radius](float in) { return std::max(0.0f, radius - in); };
    if (focused) {
        if (std::min(bounds.w, bounds.h) < 12.0f) {
            canvas.border(bounds, radius, 2.0f, theme.focus);
        } else {
            if (invalid) canvas.border(inset(bounds, 3.0f), concentric(3.0f), 2.0f, invalid_ink(theme));
            canvas.border(inset(bounds, 2.0f), concentric(2.0f), 1.0f, theme.panel); // separator
            canvas.border(bounds, radius, 2.0f, theme.focus);                       // ring
        }
    } else if (invalid) {
        canvas.border(bounds, radius, 2.0f, invalid_ink(theme));
    } else if (outline.a > 0.0f) {
        canvas.border(bounds, radius, 1.0f, outline);
    }
}
// Panel-coloured fills are invisible on their container, so they are skipped:
// quiet buttons, checkbox and radio rows at rest, disabled quiet controls.
inline void paint_surface(FrameState& frame, Rect bounds, Color fill,
                          bool focused, const Theme& theme, bool invalid = false) {
    if (!same_color(fill, theme.panel)) frame.canvas.rounded_rect(bounds, theme.control_radius, fill);
    paint_rings(frame, bounds, focused, invalid, theme);
}
// Recessed field: well fill plus a 1px outline (or the focus/invalid rings).
inline void paint_field(FrameState& frame, Rect bounds, const Theme& theme, float hover,
                        bool focused, bool enabled, bool invalid, bool open = false) {
    frame.canvas.rounded_rect(bounds, theme.control_radius, field_fill(theme, enabled));
    paint_rings(frame, bounds, focused, invalid, theme, field_outline(theme, hover, enabled, open));
}
// Down chevron `size` wide centred on (x, y): 45 degree arms 1.5px thick, one
// simple six-point polygon.
inline void paint_chevron(FrameState& frame, float x, float y, float size, Color ink) {
    const float half = size * 0.5f, drop = size * 0.25f;
    const float thick = 1.5f * std::sqrt(half * half + 4.0f * drop * drop) / half;
    const float top = y - drop - thick * 0.5f;
    const std::array<Vec2, 6> points{{{x - half, top}, {x, top + 2 * drop}, {x + half, top},
                                      {x + half, top + thick}, {x, top + 2 * drop + thick},
                                      {x - half, top + thick}}};
    frame.canvas.polygon(points, ink);
}

} // namespace wui::detail
