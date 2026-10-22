#include "text_detail.hpp"

#include <utf8proc.h>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui::detail {
namespace {

struct BufferDeleter {
    void operator()(hb_buffer_t* buffer) const noexcept { hb_buffer_destroy(buffer); }
};
using Buffer = std::unique_ptr<hb_buffer_t, BufferDeleter>;

bool finite_float(double value) {
    return std::isfinite(value) &&
           std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
}

Result<hb_language_t> intern_language(const std::string& requested) {
    if (requested.empty()) {
        return HB_LANGUAGE_INVALID;
    }
    // HarfBuzz retains languages until process exit. Bound admissions across
    // every service, including requests whose later atlas work fails.
    struct Languages {
        std::mutex mutex;
        std::array<hb_language_t, 256> values{};
        std::size_t count = 0, bytes = 0;
    };
    static Languages languages;
    const auto canonical = [](char value) {
        if (value >= 'A' && value <= 'Z') {
            return static_cast<char>(value + ('a' - 'A'));
        }
        return value == '_' ? '-' : value;
    };
    const std::lock_guard lock(languages.mutex);
    for (std::size_t i = 0; i < languages.count; ++i) {
        const std::string_view stored = hb_language_to_string(languages.values[i]);
        if (stored.size() != requested.size()) {
            continue;
        }
        std::size_t j = 0;
        while (j < stored.size() && stored[j] == canonical(requested[j])) {
            ++j;
        }
        if (j == stored.size()) {
            return languages.values[i];
        }
    }
    constexpr std::size_t byte_limit = 64 * 1024;
    if (languages.count == languages.values.size() ||
        requested.size() >= byte_limit - languages.bytes) {
        return Error{ErrorCode::capacity, "Process text language capacity exceeded"};
    }
    const auto language = hb_language_from_string(requested.c_str(), -1);
    if (language == HB_LANGUAGE_INVALID) {
        return Error{ErrorCode::capacity, "Cannot allocate HarfBuzz language tag"};
    }
    languages.values[languages.count++] = language;
    languages.bytes += requested.size() + 1;
    return language;
}

Result<std::uint32_t> physical_size(const ShapeRequest& request) {
    if (request.role != FontRole::body && request.role != FontRole::heading &&
        request.role != FontRole::display) {
        return Error{ErrorCode::invalid_argument, "Unknown text font role"};
    }
    if (!std::isfinite(request.logical_size) || request.logical_size <= 0 ||
        !std::isfinite(request.scale) || request.scale <= 0) {
        return Error{ErrorCode::invalid_argument, "Text size and scale must be finite and positive"};
    }
    const auto rounded = std::round(static_cast<double>(request.logical_size) * request.scale);
    if (rounded < 1) {
        return Error{ErrorCode::invalid_argument, "Text size rounds to zero physical pixels"};
    }
    if (rounded > std::numeric_limits<FT_UShort>::max()) {
        return Error{ErrorCode::capacity, "Physical text size exceeds FreeType pixel-size limits"};
    }
    if (request.text.size() > text_request_byte_limit ||
        request.language.size() > text_request_byte_limit - request.text.size()) {
        return Error{ErrorCode::capacity, "Text request exceeds the one-MiB byte limit"};
    }
    // HarfBuzz canonicalizes ASCII language tags. Reject bytes it would truncate
    // or turn into terminators rather than silently changing the requested tag.
    for (const auto character : request.language) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_')) {
            return Error{ErrorCode::invalid_argument, "Text language must be an ASCII language tag"};
        }
    }
    return static_cast<std::uint32_t>(rounded);
}

void sanitize(const FontFace& face, std::string_view source,
              std::string& clean, std::vector<std::size_t>& graphemes) {
    // One malformed byte becomes one replacement scalar; caller bytes are never
    // modified. Valid but uncovered scalars use this role's explicit replacement.
    static_assert(text_request_byte_limit <=
                  static_cast<std::size_t>(std::numeric_limits<int>::max()) / 3);
    clean.reserve(source.size());
    utf8proc_int32_t previous = -1;
    utf8proc_int32_t state = 0;
    for (std::size_t offset = 0; offset < source.size();) {
        utf8proc_int32_t scalar = 0;
        const auto consumed = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(source.data() + offset),
            static_cast<utf8proc_ssize_t>(source.size() - offset), &scalar);
        if (consumed <= 0) {
            scalar = static_cast<utf8proc_int32_t>(face.replacement_scalar);
            ++offset;
        } else {
            offset += static_cast<std::size_t>(consumed);
            hb_codepoint_t glyph = 0;
            if (!hb_font_get_nominal_glyph(face.font, static_cast<hb_codepoint_t>(scalar), &glyph) ||
                glyph == 0 || FT_Get_Char_Index(face.raster, static_cast<FT_ULong>(scalar)) != glyph) {
                scalar = static_cast<utf8proc_int32_t>(face.replacement_scalar);
            }
        }
        if (previous < 0 || utf8proc_grapheme_break_stateful(previous, scalar, &state)) {
            graphemes.push_back(clean.size());
        }
        utf8proc_uint8_t encoded[4];
        const auto length = utf8proc_encode_char(scalar, encoded);
        clean.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(length));
        previous = scalar;
    }
}

} // namespace

Result<std::vector<std::size_t>> TextService::grapheme_offsets(std::string_view source) {
    try {
        if (source.size() > text_request_byte_limit)
            return Error{ErrorCode::capacity, "Text request exceeds the one-MiB byte limit"};
        std::vector<std::size_t> boundaries{0};
        utf8proc_int32_t previous = -1, grapheme_state = 0;
        for (std::size_t at = 0; at < source.size();) {
            utf8proc_int32_t scalar = 0;
            auto width = utf8proc_iterate(
                reinterpret_cast<const utf8proc_uint8_t*>(source.data() + at),
                static_cast<utf8proc_ssize_t>(source.size() - at), &scalar);
            if (width <= 0) { scalar = 0xFFFD; width = 1; }
            if (previous >= 0 && utf8proc_grapheme_break_stateful(previous, scalar, &grapheme_state))
                boundaries.push_back(at);
            at += static_cast<std::size_t>(width);
            previous = scalar;
        }
        if (!source.empty()) boundaries.push_back(source.size());
        return boundaries;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

std::size_t RunKeyHash::operator()(RunKeyView key) const noexcept {
    auto value = std::hash<std::string_view>{}(key.text);
    const auto combine = [&value](std::size_t part) {
        value ^= part + static_cast<std::size_t>(0x9e3779b9U) + (value << 6) + (value >> 2);
    };
    combine(std::hash<std::string_view>{}(key.language));
    combine(static_cast<std::size_t>(key.role));
    combine(key.physical_ppem);
    combine(std::bit_cast<std::uint32_t>(key.scale));
    return value;
}

bool TextService::make_room(std::size_t bytes) {
    const auto fits = [bytes](std::size_t entries, std::size_t used) {
        return entries < text_cache_entry_limit && used <= text_cache_byte_limit &&
               bytes <= text_cache_byte_limit - used;
    };
    if (fits(runs_.size(), cached_bytes_)) {
        return true;
    }
    // Probe first so a request that cannot fit evicts nothing. Only runs no
    // caller still holds are evictable; the oldest use goes first.
    std::size_t entries = runs_.size(), used = cached_bytes_;
    bool possible = false;
    for (auto entry = lru_.rbegin(); entry != lru_.rend(); ++entry) {
        if (entry->run.use_count() != 1) {
            continue;
        }
        --entries;
        used -= entry->bytes;
        if (fits(entries, used)) {
            possible = true;
            break;
        }
    }
    if (!possible) {
        return false;
    }
    auto entry = lru_.end();
    while (!fits(runs_.size(), cached_bytes_)) {
        if (entry == lru_.begin()) {
            return false;
        }
        --entry;
        if (entry->run.use_count() != 1) {
            continue;
        }
        runs_.erase(RunKeyView(entry->key));
        cached_bytes_ -= entry->bytes;
        entry = lru_.erase(entry);
    }
    return true;
}

Result<std::shared_ptr<const TextRun>> TextService::shape(const ShapeRequest& request) {
    try {
        const auto size_result = physical_size(request);
        if (const auto* error = std::get_if<Error>(&size_result)) {
            return *error;
        }
        const auto ppem = std::get<std::uint32_t>(size_result);
        const RunKeyView lookup{request.text, request.language, request.role, ppem, request.scale};
        if (const auto found = runs_.find(lookup); found != runs_.end()) {
            // A hit is a use: move the entry to the most recent end.
            lru_.splice(lru_.begin(), lru_, found->second);
            return found->second->run;
        }
        // A single run may use the whole budget; make_room evicts for it below.
        constexpr auto available = text_cache_byte_limit;

        const auto face_slot = role_faces_[static_cast<std::size_t>(request.role)];
        const auto atlas_role = static_cast<FontRole>(face_slot);
        auto& face = fonts_[face_slot];
        if (const auto error = FT_Set_Pixel_Sizes(face.raster, ppem, ppem)) {
            return Error{error == FT_Err_Out_Of_Memory ? ErrorCode::capacity : ErrorCode::invalid_font,
                         "Cannot set physical font size"};
        }
        const double scale = request.scale;
        const double ascender = static_cast<double>(face.raster->size->metrics.ascender) / 64.0;
        const double descender = static_cast<double>(face.raster->size->metrics.descender) / 64.0;
        const double baseline = ascender / scale;
        const double height = (ascender - descender) / scale;
        if (ascender <= 0 || descender > 0 || height <= 0) {
            return Error{ErrorCode::invalid_font, "Font has invalid ascender or descender metrics"};
        }
        if (!finite_float(baseline) || !finite_float(height)) {
            return Error{ErrorCode::invalid_argument, "Logical text metrics exceed finite float range"};
        }

        RunKey key{std::string(request.text), std::string(request.language), request.role,
                   ppem, request.scale};
        auto run = std::make_shared<TextRun>();
        run->baseline = static_cast<float>(baseline);
        run->height = static_cast<float>(height);
        sanitize(face, key.text, run->display, run->grapheme_starts);
        const auto& clean = run->display;

        const auto fixed_bytes = sizeof(RunKey) + sizeof(TextRun) +
                                 sizeof(std::shared_ptr<const TextRun>) +
                                 key.text.capacity() + key.language.capacity() +
                                 run->display.capacity() + 2;
        if (fixed_bytes > available || run->grapheme_starts.capacity() >
                (available - fixed_bytes) / sizeof(std::size_t)) {
            return Error{ErrorCode::capacity, "Text run cache byte limit exceeded"};
        }
        const auto used_bytes = fixed_bytes + run->grapheme_starts.capacity() * sizeof(std::size_t);

        Buffer buffer{hb_buffer_create()};
        if (!buffer || buffer.get() == hb_buffer_get_empty() ||
            !hb_buffer_allocation_successful(buffer.get())) {
            return Error{ErrorCode::capacity, "Cannot allocate HarfBuzz shaping buffer"};
        }
        hb_font_set_scale(face.font, static_cast<int>(ppem * 64), static_cast<int>(ppem * 64));
        hb_font_set_ppem(face.font, ppem, ppem);
        hb_buffer_add_utf8(buffer.get(), clean.data(), static_cast<int>(clean.size()),
                           0, static_cast<int>(clean.size()));
        hb_buffer_set_direction(buffer.get(), HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer.get(), HB_SCRIPT_LATIN);
        const auto language = intern_language(key.language);
        if (const auto* error = std::get_if<Error>(&language)) {
            return *error;
        }
        hb_buffer_set_language(buffer.get(), std::get<hb_language_t>(language));
        if (!hb_buffer_allocation_successful(buffer.get())) {
            return Error{ErrorCode::capacity, "Cannot allocate HarfBuzz input text"};
        }
        constexpr const char* shapers[]{"ot", nullptr};
        if (!hb_shape_full(face.font, buffer.get(), nullptr, 0, shapers) ||
            !hb_buffer_allocation_successful(buffer.get())) {
            return Error{ErrorCode::capacity, "HarfBuzz shaping allocation or operation limit exceeded"};
        }
        unsigned int glyph_count = 0, position_count = 0;
        const auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &glyph_count);
        const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), &position_count);
        if (glyph_count != position_count || (glyph_count != 0 && (!infos || !positions))) {
            return Error{ErrorCode::invalid_font, "HarfBuzz returned inconsistent glyph positions"};
        }
        if (glyph_count > (available - used_bytes) / sizeof(GlyphQuad)) {
            return Error{ErrorCode::capacity, "Text run cache byte limit exceeded"};
        }
        run->glyphs.reserve(glyph_count);
        if (run->glyphs.capacity() > (available - used_bytes) / sizeof(GlyphQuad)) {
            return Error{ErrorCode::capacity, "Text run cache byte limit exceeded"};
        }
        const auto run_bytes = used_bytes + run->glyphs.capacity() * sizeof(GlyphQuad);
        // Make room before staging glyphs; held runs are never evicted.
        if (!make_room(run_bytes)) {
            return Error{ErrorCode::capacity, runs_.size() >= text_cache_entry_limit
                ? "Text run cache entry limit exceeded"
                : "Text run cache byte limit exceeded"};
        }

        // Every used face/size reserves a visible replacement before regular glyphs.
        if (glyph_count != 0) {
            const auto replacement = atlas_.glyph(face.raster, atlas_role, ppem,
                                                   face.replacement_glyph);
            if (const auto* error = std::get_if<Error>(&replacement)) {
                return *error;
            }
            const auto& image = std::get<AtlasGlyph>(replacement);
            if (image.width <= 0 || image.height <= 0) {
                return Error{ErrorCode::invalid_font, "Font replacement is invisible at the requested size"};
            }
        }

        double pen_x = 0, pen_y = 0;
        for (unsigned int index = 0; index < glyph_count; ++index) {
            const auto& info = infos[index];
            const auto& position = positions[index];
            if (info.codepoint == 0 || info.codepoint >= static_cast<unsigned long>(face.raster->num_glyphs) ||
                info.cluster >= clean.size() ||
                (static_cast<unsigned char>(clean[info.cluster]) & 0xc0U) == 0x80U) {
                return Error{ErrorCode::invalid_font, "HarfBuzz returned an invalid glyph or UTF-8 cluster"};
            }
            const auto glyph_result = atlas_.glyph(face.raster, atlas_role, ppem, info.codepoint);
            if (const auto* error = std::get_if<Error>(&glyph_result)) {
                return *error;
            }
            const auto& image = std::get<AtlasGlyph>(glyph_result);
            if (image.width > 0 && image.height > 0) {
                const double x = ((pen_x + position.x_offset) / 64.0 + image.bearing_x) / scale;
                const double y = baseline - ((pen_y + position.y_offset) / 64.0 + image.bearing_y) / scale;
                const double width = image.width / scale;
                const double glyph_height = image.height / scale;
                if (!finite_float(x) || !finite_float(y) || !finite_float(width) ||
                    !finite_float(glyph_height) || !finite_float(x + width) ||
                    !finite_float(y + glyph_height)) {
                    return Error{ErrorCode::invalid_argument, "Logical glyph geometry exceeds finite float range"};
                }
                const Rect bounds{static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(width), static_cast<float>(glyph_height)};
                if (!std::isfinite(bounds.x + bounds.w) || !std::isfinite(bounds.y + bounds.h)) {
                    return Error{ErrorCode::invalid_argument, "Logical glyph rectangle overflows"};
                }
                run->glyphs.push_back({image.texture, bounds, image.uv});
            }
            pen_x += position.x_advance;
            pen_y += position.y_advance;
        }
        const double width = pen_x / 64.0 / scale;
        if (!finite_float(width) || width < 0) {
            return Error{ErrorCode::invalid_argument, "Logical text advance exceeds finite nonnegative range"};
        }
        run->width = static_cast<float>(width);
        std::shared_ptr<const TextRun> immutable = std::move(run);
        lru_.push_front(CacheEntry{std::move(key), immutable, run_bytes});
        try {
            runs_.emplace(RunKeyView(lru_.front().key), lru_.begin());
        } catch (...) {
            lru_.pop_front();
            throw;
        }
        cached_bytes_ += run_bytes;
        return immutable;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

} // namespace wui::detail
