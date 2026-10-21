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

/// Per-property value parsing (split from xts_values.cpp by T-412): the token cursor, lengths, colors and
/// easing functions.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace xts_values_detail {

// -------------------------------------------------------------------------------------------
// Token cursor: a read-only walk over a declaration's value tokens.
// -------------------------------------------------------------------------------------------

const Token Cursor::kEndToken{};

}  // namespace xts_values_detail

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

namespace xts_values_detail {

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

}  // namespace xts_values_detail

namespace {

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

}  // namespace

namespace xts_values_detail {

/// T-322: the `/` of `hsl(h s l / a)`. The lexer has no token for it; like calc()'s `/`, it
/// arrives as an Unknown "unexpected character" token.
bool isSlashTok(const Token& t) {
    return t.kind == TokenKind::Unknown && t.text == "unexpected character '/'";
}

}  // namespace xts_values_detail

namespace {

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

}  // namespace

namespace xts_values_detail {

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

}  // namespace xts_values_detail

namespace {

// -------------------------------------------------------------------------------------------
// Easing (shared by transition/animation lists and @keyframes' animation-timing-function).
// -------------------------------------------------------------------------------------------

/// T-258b (D11): the range of `steps(n)`; an `n` outside it is clamped (`stepsClamped` set, the
/// caller warns).
constexpr float kMinSteps = 1.0f;
constexpr float kMaxSteps = 65535.0f;

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

}  // namespace

namespace xts_values_detail {

/// T-335: `linear()` needs `easingPoints` (its points go there); without it, it is rejected.
bool parseEasingCursor(Cursor& cur, Easing& out, bool* stepsClamped, std::vector<EasingPoint>* easingPoints) {
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

}  // namespace xts_values_detail

}  // namespace xt::compile
