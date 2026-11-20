#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "expr.h"
#include "expr_builtins.h"
#include "expr_eval.h"
#include "expr_exact.h"
#include "expr_lexer.h"

/// lowerExpression(): Docs/design/58-value-functions-and-drivers.md §58.3. The exact rows first (a
/// bare loopOut / loopIn, valueAtTime(time - d), linear in time), each checked against direct
/// evaluation at every sample time before it is used; otherwise samples at kExprSampleRate merged
/// where they are collinear within the tolerance (Ramer-Douglas-Peucker on the vertical error), or
/// one Hold key per 1 / fps under posterizeTime(fps).

namespace xt::compile::expr {

namespace {

/// The most Hold keys a posterized lowering makes.
constexpr size_t kMaxPosterizedKeys = 20000;
/// The simplification keeps this share of the tolerance for itself; the rest absorbs the runtime's
/// single-precision interpolation.
constexpr double kToleranceShare = 0.95;

class Lowerer {
public:
    Lowerer(const Program& prog, const ExprContext& ctx, LowerResult& res)
        : m_prog(prog), m_ctx(ctx), m_res(res) {}

    void run() {
        if (m_prog.root < 0) {
            addExprDiagnostic(m_res.diagnostics, "expr.syntax", "the expression did not parse", 1, 1);
            return;
        }
        if (m_prog.hasRuntimeLeaf) {
            m_res.status = ExprStatus::NeedsRuntime;
            return;
        }
        if (!(m_ctx.duration >= 0) || !std::isfinite(m_ctx.duration)) {
            addExprDiagnostic(m_res.diagnostics, "expr.row", "the timeline duration is not a finite number",
                              1, 1);
            return;
        }
        if (!m_row.init(m_ctx.row, m_res.diagnostics)) {
            return;
        }
        m_vec.resize(m_ctx.vecRows.size());
        for (size_t i = 0; i < m_vec.size(); ++i) {
            if (!m_vec[i].init(m_ctx.vecRows[i], m_res.diagnostics)) {
                return;
            }
        }
        m_ev = std::make_unique<Evaluator>(m_prog, m_ctx, m_row, m_vec, m_res.diagnostics);
        m_tol = std::max(0.0, std::isfinite(m_ctx.tolerance) ? m_ctx.tolerance : kExprTolerance[2]);
        m_sampleCount =
            m_ctx.duration > 0 ? static_cast<size_t>(std::ceil(m_ctx.duration * kExprSampleRate - 1e-9)) : 0;
        double v0 = 0;
        const ExprStatus first = sampleAt(0.0, v0);
        if (first != ExprStatus::Ok) {
            m_res.status = first;
            return;
        }
        if (m_ev->posterized()) {
            finish(posterized(m_ev->posterizeFps()), LowerMethod::Posterized);
            return;
        }
        if (tryExact()) {
            return;
        }
        finish(sampled(), LowerMethod::Sampled);
    }

private:
    double sampleTime(size_t k) const {
        return k >= m_sampleCount ? m_ctx.duration : static_cast<double>(k) / kExprSampleRate;
    }

    ExprStatus sampleAt(double t, double& out) {
        Val v;
        ++m_res.samples;
        const ExprStatus st = m_ev->run(t, v);
        if (st != ExprStatus::Ok) {
            return st;
        }
        ExprValue value;
        value.size = v.len;
        value.isArray = v.kind == VKind::Array;
        for (int i = 0; i < v.len; ++i) {
            value.v[i] = v.v[i];
        }
        return componentOf(value, m_ctx.component, out, m_res.diagnostics) ? ExprStatus::Ok
                                                                           : ExprStatus::Error;
    }

    void finish(bool ok, LowerMethod method) {
        if (ok) {
            m_res.status = ExprStatus::Ok;
            m_res.method = method;
        } else {
            if (m_res.status != ExprStatus::NeedsRuntime) {
                m_res.status = ExprStatus::Error;
            }
            m_res.keys.clear();
        }
    }

    // ---- exact -------------------------------------------------------------------------------

    bool tryExact() {
        int32_t finalExpr = -1;
        int32_t prefix = 0;
        if (!simpleShape(m_prog, finalExpr, prefix)) {
            return false;
        }
        const size_t maxKeys = std::max<size_t>(64, 4 * (m_sampleCount + 1));
        const Node& f = m_prog.nodes[static_cast<size_t>(finalExpr)];
        const Node& root = m_prog.nodes[static_cast<size_t>(m_prog.root)];
        KeyList list;
        LowerMethod method = LowerMethod::None;
        if (f.kind == NodeKind::Call && root.count == 1 &&
            (static_cast<Builtin>(f.op) == Builtin::LoopOut ||
             static_cast<Builtin>(f.op) == Builtin::LoopIn)) {
            LoopType type = LoopType::Cycle;
            int numKeys = 0;
            if (f.count > 0) {
                parseLoopType(m_prog.strings[static_cast<size_t>(
                                  m_prog.nodes[static_cast<size_t>(m_prog.list(f)[0])].slot)],
                              type);
            }
            if (f.count > 1) {
                const Node& nk = m_prog.nodes[static_cast<size_t>(m_prog.list(f)[1])];
                if (nk.kind != NodeKind::Number) {
                    return false;
                }
                numKeys = static_cast<int>(std::clamp(std::floor(nk.number), 0.0, 1e6));
            }
            if (!expandLoop(m_row, static_cast<Builtin>(f.op) == Builtin::LoopOut, type, numKeys,
                            m_ctx.duration, maxKeys, list)) {
                return false;
            }
            method = LowerMethod::ExactLoop;
        } else if (f.kind == NodeKind::Call && static_cast<Builtin>(f.op) == Builtin::ValueAtTime &&
                   classifyTime(m_prog, m_prog.list(f)[0], m_row.isConstant()) == TimeClass::Linear) {
            Val a0;
            Val a1;
            if (m_ev->evalAfterPrefix(0.0, prefix, m_prog.list(f)[0], a0) != ExprStatus::Ok ||
                m_ev->evalAfterPrefix(1.0, prefix, m_prog.list(f)[0], a1) != ExprStatus::Ok ||
                a0.kind != VKind::Number || a1.kind != VKind::Number) {
                return false;
            }
            if (std::fabs((a1.v[0] - a0.v[0]) - 1.0) > 1e-9) {
                return false;  // time scaled, not shifted
            }
            shiftedKeys(m_row, -a0.v[0], list);
            method = LowerMethod::ExactShift;
        } else if (classifyTime(m_prog, finalExpr, m_row.isConstant()) != TimeClass::Other) {
            double v0 = 0;
            double vm = 0;
            double v1 = 0;
            const double d = m_ctx.duration;
            if (sampleAt(0.0, v0) != ExprStatus::Ok || sampleAt(d / 2, vm) != ExprStatus::Ok ||
                sampleAt(d, v1) != ExprStatus::Ok) {
                finish(false, LowerMethod::None);
                return true;  // the error is reported
            }
            const double scale = std::max({1.0, std::fabs(v0), std::fabs(v1)});
            if (std::fabs(vm - (v0 + v1) / 2) > 1e-9 * scale) {
                return false;
            }
            list.keys.push_back(loweredFromRow(0.0, v0, Easing::linear()));
            if (d > 0) {
                list.keys.push_back(loweredFromRow(d, v1, Easing::linear()));
            }
            method = LowerMethod::ExactLinear;
        } else {
            return false;
        }
        if (!clipKeys(list, m_ctx.duration, m_row.row().easingPoints) || !verify(list)) {
            return false;
        }
        m_res.keys = std::move(list.keys);
        finish(true, method);
        return true;
    }

    /// Checks `list` against direct evaluation at every sample time (away from its jumps).
    bool verify(const KeyList& list) {
        std::vector<ExprKey> keys;
        keys.reserve(list.keys.size());
        for (const LoweredKey& k : list.keys) {
            Easing e = k.easing;
            if (k.interp == LoweredInterp::Linear) {
                e = Easing::linear();
            } else if (k.interp == LoweredInterp::Hold) {
                e = Easing::hold();
            }
            keys.push_back(ExprKey{k.time, k.value, e});
        }
        RowSampler lowered;
        std::vector<Diagnostic> ignored;
        if (!lowered.init(ExprRow{keys, m_row.row().easingPoints}, ignored)) {
            return false;
        }
        for (size_t k = 0; k <= m_sampleCount; ++k) {
            const double t = sampleTime(k);
            bool nearJump = false;
            for (double j : list.jumps) {
                nearJump = nearJump || std::fabs(t - j) <= 2 * kExprJumpSeconds;
            }
            if (nearJump) {
                continue;
            }
            double direct = 0;
            if (sampleAt(t, direct) != ExprStatus::Ok) {
                return false;
            }
            if (std::fabs(direct - lowered.sample(t)) > m_tol) {
                return false;
            }
        }
        return true;
    }

    // ---- sampled -----------------------------------------------------------------------------

    bool sampled() {
        std::vector<double> ts(m_sampleCount + 1);
        std::vector<double> vs(m_sampleCount + 1);
        for (size_t k = 0; k <= m_sampleCount; ++k) {
            ts[k] = sampleTime(k);
            const ExprStatus st = sampleAt(ts[k], vs[k]);
            if (st != ExprStatus::Ok) {
                m_res.status = st;
                return false;
            }
        }
        const double tol = m_tol * kToleranceShare;
        const std::vector<char> keep = simplifySamples(ts, vs, tol);
        for (size_t k = 0; k < ts.size(); ++k) {
            if (keep[k] != 0) {
                m_res.keys.push_back(loweredFromRow(ts[k], vs[k], Easing::linear()));
            }
        }
        return true;
    }

    // ---- posterized --------------------------------------------------------------------------

    bool posterized(double fps) {
        const double d = m_ctx.duration;
        size_t steps = 0;
        if (fps > 0) {
            const double n = std::floor(d * fps + 1e-9);
            if (n > static_cast<double>(kMaxPosterizedKeys)) {
                addExprDiagnostic(m_res.diagnostics, "expr.limit",
                                  "posterizeTime's rate gives more than 20000 keys over this timeline", 1, 1);
                return false;
            }
            steps = static_cast<size_t>(n);
        }
        const double tol = m_tol * kToleranceShare;
        for (size_t k = 0; k <= steps; ++k) {
            const double t = fps > 0 ? static_cast<double>(k) / fps : 0.0;
            double v = 0;
            const ExprStatus st = sampleAt(t, v);
            if (st != ExprStatus::Ok) {
                m_res.status = st;
                return false;
            }
            if (!m_res.keys.empty() && std::fabs(v - m_res.keys.back().value) <= tol) {
                continue;
            }
            LoweredKey key = loweredFromRow(t, v, Easing::hold());
            m_res.keys.push_back(key);
        }
        return true;
    }

    const Program& m_prog;
    const ExprContext& m_ctx;
    LowerResult& m_res;
    RowSampler m_row;
    std::vector<RowSampler> m_vec;
    std::unique_ptr<Evaluator> m_ev;
    double m_tol = 0;
    size_t m_sampleCount = 0;
};

}  // namespace

LowerResult lowerExpression(const Program& program, const ExprContext& ctx) {
    LowerResult result;
    Lowerer lowerer(program, ctx, result);
    lowerer.run();
    if (result.status != ExprStatus::Ok) {
        result.keys.clear();
        result.method = LowerMethod::None;
    }
    return result;
}

}  // namespace xt::compile::expr
