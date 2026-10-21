#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

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

bool expand1to4(const PropId order[4], const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.empty() || toks.size() > 4) {
        return false;
    }
    Length v[4];
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!parseLengthTok(toks[i], v[i])) {
            return false;
        }
    }
    Length vals[4];
    css_values::expandSides(v, toks.size(), vals);
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

bool parseBorderShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.empty() || toks.size() > 5) {  // width (1 tok) + color (up to 4 toks, e.g. rgba(...)).
        return false;
    }
    // Try "width first" then "color first" (order is arbitrary per 05.4).
    bool haveWidth = false, haveColor = false;
    Length width{};
    Color color{};
    Cursor cur(toks);
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
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

/// T-333 (05 §5.9.8): `border-color` with two to four colors onto the per-side colors (139-142);
/// false (the plain one-color path, the legacy BorderColor) unless `toks` is 2-4 colors.
bool parseBorderColorSides(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
    Color sides[4];
    if (!css_values::parseBorderColorSides(toks.data(), toks.size(), sides)) {
        return false;
    }
    const PropId props[4] = {xt::prop::BorderTopColor, xt::prop::BorderRightColor,
                             xt::prop::BorderBottomColor, xt::prop::BorderLeftColor};
    for (int i = 0; i < 4; ++i) {
        appendPropValues({{props[i], Value::ofColor(sides[i])}}, line, out);
    }
    return true;
}

/// T-333 (05 §5.9.8): `border-style` 1-4 keywords onto the four side longhands.
bool parseBorderStyleShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
    uint32_t sides[4] = {};
    if (!css_values::parseBorderStyleSides(toks.data(), toks.size(), kCompiler, sides)) {
        return false;
    }
    const uint16_t props[4] = {styleprop::BorderTopStyle, styleprop::BorderRightStyle,
                               styleprop::BorderBottomStyle, styleprop::BorderLeftStyle};
    for (int i = 0; i < 4; ++i) {
        out.push_back(enumDecl(props[i], sides[i], line));
    }
    return true;
}

/// T-325 (05 §5.9.8): `outline` -> outline-width, outline-style and, when given, outline-color (an
/// absent color leaves its initial value, the element's `color`).
bool parseOutlineShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
    css_values::OutlineValue o;
    if (!css_values::parseOutline(toks.data(), toks.size(), kCompiler, o)) {
        return false;
    }
    appendPropValues({{xt::prop::OutlineWidth, Value::ofLength(o.width.value, o.width.unit)}}, line, out);
    out.push_back(enumDecl(styleprop::OutlineStyle, o.style, line));
    if (o.haveColor) {
        appendPropValues({{xt::prop::OutlineColor, Value::ofColor(o.color)}}, line, out);
    }
    return true;
}

/// T-324: `text-decoration-line: none | [underline || overline || line-through]` -> the bit set.
bool parseTextDecorationLine(const std::vector<Tok>& toks, uint32_t& out) {
    return css_values::parseTextDecorationLine(toks.data(), toks.size(), kCompiler, out);
}

/// T-324 (05 §5.9.8): `text-decoration` -> the line set, the thickness and, when given, the color
/// (an absent color leaves its initial value, the element's `color`).
bool parseTextDecorationShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
    css_values::TextDecorationValue t;
    if (!css_values::parseTextDecoration(toks.data(), toks.size(), kCompiler, t)) {
        return false;
    }
    out.push_back(enumDecl(styleprop::TextDecorationLine, t.lines, line));
    appendPropValues(
        {{xt::prop::TextDecorationThickness, Value::ofLength(t.thickness.value, t.thickness.unit)}}, line,
        out);
    if (t.haveColor) {
        appendPropValues({{xt::prop::TextDecorationColor, Value::ofColor(t.color)}}, line, out);
    }
    return true;
}

bool parseFlexShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
    if (toks.size() == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "none") {
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
    if (idx < toks.size() && toks[idx].kind == TokKind::Number && toks[idx].unit.empty()) {
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

bool parseGapShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
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
bool parseParticlePairShorthand(const std::vector<Tok>& toks, xt::PropId first, xt::PropId second, int line,
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

bool parseParticleColorShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out) {
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

/// T-328 (06 §6.16): `aspect-ratio: auto | <w> / <h> | <number>` as one width / height Float.
bool parseAspectRatio(const std::vector<Tok>& toks, float& out) {
    return css_values::parseAspectRatio(toks.data(), toks.size(), out);
}

}  // namespace xts_values_detail

}  // namespace xt::compile
