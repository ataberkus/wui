#pragma once

#include <wui/text.hpp>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstdint>
#include <memory>

namespace wui::detail {
struct AtlasGlyph {
    TextureHandle texture;
    Rect uv;
    float width = 0, height = 0, bearing_x = 0, bearing_y = 0;
};
// Physical-pixel glyph metrics. Faces are borrowed only for the synchronous call.
// `role` names the face slot that owns `face`; callers pass one slot per face,
// so glyph ids of different faces never share an entry.
class GlyphAtlas {
public:
    explicit GlyphAtlas(std::shared_ptr<TextureStore> textures);
    ~GlyphAtlas();
    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    Result<AtlasGlyph> glyph(FT_Face face, FontRole role,
                             std::uint32_t physical_ppem, std::uint32_t glyph_id);
    Status publish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
