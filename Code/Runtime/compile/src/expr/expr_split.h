#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "expr.h"

/// splitRuntime(): the preparation T-777 needs to compile an expression that reads run-time inputs
/// (Docs/design/58-value-functions-and-drivers.md §58.5): the program is expanded at compile time
/// into straight-line scalar form -- loops unrolled, local functions inlined, arrays split into
/// components, an `if` whose condition is not a compile-time constant turned into a selection with
/// both branches expanded -- and every maximal sub-expression that depends on time but on no
/// run-time input is pulled out as a "curve" (T-777 bakes each into hidden keys). What remains per
/// component is a small expression over constants, curves and run-time leaves.
///
/// Internal to Code/Runtime/compile/src/expr; T-766 implements and tests it, T-777 consumes it.

namespace xt::compile::expr {

/// The most operations the remainder of one component may have (one run-time program, 58 §58.5).
inline constexpr size_t kSplitMaxOps = 256;
/// The most nodes the expansion may build before it gives up.
inline constexpr size_t kSplitMaxNodes = 100000;

/// Operations of the expanded form. Arguments are SplitNode::args[0 .. argc).
enum class SplitOp : uint8_t {
    Const,       ///< `value`.
    Time,        ///< Timeline time.
    CurveRef,    ///< `ref` = SplitResult::curves index (only in remainders).
    SubProgram,  ///< Time-only loop result: global `slot` (-1 = the result so far), element `comp`,
                 ///< after the first `ref` top-level statements (run with the compile-time evaluator).
    ValueAt,     ///< This row at args[0] (`value` is ValueAt(Time)).
    VecAt,       ///< Component `comp` of the property's rows (valueVec) at args[0].
    OtherRowAt,  ///< `layer(key).<prop>` of the same timeline at args[0] (T-770).
    LoopOut,     ///< loopOut(`loopType`, `value` keyframes) at args[0].
    LoopIn,      ///< loopIn(...) at args[0].
    Wiggle,      ///< row(args[4]) + args[1] × noise: (freq, amp, octaves, ampMult, t); seed site `site`.
    Noise,       ///< noise(args[0]); seed site `site`.
    Random,      ///< args[0] + (args[1] - args[0]) × draw at time args[2]; `seed`, call `site`, `timeless`.
    Posterize,   ///< floor(args[0] × args[1] + 1e-9) / args[1] (0 when args[1] is 0).
    Neg,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    Select,  ///< args[0] != 0 ? args[1] : args[2].
    Math1,   ///< `builtin`(args[0]) (Math.sin ... degreesToRadians).
    Math2,   ///< `builtin`(args[0], args[1]) (Math.atan2, Math.pow).
    Min,
    Max,
    Clamp,   ///< clamp(args[0], args[1], args[2]).
    Linear,  ///< linear(t, tMin, tMax, v1, v2).
    Ease,    ///< ease / easeIn / easeOut (`builtin`) with linear's arguments.
    // Run-time leaves (58 §58.5).
    Port,        ///< port(`name`) now (no argument; a stand-in input is read at the evaluation time).
    PortAt,      ///< port(`name`) at args[0].
    Layout,      ///< layer(key).left/top/width/height (`comp` = LayoutWhich).
    LayerProp,   ///< layer(key).<prop> of another timeline at args[0].
    PlayRandom,  ///< randomPerPlay(args[0], args[1]), call site `site`.
};

/// One node of the expanded form.
struct SplitNode {
    SplitOp op = SplitOp::Const;
    uint8_t argc = 0;
    int32_t args[5] = {-1, -1, -1, -1, -1};
    double value = 0;                 ///< Const value; LoopOut / LoopIn number of keyframes.
    Builtin builtin = Builtin::None;  ///< Math1 / Math2 / Ease.
    uint8_t loopType = 0;             ///< LoopType of LoopOut / LoopIn.
    bool timeless = false;            ///< Random: drawn after seedRandom(n, true).
    bool seeded = false;              ///< T-777 Random: after seedRandom(`seedNumber`).
    double seedNumber = 0;            ///< T-777 Random: seedRandom's number.
    /// T-777 Wiggle: not periodic over the duration -- a run-time wiggle whose frequency is not a
    /// constant (the run-time evaluator's rule: only a constant frequency repeats, 58 §58.5).
    bool aperiodic = false;
    uint32_t site = 0;     ///< Wiggle / Noise / PlayRandom call site; Random call number.
    uint32_t seed = 0;     ///< Random: the random seed in force.
    int32_t ref = -1;      ///< CurveRef: curve index; SubProgram: statement count.
    int32_t slot = -1;     ///< SubProgram: global slot.
    int32_t comp = 0;      ///< SubProgram / VecAt: element; Layout: LayoutWhich.
    std::string name;      ///< Port / PortAt: port name; layer nodes: property name.
    ExprLayerKey key;      ///< OtherRowAt / LayerProp / Layout.
    bool runtime = false;  ///< Depends on a run-time leaf.
    bool timeDep = false;  ///< Depends on time.
};

/// A maximal time-only sub-expression: node `root` of SplitResult::nodes (its subtree has no
/// run-time leaf and no CurveRef).
struct SplitCurve {
    int32_t root = -1;
};

/// The result of splitRuntime().
struct SplitResult {
    ExprStatus status = ExprStatus::Error;
    std::vector<SplitNode> nodes;      ///< The expanded form (children before parents).
    std::vector<SplitCurve> curves;    ///< The time-only sub-expressions.
    std::vector<int32_t> expanded;     ///< Per result component: the root before curves were pulled out.
    std::vector<int32_t> components;   ///< Per result component: the remainder's root.
    std::vector<size_t> remainderOps;  ///< Per component: operations in the remainder (tree size).
    bool isArray = false;              ///< The result is an array (`components.size()` elements).
    std::vector<Diagnostic> diagnostics;
};

/// Expands `program` and splits it (see the file comment). `ctx` supplies index, duration, seed and
/// the resolver for layer(...). Errors (`expr.limit` and the evaluator's codes): a loop whose count
/// is not known at compile time while it reads a run-time input, more than kSplitMaxOps operations
/// left in a component, break / continue / return under a condition that is not a compile-time
/// constant, a variable or the result set in only one branch of such a condition.
SplitResult splitRuntime(const Program& program, const ExprContext& ctx);

/// Evaluates component `component` of `split`'s remainder at `t` (curves evaluated from their
/// sub-expressions, run-time leaves from ctx.runtimeInputs): for tests and previews.
ExprStatus evalSplitComponent(const Program& program, const SplitResult& split, int component,
                              const ExprContext& ctx, double t, double& out, std::vector<Diagnostic>& diags);

/// Evaluates curve `curve` of `split` at `t` (what T-777 samples to bake it).
ExprStatus evalSplitCurve(const Program& program, const SplitResult& split, size_t curve,
                          const ExprContext& ctx, double t, double& out, std::vector<Diagnostic>& diags);

}  // namespace xt::compile::expr
