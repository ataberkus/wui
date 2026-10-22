#pragma once

#include <wui/resources.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wui {
// display selects an optional third face (for example a heavy italic display
// face); a service built without one shapes display with the semibold face.
enum class FontRole { body, heading, display };
struct ShapeRequest {
    std::string_view text;
    FontRole role = FontRole::body;
    float logical_size = 14, scale = 1;
    std::string_view language = "und";
};
struct GlyphQuad { TextureHandle texture; Rect bounds; Rect uv; };
struct TextRun {
    float width = 0, height = 0, baseline = 0;
    std::vector<GlyphQuad> glyphs;
    std::string display;
    // Byte offsets of grapheme starts in the sanitized display string; no end sentinel.
    std::vector<std::size_t> grapheme_starts;
};
// One externally synchronized caller builds a service. Shape owns cache keys;
// caller views are never retained, and font bytes are copied. Malformed UTF-8 is replaced for
// display one invalid byte at a time. Missing glyphs use the selected face's
// visible U+FFFD (or '?'); no implicit system-font fallback is performed.
// Call publish() before Canvas::finish() to freeze all staged glyph pixels.
// Runs reference stable handles while the service lives; DrawPackets retain
// immutable snapshots independently of the service and later publication.
class IText {
public:
    virtual ~IText() = default;
    virtual Result<std::shared_ptr<const TextRun>> shape(const ShapeRequest&) = 0;
    virtual Status publish() = 0;
    virtual Result<std::vector<std::size_t>> grapheme_offsets(std::string_view source) = 0;
};
class IClipboard {
public:
    virtual ~IClipboard() = default;
    virtual Result<std::string> clipboard() const = 0;
    virtual Status set_clipboard(std::string_view) = 0;
};
// Limits: eight append-only 1024x1024 RGBA8 pages; 4096 cached runs and
// 16 MiB of run/key payload (plus bounded map overhead); 1 MiB text+language
// bytes per request. A full run cache evicts least recently shaped runs that
// no caller still holds, so continuously changing strings (timers, counters,
// overlay labels) stay within the bound; it returns capacity only when every
// cached run is still held. Glyph pages are never evicted. Physical sizes round to 1..65535 pixels, but each glyph
// must fit a page with one-pixel padding. Exhaustion returns capacity.
// HarfBuzz retains language tags globally: WUI admits at most 256 distinct
// canonical tags / 64 KiB including terminators across the process lifetime,
// even for failed requests. Already admitted tags remain usable at the limit.
// Body uses regular, heading uses semibold; both supplied fonts are required.
struct FontFaces {
    std::span<const std::byte> regular, semibold;
    std::span<const std::byte> display; // optional; empty uses semibold
};
Result<std::shared_ptr<IText>> make_text_service(
    FontFaces faces, std::shared_ptr<TextureStore> textures);
Result<std::shared_ptr<IText>> make_text_service(
    std::span<const std::byte> regular, std::span<const std::byte> semibold,
    std::shared_ptr<TextureStore> textures);
}
