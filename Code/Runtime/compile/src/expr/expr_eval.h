#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "expr.h"
#include "expr_builtins.h"

/// The compile-time evaluator of timeline expressions (a tree walker over expr_ast.h). Internal to
/// Code/Runtime/compile/src/expr; evaluateExpression() / lowerExpression() are its public faces.
/// Docs/design/58-value-functions-and-drivers.md §58.2-58.3.

namespace xt::compile::expr {

/// The deepest evaluator recursion (expression nesting plus local function calls).
inline constexpr int kMaxEvalDepth = 600;

/// Kinds of an evaluator value.
enum class VKind : uint8_t {
    Undefined,  ///< No value (a declared but unset variable, posterizeTime()'s result...).
    Number,
    Array,   ///< `len` numbers.
    Poison,  ///< Derived from a run-time leaf in poison mode (splitRuntime's sub-programs).
};

/// An evaluator value.
struct Val {
    VKind kind = VKind::Undefined;
    uint8_t len = 0;
    double v[4] = {0, 0, 0, 0};

    static Val number(double x) {
        Val r;
        r.kind = VKind::Number;
        r.len = 1;
        r.v[0] = x;
        return r;
    }
    static Val poison() {
        Val r;
        r.kind = VKind::Poison;
        return r;
    }
};

/// The operand shape of linear / ease / easeIn / easeOut / clamp (`b`) over `count` evaluated
/// arguments `vals` (the evaluator's Val or splitRuntime's SVal: anything with `kind` and `len`):
/// `firstVec` is the first element-wise operand (clamp(v, a, b): all three; linear(t, [tMin, tMax,]
/// v1, v2): only v1, v2), `len` / `isArray` the common array length. Returns the diagnostic message
/// when the arrays differ in length or a time argument is not a number, else an empty string.
template <class V>
std::string mapOperandShape(Builtin b, const V* vals, int count, int& firstVec, int& len, bool& isArray) {
    firstVec = b == Builtin::Clamp ? 0 : count - 2;
    len = 1;
    isArray = false;
    for (int i = firstVec; i < count; ++i) {
        if (vals[i].kind == VKind::Array) {
            if (isArray && vals[i].len != len) {
                return std::string(builtinName(b)) + "() needs arrays of the same length";
            }
            isArray = true;
            len = vals[i].len;
        }
    }
    for (int i = 0; i < firstVec; ++i) {
        if (vals[i].kind != VKind::Number) {
            return std::string(builtinName(b)) + "()'s time arguments must be numbers";
        }
    }
    return std::string();
}

/// Evaluates one program for one row; reusable across sample times (each run starts afresh).
class Evaluator {
public:
    /// `row` samples ctx.row; `vec` samples ctx.vecRows (may be empty). All must outlive this.
    Evaluator(const Program& prog, const ExprContext& ctx, const RowSampler& row,
              const std::vector<RowSampler>& vec, std::vector<Diagnostic>& diags);
    ~Evaluator();
    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;

    /// Runs the whole program at timeline time `t`; `result` is the last expression statement's value.
    ExprStatus run(double t, Val& result);

    /// Poison mode (run-time leaves yield Poison instead of stopping): runs the first `stmtCount`
    /// top-level statements at `t` and reads component `comp` of global `slot`. Error when the value
    /// is Poison or missing.
    ExprStatus runPrefix(double t, int32_t stmtCount, int32_t slot, int comp, double& out);
    /// runPrefix's value itself (`slot` -1 = the result so far); Poison is returned as such.
    ExprStatus runPrefixValue(double t, int32_t stmtCount, int32_t slot, Val& out);

    /// Runs the first `stmtCount` top-level statements at `t`, then evaluates node `node` (the exact
    /// lowerings read the argument of a final valueAtTime() this way).
    ExprStatus evalAfterPrefix(double t, int32_t stmtCount, int32_t node, Val& out);

    /// Whether the last run called posterizeTime(), and its rate.
    bool posterized() const { return m_posterized; }
    double posterizeFps() const { return m_postFps; }

    /// Records diagnostics only for the first error of this evaluator (lowering runs many samples).
    bool hadError() const { return m_reported; }

    /// T-777: the wiggle() call sites whose frequency comes from a run-time input (splitRuntime's
    /// SplitNode::aperiodic): not periodic over the duration, as at run time.
    void setAperiodicWiggles(std::vector<uint32_t> sites) { m_aperiodicWiggles = std::move(sites); }

private:
    enum class Flow : uint8_t { Normal, Break, Continue, Return, Stop };

    struct DepthGuard;
    struct RefSampler;

    void reset(double t);
    Flow exec(int32_t id);
    Flow execLoop(const Node& n);
    bool eval(int32_t id, Val& out);
    bool evalNumber(int32_t id, double& out, const char* what);
    bool evalBinary(const Node& n, BinaryOp op, const Val& a, const Val& b, Val& out);
    bool evalCall(const Node& n, Val& out);
    bool evalMath(const Node& n, Builtin b, Val& out);
    bool evalRandom(const Node& n, Val& out);
    bool evalMap(const Node& n, Builtin b, Val& out);
    bool evalLocalCall(const Node& n, Val& out);
    bool evalLayer(const Node& n, Val& out);
    bool runtimeLeaf(const Node& n, Val& out);
    bool readTarget(const Node& target, Val& out);
    bool writeTarget(const Node& target, const Val& v);
    bool truthy(const Node& at, const Val& v, bool& out);
    bool layerKey(const Node& keyNode, ExprLayerKey& key);
    void poisonAssignments(int32_t id);
    double effTime() const;
    Val& slotRef(const Node& varNode);
    bool finite(const Node& at, Val& v);
    bool fail(const Node& at, const char* code, std::string msg);
    double randomDraw();

    const Program& m_prog;
    const ExprContext& m_ctx;
    const RowSampler& m_row;
    const std::vector<RowSampler>& m_vec;
    std::vector<Diagnostic>& m_diags;

    std::vector<Val> m_slots;  ///< Globals, then the frames of the active local calls.
    size_t m_frameBase = 0;
    int m_callDepth = 0;
    int m_depth = 0;
    int m_iterations = 0;
    double m_time = 0;
    double m_postFps = -1;
    bool m_posterized = false;
    uint32_t m_randSeed = 0;
    uint32_t m_randCalls = 0;
    bool m_timeless = false;
    bool m_poisonMode = false;
    bool m_framePoison = false;
    Val m_last;
    Val m_ret;
    ExprStatus m_status = ExprStatus::Ok;
    bool m_reported = false;
    std::vector<std::unique_ptr<RefSampler>> m_refs;  ///< T-770 rows, built on first use.
    std::vector<uint32_t> m_aperiodicWiggles;         ///< T-777: setAperiodicWiggles().
};

}  // namespace xt::compile::expr
