#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/coverage_bitmap.h"
#include "xt/node.h"
#include "xt/types.h"

/// CoverageAtlas: shelf-packed R8 pages caching path coverage bitmaps (rasterizeCoverage) for the
/// GPU frame builder's 1x anti-aliased fills. CPU side only: no GPU resources, no GpuDevice; pages
/// reach a device through flushUploads(). See Docs/design/10-backends.md §10.11 item 2,
/// Docs/tasks/T-660a.md item 3. Same packing and upload model as xt::text::GlyphAtlas.

namespace xt::gpu {

/// T-672 (§10.11 item 2, large-shape tiling): a bitmap wider or taller than this is stored as
/// tiles; one whose sides are both within it is one atlas entry, as before.
inline constexpr int kCoverageTileThreshold = 512;
/// Side of a tile; tiles start at the bitmap's top-left, the ones on the right and bottom edges
/// are smaller.
inline constexpr int kCoverageTileSide = 256;
/// A shape with more tiles than this is not cached (the caller falls back to stencil-then-cover).
inline constexpr int kMaxCoverageTiles = 1024;
/// Side of the all-255 texel block every page reserves in its bottom-right corner (sampled at its
/// centre by full tiles); written when a page is created, grown or recycled, never packed over.
inline constexpr int kCoverageReservedSide = 4;

/// Tile grid of a `width` x `height` bitmap: cols = ceil(width / kCoverageTileSide), rows likewise.
/// Returns false when a side is not positive or cols * rows > kMaxCoverageTiles.
bool coverageTileGrid(int width, int height, int& cols, int& rows);

/// Cache key of one rasterized coverage bitmap (§10.11 item 2).
struct CoverageKey {
    /// Hash of the path's contents (computed by the caller).
    uint64_t pathHash = 0;
    /// The FillRule, as its underlying value.
    uint8_t fillRule = 0;
    /// Bit patterns of the transform's linear part (a, b, c, d) as floats, -0 stored as +0
    /// (T-660b: the linear part is not quantized; only bit-identical parts hit).
    uint32_t m[4] = {0, 0, 0, 0};
    /// Fractional part of the x translation, quantized to quarter pixels (0..3).
    uint8_t subX = 0;
    /// Fractional part of the y translation, quantized to quarter pixels (0..3).
    uint8_t subY = 0;
    /// T-672: 0 = the whole shape (a small shape's bitmap, or a tiled shape's header entry);
    /// 1 = tile (tileX, tileY) of a tiled shape. Whole-shape keys keep tileX = tileY = 0.
    uint8_t tile = 0;
    /// Tile column (T-672; 0 for a whole-shape key).
    uint16_t tileX = 0;
    /// Tile row (T-672; 0 for a whole-shape key).
    uint16_t tileY = 0;
};

/// What a CoverageEntry stands for (T-672).
enum class CoverageEntryKind : uint8_t {
    /// Texels packed on its page (a small shape's bitmap, or an edge tile).
    Bitmap,
    /// A tile whose coverage is 0 everywhere: nothing to draw, no page space.
    Empty,
    /// A tile whose coverage is 255 everywhere: drawn sampling the page's reserved block (u0 == u1,
    /// v0 == v1 = its centre), no page space of its own.
    Full,
    /// A tiled shape's header (whole-shape key): width/height/offset are the whole bitmap's; the
    /// tiles are looked up with lookupTiles().
    Tiled,
};

/// Splits the translation `t` into a whole pixel and a quarter-pixel phase: whole = floor(t),
/// sub = round((t - whole) * 4); a phase that rounds to 4 becomes 0 with whole + 1 (T-660b).
/// Returns false when `t` is not finite or `whole` would leave the int32 range.
bool splitCoverageTranslation(float t, int32_t& whole, uint8_t& sub);

/// Builds the key for `pathHash` drawn with `rule` under `deviceFromLocal` into `out`: m = the
/// bit patterns of (a, b, c, d) with -0 normalized to +0; subX/subY from
/// splitCoverageTranslation(e) / (f). Returns false (the shape is not cacheable; the caller falls
/// back to stencil-then-cover) when any of the six components is NaN or infinite, or a
/// translation is out of range.
bool makeCoverageKey(uint64_t pathHash, FillRule rule, const Mat2x3& deviceFromLocal, CoverageKey& out);

/// Where a cached bitmap lives.
struct CoverageEntry {
    /// Page index.
    uint16_t page = 0;
    /// Normalized texture rect of the bitmap within its page (the 1 px packing margin excluded).
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    /// Bitmap width in pixels (CoverageBitmap::width).
    uint16_t width = 0;
    /// Bitmap height in pixels (CoverageBitmap::height).
    uint16_t height = 0;
    /// Integer offset of the bitmap's top-left from the shape's origin (CoverageBitmap::originX/Y
    /// of the bitmap that was inserted).
    int32_t offsetX = 0;
    /// See offsetX.
    int32_t offsetY = 0;
    /// T-672: Bitmap for every entry of a small shape; see CoverageEntryKind.
    CoverageEntryKind kind = CoverageEntryKind::Bitmap;
};

/// Counters reported by CoverageAtlas::stats().
struct CoverageAtlasStats {
    /// Pages allocated.
    uint32_t pages = 0;
    /// Cached entries.
    uint32_t entries = 0;
    /// lookup() calls that found their key (lifetime).
    uint64_t hits = 0;
    /// lookup() calls that did not (lifetime).
    uint64_t misses = 0;
    /// Pixels noted by noteRasterized() since the last beginFrame().
    uint64_t frameRasterizedPixels = 0;
    /// Pages recycled by insert() (lifetime).
    uint64_t recycles = 0;
};

/// Up to `maxPages` R8 pages. The first page starts at `initialSize` and grows in place to
/// `pageSize` when nothing fits any more; later pages are `pageSize` from the start. Each bitmap
/// takes its size plus a 1 px margin on every side; the kCoverageReservedSide block in each page's
/// bottom-right corner is never packed over (T-672). When a bitmap fits nowhere and no page can
/// grow or be added, the page unused for the most frames is recycled whole (every key on it is
/// dropped) and the insert is retried once; a page used in the current frame is never recycled.
class CoverageAtlas {
public:
    /// Default page side (§10.11: 1024²) and first-page starting side (512², as T-648's glyph atlas).
    static constexpr int kDefaultPageSize = 1024;
    /// See kDefaultPageSize.
    static constexpr int kDefaultInitialSize = 512;
    /// Default page limit.
    static constexpr int kDefaultMaxPages = 4;
    /// Default per-frame budget of newly rasterized pixels (§10.11 item 2: 2 Mpx).
    static constexpr uint64_t kDefaultFrameBudget = 2000000;

    /// `pageSize` > 0; `initialSize` in 1..pageSize (anything else starts at `pageSize`);
    /// `maxPages` >= 1 (anything less means 1).
    explicit CoverageAtlas(int pageSize = kDefaultPageSize, int initialSize = kDefaultInitialSize,
                           int maxPages = kDefaultMaxPages);

    /// Starts a frame: advances the frame counter and zeroes the frame's rasterized-pixel count.
    void beginFrame();
    /// Ends the frame started by beginFrame().
    void endFrame();

    /// Sets the per-frame budget of newly rasterized pixels.
    void setFrameBudget(uint64_t pixels) { m_budget = pixels; }
    /// The per-frame budget.
    uint64_t frameBudget() const { return m_budget; }
    /// True while this frame's rasterized count is below the budget (T-662, §10.11 item 2
    /// supplement 2: the count is kept after the fact, so a shape may take the frame past the
    /// budget and only the shapes after it are held back; the frame's first shape is therefore
    /// never held back, which T-672 requires of a shape larger than the budget).
    bool budgetAllows() const;
    /// Adds `pixels` (one shape's covered pixels, CoverageBitmap::coveredPixels) to this frame's
    /// rasterized count.
    void noteRasterized(uint64_t pixels);

    /// Finds `key`; on a hit fills `out` and marks its page used this frame.
    bool lookup(const CoverageKey& key, CoverageEntry& out);
    /// Packs `bitmap` under `key` (copying its pixels) and fills `out`; the page is marked used
    /// this frame. Returns false when the bitmap is empty or larger than a page, or when it fits
    /// nowhere even after recycling (every candidate page was used this frame). A key that is
    /// already cached returns its existing entry without packing again.
    bool insert(const CoverageKey& key, const CoverageBitmap& bitmap, CoverageEntry& out);

    /// T-672: the tiles of the tiled shape `shapeKey` whose header `header` (kind Tiled) lookup()
    /// returned, one entry per tile in row-major order, each tile's page marked used this frame.
    /// Returns false when any tile is missing (its page was recycled): the caller rasterizes the
    /// shape again and calls insertTiles(). Does not count hits or misses.
    bool lookupTiles(const CoverageKey& shapeKey, const CoverageEntry& header,
                     std::vector<CoverageEntry>& tiles);
    /// T-672: stores `bitmap` (a whole shape's) as tiles under `shapeKey` (a whole-shape key):
    /// tiles that are neither all 0 nor all 255 are packed like insert() (a tile already cached is
    /// not packed again); uniform tiles and the header take no page space and are recorded on the
    /// page of the shape's first packed tile (page 0, created if need be, when it has none).
    /// Fills `header` (kind Tiled) and `tiles` as lookupTiles() does. Returns false when the grid
    /// has more than kMaxCoverageTiles tiles or a tile fits nowhere; the header is written last,
    /// so a failed call leaves the shape uncached.
    bool insertTiles(const CoverageKey& shapeKey, const CoverageBitmap& bitmap, CoverageEntry& header,
                     std::vector<CoverageEntry>& tiles);

    /// Calls fn(page, x, y, w, h, r8Pixels, stride) once per page with changes since the last
    /// call, the rect covering (at least) every bitmap packed since then, margins included;
    /// clears the pending state afterward.
    template <class F>
    void flushUploads(F&& fn) {
        for (size_t i = 0; i < m_pages.size(); ++i) {
            Page& page = m_pages[i];
            if (!page.dirty) continue;
            fn(static_cast<uint16_t>(i), page.dirtyX, page.dirtyY, page.dirtyW, page.dirtyH,
               page.pixels.data(), page.size);
            page.dirty = false;
        }
    }

    /// True when some page has changes flushUploads() has not delivered yet.
    bool hasPendingUploads() const;
    /// True if a page grew (changing the uv scale of entries already handed out) since the last
    /// call; clears the flag on read (GlyphAtlas::wasReset's role for T-648 growth).
    bool wasResized();

    /// Side length of page `page` in pixels (0 for a page that does not exist).
    int pageSize(uint32_t page) const;
    /// Number of pages allocated.
    uint32_t pageCount() const { return static_cast<uint32_t>(m_pages.size()); }
    /// Bytes held by page pixels.
    uint64_t bytes() const;
    /// Page `page`'s pixels (size * size, row-major; empty for a page that does not exist).
    std::span<const uint8_t> pagePixels(uint32_t page) const;
    /// Counters; see CoverageAtlasStats.
    CoverageAtlasStats stats() const;

private:
    struct Shelf {
        int y = 0;
        int height = 0;
        int cursorX = 0;
    };
    struct Page {
        std::vector<uint8_t> pixels;  // size * size, row-major
        std::vector<Shelf> shelves;
        int size = 0;
        int nextShelfY = 0;
        uint64_t lastUsedFrame = 0;
        bool dirty = false;
        int dirtyX = 0, dirtyY = 0, dirtyW = 0, dirtyH = 0;
    };
    struct Slot {
        CoverageKey key;
        CoverageEntryKind kind = CoverageEntryKind::Bitmap;
        uint16_t page = 0;
        uint16_t x = 0;  // bitmap's top-left within the page (margin excluded)
        uint16_t y = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        int32_t offsetX = 0;
        int32_t offsetY = 0;
    };

    static void markDirty(Page& page, int x, int y, int w, int h);
    static void writeReserved(Page& page);
    static bool place(Page& page, int w, int h, int& x, int& y);
    static void growPage(Page& page, int newSize);
    void addPage(int size);
    bool tryPlace(int w, int h, uint16_t& pageIndex, int& x, int& y);
    void recyclePage(uint16_t pageIndex);
    CoverageEntry entryOf(const Slot& slot) const;
    size_t findSlot(const CoverageKey& key) const;
    bool insertBox(const CoverageKey& key, const uint8_t* pixels, size_t stride, int w, int h,
                   int32_t offsetX, int32_t offsetY, CoverageEntry& out);
    void insertMarker(const CoverageKey& key, CoverageEntryKind kind, uint16_t page, int w, int h,
                      int32_t offsetX, int32_t offsetY, CoverageEntry& out);

    int m_pageSize;
    int m_initialSize;
    int m_maxPages;
    std::vector<Page> m_pages;
    std::vector<Slot> m_slots;  // sorted by key (binary search; no hashing container per frame)
    uint64_t m_frame = 1;
    uint64_t m_budget = kDefaultFrameBudget;
    uint64_t m_frameRasterized = 0;
    uint64_t m_hits = 0;
    uint64_t m_misses = 0;
    uint64_t m_recycles = 0;
    bool m_resized = false;
};

}  // namespace xt::gpu
