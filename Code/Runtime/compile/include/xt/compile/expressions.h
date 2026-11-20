#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/timeline.h"  // Easing, EasingPoint (T-773)

/// T-767 (Docs/design/58-value-functions-and-drivers.md §58.4): what the compiler did with each
/// entry of a `<expressions>` section, for the editor's timeline panel (the warning icon of a row,
/// the read-only lowered keys). Filled into CompileResult::expressions. T-777: an entry with run-time
/// inputs becomes a program of the IR's `TEXP` chunk.

namespace xt::compile {

/// One `<expressions>` entry for one timeline it applies to (a symbol's entry once per instance).
struct ExpressionOutcome {
    /// What happened to the entry.
    enum class Status : uint8_t {
        Lowered = 0,  ///< The row's keys were replaced by `keys`.
        Off = 1,      ///< Written `off`: not lowered; the row keeps its keyframes.
        /// Reads a run-time input (58 §58.5): T-777 compiled it into a run-time program (the IR's
        /// `TEXP` chunk); the row keeps its keyframes, which the program reads as `value`. An entry
        /// with run-time inputs that cannot be compiled is Skipped (`expr.port`, `expr.limit`...).
        NeedsRuntime = 2,
        Skipped = 3,  ///< A parse, evaluation or mapping problem (`diagnostics`): skipped.
    };
    /// How one lowered key reaches the next one (expr::LoweredInterp).
    enum class Interp : uint8_t { Linear = 0, Hold = 1, Easing = 2 };
    /// One lowered key, in the row's own unit.
    struct Key {
        double time = 0;
        double value = 0;
        Interp interp = Interp::Linear;
        /// T-773: the segment to the next key in full (expr::LoweredKey::easing): Easing::linear()
        /// for Linear, Easing::hold() for Hold, the row's own easing for Easing. A LinearPoints
        /// easing's points are in `easingPoints` (its pointOffset is 0, its pointCount their number).
        Easing easing = Easing::linear();
        std::vector<EasingPoint> easingPoints;
    };

    /// The file holding the section and the 1-based line of the entry.
    std::string file;
    int line = 0;
    /// The owning `<symbol>`'s name; empty for a document-level entry.
    std::string symbol;
    /// The instance path the timeline is scoped to (a symbol entry); empty for the document.
    std::string scopePath;
    /// The entry as written: timeline name, element path (without `#`) and row (`translate.y`).
    std::string timeline;
    std::string element;
    std::string property;
    Status status = Status::Skipped;
    /// T-770: a `layer(...).<row>` this entry reads that is no row of its own timeline (another
    /// timeline's row, or a property this timeline does not key) -- a run-time input for T-777.
    struct RuntimeRef {
        std::string key;      ///< The layer() argument as evaluated: `"Logo"` (quoted) or the index.
        std::string element;  ///< The referenced element's path (a symbol instance's prefixed path).
        std::string row;      ///< The canonical row name (`translate.y`, `rotate`, `opacity`).
    };

    /// The keys that replaced the row's (Lowered only).
    std::vector<Key> keys;
    /// T-777: the expression reads run-time inputs (port(), another element's layout result or a row
    /// of another timeline, randomPerPlay(), a row with such an expression) -- also when it could not
    /// be compiled (status Skipped).
    bool runtimeInputs = false;
    /// T-777: the ports it reads (by name, as written), in first-read order.
    std::vector<std::string> ports;
    /// T-770: the references that need run time. T-777: the program reads them as PushProp, so they
    /// are no longer an `expr.ref` note.
    std::vector<RuntimeRef> runtimeRefs;
    /// The `expr.*` diagnostics of this entry (also in CompileResult::diagnostics).
    std::vector<Diagnostic> diagnostics;
};

/// T-780 (58 §58.5, "轉成關鍵幀" on a row with run-time inputs): stand-in values for the run-time
/// inputs, given through CompileOptions::expressionInputs. With them an entry with run-time inputs is
/// sampled with the compile-time evaluator (the same samples and simplification as 58 §58.3) and
/// reported Lowered (`runtimeInputs` still true) instead of becoming a run-time program -- the keys
/// the editor's "Convert to keyframes" writes. Only the editor's bake compile sets them; a missing
/// function reads 0.
struct ExpressionInputs {
    /// `port(name)` at `time` (the timeline time, or the time asked by `.valueAtTime()`).
    std::function<double(std::string_view port, double time)> port;
    /// `layer(...).left/top/width/height` of the element at `element` (its path as an entry writes it,
    /// with a symbol instance's prefix); `which`: 0 left, 1 top, 2 width, 3 height.
    std::function<double(std::string_view element, int which)> layout;
    /// `layer(...).<row>` read at play time (another timeline's row, a row whose own expression runs
    /// at play time): `row` is the canonical row name (`translate.y`, `rotate`, `opacity`).
    std::function<double(std::string_view element, std::string_view row, double time)> row;
    /// `randomPerPlay(min, max)`.
    std::function<double(double min, double max)> playRandom;
};

}  // namespace xt::compile
