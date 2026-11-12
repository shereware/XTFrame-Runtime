#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/diagnostics.h"

/// T-576 (Docs/design/24-design-import.md §24.15): the diagnostics contract every tool shares --
/// one list, two readers. `xtframe-import` writes `<ui>/<ui>.diagnostics.json` and the project's
/// `diagnostics.json`; `xtframe-compile --diagnostics <file>` and `xtframe-view --diagnostics
/// <file>` write the same format. Lives in xt_compile (not xt_import) because the runtime tools
/// cannot link the editor layer; xt/import/diagnostics.h builds the importer's sink on top of it.

namespace xt::compile {

/// §24.15.2: `error` = the output differs from the board and has no conservative value (or, for
/// the importer's verify pass, a rule that did not compile and was replaced by one); `warning` = it
/// differs but a conservative value stands in; `info` = information only.
enum class Severity : uint8_t { Error, Warning, Info };

/// Where a fix is applied: on the board (an attribute to write), in the editor (an op), or nowhere.
enum class FixKind : uint8_t { None, Board, Editor };

/// `machine` = one click applies it, `maybe` = look before applying, `hint` = a suggestion only.
enum class FixApplicability : uint8_t { Hint, Maybe, Machine };

/// One string-valued member of a JSON object, in the order it is written.
using DiagnosticField = std::pair<std::string, std::string>;

/// §24.15.2 `fix`.
struct DiagnosticFix {
    FixKind kind = FixKind::None;
    FixApplicability applicability = FixApplicability::Hint;
    /// `fix.board`: the attribute to write on the element and its value (kind Board).
    std::string boardAttr;
    std::string boardValue;
    /// `fix.editor`: the Editor Service op and its string arguments (kind Editor).
    std::string editorOp;
    std::vector<DiagnosticField> editorArgs;
};

/// One item of a diagnostics file (§24.15.2).
struct ReportDiagnostic {
    /// Stable, documented code (24 appendix): `import.*`, `anchor.*`, `tag.*`, `compile.*`...
    std::string code;
    Severity severity = Severity::Warning;
    std::string message;
    /// Output id of the element the item is about, or "" for the board / the document.
    std::string element;
    /// The file `line`/`col` point into (the board, or a generated / compiled document). An
    /// addition to §24.15.2's item: the project-level list spans several files.
    std::string source;
    int line = 0;
    int col = 0;
    /// The UI id, or "" when the item is about the whole import.
    std::string ui;
    /// Machine-readable difference ("property", "from", "to"...), string values only.
    std::vector<DiagnosticField> detail;
    DiagnosticFix fix;
    /// Suppressed by `data-xt-allow` or `.xteditor.json` `diagnostics.accepted[]` (§24.15.1 point
    /// 3): still written to the file, never printed nor counted.
    bool accepted = false;
};

/// Totals over the items that are not `accepted`.
struct DiagnosticCounts {
    size_t errors = 0;
    size_t warnings = 0;
    size_t infos = 0;
    /// The accepted items, counted apart.
    size_t accepted = 0;
};

/// "error" | "warning" | "info".
const char* severityName(Severity severity);
/// "none" | "board" | "editor".
const char* fixKindName(FixKind kind);
/// "hint" | "maybe" | "machine".
const char* fixApplicabilityName(FixApplicability applicability);

/// Counts `items` by severity (accepted ones apart).
DiagnosticCounts countDiagnostics(const std::vector<ReportDiagnostic>& items);

/// The diagnostics file (§24.15.2, `version: 1`): `{"version":1,"tool":<tool>,"source":<source>,
/// "items":[...]}`, two-space indented, members in the documented order, a newline at the end.
/// `tool` is the tool's name plus its version ("xtframe-import 0.16.0", see toolWithVersion()).
std::string diagnosticsJson(const std::vector<ReportDiagnostic>& items, std::string_view tool,
                            std::string_view source);

/// "<tool> <XT_VERSION_MAJOR>.<XT_VERSION_MINOR>.<XT_VERSION_PATCH>".
std::string toolWithVersion(std::string_view tool);

/// The human-readable line (§24.15 / T-576 item 1): `<severity> <code> <source>:<line>:<col>
/// #<element>: <message>` (no ` #<element>` without an element, `<board>` without a source), plus,
/// when the item has a fix, one indented line `  fix (<applicability>, <kind>): <what>`. No
/// trailing newline.
std::string formatDiagnostic(const ReportDiagnostic& item);

/// A compiler diagnostic in the shared format: code Diagnostic::code (T-568), else `compile.error` /
/// `compile.warning`; severity
/// Error / Warning, `source` = its file, no element.
ReportDiagnostic fromCompileDiagnostic(const Diagnostic& diagnostic);

/// Writes `text` to `path` (UTF-8, binary, truncated). False when the file cannot be written.
bool writeDiagnosticsFile(std::string_view path, std::string_view text);

}  // namespace xt::compile
