#include <string>
#include <vector>

#include "doctest.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text/glyph_atlas.h"

/// Tests for GlyphAtlas (Docs/design/07-text.md §7.4, Docs/tasks/T-29.md "測試").

namespace {

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

struct Fixture {
    xt::text::FtTextShaper shaper;
    xt::FontId font;
    Fixture() {
        REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
        const std::vector<std::string_view> families = {"Noto Sans"};
        font = shaper.match(xt::FontRequest{std::span<const std::string_view>(families), 400, false});
        REQUIRE(font != xt::kInvalidFont);
    }
};

// A-Z at some glyph offset within the subset; using match+shape to get real glyph ids.
uint32_t glyphIdFor(xt::text::FtTextShaper& shaper, xt::FontId font, char c) {
    const std::vector<std::string_view> families = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = xt::FontRequest{std::span<const std::string_view>(families), 400, false};
    params.size = 16;
    xt::ShapeResult out;
    const std::string s(1, c);
    shaper.shape(s, params, out);
    REQUIRE(!out.glyphs.empty());
    (void)font;
    return out.glyphs[0].glyphId;
}

}  // namespace

TEST_CASE("GlyphAtlas packs many small glyphs into one texture") {
    Fixture f;
    xt::text::GlyphAtlas atlas(f.shaper);

    int placed = 0;
    for (int rep = 0; rep < 8; ++rep) {
        for (char c = 'A'; c <= 'Z'; ++c) {
            const uint32_t glyphId = glyphIdFor(f.shaper, f.font, c);
            const float size = 10.0f + static_cast<float>(rep);  // distinct cache key per rep
            xt::text::AtlasEntry entry;
            if (atlas.lookup(f.font, glyphId, size, entry)) ++placed;
        }
    }
    CHECK(placed >= 200);
    CHECK(atlas.textureCount() == 1);
}

TEST_CASE("GlyphAtlas overflows to a new texture or resets when full") {
    Fixture f;
    xt::text::GlyphAtlas atlas(f.shaper);

    bool sawMultipleTextures = false;
    bool sawReset = false;
    for (int i = 0; i < 40 && !(sawMultipleTextures && sawReset); ++i) {
        const char c = static_cast<char>('A' + (i % 26));
        const uint32_t glyphId = glyphIdFor(f.shaper, f.font, c);
        // Large, slightly different size each iteration -> a distinct, big cache entry each time.
        const float size = 900.0f + static_cast<float>(i);
        xt::text::AtlasEntry entry;
        atlas.lookup(f.font, glyphId, size, entry);
        if (atlas.textureCount() > 1) sawMultipleTextures = true;
        if (atlas.wasReset()) sawReset = true;
    }
    CHECK((sawMultipleTextures || sawReset));
}

TEST_CASE("GlyphAtlas::flushUploads reports a region covering the new glyph") {
    Fixture f;
    xt::text::GlyphAtlas atlas(f.shaper);

    const uint32_t glyphId = glyphIdFor(f.shaper, f.font, 'A');
    xt::text::AtlasEntry entry;
    REQUIRE(atlas.lookup(f.font, glyphId, 24.0f, entry));

    bool covered = false;
    atlas.flushUploads([&](uint16_t texture, int x, int y, int w, int h, const uint8_t* pixels, int stride) {
        CHECK(pixels != nullptr);
        CHECK(stride == atlas.textureSize());
        if (texture == entry.texture && x <= entry.x && y <= entry.y && x + w >= entry.x + entry.w &&
            y + h >= entry.y + entry.h) {
            covered = true;
        }
    });
    CHECK(covered);

    // A second flush with no new glyphs must not invoke the callback again.
    int callCount = 0;
    atlas.flushUploads([&](uint16_t, int, int, int, int, const uint8_t*, int) { ++callCount; });
    CHECK(callCount == 0);
}
