#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"

/// T-595 (Docs/design/54-source-format-v2.md §54.14 S5, 13 §13.8.3 `hotReload`): the `--watch`
/// hot reload of xt_shell. T-598: built as its own target, xt_shell_watch (xt_shell + xt_compile),
/// so xt_shell itself never links the compiler; link xt_shell_watch to use this header. A
/// SourceWatcher polls the modification times of a UI's source files
/// with std::filesystem (no platform file-change API; xt_core is untouched), recompiles the UI
/// with xt_compile when one of them changed, and on success replaces the Document through
/// Context::hotReload() -- port values and state carry over as hotReload() defines. A failed
/// compile prints its diagnostics in xtframe-compile's format and keeps the previous Document.

namespace xt::shell {

class App;

/// One diagnostic as xtframe-compile prints it: `file:line:col: level: message`, or
/// `file: level: message` without a position (`<input>` for an empty file name). No newline.
std::string formatCompileDiagnostic(const compile::Diagnostic& diagnostic);

/// Polls a UI's source files and hot-reloads its Document when they change. See the file comment.
class SourceWatcher {
public:
    /// What one poll did.
    enum class PollResult : uint8_t {
        /// Not watching, the interval has not passed yet, or no watched file changed.
        Unchanged,
        /// The UI was recompiled and its Document replaced (state carried over, or started fresh
        /// when hotReload() reported kIncompatible).
        Reloaded,
        /// A watched file changed but the compile or the load failed; the previous Document stays.
        Failed,
    };

    /// The polling interval of poll() (Docs/tasks/T-595.md: 500 ms).
    static constexpr std::chrono::milliseconds kDefaultInterval{500};

    SourceWatcher() = default;

    /// Starts watching `uiPath` (a `.xt` file, UTF-8), loaded as `handle`, recompiled with
    /// `options` (CompileOptions::rootDir empty: the file's directory). The watched set is every
    /// `.xt`, `.xts`, `.xtcl` and `.xtflow` file under the UI file's directory (recursively) --
    /// the UI file, its linked stylesheets, its symbol libraries and its flow graph when they sit
    /// next to it --, `options.impliedStylesheets` and `options.impliedTokenFiles`, and every
    /// path added with addPath(). Takes the first snapshot now, so only later changes reload.
    void watch(std::string uiPath, DocumentHandle handle, compile::CompileOptions options);
    /// Also watches `path` (UTF-8): a file, or a directory whose source files (the extensions of
    /// watch()) are watched recursively -- e.g. a shared symbol library directory outside the UI's
    /// own. Takes effect from the next snapshot.
    void addPath(std::string path);
    /// Stops watching (poll() then returns Unchanged).
    void stop();
    /// True between watch() and stop().
    bool active() const { return m_active; }
    /// The Document handle being reloaded (the one given to watch()).
    DocumentHandle handle() const { return m_handle; }

    /// The minimum time between two snapshots in poll(); kDefaultInterval until set.
    void setInterval(std::chrono::milliseconds interval) { m_interval = interval; }
    /// Receives every line the watcher prints (compile diagnostics, a failed load). An empty
    /// function (the default) writes each line to stderr.
    void setDiagnosticSink(std::function<void(const std::string& line)> sink) { m_sink = std::move(sink); }
    /// Called after every successful reload with the handle, so the host re-resolves what the
    /// reload invalidated (Document*, ElementRef, listeners; 13 §13.8.3).
    void setReloadedHandler(std::function<void(DocumentHandle)> handler) { m_reloaded = std::move(handler); }

    /// Takes a snapshot when at least the interval has passed since the previous one (`now`), and
    /// reloads when it differs (a file added, removed or with another modification time).
    PollResult poll(Context& context, std::chrono::steady_clock::time_point now);
    /// poll() with steady_clock::now().
    PollResult poll(Context& context) { return poll(context, std::chrono::steady_clock::now()); }
    /// Snapshot and compare now, ignoring the interval.
    PollResult pollNow(Context& context);

private:
    using Snapshot = std::map<std::filesystem::path, std::filesystem::file_time_type>;
    Snapshot takeSnapshot() const;
    PollResult reload(Context& context);
    void emit(const std::string& line) const;

    bool m_active = false;
    std::string m_uiPath;
    DocumentHandle m_handle = 0;
    compile::CompileOptions m_options;
    std::vector<std::filesystem::path> m_extraPaths;
    std::chrono::milliseconds m_interval = kDefaultInterval;
    std::chrono::steady_clock::time_point m_lastPoll{};
    Snapshot m_snapshot;
    std::function<void(const std::string&)> m_sink;
    std::function<void(DocumentHandle)> m_reloaded;
};

/// T-598: hot reload for an xt_shell App. Sets App::setFramePoll() to "poll `watcher` against the
/// main window's Context; after a reload, App::syncViewport()" -- once per frame, after the SDL
/// events and before `onFrame`. Call watcher.watch(...) after loading the UI (before or after this
/// call). `watcher` must outlive the hook: keep it alive until the App is destroyed or
/// app.setFramePoll({}) removed it. Replaces any frame poll set before.
void installWatch(App& app, SourceWatcher& watcher);

}  // namespace xt::shell
