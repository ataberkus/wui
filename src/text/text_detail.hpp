#pragma once

#include "atlas.hpp"

#include <hb.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wui::detail {

struct FontLibrary {
    FT_Library value = nullptr;
    ~FontLibrary();
};

struct FontFace {
    FontFace() = default;
    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    Status initialize(FT_Library library, std::span<const std::byte> source);

    std::vector<std::byte> bytes;
    FT_Face raster = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    std::uint32_t replacement_scalar = 0;
    std::uint32_t replacement_glyph = 0;
};

// Hash of every run key field; computed once per lookup and stored with each
// cached key, so neither probing nor bucket walks rehash key strings.
std::size_t hash_run_key(std::string_view text, std::string_view language, FontRole role,
                         std::uint32_t physical_ppem, float scale) noexcept;

struct RunKeyView {
    std::string_view text, language;
    FontRole role;
    std::uint32_t physical_ppem;
    float scale;
    std::size_t hash; // hash_run_key of the fields above
};

struct RunKey {
    std::string text, language;
    FontRole role;
    std::uint32_t physical_ppem;
    float scale;
    std::size_t hash; // hash_run_key of the fields above
    operator RunKeyView() const noexcept {
        return {text, language, role, physical_ppem, scale, hash};
    }
};

struct RunKeyHash {
    std::size_t operator()(const RunKeyView& key) const noexcept { return key.hash; }
};

struct RunKeyEqual {
    bool operator()(const RunKeyView& left, const RunKeyView& right) const noexcept {
        // The stored hash rejects almost every non-matching node without
        // touching key strings.
        return left.hash == right.hash && left.physical_ppem == right.physical_ppem &&
               left.role == right.role && left.scale == right.scale &&
               left.text == right.text && left.language == right.language;
    }
};

// Payload and entry limits bound both retained runs and container overhead.
inline constexpr std::size_t text_cache_byte_limit = 16 * 1024 * 1024;
inline constexpr std::size_t text_cache_entry_limit = 4096;
inline constexpr std::size_t text_request_byte_limit = 1024 * 1024;
static_assert(text_request_byte_limit <= text_cache_byte_limit);

// Body, heading and display; display may share the semibold face.
inline constexpr std::size_t text_role_count = 3;

struct CacheEntry {
    RunKey key;
    std::shared_ptr<const TextRun> run;
    std::size_t bytes = 0; // exact amount added to cached_bytes_ on insertion
};

class TextService final : public IText {
public:
    explicit TextService(std::shared_ptr<TextureStore> textures);
    ~TextService() override;
    Status initialize(const FontFaces& faces);
    Result<std::shared_ptr<const TextRun>> shape(const ShapeRequest& request) override;
    Status publish() override;
    Result<std::vector<std::size_t>> grapheme_offsets(std::string_view source) override;

private:
    // Evicts least recently used runs held only by the cache until one more
    // run of `bytes` fits both budgets; evicts nothing and returns false when
    // no such set of runs frees enough room.
    bool make_room(std::size_t bytes);

    // Reverse member destruction releases cached runs, atlas/store, faces, library.
    FontLibrary library_;
    std::array<FontFace, text_role_count> fonts_;
    // Face slot per FontRole. A face slot also keys its atlas glyphs, so roles
    // sharing a face share glyphs and distinct faces never alias.
    std::array<std::size_t, text_role_count> role_faces_{0, 1, 1};
    GlyphAtlas atlas_;
    // Most recently used first. Map keys view the strings owned by list nodes.
    std::list<CacheEntry> lru_;
    std::unordered_map<RunKeyView, std::list<CacheEntry>::iterator, RunKeyHash, RunKeyEqual> runs_;
    std::size_t cached_bytes_ = 0;
};

} // namespace wui::detail
