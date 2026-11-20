#pragma once

#include <cstdint>
#include <vector>

#include "expr.h"
#include "xt/timeline.h"

/// Pieces the evaluator, the lowering and splitRuntime share: the row sampler (the runtime's own
/// track interpolation), AE's loopOut / loopIn, AE's linear / ease mappings and the scalar math.
/// Internal to Code/Runtime/compile/src/expr. Docs/design/58-value-functions-and-drivers.md §58.2-58.3.

namespace xt::compile::expr {

/// T-777 (ruling on `noise()`): the seed of `noise(x)` -- a fixed number, so noise(x) is a function of
/// x alone on every row and on both paths, as in After Effects (wiggle / random stay per row).
inline constexpr uint32_t kExprNoiseSeed = 0x4E4F4953u;

/// loopOut / loopIn types (AE).
enum class LoopType : uint8_t { Cycle = 0, PingPong, Offset, Continue };

/// Parses "cycle" / "pingpong" / "offset" / "continue"; false for anything else.
bool parseLoopType(const std::string& s, LoopType& out);

/// Samples one row exactly as the runtime does: the keys are copied into a one-track xt::Timeline and
/// read with xt::evalTrack (04 §4.3), so a value the compiler reads is the value the runtime would
/// play, bit for bit (single precision).
class RowSampler {
public:
    RowSampler() = default;
    RowSampler(const RowSampler&) = delete;
    RowSampler& operator=(const RowSampler&) = delete;
    RowSampler(RowSampler&&) = default;
    RowSampler& operator=(RowSampler&&) = default;

    /// Copies and checks `row` (at least one key, finite, strictly increasing times also in single
    /// precision, LinearPoints ranges inside the pool). False after an `expr.row` diagnostic.
    bool init(const ExprRow& row, std::vector<Diagnostic>& diags);

    /// The row's value at `t` (xt::evalTrack).
    double sample(double t) const;

    /// The row as given to init().
    const ExprRow& row() const { return m_row; }
    size_t keyCount() const { return m_row.keys.size(); }
    /// True when every key has the same value (the row is a constant).
    bool isConstant() const { return m_constant; }

    /// AE loopOut(type, numKeyframes) at `t`: the row itself up to its last key, then the loop. The
    /// loop covers the last `numKeyframes` segments (0 = all keys).
    double loopOut(LoopType type, int numKeyframes, double t) const;
    /// AE loopIn(type, numKeyframes) at `t`: the loop before the first key, then the row itself.
    double loopIn(LoopType type, int numKeyframes, double t) const;

    /// The index of the first key of loopOut's range / the last key of loopIn's range.
    size_t loopOutStart(int numKeyframes) const;
    size_t loopInEnd(int numKeyframes) const;

    /// The row's slope (value per second) arriving at key `i` from key `i - 1` (i >= 1).
    double slopeInto(size_t i) const;
    /// The row's slope leaving key `i` towards key `i + 1` (i + 1 < keyCount()).
    double slopeOutOf(size_t i) const;

private:
    Timeline m_timeline;
    ExprRow m_row;
    std::vector<ExprKey> m_keys;
    std::vector<EasingPoint> m_points;
    bool m_constant = true;
};

/// AE `linear(t, tMin, tMax, v1, v2)`: clamped, tMin / tMax swapped when reversed (v1 / v2 are not).
double aeLinear(double t, double tMin, double tMax, double v1, double v2);
/// AE `ease` / `easeIn` / `easeOut` (`which` = Builtin::Ease / EaseIn / EaseOut): linear's progress
/// shaped by the curve of xt/expr_noise.h, evaluated with xt::evalEasing like the runtime.
double aeEase(Builtin which, double t, double tMin, double tMax, double v1, double v2);
/// The progress of aeEase (0..1) for a linear progress `p`.
double aeEaseProgress(Builtin which, double p);

/// One-argument Math functions and degreesToRadians / radiansToDegrees.
double math1(Builtin b, double x);
/// Math.atan2(y, x) / Math.pow(x, y).
double math2(Builtin b, double x, double y);

/// The (min, max) argument counts of a built-in; max -1 = unbounded.
void builtinArity(Builtin b, int& minArgs, int& maxArgs);
/// The built-in's name as written (for diagnostics).
const char* builtinName(Builtin b);

/// Ramer-Douglas-Peucker on the vertical error (58 §58.3): which of the samples (`ts[k]`, `vs[k]`)
/// to keep so that the polyline through the kept ones stays within `tol` of every sample. The first
/// and the last are always kept; `ts` and `vs` have the same size, at least one. Shared by the
/// lowering's sampled rows and the run-time compiler's hidden curves.
std::vector<char> simplifySamples(const std::vector<double>& ts, const std::vector<double>& vs, double tol);

}  // namespace xt::compile::expr
