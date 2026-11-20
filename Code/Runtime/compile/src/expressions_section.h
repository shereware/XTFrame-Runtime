#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "expr/expr_runtime.h"
#include "xt/compile/expressions.h"

/// T-767 (Docs/design/58-value-functions-and-drivers.md §58.4, Docs/tasks/T-767.md): the
/// `<expressions>` section of a v2 `.xt` file and its lowering into `@timeline` tracks.
///
/// Grammar, one entry per line (blank lines and `//` comment lines are skipped):
///   <timeline> #<element> <row>[.x|.y|.z] [off] = <expression>
///   <timeline> #<element> <row>[.x|.y|.z] [off] = {
///     <expression lines>
///   }
/// A multi-line expression runs from the `{` to its matching `}` (brackets and string literals
/// taken into account) and the text between them is the expression.
///
/// Lowering (applyExpressions): every entry that is not `off` is evaluated by the expression
/// engine (expr/expr.h) over its timeline's duration, and the keys it produces replace that row's
/// keys in the RuleSet's TimelineDef before emitTimelineGroups() runs -- nothing downstream knows
/// about expressions. Any problem with an entry is an `expr.*` note (a warning that `--strict`
/// does not count, like DiagnosticSink::note()): the entry is skipped, the row keeps its keys and
/// the rest of the document compiles.
///
/// T-770: an entry may read another row of its own timeline with `layer("<id>").<row>` (the element
/// id, prefixed with the instance path inside a symbol, or a `<svg id>/<id>` content path) or
/// `layer(<n>).<row>` (the n-th sibling, 1-based); AE names such as `position`, `rotation`, `x`, `y`
/// map onto the row names (one table in expressions_lower.cpp). The rows are lowered in dependency
/// order; a cycle is an `expr.cycle` note on each of its entries; a row that is not keyed in this
/// timeline is an `expr.ref` note and ExpressionOutcome::runtimeRefs (status NeedsRuntime). The
/// entry's row may also be an effect parameter `effect.<index or name>.<param>[.x|.y]` or a row of a
/// shape inside an inline `<svg>` (`#<svg id>/<id>`); both need a key in the timeline.

namespace xt::compile {

class DiagnosticSink;
class IrWriter;
struct DomTree;
struct RuleSet;
struct ExpandedSymbolInstance;
struct PortScopes;
struct TimelineDef;
struct TimelineGroupBuilder;

/// One entry of a `<expressions>` section.
struct ExpressionEntry {
    std::string timeline;    ///< The `@timeline` name.
    std::string element;     ///< The element path, without its `#`.
    std::string property;    ///< The row name without its component (`translate`, `opacity`).
    std::string component;   ///< `x`, `y`, `z`, or empty.
    bool off = false;        ///< Written `off`.
    bool multiline = false;  ///< Written `= { ... }`.
    /// The expression text: the rest of the line (trimmed), or what is between the braces.
    std::string text;
    /// The `//` comment lines directly above the entry, verbatim (trimmed).
    std::vector<std::string> comments;
    /// 1-based file line and column of the entry's first character.
    int line = 0;
    int col = 0;
    /// 1-based file line and column of the expression text's first character.
    int textLine = 0;
    int textCol = 0;

    /// The row as written: `property[.component]`.
    std::string row() const { return component.empty() ? property : property + "." + component; }
};

/// Splits a section's raw `text` (whose first character is at file `line`:`col`; the columns of
/// later lines are file columns already) into entries. A line that is no entry is an `expr.entry`
/// note in `sink` (file `fileName`) and is skipped; `trailingComments` (may be null) receives the
/// comment lines after the last entry.
void parseExpressionsSection(std::string_view text, int line, int col, std::string_view fileName,
                             DiagnosticSink& sink, std::vector<ExpressionEntry>& out,
                             std::vector<std::string>* trailingComments = nullptr);

/// One `<expressions>` section to lower: its raw text and position, owning symbol and file.
struct ExpressionSource {
    std::string_view text;
    int line = 0;
    int col = 0;
    std::string symbol;  ///< The owning `<symbol>`'s name; empty for the document.
    std::string file;    ///< The file holding it, for diagnostics.
};

/// The wiggle / noise / random seed of one row (58 §58.3): FNV-1a over the timeline name, the
/// element's full path (a symbol instance's prefixed path), the row name and the component.
uint32_t expressionSeed(std::string_view timeline, std::string_view elementPath, std::string_view property,
                        std::string_view component);

/// T-777: a timeline row as the lowering resolved it: a PropId, or (prop 0xFFFF) an effect parameter
/// `effect.<effect>.<param>` matched as written.
struct RuntimeRowAddr {
    uint16_t prop = 0xFFFF;
    std::string effect;
    std::string param;
};

/// T-777 (58 §58.5): an entry compiled into a run-time program, waiting for the indices the IR
/// gives its row and inputs (emitTimelineExprs(), after the timeline groups and the ports).
struct RuntimeExpression {
    size_t outcome = 0;  ///< Its ExpressionOutcome.
    std::string file;    ///< The entry's file, line and column, and its label, for notes.
    int line = 0;
    int col = 0;
    std::string label;
    const TimelineDef* def = nullptr;  ///< Its timeline.
    uint32_t element = 0;              ///< The row's element (DomTree index == DOCT index).
    RuntimeRowAddr row;
    /// T-781: for a shape inside an inline `<svg>` (`element` is the `<svg>`), its track path
    /// `<svg id>/<inner id>` (TimelineDef::sceneRows); empty otherwise.
    std::string svgPath;
    uint32_t seed = 0;  ///< ExprContext::seed.
    float index = 1;    ///< ExprContext::index.
    expr::RuntimeProgram program;
    /// What each of program.symbols names, as the lowering resolved it.
    struct Target {
        uint32_t element = 0;           ///< Layout, LayerProp, OtherRow: the element.
        RuntimeRowAddr row;             ///< LayerProp, OtherRow: its row.
        size_t runtimeExpr = SIZE_MAX;  ///< RuntimeRow: the RuntimeExpression of that row.
    };
    std::vector<Target> targets;  ///< Parallel to program.symbols.
};

/// Options of applyExpressions().
struct ExpressionOptions {
    /// CompileOptions::expressionsAtRunTime (tests).
    bool allAtRunTime = false;
    /// CompileOptions::expressionInputs (T-780; the editor's bake compile): entries with run-time
    /// inputs are lowered with these stand-ins instead of becoming programs.
    const ExpressionInputs* inputs = nullptr;
};

/// Lowers every entry of `sources` into `ruleSet.timelines` (after symbol expansion: a symbol's
/// entries apply to each instance's copy of its timeline, element paths prefixed with the instance
/// path, `index` counted inside the instance). `outcomes` (may be null) receives one outcome per
/// entry and timeline. T-777: an entry with run-time inputs keeps its row's keys and is compiled
/// into `runtime` (null: none is compiled; such entries are skipped).
void applyExpressions(const std::vector<ExpressionSource>& sources, RuleSet& ruleSet, const DomTree& dom,
                      const std::vector<ExpandedSymbolInstance>& instances, DiagnosticSink& sink,
                      std::vector<ExpressionOutcome>* outcomes,
                      std::vector<RuntimeExpression>* runtime = nullptr,
                      const ExpressionOptions& options = {});

/// T-777: resolves the rows, ports and references of `runtime` to TGRP / PORT / DOCT indices and
/// sets the IR's `TEXP` chunk (none when nothing is left). `groupOfTimeline` maps ruleSet.timelines
/// to TGRP group indices (emitTimelineGroups()). An undeclared or non-numeric port is an `expr.port`
/// note, a RefValueAt chain deeper than kExprMaxRefDepth an `expr.limit` note; the entry is then
/// skipped (its outcome Skipped) and the row plays its keys.
void emitTimelineExprs(const std::vector<RuntimeExpression>& runtime, const RuleSet& ruleSet,
                       const DomTree& dom, const std::vector<uint32_t>& groupOfTimeline,
                       const TimelineGroupBuilder& groups, const PortScopes& ports, IrWriter& writer,
                       DiagnosticSink& sink, std::vector<ExpressionOutcome>* outcomes);

}  // namespace xt::compile
