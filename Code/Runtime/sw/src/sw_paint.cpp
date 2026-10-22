#include "sw_paint.h"

#include <algorithm>
#include <cmath>
#include <limits>

/// See Docs/tasks/T-12.md "漸層（sw_paint）", Docs/tasks/T-128.md step 2.

namespace xt::sw {

namespace {

/// Linearly interpolates a color between two stops at fraction `f` in [0, 1] (non-premultiplied).
Color lerpColor(const Color& a, const Color& b, float f) {
    return Color{
        a.r + (b.r - a.r) * f,
        a.g + (b.g - a.g) * f,
        a.b + (b.b - a.b) * f,
        a.a + (b.a - a.a) * f,
    };
}

/// Samples `stops` at `t` (already clamped to [0, 1]): before the first stop the first color is
/// used, after the last stop the last color is used, otherwise linear interpolation between the
/// bracketing stops. An empty stop list returns a fully transparent black (a degenerate input not
/// covered by Docs/tasks/T-12.md; chosen as the most conservative fallback).
Color sampleStops(std::span<const GradientStop> stops, float t) {
    if (stops.empty()) {
        return Color{0, 0, 0, 0};
    }
    if (stops.size() == 1 || t <= stops.front().offset) {
        return stops.front().color;
    }
    if (t >= stops.back().offset) {
        return stops.back().color;
    }
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const GradientStop& a = stops[i];
        const GradientStop& b = stops[i + 1];
        if (t >= a.offset && t <= b.offset) {
            float span = b.offset - a.offset;
            float f = span > 0.0f ? (t - a.offset) / span : 0.0f;
            return lerpColor(a.color, b.color, f);
        }
    }
    return stops.back().color;
}

/// True when two stop lists are the same gradient, bit for bit. A bitwise compare is what "same
/// ramp" has to mean for the cache: a ramp built from even slightly different stops is a different
/// ramp.
bool sameStops(std::span<const GradientStop> a, const std::vector<GradientStop>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].offset != b[i].offset || a[i].color.r != b[i].color.r || a[i].color.g != b[i].color.g ||
            a[i].color.b != b[i].color.b || a[i].color.a != b[i].color.a) {
            return false;
        }
    }
    return true;
}

}  // namespace

const float* GradientLutCache::get(std::span<const GradientStop> stops, float alpha) {
    for (size_t i = 0; i < m_live; ++i) {
        const Entry& e = m_entries[i];
        if (e.alpha == alpha && sameStops(stops, e.stops)) {
            return e.lut.data();
        }
    }
    if (m_live >= kMaxEntries) {
        m_live = 0;  // See kMaxEntries: drop the whole cache rather than grow the scan.
    }
    if (m_live == m_entries.size()) {
        m_entries.emplace_back();
    }
    Entry& e = m_entries[m_live];
    ++m_live;
    e.alpha = alpha;
    // assign()/resize() on vectors that already have capacity from an earlier frame allocate
    // nothing once warm (Docs/design/01-architecture.md §1.7).
    e.stops.assign(stops.begin(), stops.end());
    e.lut.resize(static_cast<size_t>(kGradientLutSize) * 4);
    for (int i = 0; i < kGradientLutSize; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kGradientLutSize - 1);
        const Color c = sampleStops(stops, t);
        const float a = c.a * alpha;
        float* p = e.lut.data() + static_cast<size_t>(i) * 4;
        p[0] = c.r * a;
        p[1] = c.g * a;
        p[2] = c.b * a;
        p[3] = a;
    }
    return e.lut.data();
}

/// T-201: a conic gradient's `t` at offset `v` from the centre: the clockwise angle (y grows
/// downwards) from the start ray `d` to `v`, over a full turn, in [0, 1).
float PaintEval::conicT(Vec2 v, Vec2 d) {
    const float cross = d.x * v.y - d.y * v.x;
    const float dot = d.x * v.x + d.y * v.y;
    float t = std::atan2(cross, dot) * (0.5f / 3.14159265358979323846f);
    if (t < 0.0f) {
        t += 1.0f;
    }
    return t >= 1.0f ? 0.0f : t;
}

PaintEval PaintEval::make(const PaintRef& paint, std::span<const GradientStop> stops, const Mat2x3& xfDevice,
                          GradientLutCache& luts, const Mat3* screenToPlane) {
    PaintEval eval;
    eval.m_kind = paint.kind;
    eval.m_solid = paint.color;
    eval.m_p0 = paint.p0;
    eval.m_p1 = paint.p1;
    eval.m_p2 = paint.p2;
    if (paint.kind != PaintKind::Solid) {
        eval.m_lut = luts.get(stops, paint.color.a);
        const Vec2 d = paint.p1 - paint.p0;
        eval.m_degenerate = (d.x * d.x + d.y * d.y) <= 0.0f;
        if (!invert(xfDevice, eval.m_inverse)) {
            // A degenerate (non-invertible) device transform is a known limitation, not covered by
            // Docs/tasks/T-12.md; fall back to identity rather than propagating NaNs.
            eval.m_inverse = Mat2x3{};
        }
        if (screenToPlane != nullptr) {
            eval.m_projective = true;
            eval.m_screenToPlane = *screenToPlane;
        }
    }
    return eval;
}

float PaintEval::tAt(Vec2 devicePoint) const {
    if (m_projective) {
        devicePoint = mat3Apply(m_screenToPlane, devicePoint);
    }
    const Vec2 p = apply(m_inverse, devicePoint);
    const Vec2 d = m_p1 - m_p0;
    const float lenSq = d.x * d.x + d.y * d.y;
    const Vec2 v = p - m_p0;
    float t = 0.0f;
    if (m_kind == PaintKind::Conic) {
        return conicT(v, d);  // T-201: a full turn, no clamping.
    }
    if (m_kind == PaintKind::Radial) {
        if (m_p2.x == m_p0.x && m_p2.y == m_p0.y) {
            // Pre-T-116 circle: p2 wasn't set (or ry == 0, which collapses p2 onto p0 too --
            // no real producer emits a degenerate 0-height ellipse, see PaintRef::p2).
            t = std::sqrt(v.x * v.x + v.y * v.y) / std::sqrt(lenSq);
        } else {
            // T-116: axis-aligned ellipse. rx == sqrt(lenSq) (|p1 - p0|); ry == |p2 - p0|.
            const Vec2 dy = m_p2 - m_p0;
            const float ry = std::sqrt(dy.x * dy.x + dy.y * dy.y);
            const float nx = v.x / std::sqrt(lenSq);
            const float ny = v.y / ry;
            t = std::sqrt(nx * nx + ny * ny);
        }
    } else {
        // Linear (and any other/unknown gradient kind, conservatively treated as linear).
        t = (v.x * d.x + v.y * d.y) / lenSq;
    }
    return std::clamp(t, 0.0f, 1.0f);
}

Color PaintEval::at(float px, float py) const {
    if (m_kind == PaintKind::Solid) {
        return m_solid;
    }

    float t = 1.0f;  // The degenerate case paints the ramp's last entry everywhere.
    if (!m_degenerate) {
        t = tAt(Vec2{px + 0.5f, py + 0.5f});
    }

    // The ramp is premultiplied; `at`'s contract is a non-premultiplied colour.
    const float* c = sample(t);
    if (c[3] <= 0.0f) {
        return Color{0, 0, 0, 0};
    }
    return Color{c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]};
}

bool PaintEval::visibleRange(float a, float b, float c, int n, int& i0, int& i1) {
    i0 = 0;
    i1 = n;
    if (a <= 0.0f) {
        // No quadratic term: the span does not move across the gradient's extent (a rotated
        // gradient sampled along a degenerate direction). Nothing to narrow.
        return true;
    }
    const float disc = b * b - 4.0f * a * c;
    if (disc <= 0.0f) {
        return false;  // Never inside the extent: the whole span clamps to a transparent end.
    }
    const float root = std::sqrt(disc);
    const float lo = (-b - root) / (2.0f * a);
    const float hi = (-b + root) / (2.0f * a);
    // One pixel of slack on each side: the loop re-derives `t` exactly as before, so a boundary
    // pixel that rounding would have excluded here is simply evaluated and found invisible.
    const float loSlack = std::floor(lo) - 1.0f;
    const float hiSlack = std::ceil(hi) + 1.0f;
    if (loSlack > 0.0f) {
        i0 = (loSlack >= static_cast<float>(n)) ? n : static_cast<int>(loSlack);
    }
    if (hiSlack < static_cast<float>(n)) {
        i1 = (hiSlack <= 0.0f) ? 0 : static_cast<int>(hiSlack);
    }
    return i0 < i1;
}

PaintEval::SpanInfo PaintEval::spanPremul(int x, int y, int n, float alphaScale, float* dst) const {
    SpanInfo info;
    info.first = n;
    info.last = 0;
    info.allOpaque = true;

    // The ramp is already premultiplied by the paint's alpha, so one source pixel is just the ramp
    // entry scaled by the span's coverage -- four multiplies, no stop search. At full coverage the
    // scale is exactly 1.0f, so the multiply is the identity rather than a second rounding.
    auto write = [&](int i, const float* c) {
        float* p = dst + static_cast<size_t>(i) * 4;
        const float a = c[3] * alphaScale;
        p[0] = c[0] * alphaScale;
        p[1] = c[1] * alphaScale;
        p[2] = c[2] * alphaScale;
        p[3] = a;
        if (a > 0.0f) {
            if (i < info.first) {
                info.first = i;
            }
            info.last = i + 1;
        }
        // Deliberately outside the `a > 0` test: a non-opaque pixel anywhere in the written range
        // disables the opaque store, which has to cover a contiguous range including it.
        if (a < 1.0f) {
            info.allOpaque = false;
        }
    };

    if (m_degenerate) {
        const float* c = sample(1.0f);
        for (int i = 0; i < n; ++i) {
            write(i, c);
        }
        return info;
    }

    if (m_projective) {
        // T-160: under a projection t is not affine along the span (the homography divides by a
        // per-pixel w), so every pixel is mapped on its own. Only projected gradients get here.
        for (int i = 0; i < n; ++i) {
            write(i, sample(tAt(Vec2{static_cast<float>(x + i) + 0.5f, static_cast<float>(y) + 0.5f})));
        }
        return info;
    }

    // Device pixel centers walk by one unit of x, so the inverse transform's first column is the
    // step of the gradient-space point between neighbouring pixels: everything below is one
    // multiply-add per pixel instead of a full apply() plus a stop search.
    const Vec2 p0 = apply(m_inverse, Vec2{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
    const Vec2 step{m_inverse.a, m_inverse.b};
    const Vec2 d = m_p1 - m_p0;
    const float lenSq = d.x * d.x + d.y * d.y;
    const Vec2 v0 = p0 - m_p0;
    // A ramp end that is fully transparent makes every pixel clamped to that end invisible, so the
    // loop can be narrowed to the part of the span that actually falls inside the gradient's own
    // extent -- for this board's two full-viewport radial washes that is most of the span
    // (Docs/tasks/T-128.md step 2).
    const bool endClear = m_lut[static_cast<size_t>(kGradientLutSize - 1) * 4 + 3] <= 0.0f;
    const bool startClear = m_lut[3] <= 0.0f;
    int i0 = 0;
    int i1 = n;

    if (m_kind == PaintKind::Conic) {
        // T-201: the angle is not affine along the span; one atan2 per pixel.
        for (int i = 0; i < n; ++i) {
            const float fi = static_cast<float>(i);
            write(i, sample(conicT(Vec2{v0.x + step.x * fi, v0.y + step.y * fi}, d)));
        }
        return info;
    }

    if (m_kind == PaintKind::Radial) {
        const float rx = std::sqrt(lenSq);
        if (m_p2.x == m_p0.x && m_p2.y == m_p0.y) {
            // Circle: same expression as `at`, with v stepped instead of recomputed. Inside the
            // extent is |v|^2 <= rx^2.
            if (endClear &&
                !visibleRange(step.x * step.x + step.y * step.y, 2.0f * (v0.x * step.x + v0.y * step.y),
                              v0.x * v0.x + v0.y * v0.y - lenSq, n, i0, i1)) {
                return info;
            }
            for (int i = i0; i < i1; ++i) {
                const float fi = static_cast<float>(i);
                const float vx = v0.x + step.x * fi;
                const float vy = v0.y + step.y * fi;
                const float t = std::clamp(std::sqrt(vx * vx + vy * vy) / rx, 0.0f, 1.0f);
                write(i, sample(t));
            }
            return info;
        }
        const Vec2 dy = m_p2 - m_p0;
        const float ry = std::sqrt(dy.x * dy.x + dy.y * dy.y);
        const float nx0 = v0.x / rx;
        const float ny0 = v0.y / ry;
        const float dnx = step.x / rx;
        const float dny = step.y / ry;
        if (endClear && !visibleRange(dnx * dnx + dny * dny, 2.0f * (nx0 * dnx + ny0 * dny),
                                      nx0 * nx0 + ny0 * ny0 - 1.0f, n, i0, i1)) {
            return info;
        }
        for (int i = i0; i < i1; ++i) {
            const float fi = static_cast<float>(i);
            const float nx = nx0 + dnx * fi;
            const float ny = ny0 + dny * fi;
            const float t = std::clamp(std::sqrt(nx * nx + ny * ny), 0.0f, 1.0f);
            write(i, sample(t));
        }
        return info;
    }

    // Linear: t is affine in x, so the whole inner loop is one multiply-add and a ramp copy. When
    // a ramp end is transparent, the half-line clamped to it is skipped the same way.
    const float t0 = (v0.x * d.x + v0.y * d.y) / lenSq;
    const float dt = (step.x * d.x + step.y * d.y) / lenSq;
    if (dt != 0.0f && (startClear || endClear)) {
        // Visible `t` window: a transparent ramp end makes everything clamped to it invisible, an
        // opaque one leaves that side open. Dividing an infinite bound by `dt` (never 0 here)
        // carries the sign correctly, so no separate case is needed for the open sides.
        const float inf = std::numeric_limits<float>::infinity();
        const float tMin = startClear ? 0.0f : -inf;
        const float tMax = endClear ? 1.0f : inf;
        const float lo = ((dt > 0.0f ? tMin : tMax) - t0) / dt;
        const float hi = ((dt > 0.0f ? tMax : tMin) - t0) / dt;
        const float loSlack = std::floor(lo) - 1.0f;
        const float hiSlack = std::ceil(hi) + 1.0f;
        if (loSlack > 0.0f) {
            i0 = (loSlack >= static_cast<float>(n)) ? n : static_cast<int>(loSlack);
        }
        if (hiSlack < static_cast<float>(n)) {
            i1 = (hiSlack <= 0.0f) ? 0 : static_cast<int>(hiSlack);
        }
        if (i0 >= i1) {
            return info;
        }
    }
    for (int i = i0; i < i1; ++i) {
        const float t = std::clamp(t0 + dt * static_cast<float>(i), 0.0f, 1.0f);
        write(i, sample(t));
    }
    return info;
}

}  // namespace xt::sw
