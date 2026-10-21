#include <algorithm>
#include <cmath>
#include <limits>

#include "xt/timeline.h"

/// Easing construction and evaluation. See docs/design/04-timeline.md §4.1.

namespace xt {

namespace {

/// Cubic Bezier with control points P0=0, P1=c1, P2=c2, P3=1, evaluated at t.
/// B(t) = 3(1-t)^2 t c1 + 3(1-t) t^2 c2 + t^3 (the (1-t)^3 * P0 term drops out since P0 = 0).
float bezierComponent(float t, float c1, float c2) {
    float mt = 1.0f - t;
    return 3.0f * mt * mt * t * c1 + 3.0f * mt * t * t * c2 + t * t * t;
}

/// Derivative of bezierComponent with respect to t.
/// B'(t) = 3(1-t)^2 c1 + 6(1-t)t (c2-c1) + 3t^2 (1-c2)
float bezierComponentDerivative(float t, float c1, float c2) {
    float mt = 1.0f - t;
    return 3.0f * mt * mt * c1 + 6.0f * mt * t * (c2 - c1) + 3.0f * t * t * (1.0f - c2);
}

/// Solves bezierComponent(t, x1, x2) == p for t, per docs/design/04-timeline.md §4.1 (identical to
/// WebKit's UnitBezier): 8 Newton iterations from t = p, falling back to 32 bisection iterations if
/// the derivative is too small or a Newton step leaves [0,1].
float solveX(float p, float x1, float x2) {
    float t = p;
    for (int i = 0; i < 8; ++i) {
        float x = bezierComponent(t, x1, x2) - p;
        if (std::fabs(x) < 1e-6f) {
            return t;
        }
        float d = bezierComponentDerivative(t, x1, x2);
        if (std::fabs(d) < 1e-6f) {
            break;
        }
        t -= x / d;
        if (t < 0.0f || t > 1.0f) {
            break;
        }
    }
    float lo = 0.0f, hi = 1.0f;
    t = p;
    for (int i = 0; i < 32; ++i) {
        float x = bezierComponent(t, x1, x2);
        if (std::fabs(x - p) < 1e-6f) {
            break;
        }
        if (x < p) {
            lo = t;
        } else {
            hi = t;
        }
        t = (lo + hi) / 2.0f;
    }
    return t;
}

}  // namespace

Easing Easing::linear() {
    Easing e;
    e.kind = EasingKind::Linear;
    return e;
}

Easing Easing::hold() {
    Easing e;
    e.kind = EasingKind::Hold;
    return e;
}

Easing Easing::bezier(float x1, float y1, float x2, float y2) {
    Easing e;
    e.kind = EasingKind::CubicBezier;
    e.x1 = x1;
    e.y1 = y1;
    e.x2 = x2;
    e.y2 = y2;
    return e;
}

Easing Easing::steps(uint16_t n, bool jumpEnd) {
    Easing e;
    e.kind = EasingKind::Steps;
    e.stepsJumpEnd = jumpEnd ? 1 : 0;
    e.stepCount = n;
    return e;
}

Easing Easing::cssEase() { return bezier(0.25f, 0.1f, 0.25f, 1.0f); }
Easing Easing::cssEaseIn() { return bezier(0.42f, 0.0f, 1.0f, 1.0f); }
Easing Easing::cssEaseOut() { return bezier(0.0f, 0.0f, 0.58f, 1.0f); }
Easing Easing::cssEaseInOut() { return bezier(0.42f, 0.0f, 0.58f, 1.0f); }

bool linearEasingPoints(std::span<const LinearStop> stops, std::vector<EasingPoint>& out) {
    if (stops.size() < 2) {
        return false;
    }
    // One point per input (a stop with two inputs is two points); NaN = no input yet.
    const float kNone = std::numeric_limits<float>::quiet_NaN();
    std::vector<EasingPoint> pts;
    pts.reserve(stops.size() * 2);
    for (const LinearStop& s : stops) {
        if (!std::isfinite(s.out) || s.inputCount > 2) {
            return false;
        }
        if (s.inputCount == 0) {
            pts.push_back(EasingPoint{kNone, s.out});
        }
        for (uint8_t i = 0; i < s.inputCount; ++i) {
            if (!std::isfinite(s.in[i])) {
                return false;
            }
            pts.push_back(EasingPoint{s.in[i], s.out});
        }
    }
    // The first / last point without an input is at 0 / 1.
    if (std::isnan(pts.front().in)) {
        pts.front().in = 0.0f;
    }
    if (std::isnan(pts.back().in)) {
        pts.back().in = 1.0f;
    }
    // An input below the largest earlier one is raised to it.
    float largest = pts.front().in;
    for (EasingPoint& pt : pts) {
        if (!std::isnan(pt.in)) {
            pt.in = std::max(pt.in, largest);
            largest = pt.in;
        }
    }
    // Each run without inputs is spread evenly between its neighbours.
    for (size_t i = 1; i < pts.size(); ++i) {
        if (!std::isnan(pts[i].in)) {
            continue;
        }
        size_t end = i;
        while (std::isnan(pts[end].in)) {
            ++end;  // The last point has an input, so this stops.
        }
        const float a = pts[i - 1].in;
        const float b = pts[end].in;
        const float n = static_cast<float>(end - i + 1);
        for (size_t k = i; k < end; ++k) {
            pts[k].in = a + (b - a) * static_cast<float>(k - i + 1) / n;
        }
        i = end;
    }
    out.insert(out.end(), pts.begin(), pts.end());
    return true;
}

namespace {

/// T-335: CSS Easing 1's linear easing function at `x` over canonical `pts` (at least 2 points).
float evalLinearPoints(std::span<const EasingPoint> pts, float x) {
    // The last point whose input is <= x (the first when none), never the last one.
    auto it =
        std::upper_bound(pts.begin(), pts.end(), x, [](float v, const EasingPoint& pt) { return v < pt.in; });
    size_t a = it == pts.begin() ? 0 : static_cast<size_t>(it - pts.begin()) - 1;
    if (a >= pts.size() - 1) {
        a = pts.size() - 2;
    }
    const EasingPoint& pa = pts[a];
    const EasingPoint& pb = pts[a + 1];
    if (pa.in == pb.in) {
        return pb.out;
    }
    return pa.out + (x - pa.in) / (pb.in - pa.in) * (pb.out - pa.out);
}

}  // namespace

float evalEasing(const Easing& easing, float p, std::span<const EasingPoint> points) {
    p = std::clamp(p, 0.0f, 1.0f);
    switch (easing.kind) {
        case EasingKind::LinearPoints: {
            const size_t end = size_t{easing.pointOffset} + easing.pointCount;
            if (easing.pointCount < 2 || end > points.size()) {
                return p;  // No pool (or a range outside it): plain linear.
            }
            return evalLinearPoints(points.subspan(easing.pointOffset, easing.pointCount), p);
        }
        case EasingKind::Linear:
            return p;
        case EasingKind::Hold:
            return 0;
        case EasingKind::CubicBezier: {
            float x1 = std::clamp(easing.x1, 0.0f, 1.0f);
            float x2 = std::clamp(easing.x2, 0.0f, 1.0f);
            float t = solveX(p, x1, x2);
            return bezierComponent(t, easing.y1, easing.y2);
        }
        case EasingKind::Steps: {
            // stepCount == 0 is not covered by the spec; treated conservatively as 1 step to avoid
            // a division by zero.
            uint16_t n = easing.stepCount == 0 ? uint16_t{1} : easing.stepCount;
            float nf = static_cast<float>(n);
            float e = easing.stepsJumpEnd ? std::floor(p * nf) / nf : std::ceil(p * nf) / nf;
            return std::clamp(e, 0.0f, 1.0f);
        }
    }
    return p;
}

}  // namespace xt
