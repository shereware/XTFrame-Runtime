#pragma once

#include <cstdint>
#include <vector>

#include "expr.h"
#include "expr_builtins.h"

/// The exact lowerings of 58 §58.3 (Docs/design/58-value-functions-and-drivers.md): a bare
/// loopOut() / loopIn() expands the row's own keys, `valueAtTime(time - d)` shifts them, an
/// expression linear in time becomes two linear keys. Internal to Code/Runtime/compile/src/expr.

namespace xt::compile::expr {

/// How far after a discontinuity the next key sits (a Hold key carries the old value up to it):
/// a loop's jump back to its first value, or a reversed Hold segment.
inline constexpr double kExprJumpSeconds = 1e-3;

/// Keys under construction, with the times where they jump (for the verification to skip).
struct KeyList {
    std::vector<LoweredKey> keys;
    std::vector<double> jumps;
    std::vector<size_t>
        jumpKeys;  ///< Keys placed kExprJumpSeconds after a jump (their segment is cut there).
    bool ok = true;
};

/// Static time-dependence of an expression (58 §58.3: "linear in time").
enum class TimeClass : uint8_t { Const = 0, Linear = 1, Other = 2 };

/// The shape exact lowerings need: top-level statements that are `var x = ...` (each variable
/// assigned exactly once) or no-ops, then one final expression statement. Fills `finalExpr` (the
/// expression of the last statement) and `prefixCount` (the statements before it).
bool simpleShape(const Program& prog, int32_t& finalExpr, int32_t& prefixCount);

/// Classifies node `id` of a simpleShape() program. `constantRow`: whether `value` is a constant.
TimeClass classifyTime(const Program& prog, int32_t id, bool constantRow);

/// The LoweredKey interpolation and easing for a row key's easing.
LoweredKey loweredFromRow(double time, double value, const Easing& e);

/// Expands loopOut / loopIn (`out` true for loopOut) of `type` over the row; unbounded in time
/// (the caller clips). `maxKeys` bounds the expansion. False when it cannot be exact (a reversed
/// linear() easing) or too many keys.
bool expandLoop(const RowSampler& row, bool out, LoopType type, int numKeyframes, double duration,
                size_t maxKeys, KeyList& list);

/// The row's keys shifted by `shift` seconds.
void shiftedKeys(const RowSampler& row, double shift, KeyList& list);

/// Clips `list` to [0, duration]: keys outside go, a segment crossing a bound is cut there with the
/// part of its easing that remains (a cubic bezier is split exactly; steps / linear() cannot be cut,
/// which makes it return false).
bool clipKeys(KeyList& list, double duration, std::span<const EasingPoint> points);

}  // namespace xt::compile::expr
