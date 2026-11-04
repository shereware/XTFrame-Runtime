#include "xt/shell/source_watcher.h"

#include <cstdio>
#include <string_view>
#include <system_error>
#include <utility>

#include "xt/compile/utf8_path.h"
#include "xt/shell/app.h"
#include "xt/types.h"

namespace fs = std::filesystem;

namespace xt::shell {

namespace {

/// The source extensions a watched directory contributes (T-595 item 1: the UI file, its
/// stylesheets, its symbol libraries, its `.xtflow`).
bool isSourceFile(const fs::path& path) {
    const fs::path ext = path.extension();
    return ext == ".xt" || ext == ".xts" || ext == ".xtcl" || ext == ".xtflow";
}

/// Adds `path` to `out`: a file as is, a directory's source files recursively. A path that is
/// missing (or vanishes while it is walked) adds nothing, so its return is a change too.
void addToSnapshot(const fs::path& path, std::map<fs::path, fs::file_time_type>& out) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        const fs::file_time_type time = fs::last_write_time(path, ec);
        if (!ec) {
            out[path] = time;
        }
        return;
    }
    if (!fs::is_directory(path, ec)) {
        return;
    }
    fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || !isSourceFile(it->path())) {
            continue;
        }
        const fs::file_time_type time = fs::last_write_time(it->path(), fileEc);
        if (!fileEc) {
            out[it->path()] = time;
        }
    }
}

}  // namespace

std::string formatCompileDiagnostic(const compile::Diagnostic& diagnostic) {
    // Same text as xtframe-compile's printDiagnostic() (Code/Tools/compile/main.cpp).
    const char* level = diagnostic.level == compile::Diagnostic::Level::Error ? "error" : "warning";
    const std::string file = diagnostic.file.empty() ? std::string("<input>") : diagnostic.file;
    if (diagnostic.line == 0 && diagnostic.col == 0) {
        return file + ": " + level + ": " + diagnostic.message;
    }
    return file + ":" + std::to_string(diagnostic.line) + ":" + std::to_string(diagnostic.col) + ": " +
           level + ": " + diagnostic.message;
}

void SourceWatcher::watch(std::string uiPath, DocumentHandle handle, compile::CompileOptions options) {
    m_uiPath = std::move(uiPath);
    m_handle = handle;
    m_options = std::move(options);
    if (m_options.rootDir.empty()) {
        m_options.rootDir = compile::pathToUtf8(compile::pathFromUtf8(m_uiPath).parent_path());
    }
    m_active = true;
    m_lastPoll = std::chrono::steady_clock::now();
    m_snapshot = takeSnapshot();
}

void SourceWatcher::addPath(std::string path) { m_extraPaths.push_back(compile::pathFromUtf8(path)); }

void SourceWatcher::stop() {
    m_active = false;
    m_snapshot.clear();
}

SourceWatcher::Snapshot SourceWatcher::takeSnapshot() const {
    Snapshot snapshot;
    const fs::path ui = compile::pathFromUtf8(m_uiPath);
    addToSnapshot(ui, snapshot);
    addToSnapshot(ui.has_parent_path() ? ui.parent_path() : fs::path("."), snapshot);
    const fs::path root = compile::pathFromUtf8(m_options.rootDir);
    for (const std::vector<std::string>* list :
         {&m_options.impliedStylesheets, &m_options.impliedTokenFiles}) {
        for (const std::string& file : *list) {
            const fs::path path = compile::pathFromUtf8(file);
            addToSnapshot(path.is_absolute() ? path : root / path, snapshot);
        }
    }
    for (const fs::path& path : m_extraPaths) {
        addToSnapshot(path, snapshot);
    }
    return snapshot;
}

SourceWatcher::PollResult SourceWatcher::poll(Context& context, std::chrono::steady_clock::time_point now) {
    if (!m_active || now - m_lastPoll < m_interval) {
        return PollResult::Unchanged;
    }
    m_lastPoll = now;
    return pollNow(context);
}

SourceWatcher::PollResult SourceWatcher::pollNow(Context& context) {
    if (!m_active) {
        return PollResult::Unchanged;
    }
    Snapshot snapshot = takeSnapshot();
    if (snapshot == m_snapshot) {
        return PollResult::Unchanged;
    }
    // Remembered before the compile: a failed one is retried on the next change, not every poll.
    m_snapshot = std::move(snapshot);
    return reload(context);
}

SourceWatcher::PollResult SourceWatcher::reload(Context& context) {
    compile::CompileResult result = compile::compileDocument(m_uiPath, m_options);
    for (const compile::Diagnostic& diagnostic : result.diagnostics) {
        emit(formatCompileDiagnostic(diagnostic));
    }
    if (!result.ok()) {
        return PollResult::Failed;  // The diagnostics above say why; the previous Document stays.
    }
    // takeOwnership: the new Document copies the bytes (Document::load), so `result` may go.
    BundleDelta delta;
    delta.ir.push_back(
        IrReload{m_handle, IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true});
    HotReloadReport report;
    const Status status = context.hotReload(delta, &report);
    if (status != Status::kOk && status != Status::kIncompatible) {
        emit(m_uiPath + ": error: hot reload failed (" + statusName(status) +
             "); keeping the previous version");
        return PollResult::Failed;
    }
    if (m_reloaded) {
        m_reloaded(m_handle);
    }
    return PollResult::Reloaded;
}

void SourceWatcher::emit(const std::string& line) const {
    if (m_sink) {
        m_sink(line);
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

void installWatch(App& app, SourceWatcher& watcher) {
    app.setFramePoll([&watcher](App& self) {
        if (watcher.poll(self.mainWindow().context()) == SourceWatcher::PollResult::Reloaded) {
            self.syncViewport();
        }
    });
}

}  // namespace xt::shell
