#include "text_runs.h"

#include <span>

#include "style_decode.h"
#include "xt/ir_pod.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// See text_runs.h. Only the property subset 07 §7.8 lists is honored; any other declaration the
/// run table carries is ignored (the compiler already drops them with a warning).

namespace xt {

namespace {

/// Px/Em/Percent only; any other unit (a run override has no layout box or device context) makes
/// the caller keep the base value. T-345: a px value is multiplied by `pxScale` (the element's
/// `ui-scale` factor, 05 §5.9.9); em / % are relative to the already scaled base.
bool lengthPx(const IrDecl& decl, float emBase, float percentBase, float pxScale, float& out) {
    const float value = irValueF32(decl.value, 0);
    switch (static_cast<LengthUnit>(decl.value[4])) {
        case LengthUnit::Px:
            out = value * pxScale;
            return true;
        case LengthUnit::Em:
            out = value * emBase;
            return true;
        case LengthUnit::Percent:
            out = value * percentBase / 100.0f;
            return true;
        default:
            return false;
    }
}

}  // namespace

void resolveTextRunStyle(const IrView& ir, const ComputedStyle& base, float baseLetterSpacing,
                         const TextStyleRun* run, TextRunStyle& out) {
    out.color = base.base[prop::Color].c;
    out.fontWeight = base.fontWeight;
    out.italic = base.fontStyle == FontStyle::Italic;
    out.fontSize = base.base[prop::FontSize].len.value;
    out.letterSpacing = baseLetterSpacing;
    out.effectOffset = base.textEffectOffset;
    out.effectCount = base.textEffectCount;
    out.hasColor = false;
    out.decorationLine = 0;
    out.hasDecorationColor = false;
    out.features = base.fontVariantNumeric == 1 ? kShapeFeatureTnum : 0;  // T-331.
    if (run == nullptr || run->declCount == 0) {
        return;
    }
    std::span<const IrDecl> decls = ir.doc().inlineDecls();
    if (run->declOffset > decls.size() || run->declCount > decls.size() - run->declOffset) {
        return;
    }
    decls = decls.subspan(run->declOffset, run->declCount);
    const float elementSize = out.fontSize;
    // font-size first: a run's `letter-spacing: 0.1em` is relative to the run's own size.
    for (const IrDecl& decl : decls) {
        if (decl.prop == prop::FontSize && decl.valueType == static_cast<uint8_t>(ValueType::Length)) {
            float px = 0;
            if (lengthPx(decl, elementSize, elementSize, base.uiScaleEff, px) && px > 0.0f) {
                out.fontSize = px;
            }
        }
    }
    for (const IrDecl& decl : decls) {
        switch (decl.prop) {
            case prop::Color:
                if (decl.valueType == static_cast<uint8_t>(ValueType::Color)) {
                    out.color = Color{irValueF32(decl.value, 0), irValueF32(decl.value, 1),
                                      irValueF32(decl.value, 2), irValueF32(decl.value, 3)};
                    out.hasColor = true;
                }
                break;
            case prop::LetterSpacing:
                if (decl.valueType == static_cast<uint8_t>(ValueType::Length)) {
                    float px = 0;
                    if (lengthPx(decl, out.fontSize, out.fontSize, base.uiScaleEff, px)) {
                        out.letterSpacing = px;
                    }
                }
                break;
            case style_decode::styleprop::FontWeight:
                out.fontWeight = static_cast<uint16_t>(irValueU32(decl.value, 0));
                break;
            case style_decode::styleprop::FontStyle:
                out.italic = irValueU32(decl.value, 0) == 1;
                break;
            case style_decode::styleprop::TextEffect:
                out.effectOffset = irValueU32(decl.value, 0);
                out.effectCount = irValueU32(decl.value, 1);
                break;
            case style_decode::styleprop::TextDecorationLine:  // T-324: united, see text_runs.h.
                out.decorationLine = static_cast<uint8_t>(out.decorationLine |
                                                          (irValueU32(decl.value, 0) & kTextDecorationAll));
                break;
            case style_decode::styleprop::FontVariantNumeric:  // T-331 (05 §5.9.8).
                out.features = irValueU32(decl.value, 0) == 1 ? kShapeFeatureTnum : 0;
                break;
            case prop::TextDecorationColor:
                if (decl.valueType == static_cast<uint8_t>(ValueType::Color)) {
                    out.decorationColor = Color{irValueF32(decl.value, 0), irValueF32(decl.value, 1),
                                                irValueF32(decl.value, 2), irValueF32(decl.value, 3)};
                    out.hasDecorationColor = true;
                }
                break;
            default:
                break;
        }
    }
}

}  // namespace xt
