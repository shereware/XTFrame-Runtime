#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "xt/compile/diagnostic_report.h"

/// T-576 (Docs/design/24-design-import.md §24.15.2): `xtframe-view --diagnostics <file>` writes
/// what the run printed as diagnostics -- compile and pack messages (`compile.error` /
/// `compile.warning`), flow continuity lines (`flow.continuity`) and, under --strict, the
/// Document's runtime warnings (`view.runtimeWarning`) -- in the shared JSON format, when the
/// process ends. stderr is unchanged. Header-only; main.cpp is the only includer.

namespace xt::tools::view {

/// Everything collected so far, in the order it was printed.
inline std::vector<xt::compile::ReportDiagnostic>& viewDiagnostics() {
    static std::vector<xt::compile::ReportDiagnostic> items;
    return items;
}

/// Adds one item that is not a compiler diagnostic.
inline void noteViewDiagnostic(const char* code, xt::compile::Severity severity, std::string message) {
    xt::compile::ReportDiagnostic item;
    item.code = code;
    item.severity = severity;
    item.message = std::move(message);
    viewDiagnostics().push_back(std::move(item));
}

/// Writes viewDiagnostics() to `path` when destroyed (a static in main.cpp: every return from
/// main and every exit() reaches it). Empty `path`: nothing.
struct ViewDiagnosticsFile {
    std::string path;
    std::string source;
    // The list is built first, so it is destroyed after this writer (statics die in reverse order).
    ViewDiagnosticsFile() { (void)viewDiagnostics(); }
    ViewDiagnosticsFile(const ViewDiagnosticsFile&) = delete;
    ViewDiagnosticsFile& operator=(const ViewDiagnosticsFile&) = delete;
    ~ViewDiagnosticsFile() {
        if (path.empty()) {
            return;
        }
        const std::string text = xt::compile::diagnosticsJson(
            viewDiagnostics(), xt::compile::toolWithVersion("xtframe-view"), source);
        if (!xt::compile::writeDiagnosticsFile(path, text)) {
            std::fprintf(stderr, "xtframe-view: cannot write %s\n", path.c_str());
        }
    }
};

}  // namespace xt::tools::view
