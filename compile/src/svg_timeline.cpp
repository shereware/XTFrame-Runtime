#include "svg_timeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "xt/compile/parse_svg.h"
#include "xt/compile/parse_xts.h"  // selectorSpecificity (T-218).
#include "xt/compile/svg_path.h"
#include "xt/ir_pod.h"  // irSvgTagCode (T-218).
#include "xt/node.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xts/xts_props.h"

/// See svg_timeline.h and docs/tasks/T-190.md.

namespace xt::compile {

namespace {

struct Sample {
    float time = 0;
    Easing arrival;
    StyleDecl decl;
    int line = 0;
    bool hasMotion = false;  // T-228: the key's motion-in / motion-out.
    Vec2 motionIn{};
    Vec2 motionOut{};
};

struct TrackKey {
    uint32_t node = 0;
    uint16_t prop = 0;
    bool operator<(const TrackKey& o) const { return node != o.node ? node < o.node : prop < o.prop; }
};

std::string domIdOf(const DomNode& node) {
    const DomAttr* a = node.attr("id");
    return a != nullptr ? a->value : std::string();
}

const char* propNameSvgTimeline(uint16_t prop) {
    // T-225: an <svg> track's 160-205 is a modifier / shape parameter, never a StyleProp.
    const PropInfo* info = findSvgNodePropById(prop);
    if (info == nullptr) {
        info = findPropById(prop);
    }
    return info != nullptr ? info->cssName : "?";
}

/// A px Length or a plain Float as a float; false for any other unit.
bool pxOrFloat(const StyleValue& v, float& out) {
    if (v.kind == StyleValueKind::Float) {
        out = v.f;
        return true;
    }
    if (v.kind == StyleValueKind::Length && v.len.unit == LengthUnit::Px) {
        out = v.len.value;
        return true;
    }
    return false;
}

void setEasing(IrKeyframe& kf, const Easing& e) {
    kf.easing.kind = static_cast<uint8_t>(e.kind);
    kf.easing.stepsJumpEnd = e.stepsJumpEnd;
    kf.easing.stepCount = e.stepCount;
    kf.easing.x1 = e.x1;
    kf.easing.y1 = e.y1;
    kf.easing.x2 = e.x2;
    kf.easing.y2 = e.y2;
}

/// T-228 (31 §31.7.2): a node whose translate keys carry motion-in / motion-out gets one Vec2
/// Translate track with spatial handles instead of the TranslateX / TranslateY pair (both removed
/// from `samples`). Needs px keys on both axes at the same times; warns and leaves them otherwise.
void mergeMotionTranslate(std::map<TrackKey, std::vector<Sample>>& samples, TimelineBuilder& tl,
                          const std::string& svgId, const std::string& where, DiagnosticSink& sink,
                          float& lastKey) {
    std::vector<uint32_t> nodes;
    for (const auto& [key, list] : samples) {
        if (key.prop != prop::TranslateX) {
            continue;
        }
        for (const Sample& s : list) {
            if (s.hasMotion) {
                nodes.push_back(key.node);
                break;
            }
        }
    }
    for (uint32_t node : nodes) {
        std::vector<Sample> xs = samples[TrackKey{node, prop::TranslateX}];
        auto yIt = samples.find(TrackKey{node, prop::TranslateY});
        bool usable = yIt != samples.end() && yIt->second.size() == xs.size();
        std::vector<Sample> ys = usable ? yIt->second : std::vector<Sample>{};
        auto byTime = [](const Sample& a, const Sample& b) { return a.time < b.time; };
        std::stable_sort(xs.begin(), xs.end(), byTime);
        std::stable_sort(ys.begin(), ys.end(), byTime);
        std::vector<Vec2> pts(xs.size());
        for (size_t i = 0; usable && i < xs.size(); ++i) {
            usable = std::fabs(xs[i].time - ys[i].time) < 1e-5f && pxOrFloat(xs[i].decl.value, pts[i].x) &&
                     pxOrFloat(ys[i].decl.value, pts[i].y);
        }
        if (!usable || xs.empty()) {
            sink.warn(where,
                      "@timeline: motion-in / motion-out on #" + svgId +
                          " content need px translate keys on both axes at the same times; ignored",
                      xs.empty() ? 0 : xs.front().line);
            continue;
        }
        IrTrack track{};
        track.target = node;
        track.prop = prop::Translate;
        track.type = static_cast<uint8_t>(ValueType::Vec2);
        track.kfOffset = static_cast<uint32_t>(tl.keyframes.size());
        for (size_t i = 0; i < xs.size(); ++i) {
            IrKeyframe kf{};
            kf.time = xs[i].time;
            setEasing(kf, i + 1 < xs.size() ? xs[i + 1].arrival : Easing::linear());
            irSetValueF32(kf.value, 0, pts[i].x);
            irSetValueF32(kf.value, 1, pts[i].y);
            const Vec2 out = xs[i].hasMotion ? xs[i].motionOut : Vec2{};
            const Vec2 in = i + 1 < xs.size() && xs[i + 1].hasMotion ? xs[i + 1].motionIn : Vec2{};
            if (out.x != 0.0f || out.y != 0.0f || in.x != 0.0f || in.y != 0.0f) {
                kf.hasSpatial = 1;
                kf.tanOut = Vec3{out.x, out.y, 0};
                kf.tanIn = Vec3{in.x, in.y, 0};
            }
            tl.keyframes.push_back(kf);
        }
        track.kfCount = static_cast<uint32_t>(xs.size());
        tl.tracks.push_back(track);
        lastKey = std::max(lastKey, xs.back().time);
        samples.erase(TrackKey{node, prop::TranslateX});
        samples.erase(TrackKey{node, prop::TranslateY});
    }
}

}  // namespace

std::string inlineSvgSceneKey(uint32_t domIndex) { return "#svg:" + std::to_string(domIndex); }

void emitInlineSvgs(const DomTree& dom, RuleSet& ruleSet, IrWriter& writer, const std::string& fileName,
                    DiagnosticSink& sink, std::map<std::string, SceneId>& sceneBySrc,
                    std::map<std::string, std::vector<std::string>>* innerIds, InlineSvgRig* rig) {
    for (uint32_t domIndex = 0; domIndex < dom.nodes.size(); ++domIndex) {
        const DomNode& node = dom.nodes[domIndex];
        if (node.tag != "svg" || node.svgSource.empty()) {
            continue;
        }
        SceneBuilder scene;
        std::vector<std::pair<std::string, uint32_t>> ids;
        // T-243: rig content -- `<xt-mesh href>` images and scene-node bindings.
        SvgParseExtras extras;
        const int lineBase = node.svgLine > 0 ? node.svgLine - 1 : 0;
        if (rig != nullptr && rig->imageAsset) {
            extras.imageAsset = [&](const std::string& href, int line) {
                return rig->imageAsset(href, lineBase + line);
            };
        }
        if (parseSvg(node.svgSource, fileName, sink, writer, scene, &ids, &extras) != Status::kOk) {
            continue;
        }
        if (rig != nullptr && !extras.binds.empty()) {
            for (SvgBindAttr& b : extras.binds) {
                b.line += lineBase;
            }
            rig->binds[domIndex] = std::move(extras.binds);
        }
        if (rig != nullptr) {
            for (auto& [id, line] : extras.boneIds) {
                rig->boneIds.emplace_back(std::move(id), lineBase + line);  // T-243b
            }
        }
        const std::string svgId = domIdOf(node);
        if (innerIds != nullptr && !svgId.empty()) {
            std::vector<std::string>& list = (*innerIds)[svgId];
            for (const auto& entry : ids) {
                list.push_back(entry.first);
            }
        }
        TimelineBuilder tl;
        tl.name = writer.intern(svgId.empty() ? std::string("svg") : svgId);

        // Pull this svg's tracks out of every timeline.
        TimelineDef* owner = nullptr;
        std::map<TrackKey, std::vector<Sample>> samples;
        bool ok = true;
        if (!svgId.empty()) {
            const std::string prefix = svgId + "/";
            for (TimelineDef& def : ruleSet.timelines) {
                std::vector<TimelineTrackDef> kept;
                bool took = false;
                for (TimelineTrackDef& track : def.tracks) {
                    if (track.path.compare(0, prefix.size(), prefix) != 0) {
                        kept.push_back(std::move(track));
                        continue;
                    }
                    took = true;
                    const std::string inner = track.path.substr(prefix.size());
                    // T-236 (05 §5.9.7): an id first; only a name no node has is a tag name
                    // (every node of that SVG element), as for scene-target rules.
                    std::vector<uint32_t> targets;
                    for (const auto& [id, index] : ids) {
                        if (id == inner) {
                            targets.push_back(index);
                            break;
                        }
                    }
                    const IrSvgTag tagCode = targets.empty() ? irSvgTagCode(inner) : IrSvgTag::None;
                    if (tagCode != IrSvgTag::None) {
                        for (uint32_t i = 1; i < scene.nodes.size(); ++i) {
                            if (scene.nodes[i].svgTag == static_cast<uint8_t>(tagCode)) {
                                targets.push_back(i);
                            }
                        }
                    }
                    if (targets.empty()) {
                        sink.error(def.fileName,
                                   "@timeline \"" + def.name + "\": <svg id=\"" + svgId +
                                       "\"> has no node \"" + inner + "\"",
                                   track.line);
                        ok = false;
                        continue;
                    }
                    for (const uint32_t target : targets) {
                        NodeKind kind = static_cast<NodeKind>(scene.nodes[target].kind);
                        for (const TimelineKey& key : track.keys) {
                            if (!key.effectDecls.empty()) {
                                sink.error(
                                    def.fileName,
                                    "@timeline \"" + def.name + "\": inline <svg> content has no effects",
                                    key.line);
                                ok = false;
                            }
                            for (StyleDecl d : key.decls) {
                                // T-229 (31 §31.7.5): `visibility` on a scene node is its Visible
                                // flag (PropId 13, 1 = shown); cels are hold keys of it.
                                if (d.prop == styleprop::Visibility && d.value.kind == StyleValueKind::Enum) {
                                    d.prop = prop::Visible;
                                    d.value.e = d.value.e == 0 ? 1u : 0u;
                                }
                                // T-225: `width`/`height` on a parametric Shape are its ShapeW/ShapeH.
                                // T-243: `strength` / `enabled` on a constraint by its kind.
                                const ConstraintKind ck =
                                    kind == NodeKind::Constraint &&
                                            scene.nodes[target].kindIndex < scene.constraints.size()
                                        ? static_cast<ConstraintKind>(
                                              scene.constraints[scene.nodes[target].kindIndex].kind)
                                        : ConstraintKind::Ik;
                                uint16_t prop = svgNodeProp(d.prop, kind, ck);
                                // T-236: a single-path `<xt-trim>` became its path's own trim
                                // (parseSvg), so its `trim-*` keys drive the path's trim (offset in
                                // degrees -> turns).
                                if ((kind == NodeKind::Path || kind == NodeKind::Shape) &&
                                    prop >= prop::ModTrimStart && prop <= prop::ModTrimOffset) {
                                    if (prop == prop::ModTrimOffset) {
                                        d.value.f /= 360.0f;
                                        d.value.len.value /= 360.0f;
                                    }
                                    prop =
                                        static_cast<uint16_t>(prop::TrimStart + (prop - prop::ModTrimStart));
                                }
                                if (!appliesToSvgNode(prop, kind)) {
                                    sink.error(def.fileName,
                                               "@timeline \"" + def.name + "\": \"" +
                                                   propNameSvgTimeline(prop) + "\" does not apply to #" +
                                                   track.path,
                                               d.line != 0 ? d.line : key.line);
                                    ok = false;
                                    continue;
                                }
                                Sample s;
                                s.time = key.time;
                                s.arrival = key.hasEasing ? key.easing : Easing::linear();
                                s.decl = d;
                                s.decl.prop = prop;
                                s.line = d.line != 0 ? d.line : key.line;
                                s.hasMotion = key.hasMotion;
                                s.motionIn = key.motionIn;
                                s.motionOut = key.motionOut;
                                samples[TrackKey{target, prop}].push_back(std::move(s));
                            }
                        }
                    }
                }
                def.tracks = std::move(kept);
                if (took) {
                    if (owner != nullptr && owner != &def) {
                        sink.error(def.fileName,
                                   "inline <svg id=\"" + svgId +
                                       "\"> is animated by more than one @timeline (\"" + owner->name +
                                       "\" and \"" + def.name + "\")",
                                   def.line);
                        ok = false;
                    }
                    owner = &def;
                }
            }
        }

        float lastKey = 0;
        mergeMotionTranslate(samples, tl, svgId, owner != nullptr ? owner->fileName : fileName, sink,
                             lastKey);
        for (auto& [key, list] : samples) {
            std::stable_sort(list.begin(), list.end(),
                             [](const Sample& a, const Sample& b) { return a.time < b.time; });
            bool trackOk = true;
            for (size_t i = 1; i < list.size(); ++i) {
                if (std::fabs(list[i].time - list[i - 1].time) < 1e-5f) {
                    sink.error(owner != nullptr ? owner->fileName : fileName,
                               "@timeline: #" + svgId + " node sets \"" +
                                   std::string(propNameSvgTimeline(key.prop)) + "\" twice at the same time",
                               list[i].line);
                    trackOk = false;
                }
            }
            if (!trackOk) {
                ok = false;
                continue;
            }
            const std::string where = owner != nullptr ? owner->fileName : fileName;
            IrTrack track{};
            track.target = key.node;
            track.prop = key.prop;
            track.kfOffset = static_cast<uint32_t>(tl.keyframes.size());

            if (key.prop == prop::PathData) {
                // 03 §3.12.6: every key parsed; when any key's topology differs from the node's
                // own path, every key carries its verbs (so a jump back also restores them).
                const IrSceneNode& sn = scene.nodes[key.node];
                const IrPathData& pd = scene.paths[sn.kindIndex];
                std::vector<SvgPath> parsed(list.size());
                bool anyDiffers = false;
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i].decl.value.kind != StyleValueKind::String ||
                        !parseSvgPathData(list[i].decl.value.str, parsed[i]) || parsed[i].verbs.empty()) {
                        sink.error(where, "@timeline: malformed \"d\" for #" + svgId, list[i].line);
                        trackOk = false;
                        continue;
                    }
                    bool same = parsed[i].verbs.size() == pd.verbCount &&
                                std::equal(parsed[i].verbs.begin(), parsed[i].verbs.end(),
                                           scene.verbPool.begin() + pd.verbOffset);
                    anyDiffers = anyDiffers || !same;
                }
                if (!trackOk) {
                    ok = false;
                    continue;
                }
                for (size_t i = 0; i < list.size(); ++i) {
                    IrKeyframe kf{};
                    kf.time = list[i].time;
                    setEasing(kf, i + 1 < list.size() ? list[i + 1].arrival : Easing::linear());
                    irSetValueU32(kf.value, 0, static_cast<uint32_t>(tl.floatPool.size()));
                    irSetValueU32(kf.value, 1, static_cast<uint32_t>(parsed[i].points.size()));
                    tl.floatPool.insert(tl.floatPool.end(), parsed[i].points.begin(), parsed[i].points.end());
                    if (anyDiffers) {
                        irSetValueU32(kf.value, 2, static_cast<uint32_t>(tl.floatPool.size()));
                        irSetValueU32(kf.value, 3, static_cast<uint32_t>(parsed[i].verbs.size()));
                        for (uint8_t v : parsed[i].verbs) {
                            tl.floatPool.push_back(static_cast<float>(v));
                        }
                    }
                    tl.keyframes.push_back(kf);
                }
                track.type = static_cast<uint8_t>(ValueType::Path);
            } else if (key.prop == prop::DashArray) {
                // T-236: a dash array never interpolates: hold keys, each its own array.
                for (const Sample& s : list) {
                    if (s.decl.value.kind != StyleValueKind::FloatArray) {
                        sink.error(where, "@timeline: malformed \"stroke-dasharray\" for #" + svgId, s.line);
                        trackOk = false;
                        break;
                    }
                    IrKeyframe kf{};
                    kf.time = s.time;
                    kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
                    irSetValueU32(kf.value, 0, static_cast<uint32_t>(tl.floatPool.size()));
                    irSetValueU32(kf.value, 1, static_cast<uint32_t>(s.decl.value.floats.size()));
                    tl.floatPool.insert(tl.floatPool.end(), s.decl.value.floats.begin(),
                                        s.decl.value.floats.end());
                    tl.keyframes.push_back(kf);
                }
                if (!trackOk) {
                    ok = false;
                    tl.keyframes.resize(track.kfOffset);
                    continue;
                }
                track.type = static_cast<uint8_t>(ValueType::FloatArray);
            } else if (key.prop == prop::ModAnchor || key.prop == prop::ModTranslate ||
                       key.prop == prop::ModCenter) {
                // T-236: repeat-anchor / repeat-translate / center, `<x> <y>`.
                for (size_t i = 0; i < list.size(); ++i) {
                    const StyleValue& v = list[i].decl.value;
                    if (v.kind != StyleValueKind::Vec2 || v.floats.size() != 2) {
                        sink.error(where,
                                   "@timeline: \"" + std::string(propNameSvgTimeline(key.prop)) + "\" on #" +
                                       svgId + " content must be <x> <y>",
                                   list[i].line);
                        trackOk = false;
                        break;
                    }
                    IrKeyframe kf{};
                    kf.time = list[i].time;
                    setEasing(kf, i + 1 < list.size() ? list[i + 1].arrival : Easing::linear());
                    irSetValueF32(kf.value, 0, v.floats[0]);
                    irSetValueF32(kf.value, 1, v.floats[1]);
                    tl.keyframes.push_back(kf);
                }
                if (!trackOk) {
                    ok = false;
                    tl.keyframes.resize(track.kfOffset);
                    continue;
                }
                track.type = static_cast<uint8_t>(ValueType::Vec2);
            } else if (key.prop == prop::Visible) {
                // T-229: hold keys, like a Lottie layer's ip/op (11 §11.5).
                for (const Sample& s : list) {
                    IrKeyframe kf{};
                    kf.time = s.time;
                    kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
                    irSetValueU32(kf.value, 0, s.decl.value.e);
                    tl.keyframes.push_back(kf);
                }
                track.type = static_cast<uint8_t>(ValueType::Enum);
            } else {
                bool isColor = key.prop == prop::FillColor || key.prop == prop::StrokeColor;
                for (size_t i = 0; i < list.size(); ++i) {
                    IrKeyframe kf{};
                    kf.time = list[i].time;
                    setEasing(kf, i + 1 < list.size() ? list[i + 1].arrival : Easing::linear());
                    const StyleValue& v = list[i].decl.value;
                    if (isColor) {
                        if (v.kind != StyleValueKind::Color) {
                            trackOk = false;
                        } else {
                            irSetValueF32(kf.value, 0, v.color.r);
                            irSetValueF32(kf.value, 1, v.color.g);
                            irSetValueF32(kf.value, 2, v.color.b);
                            irSetValueF32(kf.value, 3, v.color.a);
                        }
                    } else {
                        float f = 0;
                        if (!pxOrFloat(v, f)) {
                            trackOk = false;
                        } else {
                            irSetValueF32(kf.value, 0, f);
                        }
                    }
                    if (!trackOk) {
                        sink.error(where,
                                   "@timeline: \"" + std::string(propNameSvgTimeline(key.prop)) + "\" on #" +
                                       svgId + " content must be a px length or a number",
                                   list[i].line);
                        break;
                    }
                    tl.keyframes.push_back(kf);
                }
                if (!trackOk) {
                    ok = false;
                    tl.keyframes.resize(track.kfOffset);
                    continue;
                }
                track.type = static_cast<uint8_t>(isColor ? ValueType::Color : ValueType::Float);
            }
            track.kfCount = static_cast<uint32_t>(tl.keyframes.size()) - track.kfOffset;
            tl.tracks.push_back(track);
            lastKey = std::max(lastKey, list.back().time);
        }
        (void)ok;
        tl.duration = lastKey;

        const bool animated = !tl.tracks.empty();
        SceneId sceneId = writer.addScene(std::move(scene));
        TimelineId timelineId = writer.addTimeline(std::move(tl));
        (void)timelineId;
        sceneBySrc.emplace(inlineSvgSceneKey(domIndex), sceneId);
        if (owner != nullptr && animated) {
            owner->sceneMembers.emplace_back(domIndex, sceneId);
            owner->sceneLastKey = std::max(owner->sceneLastKey, lastKey);
        }
    }
}

namespace {

/// A `transform` component an <svg> node has no use for, at its identity value (T-218).
bool isIdentityTransformPart(const StyleDecl& d) {
    if (d.prop != prop::TranslateZ && d.prop != prop::RotationX && d.prop != prop::RotationY) {
        return false;
    }
    return (d.value.kind == StyleValueKind::Float && d.value.f == 0.0f) ||
           (d.value.kind == StyleValueKind::Length && d.value.len.value == 0.0f);
}

}  // namespace

void resolveSceneTargets(RuleSet& ruleSet, const DomTree& dom, const std::string& fileName,
                         DiagnosticSink& sink,
                         const std::map<std::string, std::vector<std::string>>* innerIds) {
    std::map<std::string, std::string> tagById;
    for (const DomNode& node : dom.nodes) {
        const std::string id = domIdOf(node);
        if (!id.empty()) {
            tagById.emplace(id, node.tag);
        }
    }
    for (StyleRule& rule : ruleSet.rules) {
        size_t scene = 0;
        for (Selector& sel : rule.selectors) {
            SelectorPart& subject = sel.parts[0];
            const size_t slash = subject.id.rfind('/');
            if (subject.sceneKind == SceneTargetKind::None && slash != std::string::npos &&
                tagById.find(subject.id) == tagById.end()) {
                auto owner = tagById.find(subject.id.substr(0, slash));
                if (owner != tagById.end() &&
                    (owner->second == "svg" || owner->second == "anim" || owner->second == "img")) {
                    std::string last = subject.id.substr(slash + 1);
                    subject.id.resize(slash);
                    // T-236 (05 §5.9.7): an id of the <svg>'s own content first; only a name no
                    // node has (or an <anim>/<img>, whose ids are not known here) is a tag name.
                    bool isId = false;
                    if (innerIds != nullptr) {
                        auto list = innerIds->find(subject.id);
                        isId = list != innerIds->end() && std::find(list->second.begin(), list->second.end(),
                                                                    last) != list->second.end();
                    }
                    subject.sceneKind = !isId && irSvgTagCode(last) != IrSvgTag::None ? SceneTargetKind::Tag
                                                                                      : SceneTargetKind::Name;
                    subject.sceneTarget = std::move(last);
                    sel.specificity = selectorSpecificity(sel);
                }
            }
            scene += subject.sceneKind != SceneTargetKind::None ? 1u : 0u;
        }
        if (scene != 0 && scene != rule.selectors.size()) {
            continue;  // A mixed group: emitStyle keeps each side's own properties.
        }
        const std::string& file = rule.file.empty() ? fileName : rule.file;
        std::vector<StyleDecl> kept;
        std::vector<uint16_t> warned;
        for (StyleDecl& d : rule.decls) {
            const bool keep = scene != 0 ? appliesToAnySvgNode(d.prop) : !isVectorProp(d.prop);
            if (keep) {
                kept.push_back(std::move(d));
                continue;
            }
            if ((scene != 0 && isIdentityTransformPart(d)) ||
                std::find(warned.begin(), warned.end(), d.prop) != warned.end()) {
                continue;
            }
            warned.push_back(d.prop);
            const std::string name = propNameSvgTimeline(d.prop);
            sink.warn(file,
                      scene != 0 ? "\"" + name + "\": only vector properties apply to <svg> content; ignored"
                                 : "\"" + name +
                                       "\" only applies to inline <svg> content in @timeline keys; ignored",
                      d.line != 0 ? d.line : rule.line);
        }
        rule.decls = std::move(kept);
    }
}

void warnUnknownSceneTargets(const RuleSet& ruleSet,
                             const std::map<std::string, std::vector<std::string>>& innerIds,
                             const std::string& fileName, DiagnosticSink& sink) {
    for (const StyleRule& rule : ruleSet.rules) {
        for (const Selector& sel : rule.selectors) {
            const SelectorPart& subject = sel.parts[0];
            if (subject.sceneKind != SceneTargetKind::Name) {
                continue;
            }
            auto it = innerIds.find(subject.id);
            if (it == innerIds.end() ||
                std::find(it->second.begin(), it->second.end(), subject.sceneTarget) != it->second.end()) {
                continue;
            }
            sink.warn(rule.file.empty() ? fileName : rule.file,
                      "vector.unknownTarget: <svg id=\"" + subject.id + "\"> has no node \"" +
                          subject.sceneTarget + "\"",
                      rule.line);
        }
    }
}

}  // namespace xt::compile
