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

/// Per-property value parsing (split from xts_values.cpp by T-412): background-gradient and clip-path.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace {

// -------------------------------------------------------------------------------------------
// background-gradient (05.5c, T-115).
// -------------------------------------------------------------------------------------------

constexpr size_t kMaxGradientStops = 16;  // Same per-gradient stop cap as GradientMask's (18.2).

// `<angle>` in 05.5c's `linear-gradient(<angle>, ...)`: deg / turn / rad, plus unitless 0.
// Normalized to degrees (the unit IrGradient stores).
bool parseGradientAngle(const Token& tok, float& deg) {
    if (tok.kind != TokenKind::Number) {
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

// A gradient geometry length (radial's rx/ry/cx/cy): `px` or `%` only. `em`/`auto` are rejected --
// the value survives unresolved all the way to Painter (a `%` needs the layout box), and no stage
// between here and there knows the element's font-size (see the T-115 report).
bool parseGradientLen(const Token& tok, Length& out) {
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

// Distributes the color stops whose position was omitted (05.5c: "可省略（平均分佈）"), following
// CSS: a missing first/last position is 0/1, a run of missing positions between two given ones is
// spaced evenly, and the result is clamped to be non-decreasing.
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
        size_t start = i - 1;  // Always known: index 0 was forced above.
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

// `<color-stop>+` for 05.5c: `<color> [<percent>]`. Unlike `gradient-mask()`'s stop list the
// position may be omitted (see distributeGradientStops); a `px` position is rejected, since a
// GradientStop offset is a 0..1 fraction of a gradient line whose length is only known after
// layout (see the T-115 report).
bool parseGradientStops(const std::vector<std::vector<Token>>& items, std::vector<GradientStop>& out) {
    if (items.size() < 2 || items.size() > kMaxGradientStops) {
        return false;
    }
    std::vector<bool> hasPos;
    for (const std::vector<Token>& item : items) {
        Cursor cur(item);
        Color c;
        if (!parseColorCursor(cur, c)) {
            return false;
        }
        GradientStop stop;
        stop.color = c;
        stop.offset = 0;
        bool given = false;
        if (!cur.atEnd()) {
            const Token& t = cur.peek();
            if (t.kind != TokenKind::Number || (t.unit != "%" && !(t.unit.empty() && t.num == 0))) {
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

// `background-gradient: none | linear-gradient(...) | radial-gradient(...) | conic-gradient(...)`
// (05.5c; conic T-201). A malformed
// value returns false with no diagnostic; the caller emits the generic "invalid value" warning and
// drops the declaration.
// -------------------------------------------------------------------------------------------
// `clip-path` (05 §5.9.8, T-332): `none | inset(<t> [<r> [<b> [<l>]]] [round <radii> [/ <radii>]])
// | circle([<r>] [at <x> <y>]) | ellipse([<rx> <ry>] [at <x> <y>]) | polygon(<x> <y>, ...)`. Lengths
// are px or % (of the border box; resolved by the runtime, like a gradient's); a circle / ellipse
// radius may also be `closest-side` / `farthest-side`. `path()` / `url()` are not supported.
// -------------------------------------------------------------------------------------------

bool parseClipLen(const Token& tok, Length& out) {
    Length len;
    if (!parseLengthTok(tok, len) || (len.unit != LengthUnit::Px && len.unit != LengthUnit::Percent)) {
        return false;
    }
    out = len;
    return true;
}

bool parseClipRadius(const Token& tok, Length& out) {
    if (tok.kind == TokenKind::Ident) {
        if (tok.text == "closest-side") {
            out = Length{kClipRadiusClosestSide, LengthUnit::Auto};
            return true;
        }
        if (tok.text == "farthest-side") {
            out = Length{kClipRadiusFarthestSide, LengthUnit::Auto};
            return true;
        }
        return false;
    }
    return parseClipLen(tok, out);
}

/// `[at <x> <y>]`; the center defaults to 50% 50%.
bool parseClipCenter(Cursor& cur, Length& cx, Length& cy) {
    cx = Length{50, LengthUnit::Percent};
    cy = Length{50, LengthUnit::Percent};
    if (cur.atEnd()) {
        return true;
    }
    if (cur.peek().kind != TokenKind::Ident || cur.peek().text != "at") {
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

/// 1-4 radii in border-radius order with its shorthand repetition; `out` receives 4 entries.
bool parseClipRadiiGroup(const std::vector<Token>& toks, Length out[4]) {
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

}  // namespace

namespace xts_values_detail {

/// Parses a `clip-path` value. `unsupported` is set for `path()` / `url()` (the caller words the
/// warning); false with it clear is a plain invalid value.
bool parseClipPathValue(const std::vector<Token>& toks, ElementClipShape& out, bool& unsupported) {
    unsupported = false;
    out.kind = ClipShapeKind::None;
    out.params.clear();
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        return true;
    }
    Cursor cur(toks);
    const Token& fnTok = cur.peek();
    if (fnTok.kind != TokenKind::Ident || cur.peek(1).kind != TokenKind::LParen) {
        return false;
    }
    const std::string fn = fnTok.text;
    if (fn == "path" || fn == "url") {
        unsupported = true;
        return false;
    }
    const bool inset = fn == "inset";
    const bool circle = fn == "circle";
    const bool ellipse = fn == "ellipse";
    const bool polygon = fn == "polygon";
    if (!inset && !circle && !ellipse && !polygon) {
        return false;
    }
    cur.advance();
    std::vector<Token> inner;
    if (!extractParenGroup(cur, inner) || !cur.atEnd()) {
        return false;
    }
    if (inset) {
        std::vector<Token> insets;
        std::vector<Token> horizontal;
        std::vector<Token> vertical;
        int section = 0;
        for (const Token& t : inner) {
            if (section == 0 && t.kind == TokenKind::Ident && t.text == "round") {
                section = 1;
                continue;
            }
            if (section == 1 && isSlashTok(t)) {
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
    Cursor in(inner);
    const auto atKeyword = [&in]() { return in.peek().kind == TokenKind::Ident && in.peek().text == "at"; };
    if (circle) {
        Length r{kClipRadiusClosestSide, LengthUnit::Auto};
        if (!in.atEnd() && !atKeyword()) {
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
        Length rx{kClipRadiusClosestSide, LengthUnit::Auto};
        Length ry{kClipRadiusClosestSide, LengthUnit::Auto};
        if (!in.atEnd() && !atKeyword()) {
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
    // polygon: at least 3 `<x> <y>` vertices, filled even-odd (05 §5.9.8).
    std::vector<std::vector<Token>> items = splitByCommaTopLevel(inner);
    if (items.size() < 3 || items.size() > kMaxClipPolygonVertices) {
        return false;
    }
    out.params.reserve(items.size() * 2);
    for (const std::vector<Token>& item : items) {
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

bool parseGradientValue(const std::vector<Token>& toks, GradientDecl& out) {
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        out.kind = GradientKind::None;
        return true;
    }
    Cursor cur(toks);
    const Token& fnTok = cur.peek();
    if (fnTok.kind != TokenKind::Ident || cur.peek(1).kind != TokenKind::LParen) {
        return false;
    }
    std::string fn = fnTok.text;
    cur.advance();
    std::vector<Token> inner;
    if (!extractParenGroup(cur, inner) || !cur.atEnd()) {
        return false;
    }
    std::vector<std::vector<Token>> items = splitByCommaTopLevel(inner);
    if (items.size() < 2) {
        return false;  // Needs the geometry group plus at least one stop.
    }
    Cursor head(items[0]);
    if (fn == "linear-gradient") {
        out.kind = GradientKind::Linear;
        if (!parseGradientAngle(head.peek(), out.angle)) {
            return false;
        }
        head.advance();
        if (!head.atEnd()) {
            return false;
        }
    } else if (fn == "radial-gradient") {
        out.kind = GradientKind::Radial;
        if (!parseGradientLen(head.peek(), out.rx)) {
            return false;
        }
        head.advance();
        if (!parseGradientLen(head.peek(), out.ry)) {
            return false;
        }
        head.advance();
        if (head.peek().kind != TokenKind::Ident || head.peek().text != "at") {
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
        if (!head.atEnd()) {
            return false;
        }
    } else if (fn == "conic-gradient") {
        // T-201 (05 §5.9.2): `[from] <angle> [at <len> <len>]`; the centre defaults to 50% 50%.
        out.kind = GradientKind::Conic;
        out.cx = Length{50, LengthUnit::Percent};
        out.cy = Length{50, LengthUnit::Percent};
        if (head.peek().kind == TokenKind::Ident && head.peek().text == "from") {
            head.advance();
        }
        if (!parseGradientAngle(head.peek(), out.angle)) {
            return false;
        }
        head.advance();
        if (head.peek().kind == TokenKind::Ident && head.peek().text == "at") {
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
    } else {
        return false;
    }
    std::vector<std::vector<Token>> stopItems(items.begin() + 1, items.end());
    return parseGradientStops(stopItems, out.stops);
}

}  // namespace xts_values_detail

}  // namespace xt::compile
