#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/node.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text_layout.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// The small-size bitmap glyph path, 1/4 px horizontal subpixel positioning and coverage gamma
/// (docs/tasks/T-101.md, docs/design/23-host-integration.md §23.4), exercised through the real
/// FreeType/HarfBuzz shaper and the OFL font subsets in tests/fonts.

namespace xt {
// core/src/painter_text.cpp (the Painter's Text branch), declared the same way
// tests/text/test_sw_glyphs.cpp declares it.
void emitTextGlyphs(const TextNodeData& text, const ComputedStyle& style, const Mat2x3& world, float alpha,
                    CommandList& out);
}  // namespace xt

using namespace xt;

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 64;

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

struct Fonts {
    text::FtTextShaper shaper;
    Fonts() { REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf"))); }
};

/// One rendered line of text plus the counters the SW backend recorded for it.
struct Rendered {
    std::vector<uint8_t> rgba;
    uint32_t bitmaps = 0;
    uint32_t outlines = 0;
    float textMs = 0;

    uint8_t red(int x, int y) const {
        return rgba[(static_cast<size_t>(y) * static_cast<size_t>(kWidth) + static_cast<size_t>(x)) * 4];
    }
};

/// Shapes + lays out + paints + rasterizes `utf8` at `originX` on a white background.
Rendered renderLine(text::FtTextShaper& shaper, const std::string& utf8, float fontSize,
                    TextRendering rendering, float originX, uint32_t frames = 1) {
    std::vector<std::string_view> families = {"Noto Sans"};
    ShapeParams params;
    params.font.families = std::span<const std::string_view>(families);
    params.size = fontSize;
    params.rendering = rendering;

    ShapeResult shaped;
    shaper.shape(utf8, params, shaped);
    REQUIRE(!shaped.runs.empty());
    const FontMetrics metrics = shaper.metrics(shaped.runs[0].font);

    TextLayoutParams layoutParams;
    layoutParams.availMain = static_cast<float>(kWidth);
    layoutParams.fontSize = fontSize;
    layoutParams.lineHeight = fontSize * 1.2f;
    layoutParams.ascent = metrics.ascent * fontSize;
    layoutParams.descent = metrics.descent * fontSize;
    layoutParams.ellipsisFont = kInvalidFont;

    TextLayoutResult result;
    layoutText(shaped, layoutParams, result);

    TextNodeData node;
    node.runs = std::move(result.runs);
    node.lines = std::move(result.lines);

    ComputedStyle style{};
    initComputedStyleDefaults(style);
    style.base[prop::FontSize] = Value::ofLength(fontSize, LengthUnit::Px);
    style.base[prop::Color] = Value::ofColor(Color{0, 0, 0, 1});
    style.textRendering = rendering;

    CommandList commands;
    commands.viewport = Rect{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)};
    commands.devicePixelRatio = 1.0f;
    emitTextGlyphs(node, style, translation(Vec2{originX, 8.0f}), 1.0f, commands);

    Rendered out;
    out.rgba.assign(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{out.rgba.data(), kWidth, kHeight, kWidth * 4};
    renderer.setTarget(target);

    PerfTracker perf;
    perf.setEnabled(true);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        perf.beginFrame(static_cast<float>(frame));
        renderer.clear(Color{1, 1, 1, 1});
        renderer.render(commands, &shaper, nullptr, nullptr, &perf);
        perf.endFrame();
    }
    const PerfSummary summary = perf.summary(frames);
    out.bitmaps = summary.max.swGlyphBitmaps;
    out.outlines = summary.max.swGlyphOutlines;
    out.textMs = summary.avg.textMs;
    return out;
}

/// Mean red channel over the stem pixels -- the ones already more than half covered, i.e. the
/// middle of a stroke rather than its antialiased edge. §23.4's coverage gamma is what pushes
/// these down (darker) for dark text on a light background.
float stemMean(const Rendered& image) {
    double sum = 0;
    int count = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const uint8_t v = image.red(x, y);
            if (v < 128) {
                sum += v;
                ++count;
            }
        }
    }
    REQUIRE(count > 0);
    return static_cast<float>(sum / count);
}

}  // namespace

TEST_CASE("glyphBitmap: the four 1/4 px subpixel variants differ, within a pixel of each other") {
    Fonts fonts;
    std::vector<std::string_view> families = {"Noto Sans"};
    FontRequest request;
    request.families = std::span<const std::string_view>(families);
    const FontId font = fonts.shaper.match(request);
    REQUIRE(font != kInvalidFont);

    ShapeResult shaped;
    ShapeParams params;
    params.font = request;
    params.size = 16;
    fonts.shaper.shape("H", params, shaped);
    REQUIRE(!shaped.glyphs.empty());
    const uint32_t glyphId = shaped.glyphs[0].glyphId;

    GlyphBitmap variants[4];
    for (int i = 0; i < 4; ++i) {
        REQUIRE(fonts.shaper.glyphBitmap(font, glyphId, 16.0f, static_cast<float>(i) * 0.25f, variants[i]));
        CHECK(variants[i].originX == doctest::Approx(static_cast<float>(i) * 0.25f));
        CHECK(variants[i].width > 0);
        CHECK(variants[i].height > 0);
    }
    // Widths/heights stay within one pixel of each other (a subpixel shift can only spill a glyph
    // into one extra column).
    for (int i = 1; i < 4; ++i) {
        CHECK(std::abs(variants[i].width - variants[0].width) <= 1);
        CHECK(std::abs(variants[i].height - variants[0].height) <= 1);
    }
    // At least one pair actually differs: otherwise the subpixel offset never reached FreeType.
    bool anyDifferent = false;
    for (int i = 1; i < 4; ++i) {
        if (variants[i].width != variants[0].width || variants[i].height != variants[0].height ||
            variants[i].alpha != variants[0].alpha) {
            anyDifferent = true;
        }
    }
    CHECK(anyDifferent);

    // The three-argument overload is the subpixel-0 variant.
    GlyphBitmap plain;
    REQUIRE(fonts.shaper.glyphBitmap(font, glyphId, 16.0f, plain));
    CHECK(plain.alpha == variants[0].alpha);
}

TEST_CASE("SW DrawGlyphs: 16 px takes the bitmap path, 32 px the outline path") {
    Fonts fonts;
    const Rendered small = renderLine(fonts.shaper, "Handgloves", 16.0f, TextRendering::Auto, 8.0f);
    CHECK(small.bitmaps > 0);
    CHECK(small.outlines == 0);

    const Rendered large = renderLine(fonts.shaper, "Handgloves", 32.0f, TextRendering::Auto, 8.0f);
    CHECK(large.outlines > 0);
    CHECK(large.bitmaps == 0);
}

TEST_CASE("SW DrawGlyphs: text-rendering geometricPrecision keeps 16 px on the outline path") {
    Fonts fonts;
    const Rendered exact =
        renderLine(fonts.shaper, "Handgloves", 16.0f, TextRendering::GeometricPrecision, 8.0f);
    CHECK(exact.outlines > 0);
    CHECK(exact.bitmaps == 0);

    // No gamma either: the outline path's pixels are exactly what they were before T-101, which
    // the next test case pins down as the lighter of the two renderings.
    const Rendered autoRendering = renderLine(fonts.shaper, "Handgloves", 16.0f, TextRendering::Auto, 8.0f);
    CHECK(stemMean(autoRendering) < stemMean(exact));
}

TEST_CASE("SW DrawGlyphs: coverage gamma darkens 16 px dark-on-light text") {
    Fonts fonts;
    const Rendered image = renderLine(fonts.shaper, "Handgloves", 16.0f, TextRendering::Auto, 8.0f);
    // Recorded from the pre-T-101 renderer (the unhinted outline path, no gamma) on this exact
    // fixture: mean stem brightness 43.9/255. Black text on white must now come out darker.
    constexpr float kPreT101StemMean = 43.8f;
    const float mean = stemMean(image);
    CAPTURE(mean);
    CHECK(mean < kPreT101StemMean);
}

TEST_CASE("SW DrawGlyphs: 1/4 px subpixel positions render differently, whole pixels translate") {
    Fonts fonts;
    Rendered phases[4];
    for (int i = 0; i < 4; ++i) {
        phases[static_cast<size_t>(i)] = renderLine(fonts.shaper, "Handgloves", 16.0f, TextRendering::Auto,
                                                    10.0f + 0.25f * static_cast<float>(i));
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            CAPTURE(i);
            CAPTURE(j);
            CHECK(phases[static_cast<size_t>(i)].rgba != phases[static_cast<size_t>(j)].rgba);
        }
    }

    // x = 10 and x = 11 are the same phase, so the second is the first shifted one pixel right.
    const Rendered shifted = renderLine(fonts.shaper, "Handgloves", 16.0f, TextRendering::Auto, 11.0f);
    int compared = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth - 1; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            REQUIRE(shifted.red(x + 1, y) == phases[0].red(x, y));
            ++compared;
        }
    }
    CHECK(compared > 0);
}
