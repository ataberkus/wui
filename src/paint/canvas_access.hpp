#pragma once

#include <wui/canvas.hpp>
#include <wui/text.hpp>

#include <span>

namespace wui::detail {
struct PanelMark {
    std::size_t first_batch = 0, semantic_index = 0;
    Rect clip{};
    float radius = 0;
    Color color{};
};
struct CanvasLayer {
    std::size_t first_batch = 0, stash_position = 0, clip_depth = 0;
    bool open = false;
};
struct CanvasAccess {
    static Status status(const Canvas&);
    // Same as status().has_value(), without building an optional Error.
    static bool failed(const Canvas&) noexcept;
    static void abort(Canvas&) noexcept;
    // Reserve ordering and a semantic at open; finalize only the background at
    // close. No callback replay or mutation of an already published packet.
    static PanelMark open_panel(Canvas&, Id, std::string_view title,
                                Rect provisional, float radius, Color color);
    // Caller has popped the panel's child clip before closing.
    static void close_panel(Canvas&, PanelMark, Rect bounds, Rect interior);
    static void sync_focus(Canvas&, Id);
    static void clear_choice_selection(Canvas&, Id);
    // Paint between begin_layer and end_layer is detached from the main batch
    // order (so enclosing panels neither reorder nor clip it) and drawn above
    // all main paint by flush_layers(), which finish() also performs. The
    // layer clip replaces, rather than intersects, the enclosing clip; it is
    // still bounded by the root viewport. Layers nest LIFO.
    static CanvasLayer begin_layer(Canvas&, Rect clip);
    static void end_layer(Canvas&, CanvasLayer);
    static void flush_layers(Canvas&);
    // Same packet and errors as Canvas::image(glyph.bounds moved by offset,
    // glyph.texture, glyph.uv, tint) for each glyph in order, but the tint is
    // validated and premultiplied once and room for the run is made once.
    static void glyph_run(Canvas&, std::span<const GlyphQuad> glyphs, Vec2 offset, Color tint);
};
} // namespace wui::detail
