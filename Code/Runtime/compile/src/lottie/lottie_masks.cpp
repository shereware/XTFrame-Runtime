#include <cstdint>
#include <string>
#include <vector>

#include "lottie_geometry.h"
#include "lottie_model.h"
#include "lottie_shapes.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/value.h"

/// masksProperties + track matte -> Mask scene nodes (T-16). See Docs/design/11-lottie-mapping.md
/// §11.1, §11.3, Docs/tasks/T-16.md "遮罩" / "Track matte".

namespace xt::compile {

namespace {

IrSceneNode defaultMaskNode(uint32_t parent, MaskMode mode, StringId name) {
    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Mask);
    n.visible = 1;
    n.blend = 0;
    n.maskMode = static_cast<uint8_t>(mode);
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

uint32_t appendMaskNode(BuildCtx& ctx, uint32_t parentIdx, MaskMode mode, StringId name) {
    uint32_t idx = static_cast<uint32_t>(ctx.scene.nodes.size());
    ctx.scene.nodes.push_back(defaultMaskNode(parentIdx, mode, name));
    return idx;
}

}  // namespace

uint32_t appendTrackMatteMaskNode(BuildCtx& ctx, uint32_t parentIdx, int matteType, StringId name) {
    MaskMode mode = MaskMode::Alpha;
    switch (matteType) {
        case 1:
            mode = MaskMode::Alpha;
            break;
        case 2:
            mode = MaskMode::AlphaInverted;
            break;
        case 3:
            mode = MaskMode::Luma;
            break;
        case 4:
            mode = MaskMode::LumaInverted;
            break;
        default:
            XT_ASSERT(false && "matteType must be validated by the caller (1..4)");
            break;
    }
    return appendMaskNode(ctx, parentIdx, mode, name);
}

void buildLayerMasks(BuildCtx& ctx, const LottieLayer& layer, uint32_t contentGroupIdx, TimeMap map) {
    if (layer.masks.empty()) {
        return;
    }

    size_t validCount = 0;
    for (const LottieMask& m : layer.masks) {
        if (m.mode == 'a' || m.mode == 's' || m.mode == 'i') {
            ++validCount;
        }
    }

    for (size_t mi = 0; mi < layer.masks.size(); ++mi) {
        const LottieMask& m = layer.masks[mi];
        if (m.mode != 'a' && m.mode != 's' && m.mode != 'i') {
            ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": mask mode \"" +
                                        std::string(1, static_cast<char>(m.mode)) +
                                        "\" is not supported; skipped");
            continue;
        }
        if (firstOrStatic(m.x) != 0.0f) {
            ctx.sink.warn(ctx.file,
                          lottieLayerDesc(layer) + ": mask expansion (\"x\") is not supported; ignored");
        }

        MaskMode base = m.mode == 'a'   ? MaskMode::Add
                        : m.mode == 's' ? MaskMode::Subtract
                                        : MaskMode::Intersect;
        MaskMode finalMode = base;
        if (m.inverted) {
            if (validCount == 1) {
                if (base == MaskMode::Add) {
                    finalMode = MaskMode::AlphaInverted;
                } else if (base == MaskMode::Subtract) {
                    finalMode = MaskMode::Add;
                } else {
                    finalMode = MaskMode::AlphaInverted;  // Intersect
                }
            } else {
                ctx.sink.warn(ctx.file, lottieLayerDesc(layer) +
                                            ": inverted mask combined with other masks is not "
                                            "supported; inversion ignored");
                finalMode = base;
            }
        }

        std::string maskLabel = layer.name + "#mask" + std::to_string(mi);
        uint32_t maskNodeIdx = appendMaskNode(ctx, contentGroupIdx, finalMode, ctx.writer.intern(maskLabel));

        // Path child: solid white fill, NonZero, FillOpacity = o/100.
        bool pathAnimated = m.pt.animated && m.pt.keys.size() > 1;
        if (pathAnimated) {
            size_t n0 = m.pt.keys.front().value.v.size();
            for (const auto& k : m.pt.keys) {
                if (k.value.v.size() != n0) {
                    ctx.sink.warn(ctx.file, lottieLayerDesc(layer) +
                                                ": animated mask path keyframes have differing vertex "
                                                "counts; using the first keyframe as a static path");
                    pathAnimated = false;
                    break;
                }
            }
        }

        LottiePath initialPath = pathAnimated ? m.pt.keys.front().value : firstOrStatic(m.pt);
        GeneratedPath gp = buildPathFromLottiePath(initialPath, 1);

        uint32_t verbOffset = static_cast<uint32_t>(ctx.scene.verbPool.size());
        for (uint8_t v : gp.verbs) {
            ctx.scene.verbPool.push_back(v);
        }
        uint32_t pointOffset = static_cast<uint32_t>(ctx.scene.floatPool.size());
        for (float f : gp.points) {
            ctx.scene.floatPool.push_back(f);
        }

        IrPathData path{};
        path.verbOffset = verbOffset;
        path.verbCount = static_cast<uint32_t>(gp.verbs.size());
        path.pointOffset = pointOffset;
        path.pointCount = static_cast<uint32_t>(gp.points.size());
        path.fillRule = static_cast<uint8_t>(FillRule::NonZero);
        path.fillKind = static_cast<uint8_t>(PaintKind::Solid);
        path.strokeKind = static_cast<uint8_t>(PaintKind::None);
        path.cap = 0;
        path.join = 0;
        path.fillColor = Color{1.0f, 1.0f, 1.0f, firstOrStatic(m.o) / 100.0f};
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

        uint32_t pathIndex = static_cast<uint32_t>(ctx.scene.paths.size());
        ctx.scene.paths.push_back(path);

        IrSceneNode pathNode{};
        pathNode.kind = static_cast<uint8_t>(NodeKind::Path);
        pathNode.visible = 1;
        pathNode.parent = maskNodeIdx;
        pathNode.kindIndex = pathIndex;
        pathNode.opacity = 1.0f;
        pathNode.anchor = Vec2{0, 0};
        pathNode.translate = Vec2{0, 0};
        pathNode.scale = Vec2{1, 1};
        pathNode.rotation = 0.0f;
        pathNode.skew = Vec2{0, 0};
        pathNode.name = ctx.writer.intern(maskLabel + "#path");
        uint32_t pathNodeIdx = static_cast<uint32_t>(ctx.scene.nodes.size());
        ctx.scene.nodes.push_back(pathNode);

        if (pathAnimated) {
            uint32_t kfOffset = static_cast<uint32_t>(ctx.timeline.keyframes.size());
            for (const LottieKeyframe<LottiePath>& k : m.pt.keys) {
                GeneratedPath kgp = buildPathFromLottiePath(k.value, 1);
                uint32_t floatOffset = static_cast<uint32_t>(ctx.timeline.floatPool.size());
                for (float f : kgp.points) {
                    ctx.timeline.floatPool.push_back(f);
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
                irSetValueU32(kf.value, 0, floatOffset);
                irSetValueU32(kf.value, 1, static_cast<uint32_t>(kgp.points.size()));
                ctx.timeline.keyframes.push_back(kf);
            }
            IrTrack track{};
            track.target = pathNodeIdx;
            track.prop = prop::PathData;
            track.type = static_cast<uint8_t>(ValueType::Path);
            track.kfOffset = kfOffset;
            track.kfCount = static_cast<uint32_t>(m.pt.keys.size());
            ctx.timeline.tracks.push_back(track);
        }

        if (m.o.animated) {
            buildFloatTrack(ctx, m.o, pathNodeIdx, prop::FillOpacity, map, layer, "Mask FillOpacity", 0.01f);
        }
    }
}

}  // namespace xt::compile
