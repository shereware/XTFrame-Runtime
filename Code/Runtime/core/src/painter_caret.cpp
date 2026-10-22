#include <cmath>
#include <vector>

#include "composition.h"
#include "rounded_rect.h"
#include "text_edit.h"
#include "xt/command_list.h"
#include "xt/node.h"
#include "xt/property.h"

/// emitTextSelection / emitTextCaret / emitCompositionDecoration: the FillPath emitters the
/// Painter's Text branch adds for a focused, editable `input` (painter.cpp). See
/// Docs/tasks/T-33.md "Painter", Docs/tasks/T-33b.md "Painter", Docs/design/08-events.md
/// §8.5, §8.8.
///
/// Not covered by Docs/tasks/T-33.md (conservative choices made here):
///   - The selection highlight is drawn only while the element has the focus, like the caret (an
///     unfocused input shows neither).
///   - Both are axis-aligned rectangles in the Text node's own coordinate system, so they follow
///     the element's transform exactly like its glyphs do.
///   - The caret's height is the line's rect height (the resolved line-height), and its line is
///     the one whose byte range contains the caret, falling back to the last line.
///   - The caret is 1 logical pixel wide, drawn to the right of its x (x .. x+1), and is clamped
///     nowhere: an input whose text overflows its box relies on the element's own overflow clip.
///
/// Not covered by Docs/tasks/T-33b.md (conservative choices made here):
///   - The composition underline spans, per GlyphRun, exactly the x range the composition's
///     byte range covers inside that run (runSelectionSpan, the same helper the selection
///     highlight uses) rather than the run's whole `bounds`, so a run only partly inside the
///     composition is not underlined end to end.
///   - The IME clause highlight uses the ordinary selection color and the line's full height.
///   - CompositionState::suppressDefault suppresses the underline and the clause highlight but
///     not the caret (the host cannot draw one of its own).

namespace xt {

namespace {

/// Selection highlight color, Docs/tasks/T-33.md: rgba(51, 153, 255, 0.35).
constexpr Color kSelectionColor{51.0f / 255.0f, 153.0f / 255.0f, 255.0f / 255.0f, 0.35f};

void appendRectFill(Rect rect, Color color, const Mat2x3& world, CommandList& out,
                    std::vector<PathVerb>& verbs, std::vector<float>& points) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a <= 0.0f) {
        return;
    }
    verbs.clear();
    points.clear();
    appendRoundedRectPath(rect, 0, 0, 0, 0, verbs, points);

    FillCmd fc{};
    fc.xf = world;
    fc.path.verbOffset = static_cast<uint32_t>(out.verbs.size());
    out.verbs.insert(out.verbs.end(), verbs.begin(), verbs.end());
    fc.path.verbCount = static_cast<uint32_t>(verbs.size());
    fc.path.pointOffset = static_cast<uint32_t>(out.points.size());
    out.points.insert(out.points.end(), points.begin(), points.end());
    fc.path.pointCount = static_cast<uint32_t>(points.size());
    fc.paint.kind = PaintKind::Solid;
    fc.paint.color = color;
    fc.rule = FillRule::NonZero;
    fc.antialias = 1;
    uint32_t index = static_cast<uint32_t>(out.fills.size());
    out.fills.push_back(fc);
    out.commands.push_back(Command{CmdKind::FillPath, {}, index});
}

/// The x range every GlyphRun of every line covers for the logical byte range [from, to),
/// handed to `emit` as (line, x0, x1). Shared by the selection highlight and the composition
/// underline/clause highlight.
template <class Fn>
void forEachRangeSpan(const TextNodeData& text, uint32_t from, uint32_t to, Fn emit) {
    for (const TextLine& line : text.lines) {
        for (uint32_t i = 0; i < line.runCount; ++i) {
            const GlyphRun& run = text.runs[line.firstRun + i];
            float x0 = 0;
            float x1 = 0;
            if (!runSelectionSpan(text.shaped, run, from, to, x0, x1)) {
                continue;
            }
            emit(line, x0, x1);
        }
    }
}

}  // namespace

void emitTextSelection(const TextNodeData& text, const EditState& state, const Mat2x3& world, float alpha,
                       CommandList& out, std::vector<PathVerb>& verbs, std::vector<float>& points) {
    if (!hasSelection(state) || text.runs.empty()) {
        return;
    }
    Color color = kSelectionColor;
    color.a *= alpha;
    forEachRangeSpan(
        text, selectionBegin(state), selectionEnd(state), [&](const TextLine& line, float x0, float x1) {
            appendRectFill(Rect{x0, line.rect.y, x1 - x0, line.rect.h}, color, world, out, verbs, points);
        });
}

void emitCompositionDecoration(const TextNodeData& text, const ComputedStyle& style,
                               const CompositionState& comp, const Mat2x3& world, float alpha,
                               CommandList& out, std::vector<PathVerb>& verbs, std::vector<float>& points) {
    if (comp.suppressDefault || comp.text.empty() || text.runs.empty()) {
        return;
    }
    // 08.8 / T-33b: the IME's selected clause first (behind the glyphs, like an ordinary
    // selection), then a 1px underline 2px below the baseline across the whole composition.
    if (comp.selLen > 0) {
        Color highlight = kSelectionColor;
        highlight.a *= alpha;
        uint32_t from = compositionCaret(comp);
        forEachRangeSpan(text, from, from + comp.selLen, [&](const TextLine& line, float x0, float x1) {
            appendRectFill(Rect{x0, line.rect.y, x1 - x0, line.rect.h}, highlight, world, out, verbs, points);
        });
    }
    Color color = style.base[prop::Color].c;
    color.a *= alpha;
    forEachRangeSpan(
        text, compositionBegin(comp), compositionEnd(comp), [&](const TextLine& line, float x0, float x1) {
            appendRectFill(Rect{x0, line.baseline + 2.0f, x1 - x0, 1.0f}, color, world, out, verbs, points);
        });
}

void emitTextCaret(const TextNodeData& text, const ComputedStyle& style, uint32_t caretOffset,
                   const Mat2x3& world, float alpha, float clock, CommandList& out,
                   std::vector<PathVerb>& verbs, std::vector<float>& points) {
    // 08.5: 1 second blink period, visible in its first half.
    float phase = std::fmod(clock, 1.0f);
    if (phase < 0.0f) {
        phase += 1.0f;
    }
    if (phase >= 0.5f) {
        return;
    }
    Rect rect{};
    if (!caretLocalRect(text, caretOffset, rect)) {
        return;
    }
    Color color = style.base[prop::Color].c;
    color.a *= alpha;
    appendRectFill(rect, color, world, out, verbs, points);
}

}  // namespace xt
