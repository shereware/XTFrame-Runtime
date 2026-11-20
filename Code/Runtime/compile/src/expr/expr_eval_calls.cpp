#include <algorithm>
#include <cmath>
#include <string>

#include "expr_eval.h"
#include "expr_lexer.h"
#include "expr_split.h"
#include "xt/expr_noise.h"

/// The built-in functions of the compile-time evaluator (Evaluator::evalCall and helpers).
/// Docs/design/58-value-functions-and-drivers.md §58.2's table, with AE's argument conventions:
/// linear / ease take 3 or 5 arguments, random 0 / 1 / 2, wiggle (freq, amp, octaves = 1,
/// amp_mult = 0.5, t = time), loopOut / loopIn (type = "cycle", numKeyframes = 0).

namespace xt::compile::expr {

namespace {

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

bool Evaluator::evalCall(const Node& n, Val& out) {
    const Builtin b = static_cast<Builtin>(n.op);
    const int32_t* args = m_prog.list(n);
    switch (b) {
        case Builtin::Port:
        case Builtin::RandomPerPlay:
            return runtimeLeaf(n, out);
        case Builtin::Wiggle: {
            double freq = 0;
            double amp = 0;
            double octaves = 1;
            double ampMult = 0.5;
            double t = effTime();
            if (!evalNumber(args[0], freq, "wiggle's frequency") ||
                !evalNumber(args[1], amp, "wiggle's amplitude")) {
                return false;
            }
            if (n.count > 2 && !evalNumber(args[2], octaves, "wiggle's octaves")) {
                return false;
            }
            if (n.count > 3 && !evalNumber(args[3], ampMult, "wiggle's amplitude multiplier")) {
                return false;
            }
            if (n.count > 4 && !evalNumber(args[4], t, "wiggle's time")) {
                return false;
            }
            const int32_t oct = std::clamp(toIntSat(std::floor(octaves)), 1, kExprMaxWiggleOctaves);
            const bool aperiodic = std::find(m_aperiodicWiggles.begin(), m_aperiodicWiggles.end(), n.site) !=
                                   m_aperiodicWiggles.end();  // T-777
            const float noise =
                exprWiggleNoise(static_cast<float>(t), static_cast<float>(freq),
                                aperiodic ? 0.0f : static_cast<float>(m_ctx.duration),
                                exprMixSeed(m_ctx.seed, n.site), oct, static_cast<float>(ampMult));
            out = Val::number(m_row.sample(t) + amp * static_cast<double>(noise));
            return finite(n, out);
        }
        case Builtin::LoopOut:
        case Builtin::LoopIn: {
            LoopType type = LoopType::Cycle;
            if (n.count > 0) {
                parseLoopType(
                    m_prog.strings[static_cast<size_t>(m_prog.nodes[static_cast<size_t>(args[0])].slot)],
                    type);
            }
            double keys = 0;
            if (n.count > 1 && !evalNumber(args[1], keys, "the number of keyframes")) {
                return false;
            }
            const int k = toIntSat(std::floor(keys));
            const double t = effTime();
            out = Val::number(b == Builtin::LoopOut ? m_row.loopOut(type, k, t) : m_row.loopIn(type, k, t));
            return finite(n, out);
        }
        case Builtin::ValueAtTime: {
            double t = 0;
            if (!evalNumber(args[0], t, "valueAtTime's time")) {
                return false;
            }
            out = Val::number(m_row.sample(t));
            return true;
        }
        case Builtin::Linear:
        case Builtin::Ease:
        case Builtin::EaseIn:
        case Builtin::EaseOut:
        case Builtin::Clamp:
            return evalMap(n, b, out);
        case Builtin::Random:
            return evalRandom(n, out);
        case Builtin::SeedRandom: {
            double seed = 0;
            if (!evalNumber(args[0], seed, "seedRandom's seed")) {
                return false;
            }
            bool timeless = false;
            if (n.count > 1) {
                Val v;
                if (!eval(args[1], v) || !truthy(n, v, timeless)) {
                    return false;
                }
            }
            m_randSeed = exprMixSeed(m_ctx.seed, exprSeedFromNumber(static_cast<float>(seed)));
            m_timeless = timeless;
            m_randCalls = 0;
            out = Val();
            return true;
        }
        case Builtin::Noise: {
            double x = 0;
            if (!evalNumber(args[0], x, "noise's argument")) {
                return false;
            }
            out = Val::number(static_cast<double>(
                exprNoise(static_cast<float>(x), kExprNoiseSeed)));  // T-777: AE's noise(x)
            return true;
        }
        case Builtin::PosterizeTime: {
            double fps = 0;
            if (!evalNumber(args[0], fps, "posterizeTime's rate")) {
                return false;
            }
            if (fps < 0) {
                return fail(n, "expr.eval", "posterizeTime's rate cannot be negative");
            }
            m_postFps = fps;
            m_posterized = true;
            out = Val();
            return true;
        }
        case Builtin::Length: {
            Val a;
            if (!eval(args[0], a)) {
                return false;
            }
            Val c;
            if (n.count > 1) {
                Val bb;
                if (!eval(args[1], bb) || !evalBinary(n, BinaryOp::Sub, a, bb, c)) {
                    return false;
                }
            } else {
                c = a;
            }
            if (c.kind == VKind::Poison) {
                out = c;
                return true;
            }
            if (c.kind == VKind::Undefined) {
                return fail(n, "expr.eval", "length() needs a value");
            }
            double sum = 0;
            for (int i = 0; i < c.len; ++i) {
                sum += c.v[i] * c.v[i];
            }
            out = Val::number(std::sqrt(sum));
            return finite(n, out);
        }
        default:
            return evalMath(n, b, out);
    }
}

bool Evaluator::evalMath(const Node& n, Builtin b, Val& out) {
    const int32_t* args = m_prog.list(n);
    if (b == Builtin::MathMin || b == Builtin::MathMax) {
        double r = 0;
        for (int32_t i = 0; i < n.count; ++i) {
            double x = 0;
            if (!evalNumber(args[i], x, builtinName(b))) {
                return false;
            }
            r = i == 0 ? x : (b == Builtin::MathMin ? std::min(r, x) : std::max(r, x));
        }
        out = Val::number(r);
        return true;
    }
    double x = 0;
    if (!evalNumber(args[0], x, builtinName(b))) {
        return false;
    }
    if (b == Builtin::MathAtan2 || b == Builtin::MathPow) {
        double y = 0;
        if (!evalNumber(args[1], y, builtinName(b))) {
            return false;
        }
        out = Val::number(math2(b, x, y));
    } else {
        out = Val::number(math1(b, x));
    }
    if (!std::isfinite(out.v[0])) {
        return fail(n, "expr.eval",
                    std::string(builtinName(b)) + "() has no finite result for this argument");
    }
    return true;
}

// linear / ease / easeIn / easeOut (3 or 5 arguments) and clamp, element-wise over arrays.
bool Evaluator::evalMap(const Node& n, Builtin b, Val& out) {
    const int32_t* args = m_prog.list(n);
    Val vals[5];
    for (int32_t i = 0; i < n.count; ++i) {
        if (!eval(args[i], vals[i])) {
            return false;
        }
        if (vals[i].kind == VKind::Poison) {
            out = vals[i];
            return true;
        }
        if (vals[i].kind == VKind::Undefined) {
            return fail(n, "expr.eval",
                        std::string(builtinName(b)) + "() needs values for all its arguments");
        }
    }
    int firstVec = 0;
    int len = 1;
    bool isArray = false;
    const std::string shapeError = mapOperandShape(b, vals, n.count, firstVec, len, isArray);
    if (!shapeError.empty()) {
        return fail(n, "expr.eval", shapeError);
    }
    auto comp = [&](int arg, int i) {
        return vals[arg].kind == VKind::Array ? vals[arg].v[i] : vals[arg].v[0];
    };
    out = Val();
    out.kind = isArray ? VKind::Array : VKind::Number;
    out.len = static_cast<uint8_t>(len);
    for (int i = 0; i < len; ++i) {
        if (b == Builtin::Clamp) {
            const double lo = std::min(comp(1, i), comp(2, i));
            const double hi = std::max(comp(1, i), comp(2, i));
            out.v[i] = std::clamp(comp(0, i), lo, hi);
            continue;
        }
        const double t = vals[0].v[0];
        const double tMin = n.count == 5 ? vals[1].v[0] : 0.0;
        const double tMax = n.count == 5 ? vals[2].v[0] : 1.0;
        const double v1 = comp(n.count - 2, i);
        const double v2 = comp(n.count - 1, i);
        out.v[i] = b == Builtin::Linear ? aeLinear(t, tMin, tMax, v1, v2) : aeEase(b, t, tMin, tMax, v1, v2);
    }
    return finite(n, out);
}

// random(): [0, 1); random(max); random(min, max) -- numbers or arrays (one draw per element).
bool Evaluator::evalRandom(const Node& n, Val& out) {
    const int32_t* args = m_prog.list(n);
    Val lo = Val::number(0);
    Val hi = Val::number(1);
    if (n.count == 1) {
        if (!eval(args[0], hi)) {
            return false;
        }
    } else if (n.count == 2) {
        if (!eval(args[0], lo) || !eval(args[1], hi)) {
            return false;
        }
    }
    if (lo.kind == VKind::Poison || hi.kind == VKind::Poison) {
        out = Val::poison();
        return true;
    }
    if (lo.kind == VKind::Undefined || hi.kind == VKind::Undefined) {
        return fail(n, "expr.eval", "random() needs values for its arguments");
    }
    if (lo.kind == VKind::Array && hi.kind == VKind::Array && lo.len != hi.len) {
        return fail(n, "expr.eval", "random() needs arrays of the same length");
    }
    const bool isArray = lo.kind == VKind::Array || hi.kind == VKind::Array;
    const int len = isArray ? (lo.kind == VKind::Array ? lo.len : hi.len) : 1;
    out = Val();
    out.kind = isArray ? VKind::Array : VKind::Number;
    out.len = static_cast<uint8_t>(len);
    for (int i = 0; i < len; ++i) {
        const double a = lo.kind == VKind::Array ? lo.v[i] : lo.v[0];
        const double c = hi.kind == VKind::Array ? hi.v[i] : hi.v[0];
        out.v[i] = a + (c - a) * randomDraw();
    }
    return finite(n, out);
}

// ---- public entry points ----------------------------------------------------------------------

EvalResult evaluateExpression(const Program& program, const ExprContext& ctx, double time) {
    EvalResult result;
    if (program.root < 0) {
        return result;
    }
    RowSampler row;
    if (!row.init(ctx.row, result.diagnostics)) {
        return result;
    }
    std::vector<RowSampler> vec(ctx.vecRows.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (!vec[i].init(ctx.vecRows[i], result.diagnostics)) {
            return result;
        }
    }
    Evaluator ev(program, ctx, row, vec, result.diagnostics);
    if (ctx.runtimeInputs != nullptr && (program.hasRuntimeLeaf || program.hasLayerRef)) {
        // T-777: a wiggle whose frequency comes from a run-time input does not repeat over the
        // duration (the run-time evaluator's rule, 58 §58.5); splitRuntime knows which ones those are.
        const SplitResult split = splitRuntime(program, ctx);
        std::vector<uint32_t> sites;
        for (const SplitNode& node : split.nodes) {
            if (node.op == SplitOp::Wiggle && node.aperiodic) {
                sites.push_back(node.site);
            }
        }
        ev.setAperiodicWiggles(std::move(sites));
    }
    Val v;
    result.status = ev.run(time, v);
    result.posterized = ev.posterized();  // T-781
    result.posterizeFps = ev.posterizeFps();
    if (result.status == ExprStatus::Ok) {
        result.value.size = v.len;
        result.value.isArray = v.kind == VKind::Array;
        for (int i = 0; i < v.len; ++i) {
            result.value.v[i] = v.v[i];
        }
    }
    return result;
}

bool componentOf(const ExprValue& value, int component, double& out, std::vector<Diagnostic>& diags) {
    if (value.size == 0) {
        addExprDiagnostic(diags, "expr.eval", "the expression produces no value", 1, 1);
        return false;
    }
    if (!value.isArray) {
        out = value.v[0];
        return true;
    }
    if (component < 0 || component >= value.size) {
        addExprDiagnostic(diags, "expr.eval",
                          "the result is an array of " + std::to_string(value.size) +
                              " numbers, with no element " + std::to_string(component) + " for this row",
                          1, 1);
        return false;
    }
    out = value.v[component];
    return true;
}

}  // namespace xt::compile::expr
