#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "bundle_json.h"
#include "symbol_internal.h"
#include "xt/bundle/symbol_library.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/utf8_path.h"

/// T-498b / T-741 (Docs/design/49-symbol-sets.md 49.3 ruling, 56-symbol-identity-style-scope.md
/// 56.2, 56.3 item 7): which symbol groups the project's UIs link -- the implied default layer pack()
/// and the Editor Service apply, and the groups a UI mixes. A namespaced copy
/// (`<shared.symbols>/<ns>/<Name>.xt`) belongs to a group only by its recorded source, never by its
/// file name. Moved out of symbol_library.cpp (file length).

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using detail::catchAll;
using detail::json;

namespace {

/// T-741: the id of the library the group directory `groupDir` sits in (`<library>/groups/<id>`,
/// its `library.json` `id`); empty when it sits in none.
std::string libraryIdOfGroupDir(const fs::path& groupDir) {
    const fs::path normal = groupDir.lexically_normal();
    fs::path dir = normal.has_filename() ? normal : normal.parent_path();
    if (pathToUtf8(dir.parent_path().filename()) != "groups") {
        return std::string();
    }
    std::string text;
    if (!detail::readFile(genericPathToUtf8(dir.parent_path().parent_path() / "library.json"), text)) {
        return std::string();
    }
    std::string error;
    const json library = detail::parseBounded(text, detail::kMaxJsonDepth, error);
    return error.empty() && library.is_object() ? detail::getString(library, "id") : std::string();
}

/// T-741: the symbol hrefs of the UI document `doc` (relative to `rootDir`); false when it cannot
/// be read or parsed (the compile reports it).
bool symbolHrefsOf(const std::string& rootDir, const std::string& doc, std::vector<std::string>& hrefs,
                   fs::path& docFile) {
    std::string text;
    compile::DiagnosticSink sink;
    compile::DomTree dom;
    docFile = pathFromUtf8(rootDir) / pathFromUtf8(doc);
    if (!detail::readFile(genericPathToUtf8(docFile), text) || !compile::parseXt(text, doc, sink, dom)) {
        return false;
    }
    hrefs = dom.symbolHrefs;
    return true;
}

/// T-741 (56.3 item 0): where a symbol href of `docFile` points: 1 directly in `symbolsAbs` (the
/// project's symbol directory, absolute and normalised; `qualified` = the file stem), 2 in one of
/// its namespace directories (`qualified` = `<ns>/<stem>`), 0 anywhere else.
int symbolFilePlace(const fs::path& docFile, const std::string& href, const fs::path& symbolsAbs,
                    std::string& qualified) {
    qualified.clear();
    if (href.empty() || href.find(':') != std::string::npos) {
        return 0;
    }
    std::error_code ec;
    const fs::path file = fs::absolute(docFile.parent_path() / pathFromUtf8(href), ec).lexically_normal();
    const std::string stem = pathToUtf8(file.stem());
    if (file.parent_path() == symbolsAbs) {
        qualified = stem;
        return detail::isPlainSymbolName(stem) ? 1 : 0;
    }
    if (file.parent_path().parent_path() == symbolsAbs) {
        qualified = pathToUtf8(file.parent_path().filename()) + "/" + stem;
        return detail::isQualifiedSymbolName(qualified) ? 2 : 0;
    }
    return 0;
}

/// T-741: the project `readCopySource()` reads `<symbolsDir>/index.json` of.
UiProject indexProbe(const std::string& rootDir, const std::string& symbolsDir) {
    UiProject probe;
    probe.rootDir = rootDir;
    probe.shared.symbols = symbolsDir.empty() ? std::string("shared/symbols") : symbolsDir;
    return probe;
}

fs::path symbolsAbsOf(const UiProject& probe) {
    std::error_code ec;
    return fs::absolute(pathFromUtf8(probe.rootDir) / pathFromUtf8(probe.shared.symbols), ec)
        .lexically_normal();
}

}  // namespace

bool detectImpliedGroup(const std::string& rootDir, const std::vector<ProjectUi>& uis,
                        const std::string& standardSymbolDir, SymbolGroup& out,
                        const std::string& symbolsDir) {
    if (standardSymbolDir.empty()) {
        return false;
    }
    const fs::path groupDir = pathFromUtf8(standardSymbolDir);
    if (loadGroup(groupDir, out) != Status::kOk) {
        return false;  // Not a group directory (a bare symbol folder): no token layer to imply.
    }
    bool found = false;
    catchAll([&]() {
        const UiProject probe = indexProbe(rootDir, symbolsDir);
        const fs::path symbolsAbs = symbolsAbsOf(probe);
        const std::string standardLibrary = libraryIdOfGroupDir(groupDir);
        for (const ProjectUi& ui : uis) {
            std::vector<std::string> hrefs;
            fs::path docFile;
            if (!symbolHrefsOf(rootDir, ui.doc, hrefs, docFile)) {
                continue;  // The compile reports it.
            }
            for (const std::string& href : hrefs) {
                if (href.rfind("xt-standard:", 0) == 0) {
                    found = true;
                    return Status::kOk;
                }
                std::string qualified;
                if (symbolFilePlace(docFile, href, symbolsAbs, qualified) == 2) {
                    // T-741: a namespaced copy is the standard group's only by its recorded source;
                    // its file name says nothing (survey-2026/Button.xt is not broadsheet's Button).
                    CopySource source;
                    if (readCopySource(probe, qualified, source) && source.group == out.id &&
                        (standardLibrary.empty() || source.library == standardLibrary)) {
                        found = true;
                        return Status::kOk;
                    }
                    continue;
                }
                const fs::path file = pathFromUtf8(href).filename();
                if (!file.empty() && !detail::findStandardSymbol(groupDir, file).empty()) {
                    found = true;
                    return Status::kOk;
                }
            }
        }
        return Status::kOk;
    });
    return found;
}

std::vector<std::string> linkedSymbolGroups(const std::string& rootDir, const std::string& doc,
                                            const std::string& standardSymbolDir,
                                            const std::string& symbolsDir) {
    std::vector<std::string> out;
    catchAll([&]() {
        std::vector<std::string> hrefs;
        fs::path docFile;
        if (!symbolHrefsOf(rootDir, doc, hrefs, docFile)) {
            return Status::kOk;
        }
        SymbolGroup standard;
        const bool haveStandard =
            !standardSymbolDir.empty() && loadGroup(pathFromUtf8(standardSymbolDir), standard) == Status::kOk;
        const std::string standardLibrary =
            haveStandard ? libraryIdOfGroupDir(pathFromUtf8(standardSymbolDir)) : std::string();
        const std::string standardLabel =
            standardLibrary.empty() ? standard.id : standardLibrary + "/" + standard.id;
        const UiProject probe = indexProbe(rootDir, symbolsDir);
        const fs::path symbolsAbs = symbolsAbsOf(probe);
        auto add = [&](const std::string& label) {
            if (!label.empty() && std::find(out.begin(), out.end(), label) == out.end()) {
                out.push_back(label);
            }
        };
        for (const std::string& href : hrefs) {
            if (href.rfind("xt-standard:", 0) == 0) {
                if (haveStandard) {
                    add(standardLabel);
                }
                continue;
            }
            std::string qualified;
            const int place = symbolFilePlace(docFile, href, symbolsAbs, qualified);
            CopySource source;
            if (place != 0 && readCopySource(probe, qualified, source)) {
                add(source.library + "/" + source.group);
                continue;
            }
            const fs::path file = pathFromUtf8(href).filename();
            if (place != 2 && haveStandard && !file.empty() &&
                !detail::findStandardSymbol(pathFromUtf8(standardSymbolDir), file).empty()) {
                add(standardLabel);
            }
        }
        return Status::kOk;
    });
    return out;
}

}  // namespace xt::bundle
