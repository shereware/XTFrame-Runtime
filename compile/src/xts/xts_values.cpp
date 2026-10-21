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

/// Per-property value parsing and shorthand expansion. See docs/design/05-style-and-css.md
/// §5.3-5.6, docs/tasks/T-22.md.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace {

// -------------------------------------------------------------------------------------------
// Single (non-shorthand) value dispatch.
// -------------------------------------------------------------------------------------------

bool parseSingleValue(const PropInfo& info, const std::vector<Tok>& toks, StyleValue& out) {
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
                const Tok& t = toks[0];
                if (t.kind != TokKind::Number) {
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
            if (info.prop == xt::prop::Perspective && toks[0].kind == TokKind::Ident &&
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
            if (info.prop == styleprop::LineClamp && toks[0].kind == TokKind::Number) {
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
                if (toks[0].kind == TokKind::Number && toks[0].unit.empty()) {
                    int w = xt::toInt32Sat(toks[0].num);  // T-258b (D11): no UB past int.
                    if (w < 100 || w > 900 || w % 100 != 0) {
                        return false;
                    }
                    out.kind = StyleValueKind::Enum;
                    out.e = static_cast<uint32_t>(w);
                    return true;
                }
            }
            if (toks[0].kind != TokKind::Ident) {
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
                if (toks.size() != 1 || toks[0].kind != TokKind::Ident) {
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
            if (toks.size() == 1 && toks[0].kind == TokKind::String) {
                out.kind = StyleValueKind::String;
                out.str = toks[0].text;
                return true;
            }
            if (toks.size() == 1 && toks[0].kind == TokKind::Ident) {
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
                toks[0].kind == TokKind::Ident && toks[0].text == "none") {
                out.kind = StyleValueKind::FloatArray;
                out.floats.clear();
                return true;
            }
            for (const Tok& t : toks) {
                if (t.kind == TokKind::Comma && info.kind == StyleValueKind::FloatArray) {
                    continue;
                }
                if (t.kind != TokKind::Number || !(t.unit.empty() || t.unit == "px")) {
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

namespace xts_values_detail {

bool parseAndExpandDecl(const PropInfo* info, const std::vector<Tok>& tokens, std::string_view fileName,
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
        if (tokens[0].kind != TokKind::Ident || tokens[1].kind != TokKind::Ident ||
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
        const Tok* str = nullptr;
        if (tokens.size() == 1 && tokens[0].kind == TokKind::String) {
            str = &tokens[0];
        } else if (tokens.size() == 4 && tokens[0].kind == TokKind::Ident && tokens[0].text == "path" &&
                   tokens[1].kind == TokKind::LParen && tokens[2].kind == TokKind::String &&
                   tokens[3].kind == TokKind::RParen) {
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

namespace {

// -------------------------------------------------------------------------------------------
// T-170: design tokens (docs/design/05-style-and-css.md §5.9.2-5.9.3).
// -------------------------------------------------------------------------------------------

bool isVarOpen(const std::vector<Tok>& tokens, size_t i) {
    return i + 1 < tokens.size() && tokens[i].kind == TokKind::Ident && tokens[i].text == "var" &&
           tokens[i + 1].kind == TokKind::LParen;
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
bool splitWholeVar(const std::vector<Tok>& tokens, std::string& name, std::vector<Tok>& fallback) {
    if (tokens.size() < 4 || !isVarOpen(tokens, 0) || tokens[2].kind != TokKind::Ident ||
        tokens[2].text.size() < 3 || tokens[2].text.compare(0, 2, "--") != 0) {
        return false;
    }
    int depth = 0;
    size_t close = tokens.size();
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokKind::LParen) {
            ++depth;
        } else if (tokens[i].kind == TokKind::RParen && --depth == 0) {
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
    if (tokens[3].kind != TokKind::Comma || tokens.size() == 5) {
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

bool containsVarFunction(const std::vector<Tok>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (isVarOpen(tokens, i)) {
            return true;
        }
    }
    return false;
}

std::string tokensToText(const std::vector<Tok>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Tok& t = tokens[i];
        const bool glueLeft = t.kind == TokKind::RParen || t.kind == TokKind::Comma ||
                              (t.kind == TokKind::LParen && i > 0 && tokens[i - 1].kind == TokKind::Ident);
        const bool afterOpen = i > 0 && tokens[i - 1].kind == TokKind::LParen;
        if (!out.empty() && !glueLeft && !afterOpen) {
            out += ' ';
        }
        switch (t.kind) {
            case TokKind::Ident:
                out += t.text;
                break;
            case TokKind::Number:
                out += numberText(t.num);
                out += t.unit;
                break;
            case TokKind::String:
                out += '"';
                for (char c : t.text) {
                    if (c == '"' || c == '\\') {
                        out += '\\';
                    }
                    out += c;
                }
                out += '"';
                break;
            case TokKind::Hash:
                out += '#';
                out += t.text;
                break;
            case TokKind::LParen:
                out += '(';
                break;
            case TokKind::RParen:
                out += ')';
                break;
            case TokKind::Comma:
                out += ',';
                break;
            case TokKind::Colon:
                out += ':';
                break;
            case TokKind::Dot:
                out += '.';
                break;
            case TokKind::Star:
                out += '*';
                break;
            case TokKind::Gt:
                out += '>';
                break;
            case TokKind::Bang:
                out += '!';
                break;
            case TokKind::At:
                out += '@';
                break;
            case TokKind::Unknown: {
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

bool parseVarDecl(const PropInfo* info, const std::vector<Tok>& tokens, std::string_view fileName, int line,
                  DiagnosticSink& sink, std::vector<StyleDecl>& outDecls) {
    std::string name;
    std::vector<Tok> fallback;
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
            if (!parseAndExpandDecl(info, fallback, fileName, line, sink, parsed, nullptr)) {
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
        Tok zero;
        zero.kind = TokKind::Number;
        zero.num = 0;
        zero.unit = "px";
        DiagnosticSink scratch;
        std::vector<StyleDecl> shape;
        std::vector<StyleDecl> parsedFallback;
        if (parseAndExpandDecl(info, std::vector<Tok>{zero}, fileName, line, scratch, shape, nullptr) &&
            !shape.empty()) {
            if (!fallback.empty() &&
                !parseAndExpandDecl(info, fallback, fileName, line, sink, parsedFallback, nullptr)) {
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

}  // namespace xts_values_detail

bool parseValue(uint16_t prop, std::string_view text, StyleValue& out) {
    const PropInfo* info = findPropById(prop);
    if (info == nullptr) {
        return false;
    }
    Lexer lexer(text);
    std::vector<Token> lexed;
    for (Token t = lexer.next(); t.kind != TokenKind::End; t = lexer.next()) {
        if (t.kind == TokenKind::Unknown) {
            return false;
        }
        lexed.push_back(t);
    }
    const std::vector<Tok> toks = toValueTokens(lexed);
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
    std::vector<Token> lexed;
    for (Token t = lexer.next(); t.kind != TokenKind::End; t = lexer.next()) {
        if (t.kind == TokenKind::Unknown) {
            sink.error("<effect>", "invalid value for \"effect\": lexer error", 1);
            return Status::kInvalidArgument;
        }
        lexed.push_back(t);
    }
    const std::vector<Tok> toks = toValueTokens(lexed);
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
bool parseTimelineTime(const Token& lexed, float& seconds) {
    const std::vector<Token> one{lexed};
    const Tok tok = toValueTokens(one)[0];
    if (tok.kind != TokKind::Number) {
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
bool parseEffectParamValue(const std::vector<Token>& lexed, bool isColor, StyleValue& out) {
    const std::vector<Tok> tokens = toValueTokens(lexed);
    if (isColor) {
        Color c;
        if (!parseColorTokens(tokens, c)) {
            return false;
        }
        out.kind = StyleValueKind::Color;
        out.color = c;
        return true;
    }
    if (tokens.size() != 1 || tokens[0].kind != TokKind::Number) {
        return false;
    }
    const Tok& t = tokens[0];
    if (!(t.unit.empty() || t.unit == "px" || t.unit == "deg")) {
        return false;
    }
    out.kind = StyleValueKind::Float;
    out.f = t.num;
    return true;
}

// The lexer-token entry points xts_parser.cpp declares: one conversion, then the css_values-token
// versions above (T-413).

bool parseEasingValue(const std::vector<Token>& tokens, Easing& out, bool* stepsClamped,
                      std::vector<EasingPoint>* easingPoints) {
    const std::vector<Tok> toks = toValueTokens(tokens);
    Cursor cur(toks);
    return parseEasingCursor(cur, out, stepsClamped, easingPoints) && cur.atEnd();
}

bool parseAndExpandDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName,
                        int line, DiagnosticSink& sink, std::vector<StyleDecl>& outDecls,
                        std::vector<EasingPoint>* easingPoints) {
    return xts_values_detail::parseAndExpandDecl(info, toValueTokens(tokens), fileName, line, sink, outDecls,
                                                 easingPoints);
}

bool containsVarFunction(const std::vector<Token>& tokens) {
    return xts_values_detail::containsVarFunction(toValueTokens(tokens));
}

bool isCalcFunctionValue(const std::vector<Token>& tokens) {
    return xts_values_detail::isCalcFunctionValue(toValueTokens(tokens));
}

std::string tokensToText(const std::vector<Token>& tokens) {
    return xts_values_detail::tokensToText(toValueTokens(tokens));
}

bool parseVarDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName, int line,
                  DiagnosticSink& sink, std::vector<StyleDecl>& outDecls) {
    return xts_values_detail::parseVarDecl(info, toValueTokens(tokens), fileName, line, sink, outDecls);
}

}  // namespace xt::compile
