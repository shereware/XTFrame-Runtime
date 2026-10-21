#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "css_color.h"          // core/src: named colours and hsl() (T-322).
#include "transform_compose.h"  // core/src: 05.5's compose/decompose path (T-120).
#include "xt/charconv.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_value.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xts_lexer.h"
#include "xts_props.h"
#include "xts_values_internal.h"

/// Per-property value parsing (split from xts_values.cpp by T-412): the box, border, outline,
/// text-decoration, flex, gap and particle shorthands.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace xts_values_detail {

// -------------------------------------------------------------------------------------------
// 1-4 value shorthands (margin/padding/border-width/border-radius/inset).
// -------------------------------------------------------------------------------------------

bool expand1to4(const PropId order[4], const std::vector<Token>& toks, int line,
                std::vector<StyleDecl>& out) {
    if (toks.empty() || toks.size() > 4) {
        return false;
    }
    Length v[4];
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!parseLengthTok(toks[i], v[i])) {
            return false;
        }
    }
    Length top, right, bottom, left;
    switch (toks.size()) {
        case 1:
            top = right = bottom = left = v[0];
            break;
        case 2:
            top = bottom = v[0];
            right = left = v[1];
            break;
        case 3:
            top = v[0];
            right = left = v[1];
            bottom = v[2];
            break;
        default:
            top = v[0];
            right = v[1];
            bottom = v[2];
            left = v[3];
            break;
    }
    Length vals[4] = {top, right, bottom, left};
    for (int i = 0; i < 4; ++i) {
        StyleDecl d;
        d.prop = order[i];
        d.value.kind = StyleValueKind::Length;
        d.value.len = vals[i];
        d.line = line;
        out.push_back(d);
    }
    return true;
}

bool parseBorderShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.empty() || toks.size() > 5) {  // width (1 tok) + color (up to 4 toks, e.g. rgba(...)).
        return false;
    }
    // Try "width first" then "color first" (order is arbitrary per 05.4).
    bool haveWidth = false, haveColor = false;
    Length width{};
    Color color{};
    Cursor cur(toks);
    while (!cur.atEnd()) {
        const Token& t = cur.peek();
        if (!haveColor) {
            Cursor probe = cur;
            Color c;
            if (parseColorCursor(probe, c)) {
                cur = probe;
                color = c;
                haveColor = true;
                continue;
            }
        }
        if (!haveWidth) {
            Length len;
            if (parseLengthTok(t, len)) {
                width = len;
                haveWidth = true;
                cur.advance();
                continue;
            }
        }
        return false;
    }
    if (!haveWidth || !haveColor) {
        return false;
    }
    const PropId widthProps[4] = {xt::prop::BorderTopWidth, xt::prop::BorderRightWidth,
                                  xt::prop::BorderBottomWidth, xt::prop::BorderLeftWidth};
    for (PropId p : widthProps) {
        StyleDecl d;
        d.prop = p;
        d.value.kind = StyleValueKind::Length;
        d.value.len = width;
        d.line = line;
        out.push_back(d);
    }
    StyleDecl dc;
    dc.prop = xt::prop::BorderColor;
    dc.value.kind = StyleValueKind::Color;
    dc.value.color = color;
    dc.line = line;
    out.push_back(dc);
    return true;
}

/// T-333 (05 §5.9.8): `border-color` with two to four colors, expanded by the CSS 1-4 value rule
/// onto the per-side colors (139-142). Returns false -- leaving the declaration to the plain
/// one-color path, which writes the legacy BorderColor -- unless `toks` is 2-4 colors.
bool parseBorderColorSides(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    Color c[4];
    size_t n = 0;
    Cursor cur(toks);
    while (!cur.atEnd()) {
        if (n == 4 || !parseColorCursor(cur, c[n])) {
            return false;
        }
        ++n;
    }
    if (n < 2) {
        return false;
    }
    const Color top = c[0];
    const Color right = c[1];
    const Color bottom = n >= 3 ? c[2] : c[0];
    const Color left = n == 4 ? c[3] : c[1];
    const Color sides[4] = {top, right, bottom, left};
    const PropId props[4] = {xt::prop::BorderTopColor, xt::prop::BorderRightColor,
                             xt::prop::BorderBottomColor, xt::prop::BorderLeftColor};
    for (int i = 0; i < 4; ++i) {
        StyleDecl d;
        d.prop = props[i];
        d.value.kind = StyleValueKind::Color;
        d.value.color = sides[i];
        d.line = line;
        out.push_back(d);
    }
    return true;
}

/// T-333 (05 §5.9.8): `border-style: <style>{1,4}` (none | solid | dashed | dotted), expanded by
/// the CSS 1-4 value rule onto the four `border-*-style` StyleProps.
bool parseBorderStyleShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.empty() || toks.size() > 4) {
        return false;
    }
    uint32_t v[4] = {};
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i].kind != TokenKind::Ident || !enumValue(styleprop::BorderTopStyle, toks[i].text, v[i])) {
            return false;
        }
    }
    const size_t n = toks.size();
    const uint32_t sides[4] = {v[0], n >= 2 ? v[1] : v[0], n >= 3 ? v[2] : v[0],
                               n == 4 ? v[3] : (n >= 2 ? v[1] : v[0])};
    const uint16_t props[4] = {styleprop::BorderTopStyle, styleprop::BorderRightStyle,
                               styleprop::BorderBottomStyle, styleprop::BorderLeftStyle};
    for (int i = 0; i < 4; ++i) {
        out.push_back(enumDecl(props[i], sides[i], line));
    }
    return true;
}

/// T-325 (05 §5.9.8): `outline: <width> <style> <color>`, each part optional and in any order
/// (at least one). An absent width / style takes its initial value (0, `none`); an absent color
/// writes no `outline-color` declaration, which leaves its initial value -- the element's `color`
/// -- in effect (there is no `currentcolor` value to write).
bool parseOutlineShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.empty()) {
        return false;
    }
    bool haveWidth = false, haveStyle = false, haveColor = false;
    Length width{0, LengthUnit::Px};
    uint32_t style = 0;
    Color color{};
    Cursor cur(toks);
    while (!cur.atEnd()) {
        const Token& t = cur.peek();
        if (!haveStyle && t.kind == TokenKind::Ident && (t.text == "none" || t.text == "solid")) {
            style = t.text == "solid" ? 1 : 0;
            haveStyle = true;
            cur.advance();
            continue;
        }
        if (!haveWidth) {
            Length len;
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
            Color c;
            if (parseColorCursor(probe, c)) {
                cur = probe;
                color = c;
                haveColor = true;
                continue;
            }
        }
        return false;
    }
    StyleDecl dw;
    dw.prop = xt::prop::OutlineWidth;
    dw.value.kind = StyleValueKind::Length;
    dw.value.len = width;
    dw.line = line;
    out.push_back(dw);
    out.push_back(enumDecl(styleprop::OutlineStyle, style, line));
    if (haveColor) {
        StyleDecl dc;
        dc.prop = xt::prop::OutlineColor;
        dc.value.kind = StyleValueKind::Color;
        dc.value.color = color;
        dc.line = line;
        out.push_back(dc);
    }
    return true;
}

}  // namespace xts_values_detail

namespace {

/// T-324 (05 §5.9.8): one `text-decoration-line` keyword's bit (xt::kTextDecoration*), or 0.
uint32_t textDecorationBit(const Token& t) {
    if (t.kind != TokenKind::Ident) {
        return 0;
    }
    if (t.text == "underline") {
        return xt::kTextDecorationUnderline;
    }
    if (t.text == "overline") {
        return xt::kTextDecorationOverline;
    }
    if (t.text == "line-through") {
        return xt::kTextDecorationLineThrough;
    }
    return 0;
}

}  // namespace

namespace xts_values_detail {

/// T-324: `text-decoration-line: none | [underline || overline || line-through]`, each keyword
/// at most once, in any order -> the bit set.
bool parseTextDecorationLine(const std::vector<Token>& toks, uint32_t& out) {
    out = 0;
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        return true;
    }
    for (const Token& t : toks) {
        const uint32_t bit = textDecorationBit(t);
        if (bit == 0 || (out & bit) != 0) {
            return false;
        }
        out |= bit;
    }
    return !toks.empty();
}

/// T-324: a `text-decoration-thickness` value: `auto` or a non-negative px / em length (no
/// percentage).
bool isDecorationThickness(const Length& len) {
    return len.unit == LengthUnit::Auto ||
           ((len.unit == LengthUnit::Px || len.unit == LengthUnit::Em) && len.value >= 0.0f);
}

/// T-324 (05 §5.9.8): `text-decoration: <line…> [<color>] [<thickness>]`, the parts in any order
/// (at least one). An absent line / thickness takes its initial value (`none`, `auto`); an absent
/// color writes no `text-decoration-color` declaration, which leaves its initial value -- the
/// element's `color` -- in effect (as parseOutlineShorthand does for `outline-color`).
bool parseTextDecorationShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.empty()) {
        return false;
    }
    bool haveLine = false, haveColor = false, haveThickness = false;
    uint32_t lines = 0;
    Length thickness{0, LengthUnit::Auto};
    Color color{};
    Cursor cur(toks);
    while (!cur.atEnd()) {
        const Token& t = cur.peek();
        if (!haveLine && t.kind == TokenKind::Ident && t.text == "none") {
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
            Length len;
            if (parseLengthTok(t, len) && isDecorationThickness(len)) {
                thickness = len;
                haveThickness = true;
                cur.advance();
                continue;
            }
        }
        if (!haveColor) {
            Cursor probe = cur;
            Color c;
            if (parseColorCursor(probe, c)) {
                cur = probe;
                color = c;
                haveColor = true;
                continue;
            }
        }
        return false;
    }
    out.push_back(enumDecl(styleprop::TextDecorationLine, lines, line));
    StyleDecl dt;
    dt.prop = xt::prop::TextDecorationThickness;
    dt.value.kind = StyleValueKind::Length;
    dt.value.len = thickness;
    dt.line = line;
    out.push_back(dt);
    if (haveColor) {
        StyleDecl dc;
        dc.prop = xt::prop::TextDecorationColor;
        dc.value.kind = StyleValueKind::Color;
        dc.value.color = color;
        dc.line = line;
        out.push_back(dc);
    }
    return true;
}

bool parseFlexShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        StyleDecl g, s, b;
        g.prop = xt::prop::FlexGrow;
        g.value.kind = StyleValueKind::Float;
        g.value.f = 0;
        g.line = line;
        s.prop = xt::prop::FlexShrink;
        s.value.kind = StyleValueKind::Float;
        s.value.f = 0;
        s.line = line;
        b.prop = xt::prop::FlexBasis;
        b.value.kind = StyleValueKind::Length;
        b.value.len = Length{0, LengthUnit::Auto};
        b.line = line;
        out.push_back(g);
        out.push_back(s);
        out.push_back(b);
        return true;
    }
    if (toks.empty() || toks.size() > 3) {
        return false;
    }
    float grow;
    if (!parseFloatTok(toks[0], grow)) {
        return false;
    }
    float shrink = 1;
    Length basis{0, LengthUnit::Px};
    size_t idx = 1;
    if (idx < toks.size() && toks[idx].kind == TokenKind::Number && toks[idx].unit.empty()) {
        if (!parseFloatTok(toks[idx], shrink)) {
            return false;
        }
        ++idx;
    }
    if (idx < toks.size()) {
        if (!parseLengthTok(toks[idx], basis)) {
            return false;
        }
        ++idx;
    }
    if (idx != toks.size()) {
        return false;
    }
    StyleDecl g, s, b;
    g.prop = xt::prop::FlexGrow;
    g.value.kind = StyleValueKind::Float;
    g.value.f = grow;
    g.line = line;
    s.prop = xt::prop::FlexShrink;
    s.value.kind = StyleValueKind::Float;
    s.value.f = shrink;
    s.line = line;
    b.prop = xt::prop::FlexBasis;
    b.value.kind = StyleValueKind::Length;
    b.value.len = basis;
    b.line = line;
    out.push_back(g);
    out.push_back(s);
    out.push_back(b);
    return true;
}

bool parseGapShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.empty() || toks.size() > 2) {
        return false;
    }
    Length row, col;
    if (!parseLengthTok(toks[0], row)) {
        return false;
    }
    col = row;
    if (toks.size() == 2 && !parseLengthTok(toks[1], col)) {
        return false;
    }
    StyleDecl dr, dc;
    dr.prop = xt::prop::RowGap;
    dr.value.kind = StyleValueKind::Length;
    dr.value.len = row;
    dr.line = line;
    dc.prop = xt::prop::ColumnGap;
    dc.value.kind = StyleValueKind::Length;
    dc.value.len = col;
    dc.line = line;
    out.push_back(dr);
    out.push_back(dc);
    return true;
}

// T-202 (03 §3.12.5): `particle-lifetime|speed|size|rotation: <a> [<b>]` -> the two longhands
// (one value sets both); `particle-color: <c> | <a> <b> | <c0> <c1> <c2> <c3>` -> the four color
// stops (t = 0, 1/3, 2/3, 1). Two colors become the stops of the straight a->b line, which the
// runtime's piecewise-linear interpolation reproduces exactly.
bool parseParticlePairShorthand(const std::vector<Token>& toks, xt::PropId first, xt::PropId second, int line,
                                std::vector<StyleDecl>& out) {
    if (toks.empty() || toks.size() > 2) {
        return false;
    }
    float v[2] = {0, 0};
    if (!parseFloatTok(toks[0], v[0])) {
        return false;
    }
    v[1] = v[0];
    if (toks.size() == 2 && !parseFloatTok(toks[1], v[1])) {
        return false;
    }
    const xt::PropId props[2] = {first, second};
    for (int i = 0; i < 2; ++i) {
        StyleDecl d;
        d.prop = props[i];
        d.value.kind = StyleValueKind::Float;
        d.value.f = v[i];
        d.line = line;
        out.push_back(d);
    }
    return true;
}

bool parseParticleColorShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out) {
    Cursor cur(toks);
    std::vector<Color> colors;
    while (!cur.atEnd()) {
        Color c;
        if (!parseColorCursor(cur, c)) {
            return false;
        }
        colors.push_back(c);
    }
    Color stops[4];
    if (colors.size() == 1) {
        stops[0] = stops[1] = stops[2] = stops[3] = colors[0];
    } else if (colors.size() == 2) {
        for (int i = 0; i < 4; ++i) {
            const float t = static_cast<float>(i) / 3.0f;
            const Color& a = colors[0];
            const Color& b = colors[1];
            stops[i] = Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                             a.a + (b.a - a.a) * t};
        }
    } else if (colors.size() == 4) {
        for (int i = 0; i < 4; ++i) {
            stops[i] = colors[static_cast<size_t>(i)];
        }
    } else {
        return false;
    }
    const xt::PropId props[4] = {xt::prop::ParticleColor0, xt::prop::ParticleColor1, xt::prop::ParticleColor2,
                                 xt::prop::ParticleColor3};
    for (int i = 0; i < 4; ++i) {
        StyleDecl d;
        d.prop = props[i];
        d.value.kind = StyleValueKind::Color;
        d.value.color = stops[i];
        d.line = line;
        out.push_back(d);
    }
    return true;
}

/// T-328 (06 §6.16): `aspect-ratio: auto | <w> / <h> | <number>` as one width / height Float, `auto`
/// = 0. Both numbers must be unitless and positive (a zero or negative ratio is dropped).
bool parseAspectRatio(const std::vector<Token>& toks, float& out) {
    const auto positive = [](const Token& t, float& v) {
        if (t.kind != TokenKind::Number || !t.unit.empty() || !(t.num > 0.0f)) {
            return false;
        }
        v = t.num;
        return true;
    };
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "auto") {
        out = 0.0f;
        return true;
    }
    float w = 0.0f;
    float h = 1.0f;
    if (toks.size() == 1) {
        if (!positive(toks[0], w)) {
            return false;
        }
        out = w;
        return true;
    }
    if (toks.size() != 3 || !positive(toks[0], w) || !isSlashTok(toks[1]) || !positive(toks[2], h)) {
        return false;
    }
    out = w / h;
    return true;
}

}  // namespace xts_values_detail

}  // namespace xt::compile
