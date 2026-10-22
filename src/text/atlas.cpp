#include "atlas.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wui::detail {
namespace {

constexpr std::uint32_t page_side = 1024;
constexpr std::size_t page_limit = 8;
constexpr std::uint32_t padding = 1;
constexpr std::size_t page_bytes = std::size_t{page_side} * page_side * 4;

struct GlyphKey {
    FontRole role;
    std::uint32_t ppem;
    std::uint32_t id;
    bool operator==(const GlyphKey&) const = default;
};

struct GlyphKeyHash {
    std::size_t operator()(const GlyphKey& key) const noexcept {
        auto value = std::hash<std::uint32_t>{}(key.id);
        value ^= std::hash<std::uint32_t>{}(key.ppem) + 0x9e3779b9u +
                 (value << 6) + (value >> 2);
        value ^= static_cast<std::size_t>(key.role) + 0x9e3779b9u +
                 (value << 6) + (value >> 2);
        return value;
    }
};

Error freetype_error(FT_Error error, ErrorCode code, const char* message) {
    if (error == FT_Err_Out_Of_Memory) {
        return {ErrorCode::capacity, {}};
    }
    return {code, message};
}

// A tentative page owns its reservation too, so failed cache insertion releases it.
struct Page {
    explicit Page(TextureStore& store) : textures(store), pixels(page_bytes, std::byte{255}) {
        for (std::size_t offset = 3; offset < pixels.size(); offset += 4) {
            pixels[offset] = std::byte{0};
        }
    }

    ~Page() {
        if (reserved) {
            // Destruction must remain safe even if a caller released an atlas handle.
            try {
                (void)textures.release(texture);
            } catch (...) {
            }
        }
    }

    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    TextureStore& textures;
    std::vector<std::byte> pixels;
    TextureHandle texture;
    std::uint32_t x = 0, y = 0, row_height = 0;
    bool reserved = false;
    bool dirty = false;
};

struct Placement {
    std::uint32_t x, y, next_x, row_height;
};

std::optional<Placement> place(const Page& page, std::uint32_t width,
                               std::uint32_t height) noexcept {
    auto x = page.x;
    auto y = page.y;
    auto row_height = page.row_height;
    if (width > page_side - x) {
        y += row_height;
        x = 0;
        row_height = 0;
    }
    if (height > page_side - y) {
        return std::nullopt;
    }
    return Placement{x, y, x + width, std::max(row_height, height)};
}

} // namespace

struct GlyphAtlas::Impl {
    explicit Impl(std::shared_ptr<TextureStore> store) : textures(std::move(store)) {}

    // Pages are destroyed before the shared store they borrow.
    std::shared_ptr<TextureStore> textures;
    std::array<std::unique_ptr<Page>, page_limit> pages;
    std::size_t page_count = 0;
    std::unordered_map<GlyphKey, AtlasGlyph, GlyphKeyHash> glyphs;
};

GlyphAtlas::GlyphAtlas(std::shared_ptr<TextureStore> textures)
    : impl_(std::make_unique<Impl>(std::move(textures))) {}

GlyphAtlas::~GlyphAtlas() = default;

Result<AtlasGlyph> GlyphAtlas::glyph(FT_Face face, FontRole role,
                                    std::uint32_t physical_ppem, std::uint32_t glyph_id) {
    try {
        if (!impl_->textures) {
            return Error{ErrorCode::invalid_argument, "Glyph atlas requires a texture store"};
        }
        if (role != FontRole::body && role != FontRole::heading && role != FontRole::display) {
            return Error{ErrorCode::invalid_argument, "Invalid glyph font role"};
        }
        if (physical_ppem == 0 || physical_ppem > std::numeric_limits<FT_UShort>::max()) {
            return Error{ErrorCode::invalid_argument, "Invalid glyph physical size"};
        }
        if (!face || !face->glyph || face->num_glyphs <= 0 ||
            glyph_id >= static_cast<FT_ULong>(face->num_glyphs)) {
            return Error{ErrorCode::invalid_font, "Invalid glyph face or index"};
        }

        const GlyphKey key{role, physical_ppem, glyph_id};
        if (const auto cached = impl_->glyphs.find(key); cached != impl_->glyphs.end()) {
            return cached->second;
        }
        if (impl_->glyphs.size() == impl_->glyphs.max_size()) {
            return Error{ErrorCode::capacity, "Glyph cache capacity exceeded"};
        }

        if (const auto error = FT_Set_Pixel_Sizes(face, 0, physical_ppem)) {
            return freetype_error(error, ErrorCode::invalid_font, "Cannot set glyph physical size");
        }
        if (!face->size || face->size->metrics.y_ppem != physical_ppem) {
            return Error{ErrorCode::invalid_font, "Font does not support the requested physical size"};
        }
        if (const auto error = FT_Load_Glyph(face, glyph_id,
                                            FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL)) {
            return freetype_error(error, ErrorCode::invalid_font, "Cannot load font glyph");
        }
        if (const auto error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
            return freetype_error(error, ErrorCode::backend_failure, "Cannot rasterize font glyph");
        }
        if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP) {
            return Error{ErrorCode::invalid_font, "Glyph rasterizer did not produce a bitmap"};
        }
        const auto& bitmap = face->glyph->bitmap;
        AtlasGlyph result{};
        result.bearing_x = static_cast<float>(face->glyph->bitmap_left);
        result.bearing_y = static_cast<float>(face->glyph->bitmap_top);
        if (bitmap.width == 0 || bitmap.rows == 0) {
            impl_->glyphs.emplace(key, result);
            return result;
        }
        if (bitmap.width > page_side - 2 * padding || bitmap.rows > page_side - 2 * padding) {
            return Error{ErrorCode::capacity, "Glyph exceeds atlas page dimensions"};
        }
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays < 2 ||
            bitmap.num_grays > 256 || !bitmap.buffer) {
            return Error{ErrorCode::invalid_font, "Unsupported glyph bitmap coverage format"};
        }
        const auto signed_pitch = static_cast<std::int64_t>(bitmap.pitch);
        const auto pitch = static_cast<std::uint64_t>(signed_pitch < 0 ? -signed_pitch : signed_pitch);
        if (pitch < bitmap.width ||
            pitch > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) / bitmap.rows) {
            return Error{ErrorCode::invalid_font, "Invalid glyph bitmap pitch"};
        }
        const auto row_stride = static_cast<std::size_t>(pitch);
        const auto row_at = [&](std::uint32_t y) {
            const auto row = bitmap.pitch < 0 ? bitmap.rows - 1 - y : y;
            return bitmap.buffer + std::size_t{row} * row_stride;
        };
        const auto gray_max = static_cast<unsigned>(bitmap.num_grays - 1);
        if (gray_max != 255) {
            for (std::uint32_t y = 0; y < bitmap.rows; ++y) {
                const auto* row = row_at(y);
                for (std::uint32_t x = 0; x < bitmap.width; ++x) {
                    if (row[x] > gray_max) {
                        return Error{ErrorCode::invalid_font, "Invalid glyph grayscale coverage"};
                    }
                }
            }
        }

        const auto outer_width = bitmap.width + 2 * padding;
        const auto outer_height = bitmap.rows + 2 * padding;
        Page* page = nullptr;
        std::optional<Placement> placement;
        for (std::size_t index = 0; index < impl_->page_count; ++index) {
            placement = place(*impl_->pages[index], outer_width, outer_height);
            if (placement) {
                page = impl_->pages[index].get();
                break;
            }
        }

        std::unique_ptr<Page> new_page;
        if (!page) {
            if (impl_->page_count == page_limit) {
                return Error{ErrorCode::capacity, "Glyph atlas page capacity exceeded"};
            }
            new_page = std::make_unique<Page>(*impl_->textures);
            auto created = impl_->textures->create(page_side, page_side, new_page->pixels,
                                                   AlphaMode::straight);
            if (const auto* error = std::get_if<Error>(&created)) {
                return *error;
            }
            new_page->texture = std::get<TextureHandle>(created);
            new_page->reserved = true;
            page = new_page.get();
            placement = place(*page, outer_width, outer_height);
        }

        const auto x_origin = placement->x + padding;
        const auto y_origin = placement->y + padding;
        result.texture = page->texture;
        result.width = static_cast<float>(bitmap.width);
        result.height = static_cast<float>(bitmap.rows);
        result.uv = {static_cast<float>(x_origin) / page_side,
                     static_cast<float>(y_origin) / page_side,
                     result.width / page_side, result.height / page_side};

        // This is the last potentially throwing operation. Only then commit pixels,
        // shelf cursors, and page ownership; failed insertions cannot consume space.
        impl_->glyphs.emplace(key, result);
        for (std::uint32_t y = 0; y < bitmap.rows; ++y) {
            const auto* source = row_at(y);
            auto* destination = page->pixels.data() +
                (std::size_t{y_origin + y} * page_side + x_origin) * 4 + 3;
            for (std::uint32_t x = 0; x < bitmap.width; ++x) {
                const auto coverage = gray_max == 255 ? source[x] :
                    (static_cast<unsigned>(source[x]) * 255 + gray_max / 2) / gray_max;
                destination[std::size_t{x} * 4] = static_cast<std::byte>(coverage);
            }
        }
        page->x = placement->next_x;
        page->y = placement->y;
        page->row_height = placement->row_height;
        page->dirty = true;
        if (new_page) {
            impl_->pages[impl_->page_count++] = std::move(new_page);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

Status GlyphAtlas::publish() {
    try {
        if (!impl_->textures) {
            return Error{ErrorCode::invalid_argument, "Glyph atlas requires a texture store"};
        }
        for (std::size_t index = 0; index < impl_->page_count; ++index) {
            auto& page = *impl_->pages[index];
            if (!page.dirty) {
                continue;
            }
            if (auto error = impl_->textures->update(page.texture, page.pixels)) {
                return error;
            }
            page.dirty = false;
        }
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}

} // namespace wui::detail
