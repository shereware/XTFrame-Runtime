#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

#include "xt/command_list.h"
#include "xt/effect_utils.h"
#include "xt/geom.h"
#include "xt/node.h"
#include "xt/project3d.h"
#include "xt/types.h"

/// Backend-independent geometry and effect-parameter arithmetic shared by the GPU frame builder
/// (gpu/src/frame_builder.cpp, gpu_effects.cpp) and the software renderer (sw/src/sw_renderer.cpp,
/// sw_effects.cpp, sw_effect_math.cpp) -- T-414 C2c. Pure functions only; each one used to be a copy
/// in both backends, with the same arithmetic, so the two stay within the golden tolerance.

namespace xt::effect_math {

inline constexpr float kDegToRad = 0.017453292519943295f;

/// Bounding box of a raw (untransformed) path point pool: every float pair in `points` (Move/Line
/// endpoints and Cubic control points alike -- a loose box is fine, it only feeds
/// project3d::projectedScaleMax's tolerance estimate, docs/design/19-3d-space.md §19.7). Empty (or
/// odd-sized, which never happens for a well-formed path) input returns a zero Rect.
inline Rect boundsOfPoints(std::span<const float> points) {
    if (points.size() < 2) {
        return Rect{};
    }
    float minX = points[0];
    float maxX = points[0];
    float minY = points[1];
    float maxY = points[1];
    for (size_t i = 0; i + 1 < points.size(); i += 2) {
        minX = std::min(minX, points[i]);
        maxX = std::max(maxX, points[i]);
        minY = std::min(minY, points[i + 1]);
        maxY = std::max(maxY, points[i + 1]);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

/// §19.7's FillPath/StrokePath/PushClipPath flattening tolerance: kFlattenTolerance when no
/// projection is active, else divided by projectedScaleMax so the *projected* (device-space) chord
/// error stays ~kFlattenTolerance. `deviceXf` is the command's own dpr-scaled xf (the flattening
/// transform); `rawPoints` is the unflattened path's point pool (fed to boundsOfPoints for the
/// tolerance estimate only).
inline float flattenTolerance(std::span<const project3d::Mapping> mappings, const Mat2x3& deviceXf,
                              std::span<const float> rawPoints) {
    if (mappings.empty()) {
        return kFlattenTolerance;
    }
    float sMax = project3d::projectedScaleMax(mappings, deviceXf, boundsOfPoints(rawPoints));
    return kFlattenTolerance / sMax;
}

/// T-160: the corners (clockwise from the top left) of the plane-space AABB of `planePts`, the shape
/// before projection, from which a projected gradient paint's plane <-> screen homography is built.
/// Returns false for an empty point set.
inline bool planeBoxCorners(std::span<const Vec2> planePts, Vec2 (&out)[4]) {
    if (planePts.empty()) {
        return false;
    }
    float x0 = planePts[0].x, y0 = planePts[0].y, x1 = x0, y1 = y0;
    for (const Vec2& p : planePts) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    out[0] = Vec2{x0, y0};
    out[1] = Vec2{x1, y0};
    out[2] = Vec2{x1, y1};
    out[3] = Vec2{x0, y1};
    return true;
}

/// T-343 (docs/design/23-host-integration.md §23.aa): a screen-space BackdropBlur's sigma.
inline float backdropSigma(const EffectInstance& screenParams) {
    return std::min(std::max(0.0f, screenParams.params[0]), 64.0f) * 0.5f;
}

/// 18 §18.15: every pyramid level reads one texel past the region, i.e. 2^levels px.
inline float pyramidMargin(float sigma) {
    return static_cast<float>(1 << backdropPyramidLevels(std::min(sigma, 32.0f)));
}

/// §23.aa.3: the PushEffect that opens scene-read run `run` when its first effect is an enabled
/// BackdropBlur (the run then reads a region of the scene), else nullptr.
inline const EffectCmd* leadingBackdropBlur(const CommandList& list, const SceneRun& run) {
    if (!run.sceneRead || run.first >= list.commands.size()) {
        return nullptr;
    }
    const Command& cmd = list.commands[run.first];
    if (cmd.kind != CmdKind::PushEffect) {
        return nullptr;
    }
    const EffectCmd& c = list.effectCmds[cmd.index];
    if (c.effectCount == 0 || c.effectOffset >= list.effects.size()) {
        return nullptr;
    }
    const EffectInstance& first = list.effects[c.effectOffset];
    if (first.kind != EffectKind::BackdropBlur || first.enabled == 0) {
        return nullptr;
    }
    return &c;
}

/// 18 §18.6: xt::sw's box width per pass for a Gaussian `sigma`, `w = floor(sqrt(12*sigma^2/3 + 1))`
/// forced odd (even -> w - 1); `w < 1` means the axis is left unchanged.
inline int boxBlurWidth(float sigma) {
    float sigma2 = sigma * sigma;
    int w = static_cast<int>(std::floor(std::sqrt(12.0f * sigma2 / 3.0f + 1.0f)));
    if (w % 2 == 0) {
        w -= 1;
    }
    return w;
}

/// 18 §18.3 Shine: the band's direction, its centre along that direction (in the space of
/// `contentBoundsPx`) and its smoothstep edges, from the effect's position / width / softness /
/// angle parameters.
struct ShineBand {
    Vec2 dir;
    float center = 0.0f;
    float edge0 = 0.0f;
    float edge1 = 0.0f;
};

inline ShineBand shineBand(const EffectInstance& p, const Rect& contentBoundsPx) {
    float position = p.params[0];
    float width = std::max(0.0f, p.params[1]);
    float softness = std::clamp(p.params[2], 0.0f, 1.0f);
    float angleDeg = p.params[3];

    float rad = angleDeg * kDegToRad;
    Vec2 d{std::cos(rad), std::sin(rad)};
    const Rect& cb = contentBoundsPx;
    Vec2 corners[4] = {
        Vec2{cb.x, cb.y},
        Vec2{cb.x + cb.w, cb.y},
        Vec2{cb.x, cb.y + cb.h},
        Vec2{cb.x + cb.w, cb.y + cb.h},
    };
    float lo = corners[0].x * d.x + corners[0].y * d.y;
    float hi = lo;
    for (int i = 1; i < 4; ++i) {
        float proj = corners[i].x * d.x + corners[i].y * d.y;
        lo = std::min(lo, proj);
        hi = std::max(hi, proj);
    }
    ShineBand band;
    band.dir = d;
    band.center = lo + (hi - lo) * position;
    band.edge0 = width * (1.0f - softness) * 0.5f;
    band.edge1 = width * 0.5f;
    return band;
}

}  // namespace xt::effect_math
