#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "xt/text/ft_text_shaper.h"
#include "xt/types.h"

/// GlyphAtlas: a small set of shelf-packed R8 textures caching rasterized glyph bitmaps, for the
/// GL backend. See Docs/design/07-text.md §7.4, Docs/tasks/T-29.md.

namespace xt::text {

/// One glyph's placement within the atlas. `x`/`y`/`w`/`h` are pixel rects within texture
/// `texture`; `bearingX`/`bearingY` mirror GlyphBitmap's (left/top, px).
struct AtlasEntry {
    /// Index of the atlas texture this glyph was packed into.
    uint16_t texture = 0;
    /// Left edge of the glyph's rect within `texture`, in pixels.
    uint16_t x = 0;
    /// Top edge of the glyph's rect within `texture`, in pixels.
    uint16_t y = 0;
    /// Width of the glyph's rect, in pixels.
    uint16_t w = 0;
    /// Height of the glyph's rect, in pixels.
    uint16_t h = 0;
    /// Horizontal bearing (left), in pixels; mirrors GlyphBitmap::bearingX.
    int16_t bearingX = 0;
    /// Vertical bearing (top), in pixels; mirrors GlyphBitmap::bearingY.
    int16_t bearingY = 0;
};

/// Up to 4 textures of `textureSize() x textureSize()` R8 pixels, shelf-packed. When a glyph does
/// not fit in any existing texture, a new texture is allocated (up to 4); once all 4 are full,
/// every texture and cache entry is cleared and packing restarts from an empty atlas (`lookup()`
/// still places the requested glyph and returns true; `wasReset()` reports the reset happened).
class GlyphAtlas {
public:
    /// `textureSize` (Docs/tasks/T-31.md: optional, for tests that want to exercise the overflow
    /// path with a small atlas) must be > 0; defaults to the production size, 2048.
    explicit GlyphAtlas(FtTextShaper& shaper, int textureSize = 2048);

    /// Finds (rasterizing and packing on a cache miss) the atlas entry for (font, glyphId) at
    /// `sizePx`, rasterized at horizontal subpixel offset `subpixelX` px (quantized to
    /// 0/0.25/0.5/0.75, one cache entry each -- T-101). Returns false only if the shaper has no
    /// bitmap for this glyph (e.g. it has no outline) or the glyph is larger than a whole empty
    /// texture.
    bool lookup(FontId font, uint32_t glyphId, float sizePx, float subpixelX, AtlasEntry& out);
    /// `lookup` at subpixel offset 0.
    bool lookup(FontId font, uint32_t glyphId, float sizePx, AtlasEntry& out) {
        return lookup(font, glyphId, sizePx, 0.0f, out);
    }

    /// Number of textures allocated so far (0..4).
    uint32_t textureCount() const { return static_cast<uint32_t>(textures_.size()); }
    /// Side length, in pixels, of each atlas texture (as passed to the constructor).
    int textureSize() const { return textureSize_; }

    /// Calls fn(textureIndex, x, y, w, h, r8Pixels, stride) once per texture with pending
    /// changes since the last flushUploads() call, covering (at least) every glyph rect added in
    /// that span; clears the pending state afterward.
    template <class F>
    void flushUploads(F&& fn) {
        for (size_t i = 0; i < textures_.size(); ++i) {
            Texture& tex = textures_[i];
            if (!tex.dirty) continue;
            fn(static_cast<uint16_t>(i), tex.dirtyX, tex.dirtyY, tex.dirtyW, tex.dirtyH, tex.pixels.data(),
               textureSize());
            tex.dirty = false;
        }
    }

    /// True if the most recent lookup() triggered a full-atlas reset; clears the flag on read.
    bool wasReset();

    /// True when some texture has changes flushUploads() has not delivered yet (T-162: a frame
    /// built before those changes cannot be resubmitted as is).
    bool hasPendingUploads() const {
        for (const Texture& tex : textures_) {
            if (tex.dirty) return true;
        }
        return false;
    }

private:
    struct Shelf {
        int y = 0;
        int height = 0;
        int cursorX = 0;
    };
    struct Texture {
        std::vector<uint8_t> pixels;  // textureSize() * textureSize(), row-major
        std::vector<Shelf> shelves;
        int nextShelfY = 0;
        bool dirty = false;
        int dirtyX = 0, dirtyY = 0, dirtyW = 0, dirtyH = 0;
    };

    struct Key {
        FontId font;
        uint32_t glyphId;
        uint32_t roundedSize;  // round(sizePx * 4)
        uint32_t subpixel;     // 0..3 == 0/0.25/0.5/0.75 px horizontal offset (T-101)
        bool operator==(const Key& o) const {
            return font == o.font && glyphId == o.glyphId && roundedSize == o.roundedSize &&
                   subpixel == o.subpixel;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = std::hash<uint32_t>()(k.font);
            h = h * 1000003u ^ std::hash<uint32_t>()(k.glyphId);
            h = h * 1000003u ^ std::hash<uint32_t>()(k.roundedSize);
            h = h * 1000003u ^ std::hash<uint32_t>()(k.subpixel);
            return h;
        }
    };

    FtTextShaper* shaper_;
    int textureSize_;
    std::vector<Texture> textures_;
    std::unordered_map<Key, AtlasEntry, KeyHash> cache_;
    bool wasReset_ = false;

    static void markDirty(Texture& tex, int x, int y, int w, int h);
    // Tries to place a w x h box in `tex`'s shelves (creating a new shelf if needed), within a
    // `size` x `size` texture; returns true and fills (x, y) on success.
    static bool place(Texture& tex, int w, int h, int size, int& x, int& y);
    void resetAll();
};

}  // namespace xt::text
