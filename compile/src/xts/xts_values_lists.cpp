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

/// Per-property value parsing (split from xts_values.cpp by T-412): font-family, border-image,
/// background-image and the transition / animation lists.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace xts_values_detail {

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

}  // namespace xts_values_detail

namespace {

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

}  // namespace

namespace xts_values_detail {

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

/// T-386 (05 §5.9.11): `hit-slop: <length>{1,4}` (px / % / em, >= 0; margin order) -> T-405 (IR
/// minor 8): the four Length longhands HitSlopTop..HitSlopLeft (top, right, bottom, left).
bool parseHitSlop(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls) {
    if (toks.empty() || toks.size() > 4) {
        return false;
    }
    Length v[4]{};
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!parseLengthTok(toks[i], v[i]) || !(v[i].value >= 0.0f) || !std::isfinite(v[i].value) ||
            (v[i].unit != LengthUnit::Px && v[i].unit != LengthUnit::Percent &&
             v[i].unit != LengthUnit::Em)) {
            return false;
        }
    }
    const size_t n = toks.size();
    const Length top = v[0];
    const Length right = n >= 2 ? v[1] : top;
    const Length bottom = n >= 3 ? v[2] : top;
    const Length left = n == 4 ? v[3] : right;
    const Length sides[4] = {top, right, bottom, left};
    for (uint16_t i = 0; i < 4; ++i) {
        StyleDecl d;
        d.prop = static_cast<uint16_t>(styleprop::HitSlopTop + i);
        d.value.kind = StyleValueKind::Length;
        d.value.len = sides[i];
        d.line = line;
        decls.push_back(d);
    }
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

}  // namespace xts_values_detail

namespace {

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

}  // namespace

namespace xts_values_detail {

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

}  // namespace xts_values_detail

namespace {

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

}  // namespace

namespace xts_values_detail {

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

}  // namespace xts_values_detail

}  // namespace xt::compile
