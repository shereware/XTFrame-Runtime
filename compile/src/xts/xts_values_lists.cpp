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

/// Per-property value parsing (split from xts_values.cpp by T-412): font-family, border-image,
/// background-image and the transition / animation lists.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace xts_values_detail {

// -------------------------------------------------------------------------------------------
// font-family.
// -------------------------------------------------------------------------------------------

bool parseFontFamilyList(const std::vector<Tok>& toks, std::vector<std::string>& out) {
    return css_values::parseFontFamilyList(toks.data(), toks.size(), out);
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
bool parseSliceValues(const std::vector<Tok>& toks, size_t& i, float out[4], bool& any) {
    float v[4] = {0, 0, 0, 0};
    size_t n = 0;
    while (i < toks.size() && n < 4 && toks[i].kind == TokKind::Number) {
        const Tok& t = toks[i];
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
bool parseBorderImageSlice(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls) {
    size_t i = 0;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    float v[4] = {nan, nan, nan, nan};
    bool any = false;
    if (i < toks.size() && toks[i].kind == TokKind::Ident && toks[i].text == "auto") {
        ++i;
    } else if (!parseSliceValues(toks, i, v, any) || !any) {
        return false;
    }
    bool fill = false;
    if (i < toks.size() && toks[i].kind == TokKind::Ident && toks[i].text == "fill") {
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

/// T-345 (05 §5.9.9): `ui-scale` -> one Color-encoded declaration (mode, min, max, 0).
bool parseUiScale(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls) {
    float v[3] = {0, 0, 0};
    if (!css_values::parseUiScale(toks.data(), toks.size(), kCompiler, v)) {
        return false;
    }
    appendPropValues({{styleprop::UiScale, Value::ofColor(Color{v[0], v[1], v[2], 0})}}, line, decls);
    return true;
}

/// T-386 (05 §5.9.11): `hit-slop` -> T-405 (IR minor 8): the four Length longhands
/// HitSlopTop..HitSlopLeft (top, right, bottom, left).
bool parseHitSlop(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls) {
    Length sides[4];
    if (!css_values::parseHitSlop(toks.data(), toks.size(), kCompiler, sides)) {
        return false;
    }
    for (uint16_t i = 0; i < 4; ++i) {
        appendPropValues({{static_cast<uint16_t>(styleprop::HitSlopTop + i),
                           Value::ofLength(sides[i].value, sides[i].unit)}},
                         line, decls);
    }
    return true;
}

/// The `border-image` shorthand: every longhand is emitted (unspecified parts take their initial
/// values, as a CSS shorthand resets them).
bool parseBorderImageShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls) {
    if (toks.empty()) {
        return false;
    }
    size_t i = 0;
    std::string source;
    if (toks[0].kind == TokKind::Ident && toks[0].text == "none") {
        source = "none";
        i = 1;
    } else if (toks[0].kind == TokKind::Ident && toks[0].text == "url" && toks.size() >= 3 &&
               toks[1].kind == TokKind::LParen) {
        size_t close = 2;
        while (close < toks.size() && toks[close].kind != TokKind::RParen) {
            ++close;
        }
        if (close >= toks.size()) {
            return false;
        }
        std::vector<Tok> url(toks.begin(), toks.begin() + static_cast<std::ptrdiff_t>(close) + 1);
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
        const Tok& t = toks[i];
        if (t.kind != TokKind::Ident) {
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

bool parseBackgroundImage(const std::vector<Tok>& toks, std::string& out) {
    if (toks.size() == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "none") {
        out = "none";
        return true;
    }
    if (toks.size() >= 3 && toks[0].kind == TokKind::Ident && toks[0].text == "url" &&
        toks[1].kind == TokKind::LParen && toks.back().kind == TokKind::RParen) {
        std::vector<Tok> inner(toks.begin() + 2, toks.end() - 1);
        if (inner.size() == 1 && inner[0].kind == TokKind::String) {
            out = inner[0].text;
            return true;
        }
        // Unquoted url(...): reconstruct simple dotted filenames (Ident (Dot Ident)*). '/' isn't
        // a lexer symbol, so an unquoted path containing '/' isn't supported -- quote it instead.
        std::string path;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (i % 2 == 0) {
                if (inner[i].kind != TokKind::Ident) {
                    return false;
                }
                path += inner[i].text;
            } else {
                if (inner[i].kind != TokKind::Dot) {
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
bool parseTransitionItem(const std::vector<Tok>& item, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<TransitionSpec>& out,
                         std::vector<EasingPoint>* easingPoints) {
    Cursor cur(item);
    const Tok& nameTok = cur.peek();
    if (nameTok.kind != TokKind::Ident) {
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
                      "unknown or non-animatable transition property \"" + std::string(nameTok.text) + "\"",
                      line);
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

bool parseTransitionList(const std::vector<Tok>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<TransitionSpec>& out,
                         std::vector<EasingPoint>* easingPoints) {
    if (toks.empty()) {
        return false;
    }
    for (const std::vector<Tok>& item : splitByCommaTopLevel(toks)) {
        parseTransitionItem(item, fileName, line, sink, out, easingPoints);
    }
    return true;
}

}  // namespace xts_values_detail

namespace {

/// One `animation` item through core's grammar (css_values); the compiler encodes `infinite` as
/// 0xFFFF and clamps a finite count to kMaxFiniteIterations with a warning.
bool parseAnimationItem(const std::vector<Tok>& item, std::string_view fileName, int line,
                        DiagnosticSink& sink, AnimationSpec& out, std::vector<EasingPoint>* easingPoints) {
    SinkDiag diag(sink, fileName, line);
    css_values::AnimationItem parsed;
    if (css_values::parseAnimationItem(item.data(), item.size(), kCompiler, &diag, parsed, easingPoints) !=
        css_values::AnimationItemResult::Ok) {
        return false;
    }
    out.name = std::string(parsed.name);
    out.duration = parsed.duration;
    out.delay = parsed.delay;
    out.easing = parsed.easing;
    out.iterations = parsed.infinite ? 0xFFFF : parsed.iterations;
    out.direction = parsed.direction;
    out.fill = parsed.fill;
    out.playState = parsed.playState;
    return true;
}

}  // namespace

namespace xts_values_detail {

bool parseAnimationList(const std::vector<Tok>& toks, std::string_view fileName, int line,
                        DiagnosticSink& sink, std::vector<AnimationSpec>& out,
                        std::vector<EasingPoint>* easingPoints) {
    if (toks.empty()) {
        return false;
    }
    if (toks.size() == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "none") {
        return true;
    }
    for (const std::vector<Tok>& item : splitByCommaTopLevel(toks)) {
        AnimationSpec spec;
        if (!parseAnimationItem(item, fileName, line, sink, spec, easingPoints)) {
            // T-413 (05 §5.3): a malformed item makes the whole value invalid, so the declaration
            // is dropped (the caller warns) instead of compiling as a shorter list -- or as
            // `animation: none` -- as the runtime parser already did.
            out.clear();
            return false;
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
