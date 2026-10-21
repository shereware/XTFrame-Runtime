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

/// Per-property value parsing (split from xts_values.cpp by T-412): the effect / text-effect lists.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace {

// -------------------------------------------------------------------------------------------
// effect (docs/design/18-effects.md §18.3-18.4, docs/tasks/T-51a.md).
// -------------------------------------------------------------------------------------------

constexpr size_t kMaxEffects = 16;            // 18.2: "每節點 16 個效果".
constexpr size_t kMaxGradientMaskStops = 16;  // 18.2: "每個 GradientMask 16 個停靠點".
// Compile-time-only em -> px rule for effect lengths. Unlike the other Length-typed properties
// (05.7 step 4's runtime cascade, which resolves em relative to the element's actual font-size),
// EffectInstance/IrEffect store plain floats with no unit tag, so there is no later stage left to
// do this per-element. A fixed rate matching font-size's own 16px initial value (05.4) is the most
// conservative choice available at this compile stage; see the T-51a report's uncovered case.
constexpr float kEffectEmPx = 16.0f;

}  // namespace

namespace xts_values_detail {

// Consumes tokens between a matching parenthesis pair (`cur` must be positioned at the opening
// '('), returning the tokens strictly inside (nested parens, e.g. a color's `rgba(...)`, are kept
// intact in the result). Leaves `cur` positioned right after the closing ')'.
bool extractParenGroup(Cursor& cur, std::vector<Token>& inner) {
    if (cur.peek().kind != TokenKind::LParen) {
        return false;
    }
    cur.advance();
    int depth = 1;
    while (!cur.atEnd()) {
        const Token& t = cur.peek();
        if (t.kind == TokenKind::LParen) {
            ++depth;
            inner.push_back(cur.advance());
            continue;
        }
        if (t.kind == TokenKind::RParen) {
            --depth;
            if (depth == 0) {
                cur.advance();
                return true;
            }
            inner.push_back(cur.advance());
            continue;
        }
        inner.push_back(cur.advance());
    }
    return false;  // Unterminated.
}

}  // namespace xts_values_detail

namespace {

// Effect lengths (18.4): px/em only; `%` is a dedicated error ("effect lengths do not accept
// percent") rather than the generic invalid-value warning, signaled via `percentError`. `em` is
// resolved to px immediately (kEffectEmPx; see above).
bool readEffectLen(Cursor& cur, float& outPx, bool& percentError) {
    Length len;
    if (!parseLengthTok(cur.peek(), len)) {
        return false;
    }
    if (len.unit == LengthUnit::Percent) {
        percentError = true;
        return false;
    }
    if (len.unit != LengthUnit::Px && len.unit != LengthUnit::Em) {
        return false;
    }
    cur.advance();
    outPx = len.unit == LengthUnit::Em ? len.value * kEffectEmPx : len.value;
    return true;
}

// True for a token that could start an effect-length argument (used to detect shadow()'s optional
// blur/spread lengths without also matching a following bare number, which is never a length
// there -- shadow()'s only unitless number is the trailing opacity, and it always comes after the
// mandatory color).
bool isEffectLenToken(const Token& t) {
    return t.kind == TokenKind::Number &&
           (t.unit == "px" || t.unit == "em" || (t.unit.empty() && t.num == 0));
}

// `<color-stop>+` for `gradient-mask()`: `<color> <percent>` (18.4 says "同既有漸層停靠點語法";
// percent is required here -- there is no auto-distribution rule specified for evenly spacing
// stops with an omitted percent, so requiring it explicitly is the conservative choice, see report).
bool parseColorStopList(const std::vector<std::vector<Token>>& items, std::string_view fileName, int line,
                        DiagnosticSink& sink, std::vector<GradientStop>& out) {
    if (items.empty()) {
        sink.error(std::string(fileName), "gradient-mask requires at least one color stop", line);
        return false;
    }
    if (items.size() > kMaxGradientMaskStops) {
        sink.error(std::string(fileName), "gradient-mask has too many color stops (max 16)", line);
        return false;
    }
    for (const std::vector<Token>& item : items) {
        Cursor cur(item);
        Color c;
        if (!parseColorCursor(cur, c)) {
            sink.error(std::string(fileName), "invalid value for \"effect\": bad gradient-mask color stop",
                       line);
            return false;
        }
        if (cur.atEnd() || cur.peek().kind != TokenKind::Number || cur.peek().unit != "%") {
            sink.error(std::string(fileName),
                       "invalid value for \"effect\": gradient-mask color stop requires a percentage", line);
            return false;
        }
        float pct = cur.advance().num;
        if (!cur.atEnd()) {
            sink.error(std::string(fileName), "invalid value for \"effect\": bad gradient-mask color stop",
                       line);
            return false;
        }
        GradientStop stop;
        stop.offset = pct / 100.0f;
        stop.color = c;
        out.push_back(stop);
    }
    return true;
}

// shadow( <len> <len> [<len> [<len>]] <color> [<number>] [inset] ) -> DropShadow.
bool parseShadowFn(const std::vector<Token>& inner, bool& percentError, EffectDeclEntry& out) {
    out.kind = EffectKind::DropShadow;
    Cursor cur(inner);
    float offX, offY;
    if (!readEffectLen(cur, offX, percentError) || !readEffectLen(cur, offY, percentError)) {
        return false;
    }
    float extra[2] = {0, 0};
    int extraCount = 0;
    while (extraCount < 2 && isEffectLenToken(cur.peek())) {
        if (!readEffectLen(cur, extra[static_cast<size_t>(extraCount)], percentError)) {
            return false;
        }
        ++extraCount;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float opacity = 1;
    if (!cur.atEnd() && cur.peek().kind == TokenKind::Number) {
        if (!cur.peek().unit.empty()) {
            return false;
        }
        opacity = cur.advance().num;
    }
    // Trailing `inset` keyword (18.4); only valid last, so anything after it is an error.
    float insetFlag = 0;
    if (!cur.atEnd() && cur.peek().kind == TokenKind::Ident && cur.peek().text == "inset") {
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

// glow( <len> <color> [<number>] [inner] ) -> Glow.
bool parseGlowFn(const std::vector<Token>& inner, bool& percentError, EffectDeclEntry& out) {
    out.kind = EffectKind::Glow;
    Cursor cur(inner);
    float radius;
    if (!readEffectLen(cur, radius, percentError)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float intensity = 1;
    if (!cur.atEnd() && cur.peek().kind == TokenKind::Number && cur.peek().unit.empty()) {
        intensity = cur.advance().num;
    }
    float innerFlag = 0;
    if (!cur.atEnd() && cur.peek().kind == TokenKind::Ident && cur.peek().text == "inner") {
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

// blur( <len> [<len>] ) -> Blur.
bool parseBlurFn(const std::vector<Token>& inner, bool& percentError, EffectDeclEntry& out) {
    out.kind = EffectKind::Blur;
    Cursor cur(inner);
    float rx;
    if (!readEffectLen(cur, rx, percentError)) {
        return false;
    }
    float ry = rx;
    if (!cur.atEnd() && !readEffectLen(cur, ry, percentError)) {
        return false;
    }
    if (!cur.atEnd()) {
        return false;
    }
    out.params[0] = rx;
    out.params[1] = ry;
    return true;
}

// color-adjust( [brightness(n)] [contrast(n)] [saturate(n)] [hue-rotate(a)] [opacity(n)]
// [invert(n|%)] [sepia(n|%)] ) ->
// ColorAdjust. Each sub-function is optional and may appear in any order, at most once.
bool parseColorAdjustFn(const std::vector<Token>& inner, EffectDeclEntry& out) {
    out.kind = EffectKind::ColorAdjust;
    out.params[0] = 0;
    out.params[1] = 0;
    out.params[2] = 0;
    out.params[3] = 0;
    out.params[4] = 1;
    out.params[5] = 0;
    out.params[6] = 0;
    bool saw[7] = {false, false, false, false, false, false, false};
    Cursor cur(inner);
    while (!cur.atEnd()) {
        const Token& fnTok = cur.peek();
        if (fnTok.kind != TokenKind::Ident || cur.peek(1).kind != TokenKind::LParen) {
            return false;
        }
        std::string fn = fnTok.text;
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
        std::vector<Token> args;
        if (!extractParenGroup(cur, args)) {
            return false;
        }
        if (idx >= 5) {
            // T-334 (05 §5.9.8): invert()/sepia() take <number> or <percentage>, default 1 when
            // empty, clamped to 0..1 like CSS Filter Effects' own amounts.
            float amount = 1.0f;
            if (args.size() == 1) {
                const Token& a = args[0];
                if (a.kind != TokenKind::Number || (!a.unit.empty() && a.unit != "%")) {
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

// gradient-mask( linear|radial , <len> <len> , <len> <len> , <color-stop>+ [, invert] ) ->
// GradientMask.
bool parseGradientMaskFn(const std::vector<Token>& inner, std::string_view fileName, int line,
                         DiagnosticSink& sink, bool& percentError, EffectDeclEntry& out) {
    out.kind = EffectKind::GradientMask;
    std::vector<std::vector<Token>> items = splitByCommaTopLevel(inner);
    if (items.size() < 4) {
        return false;
    }
    if (items[0].size() != 1 || items[0][0].kind != TokenKind::Ident) {
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
    Cursor p0cur(items[1]);
    float p0x, p0y;
    if (!readEffectLen(p0cur, p0x, percentError) || !readEffectLen(p0cur, p0y, percentError) ||
        !p0cur.atEnd()) {
        return false;
    }
    Cursor p1cur(items[2]);
    float p1x, p1y;
    if (!readEffectLen(p1cur, p1x, percentError) || !readEffectLen(p1cur, p1y, percentError) ||
        !p1cur.atEnd()) {
        return false;
    }
    std::vector<std::vector<Token>> stopItems(items.begin() + 3, items.end());
    float invert = 0;
    if (!stopItems.empty()) {
        const std::vector<Token>& last = stopItems.back();
        if (last.size() == 1 && last[0].kind == TokenKind::Ident && last[0].text == "invert") {
            invert = 1;
            stopItems.pop_back();
        }
    }
    std::vector<GradientStop> stops;
    if (!parseColorStopList(stopItems, fileName, line, sink, stops)) {
        return false;
    }
    out.params[0] = p0x;
    out.params[1] = p0y;
    out.params[2] = p1x;
    out.params[3] = p1y;
    out.params[4] = radial;
    out.params[5] = invert;
    out.stops = std::move(stops);
    return true;
}

// shine( <angle> <len> <color> [<number> [<number> [<number>]]] ) -> Shine. The optional sixth
// argument is `position` (params[0], 0..1, default 0); 18.4 (T-55b) allows it to be set either
// this way or by animation. Out-of-range values are clamped to [0, 1] with a compiler warning.
bool parseShineFn(const std::vector<Token>& inner, std::string_view fileName, int line, DiagnosticSink& sink,
                  bool& percentError, EffectDeclEntry& out) {
    out.kind = EffectKind::Shine;
    Cursor cur(inner);
    float angle;
    if (!parseAngleTok(cur.peek(), angle)) {
        return false;
    }
    cur.advance();
    float width;
    if (!readEffectLen(cur, width, percentError)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float intensity = 1;
    float softness = 0.5f;
    float position = 0;
    if (!cur.atEnd() && cur.peek().kind == TokenKind::Number && cur.peek().unit.empty()) {
        intensity = cur.advance().num;
        if (!cur.atEnd() && cur.peek().kind == TokenKind::Number && cur.peek().unit.empty()) {
            softness = cur.advance().num;
            if (!cur.atEnd() && cur.peek().kind == TokenKind::Number && cur.peek().unit.empty()) {
                position = cur.advance().num;
                if (position < 0.0f || position > 1.0f) {
                    sink.warn(std::string(fileName), "shine() position out of range [0, 1], clamped", line);
                    position = std::clamp(position, 0.0f, 1.0f);
                }
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

// outline( <len> <color> [<len>] [outer|center|inner] ) -> Outline.
bool parseOutlineFn(const std::vector<Token>& inner, bool& percentError, EffectDeclEntry& out) {
    out.kind = EffectKind::Outline;
    Cursor cur(inner);
    float radius;
    if (!readEffectLen(cur, radius, percentError)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float softness = 0;
    if (isEffectLenToken(cur.peek()) && !readEffectLen(cur, softness, percentError)) {
        return false;
    }
    float position = 0;
    if (!cur.atEnd() && cur.peek().kind == TokenKind::Ident) {
        const std::string& kw = cur.peek().text;
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

// backdrop-blur( <len> [<number> [<number>]] ) -> BackdropBlur (docs/design/18-effects.md §18.4).
// `saturation`/`brightness` are plain numbers (§18.3's -1..1 range is not enforced here, same as
// color-adjust()'s own amounts: the backends clamp the resulting colour, and a Track can animate a
// parameter outside its nominal range anyway).
bool parseBackdropBlurFn(const std::vector<Token>& inner, bool& percentError, EffectDeclEntry& out) {
    out.kind = EffectKind::BackdropBlur;
    Cursor cur(inner);
    float radius;
    if (!readEffectLen(cur, radius, percentError)) {
        return false;
    }
    float extra[2] = {0, 0};
    int extraCount = 0;
    while (extraCount < 2 && !cur.atEnd() && cur.peek().kind == TokenKind::Number &&
           cur.peek().unit.empty()) {
        extra[static_cast<size_t>(extraCount)] = cur.advance().num;
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

}  // namespace

namespace xts_values_detail {

// `text-effect` (docs/design/07-text.md §7.8, T-178): a comma-separated list of
// `outline <w> <color>` (-> Outline, params[0] = w), `glow <r> <color>` (-> Glow, params[0] = r,
// params[1] = intensity 1) and `shadow <dx> <dy> <r> <color>` (-> DropShadow, params[0..2] =
// dx, dy, r, params[4] = opacity 1), or `none`. Lengths are px/em (effect rule above). Radii
// above 8 px are clamped with a warning (07.8's glyph-level cost cap). Any other shape: a warning
// and the declaration is dropped.
bool parseTextEffectList(const std::vector<Token>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<EffectDeclEntry>& out) {
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        return true;
    }
    auto invalid = [&]() {
        sink.warn(std::string(fileName), "invalid value for \"text-effect\"", line);
        return false;
    };
    constexpr float kMaxTextEffectRadius = 8.0f;
    bool clamped = false;
    auto clampRadius = [&](float& r) {
        if (r > kMaxTextEffectRadius) {
            r = kMaxTextEffectRadius;
            clamped = true;
        }
    };
    for (const std::vector<Token>& item : splitByCommaTopLevel(toks)) {
        Cursor cur(item);
        if (cur.atEnd() || cur.peek().kind != TokenKind::Ident) {
            return invalid();
        }
        const std::string kind = cur.advance().text;
        EffectDeclEntry entry;
        bool percentError = false;
        float a = 0, b = 0, r = 0;
        if (kind == "outline" || kind == "glow") {
            if (!readEffectLen(cur, r, percentError) || r < 0.0f) {
                return invalid();
            }
            clampRadius(r);
            entry.kind = kind == "outline" ? EffectKind::Outline : EffectKind::Glow;
            entry.params[0] = r;
            if (kind == "glow") {
                entry.params[1] = 1;  // intensity (18.3's default).
            }
        } else if (kind == "shadow") {
            if (!readEffectLen(cur, a, percentError) || !readEffectLen(cur, b, percentError) ||
                !readEffectLen(cur, r, percentError) || r < 0.0f) {
                return invalid();
            }
            clampRadius(r);
            entry.kind = EffectKind::DropShadow;
            entry.params[0] = a;
            entry.params[1] = b;
            entry.params[2] = r;
            entry.params[4] = 1;  // opacity (18.3's default).
        } else {
            return invalid();
        }
        Color color;
        if (!parseColorCursor(cur, color) || !cur.atEnd()) {
            return invalid();
        }
        entry.colors[0] = color;
        out.push_back(std::move(entry));
        if (out.size() > kMaxEffects) {
            sink.error(std::string(fileName), "text-effect must have at most 16 entries", line);
            return false;
        }
    }
    if (clamped) {
        sink.warn(std::string(fileName), "text-effect radius clamped to 8px", line);
    }
    return true;
}

}  // namespace xts_values_detail

namespace {

// custom( "<asset-name>" [, <ident> <value>]* ) -> Custom (T-203, docs/design/18-effects.md §18.4).
// `<value>` is one number (float), two numbers (vec2) or a color; the parameter's declared type is
// checked by style_emit.cpp against the asset's header. Every problem is an error (not the generic
// invalid-value warning), since a custom effect that silently drops out is hard to diagnose.
bool parseCustomFn(const std::vector<Token>& inner, std::string_view fileName, int line, DiagnosticSink& sink,
                   EffectDeclEntry& out) {
    Cursor cur(inner);
    if (cur.peek().kind != TokenKind::String || cur.peek().text.empty()) {
        sink.error(std::string(fileName),
                   "custom(): the first argument must be the .xtfx asset name in quotes", line);
        return false;
    }
    out.kind = EffectKind::Custom;
    out.customAsset = cur.advance().text;
    while (!cur.atEnd()) {
        if (cur.peek().kind != TokenKind::Comma) {
            sink.error(std::string(fileName), "custom(): expected ',' before a parameter override", line);
            return false;
        }
        cur.advance();
        if (cur.peek().kind != TokenKind::Ident) {
            sink.error(std::string(fileName), "custom(): expected '<param> <value>' after ','", line);
            return false;
        }
        CustomParamOverride ov;
        ov.name = cur.advance().text;
        ov.line = line;
        for (const CustomParamOverride& prev : out.customParams) {
            if (prev.name == ov.name) {
                sink.error(std::string(fileName), "custom(): parameter '" + ov.name + "' set twice", line);
                return false;
            }
        }
        Color color;
        if (cur.peek().kind == TokenKind::Hash || cur.peek().kind == TokenKind::Ident) {
            if (!parseColorCursor(cur, color)) {
                sink.error(std::string(fileName), "custom(): bad value for parameter '" + ov.name + "'",
                           line);
                return false;
            }
            ov.value[0] = color.r;
            ov.value[1] = color.g;
            ov.value[2] = color.b;
            ov.value[3] = color.a;
            ov.count = 4;
            ov.isColor = true;
        } else {
            while (cur.peek().kind == TokenKind::Number && ov.count < 2) {
                const Token& t = cur.peek();
                if (!(t.unit.empty() || t.unit == "px")) {
                    sink.error(std::string(fileName),
                               "custom(): parameter '" + ov.name + "' takes plain numbers (or px)", line);
                    return false;
                }
                ov.value[ov.count++] = t.num;
                cur.advance();
            }
            if (ov.count == 0) {
                sink.error(std::string(fileName),
                           "custom(): expected a number or color for parameter '" + ov.name + "'", line);
                return false;
            }
        }
        out.customParams.push_back(std::move(ov));
    }
    return true;
}

}  // namespace

namespace xts_values_detail {

bool parseEffectList(const std::vector<Token>& toks, std::string_view fileName, int line,
                     DiagnosticSink& sink, std::vector<EffectDeclEntry>& out) {
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        return true;  // Empty sequence (18.4: decl exists, count 0).
    }
    Cursor cur(toks);
    if (cur.atEnd()) {
        return false;
    }
    while (!cur.atEnd()) {
        const Token& fnTok = cur.peek();
        if (fnTok.kind != TokenKind::Ident || cur.peek(1).kind != TokenKind::LParen) {
            sink.warn(std::string(fileName), "invalid value for \"effect\"", line);
            return false;
        }
        std::string fn = fnTok.text;
        cur.advance();  // fn name.
        std::vector<Token> inner;
        if (!extractParenGroup(cur, inner)) {
            sink.warn(std::string(fileName), "invalid value for \"effect\": unterminated \"" + fn + "(\"",
                      line);
            return false;
        }
        EffectDeclEntry entry;
        bool percentError = false;
        size_t diagCountBefore = sink.all().size();
        bool ok;
        if (fn == "shadow") {
            ok = parseShadowFn(inner, percentError, entry);
        } else if (fn == "glow") {
            ok = parseGlowFn(inner, percentError, entry);
        } else if (fn == "blur") {
            ok = parseBlurFn(inner, percentError, entry);
        } else if (fn == "color-adjust") {
            ok = parseColorAdjustFn(inner, entry);
        } else if (fn == "gradient-mask") {
            ok = parseGradientMaskFn(inner, fileName, line, sink, percentError, entry);
        } else if (fn == "shine") {
            ok = parseShineFn(inner, fileName, line, sink, percentError, entry);
        } else if (fn == "outline") {
            ok = parseOutlineFn(inner, percentError, entry);
        } else if (fn == "custom") {
            // T-203 (18.4, 0.3): custom("<asset>" [, <ident> <value>]*). The asset is resolved
            // and its parameter table applied by style_emit.cpp; here only the shape is checked.
            ok = parseCustomFn(inner, fileName, line, sink, entry);
            if (!ok) {
                return false;  // parseCustomFn reported the error.
            }
        } else if (fn == "backdrop-blur") {
            // 18.6: BackdropBlur reads what was drawn below the node before its layer was opened,
            // so it is only meaningful as the stack's input stage.
            if (!out.empty()) {
                sink.error(std::string(fileName), "backdrop-blur must come first", line);
                return false;
            }
            ok = parseBackdropBlurFn(inner, percentError, entry);
        } else {
            sink.warn(std::string(fileName), "invalid value for \"effect\": unknown function \"" + fn + "\"",
                      line);
            return false;
        }
        if (!ok) {
            if (percentError) {
                sink.error(std::string(fileName), "effect lengths do not accept percent", line);
            } else if (sink.all().size() == diagCountBefore) {
                sink.warn(std::string(fileName),
                          "invalid value for \"effect\": bad arguments to \"" + fn + "\"", line);
            }
            return false;
        }
        out.push_back(std::move(entry));
        if (out.size() > kMaxEffects) {
            sink.error(std::string(fileName), "effect sequence must have at most 16 effects", line);
            return false;
        }
    }
    return true;
}

}  // namespace xts_values_detail

}  // namespace xt::compile
