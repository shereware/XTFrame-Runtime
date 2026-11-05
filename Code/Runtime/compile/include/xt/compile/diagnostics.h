#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// Compiler diagnostics (warnings/errors) collected while compiling a document or a Lottie file.
/// See Docs/design/12-compiler.md §12.1, Docs/tasks/T-14.md.

namespace xt::compile {

/// One warning or error raised while compiling or parsing a source file.
struct Diagnostic {
    /// Severity level values.
    enum class Level : uint8_t { Warning, Error };
    /// Severity; `Error` means the compile ultimately fails (see `CompileResult::ok()`).
    Level level;
    /// Path of the source file the diagnostic refers to.
    std::string file;
    /// 1-based source line, or 0 if not applicable.
    int line = 0;
    /// 1-based source column, or 0 if not applicable.
    int col = 0;
    /// Human-readable description of the problem.
    std::string message;
    /// T-568 (24 §24.15.4): a stable diagnostics code (`bind.*`, `compile.*`...) when the message
    /// has one of its own; "" files it under `compile.error` / `compile.warning`.
    std::string code = {};
    /// T-587 (54 §54.13): what to change to cure the diagnostic, as one sentence; "" when the
    /// code has none. fromCompileDiagnostic() writes it as the `fix` detail.
    std::string fix = {};
};

/// Accumulates diagnostics produced by a single compile/parse call.
class DiagnosticSink {
public:
    /// Records a warning at `file:line:col`; `code` (T-568) is Diagnostic::code, `fix` (T-587)
    /// Diagnostic::fix.
    void warn(std::string file, std::string message, int line = 0, int col = 0, std::string code = {},
              std::string fix = {}) {
        m_diagnostics.push_back(Diagnostic{Diagnostic::Level::Warning, std::move(file), line, col,
                                           std::move(message), std::move(code), std::move(fix)});
    }

    /// Records an error at `file:line:col`; `code` (T-568) is Diagnostic::code, `fix` (T-587)
    /// Diagnostic::fix.
    void error(std::string file, std::string message, int line = 0, int col = 0, std::string code = {},
               std::string fix = {}) {
        m_diagnostics.push_back(Diagnostic{Diagnostic::Level::Error, std::move(file), line, col,
                                           std::move(message), std::move(code), std::move(fix)});
    }

    /// T-408 (43 §43.6): whether deprecated() records anything (CompileOptions::strict or
    /// CompileOptions::reportDeprecated).
    void setReportDeprecated(bool on) { m_reportDeprecated = on; }

    /// T-408: a use of a registered alias (xt/compile/aliases.h): recorded as a warning when
    /// setReportDeprecated(true), else dropped. Counted by deprecatedCount(), which a strict
    /// compile subtracts: an alias still compiles.
    void deprecated(std::string file, std::string message, int line = 0, int col = 0) {
        if (m_reportDeprecated) {
            warn(std::move(file), std::move(message), line, col);
            ++m_deprecatedCount;
        }
    }

    /// T-408: the warnings deprecated() recorded.
    size_t deprecatedCount() const { return m_deprecatedCount; }
    /// T-594 (54 §54.14 S4): whether the `web.*` checks run (CompileOptions::webCompat).
    void setWebCompat(bool on) { m_webCompat = on; }
    bool webCompat() const { return m_webCompat; }
    /// T-594: a `web.*` warning (`code`, `fix`), recorded only when setWebCompat(true). Counted by
    /// webCount(), which a strict compile subtracts like deprecatedCount(): the advice never fails it.
    void web(std::string file, std::string message, int line, int col, std::string code, std::string fix) {
        if (m_webCompat) {
            warn(std::move(file), std::move(message), line, col, std::move(code), std::move(fix));
            ++m_webCount;
        }
    }
    /// T-594: the warnings web() recorded.
    size_t webCount() const { return m_webCount; }

    /// Returns every diagnostic recorded so far, in the order they were raised.
    const std::vector<Diagnostic>& all() const { return m_diagnostics; }

    /// Returns true if at least one error-level diagnostic was recorded.
    bool hasErrors() const {
        for (const Diagnostic& d : m_diagnostics) {
            if (d.level == Diagnostic::Level::Error) {
                return true;
            }
        }
        return false;
    }

    /// Returns the number of warning-level diagnostics recorded.
    size_t warningCount() const {
        size_t count = 0;
        for (const Diagnostic& d : m_diagnostics) {
            if (d.level == Diagnostic::Level::Warning) {
                ++count;
            }
        }
        return count;
    }

private:
    std::vector<Diagnostic> m_diagnostics;
    bool m_reportDeprecated = false;
    size_t m_deprecatedCount = 0;
    bool m_webCompat = false;
    size_t m_webCount = 0;
};

}  // namespace xt::compile
