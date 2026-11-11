#include "xt/gpu/coverage_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

/// See Docs/design/10-backends.md §10.11 item 2, Docs/tasks/T-660a.md item 3. Packing follows
/// Code/Runtime/text/src/glyph_atlas.cpp (shelves, heights rounded up to 4, T-648 growth).

namespace xt::gpu {

namespace {

/// Bit pattern of `v` with -0 normalized to +0 (the caller has rejected NaN).
uint32_t linearBits(float v) {
    if (v == 0.0f) {
        v = 0.0f;
    }
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

/// Lexicographic order over every key field.
bool keyLess(const CoverageKey& a, const CoverageKey& b) {
    if (a.pathHash != b.pathHash) return a.pathHash < b.pathHash;
    if (a.fillRule != b.fillRule) return a.fillRule < b.fillRule;
    for (int i = 0; i < 4; ++i) {
        if (a.m[i] != b.m[i]) return a.m[i] < b.m[i];
    }
    if (a.subX != b.subX) return a.subX < b.subX;
    if (a.subY != b.subY) return a.subY < b.subY;
    // T-672: a shape's header (tile 0) sorts before its tiles, the tiles in row-major order.
    if (a.tile != b.tile) return a.tile < b.tile;
    if (a.tileY != b.tileY) return a.tileY < b.tileY;
    return a.tileX < b.tileX;
}

bool keyEqual(const CoverageKey& a, const CoverageKey& b) { return !keyLess(a, b) && !keyLess(b, a); }

}  // namespace

bool coverageTileGrid(int width, int height, int& cols, int& rows) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const int64_t c = (static_cast<int64_t>(width) + kCoverageTileSide - 1) / kCoverageTileSide;
    const int64_t r = (static_cast<int64_t>(height) + kCoverageTileSide - 1) / kCoverageTileSide;
    if (c * r > kMaxCoverageTiles) {
        return false;
    }
    cols = static_cast<int>(c);
    rows = static_cast<int>(r);
    return true;
}

bool splitCoverageTranslation(float t, int32_t& whole, uint8_t& sub) {
    if (!std::isfinite(t)) {
        return false;
    }
    double w = std::floor(static_cast<double>(t));
    long phase = std::lround((static_cast<double>(t) - w) * 4.0);
    if (phase >= 4) {
        phase = 0;
        w += 1.0;
    }
    if (w < -2147483648.0 || w > 2147483647.0) {
        return false;
    }
    whole = static_cast<int32_t>(w);
    sub = static_cast<uint8_t>(phase);
    return true;
}

bool makeCoverageKey(uint64_t pathHash, FillRule rule, const Mat2x3& deviceFromLocal, CoverageKey& out) {
    const float lin[4] = {deviceFromLocal.a, deviceFromLocal.b, deviceFromLocal.c, deviceFromLocal.d};
    for (float v : lin) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    CoverageKey key;
    int32_t wholeX = 0, wholeY = 0;
    if (!splitCoverageTranslation(deviceFromLocal.e, wholeX, key.subX) ||
        !splitCoverageTranslation(deviceFromLocal.f, wholeY, key.subY)) {
        return false;
    }
    key.pathHash = pathHash;
    key.fillRule = static_cast<uint8_t>(rule);
    for (int i = 0; i < 4; ++i) {
        key.m[i] = linearBits(lin[i]);
    }
    out = key;
    return true;
}

CoverageAtlas::CoverageAtlas(int pageSize, int initialSize, int maxPages)
    : m_pageSize(pageSize > 0 ? pageSize : kDefaultPageSize),
      m_initialSize(initialSize >= 1 && initialSize <= m_pageSize ? initialSize : m_pageSize),
      m_maxPages(maxPages >= 1 ? maxPages : 1) {}

void CoverageAtlas::beginFrame() {
    ++m_frame;
    m_frameRasterized = 0;
}

void CoverageAtlas::endFrame() {
    // Nothing is deferred to the end of a frame: use marks and the budget belong to the frame
    // until the next beginFrame().
}

bool CoverageAtlas::budgetAllows() const {
    // T-662 (§10.11 item 2, supplement 2): accounted after the fact, so only what was already used
    // is known here; the frame's first shape is always allowed (nothing used yet).
    return m_frameRasterized < m_budget;
}

void CoverageAtlas::noteRasterized(uint64_t pixels) { m_frameRasterized += pixels; }

size_t CoverageAtlas::findSlot(const CoverageKey& key) const {
    auto it = std::lower_bound(m_slots.begin(), m_slots.end(), key,
                               [](const Slot& s, const CoverageKey& k) { return keyLess(s.key, k); });
    if (it != m_slots.end() && keyEqual(it->key, key)) {
        return static_cast<size_t>(it - m_slots.begin());
    }
    return m_slots.size();
}

CoverageEntry CoverageAtlas::entryOf(const Slot& slot) const {
    const int size = m_pages[slot.page].size;
    const float inv = 1.0f / static_cast<float>(size);
    CoverageEntry e;
    e.page = slot.page;
    e.kind = slot.kind;
    if (slot.kind == CoverageEntryKind::Bitmap) {
        e.u0 = static_cast<float>(slot.x) * inv;
        e.v0 = static_cast<float>(slot.y) * inv;
        e.u1 = static_cast<float>(slot.x + slot.w) * inv;
        e.v1 = static_cast<float>(slot.y + slot.h) * inv;
    } else if (slot.kind == CoverageEntryKind::Full) {
        // The centre of the page's reserved all-255 block.
        const float c = static_cast<float>(size - kCoverageReservedSide / 2) * inv;
        e.u0 = e.u1 = c;
        e.v0 = e.v1 = c;
    }
    e.width = slot.w;
    e.height = slot.h;
    e.offsetX = slot.offsetX;
    e.offsetY = slot.offsetY;
    return e;
}

bool CoverageAtlas::lookup(const CoverageKey& key, CoverageEntry& out) {
    const size_t i = findSlot(key);
    if (i == m_slots.size()) {
        ++m_misses;
        return false;
    }
    ++m_hits;
    m_pages[m_slots[i].page].lastUsedFrame = m_frame;
    out = entryOf(m_slots[i]);
    return true;
}

void CoverageAtlas::markDirty(Page& page, int x, int y, int w, int h) {
    if (!page.dirty) {
        page.dirty = true;
        page.dirtyX = x;
        page.dirtyY = y;
        page.dirtyW = w;
        page.dirtyH = h;
        return;
    }
    const int x0 = std::min(page.dirtyX, x);
    const int y0 = std::min(page.dirtyY, y);
    const int x1 = std::max(page.dirtyX + page.dirtyW, x + w);
    const int y1 = std::max(page.dirtyY + page.dirtyH, y + h);
    page.dirtyX = x0;
    page.dirtyY = y0;
    page.dirtyW = x1 - x0;
    page.dirtyH = y1 - y0;
}

void CoverageAtlas::writeReserved(Page& page) {
    const int r = std::min(kCoverageReservedSide, page.size);
    const int x0 = page.size - r;
    const size_t stride = static_cast<size_t>(page.size);
    for (int row = x0; row < page.size; ++row) {
        std::fill_n(page.pixels.data() + static_cast<size_t>(row) * stride + static_cast<size_t>(x0),
                    static_cast<size_t>(r), uint8_t{255});
    }
    markDirty(page, x0, x0, r, r);
}

bool CoverageAtlas::place(Page& page, int w, int h, int& x, int& y) {
    if (w > page.size || h > page.size) return false;
    // T-672: shelves reaching the reserved rows stop short of the reserved columns.
    const int corner = page.size - kCoverageReservedSide;
    for (Shelf& shelf : page.shelves) {
        const int limit = shelf.y + shelf.height > corner ? corner : page.size;
        if (h <= shelf.height && shelf.cursorX + w <= limit) {
            x = shelf.cursorX;
            y = shelf.y;
            shelf.cursorX += w;  // the box already carries its 1 px margins
            return true;
        }
    }
    const int shelfHeight = std::min(((h + 3) / 4) * 4, page.size);
    if (page.nextShelfY + shelfHeight > page.size) return false;
    if (page.nextShelfY + shelfHeight > corner && w > corner) return false;
    page.shelves.push_back(Shelf{page.nextShelfY, shelfHeight, w});
    x = 0;
    y = page.nextShelfY;
    page.nextShelfY += shelfHeight;
    return true;
}

void CoverageAtlas::growPage(Page& page, int newSize) {
    const size_t oldSize = static_cast<size_t>(page.size);
    const size_t size = static_cast<size_t>(newSize);
    std::vector<uint8_t> pixels(size * size, 0);
    for (size_t row = 0; row < oldSize; ++row) {
        std::copy_n(page.pixels.begin() + static_cast<std::ptrdiff_t>(row * oldSize), oldSize,
                    pixels.begin() + static_cast<std::ptrdiff_t>(row * size));
    }
    page.pixels.swap(pixels);
    page.size = newSize;
    page.dirty = false;
    markDirty(page, 0, 0, newSize, newSize);
    writeReserved(page);  // T-672: the grown page's own corner
}

void CoverageAtlas::addPage(int size) {
    Page page;
    page.size = size;
    page.pixels.assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
    page.lastUsedFrame = m_frame;
    writeReserved(page);  // T-672
    m_pages.push_back(std::move(page));
}

bool CoverageAtlas::tryPlace(int w, int h, uint16_t& pageIndex, int& x, int& y) {
    for (size_t i = 0; i < m_pages.size(); ++i) {
        if (place(m_pages[i], w, h, x, y)) {
            pageIndex = static_cast<uint16_t>(i);
            return true;
        }
    }
    for (size_t i = 0; i < m_pages.size(); ++i) {
        if (m_pages[i].size < m_pageSize) {
            growPage(m_pages[i], m_pageSize);
            m_resized = true;
            if (place(m_pages[i], w, h, x, y)) {
                pageIndex = static_cast<uint16_t>(i);
                return true;
            }
        }
    }
    if (static_cast<int>(m_pages.size()) < m_maxPages) {
        addPage(m_pages.empty() ? m_initialSize : m_pageSize);
        const size_t i = m_pages.size() - 1;
        if (place(m_pages[i], w, h, x, y)) {
            pageIndex = static_cast<uint16_t>(i);
            return true;
        }
        // A first page smaller than the box: grow it right away.
        if (m_pages[i].size < m_pageSize) {
            growPage(m_pages[i], m_pageSize);
            m_resized = true;
            if (place(m_pages[i], w, h, x, y)) {
                pageIndex = static_cast<uint16_t>(i);
                return true;
            }
        }
    }
    return false;
}

void CoverageAtlas::recyclePage(uint16_t pageIndex) {
    Page& page = m_pages[pageIndex];
    page.shelves.clear();
    page.nextShelfY = 0;
    writeReserved(page);  // T-672: rewritten on recycle (never packed over, so it still holds 255)
    m_slots.erase(std::remove_if(m_slots.begin(), m_slots.end(),
                                 [pageIndex](const Slot& s) { return s.page == pageIndex; }),
                  m_slots.end());
    ++m_recycles;
}

bool CoverageAtlas::insert(const CoverageKey& key, const CoverageBitmap& bitmap, CoverageEntry& out) {
    if (bitmap.width <= 0 || bitmap.height <= 0 ||
        bitmap.pixels.size() < static_cast<size_t>(bitmap.width) * static_cast<size_t>(bitmap.height)) {
        return false;
    }
    return insertBox(key, bitmap.pixels.data(), static_cast<size_t>(bitmap.width), bitmap.width,
                     bitmap.height, bitmap.originX, bitmap.originY, out);
}

bool CoverageAtlas::insertBox(const CoverageKey& key, const uint8_t* pixels, size_t srcStride, int w, int h,
                              int32_t offsetX, int32_t offsetY, CoverageEntry& out) {
    const int boxW = w + 2;
    const int boxH = h + 2;
    if (boxW > m_pageSize || boxH > m_pageSize) {
        return false;
    }
    const size_t existing = findSlot(key);
    if (existing != m_slots.size()) {
        m_pages[m_slots[existing].page].lastUsedFrame = m_frame;
        out = entryOf(m_slots[existing]);
        return true;
    }

    uint16_t pageIndex = 0;
    int bx = 0, by = 0;
    if (!tryPlace(boxW, boxH, pageIndex, bx, by)) {
        // Recycle the page unused for the most frames; a page used this frame is never recycled.
        size_t victim = m_pages.size();
        for (size_t i = 0; i < m_pages.size(); ++i) {
            if (m_pages[i].lastUsedFrame == m_frame) continue;
            if (victim == m_pages.size() || m_pages[i].lastUsedFrame < m_pages[victim].lastUsedFrame) {
                victim = i;
            }
        }
        if (victim == m_pages.size()) {
            return false;
        }
        pageIndex = static_cast<uint16_t>(victim);
        recyclePage(pageIndex);
        if (!place(m_pages[pageIndex], boxW, boxH, bx, by)) {
            return false;
        }
    }

    // Write the whole box: the bitmap plus a zero margin (a recycled page holds stale pixels).
    Page& page = m_pages[pageIndex];
    const size_t stride = static_cast<size_t>(page.size);
    for (int row = 0; row < boxH; ++row) {
        uint8_t* dst = page.pixels.data() + static_cast<size_t>(by + row) * stride + static_cast<size_t>(bx);
        if (row == 0 || row == boxH - 1) {
            std::fill_n(dst, static_cast<size_t>(boxW), uint8_t{0});
            continue;
        }
        dst[0] = 0;
        const uint8_t* src = pixels + static_cast<size_t>(row - 1) * srcStride;
        std::copy_n(src, static_cast<size_t>(w), dst + 1);
        dst[boxW - 1] = 0;
    }
    markDirty(page, bx, by, boxW, boxH);
    page.lastUsedFrame = m_frame;

    Slot slot;
    slot.key = key;
    slot.page = pageIndex;
    slot.x = static_cast<uint16_t>(bx + 1);
    slot.y = static_cast<uint16_t>(by + 1);
    slot.w = static_cast<uint16_t>(w);
    slot.h = static_cast<uint16_t>(h);
    slot.offsetX = offsetX;
    slot.offsetY = offsetY;
    auto it = std::lower_bound(m_slots.begin(), m_slots.end(), key,
                               [](const Slot& s, const CoverageKey& k) { return keyLess(s.key, k); });
    it = m_slots.insert(it, slot);
    out = entryOf(*it);
    return true;
}

void CoverageAtlas::insertMarker(const CoverageKey& key, CoverageEntryKind kind, uint16_t page, int w, int h,
                                 int32_t offsetX, int32_t offsetY, CoverageEntry& out) {
    m_pages[page].lastUsedFrame = m_frame;
    const size_t existing = findSlot(key);
    if (existing != m_slots.size()) {
        out = entryOf(m_slots[existing]);
        return;
    }
    Slot slot;
    slot.key = key;
    slot.kind = kind;
    slot.page = page;
    slot.w = static_cast<uint16_t>(w);
    slot.h = static_cast<uint16_t>(h);
    slot.offsetX = offsetX;
    slot.offsetY = offsetY;
    auto it = std::lower_bound(m_slots.begin(), m_slots.end(), key,
                               [](const Slot& s, const CoverageKey& k) { return keyLess(s.key, k); });
    it = m_slots.insert(it, slot);
    out = entryOf(*it);
}

bool CoverageAtlas::lookupTiles(const CoverageKey& shapeKey, const CoverageEntry& header,
                                std::vector<CoverageEntry>& tiles) {
    int cols = 0, rows = 0;
    if (header.kind != CoverageEntryKind::Tiled ||
        !coverageTileGrid(header.width, header.height, cols, rows)) {
        return false;
    }
    tiles.resize(static_cast<size_t>(cols) * static_cast<size_t>(rows));
    CoverageKey key = shapeKey;
    key.tile = 1;
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            key.tileX = static_cast<uint16_t>(tx);
            key.tileY = static_cast<uint16_t>(ty);
            const size_t i = findSlot(key);
            if (i == m_slots.size()) {
                return false;
            }
            m_pages[m_slots[i].page].lastUsedFrame = m_frame;
            tiles[static_cast<size_t>(ty) * static_cast<size_t>(cols) + static_cast<size_t>(tx)] =
                entryOf(m_slots[i]);
        }
    }
    return true;
}

bool CoverageAtlas::insertTiles(const CoverageKey& shapeKey, const CoverageBitmap& bitmap,
                                CoverageEntry& header, std::vector<CoverageEntry>& tiles) {
    int cols = 0, rows = 0;
    if (!coverageTileGrid(bitmap.width, bitmap.height, cols, rows) ||
        bitmap.pixels.size() < static_cast<size_t>(bitmap.width) * static_cast<size_t>(bitmap.height)) {
        return false;
    }
    tiles.assign(static_cast<size_t>(cols) * static_cast<size_t>(rows), CoverageEntry{});
    const size_t stride = static_cast<size_t>(bitmap.width);
    CoverageKey key = shapeKey;
    key.tile = 1;
    // Pass 1: classify every tile and pack the edge tiles (a cached one is kept as it is).
    uint16_t home = 0;
    bool haveHome = false;
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            const int x0 = tx * kCoverageTileSide;
            const int y0 = ty * kCoverageTileSide;
            const int w = std::min(kCoverageTileSide, bitmap.width - x0);
            const int h = std::min(kCoverageTileSide, bitmap.height - y0);
            const uint8_t* base =
                bitmap.pixels.data() + static_cast<size_t>(y0) * stride + static_cast<size_t>(x0);
            bool allZero = true;
            bool allFull = true;
            for (int row = 0; row < h && (allZero || allFull); ++row) {
                const uint8_t* p = base + static_cast<size_t>(row) * stride;
                for (int x = 0; x < w; ++x) {
                    allZero = allZero && p[x] == 0;
                    allFull = allFull && p[x] == 255;
                }
            }
            CoverageEntry& out =
                tiles[static_cast<size_t>(ty) * static_cast<size_t>(cols) + static_cast<size_t>(tx)];
            if (allZero || allFull) {
                out.kind = allZero ? CoverageEntryKind::Empty : CoverageEntryKind::Full;
                continue;  // recorded in pass 2, on the home page
            }
            key.tileX = static_cast<uint16_t>(tx);
            key.tileY = static_cast<uint16_t>(ty);
            if (!insertBox(key, base, stride, w, h, bitmap.originX + x0, bitmap.originY + y0, out)) {
                return false;
            }
            if (!haveHome) {
                home = out.page;
                haveHome = true;
            }
        }
    }
    if (!haveHome) {
        if (m_pages.empty()) {
            addPage(m_initialSize);
        }
        home = 0;
    }
    // Pass 2: the uniform tiles, then the header.
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            CoverageEntry& out =
                tiles[static_cast<size_t>(ty) * static_cast<size_t>(cols) + static_cast<size_t>(tx)];
            if (out.kind == CoverageEntryKind::Bitmap) {
                continue;
            }
            const int x0 = tx * kCoverageTileSide;
            const int y0 = ty * kCoverageTileSide;
            key.tileX = static_cast<uint16_t>(tx);
            key.tileY = static_cast<uint16_t>(ty);
            insertMarker(key, out.kind, home, std::min(kCoverageTileSide, bitmap.width - x0),
                         std::min(kCoverageTileSide, bitmap.height - y0), bitmap.originX + x0,
                         bitmap.originY + y0, out);
        }
    }
    CoverageKey headerKey = shapeKey;
    headerKey.tile = 0;
    headerKey.tileX = 0;
    headerKey.tileY = 0;
    insertMarker(headerKey, CoverageEntryKind::Tiled, home, bitmap.width, bitmap.height, bitmap.originX,
                 bitmap.originY, header);
    return true;
}

bool CoverageAtlas::hasPendingUploads() const {
    for (const Page& page : m_pages) {
        if (page.dirty) return true;
    }
    return false;
}

bool CoverageAtlas::wasResized() {
    const bool r = m_resized;
    m_resized = false;
    return r;
}

int CoverageAtlas::pageSize(uint32_t page) const { return page < m_pages.size() ? m_pages[page].size : 0; }

uint64_t CoverageAtlas::bytes() const {
    uint64_t total = 0;
    for (const Page& page : m_pages) {
        total += page.pixels.size();
    }
    return total;
}

std::span<const uint8_t> CoverageAtlas::pagePixels(uint32_t page) const {
    if (page >= m_pages.size()) {
        return {};
    }
    return std::span<const uint8_t>(m_pages[page].pixels);
}

CoverageAtlasStats CoverageAtlas::stats() const {
    CoverageAtlasStats s;
    s.pages = static_cast<uint32_t>(m_pages.size());
    s.entries = static_cast<uint32_t>(m_slots.size());
    s.hits = m_hits;
    s.misses = m_misses;
    s.frameRasterizedPixels = m_frameRasterized;
    s.recycles = m_recycles;
    return s;
}

}  // namespace xt::gpu
