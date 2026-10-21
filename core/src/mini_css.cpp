#include "mini_css.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "css_number.h"  // The CSS number, shared with the compiler's lexer (T-272).
#include "css_values.h"  // The value grammar, shared with the compiler (T-413).
#include "style_decode.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/value_helpers.h"

/// Runtime mini CSS declaration-list parser (see mini_css.h): the tokenizer, the runtime's property
/// table and the Decl mapping. The value grammar itself is core/src/css_values.h, which the
/// compiler's `.xts` parser uses too (T-413).

namespace xt::mini_css {

namespace {

namespace sp = style_decode::styleprop;

// ---------------------------------------------------------------------------------------------
// Tokenizer (a cut-down copy of compile/src/xts/xts_lexer.cpp: no braces/at-rules/selector
// punctuation, and every token's text is a view into the caller's string instead of a std::string).
// ---------------------------------------------------------------------------------------------

using css_values::Cursor;
using css_values::iequals;
using css_values::Tok;
using css_values::TokKind;

constexpr css_values::Dialect kRuntime = css_values::Dialect::Runtime;

bool isIdentStart(char c) { return c == '-' || c == '_' || std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isIdentCont(char c) { return c == '-' || c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool isDigitCh(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

/// Tokenizes the whole declaration text. Returns false on a lexical error (unterminated string,
/// stray character), in which case `out`'s contents are unspecified.
bool tokenize(std::string_view css, std::vector<Tok>& out) {
    out.clear();
    size_t i = 0;
    const size_t n = css.size();
    auto at = [&](size_t k) -> char { return k < n ? css[k] : '\0'; };
    while (i < n) {
        char c = css[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        if (c == '/' && at(i + 1) == '*') {
            i += 2;
            while (i < n && !(css[i] == '*' && at(i + 1) == '/')) {
                ++i;
            }
            if (i >= n) {
                return false;  // Unterminated comment.
            }
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t start = i + 1;
            size_t end = start;
            while (end < n && css[end] != c) {
                ++end;
            }
            if (end >= n) {
                return false;  // Unterminated string.
            }
            Tok t;
            t.kind = TokKind::String;
            t.text = css.substr(start, end - start);
            out.push_back(t);
            i = end + 1;
            continue;
        }
        bool numberStart =
            isDigitCh(c) || (c == '.' && isDigitCh(at(i + 1))) ||
            ((c == '-' || c == '+') && (isDigitCh(at(i + 1)) || (at(i + 1) == '.' && isDigitCh(at(i + 2)))));
        if (numberStart) {
            // T-272: CSS Syntax 3's number, exponent included; an `e` with no digit after it still
            // starts the unit. A value beyond float is a lexical error, like any malformed number.
            Tok t;
            t.kind = TokKind::Number;
            size_t length = 0;
            if (!css_number::read(css.substr(i), t.num, length)) {
                return false;
            }
            i += length;
            if (i < n && css[i] == '%') {
                t.unit = css.substr(i, 1);
                ++i;
            } else if (i < n && isIdentStart(css[i])) {
                size_t us = i;
                while (i < n && isIdentCont(css[i])) {
                    ++i;
                }
                t.unit = css.substr(us, i - us);
            }
            out.push_back(t);
            continue;
        }
        if (isIdentStart(c)) {
            size_t start = i;
            while (i < n && isIdentCont(css[i])) {
                ++i;
            }
            Tok t;
            t.kind = TokKind::Ident;
            t.text = css.substr(start, i - start);
            out.push_back(t);
            continue;
        }
        if (c == '#') {
            ++i;
            size_t start = i;
            while (i < n && isIdentCont(css[i])) {
                ++i;
            }
            Tok t;
            t.kind = TokKind::Hash;
            t.text = css.substr(start, i - start);
            out.push_back(t);
            continue;
        }
        TokKind kind;
        switch (c) {
            case ':':
                kind = TokKind::Colon;
                break;
            case ';':
                kind = TokKind::Semicolon;
                break;
            case ',':
                kind = TokKind::Comma;
                break;
            case '(':
                kind = TokKind::LParen;
                break;
            case ')':
                kind = TokKind::RParen;
                break;
            case '!':
                kind = TokKind::Bang;
                break;
            case '/':
                kind = TokKind::Slash;
                break;
            default:
                return false;  // Stray character.
        }
        Tok t;
        t.kind = kind;
        out.push_back(t);
        ++i;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// CSS name -> prop table (an independent copy of compile/src/xts/xts_props.cpp's kProps, limited
// to the T-35 subset: no shorthands other than transform/transform-origin).
// ---------------------------------------------------------------------------------------------

enum class Kind : uint8_t {
    Float,
    Length,
    Color,
    Enum,
    FontFamily,
    String,
    Unsupported,
    Transform,
    Origin,
    PerspectiveOrigin,   ///< `perspective-origin` (05.5/19.10, T-47a).
    Effect,              ///< `effect` (18-effects.md §18.4, T-51b).
    Gradient,            ///< `background-gradient` (05.5c, T-115).
    Animation,           ///< `animation` (05.6/17.6, T-123).
    Outline,             ///< `outline` shorthand (05 §5.9.8, T-325).
    TextDecoration,      ///< `text-decoration` shorthand (05 §5.9.8, T-324).
    BackgroundSize,      ///< `background-size` (05 §5.9.8, T-327).
    BackgroundPosition,  ///< `background-position` (05 §5.9.8, T-327).
    IndepTransform,      ///< `translate` / `rotate` / `scale` (05 §5.9.8, T-329).
    ClipPath,            ///< `clip-path` (05 §5.9.8, T-332).
    BorderStyle,         ///< `border-style` shorthand (05 §5.9.8, T-333).
    UiScale,             ///< `ui-scale` (05 §5.9.9, T-345).
    HitSlop,             ///< `hit-slop` (05 §5.9.11, T-386).
};

struct NameEntry {
    const char* name;
    uint16_t prop;
    Kind kind;
};

// clang-format off
constexpr NameEntry kNames[] = {
    {"opacity", prop::Opacity, Kind::Float},
    {"width", prop::Width, Kind::Length},
    {"height", prop::Height, Kind::Length},
    {"min-width", prop::MinWidth, Kind::Length},
    {"max-width", prop::MaxWidth, Kind::Length},
    {"min-height", prop::MinHeight, Kind::Length},
    {"max-height", prop::MaxHeight, Kind::Length},
    {"margin-top", prop::MarginTop, Kind::Length},
    {"margin-right", prop::MarginRight, Kind::Length},
    {"margin-bottom", prop::MarginBottom, Kind::Length},
    {"margin-left", prop::MarginLeft, Kind::Length},
    {"padding-top", prop::PaddingTop, Kind::Length},
    {"padding-right", prop::PaddingRight, Kind::Length},
    {"padding-bottom", prop::PaddingBottom, Kind::Length},
    {"padding-left", prop::PaddingLeft, Kind::Length},
    {"border-top-width", prop::BorderTopWidth, Kind::Length},
    {"border-right-width", prop::BorderRightWidth, Kind::Length},
    {"border-bottom-width", prop::BorderBottomWidth, Kind::Length},
    {"border-left-width", prop::BorderLeftWidth, Kind::Length},
    {"left", prop::Left, Kind::Length},
    {"top", prop::Top, Kind::Length},
    {"right", prop::Right, Kind::Length},
    {"bottom", prop::Bottom, Kind::Length},
    {"flex-grow", prop::FlexGrow, Kind::Float},
    {"flex-shrink", prop::FlexShrink, Kind::Float},
    {"flex-basis", prop::FlexBasis, Kind::Length},
    {"aspect-ratio", prop::AspectRatio, Kind::Float},  // T-328 (06 §6.16).
    {"row-gap", prop::RowGap, Kind::Length},
    {"column-gap", prop::ColumnGap, Kind::Length},
    {"font-size", prop::FontSize, Kind::Length},
    {"line-height", prop::LineHeight, Kind::Length},
    {"letter-spacing", prop::LetterSpacing, Kind::Length},
    {"background-color", prop::BackgroundColor, Kind::Color},
    {"border-color", prop::BorderColor, Kind::Color},
    {"border-top-color", prop::BorderTopColor, Kind::Color},  // T-333 (05 §5.9.8).
    {"border-right-color", prop::BorderRightColor, Kind::Color},
    {"border-bottom-color", prop::BorderBottomColor, Kind::Color},
    {"border-left-color", prop::BorderLeftColor, Kind::Color},
    {"border-style", kBorderStyleProp, Kind::BorderStyle},
    {"border-top-style", sp::BorderTopStyle, Kind::Enum},
    {"border-right-style", sp::BorderRightStyle, Kind::Enum},
    {"border-bottom-style", sp::BorderBottomStyle, Kind::Enum},
    {"border-left-style", sp::BorderLeftStyle, Kind::Enum},
    {"border-top-left-radius", prop::BorderRadiusTL, Kind::Length},
    {"border-top-right-radius", prop::BorderRadiusTR, Kind::Length},
    {"border-bottom-right-radius", prop::BorderRadiusBR, Kind::Length},
    {"border-bottom-left-radius", prop::BorderRadiusBL, Kind::Length},
    {"color", prop::Color, Kind::Color},
    {"image-opacity", prop::ImageOpacity, Kind::Float},
    {"outline-width", prop::OutlineWidth, Kind::Length},  // T-325 (05 §5.9.8).
    {"outline-color", prop::OutlineColor, Kind::Color},
    {"outline-offset", prop::OutlineOffset, Kind::Length},
    {"outline-style", sp::OutlineStyle, Kind::Enum},
    {"outline", kOutlineProp, Kind::Outline},
    {"text-decoration-line", sp::TextDecorationLine, Kind::Enum},  // T-324 (05 §5.9.8).
    {"text-decoration-color", prop::TextDecorationColor, Kind::Color},
    {"text-decoration-thickness", prop::TextDecorationThickness, Kind::Length},
    {"text-decoration", kTextDecorationProp, Kind::TextDecoration},
    {"background-size", kBackgroundSizeProp, Kind::BackgroundSize},  // T-327 (05 §5.9.8).
    {"background-position", kBackgroundPositionProp, Kind::BackgroundPosition},
    {"background-repeat", sp::BackgroundRepeat, Kind::Enum},
    {"image-rendering", sp::ImageRendering, Kind::Enum},
    {"transform", kTransformProp, Kind::Transform},
    {"transform-origin", kTransformOriginProp, Kind::Origin},
    {"translate", kTranslateProp, Kind::IndepTransform},  // T-329 (05 §5.9.8).
    {"rotate", kRotateProp, Kind::IndepTransform},
    {"scale", kScaleProp, Kind::IndepTransform},
    {"perspective", prop::Perspective, Kind::Length},
    {"perspective-origin", kPerspectiveOriginProp, Kind::PerspectiveOrigin},
    {"effect", sp::Effect, Kind::Effect},
    {"backface-visibility", sp::BackfaceVisibility, Kind::Enum},
    {"text-rendering", sp::TextRendering, Kind::Enum},
    {"display", sp::Display, Kind::Enum},
    {"position", sp::Position, Kind::Enum},
    {"flex-direction", sp::FlexDirection, Kind::Enum},
    {"flex-wrap", sp::FlexWrap, Kind::Enum},
    {"justify-content", sp::JustifyContent, Kind::Enum},
    {"align-items", sp::AlignItems, Kind::Enum},
    {"align-self", sp::AlignSelf, Kind::Enum},
    {"align-content", sp::AlignContent, Kind::Enum},
    {"overflow", sp::Overflow, Kind::Enum},      // T-336: 2 values -> overflow-x / overflow-y.
    {"overflow-x", sp::OverflowX, Kind::Enum},  // T-336 (05 §5.9.8).
    {"overflow-y", sp::OverflowY, Kind::Enum},
    // T-338 (06 §6.15): grid. The templates and `grid-area` need the IR `GRID` pool / string
    // table, which a runtime declaration cannot add to: setStyle reports them unsupported.
    {"justify-items", sp::JustifyItems, Kind::Enum},
    {"grid-column-start", sp::GridColumnStart, Kind::Enum},
    {"grid-column-end", sp::GridColumnEnd, Kind::Enum},
    {"grid-row-start", sp::GridRowStart, Kind::Enum},
    {"grid-row-end", sp::GridRowEnd, Kind::Enum},
    {"grid-column", kGridColumnProp, Kind::Enum},
    {"grid-row", kGridRowProp, Kind::Enum},
    {"grid-template-columns", sp::GridTemplateColumns, Kind::Unsupported},
    {"grid-template-rows", sp::GridTemplateRows, Kind::Unsupported},
    {"grid-template-areas", sp::GridTemplateAreas, Kind::Unsupported},
    {"grid-area", sp::GridArea, Kind::Unsupported},
    {"order", prop::Order, Kind::Float},  // T-336: an integer.
    {"z-index", sp::ZIndex, Kind::Float},
    {"clip-start", sp::ClipStart, Kind::Float},  // T-191 (28.2): seconds.
    {"clip-rate", sp::ClipRate, Kind::Float},
    {"visibility", sp::Visibility, Kind::Enum},
    {"pointer-events", sp::PointerEvents, Kind::Enum},
    {"pointer-watch", sp::PointerWatch, Kind::Enum},
    {"focus-nav", sp::FocusNav, Kind::Enum},  // T-194 (08.9).
    {"layer", sp::Layer, Kind::Enum},
    {"font-family", sp::FontFamily, Kind::FontFamily},
    {"font-weight", sp::FontWeight, Kind::Enum},
    {"font-style", sp::FontStyle, Kind::Enum},
    {"text-align", sp::TextAlign, Kind::Enum},
    {"white-space", sp::WhiteSpace, Kind::Enum},
    {"text-overflow", sp::TextOverflow, Kind::Enum},
    {"text-transform", sp::TextTransform, Kind::Enum},  // T-323 (05 §5.9.8).
    {"line-clamp", sp::LineClamp, Kind::Enum},          // T-331 (05 §5.9.8).
    {"-webkit-line-clamp", sp::LineClamp, Kind::Enum},
    {"font-variant-numeric", sp::FontVariantNumeric, Kind::Enum},
    {"ui-scale", sp::UiScale, Kind::UiScale},           // T-345 (05 §5.9.9).
    {"media-override", sp::MediaOverride, Kind::Enum},  // T-345.
    {"app-region", sp::AppRegion, Kind::Enum},          // T-376 (05 §5.9.10).
    {"hit-slop", sp::HitSlop, Kind::HitSlop},           // T-386 (05 §5.9.11).
    {"mix-blend-mode", sp::MixBlendMode, Kind::Enum},   // T-326 (18 §18.12).
    {"cursor", sp::Cursor, Kind::Enum},
    {"object-fit", sp::ObjectFit, Kind::Enum},
    {"transition", sp::Transition, Kind::Unsupported},
    // T-322: the longhands are compile-time-only too (merged into `transition` by the compiler).
    {"transition-property", sp::Transition, Kind::Unsupported},
    {"transition-duration", sp::Transition, Kind::Unsupported},
    {"transition-timing-function", sp::Transition, Kind::Unsupported},
    {"transition-delay", sp::Transition, Kind::Unsupported},
    {"animation", sp::Animation, Kind::Animation},
    {"background-image", sp::BackgroundImage, Kind::String},
    {"background-gradient", sp::BackgroundGradient, Kind::Gradient},
    {"clip-path", sp::ClipPath, Kind::ClipPath},  // T-332 (05 §5.9.8).
    {"direction", sp::Direction, Kind::Enum},
    {"writing-mode", sp::WritingMode, Kind::Enum},
    {"text-orientation", sp::TextOrientation, Kind::Enum},
};
// clang-format on

struct EnumEntry {
    uint16_t prop;
    const char* name;
    uint32_t value;
};

// clang-format off
constexpr EnumEntry kEnums[] = {
    {sp::Display, "block", 0}, {sp::Display, "flex", 1}, {sp::Display, "none", 2},
    {sp::Display, "grid", 3},  // T-338 (06 §6.15).
    // T-338: `justify-items` and the grid spellings `start` / `end` (after the flex names).
    {sp::JustifyItems, "stretch", 0}, {sp::JustifyItems, "start", 1}, {sp::JustifyItems, "end", 2},
    {sp::JustifyItems, "center", 3},
    {sp::AlignItems, "start", 1}, {sp::AlignItems, "end", 2},
    {sp::AlignSelf, "start", 2}, {sp::AlignSelf, "end", 3},
    {sp::JustifyContent, "start", 0}, {sp::JustifyContent, "end", 1},
    {sp::JustifyContent, "stretch", 6}, {sp::JustifyContent, "normal", 6},
    {sp::AlignContent, "normal", 6},
    {sp::AlignContent, "start", 0}, {sp::AlignContent, "end", 1},
    {sp::Position, "static", 0}, {sp::Position, "relative", 1}, {sp::Position, "absolute", 2},
    {sp::FlexDirection, "row", 0}, {sp::FlexDirection, "column", 1},
    {sp::FlexDirection, "row-reverse", 2}, {sp::FlexDirection, "column-reverse", 3},
    {sp::FlexWrap, "nowrap", 0}, {sp::FlexWrap, "wrap", 1}, {sp::FlexWrap, "wrap-reverse", 2},
    {sp::JustifyContent, "flex-start", 0}, {sp::JustifyContent, "flex-end", 1},
    {sp::JustifyContent, "center", 2}, {sp::JustifyContent, "space-between", 3},
    {sp::JustifyContent, "space-around", 4}, {sp::JustifyContent, "space-evenly", 5},
    {sp::AlignItems, "stretch", 0}, {sp::AlignItems, "flex-start", 1},
    {sp::AlignItems, "flex-end", 2}, {sp::AlignItems, "center", 3},
    {sp::AlignSelf, "auto", 0}, {sp::AlignSelf, "stretch", 1}, {sp::AlignSelf, "flex-start", 2},
    {sp::AlignSelf, "flex-end", 3}, {sp::AlignSelf, "center", 4},
    {sp::AlignContent, "flex-start", 0}, {sp::AlignContent, "flex-end", 1},
    {sp::AlignContent, "center", 2}, {sp::AlignContent, "space-between", 3},
    {sp::AlignContent, "space-around", 4}, {sp::AlignContent, "space-evenly", 5},
    {sp::AlignContent, "stretch", 6},
    {sp::Overflow, "visible", 0}, {sp::Overflow, "hidden", 1}, {sp::Overflow, "scroll", 2},
    {sp::Overflow, "auto", 3},  // T-336.
    {sp::OverflowX, "visible", 0}, {sp::OverflowX, "hidden", 1}, {sp::OverflowX, "scroll", 2},
    {sp::OverflowX, "auto", 3},
    {sp::OverflowY, "visible", 0}, {sp::OverflowY, "hidden", 1}, {sp::OverflowY, "scroll", 2},
    {sp::OverflowY, "auto", 3},
    {sp::Visibility, "visible", 0}, {sp::Visibility, "hidden", 1},
    {sp::PointerEvents, "auto", 0}, {sp::PointerEvents, "none", 1},
    {sp::FontWeight, "normal", 400}, {sp::FontWeight, "bold", 700},
    {sp::FontStyle, "normal", 0}, {sp::FontStyle, "italic", 1},
    {sp::TextAlign, "start", 0}, {sp::TextAlign, "end", 1}, {sp::TextAlign, "left", 2},
    {sp::TextAlign, "center", 3}, {sp::TextAlign, "right", 4},
    {sp::WhiteSpace, "normal", 0}, {sp::WhiteSpace, "nowrap", 1},
    {sp::TextOverflow, "clip", 0}, {sp::TextOverflow, "ellipsis", 1},
    {sp::Cursor, "default", 0}, {sp::Cursor, "pointer", 1}, {sp::Cursor, "text", 2},
    {sp::Cursor, "move", 3},
    {sp::ObjectFit, "fill", 0}, {sp::ObjectFit, "contain", 1}, {sp::ObjectFit, "cover", 2},
    {sp::Direction, "ltr", 0}, {sp::Direction, "rtl", 1},
    {sp::WritingMode, "horizontal-tb", 0}, {sp::WritingMode, "vertical-rl", 1},
    {sp::WritingMode, "vertical-lr", 2},
    {sp::TextOrientation, "mixed", 0}, {sp::TextOrientation, "upright", 1},
    {sp::TextOrientation, "sideways", 2},
    {sp::BackfaceVisibility, "visible", 0}, {sp::BackfaceVisibility, "hidden", 1},
    {sp::TextRendering, "auto", 0}, {sp::TextRendering, "geometricPrecision", 1},
    {sp::PointerWatch, "false", 0}, {sp::PointerWatch, "true", 1},
    {sp::FocusNav, "auto", 0}, {sp::FocusNav, "explicit", 1}, {sp::FocusNav, "none", 2},
    {sp::Layer, "normal", 0}, {sp::Layer, "overlay", 1},
    {sp::OutlineStyle, "none", 0}, {sp::OutlineStyle, "solid", 1},  // T-325.
    // T-333: border-*-style (kBorderStyle*).
    {sp::BorderTopStyle, "none", 0}, {sp::BorderTopStyle, "solid", 1},
    {sp::BorderTopStyle, "dashed", 2}, {sp::BorderTopStyle, "dotted", 3},
    {sp::BorderRightStyle, "none", 0}, {sp::BorderRightStyle, "solid", 1},
    {sp::BorderRightStyle, "dashed", 2}, {sp::BorderRightStyle, "dotted", 3},
    {sp::BorderBottomStyle, "none", 0}, {sp::BorderBottomStyle, "solid", 1},
    {sp::BorderBottomStyle, "dashed", 2}, {sp::BorderBottomStyle, "dotted", 3},
    {sp::BorderLeftStyle, "none", 0}, {sp::BorderLeftStyle, "solid", 1},
    {sp::BorderLeftStyle, "dashed", 2}, {sp::BorderLeftStyle, "dotted", 3},
    {sp::TextTransform, "none", 0}, {sp::TextTransform, "uppercase", 1},  // T-323.
    {sp::TextTransform, "lowercase", 2}, {sp::TextTransform, "capitalize", 3},
    // T-326 (18 §18.12): mix-blend-mode = BlendMode (xt/node.h); `additive` is an alias of `add`.
    {sp::MixBlendMode, "normal", 0}, {sp::MixBlendMode, "multiply", 1}, {sp::MixBlendMode, "screen", 2},
    {sp::MixBlendMode, "overlay", 3}, {sp::MixBlendMode, "darken", 4}, {sp::MixBlendMode, "lighten", 5},
    {sp::MixBlendMode, "color-dodge", 6}, {sp::MixBlendMode, "color-burn", 7},
    {sp::MixBlendMode, "hard-light", 8}, {sp::MixBlendMode, "soft-light", 9},
    {sp::MixBlendMode, "difference", 10}, {sp::MixBlendMode, "exclusion", 11}, {sp::MixBlendMode, "hue", 12},
    {sp::MixBlendMode, "saturation", 13}, {sp::MixBlendMode, "color", 14}, {sp::MixBlendMode, "luminosity", 15},
    {sp::MixBlendMode, "add", 16}, {sp::MixBlendMode, "additive", 16},
    {sp::BackgroundRepeat, "stretch", 0}, {sp::BackgroundRepeat, "no-repeat", 1},  // T-327.
    {sp::BackgroundRepeat, "repeat", 2}, {sp::BackgroundRepeat, "repeat-x", 3},
    {sp::BackgroundRepeat, "repeat-y", 4},
    {sp::ImageRendering, "auto", 0}, {sp::ImageRendering, "pixelated", 1},
    {sp::LineClamp, "none", 0},  // T-331 (a count is read as a number).
    {sp::FontVariantNumeric, "normal", 0}, {sp::FontVariantNumeric, "tabular-nums", 1},
    {sp::MediaOverride, "auto", 0}, {sp::MediaOverride, "show", 1}, {sp::MediaOverride, "hide", 2},  // T-345.
    {sp::AppRegion, "none", 0}, {sp::AppRegion, "drag", 1}, {sp::AppRegion, "no-drag", 2},  // T-376.
};
// clang-format on

const NameEntry* findEntry(std::string_view cssName) {
    for (const NameEntry& e : kNames) {
        if (iequals(cssName, e.name)) {
            return &e;
        }
    }
    return nullptr;
}

const NameEntry* findEntryByProp(uint16_t propNumber) {
    for (const NameEntry& e : kNames) {
        if (e.prop == propNumber) {
            return &e;
        }
    }
    return nullptr;
}

bool enumValueOf(uint16_t propNumber, std::string_view name, uint32_t& out) {
    for (const EnumEntry& e : kEnums) {
        if (e.prop == propNumber && iequals(name, e.name)) {
            out = e.value;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Value parsing: the grammar is css_values (shared with the compiler, T-413); this file only maps
// its results onto Decl.
// ---------------------------------------------------------------------------------------------

void pushValue(std::vector<Decl>& out, uint16_t p, const Value& v) {
    Decl d;
    d.prop = p;
    d.value = v;
    out.push_back(std::move(d));
}

void pushValues(std::vector<Decl>& out, const std::vector<css_values::PropValue>& values) {
    for (const css_values::PropValue& v : values) {
        pushValue(out, v.prop, v.value);
    }
}

css_values::IndependentTransform independentKind(uint16_t which) {
    return which == kTranslateProp ? css_values::IndependentTransform::Translate
           : which == kRotateProp  ? css_values::IndependentTransform::Rotate
                                   : css_values::IndependentTransform::Scale;
}

/// `animation` (05.6/17.6, T-123): the items in order; a count-0 item creates no animation (T-272)
/// and a malformed item makes the whole value invalid.
bool parseAnimationList(const Tok* toks, size_t count, std::vector<AnimationDecl>& out) {
    if (count == 1 && css_values::isKeyword(kRuntime, toks[0], "none")) {
        return true;
    }
    for (const std::vector<Tok>& itemToks : css_values::splitByCommaTopLevel(toks, count)) {
        AnimationDecl item;
        css_values::AnimationItem parsed;
        if (css_values::parseAnimationItem(itemToks.data(), itemToks.size(), kRuntime, nullptr, parsed,
                                           &item.easingPoints) != css_values::AnimationItemResult::Ok) {
            return false;
        }
        if (!parsed.infinite && parsed.iterations == 0) {
            continue;  // T-272: CSS does not play it (IR's 0 means infinite).
        }
        item.name.assign(parsed.name);
        item.spec = IrAnimationSpec{};
        item.spec.timeline = kInvalidTimeline;  // Resolved by the caller from `name`.
        item.spec.duration = parsed.duration;
        item.spec.delay = parsed.delay;
        item.spec.easing = css_values::toIrEasing(parsed.easing);
        item.spec.iterations = parsed.infinite ? 0 : parsed.iterations;  // IrAnimationSpec: 0 = infinite.
        item.spec.direction = parsed.direction;
        item.spec.fill = parsed.fill;
        item.spec.playState = parsed.playState;
        out.push_back(std::move(item));
    }
    return true;
}

/// `effect` (18.3-18.4, T-51b): each entry's stopOffset/stopCount index `outStops`. `custom()` is
/// compile-time only (18.4).
Status parseEffectList(const Tok* toks, size_t count, std::vector<EffectInstance>& outEffects,
                       std::vector<GradientStop>& outStops) {
    if (count == 1 && css_values::isKeyword(kRuntime, toks[0], "none")) {
        return Status::kOk;  // Empty sequence (18.4: decl exists, count 0).
    }
    Cursor cur(toks, count);
    if (cur.atEnd()) {
        return Status::kInvalidArgument;
    }
    std::string_view fn;
    std::vector<Tok> inner;
    while (!cur.atEnd()) {
        const css_values::EffectCall call = css_values::nextEffectCall(cur, fn, inner);
        if (call == css_values::EffectCall::NotAFunction) {
            return Status::kInvalidArgument;
        }
        if (fn == "custom") {
            return Status::kUnsupported;
        }
        if (call == css_values::EffectCall::Unterminated) {
            return Status::kInvalidArgument;
        }
        // 18.6: backdrop-blur only as the stack's input stage.
        if (fn == "backdrop-blur" && !outEffects.empty()) {
            return Status::kInvalidArgument;
        }
        EffectInstance entry{};
        std::vector<GradientStop> stops;
        bool percentError = false;
        if (css_values::parseEffectFn(fn, inner, kRuntime, nullptr, percentError, entry, stops) !=
            css_values::EffectFnResult::Ok) {
            return Status::kInvalidArgument;
        }
        entry.enabled = 1;
        entry.shader = kInvalidAsset;
        entry.stopOffset = static_cast<uint32_t>(outStops.size());
        entry.stopCount = static_cast<uint32_t>(stops.size());
        outStops.insert(outStops.end(), stops.begin(), stops.end());
        outEffects.push_back(entry);
        if (outEffects.size() > css_values::kMaxEffects) {
            return Status::kInvalidArgument;
        }
    }
    return Status::kOk;
}

/// `font-family: A, "B C", sans-serif`. Only the first family survives into the Decl: the runtime
/// cascade can only point ComputedStyle at an existing IR font-family pool run (style_engine.cpp's
/// applyRuntimeDecl), which is a single name; the rest are still syntax-checked.
bool parseFontFamily(const Tok* toks, size_t count, std::string& firstOut) {
    std::vector<std::string> families;
    if (!css_values::parseFontFamilyList(toks, count, families)) {
        return false;
    }
    firstOut = std::move(families[0]);
    return true;
}

/// Parses one declaration's already-tokenized value. Returns kOk, kUnsupported (transition) or
/// kInvalidArgument.
Status parseValueTokens(const NameEntry& entry, const Tok* toks, size_t count, std::vector<Decl>& out) {
    if (entry.kind == Kind::Unsupported) {
        return Status::kUnsupported;
    }
    if (count == 0) {
        return Status::kInvalidArgument;
    }
    bool unsupportedGrid = false;
    if (entry.prop >= sp::GridColumnStart && entry.prop <= sp::GridRowEnd) {  // T-338.
        uint32_t line = 0;
        if (!css_values::parseGridLine(toks, count, line, unsupportedGrid)) {
            return Status::kInvalidArgument;
        }
        pushValue(out, entry.prop, Value::ofEnum(line));
        return Status::kOk;
    }
    if (entry.prop == kGridColumnProp || entry.prop == kGridRowProp) {  // T-338: `<a> [/ <b>]`.
        size_t slash = count;
        for (size_t i = 0; i < count; ++i) {
            if (css_values::isSlash(toks[i])) {
                slash = i;
                break;
            }
        }
        uint32_t start = kGridLineAuto;
        uint32_t end = kGridLineAuto;
        if (!css_values::parseGridLine(toks, slash, start, unsupportedGrid) ||
            (slash < count &&
             !css_values::parseGridLine(toks + slash + 1, count - slash - 1, end, unsupportedGrid))) {
            return Status::kInvalidArgument;
        }
        const uint16_t first = entry.prop == kGridColumnProp ? sp::GridColumnStart : sp::GridRowStart;
        pushValue(out, first, Value::ofEnum(start));
        pushValue(out, static_cast<uint16_t>(first + 1), Value::ofEnum(end));
        return Status::kOk;
    }
    std::vector<css_values::PropValue> values;
    switch (entry.kind) {
        case Kind::Transform:
            if (!css_values::parseTransform(toks, count, nullptr, values)) {
                return Status::kInvalidArgument;
            }
            pushValues(out, values);
            return Status::kOk;
        case Kind::IndepTransform:
            if (!css_values::parseIndependentTransform(independentKind(entry.prop), toks, count, kRuntime,
                                                       nullptr, values)) {
                return Status::kInvalidArgument;
            }
            pushValues(out, values);
            return Status::kOk;
        case Kind::Origin:
        case Kind::PerspectiveOrigin:
        case Kind::BackgroundPosition:
        case Kind::BackgroundSize: {
            Length x, y;
            const bool ok = entry.kind == Kind::BackgroundSize
                                ? css_values::parseBackgroundSize(toks, count, kRuntime, x, y)
                                : css_values::parseOriginPair(toks, count, kRuntime, x, y);
            if (!ok) {
                return Status::kInvalidArgument;
            }
            const uint16_t px = entry.kind == Kind::Origin               ? prop::AnchorX
                                : entry.kind == Kind::PerspectiveOrigin  ? prop::PerspectiveOriginX
                                : entry.kind == Kind::BackgroundPosition ? prop::BackgroundPositionX
                                                                         : prop::BackgroundSizeX;
            const uint16_t py = entry.kind == Kind::Origin               ? prop::AnchorY
                                : entry.kind == Kind::PerspectiveOrigin  ? prop::PerspectiveOriginY
                                : entry.kind == Kind::BackgroundPosition ? prop::BackgroundPositionY
                                                                         : prop::BackgroundSizeY;
            pushValue(out, px, Value::ofLength(x.value, x.unit));
            pushValue(out, py, Value::ofLength(y.value, y.unit));
            return Status::kOk;
        }
        case Kind::Outline: {  // T-325 (05 §5.9.8).
            css_values::OutlineValue o;
            if (!css_values::parseOutline(toks, count, kRuntime, o)) {
                return Status::kInvalidArgument;
            }
            pushValue(out, prop::OutlineWidth, Value::ofLength(o.width.value, o.width.unit));
            pushValue(out, sp::OutlineStyle, Value::ofEnum(o.style));
            if (o.haveColor) {
                pushValue(out, prop::OutlineColor, Value::ofColor(o.color));
            }
            return Status::kOk;
        }
        case Kind::BorderStyle: {  // T-333 (05 §5.9.8).
            uint32_t sides[4] = {};
            if (!css_values::parseBorderStyleSides(toks, count, kRuntime, sides)) {
                return Status::kInvalidArgument;
            }
            for (int i = 0; i < 4; ++i) {
                pushValue(out, static_cast<uint16_t>(sp::BorderTopStyle + i), Value::ofEnum(sides[i]));
            }
            return Status::kOk;
        }
        case Kind::UiScale: {  // T-345 (05 §5.9.9): Color (mode, min, max, 0), as the compiler.
            float v[3] = {0, 0, 0};
            if (!css_values::parseUiScale(toks, count, kRuntime, v)) {
                return Status::kInvalidArgument;
            }
            pushValue(out, entry.prop, Value::ofColor(Color{v[0], v[1], v[2], 0}));
            return Status::kOk;
        }
        case Kind::HitSlop: {  // T-386 (05 §5.9.11): the longhands StyleProp 277-280 (IR minor 8).
            Length sides[4];
            if (!css_values::parseHitSlop(toks, count, kRuntime, sides)) {
                return Status::kInvalidArgument;
            }
            for (int i = 0; i < 4; ++i) {
                pushValue(out, static_cast<uint16_t>(sp::HitSlopTop + i),
                          Value::ofLength(sides[i].value, sides[i].unit));
            }
            return Status::kOk;
        }
        case Kind::TextDecoration: {  // T-324 (05 §5.9.8).
            css_values::TextDecorationValue t;
            if (!css_values::parseTextDecoration(toks, count, kRuntime, t)) {
                return Status::kInvalidArgument;
            }
            pushValue(out, sp::TextDecorationLine, Value::ofEnum(t.lines));
            pushValue(out, prop::TextDecorationThickness,
                      Value::ofLength(t.thickness.value, t.thickness.unit));
            if (t.haveColor) {
                pushValue(out, prop::TextDecorationColor, Value::ofColor(t.color));
            }
            return Status::kOk;
        }
        case Kind::Gradient: {
            // `value`'s real ListRef is filled in by the caller (ElementRef::setStyle) once
            // decl.gradient is appended to Document's runtime gradient pool; see mini_css.h.
            Decl decl;
            decl.prop = entry.prop;
            if (!css_values::parseGradientValue(toks, count, kRuntime, decl.gradient)) {
                return Status::kInvalidArgument;
            }
            decl.value.type = ValueType::ListRef;
            out.push_back(std::move(decl));
            return Status::kOk;
        }
        case Kind::ClipPath: {
            // T-332: the caller writes decl.clipShape into Document's runtime pool and fills in
            // `value` (an Enum) from it.
            Decl decl;
            decl.prop = entry.prop;
            bool pathOrUrl = false;
            if (!css_values::parseClipPathValue(toks, count, kRuntime, decl.clipShape, pathOrUrl)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofEnum(0);
            out.push_back(std::move(decl));
            return Status::kOk;
        }
        case Kind::Animation: {
            // The caller resolves each item's `@keyframes` name and appends the list to
            // Document's runtime animation pool (mini_css.h).
            Decl decl;
            decl.prop = entry.prop;
            if (!parseAnimationList(toks, count, decl.animations)) {
                return Status::kInvalidArgument;
            }
            decl.value.type = ValueType::ListRef;
            out.push_back(std::move(decl));
            return Status::kOk;
        }
        case Kind::Effect: {
            // The caller appends decl.effects/effectStops to Document's runtime effect pool
            // (mini_css.h).
            Decl decl;
            decl.prop = entry.prop;
            const Status status = parseEffectList(toks, count, decl.effects, decl.effectStops);
            if (status != Status::kOk) {
                return status;
            }
            decl.value.type = ValueType::ListRef;
            out.push_back(std::move(decl));
            return Status::kOk;
        }
        default:
            break;
    }
    if (entry.prop == prop::BorderColor) {  // T-333: 2-4 colors; one color is the plain Color path.
        Color sides[4];
        if (css_values::parseBorderColorSides(toks, count, sides)) {
            for (int i = 0; i < 4; ++i) {
                pushValue(out, static_cast<uint16_t>(prop::BorderTopColor + i), Value::ofColor(sides[i]));
            }
            return Status::kOk;
        }
    }
    if (entry.prop == sp::TextDecorationLine) {  // T-324: a keyword set, not a single enum.
        uint32_t lines = 0;
        if (!css_values::parseTextDecorationLine(toks, count, kRuntime, lines)) {
            return Status::kInvalidArgument;
        }
        pushValue(out, entry.prop, Value::ofEnum(lines));
        return Status::kOk;
    }
    if (entry.prop == sp::Overflow && count == 2) {  // T-336: `overflow: <x> <y>`.
        uint32_t x = 0;
        uint32_t y = 0;
        if (toks[0].kind != TokKind::Ident || toks[1].kind != TokKind::Ident ||
            !enumValueOf(sp::OverflowX, toks[0].text, x) || !enumValueOf(sp::OverflowY, toks[1].text, y)) {
            return Status::kInvalidArgument;
        }
        pushValue(out, sp::OverflowX, Value::ofEnum(x));
        pushValue(out, sp::OverflowY, Value::ofEnum(y));
        return Status::kOk;
    }
    if (entry.prop == prop::Order) {  // T-336: `<integer>`.
        float order = 0;
        if (!css_values::parseOrderInteger(toks, count, order)) {
            return Status::kInvalidArgument;
        }
        pushValue(out, entry.prop, Value::ofFloat(order));
        return Status::kOk;
    }
    if (entry.prop == prop::AspectRatio) {  // T-328: `auto` (0) | <w> / <h> | <number>, both > 0.
        float ratio = 0.0f;
        if (!css_values::parseAspectRatio(toks, count, ratio)) {
            return Status::kInvalidArgument;
        }
        pushValue(out, entry.prop, Value::ofFloat(ratio));
        return Status::kOk;
    }
    Decl decl;
    decl.prop = entry.prop;
    switch (entry.kind) {
        case Kind::Float: {
            float f = 0;
            // T-191 (05 §5.9.3): `clip-start` is a <time>; a `clip-rate` must be positive.
            if (count != 1 ||
                !(css_values::parseFloatTok(toks[0], f) ||
                  (entry.prop == sp::ClipStart && css_values::parseTimeTok(toks[0], f))) ||
                (entry.prop == sp::ClipRate && !(f > 0.0f))) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofFloat(f);
            break;
        }
        case Kind::Length: {
            if (count != 1) {
                return Status::kInvalidArgument;
            }
            if (entry.prop == prop::LineHeight) {
                if (toks[0].kind != TokKind::Number) {
                    return Status::kInvalidArgument;  // No keyword (`auto`), as the compiler.
                }
                if (toks[0].unit.empty()) {
                    // Unitless line-height is stored as Em (05.4), including unitless 0.
                    decl.value = Value::ofLength(toks[0].num, LengthUnit::Em);
                    break;
                }
            }
            // 05.5/19.10 (T-47a): `perspective: none` encodes as LengthUnit::Auto.
            if (entry.prop == prop::Perspective && css_values::isKeyword(kRuntime, toks[0], "none")) {
                decl.value = Value::ofLength(0, LengthUnit::Auto);
                break;
            }
            Length len{};
            if (!css_values::parseLengthTok(toks[0], kRuntime, len) ||
                (entry.prop == prop::TextDecorationThickness && !css_values::isDecorationThickness(len))) {
                return Status::kInvalidArgument;  // T-324: `auto` or a non-negative px / em length.
            }
            decl.value = Value::ofLength(len.value, len.unit);
            break;
        }
        case Kind::Color: {
            Color c{};
            if (!css_values::parseColorTokens(toks, count, c)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofColor(c);
            break;
        }
        case Kind::Enum: {
            if (count != 1) {
                return Status::kInvalidArgument;
            }
            uint32_t e = 0;
            if (entry.prop == sp::FontWeight && toks[0].kind == TokKind::Number && toks[0].unit.empty()) {
                if (!css_values::parseFontWeightNumber(toks[0], e)) {
                    return Status::kInvalidArgument;
                }
            } else if (entry.prop == sp::LineClamp && toks[0].kind == TokKind::Number) {
                // T-331 (05 §5.9.8): an integer >= 1 (`none` is the keyword below).
                if (!css_values::parseLineClampNumber(toks[0], e)) {
                    return Status::kInvalidArgument;
                }
            } else if (toks[0].kind != TokKind::Ident || !enumValueOf(entry.prop, toks[0].text, e)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofEnum(e);
            break;
        }
        case Kind::FontFamily: {
            std::string first;
            if (!parseFontFamily(toks, count, first)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofStringId(kInvalidString);
            decl.value.type = ValueType::ListRef;
            decl.text = std::move(first);
            break;
        }
        case Kind::String: {
            // 05 §5.y: a bare or quoted asset name (no url()).
            if (count != 1 || (toks[0].kind != TokKind::Ident && toks[0].kind != TokKind::String)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofStringId(kInvalidString);
            decl.text.assign(toks[0].text);
            break;
        }
        default:
            return Status::kInvalidArgument;  // Handled above; unreachable.
    }
    out.push_back(std::move(decl));
    return Status::kOk;
}

/// T-170: `var(--name[, fallback])` as a declaration's whole value. Only the plain value kinds
/// (Float/Length/Color/Enum) accept it; `toks[0..1]` are already known to be `var` `(`.
Status parseVarDecl(const NameEntry& entry, const Tok* toks, size_t count, std::vector<Decl>& out) {
    if (entry.kind != Kind::Float && entry.kind != Kind::Length && entry.kind != Kind::Color &&
        entry.kind != Kind::Enum) {
        return Status::kInvalidArgument;
    }
    if (count < 4 || toks[2].kind != TokKind::Ident || toks[2].text.size() < 3 ||
        toks[2].text.substr(0, 2) != "--" || toks[count - 1].kind != TokKind::RParen) {
        return Status::kInvalidArgument;
    }
    Decl decl;
    decl.prop = entry.prop;
    decl.varRef = true;
    decl.text.assign(toks[2].text);
    if (count > 4) {
        if (toks[3].kind != TokKind::Comma || count == 5) {
            return Status::kInvalidArgument;
        }
        std::vector<Decl> fallback;
        if (parseValueTokens(entry, toks + 4, count - 5, fallback) != Status::kOk || fallback.size() != 1 ||
            fallback[0].prop != entry.prop || !fallback[0].text.empty()) {
            return Status::kInvalidArgument;
        }
        decl.varFallback = fallback[0].value;
        decl.hasVarFallback = true;
    }
    out.push_back(std::move(decl));
    return Status::kOk;
}

/// kInvalidArgument wins over kUnsupported (see parseDeclarations' contract).
Status worse(Status current, Status next) {
    if (current == Status::kInvalidArgument || next == Status::kInvalidArgument) {
        return Status::kInvalidArgument;
    }
    if (current == Status::kUnsupported || next == Status::kUnsupported) {
        return Status::kUnsupported;
    }
    return Status::kOk;
}

}  // namespace

uint16_t findProp(std::string_view cssName) {
    const NameEntry* entry = findEntry(cssName);
    return entry != nullptr ? entry->prop : prop::kInvalidProp;
}

namespace {
thread_local uint64_t t_parseDeclarationsCount = 0;  // T-389 (tests).
}  // namespace

uint64_t parseDeclarationsCount() { return t_parseDeclarationsCount; }

Status parseDeclarations(std::string_view css, std::vector<Decl>& out) {
    ++t_parseDeclarationsCount;
    std::vector<Tok> toks;
    if (!tokenize(css, toks)) {
        return Status::kInvalidArgument;
    }
    Status status = Status::kOk;
    // T-329 (05 §5.9.8): the last successfully parsed `transform` / `translate` / `rotate` /
    // `scale` value of this call, for the combination below.
    struct TransformPart {
        bool present = false;
        size_t begin = 0;
        size_t count = 0;
        uint8_t important = 0;
        size_t order = 0;
    };
    TransformPart parts[4];  // transform, translate, rotate, scale.
    size_t declOrder = 0;
    size_t i = 0;
    while (i < toks.size()) {
        // One declaration: Ident ':' <value tokens> [ '!' Ident ] , terminated by ';' or the end.
        size_t end = i;
        while (end < toks.size() && toks[end].kind != TokKind::Semicolon) {
            ++end;
        }
        size_t next = end < toks.size() ? end + 1 : end;
        if (end == i) {  // Empty declaration (";;" or a trailing ';'): ignored, not an error.
            i = next;
            continue;
        }
        bool important = false;
        if (end - i >= 2 && toks[end - 2].kind == TokKind::Bang && toks[end - 1].kind == TokKind::Ident &&
            iequals(toks[end - 1].text, "important")) {
            important = true;
            end -= 2;
        }
        if (toks[i].kind != TokKind::Ident || end - i < 2 || toks[i + 1].kind != TokKind::Colon) {
            status = Status::kInvalidArgument;
            i = next;
            continue;
        }
        const NameEntry* entry = findEntry(toks[i].text);
        if (entry == nullptr) {
            status = Status::kInvalidArgument;  // Unknown property or an unsupported shorthand.
            i = next;
            continue;
        }
        size_t before = out.size();
        const Tok* valueToks = toks.data() + i + 2;
        const size_t valueCount = end - i - 2;
        const bool isVar = valueCount >= 2 && valueToks[0].kind == TokKind::Ident &&
                           iequals(valueToks[0].text, "var") && valueToks[1].kind == TokKind::LParen;
        Status one = isVar ? parseVarDecl(*entry, valueToks, valueCount, out)
                           : parseValueTokens(*entry, valueToks, valueCount, out);
        if (one == Status::kOk && important) {
            for (size_t k = before; k < out.size(); ++k) {
                out[k].important = 1;
            }
        }
        const int partKind = entry->prop == kTransformProp   ? 0
                             : entry->prop == kTranslateProp ? 1
                             : entry->prop == kRotateProp    ? 2
                             : entry->prop == kScaleProp     ? 3
                                                             : -1;
        if (partKind >= 0 && one == Status::kOk && !isVar) {
            parts[partKind] =
                TransformPart{true, i + 2, valueCount, static_cast<uint8_t>(important ? 1 : 0), declOrder};
        }
        ++declOrder;
        status = worse(status, one);
        i = next;
    }
    // T-329: with `transform` and an independent property in the same call, the result is
    // translate * rotate * scale * transform (CSS Transforms 2 §6), parsed as one `transform`
    // sequence that replaces every transform component the declarations wrote one by one.
    if (parts[0].present && (parts[1].present || parts[2].present || parts[3].present)) {
        static constexpr uint16_t kWhich[4] = {kTransformProp, kTranslateProp, kRotateProp, kScaleProp};
        std::vector<Tok> combined;
        const TransformPart* latest = &parts[0];
        for (int k = 1; k < 4; ++k) {
            if (!parts[k].present) {
                continue;
            }
            std::vector<Tok> fn;
            bool isNone = false;
            css_values::independentTransformFnToks(independentKind(kWhich[k]), toks.data() + parts[k].begin,
                                                   parts[k].count, kRuntime, nullptr, fn, isNone);
            combined.insert(combined.end(), fn.begin(), fn.end());
            latest = parts[k].order > latest->order ? &parts[k] : latest;
        }
        const Tok* xform = toks.data() + parts[0].begin;
        const bool xformNone =
            parts[0].count == 1 && xform[0].kind == TokKind::Ident && iequals(xform[0].text, "none");
        if (!xformNone) {
            combined.insert(combined.end(), xform, xform + parts[0].count);
        }
        if (combined.empty()) {
            Tok none;
            none.kind = TokKind::Ident;
            none.text = "none";
            combined.push_back(none);
        }
        std::vector<css_values::PropValue> values;
        if (!css_values::parseTransform(combined.data(), combined.size(), nullptr, values)) {
            return Status::kInvalidArgument;
        }
        std::vector<Decl> merged;
        pushValues(merged, values);
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [](const Decl& d) { return css_values::isTransformComponentProp(d.prop); }),
                  out.end());
        for (Decl& d : merged) {
            d.important = latest->important;
            out.push_back(std::move(d));
        }
    }
    return status;
}

Status parseOne(uint16_t propNumber, std::string_view valueText, std::vector<Decl>& out) {
    const NameEntry* entry = findEntryByProp(propNumber);
    if (entry == nullptr) {
        return Status::kInvalidArgument;
    }
    std::vector<Tok> toks;
    if (!tokenize(valueText, toks)) {
        return Status::kInvalidArgument;
    }
    return parseValueTokens(*entry, toks.data(), toks.size(), out);
}

bool parseTokenText(uint16_t propNumber, std::string_view text, Value& out) {
    std::vector<Decl> decls;
    if (parseOne(propNumber, text, decls) != Status::kOk || decls.size() != 1 ||
        decls[0].prop != propNumber || decls[0].varRef || !decls[0].text.empty()) {
        return false;
    }
    ValueType type = decls[0].value.type;
    if (type != ValueType::Float && type != ValueType::Length && type != ValueType::Color &&
        type != ValueType::Enum) {
        return false;
    }
    out = decls[0].value;
    return true;
}

bool classifyTokenText(std::string_view text, Value& out) {
    Value v;
    if (parseTokenText(prop::BackgroundColor, text, v) && v.type == ValueType::Color) {
        out = v;
        return true;
    }
    if (parseTokenText(sp::ZIndex, text, v) && v.type == ValueType::Float) {
        out = v;
        return true;
    }
    if (parseTokenText(prop::Width, text, v) && v.type == ValueType::Length &&
        v.len.unit != LengthUnit::Auto) {
        out = v;
        return true;
    }
    return false;
}

}  // namespace xt::mini_css

namespace xt {

Result<Value> parseValue(uint16_t prop, std::string_view css) {
    std::vector<mini_css::Decl> decls;
    Status status = mini_css::parseOne(prop, css, decls);
    if (status != Status::kOk) {
        return Result<Value>::error(status);
    }
    if (decls.size() != 1 || decls[0].prop != prop) {
        // `transform`/`transform-origin` expand to several props and so have no single Value.
        return Result<Value>::error(Status::kUnsupported);
    }
    if (decls[0].prop == style_decode::styleprop::Animation) {
        // 17.6 (T-123): an `animation` list needs a Document to resolve its `@keyframes` names
        // into TimelineIds, which 13.4's parseValue has none of (like font-family below).
        return Result<Value>::error(Status::kUnsupported);
    }
    if (!decls[0].text.empty()) {
        // font-family / background-image need a Document to intern their string (13.4's parseValue
        // has none). See the T-35 report's uncovered cases.
        return Result<Value>::error(Status::kUnsupported);
    }
    return Result<Value>::ok(decls[0].value);
}

}  // namespace xt
