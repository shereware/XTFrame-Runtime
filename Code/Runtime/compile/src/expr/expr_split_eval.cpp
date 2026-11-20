#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "expr_lexer.h"
#include "expr_split_internal.h"
#include "xt/expr_noise.h"

/// Evaluation of splitRuntime()'s expanded form (expr_split.h): every node reachable from the
/// requested root is computed once, in index order (children come first), with no recursion; a
/// Select picks its branch, so an error in the branch not taken is ignored as in the evaluator.

namespace xt::compile::expr {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

int32_t toIntSat(double v) {
    if (!(v == v)) {
        return 0;
    }
    if (v <= -2147483648.0) {
        return INT32_MIN;
    }
    if (v >= 2147483647.0) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(v);
}

}  // namespace

bool isRuntimeLeaf(SplitOp op) {
    return op == SplitOp::Port || op == SplitOp::PortAt || op == SplitOp::Layout ||
           op == SplitOp::LayerProp || op == SplitOp::PlayRandom;
}

double computeSplitNode(const SplitNode& n, int32_t id, const double* a, SplitEnv& env) {
    const ExprRuntimeInputs* in = env.ctx.runtimeInputs;
    switch (n.op) {
        case SplitOp::Const:
            return n.value;
        case SplitOp::Time:
            return env.time;
        case SplitOp::CurveRef:
            return kNaN;  // resolved by the caller
        case SplitOp::SubProgram: {
            if (env.evaluator == nullptr) {
                return kNaN;
            }
            double v = 0;
            const ExprStatus st = env.evaluator->runPrefix(env.time, n.ref, n.slot, n.comp, v);
            if (st != ExprStatus::Ok) {
                env.status = st;
                return kNaN;
            }
            return v;
        }
        case SplitOp::ValueAt:
            return env.row.sample(a[0]);
        case SplitOp::VecAt:
            return static_cast<size_t>(n.comp) < env.vec.size()
                       ? env.vec[static_cast<size_t>(n.comp)].sample(a[0])
                       : kNaN;
        case SplitOp::OtherRowAt: {
            RowSampler* s = nullptr;
            for (auto& o : env.others) {
                if (o.first == id) {
                    s = o.second.get();
                }
            }
            if (s == nullptr) {
                auto made = std::make_unique<RowSampler>();
                std::vector<Diagnostic> ignored;
                if (!env.ctx.otherRows) {
                    return kNaN;
                }
                const ExprRowRef ref = env.ctx.otherRows(n.key, n.name);
                if (ref.kind != ExprRefKind::Row || !made->init(ref.row, ignored)) {
                    return kNaN;
                }
                s = made.get();
                env.others.emplace_back(id, std::move(made));
            }
            return s->sample(a[0]);
        }
        case SplitOp::LoopOut:
            return env.row.loopOut(static_cast<LoopType>(n.loopType), toIntSat(n.value), a[0]);
        case SplitOp::LoopIn:
            return env.row.loopIn(static_cast<LoopType>(n.loopType), toIntSat(n.value), a[0]);
        case SplitOp::Wiggle: {
            const int32_t oct = std::clamp(toIntSat(std::floor(a[2])), 1, kExprMaxWiggleOctaves);
            const float noise =
                exprWiggleNoise(static_cast<float>(a[4]), static_cast<float>(a[0]),
                                n.aperiodic ? 0.0f : static_cast<float>(env.ctx.duration),  // T-777
                                exprMixSeed(env.ctx.seed, n.site), oct, static_cast<float>(a[3]));
            return env.row.sample(a[4]) + a[1] * static_cast<double>(noise);
        }
        case SplitOp::Noise:
            return static_cast<double>(
                exprNoise(static_cast<float>(a[0]), kExprNoiseSeed));  // T-777: AE's noise(x)
        case SplitOp::Random: {
            const uint32_t frame = n.timeless ? 0u : exprRandomFrame(static_cast<float>(a[2]));
            const double draw =
                static_cast<double>(exprRandom01(exprRandomSeed(n.seed, frame, n.site, n.timeless)));
            return a[0] + (a[1] - a[0]) * draw;
        }
        case SplitOp::Posterize:
            if (a[1] == 0) {
                return 0.0;
            }
            return std::floor(a[0] * a[1] + 1e-9) / a[1];
        case SplitOp::Neg:
            return -a[0];
        case SplitOp::Not:
            return a[0] != 0 ? 0.0 : 1.0;
        case SplitOp::Add:
            return a[0] + a[1];
        case SplitOp::Sub:
            return a[0] - a[1];
        case SplitOp::Mul:
            return a[0] * a[1];
        case SplitOp::Div:
            return a[1] == 0 ? kNaN : a[0] / a[1];
        case SplitOp::Mod:
            return a[1] == 0 ? kNaN : std::fmod(a[0], a[1]);
        case SplitOp::Lt:
            return a[0] < a[1] ? 1.0 : 0.0;
        case SplitOp::Le:
            return a[0] <= a[1] ? 1.0 : 0.0;
        case SplitOp::Gt:
            return a[0] > a[1] ? 1.0 : 0.0;
        case SplitOp::Ge:
            return a[0] >= a[1] ? 1.0 : 0.0;
        case SplitOp::Eq:
            return a[0] == a[1] ? 1.0 : 0.0;
        case SplitOp::Ne:
            return a[0] != a[1] ? 1.0 : 0.0;
        case SplitOp::Select:
            return a[0] != 0 ? a[1] : a[2];
        case SplitOp::Math1:
            return math1(n.builtin, a[0]);
        case SplitOp::Math2:
            return math2(n.builtin, a[0], a[1]);
        case SplitOp::Min:
            return std::min(a[0], a[1]);
        case SplitOp::Max:
            return std::max(a[0], a[1]);
        case SplitOp::Clamp: {
            const double lo = std::min(a[1], a[2]);
            const double hi = std::max(a[1], a[2]);
            return std::clamp(a[0], lo, hi);
        }
        case SplitOp::Linear:
            return aeLinear(a[0], a[1], a[2], a[3], a[4]);
        case SplitOp::Ease:
            return aeEase(n.builtin, a[0], a[1], a[2], a[3], a[4]);
        case SplitOp::Port:
        case SplitOp::PortAt:
            if (in != nullptr && in->port) {
                return in->port(n.name, n.op == SplitOp::Port ? env.time : a[0]);
            }
            break;
        case SplitOp::Layout:
            if (in != nullptr && in->layout) {
                return in->layout(n.key, n.comp);
            }
            break;
        case SplitOp::LayerProp:
            if (in != nullptr && in->layerProp) {
                return in->layerProp(n.key, n.name, a[0]);
            }
            break;
        case SplitOp::PlayRandom:
            if (in != nullptr && in->playRandom) {
                return in->playRandom(n.site, a[0], a[1]);
            }
            break;
    }
    env.status = ExprStatus::NeedsRuntime;
    return kNaN;
}

namespace {

/// Evaluates node `root` of `split` at `t`.
ExprStatus evalSplitRoot(const Program& program, const SplitResult& split, int32_t root,
                         const ExprContext& ctx, double t, double& out, std::vector<Diagnostic>& diags) {
    if (split.status != ExprStatus::Ok || root < 0 || static_cast<size_t>(root) >= split.nodes.size()) {
        addExprDiagnostic(diags, "expr.eval", "nothing to evaluate", 1, 1);
        return ExprStatus::Error;
    }
    RowSampler row;
    if (!row.init(ctx.row, diags)) {
        return ExprStatus::Error;
    }
    std::vector<RowSampler> vec(ctx.vecRows.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (!vec[i].init(ctx.vecRows[i], diags)) {
            return ExprStatus::Error;
        }
    }
    Evaluator ev(program, ctx, row, vec, diags);
    SplitEnv env(program, ctx, row, vec);
    env.evaluator = &ev;
    env.time = t;
    // The reachable nodes (a CurveRef reaches its curve's root), then computed in index order.
    std::vector<char> reach(split.nodes.size(), 0);
    std::vector<int32_t> stack{root};
    while (!stack.empty()) {
        const int32_t id = stack.back();
        stack.pop_back();
        if (reach[static_cast<size_t>(id)] != 0) {
            continue;
        }
        reach[static_cast<size_t>(id)] = 1;
        const SplitNode& n = split.nodes[static_cast<size_t>(id)];
        if (n.op == SplitOp::CurveRef) {
            stack.push_back(split.curves[static_cast<size_t>(n.ref)].root);
        }
        for (int i = 0; i < n.argc; ++i) {
            stack.push_back(n.args[i]);
        }
    }
    std::vector<double> val(split.nodes.size(), 0.0);
    for (size_t id = 0; id < split.nodes.size(); ++id) {
        if (reach[id] == 0) {
            continue;
        }
        const SplitNode& n = split.nodes[id];
        if (n.op == SplitOp::CurveRef) {
            val[id] = val[static_cast<size_t>(split.curves[static_cast<size_t>(n.ref)].root)];
            continue;
        }
        double a[5] = {0, 0, 0, 0, 0};
        for (int i = 0; i < n.argc; ++i) {
            a[i] = val[static_cast<size_t>(n.args[i])];
        }
        val[id] = computeSplitNode(n, static_cast<int32_t>(id), a, env);
        if (env.status != ExprStatus::Ok) {
            if (env.status == ExprStatus::Error && diags.empty()) {
                addExprDiagnostic(diags, "expr.eval", "a sub-program failed", 1, 1);
            }
            return env.status;
        }
    }
    out = val[static_cast<size_t>(root)];
    if (!std::isfinite(out)) {
        addExprDiagnostic(diags, "expr.eval", "the result is not a finite number", 1, 1);
        return ExprStatus::Error;
    }
    return ExprStatus::Ok;
}

}  // namespace

ExprStatus evalSplitComponent(const Program& program, const SplitResult& split, int component,
                              const ExprContext& ctx, double t, double& out, std::vector<Diagnostic>& diags) {
    if (component < 0 || static_cast<size_t>(component) >= split.components.size()) {
        addExprDiagnostic(diags, "expr.eval", "no such component", 1, 1);
        return ExprStatus::Error;
    }
    return evalSplitRoot(program, split, split.components[static_cast<size_t>(component)], ctx, t, out,
                         diags);
}

ExprStatus evalSplitCurve(const Program& program, const SplitResult& split, size_t curve,
                          const ExprContext& ctx, double t, double& out, std::vector<Diagnostic>& diags) {
    if (curve >= split.curves.size()) {
        addExprDiagnostic(diags, "expr.eval", "no such curve", 1, 1);
        return ExprStatus::Error;
    }
    return evalSplitRoot(program, split, split.curves[curve].root, ctx, t, out, diags);
}

}  // namespace xt::compile::expr
