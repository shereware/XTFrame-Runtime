#include <algorithm>
#include <cmath>
#include <span>

#include "rounded_rect.h"
#include "text_runs.h"
#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/property.h"

/// emitTextGlyphs: one DrawGlyphs command per laid-out GlyphRun of a Text node, the Painter's
/// Text branch (painter.cpp). See Docs/tasks/T-30.md goal 3, Docs/design/09-command-list.md §9.3,
/// Docs/design/07-text.md §7.5.
///
/// Not covered by Docs/tasks/T-30.md (conservative choices made here):
///   - A Text node has no style of its own, so `color`/`font-size` come from its parent Element's
///     ComputedStyle (the same style its layout used).
///   - A run whose font is kInvalidFont, or which has no glyphs, emits no command at all.
///   - `visibility: hidden` on the parent Element suppresses its text as well as its own
///     background/border/image (T-27 rule 1 treats text as part of the element's own visuals).
///
/// T-178 (Docs/design/07-text.md §7.8):
///   - A rich-text run draws in its own color/size; its `text-effect` (or the element's) becomes
///     extra DrawGlyphs commands with GlyphsCmd::effect set, emitted before (under) the fill, the
///     last-listed effect first. `shadow` moves its command's transform by (dx, dy).
///   - `text-reveal` < 1 keeps the glyphs whose cluster starts before the first hidden grapheme
///     cluster (`floor(reveal * clusters + 1e-4)` clusters are shown) and the inline objects whose
///     placeholder does.
///   - Inline objects (TextInline with a laid-out box and an asset) become DrawImage commands after
///     the text.
///
/// T-324 (Docs/design/05-style-and-css.md §5.9.8): `text-decoration` is one FillPath rectangle per
/// line kind per run, emitted right after the run's glyphs (text effect -> glyphs -> decorations),
/// never through the effect chain. The rectangle spans the run box horizontally (its advances,
/// letter-spacing included; RTL runs alike) and is kept inside the run box vertically, so the
/// text's paint bounds do not grow. Conservative choices made here:
///   - Underline and line-through are centered on their line (FontMetrics::underlineOffset is the
///     underline's center, FreeType's convention); the overline's top edge sits at the ascent.
///   - The rectangle is snapped to whole text-node pixels (thickness >= 1px) before it is kept
///     in the run box, so the line stays crisp and the SW and GL rasterizers cover the same rows.
///   - Vertical (upright or rotated) runs draw no decoration.
///   - A run hidden by `text-reveal` draws none; a partly revealed run keeps its whole line.

namespace xt {

namespace {

/// `text-reveal`: the first byte offset that is hidden, or UINT32_MAX when everything is shown.
uint32_t revealLimit(const TextNodeData& text, float reveal) {
    if (!(reveal < 1.0f)) {
        return 0xFFFFFFFFu;
    }
    const std::vector<uint8_t>& starts = text.shaped.graphemeStart;
    uint32_t clusters = 0;
    for (uint8_t s : starts) {
        clusters += s != 0 ? 1u : 0u;
    }
    const float clamped = std::max(0.0f, reveal);
    const uint32_t shown = static_cast<uint32_t>(std::floor(clamped * static_cast<float>(clusters) + 1e-4f));
    if (shown >= clusters) {
        return 0xFFFFFFFFu;
    }
    uint32_t seen = 0;
    for (uint32_t i = 0; i < starts.size(); ++i) {
        if (starts[i] != 0) {
            if (seen == shown) {
                return i;
            }
            ++seen;
        }
    }
    return 0xFFFFFFFFu;
}

/// T-324: one solid, antialiased rectangle in text-node space.
void appendDecorationRect(Rect rect, Color color, const Mat2x3& world, CommandList& out,
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
    const uint32_t index = static_cast<uint32_t>(out.fills.size());
    out.fills.push_back(fc);
    out.commands.push_back(Command{CmdKind::FillPath, {}, index});
}

/// T-324: the baseline of `runIndex`'s line (a horizontal run's glyph origins sit on it).
float runBaseline(const TextNodeData& text, size_t runIndex) {
    for (const TextLine& line : text.lines) {
        if (runIndex >= line.firstRun && runIndex < static_cast<size_t>(line.firstRun) + line.runCount) {
            return line.baseline;
        }
    }
    const GlyphRun& run = text.runs[runIndex];
    return run.positions.empty() ? run.bounds.y + run.bounds.h : run.positions[0].y;
}

/// T-324: the color and thickness (px; < 0 = `auto`) one decoration line is drawn with.
struct DecorationPaint {
    Color color{0, 0, 0, 1};
    float thickness = -1.0f;
};

/// T-324: which decoration lines `run` draws and in what paint, per kTextDecoration* bit (index
/// 0 underline, 1 overline, 2 line-through). The element's own lines use its (animatable)
/// `text-decoration-color` / `-thickness`; the ancestors' lines theirs (ComputedStyle::
/// inheritedDecoration*); a run override's extra lines its own `text-decoration-color`, else its
/// own `color`, else the element's decoration color.
uint8_t decorationsOf(const ComputedStyle& style, const TextRunStyle* runStyle, DecorationPaint (&paint)[3]) {
    const uint8_t own = style.textDecorationLine;
    const uint8_t inherited = static_cast<uint8_t>(style.inheritedDecorations & ~own);
    const uint8_t fromRun =
        runStyle != nullptr ? static_cast<uint8_t>(runStyle->decorationLine & ~(own | inherited)) : 0;
    const Length& ownThickness = style.base[prop::TextDecorationThickness].len;
    DecorationPaint ownPaint{style.base[prop::TextDecorationColor].c,
                             ownThickness.unit == LengthUnit::Px ? ownThickness.value : -1.0f};
    DecorationPaint inheritedPaint{style.inheritedDecorationColor, style.inheritedDecorationThickness};
    DecorationPaint runPaint = ownPaint;
    if (runStyle != nullptr) {
        if (runStyle->hasDecorationColor) {
            runPaint.color = runStyle->decorationColor;
        } else if (runStyle->hasColor) {
            runPaint.color = runStyle->color;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        paint[i] = (own & bit) != 0 ? ownPaint : (inherited & bit) != 0 ? inheritedPaint : runPaint;
    }
    return static_cast<uint8_t>((own | inherited | fromRun) & kTextDecorationAll);
}

/// T-324: emits `run`'s decoration rectangles (see the file comment).
void emitRunDecorations(const TextNodeData& text, size_t runIndex, const ComputedStyle& style,
                        const TextRunStyle* runStyle, float size, float alpha, const Mat2x3& world,
                        CommandList& out) {
    DecorationPaint paint[3];
    const uint8_t lines = decorationsOf(style, runStyle, paint);
    const GlyphRun& run = text.runs[runIndex];
    if (lines == 0 || run.vertical != 0 || run.rotated != 0 || run.bounds.w <= 0.0f || run.bounds.h <= 0.0f) {
        return;
    }
    const float baseline = runBaseline(text, runIndex);
    // The font's height (ascent + descent) caps an extreme thickness; the run box caps it too.
    float fontHeight = (style.decoAscent + std::fabs(style.decoDescent)) * size;
    if (!(fontHeight > 0.0f)) {
        fontHeight = size;
    }
    const float maxThickness = std::min(fontHeight, run.bounds.h);
    std::vector<PathVerb> verbs;
    std::vector<float> points;
    for (int i = 0; i < 3; ++i) {
        if ((lines & (1u << i)) == 0) {
            continue;
        }
        float t = paint[i].thickness;
        if (t < 0.0f) {  // `auto`: the font's own thickness, else max(1px, 1/14 em).
            t = style.decoUnderlineThickness > 0.0f ? style.decoUnderlineThickness * size
                                                    : std::max(1.0f, size / 14.0f);
        }
        t = std::min(t, maxThickness);
        if (!(t > 0.0f)) {
            continue;
        }
        float top = 0;
        if (i == 0) {  // underline
            const float offset = style.decoUnderlineOffset != 0.0f ? style.decoUnderlineOffset : 0.1f;
            top = baseline + offset * size - t * 0.5f;
        } else if (i == 1) {  // overline
            top = style.decoAscent > 0.0f ? baseline - style.decoAscent * size : run.bounds.y;
        } else {  // line-through
            top = baseline - 0.3f * size - t * 0.5f;
        }
        // Whole pixels (see the file comment), then inside the run box.
        t = std::min(std::max(1.0f, std::round(t)), std::max(1.0f, std::floor(run.bounds.h)));
        top = std::round(top);
        const float boxTop = std::ceil(run.bounds.y);
        const float boxBottom = std::floor(run.bounds.y + run.bounds.h);
        if (boxBottom - boxTop >= t) {
            top = std::clamp(top, boxTop, boxBottom - t);
        } else {
            top = std::clamp(top, run.bounds.y, run.bounds.y + run.bounds.h - t);
        }
        Color color = paint[i].color;
        color.a *= alpha;
        appendDecorationRect(Rect{run.bounds.x, top, run.bounds.w, t}, color, world, out, verbs, points);
    }
}

}  // namespace

void emitTextGlyphs(const TextNodeData& text, const ComputedStyle& style, const Mat2x3& world, float alpha,
                    const IrView* ir, CommandList& out) {
    if (text.runs.empty()) {
        return;
    }
    const Color baseColor = style.base[prop::Color].c;
    const float baseSize = style.base[prop::FontSize].len.value;
    const uint32_t limit = revealLimit(text, style.base[prop::TextReveal].f);
    std::span<const IrEffect> effectPool;
    if (ir != nullptr) {
        effectPool = ir->style().effects();
    }

    TextRunStyle runStyle;
    for (size_t runIndex = 0; runIndex < text.runs.size(); ++runIndex) {
        const GlyphRun& run = text.runs[runIndex];
        if (run.font == kInvalidFont || run.glyphs.empty()) {
            continue;
        }
        Color color = baseColor;
        uint32_t effectOffset = style.textEffectOffset;
        uint32_t effectCount = style.textEffectCount;
        const TextRunStyle* overrides = nullptr;
        if (ir != nullptr && run.tag < text.styleRuns.size()) {
            resolveTextRunStyle(*ir, style, 0.0f, &text.styleRuns[run.tag], runStyle);
            overrides = &runStyle;
            if (runStyle.hasColor) {
                color = runStyle.color;
            }
            effectOffset = runStyle.effectOffset;
            effectCount = runStyle.effectCount;
        }
        color.a *= alpha;
        if (color.a <= 0.0f) {
            continue;
        }
        const float size = run.fontSize > 0.0f ? run.fontSize : baseSize;

        GlyphsCmd cmd{};
        bool rotated = run.rotated != 0;
        Mat2x3 local{};     // text-node space -> the command's glyph space
        Mat2x3 unrotate{};  // Only used when `rotated`.
        if (rotated) {
            // §6.8/§7.5: a rotated (sideways) run is drawn 90 degrees clockwise, about its own
            // bounds' center. layoutText() already placed its glyph origins down the column
            // (Docs/tasks/T-28.md leaves "any rotation-specific offset correction to the
            // Painter"), so the stored positions are pre-mapped through the inverse rotation
            // here: each glyph origin then lands back exactly where layout put it, with the
            // glyph itself rotated. (The pivot cancels out of that composition, so it only
            // documents the intent -- any pivot gives the same result.)
            Vec2 center{run.bounds.x + run.bounds.w * 0.5f, run.bounds.y + run.bounds.h * 0.5f};
            Vec2 back{-center.x, -center.y};
            local = mul(translation(center), mul(rotationDeg(90), translation(back)));
            unrotate = mul(translation(center), mul(rotationDeg(-90), translation(back)));
        }
        cmd.xf = mul(world, local);
        cmd.font = run.font;
        cmd.size = size;
        cmd.color = color;
        cmd.rendering = style.textRendering;
        cmd.glyphOffset = static_cast<uint32_t>(out.glyphs.size());
        const bool filter = limit != 0xFFFFFFFFu && run.clusters.size() == run.glyphs.size();
        for (size_t i = 0; i < run.glyphs.size(); ++i) {
            if (filter && run.clusters[i] >= limit) {
                continue;
            }
            out.glyphs.push_back(run.glyphs[i]);
            out.glyphPos.push_back(rotated ? apply(unrotate, run.positions[i]) : run.positions[i]);
        }
        cmd.glyphCount = static_cast<uint32_t>(out.glyphs.size()) - cmd.glyphOffset;
        if (cmd.glyphCount == 0) {
            continue;
        }

        // Glyph-level effects, bottom-most (last listed) first; they share the fill's glyphs.
        if (effectCount > 0 && effectOffset <= effectPool.size() &&
            effectCount <= effectPool.size() - effectOffset) {
            for (uint32_t e = effectCount; e-- > 0;) {
                const IrEffect& fx = effectPool[effectOffset + e];
                GlyphsCmd effectCmd = cmd;
                Vec2 offset{0, 0};
                switch (static_cast<EffectKind>(fx.kind)) {
                    case EffectKind::Outline:
                        effectCmd.effect = GlyphEffect::Dilate;
                        effectCmd.effectRadius = fx.params[0];
                        break;
                    case EffectKind::Glow:
                        effectCmd.effect = GlyphEffect::Blur;
                        effectCmd.effectRadius = fx.params[0];
                        break;
                    case EffectKind::DropShadow:
                        effectCmd.effect = GlyphEffect::Blur;
                        effectCmd.effectRadius = fx.params[2];
                        offset = Vec2{fx.params[0], fx.params[1]};
                        break;
                    default:
                        continue;  // Not a text effect.
                }
                // T-345 (05 §5.9.9): radius and offset are px, in the element's `ui-scale` factor.
                effectCmd.effectRadius *= style.uiScaleEff;
                offset = Vec2{offset.x * style.uiScaleEff, offset.y * style.uiScaleEff};
                effectCmd.effectRadius = std::clamp(effectCmd.effectRadius, 0.0f, 8.0f);
                Color fxColor = fx.colors[0];
                fxColor.a *= alpha;
                if (fxColor.a <= 0.0f) {
                    continue;
                }
                effectCmd.color = fxColor;
                effectCmd.xf = mul(world, mul(translation(offset), local));
                uint32_t index = static_cast<uint32_t>(out.glyphRuns.size());
                out.glyphRuns.push_back(effectCmd);
                out.commands.push_back(Command{CmdKind::DrawGlyphs, {}, index});
            }
        }

        uint32_t index = static_cast<uint32_t>(out.glyphRuns.size());
        out.glyphRuns.push_back(cmd);
        out.commands.push_back(Command{CmdKind::DrawGlyphs, {}, index});

        // T-324: the decorations, over the glyphs.
        emitRunDecorations(text, runIndex, style, overrides, size, alpha, world, out);
    }

    // Inline objects (images and resolved `<glyph>`s).
    for (const TextStyleRun& run : text.styleRuns) {
        if (run.inlineNode == 0 || run.inlineNode > text.inlines.size() || run.start >= limit) {
            continue;
        }
        const TextInline& object = text.inlines[run.inlineNode - 1];
        if (object.asset == kInvalidAsset || object.box.w <= 0.0f || object.box.h <= 0.0f || alpha <= 0.0f) {
            continue;
        }
        ImageCmd ic{};
        ic.xf = world;
        ic.asset = object.asset;
        ic.dst = object.box;
        ic.srcUv = Rect{0, 0, 1, 1};
        ic.opacity = alpha;
        ic.smooth = 1;
        ic.variantScale = style.uiScaleEff;  // T-345
        uint32_t idx = static_cast<uint32_t>(out.images.size());
        out.images.push_back(ic);
        out.commands.push_back(Command{CmdKind::DrawImage, {}, idx});
    }
}

void emitTextGlyphs(const TextNodeData& text, const ComputedStyle& style, const Mat2x3& world, float alpha,
                    CommandList& out) {
    emitTextGlyphs(text, style, world, alpha, nullptr, out);
}

void emitPlaceholderGlyphs(const TextNodeData& text, const ComputedStyle& style, Color color,
                           const Mat2x3& world, float alpha, CommandList& out) {
    // T-395 (08 §8.16): plain glyphs in one color -- no text effects, decorations or reveal.
    color.a *= alpha;
    if (color.a <= 0.0f) {
        return;
    }
    const float baseSize = style.base[prop::FontSize].len.value;
    for (const GlyphRun& run : text.placeholderRuns) {
        if (run.font == kInvalidFont || run.glyphs.empty()) {
            continue;
        }
        GlyphsCmd cmd{};
        Mat2x3 local{};
        Mat2x3 unrotate{};
        const bool rotated = run.rotated != 0;
        if (rotated) {  // As emitTextGlyphs(): a sideways run turns about its own bounds' center.
            Vec2 center{run.bounds.x + run.bounds.w * 0.5f, run.bounds.y + run.bounds.h * 0.5f};
            Vec2 back{-center.x, -center.y};
            local = mul(translation(center), mul(rotationDeg(90), translation(back)));
            unrotate = mul(translation(center), mul(rotationDeg(-90), translation(back)));
        }
        cmd.xf = mul(world, local);
        cmd.font = run.font;
        cmd.size = run.fontSize > 0.0f ? run.fontSize : baseSize;
        cmd.color = color;
        cmd.rendering = style.textRendering;
        cmd.glyphOffset = static_cast<uint32_t>(out.glyphs.size());
        for (size_t i = 0; i < run.glyphs.size(); ++i) {
            out.glyphs.push_back(run.glyphs[i]);
            out.glyphPos.push_back(rotated ? apply(unrotate, run.positions[i]) : run.positions[i]);
        }
        cmd.glyphCount = static_cast<uint32_t>(run.glyphs.size());
        uint32_t index = static_cast<uint32_t>(out.glyphRuns.size());
        out.glyphRuns.push_back(cmd);
        out.commands.push_back(Command{CmdKind::DrawGlyphs, {}, index});
    }
}

uint32_t textDrawableCount(const TextNodeData& text, const ComputedStyle& style, const IrView* ir) {
    uint32_t count = 0;
    for (const GlyphRun& run : text.placeholderRuns) {  // T-395: one DrawGlyphs each.
        if (run.font != kInvalidFont && !run.glyphs.empty()) {
            ++count;
        }
    }
    TextRunStyle runStyle;
    for (const GlyphRun& run : text.runs) {
        if (run.font == kInvalidFont || run.glyphs.empty()) {
            continue;
        }
        uint32_t effects = style.textEffectCount;
        const TextRunStyle* overrides = nullptr;
        if (ir != nullptr && run.tag < text.styleRuns.size()) {
            resolveTextRunStyle(*ir, style, 0.0f, &text.styleRuns[run.tag], runStyle);
            effects = runStyle.effectCount;
            overrides = &runStyle;
        }
        count += 1 + effects;
        // T-324: one FillPath per decoration line of a horizontal run.
        DecorationPaint paint[3];
        const uint8_t lines = decorationsOf(style, overrides, paint);
        if (run.vertical == 0 && run.rotated == 0) {
            count += static_cast<uint32_t>((lines & 1u) + ((lines >> 1) & 1u) + ((lines >> 2) & 1u));
        }
    }
    for (const TextInline& object : text.inlines) {
        if (object.asset != kInvalidAsset && object.box.w > 0.0f) {
            count += 1;
        }
    }
    return count;
}

}  // namespace xt
