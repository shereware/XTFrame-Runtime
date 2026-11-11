#include "xt/box_shadow.h"

#include <algorithm>
#include <cmath>

#include "effect_math.h"
#include "rounded_rect.h"

/// Analytic box shadows. See xt/box_shadow.h, Docs/design/10-backends.md §10.12 item 2 and
/// Docs/tasks/T-668.md.

namespace xt {

namespace {

constexpr float kSqrt2 = 1.41421356237309505f;
/// 1 / sqrt(2 pi): the unit-area Gaussian's normalisation.
constexpr float kInvSqrt2Pi = 0.398942280401432678f;

bool finite(float v) { return std::isfinite(v); }

/// T-680: `inner` lies inside `outer` (as rectangles; the Painter's nested terms share their corner
/// centres, so this is containment of the rounded rectangles too).
bool shapeContains(const BoxShadowShape& outer, const BoxShadowShape& inner) {
    return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 && outer.x1 >= inner.x1 && outer.y1 >= inner.y1;
}

}  // namespace

float boxShadowErf(float x) {
    const float ax = std::fabs(x);
    const float t = 1.0f / (1.0f + kBoxShadowErfP * ax);
    const float poly =
        t * (kBoxShadowErfA1 +
             t * (kBoxShadowErfA2 + t * (kBoxShadowErfA3 + t * (kBoxShadowErfA4 + t * kBoxShadowErfA5))));
    const float y = 1.0f - poly * std::exp(-ax * ax);
    return x < 0.0f ? -y : y;
}

bool boxShadowShape(const Rect& rect, const float radii[4], float spread, Vec2 offset, BoxShadowShape& out) {
    if (!finite(rect.x) || !finite(rect.y) || !finite(rect.w) || !finite(rect.h) || !finite(spread) ||
        !finite(offset.x) || !finite(offset.y)) {
        return false;
    }
    const float x0 = rect.x - spread + offset.x;
    const float y0 = rect.y - spread + offset.y;
    const float w = rect.w + 2.0f * spread;
    const float h = rect.h + 2.0f * spread;
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return false;
    }
    float r[4];
    for (int i = 0; i < 4; ++i) {
        if (!finite(radii[i])) {
            return false;
        }
        r[i] = std::max(0.0f, radii[i] + spread);
    }
    clampRoundedRectRadii(w, h, r[0], r[1], r[2], r[3]);
    out.x0 = x0;
    out.y0 = y0;
    out.x1 = x0 + w;
    out.y1 = y0 + h;
    for (int i = 0; i < 4; ++i) {
        out.r[i] = r[i];
    }
    return true;
}

bool boxShadowSpan(const BoxShadowShape& s, float y, float& x0, float& x1) {
    if (y < s.y0 || y > s.y1) {
        return false;
    }
    // Left edge: the top-left corner's circle above y0 + r_tl, the bottom-left one below y1 - r_bl.
    x0 = s.x0;
    const float rtl = s.r[0], rbl = s.r[3];
    if (y < s.y0 + rtl) {
        const float d = s.y0 + rtl - y;
        x0 = s.x0 + rtl - std::sqrt(std::max(0.0f, rtl * rtl - d * d));
    } else if (y > s.y1 - rbl) {
        const float d = y - (s.y1 - rbl);
        x0 = s.x0 + rbl - std::sqrt(std::max(0.0f, rbl * rbl - d * d));
    }
    x1 = s.x1;
    const float rtr = s.r[1], rbr = s.r[2];
    if (y < s.y0 + rtr) {
        const float d = s.y0 + rtr - y;
        x1 = s.x1 - rtr + std::sqrt(std::max(0.0f, rtr * rtr - d * d));
    } else if (y > s.y1 - rbr) {
        const float d = y - (s.y1 - rbr);
        x1 = s.x1 - rbr + std::sqrt(std::max(0.0f, rbr * rbr - d * d));
    }
    return x1 > x0;
}

void boxShadowRow(const BoxShadowShape& s, float py, float sigma, BoxShadowRow& row) {
    const float reach = kBoxShadowSigmaReach * sigma;
    const float a = std::max(s.y0, py - reach);
    const float b = std::min(s.y1, py + reach);
    row.empty = !(a < b);
    if (row.empty) {
        return;
    }
    const float half = 0.5f * (b - a);
    const float mid = 0.5f * (a + b);
    row.half = half;
    row.sigma = sigma;
    row.invErf = 1.0f / (sigma * kSqrt2);
    const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
    for (int i = 0; i < kBoxShadowQuadratureHalf; ++i) {
        const float d = half * kBoxShadowQuadratureNodes[i];
        for (int side = 0; side < 2; ++side) {
            const int k = 2 * i + side;
            const float y = side == 0 ? mid - d : mid + d;
            float x0 = 0.0f, x1 = 0.0f;
            if (!boxShadowSpan(s, y, x0, x1)) {
                row.wg[k] = 0.0f;
                row.x0[k] = row.x1[k] = 0.0f;
                continue;
            }
            const float t = y - py;
            row.wg[k] = kBoxShadowQuadratureWeights[i] * std::exp(-t * t * invTwoSigma2);
            row.x0[k] = x0;
            row.x1[k] = x1;
        }
    }
}

float boxShadowCoverageAt(const BoxShadowRow& row, float px) {
    if (row.empty) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (int k = 0; k < 2 * kBoxShadowQuadratureHalf; ++k) {
        if (row.wg[k] == 0.0f) {
            continue;
        }
        const float e = 0.5f * (boxShadowErf((row.x1[k] - px) * row.invErf) -
                                boxShadowErf((row.x0[k] - px) * row.invErf));
        sum += row.wg[k] * e;
    }
    const float coverage = sum * row.half * kInvSqrt2Pi / row.sigma;
    return std::clamp(coverage, 0.0f, 1.0f);
}

float boxShadowCoverage(const BoxShadowShape& s, float px, float py, float sigma) {
    BoxShadowRow row;
    boxShadowRow(s, py, sigma, row);
    return boxShadowCoverageAt(row, px);
}

bool boxShadowIsSharp(float sigmaDevice) {
    return !(sigmaDevice > 0.0f) || effect_math::boxBlurWidth(sigmaDevice) <= 1;
}

bool resolveBoxShadow(const BoxShadowCmd& cmd, const Mat2x3& deviceXf, BoxShadowDraw& out) {
    const float scale = std::sqrt(std::fabs(determinant(deviceXf)));
    if (!(scale > 0.0f) || !finite(scale) || !finite(cmd.blur) || !finite(cmd.color.a) ||
        !(cmd.color.a > 0.0f)) {
        return false;
    }
    // 18 §18.3's 64-device-px cap, as the effect path applies it to blur and spread.
    float blur = std::max(0.0f, cmd.blur);
    if (blur * scale > kBoxShadowMaxDevicePx) {
        blur = kBoxShadowMaxDevicePx / scale;
    }
    float spread = cmd.spread;
    if (spread * scale > kBoxShadowMaxDevicePx) {
        spread = kBoxShadowMaxDevicePx / scale;
    }
    if (!boxShadowShape(cmd.rect, cmd.radii, spread, cmd.offset, out.shape)) {
        return false;
    }
    // T-680 ("補充 2" B.1): the outline's nested pair -- its inner edge O_in (sign -1) and the border
    // box R (sign +1) under the outer edge O_out (`rect`). The spread grows the union, so the gap
    // between R and the ring narrows from both sides: O_in shrinks by the spread while R grows by it.
    // Once the gap is closed (or O_in vanishes) the union is O_out alone and the pair is dropped.
    out.extraCount = 0;
    out.extraSign[0] = out.extraSign[1] = 0.0f;
    if (cmd.extraCount == 2 && cmd.extra[0].sign < 0.0f && cmd.extra[1].sign > 0.0f) {
        BoxShadowShape hole, box;
        if (boxShadowShape(cmd.extra[0].rect, cmd.extra[0].radii, -spread, cmd.offset, hole) &&
            boxShadowShape(cmd.extra[1].rect, cmd.extra[1].radii, spread, cmd.offset, box) &&
            shapeContains(hole, box) && shapeContains(out.shape, hole)) {
            out.extra[0] = hole;
            out.extra[1] = box;
            out.extraSign[0] = -1.0f;
            out.extraSign[1] = 1.0f;
            out.extraCount = 2;
        }
    }
    out.sigmaLocal = 0.5f * blur;
    out.sigmaDevice = out.sigmaLocal * scale;
    out.sharp = boxShadowIsSharp(out.sigmaDevice);
    const float a = std::min(cmd.color.a, 1.0f);
    out.premul = Color{cmd.color.r * a, cmd.color.g * a, cmd.color.b * a, a};
    return true;
}

float boxShadowCoverageSum(const BoxShadowDraw& d, float px, float py, float sigma) {
    const float first = boxShadowCoverage(d.shape, px, py, sigma);
    if (d.extraCount == 0) {
        return first;
    }
    float sum = first;
    for (uint32_t k = 0; k < d.extraCount && k < 2; ++k) {
        sum += d.extraSign[k] * boxShadowCoverage(d.extra[k], px, py, sigma);
    }
    return std::clamp(sum, 0.0f, 1.0f);
}

Rect boxShadowDrawRect(const BoxShadowCmd& cmd) {
    BoxShadowShape s;
    if (!boxShadowShape(cmd.rect, cmd.radii, cmd.spread, cmd.offset, s)) {
        return Rect{};
    }
    // T-680: every term's shape (grown by the spread, an upper bound for the shrunk inner edge too).
    for (uint32_t k = 0; k < cmd.extraCount && k < 2; ++k) {
        BoxShadowShape e;
        if (boxShadowShape(cmd.extra[k].rect, cmd.extra[k].radii, cmd.spread, cmd.offset, e)) {
            s.x0 = std::min(s.x0, e.x0);
            s.y0 = std::min(s.y0, e.y0);
            s.x1 = std::max(s.x1, e.x1);
            s.y1 = std::max(s.y1, e.y1);
        }
    }
    const float reach = finite(cmd.blur) ? kBoxShadowSigmaReach * 0.5f * std::max(0.0f, cmd.blur) : 0.0f;
    return Rect{s.x0 - reach, s.y0 - reach, (s.x1 - s.x0) + 2.0f * reach, (s.y1 - s.y0) + 2.0f * reach};
}

BoxShadowReject judgeBoxShadow(const BoxShadowJudgeInput& in) {
    // (1) The chain holds only outer drop shadows (each one converts on its own).
    if (in.effects.empty()) {
        return BoxShadowReject::EffectChain;
    }
    for (const EffectInstance& e : in.effects) {
        if (e.kind != EffectKind::DropShadow || e.params[5] != 0.0f || e.stopCount != 0) {
            return BoxShadowReject::EffectChain;
        }
    }
    // (2) No layer: opacity 1, normal blend, not in 3D, no ancestor opacity folded into the paints.
    if (in.opacity != 1.0f || in.blend != 0 || in.in3D || in.paintAlpha != 1.0f) {
        return BoxShadowReject::NodeLayer;
    }
    // (3) An opaque background drawn on the whole border box, circular corners (the model has one
    // radius per corner, so they always are).
    if (!in.isElement || !in.backgroundDrawn || in.clipPath ||
        !(in.backgroundAlpha == 1.0f || in.gradientOpaque) || !(in.borderBox.w > 0.0f) ||
        !(in.borderBox.h > 0.0f)) {
        return BoxShadowReject::Background;
    }
    // (4) Nothing reaches past the border box: an overflow clip, or the subtree's bounds inside it.
    if (!in.skipContent && !in.overflowClips) {
        if (!in.contentKnown) {
            return BoxShadowReject::ContentOverflow;
        }
        if (!in.contentEmpty) {
            const Rect& b = in.borderBox;
            const Rect& c = in.contentBounds;
            const float eps = kBoxShadowContentEpsilon;
            if (!(c.x >= b.x - eps) || !(c.y >= b.y - eps) || !(c.x + c.w <= b.x + b.w + eps) ||
                !(c.y + c.h <= b.y + b.h + eps)) {
                return BoxShadowReject::ContentOverflow;
            }
        }
    }
    // (5) No outline, or (T-680, "補充 2" B.1) a solid opaque one: the contour is R plus the ring.
    if (in.outline && (!in.outlineSolid || in.outlineAlpha != 1.0f)) {
        return BoxShadowReject::Outline;
    }
    // (6) Finite parameters, blur >= 0.
    for (const EffectInstance& e : in.effects) {
        for (int i = 0; i < 5; ++i) {
            if (!finite(e.params[i])) {
                return BoxShadowReject::Params;
            }
        }
        const Color& c = e.colors[0];
        if (!finite(c.r) || !finite(c.g) || !finite(c.b) || !finite(c.a) || e.params[2] < 0.0f) {
            return BoxShadowReject::Params;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (!finite(in.radii[i])) {
            return BoxShadowReject::Params;
        }
    }
    return BoxShadowReject::None;
}

}  // namespace xt
