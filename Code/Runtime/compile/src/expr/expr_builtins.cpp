#include "expr_builtins.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "expr_lexer.h"
#include "xt/expr_noise.h"

/// See expr_builtins.h. Docs/design/58-value-functions-and-drivers.md §58.2-58.3.

namespace xt::compile::expr {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// floor((t - t0) / period) without drifting for t exactly on a cycle boundary.
double cycleIndex(double d, double period) { return std::floor(d / period); }

/// The slope factor of an easing at its start (p = 0) / end (p = 1): de/dp. NaN when it has none
/// that the keys alone give (a vertical bezier tangent, steps, linear() points).
double easingSlope(const Easing& e, bool atEnd, std::span<const EasingPoint> points) {
    switch (e.kind) {
        case EasingKind::Linear:
            return 1.0;
        case EasingKind::Hold:
        case EasingKind::Steps:
            return 0.0;
        case EasingKind::CubicBezier: {
            const double x = atEnd ? 1.0 - std::clamp(static_cast<double>(e.x2), 0.0, 1.0)
                                   : std::clamp(static_cast<double>(e.x1), 0.0, 1.0);
            const double y = atEnd ? 1.0 - static_cast<double>(e.y2) : static_cast<double>(e.y1);
            if (x > 1e-9) {
                return y / x;
            }
            return y == 0.0 ? 0.0 : std::numeric_limits<double>::quiet_NaN();
        }
        case EasingKind::LinearPoints: {
            const size_t end = size_t{e.pointOffset} + e.pointCount;
            if (e.pointCount < 2 || end > points.size()) {
                return 1.0;
            }
            const EasingPoint& a = atEnd ? points[end - 2] : points[e.pointOffset];
            const EasingPoint& b = atEnd ? points[end - 1] : points[e.pointOffset + 1u];
            if (b.in == a.in) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return static_cast<double>(b.out - a.out) / static_cast<double>(b.in - a.in);
        }
    }
    return 1.0;
}

}  // namespace

bool parseLoopType(const std::string& s, LoopType& out) {
    if (s == "cycle") {
        out = LoopType::Cycle;
    } else if (s == "pingpong") {
        out = LoopType::PingPong;
    } else if (s == "offset") {
        out = LoopType::Offset;
    } else if (s == "continue") {
        out = LoopType::Continue;
    } else {
        return false;
    }
    return true;
}

bool RowSampler::init(const ExprRow& row, std::vector<Diagnostic>& diags) {
    if (row.keys.empty()) {
        addExprDiagnostic(diags, "expr.row", "the row has no keyframes", 1, 1);
        return false;
    }
    m_keys.assign(row.keys.begin(), row.keys.end());
    m_points.assign(row.easingPoints.begin(), row.easingPoints.end());
    m_row.keys = m_keys;
    m_row.easingPoints = m_points;
    m_timeline = Timeline();
    m_timeline.keyframes.reserve(m_keys.size());
    m_constant = true;
    for (size_t i = 0; i < m_keys.size(); ++i) {
        const ExprKey& k = m_keys[i];
        const float tf = static_cast<float>(k.time);
        const float vf = static_cast<float>(k.value);
        if (!std::isfinite(k.time) || !std::isfinite(k.value) || !std::isfinite(tf) || !std::isfinite(vf)) {
            addExprDiagnostic(diags, "expr.row", "the row has a keyframe that is not a finite number", 1, 1);
            return false;
        }
        if (i > 0 && !(tf > m_timeline.keyframes.back().time)) {
            addExprDiagnostic(diags, "expr.row", "the row's keyframe times are not strictly increasing", 1,
                              1);
            return false;
        }
        const Easing& e = k.easing;
        if (e.kind == EasingKind::CubicBezier &&
            !(std::isfinite(e.x1) && std::isfinite(e.y1) && std::isfinite(e.x2) && std::isfinite(e.y2))) {
            addExprDiagnostic(diags, "expr.row", "the row has a cubic-bezier easing that is not finite", 1,
                              1);
            return false;
        }
        if (e.kind == EasingKind::LinearPoints && size_t{e.pointOffset} + e.pointCount > m_points.size()) {
            addExprDiagnostic(diags, "expr.row", "the row has a linear() easing outside its point pool", 1,
                              1);
            return false;
        }
        if (k.value != m_keys[0].value) {
            m_constant = false;
        }
        Keyframe kf;
        kf.time = tf;
        kf.value = Value::ofFloat(vf);
        kf.easing = e;
        m_timeline.keyframes.push_back(kf);
    }
    Track track;
    track.type = ValueType::Float;
    track.kfOffset = 0;
    track.kfCount = static_cast<uint32_t>(m_timeline.keyframes.size());
    m_timeline.tracks.push_back(track);
    return true;
}

double RowSampler::sample(double t) const {
    const Value v = evalTrack(m_timeline, m_timeline.tracks[0], static_cast<float>(t), nullptr, nullptr,
                              nullptr, m_points);
    return static_cast<double>(v.f);
}

size_t RowSampler::loopOutStart(int numKeyframes) const {
    const size_t last = m_keys.size() - 1;
    if (numKeyframes <= 0 || static_cast<size_t>(numKeyframes) >= last) {
        return 0;
    }
    return last - static_cast<size_t>(numKeyframes);
}

size_t RowSampler::loopInEnd(int numKeyframes) const {
    const size_t last = m_keys.size() - 1;
    if (numKeyframes <= 0 || static_cast<size_t>(numKeyframes) >= last) {
        return last;
    }
    return static_cast<size_t>(numKeyframes);
}

double RowSampler::slopeInto(size_t i) const {
    const ExprKey& a = m_keys[i - 1];
    const ExprKey& b = m_keys[i];
    const double dt = b.time - a.time;
    double f = easingSlope(a.easing, true, m_points);
    if (std::isnan(f)) {
        // No analytic end slope: the chord of the segment's last 1 %.
        const double h = dt * 0.01;
        return (sample(b.time) - sample(b.time - h)) / h;
    }
    return f * (b.value - a.value) / dt;
}

double RowSampler::slopeOutOf(size_t i) const {
    const ExprKey& a = m_keys[i];
    const ExprKey& b = m_keys[i + 1];
    const double dt = b.time - a.time;
    double f = easingSlope(a.easing, false, m_points);
    if (std::isnan(f)) {
        const double h = dt * 0.01;
        return (sample(a.time + h) - sample(a.time)) / h;
    }
    return f * (b.value - a.value) / dt;
}

// AE loopOut: Docs/design/58-value-functions-and-drivers.md §58.3. The range runs from key `s` to the
// last key; past the last key cycle repeats it, pingpong plays it back and forth, offset repeats it
// adding (last - first) per cycle, continue extends with the slope arriving at the last key.
double RowSampler::loopOut(LoopType type, int numKeyframes, double t) const {
    const size_t last = m_keys.size() - 1;
    if (last == 0) {
        return sample(t);
    }
    const size_t s = loopOutStart(numKeyframes);
    const double t0 = m_keys[s].time;
    const double t1 = m_keys[last].time;
    const double period = t1 - t0;
    if (!(t > t1) || !(period > 0)) {
        return sample(t);
    }
    if (type == LoopType::Continue) {
        return m_keys[last].value + slopeInto(last) * (t - t1);
    }
    const double d = t - t0;
    const double k = cycleIndex(d, period);
    const double u = std::clamp(d - k * period, 0.0, period);
    switch (type) {
        case LoopType::Cycle:
            return sample(t0 + u);
        case LoopType::PingPong:
            return std::fmod(k, 2.0) != 0.0 ? sample(t1 - u) : sample(t0 + u);
        case LoopType::Offset:
            return sample(t0 + u) + k * (m_keys[last].value - m_keys[s].value);
        case LoopType::Continue:
            break;
    }
    return sample(t);
}

double RowSampler::loopIn(LoopType type, int numKeyframes, double t) const {
    const size_t last = m_keys.size() - 1;
    if (last == 0) {
        return sample(t);
    }
    const size_t e = loopInEnd(numKeyframes);
    const double t0 = m_keys[0].time;
    const double t1 = m_keys[e].time;
    const double period = t1 - t0;
    if (!(t < t0) || !(period > 0)) {
        return sample(t);
    }
    if (type == LoopType::Continue) {
        return m_keys[0].value - slopeOutOf(0) * (t0 - t);
    }
    const double d = t0 - t;
    switch (type) {
        case LoopType::Cycle: {
            const double k = std::ceil(d / period);
            return sample(std::clamp(t + k * period, t0, t1));
        }
        case LoopType::PingPong: {
            const double k = cycleIndex(d, period);
            const double u = std::clamp(d - k * period, 0.0, period);
            return std::fmod(k, 2.0) == 0.0 ? sample(t0 + u) : sample(t1 - u);
        }
        case LoopType::Offset: {
            const double k = std::ceil(d / period);
            return sample(std::clamp(t + k * period, t0, t1)) - k * (m_keys[e].value - m_keys[0].value);
        }
        case LoopType::Continue:
            break;
    }
    return sample(t);
}

double aeLinear(double t, double tMin, double tMax, double v1, double v2) {
    if (tMax < tMin) {
        std::swap(tMin, tMax);
    }
    if (t <= tMin) {
        return v1;
    }
    if (t >= tMax) {
        return v2;
    }
    const double p = (t - tMin) / (tMax - tMin);
    return v1 + (v2 - v1) * p;
}

double aeEaseProgress(Builtin which, double p) {
    const float* c =
        which == Builtin::EaseIn ? kExprEaseIn : (which == Builtin::EaseOut ? kExprEaseOut : kExprEase);
    const Easing e = Easing::bezier(c[0], c[1], c[2], c[3]);
    return static_cast<double>(evalEasing(e, static_cast<float>(p)));
}

double aeEase(Builtin which, double t, double tMin, double tMax, double v1, double v2) {
    if (tMax < tMin) {
        std::swap(tMin, tMax);
    }
    if (t <= tMin) {
        return v1;
    }
    if (t >= tMax) {
        return v2;
    }
    const double p = (t - tMin) / (tMax - tMin);
    return v1 + (v2 - v1) * aeEaseProgress(which, p);
}

double math1(Builtin b, double x) {
    switch (b) {
        case Builtin::MathSin:
            return std::sin(x);
        case Builtin::MathCos:
            return std::cos(x);
        case Builtin::MathTan:
            return std::tan(x);
        case Builtin::MathAsin:
            return std::asin(x);
        case Builtin::MathAcos:
            return std::acos(x);
        case Builtin::MathAtan:
            return std::atan(x);
        case Builtin::MathAbs:
            return std::fabs(x);
        case Builtin::MathRound:
            return std::floor(x + 0.5);  // JS Math.round: halves go up
        case Builtin::MathFloor:
            return std::floor(x);
        case Builtin::MathCeil:
            return std::ceil(x);
        case Builtin::MathSqrt:
            return std::sqrt(x);
        case Builtin::MathExp:
            return std::exp(x);
        case Builtin::MathLog:
            return std::log(x);
        case Builtin::DegreesToRadians:
            return x * (kPi / 180.0);
        case Builtin::RadiansToDegrees:
            return x * (180.0 / kPi);
        default:
            break;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double math2(Builtin b, double x, double y) {
    if (b == Builtin::MathAtan2) {
        return std::atan2(x, y);
    }
    if (b == Builtin::MathPow) {
        return std::pow(x, y);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void builtinArity(Builtin b, int& minArgs, int& maxArgs) {
    minArgs = 1;
    maxArgs = 1;
    switch (b) {
        case Builtin::Wiggle:
            minArgs = 2;
            maxArgs = 5;
            break;
        case Builtin::LoopOut:
        case Builtin::LoopIn:
            minArgs = 0;
            maxArgs = 2;
            break;
        case Builtin::Linear:
        case Builtin::Ease:
        case Builtin::EaseIn:
        case Builtin::EaseOut:
            minArgs = 3;
            maxArgs = 5;
            break;
        case Builtin::Clamp:
            minArgs = 3;
            maxArgs = 3;
            break;
        case Builtin::Random:
            minArgs = 0;
            maxArgs = 2;
            break;
        case Builtin::SeedRandom:
        case Builtin::Length:
            minArgs = 1;
            maxArgs = 2;
            break;
        case Builtin::MathAtan2:
        case Builtin::MathPow:
            minArgs = 2;
            maxArgs = 2;
            break;
        case Builtin::MathMin:
        case Builtin::MathMax:
            minArgs = 1;
            maxArgs = 16;
            break;
        case Builtin::RandomPerPlay:
            minArgs = 0;
            maxArgs = 2;
            break;
        default:
            break;
    }
}

const char* builtinName(Builtin b) {
    switch (b) {
        case Builtin::Wiggle:
            return "wiggle";
        case Builtin::LoopOut:
            return "loopOut";
        case Builtin::LoopIn:
            return "loopIn";
        case Builtin::ValueAtTime:
            return "valueAtTime";
        case Builtin::Linear:
            return "linear";
        case Builtin::Ease:
            return "ease";
        case Builtin::EaseIn:
            return "easeIn";
        case Builtin::EaseOut:
            return "easeOut";
        case Builtin::Clamp:
            return "clamp";
        case Builtin::Random:
            return "random";
        case Builtin::SeedRandom:
            return "seedRandom";
        case Builtin::Noise:
            return "noise";
        case Builtin::PosterizeTime:
            return "posterizeTime";
        case Builtin::DegreesToRadians:
            return "degreesToRadians";
        case Builtin::RadiansToDegrees:
            return "radiansToDegrees";
        case Builtin::Length:
            return "length";
        case Builtin::MathSin:
            return "Math.sin";
        case Builtin::MathCos:
            return "Math.cos";
        case Builtin::MathTan:
            return "Math.tan";
        case Builtin::MathAsin:
            return "Math.asin";
        case Builtin::MathAcos:
            return "Math.acos";
        case Builtin::MathAtan:
            return "Math.atan";
        case Builtin::MathAtan2:
            return "Math.atan2";
        case Builtin::MathAbs:
            return "Math.abs";
        case Builtin::MathRound:
            return "Math.round";
        case Builtin::MathFloor:
            return "Math.floor";
        case Builtin::MathCeil:
            return "Math.ceil";
        case Builtin::MathMin:
            return "Math.min";
        case Builtin::MathMax:
            return "Math.max";
        case Builtin::MathPow:
            return "Math.pow";
        case Builtin::MathSqrt:
            return "Math.sqrt";
        case Builtin::MathExp:
            return "Math.exp";
        case Builtin::MathLog:
            return "Math.log";
        case Builtin::Port:
            return "port";
        case Builtin::RandomPerPlay:
            return "randomPerPlay";
        case Builtin::None:
            break;
    }
    return "?";
}

std::vector<char> simplifySamples(const std::vector<double>& ts, const std::vector<double>& vs, double tol) {
    std::vector<char> keep(ts.size(), 0);
    keep.front() = 1;
    keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack;
    if (ts.size() > 2) {
        stack.push_back({0, ts.size() - 1});
    }
    while (!stack.empty()) {
        const auto [i, j] = stack.back();
        stack.pop_back();
        double worst = -1;
        size_t at = i;
        for (size_t k = i + 1; k < j; ++k) {
            const double p = (ts[k] - ts[i]) / (ts[j] - ts[i]);
            const double err = std::fabs(vs[k] - (vs[i] + (vs[j] - vs[i]) * p));
            if (err > worst) {
                worst = err;
                at = k;
            }
        }
        if (worst > tol) {
            keep[at] = 1;
            if (at - i > 1) {
                stack.push_back({i, at});
            }
            if (j - at > 1) {
                stack.push_back({at, j});
            }
        }
    }
    return keep;
}

}  // namespace xt::compile::expr
