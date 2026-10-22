#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "xt/node.h"
#include "xt/text_shaper.h"

/// FakeTextShaper: a monospace, fixed-metrics TextShaper for test_text_layout.cpp. One glyph per
/// UTF-8 codepoint, configurable RTL ranges, no real font/HarfBuzz dependency. See
/// Docs/tasks/T-28.md "FakeTextShaper (tests)".
///
/// Simplifications (test-only, not part of the core contract):
/// - glyphId == codepoint (this is what lets layoutText's trailing-space/ellipsis logic identify
///   U+0020 via glyphId; see text_layout.cpp's top-of-file comment).
/// - '\n' (U+000A) is given 0 advance: it is a forced-break control character with no visual
///   width in this test shaper (breakAfter == 2 is what actually forces the line break; the core
///   layout algorithm does not special-case '\n').
/// - Run splitting happens on (bidi level, script) changes only, where "script" here is just the
///   two-way ASCII vs CJK (codepoint >= 0x2E80) split named by the task sheet; space and '\n' are
///   grouped with the ASCII category.
/// - glyphOutline() returns a fixed 1x1 em unit square, as placeholder data for future
///   Painter-side tests; it is not exercised by test_text_layout.cpp.

namespace xt {

class FakeTextShaper : public TextShaper {
public:
    /// `rtlRanges` are [start, end) codepoint-index ranges (not byte offsets) that get the
    /// "opposite of paragraph base direction" bidi level, per Docs/tasks/T-28.md.
    explicit FakeTextShaper(std::vector<std::pair<uint32_t, uint32_t>> rtlRanges = {})
        : m_rtlRanges(std::move(rtlRanges)) {}

    FontId match(const FontRequest&) override { return 1; }

    /// Number of shape() calls so far; used by test_layout_text_node.cpp to check that
    /// LayoutEngine's shaping cache (Docs/tasks/T-30.md) does not reshape unchanged text.
    uint32_t shapeCalls = 0;

    FontMetrics metrics(FontId) override {
        FontMetrics m;
        m.ascent = 0.8f;
        m.descent = 0.2f;
        m.lineGap = 0.0f;
        m.unitsPerEm = 1000.0f;
        return m;
    }

    /// T-331: ShapeParams::features of every shape() call so far, in call order.
    std::vector<uint8_t> featuresSeen;

    void shape(std::string_view utf8, const ShapeParams& params, ShapeResult& out) override {
        ++shapeCalls;
        featuresSeen.push_back(params.features);
        out.runs.clear();
        out.glyphs.clear();
        out.breakAfter.assign(utf8.size(), 0);
        out.graphemeStart.assign(utf8.size(), 0);

        // Decode UTF-8 into (byteOffset, byteLength, codepoint) triples.
        struct Cp {
            uint32_t byteOffset;
            uint32_t byteLength;
            uint32_t codepoint;
        };
        std::vector<Cp> cps;
        size_t i = 0;
        while (i < utf8.size()) {
            auto b0 = static_cast<uint8_t>(utf8[i]);
            uint32_t len = 1;
            uint32_t cp = b0;
            if ((b0 & 0x80u) == 0) {
                len = 1;
                cp = b0;
            } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
                len = 2;
                cp = (static_cast<uint32_t>(b0 & 0x1Fu) << 6) | (static_cast<uint32_t>(utf8[i + 1]) & 0x3Fu);
            } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
                len = 3;
                cp = (static_cast<uint32_t>(b0 & 0x0Fu) << 12) |
                     ((static_cast<uint32_t>(utf8[i + 1]) & 0x3Fu) << 6) |
                     (static_cast<uint32_t>(utf8[i + 2]) & 0x3Fu);
            } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < utf8.size()) {
                len = 4;
                cp = (static_cast<uint32_t>(b0 & 0x07u) << 18) |
                     ((static_cast<uint32_t>(utf8[i + 1]) & 0x3Fu) << 12) |
                     ((static_cast<uint32_t>(utf8[i + 2]) & 0x3Fu) << 6) |
                     (static_cast<uint32_t>(utf8[i + 3]) & 0x3Fu);
            }
            cps.push_back(Cp{static_cast<uint32_t>(i), len, cp});
            out.graphemeStart[i] = 1;
            i += len;
        }

        uint8_t baseLevel = (params.paragraphDirection == TextDirection::Rtl) ? 1 : 0;
        std::vector<uint8_t> levels(cps.size(), baseLevel);
        std::vector<uint8_t> scriptCat(cps.size(), 0);  // 0 = ASCII/other, 1 = CJK
        for (size_t k = 0; k < cps.size(); ++k) {
            bool inRtlRange = false;
            for (auto& range : m_rtlRanges) {
                if (k >= range.first && k < range.second) {
                    inRtlRange = true;
                    break;
                }
            }
            levels[k] = inRtlRange ? static_cast<uint8_t>(baseLevel + 1) : baseLevel;
            scriptCat[k] = (cps[k].codepoint >= 0x2E80u) ? 1 : 0;
        }

        // breakAfter and per-codepoint advance.
        std::vector<float> advance(cps.size(), 0.0f);
        for (size_t k = 0; k < cps.size(); ++k) {
            uint32_t cp = cps[k].codepoint;
            if (cp == 0x0Au) {
                advance[k] = 0.0f;
                out.breakAfter[cps[k].byteOffset] = 2;
            } else if (cp == 0x20u) {
                advance[k] = params.size * 0.3f;
                out.breakAfter[cps[k].byteOffset] = 1;
            } else if (cp >= 0x2E80u) {
                advance[k] = params.size * 1.0f;
                out.breakAfter[cps[k].byteOffset] = 1;
            } else {
                advance[k] = params.size * 0.6f;
            }
        }

        // Split into runs at (level, script) changes.
        size_t runStartCp = 0;
        while (runStartCp < cps.size()) {
            size_t runEndCp = runStartCp + 1;
            while (runEndCp < cps.size() && levels[runEndCp] == levels[runStartCp] &&
                   scriptCat[runEndCp] == scriptCat[runStartCp]) {
                ++runEndCp;
            }
            bool rtl = (levels[runStartCp] % 2) == 1;
            bool isVerticalMode = params.writingMode != WritingMode::HorizontalTb;
            bool isCjk = scriptCat[runStartCp] == 1;
            ShapedRun run;
            run.font = 1;
            run.bidiLevel = levels[runStartCp];
            // In vertical writing modes: CJK runs are upright, ASCII runs are rotated 90deg.
            // (Simplified "mixed" orientation; TextOrientation::Upright/Sideways are not
            // distinguished by this test shaper.)
            run.vertical = (isVerticalMode && isCjk) ? 1 : 0;
            run.rotated = (isVerticalMode && !isCjk) ? 1 : 0;
            run.byteStart = cps[runStartCp].byteOffset;
            run.byteEnd = cps[runEndCp - 1].byteOffset + cps[runEndCp - 1].byteLength;
            run.glyphStart = static_cast<uint32_t>(out.glyphs.size());
            run.glyphCount = static_cast<uint32_t>(runEndCp - runStartCp);

            size_t count = runEndCp - runStartCp;
            for (size_t n = 0; n < count; ++n) {
                size_t k = rtl ? (runEndCp - 1 - n) : (runStartCp + n);
                ShapedGlyph g;
                g.glyphId = cps[k].codepoint;
                if (run.vertical != 0) {
                    g.yAdvance = advance[k];
                } else {
                    g.xAdvance = advance[k];
                }
                g.xOffset = 0;
                g.yOffset = 0;
                g.cluster = cps[k].byteOffset;
                out.glyphs.push_back(g);
            }
            out.runs.push_back(run);
            runStartCp = runEndCp;
        }
    }

    bool glyphOutline(FontId, uint32_t, std::vector<PathVerb>& outVerbs,
                      std::vector<float>& outPoints) override {
        outVerbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
        outPoints = {0, 0, 1, 0, 1, 1, 0, 1};
        return true;
    }

private:
    std::vector<std::pair<uint32_t, uint32_t>> m_rtlRanges;
};

}  // namespace xt
