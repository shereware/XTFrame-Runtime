#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expr_ast.h"
#include "xt/compile/diagnostics.h"
#include "xt/timeline.h"

/// Timeline expressions, compile-time half (Docs/design/58-value-functions-and-drivers.md,
/// Docs/tasks/T-766.md): parse an AE-style expression written on one timeline row, evaluate it at
/// compile time, and lower it to keyframes for that row. A pure library: no file format, no IR.
///
///   ParseResult parsed = parseExpression("wiggle(2, 6)");
///   ExprContext ctx; ctx.row = {keys, {}}; ctx.duration = 15; ctx.seed = hash(...);
///   LowerResult lowered = lowerExpression(parsed.program, ctx);
///
/// Diagnostics carry the codes `expr.syntax`, `expr.unsupported`, `expr.name`, `expr.eval`,
/// `expr.limit`, `expr.recursion`, `expr.ref` and `expr.row`, with a line and column inside the
/// expression text; `Diagnostic::file` is left empty for the caller to fill.

namespace xt::compile::expr {

/// One keyframe of the row an expression sits on (one component of the property). `easing` is the
/// segment to the next key, as in xt::Keyframe (04 §4.2).
struct ExprKey {
    double time = 0;     ///< Seconds on the timeline; strictly increasing.
    double value = 0;    ///< The row's value at `time`, in the row's own unit.
    Easing easing = {};  ///< Segment to the next key (LinearPoints reads ExprRow::easingPoints).
};

/// The keyframes of one row (at least one key).
struct ExprRow {
    std::span<const ExprKey> keys;
    std::span<const EasingPoint> easingPoints;  ///< The pool a LinearPoints easing indexes.
};

/// How `layer(...)` names another element.
struct ExprLayerKey {
    bool byIndex = false;  ///< `layer(index - 1)`.
    std::string name;      ///< `layer("Logo")`.
    int index = 0;         ///< 1-based sibling index when byIndex.
};

/// What the caller's resolver knows about `layer(...).<row>` (T-770 wires it).
enum class ExprRefKind : uint8_t {
    NotFound,  ///< No such element or row: `expr.ref`.
    Row,       ///< A row of the same timeline with these keys (the resolver keeps them alive).
    Runtime,   ///< A row of another timeline: a run-time input (58 §58.5, T-777).
};

/// The resolver's answer.
struct ExprRowRef {
    ExprRefKind kind = ExprRefKind::NotFound;
    ExprRow row;
};

/// Resolves `layer(key).<prop>`.
using ExprRowResolver = std::function<ExprRowRef(const ExprLayerKey& key, std::string_view prop)>;

/// Stand-ins for the run-time inputs of 58 §58.5, for previews and tests (T-777's reference values).
/// Without them a run-time leaf makes evaluation answer NeedsRuntime.
struct ExprRuntimeInputs {
    /// `port(name)` at `time`: the timeline time for `port(name)` (not rounded by posterizeTime),
    /// the given time for `port(name).valueAtTime(t)`.
    std::function<double(std::string_view name, double time)> port;
    /// `layer(key).left/top/width/height` (`which` = LayoutWhich).
    std::function<double(const ExprLayerKey& key, int which)> layout;
    /// `layer(key).<prop>` at `time` when the resolver says Runtime.
    std::function<double(const ExprLayerKey& key, std::string_view prop, double time)> layerProp;
    /// `randomPerPlay(min, max)`: the draw of call site `site` for this play.
    std::function<double(uint32_t site, double min, double max)> playRandom;
};

/// The kind of property a row animates, which sets the lowering tolerance (58 §58.3).
enum class ExprPropertyKind : uint8_t { Position = 0, Angle = 1, Other = 2 };

/// Lowering tolerances in the row's unit: position 0.25 px, angle 0.1 degree, everything else
/// (scale, opacity...) 0.002. Indexed by ExprPropertyKind.
inline constexpr double kExprTolerance[3] = {0.25, 0.1, 0.002};

/// kExprTolerance for `kind`.
inline double exprTolerance(ExprPropertyKind kind) { return kExprTolerance[static_cast<int>(kind)]; }

/// The sampling rate of the general lowering (per second).
inline constexpr double kExprSampleRate = 60.0;
/// The most loop iterations one evaluation may run (all loops together).
inline constexpr int kExprMaxIterations = 10000;
/// The longest expression text accepted, in bytes.
inline constexpr size_t kExprMaxTextBytes = 64 * 1024;

/// Everything an expression can read besides its own text.
struct ExprContext {
    ExprRow row;                       ///< The row the expression sits on (`value`, `valueAtTime`, loops).
    double duration = 0;               ///< Timeline length in seconds (`thisComp.duration`).
    int index = 1;                     ///< 1-based sibling index (`index`).
    uint32_t seed = 0;                 ///< wiggle / noise / random seed (timeline + element + row).
    std::span<const ExprRow> vecRows;  ///< The property's component rows, for `valueVec` (x, y[, z]).
    int component = 0;                 ///< Which element of an array result this row takes (X 0, Y 1, Z 2).
    double tolerance = kExprTolerance[2];              ///< Lowering tolerance (exprTolerance()).
    ExprRowResolver otherRows;                         ///< `layer(...).<row>` (T-770); empty: `expr.ref`.
    const ExprRuntimeInputs* runtimeInputs = nullptr;  ///< Stand-ins for run-time leaves (may be null).
};

/// The result of parseExpression().
struct ParseResult {
    Program program;
    std::vector<Diagnostic> diagnostics;
    /// True when the text parsed and resolved without an error.
    bool ok() const { return program.root >= 0; }
};

/// Parses and resolves one expression (58 §58.2). Never throws (except std::bad_alloc).
ParseResult parseExpression(std::string_view text);

/// A value an expression produced.
struct ExprValue {
    int size = 0;          ///< 0 = no value, 1 = a number, 2..4 = an array of that length.
    bool isArray = false;  ///< True for an array (even of length 1).
    double v[4] = {0, 0, 0, 0};
};

/// Outcome of an evaluation or a lowering.
enum class ExprStatus : uint8_t {
    Ok = 0,
    NeedsRuntime,  ///< The expression reads a run-time input (58 §58.5): not an error.
    Error,         ///< See the diagnostics.
};

/// The result of evaluateExpression().
struct EvalResult {
    ExprStatus status = ExprStatus::Error;
    ExprValue value;
    std::vector<Diagnostic> diagnostics;
    /// T-781: whether the run called posterizeTime(), and its rate (lowerExpression()'s Hold-key
    /// condition, for the bake with stand-ins).
    bool posterized = false;
    double posterizeFps = 0;
};

/// Evaluates `program` at timeline time `time` (the result is the last expression statement run,
/// 58 §58.2). posterizeTime() rounds `time` for what follows it, as in AE.
EvalResult evaluateExpression(const Program& program, const ExprContext& ctx, double time);

/// The number the row gets from an evaluation: the number itself, or element `component` of an
/// array. False (with a diagnostic) when there is none.
bool componentOf(const ExprValue& value, int component, double& out, std::vector<Diagnostic>& diags);

/// How a lowered key reaches the next one.
enum class LoweredInterp : uint8_t {
    Linear = 0,  ///< Straight line.
    Hold = 1,    ///< Keeps this value until the next key.
    Easing = 2,  ///< LoweredKey::easing, copied from the row's own key (exact lowerings only).
};

/// One key the lowering produced. `easing` is meaningful when interp == Easing (a LinearPoints
/// easing indexes the row's ExprRow::easingPoints).
struct LoweredKey {
    double time = 0;
    double value = 0;
    LoweredInterp interp = LoweredInterp::Linear;
    Easing easing = {};
};

/// Which lowering produced the keys (58 §58.3's table).
enum class LowerMethod : uint8_t {
    None = 0,
    ExactLoop,    ///< A bare loopOut() / loopIn(): the row's keys repeated.
    ExactShift,   ///< valueAtTime(time - d): the row's keys shifted by d.
    ExactLinear,  ///< Linear in time: two linear keys.
    Sampled,      ///< Sampled at kExprSampleRate and simplified within the tolerance.
    Posterized,   ///< posterizeTime(fps): one Hold key every 1 / fps.
};

/// The result of lowerExpression().
struct LowerResult {
    ExprStatus status = ExprStatus::Error;
    LowerMethod method = LowerMethod::None;
    std::vector<LoweredKey> keys;  ///< Times in [0, duration], strictly increasing; empty unless Ok.
    std::vector<Diagnostic> diagnostics;
    size_t samples = 0;  ///< Evaluations made (0 for an exact lowering past the checks).
};

/// Lowers `program` for the row in `ctx` over [0, ctx.duration] (58 §58.3): the exact cases
/// first, else sampling plus simplification within ctx.tolerance, or Hold keys under
/// posterizeTime(). NeedsRuntime (no keys, no diagnostic) when the program has a run-time leaf;
/// Error (no keys) on any evaluation error -- the caller keeps the row's own keys.
LowerResult lowerExpression(const Program& program, const ExprContext& ctx);

}  // namespace xt::compile::expr
