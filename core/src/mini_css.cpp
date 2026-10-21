#include "mini_css.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "css_color.h"   // Named colours and hsl(), shared with the compiler (T-322).
#include "css_number.h"  // The CSS number, shared with the compiler's lexer (T-272).
#include "style_decode.h"
#include "transform_compose.h"  // Compose/decompose for arbitrary orders (05.5, T-120).
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/value_helpers.h"

/// Runtime mini CSS declaration-list parser (see mini_css.h). Mirrors the compiler's value grammar
/// (compile/src/xts/xts_lexer.cpp + xts_values.cpp) for the subset T-35 lists.

namespace xt::mini_css {

namespace {

namespace sp = style_decode::styleprop;

// ---------------------------------------------------------------------------------------------
// Tokenizer (a cut-down copy of compile/src/xts/xts_lexer.cpp: no braces/at-rules/selector
// punctuation, and every token's text is a view into the caller's string instead of a std::string).
// ---------------------------------------------------------------------------------------------

enum class TokKind : uint8_t {
    End,
    Ident,
    Number,
    String,
    Hash,
    Colon,
    Semicolon,
    Comma,
    LParen,
    RParen,
    Bang,
    Slash,  ///< T-322: `hsl(h s l / a)`.
};

struct Tok {
    TokKind kind = TokKind::End;
    std::string_view text{};  // Ident / String contents / Hash hex digits.
    float num = 0;
    std::string_view unit{};  // Number only ("" = unitless, "%" = percent, else px/em/deg/...).
};

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

/// ASCII case-insensitive compare (CSS keywords are ASCII).
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
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
// Value parsing.
// ---------------------------------------------------------------------------------------------

/// A read-only cursor over one declaration's value tokens.
class Cursor {
public:
    Cursor(const Tok* toks, size_t count) : m_toks(toks), m_count(count) {}
    const Tok& peek(size_t ahead = 0) const {
        size_t i = m_idx + ahead;
        return i < m_count ? m_toks[i] : kEnd;
    }
    void advance() {
        if (m_idx < m_count) {
            ++m_idx;
        }
    }
    bool atEnd() const { return m_idx >= m_count; }

private:
    static const Tok kEnd;
    const Tok* m_toks;
    size_t m_count;
    size_t m_idx = 0;
};
const Tok Cursor::kEnd{};

/// px/%/em/rem/auto, plus unitless 0 (-> 0px). Mirrors xts_values.cpp's parseLengthTok.
bool parseLengthTok(const Tok& tok, Length& out) {
    if (tok.kind == TokKind::Ident && iequals(tok.text, "auto")) {
        out = Length{0, LengthUnit::Auto};
        return true;
    }
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (tok.unit == "px") {
        out = Length{tok.num, LengthUnit::Px};
        return true;
    }
    if (tok.unit == "%") {
        out = Length{tok.num, LengthUnit::Percent};
        return true;
    }
    if (tok.unit == "em") {
        out = Length{tok.num, LengthUnit::Em};
        return true;
    }
    if (tok.unit == "rem") {  // T-335: resolved against the root's font size at style time.
        out = Length{tok.num, LengthUnit::Rem};
        return true;
    }
    if (tok.unit.empty() && tok.num == 0) {
        out = Length{0, LengthUnit::Px};
        return true;
    }
    return false;
}

bool parseFloatTok(const Tok& tok, float& out) {
    if (tok.kind != TokKind::Number || !tok.unit.empty()) {
        return false;
    }
    out = tok.num;
    return true;
}

/// T-328 (06 §6.16): `aspect-ratio: auto | <w> / <h> | <number>` as one width / height Float,
/// `auto` = 0; the compiler's parseAspectRatio (compile/src/xts/xts_values.cpp) has the same grammar.
bool parseAspectRatio(const Tok* toks, size_t count, float& out) {
    const auto positive = [](const Tok& t, float& v) { return parseFloatTok(t, v) && v > 0.0f; };
    if (count == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "auto") {
        out = 0.0f;
        return true;
    }
    float w = 0.0f;
    float h = 1.0f;
    if (count == 1) {
        if (!positive(toks[0], w)) {
            return false;
        }
        out = w;
        return true;
    }
    if (count != 3 || !positive(toks[0], w) || toks[1].kind != TokKind::Slash || !positive(toks[2], h)) {
        return false;
    }
    out = w / h;
    return true;
}

bool parseAngleTok(const Tok& tok, float& deg) {
    if (tok.kind != TokKind::Number || tok.unit != "deg") {
        return false;
    }
    deg = tok.num;
    return true;
}

uint8_t hexNibble(char c, bool& ok) {
    if (c >= '0' && c <= '9') {
        return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<uint8_t>(c - 'A' + 10);
    }
    ok = false;
    return 0;
}

uint8_t hexByte(char hi, char lo, bool& ok) {
    return static_cast<uint8_t>((hexNibble(hi, ok) << 4) | hexNibble(lo, ok));
}

bool parseHashColor(std::string_view hex, Color& out) {
    bool ok = true;
    for (char c : hex) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    const float kNibble = 17.0f / 255.0f;
    if (hex.size() == 3 || hex.size() == 4) {
        out.r = static_cast<float>(hexNibble(hex[0], ok)) * kNibble;
        out.g = static_cast<float>(hexNibble(hex[1], ok)) * kNibble;
        out.b = static_cast<float>(hexNibble(hex[2], ok)) * kNibble;
        out.a = hex.size() == 4 ? static_cast<float>(hexNibble(hex[3], ok)) * kNibble : 1.0f;
    } else if (hex.size() == 6 || hex.size() == 8) {
        out.r = static_cast<float>(hexByte(hex[0], hex[1], ok)) / 255.0f;
        out.g = static_cast<float>(hexByte(hex[2], hex[3], ok)) / 255.0f;
        out.b = static_cast<float>(hexByte(hex[4], hex[5], ok)) / 255.0f;
        out.a = hex.size() == 8 ? static_cast<float>(hexByte(hex[6], hex[7], ok)) / 255.0f : 1.0f;
    } else {
        return false;
    }
    return ok;
}

/// r/g/b: 0..255 or 0..100%. Returns the channel in [0, 1].
bool parseRgbChannel(const Tok& tok, float& out) {
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (tok.unit == "%") {
        out = tok.num / 100.0f;
        return true;
    }
    if (tok.unit.empty()) {
        out = tok.num / 255.0f;
        return true;
    }
    return false;
}

/// T-322: an `hsl()` hue: a number (degrees), `deg`, `turn` or `rad`. Mirrors xts_values.cpp.
bool parseHueTok(const Tok& t, float& deg) {
    if (t.kind != TokKind::Number) {
        return false;
    }
    if (t.unit.empty() || t.unit == "deg") {
        deg = t.num;
        return true;
    }
    if (t.unit == "turn") {
        deg = t.num * 360.0f;
        return true;
    }
    if (t.unit == "rad") {
        deg = t.num * 57.29577951308232f;
        return true;
    }
    return false;
}

/// T-322: an `hsl()` alpha: a number (0-1) or a percentage.
bool parseAlphaTok(const Tok& t, float& a) {
    if (t.kind != TokKind::Number) {
        return false;
    }
    if (t.unit.empty()) {
        a = t.num;
        return true;
    }
    if (t.unit == "%") {
        a = t.num / 100.0f;
        return true;
    }
    return false;
}

/// T-322: the arguments of `hsl()`/`hsla()` (aliases), `cur` at the '(': `(h, s%, l%[, a])` or
/// `(h s% l%[ / a])`. Mirrors xts_values.cpp's parseHslArgs().
bool parseHslArgs(Cursor& cur, Color& out) {
    cur.advance();  // '('
    float h = 0;
    if (!parseHueTok(cur.peek(), h)) {
        return false;
    }
    cur.advance();
    const bool legacy = cur.peek().kind == TokKind::Comma;
    float sl[2] = {0, 0};
    for (float& v : sl) {
        if (legacy) {
            if (cur.peek().kind != TokKind::Comma) {
                return false;
            }
            cur.advance();
        }
        const Tok& t = cur.peek();
        if (t.kind != TokKind::Number || t.unit != "%") {
            return false;
        }
        v = t.num / 100.0f;
        cur.advance();
    }
    float a = 1;
    if (cur.peek().kind == (legacy ? TokKind::Comma : TokKind::Slash)) {
        cur.advance();
        if (!parseAlphaTok(cur.peek(), a)) {
            return false;
        }
        cur.advance();
    }
    if (cur.peek().kind != TokKind::RParen) {
        return false;
    }
    cur.advance();
    out = css_color::hslToColor(h, sl[0], sl[1], a);
    return true;
}

bool parseColorCursor(Cursor& cur, Color& out) {
    const Tok& t = cur.peek();
    if (t.kind == TokKind::Hash) {
        Color c;
        if (!parseHashColor(t.text, c)) {
            return false;
        }
        cur.advance();
        out = c;
        return true;
    }
    if (t.kind != TokKind::Ident) {
        return false;
    }
    if (iequals(t.text, "transparent")) {
        cur.advance();
        out = Color{0, 0, 0, 0};
        return true;
    }
    if (iequals(t.text, "rgb") || iequals(t.text, "rgba")) {
        bool wantAlpha = iequals(t.text, "rgba");
        cur.advance();
        if (cur.peek().kind != TokKind::LParen) {
            return false;
        }
        cur.advance();
        float ch[4] = {0, 0, 0, 1};
        int need = wantAlpha ? 4 : 3;
        for (int i = 0; i < need; ++i) {
            if (i > 0) {
                if (cur.peek().kind != TokKind::Comma) {
                    return false;
                }
                cur.advance();
            }
            if (i < 3) {
                if (!parseRgbChannel(cur.peek(), ch[i])) {
                    return false;
                }
            } else {
                const Tok& a = cur.peek();
                if (a.kind != TokKind::Number || !a.unit.empty()) {
                    return false;
                }
                ch[3] = a.num;
            }
            cur.advance();
        }
        if (cur.peek().kind != TokKind::RParen) {
            return false;
        }
        cur.advance();
        out = Color{ch[0], ch[1], ch[2], ch[3]};
        return true;
    }
    if ((iequals(t.text, "hsl") || iequals(t.text, "hsla")) && cur.peek(1).kind == TokKind::LParen) {
        cur.advance();
        return parseHslArgs(cur, out);
    }
    // T-322: the CSS Color Level 4 named colours (core/src/css_color.h).
    Color named;
    if (css_color::findNamedColor(t.text, named)) {
        cur.advance();
        out = named;
        return true;
    }
    return false;
}

/// 05.5/19.10's category order (T-47a): translate*(0) -> rotateX(1) -> rotateY(2) -> rotate(3) ->
/// skew(4) -> scale(5). Same table as the compiler's transformCategory()
/// (compile/src/xts/xts_values.cpp).
int transformCategory(std::string_view fn) {
    if (fn == "translate" || fn == "translateX" || fn == "translateY" || fn == "translateZ" ||
        fn == "translate3d") {
        return 0;
    }
    if (fn == "rotateX") {
        return 1;
    }
    if (fn == "rotateY") {
        return 2;
    }
    if (fn == "rotate" || fn == "rotateZ") {  // T-322: rotateZ() is rotate().
        return 3;
    }
    if (fn == "skew" || fn == "skewX" || fn == "skewY") {
        return 4;
    }
    if (fn == "scale" || fn == "scaleX" || fn == "scaleY") {
        return 5;
    }
    return -1;
}

/// 05.5/19.10 (T-47a): explicitly rejected 3D functions -- treated as "unknown" here, same as
/// the compiler's dedicated error (mini_css has no DiagnosticSink to carry the distinct
/// message; the caller only sees kInvalidArgument either way).
bool transformIsUnsupported(std::string_view fn) {
    return fn == "rotate3d" || fn == "scale3d" || fn == "matrix3d" || fn == "perspective";
}

/// 05.5's fast path: functions in translate* -> rotateX -> rotateY -> rotate -> skew -> scale
/// order, each category at most once, decomposed into one Decl per PropId. Byte-identical to the
/// pre-T-120 parser.
bool parseTransformFixedOrder(const Tok* toks, size_t count, std::vector<Decl>& out) {
    auto category = transformCategory;
    auto isUnsupported = transformIsUnsupported;
    Cursor cur(toks, count);
    if (cur.atEnd()) {
        return false;
    }
    // 05.5's `none` clause (T-69): the empty sequence, i.e. every component at its identity. The
    // identity declarations are written out explicitly so `setStyle("transform: none")` also
    // clears a transform set earlier. Mixing `none` with functions falls through to the loop below
    // and is rejected there.
    if (count == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "none") {
        auto pushLenId = [&out](uint16_t p) {
            Decl d;
            d.prop = p;
            d.value = Value::ofLength(0.0f, LengthUnit::Px);
            out.push_back(d);
        };
        auto pushFloatId = [&out](uint16_t p, float v) {
            Decl d;
            d.prop = p;
            d.value = Value::ofFloat(v);
            out.push_back(d);
        };
        pushLenId(prop::TranslateX);
        pushLenId(prop::TranslateY);
        // TranslateZ is a Float scene property (19.3/T-43c); the runtime cascade only accepts a
        // Float value for it, so the identity is written as one.
        pushFloatId(prop::TranslateZ, 0.0f);
        pushFloatId(prop::RotationX, 0.0f);
        pushFloatId(prop::RotationY, 0.0f);
        pushFloatId(prop::Rotation, 0.0f);
        pushFloatId(prop::SkewX, 0.0f);
        pushFloatId(prop::SkewY, 0.0f);
        pushFloatId(prop::ScaleX, 1.0f);
        pushFloatId(prop::ScaleY, 1.0f);
        return true;
    }
    int lastCategory = -1;
    bool sawCategory[6] = {false, false, false, false, false, false};
    std::vector<Decl> decls;
    while (!cur.atEnd()) {
        const Tok& fnTok = cur.peek();
        if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
            return false;
        }
        std::string_view fn = fnTok.text;
        if (isUnsupported(fn)) {
            return false;
        }
        int cat = category(fn);
        if (cat < 0 || sawCategory[cat] || cat < lastCategory) {
            return false;  // Unknown function, repeated category, or wrong order (05.5).
        }
        sawCategory[cat] = true;
        lastCategory = cat;
        cur.advance();  // function name
        cur.advance();  // '('

        auto pushLen = [&decls](uint16_t p, Length len) {
            Decl d;
            d.prop = p;
            d.value = Value::ofLength(len.value, len.unit);
            decls.push_back(d);
        };
        auto pushFloat = [&decls](uint16_t p, float v) {
            Decl d;
            d.prop = p;
            d.value = Value::ofFloat(v);
            decls.push_back(d);
        };
        auto readLength = [&cur](Length& len) -> bool {
            if (!parseLengthTok(cur.peek(), len) || len.unit == LengthUnit::Auto) {
                return false;
            }
            cur.advance();
            return true;
        };
        // translateZ/translate3d's z component: 05.5/19.10 rejects percent (px/em only).
        auto readZLength = [&cur](Length& len) -> bool {
            if (!parseLengthTok(cur.peek(), len) || len.unit == LengthUnit::Auto ||
                len.unit == LengthUnit::Percent) {
                return false;
            }
            cur.advance();
            return true;
        };
        auto readAngle = [&cur](float& deg) -> bool {
            if (!parseAngleTok(cur.peek(), deg)) {
                return false;
            }
            cur.advance();
            return true;
        };
        auto readFloat = [&cur](float& v) -> bool {
            if (!parseFloatTok(cur.peek(), v)) {
                return false;
            }
            cur.advance();
            return true;
        };

        bool ok = true;
        if (fn == "translate") {
            Length x{}, y{};
            ok = readLength(x);
            if (ok) {
                pushLen(prop::TranslateX, x);
                if (cur.peek().kind == TokKind::Comma) {
                    cur.advance();
                    ok = readLength(y);
                    if (ok) {
                        pushLen(prop::TranslateY, y);
                    }
                }
            }
        } else if (fn == "translateX" || fn == "translateY") {
            Length v{};
            ok = readLength(v);
            if (ok) {
                pushLen(fn == "translateX" ? prop::TranslateX : prop::TranslateY, v);
            }
        } else if (fn == "translateZ") {
            Length z{};
            ok = readZLength(z);
            if (ok) {
                pushLen(prop::TranslateZ, z);
            }
        } else if (fn == "translate3d") {
            Length x{}, y{}, z{};
            ok = readLength(x);
            if (ok && cur.peek().kind == TokKind::Comma) {
                cur.advance();
                ok = readLength(y);
            } else {
                ok = false;
            }
            if (ok && cur.peek().kind == TokKind::Comma) {
                cur.advance();
                ok = readZLength(z);
            } else {
                ok = false;
            }
            if (ok) {
                pushLen(prop::TranslateX, x);
                pushLen(prop::TranslateY, y);
                pushLen(prop::TranslateZ, z);
            }
        } else if (fn == "rotateX") {
            float deg = 0;
            ok = readAngle(deg);
            if (ok) {
                pushFloat(prop::RotationX, deg);
            }
        } else if (fn == "rotateY") {
            float deg = 0;
            ok = readAngle(deg);
            if (ok) {
                pushFloat(prop::RotationY, deg);
            }
        } else if (fn == "rotate" || fn == "rotateZ") {
            float deg = 0;
            ok = readAngle(deg);
            if (ok) {
                pushFloat(prop::Rotation, deg);
            }
        } else if (fn == "skew") {
            float x = 0, y = 0;
            ok = readAngle(x);
            if (ok) {
                pushFloat(prop::SkewX, x);
                if (cur.peek().kind == TokKind::Comma) {
                    cur.advance();
                    ok = readAngle(y);
                    if (ok) {
                        pushFloat(prop::SkewY, y);
                    }
                }
            }
        } else if (fn == "skewX" || fn == "skewY") {
            float v = 0;
            ok = readAngle(v);
            if (ok) {
                pushFloat(fn == "skewX" ? prop::SkewX : prop::SkewY, v);
            }
        } else if (fn == "scale") {
            float x = 0, y = 0;
            ok = readFloat(x);
            if (ok) {
                y = x;
                if (cur.peek().kind == TokKind::Comma) {
                    cur.advance();
                    ok = readFloat(y);
                }
                if (ok) {
                    pushFloat(prop::ScaleX, x);
                    pushFloat(prop::ScaleY, y);
                }
            }
        } else if (fn == "scaleX" || fn == "scaleY") {
            float v = 0;
            ok = readFloat(v);
            if (ok) {
                pushFloat(fn == "scaleX" ? prop::ScaleX : prop::ScaleY, v);
            }
        }
        if (!ok || cur.peek().kind != TokKind::RParen) {
            return false;
        }
        cur.advance();  // ')'
    }
    for (Decl& d : decls) {
        out.push_back(std::move(d));
    }
    return true;
}

/// Pre-pass mirroring the compiler's transformNeedsCompose(): true when the sequence repeats a
/// category or breaks the fixed order. Anything malformed returns false so the fast path rejects
/// it exactly as before.
bool transformNeedsCompose(const Tok* toks, size_t count) {
    int lastCategory = -1;
    bool sawCategory[6] = {false, false, false, false, false, false};
    bool needs = false;
    size_t i = 0;
    while (i < count) {
        if (toks[i].kind != TokKind::Ident || i + 1 >= count || toks[i + 1].kind != TokKind::LParen) {
            return false;
        }
        if (toks[i].text == "matrix") {
            needs = true;  // T-322: a 2D matrix() only ever goes through compose/decompose.
        } else {
            int cat = transformCategory(toks[i].text);
            if (cat < 0) {
                return false;
            }
            if (sawCategory[cat] || cat < lastCategory) {
                needs = true;
            }
            sawCategory[cat] = true;
            lastCategory = cat;
        }
        i += 2;  // function name, '('
        while (i < count && toks[i].kind != TokKind::RParen) {
            ++i;
        }
        if (i >= count) {
            return false;  // Unterminated argument list.
        }
        ++i;  // ')'
    }
    return needs;
}

/// 05.5's slow path (T-120): compose the sequence into one 4x4 matrix and decompose it back into
/// 03.3's components, so `setStyle("transform: rotateY(18deg) translateZ(40px)")` works. Mirrors
/// compile/src/xts/xts_values.cpp's parseTransformComposed(); lengths must be px, since `%`/`em`
/// cannot be folded into a matrix before layout. Every rejection is just `false`
/// (Status::kInvalidArgument), as everywhere else in this file, and 05.5's ">89 degree skew"
/// warning has no sink to go to here, so it is dropped.
bool parseTransformComposed(const Tok* toks, size_t count, std::vector<Decl>& out) {
    Cursor cur(toks, count);
    std::vector<TransformFn> fns;
    while (!cur.atEnd()) {
        std::string_view fn = cur.peek().text;
        cur.advance();  // function name
        cur.advance();  // '(' (transformNeedsCompose() already checked both)
        auto readPx = [&cur](float& v) -> bool {
            Length len{};
            if (!parseLengthTok(cur.peek(), len) || len.unit != LengthUnit::Px) {
                return false;
            }
            v = len.value;
            cur.advance();
            return true;
        };
        auto readAngle = [&cur](float& deg) -> bool {
            if (!parseAngleTok(cur.peek(), deg)) {
                return false;
            }
            cur.advance();
            return true;
        };
        auto readFloat = [&cur](float& v) -> bool {
            if (!parseFloatTok(cur.peek(), v)) {
                return false;
            }
            cur.advance();
            return true;
        };
        auto comma = [&cur]() -> bool {
            if (cur.peek().kind != TokKind::Comma) {
                return false;
            }
            cur.advance();
            return true;
        };
        TransformFn f;
        bool ok = true;
        if (transformCategory(fn) == 0) {
            f.kind = TransformFn::Kind::Translate;
            float v = 0;
            ok = readPx(v);
            if (ok) {
                if (fn == "translateY") {
                    f.b = v;
                } else if (fn == "translateZ") {
                    f.c = v;
                } else {
                    f.a = v;
                    if (fn == "translate3d") {
                        ok = comma() && readPx(f.b) && comma() && readPx(f.c);
                    } else if (fn == "translate" && cur.peek().kind == TokKind::Comma) {
                        cur.advance();
                        ok = readPx(f.b);
                    }
                }
            }
        } else if (fn == "rotateX" || fn == "rotateY" || fn == "rotate" || fn == "rotateZ") {
            f.kind = fn == "rotateX"   ? TransformFn::Kind::RotateX
                     : fn == "rotateY" ? TransformFn::Kind::RotateY
                                       : TransformFn::Kind::RotateZ;
            ok = readAngle(f.a);
        } else if (fn == "matrix") {
            // T-322: like the compiler, a singular matrix() is dropped (no sink for its warning).
            float m[6] = {1, 0, 0, 1, 0, 0};
            for (int k = 0; ok && k < 6; ++k) {
                ok = (k == 0 || comma()) && readFloat(m[k]);
            }
            if (ok && cur.peek().kind == TokKind::RParen) {
                cur.advance();  // ')'
                TransformFn parts[4];
                if (decomposeMatrix2D(m[0], m[1], m[2], m[3], m[4], m[5], parts)) {
                    fns.insert(fns.end(), parts, parts + 4);
                }
                continue;
            }
        } else if (transformCategory(fn) == 4) {
            f.kind = TransformFn::Kind::Skew;
            float v = 0;
            ok = readAngle(v);
            if (ok) {
                if (fn == "skewY") {
                    f.b = v;
                } else {
                    f.a = v;
                    if (fn == "skew" && cur.peek().kind == TokKind::Comma) {
                        cur.advance();
                        ok = readAngle(f.b);
                    }
                }
            }
        } else {  // scale / scaleX / scaleY
            f.kind = TransformFn::Kind::Scale;
            f.a = 1;
            f.b = 1;
            float v = 1;
            ok = readFloat(v);
            if (ok) {
                if (fn == "scaleX") {
                    f.a = v;
                } else if (fn == "scaleY") {
                    f.b = v;
                } else {
                    f.a = v;
                    f.b = v;
                    if (cur.peek().kind == TokKind::Comma) {
                        cur.advance();
                        ok = readFloat(f.b);
                    }
                }
            }
        }
        if (!ok || cur.peek().kind != TokKind::RParen) {
            return false;
        }
        cur.advance();  // ')'
        fns.push_back(f);
    }
    TransformComponents c;
    if (decomposeTransformSequence(fns.data(), fns.size(), c) != TransformDecomposeStatus::Ok) {
        return false;  // 05.5: needs a 3D shear or a z scale.
    }
    auto pushLen = [&out](uint16_t p, float v) {
        Decl d;
        d.prop = p;
        d.value = Value::ofLength(v, LengthUnit::Px);
        out.push_back(d);
    };
    auto pushFloat = [&out](uint16_t p, float v) {
        Decl d;
        d.prop = p;
        d.value = Value::ofFloat(v);
        out.push_back(d);
    };
    pushLen(prop::TranslateX, c.translateX);
    pushLen(prop::TranslateY, c.translateY);
    // TranslateZ is a Float scene property (19.3/T-43c); the runtime cascade only accepts Float.
    pushFloat(prop::TranslateZ, c.translateZ);
    pushFloat(prop::RotationX, c.rotationX);
    pushFloat(prop::RotationY, c.rotationY);
    pushFloat(prop::Rotation, c.rotation);
    pushFloat(prop::SkewX, c.skewX);
    pushFloat(prop::SkewY, c.skewY);
    pushFloat(prop::ScaleX, c.scaleX);
    pushFloat(prop::ScaleY, c.scaleY);
    return true;
}

/// 05.5's `transform` sequence: the fixed-order fast path, or the compose/decompose slow path for
/// any other order (T-120).
bool parseTransform(const Tok* toks, size_t count, std::vector<Decl>& out) {
    if (transformNeedsCompose(toks, count)) {
        return parseTransformComposed(toks, count, out);
    }
    return parseTransformFixedOrder(toks, count, out);
}

/// T-329 (05 §5.9.8): `translate` / `rotate` / `scale` as the equivalent `transform` function
/// tokens (mirrors compile/src/xts/xts_values.cpp's independentTransformFnTokens()). `none` gives
/// an empty list and sets `isNone`. The argument tokens are copied, so `fn` views the same text.
bool independentTransformFnToks(uint16_t which, const Tok* toks, size_t count, std::vector<Tok>& fn,
                                bool& isNone) {
    fn.clear();
    isNone = false;
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "none")) {
        isNone = true;
        return true;
    }
    auto append = [&fn](std::string_view name, const Tok* args, size_t argCount, const Tok* extra) {
        Tok t;
        t.kind = TokKind::Ident;
        t.text = name;
        fn.push_back(t);
        t = Tok{};
        t.kind = TokKind::LParen;
        fn.push_back(t);
        for (size_t i = 0; i < argCount + (extra != nullptr ? 1 : 0); ++i) {
            if (i > 0) {
                Tok comma;
                comma.kind = TokKind::Comma;
                fn.push_back(comma);
            }
            fn.push_back(i < argCount ? args[i] : *extra);
        }
        t = Tok{};
        t.kind = TokKind::RParen;
        fn.push_back(t);
    };
    if (which == kTranslateProp) {
        // `<x> [<y> [<z>]]`: x/y px/%/em; z px/em. A missing y is 0, so both axes are set.
        if (count == 0 || count > 3) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            Length len{};
            if (!parseLengthTok(toks[i], len) || len.unit == LengthUnit::Auto ||
                (i == 2 && len.unit == LengthUnit::Percent)) {
                return false;
            }
        }
        Tok zero;
        zero.kind = TokKind::Number;
        zero.unit = "px";
        append(count == 3 ? "translate3d" : "translate", toks, count, count == 1 ? &zero : nullptr);
        return true;
    }
    if (which == kRotateProp) {
        // `<angle> | z <angle>`; `x`/`y` stay transform's rotateX()/rotateY() (19 §19.10).
        size_t i = 0;
        if (count > 0 && toks[0].kind == TokKind::Ident) {
            if (!iequals(toks[0].text, "z")) {
                return false;
            }
            i = 1;
        }
        float deg = 0;
        if (count != i + 1 || !parseAngleTok(toks[i], deg)) {
            return false;
        }
        append("rotate", toks + i, 1, nullptr);
        return true;
    }
    // scale: `<sx> [<sy>]`, unitless; a missing sy is sx.
    if (count == 0 || count > 2) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        float v = 0;
        if (!parseFloatTok(toks[i], v)) {
            return false;
        }
    }
    append("scale", toks, count, nullptr);
    return true;
}

/// T-329: one independent property on its own writes the existing PropIds straight through
/// (05.5's fixed-order path); `none` writes only that property's own components at identity.
bool parseIndependentTransform(uint16_t which, const Tok* toks, size_t count, std::vector<Decl>& out) {
    std::vector<Tok> fn;
    bool isNone = false;
    if (!independentTransformFnToks(which, toks, count, fn, isNone)) {
        return false;
    }
    if (!isNone) {
        return parseTransformFixedOrder(fn.data(), fn.size(), out);
    }
    auto push = [&out](uint16_t p, Value v) {
        Decl d;
        d.prop = p;
        d.value = v;
        out.push_back(d);
    };
    if (which == kTranslateProp) {
        push(prop::TranslateX, Value::ofLength(0.0f, LengthUnit::Px));
        push(prop::TranslateY, Value::ofLength(0.0f, LengthUnit::Px));
        push(prop::TranslateZ, Value::ofFloat(0.0f));  // A Float scene property (19.3/T-43c).
    } else if (which == kRotateProp) {
        push(prop::Rotation, Value::ofFloat(0.0f));
    } else {
        push(prop::ScaleX, Value::ofFloat(1.0f));
        push(prop::ScaleY, Value::ofFloat(1.0f));
    }
    return true;
}

/// The PropIds `transform` and the independent transform properties write.
bool isTransformComponentProp(uint16_t p) {
    return p == prop::TranslateX || p == prop::TranslateY || p == prop::TranslateZ || p == prop::RotationX ||
           p == prop::RotationY || p == prop::Rotation || p == prop::SkewX || p == prop::SkewY ||
           p == prop::ScaleX || p == prop::ScaleY;
}

// ---------------------------------------------------------------------------------------------
// `effect` (docs/design/18-effects.md §18.3-18.4, docs/tasks/T-51a.md/T-51b.md). Mirrors the
// compiler's grammar and parameter mapping (compile/src/xts/xts_values.cpp's parseEffectList()
// and its per-function helpers) exactly, ported to this file's Tok/Cursor/EffectInstance types --
// the same "independent copy, no DiagnosticSink" approach `transform` above already uses (T-35):
// every parse failure (including `%`/`auto` lengths and `custom()`) is just kInvalidArgument, with
// no distinct diagnostic message.
// ---------------------------------------------------------------------------------------------

constexpr size_t kMaxEffects = 16;            // 18.2: "每節點 16 個效果".
constexpr size_t kMaxGradientMaskStops = 16;  // 18.2: "每個 GradientMask 16 個停靠點".
// Compile-time-only em -> px rule (matches xts_values.cpp's kEffectEmPx and its rationale):
// EffectInstance stores plain floats with no unit tag, so there's no later stage to resolve em
// against the element's actual font-size the way other Length-typed props do.
constexpr float kEffectEmPx = 16.0f;

/// Effect lengths (18.4): px/em only. `%`/`auto` and anything else fail (mini_css has no
/// DiagnosticSink to carry a distinct "percent not allowed" message, unlike the compiler's
/// readEffectLen/percentError -- see the file comment above).
bool readEffectLen(Cursor& cur, float& outPx) {
    Length len{};
    if (!parseLengthTok(cur.peek(), len) || len.unit == LengthUnit::Percent || len.unit == LengthUnit::Auto ||
        len.unit == LengthUnit::Rem) {  // T-335: effect lengths stay px/em, as the compiler's.
        return false;
    }
    cur.advance();
    outPx = len.unit == LengthUnit::Em ? len.value * kEffectEmPx : len.value;
    return true;
}

/// True for a token that could start an effect-length argument (mirrors xts_values.cpp's
/// isEffectLenToken(): used to detect shadow()'s optional blur/spread lengths without also
/// matching a following bare number, which is never a length there).
bool isEffectLenToken(const Tok& t) {
    return t.kind == TokKind::Number && (t.unit == "px" || t.unit == "em" || (t.unit.empty() && t.num == 0));
}

/// Consumes tokens between a matching parenthesis pair (`cur` must be positioned at the opening
/// '('), returning the tokens strictly inside. Leaves `cur` positioned right after the closing
/// ')'. Mirrors xts_values.cpp's extractParenGroup().
bool extractParenGroup(Cursor& cur, std::vector<Tok>& inner) {
    if (cur.peek().kind != TokKind::LParen) {
        return false;
    }
    cur.advance();
    int depth = 1;
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
        if (t.kind == TokKind::LParen) {
            ++depth;
            inner.push_back(t);
            cur.advance();
            continue;
        }
        if (t.kind == TokKind::RParen) {
            --depth;
            if (depth == 0) {
                cur.advance();
                return true;
            }
            inner.push_back(t);
            cur.advance();
            continue;
        }
        inner.push_back(t);
        cur.advance();
    }
    return false;  // Unterminated.
}

/// Splits `toks` on top-level commas only (paren depth 0); a comma inside a nested call (e.g. a
/// color-stop's `rgba(...)`) doesn't split. Mirrors xts_values.cpp's splitByCommaTopLevel().
std::vector<std::vector<Tok>> splitByCommaTopLevel(const std::vector<Tok>& toks) {
    std::vector<std::vector<Tok>> items;
    std::vector<Tok> cur;
    int depth = 0;
    for (const Tok& t : toks) {
        if (t.kind == TokKind::LParen) {
            ++depth;
        } else if (t.kind == TokKind::RParen) {
            --depth;
        }
        if (t.kind == TokKind::Comma && depth == 0) {
            items.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(t);
        }
    }
    items.push_back(std::move(cur));
    return items;
}

/// `<color-stop>+` for `gradient-mask()`: `<color> <percent>`, percent required (mirrors
/// xts_values.cpp's parseColorStopList()).
bool parseColorStopList(const std::vector<std::vector<Tok>>& items, std::vector<GradientStop>& out) {
    if (items.empty() || items.size() > kMaxGradientMaskStops) {
        return false;
    }
    for (const std::vector<Tok>& item : items) {
        Cursor cur(item.data(), item.size());
        Color c{};
        if (!parseColorCursor(cur, c)) {
            return false;
        }
        if (cur.atEnd() || cur.peek().kind != TokKind::Number || cur.peek().unit != "%") {
            return false;
        }
        float pct = cur.peek().num;
        cur.advance();
        if (!cur.atEnd()) {
            return false;
        }
        out.push_back(GradientStop{pct / 100.0f, c});
    }
    return true;
}

/// shadow( <len> <len> [<len> [<len>]] <color> [<number>] [inset] ) -> DropShadow. Mirrors
/// xts_values.cpp's parseShadowFn().
bool parseShadowFn(const std::vector<Tok>& inner, EffectInstance& out) {
    out.kind = EffectKind::DropShadow;
    Cursor cur(inner.data(), inner.size());
    float offX, offY;
    if (!readEffectLen(cur, offX) || !readEffectLen(cur, offY)) {
        return false;
    }
    float extra[2] = {0, 0};
    int extraCount = 0;
    while (extraCount < 2 && isEffectLenToken(cur.peek())) {
        if (!readEffectLen(cur, extra[static_cast<size_t>(extraCount)])) {
            return false;
        }
        ++extraCount;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float opacity = 1;
    if (!cur.atEnd() && cur.peek().kind == TokKind::Number) {
        if (!cur.peek().unit.empty()) {
            return false;
        }
        opacity = cur.peek().num;
        cur.advance();
    }
    // Trailing `inset` keyword (18.4); only valid last, so anything after it is an error.
    float insetFlag = 0;
    if (!cur.atEnd() && cur.peek().kind == TokKind::Ident && cur.peek().text == "inset") {
        insetFlag = 1;
        cur.advance();
    }
    if (!cur.atEnd()) {
        return false;
    }
    out.params[0] = offX;
    out.params[1] = offY;
    out.params[2] = extra[0];
    out.params[3] = extra[1];
    out.params[4] = opacity;
    out.params[5] = insetFlag;
    out.colors[0] = color;
    return true;
}

/// glow( <len> <color> [<number>] [inner] ) -> Glow. Mirrors xts_values.cpp's parseGlowFn().
bool parseGlowFn(const std::vector<Tok>& inner, EffectInstance& out) {
    out.kind = EffectKind::Glow;
    Cursor cur(inner.data(), inner.size());
    float radius;
    if (!readEffectLen(cur, radius)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float intensity = 1;
    if (!cur.atEnd() && cur.peek().kind == TokKind::Number && cur.peek().unit.empty()) {
        intensity = cur.peek().num;
        cur.advance();
    }
    float innerFlag = 0;
    if (!cur.atEnd() && cur.peek().kind == TokKind::Ident && cur.peek().text == "inner") {
        innerFlag = 1;
        cur.advance();
    }
    if (!cur.atEnd()) {
        return false;
    }
    out.params[0] = radius;
    out.params[1] = intensity;
    out.params[2] = innerFlag;
    out.colors[0] = color;
    return true;
}

/// blur( <len> [<len>] ) -> Blur. Mirrors xts_values.cpp's parseBlurFn().
bool parseBlurFn(const std::vector<Tok>& inner, EffectInstance& out) {
    out.kind = EffectKind::Blur;
    Cursor cur(inner.data(), inner.size());
    float rx;
    if (!readEffectLen(cur, rx)) {
        return false;
    }
    float ry = rx;
    if (!cur.atEnd() && !readEffectLen(cur, ry)) {
        return false;
    }
    if (!cur.atEnd()) {
        return false;
    }
    out.params[0] = rx;
    out.params[1] = ry;
    return true;
}

/// color-adjust( [brightness(n)] [contrast(n)] [saturate(n)] [hue-rotate(a)] [opacity(n)]
/// [invert(n|%)] [sepia(n|%)] ) ->
/// ColorAdjust, each sub-function optional and at most once, any order. Mirrors xts_values.cpp's
/// parseColorAdjustFn().
bool parseColorAdjustFn(const std::vector<Tok>& inner, EffectInstance& out) {
    out.kind = EffectKind::ColorAdjust;
    out.params[0] = 0;
    out.params[1] = 0;
    out.params[2] = 0;
    out.params[3] = 0;
    out.params[4] = 1;
    out.params[5] = 0;
    out.params[6] = 0;
    bool saw[7] = {false, false, false, false, false, false, false};
    Cursor cur(inner.data(), inner.size());
    while (!cur.atEnd()) {
        const Tok& fnTok = cur.peek();
        if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
            return false;
        }
        std::string_view fn = fnTok.text;
        int idx = fn == "brightness"   ? 0
                  : fn == "contrast"   ? 1
                  : fn == "saturate"   ? 2
                  : fn == "hue-rotate" ? 3
                  : fn == "opacity"    ? 4
                  : fn == "invert"     ? 5
                  : fn == "sepia"      ? 6
                                       : -1;
        if (idx < 0 || saw[static_cast<size_t>(idx)]) {
            return false;
        }
        saw[static_cast<size_t>(idx)] = true;
        cur.advance();
        std::vector<Tok> args;
        if (!extractParenGroup(cur, args)) {
            return false;
        }
        if (idx >= 5) {
            // T-334 (05 §5.9.8): invert()/sepia() take <number> or <percentage>, default 1 when
            // empty, clamped to 0..1 like CSS Filter Effects' own amounts.
            float amount = 1.0f;
            if (args.size() == 1) {
                const Tok& a = args[0];
                if (a.kind != TokKind::Number || (!a.unit.empty() && a.unit != "%")) {
                    return false;
                }
                amount = a.unit == "%" ? a.num / 100.0f : a.num;
            } else if (!args.empty()) {
                return false;
            }
            out.params[static_cast<size_t>(idx)] = std::clamp(amount, 0.0f, 1.0f);
            continue;
        }
        if (args.size() != 1) {
            return false;
        }
        float v;
        bool ok = idx == 3 ? parseAngleTok(args[0], v) : parseFloatTok(args[0], v);
        if (!ok) {
            return false;
        }
        out.params[static_cast<size_t>(idx)] = v;
    }
    return true;
}

/// gradient-mask( linear|radial , <len> <len> , <len> <len> , <color-stop>+ [, invert] ) ->
/// GradientMask. Mirrors xts_values.cpp's parseGradientMaskFn().
bool parseGradientMaskFn(const std::vector<Tok>& inner, EffectInstance& out,
                         std::vector<GradientStop>& stops) {
    out.kind = EffectKind::GradientMask;
    std::vector<std::vector<Tok>> items = splitByCommaTopLevel(inner);
    if (items.size() < 4) {
        return false;
    }
    if (items[0].size() != 1 || items[0][0].kind != TokKind::Ident) {
        return false;
    }
    float radial;
    if (items[0][0].text == "linear") {
        radial = 0;
    } else if (items[0][0].text == "radial") {
        radial = 1;
    } else {
        return false;
    }
    if (items[1].size() != 2 || items[2].size() != 2) {
        return false;
    }
    Cursor p0cur(items[1].data(), items[1].size());
    float p0x, p0y;
    if (!readEffectLen(p0cur, p0x) || !readEffectLen(p0cur, p0y) || !p0cur.atEnd()) {
        return false;
    }
    Cursor p1cur(items[2].data(), items[2].size());
    float p1x, p1y;
    if (!readEffectLen(p1cur, p1x) || !readEffectLen(p1cur, p1y) || !p1cur.atEnd()) {
        return false;
    }
    std::vector<std::vector<Tok>> stopItems(items.begin() + 3, items.end());
    float invert = 0;
    if (!stopItems.empty()) {
        const std::vector<Tok>& last = stopItems.back();
        if (last.size() == 1 && last[0].kind == TokKind::Ident && last[0].text == "invert") {
            invert = 1;
            stopItems.pop_back();
        }
    }
    if (!parseColorStopList(stopItems, stops)) {
        return false;
    }
    out.params[0] = p0x;
    out.params[1] = p0y;
    out.params[2] = p1x;
    out.params[3] = p1y;
    out.params[4] = radial;
    out.params[5] = invert;
    return true;
}

/// shine( <angle> <len> <color> [<number> [<number> [<number>]]] ) -> Shine. The optional sixth
/// argument is `position` (params[0], 0..1, default 0); 18.4 (T-55b) allows it to be set either
/// this way or by animation. Out-of-range values are clamped to [0, 1]; unlike xts_values.cpp's
/// parseShineFn(), this runtime copy has no DiagnosticSink (T-35 note above) so it clamps silently.
bool parseShineFn(const std::vector<Tok>& inner, EffectInstance& out) {
    out.kind = EffectKind::Shine;
    Cursor cur(inner.data(), inner.size());
    float angle;
    if (!parseAngleTok(cur.peek(), angle)) {
        return false;
    }
    cur.advance();
    float width;
    if (!readEffectLen(cur, width)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float intensity = 1;
    float softness = 0.5f;
    float position = 0;
    if (!cur.atEnd() && cur.peek().kind == TokKind::Number && cur.peek().unit.empty()) {
        intensity = cur.peek().num;
        cur.advance();
        if (!cur.atEnd() && cur.peek().kind == TokKind::Number && cur.peek().unit.empty()) {
            softness = cur.peek().num;
            cur.advance();
            if (!cur.atEnd() && cur.peek().kind == TokKind::Number && cur.peek().unit.empty()) {
                position = cur.peek().num;
                cur.advance();
                position = std::clamp(position, 0.0f, 1.0f);
            }
        }
    }
    if (!cur.atEnd()) {
        return false;
    }
    out.params[0] = position;
    out.params[1] = width;
    out.params[2] = softness;
    out.params[3] = angle;
    out.params[4] = intensity;
    out.colors[0] = color;
    return true;
}

/// outline( <len> <color> [<len>] [outer|center|inner] ) -> Outline. Mirrors xts_values.cpp's
/// parseOutlineFn().
bool parseOutlineFn(const std::vector<Tok>& inner, EffectInstance& out) {
    out.kind = EffectKind::Outline;
    Cursor cur(inner.data(), inner.size());
    float radius;
    if (!readEffectLen(cur, radius)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float softness = 0;
    if (isEffectLenToken(cur.peek()) && !readEffectLen(cur, softness)) {
        return false;
    }
    float position = 0;
    if (!cur.atEnd() && cur.peek().kind == TokKind::Ident) {
        const std::string_view kw = cur.peek().text;
        if (kw == "outer") {
            position = 0;
        } else if (kw == "center") {
            position = 1;
        } else if (kw == "inner") {
            position = 2;
        } else {
            return false;
        }
        cur.advance();
    }
    if (!cur.atEnd()) {
        return false;
    }
    out.params[0] = radius;
    out.params[1] = softness;
    out.params[2] = position;
    out.colors[0] = color;
    return true;
}

/// backdrop-blur( <len> [<number> [<number>]] ) -> BackdropBlur. Mirrors xts_values.cpp's
/// parseBackdropBlurFn().
bool parseBackdropBlurFn(const std::vector<Tok>& inner, EffectInstance& out) {
    out.kind = EffectKind::BackdropBlur;
    Cursor cur(inner.data(), inner.size());
    float radius;
    if (!readEffectLen(cur, radius)) {
        return false;
    }
    float extra[2] = {0, 0};
    int extraCount = 0;
    while (extraCount < 2 && !cur.atEnd() && cur.peek().kind == TokKind::Number && cur.peek().unit.empty()) {
        extra[static_cast<size_t>(extraCount)] = cur.peek().num;
        cur.advance();
        ++extraCount;
    }
    if (!cur.atEnd()) {
        return false;
    }
    out.params[0] = radius;
    out.params[1] = extra[0];
    out.params[2] = extra[1];
    return true;
}

/// `effect: none | <effect-fn>+` (18.4). Mirrors xts_values.cpp's parseEffectList(): `custom()`
/// is kUnsupported (0.2 has no shader support -- xts_values.cpp's compiler gives it a dedicated
/// error message for the same reason, so this parser gives it a distinct status rather than
/// folding it into kInvalidArgument); an unknown function name or any other parse failure is
/// kInvalidArgument, and the sequence is capped at kMaxEffects. Each produced EffectInstance's
/// stopOffset/stopCount are rebased to index `outStops` (this declaration's own local pool), same
/// as the compiler's StyleBuilder::effectStops pool per-entry rebasing in
/// compile/src/style_emit.cpp.
Status parseEffectList(const Tok* toks, size_t count, std::vector<EffectInstance>& outEffects,
                       std::vector<GradientStop>& outStops) {
    if (count == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "none") {
        return Status::kOk;  // Empty sequence (18.4: decl exists, count 0).
    }
    Cursor cur(toks, count);
    if (cur.atEnd()) {
        return Status::kInvalidArgument;
    }
    while (!cur.atEnd()) {
        const Tok& fnTok = cur.peek();
        if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
            return Status::kInvalidArgument;
        }
        std::string_view fn = fnTok.text;
        if (fn == "custom") {
            return Status::kUnsupported;  // 0.2: custom effects always rejected (18.4).
        }
        cur.advance();  // fn name.
        std::vector<Tok> inner;
        if (!extractParenGroup(cur, inner)) {
            return Status::kInvalidArgument;
        }
        EffectInstance entry{};
        std::vector<GradientStop> stops;
        bool ok;
        if (fn == "shadow") {
            ok = parseShadowFn(inner, entry);
        } else if (fn == "glow") {
            ok = parseGlowFn(inner, entry);
        } else if (fn == "blur") {
            ok = parseBlurFn(inner, entry);
        } else if (fn == "color-adjust") {
            ok = parseColorAdjustFn(inner, entry);
        } else if (fn == "gradient-mask") {
            ok = parseGradientMaskFn(inner, entry, stops);
        } else if (fn == "shine") {
            ok = parseShineFn(inner, entry);
        } else if (fn == "outline") {
            ok = parseOutlineFn(inner, entry);
        } else if (fn == "backdrop-blur") {
            // 18.6: "backdrop-blur must come first" (the compiler's wording; mini_css has no
            // diagnostic sink, so this is a plain kInvalidArgument like every other parse error).
            if (!outEffects.empty()) {
                return Status::kInvalidArgument;
            }
            ok = parseBackdropBlurFn(inner, entry);
        } else {
            return Status::kInvalidArgument;  // Unknown function.
        }
        if (!ok) {
            return Status::kInvalidArgument;
        }
        entry.enabled = 1;
        entry.shader = kInvalidAsset;
        entry.stopOffset = static_cast<uint32_t>(outStops.size());
        entry.stopCount = static_cast<uint32_t>(stops.size());
        for (const GradientStop& s : stops) {
            outStops.push_back(s);
        }
        outEffects.push_back(entry);
        if (outEffects.size() > kMaxEffects) {
            return Status::kInvalidArgument;
        }
    }
    return Status::kOk;
}

/// left/center/right -> 0%/50%/100%, top/center/bottom -> 0%/50%/100%, else a plain length.
/// `background-gradient` (05.5c, T-115). Mirrors xts_values.cpp's parseGradientValue() and its
/// helpers (same grammar, same `%`-only stop positions and px/%-only geometry lengths), ported to
/// this file's Tok/Cursor types.
constexpr size_t kMaxGradientStops = 16;

bool parseGradientAngle(const Tok& tok, float& deg) {
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (tok.unit == "deg") {
        deg = tok.num;
        return true;
    }
    if (tok.unit == "turn") {
        deg = tok.num * 360.0f;
        return true;
    }
    if (tok.unit == "rad") {
        deg = tok.num * (180.0f / 3.14159265358979323846f);
        return true;
    }
    if (tok.unit.empty() && tok.num == 0) {
        deg = 0;
        return true;
    }
    return false;
}

bool parseGradientLen(const Tok& tok, Length& out) {
    Length len;
    if (!parseLengthTok(tok, len)) {
        return false;
    }
    if (len.unit != LengthUnit::Px && len.unit != LengthUnit::Percent) {
        return false;
    }
    out = len;
    return true;
}

void distributeGradientStops(std::vector<GradientStop>& stops, const std::vector<bool>& hasPos) {
    size_t n = stops.size();
    if (n == 0) {
        return;
    }
    std::vector<bool> known = hasPos;
    if (!known[0]) {
        stops[0].offset = 0;
        known[0] = true;
    }
    if (!known[n - 1]) {
        stops[n - 1].offset = 1;
        known[n - 1] = true;
    }
    size_t i = 1;
    while (i < n) {
        if (known[i]) {
            ++i;
            continue;
        }
        size_t start = i - 1;
        size_t end = i;
        while (end < n && !known[end]) {
            ++end;
        }
        float from = stops[start].offset;
        float to = stops[end].offset;
        for (size_t k = start + 1; k < end; ++k) {
            float t = static_cast<float>(k - start) / static_cast<float>(end - start);
            stops[k].offset = from + (to - from) * t;
            known[k] = true;
        }
        i = end;
    }
    for (size_t k = 1; k < n; ++k) {
        if (stops[k].offset < stops[k - 1].offset) {
            stops[k].offset = stops[k - 1].offset;
        }
    }
}

bool parseGradientStops(const std::vector<std::vector<Tok>>& items, std::vector<GradientStop>& out) {
    if (items.size() < 2 || items.size() > kMaxGradientStops) {
        return false;
    }
    std::vector<bool> hasPos;
    for (const std::vector<Tok>& item : items) {
        Cursor cur(item.data(), item.size());
        Color c;
        if (!parseColorCursor(cur, c)) {
            return false;
        }
        GradientStop stop;
        stop.color = c;
        stop.offset = 0;
        bool given = false;
        if (!cur.atEnd()) {
            const Tok& t = cur.peek();
            if (t.kind != TokKind::Number || (t.unit != "%" && !(t.unit.empty() && t.num == 0))) {
                return false;
            }
            stop.offset = t.unit == "%" ? t.num / 100.0f : 0.0f;
            given = true;
            cur.advance();
        }
        if (!cur.atEnd()) {
            return false;
        }
        out.push_back(stop);
        hasPos.push_back(given);
    }
    distributeGradientStops(out, hasPos);
    return true;
}

bool parseGradientValue(const Tok* toks, size_t count, ElementGradient& out) {
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "none")) {
        out.kind = GradientKind::None;
        return true;
    }
    Cursor cur(toks, count);
    const Tok& fnTok = cur.peek();
    if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
        return false;
    }
    bool linear = iequals(fnTok.text, "linear-gradient");
    bool radial = iequals(fnTok.text, "radial-gradient");
    bool conic = iequals(fnTok.text, "conic-gradient");  // T-201.
    if (!linear && !radial && !conic) {
        return false;
    }
    cur.advance();
    std::vector<Tok> inner;
    if (!extractParenGroup(cur, inner) || !cur.atEnd()) {
        return false;
    }
    std::vector<std::vector<Tok>> items = splitByCommaTopLevel(inner);
    if (items.size() < 2) {
        return false;
    }
    Cursor head(items[0].data(), items[0].size());
    if (linear) {
        out.kind = GradientKind::Linear;
        if (!parseGradientAngle(head.peek(), out.angle)) {
            return false;
        }
        head.advance();
    } else if (conic) {
        // T-201: `[from] <angle> [at <len> <len>]`, the centre defaulting to 50% 50%.
        out.kind = GradientKind::Conic;
        out.cx = Length{50, LengthUnit::Percent};
        out.cy = Length{50, LengthUnit::Percent};
        if (head.peek().kind == TokKind::Ident && iequals(head.peek().text, "from")) {
            head.advance();
        }
        if (!parseGradientAngle(head.peek(), out.angle)) {
            return false;
        }
        head.advance();
        if (head.peek().kind == TokKind::Ident && iequals(head.peek().text, "at")) {
            head.advance();
            if (!parseGradientLen(head.peek(), out.cx)) {
                return false;
            }
            head.advance();
            if (!parseGradientLen(head.peek(), out.cy)) {
                return false;
            }
            head.advance();
        }
    } else {
        out.kind = GradientKind::Radial;
        if (!parseGradientLen(head.peek(), out.rx)) {
            return false;
        }
        head.advance();
        if (!parseGradientLen(head.peek(), out.ry)) {
            return false;
        }
        head.advance();
        if (head.peek().kind != TokKind::Ident || !iequals(head.peek().text, "at")) {
            return false;
        }
        head.advance();
        if (!parseGradientLen(head.peek(), out.cx)) {
            return false;
        }
        head.advance();
        if (!parseGradientLen(head.peek(), out.cy)) {
            return false;
        }
        head.advance();
    }
    if (!head.atEnd()) {
        return false;
    }
    std::vector<std::vector<Tok>> stopItems(items.begin() + 1, items.end());
    return parseGradientStops(stopItems, out.stops);
}

/// `clip-path` (05 §5.9.8, T-332). Mirrors xts_values.cpp's parseClipPathValue(): `none`, or one
/// of `inset()`, `circle()`, `ellipse()`, `polygon()` with px / % lengths; `path()` / `url()` and
/// anything else is an invalid declaration.
bool parseClipLen(const Tok& tok, Length& out) {
    Length len;
    if (!parseLengthTok(tok, len) || (len.unit != LengthUnit::Px && len.unit != LengthUnit::Percent)) {
        return false;
    }
    out = len;
    return true;
}

/// A circle / ellipse radius: a px / % length or `closest-side` / `farthest-side`.
bool parseClipRadius(const Tok& tok, Length& out) {
    if (tok.kind == TokKind::Ident) {
        if (iequals(tok.text, "closest-side")) {
            out = Length{kClipRadiusClosestSide, LengthUnit::Auto};
            return true;
        }
        if (iequals(tok.text, "farthest-side")) {
            out = Length{kClipRadiusFarthestSide, LengthUnit::Auto};
            return true;
        }
        return false;
    }
    return parseClipLen(tok, out);
}

/// `[at <x> <y>]` for circle() / ellipse(); the center defaults to 50% 50%.
bool parseClipCenter(Cursor& cur, Length& cx, Length& cy) {
    cx = Length{50, LengthUnit::Percent};
    cy = Length{50, LengthUnit::Percent};
    if (cur.atEnd()) {
        return true;
    }
    if (cur.peek().kind != TokKind::Ident || !iequals(cur.peek().text, "at")) {
        return false;
    }
    cur.advance();
    if (!parseClipLen(cur.peek(), cx)) {
        return false;
    }
    cur.advance();
    if (!parseClipLen(cur.peek(), cy)) {
        return false;
    }
    cur.advance();
    return cur.atEnd();
}

/// 1-4 radii in CSS border-radius order (top-left, top-right, bottom-right, bottom-left) with the
/// usual shorthand repetition; `out` receives 4 entries.
bool parseClipRadiiGroup(const std::vector<Tok>& toks, Length out[4]) {
    if (toks.empty() || toks.size() > 4) {
        return false;
    }
    Length v[4];
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!parseClipLen(toks[i], v[i])) {
            return false;
        }
    }
    const size_t n = toks.size();
    out[0] = v[0];
    out[1] = n >= 2 ? v[1] : v[0];
    out[2] = n >= 3 ? v[2] : v[0];
    out[3] = n == 4 ? v[3] : out[1];
    return true;
}

bool parseClipPathValue(const Tok* toks, size_t count, ElementClipShape& out) {
    out.kind = ClipShapeKind::None;
    out.params.clear();
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "none")) {
        return true;
    }
    Cursor cur(toks, count);
    const Tok& fnTok = cur.peek();
    if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
        return false;
    }
    const bool inset = iequals(fnTok.text, "inset");
    const bool circle = iequals(fnTok.text, "circle");
    const bool ellipse = iequals(fnTok.text, "ellipse");
    const bool polygon = iequals(fnTok.text, "polygon");
    if (!inset && !circle && !ellipse && !polygon) {
        return false;  // `path()`, `url()`, a geometry box, ...: not part of this subset.
    }
    cur.advance();
    std::vector<Tok> inner;
    if (!extractParenGroup(cur, inner) || !cur.atEnd()) {
        return false;
    }
    if (inset) {
        // `<t> [<r> [<b> [<l>]]] [round <radii> [/ <radii>]]`.
        std::vector<Tok> insets;
        std::vector<Tok> horizontal;
        std::vector<Tok> vertical;
        int section = 0;
        for (const Tok& t : inner) {
            if (section == 0 && t.kind == TokKind::Ident && iequals(t.text, "round")) {
                section = 1;
                continue;
            }
            if (section == 1 && t.kind == TokKind::Slash) {
                section = 2;
                continue;
            }
            (section == 0 ? insets : section == 1 ? horizontal : vertical).push_back(t);
        }
        if (insets.empty() || insets.size() > 4 || (section >= 1 && horizontal.empty()) ||
            (section == 2 && vertical.empty())) {
            return false;
        }
        Length e[4];
        for (size_t i = 0; i < insets.size(); ++i) {
            if (!parseClipLen(insets[i], e[i])) {
                return false;
            }
        }
        const size_t n = insets.size();
        const Length top = e[0];
        const Length right = n >= 2 ? e[1] : e[0];
        const Length bottom = n >= 3 ? e[2] : e[0];
        const Length left = n == 4 ? e[3] : right;
        Length rx[4] = {};
        Length ry[4] = {};
        if (section >= 1) {
            if (!parseClipRadiiGroup(horizontal, rx)) {
                return false;
            }
            if (section == 2) {
                if (!parseClipRadiiGroup(vertical, ry)) {
                    return false;
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    ry[i] = rx[i];
                }
            }
        }
        out.kind = ClipShapeKind::Inset;
        out.params = {top, right, bottom, left, rx[0], ry[0], rx[1], ry[1], rx[2], ry[2], rx[3], ry[3]};
        return true;
    }
    Cursor in(inner.data(), inner.size());
    if (circle) {
        // `[<r>] [at <x> <y>]`; the radius defaults to closest-side.
        Length r{kClipRadiusClosestSide, LengthUnit::Auto};
        if (!in.atEnd() && !(in.peek().kind == TokKind::Ident && iequals(in.peek().text, "at"))) {
            if (!parseClipRadius(in.peek(), r)) {
                return false;
            }
            in.advance();
        }
        Length cx, cy;
        if (!parseClipCenter(in, cx, cy)) {
            return false;
        }
        out.kind = ClipShapeKind::Circle;
        out.params = {r, cx, cy};
        return true;
    }
    if (ellipse) {
        // `[<rx> <ry>] [at <x> <y>]`; both radii default to closest-side.
        Length rx{kClipRadiusClosestSide, LengthUnit::Auto};
        Length ry{kClipRadiusClosestSide, LengthUnit::Auto};
        if (!in.atEnd() && !(in.peek().kind == TokKind::Ident && iequals(in.peek().text, "at"))) {
            if (!parseClipRadius(in.peek(), rx)) {
                return false;
            }
            in.advance();
            if (!parseClipRadius(in.peek(), ry)) {
                return false;
            }
            in.advance();
        }
        Length cx, cy;
        if (!parseClipCenter(in, cx, cy)) {
            return false;
        }
        out.kind = ClipShapeKind::Ellipse;
        out.params = {rx, ry, cx, cy};
        return true;
    }
    // polygon: `<x> <y>, <x> <y>, ...` (at least 3 vertices), filled even-odd (05 §5.9.8).
    std::vector<std::vector<Tok>> items = splitByCommaTopLevel(inner);
    if (items.size() < 3 || items.size() > kMaxClipPolygonVertices) {
        return false;
    }
    out.params.reserve(items.size() * 2);
    for (const std::vector<Tok>& item : items) {
        Length x, y;
        if (item.size() != 2 || !parseClipLen(item[0], x) || !parseClipLen(item[1], y)) {
            return false;
        }
        out.params.push_back(x);
        out.params.push_back(y);
    }
    out.kind = ClipShapeKind::Polygon;
    return true;
}

bool parseOriginComponent(const Tok& tok, bool horizontal, Length& out) {
    if (tok.kind == TokKind::Ident) {
        if (iequals(tok.text, "center")) {
            out = Length{50, LengthUnit::Percent};
            return true;
        }
        if (horizontal && iequals(tok.text, "left")) {
            out = Length{0, LengthUnit::Percent};
            return true;
        }
        if (horizontal && iequals(tok.text, "right")) {
            out = Length{100, LengthUnit::Percent};
            return true;
        }
        if (!horizontal && iequals(tok.text, "top")) {
            out = Length{0, LengthUnit::Percent};
            return true;
        }
        if (!horizontal && iequals(tok.text, "bottom")) {
            out = Length{100, LengthUnit::Percent};
            return true;
        }
        return false;
    }
    return parseLengthTok(tok, out);
}

bool parseOriginShorthand(const Tok* toks, size_t count, uint16_t xProp, uint16_t yProp,
                          std::vector<Decl>& out) {
    if (count == 0 || count > 2) {
        return false;
    }
    Length x{};
    Length y{50, LengthUnit::Percent};
    if (!parseOriginComponent(toks[0], /*horizontal=*/true, x)) {
        return false;
    }
    if (count == 2 && !parseOriginComponent(toks[1], /*horizontal=*/false, y)) {
        return false;
    }
    Decl dx;
    dx.prop = xProp;
    dx.value = Value::ofLength(x.value, x.unit);
    Decl dy;
    dy.prop = yProp;
    dy.value = Value::ofLength(y.value, y.unit);
    out.push_back(dx);
    out.push_back(dy);
    return true;
}

bool parseTransformOrigin(const Tok* toks, size_t count, std::vector<Decl>& out) {
    return parseOriginShorthand(toks, count, prop::AnchorX, prop::AnchorY, out);
}

/// `perspective-origin` (05.5/19.10, T-47a): same grammar as `transform-origin`, PropId 107/108.
bool parsePerspectiveOrigin(const Tok* toks, size_t count, std::vector<Decl>& out) {
    return parseOriginShorthand(toks, count, prop::PerspectiveOriginX, prop::PerspectiveOriginY, out);
}

/// `background-position` (05 §5.9.8, T-327): same grammar as `transform-origin`, PropId 136/137;
/// the compiler's parseBackgroundPosition (compile/src/xts/xts_values.cpp).
bool parseBackgroundPosition(const Tok* toks, size_t count, std::vector<Decl>& out) {
    return parseOriginShorthand(toks, count, prop::BackgroundPositionX, prop::BackgroundPositionY, out);
}

/// `background-size: auto | cover | contain | <len|%> [<len|%>]` (05 §5.9.8, T-327), PropId
/// 134/135; the compiler's parseBackgroundSize (compile/src/xts/xts_values.cpp). `cover` /
/// `contain` are LengthUnit::Auto valued kBackgroundSizeCover / kBackgroundSizeContain.
bool parseBackgroundSize(const Tok* toks, size_t count, std::vector<Decl>& out) {
    if (count == 0 || count > 2) {
        return false;
    }
    Length x{0, LengthUnit::Auto};
    Length y{0, LengthUnit::Auto};
    if (count == 1 && toks[0].kind == TokKind::Ident &&
        (iequals(toks[0].text, "cover") || iequals(toks[0].text, "contain"))) {
        x = Length{iequals(toks[0].text, "cover") ? kBackgroundSizeCover : kBackgroundSizeContain,
                   LengthUnit::Auto};
        y = x;
    } else {
        for (size_t i = 0; i < count; ++i) {
            Length len{};
            if (!parseLengthTok(toks[i], len)) {
                return false;
            }
            if (len.unit == LengthUnit::Auto) {
                len = Length{0, LengthUnit::Auto};
            } else if (len.value < 0.0f) {
                return false;
            }
            (i == 0 ? x : y) = len;
        }
    }
    Decl dx;
    dx.prop = prop::BackgroundSizeX;
    dx.value = Value::ofLength(x.value, x.unit);
    Decl dy;
    dy.prop = prop::BackgroundSizeY;
    dy.value = Value::ofLength(y.value, y.unit);
    out.push_back(dx);
    out.push_back(dy);
    return true;
}

/// `font-family: A, "B C", sans-serif`. Only the first family survives into the Decl: the runtime
/// cascade can only point ComputedStyle at an existing IR font-family pool run (see
/// style_engine.cpp's applyRuntimeDecl), which is a single name. The remaining names are still
/// syntax-checked. See the T-35 report's uncovered cases.
bool parseFontFamily(const Tok* toks, size_t count, std::string& firstOut) {
    Cursor cur(toks, count);
    bool first = true;
    while (true) {
        const Tok& t = cur.peek();
        std::string name;
        if (t.kind == TokKind::String) {
            name.assign(t.text);
            cur.advance();
        } else if (t.kind == TokKind::Ident) {
            // An unquoted family name may be several idents ("sans serif"); join with spaces.
            while (cur.peek().kind == TokKind::Ident) {
                if (!name.empty()) {
                    name.push_back(' ');
                }
                name.append(cur.peek().text);
                cur.advance();
            }
        } else {
            return false;
        }
        if (name.empty()) {
            return false;
        }
        if (first) {
            firstOut = std::move(name);
            first = false;
        }
        if (cur.atEnd()) {
            return true;
        }
        if (cur.peek().kind != TokKind::Comma) {
            return false;
        }
        cur.advance();
        if (cur.atEnd()) {
            return false;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// animation (docs/design/05-style-and-css.md §5.6, docs/design/17-editor.md §17.6, T-123). The
// same grammar compile/src/xts/xts_values.cpp's parseAnimationItem() accepts, with `@keyframes`
// name resolution left to the caller (see mini_css.h's AnimationDecl).
// ---------------------------------------------------------------------------------------------

/// `<time>`: `s`/`ms` only (05.6). Mirrors xts_values.cpp's parseTimeTok.
bool parseTimeTok(const Tok& tok, float& seconds) {
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (tok.unit == "s") {
        seconds = tok.num;
        return true;
    }
    if (tok.unit == "ms") {
        seconds = tok.num / 1000.0f;
        return true;
    }
    return false;
}

/// True when the cursor sits on the first token of an `<easing>` (05.6's five keywords plus
/// `cubic-bezier`/`steps`).
bool isEasingStart(const Cursor& cur) {
    const Tok& t = cur.peek();
    if (t.kind != TokKind::Ident) {
        return false;
    }
    return iequals(t.text, "linear") || iequals(t.text, "ease") || iequals(t.text, "ease-in") ||
           iequals(t.text, "ease-out") || iequals(t.text, "ease-in-out") || iequals(t.text, "cubic-bezier") ||
           iequals(t.text, "steps") || iequals(t.text, "step-start") || iequals(t.text, "step-end");
}

/// T-335: `linear(<stop>[, <stop>]...)` with the cursor on `linear`; each stop is `<number>
/// [<percentage>]{0,2}` (CSS Easing 1). The canonical points are appended to `points` and `out`
/// indexes them there. Mirrors xts_values.cpp's parseLinearFunction.
bool parseLinearFunction(Cursor& cur, Easing& out, std::vector<EasingPoint>& points) {
    cur.advance();  // `linear`
    cur.advance();  // `(`
    std::vector<LinearStop> stops;
    while (true) {
        LinearStop stop;
        if (!parseFloatTok(cur.peek(), stop.out)) {
            return false;
        }
        cur.advance();
        while (stop.inputCount < 2 && cur.peek().kind == TokKind::Number && cur.peek().unit == "%") {
            stop.in[stop.inputCount++] = cur.peek().num / 100.0f;
            cur.advance();
        }
        stops.push_back(stop);
        if (cur.peek().kind == TokKind::Comma) {
            cur.advance();
            continue;
        }
        if (cur.peek().kind != TokKind::RParen) {
            return false;
        }
        cur.advance();
        break;
    }
    const size_t first = points.size();
    if (first > 0xFFFFu || !linearEasingPoints(stops, points)) {
        return false;
    }
    if (points.size() - first > 0xFFFFu) {
        points.resize(first);
        return false;
    }
    out = Easing{};
    out.kind = EasingKind::LinearPoints;
    out.pointOffset = static_cast<uint16_t>(first);
    out.pointCount = static_cast<uint16_t>(points.size() - first);
    return true;
}

/// `<easing>` (05.6). Mirrors xts_values.cpp's parseEasingCursor. T-335: `linear()` needs
/// `points` (its points go there); without it, it is not accepted.
bool parseEasingCursor(Cursor& cur, Easing& out, std::vector<EasingPoint>* points = nullptr) {
    const Tok& t = cur.peek();
    if (t.kind != TokKind::Ident) {
        return false;
    }
    if (iequals(t.text, "linear") && cur.peek(1).kind == TokKind::LParen) {
        return points != nullptr && parseLinearFunction(cur, out, *points);
    }
    if (iequals(t.text, "linear")) {
        cur.advance();
        out = Easing::linear();
        return true;
    }
    if (iequals(t.text, "ease")) {
        cur.advance();
        out = Easing::cssEase();
        return true;
    }
    if (iequals(t.text, "ease-in")) {
        cur.advance();
        out = Easing::cssEaseIn();
        return true;
    }
    if (iequals(t.text, "ease-out")) {
        cur.advance();
        out = Easing::cssEaseOut();
        return true;
    }
    if (iequals(t.text, "ease-in-out")) {
        cur.advance();
        out = Easing::cssEaseInOut();
        return true;
    }
    // T-322: CSS's step-start / step-end = steps(1, jump-start) / steps(1, jump-end).
    if (iequals(t.text, "step-start") || iequals(t.text, "step-end")) {
        out = Easing::steps(1, iequals(t.text, "step-end"));
        cur.advance();
        return true;
    }
    if (iequals(t.text, "cubic-bezier") && cur.peek(1).kind == TokKind::LParen) {
        cur.advance();
        cur.advance();
        float v[4];
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                if (cur.peek().kind != TokKind::Comma) {
                    return false;
                }
                cur.advance();
            }
            if (!parseFloatTok(cur.peek(), v[i])) {
                return false;
            }
            cur.advance();
        }
        if (cur.peek().kind != TokKind::RParen) {
            return false;
        }
        cur.advance();
        out = Easing::bezier(v[0], v[1], v[2], v[3]);
        return true;
    }
    if (iequals(t.text, "steps") && cur.peek(1).kind == TokKind::LParen) {
        cur.advance();
        cur.advance();
        float n = 0;
        if (!parseFloatTok(cur.peek(), n) || n < 0) {
            return false;
        }
        cur.advance();
        bool jumpEnd = true;
        if (cur.peek().kind == TokKind::Comma) {
            cur.advance();
            const Tok& kw = cur.peek();
            if (kw.kind != TokKind::Ident || (!iequals(kw.text, "start") && !iequals(kw.text, "end"))) {
                return false;
            }
            jumpEnd = iequals(kw.text, "end");
            cur.advance();
        }
        if (cur.peek().kind != TokKind::RParen) {
            return false;
        }
        cur.advance();
        out = Easing::steps(std::max<uint16_t>(1, toUint16Sat(n)), jumpEnd);  // T-258a: 1..65535
        return true;
    }
    return false;
}

IrEasing toIrEasing(const Easing& e) {
    if (e.kind == EasingKind::LinearPoints) {  // T-335: indexes AnimationDecl::easingPoints.
        return irLinearPointsEasing(e.pointOffset, e.pointCount);
    }
    IrEasing out{};
    out.kind = static_cast<uint8_t>(e.kind);
    out.stepsJumpEnd = e.stepsJumpEnd;
    out.stepCount = e.stepCount;
    out.x1 = e.x1;
    out.y1 = e.y1;
    out.x2 = e.x2;
    out.y2 = e.y2;
    return out;
}

/// One comma-separated `animation` item: `<name> <duration> [<easing>] [<delay>] [<count |
/// infinite>] [<direction>] [<fill-mode>] [<play-state>]`, in any order after the duration.
/// `played` is false for a count of 0 (T-272: CSS does not play it; IR's 0 means infinite).
bool parseAnimationItem(const Tok* toks, size_t count, AnimationDecl& out, bool& played) {
    played = true;
    Cursor cur(toks, count);
    if (cur.peek().kind != TokKind::Ident) {
        return false;
    }
    out.name.assign(cur.peek().text);
    cur.advance();
    float duration = 0;
    if (!parseTimeTok(cur.peek(), duration)) {
        return false;
    }
    cur.advance();
    out.spec = IrAnimationSpec{};
    out.spec.timeline = kInvalidTimeline;  // Resolved by the caller from `name`.
    out.spec.duration = duration;
    out.spec.easing = toIrEasing(Easing::linear());
    out.spec.iterations = 1;
    bool haveEasing = false, haveDelay = false, haveIter = false, haveDir = false, haveFill = false,
         havePlay = false;
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
        float num = 0;
        if (!haveDelay && parseTimeTok(t, num)) {
            out.spec.delay = num;
            haveDelay = true;
            cur.advance();
            continue;
        }
        if (!haveEasing && isEasingStart(cur)) {
            Easing e;
            out.easingPoints.clear();
            if (!parseEasingCursor(cur, e, &out.easingPoints)) {
                return false;
            }
            out.spec.easing = toIrEasing(e);
            haveEasing = true;
            continue;
        }
        if (!haveIter && t.kind == TokKind::Ident && iequals(t.text, "infinite")) {
            out.spec.iterations = 0;  // IrAnimationSpec: 0 = infinite (style_emit.cpp agrees).
            haveIter = true;
            cur.advance();
            continue;
        }
        if (!haveIter && t.kind == TokKind::Number && t.unit.empty() && t.num >= 0) {
            out.spec.iterations = toUint16Sat(t.num);  // T-258a: saturates.
            played = out.spec.iterations != 0;         // `0.5` truncates to 0 as well.
            haveIter = true;
            cur.advance();
            continue;
        }
        if (!haveDir && t.kind == TokKind::Ident &&
            (iequals(t.text, "normal") || iequals(t.text, "reverse") || iequals(t.text, "alternate") ||
             iequals(t.text, "alternate-reverse"))) {
            out.spec.direction = iequals(t.text, "normal")      ? 0
                                 : iequals(t.text, "reverse")   ? 1
                                 : iequals(t.text, "alternate") ? 2
                                                                : 3;
            haveDir = true;
            cur.advance();
            continue;
        }
        if (!haveFill && t.kind == TokKind::Ident &&
            (iequals(t.text, "none") || iequals(t.text, "forwards") || iequals(t.text, "backwards") ||
             iequals(t.text, "both"))) {
            out.spec.fill = iequals(t.text, "none")        ? 0
                            : iequals(t.text, "forwards")  ? 1
                            : iequals(t.text, "backwards") ? 2
                                                           : 3;
            haveFill = true;
            cur.advance();
            continue;
        }
        if (!havePlay && t.kind == TokKind::Ident &&
            (iequals(t.text, "running") || iequals(t.text, "paused"))) {
            out.spec.playState = iequals(t.text, "paused") ? 1 : 0;
            havePlay = true;
            cur.advance();
            continue;
        }
        return false;
    }
    return true;
}

/// The whole comma-separated `animation` list. `animation: none` yields an empty `out`.
bool parseAnimationList(const Tok* toks, size_t count, std::vector<AnimationDecl>& out) {
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "none")) {
        return true;
    }
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i <= count; ++i) {
        if (i != count) {
            if (toks[i].kind == TokKind::LParen) {
                ++depth;
            } else if (toks[i].kind == TokKind::RParen) {
                --depth;
            }
            // Only a top-level comma separates two items: `cubic-bezier(a,b,c,d)`'s own commas
            // are part of one item. (xts_values.cpp's splitByComma is not depth-aware; the
            // runtime parser has to be, since xt_flow's `playAnimation` port re-serializes an
            // element's easing back into this grammar -- T-123.)
            if (toks[i].kind != TokKind::Comma || depth != 0) {
                continue;
            }
        }
        AnimationDecl item;
        bool played = true;
        if (!parseAnimationItem(toks + start, i - start, item, played)) {
            return false;
        }
        if (played) {
            out.push_back(std::move(item));  // A count-0 item creates no animation (T-272).
        }
        start = i + 1;
    }
    return true;
}

/// T-333 (05 §5.9.8): `border-color` with 2-4 colors onto the per-side colors (the CSS 1-4 value
/// rule); the compiler's parseBorderColorSides. False unless `toks` is 2-4 colors.
bool parseBorderColorSides(const Tok* toks, size_t count, std::vector<Decl>& out) {
    Color c[4];
    size_t n = 0;
    Cursor cur(toks, count);
    while (!cur.atEnd()) {
        if (n == 4 || !parseColorCursor(cur, c[n])) {
            return false;
        }
        ++n;
    }
    if (n < 2) {
        return false;
    }
    const Color sides[4] = {c[0], c[1], n >= 3 ? c[2] : c[0], n == 4 ? c[3] : c[1]};
    for (int i = 0; i < 4; ++i) {
        Decl d;
        d.prop = static_cast<uint16_t>(prop::BorderTopColor + i);
        d.value = Value::ofColor(sides[i]);
        out.push_back(std::move(d));
    }
    return true;
}

/// T-333 (05 §5.9.8): `border-style: <style>{1,4}` onto the four `border-*-style` StyleProps.
bool parseBorderStyleShorthand(const Tok* toks, size_t count, std::vector<Decl>& out) {
    if (count == 0 || count > 4) {
        return false;
    }
    uint32_t v[4] = {};
    for (size_t i = 0; i < count; ++i) {
        if (toks[i].kind != TokKind::Ident || !enumValueOf(sp::BorderTopStyle, toks[i].text, v[i])) {
            return false;
        }
    }
    const uint32_t sides[4] = {v[0], count >= 2 ? v[1] : v[0], count >= 3 ? v[2] : v[0],
                               count == 4 ? v[3] : (count >= 2 ? v[1] : v[0])};
    for (int i = 0; i < 4; ++i) {
        Decl d;
        d.prop = static_cast<uint16_t>(sp::BorderTopStyle + i);
        d.value = Value::ofEnum(sides[i]);
        out.push_back(std::move(d));
    }
    return true;
}

/// T-325 (05 §5.9.8): `outline: <width> <style> <color>`, each part optional and in any order;
/// the same grammar as the compiler's parseOutlineShorthand (compile/src/xts/xts_values.cpp). An
/// absent width / style writes its initial value (0, `none`); an absent color writes nothing.
bool parseOutlineShorthand(const Tok* toks, size_t count, std::vector<Decl>& out) {
    bool haveWidth = false, haveStyle = false, haveColor = false;
    Length width{0, LengthUnit::Px};
    uint32_t style = 0;
    Color color{};
    Cursor cur(toks, count);
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
        uint32_t e = 0;
        if (!haveStyle && t.kind == TokKind::Ident && enumValueOf(sp::OutlineStyle, t.text, e)) {
            style = e;
            haveStyle = true;
            cur.advance();
            continue;
        }
        if (!haveWidth) {
            Length len{};
            if (parseLengthTok(t, len) && len.unit != LengthUnit::Auto && len.unit != LengthUnit::Percent &&
                len.value >= 0.0f) {
                width = len;
                haveWidth = true;
                cur.advance();
                continue;
            }
        }
        if (!haveColor) {
            Cursor probe = cur;
            Color c{};
            if (parseColorCursor(probe, c)) {
                cur = probe;
                color = c;
                haveColor = true;
                continue;
            }
        }
        return false;
    }
    Decl w;
    w.prop = prop::OutlineWidth;
    w.value = Value::ofLength(width.value, width.unit);
    out.push_back(std::move(w));
    Decl s;
    s.prop = sp::OutlineStyle;
    s.value = Value::ofEnum(style);
    out.push_back(std::move(s));
    if (haveColor) {
        Decl c;
        c.prop = prop::OutlineColor;
        c.value = Value::ofColor(color);
        out.push_back(std::move(c));
    }
    return true;
}

/// T-324 (05 §5.9.8): one `text-decoration-line` keyword's bit, or 0 for anything else.
uint32_t textDecorationBit(const Tok& t) {
    if (t.kind != TokKind::Ident) {
        return 0;
    }
    if (iequals(t.text, "underline")) {
        return kTextDecorationUnderline;
    }
    if (iequals(t.text, "overline")) {
        return kTextDecorationOverline;
    }
    if (iequals(t.text, "line-through")) {
        return kTextDecorationLineThrough;
    }
    return 0;
}

/// T-324: `text-decoration-line: none | [underline || overline || line-through]` (each keyword at
/// most once) -> the kTextDecoration* set.
bool parseTextDecorationLine(const Tok* toks, size_t count, uint32_t& out) {
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "none")) {
        out = 0;
        return true;
    }
    out = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t bit = textDecorationBit(toks[i]);
        if (bit == 0 || (out & bit) != 0) {
            return false;
        }
        out |= bit;
    }
    return count > 0;
}

/// T-324: a `text-decoration-thickness` value: `auto` (LengthUnit::Auto) or a non-negative px /
/// em length (no percentage), as the compiler accepts.
bool parseDecorationThickness(const Tok& t, Length& out) {
    Length len{};
    if (!parseLengthTok(t, len)) {
        return false;
    }
    const bool length = (len.unit == LengthUnit::Px || len.unit == LengthUnit::Em) && len.value >= 0.0f;
    if (len.unit != LengthUnit::Auto && !length) {
        return false;
    }
    out = len;
    return true;
}

/// T-324 (05 §5.9.8): `text-decoration: <line…> [<color>] [<thickness>]`, the parts in any order
/// (the line keywords together); the same grammar as the compiler's parseTextDecorationShorthand
/// (compile/src/xts/xts_values.cpp). An absent line / thickness writes its initial value (`none`,
/// `auto`); an absent color writes nothing, so `text-decoration-color` keeps following `color`.
bool parseTextDecorationShorthand(const Tok* toks, size_t count, std::vector<Decl>& out) {
    bool haveLine = false, haveColor = false, haveThickness = false;
    uint32_t lines = 0;
    Length thickness{0, LengthUnit::Auto};
    Color color{};
    Cursor cur(toks, count);
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
        if (!haveLine && t.kind == TokKind::Ident && iequals(t.text, "none")) {
            haveLine = true;
            cur.advance();
            continue;
        }
        const uint32_t bit = textDecorationBit(t);
        if (bit != 0) {
            if ((lines & bit) != 0 || (haveLine && lines == 0)) {
                return false;
            }
            lines |= bit;
            haveLine = true;
            cur.advance();
            continue;
        }
        if (!haveThickness) {
            Length len{};
            if (parseDecorationThickness(t, len)) {
                thickness = len;
                haveThickness = true;
                cur.advance();
                continue;
            }
        }
        if (!haveColor) {
            Cursor probe = cur;
            Color c{};
            if (parseColorCursor(probe, c)) {
                cur = probe;
                color = c;
                haveColor = true;
                continue;
            }
        }
        return false;
    }
    Decl l;
    l.prop = sp::TextDecorationLine;
    l.value = Value::ofEnum(lines);
    out.push_back(std::move(l));
    Decl th;
    th.prop = prop::TextDecorationThickness;
    th.value = Value::ofLength(thickness.value, thickness.unit);
    out.push_back(std::move(th));
    if (haveColor) {
        Decl c;
        c.prop = prop::TextDecorationColor;
        c.value = Value::ofColor(color);
        out.push_back(std::move(c));
    }
    return true;
}

/// Parses one declaration's already-tokenized value. Returns kOk, kUnsupported (transition) or
/// kInvalidArgument.
/// T-338 (06 §6.15): one grid line `toks[0, count)` -- `auto | <integer >= 1> | span <integer>`
/// -- as its kGridLine* encoding (the compiler's parseGridLine).
bool parseGridLineTokens(const Tok* toks, size_t count, uint32_t& out) {
    const auto integer = [](const Tok& t, int& v) {
        if (t.kind != TokKind::Number || !t.unit.empty() || std::trunc(t.num) != t.num ||
            !(t.num >= 1.0f && t.num <= 1.0e6f)) {
            return false;
        }
        v = static_cast<int>(t.num);
        return true;
    };
    int v = 0;
    if (count == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "auto") {
        out = kGridLineAuto;
        return true;
    }
    if (count == 1 && integer(toks[0], v)) {
        out = static_cast<uint32_t>(std::min<int>(v, static_cast<int>(kMaxGridTracks) + 1));
        return true;
    }
    if (count == 2 && toks[0].kind == TokKind::Ident && toks[0].text == "span" && integer(toks[1], v)) {
        out = kGridLineSpan | static_cast<uint32_t>(std::min<int>(v, static_cast<int>(kMaxGridTracks)));
        return true;
    }
    return false;
}

Status parseValueTokens(const NameEntry& entry, const Tok* toks, size_t count, std::vector<Decl>& out) {
    if (entry.kind == Kind::Unsupported) {
        return Status::kUnsupported;
    }
    if (count == 0) {
        return Status::kInvalidArgument;
    }
    if (entry.prop >= sp::GridColumnStart && entry.prop <= sp::GridRowEnd) {  // T-338.
        uint32_t line = 0;
        if (!parseGridLineTokens(toks, count, line)) {
            return Status::kInvalidArgument;
        }
        Decl decl;
        decl.prop = entry.prop;
        decl.value = Value::ofEnum(line);
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.prop == kGridColumnProp || entry.prop == kGridRowProp) {  // T-338: `<a> [/ <b>]`.
        size_t slash = count;
        for (size_t i = 0; i < count; ++i) {
            if (toks[i].kind == TokKind::Slash) {
                slash = i;
                break;
            }
        }
        uint32_t start = kGridLineAuto;
        uint32_t end = kGridLineAuto;
        if (!parseGridLineTokens(toks, slash, start) ||
            (slash < count && !parseGridLineTokens(toks + slash + 1, count - slash - 1, end))) {
            return Status::kInvalidArgument;
        }
        const uint16_t first = entry.prop == kGridColumnProp ? sp::GridColumnStart : sp::GridRowStart;
        Decl ds;
        ds.prop = first;
        ds.value = Value::ofEnum(start);
        out.push_back(std::move(ds));
        Decl de;
        de.prop = static_cast<uint16_t>(first + 1);
        de.value = Value::ofEnum(end);
        out.push_back(std::move(de));
        return Status::kOk;
    }
    if (entry.kind == Kind::Transform) {
        return parseTransform(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::IndepTransform) {
        return parseIndependentTransform(entry.prop, toks, count, out) ? Status::kOk
                                                                       : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::Origin) {
        return parseTransformOrigin(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::PerspectiveOrigin) {
        return parsePerspectiveOrigin(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::Outline) {
        return parseOutlineShorthand(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::BorderStyle) {  // T-333 (05 §5.9.8).
        return parseBorderStyleShorthand(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::UiScale) {
        // T-345 (05 §5.9.9): `auto | none | clamp(<min>, <max>)` -> Color (mode, min, max, 0), as
        // the compiler encodes StyleProp 273.
        float v[3] = {0, 0, 0};
        if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "auto")) {
            v[0] = 0;
        } else if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "none")) {
            v[0] = 1;
        } else if (count == 6 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "clamp") &&
                   toks[1].kind == TokKind::LParen && toks[3].kind == TokKind::Comma &&
                   toks[5].kind == TokKind::RParen && parseFloatTok(toks[2], v[1]) &&
                   parseFloatTok(toks[4], v[2]) && v[1] > 0.0f && v[2] >= v[1] && std::isfinite(v[2])) {
            v[0] = 2;
        } else {
            return Status::kInvalidArgument;
        }
        Decl decl;
        decl.prop = entry.prop;
        decl.value = Value::ofColor(Color{v[0], v[1], v[2], 0});
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.kind == Kind::HitSlop) {
        // T-386 (05 §5.9.11): 1-4 lengths (px / % / em, >= 0) in margin order -> the four Length
        // longhands StyleProp 277-280 (top, right, bottom, left), as the compiler writes them
        // since IR minor 8 (T-405).
        if (count < 1 || count > 4) {
            return Status::kInvalidArgument;
        }
        Length v[4]{};
        for (size_t i = 0; i < count; ++i) {
            if (!parseLengthTok(toks[i], v[i]) || !(v[i].value >= 0.0f) || !std::isfinite(v[i].value) ||
                (v[i].unit != LengthUnit::Px && v[i].unit != LengthUnit::Percent &&
                 v[i].unit != LengthUnit::Em)) {
                return Status::kInvalidArgument;
            }
        }
        const Length top = v[0];
        const Length right = count >= 2 ? v[1] : top;
        const Length bottom = count >= 3 ? v[2] : top;
        const Length left = count == 4 ? v[3] : right;
        const Length sides[4] = {top, right, bottom, left};
        for (int i = 0; i < 4; ++i) {
            Decl decl;
            decl.prop = static_cast<uint16_t>(sp::HitSlopTop + i);
            decl.value = Value::ofLength(sides[i].value, sides[i].unit);
            out.push_back(std::move(decl));
        }
        return Status::kOk;
    }
    if (entry.prop == prop::BorderColor && parseBorderColorSides(toks, count, out)) {
        return Status::kOk;  // T-333: 2-4 colors; one color falls through to the plain Color path.
    }
    if (entry.kind == Kind::BackgroundSize) {
        return parseBackgroundSize(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::BackgroundPosition) {
        return parseBackgroundPosition(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.kind == Kind::TextDecoration) {
        return parseTextDecorationShorthand(toks, count, out) ? Status::kOk : Status::kInvalidArgument;
    }
    if (entry.prop == sp::TextDecorationLine) {  // T-324: a keyword set, not a single enum.
        Decl decl;
        decl.prop = entry.prop;
        uint32_t lines = 0;
        if (!parseTextDecorationLine(toks, count, lines)) {
            return Status::kInvalidArgument;
        }
        decl.value = Value::ofEnum(lines);
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.prop == sp::Overflow && count == 2) {  // T-336: `overflow: <x> <y>`.
        uint32_t x = 0;
        uint32_t y = 0;
        if (toks[0].kind != TokKind::Ident || toks[1].kind != TokKind::Ident ||
            !enumValueOf(sp::OverflowX, toks[0].text, x) || !enumValueOf(sp::OverflowY, toks[1].text, y)) {
            return Status::kInvalidArgument;
        }
        Decl dx;
        dx.prop = sp::OverflowX;
        dx.value = Value::ofEnum(x);
        out.push_back(std::move(dx));
        Decl dy;
        dy.prop = sp::OverflowY;
        dy.value = Value::ofEnum(y);
        out.push_back(std::move(dy));
        return Status::kOk;
    }
    if (entry.prop == prop::Order) {  // T-336: `<integer>`.
        if (count != 1 || toks[0].kind != TokKind::Number || !toks[0].unit.empty() ||
            static_cast<float>(toInt32Sat(toks[0].num)) != toks[0].num) {
            return Status::kInvalidArgument;
        }
        Decl decl;
        decl.prop = entry.prop;
        decl.value = Value::ofFloat(toks[0].num);
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.prop == prop::AspectRatio) {  // T-328: `auto` (0) | <w> / <h> | <number>, both > 0.
        Decl decl;
        decl.prop = entry.prop;
        float ratio = 0.0f;
        if (!parseAspectRatio(toks, count, ratio)) {
            return Status::kInvalidArgument;
        }
        decl.value = Value::ofFloat(ratio);
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.prop == prop::TextDecorationThickness) {  // T-324: `auto` | non-negative length.
        Decl decl;
        decl.prop = entry.prop;
        Length len{};
        if (count != 1 || !parseDecorationThickness(toks[0], len)) {
            return Status::kInvalidArgument;
        }
        decl.value = Value::ofLength(len.value, len.unit);
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.kind == Kind::Gradient) {
        Decl decl;
        decl.prop = entry.prop;
        if (!parseGradientValue(toks, count, decl.gradient)) {
            return Status::kInvalidArgument;
        }
        // `value`'s real ListRef is filled in by the caller (ElementRef::setStyle) once
        // decl.gradient is appended to Document's runtime gradient pool; see mini_css.h.
        decl.value.type = ValueType::ListRef;
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.kind == Kind::ClipPath) {
        // T-332 (05 §5.9.8): like `background-gradient`, the caller writes decl.clipShape into
        // Document's runtime pool and fills in `value` (an Enum) from it.
        Decl decl;
        decl.prop = entry.prop;
        if (!parseClipPathValue(toks, count, decl.clipShape)) {
            return Status::kInvalidArgument;
        }
        decl.value = Value::ofEnum(0);
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.kind == Kind::Animation) {
        Decl decl;
        decl.prop = entry.prop;
        if (!parseAnimationList(toks, count, decl.animations)) {
            return Status::kInvalidArgument;
        }
        // `value`'s real ListRef is filled in by the caller (ElementRef::setStyle) once
        // decl.animations' `@keyframes` names are resolved and appended to Document's runtime
        // animation pool; see mini_css.h's Decl comment.
        decl.value.type = ValueType::ListRef;
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    if (entry.kind == Kind::Effect) {
        Decl decl;
        decl.prop = entry.prop;
        Status status = parseEffectList(toks, count, decl.effects, decl.effectStops);
        if (status != Status::kOk) {
            return status;
        }
        // `value`'s real ListRef (sid/floatCount) is filled in by the caller (ElementRef::setStyle)
        // once decl.effects/effectStops are appended to Document's runtime effect pool; see
        // mini_css.h's Decl comment.
        decl.value.type = ValueType::ListRef;
        out.push_back(std::move(decl));
        return Status::kOk;
    }
    Decl decl;
    decl.prop = entry.prop;
    switch (entry.kind) {
        case Kind::Float: {
            float f = 0;
            if (count != 1 || !parseFloatTok(toks[0], f)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofFloat(f);
            break;
        }
        case Kind::Length: {
            if (count != 1) {
                return Status::kInvalidArgument;
            }
            if (entry.prop == prop::LineHeight && toks[0].kind == TokKind::Number && toks[0].unit.empty()) {
                // Unitless line-height is stored as Em (05.4), including unitless 0.
                decl.value = Value::ofLength(toks[0].num, LengthUnit::Em);
                break;
            }
            // 05.5/19.10 (T-47a): `perspective: none` encodes as LengthUnit::Auto.
            if (entry.prop == prop::Perspective && toks[0].kind == TokKind::Ident &&
                iequals(toks[0].text, "none")) {
                decl.value = Value::ofLength(0, LengthUnit::Auto);
                break;
            }
            Length len{};
            if (!parseLengthTok(toks[0], len)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofLength(len.value, len.unit);
            break;
        }
        case Kind::Color: {
            Cursor cur(toks, count);
            Color c{};
            if (!parseColorCursor(cur, c) || !cur.atEnd()) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofColor(c);
            break;
        }
        case Kind::Enum: {
            if (count != 1) {
                return Status::kInvalidArgument;
            }
            if (entry.prop == sp::FontWeight && toks[0].kind == TokKind::Number && toks[0].unit.empty()) {
                int w = toInt32Sat(toks[0].num);
                if (w < 100 || w > 900 || (w % 100) != 0) {
                    return Status::kInvalidArgument;
                }
                decl.value = Value::ofEnum(static_cast<uint32_t>(w));
                break;
            }
            if (entry.prop == sp::LineClamp && toks[0].kind == TokKind::Number) {
                // T-331 (05 §5.9.8): an integer >= 1 (`none` is the keyword below).
                const int n = toInt32Sat(toks[0].num);
                if (!toks[0].unit.empty() || n < 1 || static_cast<float>(n) != toks[0].num) {
                    return Status::kInvalidArgument;
                }
                decl.value = Value::ofEnum(static_cast<uint32_t>(n));
                break;
            }
            if (toks[0].kind != TokKind::Ident) {
                return Status::kInvalidArgument;
            }
            uint32_t e = 0;
            if (!enumValueOf(entry.prop, toks[0].text, e)) {
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
            if (count != 1 || (toks[0].kind != TokKind::Ident && toks[0].kind != TokKind::String)) {
                return Status::kInvalidArgument;
            }
            decl.value = Value::ofStringId(kInvalidString);
            decl.text.assign(toks[0].text);
            break;
        }
        case Kind::Unsupported:
        case Kind::Transform:
        case Kind::Origin:
        case Kind::PerspectiveOrigin:
        case Kind::Effect:
        case Kind::Gradient:
        case Kind::Animation:
        case Kind::Outline:
        case Kind::TextDecoration:
        case Kind::BackgroundSize:
        case Kind::BackgroundPosition:
        case Kind::IndepTransform:
        case Kind::ClipPath:
        case Kind::BorderStyle:
        case Kind::UiScale:
        case Kind::HitSlop:
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
            independentTransformFnToks(kWhich[k], toks.data() + parts[k].begin, parts[k].count, fn, isNone);
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
        std::vector<Decl> merged;
        if (!parseTransform(combined.data(), combined.size(), merged)) {
            return Status::kInvalidArgument;
        }
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [](const Decl& d) { return isTransformComponentProp(d.prop); }),
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
