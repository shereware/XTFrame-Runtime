#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// Compiler diagnostics (warnings/errors) collected while compiling a document or a Lottie file.
/// See docs/design/12-compiler.md §12.1, docs/tasks/T-14.md.

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
};

/// Accumulates diagnostics produced by a single compile/parse call.
class DiagnosticSink {
public:
    /// Records a warning at `file:line:col`.
    void warn(std::string file, std::string message, int line = 0, int col = 0) {
        m_diagnostics.push_back(
            Diagnostic{Diagnostic::Level::Warning, std::move(file), line, col, std::move(message)});
    }

    /// Records an error at `file:line:col`.
    void error(std::string file, std::string message, int line = 0, int col = 0) {
        m_diagnostics.push_back(
            Diagnostic{Diagnostic::Level::Error, std::move(file), line, col, std::move(message)});
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
};

}  // namespace xt::compile
