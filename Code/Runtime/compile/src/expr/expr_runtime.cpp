#include "expr_runtime.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "expr_builtins.h"
#include "expr_lexer.h"
#include "expr_split.h"
#include "xt/expr_noise.h"

/// compileRuntime(): see expr_runtime.h and Docs/design/58-value-functions-and-drivers.md §58.5.
/// Works on splitRuntime()'s expanded form (before its own curve extraction): a node that depends
/// on a run-time input is emitted as ops; a time-only node becomes a hidden curve unless it is
/// `time`, `value` or small arithmetic on them (exact and cheaper as ops), or -- under the test
/// option forceOps -- any op the run-time evaluator has.

namespace xt::compile::expr {

namespace {

constexpr uint32_t kRandomSalt = 0x52414E44u;  // as in expr_eval.cpp
/// floor(t × fps + this) / fps: posterizeTime() at run time (single precision; the compile-time
/// evaluator adds 1e-9 in double precision).
constexpr float kPosterizeEpsilon = 1e-4f;
/// A hidden curve is simplified within this share of the row's tolerance: it is combined with
/// run-time inputs afterwards (`Math.sin(time) * port("k") * 10` multiplies its error), so it keeps
/// far more of its samples than a lowered row does.
constexpr double kCurveToleranceShare = 0.02;
/// The most nodes of time-only arithmetic kept as ops instead of a curve.
constexpr size_t kCheapTimeNodes = 8;
/// The longest port delay (58 §58.5a), seconds.
constexpr double kMaxDelaySeconds = 4.0;

/// The multiplicative inverse of an odd number modulo 2^32 (Newton's iteration).
uint32_t inverseOdd(uint32_t a) {
    uint32_t x = a;
    for (int i = 0; i < 5; ++i) {
        x *= 2u - a * x;
    }
    return x;
}

/// The inverse of xt::exprHash.
uint32_t exprUnhash(uint32_t x) {
    x ^= x >> 16;
    x *= inverseOdd(0x846ca68bU);
    x ^= (x >> 15) ^ (x >> 30);
    x *= inverseOdd(0x7feb352dU);
    x ^= x >> 16;
    return x;
}

/// A baked hidden curve: its index in RuntimeProgram::curves, or a constant.
struct Baked {
    bool constant = false;
    double value = 0;
    uint32_t index = 0;
};

class Emitter {
public:
    Emitter(const Program& prog, const ExprContext& ctx, const RuntimeCompileOptions& opt,
            RuntimeProgram& out)
        : m_prog(prog), m_ctx(ctx), m_opt(opt), m_out(out) {}

    void run() {
        m_split = splitRuntime(m_prog, m_ctx);
        if (m_split.status != ExprStatus::Ok) {
            m_out.diagnostics = m_split.diagnostics;
            return;
        }
        const size_t comp = m_split.isArray ? static_cast<size_t>(std::max(m_ctx.component, 0)) : 0;
        if (comp >= m_split.expanded.size()) {
            fail("expr.eval", "the result is an array of " + std::to_string(m_split.expanded.size()) +
                                  " numbers, with no element " + std::to_string(comp) + " for this row");
            return;
        }
        if (!emit(m_split.expanded[comp])) {
            return;
        }
        if (m_out.ops.size() > kExprMaxOps) {
            fail("expr.limit", "after expansion the expression needs " + std::to_string(m_out.ops.size()) +
                                   " operations at run time (the limit is " + std::to_string(kExprMaxOps) +
                                   ")");
            return;
        }
        uint32_t depth = 0;
        uint32_t deepest = 0;
        for (const RuntimeOp& op : m_out.ops) {
            depth = depth - op.argc + 1;
            deepest = std::max(deepest, depth);
            switch (op.op) {
                case ExprOp::PushPort:
                    m_out.inputMask |= kExprInputPort;
                    break;
                case ExprOp::PushProp:
                    m_out.inputMask |= kExprInputProp;
                    break;
                case ExprOp::PushLayout:
                    m_out.inputMask |= kExprInputLayout;
                    break;
                case ExprOp::PushPlayRandom:
                    m_out.inputMask |= kExprInputPlayRandom;
                    break;
                case ExprOp::PushPortDelayed:
                    m_out.inputMask |= kExprInputPortHistory;
                    break;
                case ExprOp::PushPropDelayed:  // T-781
                    m_out.inputMask |= kExprInputPropHistory;
                    break;
                default:
                    break;
            }
        }
        if (deepest > kExprMaxStack) {
            fail("expr.limit", "after expansion the expression needs a stack of " + std::to_string(deepest) +
                                   " values at run time (the limit is " + std::to_string(kExprMaxStack) +
                                   "); split it into shorter parts with variables");
            return;
        }
        m_out.maxStack = static_cast<uint16_t>(deepest);
        m_out.status = ExprStatus::Ok;
    }

private:
    const SplitNode& nd(int32_t id) const { return m_split.nodes[static_cast<size_t>(id)]; }

    bool fail(const char* code, std::string message) {
        if (!m_failed) {
            addExprDiagnostic(m_out.diagnostics, code, std::move(message), 1, 1);
        }
        m_failed = true;
        return false;
    }

    void op(ExprOp o, uint8_t argc, float imm = 0, uint32_t ref = 0, int32_t symbol = -1) {
        RuntimeOp r;
        r.op = o;
        r.argc = argc;
        r.imm = imm;
        r.ref = ref;
        r.symbol = symbol;
        m_out.ops.push_back(r);
    }

    bool konst(double v) {
        const float f = static_cast<float>(v);
        if (!std::isfinite(f)) {
            return fail("expr.eval", "a number in the expression is outside the single-precision range");
        }
        op(ExprOp::PushConst, 0, f);
        return true;
    }

    int32_t symbol(RuntimeSymbol::Kind kind, const std::string& name, const ExprLayerKey& key) {
        RuntimeSymbol s;
        s.kind = kind;
        s.name = name;
        s.key = key;
        m_out.symbols.push_back(std::move(s));
        return static_cast<int32_t>(m_out.symbols.size() - 1);
    }

    void notePort(const std::string& name) {
        if (std::find(m_out.ports.begin(), m_out.ports.end(), name) == m_out.ports.end()) {
            m_out.ports.push_back(name);
        }
    }

    bool isTime(int32_t id) const { return nd(id).op == SplitOp::Time; }
    bool isConstValue(int32_t id, double v) const { return nd(id).op == SplitOp::Const && nd(id).value == v; }

    /// `time`, constants and small arithmetic on them (posterizeTime() with a constant rate,
    /// valueAtTime of such a time): exact and cheap as ops.
    bool cheapTime(int32_t id, size_t& budget) const {
        if (budget == 0) {
            return false;
        }
        --budget;
        const SplitNode& n = nd(id);
        switch (n.op) {
            case SplitOp::Const:
            case SplitOp::Time:
                return true;
            case SplitOp::Posterize:
                return isTime(n.args[0]) && nd(n.args[1]).op == SplitOp::Const;
            case SplitOp::ValueAt:
            case SplitOp::Neg:
            case SplitOp::Add:
            case SplitOp::Sub:
            case SplitOp::Mul:
            case SplitOp::Div:
                for (int i = 0; i < n.argc; ++i) {
                    if (!cheapTime(n.args[i], budget)) {
                        return false;
                    }
                }
                return true;
            default:
                return false;
        }
    }

    /// The node's own op exists at run time (its arguments decide for themselves).
    bool hasOp(int32_t id) const {
        const SplitNode& n = nd(id);
        switch (n.op) {
            case SplitOp::LoopOut:
            case SplitOp::LoopIn:
            case SplitOp::SubProgram:
            case SplitOp::VecAt:
            case SplitOp::CurveRef:
                return false;
            case SplitOp::Wiggle:
                // Only a constant frequency repeats over the duration at run time; the compile-time
                // curve of a time-only wiggle always does.
                return n.runtime || nd(n.args[0]).op == SplitOp::Const;
            default:
                return true;
        }
    }

    bool inlineTimeOnly(int32_t id) const {
        size_t budget = kCheapTimeNodes;
        return cheapTime(id, budget) || (m_opt.forceOps && hasOp(id));
    }

    bool emit(int32_t id) {
        if (m_failed) {
            return false;
        }
        const SplitNode& n = nd(id);
        if (n.op == SplitOp::Const) {
            return konst(n.value);
        }
        if (!n.runtime && n.timeDep && !inlineTimeOnly(id)) {
            return emitCurve(id);
        }
        return emitOp(id);
    }

    // ---- hidden curves --------------------------------------------------------------------------

    bool hasPosterize(int32_t id) {
        const auto it = m_hasPost.find(id);
        if (it != m_hasPost.end()) {
            return it->second;
        }
        const SplitNode n = nd(id);
        bool found = n.op == SplitOp::Posterize;
        for (int i = 0; i < n.argc && !found; ++i) {
            found = hasPosterize(n.args[i]);
        }
        m_hasPost[id] = found;
        return found;
    }

    /// True when every `time` in `id`'s subtree is posterizeTime(fps)'s with one constant fps (set).
    bool uniformPosterize(int32_t id, int32_t& fps) {
        std::vector<int32_t> stack{id};
        double rate = 0;
        bool any = false;
        while (!stack.empty()) {
            const int32_t cur = stack.back();
            stack.pop_back();
            const SplitNode& n = nd(cur);
            if (n.op == SplitOp::Time) {
                return false;
            }
            if (n.op == SplitOp::Posterize) {
                if (!isTime(n.args[0]) || nd(n.args[1]).op != SplitOp::Const ||
                    (any && nd(n.args[1]).value != rate)) {
                    return false;
                }
                rate = nd(n.args[1]).value;
                fps = n.args[1];
                any = true;
                continue;
            }
            for (int i = 0; i < n.argc; ++i) {
                stack.push_back(n.args[i]);
            }
        }
        return any;
    }

    /// `id` with every posterizeTime(fps) of `time` replaced by `time` (new nodes appended).
    int32_t stripPosterize(int32_t id) {
        const auto it = m_stripped.find(id);
        if (it != m_stripped.end()) {
            return it->second;
        }
        int32_t result = id;
        if (nd(id).op == SplitOp::Posterize) {
            result = nd(id).args[0];
        } else if (hasPosterize(id)) {
            SplitNode copy = nd(id);
            for (int i = 0; i < copy.argc; ++i) {
                copy.args[i] = stripPosterize(copy.args[i]);
            }
            m_split.nodes.push_back(std::move(copy));
            result = static_cast<int32_t>(m_split.nodes.size() - 1);
        }
        m_stripped[id] = result;
        return result;
    }

    /// Samples node `root` over [0, duration] at kExprSampleRate and simplifies the samples (58 §58.3).
    bool bake(int32_t root, Baked& out) {
        const auto it = m_baked.find(root);
        if (it != m_baked.end()) {
            out = it->second;
            return true;
        }
        const size_t curve = m_split.curves.size();
        m_split.curves.push_back(SplitCurve{root});
        const double d = m_ctx.duration;
        const size_t count = d > 0 ? static_cast<size_t>(std::ceil(d * kExprSampleRate - 1e-9)) : 0;
        std::vector<double> ts(count + 1);
        std::vector<double> vs(count + 1);
        for (size_t k = 0; k <= count; ++k) {
            ts[k] = k >= count ? d : static_cast<double>(k) / kExprSampleRate;
            std::vector<Diagnostic> diags;
            const ExprStatus st = evalSplitCurve(m_prog, m_split, curve, m_ctx, ts[k], vs[k], diags);
            if (st != ExprStatus::Ok || !std::isfinite(static_cast<float>(vs[k]))) {
                return fail("expr.eval", diags.empty()
                                             ? std::string("a part that depends on time only has no "
                                                           "finite value at some time")
                                             : diags.front().message);
            }
        }
        if (std::all_of(vs.begin(), vs.end(), [&](double v) { return v == vs.front(); })) {
            out.constant = true;
            out.value = vs.front();
            m_baked[root] = out;
            return true;
        }
        const double tol =
            std::max(0.0, std::isfinite(m_ctx.tolerance) ? m_ctx.tolerance : kExprTolerance[2]) *
            kCurveToleranceShare;
        const std::vector<char> keep = simplifySamples(ts, vs, tol);
        std::vector<IrExprKey> keys;
        for (size_t k = 0; k < ts.size(); ++k) {
            if (keep[k] != 0) {
                IrExprKey key{};
                key.time = static_cast<float>(ts[k]);
                key.value = static_cast<float>(vs[k]);
                key.interp = 0;
                keys.push_back(key);
            }
        }
        out.constant = false;
        out.index = static_cast<uint32_t>(m_out.curves.size());
        m_out.curves.push_back(std::move(keys));
        m_baked[root] = out;
        return true;
    }

    bool emitCurve(int32_t id) {
        Baked b;
        if (hasPosterize(id)) {
            int32_t fps = -1;
            if (uniformPosterize(id, fps)) {
                // The curve of the expression without posterizeTime(), read at the quantized time.
                int32_t timeId = -1;
                std::vector<int32_t> stack{id};
                while (!stack.empty() && timeId < 0) {
                    const int32_t cur = stack.back();
                    stack.pop_back();
                    if (nd(cur).op == SplitOp::Posterize) {
                        timeId = nd(cur).args[0];
                    }
                    for (int i = 0; i < nd(cur).argc; ++i) {
                        stack.push_back(nd(cur).args[i]);
                    }
                }
                if (!bake(stripPosterize(id), b)) {
                    return false;
                }
                if (b.constant) {
                    return konst(b.value);
                }
                if (!emitPosterize(timeId, fps)) {
                    return false;
                }
                op(ExprOp::CurveAt, 1, 0, b.index);
                return true;
            }
            if (hasOp(id)) {
                return emitOp(id);
            }
            return fail("expr.limit",
                        "posterizeTime() applies to only part of a loopOut() / loopIn() / valueVec / loop "
                        "here, which cannot be combined with run-time inputs; call posterizeTime() first");
        }
        if (!bake(id, b)) {
            return false;
        }
        if (b.constant) {
            return konst(b.value);
        }
        op(ExprOp::PushCurve, 0, 0, b.index);
        return true;
    }

    /// floor(t × fps + epsilon) / fps (0 when fps is 0), as splitRuntime's Posterize.
    bool emitPosterize(int32_t t, int32_t fps) {
        const SplitNode& f = nd(fps);
        if (f.op == SplitOp::Const) {
            if (f.value == 0) {
                return konst(0.0);
            }
            if (!emit(t) || !konst(f.value)) {
                return false;
            }
            op(ExprOp::Mul, 2);
            konst(kPosterizeEpsilon);
            op(ExprOp::Add, 2);
            op(ExprOp::Floor, 1);
            if (!konst(f.value)) {
                return false;
            }
            op(ExprOp::Div, 2);
            return true;
        }
        // A rate from a run-time input: Select(fps == 0, 0, floor(t × fps + e) / fps).
        if (!emit(fps)) {
            return false;
        }
        konst(0.0);
        op(ExprOp::Eq, 2);
        konst(0.0);
        if (!emit(t) || !emit(fps)) {
            return false;
        }
        op(ExprOp::Mul, 2);
        konst(kPosterizeEpsilon);
        op(ExprOp::Add, 2);
        op(ExprOp::Floor, 1);
        if (!emit(fps)) {
            return false;
        }
        op(ExprOp::Div, 2);
        op(ExprOp::Select, 3);
        return true;
    }

    // ---- ops ------------------------------------------------------------------------------------

    bool emitArgs(const SplitNode& n) {
        for (int i = 0; i < n.argc; ++i) {
            if (!emit(n.args[i])) {
                return false;
            }
        }
        return true;
    }

    bool simple(const SplitNode& n, ExprOp o) {
        if (!emitArgs(n)) {
            return false;
        }
        op(o, n.argc);
        return true;
    }

    bool emitOp(int32_t id) {
        const SplitNode n = nd(id);  // A copy: emit() may append nodes.
        switch (n.op) {
            case SplitOp::Time:
                op(ExprOp::PushTime, 0);
                return true;
            case SplitOp::ValueAt:
                if (isTime(n.args[0])) {
                    op(ExprOp::PushValue, 0);
                    return true;
                }
                return simple(n, ExprOp::ValueAt);
            case SplitOp::OtherRowAt:
                if (!emit(n.args[0])) {
                    return false;
                }
                op(ExprOp::RefValueAt, 1, 0, 0, symbol(RuntimeSymbol::Kind::OtherRow, n.name, n.key));
                return true;
            case SplitOp::LayerProp:
                if (m_opt.runtimeRow && m_opt.runtimeRow(n.key, n.name)) {
                    if (!emit(n.args[0])) {
                        return false;
                    }
                    op(ExprOp::RefValueAt, 1, 0, 0, symbol(RuntimeSymbol::Kind::RuntimeRow, n.name, n.key));
                    return true;
                }
                if (!isTime(n.args[0])) {
                    return emitPropAt(n);  // T-781: from the property history.
                }
                op(ExprOp::PushProp, 0, 0, 0, symbol(RuntimeSymbol::Kind::LayerProp, n.name, n.key));
                return true;
            case SplitOp::Layout:
                op(ExprOp::PushLayout, 0, static_cast<float>(n.comp), 0,
                   symbol(RuntimeSymbol::Kind::Layout, std::string(), n.key));
                return true;
            case SplitOp::Port:
                notePort(n.name);
                op(ExprOp::PushPort, 0, 0, 0, symbol(RuntimeSymbol::Kind::Port, n.name, ExprLayerKey{}));
                return true;
            case SplitOp::PortAt:
                return emitPortAt(n);
            case SplitOp::PlayRandom:
                // randomPerPlay(min, max) = min + (max - min) × the play's draw in [0, 1).
                if (!emitArgs(n)) {
                    return false;
                }
                op(ExprOp::PushPlayRandom, 0, 0, n.site);
                op(ExprOp::Lerp, 3);
                return true;
            case SplitOp::Wiggle:
                return emitWiggle(n);
            case SplitOp::Noise:
                if (!emit(n.args[0])) {
                    return false;
                }
                op(ExprOp::Noise, 1, 0, exprNoiseRef(m_ctx.seed));
                return true;
            case SplitOp::Random:
                return emitRandom(n);
            case SplitOp::Posterize:
                return emitPosterize(n.args[0], n.args[1]);
            case SplitOp::Neg:
                return simple(n, ExprOp::Neg);
            case SplitOp::Not:
                return simple(n, ExprOp::Not);
            case SplitOp::Add:
                return simple(n, ExprOp::Add);
            case SplitOp::Sub:
                return simple(n, ExprOp::Sub);
            case SplitOp::Mul:
                return simple(n, ExprOp::Mul);
            case SplitOp::Div:
                return simple(n, ExprOp::Div);
            case SplitOp::Mod:
                return simple(n, ExprOp::Mod);
            case SplitOp::Lt:
                return simple(n, ExprOp::Lt);
            case SplitOp::Le:
                return simple(n, ExprOp::Le);
            case SplitOp::Gt:
                return simple(n, ExprOp::Gt);
            case SplitOp::Ge:
                return simple(n, ExprOp::Ge);
            case SplitOp::Eq:
                return simple(n, ExprOp::Eq);
            case SplitOp::Ne:
                return simple(n, ExprOp::Ne);
            case SplitOp::Select:
                return simple(n, ExprOp::Select);
            case SplitOp::Min:
                return simple(n, ExprOp::Min);
            case SplitOp::Max:
                return simple(n, ExprOp::Max);
            case SplitOp::Clamp:
                return simple(n, ExprOp::Clamp);
            case SplitOp::Linear:
                return simple(n, ExprOp::Linear);
            case SplitOp::Ease:
                return simple(n, n.builtin == Builtin::EaseIn    ? ExprOp::EaseIn
                                 : n.builtin == Builtin::EaseOut ? ExprOp::EaseOut
                                                                 : ExprOp::Ease);
            case SplitOp::Math1:
                return emitMath1(n);
            case SplitOp::Math2:
                return simple(n, n.builtin == Builtin::MathAtan2 ? ExprOp::Atan2 : ExprOp::Pow);
            default:
                return fail("expr.limit", "a part of the expression has no run-time form");
        }
    }

    bool emitMath1(const SplitNode& n) {
        if (n.builtin == Builtin::DegreesToRadians || n.builtin == Builtin::RadiansToDegrees) {
            if (!emit(n.args[0])) {
                return false;
            }
            const double k = 3.14159265358979323846 / 180.0;
            konst(n.builtin == Builtin::DegreesToRadians ? k : 1.0 / k);
            op(ExprOp::Mul, 2);
            return true;
        }
        ExprOp o = ExprOp::Abs;
        switch (n.builtin) {
            case Builtin::MathSin:
                o = ExprOp::Sin;
                break;
            case Builtin::MathCos:
                o = ExprOp::Cos;
                break;
            case Builtin::MathTan:
                o = ExprOp::Tan;
                break;
            case Builtin::MathAsin:
                o = ExprOp::Asin;
                break;
            case Builtin::MathAcos:
                o = ExprOp::Acos;
                break;
            case Builtin::MathAtan:
                o = ExprOp::Atan;
                break;
            case Builtin::MathAbs:
                o = ExprOp::Abs;
                break;
            case Builtin::MathRound:
                o = ExprOp::Round;
                break;
            case Builtin::MathFloor:
                o = ExprOp::Floor;
                break;
            case Builtin::MathCeil:
                o = ExprOp::Ceil;
                break;
            case Builtin::MathSqrt:
                o = ExprOp::Sqrt;
                break;
            case Builtin::MathExp:
                o = ExprOp::Exp;
                break;
            case Builtin::MathLog:
                o = ExprOp::Log;
                break;
            default:
                return fail("expr.limit", std::string(builtinName(n.builtin)) + "() has no run-time form");
        }
        return simple(n, o);
    }

    /// port(name).valueAtTime(t): the port `time - t` seconds ago (PushPortDelayed).
    bool emitPortAt(const SplitNode& n) {
        const SplitNode& t = nd(n.args[0]);
        double delay = -1;
        bool known = false;
        if (t.op == SplitOp::Sub && isTime(t.args[0])) {
            known = nd(t.args[1]).op == SplitOp::Const;
            delay = known ? nd(t.args[1]).value : -1;
            if (!emit(t.args[1])) {
                return false;
            }
        } else {
            op(ExprOp::PushTime, 0);
            if (!emit(n.args[0])) {
                return false;
            }
            op(ExprOp::Sub, 2);
        }
        if (known && delay > kMaxDelaySeconds) {
            return fail("expr.limit", "port(\"" + n.name +
                                          "\") is read with a delay longer than 4 seconds; a port's history "
                                          "is kept for 4 seconds at most");
        }
        const double seconds = known ? std::max(delay, 0.0) : kMaxDelaySeconds;
        m_out.portHistorySeconds = std::max(m_out.portHistorySeconds, static_cast<float>(seconds));
        notePort(n.name);
        op(ExprOp::PushPortDelayed, 1, 0, 0, symbol(RuntimeSymbol::Kind::Port, n.name, ExprLayerKey{}));
        return true;
    }

    /// T-781: layer(key).<row>.valueAtTime(t) (or the row under posterizeTime()) for a row this
    /// timeline does not key: the property `time - t` seconds ago (PushPropDelayed), from a history
    /// kept like a port's. The longest delay: a constant `time - c` gives c, posterizeTime(fps) adds
    /// 1 / fps (the quantized time is at most that far back); anything else the 4 s ceiling.
    bool emitPropAt(const SplitNode& n) {
        const int32_t target = n.args[0];
        const SplitNode t = nd(target);  // A copy: emit() may append nodes.
        // posterizeTime(fps)'s time with a constant positive fps: how far it lags `time`, else -1.
        auto postLag = [this](int32_t id) {
            const SplitNode& p = nd(id);
            if (p.op == SplitOp::Posterize && isTime(p.args[0]) && nd(p.args[1]).op == SplitOp::Const &&
                nd(p.args[1]).value > 0) {
                return 1.0 / nd(p.args[1]).value;
            }
            return -1.0;
        };
        double constant = -1;  // The written constant delay, when there is one.
        double bound = kMaxDelaySeconds;
        if (t.op == SplitOp::Sub && nd(t.args[1]).op == SplitOp::Const &&
            (isTime(t.args[0]) || postLag(t.args[0]) >= 0)) {
            constant = nd(t.args[1]).value;
            bound = constant + std::max(postLag(t.args[0]), 0.0);
        } else if (postLag(target) >= 0) {
            bound = postLag(target);
        }
        if (constant > kMaxDelaySeconds) {
            return fail("expr.limit", "layer(...)." + n.name +
                                          " is read with a delay longer than 4 seconds; a property's history "
                                          "is kept for 4 seconds at most");
        }
        if (t.op == SplitOp::Sub && isTime(t.args[0])) {
            if (!emit(t.args[1])) {  // time - (time - d) = d
                return false;
            }
        } else {
            op(ExprOp::PushTime, 0);
            if (!emit(target)) {
                return false;
            }
            op(ExprOp::Sub, 2);
        }
        const double seconds = std::clamp(bound, 0.0, kMaxDelaySeconds);
        m_out.portHistorySeconds = std::max(m_out.portHistorySeconds, static_cast<float>(seconds));
        op(ExprOp::PushPropDelayed, 1, 0, 0, symbol(RuntimeSymbol::Kind::LayerProp, n.name, n.key));
        return true;
    }

    /// wiggle(freq, amp[, octaves[, ampMult[, t]]]), trailing defaults left out.
    bool emitWiggle(const SplitNode& n) {
        const bool explicitTime = !isTime(n.args[4]);
        const bool halfMult = isConstValue(n.args[3], 0.5);
        const bool oneOctave = isConstValue(n.args[2], 1.0);
        int count = 2;
        if (explicitTime || !halfMult || !oneOctave) {
            count = 3;
        }
        if (explicitTime || !halfMult) {
            count = 4;
        }
        if (explicitTime) {
            count = 5;
        }
        for (int i = 0; i < count; ++i) {
            if (!emit(n.args[i])) {
                return false;
            }
        }
        op(ExprOp::Wiggle, static_cast<uint8_t>(count), 0, n.site);
        return true;
    }

    /// random(min, max) at its time; after seedRandom(n) RandomSeeded(n, min, max[, t]).
    bool emitRandom(const SplitNode& n) {
        if (!n.seeded && n.seed != exprMixSeed(m_ctx.seed, kRandomSalt)) {
            return fail("expr.limit", "random() with this seed has no run-time form");
        }
        int count = 2;
        if (n.seeded) {
            if (!konst(n.seedNumber)) {
                return false;
            }
            ++count;
        }
        if (!emit(n.args[0]) || !emit(n.args[1])) {
            return false;
        }
        if (!n.timeless && !isTime(n.args[2])) {
            if (!emit(n.args[2])) {
                return false;
            }
            ++count;
        }
        op(n.seeded ? ExprOp::RandomSeeded : ExprOp::Random, static_cast<uint8_t>(count),
           n.timeless ? 1.0f : 0.0f, n.site);
        return true;
    }

    const Program& m_prog;
    const ExprContext& m_ctx;
    const RuntimeCompileOptions& m_opt;
    RuntimeProgram& m_out;
    SplitResult m_split;
    std::map<int32_t, bool> m_hasPost;
    std::map<int32_t, int32_t> m_stripped;
    std::map<int32_t, Baked> m_baked;
    bool m_failed = false;
};

}  // namespace

uint32_t exprNoiseRef(uint32_t seed) { return exprUnhash(exprUnhash(kExprNoiseSeed) ^ seed) - 0x9e3779b9U; }

RuntimeProgram compileRuntime(const Program& program, const ExprContext& ctx,
                              const RuntimeCompileOptions& options) {
    RuntimeProgram result;
    Emitter emitter(program, ctx, options, result);
    emitter.run();
    if (result.status != ExprStatus::Ok) {
        result.status = ExprStatus::Error;
        result.ops.clear();
        result.curves.clear();
        if (result.diagnostics.empty()) {
            addExprDiagnostic(result.diagnostics, "expr.eval", "the expression cannot be compiled", 1, 1);
        }
    }
    return result;
}

}  // namespace xt::compile::expr
