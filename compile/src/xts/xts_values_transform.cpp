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

/// Per-property value parsing (split from xts_values.cpp by T-412): transform, the independent transform
/// properties and the origin / background position values.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace {

// -------------------------------------------------------------------------------------------
// transform / transform-origin.
// -------------------------------------------------------------------------------------------

// Category order enforced by 05.5/19.10 (T-47a): translate*(0) -> rotateX(1) -> rotateY(2) ->
// rotate(3) -> skew(4) -> scale(5), each at most once (any of that category's function names
// counts). `rotate3d`/`scale3d`/`matrix3d`/`perspective()` are recognized here only so the caller
// can reject them with a dedicated "unsupported transform function" error instead of the generic
// "unknown function" one.
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

/// True for the 3D transform functions 05.5/19.10 explicitly disallows (T-47a): "rotate3d/scale3d/
/// matrix3d/perspective() -> compile error".
bool isUnsupportedTransformFn(std::string_view fn) {
    return fn == "rotate3d" || fn == "scale3d" || fn == "matrix3d" || fn == "perspective";
}

}  // namespace

namespace xts_values_detail {

bool parseAngleTok(const Token& tok, float& deg) {
    if (tok.kind != TokenKind::Number || tok.unit != "deg") {
        return false;
    }
    deg = tok.num;
    return true;
}

}  // namespace xts_values_detail

namespace {

/// 05.5's `none` clause (T-69): `transform: none` is the empty sequence, i.e. every component at
/// its identity. The identity declarations are emitted explicitly (rather than emitting nothing)
/// so that `none` also resets a lower-priority `transform` in the cascade and so that a
/// `@keyframes` stop written as `none` carries a real value instead of a neutral marker (a neutral
/// marker would make the whole span neutral, see core/src/track_eval.cpp).
void pushIdentityTransform(int line, std::vector<StyleDecl>& out) {
    auto push = [&](PropId p, StyleValueKind kind, float v) {
        StyleDecl d;
        d.prop = p;
        d.value.kind = kind;
        if (kind == StyleValueKind::Length) {
            d.value.len = Length{v, LengthUnit::Px};
        } else {
            d.value.f = v;
        }
        d.line = line;
        out.push_back(d);
    };
    push(xt::prop::TranslateX, StyleValueKind::Length, 0);
    push(xt::prop::TranslateY, StyleValueKind::Length, 0);
    // TranslateZ is a Float scene property (19.3/T-43c): emitting the identity as a Float keeps a
    // `@keyframes` stop written as `none` on a Float track, like the other components.
    push(xt::prop::TranslateZ, StyleValueKind::Float, 0);
    push(xt::prop::RotationX, StyleValueKind::Float, 0);
    push(xt::prop::RotationY, StyleValueKind::Float, 0);
    push(xt::prop::Rotation, StyleValueKind::Float, 0);
    push(xt::prop::SkewX, StyleValueKind::Float, 0);
    push(xt::prop::SkewY, StyleValueKind::Float, 0);
    push(xt::prop::ScaleX, StyleValueKind::Float, 1);
    push(xt::prop::ScaleY, StyleValueKind::Float, 1);
}

/// 05.5's fast path: a sequence that already follows the fixed order translate -> rotateX ->
/// rotateY -> rotate -> skew -> scale with each category at most once maps straight onto the
/// PropIds. Byte-identical to the pre-T-120 parser, including its diagnostics.
bool parseTransformFixedOrder(const std::vector<Token>& toks, std::string_view fileName, int line,
                              DiagnosticSink& sink, std::vector<StyleDecl>& out) {
    Cursor cur(toks);
    if (cur.atEnd()) {
        return false;
    }
    // 05.5 (T-69): a lone `none` keyword. Mixing it with functions (`none rotate(10deg)`) falls
    // through to the loop below and hits the existing "invalid value" diagnostic.
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        pushIdentityTransform(line, out);
        return true;
    }
    int lastCategory = -1;
    bool sawCategory[6] = {false, false, false, false, false, false};
    while (!cur.atEnd()) {
        const Token& fnTok = cur.peek();
        if (fnTok.kind != TokenKind::Ident || cur.peek(1).kind != TokenKind::LParen) {
            sink.warn(std::string(fileName), "invalid value for \"transform\"", line);
            return false;
        }
        std::string fn = fnTok.text;
        if (isUnsupportedTransformFn(fn)) {
            sink.error(std::string(fileName),
                       "invalid value for \"transform\": unsupported transform function \"" + fn + "\"",
                       line);
            return false;
        }
        int cat = transformCategory(fn);
        if (cat < 0) {
            sink.warn(std::string(fileName),
                      "invalid value for \"transform\": unknown function \"" + fn + "\"", line);
            return false;
        }
        if (sawCategory[cat] || cat < lastCategory) {
            sink.error(std::string(fileName),
                       "transform sequence must follow the order translate, rotateX, rotateY, rotate, "
                       "skew, scale, each at most once",
                       line);
            return false;
        }
        sawCategory[cat] = true;
        lastCategory = cat;
        cur.advance();  // fn name
        cur.advance();  // '('
        // translate/-X/-Y take Length args (px or %); rotate takes 1 angle; skew/-X/-Y take
        // angle args; scale/-X/-Y take unitless float args.
        auto readLengthArg = [&](Length& len) -> bool {
            if (!parseLengthTok(cur.peek(), len)) {
                return false;
            }
            if (len.unit != LengthUnit::Px && len.unit != LengthUnit::Percent && len.unit != LengthUnit::Em &&
                len.unit != LengthUnit::Rem) {
                return false;
            }
            cur.advance();
            return true;
        };
        // translateZ/translate3d's z component: 05.5/19.10 (T-47a) rejects percent explicitly
        // ("translateZ does not accept percent") instead of the generic invalid-value warning.
        bool zPercentError = false;
        auto readZLengthArg = [&](Length& len) -> bool {
            if (!parseLengthTok(cur.peek(), len)) {
                return false;
            }
            if (len.unit == LengthUnit::Percent) {
                zPercentError = true;
                return false;
            }
            if (len.unit != LengthUnit::Px && len.unit != LengthUnit::Em && len.unit != LengthUnit::Rem) {
                return false;
            }
            cur.advance();
            return true;
        };
        auto pushLen = [&](PropId p, Length len) {
            StyleDecl d;
            d.prop = p;
            d.value.kind = StyleValueKind::Length;
            d.value.len = len;
            d.line = line;
            out.push_back(d);
        };
        auto pushFloat = [&](PropId p, float v) {
            StyleDecl d;
            d.prop = p;
            d.value.kind = StyleValueKind::Float;
            d.value.f = v;
            d.line = line;
            out.push_back(d);
        };
        bool ok = true;
        if (fn == "translate") {
            Length x, y;
            ok = readLengthArg(x);
            if (ok) {
                pushLen(xt::prop::TranslateX, x);
                if (cur.peek().kind == TokenKind::Comma) {
                    cur.advance();
                    ok = readLengthArg(y);
                    if (ok) {
                        pushLen(xt::prop::TranslateY, y);
                    }
                }
            }
        } else if (fn == "translateX") {
            Length x;
            ok = readLengthArg(x);
            if (ok) {
                pushLen(xt::prop::TranslateX, x);
            }
        } else if (fn == "translateY") {
            Length y;
            ok = readLengthArg(y);
            if (ok) {
                pushLen(xt::prop::TranslateY, y);
            }
        } else if (fn == "translateZ") {
            Length z;
            ok = readZLengthArg(z);
            if (ok) {
                pushLen(xt::prop::TranslateZ, z);
            }
        } else if (fn == "translate3d") {
            Length x, y, z;
            ok = readLengthArg(x);
            if (ok && cur.peek().kind == TokenKind::Comma) {
                cur.advance();
                ok = readLengthArg(y);
            } else {
                ok = false;
            }
            if (ok && cur.peek().kind == TokenKind::Comma) {
                cur.advance();
                ok = readZLengthArg(z);
            } else {
                ok = false;
            }
            if (ok) {
                pushLen(xt::prop::TranslateX, x);
                pushLen(xt::prop::TranslateY, y);
                pushLen(xt::prop::TranslateZ, z);
            }
        } else if (fn == "rotateX") {
            float deg;
            ok = parseAngleTok(cur.peek(), deg);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::RotationX, deg);
            }
        } else if (fn == "rotateY") {
            float deg;
            ok = parseAngleTok(cur.peek(), deg);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::RotationY, deg);
            }
        } else if (fn == "rotate" || fn == "rotateZ") {
            float deg;
            ok = parseAngleTok(cur.peek(), deg);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::Rotation, deg);
            }
        } else if (fn == "skew") {
            float x, y;
            ok = parseAngleTok(cur.peek(), x);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::SkewX, x);
                if (cur.peek().kind == TokenKind::Comma) {
                    cur.advance();
                    ok = parseAngleTok(cur.peek(), y);
                    if (ok) {
                        cur.advance();
                        pushFloat(xt::prop::SkewY, y);
                    }
                }
            }
        } else if (fn == "skewX") {
            float x;
            ok = parseAngleTok(cur.peek(), x);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::SkewX, x);
            }
        } else if (fn == "skewY") {
            float y;
            ok = parseAngleTok(cur.peek(), y);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::SkewY, y);
            }
        } else if (fn == "scale") {
            float x, y;
            ok = parseFloatTok(cur.peek(), x);
            if (ok) {
                cur.advance();
                y = x;
                if (cur.peek().kind == TokenKind::Comma) {
                    cur.advance();
                    ok = parseFloatTok(cur.peek(), y);
                    if (ok) {
                        cur.advance();
                    }
                }
                if (ok) {
                    pushFloat(xt::prop::ScaleX, x);
                    pushFloat(xt::prop::ScaleY, y);
                }
            }
        } else if (fn == "scaleX") {
            float x;
            ok = parseFloatTok(cur.peek(), x);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::ScaleX, x);
            }
        } else if (fn == "scaleY") {
            float y;
            ok = parseFloatTok(cur.peek(), y);
            if (ok) {
                cur.advance();
                pushFloat(xt::prop::ScaleY, y);
            }
        }
        if (!ok || cur.peek().kind != TokenKind::RParen) {
            if (zPercentError) {
                sink.error(std::string(fileName), "translateZ does not accept percent", line);
            } else {
                sink.warn(std::string(fileName),
                          "invalid value for \"transform\": bad arguments to \"" + fn + "\"", line);
            }
            return false;
        }
        cur.advance();  // ')'
    }
    return true;
}

/// Pre-pass over the value tokens: true when the sequence repeats a category or breaks the fixed
/// order, i.e. when 05.5's compose/decompose path has to run. Only function names are inspected;
/// anything malformed (unknown or explicitly unsupported function, missing parenthesis) returns
/// false so parseTransformFixedOrder() reports it with the wording it always had.
bool transformNeedsCompose(const std::vector<Token>& toks) {
    int lastCategory = -1;
    bool sawCategory[6] = {false, false, false, false, false, false};
    bool needs = false;
    size_t i = 0;
    while (i < toks.size()) {
        if (toks[i].kind != TokenKind::Ident || i + 1 >= toks.size() ||
            toks[i + 1].kind != TokenKind::LParen) {
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
        while (i < toks.size() && toks[i].kind != TokenKind::RParen) {
            ++i;
        }
        if (i >= toks.size()) {
            return false;  // Unterminated argument list.
        }
        ++i;  // ')'
    }
    return needs;
}

/// Re-renders the value tokens as source text, for the "含宣告文字" part of 05.5's diagnostics.
std::string transformDeclText(const std::vector<Token>& toks) {
    std::string out = "transform:";
    for (const Token& t : toks) {
        switch (t.kind) {
            case TokenKind::Ident:
                out += ' ';
                out += t.text;
                break;
            case TokenKind::Number: {
                char buf[32];
                out.append(buf, xt::formatFloat(buf, sizeof(buf), static_cast<double>(t.num), 6));
                out += t.unit;
                break;
            }
            case TokenKind::LParen:
                out += '(';
                break;
            case TokenKind::RParen:
                out += ')';
                break;
            case TokenKind::Comma:
                out += ", ";
                break;
            default:
                break;
        }
    }
    return out;
}

/// Parses the whole sequence into 4x4-composable functions. Lengths must be px: a `%` or `em`
/// translate cannot be folded into a matrix at compile time (its value depends on the element's
/// box or font size), so it is rejected here rather than silently mis-composed.
bool parseTransformFnList(const std::vector<Token>& toks, std::string_view fileName, int line,
                          DiagnosticSink& sink, std::vector<xt::TransformFn>& out) {
    Cursor cur(toks);
    while (!cur.atEnd()) {
        const Token& fnTok = cur.peek();
        std::string fn = fnTok.text;
        cur.advance();  // fn name
        cur.advance();  // '(' (transformNeedsCompose() already checked both)
        bool ok = true;
        bool nonPx = false;
        auto readPx = [&](float& v) -> bool {
            Length len;
            if (!parseLengthTok(cur.peek(), len)) {
                return false;
            }
            if (len.unit != LengthUnit::Px) {
                nonPx = true;
                return false;
            }
            v = len.value;
            cur.advance();
            return true;
        };
        auto readAngle = [&](float& deg) -> bool {
            if (!parseAngleTok(cur.peek(), deg)) {
                return false;
            }
            cur.advance();
            return true;
        };
        auto readFloat = [&](float& v) -> bool {
            if (!parseFloatTok(cur.peek(), v)) {
                return false;
            }
            cur.advance();
            return true;
        };
        auto comma = [&]() -> bool {
            if (cur.peek().kind != TokenKind::Comma) {
                return false;
            }
            cur.advance();
            return true;
        };
        xt::TransformFn f;
        if (fn == "translate" || fn == "translateX" || fn == "translateY" || fn == "translateZ" ||
            fn == "translate3d") {
            f.kind = xt::TransformFn::Kind::Translate;
            float v = 0;
            ok = readPx(v);
            if (ok) {
                if (fn == "translateX" || fn == "translate") {
                    f.a = v;
                } else if (fn == "translateY") {
                    f.b = v;
                } else if (fn == "translateZ") {
                    f.c = v;
                } else {  // translate3d
                    f.a = v;
                    ok = comma() && readPx(f.b) && comma() && readPx(f.c);
                }
                if (ok && fn == "translate" && cur.peek().kind == TokenKind::Comma) {
                    cur.advance();
                    ok = readPx(f.b);
                }
            }
        } else if (fn == "rotateX" || fn == "rotateY" || fn == "rotate" || fn == "rotateZ") {
            f.kind = fn == "rotateX"   ? xt::TransformFn::Kind::RotateX
                     : fn == "rotateY" ? xt::TransformFn::Kind::RotateY
                                       : xt::TransformFn::Kind::RotateZ;
            ok = readAngle(f.a);
        } else if (fn == "matrix") {
            // T-322: matrix(a, b, c, d, e, f) with unitless numbers (e/f are px), decomposed into
            // translate/rotate/skewX/scale; a singular matrix is dropped with a warning.
            float m[6] = {1, 0, 0, 1, 0, 0};
            for (int k = 0; ok && k < 6; ++k) {
                ok = (k == 0 || comma()) && readFloat(m[k]);
            }
            if (ok && cur.peek().kind == TokenKind::RParen) {
                cur.advance();  // ')'
                xt::TransformFn parts[4];
                if (xt::decomposeMatrix2D(m[0], m[1], m[2], m[3], m[4], m[5], parts)) {
                    out.insert(out.end(), parts, parts + 4);
                } else {
                    sink.warn(std::string(fileName),
                              "transform matrix() is singular and cannot be decomposed; function dropped: " +
                                  transformDeclText(toks),
                              line);
                }
                continue;
            }
        } else if (fn == "skew" || fn == "skewX" || fn == "skewY") {
            f.kind = xt::TransformFn::Kind::Skew;
            float v = 0;
            ok = readAngle(v);
            if (ok) {
                if (fn == "skewY") {
                    f.b = v;
                } else {
                    f.a = v;
                    if (fn == "skew" && cur.peek().kind == TokenKind::Comma) {
                        cur.advance();
                        ok = readAngle(f.b);
                    }
                }
            }
        } else {  // scale / scaleX / scaleY
            f.kind = xt::TransformFn::Kind::Scale;
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
                    if (cur.peek().kind == TokenKind::Comma) {
                        cur.advance();
                        ok = readFloat(f.b);
                    }
                }
            }
        }
        if (!ok || cur.peek().kind != TokenKind::RParen) {
            if (nonPx) {
                sink.error(std::string(fileName),
                           "transform sequence needs matrix composition, so its lengths must be px "
                           "(% and em cannot be resolved at compile time): " +
                               transformDeclText(toks),
                           line);
            } else {
                sink.warn(std::string(fileName),
                          "invalid value for \"transform\": bad arguments to \"" + fn + "\"", line);
            }
            return false;
        }
        cur.advance();  // ')'
        out.push_back(f);
    }
    return true;
}

/// 05.5's slow path (T-120): compose the sequence into one 4x4 matrix and decompose it back into
/// 03.3's component set, so any order (`rotateY(18deg) translateZ(40px)` and friends) is legal.
bool parseTransformComposed(const std::vector<Token>& toks, std::string_view fileName, int line,
                            DiagnosticSink& sink, std::vector<StyleDecl>& out) {
    std::vector<xt::TransformFn> fns;
    if (!parseTransformFnList(toks, fileName, line, sink, fns)) {
        return false;
    }
    xt::TransformComponents c;
    if (xt::decomposeTransformSequence(fns.data(), fns.size(), c) != xt::TransformDecomposeStatus::Ok) {
        sink.error(std::string(fileName),
                   "transform sequence needs a 3D shear or z scale that XTFrame cannot represent: " +
                       transformDeclText(toks),
                   line);
        return false;
    }
    if (xt::transformSkewOutOfRange(c)) {
        sink.warn(std::string(fileName),
                  "transform sequence decomposes to a skew beyond +/-89 degrees: " + transformDeclText(toks),
                  line);
    }
    auto push = [&](PropId p, StyleValueKind kind, float v) {
        StyleDecl d;
        d.prop = p;
        d.value.kind = kind;
        if (kind == StyleValueKind::Length) {
            d.value.len = Length{v, LengthUnit::Px};
        } else {
            d.value.f = v;
        }
        d.line = line;
        out.push_back(d);
    };
    push(xt::prop::TranslateX, StyleValueKind::Length, c.translateX);
    push(xt::prop::TranslateY, StyleValueKind::Length, c.translateY);
    // TranslateZ is a Float scene property (19.3/T-43c), like pushIdentityTransform() emits.
    push(xt::prop::TranslateZ, StyleValueKind::Float, c.translateZ);
    push(xt::prop::RotationX, StyleValueKind::Float, c.rotationX);
    push(xt::prop::RotationY, StyleValueKind::Float, c.rotationY);
    push(xt::prop::Rotation, StyleValueKind::Float, c.rotation);
    push(xt::prop::SkewX, StyleValueKind::Float, c.skewX);
    push(xt::prop::SkewY, StyleValueKind::Float, c.skewY);
    push(xt::prop::ScaleX, StyleValueKind::Float, c.scaleX);
    push(xt::prop::ScaleY, StyleValueKind::Float, c.scaleY);
    return true;
}

}  // namespace

namespace xts_values_detail {

bool parseTransformShorthand(const std::vector<Token>& toks, std::string_view fileName, int line,
                             DiagnosticSink& sink, std::vector<StyleDecl>& out) {
    if (transformNeedsCompose(toks)) {
        return parseTransformComposed(toks, fileName, line, sink, out);
    }
    return parseTransformFixedOrder(toks, fileName, line, sink, out);
}

}  // namespace xts_values_detail

namespace {

// -------------------------------------------------------------------------------------------
// T-329 (05 §5.9.8): the independent `translate` / `rotate` / `scale` properties.
// -------------------------------------------------------------------------------------------

Token makeFnTok(TokenKind kind, const char* text = "") {
    Token t;
    t.kind = kind;
    t.text = text;
    return t;
}

/// Appends `fn(args...)` to `out`, with the argument tokens copied as written.
void appendFnTokens(const char* fn, const std::vector<Token>& args, std::vector<Token>& out) {
    out.push_back(makeFnTok(TokenKind::Ident, fn));
    out.push_back(makeFnTok(TokenKind::LParen));
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            out.push_back(makeFnTok(TokenKind::Comma));
        }
        out.push_back(args[i]);
    }
    out.push_back(makeFnTok(TokenKind::RParen));
}

}  // namespace

bool independentTransformFnTokens(std::string_view name, const std::vector<Token>& toks,
                                  std::string_view fileName, int line, DiagnosticSink& sink,
                                  std::vector<Token>& fnToks, bool& isNone) {
    fnToks.clear();
    isNone = false;
    auto invalid = [&]() {
        sink.warn(std::string(fileName), "invalid value for \"" + std::string(name) + "\"", line);
        return false;
    };
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        isNone = true;
        return true;
    }
    if (name == "translate") {
        // `<x> [<y> [<z>]]`: x/y as transform's translate() (px, % per 06 §6.14.1, em); z as
        // translateZ() (no %). A missing y is 0 (CSS), so the property always sets both axes.
        if (toks.empty() || toks.size() > 3) {
            return invalid();
        }
        for (size_t i = 0; i < toks.size(); ++i) {
            Length len;
            if (!parseLengthTok(toks[i], len)) {
                return invalid();
            }
            if (i == 2 && len.unit == LengthUnit::Percent) {
                sink.error(std::string(fileName), "translateZ does not accept percent", line);
                return false;
            }
            const bool okUnit = len.unit == LengthUnit::Px || len.unit == LengthUnit::Em ||
                                (i < 2 && len.unit == LengthUnit::Percent);
            if (!okUnit) {
                return invalid();
            }
        }
        std::vector<Token> args = toks;
        if (args.size() == 1) {
            Token zero;
            zero.kind = TokenKind::Number;
            zero.text = "0";
            zero.unit = "px";
            args.push_back(zero);
        }
        appendFnTokens(args.size() == 3 ? "translate3d" : "translate", args, fnToks);
        return true;
    }
    if (name == "rotate") {
        // `<angle> | z <angle>`; the x/y axes belong to transform's rotateX()/rotateY() (19 §19.10).
        size_t i = 0;
        if (!toks.empty() && toks[0].kind == TokenKind::Ident) {
            if (toks[0].text == "x" || toks[0].text == "y") {
                sink.warn(std::string(fileName),
                          "\"rotate: " + toks[0].text +
                              "\" is not supported (use transform: rotateX()/rotateY()); ignored",
                          line);
                return false;
            }
            if (toks[0].text != "z") {
                return invalid();
            }
            i = 1;
        }
        float deg = 0;
        if (toks.size() != i + 1 || !parseAngleTok(toks[i], deg)) {
            return invalid();
        }
        appendFnTokens("rotate", std::vector<Token>{toks[i]}, fnToks);
        return true;
    }
    // scale: `<sx> [<sy>]`, unitless; a missing sy is sx.
    if (toks.empty() || toks.size() > 2) {
        return invalid();
    }
    for (const Token& t : toks) {
        float v = 0;
        if (!parseFloatTok(t, v)) {
            return invalid();
        }
    }
    appendFnTokens("scale", toks, fnToks);
    return true;
}

namespace xts_values_detail {

/// One independent property on its own: its function through 05.5's fixed-order fast path, so it
/// writes exactly the PropIds `transform: <fn>(...)` would. `none` writes that property's own
/// components at their identity (translate: TranslateX/Y/Z; rotate: Rotation; scale: ScaleX/Y).
bool parseIndependentTransform(std::string_view name, const std::vector<Token>& toks,
                               std::string_view fileName, int line, DiagnosticSink& sink,
                               std::vector<StyleDecl>& out) {
    std::vector<Token> fnToks;
    bool isNone = false;
    if (!independentTransformFnTokens(name, toks, fileName, line, sink, fnToks, isNone)) {
        return false;
    }
    if (!isNone) {
        return parseTransformFixedOrder(fnToks, fileName, line, sink, out);
    }
    std::vector<StyleDecl> identity;
    pushIdentityTransform(line, identity);
    for (StyleDecl& d : identity) {
        const bool mine = name == "translate"
                              ? (d.prop == xt::prop::TranslateX || d.prop == xt::prop::TranslateY ||
                                 d.prop == xt::prop::TranslateZ)
                          : name == "rotate" ? d.prop == xt::prop::Rotation
                                             : (d.prop == xt::prop::ScaleX || d.prop == xt::prop::ScaleY);
        if (mine) {
            out.push_back(std::move(d));
        }
    }
    return true;
}

}  // namespace xts_values_detail

namespace {

// Left/center/right -> 0%/50%/100%; top/center/bottom -> 0%/50%/100%.
bool parseOriginKeywordOrLength(const Token& tok, bool horizontal, Length& out) {
    if (tok.kind == TokenKind::Ident) {
        if (tok.text == "center") {
            out = Length{50, LengthUnit::Percent};
            return true;
        }
        if (horizontal && tok.text == "left") {
            out = Length{0, LengthUnit::Percent};
            return true;
        }
        if (horizontal && tok.text == "right") {
            out = Length{100, LengthUnit::Percent};
            return true;
        }
        if (!horizontal && tok.text == "top") {
            out = Length{0, LengthUnit::Percent};
            return true;
        }
        if (!horizontal && tok.text == "bottom") {
            out = Length{100, LengthUnit::Percent};
            return true;
        }
        return false;
    }
    return parseLengthTok(tok, out);
}

// Shared by `transform-origin` (-> AnchorX/Y) and `perspective-origin` (-> PerspectiveOriginX/Y,
// 05.5/19.10, T-47a): same "1-2 values, left/center/right + top/center/bottom keywords or a
// length, missing 2nd value defaults to 50%" grammar.
bool parseOriginShorthand(const std::vector<Token>& toks, PropId xProp, PropId yProp,
                          std::vector<StyleDecl>& out, int line) {
    if (toks.empty() || toks.size() > 2) {
        return false;
    }
    Length x, y{50, LengthUnit::Percent};
    if (!parseOriginKeywordOrLength(toks[0], /*horizontal=*/true, x)) {
        return false;
    }
    if (toks.size() == 2 && !parseOriginKeywordOrLength(toks[1], /*horizontal=*/false, y)) {
        return false;
    }
    StyleDecl dx;
    dx.prop = xProp;
    dx.value.kind = StyleValueKind::Length;
    dx.value.len = x;
    dx.line = line;
    StyleDecl dy;
    dy.prop = yProp;
    dy.value.kind = StyleValueKind::Length;
    dy.value.len = y;
    dy.line = line;
    out.push_back(dx);
    out.push_back(dy);
    return true;
}

}  // namespace

namespace xts_values_detail {

bool parseTransformOrigin(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line) {
    return parseOriginShorthand(toks, xt::prop::AnchorX, xt::prop::AnchorY, out, line);
}

bool parsePerspectiveOrigin(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line) {
    return parseOriginShorthand(toks, xt::prop::PerspectiveOriginX, xt::prop::PerspectiveOriginY, out, line);
}

/// T-327 (05 §5.9.8): `background-position: <len|%|left|center|right> [<len|%|top|center|bottom>]`,
/// the origin grammar (a missing second value is `center`) into BackgroundPositionX/Y.
bool parseBackgroundPosition(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line) {
    return parseOriginShorthand(toks, xt::prop::BackgroundPositionX, xt::prop::BackgroundPositionY, out,
                                line);
}

}  // namespace xts_values_detail

namespace {

/// T-327: one `background-size` component: `auto` or a non-negative length / percentage.
bool parseBackgroundSizeComponent(const Token& tok, Length& out) {
    if (!parseLengthTok(tok, out)) {
        return false;
    }
    if (out.unit == LengthUnit::Auto) {
        out = Length{0, LengthUnit::Auto};
        return true;
    }
    return out.value >= 0.0f;
}

}  // namespace

namespace xts_values_detail {

/// T-327 (05 §5.9.8): `background-size: auto | cover | contain | <len|%> [<len|%>]` into
/// BackgroundSizeX/Y. A missing second value is `auto`; `cover` / `contain` are LengthUnit::Auto
/// lengths valued xt::kBackgroundSizeCover / kBackgroundSizeContain on both axes.
bool parseBackgroundSize(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line) {
    if (toks.empty() || toks.size() > 2) {
        return false;
    }
    Length x{0, LengthUnit::Auto};
    Length y{0, LengthUnit::Auto};
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident &&
        (toks[0].text == "cover" || toks[0].text == "contain")) {
        const float keyword = toks[0].text == "cover" ? xt::kBackgroundSizeCover : xt::kBackgroundSizeContain;
        x = Length{keyword, LengthUnit::Auto};
        y = x;
    } else {
        if (!parseBackgroundSizeComponent(toks[0], x)) {
            return false;
        }
        if (toks.size() == 2 && !parseBackgroundSizeComponent(toks[1], y)) {
            return false;
        }
    }
    for (int axis = 0; axis < 2; ++axis) {
        StyleDecl d;
        d.prop = axis == 0 ? xt::prop::BackgroundSizeX : xt::prop::BackgroundSizeY;
        d.value.kind = StyleValueKind::Length;
        d.value.len = axis == 0 ? x : y;
        d.line = line;
        out.push_back(d);
    }
    return true;
}

}  // namespace xts_values_detail

}  // namespace xt::compile
