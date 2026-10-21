#include "text_edit.h"

#include <cstddef>

/// Pure text-editing operations for `input[type=text]` (docs/tasks/T-33.md "編輯操作").
///
/// Not covered by docs/tasks/T-33.md (conservative choices made here):
///   - Left/Right with an existing selection and no Shift collapse the caret to the selection's
///     logical begin/end (the usual editor behavior) instead of moving one grapheme.
///   - Visual movement treats the shaped text as one line: an `input` whose value wraps (the
///     first version never scrolls an input, so this only happens with an overlong value) moves
///     the caret along the unwrapped visual order, not per line.
///   - When the ShapeResult does not match the value byte-for-byte (two edits in the same frame,
///     before the next layout reshapes), every grapheme step falls back to UTF-8 codepoint
///     boundaries and Left/Right fall back to logical (not visual) direction.

namespace xt {

namespace {

bool isUtf8Continuation(char c) { return (static_cast<uint8_t>(c) & 0xC0u) == 0x80u; }

/// True when `shaped` describes exactly `s.value` and carries at least one run, i.e. when its
/// graphemeStart/glyph data may be trusted for this value.
bool shapedMatches(const EditState& s, const ShapeResult& shaped) {
    return !shaped.runs.empty() && shaped.graphemeStart.size() == s.value.size();
}

bool isBoundary(const EditState& s, const ShapeResult& shaped, uint32_t off) {
    if (off == 0 || off >= static_cast<uint32_t>(s.value.size())) {
        return true;
    }
    if (shapedMatches(s, shaped)) {
        return shaped.graphemeStart[off] != 0;
    }
    return !isUtf8Continuation(s.value[off]);
}

/// Per-run visual placement of one shaped paragraph: every run's x origin (in UAX#9 L2 visual
/// order, starting at 0) and its total advance. Built once per movement operation.
struct VisualRuns {
    std::vector<float> runX;
    std::vector<float> runWidth;

    void build(const ShapeResult& shaped) {
        size_t count = shaped.runs.size();
        runX.assign(count, 0.0f);
        runWidth.assign(count, 0.0f);
        m_levels.clear();
        m_levels.reserve(count);
        for (const ShapedRun& run : shaped.runs) {
            m_levels.push_back(run.bidiLevel);
            float width = 0;
            for (uint32_t g = 0; g < run.glyphCount; ++g) {
                width += shaped.glyphs[run.glyphStart + g].xAdvance;
            }
            runWidth[static_cast<size_t>(m_levels.size() - 1)] = width;
        }
        bidiReorder(std::span<const uint8_t>(m_levels), m_visualOrder);
        float x = 0;
        for (uint32_t index : m_visualOrder) {
            runX[index] = x;
            x += runWidth[index];
        }
    }

private:
    std::vector<uint8_t> m_levels;
    std::vector<uint32_t> m_visualOrder;
};

/// The run holding caret offset `off`, with trailing affinity: a boundary shared by two runs
/// belongs to the run that ends there. Falls back to a containing run, then to run 0.
size_t pickRun(const ShapeResult& shaped, uint32_t off) {
    for (size_t i = 0; i < shaped.runs.size(); ++i) {
        if (shaped.runs[i].byteStart < off && off <= shaped.runs[i].byteEnd) {
            return i;
        }
    }
    for (size_t i = 0; i < shaped.runs.size(); ++i) {
        if (shaped.runs[i].byteStart <= off && off < shaped.runs[i].byteEnd) {
            return i;
        }
    }
    return 0;
}

/// Sum of the advances of every glyph of `run` whose cluster lies in [run.byteStart, off).
float advanceBefore(const ShapeResult& shaped, const ShapedRun& run, uint32_t off) {
    float sum = 0;
    for (uint32_t g = 0; g < run.glyphCount; ++g) {
        const ShapedGlyph& glyph = shaped.glyphs[run.glyphStart + g];
        if (glyph.cluster >= run.byteStart && glyph.cluster < off) {
            sum += glyph.xAdvance;
        }
    }
    return sum;
}

/// Same, for a laid-out GlyphRun (whose byte range is the one the Painter sees).
float advanceBeforeLaidOut(const ShapeResult& shaped, const GlyphRun& run, uint32_t off) {
    float sum = 0;
    for (const ShapedGlyph& glyph : shaped.glyphs) {
        if (glyph.cluster >= run.byteStart && glyph.cluster < run.byteEnd && glyph.cluster < off) {
            sum += glyph.xAdvance;
        }
    }
    return sum;
}

/// Visual x of caret offset `off` within the shaped paragraph (line start at x == 0).
float visualCaretX(const ShapeResult& shaped, const VisualRuns& visual, uint32_t off) {
    size_t index = pickRun(shaped, off);
    const ShapedRun& run = shaped.runs[index];
    float before = advanceBefore(shaped, run, off);
    if ((run.bidiLevel % 2) == 1) {
        return visual.runX[index] + visual.runWidth[index] - before;
    }
    return visual.runX[index] + before;
}

/// Shared Left/Right body: steps to the grapheme boundary whose visual x is nearest on the
/// requested side. `dir` is +1 for Right, -1 for Left.
void moveVisual(EditState& s, const ShapeResult& shaped, bool extendSelection, int dir) {
    if (!extendSelection && hasSelection(s)) {
        s.cursor = dir > 0 ? selectionEnd(s) : selectionBegin(s);
        s.anchor = s.cursor;
        return;
    }
    uint32_t length = static_cast<uint32_t>(s.value.size());
    if (!shapedMatches(s, shaped)) {
        s.cursor = dir > 0 ? nextGrapheme(s, shaped, s.cursor) : prevGrapheme(s, shaped, s.cursor);
        if (!extendSelection) {
            s.anchor = s.cursor;
        }
        return;
    }

    VisualRuns visual;
    visual.build(shaped);
    constexpr float kEpsilon = 1.0e-4f;
    float currentX = visualCaretX(shaped, visual, s.cursor);
    bool found = false;
    uint32_t best = s.cursor;
    float bestX = 0;
    for (uint32_t off = 0; off <= length; ++off) {
        if (!isBoundary(s, shaped, off)) {
            continue;
        }
        float x = visualCaretX(shaped, visual, off);
        bool candidate = dir > 0 ? (x > currentX + kEpsilon) : (x < currentX - kEpsilon);
        if (!candidate) {
            continue;
        }
        if (!found || (dir > 0 ? (x < bestX) : (x > bestX))) {
            found = true;
            best = off;
            bestX = x;
        }
    }
    s.cursor = best;
    if (!extendSelection) {
        s.anchor = s.cursor;
    }
}

}  // namespace

uint32_t nextGrapheme(const EditState& s, const ShapeResult& shaped, uint32_t off) {
    uint32_t length = static_cast<uint32_t>(s.value.size());
    if (off >= length) {
        return length;
    }
    uint32_t next = off + 1;
    while (next < length && !isBoundary(s, shaped, next)) {
        ++next;
    }
    return next;
}

uint32_t prevGrapheme(const EditState& s, const ShapeResult& shaped, uint32_t off) {
    uint32_t length = static_cast<uint32_t>(s.value.size());
    if (off == 0) {
        return 0;
    }
    uint32_t prev = (off > length ? length : off) - 1;
    while (prev > 0 && !isBoundary(s, shaped, prev)) {
        --prev;
    }
    return prev;
}

bool insertUtf8(EditState& s, std::string_view bytes) {
    uint32_t begin = selectionBegin(s);
    uint32_t end = selectionEnd(s);
    if (bytes.empty() && begin == end) {
        return false;
    }
    s.value.replace(begin, static_cast<size_t>(end - begin), bytes);
    s.cursor = begin + static_cast<uint32_t>(bytes.size());
    s.anchor = s.cursor;
    return true;
}

bool deleteBackward(EditState& s, const ShapeResult& shaped) {
    if (hasSelection(s)) {
        return insertUtf8(s, std::string_view{});
    }
    if (s.cursor == 0) {
        return false;
    }
    uint32_t begin = prevGrapheme(s, shaped, s.cursor);
    s.value.erase(begin, static_cast<size_t>(s.cursor - begin));
    s.cursor = begin;
    s.anchor = begin;
    return true;
}

bool deleteForward(EditState& s, const ShapeResult& shaped) {
    if (hasSelection(s)) {
        return insertUtf8(s, std::string_view{});
    }
    if (s.cursor >= static_cast<uint32_t>(s.value.size())) {
        return false;
    }
    uint32_t end = nextGrapheme(s, shaped, s.cursor);
    s.value.erase(s.cursor, static_cast<size_t>(end - s.cursor));
    s.anchor = s.cursor;
    return true;
}

void moveLeft(EditState& s, const ShapeResult& shaped, bool extendSelection) {
    moveVisual(s, shaped, extendSelection, -1);
}

void moveRight(EditState& s, const ShapeResult& shaped, bool extendSelection) {
    moveVisual(s, shaped, extendSelection, 1);
}

void moveHome(EditState& s, bool extendSelection) {
    s.cursor = 0;
    if (!extendSelection) {
        s.anchor = 0;
    }
}

void moveEnd(EditState& s, bool extendSelection) {
    s.cursor = static_cast<uint32_t>(s.value.size());
    if (!extendSelection) {
        s.anchor = s.cursor;
    }
}

void selectAll(EditState& s) {
    s.anchor = 0;
    s.cursor = static_cast<uint32_t>(s.value.size());
}

void setCaret(EditState& s, const ShapeResult& shaped, uint32_t byteOffset, bool extendSelection) {
    uint32_t length = static_cast<uint32_t>(s.value.size());
    uint32_t off = byteOffset > length ? length : byteOffset;
    while (off > 0 && !isBoundary(s, shaped, off)) {
        --off;
    }
    s.cursor = off;
    if (!extendSelection) {
        s.anchor = off;
    }
}

float caretX(const ShapeResult& shaped, const std::vector<GlyphRun>& runs, uint32_t byteOffset) {
    if (runs.empty()) {
        return 0.0f;
    }
    size_t index = 0;
    bool found = false;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].byteStart < byteOffset && byteOffset <= runs[i].byteEnd) {
            index = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].byteStart <= byteOffset && byteOffset < runs[i].byteEnd) {
                index = i;
                break;
            }
        }
    }
    const GlyphRun& run = runs[index];
    float before = advanceBeforeLaidOut(shaped, run, byteOffset);
    if ((run.bidiLevel % 2) == 1) {
        return run.bounds.x + run.bounds.w - before;
    }
    return run.bounds.x + before;
}

bool runSelectionSpan(const ShapeResult& shaped, const GlyphRun& run, uint32_t from, uint32_t to,
                      float& outX0, float& outX1) {
    uint32_t lo = from > run.byteStart ? from : run.byteStart;
    uint32_t hi = to < run.byteEnd ? to : run.byteEnd;
    if (hi <= lo) {
        return false;
    }
    float a = advanceBeforeLaidOut(shaped, run, lo);
    float b = advanceBeforeLaidOut(shaped, run, hi);
    if ((run.bidiLevel % 2) == 1) {
        outX0 = run.bounds.x + run.bounds.w - b;
        outX1 = run.bounds.x + run.bounds.w - a;
    } else {
        outX0 = run.bounds.x + a;
        outX1 = run.bounds.x + b;
    }
    return true;
}

}  // namespace xt
