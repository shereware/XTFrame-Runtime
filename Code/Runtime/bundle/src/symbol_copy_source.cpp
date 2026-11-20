#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bundle_json.h"
#include "symbol_internal.h"
#include "xt/bundle/symbol_library.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/sha256.h"

/// T-739 (Docs/design/56-symbol-identity-style-scope.md 56.1 item 4, 56.3 item 6; Docs/tasks/T-739.md
/// A, B, C): a project copy is compared with its recorded source only -- the library and group the
/// project's `<shared.symbols>/index.json` records, never "any library with that name" -- and a
/// drag-in that meets a name the project already has says whether it is the same symbol or another
/// one. The update itself stays in symbol_library.cpp (updateCopy(), it shares the copy rule).

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using detail::catchAll;
using detail::fileNameOf;
using detail::isPlainSymbolName;
using detail::json;
using detail::lfText;
using detail::resolveInside;

namespace {

constexpr const char* kCopyIndexFile = "index.json";

std::string symbolsRelOfCopy(const UiProject& project) { return detail::projectSymbolsRel(project); }

/// `sha256:<hex>` of `text` with CRLF read as LF: the form `index.json` records.
std::string contentShaCopy(const std::string& text) { return "sha256:" + util::sha256Hex(lfText(text)); }

/// T-741 (56.3 item 6): the key of a project `index.json` entry -- its `qualified` name, else its
/// `name` (an entry written before T-741 is a flat copy).
std::string indexEntryKey(const json& entry) {
    const std::string qualified = detail::getString(entry, "qualified");
    return qualified.empty() ? detail::getString(entry, "name") : qualified;
}

bool readCopySourceImpl(const UiProject& project, std::string_view name, CopySource& out) {
    out = CopySource();
    if (project.rootDir.empty() || !detail::isQualifiedSymbolName(name)) {
        return false;
    }
    fs::path indexPath;
    if (!resolveInside(pathFromUtf8(project.rootDir), symbolsRelOfCopy(project) + "/" + kCopyIndexFile,
                       indexPath)) {
        return false;
    }
    std::string text;
    if (!detail::readFile(genericPathToUtf8(indexPath), text)) {
        return false;
    }
    std::string error;
    const json index = detail::parseBounded(text, detail::kMaxJsonDepth, error);
    if (!error.empty() || !index.is_object() || !index.contains("symbols") || !index["symbols"].is_array()) {
        return false;
    }
    for (const json& entry : index["symbols"]) {
        if (!entry.is_object() || indexEntryKey(entry) != name) {
            continue;
        }
        if (!entry.contains("source") || !entry["source"].is_object()) {
            return false;  // An entry the notes editor wrote: the project's own symbol.
        }
        const json& source = entry["source"];
        CopySource found;
        found.library = detail::getString(source, "library");
        found.group = detail::getString(source, "group");
        found.set = detail::getString(source, "set");
        found.setVersion = detail::getString(source, "setVersion");
        found.sha256 = detail::getString(source, "sha256");
        found.librarySha256 = detail::getString(source, "librarySha256");
        if (found.library.empty() || found.group.empty()) {
            return false;
        }
        out = std::move(found);
        return true;
    }
    return false;
}

CopyStatus copyStatusImpl(const LibraryMounts& mounts, const UiProject& project, std::string_view name) {
    CopyStatus status;
    std::string ns;
    std::string bare;
    if (project.rootDir.empty() || !detail::splitQualifiedName(name, ns, bare)) {
        return status;
    }
    fs::path copyPath;
    std::string mine;
    if (!resolveInside(pathFromUtf8(project.rootDir),
                       symbolsRelOfCopy(project) + "/" + std::string(name) + ".xt", copyPath) ||
        !detail::readFile(genericPathToUtf8(copyPath), mine)) {
        return status;  // NoCopy
    }
    status.recorded = readCopySourceImpl(project, name, status.source);
    if (!status.recorded) {
        status.state = CopyState::NoSource;
        return status;
    }
    status.entry = findSymbolIn(mounts, status.source.library, status.source.group, bare, &status.library,
                                &status.group);
    fs::path libraryFile;
    std::string theirs;
    if (status.entry == nullptr || status.group == nullptr || status.group->dir.empty() ||
        !resolveInside(pathFromUtf8(status.group->dir), status.entry->file, libraryFile) ||
        !detail::readFile(genericPathToUtf8(libraryFile), theirs)) {
        status.state = CopyState::SourceMissing;
        return status;
    }
    const std::string projectSha = contentShaCopy(mine);
    const std::string librarySha = contentShaCopy(theirs);
    if (status.source.sha256.empty()) {
        // A record without a hash: as before T-739 -- the same text is current, anything else stale
        // (updateCopy() then writes the hash).
        status.state = projectSha == librarySha ? CopyState::UpToDate : CopyState::Stale;
        return status;
    }
    const std::string& recorded =
        status.source.librarySha256.empty() ? status.source.sha256 : status.source.librarySha256;
    const bool edited = projectSha != status.source.sha256;
    if (librarySha == recorded) {
        status.state = edited ? CopyState::LocallyModified : CopyState::UpToDate;
    } else {
        status.state = edited ? CopyState::StaleAndModified : CopyState::Stale;
    }
    return status;
}

}  // namespace

namespace {

/// What is at one candidate place of a symbol (useTargetOf()).
struct PlaceState {
    fs::path file;
    bool exists = false;
    /// The project's copy there is this library's group's symbol (recorded source).
    bool same = false;
    CopySource source;
};

PlaceState placeState(const UiProject& project, const fs::path& file, const std::string& qualified,
                      const SymbolLibrary& library, const SymbolGroup& group) {
    PlaceState state;
    state.file = file;
    std::error_code ec;
    state.exists = fs::exists(file, ec);
    if (state.exists && readCopySourceImpl(project, qualified, state.source)) {
        state.same = state.source.library == library.id && state.source.group == group.id;
    }
    return state;
}

/// The `<use symbol="X">` values of the symbol file `text` (`file` for messages only), in document
/// order, without repeats.
std::vector<std::string> usedSymbolNames(const std::string& text, const std::string& file) {
    std::vector<std::string> out;
    compile::DiagnosticSink sink;
    compile::DomTree tree;
    if (!compile::parseXtSymbolLibrary(text, file, sink, tree)) {
        return out;
    }
    for (const compile::DomNode& node : tree.nodes) {
        if (node.tag != "use") {
            continue;
        }
        const compile::DomAttr* symbol = node.attr("symbol");
        if (symbol != nullptr && !symbol->value.empty() &&
            std::find(out.begin(), out.end(), symbol->value) == out.end()) {
            out.push_back(symbol->value);
        }
    }
    return out;
}

}  // namespace

namespace detail {

bool useTargetOf(const UiProject& project, const fs::path& root, const SymbolLibrary& library,
                 const SymbolGroup& group, const SymbolEntry& entry, bool namespaced, fs::path& target,
                 UseTarget& out) {
    out = UseTarget();
    fs::path symbolsDir;
    if (!resolveInside(root, symbolsRelOfCopy(project), symbolsDir)) {
        return false;
    }
    const std::string fileName = fileNameOf(entry.file);
    const std::string stem = pathToUtf8(fs::path(fileName).stem());
    fs::path flatFile;
    fs::path nsFile;
    if (!isPlainSymbolName(stem) || !isGroupId(group.id) || !resolveInside(symbolsDir, fileName, flatFile) ||
        !resolveInside(symbolsDir, group.id + "/" + fileName, nsFile)) {
        return false;
    }
    const std::string nsName = group.id + "/" + stem;
    const PlaceState flat = placeState(project, flatFile, stem, library, group);
    const PlaceState inNs = placeState(project, nsFile, nsName, library, group);
    auto take = [&](const PlaceState& place, const std::string& qualified, UseClash clash,
                    const CopySource* existing) {
        target = place.file;
        out.path = relativeText(root, place.file);
        out.clash = clash;
        out.qualified = clash == UseClash::NameTaken ? std::string() : qualified;
        if (existing != nullptr) {
            out.existingLibrary = existing->library;
            out.existingGroup = existing->group;
        }
        return true;
    };
    if (!namespaced) {
        if (flat.same) {
            return take(flat, stem, UseClash::Same, &flat.source);
        }
        if (inNs.same) {
            return take(inNs, nsName, UseClash::Same, &inNs.source);  // 56.3: already in its namespace
        }
        if (!flat.exists) {
            return take(flat, stem, UseClash::None, nullptr);  // Flat, as before 56.3.
        }
    }
    if (inNs.same) {
        return take(inNs, nsName, UseClash::Same, &inNs.source);
    }
    if (inNs.exists) {
        return take(inNs, nsName, UseClash::NameTaken, &inNs.source);  // No third place (one level).
    }
    // Placed: the occupant of the flat name is reported (none when the flat name is free and the
    // symbol is here only because of a dependency, or it is this very symbol flat).
    return take(inNs, nsName, UseClash::Placed, flat.exists && !flat.same ? &flat.source : nullptr);
}

void planCopies(const UiProject& project, const fs::path& root, const SymbolLibrary& library,
                const SymbolGroup& group, const SymbolEntry& entry, std::vector<PlannedCopy>& out) {
    out.clear();
    // The closure, breadth first: `uses[i]` are the indices (into `out`) symbol i names.
    std::vector<std::vector<size_t>> uses;
    auto indexOf = [&](const SymbolEntry* e) {
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].entry == e) {
                return i;
            }
        }
        return out.size();
    };
    {
        PlannedCopy first;
        first.entry = &entry;
        if (!useTargetOf(project, root, library, group, entry, false, first.target, first.place)) {
            return;
        }
        out.push_back(std::move(first));
        uses.emplace_back();
    }
    const fs::path groupDir = pathFromUtf8(group.dir);
    for (size_t i = 0; i < out.size(); ++i) {
        fs::path file;
        std::string text;
        if (group.dir.empty() || !resolveInside(groupDir, out[i].entry->file, file) ||
            !detail::readFile(genericPathToUtf8(file), text)) {
            continue;
        }
        for (const std::string& used : usedSymbolNames(text, out[i].entry->file)) {
            if (used == out[i].entry->name) {
                continue;
            }
            const SymbolEntry* dep = nullptr;
            for (const SymbolEntry& candidate : group.symbols) {
                if (candidate.name == used) {
                    dep = &candidate;
                    break;
                }
            }
            if (dep == nullptr) {
                continue;  // Another group's symbol, a qualified name, or a broken reference.
            }
            size_t at = indexOf(dep);
            if (at == out.size()) {
                PlannedCopy copy;
                copy.entry = dep;
                copy.dependency = true;
                if (!useTargetOf(project, root, library, group, *dep, false, copy.target, copy.place)) {
                    continue;
                }
                out.push_back(std::move(copy));
                uses.emplace_back();
            }
            if (std::find(uses[i].begin(), uses[i].end(), at) == uses[i].end()) {
                uses[i].push_back(at);
            }
        }
    }
    // Dependencies travel together: a symbol using one in the namespace goes there too.
    auto inNamespace = [](const PlannedCopy& c) {
        return c.place.clash == UseClash::NameTaken || c.place.qualified.find('/') != std::string::npos;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < out.size(); ++i) {
            if (inNamespace(out[i])) {
                continue;
            }
            for (const size_t j : uses[i]) {
                if (!inNamespace(out[j])) {
                    continue;
                }
                PlannedCopy& moved = out[i];
                if (useTargetOf(project, root, library, group, *moved.entry, true, moved.target,
                                moved.place)) {
                    const std::string fileName = fileNameOf(out[j].entry->file);
                    moved.place.because = group.id + "/" + pathToUtf8(fs::path(fileName).stem());
                }
                changed = true;
                break;
            }
        }
    }
}

}  // namespace detail

bool readCopySource(const UiProject& project, std::string_view name, CopySource& out) {
    bool found = false;
    catchAll([&]() {
        found = readCopySourceImpl(project, name, out);
        return Status::kOk;
    });
    if (!found) {
        out = CopySource();
    }
    return found;
}

const SymbolEntry* findSymbolIn(const LibraryMounts& mounts, std::string_view libraryId,
                                std::string_view groupId, std::string_view name,
                                const SymbolLibrary** libraryOut, const SymbolGroup** groupOut) {
    if (libraryId.empty() || groupId.empty()) {
        return nullptr;
    }
    for (const SymbolLibrary& library : mounts.libraries) {
        if (library.id != libraryId) {
            continue;
        }
        const SymbolGroup* group = detail::groupOf(library, groupId);
        if (group == nullptr) {
            return nullptr;
        }
        for (const SymbolEntry& entry : group->symbols) {
            if (entry.name == name) {
                if (libraryOut != nullptr) {
                    *libraryOut = &library;
                }
                if (groupOut != nullptr) {
                    *groupOut = group;
                }
                return &entry;
            }
        }
        return nullptr;  // The first library of that id decides, as findSymbol()'s mount order does.
    }
    return nullptr;
}

const char* copyStateName(CopyState state) {
    switch (state) {
        case CopyState::NoCopy:
            return "noCopy";
        case CopyState::NoSource:
            return "noSource";
        case CopyState::SourceMissing:
            return "sourceMissing";
        case CopyState::UpToDate:
            return "upToDate";
        case CopyState::LocallyModified:
            return "locallyModified";
        case CopyState::Stale:
            return "stale";
        case CopyState::StaleAndModified:
            return "staleAndModified";
    }
    return "noCopy";
}

CopyStatus copyStatus(const LibraryMounts& mounts, const UiProject& project, std::string_view name) {
    CopyStatus status;
    const Status ran = catchAll([&]() {
        status = copyStatusImpl(mounts, project, name);
        return Status::kOk;
    });
    if (ran != Status::kOk) {
        status = CopyStatus();
    }
    return status;
}

bool isCopyStale(const LibraryMounts& mounts, const UiProject& project, std::string_view name) {
    const CopyState state = copyStatus(mounts, project, name).state;
    return state == CopyState::Stale || state == CopyState::StaleAndModified;
}

const char* useClashName(UseClash clash) {
    switch (clash) {
        case UseClash::None:
            return "none";
        case UseClash::Same:
            return "same";
        case UseClash::NameTaken:
            return "nameTaken";
        case UseClash::Placed:
            return "placed";
    }
    return "none";
}

std::string existingSourceText(const UseTarget& target) {
    return target.existingLibrary.empty() || target.existingGroup.empty()
               ? std::string("project")
               : target.existingLibrary + "/" + target.existingGroup;
}

UseTarget planUse(const LibraryMounts& mounts, const UiProject& project, std::string_view name,
                  std::string_view groupId, bool* found) {
    UseTarget out;
    bool have = false;
    catchAll([&]() {
        if (project.rootDir.empty() || !isPlainSymbolName(name)) {
            return Status::kOk;
        }
        const SymbolLibrary* library = nullptr;
        const SymbolGroup* group = nullptr;
        const SymbolEntry* entry = findSymbol(mounts, name, groupId, &library, &group);
        if (entry == nullptr || library == nullptr || group == nullptr || group->dir.empty()) {
            return Status::kOk;
        }
        have = true;
        std::error_code ec;
        const fs::path root = fs::absolute(pathFromUtf8(project.rootDir), ec).lexically_normal();
        std::vector<detail::PlannedCopy>
            plan;  // T-741: the decision useSymbols() makes, dependencies included.
        detail::planCopies(project, root, *library, *group, *entry, plan);
        if (!plan.empty()) {
            out = plan.front().place;
        }
        return Status::kOk;
    });
    if (found != nullptr) {
        *found = have;
    }
    return out;
}

}  // namespace xt::bundle
