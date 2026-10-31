#include "xt/bundle/symbol_library.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "symbol_internal.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/sha256.h"

/// T-498 (Docs/design/49-symbol-sets.md): symbol libraries, see the header. The readers follow
/// ui_project.cpp: bounded JSON parse, type-checked accessors (bundle_json.h), paths from a file
/// joined under their directory only after resolveInside() said they stay there, and every public
/// entry point wrapped by catchAll() so no exception leaves the bundle layer (14 §14.3, §14.9).

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::Diagnostic;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using detail::groupOf;
using detail::isPlainSymbolName;
using detail::json;
using detail::nowSeconds;
using detail::relativeText;

namespace {

constexpr int kLibraryVersion = 1;
constexpr const char* kLibraryFile = "library.json";
constexpr const char* kGroupFile = "group.json";
constexpr const char* kIndexFile = "index.json";
constexpr const char* kGroupsDir = "groups";
constexpr const char* kTrashDir = ".trash";
constexpr const char* kUserLibraryId = "mine";
constexpr const char* kUserLibraryName = "My symbols";
constexpr const char* kMountsFile = "symbol-libraries.json";
constexpr const char* kProjectLibraryDir = "shared/symbol-library";
constexpr const char* kProjectIndexFile = "index.json";
constexpr int kMountsVersion = 1;
constexpr int kProjectIndexVersion = 1;

// T-499: shared with symbol_lint.cpp and symbol_set.cpp (symbol_internal.h).
using detail::catchAll;
using detail::fileNameOf;
using detail::isGroupId;
using detail::lfText;
using detail::resolveInside;
using detail::trimmed;

void problem(SymbolGroup& group, const std::string& file, std::string message,
             Diagnostic::Level level = Diagnostic::Level::Error) {
    Diagnostic d;
    d.level = level;
    d.file = file;
    d.message = std::move(message);
    group.problems.push_back(std::move(d));
}

/// `<dir>/<name>` parsed as a JSON object. `error` names what went wrong (empty when the file is
/// missing: `missing` is set instead).
json readJsonObject(const fs::path& dir, const char* name, bool& missing, std::string& error) {
    missing = false;
    error.clear();
    std::string text;
    if (!detail::readFile(genericPathToUtf8(dir / name), text)) {
        missing = true;
        return json(json::value_t::discarded);
    }
    std::string depthError;
    json root = detail::parseBounded(text, detail::kMaxJsonDepth, depthError);
    if (!depthError.empty()) {
        error = depthError;
        return json(json::value_t::discarded);
    }
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return json(json::value_t::discarded);
    }
    return root;
}

/// `library.json`'s `default`, else its first group id; empty when neither is a group id.
std::string defaultGroupId(const json& library) {
    std::string id = detail::getString(library, "default");
    if (id.empty()) {
        const std::vector<std::string> groups = detail::getStringArray(library, "groups");
        if (!groups.empty()) {
            id = groups.front();
        }
    }
    return isGroupId(id) ? id : std::string();
}

/// A relative path member of `obj`, accepted only inside `root`; empty (and a problem) otherwise.
std::string pathMember(SymbolGroup& group, const fs::path& root, const json& obj, const char* key,
                       const std::string& file, bool required) {
    const std::string value = detail::getString(obj, key);
    if (value.empty()) {
        if (required) {
            problem(group, file, std::string("missing '") + key + "'");
        }
        return std::string();
    }
    fs::path resolved;
    if (!resolveInside(root, value, resolved)) {
        problem(group, file, std::string("'") + key + "' leaves the group directory: " + value);
        return std::string();
    }
    return value;
}

/// The string members of the array `obj[key]` that name files inside `root`.
std::vector<std::string> pathList(SymbolGroup& group, const fs::path& root, const json& obj, const char* key,
                                  const std::string& file) {
    std::vector<std::string> out;
    for (const std::string& value : detail::getStringArray(obj, key)) {
        fs::path resolved;
        if (!resolveInside(root, value, resolved)) {
            problem(group, file, std::string("'") + key + "' entry leaves the group directory: " + value);
            continue;
        }
        out.push_back(value);
    }
    return out;
}

void readIndexEntry(SymbolGroup& group, const fs::path& root, const json& entry, const std::string& file) {
    SymbolEntry symbol;
    symbol.name = detail::getString(entry, "name");
    if (symbol.name.empty()) {
        problem(group, file, "a symbol entry has no 'name'");
        return;
    }
    symbol.file = pathMember(group, root, entry, "file", file, true);
    if (symbol.file.empty()) {
        return;
    }
    symbol.thumbnail = pathMember(group, root, entry, "thumbnail", file, false);
    symbol.description = detail::getString(entry, "description");
    symbol.category = detail::getString(entry, "category");
    symbol.content = detail::getString(entry, "content");
    if (const auto contract = entry.find("requires"); contract != entry.end() && contract->is_object()) {
        symbol.requiredStates = detail::getStringArray(*contract, "states");
        symbol.requiredSlots = detail::getStringArray(*contract, "slots");
    }
    if (const auto ports = entry.find("ports"); ports != entry.end() && ports->is_array()) {
        for (const json& port : *ports) {
            SymbolPortInfo info{detail::getString(port, "name"), detail::getString(port, "type"),
                                detail::getString(port, "dir")};
            if (!info.name.empty()) {
                symbol.ports.push_back(std::move(info));
            }
        }
    }
    symbol.timelines = detail::getStringArray(entry, "timelines");
    if (const auto tokens = entry.find("tokens"); tokens != entry.end() && tokens->is_array()) {
        for (const json& token : *tokens) {
            TokenDefault def;
            if (token.is_string()) {
                def.name = token.get<std::string>();  // The pre-T-498 spelling: a bare name.
                if (def.name.rfind("--", 0) != 0) {
                    def.name = "--" + def.name;
                }
            } else {
                def.name = detail::getString(token, "name");
                def.value = detail::getString(token, "default");
            }
            if (def.name.size() > 2 && def.name.rfind("--", 0) == 0) {
                symbol.tokens.push_back(std::move(def));
            }
        }
    }
    symbol.fonts = pathList(group, root, entry, "fonts", file);
    symbol.textures = pathList(group, root, entry, "textures", file);
    group.symbols.push_back(std::move(symbol));
}

/// Reads `group.json` and `index.json` of `group.dir` (already an existing directory).
void loadGroupFiles(SymbolGroup& group) {
    const fs::path root = pathFromUtf8(group.dir);
    group.name = group.id;
    bool missing = false;
    std::string error;
    const json meta = readJsonObject(root, kGroupFile, missing, error);
    if (missing) {
        problem(group, kGroupFile, "missing");
    } else if (!error.empty()) {
        problem(group, kGroupFile, error);
    } else {
        const std::string id = detail::getString(meta, "id");
        if (id.empty()) {
            problem(group, kGroupFile, "missing 'id'");
        } else if (id != group.id) {
            problem(group, kGroupFile, "'id' is '" + id + "', the directory is '" + group.id + "'",
                    Diagnostic::Level::Warning);
        }
        const std::string name = detail::getString(meta, "name");
        if (!name.empty()) {
            group.name = name;
        }
        group.description = detail::getString(meta, "description");
        if (const auto source = meta.find("source"); source != meta.end() && source->is_object()) {
            group.source.kind = detail::getString(*source, "kind");
            group.source.set = detail::getString(*source, "set");
            group.source.setVersion = detail::getString(*source, "setVersion");
        }
        group.tokensFile = pathMember(group, root, meta, "tokens", kGroupFile, false);
        group.themeFiles = pathList(group, root, meta, "themes", kGroupFile);
        group.thumbnail = pathMember(group, root, meta, "thumbnail", kGroupFile, false);
    }
    const json index = readJsonObject(root, kIndexFile, missing, error);
    if (missing) {
        problem(group, kIndexFile, "missing");
        return;
    }
    if (!error.empty()) {
        problem(group, kIndexFile, error);
        return;
    }
    if (group.tokensFile.empty()) {
        // group.json named no usable token file: the index names it too (`"tokens": "tokens.xts"`).
        group.tokensFile = pathMember(group, root, index, "tokens", kIndexFile, false);
    }
    const auto symbols = index.find("symbols");
    if (symbols == index.end() || !symbols->is_array()) {
        problem(group, kIndexFile, "missing 'symbols' array");
        return;
    }
    for (const json& entry : *symbols) {
        readIndexEntry(group, root, entry, kIndexFile);
    }
}

Status loadLibraryImpl(const fs::path& dir, SymbolLibrary& out) {
    if (dir.empty()) {
        return Status::kInvalidArgument;
    }
    bool missing = false;
    std::string error;
    const json root = readJsonObject(dir, kLibraryFile, missing, error);
    if (missing) {
        return Status::kNotFound;
    }
    if (!error.empty()) {
        return Status::kInvalidArgument;
    }
    const float version = detail::getFloat(root, "version", static_cast<float>(kLibraryVersion));
    if (version > static_cast<float>(kLibraryVersion)) {
        return Status::kUnsupported;
    }
    out.id = detail::getString(root, "id");
    if (!detail::isValidId(out.id)) {
        out.id.clear();
        return Status::kInvalidArgument;
    }
    out.name = detail::getString(root, "name");
    if (out.name.empty()) {
        out.name = out.id;
    }
    out.dir = genericPathToUtf8(dir);
    while (out.dir.size() > 1 && out.dir.back() == '/') {
        out.dir.pop_back();
    }
    out.readOnly = detail::getBool(root, "readOnly", false);
    out.defaultGroup = defaultGroupId(root);
    // T-501: the persisted lint state per group (`problems: { "<id>": { errors, warnings } }`).
    const auto problems = root.find("problems");
    std::error_code ec;
    for (const std::string& id : detail::getStringArray(root, "groups")) {
        SymbolGroup group;
        group.id = id;
        group.name = id;
        if (problems != root.end() && problems->is_object()) {
            if (const auto it = problems->find(id); it != problems->end() && it->is_object()) {
                group.lint.errors =
                    static_cast<size_t>(std::max(0.0f, detail::getFloat(*it, "errors", 0.0f)));
                group.lint.warnings =
                    static_cast<size_t>(std::max(0.0f, detail::getFloat(*it, "warnings", 0.0f)));
            }
        }
        if (!isGroupId(id)) {
            problem(group, kLibraryFile, "'" + id + "' is not a group id");
            out.groups.push_back(std::move(group));
            continue;
        }
        const fs::path groupDir = dir / kGroupsDir / pathFromUtf8(id);
        if (!fs::is_directory(groupDir, ec)) {
            problem(group, kLibraryFile, "no directory groups/" + id);
            out.groups.push_back(std::move(group));
            continue;
        }
        group.dir = genericPathToUtf8(groupDir);
        loadGroupFiles(group);
        out.groups.push_back(std::move(group));
    }
    return Status::kOk;
}

Status saveLibraryJsonImpl(const SymbolLibrary& library) {
    if (library.readOnly) {
        return Status::kUnsupported;
    }
    if (library.dir.empty() || !detail::isValidId(library.id)) {
        return Status::kInvalidArgument;
    }
    nlohmann::ordered_json root;
    root["version"] = kLibraryVersion;
    root["id"] = library.id;
    root["name"] = library.name.empty() ? library.id : library.name;
    root["readOnly"] = false;
    if (!library.defaultGroup.empty()) {
        root["default"] = library.defaultGroup;
    }
    nlohmann::ordered_json groups = nlohmann::ordered_json::array();
    for (const SymbolGroup& group : library.groups) {
        groups.push_back(group.id);
    }
    root["groups"] = std::move(groups);
    nlohmann::ordered_json problems = nlohmann::ordered_json::object();
    for (const SymbolGroup& group : library.groups) {
        if (group.lint.errors != 0 || group.lint.warnings != 0) {
            nlohmann::ordered_json summary;
            summary["errors"] = group.lint.errors;
            summary["warnings"] = group.lint.warnings;
            problems[group.id] = std::move(summary);
        }
    }
    if (!problems.empty()) {
        root["problems"] = std::move(problems);  // T-501
    }
    const std::string text =
        root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    return detail::writeFile(detail::joinPath(library.dir, kLibraryFile), text) ? Status::kOk
                                                                                : Status::kIoError;
}

/// Replaces the library of `mounts` with the same directory, or appends.
void putLibrary(LibraryMounts& mounts, SymbolLibrary library, size_t at) {
    for (SymbolLibrary& existing : mounts.libraries) {
        if (existing.dir == library.dir) {
            existing = std::move(library);
            return;
        }
    }
    at = at < mounts.libraries.size() ? at : mounts.libraries.size();
    mounts.libraries.insert(mounts.libraries.begin() + static_cast<std::ptrdiff_t>(at), std::move(library));
}

/// Copies the group directory `from` to `to` (which must not exist). kIoError on failure.
Status copyGroupTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive, ec);
    return ec ? Status::kIoError : Status::kOk;
}

Status mountDefaultsImpl(LibraryMounts& mounts, const fs::path& presetDir, const fs::path& userDataDir) {
    if (userDataDir.empty()) {
        return Status::kInvalidArgument;
    }
    SymbolLibrary preset;
    const Status presetStatus = loadLibraryImpl(presetDir, preset);
    if (presetStatus != Status::kOk) {
        return presetStatus == Status::kInvalidArgument || presetStatus == Status::kUnsupported
                   ? Status::kNotFound
                   : presetStatus;
    }
    const fs::path mineDir = userDataDir / "XTFrame" / "symbols";
    std::error_code ec;
    if (!fs::is_regular_file(mineDir / kLibraryFile, ec)) {
        // The first mount: create the user's library with a copy of the presets' default group.
        fs::create_directories(mineDir / kGroupsDir, ec);
        if (ec) {
            return Status::kIoError;
        }
        SymbolLibrary mine;
        mine.id = kUserLibraryId;
        mine.name = kUserLibraryName;
        mine.dir = genericPathToUtf8(mineDir);
        const SymbolGroup* source = groupOf(preset, preset.defaultGroup);
        if (source != nullptr && !source->dir.empty()) {
            const fs::path to = mineDir / kGroupsDir / pathFromUtf8(source->id);
            if (fs::exists(to, ec)) {
                fs::remove_all(to, ec);
            }
            if (const Status copied = copyGroupTree(pathFromUtf8(source->dir), to); copied != Status::kOk) {
                return copied;
            }
            SymbolGroup group;
            group.id = source->id;
            mine.groups.push_back(std::move(group));
            mine.defaultGroup = source->id;
        }
        if (const Status saved = saveLibraryJsonImpl(mine); saved != Status::kOk) {
            return saved;
        }
    }
    SymbolLibrary mine;
    if (const Status loaded = loadLibraryImpl(mineDir, mine); loaded != Status::kOk) {
        return loaded == Status::kNotFound ? Status::kIoError : loaded;
    }
    preset.scope = LibraryScope::Preset;
    mine.scope = LibraryScope::User;
    putLibrary(mounts, std::move(preset), 0);
    putLibrary(mounts, std::move(mine), 1);
    return Status::kOk;
}

Status reloadPresetImpl(LibraryMounts& mounts, std::string_view groupId, bool keepOld) {
    if (!detail::isValidId(groupId)) {
        return Status::kInvalidArgument;
    }
    const SymbolGroup* source = nullptr;
    for (const SymbolLibrary& library : mounts.libraries) {
        if (!library.readOnly) {
            continue;
        }
        if (const SymbolGroup* group = groupOf(library, groupId); group != nullptr && !group->dir.empty()) {
            source = group;
            break;
        }
    }
    SymbolLibrary* target = nullptr;
    for (SymbolLibrary& library : mounts.libraries) {
        if (!library.readOnly && !library.dir.empty()) {
            target = &library;
            break;
        }
    }
    if (source == nullptr || target == nullptr) {
        return Status::kNotFound;
    }
    const fs::path targetDir = pathFromUtf8(target->dir);
    const fs::path to = targetDir / kGroupsDir / pathFromUtf8(groupId);
    std::error_code ec;
    std::string keptId;
    if (fs::exists(to, ec)) {
        fs::path moved;
        if (keepOld) {
            const SymbolGroup* old = groupOf(*target, groupId);
            const std::string version = old != nullptr && !old->source.setVersion.empty()
                                            ? old->source.setVersion
                                            : std::string("old");
            keptId = std::string(groupId) + "@" + version;
            if (!isGroupId(keptId)) {
                keptId = std::string(groupId) + "@old";  // A version text that is not a group id.
            }
            if (fs::exists(targetDir / kGroupsDir / pathFromUtf8(keptId), ec)) {
                keptId += "-" + nowSeconds();
            }
            moved = targetDir / kGroupsDir / pathFromUtf8(keptId);
        } else {
            moved = targetDir / kTrashDir / pathFromUtf8(std::string(groupId) + "-" + nowSeconds());
            fs::create_directories(moved.parent_path(), ec);
        }
        fs::rename(to, moved, ec);
        if (ec) {
            return Status::kIoError;
        }
    }
    if (const Status copied = copyGroupTree(pathFromUtf8(source->dir), to); copied != Status::kOk) {
        return copied;
    }
    // The group list: the reloaded group where it was (or appended), the kept copy right after.
    SymbolLibrary updated = *target;
    size_t at = updated.groups.size();
    for (size_t i = 0; i < updated.groups.size(); ++i) {
        if (updated.groups[i].id == groupId) {
            at = i;
            break;
        }
    }
    if (at == updated.groups.size()) {
        SymbolGroup group;
        group.id = std::string(groupId);
        updated.groups.push_back(std::move(group));
    }
    if (!keptId.empty()) {
        SymbolGroup kept;
        kept.id = keptId;
        updated.groups.insert(updated.groups.begin() + static_cast<std::ptrdiff_t>(at + 1), std::move(kept));
    }
    if (updated.defaultGroup.empty()) {
        updated.defaultGroup = std::string(groupId);
    }
    if (const Status saved = saveLibraryJsonImpl(updated); saved != Status::kOk) {
        return saved;
    }
    SymbolLibrary reloaded;
    if (const Status loaded = loadLibraryImpl(targetDir, reloaded); loaded != Status::kOk) {
        return loaded;
    }
    *target = std::move(reloaded);
    return Status::kOk;
}

// --- T-498b: mounts -----------------------------------------------------------------------------

std::string normalDirText(const fs::path& dir) {
    std::string text = genericPathToUtf8(dir.lexically_normal());
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

Status mountImpl(LibraryMounts& mounts, const fs::path& dir, LibraryScope scope, bool listEntry) {
    SymbolLibrary library;
    if (const Status loaded = loadLibraryImpl(dir, library); loaded != Status::kOk) {
        return loaded;
    }
    library.scope = scope;
    if (listEntry) {
        bool listed = false;
        for (const MountEntry& entry : mounts.entries) {
            listed = listed || normalDirText(pathFromUtf8(entry.path)) == library.dir;
        }
        if (!listed) {
            mounts.entries.push_back(MountEntry{genericPathToUtf8(dir), true});
        }
    }
    putLibrary(mounts, std::move(library), mounts.libraries.size());
    return Status::kOk;
}

Status loadMountsImpl(const fs::path& userDataDir, LibraryMounts& mounts) {
    if (userDataDir.empty()) {
        return Status::kInvalidArgument;
    }
    bool missing = false;
    std::string error;
    const json root = readJsonObject(userDataDir / "XTFrame", kMountsFile, missing, error);
    if (missing) {
        return Status::kNotFound;
    }
    if (!error.empty()) {
        return Status::kInvalidArgument;
    }
    if (detail::getFloat(root, "version", static_cast<float>(kMountsVersion)) >
        static_cast<float>(kMountsVersion)) {
        return Status::kUnsupported;
    }
    mounts.entries.clear();
    if (const auto list = root.find("libraries"); list != root.end() && list->is_array()) {
        for (const json& item : *list) {
            MountEntry entry;
            entry.path = detail::getString(item, "path");
            entry.enabled = detail::getBool(item, "enabled", true);
            if (!entry.path.empty()) {
                mounts.entries.push_back(std::move(entry));
            }
        }
    }
    for (const MountEntry& entry : mounts.entries) {
        if (entry.enabled) {
            mountImpl(mounts, pathFromUtf8(entry.path), LibraryScope::Extra, false);  // Failures stay listed.
        }
    }
    return Status::kOk;
}

Status saveMountsImpl(const fs::path& userDataDir, const LibraryMounts& mounts) {
    if (userDataDir.empty()) {
        return Status::kInvalidArgument;
    }
    nlohmann::ordered_json root;
    root["version"] = kMountsVersion;
    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const MountEntry& entry : mounts.entries) {
        nlohmann::ordered_json item;
        item["path"] = entry.path;
        item["enabled"] = entry.enabled;
        list.push_back(std::move(item));
    }
    root["libraries"] = std::move(list);
    std::error_code ec;
    fs::create_directories(userDataDir / "XTFrame", ec);
    const std::string text =
        root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    return detail::writeFile(genericPathToUtf8(userDataDir / "XTFrame" / kMountsFile), text)
               ? Status::kOk
               : Status::kIoError;
}

// --- T-498b: the copy rule ----------------------------------------------------------------------

/// The path text of `target` as seen from the directory of `fromFile`, '/' separated
/// (`shared/textures/a.png` from `shared/themes/night.xts` -> `../textures/a.png`).
std::string referenceFrom(const fs::path& fromFile, const fs::path& target) {
    return genericPathToUtf8(
        target.lexically_normal().lexically_relative(fromFile.parent_path().lexically_normal()));
}

/// Parses an `.xts` file into `out` without diagnostics; false when it cannot be read.
bool parseSheet(const fs::path& file, compile::RuleSet& out) {
    std::string text;
    if (!detail::readFile(genericPathToUtf8(file), text)) {
        return false;
    }
    compile::DiagnosticSink sink;
    compile::parseXts(text, genericPathToUtf8(file), 1, sink, out);
    return true;
}

/// The rewrite of library-relative asset references in text copied into the project.
struct AssetRewrite {
    /// The library-relative path as the group's files write it (`textures/hatch-ink.png`).
    std::string from;
    /// Where the project holds it, project root relative (`shared/textures/hatch-ink.png`).
    fs::path projectPath;
};

/// Replaces every `from` of `rewrites` in `text` with its path relative to `targetFile`'s directory
/// (longest `from` first so a prefix never eats a longer name).
std::string rewriteReferences(std::string text, const std::vector<AssetRewrite>& rewrites,
                              const fs::path& targetFile) {
    std::vector<const AssetRewrite*> ordered;
    for (const AssetRewrite& r : rewrites) {
        ordered.push_back(&r);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const AssetRewrite* a, const AssetRewrite* b) { return a->from.size() > b->from.size(); });
    for (const AssetRewrite* r : ordered) {
        const std::string to = referenceFrom(targetFile, r->projectPath);
        if (r->from.empty() || to == r->from) {
            continue;
        }
        size_t at = 0;
        while ((at = text.find(r->from, at)) != std::string::npos) {
            text.replace(at, r->from.size(), to);
            at += to.size();
        }
    }
    return text;
}

/// The path a `url("...")` / `url(...)` value names; empty when the value has none.
std::string urlPathOf(std::string_view value) {
    const size_t at = value.find("url(");
    if (at == std::string_view::npos) {
        return std::string();
    }
    std::string_view rest = value.substr(at + 4);
    const size_t close = rest.find(')');
    if (close == std::string_view::npos) {
        return std::string();
    }
    rest = rest.substr(0, close);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '"' || rest.front() == '\'')) {
        rest.remove_prefix(1);
    }
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '"' || rest.back() == '\'')) {
        rest.remove_suffix(1);
    }
    return std::string(rest);
}

struct UseContext {
    const UiProject& project;
    fs::path root;
    UseReport& report;
    /// Asset copies made (or found) this call, by library-relative path.
    std::vector<AssetRewrite> rewrites;
    /// T-501 updateCopy(): the symbol file is written over the project's copy.
    bool overwrite = false;
    /// T-501 updateCopy(): false leaves the token and theme files alone (step 3 skipped).
    bool appendTokens = true;

    /// True when `value` names a `url()` file that is not among `rewrites`.
    bool referencesUncopied(std::string_view value) const {
        const std::string path = urlPathOf(value);
        if (path.empty()) {
            return false;
        }
        for (const AssetRewrite& r : rewrites) {
            if (r.from == path) {
                return false;
            }
        }
        return true;
    }
};

bool noteWritten(UseContext& ctx, const fs::path& file, const std::string& text) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!detail::writeFile(genericPathToUtf8(file), text)) {
        ctx.report.status = Status::kIoError;
        return false;
    }
    ctx.report.written.push_back(relativeText(ctx.root, file));
    return true;
}

/// Step 2 for one font or texture `libraryRelative` of the group at `groupDir`: copied to
/// `<assetRoot>/<kind>/<file name>`, skipped on the same content, renamed on different content.
void copyAsset(UseContext& ctx, const fs::path& groupDir, const std::string& libraryRelative,
               const char* kind) {
    fs::path source;
    if (!resolveInside(groupDir, libraryRelative, source)) {
        ctx.report.skipped.push_back(libraryRelative + ": leaves the group directory");
        return;
    }
    std::string bytes;
    if (!detail::readFile(genericPathToUtf8(source), bytes)) {
        ctx.report.skipped.push_back(libraryRelative + ": missing in the library");
        return;
    }
    const std::string assetRoot =
        ctx.project.shared.assetRoot.empty() ? std::string("shared") : ctx.project.shared.assetRoot;
    const std::string fileName = fileNameOf(libraryRelative);
    fs::path target;
    if (!resolveInside(ctx.root, assetRoot + "/" + kind + "/" + fileName, target)) {
        ctx.report.skipped.push_back(libraryRelative + ": leaves the project");
        return;
    }
    const std::string hash = util::sha256Hex(bytes);
    std::error_code ec;
    if (fs::is_regular_file(target, ec)) {
        std::string existing;
        if (detail::readFile(genericPathToUtf8(target), existing) && util::sha256Hex(existing) == hash) {
            ctx.rewrites.push_back(AssetRewrite{libraryRelative, target});
            ctx.report.skipped.push_back(relativeText(ctx.root, target) +
                                         ": same content already in the project");
            return;
        }
        // 49.3 step 2: another file of that name -> `<stem>-<8 hex><ext>`, references rewritten.
        const fs::path renamed =
            target.parent_path() / pathFromUtf8(pathToUtf8(target.stem()) + "-" + hash.substr(0, 8) +
                                                pathToUtf8(target.extension()));
        ctx.rewrites.push_back(AssetRewrite{libraryRelative, renamed});
        ctx.report.renamed.push_back(libraryRelative + " -> " + relativeText(ctx.root, renamed));
        if (fs::is_regular_file(renamed, ec)) {
            if (detail::readFile(genericPathToUtf8(renamed), existing) && util::sha256Hex(existing) == hash) {
                ctx.report.skipped.push_back(relativeText(ctx.root, renamed) +
                                             ": same content already in the project");
                return;
            }
        }
        noteWritten(ctx, renamed, bytes);
        return;
    }
    ctx.rewrites.push_back(AssetRewrite{libraryRelative, target});
    noteWritten(ctx, target, bytes);
}

/// The `/* added with <Name> from <group> */` section appended to `file` (created when missing):
/// `fontFaces` (already rewritten) before a `:root { }` of `tokens` (name, value).
void appendSection(UseContext& ctx, const fs::path& file, const std::string& symbolName,
                   const std::string& groupId, const std::vector<std::string>& fontFaces,
                   const std::vector<std::pair<std::string, std::string>>& tokens, const std::string& created,
                   const char* tokenLabelPrefix) {
    if (fontFaces.empty() && tokens.empty()) {
        return;
    }
    std::string text;
    const bool exists = detail::readFile(genericPathToUtf8(file), text);
    if (!exists) {
        text = created;
    }
    if (!text.empty() && text.back() != '\n') {
        text += '\n';
    }
    text += "\n/* added with " + symbolName + " from " + groupId + " */\n";
    for (const std::string& face : fontFaces) {
        text += face + "\n";
    }
    if (!tokens.empty()) {
        text += ":root {\n";
        for (const auto& [name, value] : tokens) {
            text += "  " + name + ": " + value + ";\n";
            ctx.report.tokensAdded.push_back(tokenLabelPrefix + name);
        }
        text += "}\n";
    }
    noteWritten(ctx, file, text);
}

std::string fontFaceText(const compile::FontFaceDef& face, const std::string& src) {
    std::string text = "@font-face { font-family: \"" + face.family + "\"; src: \"" + src +
                       "\"; font-weight: " + std::to_string(face.weight) + ";";
    if (face.italic) {
        text += " font-style: italic;";
    }
    return text + " }";
}

/// Step 4: `<shared.symbols>/index.json` gets (or replaces) the entry for `symbolName`.
void recordSource(UseContext& ctx, const fs::path& symbolsDir, const std::string& symbolName,
                  const std::string& file, const SymbolLibrary& library, const SymbolGroup& group,
                  const std::string& sha) {
    const fs::path indexPath = symbolsDir / kProjectIndexFile;
    nlohmann::ordered_json root;
    std::string text;
    if (detail::readFile(genericPathToUtf8(indexPath), text)) {
        std::string error;
        const json parsed = detail::parseBounded(text, detail::kMaxJsonDepth, error);
        if (error.empty() && parsed.is_object()) {
            root =
                nlohmann::ordered_json::parse(text, nullptr, false);  // Bounded above; keeps the key order.
        }
    }
    if (!root.is_object()) {
        root = nlohmann::ordered_json::object();
    }
    root["version"] = kProjectIndexVersion;
    nlohmann::ordered_json symbols = nlohmann::ordered_json::array();
    if (root.contains("symbols") && root["symbols"].is_array()) {
        for (const nlohmann::ordered_json& entry : root["symbols"]) {
            if (entry.is_object() && entry.contains("name") && entry["name"].is_string() &&
                entry["name"].get<std::string>() != symbolName) {
                symbols.push_back(entry);
            }
        }
    }
    nlohmann::ordered_json entry;
    entry["name"] = symbolName;
    entry["file"] = file;
    nlohmann::ordered_json source;
    source["library"] = library.id;
    source["group"] = group.id;
    source["set"] = group.source.set;
    source["setVersion"] = group.source.setVersion;
    source["sha256"] = sha;
    entry["source"] = std::move(source);
    symbols.push_back(std::move(entry));
    root["symbols"] = std::move(symbols);
    noteWritten(ctx, indexPath,
                root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n");
}

void useOneSymbol(UseContext& ctx, const SymbolLibrary& library, const SymbolGroup& group,
                  const SymbolEntry& entry) {
    const fs::path groupDir = pathFromUtf8(group.dir);
    const std::string symbolsRel =
        ctx.project.shared.symbols.empty() ? std::string("shared/symbols") : ctx.project.shared.symbols;
    fs::path symbolsDir;
    if (!resolveInside(ctx.root, symbolsRel, symbolsDir)) {
        ctx.report.skipped.push_back(entry.name + ": shared.symbols leaves the project");
        return;
    }
    fs::path source;
    if (!resolveInside(groupDir, entry.file, source)) {
        ctx.report.skipped.push_back(entry.name + ": '" + entry.file + "' leaves the group directory");
        return;
    }
    std::string symbolText;
    if (!detail::readFile(genericPathToUtf8(source), symbolText)) {
        ctx.report.skipped.push_back(entry.name + ": '" + entry.file + "' missing in the library");
        return;
    }
    const std::string fileName = fileNameOf(entry.file);
    fs::path target;
    if (!isPlainSymbolName(pathToUtf8(fs::path(fileName).stem())) ||
        !resolveInside(symbolsDir, fileName, target)) {
        ctx.report.skipped.push_back(entry.name + ": '" + entry.file + "' is not a symbol file name");
        return;
    }

    // Step 2 first: the renames decide how the symbol and token lines reference the files.
    ctx.rewrites.clear();
    for (const std::string& font : entry.fonts) {
        copyAsset(ctx, groupDir, font, "fonts");
    }
    for (const std::string& texture : entry.textures) {
        copyAsset(ctx, groupDir, texture, "textures");
    }

    // Step 1: the symbol file, never over the project's own (T-501 updateCopy(): over it).
    std::error_code ec;
    if (fs::exists(target, ec) && !ctx.overwrite) {
        ctx.report.skipped.push_back(relativeText(ctx.root, target) +
                                     ": the project already has it; using the project's");
    } else {
        const std::string written = rewriteReferences(symbolText, ctx.rewrites, target);
        if (noteWritten(ctx, target, written)) {
            recordSource(ctx, symbolsDir, entry.name, symbolsRel + "/" + fileName, library, group,
                         "sha256:" + util::sha256Hex(lfText(written)));
        }
    }

    if (!ctx.appendTokens) {
        return;  // T-501 updateCopy(): the project's token values are its own.
    }
    // Step 3: the tokens the project lacks, from the group's tokens.xts (the index default when
    // that file does not declare the name), plus the @font-face blocks of the copied fonts.
    compile::RuleSet groupTokens;
    if (!group.tokensFile.empty()) {
        fs::path tokensPath;
        if (resolveInside(groupDir, group.tokensFile, tokensPath)) {
            parseSheet(tokensPath, groupTokens);
        }
    }
    std::map<std::string, std::string> groupValue;
    for (const compile::TokenDecl& token : groupTokens.tokens) {
        groupValue[token.name] = token.value;
    }
    if (!ctx.project.shared.tokens.empty()) {
        fs::path tokensFile;
        if (!resolveInside(ctx.root, ctx.project.shared.tokens, tokensFile)) {
            ctx.report.skipped.push_back(ctx.project.shared.tokens + ": shared.tokens leaves the project");
        } else {
            compile::RuleSet mine;
            parseSheet(tokensFile, mine);
            std::set<std::string> declared;
            for (const compile::TokenDecl& token : mine.tokens) {
                declared.insert(token.name);
            }
            std::set<std::string> faces;
            for (const compile::FontFaceDef& face : mine.fontFaces) {
                faces.insert(face.family + "|" + std::to_string(face.weight) + (face.italic ? "|i" : ""));
            }
            std::vector<std::pair<std::string, std::string>> missing;
            for (const TokenDefault& token : entry.tokens) {
                if (declared.count(token.name) != 0) {
                    continue;
                }
                const auto it = groupValue.find(token.name);
                const std::string value = it != groupValue.end() ? it->second : token.value;
                if (value.empty()) {
                    continue;
                }
                if (ctx.referencesUncopied(value)) {
                    ctx.report.skipped.push_back(token.name + ": names " + urlPathOf(value) +
                                                 ", which the entry's fonts/textures do not list");
                    continue;
                }
                missing.emplace_back(token.name, rewriteReferences(value, ctx.rewrites, tokensFile));
                declared.insert(token.name);
            }
            std::vector<std::string> faceLines;
            for (const compile::FontFaceDef& face : groupTokens.fontFaces) {
                const bool copied =
                    std::find(entry.fonts.begin(), entry.fonts.end(), face.src) != entry.fonts.end();
                const std::string key =
                    face.family + "|" + std::to_string(face.weight) + (face.italic ? "|i" : "");
                if (!copied || faces.count(key) != 0) {
                    continue;
                }
                std::string src = face.src;
                for (const AssetRewrite& r : ctx.rewrites) {
                    if (r.from == face.src) {
                        src = referenceFrom(tokensFile, r.projectPath);
                    }
                }
                faceLines.push_back(fontFaceText(face, src));
                faces.insert(key);
            }
            appendSection(ctx, tokensFile, entry.name, group.id, faceLines, missing,
                          "/* Design tokens of this UI Project (shared.tokens). */\n", "");
        }
    }

    // Theme files: `tokens-night.xts` -> `<shared.themes>/night.xts`, the entry's tokens it sets.
    const std::string themesRel =
        ctx.project.shared.themes.empty() ? std::string("shared/themes") : ctx.project.shared.themes;
    for (const std::string& themeFile : group.themeFiles) {
        fs::path themePath;
        if (!resolveInside(groupDir, themeFile, themePath)) {
            continue;
        }
        compile::RuleSet themeTokens;
        if (!parseSheet(themePath, themeTokens)) {
            continue;
        }
        const std::string themeName = themeNameOf(themeFile);
        fs::path target2;
        if (themeName.empty() || !resolveInside(ctx.root, themesRel + "/" + themeName + ".xts", target2)) {
            continue;
        }
        compile::RuleSet mine;
        parseSheet(target2, mine);
        std::set<std::string> declared;
        for (const compile::TokenDecl& token : mine.tokens) {
            declared.insert(token.name);
        }
        std::map<std::string, std::string> themeValue;
        for (const compile::TokenDecl& token : themeTokens.tokens) {
            themeValue[token.name] = token.value;
        }
        std::vector<std::pair<std::string, std::string>> missing;
        for (const TokenDefault& token : entry.tokens) {
            const auto it = themeValue.find(token.name);
            if (it == themeValue.end() || declared.count(token.name) != 0) {
                continue;
            }
            if (ctx.referencesUncopied(it->second)) {
                // `index.json`'s `textures[]` covers every theme file since T-498c, so this only
                // guards a stale index: a value naming a file this call did not copy is not appended
                // (it would dangle).
                ctx.report.skipped.push_back(themeName + ":" + token.name + ": names " +
                                             urlPathOf(it->second) +
                                             ", which the entry's fonts/textures do not list");
                continue;
            }
            missing.emplace_back(token.name, rewriteReferences(it->second, ctx.rewrites, target2));
            declared.insert(token.name);
        }
        appendSection(ctx, target2, entry.name, group.id, {}, missing,
                      "/* Theme \"" + themeName + "\": token overrides only. */\n",
                      (themeName + ":").c_str());
    }
}

UseReport useSymbolsImpl(const LibraryMounts& mounts, const UiProject& project,
                         std::span<const std::string_view> names, std::string_view groupId,
                         bool overwrite = false, bool appendTokens = true) {
    UseReport report;
    if (project.rootDir.empty()) {
        report.status = Status::kInvalidArgument;
        return report;
    }
    for (const std::string_view name : names) {
        if (!isPlainSymbolName(name)) {
            report.status = Status::kInvalidArgument;
            return report;
        }
    }
    std::error_code ec;
    UseContext ctx{project,   fs::absolute(pathFromUtf8(project.rootDir), ec).lexically_normal(),
                   report,    {},
                   overwrite, appendTokens};
    for (const std::string_view name : names) {
        const SymbolLibrary* library = nullptr;
        const SymbolGroup* group = nullptr;
        const SymbolEntry* entry = findSymbol(mounts, name, groupId, &library, &group);
        if (entry == nullptr || group == nullptr || group->dir.empty()) {
            report.notFound.push_back(std::string(name));
            continue;
        }
        useOneSymbol(ctx, *library, *group, *entry);
    }
    return report;
}

bool isCopyStaleImpl(const LibraryMounts& mounts, const UiProject& project, std::string_view name) {
    if (project.rootDir.empty() || !isPlainSymbolName(name)) {
        return false;
    }
    const SymbolGroup* group = nullptr;
    const SymbolEntry* entry = findSymbol(mounts, name, {}, nullptr, &group);
    if (entry == nullptr || group == nullptr || group->dir.empty()) {
        return false;
    }
    fs::path libraryFile;
    if (!resolveInside(pathFromUtf8(group->dir), entry->file, libraryFile)) {
        return false;
    }
    const std::string symbolsRel =
        project.shared.symbols.empty() ? std::string("shared/symbols") : project.shared.symbols;
    fs::path copy;
    if (!resolveInside(pathFromUtf8(project.rootDir), symbolsRel + "/" + fileNameOf(entry->file), copy)) {
        return false;
    }
    std::string mine;
    std::string theirs;
    if (!detail::readFile(genericPathToUtf8(copy), mine) ||
        !detail::readFile(genericPathToUtf8(libraryFile), theirs)) {
        return false;
    }
    return util::sha256Hex(lfText(mine)) != util::sha256Hex(lfText(theirs));
}

}  // namespace

std::string defaultGroupDir(std::string_view libraryDir) {
    std::string out;
    catchAll([&]() {
        if (libraryDir.empty()) {
            return Status::kOk;
        }
        const fs::path dir = pathFromUtf8(libraryDir);
        bool missing = false;
        std::string error;
        const json library = readJsonObject(dir, kLibraryFile, missing, error);
        if (library.is_discarded()) {
            return Status::kOk;
        }
        const std::string id = defaultGroupId(library);
        if (id.empty()) {
            return Status::kOk;
        }
        std::error_code ec;
        const fs::path group = dir / kGroupsDir / pathFromUtf8(id);  // A group id has no separator.
        if (fs::is_directory(group, ec)) {
            out = genericPathToUtf8(group);
        }
        return Status::kOk;
    });
    return out;
}

Status loadLibrary(const fs::path& dir, SymbolLibrary& out) {
    out = SymbolLibrary();
    const Status status = catchAll([&]() { return loadLibraryImpl(dir, out); });
    if (status == Status::kInternal) {
        out = SymbolLibrary();
    }
    return status;
}

Status saveLibraryJson(const SymbolLibrary& library) {
    return catchAll([&]() { return saveLibraryJsonImpl(library); });
}

const SymbolEntry* findSymbol(const SymbolLibrary& library, std::string_view name, std::string_view group) {
    for (const SymbolGroup& g : library.groups) {
        if (!group.empty() && g.id != group) {
            continue;
        }
        for (const SymbolEntry& entry : g.symbols) {
            if (entry.name == name) {
                return &entry;
            }
        }
    }
    return nullptr;
}

Status mountDefaults(LibraryMounts& mounts, const fs::path& presetDir, const fs::path& userDataDir) {
    return catchAll([&]() { return mountDefaultsImpl(mounts, presetDir, userDataDir); });
}

Status reloadPreset(LibraryMounts& mounts, std::string_view groupId, bool keepOld) {
    return catchAll([&]() { return reloadPresetImpl(mounts, groupId, keepOld); });
}

Status loadGroup(const fs::path& dir, SymbolGroup& out) {
    out = SymbolGroup();
    return catchAll([&]() {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            return Status::kNotFound;
        }
        const std::string id = pathToUtf8(dir.lexically_normal().has_filename()
                                              ? dir.lexically_normal().filename()
                                              : dir.lexically_normal().parent_path().filename());
        if (!isGroupId(id)) {
            return Status::kInvalidArgument;
        }
        out.id = id;
        out.dir = normalDirText(dir);
        loadGroupFiles(out);
        return Status::kOk;
    });
}

const SymbolEntry* findSymbol(const LibraryMounts& mounts, std::string_view name, std::string_view group,
                              const SymbolLibrary** libraryOut, const SymbolGroup** groupOut) {
    for (const SymbolLibrary& library : mounts.libraries) {
        for (const SymbolGroup& g : library.groups) {
            if (!group.empty() && g.id != group) {
                continue;
            }
            for (const SymbolEntry& entry : g.symbols) {
                if (entry.name == name) {
                    if (libraryOut != nullptr) {
                        *libraryOut = &library;
                    }
                    if (groupOut != nullptr) {
                        *groupOut = &g;
                    }
                    return &entry;
                }
            }
        }
    }
    return nullptr;
}

const SymbolGroup* standardGroup(const LibraryMounts& mounts) {
    for (const SymbolLibrary& library : mounts.libraries) {
        if (library.scope != LibraryScope::Preset) {
            continue;
        }
        const SymbolGroup* group = groupOf(library, library.defaultGroup);
        return group != nullptr && !group->dir.empty() ? group : nullptr;
    }
    return nullptr;
}

Status mount(LibraryMounts& mounts, const fs::path& dir) {
    return catchAll([&]() { return mountImpl(mounts, dir, LibraryScope::Extra, true); });
}

bool unmount(LibraryMounts& mounts, std::string_view id) {
    bool removed = false;
    catchAll([&]() {
        for (auto it = mounts.libraries.begin(); it != mounts.libraries.end(); ++it) {
            if (it->id != id) {
                continue;
            }
            const std::string dir = it->dir;
            mounts.libraries.erase(it);
            for (auto entry = mounts.entries.begin(); entry != mounts.entries.end(); ++entry) {
                if (normalDirText(pathFromUtf8(entry->path)) == dir) {
                    mounts.entries.erase(entry);
                    break;
                }
            }
            removed = true;
            break;
        }
        return Status::kOk;
    });
    return removed;
}

Status mountProjectLibrary(LibraryMounts& mounts, const UiProject& project) {
    return catchAll([&]() {
        if (project.rootDir.empty()) {
            return Status::kInvalidArgument;
        }
        const fs::path dir = pathFromUtf8(project.rootDir) / pathFromUtf8(kProjectLibraryDir);
        std::error_code ec;
        if (!fs::is_regular_file(dir / kLibraryFile, ec)) {
            return Status::kNotFound;
        }
        return mountImpl(mounts, dir, LibraryScope::Project, false);
    });
}

Status loadMounts(const fs::path& userDataDir, LibraryMounts& mounts) {
    return catchAll([&]() { return loadMountsImpl(userDataDir, mounts); });
}

Status saveMounts(const fs::path& userDataDir, const LibraryMounts& mounts) {
    return catchAll([&]() { return saveMountsImpl(userDataDir, mounts); });
}

UseReport useSymbols(const LibraryMounts& mounts, const UiProject& project,
                     std::span<const std::string_view> names, std::string_view groupId) {
    UseReport report;
    const Status status = catchAll([&]() {
        report = useSymbolsImpl(mounts, project, names, groupId);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report.status = status;
    }
    return report;
}

UseReport updateCopy(const LibraryMounts& mounts, const UiProject& project, std::string_view name) {
    UseReport report;
    const Status status = catchAll([&]() {
        const std::string_view names[] = {name};
        report = useSymbolsImpl(mounts, project, names, {}, true, false);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report.status = status;
    }
    return report;
}

bool isCopyStale(const LibraryMounts& mounts, const UiProject& project, std::string_view name) {
    bool stale = false;
    catchAll([&]() {
        stale = isCopyStaleImpl(mounts, project, name);
        return Status::kOk;
    });
    return stale;
}

std::string themeNameOf(std::string_view themeFile) {
    std::string stem = fileNameOf(themeFile);
    if (const size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) {
        stem.erase(dot);
    }
    constexpr std::string_view kPrefix = "tokens-";
    if (stem.size() > kPrefix.size() && stem.compare(0, kPrefix.size(), kPrefix) == 0) {
        stem.erase(0, kPrefix.size());
    }
    return stem;
}

std::vector<TokenDefault> sameAsDefaultTokens(const fs::path& projectTokensFile,
                                              const fs::path& groupTokensFile) {
    std::vector<TokenDefault> out;
    catchAll([&]() {
        compile::RuleSet mine;
        compile::RuleSet theirs;
        if (!parseSheet(projectTokensFile, mine) || !parseSheet(groupTokensFile, theirs)) {
            return Status::kOk;
        }
        std::map<std::string, std::string> defaults;
        for (const compile::TokenDecl& token : theirs.tokens) {
            defaults[token.name] = trimmed(token.value);
        }
        std::set<std::string> reported;
        for (const compile::TokenDecl& token : mine.tokens) {
            const auto it = defaults.find(token.name);
            if (it != defaults.end() && it->second == trimmed(token.value) &&
                reported.insert(token.name).second) {
                out.push_back(TokenDefault{token.name, token.value});
            }
        }
        return Status::kOk;
    });
    return out;
}

bool detectImpliedGroup(const std::string& rootDir, const std::vector<ProjectUi>& uis,
                        const std::string& standardSymbolDir, SymbolGroup& out) {
    if (standardSymbolDir.empty()) {
        return false;
    }
    const fs::path groupDir = pathFromUtf8(standardSymbolDir);
    if (loadGroup(groupDir, out) != Status::kOk) {
        return false;  // Not a group directory (a bare symbol folder): no token layer to imply.
    }
    for (const ProjectUi& ui : uis) {
        std::string text;
        compile::DiagnosticSink sink;
        compile::DomTree dom;
        if (!detail::readFile(genericPathToUtf8(pathFromUtf8(rootDir) / pathFromUtf8(ui.doc)), text) ||
            !compile::parseXt(text, ui.doc, sink, dom)) {
            continue;  // The compile reports it.
        }
        for (const std::string& href : dom.symbolHrefs) {
            if (href.rfind("xt-standard:", 0) == 0) {
                return true;
            }
            const fs::path file = pathFromUtf8(href).filename();
            if (!file.empty() && !detail::findStandardSymbol(groupDir, file).empty()) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace xt::bundle
