#pragma once

#include <wui/draw.hpp>
#include <wui/text.hpp>

#include "../core/float_bits.hpp"
#include "simd.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace wui::detail {

// What Canvas::image does with one glyph of a run, its bounds moved by the
// run's offset: fail (invalid bounds or uv), skip (empty bounds), or append
// the quad.
enum class GlyphVerdict { invalid, empty, quad };

// Computes a glyph's verdict and, for a quad, its four vertices and six
// indices exactly as Canvas::image computes them: corner (x, y) is the
// moved bounds origin, x + w and y + h its far edges, and likewise for the
// uv, every sum taken in the same order and precision; the indices are
// base + (0, 1, 2, 0, 2, 3). The verdict is image()'s: valid_rect() of the
// moved bounds and of the uv (finite far edges, nonnegative extents), then
// empty when the bounds width or height is zero. The scalar and packed
// versions return the same verdict and write the same vertex and index
// bits; they differ only in instructions. Either may also write vertices
// for a glyph that is not a quad, and the packed version writes two more
// indices (base + 4 and base + 5) after the six, so callers keep two slack
// index slots after the last quad.
class GlyphCorners {
public:
    GlyphCorners(Vec2 offset, Color premultiplied)
        : offset_(offset), color_(premultiplied)
#ifdef WUI_PAINT_SSE2
          ,
          offset4_(_mm_setr_ps(offset.x, offset.y, 0.0f, 0.0f)),
          color4_(_mm_setr_ps(premultiplied.r, premultiplied.g, premultiplied.b,
                              premultiplied.a))
#endif
    {
    }

    GlyphVerdict operator()(const GlyphQuad& glyph, Vertex* out, std::uint32_t* indices,
                            std::uint32_t base) const {
#ifdef WUI_PAINT_SSE2
        return packed(glyph, out, indices, base);
#else
        return scalar(glyph, out, indices, base);
#endif
    }

    GlyphVerdict scalar(const GlyphQuad& glyph, Vertex* out, std::uint32_t* indices,
                        std::uint32_t base) const {
        // `low` holds the moved bounds origin and the uv origin, `high`
        // adds the extents.
        const std::array low{glyph.bounds.x + offset_.x, glyph.bounds.y + offset_.y,
                             glyph.uv.x, glyph.uv.y};
        const std::array extent{glyph.bounds.w, glyph.bounds.h, glyph.uv.w, glyph.uv.h};
        std::array<float, 4> high;
        std::uint32_t nonfinite = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            high[k] = low[k] + extent[k];
            nonfinite |= nonfinite_bits(high[k]);
        }
        // A NaN extent makes its sum NaN, so the minimum needs to be exact
        // only when no extent is NaN, and then it is.
        const float least_size = std::min(extent[0], extent[1]);
        if ((nonfinite & nonfinite_flag) != 0 ||
            !(std::min(least_size, std::min(extent[2], extent[3])) >= 0.0f)) {
            return GlyphVerdict::invalid;
        }
        // Both extents are now nonnegative numbers: one is zero exactly when
        // their minimum is not positive.
        if (!(least_size > 0.0f)) {
            return GlyphVerdict::empty;
        }
        out[0] = {{low[0], low[1]}, {low[2], low[3]}, color_};
        out[1] = {{high[0], low[1]}, {high[2], low[3]}, color_};
        out[2] = {{high[0], high[1]}, {high[2], high[3]}, color_};
        out[3] = {{low[0], high[1]}, {low[2], high[3]}, color_};
        const std::array quad{base, base + 1, base + 2, base, base + 2, base + 3};
        std::copy(quad.begin(), quad.end(), indices);
        return GlyphVerdict::quad;
    }

#ifdef WUI_PAINT_SSE2
    // The same lanes as scalar(): low = (x, y, u, v), extent = (w, h, uw,
    // uh), high = low + extent, with IEEE single precision adds; the
    // finiteness flags are float_bits.hpp's, and the comparisons are the
    // ordered ones (false for NaN) that scalar() uses.
    GlyphVerdict packed(const GlyphQuad& glyph, Vertex* out, std::uint32_t* indices,
                        std::uint32_t base) const {
        const __m128 bounds = _mm_loadu_ps(&glyph.bounds.x);
        const __m128 uv = _mm_loadu_ps(&glyph.uv.x);
        // Lanes 2 and 3 of the moved bounds are not used.
        const __m128 low = _mm_movelh_ps(_mm_add_ps(bounds, offset4_), uv);
        const __m128 extent = _mm_movehl_ps(uv, bounds);
        const __m128 high = _mm_add_ps(low, extent);
        const __m128i flags = _mm_add_epi32(
            _mm_and_si128(_mm_castps_si128(high), _mm_set1_epi32(0x7f800000)),
            _mm_set1_epi32(0x00800000));
        const __m128 zero = _mm_setzero_ps();
        if (_mm_movemask_ps(_mm_castsi128_ps(flags)) != 0 ||
            _mm_movemask_ps(_mm_cmpge_ps(extent, zero)) != 0xf) {
            return GlyphVerdict::invalid;
        }
        if ((_mm_movemask_ps(_mm_cmpgt_ps(extent, zero)) & 0x3) != 0x3) {
            return GlyphVerdict::empty;
        }
        const __m128 xy = _mm_unpacklo_ps(high, low); // (x + w, x, y + h, y)
        const __m128 uvs = _mm_unpackhi_ps(high, low); // (u + uw, u, v + uh, v)
        const __m128 right_top = _mm_shuffle_ps(xy, uvs, _MM_SHUFFLE(3, 0, 3, 0));
        const __m128 left_bottom = _mm_shuffle_ps(xy, uvs, _MM_SHUFFLE(2, 1, 2, 1));
        _mm_storeu_ps(&out[0].position.x, low);
        _mm_storeu_ps(&out[0].premultiplied.r, color4_);
        _mm_storeu_ps(&out[1].position.x, right_top);
        _mm_storeu_ps(&out[1].premultiplied.r, color4_);
        _mm_storeu_ps(&out[2].position.x, high);
        _mm_storeu_ps(&out[2].premultiplied.r, color4_);
        _mm_storeu_ps(&out[3].position.x, left_bottom);
        _mm_storeu_ps(&out[3].premultiplied.r, color4_);
        const __m128i first = _mm_set1_epi32(static_cast<int>(base));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(indices),
                         _mm_add_epi32(first, _mm_setr_epi32(0, 1, 2, 0)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(indices + 4),
                         _mm_add_epi32(first, _mm_setr_epi32(2, 3, 4, 5)));
        return GlyphVerdict::quad;
    }
#endif

private:
    Vec2 offset_;
    Color color_;
#ifdef WUI_PAINT_SSE2
    __m128 offset4_;
    __m128 color4_;
#endif
};

} // namespace wui::detail
