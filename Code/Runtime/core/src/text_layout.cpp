#include "xt/text_layout.h"

#include <algorithm>
#include <cstddef>

/// Line/column layout on top of a ShapeResult. See Docs/design/06-layout.md §6.8,
/// Docs/design/07-text.md §7.2/§7.5, Docs/tasks/T-28.md.
///
/// Known simplifications (uncovered by the design docs, chosen conservatively; see report):
/// - "Trailing space at line end" is detected as a glyph whose glyphId == 0x20 (U+0020). This is
///   exact for FakeTextShaper (glyphId == codepoint) but is a shaper-specific assumption for a
///   real HarfBuzz-backed shaper, where glyph ids are font-specific. Revisit when ShapeResult
///   gains an explicit "is collapsible space" bit, or when this core layer is given the source
///   text.
/// - text-overflow: ellipsis removes one *glyph* at a time from the line's visual end, standing
///   in for "one grapheme cluster at a time" (06.8/07.5). This is exact when a grapheme is always
///   one glyph (FakeTextShaper, and most non-ligature text); a multi-glyph ligature grapheme
///   could be partially trimmed by a real shaper.
/// - Vertical writing-mode text-align (top/bottom/center along the column's main axis) is not
///   detailed by 06.8/07.5 beyond column placement; this file maps TextAlign Start/Left to the
///   column's top and End/Right to its bottom (direction-resolved like the horizontal case), and
///   applies xOffset/yOffset to rotated runs the same way as upright runs, leaving any
///   rotation-specific offset correction to the Painter (T-30), consistent with "GlyphRun::rotated
///   保留給 Painter".

namespace xt {

namespace {

/// One shaped glyph flattened into logical (byte-ascending) order across the whole ShapeResult,
/// used for line breaking.
struct FlatGlyph {
    uint32_t runIndex = 0;
    uint32_t arrIndex = 0;  // index into ShapeResult::glyphs
    float advance = 0;      // main-axis advance for this glyph
    uint32_t clusterByte = 0;
};

/// Selects the main-axis advance of a glyph per Docs/design/06-layout.md §6.8: horizontal writing
/// mode always advances along x; vertical writing mode advances along y for upright runs and
/// along x for rotated runs.
float mainAdvanceOf(const ShapedRun& run, const ShapedGlyph& glyph, WritingMode writingMode) {
    if (writingMode == WritingMode::HorizontalTb) {
        return glyph.xAdvance;
    }
    return run.vertical != 0 ? glyph.yAdvance : glyph.xAdvance;
}

/// Byte offset one past the glyph at `arrIndex` in `run` (i.e. the byte offset of the next
/// logical glyph, or run.byteEnd if this is the run's last logical glyph).
uint32_t byteEndOfGlyph(const ShapedRun& run, uint32_t arrIndex, const ShapeResult& shaped) {
    bool rtl = (run.bidiLevel % 2) == 1;
    uint32_t l = rtl ? (run.glyphStart + run.glyphCount - 1 - arrIndex) : (arrIndex - run.glyphStart);
    if (l + 1 >= run.glyphCount) {
        return run.byteEnd;
    }
    uint32_t nextArr = rtl ? (arrIndex - 1) : (arrIndex + 1);
    return shaped.glyphs[nextArr].cluster;
}

/// Builds the logical-order glyph stream used for line breaking (§6.8).
void buildFlat(const ShapeResult& shaped, WritingMode writingMode, std::vector<FlatGlyph>& flat) {
    flat.clear();
    for (uint32_t ri = 0; ri < shaped.runs.size(); ++ri) {
        const ShapedRun& run = shaped.runs[ri];
        bool rtl = (run.bidiLevel % 2) == 1;
        for (uint32_t l = 0; l < run.glyphCount; ++l) {
            uint32_t arrIndex = rtl ? (run.glyphStart + run.glyphCount - 1 - l) : (run.glyphStart + l);
            const ShapedGlyph& g = shaped.glyphs[arrIndex];
            FlatGlyph fg;
            fg.runIndex = ri;
            fg.arrIndex = arrIndex;
            fg.advance = mainAdvanceOf(run, g, writingMode);
            fg.clusterByte = g.cluster;
            flat.push_back(fg);
        }
    }
}

/// Greedy line breaking over `flat` (§6.8). When `wrapEnabled` is false, only forced breaks
/// (breakAfter == 2) are honored (white-space: nowrap). Produces half-open [start, end) index
/// ranges into `flat`, one per line; never empty ranges except when `flat` itself is empty (no
/// ranges at all).
void breakLines(const std::vector<FlatGlyph>& flat, const std::vector<uint8_t>& breakAfter, float mainAvail,
                bool wrapEnabled, std::vector<std::pair<uint32_t, uint32_t>>& outRanges) {
    outRanges.clear();
    uint32_t n = static_cast<uint32_t>(flat.size());
    if (n == 0) {
        return;
    }
    std::vector<float> prefix(static_cast<size_t>(n) + 1, 0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + flat[i].advance;
    }

    uint32_t lineStart = 0;
    // Index into flat of the last break-allowed (breakAfter==1) glyph seen since lineStart, or
    // -1 if none.
    int64_t lastBreak = -1;
    uint32_t i = 0;
    while (i < n) {
        float lineAdvance = prefix[i + 1] - prefix[lineStart];
        if (wrapEnabled && lineAdvance > mainAvail && i > lineStart) {
            uint32_t cutEnd = (lastBreak >= static_cast<int64_t>(lineStart))
                                  ? static_cast<uint32_t>(lastBreak) + 1
                                  : i;  // no candidate: force break before this grapheme
            outRanges.push_back({lineStart, cutEnd});
            lineStart = cutEnd;
            lastBreak = -1;
            continue;  // reprocess glyph i against the new line
        }
        uint32_t cluster = flat[i].clusterByte;
        uint8_t breakVal = cluster < breakAfter.size() ? breakAfter[cluster] : 0;
        if (breakVal == 2) {
            outRanges.push_back({lineStart, i + 1});
            lineStart = i + 1;
            lastBreak = -1;
            ++i;
            continue;
        }
        if (breakVal == 1) {
            lastBreak = static_cast<int64_t>(i);
        }
        ++i;
    }
    if (lineStart < n) {
        outRanges.push_back({lineStart, n});
    }
}

/// One glyph in a line's final visual (left-to-right, or top-to-bottom for a column) order.
struct VisEntry {
    uint32_t glyphId = 0;
    float advance = 0;
    float xOffset = 0;
    float yOffset = 0;
    FontId font = kInvalidFont;
    uint8_t bidiLevel = 0;
    uint8_t vertical = 0;
    uint8_t rotated = 0;
    uint32_t byteStart = 0;
    uint32_t byteEnd = 0;
    bool isEllipsis = false;
    bool space = false;        // T-331: collapsible white space (ShapedGlyph::isSpace or U+0020).
    uint32_t tag = kNoRunTag;  // T-178: TextRunInfo::tag of the source run.
    float fontSize = 0;        // T-178: TextRunInfo::fontSize of the source run.
};

/// A contiguous (by original run) chunk of a line, in the line's logical (byte-ascending) order.
struct RunChunk {
    uint32_t runIndex = 0;
    uint32_t arrLo = 0;  // ascending array-index range into ShapeResult::glyphs; ascending array
    uint32_t arrHi = 0;  // index order is always visual left-to-right order (§7.2).
};

/// Splits flat[lineStart, lineEnd) into contiguous same-run chunks.
void collectChunks(const std::vector<FlatGlyph>& flat, uint32_t lineStart, uint32_t lineEnd,
                   std::vector<RunChunk>& outChunks) {
    outChunks.clear();
    uint32_t k = lineStart;
    while (k < lineEnd) {
        uint32_t runIndex = flat[k].runIndex;
        uint32_t segStart = k;
        uint32_t arrLo = flat[k].arrIndex;
        uint32_t arrHi = flat[k].arrIndex;
        ++k;
        while (k < lineEnd && flat[k].runIndex == runIndex) {
            arrLo = std::min(arrLo, flat[k].arrIndex);
            arrHi = std::max(arrHi, flat[k].arrIndex);
            ++k;
        }
        (void)segStart;
        RunChunk chunk;
        chunk.runIndex = runIndex;
        chunk.arrLo = arrLo;
        chunk.arrHi = arrHi + 1;
        outChunks.push_back(chunk);
    }
}

/// Resolves the main-axis start offset for `textAlign`/`direction` (§6.8: start/end resolve
/// against `direction`; left/center/right are absolute).
float resolveMainStart(TextAlign align, TextDirection direction, float mainAvail, float lineMain) {
    TextAlign effective = align;
    if (align == TextAlign::Start) {
        effective = (direction == TextDirection::Ltr) ? TextAlign::Left : TextAlign::Right;
    } else if (align == TextAlign::End) {
        effective = (direction == TextDirection::Ltr) ? TextAlign::Right : TextAlign::Left;
    }
    if (effective == TextAlign::Center) {
        return (mainAvail - lineMain) * 0.5f;
    }
    if (effective == TextAlign::Right) {
        return mainAvail - lineMain;
    }
    return 0.0f;
}

}  // namespace

void bidiReorder(std::span<const uint8_t> levels, std::vector<uint32_t>& outVisualOrder) {
    size_t n = levels.size();
    outVisualOrder.resize(n);
    for (size_t i = 0; i < n; ++i) {
        outVisualOrder[i] = static_cast<uint32_t>(i);
    }
    if (n == 0) {
        return;
    }
    uint8_t maxLevel = 0;
    uint8_t minOdd = 0xFFu;
    for (size_t i = 0; i < n; ++i) {
        maxLevel = std::max(maxLevel, levels[i]);
        if ((levels[i] % 2) == 1 && levels[i] < minOdd) {
            minOdd = levels[i];
        }
    }
    if (minOdd == 0xFFu) {
        return;  // no RTL levels: already in visual order
    }
    for (int level = static_cast<int>(maxLevel); level >= static_cast<int>(minOdd); --level) {
        size_t i = 0;
        while (i < n) {
            if (levels[i] >= static_cast<uint8_t>(level)) {
                size_t j = i + 1;
                while (j < n && levels[j] >= static_cast<uint8_t>(level)) {
                    ++j;
                }
                size_t lo = i;
                size_t hi = j - 1;
                while (lo < hi) {
                    std::swap(outVisualOrder[lo], outVisualOrder[hi]);
                    ++lo;
                    --hi;
                }
                i = j;
            } else {
                ++i;
            }
        }
    }
}

/// layoutText()'s "no line is clamped" marker (T-331).
constexpr uint32_t kNoClampedLine = 0xFFFFFFFFu;

void layoutText(const ShapeResult& shaped, const TextLayoutParams& params, TextLayoutResult& out) {
    out.runs.clear();
    out.lines.clear();
    out.mainSize = 0;
    out.crossSize = 0;
    out.intrinsicMain = 0;

    if (shaped.runs.empty()) {
        return;
    }

    bool vertical = params.writingMode != WritingMode::HorizontalTb;
    bool wrapEnabled = params.whiteSpace != WhiteSpace::NoWrap;

    std::vector<FlatGlyph> flat;
    buildFlat(shaped, params.writingMode, flat);
    if (flat.empty()) {
        return;
    }

    // intrinsicMain: longest run of text with no soft wrapping (forced breaks still apply).
    {
        std::vector<std::pair<uint32_t, uint32_t>> forcedOnly;
        breakLines(flat, shaped.breakAfter, 0.0f, false, forcedOnly);
        std::vector<float> prefix(flat.size() + 1, 0.0f);
        for (size_t i = 0; i < flat.size(); ++i) {
            prefix[i + 1] = prefix[i] + flat[i].advance;
        }
        for (const auto& r : forcedOnly) {
            out.intrinsicMain = std::max(out.intrinsicMain, prefix[r.second] - prefix[r.first]);
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> lineRanges;
    breakLines(flat, shaped.breakAfter, params.availMain, wrapEnabled, lineRanges);

    // line-clamp (05 §5.9.8, T-331): keep the first n lines; when any text follows, the last one
    // kept ends in the ellipsis below (the nowrap rule's trimming at its visual end).
    uint32_t clampedLine = kNoClampedLine;
    if (params.lineClamp > 0 && lineRanges.size() > params.lineClamp) {
        lineRanges.resize(params.lineClamp);
        clampedLine = params.lineClamp - 1;
    }

    std::vector<float> prefix(flat.size() + 1, 0.0f);
    for (size_t i = 0; i < flat.size(); ++i) {
        prefix[i + 1] = prefix[i] + flat[i].advance;
    }

    std::vector<RunChunk> chunks;
    std::vector<uint8_t> chunkLevels;
    std::vector<uint32_t> visualOrder;
    std::vector<VisEntry> vis;

    // T-178: with per-run metrics (rich text) every line gets its own cross size and baseline
    // metrics -- the largest of the runs on it (§6.8 "以行內最大字型度量"); otherwise every line
    // uses the uniform params.
    const bool rich = !params.runInfo.empty() && params.runInfo.size() == shaped.runs.size();
    std::vector<float> lineCross(lineRanges.size(), params.lineHeight);
    std::vector<float> lineAscent(lineRanges.size(), params.ascent);
    std::vector<float> lineDescent(lineRanges.size(), params.descent);
    if (rich) {
        for (size_t li = 0; li < lineRanges.size(); ++li) {
            float cross = 0;
            float ascent = 0;
            float descent = 0;
            for (uint32_t k = lineRanges[li].first; k < lineRanges[li].second; ++k) {
                const TextRunInfo& info = params.runInfo[flat[k].runIndex];
                cross = std::max(cross, info.lineHeight);
                ascent = std::max(ascent, info.ascent);
                descent = std::max(descent, info.descent);
            }
            lineCross[li] = cross;
            lineAscent[li] = ascent;
            lineDescent[li] = descent;
        }
    }
    float totalCross = 0;
    for (float c : lineCross) {
        totalCross += c;
    }
    float crossCursor = 0;  // sum of the cross sizes of the lines before the current one

    for (uint32_t lineIdx = 0; lineIdx < lineRanges.size(); ++lineIdx) {
        uint32_t lineStart = lineRanges[lineIdx].first;
        uint32_t lineEnd = lineRanges[lineIdx].second;

        uint32_t lineByteStart = flat[lineStart].clusterByte;
        uint32_t lineByteEnd =
            (lineEnd < flat.size()) ? flat[lineEnd].clusterByte : shaped.runs.back().byteEnd;

        // Trailing-space advance: trimmed from the reported line length, glyph itself is kept.
        float trailingSpaceAdvance = 0;
        {
            uint32_t k = lineEnd;
            while (k > lineStart) {
                const ShapedGlyph& g = shaped.glyphs[flat[k - 1].arrIndex];
                if (g.glyphId != 0x20u) {
                    break;
                }
                trailingSpaceAdvance += flat[k - 1].advance;
                --k;
            }
        }
        float lineMain = (prefix[lineEnd] - prefix[lineStart]) - trailingSpaceAdvance;

        // Build the line's run chunks (logical order) and their UAX#9 L2 visual order.
        collectChunks(flat, lineStart, lineEnd, chunks);
        chunkLevels.resize(chunks.size());
        for (size_t c = 0; c < chunks.size(); ++c) {
            chunkLevels[c] = shaped.runs[chunks[c].runIndex].bidiLevel;
        }
        bidiReorder(std::span<const uint8_t>(chunkLevels.data(), chunkLevels.size()), visualOrder);

        // Flatten into final visual order.
        vis.clear();
        for (uint32_t vo : visualOrder) {
            const RunChunk& chunk = chunks[vo];
            const ShapedRun& run = shaped.runs[chunk.runIndex];
            for (uint32_t arrIndex = chunk.arrLo; arrIndex < chunk.arrHi; ++arrIndex) {
                const ShapedGlyph& g = shaped.glyphs[arrIndex];
                VisEntry e;
                e.glyphId = g.glyphId;
                e.advance = mainAdvanceOf(run, g, params.writingMode);
                e.xOffset = g.xOffset;
                e.yOffset = g.yOffset;
                e.font = run.font;
                e.bidiLevel = run.bidiLevel;
                e.vertical = run.vertical;
                e.rotated = run.rotated;
                e.byteStart = g.cluster;
                e.byteEnd = byteEndOfGlyph(run, arrIndex, shaped);
                e.space = g.isSpace != 0 || g.glyphId == 0x20u;
                if (rich) {
                    e.tag = params.runInfo[chunk.runIndex].tag;
                    e.fontSize = params.runInfo[chunk.runIndex].fontSize;
                }
                vis.push_back(e);
            }
        }

        // text-overflow: ellipsis (nowrap only), and the last line a line-clamp keeps (T-331).
        const bool clamped = lineIdx == clampedLine;
        if (clamped && params.ellipsisAdvance > 0) {
            // The white space at the clamped line's visual end goes first, so the ellipsis
            // follows the last visible glyph (a U+0020 there is already outside lineMain).
            const bool ltr = params.direction == TextDirection::Ltr;
            while (!vis.empty() && (ltr ? vis.back() : vis.front()).space) {
                const VisEntry& end = ltr ? vis.back() : vis.front();
                if (end.glyphId != 0x20u) {
                    lineMain -= end.advance;
                }
                if (ltr) {
                    vis.pop_back();
                } else {
                    vis.erase(vis.begin());
                }
            }
        }
        if (((!wrapEnabled && params.textOverflow == TextOverflow::Ellipsis && lineMain > params.availMain) ||
             clamped) &&
            params.ellipsisAdvance > 0) {
            float budget = params.availMain - params.ellipsisAdvance;
            if (params.direction == TextDirection::Ltr) {
                while (!vis.empty() && lineMain > budget) {
                    lineMain -= vis.back().advance;
                    vis.pop_back();
                }
            } else {
                while (!vis.empty() && lineMain > budget) {
                    lineMain -= vis.front().advance;
                    vis.erase(vis.begin());
                }
            }
            VisEntry e;
            e.glyphId = params.ellipsisGlyph;
            e.advance = params.ellipsisAdvance;
            e.font = params.ellipsisFont;
            e.bidiLevel = (params.direction == TextDirection::Rtl) ? 1 : 0;
            e.isEllipsis = true;
            if (params.direction == TextDirection::Ltr) {
                e.byteStart = e.byteEnd = lineByteEnd;
                vis.push_back(e);
            } else {
                e.byteStart = e.byteEnd = lineByteStart;
                vis.insert(vis.begin(), e);
            }
            lineMain += params.ellipsisAdvance;
        }

        float mainStart = resolveMainStart(params.textAlign, params.direction, params.availMain, lineMain);

        // Cross-axis placement for this line/column.
        const float cross = lineCross[lineIdx];
        float crossOrigin;  // y for horizontal; x (left edge of the column) for vertical
        if (!vertical) {
            crossOrigin = crossCursor;
        } else if (params.writingMode == WritingMode::VerticalRl) {
            crossOrigin = totalCross - crossCursor - cross;
        } else {  // VerticalLr
            crossOrigin = crossCursor;
        }
        crossCursor += cross;
        float baseline =
            crossOrigin + (cross - (lineAscent[lineIdx] + lineDescent[lineIdx])) * 0.5f + lineAscent[lineIdx];
        float columnCenter = crossOrigin + cross * 0.5f;

        // Emit GlyphRuns: group consecutive vis entries sharing (font, bidiLevel, vertical,
        // rotated, isEllipsis).
        uint32_t firstRunForLine = static_cast<uint32_t>(out.runs.size());
        size_t vi = 0;
        float cursor = 0;  // running main-axis advance from mainStart
        while (vi < vis.size()) {
            size_t segEnd = vi + 1;
            while (segEnd < vis.size() && vis[segEnd].font == vis[vi].font &&
                   vis[segEnd].bidiLevel == vis[vi].bidiLevel && vis[segEnd].vertical == vis[vi].vertical &&
                   vis[segEnd].rotated == vis[vi].rotated && vis[segEnd].isEllipsis == vis[vi].isEllipsis &&
                   vis[segEnd].tag == vis[vi].tag) {
                ++segEnd;
            }
            GlyphRun run;
            run.font = vis[vi].font;
            run.bidiLevel = vis[vi].bidiLevel;
            run.vertical = vis[vi].vertical;
            run.rotated = vis[vi].rotated;
            run.byteStart = vis[vi].byteStart;
            run.byteEnd = vis[vi].byteStart;
            run.tag = vis[vi].tag;
            run.fontSize = vis[vi].fontSize;
            float segStartCursor = cursor;
            for (size_t k = vi; k < segEnd; ++k) {
                const VisEntry& e = vis[k];
                run.glyphs.push_back(e.glyphId);
                run.clusters.push_back(e.byteStart);
                if (!vertical) {
                    run.positions.push_back(Vec2{mainStart + cursor + e.xOffset, baseline + e.yOffset});
                } else {
                    run.positions.push_back(Vec2{columnCenter + e.xOffset, mainStart + cursor + e.yOffset});
                }
                cursor += e.advance;
                run.byteStart = std::min(run.byteStart, e.byteStart);
                run.byteEnd = std::max(run.byteEnd, e.byteEnd);
            }
            float segEndCursor = cursor;
            if (!vertical) {
                run.bounds =
                    Rect{mainStart + segStartCursor, crossOrigin, segEndCursor - segStartCursor, cross};
            } else {
                run.bounds =
                    Rect{crossOrigin, mainStart + segStartCursor, cross, segEndCursor - segStartCursor};
            }
            out.runs.push_back(std::move(run));
            vi = segEnd;
        }

        TextLine line;
        if (!vertical) {
            line.rect = Rect{mainStart, crossOrigin, lineMain, cross};
            line.baseline = baseline;
        } else {
            line.rect = Rect{crossOrigin, mainStart, cross, lineMain};
            line.baseline = 0;
        }
        line.firstRun = firstRunForLine;
        line.runCount = static_cast<uint32_t>(out.runs.size()) - firstRunForLine;
        line.byteStart = lineByteStart;
        line.byteEnd = lineByteEnd;
        out.lines.push_back(line);

        out.mainSize = std::max(out.mainSize, lineMain);
    }

    out.crossSize = totalCross;
}

}  // namespace xt
