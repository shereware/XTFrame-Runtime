#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "expr.h"
#include "xt/ir_pod.h"

/// T-777 (Docs/design/58-value-functions-and-drivers.md §58.5, §58.5a): an expression that reads
/// run-time inputs, compiled into one program of the IR's `TEXP` chunk. splitRuntime() expands it
/// into straight-line scalar form; every maximal sub-expression that depends on time only becomes a
/// hidden curve (sampled at kExprSampleRate and simplified within the row's tolerance, as in §58.3;
/// `time`, `value` and small arithmetic on them stay ops), and the rest is emitted as ExprOp ops --
/// any function whose arguments involve a run-time input as its run-time op. Under posterizeTime()
/// a curve is read with CurveAt at the quantized time. The inputs and rows the program reads are
/// left by name (RuntimeSymbol); the caller resolves them to PORT / DOCT / TGRP indices.
///
/// Internal to Code/Runtime/compile/src.

namespace xt::compile::expr {

/// Something a program reads that only the caller can turn into an index.
struct RuntimeSymbol {
    enum class Kind : uint8_t {
        Port,        ///< PushPort / PushPortDelayed: `name` = the port's name.
        Layout,      ///< PushLayout: `key`'s element (`imm` already holds LayoutWhich).
        LayerProp,   ///< PushProp / PushPropDelayed (T-781): `key`'s row `name` (another timeline,
                     ///< or not keyed here).
        OtherRow,    ///< RefValueAt (row form): `key`'s row `name` of this timeline.
        RuntimeRow,  ///< RefValueAt (expression form): a row of this timeline with a run-time program.
    };
    Kind kind = Kind::Port;
    std::string name;
    ExprLayerKey key;
};

/// One op; `ref` is final unless `symbol` >= 0 (then the caller fills it, and PushProp's `imm`).
struct RuntimeOp {
    ExprOp op = ExprOp::PushConst;
    uint8_t argc = 0;
    float imm = 0;
    uint32_t ref = 0;
    int32_t symbol = -1;  ///< Index into RuntimeProgram::symbols.
};

/// What compileRuntime() made.
struct RuntimeProgram {
    ExprStatus status = ExprStatus::Error;
    std::vector<RuntimeOp> ops;
    std::vector<RuntimeSymbol> symbols;
    std::vector<std::vector<IrExprKey>> curves;  ///< The hidden curves, in PushCurve / CurveAt `ref` order.
    uint16_t maxStack = 0;
    uint16_t inputMask = 0;          ///< kExprInput* bits of the ops.
    float portHistorySeconds = 0;    ///< The longest delay a port (T-781: or a property) is read with
                                     ///< (4 s when not constant).
    std::vector<std::string> ports;  ///< The distinct port names read, in first-read order.
    std::vector<Diagnostic> diagnostics;
};

/// Options of compileRuntime().
struct RuntimeCompileOptions {
    /// Tests: emit the time-only parts as ops too wherever an op can compute them (only loopOut /
    /// loopIn, loops whose count depends on time, valueVec and a wiggle whose frequency is not a
    /// constant stay curves), so the run-time evaluator is checked against the compile-time one.
    bool forceOps = false;
    /// True when `layer(key).<row>` (which the resolver answered Runtime) is a row of this timeline
    /// whose own expression runs at run time (RefValueAt in expression form); false for another
    /// timeline's row or a property this timeline does not key (PushProp). Empty: always false.
    std::function<bool(const ExprLayerKey& key, std::string_view row)> runtimeRow;
};

/// Compiles `program` for the row of `ctx` (see the file comment). Errors are `expr.limit` (more
/// than kExprMaxOps ops or a stack deeper than kExprMaxStack, a constant port or property delay
/// over 4 s) and the codes of splitRuntime() and the evaluator.
RuntimeProgram compileRuntime(const Program& program, const ExprContext& ctx,
                              const RuntimeCompileOptions& options);

/// The Noise op's `ref` for an expression of seed `seed`: the run-time evaluator seeds noise with
/// exprMixSeed(seed, ref), and this `ref` makes that kExprNoiseSeed (noise(x) is a function of x alone).
uint32_t exprNoiseRef(uint32_t seed);

}  // namespace xt::compile::expr
