#include "xt/text/glyph_atlas.h"

#include <algorithm>
#include <cmath>

/// Shelf-packed R8 glyph atlas. See docs/design/07-text.md §7.4, docs/tasks/T-29.md.

namespace xt::text {

GlyphAtlas::GlyphAtlas(FtTextShaper& shaper, int textureSize) : shaper_(&shaper), textureSize_(textureSize) {}

void GlyphAtlas::markDirty(Texture& tex, int x, int y, int w, int h) {
    if (!tex.dirty) {
        tex.dirty = true;
        tex.dirtyX = x;
        tex.dirtyY = y;
        tex.dirtyW = w;
        tex.dirtyH = h;
        return;
    }
    const int x0 = std::min(tex.dirtyX, x);
    const int y0 = std::min(tex.dirtyY, y);
    const int x1 = std::max(tex.dirtyX + tex.dirtyW, x + w);
    const int y1 = std::max(tex.dirtyY + tex.dirtyH, y + h);
    tex.dirtyX = x0;
    tex.dirtyY = y0;
    tex.dirtyW = x1 - x0;
    tex.dirtyH = y1 - y0;
}

bool GlyphAtlas::place(Texture& tex, int w, int h, int size, int& x, int& y) {
    if (w > size || h > size) return false;
    for (Shelf& shelf : tex.shelves) {
        if (h <= shelf.height && shelf.cursorX + w <= size) {
            x = shelf.cursorX;
            y = shelf.y;
            shelf.cursorX += w + 1;  // 1px margin between glyphs
            return true;
        }
    }
    // New shelf: height rounded up to a multiple of 4 (docs/tasks/T-29.md "每架高度 = 首個字形
    // 高度向上取 4 的倍數").
    const int shelfHeight = ((h + 3) / 4) * 4;
    if (tex.nextShelfY + shelfHeight > size) return false;
    Shelf shelf{tex.nextShelfY, shelfHeight, w + 1};
    x = 0;
    y = tex.nextShelfY;
    tex.nextShelfY += shelfHeight + 1;
    tex.shelves.push_back(shelf);
    return true;
}

void GlyphAtlas::resetAll() {
    textures_.clear();
    cache_.clear();
}

bool GlyphAtlas::lookup(FontId font, uint32_t glyphId, float sizePx, float subpixelX, AtlasEntry& out) {
    const uint32_t roundedSize = static_cast<uint32_t>(std::lround(sizePx * 4.0f));
    const uint32_t subpixel = static_cast<uint32_t>(std::lround(subpixelX * 4.0f)) & 3u;
    const Key key{font, glyphId, roundedSize, subpixel};
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        out = it->second;
        return true;
    }

    GlyphBitmap bitmap;
    if (!shaper_->glyphBitmap(font, glyphId, sizePx, static_cast<float>(subpixel) * 0.25f, bitmap))
        return false;

    for (size_t attempt = 0; attempt < 2; ++attempt) {
        for (size_t i = 0; i < textures_.size(); ++i) {
            int x = 0, y = 0;
            if (place(textures_[i], bitmap.width, bitmap.height, textureSize_, x, y)) {
                Texture& tex = textures_[i];
                for (int row = 0; row < bitmap.height; ++row) {
                    const size_t dst = static_cast<size_t>(y + row) * static_cast<size_t>(textureSize()) +
                                       static_cast<size_t>(x);
                    const size_t src = static_cast<size_t>(row) * static_cast<size_t>(bitmap.width);
                    std::copy_n(bitmap.alpha.begin() + static_cast<long>(src),
                                static_cast<size_t>(bitmap.width),
                                tex.pixels.begin() + static_cast<long>(dst));
                }
                markDirty(tex, x, y, bitmap.width, bitmap.height);
                AtlasEntry entry{static_cast<uint16_t>(i),
                                 static_cast<uint16_t>(x),
                                 static_cast<uint16_t>(y),
                                 static_cast<uint16_t>(bitmap.width),
                                 static_cast<uint16_t>(bitmap.height),
                                 static_cast<int16_t>(bitmap.bearingX),
                                 static_cast<int16_t>(bitmap.bearingY)};
                cache_.emplace(key, entry);
                out = entry;
                return true;
            }
        }
        if (textures_.size() < 4) {
            Texture tex;
            tex.pixels.assign(static_cast<size_t>(textureSize()) * static_cast<size_t>(textureSize()), 0);
            textures_.push_back(std::move(tex));
            continue;  // retry placement into the freshly added texture
        }
        // All 4 textures full: reset and try once more from an empty atlas.
        resetAll();
        wasReset_ = true;
        Texture tex;
        tex.pixels.assign(static_cast<size_t>(textureSize()) * static_cast<size_t>(textureSize()), 0);
        textures_.push_back(std::move(tex));
    }
    return false;  // glyph larger than a whole empty texture
}

bool GlyphAtlas::wasReset() {
    const bool r = wasReset_;
    wasReset_ = false;
    return r;
}

}  // namespace xt::text
