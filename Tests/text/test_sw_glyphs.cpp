#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text_layout.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// SW glyph rendering (Docs/tasks/T-30.md goal 4, Docs/design/10-backends.md §10.2.6) against the
/// real FreeType/HarfBuzz shaper and the OFL font subsets in Tests/fonts: shape -> layoutText ->
/// Painter's DrawGlyphs emitter -> SwRenderer -> pixel checks.

namespace xt {
// Code/Runtime/core/src/painter_text.cpp (the Painter's Text branch; declared here rather than in a public
// header, matching how painter.cpp itself declares it).
void emitTextGlyphs(const TextNodeData& text, const ComputedStyle& style, const Mat2x3& world, float alpha,
                    CommandList& out);
}  // namespace xt

using namespace xt;

namespace {

constexpr int kSize = 200;

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

/// One rendered image plus the geometry the test wants to assert on.
struct Rendered {
    std::vector<uint8_t> rgba;
    bool dark(int x, int y) const {
        size_t index = (static_cast<size_t>(y) * static_cast<size_t>(kSize) + static_cast<size_t>(x)) * 4;
        return rgba[index] < 128;
    }
    bool anyDarkInRow(int y, int x0, int x1) const {
        for (int x = x0; x < x1; ++x) {
            if (dark(x, y)) {
                return true;
            }
        }
        return false;
    }
    int darkCount() const {
        int count = 0;
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                if (dark(x, y)) {
                    ++count;
                }
            }
        }
        return count;
    }
};

/// Shapes + lays out + paints + rasterizes `utf8` at `origin` into a white kSize x kSize image.
Rendered renderText(text::FtTextShaper& shaper, const std::string& utf8, float fontSize,
                    TextDirection direction, WritingMode writingMode, Vec2 origin) {
    std::vector<std::string_view> families = {"Noto Sans", "Noto Sans SC", "Noto Naskh Arabic"};
    ShapeParams params;
    params.font.families = std::span<const std::string_view>(families);
    params.font.weight = 400;
    params.font.italic = false;
    params.size = fontSize;
    params.letterSpacing = 0;
    params.paragraphDirection = direction;
    params.writingMode = writingMode;
    params.orientation = TextOrientation::Mixed;

    ShapeResult shaped;
    shaper.shape(utf8, params, shaped);
    REQUIRE(!shaped.runs.empty());
    FontMetrics metrics = shaper.metrics(shaped.runs[0].font);

    TextLayoutParams layoutParams;
    layoutParams.availMain = 180.0f;
    layoutParams.fontSize = fontSize;
    layoutParams.lineHeight = fontSize * 1.2f;
    layoutParams.ascent = metrics.ascent * fontSize;
    layoutParams.descent = metrics.descent * fontSize;
    layoutParams.textAlign = TextAlign::Start;
    layoutParams.whiteSpace = WhiteSpace::Normal;
    layoutParams.textOverflow = TextOverflow::Clip;
    layoutParams.direction = direction;
    layoutParams.writingMode = writingMode;
    layoutParams.ellipsisAdvance = 0;
    layoutParams.ellipsisGlyph = 0;
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

    CommandList commands;
    commands.viewport = Rect{0, 0, static_cast<float>(kSize), static_cast<float>(kSize)};
    commands.devicePixelRatio = 1.0f;
    emitTextGlyphs(node, style, translation(origin), 1.0f, commands);

    Rendered out;
    out.rgba.assign(static_cast<size_t>(kSize) * static_cast<size_t>(kSize) * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{out.rgba.data(), kSize, kSize, kSize * 4};
    renderer.setTarget(target);
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(commands, &shaper, nullptr, nullptr);
    return out;
}

struct Fonts {
    text::FtTextShaper shaper;
    Fonts() {
        REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
        REQUIRE(shaper.addFontFile(fontPath("NotoSansSC-Regular.subset.ttf")));
        REQUIRE(shaper.addFontFile(fontPath("NotoNaskhArabic-Regular.subset.ttf")));
    }
};

}  // namespace

TEST_CASE("SW DrawGlyphs: a 64px 'H' has two stems and a solid crossbar") {
    Fonts fonts;
    Rendered image =
        renderText(fonts.shaper, "H", 64.0f, TextDirection::Ltr, WritingMode::HorizontalTb, Vec2{20, 20});
    CHECK(image.darkCount() > 200);

    // Dark bounding box.
    int minX = kSize, maxX = -1, minY = kSize, maxY = -1;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            if (image.dark(x, y)) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    REQUIRE(maxX > minX);
    REQUIRE(maxY > minY);

    // The crossbar row (vertical middle) is dark from stem to stem, with no gap.
    int midY = (minY + maxY) / 2;
    for (int x = minX + 1; x < maxX; ++x) {
        CAPTURE(x);
        CAPTURE(midY);
        CHECK(image.dark(x, midY));
    }

    // A row near the top has exactly two dark spans (the two stems).
    int topY = minY + (maxY - minY) / 6;
    int spans = 0;
    bool inSpan = false;
    for (int x = 0; x < kSize; ++x) {
        bool d = image.dark(x, topY);
        if (d && !inSpan) {
            ++spans;
        }
        inSpan = d;
    }
    CHECK(spans == 2);
}

TEST_CASE("SW DrawGlyphs: Arabic renders and sits at the RTL (right) end of the line") {
    Fonts fonts;
    // "مرحبا" = "marhaba".
    Rendered image = renderText(fonts.shaper, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7", 40.0f,
                                TextDirection::Rtl, WritingMode::HorizontalTb, Vec2{10, 20});
    CHECK(image.darkCount() > 100);

    int minX = kSize, maxX = -1;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            if (image.dark(x, y)) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
        }
    }
    REQUIRE(maxX > minX);
    // text-align: start with direction: rtl right-aligns the line inside availMain (180px from
    // x = 10), so the run ends near x = 190 and starts well past the left edge.
    CHECK(maxX > 170);
    CHECK(minX > 40);
}

TEST_CASE("SW DrawGlyphs: vertical writing-mode stacks two CJK glyphs") {
    Fonts fonts;
    // "日本" = "Nihon", upright in a vertical-rl column.
    Rendered image = renderText(fonts.shaper, "\xE6\x97\xA5\xE6\x9C\xAC", 40.0f, TextDirection::Ltr,
                                WritingMode::VerticalRl, Vec2{20, 10});
    CHECK(image.darkCount() > 100);

    int minY = kSize, maxY = -1;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            if (image.dark(x, y)) {
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    REQUIRE(maxY > minY);
    int midY = (minY + maxY) / 2;
    // Both halves of the column carry a glyph, and the two are separated by a blank row.
    CHECK(image.anyDarkInRow((minY + midY) / 2, 0, kSize));
    CHECK(image.anyDarkInRow((midY + maxY) / 2, 0, kSize));
    bool foundGap = false;
    for (int y = midY - 6; y <= midY + 6; ++y) {
        if (!image.anyDarkInRow(y, 0, kSize)) {
            foundGap = true;
        }
    }
    CHECK(foundGap);
}
