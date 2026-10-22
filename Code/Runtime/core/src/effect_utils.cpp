#include "xt/effect_utils.h"

#include <algorithm>
#include <cmath>

/// effectToScreen / effectExpand. See Docs/design/18-effects.md §18.3, §18.5.

namespace xt {

namespace {

/// Docs/design/18-effects.md §18.3: radius-like parameters are capped at 64 device px after the
/// conversion to screen units. Mirrored by the backends (sw_effects.cpp's clampDevicePx64,
/// gl_effects.cpp's), which warn once when they clamp; this read-range computation must use the
/// same cap so the damage rectangle and the backdrop copy agree with what the blur actually reads.
constexpr float kMaxEffectDevicePx = 64.0f;

constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kRadToDeg = 57.29577951308232f;

/// Transforms a direction vector by `xf`'s linear part (no translation).
Vec2 xformVector(const Mat2x3& xf, float vx, float vy) {
    return Vec2{xf.a * vx + xf.c * vy, xf.b * vx + xf.d * vy};
}

/// Transforms a point by the full `xf` (linear part + translation).
Vec2 xformPoint(const Mat2x3& xf, float px, float py) {
    return Vec2{xf.a * px + xf.c * py + xf.e, xf.b * px + xf.d * py + xf.f};
}

}  // namespace

EffectInstance effectToScreen(const EffectInstance& e, const Mat2x3& xf) {
    EffectInstance out = e;
    float det = xf.a * xf.d - xf.b * xf.c;
    float s = std::sqrt(std::fabs(det));

    switch (e.kind) {
        case EffectKind::DropShadow: {
            Vec2 off = xformVector(xf, e.params[0], e.params[1]);
            out.params[0] = off.x;
            out.params[1] = off.y;
            out.params[2] = e.params[2] * s;  // blur
            out.params[3] = e.params[3] * s;  // spread
            break;
        }
        case EffectKind::Glow:
            out.params[0] = e.params[0] * s;  // radius
            break;
        case EffectKind::Blur:
            out.params[0] = e.params[0] * s;  // radiusX
            out.params[1] = e.params[1] * s;  // radiusY
            break;
        case EffectKind::GradientMask: {
            Vec2 p0 = xformPoint(xf, e.params[0], e.params[1]);
            Vec2 p1 = xformPoint(xf, e.params[2], e.params[3]);
            out.params[0] = p0.x;
            out.params[1] = p0.y;
            out.params[2] = p1.x;
            out.params[3] = p1.y;
            break;
        }
        case EffectKind::Shine: {
            out.params[1] = e.params[1] * s;  // width
            float rad = e.params[3] * kDegToRad;
            Vec2 dir = xformVector(xf, std::cos(rad), std::sin(rad));
            out.params[3] = std::atan2(dir.y, dir.x) * kRadToDeg;  // angle
            break;
        }
        case EffectKind::Outline:
            out.params[0] = e.params[0] * s;  // radius
            out.params[1] = e.params[1] * s;  // softness
            break;
        case EffectKind::BackdropBlur:
            out.params[0] = e.params[0] * s;  // radius
            break;
        case EffectKind::None:
        case EffectKind::ColorAdjust:
        case EffectKind::Custom:
            break;
    }
    return out;
}

float effectExpand(const EffectInstance& screenParams) {
    switch (screenParams.kind) {
        case EffectKind::DropShadow: {
            // inset shadows stay inside the shape, so they expand nothing (18.3's table).
            if (screenParams.params[5] != 0.0f) {
                return 0.0f;
            }
            float offsetX = screenParams.params[0];
            float offsetY = screenParams.params[1];
            float blur = screenParams.params[2];
            float spread = screenParams.params[3];
            float sigma = blur * 0.5f;
            return std::max(std::fabs(offsetX), std::fabs(offsetY)) + spread + 3.0f * sigma;
        }
        case EffectKind::Glow: {
            bool inner = screenParams.params[2] != 0.0f;
            if (inner) {
                return 0.0f;
            }
            float sigma = screenParams.params[0] * 0.5f;
            return 3.0f * sigma;
        }
        case EffectKind::Blur: {
            float sigmaX = screenParams.params[0] * 0.5f;
            float sigmaY = screenParams.params[1] * 0.5f;
            return 3.0f * std::max(sigmaX, sigmaY);
        }
        case EffectKind::Outline: {
            // position: 0 outer, 1 center, 2 inner (Docs/design/18-effects.md §18.3).
            if (screenParams.params[2] >= 2.0f) {
                return 0.0f;
            }
            return screenParams.params[0] + screenParams.params[1];
        }
        case EffectKind::None:
        case EffectKind::ColorAdjust:
        case EffectKind::GradientMask:
        case EffectKind::Shine:
        // BackdropBlur draws nothing outside the content (its blur only *reads* outwards; see
        // effectReadExpand), so it expands the content AABB by 0 (18.3's table).
        case EffectKind::BackdropBlur:
        case EffectKind::Custom:
            return 0.0f;
    }
    return 0.0f;
}

float customEffectExpand(const ShaderRef* ref, const Mat2x3& xf) {
    if (ref == nullptr) {
        return 0.0f;
    }
    const float s = std::sqrt(std::fabs(xf.a * xf.d - xf.b * xf.c));
    float expand = std::max(0.0f, ref->expandPx) * s;
    if (ref->fallback.kind != EffectKind::None && ref->fallback.kind != EffectKind::Custom) {
        expand = std::max(expand, effectExpand(effectToScreen(ref->fallback, xf)));
    }
    return expand;
}

float effectReadExpand(const EffectInstance& screenParams) {
    if (screenParams.enabled == 0 || screenParams.kind != EffectKind::BackdropBlur) {
        return 0.0f;
    }
    float radius = std::min(std::max(0.0f, screenParams.params[0]), kMaxEffectDevicePx);
    return 3.0f * (radius * 0.5f);
}

// --- T-343 (Docs/design/18-effects.md 18.15) -----------------------------------------------

int blurDownsampleShift(float sigma) {
    if (!(sigma > 4.0f)) {
        return 0;
    }
    if (sigma > 16.0f) {
        return 3;
    }
    return sigma > 8.0f ? 2 : 1;
}

float backdropPyramidSigma(int level) {
    // Variance per level: the 2 x 2 box of the previous level's texels (1/4 of a previous texel
    // squared = 1/16 of this level's) plus the Kawase pass (half the weight on four 2 x 2 blocks
    // 1.5 texels away: 0.5 * (1.5^2 + 0.25) = 1.25 of this level's texel squared), in full
    // resolution px^2 (a level-l texel is 2^l px).
    float variance = 0.0f;
    float texelPx2 = 1.0f;  // (2^level)^2
    for (int l = 1; l <= level && l <= kBackdropPyramidMaxLevels; ++l) {
        texelPx2 *= 4.0f;
        variance += (1.0f / 16.0f + 1.25f) * texelPx2;
    }
    return std::sqrt(variance);
}

int backdropPyramidLevels(float maxSigma) {
    if (!(maxSigma > 0.0f)) {
        return 0;
    }
    for (int l = 1; l <= kBackdropPyramidMaxLevels; ++l) {
        if (backdropPyramidSigma(l) >= maxSigma) {
            return l;
        }
    }
    return kBackdropPyramidMaxLevels;
}

void backdropPyramidMix(float sigma, int levels, int& lower, int& upper, float& t) {
    levels = std::clamp(levels, 0, kBackdropPyramidMaxLevels);
    lower = levels;
    upper = levels;
    t = 0.0f;
    if (!(sigma > 0.0f) || levels == 0) {
        lower = upper = 0;
        return;
    }
    for (int l = 1; l <= levels; ++l) {
        const float lo = backdropPyramidSigma(l - 1);
        const float hi = backdropPyramidSigma(l);
        if (sigma <= hi) {
            lower = l - 1;
            upper = l;
            t = hi > lo ? std::clamp((sigma - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
            return;
        }
    }
}

}  // namespace xt
