#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

#include "doctest.h"
#include "xt/coverage_bitmap.h"
#include "xt/gpu/coverage_atlas.h"
#include "xt/node.h"
#include "xt/types.h"

/// CoverageAtlas: packing, key quantization, page recycling, frame budget and uploads.
/// See Docs/design/10-backends.md §10.11 item 2, Docs/tasks/T-660a.md item 4.

using namespace xt;
using namespace xt::gpu;

namespace {

/// A w x h bitmap filled with `value`, origin (ox, oy).
CoverageBitmap makeBitmap(int w, int h, uint8_t value, int ox = 0, int oy = 0) {
    CoverageBitmap bm;
    bm.width = w;
    bm.height = h;
    bm.originX = ox;
    bm.originY = oy;
    bm.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), value);
    return bm;
}

CoverageKey keyOf(uint64_t hash, FillRule rule, const Mat2x3& xf) {
    CoverageKey key;
    REQUIRE(makeCoverageKey(hash, rule, xf, key));
    return key;
}

CoverageKey keyFor(uint64_t hash) { return keyOf(hash, FillRule::NonZero, Mat2x3{}); }

}  // namespace

TEST_CASE("CoverageAtlas: insert then lookup hits with the same entry and pixels") {
    CoverageAtlas atlas;
    atlas.beginFrame();
    CoverageEntry e;
    CHECK_FALSE(atlas.lookup(keyFor(1), e));
    const CoverageBitmap bm = makeBitmap(10, 6, 200, -3, 7);
    CoverageEntry ins;
    REQUIRE(atlas.insert(keyFor(1), bm, ins));
    CHECK(ins.width == 10);
    CHECK(ins.height == 6);
    CHECK(ins.offsetX == -3);
    CHECK(ins.offsetY == 7);
    CHECK(atlas.pageCount() == 1);
    CHECK(atlas.pageSize(0) == 512);
    CHECK(atlas.bytes() == 512u * 512u);

    CoverageEntry hit;
    REQUIRE(atlas.lookup(keyFor(1), hit));
    CHECK(hit.page == ins.page);
    CHECK(hit.u0 == ins.u0);
    CHECK(hit.v1 == ins.v1);
    CHECK((hit.u1 - hit.u0) * 512.0f == doctest::Approx(10.0f));
    CHECK((hit.v1 - hit.v0) * 512.0f == doctest::Approx(6.0f));
    CHECK(hit.u0 * 512.0f >= 1.0f);  // inside its 1 px margin
    CHECK(hit.v0 * 512.0f >= 1.0f);

    const CoverageAtlasStats s = atlas.stats();
    CHECK(s.pages == 1);
    CHECK(s.entries == 1);
    CHECK(s.hits == 1);
    CHECK(s.misses == 1);
    CHECK(s.recycles == 0);
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: key quantization of translation; the linear part matches bit for bit") {
    const Mat2x3 base{1.0f, 0.0f, 0.0f, 1.0f, 10.25f, 20.25f};
    const CoverageKey k0 = keyOf(7, FillRule::NonZero, base);
    CHECK(k0.subX == 1);
    CHECK(k0.subY == 1);
    CHECK(k0.m[0] == 0x3F800000u);
    CHECK(k0.m[1] == 0u);
    CHECK(k0.m[3] == 0x3F800000u);

    CoverageAtlas atlas;
    atlas.beginFrame();
    CoverageEntry e;
    REQUIRE(atlas.insert(k0, makeBitmap(4, 4, 255), e));

    // Translation differing by < 1/8 px (fraction and integer part) hits the same entry.
    Mat2x3 near = base;
    near.e += 0.1f;
    near.f -= 0.1f;
    CHECK(atlas.lookup(keyOf(7, FillRule::NonZero, near), e));
    Mat2x3 shifted = base;
    shifted.e += 3.0f;  // whole-pixel moves are not in the key
    shifted.f -= 5.0f;
    CHECK(atlas.lookup(keyOf(7, FillRule::NonZero, shifted), e));

    // 1/4 px apart: a different entry.
    Mat2x3 quarter = base;
    quarter.e += 0.25f;
    CHECK_FALSE(atlas.lookup(keyOf(7, FillRule::NonZero, quarter), e));
    Mat2x3 quarterY = base;
    quarterY.f -= 0.25f;
    CHECK_FALSE(atlas.lookup(keyOf(7, FillRule::NonZero, quarterY), e));

    // T-660b: any change of the linear part misses, even one ulp; -0 and +0 are the same key.
    Mat2x3 ulp = base;
    ulp.a = 1.0000001f;
    CHECK_FALSE(atlas.lookup(keyOf(7, FillRule::NonZero, ulp), e));
    Mat2x3 lin = base;
    lin.b += 1.0f / 160.0f;
    CHECK_FALSE(atlas.lookup(keyOf(7, FillRule::NonZero, lin), e));
    Mat2x3 negZero = base;
    negZero.b = -0.0f;
    negZero.c = -0.0f;
    CHECK(atlas.lookup(keyOf(7, FillRule::NonZero, negZero), e));

    // Fill rule and path hash are part of the key.
    CHECK_FALSE(atlas.lookup(keyOf(7, FillRule::EvenOdd, base), e));
    CHECK_FALSE(atlas.lookup(keyOf(8, FillRule::NonZero, base), e));
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: a quarter phase that rounds up carries into the whole pixel") {
    int32_t whole = 0;
    uint8_t sub = 9;
    REQUIRE(splitCoverageTranslation(10.9f, whole, sub));
    CHECK(whole == 11);
    CHECK(sub == 0);
    REQUIRE(splitCoverageTranslation(10.6f, whole, sub));
    CHECK(whole == 10);
    CHECK(sub == 2);
    REQUIRE(splitCoverageTranslation(-0.05f, whole, sub));
    CHECK(whole == 0);
    CHECK(sub == 0);
    REQUIRE(splitCoverageTranslation(-0.3f, whole, sub));
    CHECK(whole == -1);
    CHECK(sub == 3);
    // 10.9 and 11.0 share the key (phase 0).
    CoverageKey a, b;
    REQUIRE(makeCoverageKey(1, FillRule::NonZero, Mat2x3{1, 0, 0, 1, 10.9f, 0}, a));
    REQUIRE(makeCoverageKey(1, FillRule::NonZero, Mat2x3{1, 0, 0, 1, 11.0f, 0}, b));
    CHECK(a.subX == b.subX);
}

TEST_CASE("CoverageAtlas: a NaN or infinite transform is not cacheable") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CoverageKey key;
    CHECK_FALSE(makeCoverageKey(1, FillRule::NonZero, Mat2x3{nan, 0, 0, 1, 0, 0}, key));
    CHECK_FALSE(makeCoverageKey(1, FillRule::NonZero, Mat2x3{1, 0, 0, inf, 0, 0}, key));
    CHECK_FALSE(makeCoverageKey(1, FillRule::NonZero, Mat2x3{1, 0, 0, 1, nan, 0}, key));
    CHECK_FALSE(makeCoverageKey(1, FillRule::NonZero, Mat2x3{1, 0, 0, 1, 0, 3e9f}, key));
    int32_t whole = 0;
    uint8_t sub = 0;
    CHECK_FALSE(splitCoverageTranslation(nan, whole, sub));
}

TEST_CASE("CoverageAtlas: a full atlas recycles the least recently used page, never one used this frame") {
    // 64 px pages, no growth, 2 pages: a 56x56 bitmap (58x58 box) fills a page (T-672: the 4x4
    // reserved corner keeps a box from reaching both the last 4 rows and the last 4 columns).
    CoverageAtlas atlas(64, 64, 2);
    CoverageEntry e;

    atlas.beginFrame();  // frame A: page 0
    REQUIRE(atlas.insert(keyFor(1), makeBitmap(56, 56, 1), e));
    CHECK(e.page == 0);
    atlas.endFrame();

    atlas.beginFrame();  // frame B: page 1
    REQUIRE(atlas.insert(keyFor(2), makeBitmap(56, 56, 2), e));
    CHECK(e.page == 1);
    atlas.endFrame();

    atlas.beginFrame();  // frame C: page 0 is the oldest -> recycled
    REQUIRE(atlas.insert(keyFor(3), makeBitmap(56, 56, 3), e));
    CHECK(e.page == 0);
    CHECK(atlas.stats().recycles == 1);
    CHECK_FALSE(atlas.lookup(keyFor(1), e));  // key 1 lived on the recycled page
    CHECK(atlas.lookup(keyFor(2), e));        // marks page 1 used this frame
    // Both pages are now used this frame: nothing can be recycled.
    CHECK_FALSE(atlas.insert(keyFor(4), makeBitmap(56, 56, 4), e));
    CHECK(atlas.stats().recycles == 1);
    CHECK(atlas.lookup(keyFor(2), e));
    CHECK(atlas.lookup(keyFor(3), e));
    atlas.endFrame();

    atlas.beginFrame();  // frame D: page 0 used now, so page 1 (last used in C) is the candidate
    CHECK(atlas.lookup(keyFor(3), e));
    REQUIRE(atlas.insert(keyFor(4), makeBitmap(56, 56, 4), e));
    CHECK(e.page == 1);
    CHECK_FALSE(atlas.lookup(keyFor(2), e));
    CHECK(atlas.stats().recycles == 2);
    atlas.endFrame();

    // Larger than a page: refused outright.
    atlas.beginFrame();
    CHECK_FALSE(atlas.insert(keyFor(9), makeBitmap(63, 10, 1), e));
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: the first page starts small and grows to the full page size") {
    CoverageAtlas atlas(128, 64, 4);
    CoverageEntry e;
    atlas.beginFrame();
    REQUIRE(atlas.insert(keyFor(1), makeBitmap(40, 40, 1), e));
    CHECK(atlas.pageSize(0) == 64);
    CHECK_FALSE(atlas.wasResized());
    REQUIRE(atlas.insert(keyFor(2), makeBitmap(40, 40, 2), e));  // does not fit 64: grows
    CHECK(atlas.pageCount() == 1);
    CHECK(atlas.pageSize(0) == 128);
    CHECK(atlas.wasResized());
    CHECK_FALSE(atlas.wasResized());
    CoverageEntry first;
    REQUIRE(atlas.lookup(keyFor(1), first));
    CHECK(first.u0 * 128.0f == doctest::Approx(1.0f));  // uv rescaled to the grown page
    // A bitmap that fits nowhere on page 0 adds a full-size page.
    REQUIRE(atlas.insert(keyFor(3), makeBitmap(120, 120, 3), e));
    CHECK(e.page == 1);
    CHECK(atlas.pageSize(1) == 128);
    CHECK(atlas.bytes() == 2u * 128u * 128u);
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: per-frame rasterization budget, accounted after each shape (T-662)") {
    CoverageAtlas atlas;
    CHECK(atlas.frameBudget() == 2000000u);
    atlas.setFrameBudget(1000);
    atlas.beginFrame();
    CHECK(atlas.budgetAllows());
    atlas.noteRasterized(600);
    CHECK(atlas.budgetAllows());  // 600 used < 1000
    atlas.noteRasterized(399);
    CHECK(atlas.budgetAllows());  // 999 used
    atlas.noteRasterized(1);
    CHECK_FALSE(atlas.budgetAllows());  // 1000 used: the budget is reached
    CHECK(atlas.stats().frameRasterizedPixels == 1000u);
    atlas.endFrame();
    atlas.beginFrame();  // a new frame restores the budget
    CHECK(atlas.budgetAllows());
    CHECK(atlas.stats().frameRasterizedPixels == 0u);
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: flushUploads delivers a rect covering every new cell with its margins") {
    CoverageAtlas atlas;
    atlas.beginFrame();
    CoverageEntry a, b;
    REQUIRE(atlas.insert(keyFor(1), makeBitmap(10, 8, 77), a));
    REQUIRE(atlas.insert(keyFor(2), makeBitmap(20, 30, 99), b));
    CHECK(atlas.hasPendingUploads());

    int calls = 0;
    atlas.flushUploads([&](uint16_t page, int x, int y, int w, int h, const uint8_t* pixels, int stride) {
        ++calls;
        CHECK(page == 0);
        CHECK(stride == 512);
        for (const CoverageEntry* e : {&a, &b}) {
            const int ex = static_cast<int>(e->u0 * 512.0f + 0.5f);
            const int ey = static_cast<int>(e->v0 * 512.0f + 0.5f);
            CHECK(x <= ex - 1);
            CHECK(y <= ey - 1);
            CHECK(x + w >= ex + e->width + 1);
            CHECK(y + h >= ey + e->height + 1);
            // The cell holds the bitmap and a zero margin.
            const uint8_t expected = e == &a ? 77 : 99;
            CHECK(pixels[static_cast<size_t>(ey) * 512 + static_cast<size_t>(ex)] == expected);
            CHECK(pixels[static_cast<size_t>(ey - 1) * 512 + static_cast<size_t>(ex)] == 0);
            CHECK(pixels[static_cast<size_t>(ey) * 512 + static_cast<size_t>(ex - 1)] == 0);
            CHECK(pixels[static_cast<size_t>(ey) * 512 + static_cast<size_t>(ex + e->width)] == 0);
        }
    });
    CHECK(calls == 1);
    CHECK_FALSE(atlas.hasPendingUploads());
    int again = 0;
    atlas.flushUploads([&](uint16_t, int, int, int, int, const uint8_t*, int) { ++again; });
    CHECK(again == 0);
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: inserting a rasterizeCoverage bitmap keeps its origin as the offset") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    std::vector<float> points{2.5f, 3.25f, 20.75f, 3.25f, 11.0f, 18.5f};
    const Mat2x3 xf{1.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.0f};
    CoverageBitmap bm;
    REQUIRE(rasterizeCoverage(verbs, points, xf, FillRule::NonZero, bm));
    CoverageAtlas atlas;
    atlas.beginFrame();
    CoverageEntry e;
    REQUIRE(atlas.insert(keyOf(42, FillRule::NonZero, xf), bm, e));
    CHECK(e.offsetX == bm.originX);
    CHECK(e.offsetY == bm.originY);
    CHECK(e.width == bm.width);
    CHECK(e.height == bm.height);
    atlas.endFrame();
}

// ---------------------------------------------------------------------------------------------
// T-672: large shapes are tiled (Docs/design/10-backends.md §10.11 item 2, tiling supplement).
// ---------------------------------------------------------------------------------------------

namespace {

/// A w x h bitmap whose every value is strictly between 0 and 255 (every tile is an edge tile).
CoverageBitmap makeEdgeBitmap(int w, int h, int ox = 0, int oy = 0) {
    CoverageBitmap bm = makeBitmap(w, h, 0, ox, oy);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            bm.pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(1 + (x + 3 * y) % 253);
        }
    }
    return bm;
}

/// True when the reserved corner of `page` holds 255 everywhere.
bool reservedIsFull(const CoverageAtlas& atlas, uint32_t page) {
    const int size = atlas.pageSize(page);
    const std::span<const uint8_t> px = atlas.pagePixels(page);
    if (size < kCoverageReservedSide || px.size() != static_cast<size_t>(size) * static_cast<size_t>(size)) {
        return false;
    }
    for (int y = size - kCoverageReservedSide; y < size; ++y) {
        for (int x = size - kCoverageReservedSide; x < size; ++x) {
            if (px[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] != 255) {
                return false;
            }
        }
    }
    return true;
}

/// Checks that the texels of `e` (kind Bitmap) equal the tile of `bm` at (x0, y0).
void checkTileTexels(const CoverageAtlas& atlas, const CoverageEntry& e, const CoverageBitmap& bm, int x0,
                     int y0) {
    REQUIRE(e.kind == CoverageEntryKind::Bitmap);
    const int size = atlas.pageSize(e.page);
    const std::span<const uint8_t> px = atlas.pagePixels(e.page);
    const int tx = static_cast<int>(e.u0 * static_cast<float>(size) + 0.5f);
    const int ty = static_cast<int>(e.v0 * static_cast<float>(size) + 0.5f);
    size_t mismatches = 0;
    for (int y = 0; y < e.height; ++y) {
        for (int x = 0; x < e.width; ++x) {
            const uint8_t got =
                px[static_cast<size_t>(ty + y) * static_cast<size_t>(size) + static_cast<size_t>(tx + x)];
            const uint8_t want = bm.pixels[static_cast<size_t>(y0 + y) * static_cast<size_t>(bm.width) +
                                           static_cast<size_t>(x0 + x)];
            mismatches += got != want ? 1 : 0;
        }
    }
    CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("CoverageAtlas: tile grid of 256 px tiles, at most 1024 tiles") {
    int cols = 0, rows = 0;
    REQUIRE(coverageTileGrid(513, 7, cols, rows));
    CHECK(cols == 3);
    CHECK(rows == 1);
    REQUIRE(coverageTileGrid(4096, 4096, cols, rows));  // the largest bitmap: 16 x 16
    CHECK(cols == 16);
    CHECK(rows == 16);
    REQUIRE(coverageTileGrid(256 * 1024, 1, cols, rows));
    CHECK(cols == 1024);
    CHECK_FALSE(coverageTileGrid(256 * 1024 + 1, 1, cols, rows));  // 1025 tiles
    CHECK_FALSE(coverageTileGrid(256 * 33, 256 * 32, cols, rows));
    CHECK_FALSE(coverageTileGrid(0, 10, cols, rows));

    // insertTiles refuses a grid of more than kMaxCoverageTiles tiles.
    CoverageAtlas atlas;
    atlas.beginFrame();
    CoverageEntry header;
    std::vector<CoverageEntry> tiles;
    CHECK_FALSE(atlas.insertTiles(keyFor(5), makeBitmap(256 * 1024 + 1, 1, 128), header, tiles));
    CHECK_FALSE(atlas.lookup(keyFor(5), header));
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: tile keys -- the whole shape and tile (0, 0) are different keys") {
    CoverageKey whole = keyFor(11);
    CHECK(whole.tile == 0);
    CHECK(whole.tileX == 0);
    CHECK(whole.tileY == 0);
    CoverageKey tile00 = whole;
    tile00.tile = 1;
    CoverageKey tile10 = tile00;
    tile10.tileX = 1;

    CoverageAtlas atlas;
    atlas.beginFrame();
    CoverageEntry e;
    REQUIRE(atlas.insert(tile00, makeBitmap(8, 8, 9), e));
    CHECK_FALSE(atlas.lookup(whole, e));
    CHECK_FALSE(atlas.lookup(tile10, e));
    REQUIRE(atlas.lookup(tile00, e));
    CHECK(e.kind == CoverageEntryKind::Bitmap);
    REQUIRE(atlas.insert(whole, makeBitmap(8, 8, 3), e));
    CHECK(atlas.stats().entries == 2);
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: a tiled shape -- empty and full tiles take no packing space") {
    // 600 x 300: columns 0..255 empty, 256..511 an edge band (value 100 in x < 300), 512..599 full.
    CoverageBitmap bm = makeBitmap(600, 300, 0, -1, 4);
    for (int y = 0; y < 300; ++y) {
        for (int x = 256; x < 600; ++x) {
            bm.pixels[static_cast<size_t>(y) * 600 + static_cast<size_t>(x)] = x < 300 ? 100 : 255;
        }
    }
    CoverageAtlas atlas;
    atlas.beginFrame();
    CoverageEntry header;
    std::vector<CoverageEntry> tiles;
    REQUIRE(atlas.insertTiles(keyFor(21), bm, header, tiles));
    CHECK(header.kind == CoverageEntryKind::Tiled);
    CHECK(header.width == 600);
    CHECK(header.height == 300);
    CHECK(header.offsetX == -1);
    CHECK(header.offsetY == 4);
    REQUIRE(tiles.size() == 6);  // 3 x 2
    const CoverageEntryKind want[6] = {CoverageEntryKind::Empty,  CoverageEntryKind::Bitmap,
                                       CoverageEntryKind::Full,   CoverageEntryKind::Empty,
                                       CoverageEntryKind::Bitmap, CoverageEntryKind::Full};
    for (size_t i = 0; i < 6; ++i) {
        INFO("tile ", i);
        CHECK(tiles[i].kind == want[i]);
        CHECK(tiles[i].offsetX == -1 + static_cast<int>(i % 3) * 256);
        CHECK(tiles[i].offsetY == 4 + static_cast<int>(i / 3) * 256);
        CHECK(tiles[i].width == (i % 3 == 2 ? 88 : 256));
        CHECK(tiles[i].height == (i < 3 ? 256 : 44));
    }
    checkTileTexels(atlas, tiles[1], bm, 256, 0);
    checkTileTexels(atlas, tiles[4], bm, 256, 256);
    // A full tile samples the centre of its page's reserved block.
    CHECK(tiles[2].u0 == tiles[2].u1);
    CHECK(tiles[2].v0 == tiles[2].v1);
    CHECK(tiles[2].u0 * static_cast<float>(atlas.pageSize(tiles[2].page)) ==
          doctest::Approx(static_cast<float>(atlas.pageSize(tiles[2].page) - 2)));
    CHECK(reservedIsFull(atlas, tiles[2].page));

    // An all-255 shape packs nothing.
    std::vector<CoverageEntry> fullTiles;
    REQUIRE(atlas.insertTiles(keyFor(22), makeBitmap(700, 700, 255), header, fullTiles));
    REQUIRE(fullTiles.size() == 9);
    for (const CoverageEntry& t : fullTiles) {
        CHECK(t.kind == CoverageEntryKind::Full);
    }
    // On a fresh atlas, after an all-255 and an all-0 shape the next bitmap still lands at the
    // page's top-left.
    CoverageAtlas fresh;
    fresh.beginFrame();
    REQUIRE(fresh.insertTiles(keyFor(23), makeBitmap(700, 700, 255), header, fullTiles));
    REQUIRE(fresh.insertTiles(keyFor(24), makeBitmap(900, 600, 0), header, fullTiles));
    CoverageEntry small;
    REQUIRE(fresh.insert(keyFor(25), makeBitmap(10, 10, 7), small));
    CHECK(small.u0 * 512.0f == doctest::Approx(1.0f));
    CHECK(small.v0 * 512.0f == doctest::Approx(1.0f));
    CHECK(fresh.pageCount() == 1);
    fresh.endFrame();

    // The header and every tile hit on lookup.
    CoverageEntry hit;
    REQUIRE(atlas.lookup(keyFor(21), hit));
    CHECK(hit.kind == CoverageEntryKind::Tiled);
    std::vector<CoverageEntry> again;
    REQUIRE(atlas.lookupTiles(keyFor(21), hit, again));
    REQUIRE(again.size() == tiles.size());
    for (size_t i = 0; i < tiles.size(); ++i) {
        CHECK(again[i].kind == tiles[i].kind);
        CHECK(again[i].u0 == tiles[i].u0);
        CHECK(again[i].v0 == tiles[i].v0);
    }
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: the reserved block is 255 on creation, growth and after a recycle") {
    CoverageAtlas atlas(64, 32, 1);
    CoverageEntry e;
    atlas.beginFrame();
    REQUIRE(atlas.insert(keyFor(1), makeBitmap(8, 8, 0), e));
    CHECK(atlas.pageSize(0) == 32);
    CHECK(reservedIsFull(atlas, 0));
    REQUIRE(atlas.insert(keyFor(2), makeBitmap(40, 40, 0), e));  // grows to 64
    CHECK(atlas.pageSize(0) == 64);
    CHECK(reservedIsFull(atlas, 0));
    atlas.endFrame();

    atlas.beginFrame();  // the only page is recycled for a box that needs it whole
    REQUIRE(atlas.insert(keyFor(3), makeBitmap(56, 56, 0), e));
    CHECK(atlas.stats().recycles == 1);
    CHECK(reservedIsFull(atlas, 0));
    atlas.endFrame();

    // The reserved block reaches the device with the upload.
    bool covered = false;
    atlas.flushUploads([&](uint16_t, int x, int y, int w, int h, const uint8_t* px, int stride) {
        covered = x <= 60 && y <= 60 && x + w >= 64 && y + h >= 64 && px[63 * stride + 63] == 255;
    });
    CHECK(covered);

    // A box that would reach both the last 4 rows and the last 4 columns does not fit.
    CoverageAtlas tight(64, 64, 1);
    tight.beginFrame();
    CHECK_FALSE(tight.insert(keyFor(4), makeBitmap(60, 60, 0), e));
    REQUIRE(tight.insert(keyFor(5), makeBitmap(62, 56, 0), e));  // the last 4 rows stay free
    CHECK(reservedIsFull(tight, 0));
    tight.endFrame();
}

TEST_CASE("CoverageAtlas: a tile lost to a recycled page is put back; cached tiles are not packed again") {
    // 1024 px pages, 2 of them. Frame A: a filler takes the top 702 rows of page 0, so tiles 0..2
    // of a 1024 x 256 all-edge shape go on page 0 and tile 3 on page 1.
    CoverageAtlas atlas(1024, 1024, 2);
    const CoverageBitmap shape = makeEdgeBitmap(1024, 256, 5, 6);
    CoverageEntry e, header;
    std::vector<CoverageEntry> tiles;
    atlas.beginFrame();
    REQUIRE(atlas.insert(keyFor(1), makeBitmap(1000, 700, 1), e));
    REQUIRE(atlas.insertTiles(keyFor(2), shape, header, tiles));
    REQUIRE(tiles.size() == 4);
    CHECK(tiles[0].page == 0);
    CHECK(tiles[2].page == 0);
    CHECK(tiles[3].page == 1);
    CHECK(header.page == 0);  // the page of the first packed tile
    atlas.endFrame();
    const std::vector<CoverageEntry> before = tiles;

    // Frame B: only page 0 is used; a 1000 x 1000 bitmap recycles page 1 (tile 3 is dropped).
    atlas.beginFrame();
    REQUIRE(atlas.lookup(keyFor(1), e));
    REQUIRE(atlas.insert(keyFor(3), makeBitmap(1000, 1000, 2), e));
    CHECK(e.page == 1);
    CHECK(atlas.stats().recycles == 1);
    atlas.endFrame();

    // Frame C: the header hits, a tile is missing; insertTiles puts it back (recycling page 1
    // again) and leaves tiles 0..2 where they were.
    atlas.beginFrame();
    REQUIRE(atlas.lookup(keyFor(2), header));
    CHECK(header.kind == CoverageEntryKind::Tiled);
    CHECK_FALSE(atlas.lookupTiles(keyFor(2), header, tiles));
    const uint32_t entriesBefore = atlas.stats().entries;
    REQUIRE(atlas.insertTiles(keyFor(2), shape, header, tiles));
    CHECK(atlas.stats().recycles == 2);
    CHECK(atlas.stats().entries == entriesBefore);  // tile 3 back, the 1000 x 1000 bitmap gone
    for (size_t i = 0; i < 3; ++i) {
        CHECK(tiles[i].page == before[i].page);
        CHECK(tiles[i].u0 == before[i].u0);
        CHECK(tiles[i].v0 == before[i].v0);
    }
    for (size_t i = 0; i < 4; ++i) {
        checkTileTexels(atlas, tiles[i], shape, static_cast<int>(i) * 256, 0);
    }
    std::vector<CoverageEntry> again;
    CHECK(atlas.lookupTiles(keyFor(2), header, again));
    atlas.endFrame();
}

TEST_CASE("CoverageAtlas: the first shape of a frame is never held back by the budget") {
    CoverageAtlas atlas;
    atlas.setFrameBudget(1000);
    atlas.beginFrame();
    CHECK(atlas.budgetAllows());  // nothing rasterized yet this frame
    atlas.noteRasterized(5000);   // one shape larger than the whole budget
    CHECK_FALSE(atlas.budgetAllows());
    CHECK(atlas.stats().frameRasterizedPixels == 5000u);
    atlas.endFrame();
    atlas.beginFrame();
    CHECK(atlas.budgetAllows());
    atlas.endFrame();
}
