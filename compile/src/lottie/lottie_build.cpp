#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>

#include "lottie_model.h"
#include "lottie_parent_chain.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/value.h"

/// LottieDoc -> SceneBuilder + TimelineBuilder. See docs/design/11-lottie-mapping.md §11.2-§11.5,
/// docs/tasks/T-14.md "模型 → IR".

namespace xt::compile {

// TimeMap, BuildCtx and mapFrameToSec are declared/defined once in lottie_model.h so
// lottie_shapes_build.cpp (T-15) can reuse them; only mapFrameToSec's definition lives here.

float mapFrameToSec(const BuildCtx& ctx, TimeMap map, float rawFrame) {
    float absFrame = map.apply(rawFrame);
    return ctx.doc.fr != 0.0f ? (absFrame - ctx.doc.ip) / ctx.doc.fr : 0.0f;
}

/// Builds a Float Track from `av` if it is (still, after dedup) animated by more than one
/// keyframe; a single remaining keyframe is treated as a static value (no track). Returns true if
/// a track was emitted.
bool buildFloatTrack(BuildCtx& ctx, const AnimatedValue<float>& av, uint32_t target, PropId propId,
                     TimeMap map, const LottieLayer& layer, const char* propLabel, float scale) {
    if (!av.animated) {
        return false;
    }
    std::vector<const LottieKeyframe<float>*> kept = dedupKeyframes(av, ctx, layer, propLabel);
    if (kept.size() < 2) {
        return false;
    }
    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (const LottieKeyframe<float>* kp : kept) {
        const LottieKeyframe<float>& k = *kp;
        IrKeyframe kf{};
        kf.time = mapFrameToSec(ctx, map, k.frame);
        if (k.hold) {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
        } else {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::CubicBezier);
            kf.easing.x1 = k.ox;
            kf.easing.y1 = k.oy;
            kf.easing.x2 = k.ix;
            kf.easing.y2 = k.iy;
        }
        irSetValueF32(kf.value, 0, k.value * scale);
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = target;
    track.prop = propId;
    track.type = static_cast<uint8_t>(ValueType::Float);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(kept.size());
    ctx.timeline.tracks.push_back(track);
    return true;
}

/// Same as buildFloatTrack but for Vec2 properties; also carries spatial tangents (Lottie's
/// `ti`/`to`), scaled the same way as the value.
bool buildVec2Track(BuildCtx& ctx, const AnimatedValue<Vec2>& av, uint32_t target, PropId propId, TimeMap map,
                    const LottieLayer& layer, const char* propLabel, float scale) {
    if (!av.animated) {
        return false;
    }
    std::vector<const LottieKeyframe<Vec2>*> kept = dedupKeyframes(av, ctx, layer, propLabel);
    if (kept.size() < 2) {
        return false;
    }
    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (const LottieKeyframe<Vec2>* kp : kept) {
        const LottieKeyframe<Vec2>& k = *kp;
        IrKeyframe kf{};
        kf.time = mapFrameToSec(ctx, map, k.frame);
        if (k.hold) {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
        } else {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::CubicBezier);
            kf.easing.x1 = k.ox;
            kf.easing.y1 = k.oy;
            kf.easing.x2 = k.ix;
            kf.easing.y2 = k.iy;
        }
        kf.hasSpatial = k.hasSpatial ? 1 : 0;
        if (k.hasSpatial) {
            // Lottie only ever carries 2D (Vec2) spatial tangents; T-42c widened IrKeyframe's
            // tanOut/tanIn to Vec3, z = 0 here.
            kf.tanOut = Vec3{k.to.x * scale, k.to.y * scale, 0.0f};
            kf.tanIn = Vec3{k.ti.x * scale, k.ti.y * scale, 0.0f};
        }
        irSetValueF32(kf.value, 0, k.value.x * scale);
        irSetValueF32(kf.value, 1, k.value.y * scale);
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = target;
    track.prop = propId;
    track.type = static_cast<uint8_t>(ValueType::Vec2);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(kept.size());
    ctx.timeline.tracks.push_back(track);
    return true;
}

/// Same as buildFloatTrack but for Color properties (T-15's Fill/Stroke color, T-55's Drop Shadow
/// color); moved here (out of lottie_shapes_build.cpp's anonymous namespace) so both files can use
/// it without duplicating the Color track-building logic.
bool buildColorTrack(BuildCtx& ctx, const AnimatedValue<Color>& av, uint32_t target, PropId propId,
                     TimeMap map, const LottieLayer& layer, const char* propLabel) {
    if (!av.animated) {
        return false;
    }
    std::vector<const LottieKeyframe<Color>*> kept = dedupKeyframes(av, ctx, layer, propLabel);
    if (kept.size() < 2) {
        return false;
    }
    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (const LottieKeyframe<Color>* kp : kept) {
        const LottieKeyframe<Color>& k = *kp;
        IrKeyframe kf{};
        kf.time = mapFrameToSec(ctx, map, k.frame);
        if (k.hold) {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
        } else {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::CubicBezier);
            kf.easing.x1 = k.ox;
            kf.easing.y1 = k.oy;
            kf.easing.x2 = k.ix;
            kf.easing.y2 = k.iy;
        }
        irSetValueF32(kf.value, 0, k.value.r);
        irSetValueF32(kf.value, 1, k.value.g);
        irSetValueF32(kf.value, 2, k.value.b);
        irSetValueF32(kf.value, 3, k.value.a);
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = target;
    track.prop = propId;
    track.type = static_cast<uint8_t>(ValueType::Color);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(kept.size());
    ctx.timeline.tracks.push_back(track);
    return true;
}

namespace {

/// T-47b: Vec3 Track builder (docs/design/19-3d-space.md §19.3's Vec3 tracks: Translate3/
/// Orientation/CameraTarget). `scale` is applied component-wise to both the value and (when
/// present) the spatial tangents; it encodes the z-axis negation (§19.2) via `scale.z == -1`
/// (position/camera target) while `scale.x/y` stay 1, or negates x/y only for `orientation`
/// (`scale == {-1,-1,1}`).
bool buildVec3Track(BuildCtx& ctx, const AnimatedValue<Vec3>& av, uint32_t target, PropId propId, TimeMap map,
                    const LottieLayer& layer, const char* propLabel, Vec3 scale) {
    if (!av.animated) {
        return false;
    }
    std::vector<const LottieKeyframe<Vec3>*> kept = dedupKeyframes(av, ctx, layer, propLabel);
    if (kept.size() < 2) {
        return false;
    }
    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (const LottieKeyframe<Vec3>* kp : kept) {
        const LottieKeyframe<Vec3>& k = *kp;
        IrKeyframe kf{};
        kf.time = mapFrameToSec(ctx, map, k.frame);
        if (k.hold) {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
        } else {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::CubicBezier);
            kf.easing.x1 = k.ox;
            kf.easing.y1 = k.oy;
            kf.easing.x2 = k.ix;
            kf.easing.y2 = k.iy;
        }
        kf.hasSpatial = k.hasSpatial ? 1 : 0;
        if (k.hasSpatial) {
            kf.tanOut = Vec3{k.to.x * scale.x, k.to.y * scale.y, k.to.z * scale.z};
            kf.tanIn = Vec3{k.ti.x * scale.x, k.ti.y * scale.y, k.ti.z * scale.z};
        }
        irSetValueF32(kf.value, 0, k.value.x * scale.x);
        irSetValueF32(kf.value, 1, k.value.y * scale.y);
        irSetValueF32(kf.value, 2, k.value.z * scale.z);
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = target;
    track.prop = propId;
    track.type = static_cast<uint8_t>(ValueType::Vec3);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(kept.size());
    ctx.timeline.tracks.push_back(track);
    return true;
}

/// Layer visibility (docs/design/11-lottie-mapping.md §11.5): `ip`/`op` are in the ENCLOSING
/// composition's frame axis (not scaled by this layer's own `st`/`sr`), so `outer` -- not this
/// layer's ownMap -- is used to convert them.
void buildVisibleTrack(BuildCtx& ctx, const LottieLayer& layer, uint32_t contentIdx, TimeMap outer,
                       uint8_t& baseVisible) {
    float ipAbs = outer.apply(layer.ip);
    float opAbs = outer.apply(layer.op);
    bool prependZero = ipAbs > ctx.doc.ip;
    bool needsTrack = prependZero || opAbs < ctx.doc.op;
    if (!needsTrack) {
        baseVisible = 1;
        return;
    }
    baseVisible = prependZero ? 0 : 1;

    std::vector<IrKeyframe> kfs;
    auto pushKf = [&](float sec, uint32_t value) {
        IrKeyframe kf{};
        kf.time = sec;
        kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
        irSetValueU32(kf.value, 0, value);
        kfs.push_back(kf);
    };
    if (prependZero) {
        pushKf(0.0f, 0);
    }
    pushKf(ctx.doc.fr != 0.0f ? (ipAbs - ctx.doc.ip) / ctx.doc.fr : 0.0f, 1);
    pushKf(ctx.doc.fr != 0.0f ? (opAbs - ctx.doc.ip) / ctx.doc.fr : 0.0f, 0);

    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (const IrKeyframe& kf : kfs) {
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = contentIdx;
    track.prop = prop::Visible;
    track.type = static_cast<uint8_t>(ValueType::Enum);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(kfs.size());
    ctx.timeline.tracks.push_back(track);
}

/// TransformGroup: `ks`'s a/p/s/r/sk/sa. `map` is this layer's own frame-to-root-second mapping
/// (already folds in this layer's `st`/`sr`, see buildLayerNode).
IrSceneNode buildTransformNode(BuildCtx& ctx, const LottieLayer& layer, uint32_t parentIdx, uint32_t selfIdx,
                               TimeMap map, StringId nameId) {
    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Group);
    n.visible = 1;
    n.blend = 0;
    n.maskMode = 0;
    n.parent = parentIdx;
    n.kindIndex = 0;
    n.opacity = 1.0f;
    n.anchor = firstOrStatic(layer.ks.anchor);
    if (layer.ks.splitPosition) {
        n.translate = Vec2{firstOrStatic(layer.ks.posX), firstOrStatic(layer.ks.posY)};
    } else {
        n.translate = firstOrStatic(layer.ks.position);
    }
    Vec2 rawScale = firstOrStatic(layer.ks.scale);
    n.scale = Vec2{rawScale.x / 100.0f, rawScale.y / 100.0f};
    n.rotation = firstOrStatic(layer.ks.rotation);
    n.skew = Vec2{firstOrStatic(layer.ks.skew), firstOrStatic(layer.ks.skewAxis)};
    n.name = nameId;
    n.autoOrient = layer.autoOrient ? 1 : 0;  // T-228 (31 §31.7.2).

    // T-47b: 3D layer fields (docs/design/19-3d-space.md §19.2-§19.3). z is negated on import;
    // rotationX/Y are negated (the rotation axes flip along with z); rotation (Z) and orientation's
    // z component are not negated; orientation's x/y are.
    if (layer.ddd) {
        n.is3D = 1;
        Vec3 pos3 = layer.ks.splitPosition ? Vec3{firstOrStatic(layer.ks.posX), firstOrStatic(layer.ks.posY),
                                                  firstOrStatic(layer.ks.posZ)}
                                           : firstOrStatic(layer.ks.position3);
        n.translateZ = -pos3.z;
        n.anchorZ = -firstOrStatic(layer.ks.anchorZ);
        n.rotationX = -firstOrStatic(layer.ks.rx);
        n.rotationY = -firstOrStatic(layer.ks.ry);
        Vec3 orient0 = firstOrStatic(layer.ks.orientation);
        n.orientation = Vec3{-orient0.x, -orient0.y, orient0.z};
    }

    if (layer.ks.anchor.animated) {
        buildVec2Track(ctx, layer.ks.anchor, selfIdx, prop::Anchor, map, layer, "Anchor");
    }
    if (layer.ks.splitPosition) {
        if (layer.ks.posX.animated) {
            buildFloatTrack(ctx, layer.ks.posX, selfIdx, prop::TranslateX, map, layer, "TranslateX");
        }
        if (layer.ks.posY.animated) {
            buildFloatTrack(ctx, layer.ks.posY, selfIdx, prop::TranslateY, map, layer, "TranslateY");
        }
        if (layer.ddd && layer.ks.posZ.animated) {
            buildFloatTrack(ctx, layer.ks.posZ, selfIdx, prop::TranslateZ, map, layer, "TranslateZ", -1.0f);
        }
    } else if (layer.ddd) {
        // T-47b: Translate3 (Vec3, PropId 20) replaces Translate (11) for a 3D layer's combined
        // position, carrying the z-negated value and spatial tangents.
        if (layer.ks.position3.animated) {
            buildVec3Track(ctx, layer.ks.position3, selfIdx, prop::Translate3, map, layer, "Translate3",
                           Vec3{1.0f, 1.0f, -1.0f});
        }
    } else if (layer.ks.position.animated) {
        buildVec2Track(ctx, layer.ks.position, selfIdx, prop::Translate, map, layer, "Translate");
    }
    if (layer.ks.scale.animated) {
        buildVec2Track(ctx, layer.ks.scale, selfIdx, prop::Scale, map, layer, "Scale", 0.01f);
    }
    if (layer.ks.rotation.animated) {
        buildFloatTrack(ctx, layer.ks.rotation, selfIdx, prop::Rotation, map, layer, "Rotation");
    }
    if (layer.ks.skew.animated) {
        buildFloatTrack(ctx, layer.ks.skew, selfIdx, prop::SkewX, map, layer, "SkewX");
    }
    if (layer.ks.skewAxis.animated) {
        buildFloatTrack(ctx, layer.ks.skewAxis, selfIdx, prop::SkewY, map, layer, "SkewY");
    }
    if (layer.ddd) {
        if (layer.ks.anchorZ.animated) {
            buildFloatTrack(ctx, layer.ks.anchorZ, selfIdx, prop::AnchorZ, map, layer, "AnchorZ", -1.0f);
        }
        if (layer.ks.rx.animated) {
            buildFloatTrack(ctx, layer.ks.rx, selfIdx, prop::RotationX, map, layer, "RotationX", -1.0f);
        }
        if (layer.ks.ry.animated) {
            buildFloatTrack(ctx, layer.ks.ry, selfIdx, prop::RotationY, map, layer, "RotationY", -1.0f);
        }
        if (layer.ks.orientation.animated) {
            buildVec3Track(ctx, layer.ks.orientation, selfIdx, prop::Orientation, map, layer, "Orientation",
                           Vec3{-1.0f, -1.0f, 1.0f});
        }
    }
    return n;
}

/// ContentGroup: opacity, blend, visible track, then the layer's content (attached separately by
/// the caller). `map` is used for the opacity track (subject to this layer's own st/sr); `outer`
/// is used for the visible track (ip/op are not).
IrSceneNode buildContentNode(BuildCtx& ctx, const LottieLayer& layer, uint32_t transformIdx, uint32_t selfIdx,
                             TimeMap map, TimeMap outer, StringId nameId) {
    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Group);
    n.blend = 0;
    n.maskMode = 0;
    n.parent = transformIdx;
    n.kindIndex = 0;
    n.anchor = Vec2{0, 0};
    n.translate = Vec2{0, 0};
    n.scale = Vec2{1, 1};
    n.rotation = 0;
    n.skew = Vec2{0, 0};
    n.name = nameId;

    n.opacity = firstOrStatic(layer.ks.opacity) / 100.0f;
    if (layer.ks.opacity.animated) {
        buildFloatTrack(ctx, layer.ks.opacity, selfIdx, prop::Opacity, map, layer, "Opacity", 0.01f);
    }

    if (layer.blendMode >= 0 && layer.blendMode <= 16) {
        n.blend = static_cast<uint8_t>(layer.blendMode);
    } else {
        n.blend = 0;
        ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": unsupported blend mode " +
                                    std::to_string(layer.blendMode) + "; using Normal");
    }

    if (layer.hidden) {
        n.visible = 0;
    } else {
        uint8_t baseVisible = 1;
        buildVisibleTrack(ctx, layer, selfIdx, outer, baseVisible);
        n.visible = baseVisible;
    }
    return n;
}

// -------------------------------------------------------------------------------------------
// T-55: layer `ef[]` -> SCEN effects pool + effectParam(i, p) Tracks. See
// docs/design/11-lottie-mapping.md, docs/design/18-effects.md §18.9.
// -------------------------------------------------------------------------------------------

/// Fills `ir` from a Gaussian Blur (`ty:29`) effect group's static (first-or-only) values. Always
/// succeeds (ty:29 has no further validity constraints).
void fillBlurEffect(const LottieEffect& eff, IrEffect& ir) {
    ir.kind = static_cast<uint8_t>(EffectKind::Blur);
    ir.enabled = 1;
    float radius = firstOrStatic(eff.blurriness) / 2.0f;  // AE blurriness ~ diameter (18.9's note).
    float radiusX = radius;
    float radiusY = radius;
    if (eff.blurDimensions == 2) {  // Horizontal only.
        radiusY = 0.0f;
    } else if (eff.blurDimensions == 3) {  // Vertical only.
        radiusX = 0.0f;
    }
    ir.params[0] = radiusX;
    ir.params[1] = radiusY;
}

/// Builds the Blur radiusX/radiusY Tracks (effectParam(i,1)/effectParam(i,2)) from an animated
/// `blurriness`; only the axis(es) selected by `blurDimensions` get a track (18.9: "dimensions 非 1
/// 時只產生對應軸").
void buildBlurTracks(BuildCtx& ctx, const LottieLayer& layer, const LottieEffect& eff, TimeMap map,
                     uint32_t target, uint32_t effectIndex) {
    if (!eff.blurriness.animated) {
        return;
    }
    if (eff.blurDimensions != 3) {  // both (1) or horizontal (2): radiusX.
        buildFloatTrack(ctx, eff.blurriness, target, effectParam(effectIndex, 1), map, layer, "Blur radiusX",
                        0.5f);
    }
    if (eff.blurDimensions != 2) {  // both (1) or vertical (3): radiusY.
        buildFloatTrack(ctx, eff.blurriness, target, effectParam(effectIndex, 2), map, layer, "Blur radiusY",
                        0.5f);
    }
}

/// Fills `ir` from a Drop Shadow (`ty:25`) effect group's static (first-or-only) values, per
/// 18.9's formula: `offsetX = dist*sin(dir)`, `offsetY = -dist*cos(dir)`, `blur = softness`,
/// `spread = 0`, `opacity = op/255`.
void fillShadowEffect(const LottieEffect& eff, IrEffect& ir) {
    ir.kind = static_cast<uint8_t>(EffectKind::DropShadow);
    ir.enabled = 1;
    float dirRad = firstOrStatic(eff.shadowDirection) * (3.14159265358979323846f / 180.0f);
    float dist = firstOrStatic(eff.shadowDistance);
    ir.params[0] = dist * std::sin(dirRad);
    ir.params[1] = -dist * std::cos(dirRad);
    ir.params[2] = firstOrStatic(eff.shadowSoftness);
    ir.params[3] = 0.0f;
    ir.params[4] = firstOrStatic(eff.shadowOpacity) / 255.0f;
    ir.colors[0] = firstOrStatic(eff.shadowColor);
}

/// Linearly samples `av` at raw Lottie frame `frame` (18.9: the distance/direction union sampling
/// ignores each property's own Bezier/hold easing and always interpolates linearly). `av.keys` is
/// assumed sorted by frame (true of every AnimatedValue parsed from JSON, T-14's "關鍵幀時間必須嚴格
/// 遞增" convention); a `frame` before the first or after the last keyframe clamps to that end.
float sampleLinearAtFrame(const AnimatedValue<float>& av, float frame) {
    if (!av.animated || av.keys.empty()) {
        return av.value;
    }
    if (frame <= av.keys.front().frame) {
        return av.keys.front().value;
    }
    if (frame >= av.keys.back().frame) {
        return av.keys.back().value;
    }
    for (size_t i = 0; i + 1 < av.keys.size(); ++i) {
        const LottieKeyframe<float>& a = av.keys[i];
        const LottieKeyframe<float>& b = av.keys[i + 1];
        if (frame >= a.frame && frame <= b.frame) {
            if (b.frame == a.frame) {
                return a.value;
            }
            float t = (frame - a.frame) / (b.frame - a.frame);
            return a.value + (b.value - a.value) * t;
        }
    }
    return av.keys.back().value;  // Unreachable given the clamps above; defensive.
}

/// Builds the DropShadow offset Track (effectParam(i,18), a composite Vec2): 18.9's rule is that
/// distance/direction share one Vec2 offset track, sampled on the union of their own keyframe times
/// (linear interpolation, see sampleLinearAtFrame) whenever either is animated.
void buildShadowOffsetTrack(BuildCtx& ctx, const LottieEffect& eff, TimeMap map, uint32_t target,
                            uint32_t effectIndex) {
    if (!eff.shadowDistance.animated && !eff.shadowDirection.animated) {
        return;
    }
    std::vector<float> times;
    if (eff.shadowDistance.animated) {
        for (const LottieKeyframe<float>& k : eff.shadowDistance.keys) {
            times.push_back(k.frame);
        }
    }
    if (eff.shadowDirection.animated) {
        for (const LottieKeyframe<float>& k : eff.shadowDirection.keys) {
            times.push_back(k.frame);
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    if (times.size() < 2) {
        return;  // A single distinct time is a static value, not a track.
    }

    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (float f : times) {
        float dist = sampleLinearAtFrame(eff.shadowDistance, f);
        float dirRad = sampleLinearAtFrame(eff.shadowDirection, f) * (3.14159265358979323846f / 180.0f);
        IrKeyframe kf{};
        kf.time = mapFrameToSec(ctx, map, f);
        kf.easing.kind = static_cast<uint8_t>(EasingKind::Linear);
        irSetValueF32(kf.value, 0, dist * std::sin(dirRad));
        irSetValueF32(kf.value, 1, -dist * std::cos(dirRad));
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = target;
    track.prop = effectParam(effectIndex, prop::kEffectParamOffset);
    track.type = static_cast<uint8_t>(ValueType::Vec2);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(times.size());
    ctx.timeline.tracks.push_back(track);
}

/// Builds every animated Track for one already-emitted Drop Shadow effect: the combined offset
/// (effectParam(i,18)), softness (p=3), opacity (p=5, scaled 0..255 -> 0..1) and color (p=16).
void buildShadowTracks(BuildCtx& ctx, const LottieLayer& layer, const LottieEffect& eff, TimeMap map,
                       uint32_t target, uint32_t effectIndex) {
    buildShadowOffsetTrack(ctx, eff, map, target, effectIndex);
    if (eff.shadowSoftness.animated) {
        buildFloatTrack(ctx, eff.shadowSoftness, target, effectParam(effectIndex, 3), map, layer,
                        "Shadow softness");
    }
    if (eff.shadowOpacity.animated) {
        buildFloatTrack(ctx, eff.shadowOpacity, target, effectParam(effectIndex, 5), map, layer,
                        "Shadow opacity", 1.0f / 255.0f);
    }
    if (eff.shadowColor.animated) {
        buildColorTrack(ctx, eff.shadowColor, target, effectParam(effectIndex, prop::kEffectParamColor0), map,
                        layer, "Shadow color");
    }
}

/// Builds `layer.effects` (`ef[]`, 18.9) onto `contentIdx`'s IrSceneNode: appends one IrEffect per
/// supported entry (ty:29 Blur, ty:25 DropShadow, in `ef[]` order, contiguous in `ctx.scene.effects`
/// so a single offset/count pair covers them) plus that entry's animated Tracks, and warns once per
/// unsupported `ty`. Must run after `contentIdx`'s IrSceneNode has been pushed into `ctx.scene.nodes`
/// (mutates it in place by index, not by a held reference, since later sibling/content nodes are
/// still to be appended to that same vector).
void buildLayerEffects(BuildCtx& ctx, const LottieLayer& layer, TimeMap map, uint32_t contentIdx) {
    if (layer.effects.empty()) {
        return;
    }
    uint32_t offset = 0;
    uint32_t count = 0;
    for (const LottieEffect& eff : layer.effects) {
        IrEffect ir{};
        ir.shader = kInvalidAsset;
        bool built = false;
        if (eff.ty == 29) {
            fillBlurEffect(eff, ir);
            built = true;
        } else if (eff.ty == 25) {
            fillShadowEffect(eff, ir);
            built = true;
        } else {
            ctx.sink.warn(ctx.file,
                          lottieLayerDesc(layer) + ": unsupported effect ty " + std::to_string(eff.ty));
        }
        if (!built) {
            continue;
        }
        if (count >= 16) {  // 18.2: "每節點 16 個效果" pool cap.
            ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": more than 16 effects; ignoring the rest");
            break;
        }
        uint32_t idx = IrWriter::addSceneEffect(ctx.scene, ir);
        if (count == 0) {
            offset = idx;
        }
        uint32_t effectIndex = count;
        ++count;
        if (eff.ty == 29) {
            buildBlurTracks(ctx, layer, eff, map, contentIdx, effectIndex);
        } else {
            buildShadowTracks(ctx, layer, eff, map, contentIdx, effectIndex);
        }
    }
    ctx.scene.nodes[contentIdx].effectOffset = offset;
    ctx.scene.nodes[contentIdx].effectCount = count;
}

/// Solid layer content: a `sw`x`sh` rectangle Path (Move + 3 Line + Close = 5 verbs), solid-filled
/// with `sc`. See docs/tasks/T-14.md "內容".
void appendSolidPath(BuildCtx& ctx, const LottieLayer& layer, uint32_t parentIdx, StringId nameId) {
    float w = static_cast<float>(layer.solidW);
    float h = static_cast<float>(layer.solidH);

    uint32_t verbOffset = static_cast<uint32_t>(ctx.scene.verbPool.size());
    ctx.scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Move));
    ctx.scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Line));
    ctx.scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Line));
    ctx.scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Line));
    ctx.scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Close));

    uint32_t pointOffset = static_cast<uint32_t>(ctx.scene.floatPool.size());
    const float pts[8] = {0, 0, w, 0, w, h, 0, h};
    for (float p : pts) {
        ctx.scene.floatPool.push_back(p);
    }

    IrPathData path{};
    path.verbOffset = verbOffset;
    path.verbCount = 5;
    path.pointOffset = pointOffset;
    path.pointCount = 8;
    path.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    path.fillKind = static_cast<uint8_t>(PaintKind::Solid);
    path.strokeKind = static_cast<uint8_t>(PaintKind::None);
    path.cap = 0;
    path.join = 0;
    path.fillColor = layer.solidColor;
    path.strokeColor = Color{0, 0, 0, 1};
    path.trimStart = 0;
    path.trimEnd = 1;
    path.trimOffset = 0;

    uint32_t pathIndex = static_cast<uint32_t>(ctx.scene.paths.size());
    ctx.scene.paths.push_back(path);

    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Path);
    node.visible = 1;
    node.parent = parentIdx;
    node.kindIndex = pathIndex;
    node.opacity = 1;
    node.scale = Vec2{1, 1};
    node.name = nameId;
    ctx.scene.nodes.push_back(node);
}

/// Image layer content (T-16): resolves `layer.refId` into an ASST entry and the asset's `w`/`h`.
void appendImageNode(BuildCtx& ctx, const LottieLayer& layer, uint32_t parentIdx, StringId nameId) {
    IrImageData img{};
    img.asset = kInvalidAsset;
    img.width = 0;
    img.height = 0;
    AssetId assetId = resolveImageAsset(ctx, layer.refId, layer);
    if (assetId != kInvalidAsset) {
        img.asset = assetId;
        auto it = ctx.doc.assets.find(layer.refId);
        if (it != ctx.doc.assets.end()) {
            img.width = static_cast<float>(it->second.w);
            img.height = static_cast<float>(it->second.h);
        }
    }
    uint32_t imgIndex = static_cast<uint32_t>(ctx.scene.images.size());
    ctx.scene.images.push_back(img);

    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Image);
    node.visible = 1;
    node.parent = parentIdx;
    node.kindIndex = imgIndex;
    node.opacity = 1;
    node.scale = Vec2{1, 1};
    node.name = nameId;
    ctx.scene.nodes.push_back(node);
}

/// Camera layer (`ty:13`, T-47b). See docs/design/19-3d-space.md §19.4. A camera is never drawn
/// and has no opacity/masks/content, so it gets a single Camera scene node instead of the usual
/// TransformGroup+ContentGroup pair; `ip`/`op` are ignored (no Visible track).
uint32_t buildCameraNode(BuildCtx& ctx, const LottieLayer& layer, uint32_t parentIdx, TimeMap map,
                         StringId nameId) {
    uint32_t idx = static_cast<uint32_t>(ctx.scene.nodes.size());
    ctx.scene.nodes.push_back(IrSceneNode{});

    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Camera);
    n.visible = layer.hidden ? 0 : 1;
    n.blend = 0;
    n.maskMode = 0;
    n.parent = parentIdx;
    n.opacity = 1.0f;
    n.scale = Vec2{1, 1};
    n.name = nameId;

    // Position (z negated, §19.2). Written both to the scene node's translate/translateZ (which
    // the runtime reads for the node's own transform, core/src/camera.cpp: eye = {c.translate,
    // c.translateZ}) and to IrCameraData.position (T-47a's field, filled in here per T-47b).
    Vec3 pos3 = layer.ks.splitPosition ? Vec3{firstOrStatic(layer.ks.posX), firstOrStatic(layer.ks.posY),
                                              firstOrStatic(layer.ks.posZ)}
                                       : firstOrStatic(layer.ks.position3);
    n.translate = Vec2{pos3.x, pos3.y};
    n.translateZ = -pos3.z;

    if (!layer.cameraHasTarget) {
        // Single-node camera (no point of interest): orientation from rx/ry/rz (camera.cpp only
        // reads these when useTarget == 0).
        n.rotation = firstOrStatic(layer.ks.rotation);
        n.rotationX = -firstOrStatic(layer.ks.rx);
        n.rotationY = -firstOrStatic(layer.ks.ry);
    }

    IrCameraData cam{};
    cam.kind = 0;  // Lottie cameras are always perspective.
    cam.useTarget = layer.cameraHasTarget ? 1 : 0;
    cam.zoom = firstOrStatic(layer.cameraZoom);
    cam.position = Vec3{pos3.x, pos3.y, -pos3.z};
    if (layer.cameraHasTarget) {
        Vec3 poi0 = firstOrStatic(layer.cameraPointOfInterest);
        cam.target = Vec3{poi0.x, poi0.y, -poi0.z};
    }
    cam.nearZ = 1.0f;
    cam.farZ = 100000.0f;
    n.kindIndex = IrWriter::addSceneCamera(ctx.scene, cam);

    ctx.scene.nodes[idx] = n;

    if (layer.cameraZoom.animated) {
        buildFloatTrack(ctx, layer.cameraZoom, idx, prop::CameraZoom, map, layer, "CameraZoom");
    }
    if (layer.cameraHasTarget) {
        buildVec3Track(ctx, layer.cameraPointOfInterest, idx, prop::CameraTarget, map, layer, "CameraTarget",
                       Vec3{1.0f, 1.0f, -1.0f});
    } else {
        if (layer.ks.rotation.animated) {
            buildFloatTrack(ctx, layer.ks.rotation, idx, prop::Rotation, map, layer, "Rotation");
        }
        if (layer.ks.rx.animated) {
            buildFloatTrack(ctx, layer.ks.rx, idx, prop::RotationX, map, layer, "RotationX", -1.0f);
        }
        if (layer.ks.ry.animated) {
            buildFloatTrack(ctx, layer.ks.ry, idx, prop::RotationY, map, layer, "RotationY", -1.0f);
        }
    }
    if (layer.ks.splitPosition) {
        if (layer.ks.posX.animated) {
            buildFloatTrack(ctx, layer.ks.posX, idx, prop::TranslateX, map, layer, "TranslateX");
        }
        if (layer.ks.posY.animated) {
            buildFloatTrack(ctx, layer.ks.posY, idx, prop::TranslateY, map, layer, "TranslateY");
        }
        if (layer.ks.posZ.animated) {
            buildFloatTrack(ctx, layer.ks.posZ, idx, prop::TranslateZ, map, layer, "TranslateZ", -1.0f);
        }
    } else if (layer.ks.position3.animated) {
        buildVec3Track(ctx, layer.ks.position3, idx, prop::Translate3, map, layer, "Translate3",
                       Vec3{1.0f, 1.0f, -1.0f});
    }

    return idx;
}

void processLayers(BuildCtx& ctx, const std::vector<LottieLayer>& layers, uint32_t scopeGroupIdx,
                   TimeMap outer, int depth);

/// Builds one layer's TransformGroup + ContentGroup (+ content), returning the TransformGroup's
/// scene-local node index (or, for a camera layer, the Camera node's index). `outer` is the
/// enclosing composition's frame mapping (see TimeMap); this layer's own `st`/`sr` are folded in
/// here to produce `ownMap`. T-258b (D6): kInvalidNode, building nothing, once the build has made
/// kMaxLottieLayerInstances layer instances (the first one past it reports the error).
uint32_t buildLayerNode(BuildCtx& ctx, const LottieLayer& layer, uint32_t parentGroupIdx, TimeMap outer,
                        int depth) {
    if (ctx.expansionExceeded) {
        return kInvalidNode;
    }
    if (++ctx.layerInstances > kMaxLottieLayerInstances) {
        ctx.expansionExceeded = true;
        ctx.sink.error(ctx.file,
                       "lottie: expansion exceeds " + std::to_string(kMaxLottieLayerInstances) + " layers");
        return kInvalidNode;
    }
    TimeMap ownMap{outer.scale * layer.sr, outer.apply(layer.st)};

    StringId nameId = ctx.writer.intern(layer.name);

    if (layer.type == 13) {
        return buildCameraNode(ctx, layer, parentGroupIdx, ownMap, nameId);
    }

    StringId contentNameId = ctx.writer.intern(layer.name + "#content");

    uint32_t tIdx = static_cast<uint32_t>(ctx.scene.nodes.size());
    ctx.scene.nodes.push_back(IrSceneNode{});
    uint32_t cIdx = static_cast<uint32_t>(ctx.scene.nodes.size());
    ctx.scene.nodes.push_back(IrSceneNode{});

    ctx.scene.nodes[tIdx] = buildTransformNode(ctx, layer, parentGroupIdx, tIdx, ownMap, nameId);
    ctx.scene.nodes[cIdx] = buildContentNode(ctx, layer, tIdx, cIdx, ownMap, outer, contentNameId);
    // T-55: ef[] effects (18.9) attach to the ContentGroup, alongside its opacity/blend, so the
    // effect wraps the layer's whole rendered content (not just the transform).
    buildLayerEffects(ctx, layer, ownMap, cIdx);

    if (!layer.hidden) {
        // masksProperties (T-16): must precede the layer's own content in ctx.scene.nodes so it
        // ends up as the first child of the ContentGroup (see docs/design/03-scene-and-properties.md
        // §3.7: a Mask node affects the siblings that follow it).
        buildLayerMasks(ctx, layer, cIdx, ownMap);

        switch (layer.type) {
            case 1:  // Solid
                appendSolidPath(ctx, layer, cIdx, ctx.writer.intern(layer.name + "#rect"));
                break;
            case 2:  // Image
                appendImageNode(ctx, layer, cIdx, ctx.writer.intern(layer.name + "#image"));
                break;
            case 0: {  // Precomp
                if (!ctx.warnedPrecompClip) {
                    ctx.sink.warn(ctx.file, "precomp clipping not applied");
                    ctx.warnedPrecompClip = true;
                }
                if (depth + 1 > 32) {
                    ctx.sink.error(ctx.file, lottieLayerDesc(layer) + ": precomp nesting depth exceeds 32");
                    break;
                }
                auto it = ctx.doc.assets.find(layer.refId);
                if (it == ctx.doc.assets.end()) {
                    ctx.sink.error(ctx.file, lottieLayerDesc(layer) + ": precomp asset \"" + layer.refId +
                                                 "\" not found");
                    break;
                }
                processLayers(ctx, it->second.layers, cIdx, ownMap, depth + 1);
                break;
            }
            case 4:  // Shape
                buildShapeLayerContent(ctx, layer, cIdx, ownMap);
                break;
            case 3:   // Null: no content.
            default:  // Unreachable (unsupported types are filtered out in lottie_json.cpp).
                break;
        }
    }

    return tIdx;
}

/// Resolves track matte (T-16) `tt`/`tp` pairs within one composition's layer array. For each
/// layer index `i` with `tt` in [1,4], `matteTarget[i]` is the array position of its matte source
/// (`tp`'s target if present, else the immediately preceding array entry, per docs/tasks/T-16.md
/// "Track matte") or -1 if none was found (warned). `consumed[pos]` marks a layer that is a matte
/// source for at least one other layer: it must never be built as an ordinary top-level sibling
/// (its subtree is built once per consumer instead, see buildMatteFor below).
struct MatteResolution {
    std::vector<int> matteTarget;
    std::vector<uint8_t> consumed;
};

MatteResolution resolveTrackMatte(BuildCtx& ctx, const std::vector<LottieLayer>& layers,
                                  const std::unordered_map<int, size_t>& indToPos) {
    MatteResolution res;
    res.matteTarget.assign(layers.size(), -1);
    res.consumed.assign(layers.size(), 0);

    for (size_t i = 0; i < layers.size(); ++i) {
        const LottieLayer& a = layers[i];
        if (a.matteType < 1 || a.matteType > 4) {
            continue;
        }
        int bPos = -1;
        if (a.matteParent >= 0) {
            auto it = indToPos.find(a.matteParent);
            if (it != indToPos.end()) {
                bPos = static_cast<int>(it->second);
            }
        } else if (i > 0) {
            bPos = static_cast<int>(i - 1);
        }
        if (bPos < 0 || static_cast<size_t>(bPos) == i) {
            ctx.sink.warn(ctx.file, lottieLayerDesc(a) + ": track matte layer not found; drawing normally");
            continue;
        }
        const LottieLayer& b = layers[static_cast<size_t>(bPos)];
        if (b.td != 1) {
            ctx.sink.warn(ctx.file, lottieLayerDesc(a) + ": track matte source " + lottieLayerDesc(b) +
                                        " is missing \"td\":1");
        }
        res.matteTarget[i] = bPos;
        res.consumed[static_cast<size_t>(bPos)] = 1;
    }
    return res;
}

/// T-47b: within one composition's layer array, at most one camera (`ty:13`) layer is built --
/// the one with the smallest `ind` (docs/design/19-3d-space.md §19.4: "同一父節點下多台攝影機時取
/// 文件順序第一台"). Every other camera layer in the array is skipped entirely (never built) and
/// warned about once.
std::vector<uint8_t> resolveCameraSelection(BuildCtx& ctx, const std::vector<LottieLayer>& layers) {
    std::vector<uint8_t> skip(layers.size(), 0);
    int bestPos = -1;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].type != 13) {
            continue;
        }
        if (bestPos < 0 || layers[i].index < layers[static_cast<size_t>(bestPos)].index) {
            bestPos = static_cast<int>(i);
        }
    }
    if (bestPos < 0) {
        return skip;
    }
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].type == 13 && static_cast<int>(i) != bestPos) {
            skip[i] = 1;
            ctx.sink.warn(ctx.file, lottieLayerDesc(layers[i]) +
                                        ": multiple cameras in this composition; "
                                        "ignoring (using " +
                                        lottieLayerDesc(layers[static_cast<size_t>(bestPos)]) + ")");
        }
    }
    return skip;
}

/// T-257: the longest `parent` chain processLayers() follows; a longer one is an error.
constexpr size_t kMaxParentChain = 256;

/// Processes one composition's layer array: resolves `parent` chains (building ancestors first,
/// detecting cycles) and appends siblings in reverse array order so that
/// `layers[0]` (Lottie's topmost) ends up last among its siblings (drawn on top). See
/// docs/tasks/T-14.md "圖層順序" and "parent 循環". Track matte (T-16, see resolveTrackMatte) is
/// resolved first: a matted layer gets a Mask(mode) node holding its matte source's whole subtree
/// inserted immediately before it; the matte source itself is never built as an ordinary sibling.
void processLayers(BuildCtx& ctx, const std::vector<LottieLayer>& layers, uint32_t scopeGroupIdx,
                   TimeMap outer, int depth) {
    std::unordered_map<int, size_t> indToPos;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].index >= 0) {
            indToPos.emplace(layers[i].index, i);
        }
    }

    MatteResolution matte = resolveTrackMatte(ctx, layers, indToPos);
    std::vector<uint8_t> skipCamera = resolveCameraSelection(ctx, layers);

    std::vector<uint32_t> builtIdx(layers.size(), kInvalidNode);
    LottieParentChainWalker walker(
        layers.size(),
        [&](size_t pos) -> std::ptrdiff_t {
            if (layers[pos].parent < 0) {
                return -1;
            }
            auto it = indToPos.find(layers[pos].parent);
            // A dangling parent reference (points at a layer that was filtered out, e.g. a text
            // layer) is not covered by the spec; conservatively attaches to the scope root.
            return it != indToPos.end() ? static_cast<std::ptrdiff_t>(it->second) : -1;
        },
        kMaxParentChain);
    bool chainTooLongReported = false;

    // Builds one layer (and its track matte) under `parentGroup`; returns its node index.
    auto buildOne = [&](size_t pos, uint32_t parentGroup) -> uint32_t {
        const LottieLayer& layer = layers[pos];
        if (matte.matteTarget[pos] >= 0) {
            // Insert Mask(mode){matte source subtree} as the sibling immediately before `layer`
            // (see docs/design/03-scene-and-properties.md §3.7): pushed into ctx.scene.nodes now,
            // before buildLayerNode(layer, ...) below pushes layer's own nodes.
            uint32_t maskNodeIdx = appendTrackMatteMaskNode(ctx, parentGroup, layer.matteType,
                                                            ctx.writer.intern(layer.name + "#matte"));
            // The matte source's own `parent` chain is not honored here: it is pulled out of the
            // ordinary layer tree entirely to become this Mask node's subtree (not covered by the
            // spec for a matte source that also has its own `parent`; conservative choice).
            buildLayerNode(ctx, layers[static_cast<size_t>(matte.matteTarget[pos])], maskNodeIdx, outer,
                           depth);
        }
        uint32_t idx = buildLayerNode(ctx, layer, parentGroup, outer, depth);
        builtIdx[pos] = idx;
        return idx;
    };

    // T-257: builds `pos` after its ancestors, without recursion (LottieParentChainWalker); a
    // cycle or a chain longer than kMaxParentChain is an error, and the chain is built top down.
    auto ensureBuilt = [&](size_t pos) -> uint32_t {
        const LottieParentChain chain = walker.walk(pos);
        if (chain.cycleAt >= 0) {
            ctx.sink.error(ctx.file, lottieLayerDesc(layers[static_cast<size_t>(chain.cycleAt)]) +
                                         ": parent cycle detected");
        }
        if (chain.tooLong && !chainTooLongReported) {
            chainTooLongReported = true;
            ctx.sink.error(ctx.file, lottieLayerDesc(layers[pos]) + ": parent chain longer than " +
                                         std::to_string(kMaxParentChain) + " layers");
        }
        uint32_t parentGroup = scopeGroupIdx;
        if (chain.attachTo >= 0 && builtIdx[static_cast<size_t>(chain.attachTo)] != kInvalidNode) {
            parentGroup = builtIdx[static_cast<size_t>(chain.attachTo)];
        }
        for (size_t at : chain.order) {
            const uint32_t idx = buildOne(at, parentGroup);
            parentGroup = idx != kInvalidNode ? idx : scopeGroupIdx;
        }
        return builtIdx[pos];
    };

    for (size_t i = layers.size(); i-- > 0;) {
        if (ctx.expansionExceeded) {
            return;  // T-258b (D6): the build stops.
        }
        if (matte.consumed[i]) {
            continue;  // Built only as a matte source (possibly more than once), never as a sibling.
        }
        if (skipCamera[i]) {
            continue;  // T-47b: a non-selected extra camera in this composition; never built.
        }
        ensureBuilt(i);
    }
}

}  // namespace

bool buildLottieScene(const LottieDoc& doc, const LottieOptions& options, const std::string& file,
                      IrWriter& writer, DiagnosticSink& sink, SceneBuilder& scene,
                      TimelineBuilder& timeline) {
    scene.name = writer.intern(options.sourceName);
    scene.width = doc.w;
    scene.height = doc.h;

    timeline.name = writer.intern(options.sourceName);
    timeline.duration = doc.fr != 0.0f ? (doc.op - doc.ip) / doc.fr : 0.0f;
    timeline.frameRate = doc.fr;

    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = kInvalidNode;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    scene.nodes.push_back(root);

    BuildCtx ctx{doc, options, file, writer, sink, scene, timeline, false, {}};
    processLayers(ctx, doc.layers, 0, TimeMap{1.0f, 0.0f}, 0);

    for (const LottieMarker& m : doc.markers) {
        IrMarker marker{};
        marker.name = writer.intern(m.name);
        marker.start = doc.fr != 0.0f ? (m.time - doc.ip) / doc.fr : 0.0f;
        marker.duration = doc.fr != 0.0f ? m.duration / doc.fr : 0.0f;
        timeline.markers.push_back(marker);
    }

    return !sink.hasErrors() && !(options.strict && sink.warningCount() > 0);
}

}  // namespace xt::compile
