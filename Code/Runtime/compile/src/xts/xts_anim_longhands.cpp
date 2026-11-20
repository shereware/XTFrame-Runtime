#include "xts_anim_longhands.h"

#include <string>
#include <utility>

#include "xts_props.h"  // styleprop::Animation, the longhand StyleProps.
#include "xts_values_internal.h"

/// T-756 / T-778: one block's `animation` / `animation-*` declarations (xts_anim_longhands.h).

namespace xt::compile {

// Implemented in xts_values.cpp: the tokens re-serialized as `.xts` text.
std::string tokensToText(const std::vector<Token>& tokens);

namespace {

using xts_values_detail::kCompiler;
using xts_values_detail::SinkDiag;
using xts_values_detail::Tok;
using xts_values_detail::toValueTokens;

/// Copies longhand `field`'s part of `from` into `to` (css_values::AnimationLonghand order).
void copyAnimationField(int field, const AnimationSpec& from, AnimationSpec& to) {
    switch (field) {
        case 0:
            to.name = from.name;
            break;
        case 1:
            to.duration = from.duration;
            break;
        case 2:
            to.easing = from.easing;
            break;
        case 3:
            to.delay = from.delay;
            break;
        case 4:
            to.iterations = from.iterations;
            break;
        case 5:
            to.direction = from.direction;
            break;
        case 6:
            to.fill = from.fill;
            break;
        default:
            to.playState = from.playState;
            break;
    }
}

/// Longhand `longhand`'s comma-separated items, or false (after a warning) when one is malformed.
bool parseLonghandList(int longhand, const std::vector<Token>& value, std::string_view fileName, int line,
                       DiagnosticSink& sink, std::vector<EasingPoint>* easingPoints,
                       std::vector<AnimationSpec>& out) {
    const std::string name = css_values::animationLonghandName(longhand);
    const std::vector<Tok> toks = toValueTokens(value);
    out.clear();
    if (!toks.empty()) {
        SinkDiag diag(sink, fileName, line);
        for (const std::vector<Tok>& item : css_values::splitByCommaTopLevel(toks.data(), toks.size())) {
            css_values::AnimationItem parsed;
            parsed.easing = Easing::linear();
            if (!css_values::parseAnimationLonghandItem(longhand, item.data(), item.size(), kCompiler, &diag,
                                                        parsed, easingPoints)) {
                out.clear();
                break;
            }
            AnimationSpec spec;
            spec.name = std::string(parsed.name);
            spec.duration = parsed.duration;
            spec.delay = parsed.delay;
            spec.easing = parsed.easing;
            spec.iterations = parsed.infinite ? 0xFFFF : parsed.iterations;
            spec.direction = parsed.direction;
            spec.fill = parsed.fill;
            spec.playState = parsed.playState;
            if (longhand == static_cast<int>(css_values::AnimationLonghand::IterationCount) &&
                spec.iterations == 0) {
                sink.warn(std::string(fileName), "animation-iteration-count 0: the animation is not played",
                          line);
            }
            out.push_back(std::move(spec));
        }
    }
    if (out.empty()) {
        sink.warn(std::string(fileName), "invalid value for \"" + name + "\"", line);
        return false;
    }
    return true;
}

/// T-778: CSS's list combination of one block's eight lists, keeping every item (a `none` name and
/// an iteration count of 0 included: the runtime leaves them out after the cascade, so they keep
/// their place should another rule's longhand win one of the other lists).
std::vector<AnimationSpec> combineAllFields(const std::vector<AnimationSpec> (&fields)[8]) {
    std::vector<AnimationSpec> out;
    for (size_t i = 0; i < fields[0].size(); ++i) {
        AnimationSpec item;
        item.easing = Easing::linear();
        for (int f = 0; f < 8; ++f) {
            if (!fields[f].empty()) {
                copyAnimationField(f, fields[f][i % fields[f].size()], item);
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

/// T-778: a `var()` longhand, substituted once the tokens are known (style_emit.cpp
/// resolveVarDecls() re-parses it as a declaration list of its own).
StyleDecl varLonghandDecl(const AnimationPart& part) {
    StyleDecl d;
    d.prop = static_cast<uint16_t>(styleprop::AnimationName + part.longhand);
    d.value.kind = StyleValueKind::VarSubst;
    d.value.str =
        std::string(css_values::animationLonghandName(part.longhand)) + ": " + tokensToText(part.value);
    d.important = part.important;
    d.line = part.line;
    return d;
}

}  // namespace

void mergeAnimationLonghands(const std::vector<AnimationPart>& parts, std::vector<StyleDecl>& decls,
                             std::string_view fileName, DiagnosticSink& sink,
                             std::vector<EasingPoint>* easingPoints) {
    // The last valid shorthand sets all eight: the longhands before it change nothing.
    size_t first = 0;
    bool haveShorthand = false;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].longhand < 0 && parts[i].emitted > 0) {
            first = i + 1;
            haveShorthand = true;
        }
    }
    std::vector<AnimationSpec> fields[8];
    uint8_t mask = 0;
    const AnimationPart* varPart[8] = {};
    bool anyLonghand = false;
    for (size_t i = first; i < parts.size(); ++i) {
        const AnimationPart& part = parts[i];
        if (part.longhand < 0) {
            continue;  // An invalid shorthand: already reported, and CSS ignores it.
        }
        anyLonghand = true;
        const uint8_t bit = static_cast<uint8_t>(1u << part.longhand);
        if (containsVarFunction(part.value)) {
            varPart[part.longhand] = &part;
            mask = static_cast<uint8_t>(mask & ~bit);
            continue;
        }
        std::vector<AnimationSpec> items;
        if (parseLonghandList(part.longhand, part.value, fileName, part.line, sink, easingPoints, items)) {
            fields[part.longhand] = std::move(items);
            mask = static_cast<uint8_t>(mask | bit);
            varPart[part.longhand] = nullptr;
        }
    }
    if (!anyLonghand) {
        return;  // Only shorthands: the block stays as parsed.
    }
    const AnimationPart& last = parts.back();
    // The shorthand's StyleDecl: the block's last declaration on `animation`.
    size_t at = decls.size();
    for (size_t i = 0; i < decls.size(); ++i) {
        if (decls[i].prop == styleprop::Animation) {
            at = i;
        }
    }
    if (mask != 0 && haveShorthand && at < decls.size() &&
        decls[at].value.kind == StyleValueKind::AnimationList) {
        // The shorthand's lists, the later longhands changing theirs: one complete list.
        std::vector<AnimationSpec> all[8];
        for (int f = 0; f < 8; ++f) {
            all[f] = (mask & (1u << f)) != 0 ? fields[f] : decls[at].value.animations;
            if (all[f].empty()) {
                all[f].emplace_back();  // `animation: none`: one item, named `none`.
            }
        }
        decls[at].value.animations = combineAllFields(all);
        decls[at].important = last.important;
        decls[at].line = last.line;
    } else if (mask != 0) {
        // T-778: no shorthand to complete (or one whose value is a var()): the given longhands are
        // declarations of their own, which the cascade completes at run time.
        StyleDecl merged;
        merged.prop = styleprop::Animation;
        merged.important = last.important;
        merged.line = last.line;
        if (mask == css_values::kAllAnimationLonghands) {
            merged.value.kind = StyleValueKind::AnimationList;
            merged.value.animations = combineAllFields(fields);
        } else {
            merged.value.kind = StyleValueKind::AnimLonghands;
            merged.value.animationMask = mask;
            merged.value.animationFields.assign(std::make_move_iterator(std::begin(fields)),
                                                std::make_move_iterator(std::end(fields)));
        }
        decls.push_back(std::move(merged));
    }
    for (const AnimationPart* part : varPart) {
        if (part != nullptr) {
            decls.push_back(varLonghandDecl(*part));
        }
    }
}

bool longhandInitialDecl(uint16_t prop, StyleDecl& out) {
    if (style_decode::styleprop::isAnimationLonghand(prop)) {
        out.prop = styleprop::Animation;
        out.value = StyleValue{};
        out.value.kind = StyleValueKind::AnimLonghands;
        out.value.animationMask = static_cast<uint8_t>(1u << (prop - styleprop::AnimationName));
        out.value.animationFields.resize(8);
        return true;
    }
    if (style_decode::styleprop::isTransitionLonghand(prop)) {
        out.prop = styleprop::Transition;
        out.value = StyleValue{};
        out.value.kind = StyleValueKind::TransLonghands;
        out.value.transitionMask = static_cast<uint8_t>(1u << (prop - styleprop::TransitionProperty));
        out.value.transitionFields.resize(4);
        return true;
    }
    return false;
}

}  // namespace xt::compile
