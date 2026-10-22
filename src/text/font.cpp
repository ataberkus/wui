#include "text_detail.hpp"

#include <hb-ot.h>
#include <utf8proc.h>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui::detail {
namespace {

Error font_error(FT_Error error, const char* message) {
    return {error == FT_Err_Out_Of_Memory ? ErrorCode::capacity : ErrorCode::invalid_font,
            message};
}

} // namespace

FontLibrary::~FontLibrary() {
    if (value) {
        FT_Done_FreeType(value);
    }
}

FontFace::~FontFace() {
    if (font) {
        hb_font_destroy(font);
    }
    if (face) {
        hb_face_destroy(face);
    }
    if (blob) {
        hb_blob_destroy(blob);
    }
    if (raster) {
        FT_Done_Face(raster);
    }
}

Status FontFace::initialize(FT_Library library, std::span<const std::byte> source) {
    if (source.empty()) {
        return Error{ErrorCode::invalid_font, "Both font roles require supplied font bytes"};
    }
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max()) ||
        source.size() > std::numeric_limits<unsigned int>::max()) {
        return Error{ErrorCode::capacity, "Font byte count exceeds library limits"};
    }
    bytes.assign(source.begin(), source.end());
    if (const auto error = FT_New_Memory_Face(
            library, reinterpret_cast<const FT_Byte*>(bytes.data()),
            static_cast<FT_Long>(bytes.size()), 0, &raster)) {
        return font_error(error, "Cannot open supplied font face");
    }
    if (!FT_IS_SCALABLE(raster) || !FT_IS_SFNT(raster) || raster->num_glyphs <= 0) {
        return Error{ErrorCode::invalid_font, "Text requires a scalable OpenType font face"};
    }
    if (const auto error = FT_Select_Charmap(raster, FT_ENCODING_UNICODE)) {
        return font_error(error, "Font requires a Unicode character map");
    }

    blob = hb_blob_create_or_fail(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<unsigned int>(bytes.size()),
                                 HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    if (!blob) {
        return Error{ErrorCode::capacity, "Cannot allocate HarfBuzz font blob"};
    }
    face = hb_face_create(blob, 0);
    if (!face || face == hb_face_get_empty()) {
        return Error{ErrorCode::capacity, "Cannot allocate HarfBuzz font face"};
    }
    if (hb_face_get_glyph_count(face) != static_cast<unsigned long>(raster->num_glyphs) ||
        hb_face_get_upem(face) != raster->units_per_EM) {
        return Error{ErrorCode::invalid_font, "Font shaping and rasterization tables disagree"};
    }
    font = hb_font_create(face);
    if (!font || font == hb_font_get_empty()) {
        return Error{ErrorCode::capacity, "Cannot allocate HarfBuzz font"};
    }
    hb_ot_font_set_funcs(font);

    // Use only this role's supplied face, never .notdef or an implicit system font.
    constexpr std::array<hb_codepoint_t, 2> replacements{0xfffd, '?'};
    for (const auto scalar : replacements) {
        hb_codepoint_t glyph = 0;
        if (!hb_font_get_nominal_glyph(font, scalar, &glyph) || glyph == 0 ||
            FT_Get_Char_Index(raster, scalar) != glyph) {
            continue;
        }
        if (const auto error = FT_Load_Glyph(
                raster, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
            return font_error(error, "Cannot load font replacement glyph");
        }
        if (raster->glyph->format != FT_GLYPH_FORMAT_OUTLINE ||
            raster->glyph->outline.n_points <= 0 || raster->glyph->metrics.width <= 0 ||
            raster->glyph->metrics.height <= 0) {
            continue;
        }
        replacement_scalar = scalar;
        replacement_glyph = glyph;
        return std::nullopt;
    }
    return Error{ErrorCode::invalid_font, "Font needs a visible U+FFFD or question-mark replacement"};
}

TextService::TextService(std::shared_ptr<TextureStore> textures)
    : atlas_(std::move(textures)) {}

TextService::~TextService() = default;

Status TextService::initialize(const FontFaces& faces) {
    if (const auto error = FT_Init_FreeType(&library_.value)) {
        return font_error(error, "Cannot initialize FreeType");
    }
    FT_Int major = 0, minor = 0, patch = 0;
    FT_Library_Version(library_.value, &major, &minor, &patch);
    if (major != 2 || minor != 14 || patch != 3 ||
        std::strcmp(hb_version_string(), "14.4.0") != 0 ||
        std::strcmp(utf8proc_version(), "2.11.3") != 0) {
        return Error{ErrorCode::unsupported, "Text runtime requires FreeType 2.14.3, HarfBuzz 14.4.0, utf8proc 2.11.3"};
    }
    if (const auto error = fonts_[0].initialize(library_.value, faces.regular)) {
        return error;
    }
    if (const auto error = fonts_[1].initialize(library_.value, faces.semibold)) {
        return error;
    }
    if (faces.display.empty()) {
        // Display reuses the loaded semibold face and its atlas glyphs.
        role_faces_ = {0, 1, 1};
        return std::nullopt;
    }
    if (const auto error = fonts_[2].initialize(library_.value, faces.display)) {
        return error;
    }
    role_faces_ = {0, 1, 2};
    return std::nullopt;
}

Status TextService::publish() {
    try {
        return atlas_.publish();
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

} // namespace wui::detail

namespace wui {

Result<std::shared_ptr<IText>> make_text_service(
    FontFaces faces, std::shared_ptr<TextureStore> textures) {
    try {
        if (!textures) {
            return Error{ErrorCode::invalid_argument, "Text service requires a texture store"};
        }
        auto service = std::make_shared<detail::TextService>(std::move(textures));
        if (const auto error = service->initialize(faces)) {
            return *error;
        }
        return std::shared_ptr<IText>{std::move(service)};
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Result<std::shared_ptr<IText>> make_text_service(
    std::span<const std::byte> regular, std::span<const std::byte> semibold,
    std::shared_ptr<TextureStore> textures) {
    return make_text_service(FontFaces{regular, semibold, {}}, std::move(textures));
}

} // namespace wui
