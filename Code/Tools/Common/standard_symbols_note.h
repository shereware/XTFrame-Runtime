#pragma once

/// T-263f item 12: a UI Project keeps the standard symbols its UIs link in its symbol directory
/// (`<root>/<shared.symbols>`, "shared/symbols" by default), and that directory's `*.xt` files are
/// not checked in: xtframe-editor (bundle::pullStandardSymbols() when the project is opened) and
/// xtframe-pack (on a first pack) copy them there from the standard library. The runtime-layer
/// tools that compile such a document without doing that -- xtframe-compile, xtframe-view -- fail
/// on the missing files; this header lets them say what to do, after their diagnostics. They
/// never write into the project themselves. Header-only (Code/Tools/Common has no library); needs
/// xt_bundle.

#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "xt/bundle/ui_project.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/utf8_path.h"

namespace xt::tools {

/// The UI Project whose directory tree holds `document`: the nearest directory at or above the
/// document's with a UI Project root file (`*.xtui`, `ui-project.json`, `bundle.xtproj`). False
/// when there is none or it cannot be read.
inline bool enclosingUiProject(const std::filesystem::path& document, xt::bundle::UiProject& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::absolute(document, ec).lexically_normal().parent_path();
    while (!dir.empty()) {
        const std::string dirUtf8 = xt::compile::genericPathToUtf8(dir);
        if (!xt::bundle::findUiProjectFile(dirUtf8).empty()) {
            xt::Result<xt::bundle::UiProject> loaded = xt::bundle::UiProject::load(dirUtf8);
            if (!loaded.ok()) {
                return false;
            }
            out = std::move(loaded.value());
            return true;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return false;
}

/// One document of `project` whose compile produced diagnostics: the directory its relative file
/// names are taken from, and the text its diagnostics' `file` starts with (empty for a plain
/// compile; "<ui id>: " for the diagnostics of bundle::pack()).
struct NoteDocument {
    std::filesystem::path dir;
    std::string filePrefix;
};

/// Prints, once per run and to stderr (where the tools print their diagnostics), the line
///   note: the project's standard symbols are not checked in; open the project in xtframe-editor
///   once, or run xtframe-pack on it, to fetch them into <directory>
/// when an error of `diagnostics` names a file that does not exist and belongs in `project`'s
/// symbol directory -- `<rootDir>/<shared.symbols>`, the directory bundle::pullStandardSymbols()
/// fills, read from the same UiProject field. Nothing is written; the caller's exit code is its
/// own. Returns whether the line was printed by this call.
inline bool noteMissingStandardSymbols(const xt::bundle::UiProject& project,
                                       std::span<const NoteDocument> documents,
                                       std::span<const xt::compile::Diagnostic> diagnostics) {
    namespace fs = std::filesystem;
    static bool printed = false;
    if (printed || project.shared.symbols.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path root = fs::absolute(xt::compile::pathFromUtf8(project.rootDir), ec).lexically_normal();
    const fs::path symbolsDir = (root / xt::compile::pathFromUtf8(project.shared.symbols)).lexically_normal();
    for (const xt::compile::Diagnostic& d : diagnostics) {
        if (d.level != xt::compile::Diagnostic::Level::Error || d.file.empty()) {
            continue;
        }
        for (const NoteDocument& document : documents) {
            std::string_view name = d.file;
            if (!document.filePrefix.empty()) {
                if (name.substr(0, document.filePrefix.size()) != document.filePrefix) {
                    continue;
                }
                name.remove_prefix(document.filePrefix.size());
            }
            const fs::path target =
                fs::absolute(document.dir / xt::compile::pathFromUtf8(name), ec).lexically_normal();
            if (target.parent_path() != symbolsDir || fs::exists(target, ec)) {
                continue;
            }
            std::fprintf(stderr,
                         "note: the project's standard symbols are not checked in; open the project in "
                         "xtframe-editor once, or run xtframe-pack on it, to fetch them into %s\n",
                         xt::compile::genericPathToUtf8(symbolsDir).c_str());
            printed = true;
            return true;
        }
    }
    return false;
}

}  // namespace xt::tools
