#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <utility>

#include "lottie_geometry.h"
#include "lottie_model.h"
#include "lottie_shapes.h"
#include "shape_modifiers.h"  // core/src: kMaxGeneratedPoints, kMaxRepeatCopies (T-258b).
#include "xt/node.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/value.h"

/// LottieShapeItem tree -> SceneBuilder nodes + Tracks. See docs/design/11-lottie-mapping.md
/// §11.3.2-11.3.3, docs/tasks/T-15.md "建構規則" / "樣式映射".

namespace xt::compile {

namespace {

// -------------------------------------------------------------------------------------------
// Small pool/node helpers.
// -------------------------------------------------------------------------------------------

uint32_t appendNode(BuildCtx& ctx, const IrSceneNode& node) {
    uint32_t idx = static_cast<uint32_t>(ctx.scene.nodes.size());
    ctx.scene.nodes.push_back(node);
    return idx;
}

IrSceneNode defaultGroupNode(uint32_t parent, StringId name) {
    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Group);
    n.visible = 1;
    n.parent = parent;
    n.kindIndex = 0;
    n.opacity = 1.0f;
    n.anchor = Vec2{0, 0};
    n.translate = Vec2{0, 0};
    n.scale = Vec2{1, 1};
    n.rotation = 0.0f;
    n.skew = Vec2{0, 0};
    n.name = name;
    return n;
}

struct PoolRange {
    uint32_t verbOffset = 0, verbCount = 0, pointOffset = 0, pointCount = 0;
};

PoolRange appendSceneGeometry(BuildCtx& ctx, const GeneratedPath& gp) {
    PoolRange r;
    r.verbOffset = static_cast<uint32_t>(ctx.scene.verbPool.size());
    for (uint8_t v : gp.verbs) {
        ctx.scene.verbPool.push_back(v);
    }
    r.verbCount = static_cast<uint32_t>(gp.verbs.size());
    r.pointOffset = static_cast<uint32_t>(ctx.scene.floatPool.size());
    for (float f : gp.points) {
        ctx.scene.floatPool.push_back(f);
    }
    r.pointCount = static_cast<uint32_t>(gp.points.size());
    return r;
}

IrPathData defaultPathData(const PoolRange& r) {
    IrPathData path{};
    path.verbOffset = r.verbOffset;
    path.verbCount = r.verbCount;
    path.pointOffset = r.pointOffset;
    path.pointCount = r.pointCount;
    path.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    path.fillKind = static_cast<uint8_t>(PaintKind::None);
    path.strokeKind = static_cast<uint8_t>(PaintKind::None);
    path.cap = 0;
    path.join = 0;
    path.fillColor = Color{0, 0, 0, 1};
    path.strokeColor = Color{0, 0, 0, 1};
    path.fillP0 = path.fillP1 = path.strokeP0 = path.strokeP1 = Vec2{0, 0};
    path.fillStopOffset = path.fillStopCount = path.strokeStopOffset = path.strokeStopCount = 0;
    path.strokeWidth = 0.0f;
    path.miterLimit = 4.0f;
    path.trimStart = 0.0f;
    path.trimEnd = 1.0f;
    path.trimOffset = 0.0f;
    path.dashOffset = path.dashCount = 0;
    path.dashPhase = 0.0f;
    return path;
}

/// Appends a new Path scene node (with its own IrPathData entry) under `parentIdx`, using `gp` as
/// the node's static/template geometry. Returns (nodeIdx, pathIndex).
struct NewPathNode {
    uint32_t nodeIdx = kInvalidNode;
    uint32_t pathIndex = 0;
};

NewPathNode appendPathNode(BuildCtx& ctx, uint32_t parentIdx, const GeneratedPath& gp, StringId name) {
    PoolRange r = appendSceneGeometry(ctx, gp);
    NewPathNode out;
    out.pathIndex = static_cast<uint32_t>(ctx.scene.paths.size());
    ctx.scene.paths.push_back(defaultPathData(r));

    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Path);
    node.visible = 1;
    node.parent = parentIdx;
    node.kindIndex = out.pathIndex;
    node.opacity = 1.0f;
    node.anchor = Vec2{0, 0};
    node.translate = Vec2{0, 0};
    node.scale = Vec2{1, 1};
    node.rotation = 0.0f;
    node.skew = Vec2{0, 0};
    node.name = name;
    out.nodeIdx = appendNode(ctx, node);
    return out;
}

// -------------------------------------------------------------------------------------------
// Keyframe-time union helpers, used to expand animated rc/el/sr (+ optional RoundCorners radius)
// geometry into a single PathData Track. See docs/tasks/T-15.md "動畫幾何".
// -------------------------------------------------------------------------------------------

template <class T>
void addTimes(std::vector<float>& times, const AnimatedValue<T>& av) {
    if (!av.animated) {
        return;
    }
    for (const LottieKeyframe<T>& k : av.keys) {
        times.push_back(k.frame);
    }
}

std::vector<float> uniqueSorted(std::vector<float> times) {
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

/// The easing driving the segment starting at a synthesized union-time keyframe: copied verbatim
/// from whichever animated parameter owns a keyframe exactly at that time (first match in a fixed
/// priority order, per docs/tasks/T-15.md "多參數同時動畫時取第一個參數的easing"); a
/// linear-equivalent bezier (0,0,1,1) otherwise (the time came only from another parameter).
struct EasingPick {
    bool hold = false;
    float ox = 0, oy = 0, ix = 1, iy = 1;
};

template <class T>
bool pickEasingFrom(const AnimatedValue<T>& av, float frame, EasingPick& out) {
    if (!av.animated) {
        return false;
    }
    for (const LottieKeyframe<T>& k : av.keys) {
        if (k.frame == frame) {
            out.hold = k.hold;
            out.ox = k.ox;
            out.oy = k.oy;
            out.ix = k.ix;
            out.iy = k.iy;
            return true;
        }
    }
    return false;
}

/// Appends a Path-typed Track (PropId 32, PathData) whose keyframes point into
/// ctx.timeline.floatPool. `frames`/`paths`/`easings` must be the same size (one entry per union
/// keyframe, already sorted by time); every `paths[i].points` must have the same size (verified by
/// the caller).
void appendPathDataTrack(BuildCtx& ctx, uint32_t target, const std::vector<float>& frames,
                         const std::vector<GeneratedPath>& paths, const std::vector<EasingPick>& easings,
                         TimeMap map) {
    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        uint32_t floatOffset = static_cast<uint32_t>(ctx.timeline.floatPool.size());
        for (float f : paths[i].points) {
            ctx.timeline.floatPool.push_back(f);
        }
        IrKeyframe kf{};
        kf.time = mapFrameToSec(ctx, map, frames[i]);
        if (easings[i].hold) {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
        } else {
            kf.easing.kind = static_cast<uint8_t>(EasingKind::CubicBezier);
            kf.easing.x1 = easings[i].ox;
            kf.easing.y1 = easings[i].oy;
            kf.easing.x2 = easings[i].ix;
            kf.easing.y2 = easings[i].iy;
        }
        irSetValueU32(kf.value, 0, floatOffset);
        irSetValueU32(kf.value, 1, static_cast<uint32_t>(paths[i].points.size()));
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = target;
    track.prop = prop::PathData;
    track.type = static_cast<uint8_t>(ValueType::Path);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(frames.size());
    ctx.timeline.tracks.push_back(track);
}

// -------------------------------------------------------------------------------------------
// Geometry node construction: Rect / Ellipse / Star / Path("sh"), each producing exactly one Path
// scene node (+ PathData Track if animated). `roundRadius` (Rect/Path only) is the RoundCorners
// item's `r`, or nullptr if this geometry list has no RoundCorners item.
// -------------------------------------------------------------------------------------------

uint32_t appendRectNode(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& rect,
                        const AnimatedValue<float>* roundRadius, uint32_t parentIdx, TimeMap map,
                        StringId name) {
    bool animated = rect.position.animated || rect.size.animated || rect.radius.animated ||
                    (roundRadius != nullptr && roundRadius->animated);
    auto radiusAt = [&](float frame) {
        float r = sampleAnimated(rect.radius, frame);
        if (roundRadius != nullptr) {
            r = std::max(r, sampleAnimated(*roundRadius, frame));
        }
        return r;
    };

    if (!animated) {
        GeneratedPath gp = buildRectPath(firstOrStatic(rect.position), firstOrStatic(rect.size),
                                         radiusAt(0.0f), rect.direction);
        return appendPathNode(ctx, parentIdx, gp, name).nodeIdx;
    }

    std::vector<float> times;
    addTimes(times, rect.position);
    addTimes(times, rect.size);
    addTimes(times, rect.radius);
    if (roundRadius != nullptr) {
        addTimes(times, *roundRadius);
    }
    times = uniqueSorted(times);
    bool anyRounded = false;
    for (float f : times) {
        if (radiusAt(f) > 0.0f) {
            anyRounded = true;
            break;
        }
    }
    int animatedParamCount = (rect.position.animated ? 1 : 0) + (rect.size.animated ? 1 : 0) +
                             (rect.radius.animated ? 1 : 0) +
                             ((roundRadius != nullptr && roundRadius->animated) ? 1 : 0);
    if (animatedParamCount > 1) {
        ctx.sink.warn(ctx.file, lottieLayerDesc(layer) +
                                    ": multiple animated Rect parameters; using the first one's easing");
    }

    std::vector<GeneratedPath> paths;
    std::vector<EasingPick> easings;
    paths.reserve(times.size());
    easings.reserve(times.size());
    for (float f : times) {
        paths.push_back(buildRectPath(sampleAnimated(rect.position, f), sampleAnimated(rect.size, f),
                                      radiusAt(f), rect.direction, anyRounded));
        EasingPick pick;
        if (!pickEasingFrom(rect.position, f, pick) && !pickEasingFrom(rect.size, f, pick) &&
            !pickEasingFrom(rect.radius, f, pick)) {
            if (roundRadius == nullptr || !pickEasingFrom(*roundRadius, f, pick)) {
                pick = EasingPick{};
            }
        }
        easings.push_back(pick);
    }

    uint32_t nodeIdx = appendPathNode(ctx, parentIdx, paths.front(), name).nodeIdx;
    appendPathDataTrack(ctx, nodeIdx, times, paths, easings, map);
    return nodeIdx;
}

uint32_t appendEllipseNode(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& el,
                           uint32_t parentIdx, TimeMap map, StringId name) {
    bool animated = el.position.animated || el.size.animated;
    if (!animated) {
        GeneratedPath gp = buildEllipsePath(firstOrStatic(el.position), firstOrStatic(el.size), el.direction);
        return appendPathNode(ctx, parentIdx, gp, name).nodeIdx;
    }
    std::vector<float> times;
    addTimes(times, el.position);
    addTimes(times, el.size);
    times = uniqueSorted(times);
    if (el.position.animated && el.size.animated) {
        ctx.sink.warn(ctx.file, lottieLayerDesc(layer) +
                                    ": multiple animated Ellipse parameters; using the first one's easing");
    }
    std::vector<GeneratedPath> paths;
    std::vector<EasingPick> easings;
    for (float f : times) {
        paths.push_back(
            buildEllipsePath(sampleAnimated(el.position, f), sampleAnimated(el.size, f), el.direction));
        EasingPick pick;
        if (!pickEasingFrom(el.position, f, pick)) {
            pickEasingFrom(el.size, f, pick);
        }
        easings.push_back(pick);
    }
    uint32_t nodeIdx = appendPathNode(ctx, parentIdx, paths.front(), name).nodeIdx;
    appendPathDataTrack(ctx, nodeIdx, times, paths, easings, map);
    return nodeIdx;
}

/// T-258b (D3): warns when a star's `pt` (static value or any key) is above
/// modifiers::kMaxGeneratedPoints, which buildStarPath() and the run time clamp it to.
void warnStarPoints(BuildCtx& ctx, const LottieLayer& layer, const AnimatedValue<float>& points) {
    constexpr float kMaxPoints = static_cast<float>(modifiers::kMaxGeneratedPoints);
    bool clamped = !points.animated && points.value > kMaxPoints;
    for (const LottieKeyframe<float>& k : points.keys) {
        clamped = clamped || k.value > kMaxPoints;
    }
    if (clamped) {
        ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": star points clamped to " +
                                    std::to_string(modifiers::kMaxGeneratedPoints));
    }
}

uint32_t appendStarNode(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& star,
                        uint32_t parentIdx, TimeMap map, StringId name) {
    warnStarPoints(ctx, layer, star.points);
    const AnimatedValue<float>* params[] = {&star.points,         &star.innerRadius,    &star.outerRadius,
                                            &star.innerRoundness, &star.outerRoundness, &star.rotation};
    bool animated = star.position.animated;
    int animatedCount = star.position.animated ? 1 : 0;
    for (const AnimatedValue<float>* p : params) {
        if (p->animated) {
            animated = true;
            ++animatedCount;
        }
    }
    auto sampleAt = [&](float f) {
        return buildStarPath(sampleAnimated(star.points, f), sampleAnimated(star.innerRadius, f),
                             sampleAnimated(star.outerRadius, f), sampleAnimated(star.innerRoundness, f),
                             sampleAnimated(star.outerRoundness, f), sampleAnimated(star.rotation, f),
                             sampleAnimated(star.position, f), star.starType, star.direction);
    };
    if (!animated) {
        return appendPathNode(ctx, parentIdx, sampleAt(0.0f), name).nodeIdx;
    }
    std::vector<float> times;
    addTimes(times, star.position);
    for (const AnimatedValue<float>* p : params) {
        addTimes(times, *p);
    }
    times = uniqueSorted(times);
    if (animatedCount > 1) {
        ctx.sink.warn(ctx.file, lottieLayerDesc(layer) +
                                    ": multiple animated Star parameters; using the first one's easing");
    }
    std::vector<GeneratedPath> paths;
    std::vector<EasingPick> easings;
    for (float f : times) {
        paths.push_back(sampleAt(f));
        EasingPick pick;
        bool found = pickEasingFrom(star.position, f, pick);
        for (const AnimatedValue<float>* p : params) {
            if (found) {
                break;
            }
            found = pickEasingFrom(*p, f, pick);
        }
        easings.push_back(pick);
    }
    uint32_t nodeIdx = appendPathNode(ctx, parentIdx, paths.front(), name).nodeIdx;
    appendPathDataTrack(ctx, nodeIdx, times, paths, easings, map);
    return nodeIdx;
}

/// Path ("sh") + optional RoundCorners, per docs/tasks/T-15.md bullet 6. Vertex-count mismatches
/// across `sh.ks` keyframes fall back to a static path (first keyframe), warned once.
uint32_t appendShNode(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& shape,
                      const AnimatedValue<float>* roundRadius, uint32_t parentIdx, TimeMap map,
                      StringId name) {
    auto cut = [&](const LottiePath& p, float r) {
        return r > 0.0f || roundRadius != nullptr ? applyRoundCornersToPath(p, r) : p;
    };
    bool pathAnimated = shape.path.animated;
    bool rdAnimated = roundRadius != nullptr && roundRadius->animated;

    if (pathAnimated) {
        // Verify all keyframes share the same vertex count.
        size_t n0 = shape.path.keys.front().value.v.size();
        bool consistent = true;
        for (const auto& k : shape.path.keys) {
            if (k.value.v.size() != n0) {
                consistent = false;
                break;
            }
        }
        if (!consistent) {
            ctx.sink.warn(ctx.file, lottieLayerDesc(layer) +
                                        ": animated path (\"sh\") keyframes have differing vertex "
                                        "counts; using the first keyframe as a static path");
            pathAnimated = false;
        }
    }

    if (!pathAnimated && !rdAnimated) {
        float r = roundRadius != nullptr ? firstOrStatic(*roundRadius) : 0.0f;
        LottiePath p = cut(firstOrStatic(shape.path), r);
        GeneratedPath gp = buildPathFromLottiePath(p, shape.direction);
        return appendPathNode(ctx, parentIdx, gp, name).nodeIdx;
    }

    if (pathAnimated && !rdAnimated) {
        float r = roundRadius != nullptr ? firstOrStatic(*roundRadius) : 0.0f;
        std::vector<float> frames;
        std::vector<GeneratedPath> paths;
        std::vector<EasingPick> easings;
        for (const auto& k : shape.path.keys) {
            frames.push_back(k.frame);
            paths.push_back(buildPathFromLottiePath(cut(k.value, r), shape.direction));
            easings.push_back(EasingPick{k.hold, k.ox, k.oy, k.ix, k.iy});
        }
        uint32_t nodeIdx = appendPathNode(ctx, parentIdx, paths.front(), name).nodeIdx;
        appendPathDataTrack(ctx, nodeIdx, frames, paths, easings, map);
        return nodeIdx;
    }

    if (!pathAnimated && rdAnimated) {
        LottiePath basePath = firstOrStatic(shape.path);
        std::vector<float> frames;
        std::vector<GeneratedPath> paths;
        std::vector<EasingPick> easings;
        for (const auto& k : roundRadius->keys) {
            frames.push_back(k.frame);
            paths.push_back(buildPathFromLottiePath(cut(basePath, k.value), shape.direction));
            easings.push_back(EasingPick{k.hold, k.ox, k.oy, k.ix, k.iy});
        }
        uint32_t nodeIdx = appendPathNode(ctx, parentIdx, paths.front(), name).nodeIdx;
        appendPathDataTrack(ctx, nodeIdx, frames, paths, easings, map);
        return nodeIdx;
    }

    // Both animated.
    bool sameTimes = shape.path.keys.size() == roundRadius->keys.size();
    if (sameTimes) {
        for (size_t i = 0; i < shape.path.keys.size(); ++i) {
            if (shape.path.keys[i].frame != roundRadius->keys[i].frame) {
                sameTimes = false;
                break;
            }
        }
    }
    if (sameTimes) {
        std::vector<float> frames;
        std::vector<GeneratedPath> paths;
        std::vector<EasingPick> easings;
        for (size_t i = 0; i < shape.path.keys.size(); ++i) {
            const auto& k = shape.path.keys[i];
            frames.push_back(k.frame);
            paths.push_back(
                buildPathFromLottiePath(cut(k.value, roundRadius->keys[i].value), shape.direction));
            easings.push_back(EasingPick{k.hold, k.ox, k.oy, k.ix, k.iy});
        }
        uint32_t nodeIdx = appendPathNode(ctx, parentIdx, paths.front(), name).nodeIdx;
        appendPathDataTrack(ctx, nodeIdx, frames, paths, easings, map);
        return nodeIdx;
    }

    ctx.sink.warn(ctx.file, lottieLayerDesc(layer) +
                                ": animated path and animated RoundCorners have different keyframe "
                                "times; using RoundCorners' first frame");
    float r = firstOrStatic(*roundRadius);
    std::vector<float> frames;
    std::vector<GeneratedPath> paths;
    std::vector<EasingPick> easings;
    for (const auto& k : shape.path.keys) {
        frames.push_back(k.frame);
        paths.push_back(buildPathFromLottiePath(cut(k.value, r), shape.direction));
        easings.push_back(EasingPick{k.hold, k.ox, k.oy, k.ix, k.iy});
    }
    uint32_t nodeIdx = appendPathNode(ctx, parentIdx, paths.front(), name).nodeIdx;
    appendPathDataTrack(ctx, nodeIdx, frames, paths, easings, map);
    return nodeIdx;
}

uint32_t appendGeometryNode(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& geom,
                            const AnimatedValue<float>* roundRadius, uint32_t parentIdx, TimeMap map,
                            StringId name) {
    switch (geom.kind) {
        case LottieShapeItem::Kind::Rect:
            return appendRectNode(ctx, layer, geom, roundRadius, parentIdx, map, name);
        case LottieShapeItem::Kind::Ellipse:
            return appendEllipseNode(ctx, layer, geom, parentIdx, map, name);
        case LottieShapeItem::Kind::Star:
            return appendStarNode(ctx, layer, geom, parentIdx, map, name);
        case LottieShapeItem::Kind::Path:
            return appendShNode(ctx, layer, geom, roundRadius, parentIdx, map, name);
        default:
            XT_ASSERT(false && "unreachable: not a geometry kind");
            return kInvalidNode;
    }
}

// -------------------------------------------------------------------------------------------
// Paint application (Fill/Stroke/GradientFill/GradientStroke onto an already-created Path node).
// -------------------------------------------------------------------------------------------

// buildColorTrack moved to lottie_build.cpp (T-55): shared with the Drop Shadow color track,
// declared in lottie_model.h.

/// Decodes a flattened gradient stop array (`g.k`): the first `stopCount*4` floats are (offset, r,
/// g, b) color-stop quads, the rest are (offset, alpha) pairs. The result has one entry per color
/// stop offset, its alpha linearly interpolated from the alpha-stop list (clamped at the ends); if
/// there are no alpha stops, alpha defaults to 1. See docs/design/11-lottie-mapping.md §11.3.3.
std::vector<IrGradientStop> decodeGradientStops(const std::vector<float>& flat, int stopCount) {
    std::vector<IrGradientStop> stops;
    int cn = std::max(0, stopCount);
    for (int i = 0; i < cn; ++i) {
        size_t base = static_cast<size_t>(i) * 4;
        if (base + 3 >= flat.size()) {
            break;
        }
        IrGradientStop s{};
        s.offset = flat[base];
        s.color = Color{flat[base + 1], flat[base + 2], flat[base + 3], 1.0f};
        stops.push_back(s);
    }
    std::vector<std::pair<float, float>> alphaStops;
    size_t alphaStart = static_cast<size_t>(cn) * 4;
    for (size_t i = alphaStart; i + 1 < flat.size(); i += 2) {
        alphaStops.emplace_back(flat[i], flat[i + 1]);
    }
    if (alphaStops.empty()) {
        return stops;
    }
    for (IrGradientStop& s : stops) {
        if (s.offset <= alphaStops.front().first) {
            s.color.a = alphaStops.front().second;
        } else if (s.offset >= alphaStops.back().first) {
            s.color.a = alphaStops.back().second;
        } else {
            for (size_t i = 0; i + 1 < alphaStops.size(); ++i) {
                if (s.offset >= alphaStops[i].first && s.offset <= alphaStops[i + 1].first) {
                    float span = alphaStops[i + 1].first - alphaStops[i].first;
                    float t = span != 0.0f ? (s.offset - alphaStops[i].first) / span : 0.0f;
                    s.color.a = alphaStops[i].second + (alphaStops[i + 1].second - alphaStops[i].second) * t;
                    break;
                }
            }
        }
    }
    return stops;
}

bool buildStopsTrack(BuildCtx& ctx, const AnimatedValue<std::vector<float>>& av, int stopCount,
                     uint32_t target, PropId propId, TimeMap map, const LottieLayer& layer,
                     const char* label) {
    if (!av.animated) {
        return false;
    }
    std::vector<const LottieKeyframe<std::vector<float>>*> kept = dedupKeyframes(av, ctx, layer, label);
    if (kept.size() < 2) {
        return false;
    }
    uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
    for (const LottieKeyframe<std::vector<float>>* kp : kept) {
        const LottieKeyframe<std::vector<float>>& k = *kp;
        std::vector<IrGradientStop> stops = decodeGradientStops(k.value, stopCount);
        uint32_t stopOffset = static_cast<uint32_t>(ctx.timeline.stopPool.size());
        for (const IrGradientStop& s : stops) {
            ctx.timeline.stopPool.push_back(s);
        }
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
        irSetValueU32(kf.value, 0, stopOffset);
        irSetValueU32(kf.value, 1, static_cast<uint32_t>(stops.size()));
        ctx.timeline.keyframes.push_back(kf);
    }
    IrTrack track{};
    track.target = target;
    track.prop = propId;
    track.type = static_cast<uint8_t>(ValueType::Stops);
    track.kfOffset = kfOffset;
    track.kfCount = static_cast<uint32_t>(kept.size());
    ctx.timeline.tracks.push_back(track);
    return true;
}

// T-230: the paint helpers write IrPathData (Path nodes) or IrShapeData (Shape nodes); both carry
// the same paint field names.
template <class D>
void applyFill(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, D& path,
               const LottieShapeItem& fill, TimeMap map) {
    path.fillKind = static_cast<uint8_t>(PaintKind::Solid);
    path.fillColor = firstOrStatic(fill.color);
    path.fillRule = fill.fillRule == 2 ? static_cast<uint8_t>(FillRule::EvenOdd)
                                       : static_cast<uint8_t>(FillRule::NonZero);
    buildColorTrack(ctx, fill.color, nodeIdx, prop::FillColor, map, layer, "FillColor");
    buildFloatTrack(ctx, fill.opacity, nodeIdx, prop::FillOpacity, map, layer, "FillOpacity", 0.01f);
    path.fillColor.a *= firstOrStatic(fill.opacity) / 100.0f;
}

template <class D>
void applyStrokeCommon(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, D& path,
                       const LottieShapeItem& stroke, TimeMap map) {
    path.strokeColor = firstOrStatic(stroke.color);
    path.strokeColor.a *= firstOrStatic(stroke.opacity) / 100.0f;
    path.strokeWidth = firstOrStatic(stroke.width);
    path.miterLimit = firstOrStatic(stroke.miterLimit);
    path.cap = stroke.lineCap == 1   ? static_cast<uint8_t>(LineCap::Butt)
               : stroke.lineCap == 3 ? static_cast<uint8_t>(LineCap::Square)
                                     : static_cast<uint8_t>(LineCap::Round);
    path.join = stroke.lineJoin == 2   ? static_cast<uint8_t>(LineJoin::Round)
                : stroke.lineJoin == 3 ? static_cast<uint8_t>(LineJoin::Bevel)
                                       : static_cast<uint8_t>(LineJoin::Miter);

    buildColorTrack(ctx, stroke.color, nodeIdx, prop::StrokeColor, map, layer, "StrokeColor");
    buildFloatTrack(ctx, stroke.opacity, nodeIdx, prop::StrokeOpacity, map, layer, "StrokeOpacity", 0.01f);
    buildFloatTrack(ctx, stroke.width, nodeIdx, prop::StrokeWidth, map, layer, "StrokeWidth");
    buildFloatTrack(ctx, stroke.miterLimit, nodeIdx, prop::MiterLimit, map, layer, "MiterLimit");

    float phase = 0.0f;
    std::vector<float> dashValues;
    AnimatedValue<float> phaseAnim;
    bool havePhase = false;
    for (const LottieDash& d : stroke.dashes) {
        if (d.kind == LottieDash::Kind::Offset) {
            phase = firstOrStatic(d.value);
            phaseAnim = d.value;
            havePhase = true;
        } else {
            dashValues.push_back(firstOrStatic(d.value));
        }
    }
    path.dashPhase = phase;
    if (havePhase) {
        buildFloatTrack(ctx, phaseAnim, nodeIdx, prop::DashOffset, map, layer, "DashOffset");
    }
    if (!dashValues.empty()) {
        path.dashOffset = static_cast<uint32_t>(ctx.scene.floatPool.size());
        for (float v : dashValues) {
            ctx.scene.floatPool.push_back(v);
        }
        path.dashCount = static_cast<uint32_t>(dashValues.size());

        bool anyDashAnimated = false;
        for (const LottieDash& d : stroke.dashes) {
            if (d.kind != LottieDash::Kind::Offset && d.value.animated) {
                anyDashAnimated = true;
                break;
            }
        }
        if (anyDashAnimated) {
            std::vector<float> times;
            for (const LottieDash& d : stroke.dashes) {
                if (d.kind != LottieDash::Kind::Offset) {
                    addTimes(times, d.value);
                }
            }
            times = uniqueSorted(times);
            uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
            for (float f : times) {
                uint32_t floatOffset = static_cast<uint32_t>(ctx.timeline.floatPool.size());
                for (const LottieDash& d : stroke.dashes) {
                    if (d.kind != LottieDash::Kind::Offset) {
                        ctx.timeline.floatPool.push_back(sampleAnimated(d.value, f));
                    }
                }
                IrKeyframe kf{};
                kf.time = mapFrameToSec(ctx, map, f);
                EasingPick pick;
                bool found = false;
                for (const LottieDash& d : stroke.dashes) {
                    if (d.kind != LottieDash::Kind::Offset && pickEasingFrom(d.value, f, pick)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    pick = EasingPick{};
                }
                if (pick.hold) {
                    kf.easing.kind = static_cast<uint8_t>(EasingKind::Hold);
                } else {
                    kf.easing.kind = static_cast<uint8_t>(EasingKind::CubicBezier);
                    kf.easing.x1 = pick.ox;
                    kf.easing.y1 = pick.oy;
                    kf.easing.x2 = pick.ix;
                    kf.easing.y2 = pick.iy;
                }
                irSetValueU32(kf.value, 0, floatOffset);
                irSetValueU32(kf.value, 1, static_cast<uint32_t>(dashValues.size()));
                ctx.timeline.keyframes.push_back(kf);
            }
            IrTrack track{};
            track.target = nodeIdx;
            track.prop = prop::DashArray;
            track.type = static_cast<uint8_t>(ValueType::FloatArray);
            track.kfOffset = kfOffset;
            track.kfCount = static_cast<uint32_t>(times.size());
            ctx.timeline.tracks.push_back(track);
        }
    }
}

template <class D>
void applyStroke(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, D& path,
                 const LottieShapeItem& stroke, TimeMap map) {
    path.strokeKind = static_cast<uint8_t>(PaintKind::Solid);
    applyStrokeCommon(ctx, layer, nodeIdx, path, stroke, map);
}

void applyGradientCommon(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, bool isFill,
                         const LottieShapeItem& grad, TimeMap map, Vec2& outP0, Vec2& outP1,
                         uint32_t& outStopOffset, uint32_t& outStopCount) {
    outP0 = firstOrStatic(grad.gradientStart);
    outP1 = firstOrStatic(grad.gradientEnd);
    std::vector<float> flat = firstOrStatic(grad.stops);
    std::vector<IrGradientStop> stops = decodeGradientStops(flat, grad.stopCount);
    outStopOffset = static_cast<uint32_t>(ctx.scene.stopPool.size());
    for (const IrGradientStop& s : stops) {
        ctx.scene.stopPool.push_back(s);
    }
    outStopCount = static_cast<uint32_t>(stops.size());

    PropId p0Prop = isFill ? prop::FillGradientP0 : prop::StrokeGradientP0;
    PropId p1Prop = isFill ? prop::FillGradientP1 : prop::StrokeGradientP1;
    PropId stopsProp = isFill ? prop::FillGradientStops : prop::StrokeGradientStops;
    buildVec2Track(ctx, grad.gradientStart, nodeIdx, p0Prop, map, layer, "GradientP0");
    buildVec2Track(ctx, grad.gradientEnd, nodeIdx, p1Prop, map, layer, "GradientP1");
    buildStopsTrack(ctx, grad.stops, grad.stopCount, nodeIdx, stopsProp, map, layer, "GradientStops");
}

template <class D>
void applyGradientFill(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, D& path,
                       const LottieShapeItem& grad, TimeMap map) {
    path.fillKind = grad.gradientKind == 2 ? static_cast<uint8_t>(PaintKind::Radial)
                                           : static_cast<uint8_t>(PaintKind::Linear);
    path.fillRule = grad.fillRule == 2 ? static_cast<uint8_t>(FillRule::EvenOdd)
                                       : static_cast<uint8_t>(FillRule::NonZero);
    applyGradientCommon(ctx, layer, nodeIdx, true, grad, map, path.fillP0, path.fillP1, path.fillStopOffset,
                        path.fillStopCount);
    buildFloatTrack(ctx, grad.opacity, nodeIdx, prop::FillOpacity, map, layer, "FillOpacity", 0.01f);
    for (IrGradientStop& s : std::span(ctx.scene.stopPool).subspan(path.fillStopOffset, path.fillStopCount)) {
        s.color.a *= firstOrStatic(grad.opacity) / 100.0f;
    }
}

template <class D>
void applyGradientStroke(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, D& path,
                         const LottieShapeItem& grad, TimeMap map) {
    path.strokeKind = grad.gradientKind == 2 ? static_cast<uint8_t>(PaintKind::Radial)
                                             : static_cast<uint8_t>(PaintKind::Linear);
    applyGradientCommon(ctx, layer, nodeIdx, false, grad, map, path.strokeP0, path.strokeP1,
                        path.strokeStopOffset, path.strokeStopCount);
    for (IrGradientStop& s :
         std::span(ctx.scene.stopPool).subspan(path.strokeStopOffset, path.strokeStopCount)) {
        s.color.a *= firstOrStatic(grad.opacity) / 100.0f;
    }
    applyStrokeCommon(ctx, layer, nodeIdx, path, grad, map);
}

template <class D>
void applyPaintTo(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, D& path,
                  const LottieShapeItem& style, TimeMap map) {
    switch (style.kind) {
        case LottieShapeItem::Kind::Fill:
            applyFill(ctx, layer, nodeIdx, path, style, map);
            break;
        case LottieShapeItem::Kind::Stroke:
            applyStroke(ctx, layer, nodeIdx, path, style, map);
            break;
        case LottieShapeItem::Kind::GradientFill:
            applyGradientFill(ctx, layer, nodeIdx, path, style, map);
            break;
        case LottieShapeItem::Kind::GradientStroke:
            applyGradientStroke(ctx, layer, nodeIdx, path, style, map);
            break;
        default:
            XT_ASSERT(false && "unreachable: not a paint kind");
            break;
    }
}

void applyPaint(BuildCtx& ctx, const LottieLayer& layer, uint32_t nodeIdx, uint32_t pathIndex,
                const LottieShapeItem& style, TimeMap map) {
    applyPaintTo(ctx, layer, nodeIdx, ctx.scene.paths[pathIndex], style, map);
}

// -------------------------------------------------------------------------------------------
// Trim ("tm"): writes static values onto every produced Path node, or Tracks if animated.
// -------------------------------------------------------------------------------------------

void applyTrim(BuildCtx& ctx, const LottieLayer& layer, const std::vector<uint32_t>& nodeIndices,
               const std::vector<uint32_t>& pathIndices, const LottieShapeItem& trim, TimeMap map) {
    float s = firstOrStatic(trim.trimStart) / 100.0f;
    float e = firstOrStatic(trim.trimEnd) / 100.0f;
    // Lottie `tm.o` is in degrees (lottie-web divides by 360); the per-path TrimOffset is in turns.
    float o = firstOrStatic(trim.trimOffset) / 360.0f;
    for (size_t i = 0; i < pathIndices.size(); ++i) {
        IrPathData& path = ctx.scene.paths[pathIndices[i]];
        path.trimStart = s;
        path.trimEnd = e;
        path.trimOffset = o;
        buildFloatTrack(ctx, trim.trimStart, nodeIndices[i], prop::TrimStart, map, layer, "TrimStart", 0.01f);
        buildFloatTrack(ctx, trim.trimEnd, nodeIndices[i], prop::TrimEnd, map, layer, "TrimEnd", 0.01f);
        buildFloatTrack(ctx, trim.trimOffset, nodeIndices[i], prop::TrimOffset, map, layer, "TrimOffset",
                        1.0f / 360.0f);
    }
}

// -------------------------------------------------------------------------------------------
// Forward declaration: recursive group processing.
// -------------------------------------------------------------------------------------------

void processShapeItems(BuildCtx& ctx, const LottieLayer& layer, const std::vector<LottieShapeItem>& items,
                       uint32_t parentIdx, TimeMap map, StringId groupName);

/// Builds one repeated copy's Group node for Repeater ("rp"): transform = tr's anchor/position/
/// scale/rotation each multiplied by `mult` (position/rotation linear, scale = pow(scale/100,
/// mult)*100), opacity = lerp(so, eo, t). `content` is invoked once to populate the copy's children
/// under the returned node index. See docs/tasks/T-15.md bullet 7.
uint32_t buildRepeaterCopy(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeTransform& tr,
                           float mult, float opacityT, TimeMap map, StringId name,
                           const std::function<void(uint32_t)>& content, uint32_t parentIdx) {
    auto scaleFloat = [mult](float v) { return v * mult; };
    auto scaleVec2 = [mult](Vec2 v) { return Vec2{v.x * mult, v.y * mult}; };
    auto scalePow = [mult](Vec2 v) {
        float sx = std::pow(v.x / 100.0f, mult) * 100.0f;
        float sy = std::pow(v.y / 100.0f, mult) * 100.0f;
        return Vec2{sx, sy};
    };

    AnimatedValue<Vec2> anchor = tr.base.anchor;
    AnimatedValue<Vec2> position = tr.base.position;
    AnimatedValue<Vec2> scale = tr.base.scale;
    AnimatedValue<float> rotation = tr.base.rotation;
    anchor.value = scaleVec2(anchor.value);
    for (auto& k : anchor.keys) {
        k.value = scaleVec2(k.value);
    }
    position.value = scaleVec2(position.value);
    for (auto& k : position.keys) {
        k.value = scaleVec2(k.value);
    }
    rotation.value = scaleFloat(rotation.value);
    for (auto& k : rotation.keys) {
        k.value = scaleFloat(k.value);
    }
    scale.value = scalePow(scale.value);
    for (auto& k : scale.keys) {
        k.value = scalePow(k.value);
    }

    IrSceneNode node = defaultGroupNode(parentIdx, name);
    node.anchor = firstOrStatic(anchor);
    node.translate = firstOrStatic(position);
    Vec2 rawScale = firstOrStatic(scale);
    node.scale = Vec2{rawScale.x / 100.0f, rawScale.y / 100.0f};
    node.rotation = firstOrStatic(rotation);
    node.opacity = opacityT;
    uint32_t idx = appendNode(ctx, node);

    buildVec2Track(ctx, anchor, idx, prop::Anchor, map, layer, "Anchor");
    buildVec2Track(ctx, position, idx, prop::Translate, map, layer, "Translate");
    buildVec2Track(ctx, scale, idx, prop::Scale, map, layer, "Scale", 0.01f);
    buildFloatTrack(ctx, rotation, idx, prop::Rotation, map, layer, "Rotation");

    content(idx);
    return idx;
}

/// Repeater ("rp"): duplicates the drawing units already produced by this items list `c` times.
/// `emitUnits` re-runs geometry/style/subgroup construction into the given parent (used once per
/// copy, since each copy needs its own scene subtree). T-258b (D6): `c` is clamped to
/// [0, modifiers::kMaxRepeatCopies] (non-finite = 0; above it warns), and the build stops with an
/// error once its repeaters have made kMaxLottieRepeaterCopies copies in all (nested ones multiply).
void applyRepeater(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& rp, TimeMap map,
                   uint32_t parentIdx, const std::function<void(uint32_t)>& emitUnits) {
    constexpr float kMaxCopies = static_cast<float>(modifiers::kMaxRepeatCopies);
    float copiesF = xt::finiteOr(firstOrStatic(rp.copies), 0.0f);
    if (copiesF > kMaxCopies) {
        ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": repeater copies clamped to " +
                                    std::to_string(modifiers::kMaxRepeatCopies));
        copiesF = kMaxCopies;
    }
    int copies = static_cast<int>(std::lround(std::max(copiesF, 0.0f)));
    if (copies <= 0) {
        return;
    }
    float offset = firstOrStatic(rp.offset);
    float so = firstOrStatic(rp.repeaterTransform.startOpacity) / 100.0f;
    float eo = firstOrStatic(rp.repeaterTransform.endOpacity) / 100.0f;
    for (int k = 0; k < copies; ++k) {
        if (ctx.expansionExceeded) {
            return;
        }
        if (++ctx.repeaterCopies > kMaxLottieRepeaterCopies) {
            ctx.expansionExceeded = true;
            ctx.sink.error(ctx.file, "lottie: expansion exceeds " + std::to_string(kMaxLottieRepeaterCopies) +
                                         " shapes");
            return;
        }
        float mult = offset + static_cast<float>(k);
        float t = copies == 1 ? 0.0f : static_cast<float>(k) / static_cast<float>(copies - 1);
        float opacity = so + (eo - so) * t;
        StringId name = ctx.writer.intern(layer.name + "#repeater" + std::to_string(k));
        buildRepeaterCopy(ctx, layer, rp.repeaterTransform, mult, opacity, map, name, emitUnits, parentIdx);
    }
}

// -------------------------------------------------------------------------------------------
// T-230 (31 §31.9): modifier nodes, parametric Shape nodes (native import only) and the
// structured group emission shared by both builders.
// -------------------------------------------------------------------------------------------

bool isGeometryKind(LottieShapeItem::Kind k) {
    return k == LottieShapeItem::Kind::Rect || k == LottieShapeItem::Kind::Ellipse ||
           k == LottieShapeItem::Kind::Star || k == LottieShapeItem::Kind::Path;
}

bool isPaintKind(LottieShapeItem::Kind k) {
    return k == LottieShapeItem::Kind::Fill || k == LottieShapeItem::Kind::Stroke ||
           k == LottieShapeItem::Kind::GradientFill || k == LottieShapeItem::Kind::GradientStroke;
}

bool isModifierKind(LottieShapeItem::Kind k) {
    switch (k) {
        case LottieShapeItem::Kind::Trim:
        case LottieShapeItem::Kind::RoundCorners:
        case LottieShapeItem::Kind::Repeater:
        case LottieShapeItem::Kind::PuckerBloat:
        case LottieShapeItem::Kind::ZigZag:
        case LottieShapeItem::Kind::Twist:
        case LottieShapeItem::Kind::OffsetPath:
        case LottieShapeItem::Kind::MergePaths:
            return true;
        default:
            return false;
    }
}

bool isStrokeKind(LottieShapeItem::Kind k) {
    return k == LottieShapeItem::Kind::Stroke || k == LottieShapeItem::Kind::GradientStroke;
}

void setLabel(BuildCtx& ctx, uint32_t nodeIdx, const std::string& label) {
    if (!ctx.native) {
        return;
    }
    if (ctx.nodeLabels.size() <= nodeIdx) {
        ctx.nodeLabels.resize(nodeIdx + 1);
    }
    ctx.nodeLabels[nodeIdx] = label;
}

/// One component of a Vec2 property as a float property, `c * mul + add` (c = x or y); spatial
/// tangents are dropped (callers only use it where there are none).
AnimatedValue<float> vecComponent(const AnimatedValue<Vec2>& av, int comp, float mul, float add) {
    AnimatedValue<float> out;
    auto pick = [&](Vec2 v) { return (comp == 0 ? v.x : v.y) * mul + add; };
    out.animated = av.animated;
    out.value = pick(av.value);
    for (const LottieKeyframe<Vec2>& k : av.keys) {
        LottieKeyframe<float> f;
        f.frame = k.frame;
        f.value = pick(k.value);
        f.hold = k.hold;
        f.ox = k.ox;
        f.oy = k.oy;
        f.ix = k.ix;
        f.iy = k.iy;
        out.keys.push_back(f);
    }
    return out;
}

bool hasSpatialTangents(const AnimatedValue<Vec2>& av) {
    if (!av.animated) {
        return false;
    }
    for (const LottieKeyframe<Vec2>& k : av.keys) {
        if (k.hasSpatial &&
            std::fabs(k.to.x) + std::fabs(k.to.y) + std::fabs(k.ti.x) + std::fabs(k.ti.y) > 1e-6f) {
            return true;
        }
    }
    return false;
}

bool uniformScale(const AnimatedValue<Vec2>& s) {
    if (s.value.x != s.value.y) {
        return false;
    }
    for (const LottieKeyframe<Vec2>& k : s.keys) {
        if (k.value.x != k.value.y) {
            return false;
        }
    }
    return true;
}

/// A repeater the runtime Repeat modifier can draw: uniform scale and one position property.
/// T-236: an animated anchor / position stays a modifier in the native import too (`repeat-anchor`
/// / `repeat-translate` tracks).
bool repeaterAsModifier(const BuildCtx& ctx, const LottieShapeItem& rp) {
    (void)ctx;
    const LottieShapeTransform& tr = rp.repeaterTransform;
    return !tr.base.splitPosition && uniformScale(tr.base.scale);
}

uint32_t appendModifierNode(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& it,
                            uint32_t parentIdx, TimeMap map, const std::string& label) {
    IrModifierData m{};
    m.enabled = 1;
    m.opacityA = 1;
    m.opacityB = 1;
    switch (it.kind) {
        case LottieShapeItem::Kind::Trim:
            m.kind = static_cast<uint8_t>(ModifierKind::Trim);
            m.mode = it.trimMode == 2 ? 1 : 0;
            m.params[0] = firstOrStatic(it.trimStart) / 100.0f;
            m.params[1] = firstOrStatic(it.trimEnd) / 100.0f;
            m.params[2] = firstOrStatic(it.trimOffset);
            break;
        case LottieShapeItem::Kind::Repeater: {
            const LottieShapeTransform& tr = it.repeaterTransform;
            m.kind = static_cast<uint8_t>(ModifierKind::Repeat);
            m.mode = it.repeaterComposite == 2 ? 0 : 1;
            m.params[0] = firstOrStatic(it.copies);
            m.params[1] = firstOrStatic(it.offset);
            const Vec2 a = firstOrStatic(tr.base.anchor);
            const Vec2 p = firstOrStatic(tr.base.position);
            m.params[2] = a.x;
            m.params[3] = a.y;
            m.params[4] = p.x;
            m.params[5] = p.y;
            m.params[6] = firstOrStatic(tr.base.rotation);
            m.params[7] = firstOrStatic(tr.base.scale).x / 100.0f;
            m.opacityA = firstOrStatic(tr.startOpacity) / 100.0f;
            m.opacityB = firstOrStatic(tr.endOpacity) / 100.0f;
            break;
        }
        case LottieShapeItem::Kind::RoundCorners:
            m.kind = static_cast<uint8_t>(ModifierKind::Round);
            m.params[0] = firstOrStatic(it.radius);
            break;
        case LottieShapeItem::Kind::PuckerBloat:
            m.kind = static_cast<uint8_t>(ModifierKind::Pucker);
            m.params[0] = firstOrStatic(it.amount);
            break;
        case LottieShapeItem::Kind::ZigZag:
            m.kind = static_cast<uint8_t>(ModifierKind::ZigZag);
            m.mode = it.zigzagPointType == 2 ? 1 : 0;
            m.params[0] = firstOrStatic(it.zigzagSize);
            m.params[1] = firstOrStatic(it.zigzagRidges);
            break;
        case LottieShapeItem::Kind::Twist: {
            m.kind = static_cast<uint8_t>(ModifierKind::Twist);
            m.params[0] = firstOrStatic(it.amount);
            const Vec2 c = firstOrStatic(it.twistCenter);
            m.params[1] = c.x;
            m.params[2] = c.y;
            break;
        }
        case LottieShapeItem::Kind::OffsetPath:
            m.kind = static_cast<uint8_t>(ModifierKind::Offset);
            m.mode = static_cast<uint8_t>(std::clamp(it.offsetJoin, 1, 3) - 1);
            m.params[0] = firstOrStatic(it.amount);
            m.params[1] = firstOrStatic(it.offsetMiter);
            m.enabled = 0;  // Baked by the editor (31 §31.6.3).
            break;
        case LottieShapeItem::Kind::MergePaths:
            m.kind = static_cast<uint8_t>(ModifierKind::Merge);
            m.mode = static_cast<uint8_t>(std::clamp(it.mergeMode, 1, 5) - 1);
            m.enabled = 0;
            break;
        default:
            XT_ASSERT(false && "unreachable: not a modifier kind");
            return kInvalidNode;
    }
    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Modifier);
    node.visible = 1;
    node.parent = parentIdx;
    node.kindIndex = static_cast<uint32_t>(ctx.scene.modifiers.size());
    node.opacity = 1.0f;
    node.scale = Vec2{1, 1};
    node.name = ctx.writer.intern(layer.name + "#" + it.name);
    node.svgTag = static_cast<uint8_t>(IrSvgTag::Modifier);
    ctx.scene.modifiers.push_back(m);
    const uint32_t idx = appendNode(ctx, node);
    setLabel(ctx, idx, label);

    switch (it.kind) {
        case LottieShapeItem::Kind::Trim:
            buildFloatTrack(ctx, it.trimStart, idx, prop::ModTrimStart, map, layer, "TrimStart", 0.01f);
            buildFloatTrack(ctx, it.trimEnd, idx, prop::ModTrimEnd, map, layer, "TrimEnd", 0.01f);
            buildFloatTrack(ctx, it.trimOffset, idx, prop::ModTrimOffset, map, layer, "TrimOffset");
            break;
        case LottieShapeItem::Kind::Repeater: {
            const LottieShapeTransform& tr = it.repeaterTransform;
            buildFloatTrack(ctx, it.copies, idx, prop::ModCopies, map, layer, "Copies");
            buildFloatTrack(ctx, it.offset, idx, prop::ModOffset, map, layer, "RepeatOffset");
            buildVec2Track(ctx, tr.base.anchor, idx, prop::ModAnchor, map, layer, "RepeatAnchor");
            buildVec2Track(ctx, tr.base.position, idx, prop::ModTranslate, map, layer, "RepeatTranslate");
            buildFloatTrack(ctx, tr.base.rotation, idx, prop::ModRotation, map, layer, "RepeatRotation");
            buildFloatTrack(ctx, vecComponent(tr.base.scale, 0, 0.01f, 0.0f), idx, prop::ModScale, map, layer,
                            "RepeatScale");
            buildFloatTrack(ctx, tr.startOpacity, idx, prop::ModStartOpacity, map, layer, "StartOpacity",
                            0.01f);
            buildFloatTrack(ctx, tr.endOpacity, idx, prop::ModEndOpacity, map, layer, "EndOpacity", 0.01f);
            break;
        }
        case LottieShapeItem::Kind::RoundCorners:
            buildFloatTrack(ctx, it.radius, idx, prop::ModRadius, map, layer, "RoundRadius");
            break;
        case LottieShapeItem::Kind::PuckerBloat:
        case LottieShapeItem::Kind::OffsetPath:
            buildFloatTrack(ctx, it.amount, idx, prop::ModAmount, map, layer, "Amount");
            break;
        case LottieShapeItem::Kind::ZigZag:
            buildFloatTrack(ctx, it.zigzagSize, idx, prop::ModSize, map, layer, "ZigZagSize");
            buildFloatTrack(ctx, it.zigzagRidges, idx, prop::ModRidges, map, layer, "ZigZagRidges");
            break;
        case LottieShapeItem::Kind::Twist:
            buildFloatTrack(ctx, it.amount, idx, prop::ModAngle, map, layer, "TwistAngle");
            if (it.twistCenter.animated) {
                // T-236: the native import writes it as a `center` track.
                buildVec2Track(ctx, it.twistCenter, idx, prop::ModCenter, map, layer, "TwistCenter");
            }
            break;
        default:
            break;
    }
    return idx;
}

/// A parametric Shape node for `geom` (native import only), or kInvalidNode when the SVG element
/// would not draw Lottie's path: a reversed direction or a curved motion path; a rect/ellipse whose
/// start point matters (`startMatters`: a trim or zig zag after it, or a dashed paint -- SVG rects
/// start top-left and ellipses at the right, Lottie's at the top-right corner / the top); rounded
/// star corners (Lottie's `is`/`os` are not `<xt-star round>`); rect animation the parameters
/// cannot carry (position and size both animated; a radius the size clamps differently).
uint32_t appendShapeNode(BuildCtx& ctx, const LottieLayer& layer, const LottieShapeItem& geom,
                         uint32_t parentIdx, TimeMap map, StringId name, bool startMatters) {
    if (geom.direction == 3 || hasSpatialTangents(geom.position)) {
        return kInvalidNode;
    }
    IrShapeData data{};
    data.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    data.fillKind = static_cast<uint8_t>(PaintKind::None);
    data.strokeKind = static_cast<uint8_t>(PaintKind::None);
    data.fillColor = Color{0, 0, 0, 1};
    data.strokeColor = Color{0, 0, 0, 1};
    data.miterLimit = 4.0f;
    data.trimEnd = 1.0f;
    const Vec2 p = firstOrStatic(geom.position);
    const Vec2 sz = firstOrStatic(geom.size);
    switch (geom.kind) {
        case LottieShapeItem::Kind::Rect: {
            if (startMatters || (geom.position.animated && geom.size.animated)) {
                return kInvalidNode;
            }
            const float half = std::min(std::fabs(sz.x), std::fabs(sz.y)) * 0.5f;
            float rMax = firstOrStatic(geom.radius);
            for (const auto& k : geom.radius.keys) {
                rMax = std::max(rMax, k.value);
            }
            if ((geom.size.animated && rMax > 0.0f) || (rMax > half && sz.x != sz.y)) {
                return kInvalidNode;
            }
            const float r = std::min(firstOrStatic(geom.radius), half);
            data.kind = static_cast<uint8_t>(ShapeKind::Rect);
            data.params[0] = p.x - sz.x * 0.5f;
            data.params[1] = p.y - sz.y * 0.5f;
            data.params[2] = sz.x;
            data.params[3] = sz.y;
            data.params[4] = r;
            data.params[5] = r;
            break;
        }
        case LottieShapeItem::Kind::Ellipse:
            if (startMatters) {
                return kInvalidNode;
            }
            data.kind = static_cast<uint8_t>(ShapeKind::Ellipse);
            data.params[0] = p.x;
            data.params[1] = p.y;
            data.params[2] = sz.x * 0.5f;
            data.params[3] = sz.y * 0.5f;
            break;
        case LottieShapeItem::Kind::Star:
            if (geom.innerRoundness.animated || geom.outerRoundness.animated ||
                firstOrStatic(geom.innerRoundness) != 0.0f || firstOrStatic(geom.outerRoundness) != 0.0f) {
                return kInvalidNode;
            }
            data.kind = static_cast<uint8_t>(ShapeKind::Star);
            data.params[0] = p.x;
            data.params[1] = p.y;
            // T-258b (D3): clamped as buildStarPath() and the run time do.
            data.params[2] =
                std::min(firstOrStatic(geom.points), static_cast<float>(modifiers::kMaxGeneratedPoints));
            data.params[3] = firstOrStatic(geom.outerRadius);
            data.params[4] = geom.starType == 2 ? 0.0f : firstOrStatic(geom.innerRadius);
            data.params[5] = firstOrStatic(geom.rotation);
            if (geom.starType != 2 && data.params[4] <= 0.0f) {
                return kInvalidNode;  // A zero inner radius would read as a polygon.
            }
            warnStarPoints(ctx, layer, geom.points);
            break;
        default:
            return kInvalidNode;
    }
    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Shape);
    node.visible = 1;
    node.parent = parentIdx;
    node.kindIndex = static_cast<uint32_t>(ctx.scene.shapes.size());
    node.opacity = 1.0f;
    node.scale = Vec2{1, 1};
    node.name = name;
    node.svgTag = static_cast<uint8_t>(geom.kind == LottieShapeItem::Kind::Rect      ? IrSvgTag::Rect
                                       : geom.kind == LottieShapeItem::Kind::Ellipse ? IrSvgTag::Ellipse
                                                                                     : IrSvgTag::XtStar);
    ctx.scene.shapes.push_back(data);
    const uint32_t idx = appendNode(ctx, node);

    switch (geom.kind) {
        case LottieShapeItem::Kind::Rect:
            if (geom.position.animated) {
                buildFloatTrack(ctx, vecComponent(geom.position, 0, 1.0f, -sz.x * 0.5f), idx, prop::ShapeX,
                                map, layer, "RectX");
                buildFloatTrack(ctx, vecComponent(geom.position, 1, 1.0f, -sz.y * 0.5f), idx, prop::ShapeY,
                                map, layer, "RectY");
            } else if (geom.size.animated) {
                buildFloatTrack(ctx, vecComponent(geom.size, 0, -0.5f, p.x), idx, prop::ShapeX, map, layer,
                                "RectX");
                buildFloatTrack(ctx, vecComponent(geom.size, 1, -0.5f, p.y), idx, prop::ShapeY, map, layer,
                                "RectY");
                buildFloatTrack(ctx, vecComponent(geom.size, 0, 1.0f, 0.0f), idx, prop::ShapeW, map, layer,
                                "RectW");
                buildFloatTrack(ctx, vecComponent(geom.size, 1, 1.0f, 0.0f), idx, prop::ShapeH, map, layer,
                                "RectH");
            }
            buildFloatTrack(ctx, geom.radius, idx, prop::ShapeRx, map, layer, "RectRx");
            buildFloatTrack(ctx, geom.radius, idx, prop::ShapeRy, map, layer, "RectRy");
            break;
        case LottieShapeItem::Kind::Ellipse:
            buildFloatTrack(ctx, vecComponent(geom.position, 0, 1.0f, 0.0f), idx, prop::ShapeCx, map, layer,
                            "EllipseCx");
            buildFloatTrack(ctx, vecComponent(geom.position, 1, 1.0f, 0.0f), idx, prop::ShapeCy, map, layer,
                            "EllipseCy");
            buildFloatTrack(ctx, vecComponent(geom.size, 0, 0.5f, 0.0f), idx, prop::ShapeRx, map, layer,
                            "EllipseRx");
            buildFloatTrack(ctx, vecComponent(geom.size, 1, 0.5f, 0.0f), idx, prop::ShapeRy, map, layer,
                            "EllipseRy");
            break;
        default:
            buildFloatTrack(ctx, vecComponent(geom.position, 0, 1.0f, 0.0f), idx, prop::ShapeCx, map, layer,
                            "StarCx");
            buildFloatTrack(ctx, vecComponent(geom.position, 1, 1.0f, 0.0f), idx, prop::ShapeCy, map, layer,
                            "StarCy");
            buildFloatTrack(ctx, geom.points, idx, prop::ShapePoints, map, layer, "StarPoints");
            buildFloatTrack(ctx, geom.outerRadius, idx, prop::ShapeR, map, layer, "StarOuterRadius");
            if (geom.starType != 2) {
                buildFloatTrack(ctx, geom.innerRadius, idx, prop::ShapeR2, map, layer, "StarInnerRadius");
            }
            buildFloatTrack(ctx, geom.rotation, idx, prop::ShapeRotation, map, layer, "StarRotation");
            break;
    }
    return idx;
}

/// A fill and/or a stroke drawn on the same shapes (31 §31.2: one fill and one stroke per element).
/// The shapes it draws are the group's geometry items before `scopeEnd` (Skottie: a paint draws
/// the shapes above it).
struct PaintSet {
    const LottieShapeItem* fill = nullptr;
    const LottieShapeItem* stroke = nullptr;
    size_t scopeEnd = 0;
};

void processShapeItems(BuildCtx& ctx, const LottieLayer& layer, const std::vector<LottieShapeItem>& items,
                       uint32_t parentIdx, TimeMap map, StringId groupName);

/// T-230: one group's content as geometry nodes carrying their paints, interleaved in stack order
/// with Modifier nodes (a modifier acts on the shapes before it, 31 §31.6.1). Several paint sets
/// with modifiers each get their own group holding the shapes and a copy of the modifiers, so a
/// serial trim or a repeater sees each shape once per paint. Draw order: paint sets and nested
/// groups bottom (last in the array) first. Used by the native import always and by the `<anim>`
/// builder when the group has modifier nodes.
void emitStructured(BuildCtx& ctx, const LottieLayer& layer, const std::vector<LottieShapeItem>& items,
                    const std::vector<const LottieShapeItem*>& mods, uint32_t into, TimeMap map) {
    auto shapeOrGroupBetween = [&](size_t lo, size_t hi) {
        for (size_t i = lo + 1; i < hi; ++i) {
            if (isGeometryKind(items[i].kind) || items[i].kind == LottieShapeItem::Kind::Group) {
                return true;
            }
        }
        return false;
    };
    // A stroke directly above a fill (same shapes, nothing drawn in between) shares its element in
    // the native import: SVG strokes over the fill, as the stroke above the fill does in AE.
    std::vector<PaintSet> sets;
    for (size_t i = items.size(); i-- > 0;) {
        const LottieShapeItem& it = items[i];
        if (!isPaintKind(it.kind)) {
            continue;
        }
        if (ctx.native && isStrokeKind(it.kind) && !sets.empty() && sets.back().fill != nullptr &&
            sets.back().stroke == nullptr && !shapeOrGroupBetween(i, sets.back().scopeEnd)) {
            sets.back().stroke = &it;
            sets.back().scopeEnd = i;
            continue;
        }
        PaintSet ps;
        (isStrokeKind(it.kind) ? ps.stroke : ps.fill) = &it;
        ps.scopeEnd = i;
        sets.push_back(ps);
    }
    const bool wrap = sets.size() > 1 && !mods.empty();

    struct Unit {
        size_t index;
        const PaintSet* set;
        const LottieShapeItem* group;
    };
    std::vector<Unit> units;
    for (const PaintSet& ps : sets) {
        units.push_back(Unit{ps.scopeEnd, &ps, nullptr});
    }
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].kind == LottieShapeItem::Kind::Group) {
            units.push_back(Unit{i, nullptr, &items[i]});
        }
    }
    std::sort(units.begin(), units.end(), [](const Unit& a, const Unit& b) { return a.index > b.index; });

    int ordinal = 0;
    bool modsEmitted = false;
    for (const Unit& u : units) {
        if (u.group != nullptr) {
            processShapeItems(ctx, layer, u.group->items, into, map, ctx.writer.intern(u.group->name));
            continue;
        }
        const PaintSet& ps = *u.set;
        ++ordinal;
        const std::string suffix = ordinal == 1 ? std::string() : "_" + std::to_string(ordinal);
        const LottieShapeItem* first = ps.stroke != nullptr ? ps.stroke : ps.fill;
        uint32_t target = into;
        if (wrap) {
            target =
                appendNode(ctx, defaultGroupNode(into, ctx.writer.intern(layer.name + "#" + first->name)));
            setLabel(ctx, target, first->name + "Group");
        }
        const bool withMods = wrap || !modsEmitted;
        modsEmitted = true;
        const bool dashed = (ps.fill != nullptr && !ps.fill->dashes.empty()) ||
                            (ps.stroke != nullptr && !ps.stroke->dashes.empty());
        bool drewAny = false;
        for (size_t i = 0; i < items.size(); ++i) {
            const LottieShapeItem& it = items[i];
            if (isGeometryKind(it.kind) && i < ps.scopeEnd) {
                bool startMatters = dashed;
                for (const LottieShapeItem* m : mods) {
                    const auto mi = static_cast<size_t>(m - items.data());
                    if (mi > i && (m->kind == LottieShapeItem::Kind::Trim ||
                                   m->kind == LottieShapeItem::Kind::ZigZag)) {
                        startMatters = true;
                    }
                }
                const StringId nm = ctx.writer.intern(layer.name + "#" + first->name + "_" + it.name);
                uint32_t nodeIdx = kInvalidNode;
                if (ctx.native && it.kind != LottieShapeItem::Kind::Path) {
                    nodeIdx = appendShapeNode(ctx, layer, it, target, map, nm, startMatters);
                    if (nodeIdx != kInvalidNode) {
                        const uint32_t si = ctx.scene.nodes[nodeIdx].kindIndex;
                        for (const LottieShapeItem* paint : {ps.fill, ps.stroke}) {
                            if (paint != nullptr) {
                                applyPaintTo(ctx, layer, nodeIdx, ctx.scene.shapes[si], *paint, map);
                            }
                        }
                    }
                }
                if (nodeIdx == kInvalidNode) {
                    nodeIdx = appendGeometryNode(ctx, layer, it, nullptr, target, map, nm);
                    const uint32_t pathIndex = ctx.scene.nodes[nodeIdx].kindIndex;
                    for (const LottieShapeItem* paint : {ps.fill, ps.stroke}) {
                        if (paint != nullptr) {
                            applyPaint(ctx, layer, nodeIdx, pathIndex, *paint, map);
                        }
                    }
                }
                setLabel(ctx, nodeIdx, it.name + suffix);
                drewAny = true;
            } else if (withMods && isModifierKind(it.kind) &&
                       std::find(mods.begin(), mods.end(), &it) != mods.end()) {
                appendModifierNode(ctx, layer, it, target, map, it.name + suffix);
            }
        }
        if (!drewAny) {
            ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": paint \"" + first->name +
                                        "\" has no shape above it in its group; it draws nothing");
        }
    }
}

void processShapeItems(BuildCtx& ctx, const LottieLayer& layer, const std::vector<LottieShapeItem>& items,
                       uint32_t parentIdx, TimeMap map, StringId groupName) {
    const LottieShapeItem* transformItem = nullptr;
    for (const LottieShapeItem& it : items) {
        if (it.kind == LottieShapeItem::Kind::Transform) {
            transformItem = &it;
            break;
        }
    }

    uint32_t gPrime = parentIdx;
    if (transformItem != nullptr) {
        const LottieTransform& t = transformItem->transform.base;
        IrSceneNode node = defaultGroupNode(parentIdx, groupName);
        node.anchor = firstOrStatic(t.anchor);
        if (t.splitPosition) {
            node.translate = Vec2{firstOrStatic(t.posX), firstOrStatic(t.posY)};
        } else {
            node.translate = firstOrStatic(t.position);
        }
        Vec2 rawScale = firstOrStatic(t.scale);
        node.scale = Vec2{rawScale.x / 100.0f, rawScale.y / 100.0f};
        node.rotation = firstOrStatic(t.rotation);
        node.skew = Vec2{firstOrStatic(t.skew), firstOrStatic(t.skewAxis)};
        node.opacity = firstOrStatic(t.opacity) / 100.0f;
        gPrime = appendNode(ctx, node);

        buildVec2Track(ctx, t.anchor, gPrime, prop::Anchor, map, layer, "Anchor");
        if (t.splitPosition) {
            buildFloatTrack(ctx, t.posX, gPrime, prop::TranslateX, map, layer, "TranslateX");
            buildFloatTrack(ctx, t.posY, gPrime, prop::TranslateY, map, layer, "TranslateY");
        } else {
            buildVec2Track(ctx, t.position, gPrime, prop::Translate, map, layer, "Translate");
        }
        buildVec2Track(ctx, t.scale, gPrime, prop::Scale, map, layer, "Scale", 0.01f);
        buildFloatTrack(ctx, t.rotation, gPrime, prop::Rotation, map, layer, "Rotation");
        buildFloatTrack(ctx, t.skew, gPrime, prop::SkewX, map, layer, "SkewX");
        buildFloatTrack(ctx, t.skewAxis, gPrime, prop::SkewY, map, layer, "SkewY");
        buildFloatTrack(ctx, t.opacity, gPrime, prop::Opacity, map, layer, "Opacity", 0.01f);
    }

    // T-230: the modifiers, in stack order. A repeater with a nested group before it (Modifier nodes
    // only reach the paths directly in their group, 31 §31.6.1) or with a transform the runtime
    // repeat cannot draw is expanded at compile time as before (an animated `c` then uses its first
    // key). Offset/merge paths only exist as editor-baked modifiers: the `<anim>` builder ignores
    // them.
    std::vector<const LottieShapeItem*> mods;
    const LottieShapeItem* repeater = nullptr;  // The compile-time expanded one, if any.
    bool groupSeen = false;
    bool modAfterGroup = false;
    for (const LottieShapeItem& it : items) {
        if (it.kind == LottieShapeItem::Kind::Group) {
            groupSeen = true;
            continue;
        }
        if (!isModifierKind(it.kind)) {
            continue;
        }
        if (it.kind == LottieShapeItem::Kind::Repeater && (groupSeen || !repeaterAsModifier(ctx, it))) {
            if (repeater == nullptr) {
                repeater = &it;
                if (it.copies.animated) {
                    ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": repeater \"" + it.name +
                                                "\" is expanded at compile time; its animated copies use the "
                                                "first key");
                }
            } else {
                ctx.sink.warn(ctx.file,
                              lottieLayerDesc(layer) +
                                  ": multiple expanded repeaters in one group; using the first one");
            }
            continue;
        }
        if (!ctx.native &&
            (it.kind == LottieShapeItem::Kind::OffsetPath || it.kind == LottieShapeItem::Kind::MergePaths)) {
            if (!ctx.warnedBakedModifier) {
                ctx.sink.warn(ctx.file,
                              lottieLayerDesc(layer) +
                                  ": offset paths / merge paths are only baked by the editor after a "
                                  "native import; ignored");
                ctx.warnedBakedModifier = true;
            }
            continue;
        }
        modAfterGroup = modAfterGroup || groupSeen;
        mods.push_back(&it);
    }
    if (modAfterGroup) {
        ctx.sink.warn(ctx.file,
                      lottieLayerDesc(layer) +
                          ": a path modifier does not reach the shapes of the nested groups before it");
    }
    const bool plainTrim =
        mods.empty() ||
        (mods.size() == 1 && mods[0]->kind == LottieShapeItem::Kind::Trim && mods[0]->trimMode == 1);
    if (ctx.native || !plainTrim) {
        auto emit = [&](uint32_t into) { emitStructured(ctx, layer, items, mods, into, map); };
        if (repeater != nullptr) {
            applyRepeater(ctx, layer, *repeater, map, gPrime, emit);
        } else {
            emit(gPrime);
        }
        return;
    }

    // The `<anim>` builder's original emission for a group without modifier nodes (at most one
    // parallel trim, written onto each path), so those fixtures render exactly as before.
    std::vector<const LottieShapeItem*> geoms;
    const LottieShapeItem* trim = mods.empty() ? nullptr : mods[0];
    const LottieShapeItem* roundCorners = nullptr;
    // (index in `items`, is style [true] or subgroup [false], pointer).
    std::vector<std::pair<size_t, const LottieShapeItem*>> styleUnits;
    std::vector<std::pair<size_t, const LottieShapeItem*>> groupUnits;

    for (size_t i = 0; i < items.size(); ++i) {
        const LottieShapeItem& it = items[i];
        if (isGeometryKind(it.kind)) {
            geoms.push_back(&it);
        } else if (isPaintKind(it.kind)) {
            styleUnits.emplace_back(i, &it);
        } else if (it.kind == LottieShapeItem::Kind::Group) {
            groupUnits.emplace_back(i, &it);
        }
    }

    // Geometry with no styles in this group produces no Path nodes at all (docs/tasks/T-15.md
    // "樣式映射"): simply never referenced below since only styleUnits drive Path node creation.

    // Merge style-units and group-units into one draw-order list, sorted by descending original
    // index (reverse array order): docs/tasks/T-15.md rule 4's "混合排序規則".
    struct Unit {
        size_t index;
        bool isStyle;
        const LottieShapeItem* item;
    };
    std::vector<Unit> units;
    units.reserve(styleUnits.size() + groupUnits.size());
    for (const auto& s : styleUnits) {
        units.push_back(Unit{s.first, true, s.second});
    }
    for (const auto& g : groupUnits) {
        units.push_back(Unit{g.first, false, g.second});
    }
    std::sort(units.begin(), units.end(), [](const Unit& a, const Unit& b) { return a.index > b.index; });

    std::vector<uint32_t> trimNodeIndices;
    std::vector<uint32_t> trimPathIndices;

    auto emitAllUnits = [&](uint32_t into) {
        for (const Unit& u : units) {
            if (u.isStyle) {
                bool warnedPrecedes = false;
                for (const LottieShapeItem* g : geoms) {
                    size_t geomIdx =
                        static_cast<size_t>(std::find(geoms.begin(), geoms.end(), g) - geoms.begin());
                    if (u.index < geomIdx && !warnedPrecedes) {
                        ctx.sink.warn(ctx.file,
                                      lottieLayerDesc(layer) + ": style precedes geometry; applied anyway");
                        warnedPrecedes = true;
                    }
                    StringId nm = ctx.writer.intern(layer.name + "#" + u.item->name + "_" + g->name);
                    const AnimatedValue<float>* roundRadius =
                        roundCorners != nullptr ? &roundCorners->radius : nullptr;
                    uint32_t nodeIdx = appendGeometryNode(ctx, layer, *g, roundRadius, into, map, nm);
                    uint32_t pathIndex = ctx.scene.nodes[nodeIdx].kindIndex;
                    applyPaint(ctx, layer, nodeIdx, pathIndex, *u.item, map);
                    trimNodeIndices.push_back(nodeIdx);
                    trimPathIndices.push_back(pathIndex);
                }
            } else {
                StringId nm = ctx.writer.intern(u.item->name);
                processShapeItems(ctx, layer, u.item->items, into, map, nm);
            }
        }
    };

    if (repeater != nullptr) {
        applyRepeater(ctx, layer, *repeater, map, gPrime, emitAllUnits);
    } else {
        emitAllUnits(gPrime);
    }

    if (trim != nullptr && !trimNodeIndices.empty()) {
        applyTrim(ctx, layer, trimNodeIndices, trimPathIndices, *trim, map);
    }
}

}  // namespace

void buildShapeLayerContent(BuildCtx& ctx, const LottieLayer& layer, uint32_t contentGroupIdx, TimeMap map) {
    processShapeItems(ctx, layer, layer.shapes, contentGroupIdx, map, kInvalidString);
}

}  // namespace xt::compile
