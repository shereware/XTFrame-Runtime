#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "expr_builtins.h"
#include "expr_eval.h"
#include "expr_split.h"

/// Shared by expr_split.cpp (constant folding while expanding) and expr_split_eval.cpp (evaluating
/// the expanded form): the value of one SplitNode from its arguments' values.

namespace xt::compile::expr {

/// What computing a SplitNode can read.
struct SplitEnv {
    SplitEnv(const Program& p, const ExprContext& c, const RowSampler& r, const std::vector<RowSampler>& v)
        : prog(p), ctx(c), row(r), vec(v) {}

    const Program& prog;
    const ExprContext& ctx;
    const RowSampler& row;
    const std::vector<RowSampler>& vec;
    Evaluator* evaluator = nullptr;      ///< For SubProgram (null while folding: SubProgram never folds).
    double time = 0;                     ///< Time of the evaluation.
    ExprStatus status = ExprStatus::Ok;  ///< NeedsRuntime / Error raised by a leaf.
    /// OtherRowAt samplers by node id, built on first use.
    std::vector<std::pair<int32_t, std::unique_ptr<RowSampler>>> others;
};

/// The value of node `id` (`n`) given its arguments' values `a`. A run-time leaf reads
/// ctx.runtimeInputs (missing: env.status = NeedsRuntime). Arithmetic errors give NaN / infinity,
/// which the caller reports if they reach a result (a Select ignores its unselected branch).
double computeSplitNode(const SplitNode& n, int32_t id, const double* a, SplitEnv& env);

/// True for the run-time leaves.
bool isRuntimeLeaf(SplitOp op);

}  // namespace xt::compile::expr
