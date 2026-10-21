#include "style_emit.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <vector>

#include "calc_emit.h"
#include "compiled_assets.h"
#include "xt/asset_resolver.h"  // isExternalAssetName / isAtlasAssetName (T-150, T-199).
#include "xt/command_list.h"    // customParamSlots() (T-203).
#include "xt/compile/parse_xts.h"
#include "xt/compile/svg_path.h"  // T-218: scene-target `d`.
#include "xt/compile/utf8_path.h"
#include "xt/compile/xtfx.h"
#include "xt/node.h"        // EffectKind (18-effects.md §18.2).
#include "xt/property.h"    // effectParam()/prop::kEffectParam* (18-effects.md §18.2).
#include "xts/xts_props.h"  // styleprop::Effect (T-51a).

/// See docs/design/02-ir-format.md §2.6, §2.9, docs/design/12-compiler.md §12.3, docs/tasks/T-23.md,
/// docs/design/18-effects.md §18.4 (T-51a, `effect`'s STYL encoding and its `@keyframes` expansion).

namespace xt::compile {

namespace {

namespace fs = std::filesystem;

bool readFileBytesStyleEmit(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return static_cast<bool>(in) || in.eof();
}

IrEasing toIrEasing(const Easing& e) {
    if (e.kind == EasingKind::LinearPoints) {
        // T-335: RuleSet::easingPoints and StyleBuilder::easingPoints share the offsets.
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

/// One neutral keyframe (docs/design/12-compiler.md §12.3): value.type == Enum, e == 0xFFFFFFFF.
/// Its easing is irrelevant when it's the last keyframe on the track and Linear otherwise (no
/// `animation-timing-function` applies to a synthesized edge -- there is no stop here to read one
/// from; conservative choice, see report).
IrKeyframe makeNeutralKeyframe(float time) {
    IrKeyframe kf{};
    kf.time = time;
    kf.easing = toIrEasing(Easing::linear());
    irSetValueU32(kf.value, 0, 0xFFFFFFFFu);
    return kf;
}

// docs/design/18-effects.md §18.4's `@keyframes`/`effect` expansion (T-51a): a param slot per
// builtin EffectKind (18.3), excluding DropShadow's offsetX/offsetY (paramIndex 1/2), which
// emitEffectKeyframeTracks() replaces with the composite Vec2 rule (paramIndex 18) instead.
struct EffectParamSlot {
    uint32_t paramIndex;  ///< 18.2 paramIndex (1..8 float, 16/17 color).
    bool isColor;
};

std::vector<EffectParamSlot> effectParamSlots(EffectKind kind) {
    switch (kind) {
        case EffectKind::DropShadow:
            return {{3, false}, {4, false}, {5, false}, {16, true}};  // blur, spread, opacity, color0.
        case EffectKind::Glow:
            return {{1, false}, {2, false}, {3, false}, {16, true}};  // radius, intensity, inner, color0.
        case EffectKind::Blur:
            return {{1, false}, {2, false}};  // radiusX, radiusY.
        case EffectKind::ColorAdjust:
            return {{1, false}, {2, false}, {3, false}, {4, false}, {5, false}, {6, false}, {7, false}};
        case EffectKind::GradientMask:
            return {{1, false}, {2, false}, {3, false}, {4, false}, {5, false}, {6, false}};
        case EffectKind::Shine:
            return {{1, false}, {2, false}, {3, false}, {4, false}, {5, false}, {16, true}};
        case EffectKind::Outline:
            return {{1, false}, {2, false}, {3, false}, {16, true}};
        case EffectKind::BackdropBlur:
            return {{1, false}, {2, false}, {3, false}};  // radius, saturation, brightness.
        default:
            return {};
    }
}

// Last `effect` decl in `stop`, or nullptr if it has none ("later decl for the same prop in the
// same stop wins", 12.3, generalized to `effect`).
const StyleDecl* effectDeclOf(const KeyframeStop& stop) {
    const StyleDecl* found = nullptr;
    for (const StyleDecl& d : stop.decls) {
        if (d.prop == styleprop::Effect) {
            found = &d;
        }
    }
    return found;
}

bool sameEffectSequence(const std::vector<EffectDeclEntry>& a, const std::vector<EffectDeclEntry>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].customAsset != b[i].customAsset) {
            return false;
        }
    }
    return true;
}

// `@keyframes` for `effect` (18.4): expands into per-effect-slot, per-parameter Tracks
// (`prop = effectParam(effectIndex, paramIndex)`), one only for a parameter whose value differs
// across at least two stops ("全相同者不產生"). Adds nothing to `tb` (reports an error instead) if
// some stop of `kdef` has no `effect` decl at all, or if the effect sequences (kind, count, order)
// aren't identical across every stop that does -- 18.4: "效果無恆等", so unlike `transform` there
// is no identity value to fill a missing/mismatched stop with.
void emitEffectKeyframeTracks(const KeyframesDef& kdef, TimelineBuilder& tb, std::string_view fileName,
                              DiagnosticSink& sink) {
    std::map<float, const KeyframeStop*> stopByOffset;
    for (const KeyframeStop& stop : kdef.stops) {
        stopByOffset[stop.offset] = &stop;  // Later same-offset stop wins (12.3's generalized rule).
    }
    for (const auto& [offset, stopPtr] : stopByOffset) {
        if (effectDeclOf(*stopPtr) == nullptr) {
            int line = stopPtr->decls.empty() ? 0 : stopPtr->decls.front().line;
            sink.error(std::string(fileName), "effect keyframes must use the same effect sequence", line);
            return;
        }
    }
    const StyleDecl* refDecl = effectDeclOf(*stopByOffset.begin()->second);
    const std::vector<EffectDeclEntry>& refSeq = refDecl->value.effects;
    for (const auto& [offset, stopPtr] : stopByOffset) {
        const StyleDecl* d = effectDeclOf(*stopPtr);
        if (!sameEffectSequence(d->value.effects, refSeq)) {
            sink.error(std::string(fileName), "effect keyframes must use the same effect sequence", d->line);
            return;
        }
    }

    for (size_t i = 0; i < refSeq.size(); ++i) {
        EffectKind kind = refSeq[i].kind;

        if (kind == EffectKind::DropShadow) {
            bool differs = false;
            bool haveFirst = false;
            float first0 = 0, first1 = 0;
            for (const auto& [offset, stopPtr] : stopByOffset) {
                const float* p = effectDeclOf(*stopPtr)->value.effects[i].params;
                if (!haveFirst) {
                    first0 = p[0];
                    first1 = p[1];
                    haveFirst = true;
                } else if (p[0] != first0 || p[1] != first1) {
                    differs = true;
                }
            }
            if (differs) {
                IrTrack track{};
                track.target = kSelfNode;
                track.prop = xt::effectParam(static_cast<uint32_t>(i), xt::prop::kEffectParamOffset);
                track.type = static_cast<uint8_t>(ValueType::Vec2);
                track.kfOffset = static_cast<uint32_t>(tb.keyframes.size());
                for (const auto& [offset, stopPtr] : stopByOffset) {
                    const KeyframeStop& stop = *stopPtr;
                    const float* p = effectDeclOf(stop)->value.effects[i].params;
                    IrKeyframe kf{};
                    kf.time = offset / 100.0f;
                    kf.easing = toIrEasing(stop.hasTiming ? stop.timing : Easing::linear());
                    irSetValueF32(kf.value, 0, p[0]);
                    irSetValueF32(kf.value, 1, p[1]);
                    tb.keyframes.push_back(kf);
                }
                track.kfCount = static_cast<uint32_t>(tb.keyframes.size()) - track.kfOffset;
                tb.tracks.push_back(track);
            }
        }

        for (const EffectParamSlot& slot : effectParamSlots(kind)) {
            if (!slot.isColor) {
                size_t arrIdx = slot.paramIndex - 1;
                bool differs = false;
                bool haveFirst = false;
                float first = 0;
                for (const auto& [offset, stopPtr] : stopByOffset) {
                    float v = effectDeclOf(*stopPtr)->value.effects[i].params[arrIdx];
                    if (!haveFirst) {
                        first = v;
                        haveFirst = true;
                    } else if (v != first) {
                        differs = true;
                    }
                }
                if (!differs) {
                    continue;
                }
                IrTrack track{};
                track.target = kSelfNode;
                track.prop = xt::effectParam(static_cast<uint32_t>(i), slot.paramIndex);
                track.type = static_cast<uint8_t>(ValueType::Float);
                track.kfOffset = static_cast<uint32_t>(tb.keyframes.size());
                for (const auto& [offset, stopPtr] : stopByOffset) {
                    const KeyframeStop& stop = *stopPtr;
                    IrKeyframe kf{};
                    kf.time = offset / 100.0f;
                    kf.easing = toIrEasing(stop.hasTiming ? stop.timing : Easing::linear());
                    irSetValueF32(kf.value, 0, effectDeclOf(stop)->value.effects[i].params[arrIdx]);
                    tb.keyframes.push_back(kf);
                }
                track.kfCount = static_cast<uint32_t>(tb.keyframes.size()) - track.kfOffset;
                tb.tracks.push_back(track);
            } else {
                uint32_t colorIdx = slot.paramIndex == 16 ? 0 : 1;
                bool differs = false;
                bool haveFirst = false;
                Color first{};
                for (const auto& [offset, stopPtr] : stopByOffset) {
                    Color c = effectDeclOf(*stopPtr)->value.effects[i].colors[colorIdx];
                    if (!haveFirst) {
                        first = c;
                        haveFirst = true;
                    } else if (c != first) {
                        differs = true;
                    }
                }
                if (!differs) {
                    continue;
                }
                IrTrack track{};
                track.target = kSelfNode;
                track.prop = xt::effectParam(static_cast<uint32_t>(i), slot.paramIndex);
                track.type = static_cast<uint8_t>(ValueType::Color);
                track.kfOffset = static_cast<uint32_t>(tb.keyframes.size());
                for (const auto& [offset, stopPtr] : stopByOffset) {
                    const KeyframeStop& stop = *stopPtr;
                    Color c = effectDeclOf(stop)->value.effects[i].colors[colorIdx];
                    IrKeyframe kf{};
                    kf.time = offset / 100.0f;
                    kf.easing = toIrEasing(stop.hasTiming ? stop.timing : Easing::linear());
                    irSetValueF32(kf.value, 0, c.r);
                    irSetValueF32(kf.value, 1, c.g);
                    irSetValueF32(kf.value, 2, c.b);
                    irSetValueF32(kf.value, 3, c.a);
                    tb.keyframes.push_back(kf);
                }
                track.kfCount = static_cast<uint32_t>(tb.keyframes.size()) - track.kfOffset;
                tb.tracks.push_back(track);
            }
        }
    }
}

// -------------------------------------------------------------------------------------------
// T-170: design tokens.
// -------------------------------------------------------------------------------------------

bool isIdentChar(char c) { return c == '-' || c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0; }

std::string trimCopy(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

/// Replaces every `var(--name[, fallback])` in `text` by the token's text, else its (itself
/// substituted) fallback. `lookup(name, out)` supplies token text. False, with `missing` set to the
/// name, when a reference has neither; also false past a nesting depth of 16 (a cycle).
template <class Lookup>
bool substituteVars(std::string_view text, const Lookup& lookup, std::string& out, std::string& missing,
                    int depth) {
    if (depth > 16) {
        missing = "(cycle)";
        return false;
    }
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t at = text.find("var(", pos);
        while (at != std::string_view::npos && at > 0 && isIdentChar(text[at - 1])) {
            at = text.find("var(", at + 1);
        }
        if (at == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, at - pos));
        size_t open = at + 3;
        int parens = 0;
        size_t close = std::string_view::npos;
        size_t comma = std::string_view::npos;
        char quote = 0;
        for (size_t i = open; i < text.size(); ++i) {
            char c = text[i];
            if (quote != 0) {
                if (c == '\\') {
                    ++i;
                } else if (c == quote) {
                    quote = 0;
                }
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++parens;
            } else if (c == ')') {
                if (--parens == 0) {
                    close = i;
                    break;
                }
            } else if (c == ',' && parens == 1 && comma == std::string_view::npos) {
                comma = i;
            }
        }
        if (close == std::string_view::npos) {
            out.append(text.substr(at));  // Unbalanced: left alone; the re-parse reports it.
            break;
        }
        std::string_view inner = text.substr(open + 1, close - open - 1);
        std::string name =
            trimCopy(comma == std::string_view::npos ? inner : inner.substr(0, comma - open - 1));
        std::string replacement;
        std::string value;
        if (lookup(name, value)) {
            replacement = std::move(value);
        } else if (comma != std::string_view::npos) {
            std::string fallback = trimCopy(text.substr(comma + 1, close - comma - 1));
            if (!substituteVars(fallback, lookup, replacement, missing, depth + 1)) {
                return false;
            }
        } else {
            missing = name;
            return false;
        }
        out.append(replacement);
        pos = close + 1;
    }
    return true;
}

bool scopeLookup(const TokenScope& tokens, const std::string& name, std::string& out) {
    auto it = tokens.text.find(name);
    if (it == tokens.text.end()) {
        return false;
    }
    out = it->second;
    return true;
}

/// A token's value typed for `VARS`: Color, then a unitless number, then a non-`auto` length,
/// else its text (runtime re-parses it per property). Mirrors core's mini_css::classifyTokenText.
IrVar makeIrVar(IrWriter& writer, const std::string& name, const std::string& text) {
    IrVar var{};
    var.name = writer.intern(name);
    StyleValue v;
    if (parseValue(xt::prop::BackgroundColor, text, v) && v.kind == StyleValueKind::Color) {
        var.valueType = static_cast<uint8_t>(ValueType::Color);
        irSetValueF32(var.value, 0, v.color.r);
        irSetValueF32(var.value, 1, v.color.g);
        irSetValueF32(var.value, 2, v.color.b);
        irSetValueF32(var.value, 3, v.color.a);
        return var;
    }
    v = StyleValue{};
    if (parseValue(styleprop::ZIndex, text, v) && v.kind == StyleValueKind::Float) {
        var.valueType = static_cast<uint8_t>(ValueType::Float);
        irSetValueF32(var.value, 0, v.f);
        return var;
    }
    v = StyleValue{};
    if (parseValue(xt::prop::Width, text, v) && v.kind == StyleValueKind::Length &&
        v.len.unit != LengthUnit::Auto) {
        var.valueType = static_cast<uint8_t>(ValueType::Length);
        irSetValueF32(var.value, 0, v.len.value);
        var.value[4] = static_cast<uint8_t>(v.len.unit);
        return var;
    }
    var.valueType = static_cast<uint8_t>(ValueType::StringRef);
    irSetValueU32(var.value, 0, writer.intern(text));
    return var;
}

std::string propName(uint16_t prop) {
    const PropInfo* info = findPropById(prop);
    return info != nullptr ? std::string(info->cssName) : std::to_string(prop);
}

}  // namespace

void emitTokens(const RuleSet& ruleSet, IrWriter& writer, DiagnosticSink& sink, TokenScope& out) {
    std::map<std::string, const TokenDecl*> raw;
    for (const TokenDecl& token : ruleSet.tokens) {
        auto it = raw.find(token.name);
        if (it != raw.end()) {
            sink.warn(token.file,
                      "token \"" + token.name + "\" is declared more than once; the last declaration wins",
                      token.line);
            it->second = &token;
        } else {
            raw.emplace(token.name, &token);
        }
    }
    // Resolve token-to-token references against the raw table (depth-limited, so a cycle fails).
    std::function<bool(const std::string&, std::string&, int)> resolveOne;
    resolveOne = [&](const std::string& name, std::string& value, int depth) -> bool {
        auto it = raw.find(name);
        if (it == raw.end() || depth > 16) {
            return false;
        }
        std::string missing;
        auto lookup = [&](const std::string& inner, std::string& innerValue) {
            return resolveOne(inner, innerValue, depth + 1);
        };
        return substituteVars(it->second->value, lookup, value, missing, 0);
    };
    for (const auto& [name, decl] : raw) {
        std::string value;
        if (!resolveOne(name, value, 0)) {
            sink.warn(decl->file, "token \"" + name + "\" refers to an undefined token or to itself; ignored",
                      decl->line);
            continue;
        }
        out.text.emplace(name, value);
    }
    for (const auto& [name, value] : out.text) {
        writer.addVarToken(makeIrVar(writer, name, value));
    }
}

void resolveVarDecls(std::vector<StyleDecl>& decls, const TokenScope& tokens, bool keepRefs,
                     std::string_view fileName, DiagnosticSink& sink, const std::string& note) {
    const std::string file(fileName);
    auto lookup = [&tokens](const std::string& name, std::string& value) {
        return scopeLookup(tokens, name, value);
    };
    std::vector<StyleDecl> out;
    out.reserve(decls.size());
    for (StyleDecl& decl : decls) {
        if (decl.value.kind == StyleValueKind::Calc) {
            // T-176: calc() keeps its var() terms for the runtime lookup (themes can change
            // them); warn now about one that nothing defines.
            for (const CalcOpDecl& op : decl.value.calc) {
                std::string value;
                if (op.op == CalcOpKind::PushVar && !op.hasFallback && !lookup(op.varName, value)) {
                    sink.warn(file,
                              "var(" + op.varName +
                                  ") inside calc() is undefined and has no fallback; the declaration is "
                                  "ignored unless a theme defines it" +
                                  note,
                              decl.line);
                }
            }
        }
        if (decl.value.kind == StyleValueKind::VarSubst) {
            std::string text;
            std::string missing;
            if (!substituteVars(decl.value.str, lookup, text, missing, 0)) {
                const bool longhand = decl.prop != kShorthandProp;
                sink.warn(file,
                          "var(" + missing + ") is undefined and has no fallback; " +
                              (longhand ? "using the initial value" : "declaration ignored") + note,
                          decl.line);
                if (longhand) {
                    StyleDecl initial;
                    initial.prop = decl.prop;
                    initial.value = initialValue(decl.prop);
                    initial.important = decl.important;
                    initial.line = decl.line;
                    out.push_back(std::move(initial));
                }
                continue;
            }
            std::vector<StyleDecl> parsed;
            parseDeclarations(text, fileName, decl.line, sink, parsed);
            for (StyleDecl& p : parsed) {
                p.important = decl.important;
                out.push_back(std::move(p));
            }
            continue;
        }
        if (decl.value.kind != StyleValueKind::VarRef) {
            out.push_back(std::move(decl));
            continue;
        }
        std::string tokenText;
        const bool known = scopeLookup(tokens, decl.value.varName, tokenText);
        StyleValue tokenValue;
        const bool fits = known && parseValue(decl.prop, tokenText, tokenValue);
        if (!known && decl.value.varFallback.empty()) {
            sink.warn(file,
                      "var(" + decl.value.varName +
                          ") is undefined and has no fallback; using the initial value of \"" +
                          propName(decl.prop) + "\"" + note,
                      decl.line);
        } else if (known && !fits) {
            sink.warn(file,
                      "token \"" + decl.value.varName + "\" value \"" + tokenText + "\" is not a valid \"" +
                          propName(decl.prop) + "\" value",
                      decl.line);
        }
        if (keepRefs) {
            if (decl.value.varFallback.empty()) {
                decl.value.varFallback.push_back(initialValue(decl.prop));
            }
            out.push_back(std::move(decl));
            continue;
        }
        StyleDecl flat;
        flat.prop = decl.prop;
        flat.important = decl.important;
        flat.line = decl.line;
        if (fits) {
            flat.value = std::move(tokenValue);
        } else if (!decl.value.varFallback.empty()) {
            flat.value = decl.value.varFallback.front();
        } else {
            flat.value = initialValue(decl.prop);
        }
        out.push_back(std::move(flat));
    }
    decls = std::move(out);
}

void resolveRuleSetVars(RuleSet& ruleSet, const TokenScope& tokens, std::string_view fileName,
                        DiagnosticSink& sink) {
    for (StyleRule& rule : ruleSet.rules) {
        // T-235: a rule from a symbol library reports against that file, noting the instance.
        const std::string_view file = rule.file.empty() ? fileName : std::string_view(rule.file);
        const std::string note =
            rule.instanceSite.empty() ? std::string() : " (instance " + rule.instanceSite + ")";
        resolveVarDecls(rule.decls, tokens, /*keepRefs=*/true, file, sink, note);
    }
    for (KeyframesDef& keyframes : ruleSet.keyframes) {
        for (KeyframeStop& stop : keyframes.stops) {
            resolveVarDecls(stop.decls, tokens, /*keepRefs=*/false, fileName, sink);
        }
    }
}

/// px value of a translate keyframe declaration (a px Length or a bare number); false otherwise.
bool translatePx(const StyleValue& v, float& out) {
    if (v.kind == StyleValueKind::Float) {
        out = v.f;
        return true;
    }
    if (v.kind == StyleValueKind::Length && v.len.unit == xt::LengthUnit::Px) {
        out = v.len.value;
        return true;
    }
    return false;
}

/// T-228: when `kdef`'s stops carry motion handles and its TranslateX / TranslateY stops sit at the
/// same offsets in px, emits them as one Vec2 `Translate` track (04 §4.2: a segment's tanOut is
/// its start key's motion-out, tanIn the end key's motion-in; a segment with both handles zero
/// stays a plain lerp) and removes the two Float tracks from `propOrder`.
void emitMotionTranslateTrack(const KeyframesDef& kdef,
                              std::unordered_map<uint16_t, std::map<float, const StyleDecl*>>& perPropDecls,
                              std::unordered_map<uint16_t, std::map<float, Easing>>& perPropEasing,
                              std::vector<uint16_t>& propOrder, TimelineBuilder& tb,
                              std::string_view fileName, DiagnosticSink& sink) {
    std::map<float, std::pair<Vec2, Vec2>> motion;  // offset -> (in, out)
    int line = 0;
    for (const KeyframeStop& stop : kdef.stops) {
        if (stop.hasMotion) {
            motion[stop.offset] = {stop.motionIn, stop.motionOut};
            if (line == 0 && !stop.decls.empty()) {
                line = stop.decls.front().line;
            }
        }
    }
    if (motion.empty()) {
        return;
    }
    auto xs = perPropDecls.find(xt::prop::TranslateX);
    auto ys = perPropDecls.find(xt::prop::TranslateY);
    bool usable =
        xs != perPropDecls.end() && ys != perPropDecls.end() && xs->second.size() == ys->second.size();
    std::vector<std::pair<float, Vec2>> points;
    if (usable) {
        auto yi = ys->second.begin();
        for (auto xi = xs->second.begin(); usable && xi != xs->second.end(); ++xi, ++yi) {
            Vec2 p{};
            usable = xi->first == yi->first && translatePx(xi->second->value, p.x) &&
                     translatePx(yi->second->value, p.y);
            points.emplace_back(xi->first, p);
        }
    }
    if (!usable) {
        sink.warn(std::string(fileName),
                  "motion-in / motion-out need px translate keys on both axes at the same times; ignored",
                  line);
        return;
    }
    std::map<float, Easing>& easings = perPropEasing[xt::prop::TranslateX];
    IrTrack track{};
    track.target = kSelfNode;
    track.prop = xt::prop::Translate;
    track.type = static_cast<uint8_t>(ValueType::Vec2);
    track.kfOffset = static_cast<uint32_t>(tb.keyframes.size());
    if (points.front().first != 0.0f) {
        tb.keyframes.push_back(makeNeutralKeyframe(0.0f));
    }
    for (size_t i = 0; i < points.size(); ++i) {
        IrKeyframe kf{};
        kf.time = points[i].first / 100.0f;
        kf.easing = toIrEasing(easings[points[i].first]);
        irSetValueF32(kf.value, 0, points[i].second.x);
        irSetValueF32(kf.value, 1, points[i].second.y);
        Vec2 out{};
        Vec2 in{};
        if (auto m = motion.find(points[i].first); m != motion.end()) {
            out = m->second.second;
        }
        if (i + 1 < points.size()) {
            if (auto m = motion.find(points[i + 1].first); m != motion.end()) {
                in = m->second.first;
            }
        }
        if (out.x != 0.0f || out.y != 0.0f || in.x != 0.0f || in.y != 0.0f) {
            kf.hasSpatial = 1;
            kf.tanOut = Vec3{out.x, out.y, 0};
            kf.tanIn = Vec3{in.x, in.y, 0};
        }
        tb.keyframes.push_back(kf);
    }
    if (points.back().first != 100.0f) {
        tb.keyframes.push_back(makeNeutralKeyframe(1.0f));
    }
    track.kfCount = static_cast<uint32_t>(tb.keyframes.size()) - track.kfOffset;
    tb.tracks.push_back(track);
    propOrder.erase(
        std::remove_if(propOrder.begin(), propOrder.end(),
                       [](uint16_t p) { return p == xt::prop::TranslateX || p == xt::prop::TranslateY; }),
        propOrder.end());
}

void emitKeyframesTimelines(const RuleSet& ruleSet, IrWriter& writer, StyleBuilder& style,
                            std::string_view fileName, DiagnosticSink& sink,
                            std::unordered_map<std::string, TimelineId>& outKeyframesByName) {
    for (const KeyframesDef& kdef : ruleSet.keyframes) {
        TimelineBuilder tb;
        tb.name = writer.intern(kdef.name);
        tb.duration = 1.0f;
        tb.frameRate = 0.0f;

        // Per PropId: offset (0..100) -> the winning decl for that stop ("later decl for the same
        // prop in the same stop wins", 12.3) and the stop's easing. std::map keeps offsets sorted
        // ascending, which is also increasing time order (a keyframe's time must be strictly
        // increasing). A duplicate offset across two different stops resolves to the later stop,
        // consistent with the same-stop rule generalized (uncovered case, see report).
        std::vector<uint16_t> propOrder;
        std::unordered_map<uint16_t, std::map<float, const StyleDecl*>> perPropDecls;
        std::unordered_map<uint16_t, std::map<float, Easing>> perPropEasing;

        for (const KeyframeStop& stop : kdef.stops) {
            std::unordered_map<uint16_t, const StyleDecl*> lastInStop;
            for (const StyleDecl& d : stop.decls) {
                lastInStop[d.prop] = &d;
            }
            Easing stopEasing = stop.hasTiming ? stop.timing : Easing::linear();
            for (const auto& [propId, declPtr] : lastInStop) {
                if (perPropDecls.find(propId) == perPropDecls.end()) {
                    propOrder.push_back(propId);
                }
                perPropDecls[propId][stop.offset] = declPtr;
                perPropEasing[propId][stop.offset] = stopEasing;
            }
        }

        // T-228 (31 §31.7.2): a synthesized `@timeline` track whose translate keys carry
        // motion-in / motion-out becomes one Vec2 Translate track with spatial handles.
        emitMotionTranslateTrack(kdef, perPropDecls, perPropEasing, propOrder, tb, fileName, sink);

        // `effect` (18.4, T-51a): expands into per-effectParam Tracks instead of the generic
        // single-ListRef-value track the loop below would otherwise build; handled separately and
        // excluded from `propOrder`.
        if (std::find(propOrder.begin(), propOrder.end(), styleprop::Effect) != propOrder.end()) {
            propOrder.erase(std::remove(propOrder.begin(), propOrder.end(), styleprop::Effect),
                            propOrder.end());
            emitEffectKeyframeTracks(kdef, tb, fileName, sink);
        }

        for (uint16_t propId : propOrder) {
            std::map<float, const StyleDecl*>& stops = perPropDecls[propId];
            std::map<float, Easing>& easings = perPropEasing[propId];
            if (propId == xt::prop::Order) {
                // T-336 (05 §5.9.8): `order` is not animatable.
                sink.warn(std::string(fileName),
                          "@keyframes \"" + kdef.name + "\": \"order\" is not animatable; ignored",
                          stops.begin()->second->line);
                continue;
            }

            IrTrack track{};
            track.target = kSelfNode;
            // T-229 (31 §31.7.5): an animated `visibility` drives the node's Visible flag (PropId
            // 13, 1 = shown); the style property itself has no runtime track.
            const bool visibility = propId == styleprop::Visibility;
            track.prop = visibility ? xt::prop::Visible : propId;
            track.kfOffset = static_cast<uint32_t>(tb.keyframes.size());

            ValueType trackType = ValueType::Float;
            bool trackTypeSet = false;

            // T-211 (05 tail ruling): a translate track with any `%` stop stays Length-typed (px
            // stops as Length px, resolved against the element's own border box at the §6.7
            // fold); two adjacent stops must share the unit (the minimal subset of calc
            // interpolation), otherwise it is a compile error.
            const bool isTranslate = propId == xt::prop::TranslateX || propId == xt::prop::TranslateY;
            bool percentTranslate = false;
            if (isTranslate) {
                int prevUnit = -1;
                for (const auto& [offset, declPtr] : stops) {
                    const StyleValue& v = declPtr->value;
                    if (v.kind != StyleValueKind::Length ||
                        (v.len.unit != xt::LengthUnit::Px && v.len.unit != xt::LengthUnit::Percent)) {
                        continue;  // Skipped with a warning below.
                    }
                    const int unit = static_cast<int>(v.len.unit);
                    if (v.len.unit == xt::LengthUnit::Percent) {
                        percentTranslate = true;
                    }
                    if (prevUnit >= 0 && prevUnit != unit) {
                        sink.error(std::string(fileName),
                                   "@keyframes transform translate: adjacent keyframes must use the same "
                                   "unit (px or %)",
                                   declPtr->line);
                    }
                    prevUnit = unit;
                }
            }

            if (stops.find(0.0f) == stops.end()) {
                tb.keyframes.push_back(makeNeutralKeyframe(0.0f));
            }
            for (const auto& [offset, declPtr] : stops) {
                IrKeyframe kf{};
                kf.time = offset / 100.0f;
                kf.easing = toIrEasing(easings[offset]);
                ValueType vt = ValueType::Float;
                uint8_t bytes[16] = {};
                if (!encodeDeclValue(writer, style, outKeyframesByName, declPtr->value, fileName,
                                     declPtr->line, sink, bytes, vt)) {
                    // Only reachable for a stray `animation` (unresolved @keyframes name) inside a
                    // keyframe stop -- nonsensical input, not covered by T-23's grammar; skip the
                    // value defensively rather than corrupt the track (a diagnostic was already
                    // reported by encodeDeclValue).
                    continue;
                }
                if (isTranslate && vt == ValueType::Length) {
                    // Transform components are Float scene properties (03); a px-only keyframe track
                    // carries px floats. em/viewport units cannot be resolved at compile time and are
                    // rejected; `%` keeps the Length form (T-211, see above).
                    const auto unit = static_cast<xt::LengthUnit>(bytes[4]);
                    if (unit != xt::LengthUnit::Px && unit != xt::LengthUnit::Percent) {
                        sink.warn(std::string(fileName),
                                  "@keyframes transform translate must use px or %; stop skipped",
                                  declPtr->line);
                        continue;
                    }
                    if (!percentTranslate) {
                        vt = ValueType::Float;
                        bytes[4] = 0;
                    }
                }
                if (visibility && vt == ValueType::Enum) {
                    irSetValueU32(bytes, 0, irValueU32(bytes, 0) == 0 ? 1u : 0u);
                }
                std::memcpy(kf.value, bytes, sizeof(bytes));
                if (!trackTypeSet) {
                    trackType = vt;
                    trackTypeSet = true;
                }
                tb.keyframes.push_back(kf);
            }
            if (stops.find(100.0f) == stops.end()) {
                tb.keyframes.push_back(makeNeutralKeyframe(1.0f));
            }

            track.kfCount = static_cast<uint32_t>(tb.keyframes.size()) - track.kfOffset;
            track.type = static_cast<uint8_t>(trackType);
            tb.tracks.push_back(track);
        }

        TimelineId tid = writer.addTimeline(std::move(tb));
        outKeyframesByName.emplace(kdef.name, tid);

        IrKeyframesRef ref{};
        ref.name = writer.intern(kdef.name);
        ref.timeline = tid;
        style.keyframesRefs.push_back(ref);
    }
}

bool encodeDeclValue(IrWriter& writer, StyleBuilder& style,
                     const std::unordered_map<std::string, TimelineId>& keyframesByName,
                     const StyleValue& value, std::string_view fileName, int line, DiagnosticSink& sink,
                     uint8_t outBytes[16], ValueType& outType) {
    std::memset(outBytes, 0, 16);
    switch (value.kind) {
        case StyleValueKind::Float:
            outType = ValueType::Float;
            irSetValueF32(outBytes, 0, value.f);
            return true;
        case StyleValueKind::Length:
            outType = ValueType::Length;
            irSetValueF32(outBytes, 0, value.len.value);
            outBytes[4] = static_cast<uint8_t>(value.len.unit);
            return true;
        case StyleValueKind::Color:
            outType = ValueType::Color;
            irSetValueF32(outBytes, 0, value.color.r);
            irSetValueF32(outBytes, 1, value.color.g);
            irSetValueF32(outBytes, 2, value.color.b);
            irSetValueF32(outBytes, 3, value.color.a);
            return true;
        case StyleValueKind::Enum:
            outType = ValueType::Enum;
            irSetValueU32(outBytes, 0, value.e);
            return true;
        case StyleValueKind::String:
            outType = ValueType::StringRef;
            irSetValueU32(outBytes, 0, writer.intern(value.str));
            return true;
        case StyleValueKind::FloatArray:
            // T-236: a scene-target `stroke-dasharray`, a ListRef (offset, count) into STYL's
            // floatPool[] (count 0 = `none`).
            outType = ValueType::ListRef;
            irSetValueU32(outBytes, 0, static_cast<uint32_t>(style.floatPool.size()));
            irSetValueU32(outBytes, 1, static_cast<uint32_t>(value.floats.size()));
            style.floatPool.insert(style.floatPool.end(), value.floats.begin(), value.floats.end());
            return true;
        case StyleValueKind::Vec2:
            // T-236: a scene-target `repeat-anchor` / `repeat-translate` / `center`.
            outType = ValueType::Vec2;
            irSetValueF32(outBytes, 0, value.floats.size() > 0 ? value.floats[0] : 0.0f);
            irSetValueF32(outBytes, 1, value.floats.size() > 1 ? value.floats[1] : 0.0f);
            return true;
        case StyleValueKind::TransitionList: {
            outType = ValueType::ListRef;
            uint32_t offset = static_cast<uint32_t>(style.transitions.size());
            for (const TransitionSpec& t : value.transitions) {
                IrTransitionSpec spec{};
                spec.prop = t.prop;
                spec.duration = t.duration;
                spec.delay = t.delay;
                spec.easing = toIrEasing(t.easing);
                style.transitions.push_back(spec);
            }
            irSetValueU32(outBytes, 0, offset);
            irSetValueU32(outBytes, 1, static_cast<uint32_t>(value.transitions.size()));
            return true;
        }
        case StyleValueKind::AnimationList: {
            std::vector<IrAnimationSpec> specs;
            specs.reserve(value.animations.size());
            for (const AnimationSpec& a : value.animations) {
                auto it = keyframesByName.find(a.name);
                if (it == keyframesByName.end()) {
                    sink.error(std::string(fileName), "unknown @keyframes \"" + a.name + "\"", line);
                    return false;
                }
                IrAnimationSpec spec{};
                spec.timeline = it->second;
                spec.duration = a.duration;
                spec.delay = a.delay;
                spec.easing = toIrEasing(a.easing);
                // docs/design/02-ir-format.md §2.10 IrAnimationSpec: "0 = infinite", while the
                // style model uses 0xFFFF for it (style_model.h). Map here, or core would read
                // `infinite` as 65535 Once-iterations and finish the animation after the first
                // one (found by T-35's integration test).
                spec.iterations = a.iterations == 0xFFFFu ? 0 : a.iterations;
                spec.direction = a.direction;
                spec.fill = a.fill;
                spec.playState = a.playState;
                specs.push_back(spec);
            }
            outType = ValueType::ListRef;
            uint32_t offset = static_cast<uint32_t>(style.animations.size());
            for (const IrAnimationSpec& spec : specs) {
                style.animations.push_back(spec);
            }
            irSetValueU32(outBytes, 0, offset);
            irSetValueU32(outBytes, 1, static_cast<uint32_t>(specs.size()));
            return true;
        }
        case StyleValueKind::EffectList: {
            outType = ValueType::ListRef;
            uint32_t offset = static_cast<uint32_t>(style.effects.size());
            for (const EffectDeclEntry& e : value.effects) {
                IrEffect ir{};
                ir.kind = static_cast<uint8_t>(e.kind);
                ir.enabled = 1;
                ir.shader = kInvalidAsset;
                if (e.kind == EffectKind::Custom) {
                    // T-203: resolveCustomEffects() loaded the asset before any declaration is
                    // encoded (a reparsed inline style finds it by name).
                    ir.shader = e.shaderAsset != kInvalidAsset ? e.shaderAsset
                                                               : findShaderAsset(writer, e.customAsset);
                    if (ir.shader == kInvalidAsset) {
                        sink.error(std::string(fileName),
                                   "custom effect \"" + e.customAsset + "\" is not loaded", line);
                        return false;
                    }
                }
                std::memcpy(ir.params, e.params, sizeof(ir.params));
                ir.colors[0] = e.colors[0];
                ir.colors[1] = e.colors[1];
                ir.stopOffset = static_cast<uint32_t>(style.effectStops.size());
                ir.stopCount = static_cast<uint32_t>(e.stops.size());
                for (const GradientStop& s : e.stops) {
                    IrGradientStop gs{};
                    gs.offset = s.offset;
                    gs.color = s.color;
                    style.effectStops.push_back(gs);
                }
                style.effects.push_back(ir);
            }
            irSetValueU32(outBytes, 0, offset);
            irSetValueU32(outBytes, 1, static_cast<uint32_t>(value.effects.size()));
            return true;
        }
        case StyleValueKind::ClipShape: {
            // T-332 (05 §5.9.8): one `CLIP` chunk entry; the declaration is an Enum holding
            // 1 + its index, 0 for `none`.
            outType = ValueType::Enum;
            if (value.clipShape.kind == ClipShapeKind::None) {
                irSetValueU32(outBytes, 0, 0);
                return true;
            }
            irSetValueU32(outBytes, 0, writer.addClipShape(value.clipShape) + 1);
            return true;
        }
        case StyleValueKind::GridTracks:
            // T-338 (06 §6.15): one `GRID` track list; the declaration is an Enum holding 1 + its
            // index, 0 for `none`.
            outType = ValueType::Enum;
            irSetValueU32(outBytes, 0,
                          value.gridTracks.empty() ? 0u : writer.addGridTrackList(value.gridTracks) + 1);
            return true;
        case StyleValueKind::GridAreas: {
            // T-338: one `GRID` area list (names interned), same Enum convention.
            outType = ValueType::Enum;
            if (value.gridAreaRows == 0 || value.gridAreaCols == 0) {
                irSetValueU32(outBytes, 0, 0);
                return true;
            }
            std::vector<GridArea> areas;
            areas.reserve(value.gridAreas.size());
            for (const GridAreaDecl& a : value.gridAreas) {
                areas.push_back(GridArea{writer.intern(a.name), a.rowStart, a.rowEnd, a.colStart, a.colEnd});
            }
            irSetValueU32(outBytes, 0,
                          writer.addGridAreaList(areas, value.gridAreaRows, value.gridAreaCols) + 1);
            return true;
        }
        case StyleValueKind::Gradient: {
            // 05.5c (T-115): one `gradientPool[]` entry, its stops appended to the same
            // `effectStops[]` pool IrEffect uses. `none` emits a ListRef with count 0.
            outType = ValueType::ListRef;
            uint32_t offset = static_cast<uint32_t>(style.gradients.size());
            if (value.gradient.kind == GradientKind::None) {
                irSetValueU32(outBytes, 0, offset);
                irSetValueU32(outBytes, 1, 0);
                return true;
            }
            IrGradient ir{};
            ir.kind = static_cast<uint8_t>(value.gradient.kind);
            const Length lengths[4] = {value.gradient.rx, value.gradient.ry, value.gradient.cx,
                                       value.gradient.cy};
            if (value.gradient.kind == GradientKind::Linear) {
                ir.params[0] = value.gradient.angle;  // Degrees; `units[0]` unused (02.6).
            } else if (value.gradient.kind == GradientKind::Conic) {
                // T-201: the start angle, then the centre in params[2..3] like a radial's.
                ir.params[0] = value.gradient.angle;
                for (size_t i = 2; i < 4; ++i) {
                    ir.params[i] = lengths[i].value;
                    ir.units[i] = static_cast<uint8_t>(lengths[i].unit);
                }
            } else {
                for (size_t i = 0; i < 4; ++i) {
                    ir.params[i] = lengths[i].value;
                    ir.units[i] = static_cast<uint8_t>(lengths[i].unit);
                }
            }
            ir.stopOffset = static_cast<uint32_t>(style.effectStops.size());
            ir.stopCount = static_cast<uint32_t>(value.gradient.stops.size());
            for (const GradientStop& stop : value.gradient.stops) {
                IrGradientStop gs{};
                gs.offset = stop.offset;
                gs.color = stop.color;
                style.effectStops.push_back(gs);
            }
            style.gradients.push_back(ir);
            irSetValueU32(outBytes, 0, offset);
            irSetValueU32(outBytes, 1, 1);
            return true;
        }
        case StyleValueKind::FontFamilyList: {
            outType = ValueType::ListRef;
            uint32_t offset = static_cast<uint32_t>(style.fontFamilies.size());
            for (const std::string& fam : value.families) {
                style.fontFamilies.push_back(writer.intern(fam));
            }
            irSetValueU32(outBytes, 0, offset);
            irSetValueU32(outBytes, 1, static_cast<uint32_t>(value.families.size()));
            return true;
        }
        case StyleValueKind::Neutral:
            sink.error(std::string(fileName), "Neutral value is not allowed outside @keyframes", line);
            return false;
        case StyleValueKind::Shorthand:
            sink.error(std::string(fileName), "internal error: unexpanded shorthand value", line);
            return false;
        case StyleValueKind::VarRef:
        case StyleValueKind::VarSubst:
            // T-170: a VarRef is only encodable as a whole declaration (encodeDecl), and a
            // VarSubst never survives resolveVarDecls().
            sink.error(std::string(fileName), "internal error: unresolved var() value", line);
            return false;
        case StyleValueKind::Calc:
            // T-176: only a rule or `style` declaration (encodeDecl) can keep a calc() program; a
            // keyframe, `@timeline` key or var() fallback has no layout-time resolve.
            sink.error(std::string(fileName), "calc() is not supported here (only in style rules)", line);
            return false;
    }
    return false;
}

bool encodeDecl(IrWriter& writer, StyleBuilder& style,
                const std::unordered_map<std::string, TimelineId>& keyframesByName, const StyleDecl& decl,
                std::string_view fileName, DiagnosticSink& sink, IrDecl& out) {
    out = IrDecl{};
    if (decl.value.kind == StyleValueKind::VarRef) {
        // T-170 (02/12.7): name + fallback index into VARS `fallbacks[]`.
        uint32_t fallbackIndex = kNoVarFallback;
        if (!decl.value.varFallback.empty()) {
            IrDecl fallback{};
            ValueType fvt = ValueType::Float;
            if (!encodeDeclValue(writer, style, keyframesByName, decl.value.varFallback.front(), fileName,
                                 decl.line, sink, fallback.value, fvt)) {
                return false;
            }
            fallback.prop = decl.prop;
            fallback.valueType = static_cast<uint8_t>(fvt);
            fallbackIndex = writer.addVarFallback(fallback);
        }
        out.prop = decl.prop;
        out.valueType = kDeclVarRef;
        out.important = decl.important ? 1 : 0;
        irSetValueU32(out.value, 0, writer.intern(decl.value.varName));
        irSetValueU32(out.value, 1, fallbackIndex);
        return true;
    }
    if (decl.value.kind == StyleValueKind::Calc) {
        float folded = 0;
        if (foldCalcToPx(decl.value.calc, folded)) {
            // T-330: an all-px min() / max() / clamp() is a plain px length.
            StyleDecl constant;
            constant.prop = decl.prop;
            constant.value.kind = StyleValueKind::Length;
            constant.value.len = Length{folded, LengthUnit::Px};
            constant.important = decl.important;
            constant.line = decl.line;
            return encodeDecl(writer, style, keyframesByName, constant, fileName, sink, out);
        }
        // T-176: the program goes to CALC; the declaration keeps its index.
        out.prop = decl.prop;
        out.valueType = kDeclCalc;
        out.important = decl.important ? 1 : 0;
        irSetValueU32(out.value, 0, emitCalc(decl.value.calc, writer));
        return true;
    }
    const bool namesImage =
        (decl.prop == styleprop::BorderImageSource || decl.prop == styleprop::BackgroundImage) &&
        decl.value.kind == StyleValueKind::String && decl.value.str != "none";
    // T-273b: an asset referenced by its ASST name (CompileOptions::publishAssetNames) is encoded
    // as that name from the start, so its source name is never interned.
    const StyleValue* value = &decl.value;
    StyleValue published;
    if (namesImage) {
        for (const AssetBuilder& ab : writer.assets()) {
            if ((ab.kind == 0 || ab.kind == 3) && ab.sourceName == decl.value.str && ab.referencedByName) {
                published = decl.value;
                published.str = std::string(writer.stringAt(ab.name));
                value = &published;
                break;
            }
        }
    }
    ValueType vt = ValueType::Float;
    if (!encodeDeclValue(writer, style, keyframesByName, *value, fileName, decl.line, sink, out.value, vt)) {
        return false;
    }
    // T-199: an image-naming declaration carries the ASST entry's *final* name (after
    // `assetNameRewrite`: `assets/<hash>.png`, `atlas:<n>#...`), which is what the runtime looks
    // up (style_engine.cpp resolveBackgroundImageByName); the source name stays when no entry
    // was registered for it.
    if ((decl.prop == styleprop::BorderImageSource || decl.prop == styleprop::BackgroundImage) &&
        decl.value.kind == StyleValueKind::String && decl.value.str != "none") {
        const std::vector<AssetBuilder>& assets = writer.assets();
        for (const AssetBuilder& ab : assets) {
            if ((ab.kind == 0 || ab.kind == 3) && ab.sourceName == decl.value.str) {
                irSetValueU32(out.value, 0, ab.name);
                break;
            }
        }
    }
    out.prop = decl.prop;
    out.valueType = static_cast<uint8_t>(vt);
    out.important = decl.important ? 1 : 0;
    return true;
}

// T-218 (31 §31.5.1): a scene-target rule's `d` as a ListRef into STYL's floatPool[] -- points,
// then the verbs as floats (IrDecl::value: pointOffset, pointCount, verbOffset, verbCount). A `d`
// that does not parse is dropped with a warning.
static bool encodeScenePathDecl(StyleBuilder& style, const StyleDecl& decl, std::string_view fileName,
                                DiagnosticSink& sink, IrDecl& out) {
    SvgPath path;
    if (decl.value.kind != StyleValueKind::String || !parseSvgPathData(decl.value.str, path) ||
        path.verbs.empty()) {
        sink.warn(std::string(fileName), "malformed \"d\" in a style rule; ignored", decl.line);
        return false;
    }
    out = IrDecl{};
    out.prop = xt::prop::PathData;
    out.valueType = static_cast<uint8_t>(ValueType::ListRef);
    out.important = decl.important ? 1 : 0;
    irSetValueU32(out.value, 0, static_cast<uint32_t>(style.floatPool.size()));
    irSetValueU32(out.value, 1, static_cast<uint32_t>(path.points.size()));
    style.floatPool.insert(style.floatPool.end(), path.points.begin(), path.points.end());
    irSetValueU32(out.value, 2, static_cast<uint32_t>(style.floatPool.size()));
    irSetValueU32(out.value, 3, static_cast<uint32_t>(path.verbs.size()));
    for (uint8_t v : path.verbs) {
        style.floatPool.push_back(static_cast<float>(v));
    }
    return true;
}

// T-173: one SelectorCond as a kIrSelCondition part (xt/ir_pod.h's IrSelPart). SelectorCondKind's
// values are the IrSelCond codes.
static IrSelPart encodeSelectorCond(IrWriter& writer, const SelectorCond& cond) {
    static_assert(static_cast<uint8_t>(SelectorCondKind::Lang) == static_cast<uint8_t>(IrSelCond::Lang));
    IrSelPart p{};
    p.combinator = kIrSelCondition;
    p.classCount =
        static_cast<uint8_t>(static_cast<uint8_t>(cond.kind) | (cond.negated ? kIrSelCondNegated : 0));
    p.tag = kInvalidString;
    p.id = kInvalidString;
    p.classOffset = 0;
    switch (cond.kind) {
        case SelectorCondKind::Id:
            p.id = writer.intern(cond.name);
            break;
        case SelectorCondKind::State:
            p.stateMask = cond.state;
            break;
        case SelectorCondKind::Tag:
        case SelectorCondKind::Class:
        case SelectorCondKind::AttrExists:
        case SelectorCondKind::Lang:
        case SelectorCondKind::CustomState:  // T-174: the state name; STAT maps it to a bit.
            p.tag = writer.intern(cond.name);
            break;
        case SelectorCondKind::AttrEquals:
        case SelectorCondKind::AttrPrefix:
        case SelectorCondKind::AttrSuffix:
        case SelectorCondKind::AttrSubstring:
            p.tag = writer.intern(cond.name);
            p.id = writer.intern(cond.value);
            break;
        case SelectorCondKind::NthChild:
            p.tag = static_cast<uint32_t>(cond.a);
            p.id = static_cast<uint32_t>(cond.b);
            break;
        case SelectorCondKind::FocusPlayer:  // T-194: the player index, not a StringId.
            p.tag = static_cast<uint32_t>(cond.a);
            break;
        default:
            break;
    }
    return p;
}

namespace {

/// One path element against another; on Windows ignoring case (T-273c: std::towlower, as the
/// editor's copy of this test did).
bool samePathElement(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    const std::wstring x = a.native();
    const std::wstring y = b.native();
    if (x.size() != y.size()) {
        return false;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        if (std::towlower(x[i]) != std::towlower(y[i])) {
            return false;
        }
    }
    return true;
#else
    return a == b;
#endif
}

/// T-273b: the file name of `src` (after its last `/` or `\`).
std::string fileNameOf(std::string_view src) {
    const size_t slash = src.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? src : src.substr(slash + 1));
}

/// T-273b: `src`'s file name, or `<stem>-2<ext>`, `<stem>-3<ext>`... when an ASST entry of
/// `writer` already carries that name.
std::string freeFileName(const IrWriter& writer, std::string_view src) {
    const std::string file = fileNameOf(src);
    const size_t dot = file.find_last_of('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
    const std::string ext = dot == std::string::npos || dot == 0 ? std::string() : file.substr(dot);
    auto taken = [&writer](const std::string& name) {
        for (const AssetBuilder& ab : writer.assets()) {
            if (writer.stringAt(ab.name) == name) {
                return true;
            }
        }
        return false;
    };
    std::string name = file;
    for (int n = 2; taken(name); ++n) {
        name = stem + "-" + std::to_string(n) + ext;
    }
    return name;
}

/// T-273b (`CompileOptions::publishAssetNames`): the name the asset `src` gets in ASST, given the
/// name it would get otherwise (`assetName`), and whether the document then references it by that
/// name. An asset inside the project, or any asset when not publishing, keeps `assetName`.
std::string publishedAssetName(const IrWriter& writer, const std::string& src, std::string assetName,
                               bool embedded, std::string_view xtDir, const CompileOptions& options,
                               bool& referencedByName) {
    referencedByName = false;
    if (!options.publishAssetNames || !assetLeavesProject(src, xtDir, options.rootDir)) {
        return assetName;
    }
    // T-273c: a file the rewrite could not find (it left the name as written) gets its file name
    // too, so its machine path stays out of the `.xtir`; the "not found" warning is unchanged.
    if (embedded || assetName == src) {
        assetName = freeFileName(writer, src);
    }
    referencedByName = assetName != src;
    return assetName;
}

}  // namespace

bool isMachinePath(std::string_view src) {
    if (src.size() >= 2 && ((src[0] == '/' && src[1] == '/') || (src[0] == '\\' && src[1] == '\\'))) {
        return true;
    }
    if (src.size() >= 2 && src[1] == ':' &&
        ((src[0] >= 'A' && src[0] <= 'Z') || (src[0] >= 'a' && src[0] <= 'z'))) {
        return true;
    }
    return pathFromUtf8(src).has_root_path();
}

bool assetLeavesProject(std::string_view src, std::string_view xtDir, std::string_view rootDir) {
    if (src.empty() || isExternalAssetName(src)) {
        return false;
    }
    if (isMachinePath(src)) {
        return true;
    }
    const fs::path base = pathFromUtf8(xtDir.empty() ? rootDir : xtDir).lexically_normal();
    const fs::path root = rootDir.empty() ? base : pathFromUtf8(rootDir).lexically_normal();
    return !pathInsideRoot(base / pathFromUtf8(src), root);
}

bool pathInsideRoot(const fs::path& path, const fs::path& root) {
    const fs::path normalPath = path.lexically_normal();
    auto p = normalPath.begin();
    for (const fs::path& element : root.lexically_normal()) {
        if (element.empty() || element == ".") {
            continue;  // A trailing separator, or a root of ".".
        }
        if (p == normalPath.end() || !samePathElement(*p, element)) {
            return false;
        }
        ++p;
    }
    return p == normalPath.end() || *p != "..";
}

fs::path referencePath(std::string_view ref, const fs::path& baseDir) {
    const fs::path p = pathFromUtf8(ref);
    return (isMachinePath(ref) ? p : baseDir / p).lexically_normal();
}

// T-273d: an outside `<anim src>` or `<img src="*.svg">` is only renamed here, never copied into
// the bundle, and that is deliberate: its content is compiled into the `.xtir` (a scene) at pack
// time, so the run time never reads that file again and the name is only a label. Do not "fix"
// this by packing the file.
std::string publishedReference(const std::string& src, std::string_view xtDir,
                               const CompileOptions& options) {
    if (!options.publishAssetNames || !assetLeavesProject(src, xtDir, options.rootDir)) {
        return src;
    }
    std::string_view rest = src;
    while (rest.size() > 1 && (rest.back() == '/' || rest.back() == '\\')) {
        rest.remove_suffix(1);  // A folder written as `frames/`.
    }
    return fileNameOf(rest);
}

AssetId ensureImageAsset(IrWriter& writer, const std::string& src, std::string_view xtDir,
                         std::string_view fileName, int line, int col, const CompileOptions& options,
                         DiagnosticSink& sink) {
    const std::vector<AssetBuilder>& existing = writer.assets();
    for (size_t i = 0; i < existing.size(); ++i) {
        const AssetBuilder& ab = existing[i];
        if ((ab.kind == 0 || ab.kind == 3) &&
            (ab.sourceName == src || (ab.sourceName.empty() && writer.stringAt(ab.name) == src))) {
            return static_cast<AssetId>(i);
        }
    }
    AssetBuilder ab;
    ab.kind = 0;  // Image.
    if (isExternalAssetName(src)) {
        // T-150: an `engine://<name>` external texture. The name is the ASST entry as written:
        // no file lookup, no embedding, no warning, no `assetNameRewrite` (so a bundle never
        // packs or lists it); the host binds it at runtime.
        ab.sourceName = src;
        ab.name = writer.intern(src);
        return writer.addAsset(std::move(ab));
    }
    fs::path full = resolveAsset(options, xtDir, src);
    bool embedded = false;
    if (options.embedImages) {
        std::vector<uint8_t> bytes;
        if (readFileBytesStyleEmit(full, bytes)) {
            ab.embedded = std::move(bytes);
            embedded = true;
        } else {
            sink.warn(std::string(fileName), "failed to embed image asset \"" + src + "\"; kept external",
                      line, col);
        }
    }
    std::error_code ec;
    bool onDisk = fs::exists(full, ec);
    if (!embedded && !onDisk) {
        sink.warn(std::string(fileName), "image asset \"" + src + "\" not found", line, col);
    }
    ab.sourceName = src;
    ab.resolvedPath = onDisk ? pathToUtf8(full) : std::string();
    // An embedded asset carries its bytes, so its name stays as written (T-91).
    std::string assetName = src;
    if (!embedded && options.assetNameRewrite) {
        std::string rewritten = options.assetNameRewrite(src, ab.resolvedPath, ab.kind);
        if (!rewritten.empty()) {
            assetName = std::move(rewritten);
        }
    }
    assetName = publishedAssetName(writer, src, std::move(assetName), embedded, xtDir, options,
                                   ab.referencedByName);  // T-273b
    if (isAtlasAssetName(assetName)) {
        ab.kind = 3;  // T-199: AtlasImage (docs/design/02-ir-format.md §2.5).
    }
    ab.name = writer.intern(assetName);
    return writer.addAsset(std::move(ab));
}

namespace {

/// The IrEffect of a parsed built-in effect function without stops (an `.xtfx` `fallback:`).
IrEffect toFallbackIrEffect(const EffectDeclEntry& e) {
    IrEffect ir{};
    ir.kind = static_cast<uint8_t>(e.kind);
    ir.enabled = e.kind == EffectKind::None ? 0 : 1;
    ir.shader = kInvalidAsset;
    std::memcpy(ir.params, e.params, sizeof(ir.params));
    ir.colors[0] = e.colors[0];
    ir.colors[1] = e.colors[1];
    return ir;
}

/// The name a `custom()` parameter type is written as, for diagnostics.
const char* xtfxTypeName(uint8_t type) { return type == 0 ? "float" : (type == 1 ? "vec2" : "color"); }

}  // namespace

std::vector<XtfxParam> xtfxParamsOf(const ShaderBuilder& shader) {
    std::vector<XtfxParam> out;
    std::vector<uint8_t> types;
    for (size_t i = 0; i < shader.params.size(); ++i) {
        const IrShaderParam& ip = shader.params[i];
        XtfxParam p;
        p.name = i < shader.paramNames.size() ? shader.paramNames[i] : std::string();
        p.type = ip.type;
        std::memcpy(p.def, ip.defaultValue, sizeof(p.def));
        p.min = ip.minValue;
        p.max = ip.maxValue;
        p.hasRange = ip.minValue != 0.0f || ip.maxValue != 0.0f;
        out.push_back(std::move(p));
        types.push_back(ip.type);
    }
    std::vector<uint8_t> slots(types.size());
    if (customParamSlots(types.data(), types.size(), slots.data())) {
        for (size_t i = 0; i < slots.size(); ++i) {
            out[i].slot = slots[i];
        }
    }
    return out;
}

AssetId findShaderAsset(const IrWriter& writer, std::string_view src) {
    const std::vector<AssetBuilder>& assets = writer.assets();
    for (size_t i = 0; i < assets.size(); ++i) {
        if (assets[i].kind == 2 && assets[i].sourceName == src) {
            return writer.shaderForAsset(static_cast<AssetId>(i)) != nullptr ? static_cast<AssetId>(i)
                                                                             : kInvalidAsset;
        }
    }
    return kInvalidAsset;
}

AssetId ensureShaderAsset(IrWriter& writer, const std::string& src, std::string_view xtDir,
                          std::string_view fileName, int line, const CompileOptions& options,
                          DiagnosticSink& sink) {
    const std::vector<AssetBuilder>& existing = writer.assets();
    for (size_t i = 0; i < existing.size(); ++i) {
        if (existing[i].kind == 2 && existing[i].sourceName == src) {
            // Loaded before (or failed before: that error was reported once already).
            return writer.shaderForAsset(static_cast<AssetId>(i)) != nullptr ? static_cast<AssetId>(i)
                                                                             : kInvalidAsset;
        }
    }
    fs::path full = resolveAsset(options, xtDir, src);
    std::vector<uint8_t> bytes;
    AssetBuilder ab;
    ab.kind = 2;  // Shader (docs/design/02-ir-format.md §2.5).
    ab.sourceName = src;
    std::error_code ec;
    const bool onDisk = fs::is_regular_file(full, ec) && readFileBytesStyleEmit(full, bytes);
    ab.resolvedPath = onDisk ? pathToUtf8(full) : std::string();
    std::string assetName = src;
    if (onDisk && options.assetNameRewrite) {
        std::string rewritten = options.assetNameRewrite(src, ab.resolvedPath, ab.kind);
        if (!rewritten.empty()) {
            assetName = std::move(rewritten);
        }
    }
    ab.name = writer.intern(assetName);
    const AssetId id = writer.addAsset(std::move(ab));
    if (!onDisk) {
        sink.error(std::string(fileName), "custom effect asset \"" + src + "\" not found", line);
        return kInvalidAsset;
    }
    XtfxResult r = compileXtfx(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!r.ok) {
        sink.error(pathToUtf8(full), r.error.message, r.error.line, r.error.col);
        return kInvalidAsset;
    }
    if (r.header.fallback.kind == EffectKind::GradientMask ||
        r.header.fallback.kind == EffectKind::BackdropBlur) {
        sink.error(pathToUtf8(full), "fallback: gradient-mask and backdrop-blur cannot be a fallback");
        return kInvalidAsset;
    }
    ShaderBuilder sb;
    sb.asset = id;
    sb.source = std::move(r.source);
    sb.glsl = std::move(r.glsl);
    for (const XtfxParam& p : r.header.params) {
        IrShaderParam ip{};
        ip.type = p.type;
        std::memcpy(ip.defaultValue, p.def, sizeof(ip.defaultValue));
        ip.minValue = p.hasRange ? p.min : 0.0f;
        ip.maxValue = p.hasRange ? p.max : 0.0f;
        sb.params.push_back(ip);
        sb.paramNames.push_back(p.name);
    }
    sb.expandPx = r.header.expandPx;
    sb.fallback = toFallbackIrEffect(r.header.fallback);
    sb.name = !r.header.name.empty() ? r.header.name : pathToUtf8(full.stem());
    writer.addShader(std::move(sb));
    return id;
}

bool resolveCustomEffects(std::vector<StyleDecl>& decls, IrWriter& writer, std::string_view xtDir,
                          std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink) {
    bool ok = true;
    for (size_t d = 0; d < decls.size();) {
        StyleDecl& decl = decls[d];
        bool declOk = true;
        if (decl.value.kind == StyleValueKind::EffectList) {
            for (EffectDeclEntry& e : decl.value.effects) {
                if (e.kind != EffectKind::Custom || e.shaderAsset != kInvalidAsset) {
                    continue;
                }
                const AssetId id =
                    ensureShaderAsset(writer, e.customAsset, xtDir, fileName, decl.line, options, sink);
                if (id == kInvalidAsset) {
                    declOk = false;
                    break;
                }
                const std::vector<XtfxParam> params = xtfxParamsOf(*writer.shaderForAsset(id));
                float values[8] = {};
                float colors[2][4] = {};
                applyXtfxDefaults(params, values, colors);
                for (const CustomParamOverride& ov : e.customParams) {
                    const XtfxParam* p = nullptr;
                    for (const XtfxParam& candidate : params) {
                        if (candidate.name == ov.name) {
                            p = &candidate;
                        }
                    }
                    if (p == nullptr) {
                        sink.error(std::string(fileName),
                                   "custom(\"" + e.customAsset + "\"): unknown parameter '" + ov.name + "'",
                                   ov.line);
                        declOk = false;
                        break;
                    }
                    const bool fits =
                        p->type == 2 ? ov.isColor : (!ov.isColor && ov.count == (p->type == 1 ? 2 : 1));
                    if (!fits) {
                        sink.error(std::string(fileName),
                                   "custom(\"" + e.customAsset + "\"): parameter '" + ov.name + "' takes a " +
                                       xtfxTypeName(p->type),
                                   ov.line);
                        declOk = false;
                        break;
                    }
                    if (p->type == 2) {
                        std::memcpy(colors[p->slot], ov.value, sizeof(colors[p->slot]));
                    } else {
                        values[p->slot] = ov.value[0];
                        if (p->type == 1) {
                            values[p->slot + 1] = ov.value[1];
                        }
                    }
                }
                if (!declOk) {
                    break;
                }
                std::memcpy(e.params, values, sizeof(e.params));
                for (int c = 0; c < 2; ++c) {
                    e.colors[c] = Color{colors[c][0], colors[c][1], colors[c][2], colors[c][3]};
                }
                e.shaderAsset = id;
            }
        }
        if (!declOk) {
            ok = false;
            decls.erase(decls.begin() + static_cast<std::ptrdiff_t>(d));
            continue;
        }
        ++d;
    }
    return ok;
}

bool resolveCustomEffects(RuleSet& ruleSet, IrWriter& writer, std::string_view xtDir,
                          std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink) {
    bool ok = true;
    for (StyleRule& rule : ruleSet.rules) {
        ok &= resolveCustomEffects(rule.decls, writer, xtDir, rule.file.empty() ? fileName : rule.file,
                                   options, sink);
    }
    for (KeyframesDef& kf : ruleSet.keyframes) {
        for (KeyframeStop& stop : kf.stops) {
            ok &= resolveCustomEffects(stop.decls, writer, xtDir, fileName, options, sink);
        }
    }
    for (TimelineDef& tl : ruleSet.timelines) {
        for (TimelineTrackDef& track : tl.tracks) {
            for (TimelineKey& key : track.keys) {
                ok &= resolveCustomEffects(key.decls, writer, xtDir,
                                           tl.fileName.empty() ? fileName : tl.fileName, options, sink);
            }
        }
    }
    return ok;
}

bool emitStyle(const RuleSet& ruleSet, IrWriter& writer, StyleBuilder& style,
               const std::unordered_map<std::string, TimelineId>& keyframesByName, std::string_view xtDir,
               std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink) {
    bool ok = true;
    std::vector<IrMediaRule> mediaRules;  // T-176: STYL rule -> `@media` block.

    // T-199: `border-image-source: url(x)` names an image asset the runtime resolves by name
    // (style_engine.cpp resolveBackgroundImageByName), so it needs its ASST entry.
    // `background-image` gets the same treatment (its ASST entry used to depend on an `<img>` of
    // the same name), and encodeDecl() below writes the entry's final name into the declaration.
    for (const StyleRule& rule : ruleSet.rules) {
        for (const StyleDecl& decl : rule.decls) {
            if ((decl.prop == styleprop::BorderImageSource || decl.prop == styleprop::BackgroundImage) &&
                decl.value.kind == StyleValueKind::String && decl.value.str != "none") {
                ensureImageAsset(writer, decl.value.str, xtDir, fileName, decl.line, 0, options, sink);
            }
        }
    }

    for (const StyleRule& rule : ruleSet.rules) {
        // Group selector expansion (`a, b { ... }`): one IrRule per Selector, sharing the same decl
        // range; source order = the expanded index in `style.rules`.
        for (const Selector& sel : rule.selectors) {
            uint32_t selOffset = static_cast<uint32_t>(style.selectorParts.size());
            for (const SelectorPart& part : sel.parts) {
                IrSelPart p{};
                p.combinator = part.combinator;
                p.classCount = static_cast<uint8_t>(part.classes.size());
                p.stateMask = part.stateMask;
                p.tag = part.tag.empty() ? kInvalidString : writer.intern(part.tag);
                p.id = part.id.empty() ? kInvalidString : writer.intern(part.id);
                p.classOffset = static_cast<uint32_t>(style.classIds.size());
                for (const std::string& c : part.classes) {
                    style.classIds.push_back(writer.intern(c));
                }
                style.selectorParts.push_back(p);
                // T-218: a scene target follows the subject compound, before its conditions.
                if (part.sceneKind != SceneTargetKind::None) {
                    IrSelPart target{};
                    target.combinator = kIrSelSceneTarget;
                    target.classCount = static_cast<uint8_t>(static_cast<uint8_t>(part.sceneKind) - 1u);
                    target.tag = part.sceneTarget.empty() ? kInvalidString : writer.intern(part.sceneTarget);
                    target.id = kInvalidString;
                    style.selectorParts.push_back(target);
                }
                // T-173: each extra condition follows its compound as a kIrSelCondition part.
                for (const SelectorCond& cond : part.conds) {
                    style.selectorParts.push_back(encodeSelectorCond(writer, cond));
                }
            }

            uint32_t declOffset = static_cast<uint32_t>(style.decls.size());
            uint32_t declCount = 0;
            const bool sceneTarget = sel.parts[0].sceneKind != SceneTargetKind::None;
            for (const StyleDecl& d : rule.decls) {
                // T-218: a scene-target selector takes only what an <svg> node takes, an element
                // selector never a vector property (both only differ in a mixed group, which the
                // parser let through without a warning).
                if (sceneTarget ? !appliesToAnySvgNode(d.prop) : isVectorProp(d.prop)) {
                    continue;
                }
                IrDecl decl{};
                if (sceneTarget && d.prop == xt::prop::PathData) {
                    if (!encodeScenePathDecl(style, d, rule.file.empty() ? fileName : rule.file, sink,
                                             decl)) {
                        continue;
                    }
                    style.decls.push_back(decl);
                    ++declCount;
                    continue;
                }
                if (!encodeDecl(writer, style, keyframesByName, d, fileName, sink, decl)) {
                    ok = false;
                    continue;
                }
                style.decls.push_back(decl);
                ++declCount;
            }

            IrRule irRule{};
            irRule.selOffset = selOffset;
            irRule.selCount =
                static_cast<uint32_t>(style.selectorParts.size()) - selOffset;  // T-173: + conditions.
            irRule.declOffset = declOffset;
            irRule.declCount = declCount;
            irRule.specificity = sel.specificity;
            if (rule.media >= 0) {
                mediaRules.push_back(IrMediaRule{static_cast<uint32_t>(style.rules.size()),
                                                 static_cast<uint32_t>(rule.media)});
            }
            style.rules.push_back(irRule);
        }
    }
    emitMedia(ruleSet.media, mediaRules, writer);

    // @font-face -> ASST Font asset + style.fontFaces[]. Dedup by `src` (same convention as
    // doc_emit.cpp's `img` asset dedup / lottie_assets.cpp's image asset cache).
    std::unordered_map<std::string, AssetId> fontAssetsBySrc;
    for (const FontFaceDef& ff : ruleSet.fontFaces) {
        AssetId assetId;
        auto cached = fontAssetsBySrc.find(ff.src);
        if (cached != fontAssetsBySrc.end()) {
            assetId = cached->second;
        } else {
            AssetBuilder ab;
            ab.kind = 1;  // Font.
            fs::path full = resolveAsset(options, xtDir, ff.src);
            bool embedded = false;
            if (options.embedFonts) {
                std::vector<uint8_t> bytes;
                if (readFileBytesStyleEmit(full, bytes)) {
                    ab.embedded = std::move(bytes);
                    embedded = true;
                } else {
                    sink.warn(std::string(fileName),
                              "failed to embed font asset \"" + ff.src + "\"; kept external");
                }
            }
            std::error_code ec;
            bool onDisk = fs::exists(full, ec);
            if (!embedded && !onDisk) {
                sink.warn(std::string(fileName), "font asset \"" + ff.src + "\" not found");
            }
            ab.sourceName = ff.src;
            ab.resolvedPath = onDisk ? pathToUtf8(full) : std::string();
            // An embedded asset carries its bytes, so its name stays as written (T-91).
            std::string assetName = ff.src;
            if (!embedded && options.assetNameRewrite) {
                std::string rewritten = options.assetNameRewrite(ff.src, ab.resolvedPath, ab.kind);
                if (!rewritten.empty()) {
                    assetName = std::move(rewritten);
                }
            }
            assetName = publishedAssetName(writer, ff.src, std::move(assetName), embedded, xtDir, options,
                                           ab.referencedByName);  // T-273b
            ab.name = writer.intern(assetName);
            assetId = writer.addAsset(std::move(ab));
            fontAssetsBySrc.emplace(ff.src, assetId);
        }

        IrFontFace face{};
        face.family = writer.intern(ff.family);
        face.asset = assetId;
        face.weight = ff.weight;
        face.italic = ff.italic ? 1 : 0;
        style.fontFaces.push_back(face);
    }

    return ok;
}

// -------------------------------------------------------------------------------------------
// `@timeline` -> per-track `@keyframes` + TGRP group (docs/design/28-animation-editing.md §28.1,
// 04-timeline.md §4.10.1, 12-compiler.md §12.7, docs/tasks/T-172.md).
// -------------------------------------------------------------------------------------------

namespace {

std::string_view domId(const DomNode& node) {
    const DomAttr* a = node.attr("id");
    return a != nullptr ? std::string_view(a->value) : std::string_view{};
}

bool domHasClass(const DomNode& node, const std::string& cls) {
    const DomAttr* a = node.attr("class");
    if (a == nullptr) {
        return false;
    }
    std::string_view list(a->value);
    size_t pos = 0;
    while (pos < list.size()) {
        while (pos < list.size() && (list[pos] == ' ' || list[pos] == '\t' || list[pos] == '\n')) {
            ++pos;
        }
        size_t end = pos;
        while (end < list.size() && list[end] != ' ' && list[end] != '\t' && list[end] != '\n') {
            ++end;
        }
        if (end > pos && list.substr(pos, end - pos) == cls) {
            return true;
        }
        pos = end;
    }
    return false;
}

bool partMatches(const SelectorPart& part, const DomNode& node) {
    if (part.stateMask != 0 || !part.conds.empty()) {
        // Only the state-less (base) cascade is known at compile time; T-173's extra conditions
        // are treated the same conservative way (not statically matched).
        return false;
    }
    if (!part.tag.empty() && part.tag != node.tag) {
        return false;
    }
    if (!part.id.empty() && part.id != domId(node)) {
        return false;
    }
    for (const std::string& cls : part.classes) {
        if (!domHasClass(node, cls)) {
            return false;
        }
    }
    return true;
}

// Right-to-left match of `sel.parts[partIndex..]` with parts[partIndex] matched against `element`.
bool selectorMatchesFrom(const Selector& sel, size_t partIndex, const DomTree& dom, uint32_t element) {
    if (!partMatches(sel.parts[partIndex], dom.nodes[element])) {
        return false;
    }
    if (partIndex + 1 == sel.parts.size()) {
        return true;
    }
    uint8_t comb = sel.parts[partIndex + 1].combinator;
    uint32_t anc = dom.nodes[element].parent;
    while (anc != 0xFFFFFFFFu) {
        if (selectorMatchesFrom(sel, partIndex + 1, dom, anc)) {
            return true;
        }
        if (comb == 1) {
            return false;  // child combinator: only the direct parent may match.
        }
        anc = dom.nodes[anc].parent;
    }
    return false;
}

// The element's state-less `effect` sequence as far as the compiler can tell (the winning `effect`
// declaration among the rules whose selectors carry no pseudo-class, then the inline `style`
// attribute). nullptr when no such declaration exists. 18.12's effect-name addressing needs it to
// turn `effect.<name>` into a stack index.
const std::vector<EffectDeclEntry>* staticEffectSequence(const RuleSet& ruleSet, const DomTree& dom,
                                                         uint32_t element,
                                                         std::vector<StyleDecl>& inlineScratch,
                                                         std::string_view fileName) {
    const StyleDecl* best = nullptr;
    uint64_t bestKey = 0;
    uint64_t order = 0;
    for (const StyleRule& rule : ruleSet.rules) {
        for (const Selector& sel : rule.selectors) {
            ++order;
            if (sel.parts.empty() || !selectorMatchesFrom(sel, 0, dom, element)) {
                continue;
            }
            for (const StyleDecl& d : rule.decls) {
                if (d.prop != styleprop::Effect) {
                    continue;
                }
                uint64_t key = (static_cast<uint64_t>(d.important ? 1 : 0) << 62) |
                               (static_cast<uint64_t>(sel.specificity) << 24) | (order & 0xFFFFFFu);
                if (best == nullptr || key >= bestKey) {
                    best = &d;
                    bestKey = key;
                }
            }
        }
    }
    const DomAttr* styleAttr = dom.nodes[element].attr("style");
    if (styleAttr != nullptr) {
        DiagnosticSink quiet;  // The inline style's own diagnostics are emitDoc's to report.
        inlineScratch.clear();
        parseDeclarations(styleAttr->value, fileName, styleAttr->line, quiet, inlineScratch);
        for (const StyleDecl& d : inlineScratch) {
            if (d.prop == styleprop::Effect && (best == nullptr || !best->important || d.important)) {
                best = &d;
            }
        }
    }
    return best != nullptr ? &best->value.effects : nullptr;
}

bool effectKindByName(std::string_view name, EffectKind& out) {
    struct Entry {
        const char* name;
        EffectKind kind;
    };
    static constexpr Entry kTable[] = {
        {"shadow", EffectKind::DropShadow},
        {"glow", EffectKind::Glow},
        {"blur", EffectKind::Blur},
        {"color-adjust", EffectKind::ColorAdjust},
        {"gradient-mask", EffectKind::GradientMask},
        {"shine", EffectKind::Shine},
        {"outline", EffectKind::Outline},
        {"backdrop-blur", EffectKind::BackdropBlur},
    };
    for (const Entry& e : kTable) {
        if (name == e.name) {
            out = e.kind;
            return true;
        }
    }
    return false;
}

// docs/design/18-effects.md §18.2-18.3: parameter name -> paramIndex for `kind` (`enabled` = 0,
// params[i] = i + 1, `color` = 16). Returns false for a name `kind` does not define.
bool effectParamIndexByName(EffectKind kind, std::string_view param, uint32_t& out) {
    if (param == "enabled") {
        out = xt::prop::kEffectParamEnabled;
        return true;
    }
    static constexpr const char* kShadow[] = {"offsetX", "offsetY", "blur", "spread", "opacity", "inset"};
    static constexpr const char* kGlow[] = {"radius", "intensity", "inner"};
    static constexpr const char* kBlur[] = {"radiusX", "radiusY"};
    static constexpr const char* kColorAdjust[] = {"brightness", "contrast", "saturation", "hue",
                                                   "opacity",    "invert",   "sepia"};
    static constexpr const char* kGradientMask[] = {"p0x", "p0y", "p1x", "p1y", "radial", "invert"};
    static constexpr const char* kShine[] = {"position", "width", "softness", "angle", "intensity"};
    static constexpr const char* kOutline[] = {"radius", "softness", "position"};
    static constexpr const char* kBackdrop[] = {"radius", "saturation", "brightness"};
    const char* const* names = nullptr;
    size_t count = 0;
    bool hasColor = false;
    switch (kind) {
        case EffectKind::DropShadow:
            names = kShadow, count = 6, hasColor = true;
            break;
        case EffectKind::Glow:
            names = kGlow, count = 3, hasColor = true;
            break;
        case EffectKind::Blur:
            names = kBlur, count = 2;
            break;
        case EffectKind::ColorAdjust:
            names = kColorAdjust, count = 7;
            break;
        case EffectKind::GradientMask:
            names = kGradientMask, count = 6;
            break;
        case EffectKind::Shine:
            names = kShine, count = 5, hasColor = true;
            break;
        case EffectKind::Outline:
            names = kOutline, count = 3, hasColor = true;
            break;
        case EffectKind::BackdropBlur:
            names = kBackdrop, count = 3;
            break;
        default:
            return false;
    }
    if (param == "color") {
        if (!hasColor) {
            return false;
        }
        out = xt::prop::kEffectParamColor0;
        return true;
    }
    for (size_t i = 0; i < count; ++i) {
        if (param == names[i]) {
            out = static_cast<uint32_t>(i + 1);
            return true;
        }
    }
    return false;
}

// Resolves one `effect.<effect>.<param>` declaration against `element`'s effect sequence into a
// StyleDecl on the 18.2 effect PropId. Reports an error and returns false when it cannot.
bool resolveTimelineEffectDecl(const IrWriter& writer, const TimelineEffectDecl& fx,
                               const std::vector<EffectDeclEntry>* seq, const std::string& path,
                               std::string_view fileName, DiagnosticSink& sink, StyleDecl& out) {
    std::string where = "effect." + fx.effect + "." + fx.param + " on #" + path;
    if (seq == nullptr || seq->empty()) {
        sink.error(std::string(fileName), "@timeline " + where + ": the element has no effect", fx.line);
        return false;
    }
    uint32_t index = 0;
    bool isIndex = !fx.effect.empty() && fx.effect.find_first_not_of("0123456789") == std::string::npos;
    if (isIndex) {
        index = static_cast<uint32_t>(std::strtoul(fx.effect.c_str(), nullptr, 10));
        if (index >= seq->size()) {
            sink.error(std::string(fileName), "@timeline " + where + ": effect index out of range", fx.line);
            return false;
        }
    } else {
        EffectKind kind = EffectKind::None;
        bool found = false;
        if (!effectKindByName(fx.effect, kind)) {
            // T-203 (18.12): a custom effect is named by its `.xtfx` header `name:` (else the
            // file's stem).
            for (size_t i = 0; i < seq->size() && !found; ++i) {
                if ((*seq)[i].kind != EffectKind::Custom) {
                    continue;
                }
                const ShaderBuilder* shader =
                    writer.shaderForAsset(findShaderAsset(writer, (*seq)[i].customAsset));
                if (shader != nullptr && shader->name == fx.effect) {
                    index = static_cast<uint32_t>(i);
                    found = true;
                }
            }
            if (!found) {
                sink.error(std::string(fileName),
                           "@timeline " + where + ": unknown effect \"" + fx.effect + "\"", fx.line);
                return false;
            }
        }
        for (size_t i = 0; i < seq->size() && !found; ++i) {
            if ((*seq)[i].kind == kind) {
                index = static_cast<uint32_t>(i);
                found = true;
                break;
            }
        }
        if (!found) {
            sink.error(std::string(fileName), "@timeline " + where + ": the element has no such effect",
                       fx.line);
            return false;
        }
    }
    uint32_t paramIndex = 0;
    bool paramOk = false;
    if ((*seq)[index].kind == EffectKind::Custom) {
        const ShaderBuilder* shader =
            writer.shaderForAsset(findShaderAsset(writer, (*seq)[index].customAsset));
        paramOk = shader != nullptr && xtfxParamIndex(xtfxParamsOf(*shader), fx.param, paramIndex);
    } else {
        paramOk = effectParamIndexByName((*seq)[index].kind, fx.param, paramIndex);
    }
    if (!paramOk) {
        sink.error(std::string(fileName), "@timeline " + where + ": unknown effect parameter", fx.line);
        return false;
    }
    out.prop = xt::effectParam(index, paramIndex);
    out.value = fx.value;
    out.line = fx.line;
    return true;
}

// One (prop, time) sample of a merged member track.
struct TrackSample {
    float time = 0;
    Easing arrival;  // Easing of the segment arriving at this sample.
    StyleDecl decl;
    bool hasMotion = false;  // T-228: the key's motion-in / motion-out (translate samples only).
    Vec2 motionIn{};
    Vec2 motionOut{};
};

bool sameTime(float a, float b) { return std::fabs(a - b) < 1e-5f; }

}  // namespace

void emitTimelineGroups(const RuleSet& ruleSet, const DomTree& dom, IrWriter& writer, StyleBuilder& style,
                        std::unordered_map<std::string, TimelineId>& keyframesByName, DiagnosticSink& sink,
                        TimelineGroupBuilder& out) {
    std::vector<std::string> seenNames;
    std::vector<StyleDecl> inlineScratch;
    for (const TimelineDef& def : ruleSet.timelines) {
        const std::string& fileName = def.fileName;
        // T-190: names are unique per scope (a symbol-scoped timeline repeats per instance).
        const std::string scopedName = def.scopePath + "|" + def.name;
        if (std::find(seenNames.begin(), seenNames.end(), scopedName) != seenNames.end()) {
            sink.error(fileName, "duplicate @timeline \"" + def.name + "\"", def.line);
            continue;
        }
        seenNames.push_back(scopedName);
        uint32_t scopeElement = 0;
        if (!def.scopePath.empty()) {
            scopeElement = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
                if (domId(dom.nodes[i]) == def.scopePath) {
                    scopeElement = i;
                    break;
                }
            }
            if (scopeElement == 0xFFFFFFFFu) {
                sink.error(fileName, "@timeline \"" + def.name + "\": no instance \"" + def.scopePath + "\"",
                           def.line);
                continue;
            }
        }

        // Merge the tracks by path, in first-appearance order.
        std::vector<std::string> paths;
        std::map<std::string, std::vector<const TimelineTrackDef*>> tracksByPath;
        for (const TimelineTrackDef& t : def.tracks) {
            if (tracksByPath.find(t.path) == tracksByPath.end()) {
                paths.push_back(t.path);
            }
            tracksByPath[t.path].push_back(&t);
        }

        bool ok = true;
        float lastKey = 0;
        RuleSet synth;  // One `@keyframes <timeline>__<path>` per track.
        struct PendingMember {
            uint32_t element;
            std::string keyframes;
            float delay;
            float duration;
        };
        std::vector<PendingMember> pendingMembers;

        for (const std::string& path : paths) {
            const std::vector<const TimelineTrackDef*>& defs = tracksByPath[path];
            int trackLine = defs.front()->line;
            uint32_t element = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
                if (domId(dom.nodes[i]) == path) {
                    element = i;
                    break;
                }
            }
            if (element == 0xFFFFFFFFu) {
                sink.error(fileName, "@timeline \"" + def.name + "\": no element with id \"" + path + "\"",
                           trackLine);
                ok = false;
                continue;
            }
            const std::vector<EffectDeclEntry>* effects = nullptr;
            bool effectsLooked = false;

            // Per prop: its samples in time order, plus first-appearance prop order.
            std::vector<uint16_t> propOrder;
            std::map<uint16_t, std::vector<TrackSample>> samples;
            auto addSample = [&](const TimelineKey& key, const StyleDecl& decl) {
                if (isVectorProp(decl.prop)) {
                    // T-190: vector properties address an inline <svg>'s nodes (`#<svg id>/<id>`).
                    sink.error(fileName,
                               "@timeline \"" + def.name + "\": #" + path +
                                   " is not inline <svg> content; vector properties (d, fill, stroke, "
                                   "path-trim, ...) need a #<svg id>/<id> track",
                               decl.line != 0 ? decl.line : key.line);
                    ok = false;
                    return;
                }
                if (decl.prop == styleprop::Effect) {
                    sink.error(fileName,
                               "@timeline \"" + def.name +
                                   "\": animate effect parameters with "
                                   "effect.<name|index>.<param>, not a whole effect sequence",
                               decl.line);
                    ok = false;
                    return;
                }
                std::vector<TrackSample>& list = samples[decl.prop];
                if (list.empty()) {
                    propOrder.push_back(decl.prop);
                }
                for (const TrackSample& s : list) {
                    if (sameTime(s.time, key.time)) {
                        // 05.9.3: the same element, property and time twice is an error.
                        sink.error(fileName,
                                   "@timeline \"" + def.name + "\": #" + path +
                                       " sets the same property twice at the same time",
                                   decl.line != 0 ? decl.line : key.line);
                        ok = false;
                        return;
                    }
                }
                TrackSample sample;
                sample.time = key.time;
                sample.arrival = key.hasEasing ? key.easing : Easing::linear();
                sample.decl = decl;
                if (key.hasMotion &&
                    (decl.prop == xt::prop::TranslateX || decl.prop == xt::prop::TranslateY)) {
                    sample.hasMotion = true;
                    sample.motionIn = key.motionIn;
                    sample.motionOut = key.motionOut;
                }
                list.push_back(std::move(sample));
            };
            for (const TimelineTrackDef* t : defs) {
                for (const TimelineKey& key : t->keys) {
                    for (const StyleDecl& d : key.decls) {
                        addSample(key, d);
                    }
                    for (const TimelineEffectDecl& fx : key.effectDecls) {
                        if (!effectsLooked) {
                            effects = staticEffectSequence(ruleSet, dom, element, inlineScratch, fileName);
                            effectsLooked = true;
                        }
                        StyleDecl d;
                        if (!resolveTimelineEffectDecl(writer, fx, effects, path, fileName, sink, d)) {
                            ok = false;
                            continue;
                        }
                        addSample(key, d);
                    }
                }
            }
            if (propOrder.empty()) {
                continue;  // A track with no usable declaration animates nothing.
            }

            float start = 0, end = 0;
            bool first = true;
            for (uint16_t p : propOrder) {
                std::vector<TrackSample>& list = samples[p];
                std::stable_sort(list.begin(), list.end(),
                                 [](const TrackSample& a, const TrackSample& b) { return a.time < b.time; });
                if (first) {
                    start = list.front().time;
                    end = list.back().time;
                    first = false;
                } else {
                    start = std::min(start, list.front().time);
                    end = std::max(end, list.back().time);
                }
            }
            lastKey = std::max(lastKey, end);
            float span = end - start;

            KeyframesDef kdef;
            kdef.name = def.name + "__" + path;
            // One KeyframeStop per sample: its timing is the easing of the NEXT sample of the same
            // property (28.1.2: a key's easing belongs to the segment arriving at it, while a
            // @keyframes stop's timing belongs to the segment leaving it). A property that starts
            // later / ends earlier than the track holds its first / last value (a timeline hold,
            // not a CSS neutral stop).
            auto pushStop = [&](float time, const StyleDecl& decl, const Easing& leaving,
                                const TrackSample* motion = nullptr) {
                KeyframeStop stop;
                stop.offset = span > 0 ? (time - start) / span * 100.0f : 0.0f;
                stop.decls.push_back(decl);
                stop.timing = leaving;
                stop.hasTiming = true;
                if (motion != nullptr && motion->hasMotion) {
                    stop.hasMotion = true;
                    stop.motionIn = motion->motionIn;
                    stop.motionOut = motion->motionOut;
                }
                kdef.stops.push_back(stop);
                if (span <= 0) {
                    stop.offset = 100.0f;
                    kdef.stops.push_back(std::move(stop));
                }
            };
            for (uint16_t p : propOrder) {
                const std::vector<TrackSample>& list = samples[p];
                if (span > 0 && !sameTime(list.front().time, start)) {
                    pushStop(start, list.front().decl, Easing::linear());
                }
                for (size_t i = 0; i < list.size(); ++i) {
                    Easing leaving = i + 1 < list.size() ? list[i + 1].arrival : Easing::linear();
                    pushStop(list[i].time, list[i].decl, leaving, &list[i]);
                }
                if (span > 0 && !sameTime(list.back().time, end)) {
                    pushStop(end, list.back().decl, Easing::linear());
                }
            }
            if (keyframesByName.find(kdef.name) != keyframesByName.end()) {
                sink.error(fileName, "@timeline track keyframes name \"" + kdef.name + "\" is already taken",
                           trackLine);
                ok = false;
                continue;
            }
            pendingMembers.push_back(PendingMember{element, kdef.name, start, span});
            synth.keyframes.push_back(std::move(kdef));
        }
        if (!ok) {
            continue;
        }

        emitKeyframesTimelines(synth, writer, style, fileName, sink, keyframesByName);

        IrTimelineGroup group{};
        group.name = writer.intern(def.name);
        group.scopeElement = scopeElement;  // T-190: the instance root for a symbol timeline.
        // 05.9.3: no `duration` -> the last key. Markers and sound cues past the last key extend
        // it too, so every declared time point is reachable (conservative reading, see report).
        float implicitDuration = std::max(lastKey, def.sceneLastKey);
        for (const auto& marker : def.markers) {
            implicitDuration = std::max(implicitDuration, marker.second);
        }
        for (const auto& snd : def.sounds) {
            implicitDuration = std::max(implicitDuration, snd.first);
        }
        group.duration = def.duration >= 0 ? def.duration : implicitDuration;
        group.markerOffset = static_cast<uint32_t>(out.markers.size());
        for (const auto& [markerName, t] : def.markers) {
            out.markers.push_back(IrTimelineGroupMarker{t, writer.intern(markerName)});
        }
        group.markerCount = static_cast<uint32_t>(out.markers.size()) - group.markerOffset;
        group.soundOffset = static_cast<uint32_t>(out.sounds.size());
        for (const auto& [t, cue] : def.sounds) {
            out.sounds.push_back(IrTimelineGroupSound{t, writer.intern(cue)});
        }
        group.soundCount = static_cast<uint32_t>(out.sounds.size()) - group.soundOffset;
        group.memberOffset = static_cast<uint32_t>(out.members.size());
        for (const PendingMember& m : pendingMembers) {
            auto it = keyframesByName.find(m.keyframes);
            if (it == keyframesByName.end()) {
                continue;
            }
            out.members.push_back(IrTimelineGroupMember{m.element, it->second, m.delay, m.duration});
        }
        // T-190 (11 §11.9): an inline <svg> animated by this timeline is a member whose timeline is
        // the svg scene's own (seconds, not normalized); the runtime recognizes it by that
        // identity and seeks it in group time.
        for (const auto& [svgElement, sceneTimeline] : def.sceneMembers) {
            out.members.push_back(IrTimelineGroupMember{svgElement, sceneTimeline, 0.0f, group.duration});
        }
        group.memberCount = static_cast<uint32_t>(out.members.size()) - group.memberOffset;
        out.groups.push_back(group);
    }
}

bool selectorMatchesStatically(const Selector& sel, const DomTree& dom, uint32_t element) {
    return !sel.parts.empty() && selectorMatchesFrom(sel, 0, dom, element);
}

}  // namespace xt::compile
