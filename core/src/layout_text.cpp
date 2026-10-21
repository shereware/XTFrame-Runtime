#include <algorithm>
#include <span>
#include <string_view>

#include "composition.h"
#include "text_case.h"
#include "text_edit.h"
#include "text_runs.h"
#include "xt/assert.h"
#include "xt/asset_resolver.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// Text node measurement/positioning (docs/design/06-layout.md §6.8, docs/design/07-text.md §7.5):
/// TextShaper::shape() -- behind TextNodeData's shaping cache -- plus layoutText() (text_layout.cpp),
/// writing TextNodeData::runs/lines and the node's own box. See docs/tasks/T-30.md.
///
/// Not covered by docs/tasks/T-30.md / §6.8 (conservative choices made here):
///   - `line-height` resolution accepts Em (the initial value, 1.2em, is the one Length that
///     reaches ComputedStyle un-converted), Percent (x font-size) and Px; `auto` falls back to
///     1.2 x font-size.
///   - The text node's own cross size is the laid-out cross size (lines x line-height); its main
///     size is the available main size when one was given (so that text-align positions and the
///     node's box agree), and the longest line otherwise.
///   - A text node whose parent has no usable font (kInvalidFont) or whose shaper is null is laid
///     out as 0x0 with no runs, per docs/design/07-text.md §7.2's "layout treats the text as
///     0-size".

namespace xt {

namespace {

/// T-179: BCP 47 prefix match, ASCII case-insensitive (`zh` matches `zh` and `zh-TW`); the same
/// rule `:lang()` uses (selector_match.h). An empty `lang` matches nothing.
bool langTagMatches(std::string_view lang, std::string_view tag) {
    if (tag.empty() || lang.size() < tag.size()) {
        return false;
    }
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (size_t i = 0; i < tag.size(); ++i) {
        if (lower(lang[i]) != lower(tag[i])) {
            return false;
        }
    }
    return lang.size() == tag.size() || lang[tag.size()] == '-';
}

/// §6.8's `resolve(lineHeight, fontSize)`. Unlike resolveLength() this accepts Em, which is the
/// initial `line-height` value's unit (1.2em) and is never converted to px by style resolution.
float resolveLineHeight(Length len, float fontSize) {
    switch (len.unit) {
        case LengthUnit::Px:
            return len.value;
        case LengthUnit::Em:
            return len.value * fontSize;
        case LengthUnit::Percent:
            return fontSize * len.value / 100.0f;
        case LengthUnit::Auto:
            break;
        default: {
            // T-176: device units and `calc()` (a `%` inside one is of the font size).
            float px = 0;
            if (lengthToPxEnv(currentLengthEnv(), len, fontSize, px)) {
                return px;
            }
            break;
        }
    }
    return 1.2f * fontSize;
}

/// `letter-spacing`: Px passes through, Em/Percent resolve against the font size, Auto is 0.
float resolveLetterSpacing(Length len, float fontSize) {
    switch (len.unit) {
        case LengthUnit::Px:
            return len.value;
        case LengthUnit::Em:
            return len.value * fontSize;
        case LengthUnit::Percent:
            return fontSize * len.value / 100.0f;
        case LengthUnit::Auto:
            break;
        default: {
            float px = 0;  // T-176: device units and `calc()`.
            if (lengthToPxEnv(currentLengthEnv(), len, fontSize, px)) {
                return px;
            }
            break;
        }
    }
    return 0.0f;
}

/// Appends `seg` (the shape() result of the substring at byte `offset`) to `out`, shifting every
/// byte offset by `offset`. breakAfter/graphemeStart are not touched (the caller fills them from
/// a whole-text shape).
void appendShaped(const ShapeResult& seg, uint32_t offset, ShapeResult& out) {
    const uint32_t glyphBase = static_cast<uint32_t>(out.glyphs.size());
    for (ShapedRun run : seg.runs) {
        run.byteStart += offset;
        run.byteEnd += offset;
        run.glyphStart += glyphBase;
        out.runs.push_back(run);
    }
    for (ShapedGlyph glyph : seg.glyphs) {
        glyph.cluster += offset;
        out.glyphs.push_back(glyph);
    }
}

/// T-178: the inline object's size, (width, height) in px. Height is the `height` attribute or
/// the run's font size; width keeps the image's aspect ratio (a square when the size is unknown).
Vec2 inlineObjectSize(const TextInline& object, float fontSize, AssetResolver* assets, const IrView& ir) {
    const float h = object.height > 0.0f ? object.height : fontSize;
    Vec2 native{0, 0};
    if (object.asset != kInvalidAsset && assets != nullptr && object.asset < ir.assets().assets().size()) {
        if (isExternalAsset(object.asset, ir)) {
            const AssetResolver::ExternalTexture* ext = assets->externalTexture(object.asset, ir);
            if (ext != nullptr) {
                native = Vec2{static_cast<float>(ext->width), static_cast<float>(ext->height)};
            }
        } else {
            native = assets->imageSize(object.asset, ir);
        }
    }
    const float w = native.y > 0.0f ? h * native.x / native.y : h;
    return Vec2{w, h};
}

}  // namespace

void LayoutEngine::layoutTextNode(NodeArena& arena, NodeId textNode, const ComputedStyle& style,
                                  float availMain) {
    XT_ASSERT(m_ir != nullptr);
    TextNodeData& data = arena.text(textNode);
    data.runs.clear();
    data.lines.clear();
    data.width = 0;
    data.height = 0;
    data.intrinsicMain = 0;
    data.placeholderRuns.clear();

    // T-33/T-33b: the display string's source, in priority order (docs/tasks/T-33b.md
    // "顯示字串來源優先序"): an active composition on this element, else the EditState value
    // of an editable `input`, else the IR text.
    bool editable = data.editIndex != kInvalidEditIndex && m_editStates != nullptr &&
                    data.editIndex < m_editStates->size();
    std::string_view text;
    // T-395 (08 §8.16): the `placeholder` attribute of an editable input whose value is empty and
    // which is not composing (any input hides it). An `@key` placeholder is already the localized
    // string here (Document::setLocalization rewrites the attribute).
    StringId placeholderId = kInvalidString;
    if (editable) {
        const NodeId element = arena.parent(textNode);
        bool composing =
            m_composition != nullptr && m_composition->active && m_composition->element == element;
        text = composing ? std::string_view(m_composition->display)
                         : std::string_view((*m_editStates)[data.editIndex].value);
        if (!composing && text.empty() && element != kInvalidNode &&
            arena.kind(element) == NodeKind::Element) {
            for (const auto& attr : arena.element(element).attributes) {
                if (string(attr.first) == "placeholder") {
                    // T-397 (08 §8.16): an unresolved `@key` placeholder draws nothing.
                    const bool unresolved = m_unresolvedPlaceholders != nullptr &&
                                            std::binary_search(m_unresolvedPlaceholders->begin(),
                                                               m_unresolvedPlaceholders->end(), attr.second);
                    if (!string(attr.second).empty() && !unresolved) {
                        placeholderId = attr.second;
                    }
                    break;
                }
            }
        }
    } else if (data.text != kInvalidString) {
        text = string(data.text);
    }

    const float fontSize = style.base[prop::FontSize].len.value;
    const float lineHeight = resolveLineHeight(style.base[prop::LineHeight].len, fontSize);
    const float letterSpacing = resolveLetterSpacing(style.base[prop::LetterSpacing].len, fontSize);

    if (m_shaper == nullptr || style.resolvedFont == kInvalidFont || text.empty()) {
        if (editable) {
            // An empty editable input still occupies exactly one line, so that its box keeps the
            // element's height and the caret has a line rect to sit on (T-33's Painter).
            float ascent = fontSize * 0.8f;
            if (m_shaper != nullptr && style.resolvedFont != kInvalidFont) {
                ascent = m_shaper->metrics(style.resolvedFont).ascent * fontSize;
            }
            TextLine line;
            line.rect = Rect{0, 0, 0, lineHeight};
            line.baseline = ascent;
            line.firstRun = 0;
            line.runCount = 0;
            line.byteStart = 0;
            line.byteEnd = 0;
            data.lines.push_back(line);
            data.width = availMain < 0.0f ? 0.0f : availMain;
            data.height = lineHeight;
        }
        if (placeholderId == kInvalidString || m_shaper == nullptr || style.resolvedFont == kInvalidFont) {
            return;
        }
        // T-395: an empty input with a placeholder goes on to shape and lay out the placeholder
        // below; the node's own line, box and intrinsic size stay the empty input's.
    }
    const StringId lang = style.lang;
    // T-178 (07 §7.8): rich text shapes run by run; an editable input's text is never rich.
    const bool rich = !editable && !data.styleRuns.empty();
    // T-323 (05 §5.9.8): `text-transform` applies to the whole text node (the run overrides do not
    // carry it), after `@key` substitution (`data.text` is already the localized string). An
    // editable input is the edit value itself -- display and editing share this path -- so it is
    // never transformed (the value stays as typed, as in a browser).
    const TextTransform transform = editable ? TextTransform::None : style.textTransform;
    // T-331 (05 §5.9.8): `font-variant-numeric` as OpenType features; part of the shaping key.
    const uint8_t features = style.fontVariantNumeric == 1 ? kShapeFeatureTnum : 0;

    // Shaping cache (docs/tasks/T-30.md goal 2): reshape only when a key field changed; a changed
    // `availMain` alone only re-runs layoutText() below.
    bool cacheHit = data.shapedValid != 0 && data.keyText == data.text &&
                    data.keyFont == style.resolvedFont && data.keySize == fontSize &&
                    data.keyLetterSpacing == letterSpacing && data.keyDirection == style.direction &&
                    data.keyWritingMode == style.writingMode &&
                    data.keyOrientation == style.textOrientation && data.keyLang == lang &&
                    data.keyTransform == transform && data.keyFeatures == features;

    m_familyScratch.clear();
    std::span<const uint32_t> familyPool = m_ir->style().fontFamilies();
    for (uint32_t i = 0; i < style.fontFamilyCount; ++i) {
        uint32_t index = style.fontFamilyOffset + i;
        if (index < familyPool.size()) {
            m_familyScratch.push_back(m_ir->string(familyPool[index]));
        }
    }

    // T-179 (07.8): the `@font-fallback` chain of the document language (else the element's own
    // `lang`) follows the element's families, so a glyph they lack is looked up there before the
    // shaper's global fallback list. An exact tag wins; otherwise the longest chain whose tag is a
    // BCP 47 prefix (`zh` for `zh-TW`).
    if (m_localization != nullptr && !m_localization->fallbacks().empty()) {
        std::string_view chainLang = m_language;
        if (chainLang.empty() && lang != kInvalidString) {
            chainLang = string(lang);
        }
        const IrFontFallback* best = nullptr;
        size_t bestLength = 0;
        for (const IrFontFallback& fb : m_localization->fallbacks()) {
            const std::string_view tag = m_ir->string(fb.lang);
            if (!langTagMatches(chainLang, tag)) {
                continue;
            }
            if (best == nullptr || tag.size() > bestLength) {
                best = &fb;
                bestLength = tag.size();
            }
        }
        if (best != nullptr) {
            std::span<const uint32_t> families = m_localization->fallbackFamilies();
            for (uint32_t i = 0; i < best->familyCount; ++i) {
                m_familyScratch.push_back(m_ir->string(families[best->familyOffset + i]));
            }
        }
    }

    ShapeParams params;
    params.font.families = std::span<const std::string_view>(m_familyScratch);
    params.font.weight = style.fontWeight;
    params.font.italic = style.fontStyle == FontStyle::Italic;
    params.size = fontSize;
    params.letterSpacing = letterSpacing;
    params.paragraphDirection = style.direction;
    params.writingMode = style.writingMode;
    params.orientation = style.textOrientation;
    params.lang = lang == kInvalidString ? std::string_view{} : m_ir->string(lang);
    params.features = features;

    if (placeholderId != kInvalidString) {
        // T-395 (08 §8.16): the placeholder's own shaping cache; `shaped` keeps the value's.
        const bool placeholderHit =
            data.placeholderShapedValid != 0 && data.placeholderKeyText == placeholderId &&
            data.placeholderKeyFont == style.resolvedFont && data.placeholderKeySize == fontSize &&
            data.placeholderKeyLetterSpacing == letterSpacing &&
            data.placeholderKeyDirection == style.direction &&
            data.placeholderKeyWritingMode == style.writingMode &&
            data.placeholderKeyOrientation == style.textOrientation && data.placeholderKeyLang == lang &&
            data.placeholderKeyFeatures == features;
        if (!placeholderHit) {
            m_shaper->shape(string(placeholderId), params, data.placeholderShaped);
            data.placeholderShapedValid = 1;
            data.placeholderKeyText = placeholderId;
            data.placeholderKeyFont = style.resolvedFont;
            data.placeholderKeySize = fontSize;
            data.placeholderKeyLetterSpacing = letterSpacing;
            data.placeholderKeyDirection = style.direction;
            data.placeholderKeyWritingMode = style.writingMode;
            data.placeholderKeyOrientation = style.textOrientation;
            data.placeholderKeyLang = lang;
            data.placeholderKeyFeatures = features;
        }
    } else if (!cacheHit) {
        if (transform != TextTransform::None) {
            // Converted before shaping, only on a cache miss. The case tables keep every code
            // point's UTF-8 length, so the style runs' byte offsets stay valid as they are.
            text_case::apply(text, transform, m_caseScratch);
            XT_ASSERT(m_caseScratch.size() == text.size());
            text = m_caseScratch;
        }
        m_shaper->shape(text, params, data.shaped);
        data.shapedRunInfo.clear();
        if (rich) {
            shapeRichText(data, text, params, style, lineHeight);
        }

        // Ellipsis metrics for this (font, size) pair: one extra shape() of U+2026 (§6.8's
        // text-overflow rule needs its advance before it knows whether it will be used).
        data.ellipsisAdvance = 0;
        data.ellipsisGlyph = 0;
        data.ellipsisFont = kInvalidFont;
        ShapeResult ellipsis;
        m_shaper->shape(std::string_view("\xE2\x80\xA6"), params, ellipsis);
        if (!ellipsis.glyphs.empty() && !ellipsis.runs.empty()) {
            const ShapedGlyph& glyph = ellipsis.glyphs[0];
            data.ellipsisAdvance =
                style.writingMode == WritingMode::HorizontalTb || ellipsis.runs[0].vertical == 0
                    ? glyph.xAdvance
                    : glyph.yAdvance;
            data.ellipsisGlyph = glyph.glyphId;
            data.ellipsisFont = ellipsis.runs[0].font;
        }

        data.shapedValid = 1;
        data.keyText = data.text;
        data.keyFont = style.resolvedFont;
        data.keySize = fontSize;
        data.keyLetterSpacing = letterSpacing;
        data.keyDirection = style.direction;
        data.keyWritingMode = style.writingMode;
        data.keyOrientation = style.textOrientation;
        data.keyLang = lang;
        data.keyTransform = transform;
        data.keyFeatures = features;
    }

    FontMetrics metrics = m_shaper->metrics(style.resolvedFont);

    TextLayoutParams layoutParams;
    // An undefined available main size (shrink-to-fit / intrinsic measurement) means "no soft
    // wrapping": layoutText() only breaks a line when it overflows `availMain`, so a huge value
    // is equivalent to unbounded here.
    layoutParams.availMain = availMain < 0.0f ? 1.0e30f : availMain;
    layoutParams.fontSize = fontSize;
    layoutParams.lineHeight = lineHeight;
    layoutParams.ascent = metrics.ascent * fontSize;
    layoutParams.descent = metrics.descent * fontSize;
    layoutParams.textAlign = style.textAlign;
    layoutParams.whiteSpace = style.whiteSpace;
    layoutParams.textOverflow = style.textOverflow;
    // T-331 (05 §5.9.8): `line-clamp` only with soft wrapping; `nowrap` + `text-overflow:
    // ellipsis` wins and counts as one line, any other `nowrap` ignores it.
    if (style.whiteSpace != WhiteSpace::NoWrap) {
        layoutParams.lineClamp = style.lineClamp;
    } else if (style.lineClamp != 0 && style.textOverflow == TextOverflow::Ellipsis) {
        layoutParams.lineClamp = 1;
    }
    layoutParams.direction = style.direction;
    layoutParams.writingMode = style.writingMode;
    layoutParams.ellipsisAdvance = data.ellipsisAdvance;
    layoutParams.ellipsisGlyph = data.ellipsisGlyph;
    layoutParams.ellipsisFont = data.ellipsisFont;
    if (rich) {
        layoutParams.runInfo = std::span<const TextRunInfo>(data.shapedRunInfo);
    }

    if (placeholderId != kInvalidString) {
        // T-395: same size, line height and alignment as the value; no ellipsis glyph (that is
        // shaped with the value only). An undefined available size (intrinsic measurement) lays
        // it out from the line start, so no alignment offset depends on the unbounded width.
        layoutParams.ellipsisAdvance = 0;
        layoutParams.ellipsisGlyph = 0;
        layoutParams.ellipsisFont = kInvalidFont;
        if (availMain < 0.0f) {
            layoutParams.textAlign = TextAlign::Start;
        }
        layoutText(data.placeholderShaped, layoutParams, m_textScratch);
        data.placeholderRuns = std::move(m_textScratch.runs);
        m_textScratch.runs.clear();
        m_textScratch.lines.clear();
        return;
    }

    layoutText(data.shaped, layoutParams, m_textScratch);

    data.runs = std::move(m_textScratch.runs);
    data.lines = std::move(m_textScratch.lines);
    m_textScratch.runs.clear();
    m_textScratch.lines.clear();
    data.intrinsicMain = m_textScratch.intrinsicMain;
    if (rich) {
        placeInlineObjects(data, style.writingMode);
    }

    float mainSize = availMain < 0.0f ? m_textScratch.mainSize : availMain;
    if (style.writingMode == WritingMode::HorizontalTb) {
        data.width = mainSize;
        data.height = m_textScratch.crossSize;
    } else {
        data.width = m_textScratch.crossSize;
        data.height = mainSize;
    }
}

void LayoutEngine::shapeRichText(TextNodeData& data, std::string_view text, const ShapeParams& baseParams,
                                 const ComputedStyle& style, float baseLineHeight) {
    // `data.shaped` holds the whole text shaped with the element's style: its per-byte
    // breakAfter/graphemeStart stay (line breaking and `text-reveal` work across runs); its runs
    // and glyphs are rebuilt segment by segment, each segment shaped with its own run style.
    ShapeResult& out = data.shaped;
    out.runs.clear();
    out.glyphs.clear();
    const uint32_t length = static_cast<uint32_t>(text.size());
    const uint8_t baseLevel = style.direction == TextDirection::Rtl ? 1 : 0;
    const Length lineHeightDecl = style.base[prop::LineHeight].len;
    const float baseSize = baseParams.size;

    ShapeResult segment;
    std::string fallbackText;
    auto pushSegment = [&](uint32_t start, uint32_t end, uint32_t runIndex) {
        const TextStyleRun* run = runIndex == kNoRunTag ? nullptr : &data.styleRuns[runIndex];
        TextRunStyle rs;
        resolveTextRunStyle(*m_ir, style, baseParams.letterSpacing, run, rs);
        ShapeParams params = baseParams;
        params.font.weight = rs.fontWeight;
        params.font.italic = rs.italic;
        params.size = rs.fontSize;
        params.letterSpacing = rs.letterSpacing;
        params.features = rs.features;  // T-331: the run's `font-variant-numeric`.
        // Line metrics of the run's own face (07.3's match(), like the element's resolvedFont).
        const FontId face =
            rs.fontWeight == style.fontWeight && rs.italic == (style.fontStyle == FontStyle::Italic)
                ? style.resolvedFont
                : m_shaper->match(params.font);
        FontMetrics metrics = m_shaper->metrics(face == kInvalidFont ? style.resolvedFont : face);
        TextRunInfo info;
        info.ascent = metrics.ascent * rs.fontSize;
        info.descent = metrics.descent * rs.fontSize;
        info.lineHeight =
            rs.fontSize == baseSize ? baseLineHeight : resolveLineHeight(lineHeightDecl, rs.fontSize);
        info.fontSize = rs.fontSize;
        info.tag = runIndex;

        const TextInline* object =
            run != nullptr && run->inlineNode != 0 && run->inlineNode <= data.inlines.size()
                ? &data.inlines[run->inlineNode - 1]
                : nullptr;
        const bool drawAsBox =
            object != nullptr && (object->kind == TextInlineKind::Image || object->asset != kInvalidAsset);
        if (drawAsBox) {
            // An inline-block: one placeholder glyph (no font) whose advance is the box width;
            // it sits on the baseline, so the box's height is all ascent.
            const Vec2 size = inlineObjectSize(*object, rs.fontSize, m_assets, *m_ir);
            ShapedRun sr;
            sr.font = kInvalidFont;
            sr.bidiLevel = baseLevel;
            sr.byteStart = start;
            sr.byteEnd = end;
            sr.glyphStart = static_cast<uint32_t>(out.glyphs.size());
            sr.glyphCount = 1;
            ShapedGlyph glyph;
            glyph.glyphId = 0;
            glyph.xAdvance = size.x;
            glyph.yAdvance = size.x;
            glyph.cluster = start;
            out.runs.push_back(sr);
            out.glyphs.push_back(glyph);
            info.ascent = std::max(info.ascent, size.y);
            info.lineHeight = std::max(info.lineHeight, size.y);
            data.shapedRunInfo.push_back(info);
            return;
        }
        if (object != nullptr) {
            // 13.8.3: an unresolved glyph (no provider, or no asset for the current device) is
            // drawn as the text `[key]`. Every glyph but the logically last one is tagged with
            // the placeholder's continuation byte, which never allows a break, so `[key]` stays
            // on one line; the last one carries the placeholder's own break opportunity.
            fallbackText.assign("[");
            fallbackText.append(string(object->action));
            fallbackText.append("]");
            segment.runs.clear();
            segment.glyphs.clear();
            m_shaper->shape(fallbackText, params, segment);
            const size_t runsBefore = out.runs.size();
            appendShaped(segment, 0, out);
            for (size_t r = runsBefore; r < out.runs.size(); ++r) {
                ShapedRun& sr = out.runs[r];
                const bool rtl = (sr.bidiLevel % 2) == 1;
                for (uint32_t g = 0; g < sr.glyphCount; ++g) {
                    const bool logicalLast =
                        r + 1 == out.runs.size() && (rtl ? g == 0 : g + 1 == sr.glyphCount);
                    out.glyphs[sr.glyphStart + g].cluster =
                        logicalLast || end - start < 2 ? start : start + 1;
                    out.glyphs[sr.glyphStart + g].isSpace = 0;
                }
                sr.byteStart = start;
                sr.byteEnd = end;
                data.shapedRunInfo.push_back(info);
            }
            return;
        }
        segment.runs.clear();
        segment.glyphs.clear();
        m_shaper->shape(text.substr(start, end - start), params, segment);
        appendShaped(segment, start, out);
        for (size_t r = 0; r < segment.runs.size(); ++r) {
            data.shapedRunInfo.push_back(info);
        }
    };

    uint32_t cursor = 0;
    for (uint32_t i = 0; i < data.styleRuns.size(); ++i) {
        const TextStyleRun& run = data.styleRuns[i];
        const uint32_t start = std::min(run.start, length);
        const uint32_t end = std::min(run.end, length);
        if (start < cursor || end <= start) {
            continue;  // Malformed (overlapping or empty): ignored.
        }
        if (start > cursor) {
            pushSegment(cursor, start, kNoRunTag);
        }
        pushSegment(start, end, i);
        cursor = end;
    }
    if (cursor < length) {
        pushSegment(cursor, length, kNoRunTag);
    }
}

void LayoutEngine::placeInlineObjects(TextNodeData& data, WritingMode writingMode) {
    for (TextInline& object : data.inlines) {
        object.box = Rect{};
    }
    for (const TextLine& line : data.lines) {
        for (uint32_t r = line.firstRun; r < line.firstRun + line.runCount && r < data.runs.size(); ++r) {
            const GlyphRun& run = data.runs[r];
            if (run.font != kInvalidFont || run.tag >= data.styleRuns.size() || run.positions.empty()) {
                continue;
            }
            const uint32_t objectIndex = data.styleRuns[run.tag].inlineNode;
            if (objectIndex == 0 || objectIndex > data.inlines.size()) {
                continue;
            }
            TextInline& object = data.inlines[objectIndex - 1];
            const float h = object.height > 0.0f ? object.height : run.fontSize;
            if (writingMode == WritingMode::HorizontalTb) {
                object.box = Rect{run.positions[0].x, line.baseline - h, run.bounds.w, h};
            } else {
                // Vertical writing modes: the object fills its cell of the column (the rich-text
                // design covers horizontal text; kept conservative).
                object.box = run.bounds;
            }
        }
    }
}

TextIntrinsic LayoutEngine::measureTextChildren(NodeArena& arena, NodeId element) {
    TextIntrinsic out{};
    if (arena.kind(element) != NodeKind::Element) {
        return out;
    }
    const ComputedStyle& style = arena.element(element).style;
    // NodeArena's traversal helpers are not re-entrant; layoutTextNode() runs no traversal of its
    // own, so a single forEachChild here is safe.
    arena.forEachChild(element, [&](NodeId child) {
        if (arena.kind(child) != NodeKind::Text) {
            return;
        }
        layoutTextNode(arena, child, style, kUndefinedRef);
        const TextNodeData& text = arena.text(child);
        out.main = std::max(out.main, text.intrinsicMain);
        out.width = std::max(out.width, text.width);
        out.height = std::max(out.height, text.height);
    });
    return out;
}

}  // namespace xt
