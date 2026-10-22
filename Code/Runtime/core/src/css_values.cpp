#include "css_values.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "css_color.h"          // Named colours and hsl() (T-322).
#include "transform_compose.h"  // Compose/decompose for arbitrary orders (05.5, T-120).
#include "xt/charconv.h"
#include "xt/num_guard.h"
#include "xt/property.h"

/// The CSS value grammar shared by the runtime and the compiler (T-413; see css_values.h). The
/// diagnostics wording is the compiler's, unchanged from Code/Runtime/compile/src/xts/xts_values*.cpp.

namespace xt::css_values {

const Tok Cursor::kEnd{};

bool isSlash(const Tok& t) {
    return t.kind == TokKind::Slash || (t.kind == TokKind::Unknown && t.text == "unexpected character '/'");
}

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

namespace {

/// Keyword text match without the Ident check (callers that already know the kind). ASCII
/// case-insensitive for both dialects (05 §5.3, ruling 2026-10-21; T-421).
bool matches(Dialect, std::string_view text, std::string_view word) { return iequals(text, word); }

void warn(Diag* diag, std::string_view message) {
    if (diag != nullptr) {
        diag->warn(message);
    }
}

void error(Diag* diag, std::string_view message) {
    if (diag != nullptr) {
        diag->error(message);
    }
}

}  // namespace

bool isKeyword(Dialect, const Tok& t, std::string_view word) {
    return t.kind == TokKind::Ident && iequals(t.text, word);
}

// ---------------------------------------------------------------------------------------------
// Single tokens.
// ---------------------------------------------------------------------------------------------

bool parseLengthTok(const Tok& tok, Dialect d, Length& out) {
    if (isKeyword(d, tok, "auto")) {
        out = Length{0, LengthUnit::Auto};
        return true;
    }
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (iequals(tok.unit, "px")) {
        out = Length{tok.num, LengthUnit::Px};
        return true;
    }
    if (iequals(tok.unit, "%")) {
        out = Length{tok.num, LengthUnit::Percent};
        return true;
    }
    if (iequals(tok.unit, "em")) {
        out = Length{tok.num, LengthUnit::Em};
        return true;
    }
    if (iequals(tok.unit, "rem")) {  // T-335: the root element's font size, resolved like em at style time.
        out = Length{tok.num, LengthUnit::Rem};
        return true;
    }
    if (tok.unit.empty() && tok.num == 0) {
        out = Length{0, LengthUnit::Px};
        return true;
    }
    if (d == Dialect::Runtime) {
        return false;  // 05 §5.9.3: setStyle does not take the T-176 units.
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

bool parseFloatTok(const Tok& tok, float& out) {
    if (tok.kind != TokKind::Number || !tok.unit.empty()) {
        return false;
    }
    out = tok.num;
    return true;
}

bool parseAngleTok(const Tok& tok, float& deg) {
    if (tok.kind != TokKind::Number || !iequals(tok.unit, "deg")) {
        return false;
    }
    deg = tok.num;
    return true;
}

bool parseTimeTok(const Tok& tok, float& seconds) {
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (iequals(tok.unit, "s")) {
        seconds = tok.num;
        return true;
    }
    if (iequals(tok.unit, "ms")) {
        seconds = tok.num / 1000.0f;
        return true;
    }
    return false;
}

bool parseAspectRatio(const Tok* toks, size_t count, float& out) {
    const auto positive = [](const Tok& t, float& v) { return parseFloatTok(t, v) && v > 0.0f; };
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "auto")) {
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
    if (count != 3 || !positive(toks[0], w) || !isSlash(toks[1]) || !positive(toks[2], h)) {
        return false;
    }
    out = w / h;
    return true;
}

bool parseFontWeightNumber(const Tok& tok, uint32_t& out) {
    if (tok.kind != TokKind::Number || !tok.unit.empty()) {
        return false;
    }
    const int w = toInt32Sat(tok.num);  // T-258b (D11): no UB past int.
    if (w < 100 || w > 900 || w % 100 != 0) {
        return false;
    }
    out = static_cast<uint32_t>(w);
    return true;
}

bool parseLineClampNumber(const Tok& tok, uint32_t& out) {
    const int n = toInt32Sat(tok.num);
    if (tok.kind != TokKind::Number || !tok.unit.empty() || n < 1 || static_cast<float>(n) != tok.num) {
        return false;
    }
    out = static_cast<uint32_t>(n);
    return true;
}

bool parseOrderInteger(const Tok* toks, size_t count, float& out) {
    if (count != 1 || toks[0].kind != TokKind::Number || !toks[0].unit.empty() ||
        !(std::fabs(toks[0].num) <= 2147483520.0f) || std::trunc(toks[0].num) != toks[0].num) {
        return false;  // T-336 (06 §6.9): `order` takes an <integer>.
    }
    out = toks[0].num;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Colours.
// ---------------------------------------------------------------------------------------------

namespace {

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

/// r/g/b: 0..255 or 0..100%. Returns the channel in [0, 1].
bool parseRgbChannel(const Tok& tok, float& out) {
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (iequals(tok.unit, "%")) {
        out = tok.num / 100.0f;
        return true;
    }
    if (tok.unit.empty()) {
        out = tok.num / 255.0f;
        return true;
    }
    return false;
}

/// T-322: an `hsl()` hue: a number (degrees), `deg`, `turn` or `rad`.
bool parseHueTok(const Tok& t, float& deg) {
    if (t.kind != TokKind::Number) {
        return false;
    }
    if (t.unit.empty() || iequals(t.unit, "deg")) {
        deg = t.num;
        return true;
    }
    if (iequals(t.unit, "turn")) {
        deg = t.num * 360.0f;
        return true;
    }
    if (iequals(t.unit, "rad")) {
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
    if (iequals(t.unit, "%")) {
        a = t.num / 100.0f;
        return true;
    }
    return false;
}

/// T-322: the arguments of `hsl()`/`hsla()` (aliases), `cur` at the '(': `(h, s%, l%[, a])` or
/// `(h s% l%[ / a])`.
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
        if (t.kind != TokKind::Number || !iequals(t.unit, "%")) {
            return false;
        }
        v = t.num / 100.0f;
        cur.advance();
    }
    float a = 1;
    if (legacy ? cur.peek().kind == TokKind::Comma : isSlash(cur.peek())) {
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

}  // namespace

bool parseHashColor(std::string_view hex, Color& out) {
    bool ok = true;
    for (char c : hex) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    // `#abc` is `#aabbcc` (CSS Color 4): n * 17 / 255, the same float as the six-digit form.
    const auto nibble = [&ok](char c) { return static_cast<float>(hexNibble(c, ok)) * 17.0f / 255.0f; };
    if (hex.size() == 3 || hex.size() == 4) {
        out.r = nibble(hex[0]);
        out.g = nibble(hex[1]);
        out.b = nibble(hex[2]);
        out.a = hex.size() == 4 ? nibble(hex[3]) : 1.0f;
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
    // Colour keywords and function names are ASCII case-insensitive for both callers.
    if (iequals(t.text, "transparent")) {
        cur.advance();
        out = Color{0, 0, 0, 0};
        return true;
    }
    if (iequals(t.text, "rgb") || iequals(t.text, "rgba")) {
        const bool wantAlpha = iequals(t.text, "rgba");
        cur.advance();
        if (cur.peek().kind != TokKind::LParen) {
            return false;
        }
        cur.advance();
        float ch[4] = {0, 0, 0, 1};
        const int need = wantAlpha ? 4 : 3;
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
    // T-322: the CSS Color Level 4 named colours (Code/Runtime/core/src/css_color.h).
    Color named;
    if (css_color::findNamedColor(t.text, named)) {
        cur.advance();
        out = named;
        return true;
    }
    return false;
}

bool parseColorTokens(const Tok* toks, size_t count, Color& out) {
    Cursor cur(toks, count);
    Color c;
    if (!parseColorCursor(cur, c) || !cur.atEnd()) {
        return false;
    }
    out = c;
    return true;
}

bool parseBorderColorSides(const Tok* toks, size_t count, Color out[4]) {
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
    expandSides(c, n, out);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Easing and lists.
// ---------------------------------------------------------------------------------------------

namespace {

constexpr float kMinSteps = 1.0f;
constexpr float kMaxSteps = 65535.0f;

/// T-335: `linear(<stop>[, <stop>]...)` with the cursor on `linear`; each stop is `<number>
/// [<percentage>]{0,2}` (CSS Easing 1). The canonical points are appended to `points` and `out`
/// indexes them there.
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

}  // namespace

bool isEasingStart(const Cursor& cur, Dialect d) {
    const Tok& t = cur.peek();
    if (t.kind != TokKind::Ident) {
        return false;
    }
    for (std::string_view word : {"linear", "ease", "ease-in", "ease-out", "ease-in-out", "cubic-bezier",
                                  "steps", "step-start", "step-end"}) {
        if (matches(d, t.text, word)) {
            return true;
        }
    }
    return false;
}

bool parseEasingCursor(Cursor& cur, Dialect d, Easing& out, bool* stepsClamped,
                       std::vector<EasingPoint>* points) {
    const Tok& t = cur.peek();
    if (t.kind != TokKind::Ident) {
        return false;
    }
    const auto is = [&](std::string_view word) { return matches(d, t.text, word); };
    if (is("linear") && cur.peek(1).kind == TokKind::LParen) {
        return points != nullptr && parseLinearFunction(cur, out, *points);
    }
    if (is("linear")) {
        cur.advance();
        out = Easing::linear();
        return true;
    }
    if (is("ease")) {
        cur.advance();
        out = Easing::cssEase();
        return true;
    }
    if (is("ease-in")) {
        cur.advance();
        out = Easing::cssEaseIn();
        return true;
    }
    if (is("ease-out")) {
        cur.advance();
        out = Easing::cssEaseOut();
        return true;
    }
    if (is("ease-in-out")) {
        cur.advance();
        out = Easing::cssEaseInOut();
        return true;
    }
    // T-322: CSS's step-start / step-end = steps(1, jump-start) / steps(1, jump-end).
    if (is("step-start") || is("step-end")) {
        out = Easing::steps(1, is("step-end"));
        cur.advance();
        return true;
    }
    if (is("cubic-bezier") && cur.peek(1).kind == TokKind::LParen) {
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
    if (is("steps") && cur.peek(1).kind == TokKind::LParen) {
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
            if (!isKeyword(d, kw, "start") && !isKeyword(d, kw, "end")) {
                return false;
            }
            jumpEnd = isKeyword(d, kw, "end");
            cur.advance();
        }
        if (cur.peek().kind != TokKind::RParen) {
            return false;
        }
        cur.advance();
        if (!(n >= kMinSteps && n <= kMaxSteps)) {  // T-258a: 1..65535.
            n = clampFinite(n, kMinSteps, kMaxSteps, kMinSteps);
            if (stepsClamped != nullptr) {
                *stepsClamped = true;
            }
        }
        out = Easing::steps(static_cast<uint16_t>(n), jumpEnd);
        return true;
    }
    return false;
}

IrEasing toIrEasing(const Easing& e) {
    if (e.kind == EasingKind::LinearPoints) {  // T-335: the offset / count into the point pool.
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
        } else if (t.kind == TokKind::RParen && --depth == 0) {
            cur.advance();
            return true;
        }
        inner.push_back(cur.advance());
    }
    return false;  // Unterminated.
}

std::vector<std::vector<Tok>> splitByCommaTopLevel(const Tok* toks, size_t count) {
    std::vector<std::vector<Tok>> items;
    std::vector<Tok> cur;
    int depth = 0;
    for (size_t i = 0; i < count; ++i) {
        const Tok& t = toks[i];
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

bool parseFontFamilyList(const Tok* toks, size_t count, std::vector<std::string>& out) {
    Cursor cur(toks, count);
    if (cur.atEnd()) {
        return false;
    }
    for (;;) {
        if (cur.peek().kind == TokKind::String) {
            if (cur.peek().text.empty()) {
                return false;  // `""` names no family.
            }
            out.emplace_back(cur.advance().text);
        } else if (cur.peek().kind == TokKind::Ident) {
            std::string name(cur.advance().text);
            while (cur.peek().kind == TokKind::Ident) {  // An unquoted name may be several idents.
                name += ' ';
                name += cur.advance().text;
            }
            out.push_back(std::move(name));
        } else {
            return false;
        }
        if (cur.peek().kind == TokKind::Comma) {
            cur.advance();
            continue;
        }
        break;
    }
    return cur.atEnd() && !out.empty();
}

AnimationItemResult parseAnimationItem(const Tok* toks, size_t count, Dialect d, Diag* diag,
                                       AnimationItem& out, std::vector<EasingPoint>* points) {
    Cursor cur(toks, count);
    if (cur.peek().kind != TokKind::Ident) {
        return AnimationItemResult::BadName;
    }
    out = AnimationItem{};
    out.name = cur.advance().text;
    if (!parseTimeTok(cur.peek(), out.duration)) {
        return AnimationItemResult::BadValue;
    }
    cur.advance();
    out.easing = Easing::linear();
    bool haveEasing = false, haveDelay = false, haveIter = false, haveDir = false, haveFill = false,
         havePlay = false;
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
        float num = 0;
        if (!haveDelay && parseTimeTok(t, num)) {
            out.delay = num;
            haveDelay = true;
            cur.advance();
            continue;
        }
        if (!haveEasing && isEasingStart(cur, d)) {
            bool stepsClamped = false;
            if (!parseEasingCursor(cur, d, out.easing, &stepsClamped, points)) {
                return AnimationItemResult::BadValue;
            }
            if (stepsClamped) {
                warn(diag, kStepsClampedMessage);
            }
            haveEasing = true;
            continue;
        }
        if (!haveIter && isKeyword(d, t, "infinite")) {
            out.infinite = true;
            haveIter = true;
            cur.advance();
            continue;
        }
        if (!haveIter && t.kind == TokKind::Number && t.unit.empty() && t.num >= 0) {
            if (d == Dialect::Runtime) {
                out.iterations = toUint16Sat(t.num);  // T-258a: saturates (05.x).
            } else if (t.num > static_cast<float>(kMaxFiniteIterations)) {
                // T-258b (D11): a finite count stays below the IR's `infinite` sentinel 0xFFFF.
                warn(diag, "animation iteration count clamped to " + std::to_string(kMaxFiniteIterations));
                out.iterations = kMaxFiniteIterations;
            } else {
                out.iterations = static_cast<uint16_t>(t.num);  // A fractional count truncates.
            }
            haveIter = true;
            cur.advance();
            continue;
        }
        if (!haveDir && t.kind == TokKind::Ident) {
            const std::string_view kDirections[] = {"normal", "reverse", "alternate", "alternate-reverse"};
            bool hit = false;
            for (uint8_t i = 0; i < 4 && !hit; ++i) {
                if (matches(d, t.text, kDirections[i])) {
                    out.direction = i;
                    hit = true;
                }
            }
            if (hit) {
                haveDir = true;
                cur.advance();
                continue;
            }
        }
        if (!haveFill && t.kind == TokKind::Ident) {
            const std::string_view kFills[] = {"none", "forwards", "backwards", "both"};
            bool hit = false;
            for (uint8_t i = 0; i < 4 && !hit; ++i) {
                if (matches(d, t.text, kFills[i])) {
                    out.fill = i;
                    hit = true;
                }
            }
            if (hit) {
                haveFill = true;
                cur.advance();
                continue;
            }
        }
        if (!havePlay && (isKeyword(d, t, "running") || isKeyword(d, t, "paused"))) {
            out.playState = isKeyword(d, t, "paused") ? 1 : 0;
            havePlay = true;
            cur.advance();
            continue;
        }
        return AnimationItemResult::BadValue;
    }
    return AnimationItemResult::Ok;
}

// ---------------------------------------------------------------------------------------------
// transform.
// ---------------------------------------------------------------------------------------------

int transformCategory(std::string_view fn) {
    if (iequals(fn, "translate") || iequals(fn, "translateX") || iequals(fn, "translateY") ||
        iequals(fn, "translateZ") || iequals(fn, "translate3d")) {
        return 0;
    }
    if (iequals(fn, "rotateX")) {
        return 1;
    }
    if (iequals(fn, "rotateY")) {
        return 2;
    }
    if (iequals(fn, "rotate") || iequals(fn, "rotateZ")) {  // T-322: rotateZ() is rotate().
        return 3;
    }
    if (iequals(fn, "skew") || iequals(fn, "skewX") || iequals(fn, "skewY")) {
        return 4;
    }
    if (iequals(fn, "scale") || iequals(fn, "scaleX") || iequals(fn, "scaleY")) {
        return 5;
    }
    return -1;
}

bool isUnsupportedTransformFn(std::string_view fn) {
    return iequals(fn, "rotate3d") || iequals(fn, "scale3d") || iequals(fn, "matrix3d") ||
           iequals(fn, "perspective");
}

namespace {

void pushLen(std::vector<PropValue>& out, uint16_t prop, Length len) {
    out.push_back(PropValue{prop, Value::ofLength(len.value, len.unit)});
}

void pushFloat(std::vector<PropValue>& out, uint16_t prop, float v) {
    out.push_back(PropValue{prop, Value::ofFloat(v)});
}

/// The ten components of a decomposed sequence. TranslateZ is a Float scene property (19.3/T-43c).
void pushComponents(const TransformComponents& c, std::vector<PropValue>& out) {
    pushLen(out, prop::TranslateX, Length{c.translateX, LengthUnit::Px});
    pushLen(out, prop::TranslateY, Length{c.translateY, LengthUnit::Px});
    pushFloat(out, prop::TranslateZ, c.translateZ);
    pushFloat(out, prop::RotationX, c.rotationX);
    pushFloat(out, prop::RotationY, c.rotationY);
    pushFloat(out, prop::Rotation, c.rotation);
    pushFloat(out, prop::SkewX, c.skewX);
    pushFloat(out, prop::SkewY, c.skewY);
    pushFloat(out, prop::ScaleX, c.scaleX);
    pushFloat(out, prop::ScaleY, c.scaleY);
}

/// Pre-pass: true when the sequence repeats a category or breaks the fixed order (or holds a
/// matrix()), i.e. when 05.5's compose/decompose path has to run. Anything malformed returns false
/// so the fixed-order parser reports it with its usual wording.
bool transformNeedsCompose(const Tok* toks, size_t count) {
    int lastCategory = -1;
    bool sawCategory[6] = {false, false, false, false, false, false};
    bool needs = false;
    size_t i = 0;
    while (i < count) {
        if (toks[i].kind != TokKind::Ident || i + 1 >= count || toks[i + 1].kind != TokKind::LParen) {
            return false;
        }
        if (iequals(toks[i].text, "matrix")) {
            needs = true;  // T-322: a 2D matrix() only ever goes through compose/decompose.
        } else {
            const int cat = transformCategory(toks[i].text);
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

/// Parses the whole sequence into 4x4-composable functions. Lengths must be px: a `%` or `em`
/// translate cannot be folded into a matrix (its value depends on the box or font size).
bool parseTransformFnList(const Tok* toks, size_t count, Diag* diag, std::vector<TransformFn>& out) {
    Cursor cur(toks, count);
    while (!cur.atEnd()) {
        const std::string_view fn = cur.peek().text;
        cur.advance();  // fn name
        cur.advance();  // '(' (transformNeedsCompose() already checked both)
        bool ok = true;
        bool nonPx = false;
        auto readPx = [&](float& v) -> bool {
            Length len;
            if (!parseLengthTok(cur.peek(), Dialect::Compiler, len)) {
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
            if (cur.peek().kind != TokKind::Comma) {
                return false;
            }
            cur.advance();
            return true;
        };
        TransformFn f;
        if (transformCategory(fn) == 0) {
            f.kind = TransformFn::Kind::Translate;
            float v = 0;
            ok = readPx(v);
            if (ok) {
                if (iequals(fn, "translateX") || iequals(fn, "translate")) {
                    f.a = v;
                } else if (iequals(fn, "translateY")) {
                    f.b = v;
                } else if (iequals(fn, "translateZ")) {
                    f.c = v;
                } else {  // translate3d
                    f.a = v;
                    ok = comma() && readPx(f.b) && comma() && readPx(f.c);
                }
                if (ok && iequals(fn, "translate") && cur.peek().kind == TokKind::Comma) {
                    cur.advance();
                    ok = readPx(f.b);
                }
            }
        } else if (iequals(fn, "rotateX") || iequals(fn, "rotateY") || iequals(fn, "rotate") ||
                   iequals(fn, "rotateZ")) {
            f.kind = iequals(fn, "rotateX")   ? TransformFn::Kind::RotateX
                     : iequals(fn, "rotateY") ? TransformFn::Kind::RotateY
                                              : TransformFn::Kind::RotateZ;
            ok = readAngle(f.a);
        } else if (iequals(fn, "matrix")) {
            // T-322: matrix(a, b, c, d, e, f) with unitless numbers (e/f are px), decomposed into
            // translate/rotate/skewX/scale; a singular matrix is dropped with a warning.
            float m[6] = {1, 0, 0, 1, 0, 0};
            for (int k = 0; ok && k < 6; ++k) {
                ok = (k == 0 || comma()) && readFloat(m[k]);
            }
            if (ok && cur.peek().kind == TokKind::RParen) {
                cur.advance();  // ')'
                TransformFn parts[4];
                if (decomposeMatrix2D(m[0], m[1], m[2], m[3], m[4], m[5], parts)) {
                    out.insert(out.end(), parts, parts + 4);
                } else if (diag != nullptr) {
                    diag->warn("transform matrix() is singular and cannot be decomposed; function dropped: " +
                               transformDeclText(toks, count));
                }
                continue;
            }
        } else if (transformCategory(fn) == 4) {
            f.kind = TransformFn::Kind::Skew;
            float v = 0;
            ok = readAngle(v);
            if (ok) {
                if (iequals(fn, "skewY")) {
                    f.b = v;
                } else {
                    f.a = v;
                    if (iequals(fn, "skew") && cur.peek().kind == TokKind::Comma) {
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
                if (iequals(fn, "scaleX")) {
                    f.a = v;
                } else if (iequals(fn, "scaleY")) {
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
            if (diag != nullptr) {
                if (nonPx) {
                    diag->error(
                        "transform sequence needs matrix composition, so its lengths must be px "
                        "(% and em cannot be resolved at compile time): " +
                        transformDeclText(toks, count));
                } else {
                    diag->warn("invalid value for \"transform\": bad arguments to \"" + std::string(fn) +
                               "\"");
                }
            }
            return false;
        }
        cur.advance();  // ')'
        out.push_back(f);
    }
    return true;
}

/// 05.5's slow path (T-120): compose the sequence into one 4x4 matrix and decompose it back into
/// 03.3's component set, so any order is legal.
bool parseTransformComposed(const Tok* toks, size_t count, Diag* diag, std::vector<PropValue>& out) {
    std::vector<TransformFn> fns;
    if (!parseTransformFnList(toks, count, diag, fns)) {
        return false;
    }
    TransformComponents c;
    if (decomposeTransformSequence(fns.data(), fns.size(), c) != TransformDecomposeStatus::Ok) {
        if (diag != nullptr) {
            diag->error("transform sequence needs a 3D shear or z scale that XTFrame cannot represent: " +
                        transformDeclText(toks, count));
        }
        return false;
    }
    if (transformSkewOutOfRange(c) && diag != nullptr) {
        diag->warn("transform sequence decomposes to a skew beyond +/-89 degrees: " +
                   transformDeclText(toks, count));
    }
    pushComponents(c, out);
    return true;
}

}  // namespace

void pushIdentityTransform(std::vector<PropValue>& out) { pushComponents(TransformComponents{}, out); }

std::string transformDeclText(const Tok* toks, size_t count) {
    std::string out = "transform:";
    for (size_t i = 0; i < count; ++i) {
        const Tok& t = toks[i];
        switch (t.kind) {
            case TokKind::Ident:
                out += ' ';
                out += t.text;
                break;
            case TokKind::Number: {
                char buf[32];
                out.append(buf, formatFloat(buf, sizeof(buf), static_cast<double>(t.num), 6));
                out += t.unit;
                break;
            }
            case TokKind::LParen:
                out += '(';
                break;
            case TokKind::RParen:
                out += ')';
                break;
            case TokKind::Comma:
                out += ", ";
                break;
            default:
                break;
        }
    }
    return out;
}

bool parseTransformFixedOrder(const Tok* toks, size_t count, Diag* diag, std::vector<PropValue>& out) {
    Cursor cur(toks, count);
    if (cur.atEnd()) {
        return false;
    }
    // 05.5 (T-69): a lone `none` keyword. Mixing it with functions (`none rotate(10deg)`) falls
    // through to the loop below and hits the "invalid value" diagnostic.
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "none")) {
        pushIdentityTransform(out);
        return true;
    }
    int lastCategory = -1;
    bool sawCategory[6] = {false, false, false, false, false, false};
    std::vector<PropValue> decls;
    while (!cur.atEnd()) {
        const Tok& fnTok = cur.peek();
        if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
            warn(diag, "invalid value for \"transform\"");
            return false;
        }
        const std::string_view fn = fnTok.text;
        if (isUnsupportedTransformFn(fn)) {
            if (diag != nullptr) {
                diag->error("invalid value for \"transform\": unsupported transform function \"" +
                            std::string(fn) + "\"");
            }
            return false;
        }
        const int cat = transformCategory(fn);
        if (cat < 0) {
            if (diag != nullptr) {
                diag->warn("invalid value for \"transform\": unknown function \"" + std::string(fn) + "\"");
            }
            return false;
        }
        if (sawCategory[cat] || cat < lastCategory) {
            error(
                diag,
                "transform sequence must follow the order translate, rotateX, rotateY, rotate, skew, scale, "
                "each at most once");
            return false;
        }
        sawCategory[cat] = true;
        lastCategory = cat;
        cur.advance();  // fn name
        cur.advance();  // '('
        // translate*: Length args (px, %, em, rem); rotate*/skew*: angles; scale*: unitless.
        auto readLength = [&](Length& len) -> bool {
            if (!parseLengthTok(cur.peek(), Dialect::Compiler, len) ||
                (len.unit != LengthUnit::Px && len.unit != LengthUnit::Percent &&
                 len.unit != LengthUnit::Em && len.unit != LengthUnit::Rem)) {
                return false;
            }
            cur.advance();
            return true;
        };
        // translateZ/translate3d's z: 05.5/19.10 (T-47a) rejects percent with its own message.
        bool zPercentError = false;
        auto readZLength = [&](Length& len) -> bool {
            if (!parseLengthTok(cur.peek(), Dialect::Compiler, len)) {
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
            if (cur.peek().kind != TokKind::Comma) {
                return false;
            }
            cur.advance();
            return true;
        };
        bool ok = true;
        if (iequals(fn, "translate")) {
            Length x, y;
            ok = readLength(x);
            if (ok) {
                pushLen(decls, prop::TranslateX, x);
                if (comma()) {
                    ok = readLength(y);
                    if (ok) {
                        pushLen(decls, prop::TranslateY, y);
                    }
                }
            }
        } else if (iequals(fn, "translateX") || iequals(fn, "translateY")) {
            Length v;
            ok = readLength(v);
            if (ok) {
                pushLen(decls, iequals(fn, "translateX") ? prop::TranslateX : prop::TranslateY, v);
            }
        } else if (iequals(fn, "translateZ")) {
            Length z;
            ok = readZLength(z);
            if (ok) {
                pushLen(decls, prop::TranslateZ, z);
            }
        } else if (iequals(fn, "translate3d")) {
            Length x, y, z;
            ok = readLength(x) && comma() && readLength(y) && comma() && readZLength(z);
            if (ok) {
                pushLen(decls, prop::TranslateX, x);
                pushLen(decls, prop::TranslateY, y);
                pushLen(decls, prop::TranslateZ, z);
            }
        } else if (cat >= 1 && cat <= 3) {  // rotateX / rotateY / rotate / rotateZ
            float deg = 0;
            ok = readAngle(deg);
            if (ok) {
                pushFloat(decls,
                          cat == 1   ? prop::RotationX
                          : cat == 2 ? prop::RotationY
                                     : prop::Rotation,
                          deg);
            }
        } else if (iequals(fn, "skew")) {
            float x = 0, y = 0;
            ok = readAngle(x);
            if (ok) {
                pushFloat(decls, prop::SkewX, x);
                if (comma()) {
                    ok = readAngle(y);
                    if (ok) {
                        pushFloat(decls, prop::SkewY, y);
                    }
                }
            }
        } else if (iequals(fn, "skewX") || iequals(fn, "skewY")) {
            float v = 0;
            ok = readAngle(v);
            if (ok) {
                pushFloat(decls, iequals(fn, "skewX") ? prop::SkewX : prop::SkewY, v);
            }
        } else if (iequals(fn, "scale")) {
            float x = 0, y = 0;
            ok = readFloat(x);
            if (ok) {
                y = x;
                if (comma()) {
                    ok = readFloat(y);
                }
                if (ok) {
                    pushFloat(decls, prop::ScaleX, x);
                    pushFloat(decls, prop::ScaleY, y);
                }
            }
        } else {  // scaleX / scaleY
            float v = 0;
            ok = readFloat(v);
            if (ok) {
                pushFloat(decls, iequals(fn, "scaleX") ? prop::ScaleX : prop::ScaleY, v);
            }
        }
        if (!ok || cur.peek().kind != TokKind::RParen) {
            if (zPercentError) {
                error(diag, "translateZ does not accept percent");
            } else if (diag != nullptr) {
                diag->warn("invalid value for \"transform\": bad arguments to \"" + std::string(fn) + "\"");
            }
            return false;
        }
        cur.advance();  // ')'
    }
    out.insert(out.end(), decls.begin(), decls.end());
    return true;
}

bool parseTransform(const Tok* toks, size_t count, Diag* diag, std::vector<PropValue>& out) {
    if (transformNeedsCompose(toks, count)) {
        return parseTransformComposed(toks, count, diag, out);
    }
    return parseTransformFixedOrder(toks, count, diag, out);
}

bool independentTransformFnToks(IndependentTransform which, const Tok* toks, size_t count, Dialect d,
                                Diag* diag, std::vector<Tok>& fnToks, bool& isNone) {
    fnToks.clear();
    isNone = false;
    const char* name = which == IndependentTransform::Translate ? "translate"
                       : which == IndependentTransform::Rotate  ? "rotate"
                                                                : "scale";
    auto invalid = [&]() {
        if (diag != nullptr) {
            diag->warn("invalid value for \"" + std::string(name) + "\"");
        }
        return false;
    };
    if (count == 1 && isKeyword(d, toks[0], "none")) {
        isNone = true;
        return true;
    }
    const auto token = [](TokKind kind, std::string_view text = {}) {
        Tok t;
        t.kind = kind;
        t.text = text;
        return t;
    };
    // `fn(args...)` with the argument tokens as written (plus `extra`, when given, as a last one).
    const auto append = [&](std::string_view fn, const Tok* args, size_t argCount, const Tok* extra) {
        fnToks.push_back(token(TokKind::Ident, fn));
        fnToks.push_back(token(TokKind::LParen));
        const size_t total = argCount + (extra != nullptr ? 1 : 0);
        for (size_t i = 0; i < total; ++i) {
            if (i > 0) {
                fnToks.push_back(token(TokKind::Comma));
            }
            fnToks.push_back(i < argCount ? args[i] : *extra);
        }
        fnToks.push_back(token(TokKind::RParen));
    };
    if (which == IndependentTransform::Translate) {
        // `<x> [<y> [<z>]]`: x/y as transform's translate() (px, %, em, rem); z as translateZ() (no
        // %). A missing y is 0 (CSS), so the property always sets both axes.
        if (count == 0 || count > 3) {
            return invalid();
        }
        for (size_t i = 0; i < count; ++i) {
            Length len;
            if (!parseLengthTok(toks[i], d, len)) {
                return invalid();
            }
            if (i == 2 && len.unit == LengthUnit::Percent) {
                error(diag, "translateZ does not accept percent");
                return false;
            }
            const bool okUnit = len.unit == LengthUnit::Px || len.unit == LengthUnit::Em ||
                                len.unit == LengthUnit::Rem || (i < 2 && len.unit == LengthUnit::Percent);
            if (!okUnit) {
                return invalid();
            }
        }
        Tok zero = token(TokKind::Number, "0");
        zero.unit = "px";
        append(count == 3 ? "translate3d" : "translate", toks, count, count == 1 ? &zero : nullptr);
        return true;
    }
    if (which == IndependentTransform::Rotate) {
        // `<angle> | z <angle>`; the x/y axes belong to transform's rotateX()/rotateY() (19 §19.10).
        size_t i = 0;
        if (count > 0 && toks[0].kind == TokKind::Ident) {
            if (isKeyword(d, toks[0], "x") || isKeyword(d, toks[0], "y")) {
                if (diag != nullptr) {
                    diag->warn("\"rotate: " + std::string(toks[0].text) +
                               "\" is not supported (use transform: rotateX()/rotateY()); ignored");
                }
                return false;
            }
            if (!isKeyword(d, toks[0], "z")) {
                return invalid();
            }
            i = 1;
        }
        float deg = 0;
        if (count != i + 1 || !parseAngleTok(toks[i], deg)) {
            return invalid();
        }
        append("rotate", toks + i, 1, nullptr);
        return true;
    }
    // scale: `<sx> [<sy>]`, unitless; a missing sy is sx.
    if (count == 0 || count > 2) {
        return invalid();
    }
    for (size_t i = 0; i < count; ++i) {
        float v = 0;
        if (!parseFloatTok(toks[i], v)) {
            return invalid();
        }
    }
    append("scale", toks, count, nullptr);
    return true;
}

bool parseIndependentTransform(IndependentTransform which, const Tok* toks, size_t count, Dialect d,
                               Diag* diag, std::vector<PropValue>& out) {
    std::vector<Tok> fnToks;
    bool isNone = false;
    if (!independentTransformFnToks(which, toks, count, d, diag, fnToks, isNone)) {
        return false;
    }
    if (!isNone) {
        return parseTransformFixedOrder(fnToks.data(), fnToks.size(), diag, out);
    }
    std::vector<PropValue> identity;
    pushIdentityTransform(identity);
    for (const PropValue& v : identity) {
        const bool mine =
            which == IndependentTransform::Translate
                ? (v.prop == prop::TranslateX || v.prop == prop::TranslateY || v.prop == prop::TranslateZ)
            : which == IndependentTransform::Rotate ? v.prop == prop::Rotation
                                                    : (v.prop == prop::ScaleX || v.prop == prop::ScaleY);
        if (mine) {
            out.push_back(v);
        }
    }
    return true;
}

bool isTransformComponentProp(uint16_t p) {
    return p == prop::TranslateX || p == prop::TranslateY || p == prop::TranslateZ || p == prop::RotationX ||
           p == prop::RotationY || p == prop::Rotation || p == prop::SkewX || p == prop::SkewY ||
           p == prop::ScaleX || p == prop::ScaleY;
}

// ---------------------------------------------------------------------------------------------
// Origins, background, shorthands.
// ---------------------------------------------------------------------------------------------

namespace {

/// left/center/right -> 0%/50%/100%; top/center/bottom -> 0%/50%/100%; or a length.
bool parseOriginComponent(const Tok& tok, bool horizontal, Dialect d, Length& out) {
    if (tok.kind == TokKind::Ident) {
        const auto is = [&](std::string_view word) { return matches(d, tok.text, word); };
        if (is("center")) {
            out = Length{50, LengthUnit::Percent};
            return true;
        }
        if (is(horizontal ? "left" : "top")) {
            out = Length{0, LengthUnit::Percent};
            return true;
        }
        if (is(horizontal ? "right" : "bottom")) {
            out = Length{100, LengthUnit::Percent};
            return true;
        }
        return false;
    }
    return parseLengthTok(tok, d, out);
}

/// T-327: one `background-size` component: `auto` or a non-negative length / percentage.
bool parseBackgroundSizeComponent(const Tok& tok, Dialect d, Length& out) {
    if (!parseLengthTok(tok, d, out)) {
        return false;
    }
    if (out.unit == LengthUnit::Auto) {
        out = Length{0, LengthUnit::Auto};
        return true;
    }
    return out.value >= 0.0f;
}

uint32_t textDecorationBit(const Tok& t, Dialect d) {
    if (isKeyword(d, t, "underline")) {
        return kTextDecorationUnderline;
    }
    if (isKeyword(d, t, "overline")) {
        return kTextDecorationOverline;
    }
    if (isKeyword(d, t, "line-through")) {
        return kTextDecorationLineThrough;
    }
    return 0;
}

}  // namespace

bool parseOriginPair(const Tok* toks, size_t count, Dialect d, Length& x, Length& y) {
    if (count == 0 || count > 2) {
        return false;
    }
    y = Length{50, LengthUnit::Percent};
    if (!parseOriginComponent(toks[0], /*horizontal=*/true, d, x)) {
        return false;
    }
    return count == 1 || parseOriginComponent(toks[1], /*horizontal=*/false, d, y);
}

bool parseBackgroundSize(const Tok* toks, size_t count, Dialect d, Length& x, Length& y) {
    if (count == 0 || count > 2) {
        return false;
    }
    x = Length{0, LengthUnit::Auto};
    y = Length{0, LengthUnit::Auto};
    if (count == 1 && (isKeyword(d, toks[0], "cover") || isKeyword(d, toks[0], "contain"))) {
        x = Length{isKeyword(d, toks[0], "cover") ? kBackgroundSizeCover : kBackgroundSizeContain,
                   LengthUnit::Auto};
        y = x;
        return true;
    }
    if (!parseBackgroundSizeComponent(toks[0], d, x)) {
        return false;
    }
    return count == 1 || parseBackgroundSizeComponent(toks[1], d, y);
}

bool parseBorderStyleSides(const Tok* toks, size_t count, Dialect d, uint32_t out[4]) {
    if (count == 0 || count > 4) {
        return false;
    }
    static constexpr std::string_view kStyles[] = {"none", "solid", "dashed", "dotted"};  // kBorderStyle*.
    uint32_t v[4] = {};
    for (size_t i = 0; i < count; ++i) {
        bool hit = false;
        for (uint32_t k = 0; k < 4 && !hit; ++k) {
            if (isKeyword(d, toks[i], kStyles[k])) {
                v[i] = k;
                hit = true;
            }
        }
        if (!hit) {
            return false;
        }
    }
    expandSides(v, count, out);
    return true;
}

bool parseOutline(const Tok* toks, size_t count, Dialect d, OutlineValue& out) {
    if (count == 0) {
        return false;
    }
    out = OutlineValue{};
    bool haveWidth = false, haveStyle = false;
    Cursor cur(toks, count);
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
        if (!haveStyle && (isKeyword(d, t, "none") || isKeyword(d, t, "solid"))) {
            out.style = isKeyword(d, t, "solid") ? 1 : 0;
            haveStyle = true;
            cur.advance();
            continue;
        }
        if (!haveWidth) {
            Length len;
            if (parseLengthTok(t, d, len) && len.unit != LengthUnit::Auto &&
                len.unit != LengthUnit::Percent && len.value >= 0.0f) {
                out.width = len;
                haveWidth = true;
                cur.advance();
                continue;
            }
        }
        if (!out.haveColor) {
            Cursor probe = cur;
            Color c;
            if (parseColorCursor(probe, c)) {
                cur = probe;
                out.color = c;
                out.haveColor = true;
                continue;
            }
        }
        return false;
    }
    return true;
}

bool parseTextDecorationLine(const Tok* toks, size_t count, Dialect d, uint32_t& out) {
    out = 0;
    if (count == 1 && isKeyword(d, toks[0], "none")) {
        return true;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t bit = textDecorationBit(toks[i], d);
        if (bit == 0 || (out & bit) != 0) {
            return false;
        }
        out |= bit;
    }
    return count > 0;
}

bool isDecorationThickness(const Length& len) {
    return len.unit == LengthUnit::Auto ||
           ((len.unit == LengthUnit::Px || len.unit == LengthUnit::Em) && len.value >= 0.0f);
}

bool parseTextDecoration(const Tok* toks, size_t count, Dialect d, TextDecorationValue& out) {
    if (count == 0) {
        return false;
    }
    out = TextDecorationValue{};
    bool haveLine = false, haveThickness = false;
    Cursor cur(toks, count);
    while (!cur.atEnd()) {
        const Tok& t = cur.peek();
        if (!haveLine && isKeyword(d, t, "none")) {
            haveLine = true;
            cur.advance();
            continue;
        }
        const uint32_t bit = textDecorationBit(t, d);
        if (bit != 0) {
            if ((out.lines & bit) != 0 || (haveLine && out.lines == 0)) {
                return false;  // A repeated line keyword, or one after `none`.
            }
            out.lines |= bit;
            haveLine = true;
            cur.advance();
            continue;
        }
        if (!haveThickness) {
            Length len;
            if (parseLengthTok(t, d, len) && isDecorationThickness(len)) {
                out.thickness = len;
                haveThickness = true;
                cur.advance();
                continue;
            }
        }
        if (!out.haveColor) {
            Cursor probe = cur;
            Color c;
            if (parseColorCursor(probe, c)) {
                cur = probe;
                out.color = c;
                out.haveColor = true;
                continue;
            }
        }
        return false;
    }
    return true;
}

bool parseUiScale(const Tok* toks, size_t count, Dialect d, float out[3]) {
    float v[3] = {0, 0, 0};
    if (count == 1 && isKeyword(d, toks[0], "auto")) {
        v[0] = 0;
    } else if (count == 1 && isKeyword(d, toks[0], "none")) {
        v[0] = 1;
    } else if (count == 6 && isKeyword(d, toks[0], "clamp") && toks[1].kind == TokKind::LParen &&
               toks[3].kind == TokKind::Comma && toks[5].kind == TokKind::RParen &&
               parseFloatTok(toks[2], v[1]) && parseFloatTok(toks[4], v[2]) && v[1] > 0.0f && v[2] >= v[1] &&
               std::isfinite(v[2])) {
        v[0] = 2;
    } else {
        return false;
    }
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    return true;
}

bool parseHitSlop(const Tok* toks, size_t count, Dialect d, Length out[4]) {
    if (count < 1 || count > 4) {
        return false;
    }
    Length v[4]{};
    for (size_t i = 0; i < count; ++i) {
        if (!parseLengthTok(toks[i], d, v[i]) || !(v[i].value >= 0.0f) || !std::isfinite(v[i].value) ||
            (v[i].unit != LengthUnit::Px && v[i].unit != LengthUnit::Percent &&
             v[i].unit != LengthUnit::Em)) {
            return false;
        }
    }
    expandSides(v, count, out);
    return true;
}

bool parseGridLine(const Tok* toks, size_t count, uint32_t& out, bool& unsupported) {
    const auto integer = [](const Tok& t, int& v) {
        if (t.kind != TokKind::Number || !t.unit.empty() || std::trunc(t.num) != t.num ||
            !(std::fabs(t.num) <= 1.0e6f)) {
            return false;
        }
        v = static_cast<int>(t.num);
        return true;
    };
    int v = 0;
    if (count == 1 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "auto")) {
        out = kGridLineAuto;
        return true;
    }
    if (count == 1 && integer(toks[0], v)) {
        if (v < 1) {
            unsupported = true;  // 0 is invalid in CSS; negative lines are not done.
            return false;
        }
        out = static_cast<uint32_t>(std::min<int>(v, static_cast<int>(kMaxGridTracks) + 1));
        return true;
    }
    if (count == 2 && toks[0].kind == TokKind::Ident && iequals(toks[0].text, "span") &&
        integer(toks[1], v)) {
        if (v < 1) {
            return false;
        }
        out = kGridLineSpan | static_cast<uint32_t>(std::min<int>(v, static_cast<int>(kMaxGridTracks)));
        return true;
    }
    if (count >= 1 && toks[0].kind == TokKind::Ident) {
        unsupported = true;  // A named line.
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// effect.
// ---------------------------------------------------------------------------------------------

bool readEffectLen(Cursor& cur, Dialect d, float& outPx, bool& percentError) {
    Length len;
    if (!parseLengthTok(cur.peek(), d, len)) {
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

namespace {

/// A token that could start an effect length (shadow()'s optional blur/spread, outline()'s
/// softness), never the bare number that follows the colour.
bool isEffectLenTok(const Tok& t) {
    return t.kind == TokKind::Number &&
           (iequals(t.unit, "px") || iequals(t.unit, "em") || (t.unit.empty() && t.num == 0));
}

bool atIdent(const Cursor& cur, std::string_view word) {
    return !cur.atEnd() && cur.peek().kind == TokKind::Ident && cur.peek().text == word;
}

bool atNumber(const Cursor& cur) {
    return !cur.atEnd() && cur.peek().kind == TokKind::Number && cur.peek().unit.empty();
}

// shadow( <len> <len> [<len> [<len>]] <color> [<number>] [inset] ) -> DropShadow.
bool parseShadowFn(const std::vector<Tok>& inner, Dialect d, bool& percentError, EffectInstance& out) {
    out.kind = EffectKind::DropShadow;
    Cursor cur(inner);
    float offX = 0, offY = 0;
    if (!readEffectLen(cur, d, offX, percentError) || !readEffectLen(cur, d, offY, percentError)) {
        return false;
    }
    float extra[2] = {0, 0};
    int extraCount = 0;
    while (extraCount < 2 && isEffectLenTok(cur.peek())) {
        if (!readEffectLen(cur, d, extra[static_cast<size_t>(extraCount)], percentError)) {
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
        opacity = cur.advance().num;
    }
    float insetFlag = 0;  // A trailing `inset` (18.4), only valid last.
    if (atIdent(cur, "inset")) {
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
bool parseGlowFn(const std::vector<Tok>& inner, Dialect d, bool& percentError, EffectInstance& out) {
    out.kind = EffectKind::Glow;
    Cursor cur(inner);
    float radius = 0;
    if (!readEffectLen(cur, d, radius, percentError)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float intensity = 1;
    if (atNumber(cur)) {
        intensity = cur.advance().num;
    }
    float innerFlag = 0;
    if (atIdent(cur, "inner")) {
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
bool parseBlurFn(const std::vector<Tok>& inner, Dialect d, bool& percentError, EffectInstance& out) {
    out.kind = EffectKind::Blur;
    Cursor cur(inner);
    float rx = 0;
    if (!readEffectLen(cur, d, rx, percentError)) {
        return false;
    }
    float ry = rx;
    if (!cur.atEnd() && !readEffectLen(cur, d, ry, percentError)) {
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
// [invert(n|%)] [sepia(n|%)] ) -> ColorAdjust; each optional, at most once, any order.
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
    Cursor cur(inner);
    while (!cur.atEnd()) {
        const Tok& fnTok = cur.peek();
        if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
            return false;
        }
        const std::string_view fn = fnTok.text;
        const int idx = iequals(fn, "brightness")   ? 0
                        : iequals(fn, "contrast")   ? 1
                        : iequals(fn, "saturate")   ? 2
                        : iequals(fn, "hue-rotate") ? 3
                        : iequals(fn, "opacity")    ? 4
                        : iequals(fn, "invert")     ? 5
                        : iequals(fn, "sepia")      ? 6
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
                if (a.kind != TokKind::Number || (!a.unit.empty() && !iequals(a.unit, "%"))) {
                    return false;
                }
                amount = iequals(a.unit, "%") ? a.num / 100.0f : a.num;
            } else if (!args.empty()) {
                return false;
            }
            out.params[static_cast<size_t>(idx)] = std::clamp(amount, 0.0f, 1.0f);
            continue;
        }
        float v = 0;
        if (args.size() != 1 || !(idx == 3 ? parseAngleTok(args[0], v) : parseFloatTok(args[0], v))) {
            return false;
        }
        out.params[static_cast<size_t>(idx)] = v;
    }
    return true;
}

// `<color> <percent>` stops for gradient-mask() (the percent is required).
bool parseColorStopList(const std::vector<std::vector<Tok>>& items, Diag* diag,
                        std::vector<GradientStop>& out) {
    if (items.empty()) {
        error(diag, "gradient-mask requires at least one color stop");
        return false;
    }
    if (items.size() > kMaxGradientMaskStops) {
        error(diag, "gradient-mask has too many color stops (max 16)");
        return false;
    }
    for (const std::vector<Tok>& item : items) {
        Cursor cur(item);
        Color c;
        if (!parseColorCursor(cur, c)) {
            error(diag, "invalid value for \"effect\": bad gradient-mask color stop");
            return false;
        }
        if (cur.atEnd() || cur.peek().kind != TokKind::Number || cur.peek().unit != "%") {
            error(diag, "invalid value for \"effect\": gradient-mask color stop requires a percentage");
            return false;
        }
        const float pct = cur.advance().num;
        if (!cur.atEnd()) {
            error(diag, "invalid value for \"effect\": bad gradient-mask color stop");
            return false;
        }
        out.push_back(GradientStop{pct / 100.0f, c});
    }
    return true;
}

// gradient-mask( linear|radial , <len> <len> , <len> <len> , <color-stop>+ [, invert] ).
bool parseGradientMaskFn(const std::vector<Tok>& inner, Dialect d, Diag* diag, bool& percentError,
                         EffectInstance& out, std::vector<GradientStop>& stops) {
    out.kind = EffectKind::GradientMask;
    std::vector<std::vector<Tok>> items = splitByCommaTopLevel(inner.data(), inner.size());
    if (items.size() < 4 || items[0].size() != 1 || items[0][0].kind != TokKind::Ident) {
        return false;
    }
    float radial = 0;
    if (iequals(items[0][0].text, "linear")) {
        radial = 0;
    } else if (iequals(items[0][0].text, "radial")) {
        radial = 1;
    } else {
        return false;
    }
    if (items[1].size() != 2 || items[2].size() != 2) {
        return false;
    }
    float p[4] = {0, 0, 0, 0};
    for (int k = 0; k < 2; ++k) {
        Cursor pc(items[static_cast<size_t>(k + 1)]);
        if (!readEffectLen(pc, d, p[2 * k], percentError) ||
            !readEffectLen(pc, d, p[2 * k + 1], percentError) || !pc.atEnd()) {
            return false;
        }
    }
    std::vector<std::vector<Tok>> stopItems(items.begin() + 3, items.end());
    float invert = 0;
    if (!stopItems.empty()) {
        const std::vector<Tok>& last = stopItems.back();
        if (last.size() == 1 && last[0].kind == TokKind::Ident && iequals(last[0].text, "invert")) {
            invert = 1;
            stopItems.pop_back();
        }
    }
    std::vector<GradientStop> parsed;
    if (!parseColorStopList(stopItems, diag, parsed)) {
        return false;
    }
    out.params[0] = p[0];
    out.params[1] = p[1];
    out.params[2] = p[2];
    out.params[3] = p[3];
    out.params[4] = radial;
    out.params[5] = invert;
    stops = std::move(parsed);
    return true;
}

// shine( <angle> <len> <color> [<number> [<number> [<number>]]] ) -> Shine; the sixth argument is
// `position` (0..1, clamped with a warning).
bool parseShineFn(const std::vector<Tok>& inner, Dialect d, Diag* diag, bool& percentError,
                  EffectInstance& out) {
    out.kind = EffectKind::Shine;
    Cursor cur(inner);
    float angle = 0;
    if (!parseAngleTok(cur.peek(), angle)) {
        return false;
    }
    cur.advance();
    float width = 0;
    if (!readEffectLen(cur, d, width, percentError)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float intensity = 1;
    float softness = 0.5f;
    float position = 0;
    if (atNumber(cur)) {
        intensity = cur.advance().num;
        if (atNumber(cur)) {
            softness = cur.advance().num;
            if (atNumber(cur)) {
                position = cur.advance().num;
                if (position < 0.0f || position > 1.0f) {
                    warn(diag, "shine() position out of range [0, 1], clamped");
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
bool parseOutlineFn(const std::vector<Tok>& inner, Dialect d, bool& percentError, EffectInstance& out) {
    out.kind = EffectKind::Outline;
    Cursor cur(inner);
    float radius = 0;
    if (!readEffectLen(cur, d, radius, percentError)) {
        return false;
    }
    Color color;
    if (!parseColorCursor(cur, color)) {
        return false;
    }
    float softness = 0;
    if (isEffectLenTok(cur.peek()) && !readEffectLen(cur, d, softness, percentError)) {
        return false;
    }
    float position = 0;
    if (!cur.atEnd() && cur.peek().kind == TokKind::Ident) {
        const std::string_view kw = cur.peek().text;
        if (iequals(kw, "outer")) {
            position = 0;
        } else if (iequals(kw, "center")) {
            position = 1;
        } else if (iequals(kw, "inner")) {
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

// backdrop-blur( <len> [<number> [<number>]] ) -> BackdropBlur (18.4).
bool parseBackdropBlurFn(const std::vector<Tok>& inner, Dialect d, bool& percentError, EffectInstance& out) {
    out.kind = EffectKind::BackdropBlur;
    Cursor cur(inner);
    float radius = 0;
    if (!readEffectLen(cur, d, radius, percentError)) {
        return false;
    }
    float extra[2] = {0, 0};
    int extraCount = 0;
    while (extraCount < 2 && atNumber(cur)) {
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

EffectCall nextEffectCall(Cursor& cur, std::string_view& fn, std::vector<Tok>& inner) {
    const Tok& fnTok = cur.peek();
    if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
        return EffectCall::NotAFunction;
    }
    fn = fnTok.text;
    cur.advance();  // fn name.
    inner.clear();
    return extractParenGroup(cur, inner) ? EffectCall::Ok : EffectCall::Unterminated;
}

EffectFnResult parseEffectFn(std::string_view fn, const std::vector<Tok>& inner, Dialect d, Diag* diag,
                             bool& percentError, EffectInstance& out, std::vector<GradientStop>& stops) {
    bool ok = false;
    if (iequals(fn, "shadow")) {
        ok = parseShadowFn(inner, d, percentError, out);
    } else if (iequals(fn, "glow")) {
        ok = parseGlowFn(inner, d, percentError, out);
    } else if (iequals(fn, "blur")) {
        ok = parseBlurFn(inner, d, percentError, out);
    } else if (iequals(fn, "color-adjust")) {
        ok = parseColorAdjustFn(inner, out);
    } else if (iequals(fn, "gradient-mask")) {
        ok = parseGradientMaskFn(inner, d, diag, percentError, out, stops);
    } else if (iequals(fn, "shine")) {
        ok = parseShineFn(inner, d, diag, percentError, out);
    } else if (iequals(fn, "outline")) {
        ok = parseOutlineFn(inner, d, percentError, out);
    } else if (iequals(fn, "backdrop-blur")) {
        ok = parseBackdropBlurFn(inner, d, percentError, out);
    } else {
        return EffectFnResult::Unknown;
    }
    return ok ? EffectFnResult::Ok : EffectFnResult::Bad;
}

// ---------------------------------------------------------------------------------------------
// background-gradient.
// ---------------------------------------------------------------------------------------------

namespace {

constexpr size_t kMaxGradientStops = 16;  // Same per-gradient stop cap as GradientMask's (18.2).

/// `<angle>`: deg / turn / rad, plus unitless 0; in degrees.
bool parseGradientAngle(const Tok& tok, float& deg) {
    if (tok.kind != TokKind::Number) {
        return false;
    }
    if (iequals(tok.unit, "deg")) {
        deg = tok.num;
        return true;
    }
    if (iequals(tok.unit, "turn")) {
        deg = tok.num * 360.0f;
        return true;
    }
    if (iequals(tok.unit, "rad")) {
        deg = tok.num * (180.0f / 3.14159265358979323846f);
        return true;
    }
    if (tok.unit.empty() && tok.num == 0) {
        deg = 0;
        return true;
    }
    return false;
}

/// A gradient geometry length: px or % only (no stage before Painter knows the font size).
bool parseGradientLen(const Tok& tok, Dialect d, Length& out) {
    Length len;
    if (!parseLengthTok(tok, d, len) || (len.unit != LengthUnit::Px && len.unit != LengthUnit::Percent)) {
        return false;
    }
    out = len;
    return true;
}

/// Distributes the stops whose position was omitted (05.5c), as CSS: a missing first / last
/// position is 0 / 1, a run between two given ones is spaced evenly, then non-decreasing.
void distributeGradientStops(std::vector<GradientStop>& stops, const std::vector<bool>& hasPos) {
    const size_t n = stops.size();
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
        const size_t start = i - 1;  // Always known: index 0 was forced above.
        size_t end = i;
        while (end < n && !known[end]) {
            ++end;
        }
        const float from = stops[start].offset;
        const float to = stops[end].offset;
        for (size_t k = start + 1; k < end; ++k) {
            const float t = static_cast<float>(k - start) / static_cast<float>(end - start);
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

/// `<color> [<percent>]` stops, 2 to 16; a px position is rejected (a stop offset is a fraction).
bool parseGradientStops(const std::vector<std::vector<Tok>>& items, std::vector<GradientStop>& out) {
    if (items.size() < 2 || items.size() > kMaxGradientStops) {
        return false;
    }
    std::vector<bool> hasPos;
    for (const std::vector<Tok>& item : items) {
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
            const Tok& t = cur.peek();
            if (t.kind != TokKind::Number || (!iequals(t.unit, "%") && !(t.unit.empty() && t.num == 0))) {
                return false;
            }
            stop.offset = iequals(t.unit, "%") ? t.num / 100.0f : 0.0f;
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

}  // namespace

bool parseGradientValue(const Tok* toks, size_t count, Dialect d, ElementGradient& out) {
    if (count == 1 && isKeyword(d, toks[0], "none")) {
        out.kind = GradientKind::None;
        return true;
    }
    Cursor cur(toks, count);
    const Tok& fnTok = cur.peek();
    if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
        return false;
    }
    const bool linear = matches(d, fnTok.text, "linear-gradient");
    const bool radial = matches(d, fnTok.text, "radial-gradient");
    const bool conic = matches(d, fnTok.text, "conic-gradient");  // T-201.
    if (!linear && !radial && !conic) {
        return false;
    }
    cur.advance();
    std::vector<Tok> inner;
    if (!extractParenGroup(cur, inner) || !cur.atEnd()) {
        return false;
    }
    std::vector<std::vector<Tok>> items = splitByCommaTopLevel(inner.data(), inner.size());
    if (items.size() < 2) {
        return false;  // Needs the geometry group plus at least one stop.
    }
    Cursor head(items[0]);
    const auto readLen = [&](Length& l) {
        if (!parseGradientLen(head.peek(), d, l)) {
            return false;
        }
        head.advance();
        return true;
    };
    if (linear) {
        out.kind = GradientKind::Linear;
        if (!parseGradientAngle(head.peek(), out.angle)) {
            return false;
        }
        head.advance();
    } else if (conic) {
        // T-201 (05 §5.9.2): `[from] <angle> [at <len> <len>]`; the centre defaults to 50% 50%.
        out.kind = GradientKind::Conic;
        out.cx = Length{50, LengthUnit::Percent};
        out.cy = Length{50, LengthUnit::Percent};
        if (isKeyword(d, head.peek(), "from")) {
            head.advance();
        }
        if (!parseGradientAngle(head.peek(), out.angle)) {
            return false;
        }
        head.advance();
        if (isKeyword(d, head.peek(), "at")) {
            head.advance();
            if (!readLen(out.cx) || !readLen(out.cy)) {
                return false;
            }
        }
    } else {
        out.kind = GradientKind::Radial;
        if (!readLen(out.rx) || !readLen(out.ry) || !isKeyword(d, head.peek(), "at")) {
            return false;
        }
        head.advance();
        if (!readLen(out.cx) || !readLen(out.cy)) {
            return false;
        }
    }
    if (!head.atEnd()) {
        return false;
    }
    std::vector<std::vector<Tok>> stopItems(items.begin() + 1, items.end());
    return parseGradientStops(stopItems, out.stops);
}

// ---------------------------------------------------------------------------------------------
// clip-path.
// ---------------------------------------------------------------------------------------------

namespace {

bool parseClipLen(const Tok& tok, Dialect d, Length& out) {
    Length len;
    if (!parseLengthTok(tok, d, len) || (len.unit != LengthUnit::Px && len.unit != LengthUnit::Percent)) {
        return false;
    }
    out = len;
    return true;
}

bool parseClipRadius(const Tok& tok, Dialect d, Length& out) {
    if (tok.kind == TokKind::Ident) {
        if (matches(d, tok.text, "closest-side")) {
            out = Length{kClipRadiusClosestSide, LengthUnit::Auto};
            return true;
        }
        if (matches(d, tok.text, "farthest-side")) {
            out = Length{kClipRadiusFarthestSide, LengthUnit::Auto};
            return true;
        }
        return false;
    }
    return parseClipLen(tok, d, out);
}

/// `[at <x> <y>]`; the center defaults to 50% 50%.
bool parseClipCenter(Cursor& cur, Dialect d, Length& cx, Length& cy) {
    cx = Length{50, LengthUnit::Percent};
    cy = Length{50, LengthUnit::Percent};
    if (cur.atEnd()) {
        return true;
    }
    if (!isKeyword(d, cur.peek(), "at")) {
        return false;
    }
    cur.advance();
    if (!parseClipLen(cur.peek(), d, cx)) {
        return false;
    }
    cur.advance();
    if (!parseClipLen(cur.peek(), d, cy)) {
        return false;
    }
    cur.advance();
    return cur.atEnd();
}

/// 1-4 radii in border-radius order with its shorthand repetition; `out` receives 4 entries.
bool parseClipRadiiGroup(const std::vector<Tok>& toks, Dialect d, Length out[4]) {
    if (toks.empty() || toks.size() > 4) {
        return false;
    }
    Length v[4];
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!parseClipLen(toks[i], d, v[i])) {
            return false;
        }
    }
    expandSides(v, toks.size(), out);
    return true;
}

}  // namespace

bool parseClipPathValue(const Tok* toks, size_t count, Dialect d, ElementClipShape& out, bool& unsupported) {
    unsupported = false;
    out.kind = ClipShapeKind::None;
    out.params.clear();
    if (count == 1 && isKeyword(d, toks[0], "none")) {
        return true;
    }
    Cursor cur(toks, count);
    const Tok& fnTok = cur.peek();
    if (fnTok.kind != TokKind::Ident || cur.peek(1).kind != TokKind::LParen) {
        return false;
    }
    if (iequals(fnTok.text, "path") || iequals(fnTok.text, "url")) {
        unsupported = true;
        return false;
    }
    const bool inset = matches(d, fnTok.text, "inset");
    const bool circle = matches(d, fnTok.text, "circle");
    const bool ellipse = matches(d, fnTok.text, "ellipse");
    const bool polygon = matches(d, fnTok.text, "polygon");
    if (!inset && !circle && !ellipse && !polygon) {
        return false;
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
            if (section == 0 && isKeyword(d, t, "round")) {
                section = 1;
                continue;
            }
            if (section == 1 && isSlash(t)) {
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
            if (!parseClipLen(insets[i], d, e[i])) {
                return false;
            }
        }
        Length sides[4];
        expandSides(e, insets.size(), sides);
        Length rx[4] = {};
        Length ry[4] = {};
        if (section >= 1) {
            if (!parseClipRadiiGroup(horizontal, d, rx)) {
                return false;
            }
            if (section == 2) {
                if (!parseClipRadiiGroup(vertical, d, ry)) {
                    return false;
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    ry[i] = rx[i];
                }
            }
        }
        out.kind = ClipShapeKind::Inset;
        out.params = {sides[0], sides[1], sides[2], sides[3], rx[0], ry[0],
                      rx[1],    ry[1],    rx[2],    ry[2],    rx[3], ry[3]};
        return true;
    }
    Cursor in(inner);
    if (circle || ellipse) {
        // circle: `[<r>] [at <x> <y>]`; ellipse: `[<rx> <ry>] [at <x> <y>]`; radii default to
        // closest-side.
        Length rx{kClipRadiusClosestSide, LengthUnit::Auto};
        Length ry{kClipRadiusClosestSide, LengthUnit::Auto};
        if (!in.atEnd() && !isKeyword(d, in.peek(), "at")) {
            if (!parseClipRadius(in.peek(), d, rx)) {
                return false;
            }
            in.advance();
            if (ellipse) {
                if (!parseClipRadius(in.peek(), d, ry)) {
                    return false;
                }
                in.advance();
            }
        }
        Length cx, cy;
        if (!parseClipCenter(in, d, cx, cy)) {
            return false;
        }
        if (circle) {
            out.kind = ClipShapeKind::Circle;
            out.params = {rx, cx, cy};
        } else {
            out.kind = ClipShapeKind::Ellipse;
            out.params = {rx, ry, cx, cy};
        }
        return true;
    }
    // polygon: at least 3 `<x> <y>` vertices, filled even-odd (05 §5.9.8).
    std::vector<std::vector<Tok>> items = splitByCommaTopLevel(inner.data(), inner.size());
    if (items.size() < 3 || items.size() > kMaxClipPolygonVertices) {
        return false;
    }
    out.params.reserve(items.size() * 2);
    for (const std::vector<Tok>& item : items) {
        Length x, y;
        if (item.size() != 2 || !parseClipLen(item[0], d, x) || !parseClipLen(item[1], d, y)) {
            return false;
        }
        out.params.push_back(x);
        out.params.push_back(y);
    }
    out.kind = ClipShapeKind::Polygon;
    return true;
}

}  // namespace xt::css_values
