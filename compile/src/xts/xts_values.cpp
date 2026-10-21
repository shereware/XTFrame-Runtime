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

/// Per-property value parsing and shorthand expansion. See docs/design/05-style-and-css.md
/// §5.3-5.6, docs/tasks/T-22.md.

namespace xt::compile {

// Declared in xts_parser.cpp; defined here.
bool parseAndExpandDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName,
                        int line, DiagnosticSink& sink, std::vector<StyleDecl>& outDecls,
                        std::vector<EasingPoint>* easingPoints = nullptr);
bool parseEasingValue(const std::vector<Token>& tokens, Easing& out, bool* stepsClamped = nullptr,
                      std::vector<EasingPoint>* easingPoints = nullptr);
bool containsVarFunction(const std::vector<Token>& tokens);
// T-329: also used by xts_parser.cpp to combine `translate`/`rotate`/`scale` with `transform`.
bool independentTransformFnTokens(std::string_view name, const std::vector<Token>& toks,
                                  std::string_view fileName, int line, DiagnosticSink& sink,
                                  std::vector<Token>& fnToks, bool& isNone);
bool isCalcFunctionValue(const std::vector<Token>& tokens);
bool parseCalcValue(const std::vector<Token>& tokens, std::vector<CalcOpDecl>& out, std::string& error);

namespace {

// -------------------------------------------------------------------------------------------
// Token cursor: a read-only walk over a declaration's value tokens.
// -------------------------------------------------------------------------------------------

class Cursor {
public:
    explicit Cursor(const std::vector<Token>& toks) : m_toks(&toks) {}
    const Token& peek(size_t ahead = 0) const {
        size_t i = m_idx + ahead;
        return i < m_toks->size() ? (*m_toks)[i] : kEndToken;
    }
    const Token& advance() {
        const Token& t = peek();
        if (m_idx < m_toks->size()) {
            ++m_idx;
        }
        return t;
    }
    bool atEnd() const { return m_idx >= m_toks->size(); }

private:
    static const Token kEndToken;
    const std::vector<Token>* m_toks;
    size_t m_idx = 0;
};
const Token Cursor::kEndToken{};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// -------------------------------------------------------------------------------------------
// Length.
// -------------------------------------------------------------------------------------------

// Generic px/%/em/auto length. Unitless is only accepted for 0 (-> 0px); callers for the
// unitless-allowed properties (flex-grow/shrink, opacity, z-index, image-opacity) use
// parseFloatTok instead, and line-height handles unitless specially (-> Em) in parseSingleValue.
bool parseLengthTok(const Token& tok, Length& out) {
    if (tok.kind == TokenKind::Ident && tok.text == "auto") {
        out = Length{0, LengthUnit::Auto};
        return true;
    }
    if (tok.kind != TokenKind::Number) {
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
    if (tok.unit == "rem") {  // T-335: the root element's font size, resolved like em at style time.
        out = Length{tok.num, LengthUnit::Rem};
        return true;
    }
    if (tok.unit.empty() && tok.num == 0) {
        out = Length{0, LengthUnit::Px};
        return true;
    }
    // T-176 (03.12.6): viewport, safe-area and uiscale units.
    static constexpr struct {
        const char* name;
        LengthUnit unit;
    } kDeviceUnits[] = {
        {"vw", LengthUnit::Vw},
        {"vh", LengthUnit::Vh},
        {"vmin", LengthUnit::Vmin},
        {"vmax", LengthUnit::Vmax},
        {"safe-top", LengthUnit::SafeTop},
        {"safe-right", LengthUnit::SafeRight},
        {"safe-bottom", LengthUnit::SafeBottom},
        {"safe-left", LengthUnit::SafeLeft},
        {"uiscale", LengthUnit::UiScale},
    };
    for (const auto& entry : kDeviceUnits) {
        if (tok.unit == entry.name) {
            out = Length{tok.num, entry.unit};
            return true;
        }
    }
    return false;
}

bool parseFloatTok(const Token& tok, float& out) {
    if (tok.kind != TokenKind::Number || !tok.unit.empty()) {
        return false;
    }
    out = tok.num;
    return true;
}

// -------------------------------------------------------------------------------------------
// Color.
// -------------------------------------------------------------------------------------------

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

bool parseHashColor(const std::string& hex, Color& out) {
    bool ok = true;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    if (hex.size() == 3) {
        out.r = static_cast<float>(hexNibble(hex[0], ok)) * 17.0f / 255.0f;
        out.g = static_cast<float>(hexNibble(hex[1], ok)) * 17.0f / 255.0f;
        out.b = static_cast<float>(hexNibble(hex[2], ok)) * 17.0f / 255.0f;
        out.a = 1;
    } else if (hex.size() == 4) {
        out.r = static_cast<float>(hexNibble(hex[0], ok)) * 17.0f / 255.0f;
        out.g = static_cast<float>(hexNibble(hex[1], ok)) * 17.0f / 255.0f;
        out.b = static_cast<float>(hexNibble(hex[2], ok)) * 17.0f / 255.0f;
        out.a = static_cast<float>(hexNibble(hex[3], ok)) * 17.0f / 255.0f;
    } else if (hex.size() == 6) {
        out.r = static_cast<float>(hexByte(hex[0], hex[1], ok)) / 255.0f;
        out.g = static_cast<float>(hexByte(hex[2], hex[3], ok)) / 255.0f;
        out.b = static_cast<float>(hexByte(hex[4], hex[5], ok)) / 255.0f;
        out.a = 1;
    } else if (hex.size() == 8) {
        out.r = static_cast<float>(hexByte(hex[0], hex[1], ok)) / 255.0f;
        out.g = static_cast<float>(hexByte(hex[2], hex[3], ok)) / 255.0f;
        out.b = static_cast<float>(hexByte(hex[4], hex[5], ok)) / 255.0f;
        out.a = static_cast<float>(hexByte(hex[6], hex[7], ok)) / 255.0f;
    } else {
        return false;
    }
    return ok;
}

// r/g/b: 0..255 or percent (0..100%). Returns the channel in [0,1].
bool parseRgbChannel(const Token& tok, float& out) {
    if (tok.kind != TokenKind::Number) {
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

/// T-322: the `/` of `hsl(h s l / a)`. The lexer has no token for it; like calc()'s `/`, it
/// arrives as an Unknown "unexpected character" token.
bool isSlashTok(const Token& t) {
    return t.kind == TokenKind::Unknown && t.text == "unexpected character '/'";
}

/// T-322: an `hsl()` hue: a number (degrees), `deg`, `turn` or `rad`.
bool parseHueTok(const Token& t, float& deg) {
    if (t.kind != TokenKind::Number) {
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
bool parseAlphaTok(const Token& t, float& a) {
    if (t.kind != TokenKind::Number) {
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

/// T-322: the arguments of `hsl()`/`hsla()` (the two names are aliases), `cur` at the '(':
/// `(h, s%, l%[, a])` or `(h s% l%[ / a])`. Saturation and lightness must be percentages.
bool parseHslArgs(Cursor& cur, Color& out) {
    cur.advance();  // '('
    float h = 0;
    if (!parseHueTok(cur.peek(), h)) {
        return false;
    }
    cur.advance();
    const bool legacy = cur.peek().kind == TokenKind::Comma;
    float sl[2] = {0, 0};
    for (float& v : sl) {
        if (legacy) {
            if (cur.peek().kind != TokenKind::Comma) {
                return false;
            }
            cur.advance();
        }
        const Token& t = cur.peek();
        if (t.kind != TokenKind::Number || t.unit != "%") {
            return false;
        }
        v = t.num / 100.0f;
        cur.advance();
    }
    float a = 1;
    if (legacy ? cur.peek().kind == TokenKind::Comma : isSlashTok(cur.peek())) {
        cur.advance();
        if (!parseAlphaTok(cur.peek(), a)) {
            return false;
        }
        cur.advance();
    }
    if (cur.peek().kind != TokenKind::RParen) {
        return false;
    }
    cur.advance();
    out = xt::css_color::hslToColor(h, sl[0], sl[1], a);
    return true;
}

// Parses a color starting at cur (which must be positioned at the first token of the color).
// Consumes exactly the tokens that make up the color.
bool parseColorCursor(Cursor& cur, Color& out) {
    const Token& t = cur.peek();
    if (t.kind == TokenKind::Hash) {
        Color c;
        if (!parseHashColor(t.text, c)) {
            return false;
        }
        cur.advance();
        out = c;
        return true;
    }
    if (t.kind == TokenKind::Ident) {
        std::string name = lower(t.text);
        if (name == "transparent") {
            cur.advance();
            out = Color{0, 0, 0, 0};
            return true;
        }
        if (name == "rgb" || name == "rgba") {
            bool wantAlpha = (name == "rgba");
            cur.advance();
            if (cur.peek().kind != TokenKind::LParen) {
                return false;
            }
            cur.advance();
            Color c{0, 0, 0, 1};
            float ch[4] = {0, 0, 0, 1};
            int need = wantAlpha ? 4 : 3;
            for (int i = 0; i < need; ++i) {
                if (i > 0) {
                    if (cur.peek().kind != TokenKind::Comma) {
                        return false;
                    }
                    cur.advance();
                }
                if (i < 3) {
                    if (!parseRgbChannel(cur.peek(), ch[i])) {
                        return false;
                    }
                    cur.advance();
                } else {
                    const Token& a = cur.peek();
                    if (a.kind != TokenKind::Number || !a.unit.empty()) {
                        return false;
                    }
                    ch[3] = a.num;
                    cur.advance();
                }
            }
            if (cur.peek().kind != TokenKind::RParen) {
                return false;
            }
            cur.advance();
            c = Color{ch[0], ch[1], ch[2], ch[3]};
            out = c;
            return true;
        }
        if ((name == "hsl" || name == "hsla") && cur.peek(1).kind == TokenKind::LParen) {
            cur.advance();
            return parseHslArgs(cur, out);
        }
        // T-322: the CSS Color Level 4 named colours (core/src/css_color.h).
        Color named;
        if (xt::css_color::findNamedColor(name, named)) {
            cur.advance();
            out = named;
            return true;
        }
    }
    return false;
}

bool parseColorTokens(const std::vector<Token>& toks, Color& out) {
    Cursor cur(toks);
    if (!parseColorCursor(cur, out)) {
        return false;
    }
    return cur.atEnd();
}

// -------------------------------------------------------------------------------------------
// Easing (shared by transition/animation lists and @keyframes' animation-timing-function).
// -------------------------------------------------------------------------------------------

/// T-258b (D11): the range of `steps(n)`; an `n` outside it is clamped (`stepsClamped` set, the
/// caller warns).
constexpr float kMinSteps = 1.0f;
constexpr float kMaxSteps = 65535.0f;
constexpr const char* kStepsClampedMessage = "steps() count clamped to [1, 65535]";
/// T-258b (D11): the largest finite `animation` iteration count; 0xFFFF means `infinite`.
constexpr uint16_t kMaxFiniteIterations = 0xFFFE;

/// T-335: `linear(<stop>[, <stop>]...)` with the cursor on `linear`; each stop is `<number>
/// [<percentage>]{0,2}` (CSS Easing 1). The canonical points (linearEasingPoints()) are appended to
/// `points` and `out` indexes them there. Fails for fewer than two stops, or when the pool would
/// pass the u16 offset / count of a runtime Easing.
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
        while (stop.inputCount < 2 && cur.peek().kind == TokenKind::Number && cur.peek().unit == "%") {
            stop.in[stop.inputCount++] = cur.peek().num / 100.0f;
            cur.advance();
        }
        stops.push_back(stop);
        if (cur.peek().kind == TokenKind::Comma) {
            cur.advance();
            continue;
        }
        if (cur.peek().kind != TokenKind::RParen) {
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

/// T-335: `linear()` needs `easingPoints` (its points go there); without it, it is rejected.
bool parseEasingCursor(Cursor& cur, Easing& out, bool* stepsClamped = nullptr,
                       std::vector<EasingPoint>* easingPoints = nullptr) {
    const Token& t = cur.peek();
    if (t.kind != TokenKind::Ident) {
        return false;
    }
    if (t.text == "linear" && cur.peek(1).kind == TokenKind::LParen) {
        return easingPoints != nullptr && parseLinearFunction(cur, out, *easingPoints);
    }
    if (t.text == "linear") {
        cur.advance();
        out = Easing::linear();
        return true;
    }
    if (t.text == "ease") {
        cur.advance();
        out = Easing::cssEase();
        return true;
    }
    if (t.text == "ease-in") {
        cur.advance();
        out = Easing::cssEaseIn();
        return true;
    }
    if (t.text == "ease-out") {
        cur.advance();
        out = Easing::cssEaseOut();
        return true;
    }
    if (t.text == "ease-in-out") {
        cur.advance();
        out = Easing::cssEaseInOut();
        return true;
    }
    // T-322: CSS's step-start / step-end = steps(1, jump-start) / steps(1, jump-end).
    if (t.text == "step-start" || t.text == "step-end") {
        out = Easing::steps(1, t.text == "step-end");
        cur.advance();
        return true;
    }
    if (t.text == "cubic-bezier" && cur.peek(1).kind == TokenKind::LParen) {
        cur.advance();
        cur.advance();
        float v[4];
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                if (cur.peek().kind != TokenKind::Comma) {
                    return false;
                }
                cur.advance();
            }
            if (!parseFloatTok(cur.peek(), v[i])) {
                return false;
            }
            cur.advance();
        }
        if (cur.peek().kind != TokenKind::RParen) {
            return false;
        }
        cur.advance();
        out = Easing::bezier(v[0], v[1], v[2], v[3]);
        return true;
    }
    if (t.text == "steps" && cur.peek(1).kind == TokenKind::LParen) {
        cur.advance();
        cur.advance();
        float n;
        if (!parseFloatTok(cur.peek(), n) || n < 0) {
            return false;
        }
        cur.advance();
        bool jumpEnd = true;
        if (cur.peek().kind == TokenKind::Comma) {
            cur.advance();
            const Token& kw = cur.peek();
            if (kw.kind != TokenKind::Ident || (kw.text != "start" && kw.text != "end")) {
                return false;
            }
            jumpEnd = (kw.text == "end");
            cur.advance();
        }
        if (cur.peek().kind != TokenKind::RParen) {
            return false;
        }
        cur.advance();
        if (!(n >= kMinSteps && n <= kMaxSteps)) {
            n = xt::clampFinite(n, kMinSteps, kMaxSteps, kMinSteps);
            if (stepsClamped != nullptr) {
                *stepsClamped = true;
            }
        }
        out = Easing::steps(static_cast<uint16_t>(n), jumpEnd);
        return true;
    }
    return false;
}

bool isEasingStart(const Cursor& cur) {
    const Token& t = cur.peek();
    if (t.kind != TokenKind::Ident) {
        return false;
    }
    return t.text == "linear" || t.text == "ease" || t.text == "ease-in" || t.text == "ease-out" ||
           t.text == "ease-in-out" || t.text == "cubic-bezier" || t.text == "steps" ||
           t.text == "step-start" || t.text == "step-end";
}

// -------------------------------------------------------------------------------------------
// font-family.
// -------------------------------------------------------------------------------------------

bool parseFontFamilyList(const std::vector<Token>& toks, std::vector<std::string>& out) {
    Cursor cur(toks);
    if (cur.atEnd()) {
        return false;
    }
    for (;;) {
        if (cur.peek().kind == TokenKind::String) {
            out.push_back(cur.advance().text);
        } else if (cur.peek().kind == TokenKind::Ident) {
            std::string name = cur.advance().text;
            while (cur.peek().kind == TokenKind::Ident) {
                name += ' ';
                name += cur.advance().text;
            }
            out.push_back(std::move(name));
        } else {
            return false;
        }
        if (cur.peek().kind == TokenKind::Comma) {
            cur.advance();
            continue;
        }
        break;
    }
    return cur.atEnd() && !out.empty();
}

// -------------------------------------------------------------------------------------------
// background-image.
// -------------------------------------------------------------------------------------------

// -------------------------------------------------------------------------------------------
// border-image (T-199, docs/design/05-style-and-css.md §5.9.2, 26 §26.3.4):
//   border-image: <url(x) | none> [<L> [<R> <T> <B>]] [fill] [stretch | repeat | round]
//   border-image-slice: <L> [<R> <T> <B>] [fill] | auto
// Slices are unitless numbers (source pixels) or percentages; encoded per style_decode.h's
// StyleProp 198 (a Color-kind value: >= 0 pixels, a negative -f = f * 100 %, NaN = auto).
// -------------------------------------------------------------------------------------------

bool parseBackgroundImage(const std::vector<Token>& toks, std::string& out);

/// Reads up to four slice numbers at `i`; 1 value = all sides, 2 = L/R then T/B, 4 = L R T B.
/// False on 3 values or a number with any unit but `%`. `any` = at least one value read.
bool parseSliceValues(const std::vector<Token>& toks, size_t& i, float out[4], bool& any) {
    float v[4] = {0, 0, 0, 0};
    size_t n = 0;
    while (i < toks.size() && n < 4 && toks[i].kind == TokenKind::Number) {
        const Token& t = toks[i];
        if (!t.unit.empty() && t.unit != "%") {
            return false;
        }
        if (t.num < 0.0f) {
            return false;
        }
        v[n++] = t.unit == "%" ? -(t.num / 100.0f) : t.num;
        ++i;
    }
    any = n > 0;
    switch (n) {
        case 0:
            return true;
        case 1:
            out[0] = out[1] = out[2] = out[3] = v[0];
            return true;
        case 2:
            out[0] = out[1] = v[0];
            out[2] = out[3] = v[1];
            return true;
        case 4:
            for (int k = 0; k < 4; ++k) {
                out[k] = v[k];
            }
            return true;
        default:
            return false;
    }
}

StyleDecl sliceDecl(const float v[4], int line) {
    StyleDecl d;
    d.prop = styleprop::BorderImageSlice;
    d.value.kind = StyleValueKind::Color;
    d.value.color = Color{v[0], v[1], v[2], v[3]};
    d.line = line;
    return d;
}

StyleDecl enumDecl(uint16_t prop, uint32_t e, int line) {
    StyleDecl d;
    d.prop = prop;
    d.value.kind = StyleValueKind::Enum;
    d.value.e = e;
    d.line = line;
    return d;
}

/// `border-image-slice` longhand: values (or `auto`) plus an optional `fill`.
bool parseBorderImageSlice(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls) {
    size_t i = 0;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    float v[4] = {nan, nan, nan, nan};
    bool any = false;
    if (i < toks.size() && toks[i].kind == TokenKind::Ident && toks[i].text == "auto") {
        ++i;
    } else if (!parseSliceValues(toks, i, v, any) || !any) {
        return false;
    }
    bool fill = false;
    if (i < toks.size() && toks[i].kind == TokenKind::Ident && toks[i].text == "fill") {
        fill = true;
        ++i;
    }
    if (i != toks.size()) {
        return false;
    }
    decls.push_back(sliceDecl(v, line));
    if (fill) {
        decls.push_back(enumDecl(styleprop::BorderImageFill, 1, line));
    }
    return true;
}

/// T-345 (05 §5.9.9): `ui-scale: auto | none | clamp(<min>, <max>)` -> one Color-encoded
/// declaration (mode, min, max, 0); a clamp needs 0 < min <= max.
bool parseUiScale(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls) {
    float v[4] = {0, 0, 0, 0};
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident) {
        if (toks[0].text == "auto") {
            v[0] = 0;
        } else if (toks[0].text == "none") {
            v[0] = 1;
        } else {
            return false;
        }
    } else if (toks.size() == 6 && toks[0].kind == TokenKind::Ident && toks[0].text == "clamp" &&
               toks[1].kind == TokenKind::LParen && toks[3].kind == TokenKind::Comma &&
               toks[5].kind == TokenKind::RParen && parseFloatTok(toks[2], v[1]) &&
               parseFloatTok(toks[4], v[2])) {
        if (!(v[1] > 0.0f) || !(v[2] >= v[1]) || !std::isfinite(v[2])) {
            return false;
        }
        v[0] = 2;
    } else {
        return false;
    }
    StyleDecl d;
    d.prop = styleprop::UiScale;
    d.value.kind = StyleValueKind::Color;
    d.value.color = Color{v[0], v[1], v[2], v[3]};
    d.line = line;
    decls.push_back(d);
    return true;
}

/// T-386 (05 §5.9.11): `hit-slop: <length>{1,4}` (px / % / em, >= 0; margin order) -> one
/// Color-encoded declaration (top, right, bottom, left) per xt::kHitSlopEmBias.
bool parseHitSlop(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls) {
    if (toks.empty() || toks.size() > 4) {
        return false;
    }
    float v[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < toks.size(); ++i) {
        Length len{};
        if (!parseLengthTok(toks[i], len) || !(len.value >= 0.0f) || !std::isfinite(len.value)) {
            return false;
        }
        if (len.unit == LengthUnit::Px) {
            v[i] = len.value;
        } else if (len.unit == LengthUnit::Percent) {
            v[i] = -len.value / 100.0f;
        } else if (len.unit == LengthUnit::Em) {
            v[i] = -(xt::kHitSlopEmBias + len.value);
        } else {
            return false;
        }
    }
    const size_t n = toks.size();
    const float top = v[0];
    const float right = n >= 2 ? v[1] : top;
    const float bottom = n >= 3 ? v[2] : top;
    const float left = n == 4 ? v[3] : right;
    StyleDecl d;
    d.prop = styleprop::HitSlop;
    d.value.kind = StyleValueKind::Color;
    d.value.color = Color{top, right, bottom, left};
    d.line = line;
    decls.push_back(d);
    return true;
}

/// The `border-image` shorthand: every longhand is emitted (unspecified parts take their initial
/// values, as a CSS shorthand resets them).
bool parseBorderImageShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls) {
    if (toks.empty()) {
        return false;
    }
    size_t i = 0;
    std::string source;
    if (toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        source = "none";
        i = 1;
    } else if (toks[0].kind == TokenKind::Ident && toks[0].text == "url" && toks.size() >= 3 &&
               toks[1].kind == TokenKind::LParen) {
        size_t close = 2;
        while (close < toks.size() && toks[close].kind != TokenKind::RParen) {
            ++close;
        }
        if (close >= toks.size()) {
            return false;
        }
        std::vector<Token> url(toks.begin(), toks.begin() + static_cast<std::ptrdiff_t>(close) + 1);
        if (!parseBackgroundImage(url, source)) {
            return false;
        }
        i = close + 1;
    } else {
        return false;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    float v[4] = {nan, nan, nan, nan};
    bool any = false;
    if (!parseSliceValues(toks, i, v, any)) {
        return false;
    }
    bool fill = false;
    bool sawMode = false;
    uint32_t mode = 0;
    while (i < toks.size()) {
        const Token& t = toks[i];
        if (t.kind != TokenKind::Ident) {
            return false;
        }
        if (t.text == "fill" && !fill) {
            fill = true;
        } else if (!sawMode && (t.text == "stretch" || t.text == "repeat" || t.text == "round")) {
            sawMode = true;
            mode = t.text == "stretch" ? 0 : (t.text == "repeat" ? 1 : 2);
        } else {
            return false;
        }
        ++i;
    }
    StyleDecl src;
    src.prop = styleprop::BorderImageSource;
    src.value.kind = StyleValueKind::String;
    src.value.str = source;
    src.line = line;
    decls.push_back(std::move(src));
    decls.push_back(sliceDecl(v, line));
    decls.push_back(enumDecl(styleprop::BorderImageRepeat, mode, line));
    decls.push_back(enumDecl(styleprop::BorderImageFill, fill ? 1 : 0, line));
    return true;
}

bool parseBackgroundImage(const std::vector<Token>& toks, std::string& out) {
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        out = "none";
        return true;
    }
    if (toks.size() >= 3 && toks[0].kind == TokenKind::Ident && toks[0].text == "url" &&
        toks[1].kind == TokenKind::LParen && toks.back().kind == TokenKind::RParen) {
        std::vector<Token> inner(toks.begin() + 2, toks.end() - 1);
        if (inner.size() == 1 && inner[0].kind == TokenKind::String) {
            out = inner[0].text;
            return true;
        }
        // Unquoted url(...): reconstruct simple dotted filenames (Ident (Dot Ident)*). '/' isn't
        // a lexer symbol, so an unquoted path containing '/' isn't supported -- quote it instead.
        std::string path;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (i % 2 == 0) {
                if (inner[i].kind != TokenKind::Ident) {
                    return false;
                }
                path += inner[i].text;
            } else {
                if (inner[i].kind != TokenKind::Dot) {
                    return false;
                }
                path += '.';
            }
        }
        if (path.empty()) {
            return false;
        }
        out = path;
        return true;
    }
    return false;
}

// -------------------------------------------------------------------------------------------
// transition / animation lists.
// -------------------------------------------------------------------------------------------

// Splits `toks` on top-level commas only (paren depth 0); a comma inside a nested call does not
// split. Needed by `transition`/`animation` (whose per-item easing can be
// `cubic-bezier(a,b,c,d)`/`steps(n, end)` -- T-119: a depth-blind split cut those calls apart and
// every item after the first failed to parse) and by `gradient-mask()`'s comma-separated groups.
std::vector<std::vector<Token>> splitByCommaTopLevel(const std::vector<Token>& toks) {
    std::vector<std::vector<Token>> items;
    std::vector<Token> cur;
    int depth = 0;
    for (const Token& t : toks) {
        if (t.kind == TokenKind::LParen) {
            ++depth;
        } else if (t.kind == TokenKind::RParen) {
            --depth;
        }
        if (t.kind == TokenKind::Comma && depth == 0) {
            items.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(t);
        }
    }
    items.push_back(std::move(cur));
    return items;
}

bool parseTimeTok(const Token& tok, float& seconds) {
    if (tok.kind != TokenKind::Number) {
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

// The multi-output shorthands of docs/design/05-style-and-css.md §5.5: each names several
// animatable PropIds rather than one, so `transition: transform 300ms` expands into one
// TransitionSpec per component instead of being rejected as a shorthand (T-79 item 1). CSS
// interpolates a `transform` list as a whole; this engine keeps the components as independent
// per-node floats, so transitioning each one separately is the closest equivalent.
constexpr xt::PropId kTransformComponents[] = {
    xt::prop::TranslateX, xt::prop::TranslateY, xt::prop::TranslateZ, xt::prop::RotationX,
    xt::prop::RotationY,  xt::prop::Rotation,   xt::prop::SkewX,      xt::prop::SkewY,
    xt::prop::ScaleX,     xt::prop::ScaleY};
constexpr xt::PropId kTransformOriginComponents[] = {xt::prop::AnchorX, xt::prop::AnchorY};
// T-329 (05 §5.9.8): the independent transform properties name a subset of the same PropIds.
constexpr xt::PropId kTranslateComponents[] = {xt::prop::TranslateX, xt::prop::TranslateY,
                                               xt::prop::TranslateZ};
constexpr xt::PropId kScaleComponents[] = {xt::prop::ScaleX, xt::prop::ScaleY};
// T-218: `transition: path-trim ...` covers the three trim longhands.
constexpr xt::PropId kPathTrimComponents[] = {xt::prop::TrimStart, xt::prop::TrimEnd, xt::prop::TrimOffset};
constexpr xt::PropId kPerspectiveOriginComponents[] = {xt::prop::PerspectiveOriginX,
                                                       xt::prop::PerspectiveOriginY};
// T-327 (05 §5.9.8): `background-size` / `background-position` are two PropIds each.
constexpr xt::PropId kBackgroundSizeComponents[] = {xt::prop::BackgroundSizeX, xt::prop::BackgroundSizeY};
constexpr xt::PropId kBackgroundPositionComponents[] = {xt::prop::BackgroundPositionX,
                                                        xt::prop::BackgroundPositionY};

/// Appends one TransitionSpec per property `item` names (several for a multi-output shorthand,
/// one otherwise) to `out`. Returns false (after warning) when the item is malformed.
bool parseTransitionItem(const std::vector<Token>& item, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<TransitionSpec>& out,
                         std::vector<EasingPoint>* easingPoints) {
    Cursor cur(item);
    const Token& nameTok = cur.peek();
    if (nameTok.kind != TokenKind::Ident) {
        sink.warn(std::string(fileName), "invalid transition item", line);
        return false;
    }
    xt::PropId single = 0xFFFF;
    const xt::PropId* props = &single;
    size_t propCount = 1;
    if (nameTok.text == "all") {
        single = 0xFFFF;
    } else if (nameTok.text == "transform") {
        props = kTransformComponents;
        propCount = std::size(kTransformComponents);
    } else if (nameTok.text == "translate") {
        props = kTranslateComponents;
        propCount = std::size(kTranslateComponents);
    } else if (nameTok.text == "rotate") {
        single = xt::prop::Rotation;
    } else if (nameTok.text == "scale") {
        props = kScaleComponents;
        propCount = std::size(kScaleComponents);
    } else if (nameTok.text == "transform-origin") {
        props = kTransformOriginComponents;
        propCount = std::size(kTransformOriginComponents);
    } else if (nameTok.text == "perspective-origin") {
        props = kPerspectiveOriginComponents;
        propCount = std::size(kPerspectiveOriginComponents);
    } else if (nameTok.text == "background-size") {
        props = kBackgroundSizeComponents;
        propCount = std::size(kBackgroundSizeComponents);
    } else if (nameTok.text == "background-position") {
        props = kBackgroundPositionComponents;
        propCount = std::size(kBackgroundPositionComponents);
    } else if (nameTok.text == "path-trim") {
        props = kPathTrimComponents;  // T-218: scene-target rules transition <svg> content.
        propCount = std::size(kPathTrimComponents);
    } else {
        // T-218: vector properties transition on the <svg> nodes a scene-target rule writes;
        // `d` never does (it jumps).
        const PropInfo* info = findProp(nameTok.text);
        if (info == nullptr || info->kind == StyleValueKind::Shorthand || !info->animatable ||
            info->prop == xt::prop::PathData || info->prop == xt::prop::DashArray) {
            sink.warn(std::string(fileName),
                      "unknown or non-animatable transition property \"" + nameTok.text + "\"", line);
            return false;
        }
        single = info->prop;
    }
    cur.advance();
    float duration = 0;
    if (!parseTimeTok(cur.peek(), duration)) {
        sink.warn(std::string(fileName), "invalid value for \"transition\"", line);
        return false;
    }
    cur.advance();
    TransitionSpec spec;
    spec.duration = duration;
    spec.easing = Easing{};
    spec.delay = 0;
    bool haveEasing = false, haveDelay = false;
    while (!cur.atEnd()) {
        if (!haveEasing && isEasingStart(cur)) {
            Easing e;
            bool stepsClamped = false;
            if (!parseEasingCursor(cur, e, &stepsClamped, easingPoints)) {
                sink.warn(std::string(fileName), "invalid value for \"transition\"", line);
                return false;
            }
            if (stepsClamped) {
                sink.warn(std::string(fileName), kStepsClampedMessage, line);
            }
            spec.easing = e;
            haveEasing = true;
            continue;
        }
        float delay;
        if (!haveDelay && parseTimeTok(cur.peek(), delay)) {
            spec.delay = delay;
            haveDelay = true;
            cur.advance();
            continue;
        }
        sink.warn(std::string(fileName), "invalid value for \"transition\"", line);
        return false;
    }
    for (size_t i = 0; i < propCount; ++i) {
        spec.prop = props[i];
        out.push_back(spec);
    }
    return true;
}

bool parseTransitionList(const std::vector<Token>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<TransitionSpec>& out,
                         std::vector<EasingPoint>* easingPoints) {
    if (toks.empty()) {
        return false;
    }
    for (const std::vector<Token>& item : splitByCommaTopLevel(toks)) {
        parseTransitionItem(item, fileName, line, sink, out, easingPoints);
    }
    return true;
}

bool parseAnimationItem(const std::vector<Token>& item, std::string_view fileName, int line,
                        DiagnosticSink& sink, AnimationSpec& out, std::vector<EasingPoint>* easingPoints) {
    Cursor cur(item);
    const Token& nameTok = cur.peek();
    if (nameTok.kind != TokenKind::Ident) {
        sink.warn(std::string(fileName), "invalid animation item", line);
        return false;
    }
    out.name = nameTok.text;
    cur.advance();
    float duration = 0;
    if (!parseTimeTok(cur.peek(), duration)) {
        sink.warn(std::string(fileName), "invalid value for \"animation\"", line);
        return false;
    }
    cur.advance();
    out.duration = duration;
    out.delay = 0;
    out.easing = Easing{};
    out.iterations = 1;
    out.direction = static_cast<uint8_t>(AnimDirection::Normal);
    out.fill = static_cast<uint8_t>(AnimFillMode::None);
    out.playState = static_cast<uint8_t>(AnimPlayState::Running);
    bool haveEasing = false, haveDelay = false, haveIter = false, haveDir = false, haveFill = false,
         havePlay = false;
    while (!cur.atEnd()) {
        const Token& t = cur.peek();
        float num;
        if (!haveDelay && parseTimeTok(t, num)) {
            out.delay = num;
            haveDelay = true;
            cur.advance();
            continue;
        }
        if (!haveEasing && isEasingStart(cur)) {
            Easing e;
            bool stepsClamped = false;
            if (!parseEasingCursor(cur, e, &stepsClamped, easingPoints)) {
                sink.warn(std::string(fileName), "invalid value for \"animation\"", line);
                return false;
            }
            if (stepsClamped) {
                sink.warn(std::string(fileName), kStepsClampedMessage, line);
            }
            out.easing = e;
            haveEasing = true;
            continue;
        }
        if (!haveIter && t.kind == TokenKind::Ident && t.text == "infinite") {
            out.iterations = 0xFFFF;
            haveIter = true;
            cur.advance();
            continue;
        }
        if (!haveIter && t.kind == TokenKind::Number && t.unit.empty() && t.num >= 0) {
            // T-258b (D11): a finite count stays below the `infinite` sentinel 0xFFFF; a fractional
            // count still truncates.
            if (t.num > static_cast<float>(kMaxFiniteIterations)) {
                sink.warn(std::string(fileName),
                          "animation iteration count clamped to " + std::to_string(kMaxFiniteIterations),
                          line);
                out.iterations = kMaxFiniteIterations;
            } else {
                out.iterations = static_cast<uint16_t>(t.num);
            }
            haveIter = true;
            cur.advance();
            continue;
        }
        if (!haveDir && t.kind == TokenKind::Ident &&
            (t.text == "normal" || t.text == "reverse" || t.text == "alternate" ||
             t.text == "alternate-reverse")) {
            AnimDirection d = t.text == "normal"      ? AnimDirection::Normal
                              : t.text == "reverse"   ? AnimDirection::Reverse
                              : t.text == "alternate" ? AnimDirection::Alternate
                                                      : AnimDirection::AlternateReverse;
            out.direction = static_cast<uint8_t>(d);
            haveDir = true;
            cur.advance();
            continue;
        }
        if (!haveFill && t.kind == TokenKind::Ident &&
            (t.text == "none" || t.text == "forwards" || t.text == "backwards" || t.text == "both")) {
            AnimFillMode f = t.text == "none"        ? AnimFillMode::None
                             : t.text == "forwards"  ? AnimFillMode::Forwards
                             : t.text == "backwards" ? AnimFillMode::Backwards
                                                     : AnimFillMode::Both;
            out.fill = static_cast<uint8_t>(f);
            haveFill = true;
            cur.advance();
            continue;
        }
        if (!havePlay && t.kind == TokenKind::Ident && (t.text == "running" || t.text == "paused")) {
            out.playState =
                static_cast<uint8_t>(t.text == "paused" ? AnimPlayState::Paused : AnimPlayState::Running);
            havePlay = true;
            cur.advance();
            continue;
        }
        sink.warn(std::string(fileName), "invalid value for \"animation\"", line);
        return false;
    }
    return true;
}

bool parseAnimationList(const std::vector<Token>& toks, std::string_view fileName, int line,
                        DiagnosticSink& sink, std::vector<AnimationSpec>& out,
                        std::vector<EasingPoint>* easingPoints) {
    if (toks.empty()) {
        return false;
    }
    for (const std::vector<Token>& item : splitByCommaTopLevel(toks)) {
        AnimationSpec spec;
        if (!parseAnimationItem(item, fileName, line, sink, spec, easingPoints)) {
            continue;
        }
        if (spec.iterations == 0) {
            // T-272: a count of 0 (CSS: the animation does not play) is not emitted; IR's 0 means
            // infinite. A list left empty is `animation: none`.
            sink.warn(std::string(fileName),
                      "animation \"" + spec.name + "\": iteration count 0, the animation is not played",
                      line);
            continue;
        }
        out.push_back(spec);
    }
    return true;
}

// -------------------------------------------------------------------------------------------
// effect (docs/design/18-effects.md §18.3-18.4, docs/tasks/T-51a.md).
// -------------------------------------------------------------------------------------------

// Forward declaration: defined below in the "transform / transform-origin" section (also used
// there); `shine()`'s angle argument reuses it.
bool parseAngleTok(const Token& tok, float& deg);

constexpr size_t kMaxEffects = 16;            // 18.2: "每節點 16 個效果".
constexpr size_t kMaxGradientMaskStops = 16;  // 18.2: "每個 GradientMask 16 個停靠點".
// Compile-time-only em -> px rule for effect lengths. Unlike the other Length-typed properties
// (05.7 step 4's runtime cascade, which resolves em relative to the element's actual font-size),
// EffectInstance/IrEffect store plain floats with no unit tag, so there is no later stage left to
// do this per-element. A fixed rate matching font-size's own 16px initial value (05.4) is the most
// conservative choice available at this compile stage; see the T-51a report's uncovered case.
constexpr float kEffectEmPx = 16.0f;

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

bool parseAngleTok(const Token& tok, float& deg) {
    if (tok.kind != TokenKind::Number || tok.unit != "deg") {
        return false;
    }
    deg = tok.num;
    return true;
}

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

bool parseTransformShorthand(const std::vector<Token>& toks, std::string_view fileName, int line,
                             DiagnosticSink& sink, std::vector<StyleDecl>& out) {
    if (transformNeedsCompose(toks)) {
        return parseTransformComposed(toks, fileName, line, sink, out);
    }
    return parseTransformFixedOrder(toks, fileName, line, sink, out);
}

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

namespace {

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

// -------------------------------------------------------------------------------------------
// T-338 (06 §6.15): the grid subset.
// -------------------------------------------------------------------------------------------

/// Why a grid value was rejected, for the warning: Unsupported names a CSS grid feature outside
/// the subset (`minmax()`, `auto-fill`, named lines, ...), NotRectangle a `grid-template-areas`
/// area that is not a rectangle; Invalid is any other malformed value.
enum class GridReject : uint8_t { Invalid, Unsupported, NotRectangle };

/// One track: `<px> | <%> | <n>fr | auto` (a bare 0 is 0px).
bool parseGridTrackTok(const Token& t, GridTrack& out, GridReject& why) {
    if (t.kind == TokenKind::Ident) {
        if (t.text == "auto") {
            out = GridTrack{GridTrackKind::Auto, 0};
            return true;
        }
        if (t.text == "min-content" || t.text == "max-content" || t.text == "minmax" ||
            t.text == "fit-content" || t.text == "subgrid" || t.text == "masonry") {
            why = GridReject::Unsupported;
        }
        return false;
    }
    if (t.kind == TokenKind::LBracket) {
        why = GridReject::Unsupported;  // A named line.
        return false;
    }
    if (t.kind != TokenKind::Number || !std::isfinite(t.num) || t.num < 0.0f) {
        return false;
    }
    if (t.unit == "px" || (t.unit.empty() && t.num == 0.0f)) {
        out = GridTrack{GridTrackKind::Px, t.num};
    } else if (t.unit == "%") {
        out = GridTrack{GridTrackKind::Percent, t.num};
    } else if (t.unit == "fr") {
        out = GridTrack{GridTrackKind::Fr, t.num};
    } else {
        return false;
    }
    return true;
}

/// `none | [<track> | repeat(<n>, <track>+)]+`, `repeat()` expanded; `none` is an empty list.
bool parseGridTrackList(const std::vector<Token>& toks, std::vector<GridTrack>& out, GridReject& why) {
    why = GridReject::Invalid;
    out.clear();
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        return true;
    }
    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.kind == TokenKind::Ident && i + 1 < toks.size() && toks[i + 1].kind == TokenKind::LParen) {
            if (t.text != "repeat") {
                why = (t.text == "minmax" || t.text == "fit-content") ? GridReject::Unsupported : why;
                return false;
            }
            size_t close = i + 2;
            while (close < toks.size() && toks[close].kind != TokenKind::RParen) {
                if (toks[close].kind == TokenKind::LParen) {
                    why = GridReject::Unsupported;  // minmax() / fit-content() inside repeat().
                    return false;
                }
                ++close;
            }
            if (close >= toks.size() || close < i + 5 || toks[i + 3].kind != TokenKind::Comma) {
                return false;
            }
            const Token& count = toks[i + 2];
            if (count.kind == TokenKind::Ident && (count.text == "auto-fill" || count.text == "auto-fit")) {
                why = GridReject::Unsupported;
                return false;
            }
            if (count.kind != TokenKind::Number || !count.unit.empty() || !(count.num >= 1.0f) ||
                count.num > static_cast<float>(kMaxGridTracks) || std::trunc(count.num) != count.num) {
                return false;
            }
            std::vector<GridTrack> pattern;
            for (size_t k = i + 4; k < close; ++k) {
                GridTrack track;
                if (!parseGridTrackTok(toks[k], track, why)) {
                    return false;
                }
                pattern.push_back(track);
            }
            const size_t n = static_cast<size_t>(count.num);
            if (out.size() + n * pattern.size() > kMaxGridTracks) {
                return false;
            }
            for (size_t r = 0; r < n; ++r) {
                out.insert(out.end(), pattern.begin(), pattern.end());
            }
            i = close;
            continue;
        }
        GridTrack track;
        if (!parseGridTrackTok(t, track, why)) {
            return false;
        }
        out.push_back(track);
        if (out.size() > kMaxGridTracks) {
            return false;
        }
    }
    return !out.empty();
}

/// An area name: CSS name code points (letters, digits, `-`, `_`, anything non-ASCII).
bool isGridAreaNameChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) != 0 || c == '-' || c == '_';
}

/// `none | <string>+`: one string per row, cells separated by whitespace; a run of `.` is an
/// empty cell. Every row must have the same cell count and every name must cover a rectangle,
/// or the whole value is rejected (06 §6.15).
bool parseGridAreasValue(const std::vector<Token>& toks, StyleValue& out, GridReject& why) {
    why = GridReject::Invalid;
    out.kind = StyleValueKind::GridAreas;
    out.gridAreas.clear();
    out.gridAreaRows = 0;
    out.gridAreaCols = 0;
    if (toks.size() == 1 && toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
        return true;
    }
    if (toks.empty() || toks.size() > kMaxGridTracks) {
        return false;
    }
    std::vector<std::vector<std::string>> cells;
    for (const Token& t : toks) {
        if (t.kind != TokenKind::String) {
            return false;
        }
        std::vector<std::string> row;
        const std::string& text = t.text;
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') {
                ++i;
                continue;
            }
            if (text[i] == '.') {
                while (i < text.size() && text[i] == '.') {
                    ++i;
                }
                if (i < text.size() && isGridAreaNameChar(text[i])) {
                    return false;  // `.a` is neither a null cell nor a name.
                }
                row.emplace_back();
                continue;
            }
            const size_t start = i;
            while (i < text.size() && isGridAreaNameChar(text[i])) {
                ++i;
            }
            if (i == start) {
                return false;  // A character that is neither a name nor whitespace nor `.`.
            }
            row.push_back(text.substr(start, i - start));
        }
        if (row.empty() || row.size() > kMaxGridTracks || (!cells.empty() && row.size() != cells[0].size())) {
            return false;
        }
        cells.push_back(std::move(row));
    }
    const size_t rows = cells.size();
    const size_t cols = cells[0].size();
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            const std::string& name = cells[r][c];
            if (name.empty()) {
                continue;
            }
            const auto known = std::find_if(out.gridAreas.begin(), out.gridAreas.end(),
                                            [&name](const GridAreaDecl& a) { return a.name == name; });
            if (known != out.gridAreas.end()) {
                continue;  // Its first (top-left) cell built it; the check below covers the rest.
            }
            size_t c1 = c;
            while (c1 < cols && cells[r][c1] == name) {
                ++c1;
            }
            size_t r1 = r;
            while (r1 < rows && cells[r1][c] == name) {
                ++r1;
            }
            GridAreaDecl area;
            area.name = name;
            area.rowStart = static_cast<uint16_t>(r);
            area.rowEnd = static_cast<uint16_t>(r1);
            area.colStart = static_cast<uint16_t>(c);
            area.colEnd = static_cast<uint16_t>(c1);
            out.gridAreas.push_back(std::move(area));
        }
    }
    // Rectangle check: every cell holds the name of exactly the area that covers it.
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            const std::string& name = cells[r][c];
            for (const GridAreaDecl& a : out.gridAreas) {
                const bool inside = r >= a.rowStart && r < a.rowEnd && c >= a.colStart && c < a.colEnd;
                if (inside != (name == a.name)) {
                    why = GridReject::NotRectangle;
                    out.gridAreas.clear();
                    return false;
                }
            }
        }
    }
    out.gridAreaRows = static_cast<uint16_t>(rows);
    out.gridAreaCols = static_cast<uint16_t>(cols);
    return true;
}

/// One grid line `toks[begin, end)`: `auto | <integer> | span <integer>` into the kGridLine*
/// encoding. Negative lines and named lines are outside the subset.
bool parseGridLine(const std::vector<Token>& toks, size_t begin, size_t end, uint32_t& out, GridReject& why) {
    const auto integer = [](const Token& t, int& v) {
        if (t.kind != TokenKind::Number || !t.unit.empty() || std::trunc(t.num) != t.num ||
            !(std::fabs(t.num) <= 1.0e6f)) {
            return false;
        }
        v = static_cast<int>(t.num);
        return true;
    };
    const size_t n = end - begin;
    int v = 0;
    if (n == 1 && toks[begin].kind == TokenKind::Ident && toks[begin].text == "auto") {
        out = kGridLineAuto;
        return true;
    }
    if (n == 1 && integer(toks[begin], v)) {
        if (v < 1) {
            why = GridReject::Unsupported;  // 0 is invalid in CSS; negative lines are not done.
            return false;
        }
        out = static_cast<uint32_t>(std::min<int>(v, static_cast<int>(kMaxGridTracks) + 1));
        return true;
    }
    if (n == 2 && toks[begin].kind == TokenKind::Ident && toks[begin].text == "span" &&
        integer(toks[begin + 1], v)) {
        if (v < 1) {
            return false;
        }
        out = kGridLineSpan | static_cast<uint32_t>(std::min<int>(v, static_cast<int>(kMaxGridTracks)));
        return true;
    }
    if (n >= 1 && toks[begin].kind == TokenKind::Ident) {
        why = GridReject::Unsupported;  // A named line.
    }
    return false;
}

/// `grid-column` / `grid-row`: `<line> [/ <line>]` -> the start / end longhands (a missing end
/// is `auto`).
bool parseGridLineShorthand(const std::vector<Token>& toks, uint16_t startProp, int line,
                            std::vector<StyleDecl>& out, GridReject& why) {
    size_t slash = toks.size();
    for (size_t i = 0; i < toks.size(); ++i) {
        if (isSlashTok(toks[i])) {
            slash = i;
            break;
        }
    }
    uint32_t start = kGridLineAuto;
    uint32_t end = kGridLineAuto;
    if (slash == 0 || !parseGridLine(toks, 0, slash, start, why)) {
        return false;
    }
    if (slash < toks.size() &&
        (slash + 1 >= toks.size() || !parseGridLine(toks, slash + 1, toks.size(), end, why))) {
        return false;
    }
    out.push_back(enumDecl(startProp, start, line));
    out.push_back(enumDecl(static_cast<uint16_t>(startProp + 1), end, line));
    return true;
}

// -------------------------------------------------------------------------------------------
// Single (non-shorthand) value dispatch.
// -------------------------------------------------------------------------------------------

bool parseSingleValue(const PropInfo& info, const std::vector<Token>& toks, StyleValue& out) {
    switch (info.kind) {
        case StyleValueKind::Float: {
            if (info.prop == xt::prop::AspectRatio) {  // T-328 (06 §6.16).
                float ratio = 0.0f;
                if (!parseAspectRatio(toks, ratio)) {
                    return false;
                }
                out.kind = StyleValueKind::Float;
                out.f = ratio;
                return true;
            }
            if (toks.size() != 1) {
                return false;
            }
            float f;
            if (!parseFloatTok(toks[0], f) &&
                !(info.prop == styleprop::ClipStart && parseTimeTok(toks[0], f))) {  // T-191.
                return false;
            }
            if (info.prop == styleprop::ClipRate && !(f > 0.0f)) {
                return false;  // T-191: a clip rate must be positive.
            }
            if (info.prop == xt::prop::Order &&
                (!toks[0].unit.empty() || !(std::fabs(f) <= 2147483520.0f) || std::trunc(f) != f)) {
                return false;  // T-336 (06 §6.9): `order` takes an <integer>.
            }
            out.kind = StyleValueKind::Float;
            out.f = f;
            return true;
        }
        case StyleValueKind::Length: {
            if (isCalcFunctionValue(toks)) {
                // T-176: `calc(<expr>)`.
                std::string error;
                if (!parseCalcValue(toks, out.calc, error)) {
                    return false;
                }
                out.kind = StyleValueKind::Calc;
                return true;
            }
            if (toks.size() != 1) {
                return false;
            }
            if (info.prop == xt::prop::LineHeight) {
                const Token& t = toks[0];
                if (t.kind != TokenKind::Number) {
                    return false;
                }
                if (t.unit.empty()) {
                    out.kind = StyleValueKind::Length;
                    out.len = Length{t.num, LengthUnit::Em};
                    return true;
                }
            }
            // 05.5/19.10 (T-47a): `perspective: none` encodes as LengthUnit::Auto ("none" isn't
            // `parseLengthTok`'s "auto" spelling, so it needs its own check here).
            if (info.prop == xt::prop::Perspective && toks[0].kind == TokenKind::Ident &&
                toks[0].text == "none") {
                out.kind = StyleValueKind::Length;
                out.len = Length{0, LengthUnit::Auto};
                return true;
            }
            Length len;
            if (!parseLengthTok(toks[0], len)) {
                return false;
            }
            if (info.prop == xt::prop::TextDecorationThickness && !isDecorationThickness(len)) {
                return false;  // T-324: `auto` or a non-negative px / em length.
            }
            out.kind = StyleValueKind::Length;
            out.len = len;
            return true;
        }
        case StyleValueKind::Color: {
            Color c;
            if (!parseColorTokens(toks, c)) {
                return false;
            }
            out.kind = StyleValueKind::Color;
            out.color = c;
            return true;
        }
        case StyleValueKind::Enum: {
            if (info.prop == styleprop::TextDecorationLine) {  // T-324: a keyword set.
                uint32_t lines = 0;
                if (!parseTextDecorationLine(toks, lines)) {
                    return false;
                }
                out.kind = StyleValueKind::Enum;
                out.e = lines;
                return true;
            }
            if (info.prop >= styleprop::GridColumnStart && info.prop <= styleprop::GridRowEnd) {
                GridReject why = GridReject::Invalid;  // T-338 (06 §6.15).
                uint32_t v = 0;
                if (!parseGridLine(toks, 0, toks.size(), v, why)) {
                    return false;
                }
                out.kind = StyleValueKind::Enum;
                out.e = v;
                return true;
            }
            if (toks.size() != 1) {
                return false;
            }
            if (info.prop == styleprop::LineClamp && toks[0].kind == TokenKind::Number) {
                // T-331 (05 §5.9.8): an integer >= 1 (`none` is the keyword below).
                const int n = xt::toInt32Sat(toks[0].num);
                if (!toks[0].unit.empty() || n < 1 || static_cast<float>(n) != toks[0].num) {
                    return false;
                }
                out.kind = StyleValueKind::Enum;
                out.e = static_cast<uint32_t>(n);
                return true;
            }
            if (info.prop == styleprop::FontWeight) {
                if (toks[0].kind == TokenKind::Number && toks[0].unit.empty()) {
                    int w = xt::toInt32Sat(toks[0].num);  // T-258b (D11): no UB past int.
                    if (w < 100 || w > 900 || w % 100 != 0) {
                        return false;
                    }
                    out.kind = StyleValueKind::Enum;
                    out.e = static_cast<uint32_t>(w);
                    return true;
                }
            }
            if (toks[0].kind != TokenKind::Ident) {
                return false;
            }
            uint32_t e;
            if (!enumValue(info.prop, toks[0].text, e)) {
                return false;
            }
            out.kind = StyleValueKind::Enum;
            out.e = e;
            return true;
        }
        case StyleValueKind::String: {
            if (info.prop == styleprop::GridArea) {
                // T-338 (06 §6.15): only `<name>` or `auto` (stored as "").
                if (toks.size() != 1 || toks[0].kind != TokenKind::Ident) {
                    return false;
                }
                out.kind = StyleValueKind::String;
                out.str = toks[0].text == "auto" ? std::string() : toks[0].text;
                return true;
            }
            if (info.prop == styleprop::BackgroundImage) {
                std::string s;
                if (!parseBackgroundImage(toks, s)) {
                    return false;
                }
                out.kind = StyleValueKind::String;
                out.str = std::move(s);
                return true;
            }
            if (toks.size() == 1 && toks[0].kind == TokenKind::String) {
                out.kind = StyleValueKind::String;
                out.str = toks[0].text;
                return true;
            }
            if (toks.size() == 1 && toks[0].kind == TokenKind::Ident) {
                out.kind = StyleValueKind::String;
                out.str = toks[0].text;
                return true;
            }
            return false;
        }
        case StyleValueKind::FontFamilyList: {
            std::vector<std::string> families;
            if (!parseFontFamilyList(toks, families)) {
                return false;
            }
            out.kind = StyleValueKind::FontFamilyList;
            out.families = std::move(families);
            return true;
        }
        case StyleValueKind::GridTracks: {  // T-338 (06 §6.15).
            GridReject why = GridReject::Invalid;
            out.kind = StyleValueKind::GridTracks;
            return parseGridTrackList(toks, out.gridTracks, why);
        }
        case StyleValueKind::GridAreas: {
            GridReject why = GridReject::Invalid;
            return parseGridAreasValue(toks, out, why);
        }
        case StyleValueKind::FloatArray:
        case StyleValueKind::Vec2: {
            // T-236: `stroke-dasharray: none | <n> [,] <n> ...` and the `<x> <y>` modifier
            // parameters; each number is px or plain.
            std::vector<float> values;
            if (info.kind == StyleValueKind::FloatArray && toks.size() == 1 &&
                toks[0].kind == TokenKind::Ident && toks[0].text == "none") {
                out.kind = StyleValueKind::FloatArray;
                out.floats.clear();
                return true;
            }
            for (const Token& t : toks) {
                if (t.kind == TokenKind::Comma && info.kind == StyleValueKind::FloatArray) {
                    continue;
                }
                if (t.kind != TokenKind::Number || !(t.unit.empty() || t.unit == "px")) {
                    return false;
                }
                values.push_back(t.num);
            }
            if (info.kind == StyleValueKind::Vec2) {
                if (values.size() != 2) {
                    return false;
                }
            } else {
                bool positive = false;
                for (float v : values) {
                    if (v < 0.0f) {
                        return false;
                    }
                    positive = positive || v > 0.0f;
                }
                if (values.empty()) {
                    return false;
                }
                if (!positive) {
                    values.clear();  // All zero: no dashes (as parseSvg's `stroke-dasharray`).
                }
            }
            out.kind = info.kind;
            out.floats = std::move(values);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

bool parseEasingValue(const std::vector<Token>& tokens, Easing& out, bool* stepsClamped,
                      std::vector<EasingPoint>* easingPoints) {
    Cursor cur(tokens);
    if (!parseEasingCursor(cur, out, stepsClamped, easingPoints)) {
        return false;
    }
    return cur.atEnd();
}

bool parseAndExpandDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName,
                        int line, DiagnosticSink& sink, std::vector<StyleDecl>& outDecls,
                        std::vector<EasingPoint>* easingPoints) {
    auto invalid = [&](const std::string& extra) {
        sink.warn(std::string(fileName), "invalid value for \"" + std::string(info->cssName) + "\"" + extra,
                  line);
        return false;
    };
    if (info->prop == xt::prop::BorderColor) {
        // T-333 (05 §5.9.8): 2-4 colors set the sides; one color stays the legacy BorderColor.
        std::vector<StyleDecl> decls;
        if (parseBorderColorSides(tokens, line, decls)) {
            outDecls = std::move(decls);
            return true;
        }
    }
    if (info->prop == styleprop::Overflow && tokens.size() == 2) {
        // T-336 (05 §5.9.8): `overflow: <x> <y>` sets the two axes; one value stays StyleProp 168.
        uint32_t x = 0;
        uint32_t y = 0;
        if (tokens[0].kind != TokenKind::Ident || tokens[1].kind != TokenKind::Ident ||
            !enumValue(styleprop::OverflowX, tokens[0].text, x) ||
            !enumValue(styleprop::OverflowY, tokens[1].text, y)) {
            return invalid("");
        }
        outDecls.push_back(enumDecl(styleprop::OverflowX, x, line));
        outDecls.push_back(enumDecl(styleprop::OverflowY, y, line));
        return true;
    }
    if (info->kind == StyleValueKind::Shorthand) {
        std::string_view name = info->cssName;
        std::vector<StyleDecl> decls;
        bool ok = false;
        if (name == "margin") {
            const PropId order[4] = {xt::prop::MarginTop, xt::prop::MarginRight, xt::prop::MarginBottom,
                                     xt::prop::MarginLeft};
            ok = expand1to4(order, tokens, line, decls);
        } else if (name == "padding") {
            const PropId order[4] = {xt::prop::PaddingTop, xt::prop::PaddingRight, xt::prop::PaddingBottom,
                                     xt::prop::PaddingLeft};
            ok = expand1to4(order, tokens, line, decls);
        } else if (name == "border-width") {
            const PropId order[4] = {xt::prop::BorderTopWidth, xt::prop::BorderRightWidth,
                                     xt::prop::BorderBottomWidth, xt::prop::BorderLeftWidth};
            ok = expand1to4(order, tokens, line, decls);
        } else if (name == "border-radius") {
            const PropId order[4] = {xt::prop::BorderRadiusTL, xt::prop::BorderRadiusTR,
                                     xt::prop::BorderRadiusBR, xt::prop::BorderRadiusBL};
            ok = expand1to4(order, tokens, line, decls);
        } else if (name == "inset") {
            const PropId order[4] = {xt::prop::Top, xt::prop::Right, xt::prop::Bottom, xt::prop::Left};
            ok = expand1to4(order, tokens, line, decls);
        } else if (name == "border") {
            ok = parseBorderShorthand(tokens, line, decls);
        } else if (name == "border-style") {
            ok = parseBorderStyleShorthand(tokens, line, decls);  // T-333 (05 §5.9.8).
        } else if (name == "outline") {
            ok = parseOutlineShorthand(tokens, line, decls);  // T-325 (05 §5.9.8).
        } else if (name == "text-decoration") {
            ok = parseTextDecorationShorthand(tokens, line, decls);  // T-324 (05 §5.9.8).
        } else if (name == "flex") {
            ok = parseFlexShorthand(tokens, line, decls);
        } else if (name == "gap") {
            ok = parseGapShorthand(tokens, line, decls);
        } else if (name == "grid-column" || name == "grid-row") {
            // T-338 (06 §6.15): `<start> [/ <end>]`.
            GridReject why = GridReject::Invalid;
            ok = parseGridLineShorthand(
                tokens, name == "grid-column" ? styleprop::GridColumnStart : styleprop::GridRowStart, line,
                decls, why);
            if (!ok && why == GridReject::Unsupported) {
                return invalid(" (named and negative grid lines are not supported)");
            }
        } else if (name == "transform") {
            ok = parseTransformShorthand(tokens, fileName, line, sink, decls);
            if (!ok) {
                return false;  // Diagnostic already emitted (warning or the 05.5 ordering error).
            }
        } else if (name == "translate" || name == "rotate" || name == "scale") {
            // T-329 (05 §5.9.8): alone they write the existing PropIds straight through.
            ok = parseIndependentTransform(name, tokens, fileName, line, sink, decls);
            if (!ok) {
                return false;  // Diagnostic already emitted.
            }
        } else if (name == "transform-origin") {
            ok = parseTransformOrigin(tokens, decls, line);
        } else if (name == "perspective-origin") {
            ok = parsePerspectiveOrigin(tokens, decls, line);
        } else if (name == "background-size") {
            ok = parseBackgroundSize(tokens, decls, line);  // T-327 (05 §5.9.8).
        } else if (name == "background-position") {
            ok = parseBackgroundPosition(tokens, decls, line);
        } else if (name == "border-image") {
            ok = parseBorderImageShorthand(tokens, line, decls);  // T-199 (26.3.4).
        } else if (name == "border-image-slice") {
            ok = parseBorderImageSlice(tokens, line, decls);
        } else if (name == "ui-scale") {
            ok = parseUiScale(tokens, line, decls);  // T-345 (05 §5.9.9).
        } else if (name == "hit-slop") {
            ok = parseHitSlop(tokens, line, decls);  // T-386 (05 §5.9.11).
        } else if (name == "particle-lifetime") {
            ok = parseParticlePairShorthand(tokens, xt::prop::ParticleLifetimeMin,
                                            xt::prop::ParticleLifetimeMax, line, decls);
        } else if (name == "particle-speed") {
            ok = parseParticlePairShorthand(tokens, xt::prop::ParticleSpeedMin, xt::prop::ParticleSpeedMax,
                                            line, decls);
        } else if (name == "particle-size") {
            ok = parseParticlePairShorthand(tokens, xt::prop::ParticleSizeStart, xt::prop::ParticleSizeEnd,
                                            line, decls);
        } else if (name == "particle-rotation") {
            ok = parseParticlePairShorthand(tokens, xt::prop::ParticleRotationMin,
                                            xt::prop::ParticleRotationMax, line, decls);
        } else if (name == "particle-color") {
            ok = parseParticleColorShorthand(tokens, line, decls);
        } else if (name == "path-trim") {
            // T-190 (03 §3.12.6): `path-trim: <start> <end> [<offset>]`, fractions of the path
            // length (offset in turns, default 0) -> TrimStart/TrimEnd/TrimOffset.
            float v[3] = {0, 1, 0};
            ok = tokens.size() == 2 || tokens.size() == 3;
            for (size_t i = 0; ok && i < tokens.size(); ++i) {
                ok = parseFloatTok(tokens[i], v[i]);
            }
            if (ok) {
                const PropId props[3] = {xt::prop::TrimStart, xt::prop::TrimEnd, xt::prop::TrimOffset};
                for (int i = 0; i < 3; ++i) {
                    StyleDecl d;
                    d.prop = props[i];
                    d.value.kind = StyleValueKind::Float;
                    d.value.f = v[i];
                    d.line = line;
                    decls.push_back(d);
                }
            }
        }
        if (!ok) {
            return invalid("");
        }
        outDecls = std::move(decls);
        return true;
    }
    if (info->kind == StyleValueKind::TransitionList) {
        std::vector<TransitionSpec> list;
        if (!parseTransitionList(tokens, fileName, line, sink, list, easingPoints)) {
            return invalid("");
        }
        StyleDecl d;
        d.prop = info->prop;
        d.value.kind = StyleValueKind::TransitionList;
        d.value.transitions = std::move(list);
        d.line = line;
        outDecls.push_back(std::move(d));
        return true;
    }
    if (info->kind == StyleValueKind::EffectList) {
        std::vector<EffectDeclEntry> list;
        const bool textEffect = info->prop == styleprop::TextEffect;  // T-178.
        if (textEffect ? !parseTextEffectList(tokens, fileName, line, sink, list)
                       : !parseEffectList(tokens, fileName, line, sink, list)) {
            return false;  // Diagnostic already emitted by parse(Text)EffectList.
        }
        StyleDecl d;
        d.prop = info->prop;
        d.value.kind = StyleValueKind::EffectList;
        d.value.effects = std::move(list);
        d.line = line;
        outDecls.push_back(std::move(d));
        return true;
    }
    if (info->kind == StyleValueKind::Gradient) {
        GradientDecl gradient;
        if (!parseGradientValue(tokens, gradient)) {
            return invalid("");  // 05.5c: a malformed gradient is dropped with a warning.
        }
        StyleDecl d;
        d.prop = info->prop;
        d.value.kind = StyleValueKind::Gradient;
        d.value.gradient = std::move(gradient);
        d.line = line;
        outDecls.push_back(std::move(d));
        return true;
    }
    if (info->kind == StyleValueKind::GridTracks || info->kind == StyleValueKind::GridAreas ||
        (info->prop >= styleprop::GridColumnStart && info->prop <= styleprop::GridRowEnd)) {
        // T-338 (06 §6.15): say which part of CSS grid is outside the subset.
        StyleValue value;
        GridReject why = GridReject::Invalid;
        bool ok = false;
        if (info->kind == StyleValueKind::GridTracks) {
            value.kind = StyleValueKind::GridTracks;
            ok = parseGridTrackList(tokens, value.gridTracks, why);
        } else if (info->kind == StyleValueKind::GridAreas) {
            ok = parseGridAreasValue(tokens, value, why);
        } else {
            uint32_t v = 0;
            ok = parseGridLine(tokens, 0, tokens.size(), v, why);
            value.kind = StyleValueKind::Enum;
            value.e = v;
        }
        if (!ok) {
            switch (why) {
                case GridReject::Unsupported:
                    return invalid(
                        " (outside the grid subset: minmax(), fit-content(), min/max-content, "
                        "auto-fill / auto-fit, named or negative lines, subgrid and masonry are not "
                        "supported)");
                case GridReject::NotRectangle:
                    return invalid(" (every grid area must be a rectangle; the declaration is dropped)");
                default:
                    return invalid("");
            }
        }
        StyleDecl d;
        d.prop = info->prop;
        d.value = std::move(value);
        d.line = line;
        outDecls.push_back(std::move(d));
        return true;
    }
    if (info->kind == StyleValueKind::ClipShape) {
        // T-332 (05 §5.9.8): `path()` / `url()` get their own wording; any other bad value the
        // generic one. Both drop the declaration.
        ElementClipShape shape;
        bool unsupported = false;
        if (!parseClipPathValue(tokens, shape, unsupported)) {
            return invalid(unsupported ? " (path() and url() are not supported; only inset(), circle(), "
                                         "ellipse() and polygon())"
                                       : "");
        }
        StyleDecl d;
        d.prop = info->prop;
        d.value.kind = StyleValueKind::ClipShape;
        d.value.clipShape = std::move(shape);
        d.line = line;
        outDecls.push_back(std::move(d));
        return true;
    }
    if (info->kind == StyleValueKind::AnimationList) {
        std::vector<AnimationSpec> list;
        if (!parseAnimationList(tokens, fileName, line, sink, list, easingPoints)) {
            return invalid("");
        }
        StyleDecl d;
        d.prop = info->prop;
        d.value.kind = StyleValueKind::AnimationList;
        d.value.animations = std::move(list);
        d.line = line;
        outDecls.push_back(std::move(d));
        return true;
    }
    if (info->prop == xt::prop::PathData) {
        // T-190: `d: path("<svg path data>")` or a bare string; parsed into verbs/points when the
        // inline `<svg>` timeline is built (compile_document.cpp).
        const Token* str = nullptr;
        if (tokens.size() == 1 && tokens[0].kind == TokenKind::String) {
            str = &tokens[0];
        } else if (tokens.size() == 4 && tokens[0].kind == TokenKind::Ident && tokens[0].text == "path" &&
                   tokens[1].kind == TokenKind::LParen && tokens[2].kind == TokenKind::String &&
                   tokens[3].kind == TokenKind::RParen) {
            str = &tokens[2];
        }
        if (str == nullptr) {
            return invalid(": expected path(\"...\")");
        }
        StyleDecl d;
        d.prop = info->prop;
        d.value.kind = StyleValueKind::String;
        d.value.str = str->text;
        d.line = line;
        outDecls.push_back(std::move(d));
        return true;
    }
    StyleValue value;
    if (!parseSingleValue(*info, tokens, value)) {
        std::string calcError;
        std::vector<CalcOpDecl> ignored;
        if (isCalcFunctionValue(tokens)) {
            // T-176: say why a calc() was rejected (or that the property takes no calc()).
            if (info->kind != StyleValueKind::Length) {
                return invalid(": calc() is only supported on length properties");
            }
            if (!parseCalcValue(tokens, ignored, calcError)) {
                return invalid(": " + calcError);
            }
        }
        return invalid("");
    }
    StyleDecl d;
    d.prop = info->prop;
    d.value = std::move(value);
    d.line = line;
    outDecls.push_back(std::move(d));
    return true;
}

// -------------------------------------------------------------------------------------------
// T-170: design tokens (docs/design/05-style-and-css.md §5.9.2-5.9.3).
// -------------------------------------------------------------------------------------------

namespace {

bool isVarOpen(const std::vector<Token>& tokens, size_t i) {
    return i + 1 < tokens.size() && tokens[i].kind == TokenKind::Ident && tokens[i].text == "var" &&
           tokens[i + 1].kind == TokenKind::LParen;
}

/// A number as `.xts` text: an integer stays one, anything else gets up to 6 decimals, trailing
/// zeros trimmed (the lexer has no exponent syntax, so `%g` is not an option).
std::string numberText(float value) {
    char buf[64];
    if (value == static_cast<float>(static_cast<long long>(value)) && value > -1e15f && value < 1e15f) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        return buf;
    }
    std::string out(buf, xt::formatFixed(buf, sizeof(buf), static_cast<double>(value), 6));
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out;
}

/// The whole-value form `var(--name)` / `var(--name, <fallback>)`: fills `name` and `fallback`.
/// False when `tokens` is anything else (including a fallback that itself holds a `var()`).
bool splitWholeVar(const std::vector<Token>& tokens, std::string& name, std::vector<Token>& fallback) {
    if (tokens.size() < 4 || !isVarOpen(tokens, 0) || tokens[2].kind != TokenKind::Ident ||
        tokens[2].text.size() < 3 || tokens[2].text.compare(0, 2, "--") != 0) {
        return false;
    }
    int depth = 0;
    size_t close = tokens.size();
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokenKind::LParen) {
            ++depth;
        } else if (tokens[i].kind == TokenKind::RParen && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close != tokens.size() - 1) {
        return false;
    }
    name = tokens[2].text;
    fallback.clear();
    if (tokens.size() == 4) {
        return true;
    }
    if (tokens[3].kind != TokenKind::Comma || tokens.size() == 5) {
        return false;
    }
    fallback.assign(tokens.begin() + 4, tokens.end() - 1);
    return !containsVarFunction(fallback);
}

/// The 1-to-4 / 1-to-2 shorthands whose longhands all take the same kind of length, so one token
/// can feed every one of them.
bool isUniformShorthand(std::string_view cssName) {
    return cssName == "margin" || cssName == "padding" || cssName == "border-width" ||
           cssName == "border-radius" || cssName == "gap" || cssName == "inset";
}

}  // namespace

bool containsVarFunction(const std::vector<Token>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (isVarOpen(tokens, i)) {
            return true;
        }
    }
    return false;
}

std::string tokensToText(const std::vector<Token>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        const bool glueLeft =
            t.kind == TokenKind::RParen || t.kind == TokenKind::Comma ||
            (t.kind == TokenKind::LParen && i > 0 && tokens[i - 1].kind == TokenKind::Ident);
        const bool afterOpen = i > 0 && tokens[i - 1].kind == TokenKind::LParen;
        if (!out.empty() && !glueLeft && !afterOpen) {
            out += ' ';
        }
        switch (t.kind) {
            case TokenKind::Ident:
                out += t.text;
                break;
            case TokenKind::Number:
                out += numberText(t.num);
                out += t.unit;
                break;
            case TokenKind::String:
                out += '"';
                for (char c : t.text) {
                    if (c == '"' || c == '\\') {
                        out += '\\';
                    }
                    out += c;
                }
                out += '"';
                break;
            case TokenKind::Hash:
                out += '#';
                out += t.text;
                break;
            case TokenKind::LParen:
                out += '(';
                break;
            case TokenKind::RParen:
                out += ')';
                break;
            case TokenKind::Comma:
                out += ',';
                break;
            case TokenKind::Colon:
                out += ':';
                break;
            case TokenKind::Dot:
                out += '.';
                break;
            case TokenKind::Star:
                out += '*';
                break;
            case TokenKind::Gt:
                out += '>';
                break;
            case TokenKind::Bang:
                out += '!';
                break;
            case TokenKind::At:
                out += '@';
                break;
            case TokenKind::Unknown: {
                // T-176: a lone operator character (`+`, `/`, `<`, `=`) the lexer has no token for.
                const std::string_view prefix = "unexpected character '";
                if (t.text.size() == prefix.size() + 2 && t.text.compare(0, prefix.size(), prefix) == 0) {
                    out += t.text[prefix.size()];
                }
                break;
            }
            default:
                break;
        }
    }
    return out;
}

bool parseVarDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName, int line,
                  DiagnosticSink& sink, std::vector<StyleDecl>& outDecls) {
    std::string name;
    std::vector<Token> fallback;
    const bool whole = splitWholeVar(tokens, name, fallback);
    const bool plain = info->kind == StyleValueKind::Float || info->kind == StyleValueKind::Length ||
                       info->kind == StyleValueKind::Color || info->kind == StyleValueKind::Enum;
    if (whole && plain) {
        StyleDecl d;
        d.prop = info->prop;
        d.value.kind = StyleValueKind::VarRef;
        d.value.varName = name;
        d.line = line;
        if (!fallback.empty()) {
            std::vector<StyleDecl> parsed;
            if (!parseAndExpandDecl(info, fallback, fileName, line, sink, parsed)) {
                return false;  // parseAndExpandDecl already warned about the fallback.
            }
            if (parsed.size() != 1) {
                sink.warn(std::string(fileName),
                          "invalid var() fallback for "
                          " + std::string(info->cssName) + "
                          "",
                          line);
                return false;
            }
            d.value.varFallback.push_back(std::move(parsed[0].value));
        }
        outDecls.push_back(std::move(d));
        return true;
    }
    if (whole && info->kind == StyleValueKind::Shorthand && isUniformShorthand(info->cssName)) {
        // Which longhands the shorthand feeds: expand a placeholder `0px` and keep only the props.
        Token zero;
        zero.kind = TokenKind::Number;
        zero.num = 0;
        zero.unit = "px";
        DiagnosticSink scratch;
        std::vector<StyleDecl> shape;
        std::vector<StyleDecl> parsedFallback;
        if (parseAndExpandDecl(info, std::vector<Token>{zero}, fileName, line, scratch, shape) &&
            !shape.empty()) {
            if (!fallback.empty() &&
                !parseAndExpandDecl(info, fallback, fileName, line, sink, parsedFallback)) {
                return false;
            }
            for (const StyleDecl& longhand : shape) {
                StyleDecl d;
                d.prop = longhand.prop;
                d.value.kind = StyleValueKind::VarRef;
                d.value.varName = name;
                d.line = line;
                for (StyleDecl& fb : parsedFallback) {
                    if (fb.prop == longhand.prop) {
                        d.value.varFallback.push_back(fb.value);
                        break;
                    }
                }
                outDecls.push_back(std::move(d));
            }
            return true;
        }
    }
    // Everything else (a `var()` inside a larger value, on a list-valued property or on a
    // shorthand) is substituted once every token is known (style_emit.cpp resolveVarDecls).
    StyleDecl d;
    d.prop = info->prop;
    d.value.kind = StyleValueKind::VarSubst;
    d.value.str = std::string(info->cssName) + ": " + tokensToText(tokens);
    d.line = line;
    outDecls.push_back(std::move(d));
    return true;
}

bool parseValue(uint16_t prop, std::string_view text, StyleValue& out) {
    const PropInfo* info = findPropById(prop);
    if (info == nullptr) {
        return false;
    }
    Lexer lexer(text);
    std::vector<Token> toks;
    for (Token t = lexer.next(); t.kind != TokenKind::End; t = lexer.next()) {
        if (t.kind == TokenKind::Unknown) {
            return false;
        }
        toks.push_back(t);
    }
    if (info->kind == StyleValueKind::TransitionList || info->kind == StyleValueKind::AnimationList) {
        // No DiagnosticSink available here; T-35's runtime mini-parser doesn't need transition/
        // animation lists (those are compile-time-only per 05.6), so this is conservatively
        // unsupported.
        return false;
    }
    return parseSingleValue(*info, toks, out);
}

Status parseEffectList(std::string_view text, std::vector<EffectDeclEntry>& out, DiagnosticSink& sink) {
    out.clear();
    Lexer lexer(text);
    std::vector<Token> toks;
    for (Token t = lexer.next(); t.kind != TokenKind::End; t = lexer.next()) {
        if (t.kind == TokenKind::Unknown) {
            sink.error("<effect>", "invalid value for \"effect\": lexer error", 1);
            return Status::kInvalidArgument;
        }
        toks.push_back(t);
    }
    if (toks.empty()) {
        sink.error("<effect>", "invalid value for \"effect\": empty", 1);
        return Status::kInvalidArgument;
    }
    if (!parseEffectList(toks, "<effect>", 1, sink, out)) {
        return Status::kInvalidArgument;
    }
    return Status::kOk;
}

// docs/design/28-animation-editing.md §28.1.2 / 05.9.3 (T-172): an `@timeline` time point is
// seconds or milliseconds; a bare `0` is accepted as 0 s. Negative times are rejected.
bool parseTimelineTime(const Token& tok, float& seconds) {
    if (tok.kind != TokenKind::Number) {
        return false;
    }
    if (tok.unit.empty()) {
        if (tok.num != 0) {
            return false;
        }
        seconds = 0;
        return true;
    }
    if (!parseTimeTok(tok, seconds)) {
        return false;
    }
    return seconds >= 0;
}

// docs/design/18-effects.md §18.12 (T-172): the value of an `effect.<effect>.<param>` timeline
// declaration. A color parameter takes any `<color>`; every other parameter is one number, unitless
// or in the unit 18.3 gives it (px for lengths, deg for angles), stored as a plain Float.
bool parseEffectParamValue(const std::vector<Token>& tokens, bool isColor, StyleValue& out) {
    if (isColor) {
        Color c;
        if (!parseColorTokens(tokens, c)) {
            return false;
        }
        out.kind = StyleValueKind::Color;
        out.color = c;
        return true;
    }
    if (tokens.size() != 1 || tokens[0].kind != TokenKind::Number) {
        return false;
    }
    const Token& t = tokens[0];
    if (!(t.unit.empty() || t.unit == "px" || t.unit == "deg")) {
        return false;
    }
    out.kind = StyleValueKind::Float;
    out.f = t.num;
    return true;
}

}  // namespace xt::compile

// -------------------------------------------------------------------------------------------
// T-176: `calc()` (docs/design/05-style-and-css.md §5.9.2, 06-layout.md §6.4).
// -------------------------------------------------------------------------------------------

namespace xt::compile {

namespace {

/// The operator character a token stands for inside `calc()`, or 0. `*` has its own token; a
/// spaced `-` lexes as the identifier "-"; `+` and `/` have no token and arrive as Unknown
/// "unexpected character" tokens.
char calcOperatorChar(const Token& t) {
    if (t.kind == TokenKind::Star) {
        return '*';
    }
    if (t.kind == TokenKind::Ident && t.text == "-") {
        return '-';
    }
    if (t.kind == TokenKind::Unknown) {
        const std::string_view prefix = "unexpected character '";
        if (t.text.size() == prefix.size() + 2 && t.text.compare(0, prefix.size(), prefix) == 0) {
            const char c = t.text[prefix.size()];
            if (c == '+' || c == '/') {
                return c;
            }
        }
    }
    return 0;
}

int calcPrecedence(char op) { return (op == '*' || op == '/') ? 2 : 1; }

CalcOpKind calcOpKind(char op) {
    switch (op) {
        case 'm':
            return CalcOpKind::Min;
        case 'M':
            return CalcOpKind::Max;
        case '+':
            return CalcOpKind::Add;
        case '-':
            return CalcOpKind::Sub;
        case '*':
            return CalcOpKind::Mul;
        default:
            return CalcOpKind::Div;
    }
}

/// A calc() operand token: a number (unitless) or a length. `%`, px, em and the T-176 units.
bool parseCalcOperand(const Token& t, CalcOpDecl& out) {
    if (t.kind != TokenKind::Number) {
        return false;
    }
    if (t.unit.empty()) {
        out.op = CalcOpKind::PushNumber;
        out.value = t.num;
        return true;
    }
    Length len;
    if (!parseLengthTok(t, len) || len.unit == LengthUnit::Auto) {
        return false;
    }
    out.op = CalcOpKind::PushLength;
    out.unit = len.unit;
    out.value = len.value;
    return true;
}

/// T-330: what an identifier opens in operand position: '(' for `calc(` (a plain group), 'm' for
/// `min(`, 'M' for `max(`, 'c' for `clamp(`; 0 for anything else.
char calcFunctionMarker(const Token& t) {
    if (t.kind != TokenKind::Ident) {
        return 0;
    }
    if (t.text == "calc") {
        return '(';
    }
    if (t.text == "min") {
        return 'm';
    }
    if (t.text == "max") {
        return 'M';
    }
    return t.text == "clamp" ? 'c' : 0;
}

}  // namespace

bool isCalcFunctionValue(const std::vector<Token>& tokens) {
    if (tokens.size() < 3 || calcFunctionMarker(tokens[0]) == 0 || tokens[1].kind != TokenKind::LParen ||
        tokens.back().kind != TokenKind::RParen) {
        return false;
    }
    if (tokens[0].text == "calc") {
        return true;
    }
    // T-330: `min(...)`, `max(...)`, `clamp(...)` -- one function call whose `)` is the last token.
    int depth = 0;
    for (size_t i = 1; i < tokens.size(); ++i) {
        depth += tokens[i].kind == TokenKind::LParen ? 1 : (tokens[i].kind == TokenKind::RParen ? -1 : 0);
        if (depth == 0 && i + 1 < tokens.size()) {
            return false;
        }
    }
    return depth == 0;
}

bool parseCalcValue(const std::vector<Token>& tokens, std::vector<CalcOpDecl>& out, std::string& error) {
    out.clear();
    if (!isCalcFunctionValue(tokens)) {
        error = "expected calc(...), min(...), max(...) or clamp(...)";
        return false;
    }
    // Shunting-yard over the tokens between `calc(` and the final `)` (T-330: over the whole
    // `min(...)` / `max(...)` / `clamp(...)`); a nested `(` or `calc(` groups. Operators: + - * /,
    // left-associative, * / binding tighter. T-330: `min(` / `max(` / `clamp(` open a function
    // whose comma-separated arguments are full expressions; min/max of n arguments emit n - 1
    // Min/Max ops, clamp(a, b, c) emits `a b c Min Max` = max(a, min(b, c)).
    std::vector<char> ops;       // '(' marks a group, 'm' / 'M' / 'c' a function.
    std::vector<int> argCounts;  // Arguments so far, one entry per open function.
    bool expectOperand = true;
    const bool calcWrapped = tokens[0].text == "calc";
    const size_t begin = calcWrapped ? 2 : 0;
    const size_t end = calcWrapped ? tokens.size() - 1 : tokens.size();
    constexpr size_t kMaxOps = 64;
    auto isMarker = [](char c) { return c == '(' || c == 'm' || c == 'M' || c == 'c'; };
    auto popOperators = [&] {
        while (!ops.empty() && !isMarker(ops.back())) {
            CalcOpDecl op;
            op.op = calcOpKind(ops.back());
            out.push_back(op);
            ops.pop_back();
        }
    };
    for (size_t i = begin; i < end; ++i) {
        const Token& t = tokens[i];
        if (expectOperand) {
            const char marker = calcFunctionMarker(t);
            if (t.kind == TokenKind::LParen ||
                (marker != 0 && i + 1 < end && tokens[i + 1].kind == TokenKind::LParen)) {
                if (t.kind == TokenKind::Ident) {
                    ++i;  // `calc(` nested: same as a plain group; `min(` etc.: a function.
                }
                const char open = t.kind == TokenKind::LParen ? '(' : marker;
                ops.push_back(open);
                if (open != '(') {
                    argCounts.push_back(1);
                }
                continue;
            }
            if (t.kind == TokenKind::Ident && t.text == "var" && i + 1 < end &&
                tokens[i + 1].kind == TokenKind::LParen) {
                // var(--name) or var(--name, <number | length>).
                CalcOpDecl v;
                v.op = CalcOpKind::PushVar;
                if (i + 3 >= end || tokens[i + 2].kind != TokenKind::Ident || tokens[i + 2].text.size() < 3 ||
                    tokens[i + 2].text.compare(0, 2, "--") != 0) {
                    error = "expected var(--name) in calc()";
                    return false;
                }
                v.varName = tokens[i + 2].text;
                size_t j = i + 3;
                if (tokens[j].kind == TokenKind::Comma) {
                    CalcOpDecl fb;
                    if (j + 2 >= tokens.size() || !parseCalcOperand(tokens[j + 1], fb) ||
                        tokens[j + 2].kind != TokenKind::RParen) {
                        error = "a var() fallback inside calc() must be one number or length";
                        return false;
                    }
                    v.hasFallback = true;
                    v.fallbackIsNumber = fb.op == CalcOpKind::PushNumber;
                    v.unit = fb.unit;
                    v.value = fb.value;
                    j += 2;
                }
                if (j >= end || tokens[j].kind != TokenKind::RParen) {
                    error = "expected ')' after var() in calc()";
                    return false;
                }
                out.push_back(v);
                i = j;
                expectOperand = false;
                continue;
            }
            CalcOpDecl operand;
            if (!parseCalcOperand(t, operand)) {
                error = "expected a number or a length in calc()";
                return false;
            }
            out.push_back(operand);
            expectOperand = false;
            continue;
        }
        if (t.kind == TokenKind::Comma) {
            // T-330: the next argument of the innermost min() / max() / clamp().
            popOperators();
            if (ops.empty() || ops.back() == '(') {
                error = "',' outside min(), max() or clamp() in calc()";
                return false;
            }
            ++argCounts.back();
            expectOperand = true;
            continue;
        }
        if (t.kind == TokenKind::RParen) {
            popOperators();
            if (ops.empty()) {
                error = "unbalanced ')' in calc()";
                return false;
            }
            const char open = ops.back();
            ops.pop_back();
            if (open == '(') {
                continue;
            }
            const int args = argCounts.back();
            argCounts.pop_back();
            CalcOpDecl op;
            if (open == 'c') {
                if (args != 3) {
                    error = "clamp() takes exactly three arguments";
                    return false;
                }
                op.op = CalcOpKind::Min;
                out.push_back(op);
                op.op = CalcOpKind::Max;
                out.push_back(op);
                continue;
            }
            op.op = calcOpKind(open);
            for (int a = 1; a < args; ++a) {
                out.push_back(op);
            }
            continue;
        }
        const char c = calcOperatorChar(t);
        if (c == 0) {
            error = "expected an operator (+ - * /, with spaces around + and -) in calc()";
            return false;
        }
        while (!ops.empty() && !isMarker(ops.back()) && calcPrecedence(ops.back()) >= calcPrecedence(c)) {
            CalcOpDecl op;
            op.op = calcOpKind(ops.back());
            out.push_back(op);
            ops.pop_back();
        }
        ops.push_back(c);
        expectOperand = true;
    }
    if (expectOperand) {
        error = "calc() ends with an operator or is empty";
        return false;
    }
    while (!ops.empty()) {
        if (isMarker(ops.back())) {
            error = "unbalanced '(' in calc()";
            return false;
        }
        CalcOpDecl op;
        op.op = calcOpKind(ops.back());
        out.push_back(op);
        ops.pop_back();
    }
    if (out.size() > kMaxOps) {
        error = "calc() is too long";
        return false;
    }
    // Type check: 0 = number, 1 = length, 2 = unknown (a var()). + and - need matching types, *
    // at most one length, / a number divisor (a literal 0 is an error); the result is a length.
    std::vector<int> stack;
    std::vector<const CalcOpDecl*> producers;
    for (const CalcOpDecl& op : out) {
        switch (op.op) {
            case CalcOpKind::PushLength:
                stack.push_back(1);
                producers.push_back(&op);
                break;
            case CalcOpKind::PushNumber:
                stack.push_back(0);
                producers.push_back(&op);
                break;
            case CalcOpKind::PushVar:
                stack.push_back(2);
                producers.push_back(&op);
                break;
            default: {
                if (stack.size() < 2 || stack.size() > 32) {
                    error = "malformed calc()";
                    return false;
                }
                const int b = stack.back();
                const CalcOpDecl* bOp = producers.back();
                stack.pop_back();
                producers.pop_back();
                int& a = stack.back();
                producers.back() = nullptr;
                if (op.op == CalcOpKind::Add || op.op == CalcOpKind::Sub) {
                    if (a != 2 && b != 2 && a != b) {
                        error = "calc() cannot add or subtract a number and a length";
                        return false;
                    }
                    a = (a == 2) ? b : a;
                } else if (op.op == CalcOpKind::Min || op.op == CalcOpKind::Max) {
                    if (a != 2 && b != 2 && a != b) {
                        error = "min(), max() and clamp() cannot mix numbers and lengths";
                        return false;
                    }
                    a = (a == 2) ? b : a;
                } else if (op.op == CalcOpKind::Mul) {
                    if (a == 1 && b == 1) {
                        error = "calc() cannot multiply two lengths";
                        return false;
                    }
                    a = (a == 1 || b == 1) ? 1 : ((a == 2 || b == 2) ? 2 : 0);
                } else {
                    if (b == 1) {
                        error = "calc() cannot divide by a length";
                        return false;
                    }
                    if (bOp != nullptr && bOp->op == CalcOpKind::PushNumber && bOp->value == 0.0f) {
                        error = "calc() divides by zero";
                        return false;
                    }
                }
                break;
            }
        }
    }
    if (stack.size() != 1 || stack.back() == 0) {
        error = "calc() must produce a length";
        return false;
    }
    return true;
}

}  // namespace xt::compile
