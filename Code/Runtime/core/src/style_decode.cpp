#include "style_decode.h"

#include "xt/layout.h"
#include "xt/property.h"

namespace xt::style_decode {

bool isInherited(uint16_t prop) {
    switch (prop) {
        // Animatable, inherited (05.4 "可動畫屬性" table).
        case prop::FontSize:
        case prop::LineHeight:
        case prop::LetterSpacing:
        case prop::Color:
            return true;
        // Non-animatable, inherited (05.4 main table).
        case styleprop::Visibility:
        case styleprop::PointerEvents:
        case styleprop::PointerWatch:  // T-174 (27.2.5).
        case styleprop::FocusNav:      // T-194 (08.9).
        case styleprop::FontFamily:
        case styleprop::FontWeight:
        case styleprop::FontStyle:
        case styleprop::TextAlign:
        case styleprop::WhiteSpace:
        case styleprop::TextTransform:       // T-323 (05 §5.9.8).
        case styleprop::ImageRendering:      // T-327 (05 §5.9.8): as in CSS.
        case styleprop::FontVariantNumeric:  // T-331 (05 §5.9.8).
        case styleprop::UiScale:             // T-345 (05 §5.9.9).
        case styleprop::MediaOverride:       // T-345.
        case styleprop::AppRegion:           // T-376 (05 §5.9.10).
        case styleprop::Cursor:
        case styleprop::Direction:
        case styleprop::WritingMode:
        case styleprop::TextOrientation:
        case styleprop::TextRendering:
        case styleprop::Lang:
        case styleprop::TextEffect:  // T-178: like CSS `text-shadow`.
            return true;
        default:
            return false;
    }
}

void initNonAnimatableDefaults(ComputedStyle& style) {
    style.display = Display::Block;
    style.position = Position::Static;
    style.flexDir = FlexDirection::Row;
    style.flexWrap = FlexWrap::NoWrap;
    style.justify = Justify::Stretch;  // `normal` (T-338): flex-start in flex.
    style.alignItems = AlignItems::Stretch;
    style.alignSelf = AlignSelf::Auto;
    style.alignContent = AlignContent::Stretch;
    style.overflowX = Overflow::Visible;
    style.overflowY = Overflow::Visible;
    style.zIndex = 0;
    style.visibility = Visibility::Visible;
    style.pointerEvents = PointerEvents::Auto;
    style.fontFamilyOffset = 0;
    style.fontFamilyCount = 0;  // Compiler always emits at least "sans-serif"; empty here just
                                // means "no rule matched" (StyleEngine falls back to the IR's
                                // font-family pool default only via an actual `font-family` decl).
    style.fontWeight = 400;
    style.fontStyle = FontStyle::Normal;
    style.textAlign = TextAlign::Start;
    style.whiteSpace = WhiteSpace::Normal;
    style.textOverflow = TextOverflow::Clip;
    style.textTransform = TextTransform::None;  // T-323 (05 §5.9.8).
    style.cursor = Cursor::Default;
    style.objectFit = ObjectFit::Contain;
    style.direction = TextDirection::Ltr;
    style.writingMode = WritingMode::HorizontalTb;
    style.textOrientation = TextOrientation::Mixed;
    style.textRendering = TextRendering::Auto;
    style.lang = kInvalidString;
    style.backfaceHidden = 0;  // `visible` (05.4/19.10, T-47a).
    style.pointerWatch = 0;    // `false` (27.2.5, T-174).
    style.focusNav = 0;        // `auto` (08.9, T-194).
    style.layerOverlay = 0;    // `normal` (08.9, T-194).
    style.transitionOffset = 0;
    style.transitionCount = 0;
    style.animationOffset = 0;
    style.animationCount = 0;
    style.effectOffset = 0;  // `effect: none` (18-effects.md §18.4, T-51b's initial value).
    style.effectCount = 0;
    style.backgroundImage = kInvalidAsset;
    style.borderImage = kInvalidAsset;  // T-199 (26.3.4): `border-image-source: none`.
    for (float& slice : style.borderImageSlice) {
        slice = kAutoSlice;
    }
    style.borderImageRepeat = 0;  // stretch
    style.borderImageFill = 0;
    style.outlineStyle = 0;                    // `none` (05 §5.9.8, T-325).
    for (uint8_t& side : style.borderStyle) {  // `solid` (05 §5.9.8, T-333).
        side = kBorderStyleSolid;
    }
    style.borderColorSides = 0;                         // Every side follows `border-color` (T-333).
    style.textDecorationLine = 0;                       // `none` (05 §5.9.8, T-324).
    style.mixBlendMode = 0;                             // `normal` (18 §18.12, T-326).
    style.backgroundRepeat = kBackgroundRepeatStretch;  // T-327 (05 §5.9.8).
    style.imageRendering = 0;                           // `auto` (T-327).
    style.clipShape = 0;                                // `clip-path: none` (05 §5.9.8, T-332).
    style.lineClamp = 0;                                // `none` (T-331).
    style.fontVariantNumeric = 0;                       // `normal` (T-331).
    style.gridColumns = 0;                              // `none` (06 §6.15, T-338).
    style.gridRows = 0;
    style.gridAreas = 0;
    style.justifyItems = AlignItems::Stretch;
    for (uint32_t& line : style.gridLines) {
        line = kGridLineAuto;
    }
    style.gridAreaName = kInvalidString;  // `auto`.
    style.uiScaleMode = kUiScaleAuto;     // T-345 (05 §5.9.9).
    style.uiScaleMin = 0;
    style.uiScaleMax = 0;
    style.uiScaleEff = 1;
    style.mediaOverride = kMediaOverrideAuto;
    style.appRegion = AppRegion::None;   // T-376 (05 §5.9.10).
    for (float& side : style.hitSlop) {  // T-386 (05 §5.9.11).
        side = 0;
    }
    style.clipStart = 0;  // T-191 (28.2).
    style.clipRate = 1;
}

bool applyEnumDecl(ComputedStyle& style, uint16_t prop, uint32_t enumValue) {
    switch (prop) {
        case styleprop::Display:
            style.display = static_cast<Display>(enumValue);
            return true;
        case styleprop::Position:
            style.position = static_cast<Position>(enumValue);
            return true;
        case styleprop::FlexDirection:
            style.flexDir = static_cast<FlexDirection>(enumValue);
            return true;
        case styleprop::FlexWrap:  // T-336: 2 = wrap-reverse.
            style.flexWrap = enumValue <= 2 ? static_cast<FlexWrap>(enumValue) : FlexWrap::NoWrap;
            return true;
        case styleprop::JustifyContent:
            style.justify = static_cast<Justify>(enumValue);
            return true;
        case styleprop::AlignItems:
            style.alignItems = static_cast<AlignItems>(enumValue);
            return true;
        case styleprop::AlignSelf:
            style.alignSelf = static_cast<AlignSelf>(enumValue);
            return true;
        case styleprop::AlignContent:
            style.alignContent = static_cast<AlignContent>(enumValue);
            return true;
        case styleprop::Overflow: {  // T-336: the shorthand sets both axes.
            const Overflow v = enumValue <= 3 ? static_cast<Overflow>(enumValue) : Overflow::Visible;
            style.overflowX = v;
            style.overflowY = v;
            return true;
        }
        case styleprop::OverflowX:  // T-336 (05 §5.9.8).
            style.overflowX = enumValue <= 3 ? static_cast<Overflow>(enumValue) : Overflow::Visible;
            return true;
        case styleprop::OverflowY:
            style.overflowY = enumValue <= 3 ? static_cast<Overflow>(enumValue) : Overflow::Visible;
            return true;
        case styleprop::Visibility:
            style.visibility = static_cast<Visibility>(enumValue);
            return true;
        case styleprop::PointerEvents:
            style.pointerEvents = static_cast<PointerEvents>(enumValue);
            return true;
        case styleprop::FontWeight:
            style.fontWeight = static_cast<uint16_t>(enumValue);
            return true;
        case styleprop::FontStyle:
            style.fontStyle = static_cast<FontStyle>(enumValue);
            return true;
        case styleprop::TextAlign:
            style.textAlign = static_cast<TextAlign>(enumValue);
            return true;
        case styleprop::WhiteSpace:
            style.whiteSpace = static_cast<WhiteSpace>(enumValue);
            return true;
        case styleprop::TextOverflow:
            style.textOverflow = static_cast<TextOverflow>(enumValue);
            return true;
        case styleprop::TextTransform:  // T-323 (05 §5.9.8): 0 none .. 3 capitalize.
            style.textTransform = static_cast<TextTransform>(enumValue <= 3 ? enumValue : 0);
            return true;
        case styleprop::Cursor:
            style.cursor = static_cast<Cursor>(enumValue);
            return true;
        case styleprop::ObjectFit:
            style.objectFit = static_cast<ObjectFit>(enumValue);
            return true;
        case styleprop::Direction:
            style.direction = static_cast<TextDirection>(enumValue);
            return true;
        case styleprop::WritingMode:
            style.writingMode = static_cast<WritingMode>(enumValue);
            return true;
        case styleprop::TextOrientation:
            style.textOrientation = static_cast<TextOrientation>(enumValue);
            return true;
        case styleprop::TextRendering:
            style.textRendering = static_cast<TextRendering>(enumValue);
            return true;
        case styleprop::BackfaceVisibility:
            style.backfaceHidden = static_cast<uint8_t>(enumValue);  // 0 = visible, 1 = hidden.
            return true;
        case styleprop::PointerWatch:
            style.pointerWatch = static_cast<uint8_t>(enumValue != 0 ? 1 : 0);
            return true;
        case styleprop::FocusNav:
            style.focusNav = static_cast<uint8_t>(enumValue <= 2 ? enumValue : 0);
            return true;
        case styleprop::Layer:
            style.layerOverlay = static_cast<uint8_t>(enumValue != 0 ? 1 : 0);
            return true;
        case styleprop::BorderImageRepeat:  // T-199 (26.3.4).
            style.borderImageRepeat = static_cast<uint8_t>(enumValue <= 2 ? enumValue : 0);
            return true;
        case styleprop::BorderImageFill:
            style.borderImageFill = static_cast<uint8_t>(enumValue != 0 ? 1 : 0);
            return true;
        case styleprop::BorderTopStyle:  // T-333 (05 §5.9.8): kBorderStyle*.
        case styleprop::BorderRightStyle:
        case styleprop::BorderBottomStyle:
        case styleprop::BorderLeftStyle:
            style.borderStyle[prop - styleprop::BorderTopStyle] =
                static_cast<uint8_t>(enumValue <= kBorderStyleDotted ? enumValue : kBorderStyleSolid);
            return true;
        case styleprop::OutlineStyle:  // T-325 (05 §5.9.8): 0 none, 1 solid.
            style.outlineStyle = static_cast<uint8_t>(enumValue == 1 ? 1 : 0);
            return true;
        case styleprop::TextDecorationLine:  // T-324 (05 §5.9.8): kTextDecoration* bits.
            style.textDecorationLine = static_cast<uint8_t>(enumValue & kTextDecorationAll);
            return true;
        case styleprop::MixBlendMode:  // T-326 (18 §18.12): a BlendMode, 0 normal .. 16 add.
            style.mixBlendMode =
                static_cast<uint8_t>(enumValue <= static_cast<uint32_t>(BlendMode::Add) ? enumValue : 0);
            return true;
        case styleprop::BackgroundRepeat:  // T-327 (05 §5.9.8): kBackgroundRepeat*.
            style.backgroundRepeat =
                static_cast<uint8_t>(enumValue <= kBackgroundRepeatY ? enumValue : kBackgroundRepeatStretch);
            return true;
        case styleprop::ImageRendering:  // T-327: 0 auto, 1 pixelated.
            style.imageRendering = static_cast<uint8_t>(enumValue == 1 ? 1 : 0);
            return true;
        case styleprop::ClipPath:  // T-332 (05 §5.9.8): 0 none, else a (tagged) shape pool index + 1.
            style.clipShape = enumValue;
            return true;
        case styleprop::LineClamp:  // T-331 (05 §5.9.8): the line count, 0 = none.
            style.lineClamp = enumValue;
            return true;
        case styleprop::FontVariantNumeric:  // T-331: 0 normal, 1 tabular-nums.
            style.fontVariantNumeric = static_cast<uint8_t>(enumValue == 1 ? 1 : 0);
            return true;
        case styleprop::MediaOverride:  // T-345 (05 §5.9.9): 0 auto, 1 show, 2 hide.
            style.mediaOverride = static_cast<uint8_t>(enumValue <= kMediaOverrideHide ? enumValue : 0);
            return true;
        case styleprop::AppRegion:  // T-376 (05 §5.9.10): 0 none, 1 drag, 2 no-drag.
            style.appRegion = enumValue <= 2 ? static_cast<AppRegion>(enumValue) : AppRegion::None;
            return true;
        case styleprop::GridTemplateColumns:  // T-338 (06 §6.15): 0 none, else 1 + a GRID list index.
            style.gridColumns = enumValue;
            return true;
        case styleprop::GridTemplateRows:
            style.gridRows = enumValue;
            return true;
        case styleprop::GridTemplateAreas:
            style.gridAreas = enumValue;
            return true;
        case styleprop::JustifyItems:  // T-338: AlignItems numbering.
            style.justifyItems = enumValue <= static_cast<uint32_t>(AlignItems::Center)
                                     ? static_cast<AlignItems>(enumValue)
                                     : AlignItems::Stretch;
            return true;
        case styleprop::GridColumnStart:  // T-338: kGridLine* (an invalid payload reads as auto).
        case styleprop::GridColumnEnd:
        case styleprop::GridRowStart:
        case styleprop::GridRowEnd:
            style.gridLines[prop - styleprop::GridColumnStart] =
                validGridLine(enumValue) ? enumValue : kGridLineAuto;
            return true;
        default:
            return false;
    }
}

Value initialGeneralValue(uint16_t prop) {
    switch (prop) {
        case prop::Opacity:
            return Value::ofFloat(1);
        case prop::TranslateX:
        case prop::TranslateY:
        case prop::Rotation:
        case prop::SkewX:
        case prop::SkewY:
            return Value::ofFloat(0);
        case prop::ScaleX:
        case prop::ScaleY:
            return Value::ofFloat(1);
        default:
            return Value::ofFloat(0);
    }
}

bool lengthToPx(const Length& len, float fontSizePx, float& outPx) {
    switch (len.unit) {
        case LengthUnit::Px:
            outPx = len.value;
            return true;
        case LengthUnit::Em:
            outPx = len.value * fontSizePx;
            return true;
        case LengthUnit::Percent:
        case LengthUnit::Auto:
            return false;
        case LengthUnit::Calc:
            // T-176: `em` inside the program resolves against fontSizePx; `%` fails as above.
            return evalCalc(currentLengthEnv(), calcProgramIndex(len.value), -1.0f, fontSizePx, outPx);
        default:
            // T-176: viewport / safe-area / uiscale units against the current LengthEnv.
            return lengthToPxEnv(currentLengthEnv(), len, -1.0f, outPx);
    }
}

}  // namespace xt::style_decode
