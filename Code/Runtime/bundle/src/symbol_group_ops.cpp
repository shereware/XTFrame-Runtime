#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "symbol_internal.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/sha256.h"

/// T-501 (Docs/design/49-symbol-sets.md 49.1, 49.5, 49.6; Docs/tasks/T-501.md item 4): the group
/// operations of the editor's symbol library tab -- saveAsGroup (the reverse of the 49.3 copy
/// rule), renameGroup, deleteGroup (to `.trash/`), copyGroup, setGroupProblems (the persisted "has
/// problems" mark) and the undo helpers snapshotForUse() / changesOfUse(). Separate from
/// symbol_library.cpp for its length; helpers live in `detail::groups` so a unity build of
/// xt_bundle never sees two files' helpers under one name. Every public entry point is wrapped by
/// catchAll() (14 §14.3) and every path read from a file is joined only after resolveInside()
/// (14 §14.9 rule 5).

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace detail::groups {

constexpr const char* kLibraryFile = "library.json";
constexpr const char* kGroupFile = "group.json";
constexpr const char* kIndexFile = "index.json";
constexpr const char* kGroupsDir = "groups";
constexpr const char* kTrashDir = ".trash";
constexpr const char* kTokensFile = "tokens.xts";

std::string dumpJson(const nlohmann::ordered_json& value) {
    return value.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

/// A plain group id (`[a-z0-9-]+`, the shape a new group gets): no `.`, no `@`.
bool isPlainGroupId(std::string_view id) {
    if (!isGroupId(id)) {
        return false;
    }
    for (const char c : id) {
        if (c == '.' || c == '@') {
            return false;
        }
    }
    return true;
}

/// Reads `file` as a JSON object (bounded parse, key order kept). False when missing or malformed.
bool readOrderedObject(const fs::path& file, nlohmann::ordered_json& out, bool& missing) {
    std::string text;
    missing = !readFile(genericPathToUtf8(file), text);
    if (missing) {
        return false;
    }
    std::string depthError;
    const json checked = parseBounded(text, kMaxJsonDepth, depthError);
    if (!depthError.empty() || !checked.is_object()) {
        return false;
    }
    out = nlohmann::ordered_json::parse(text, nullptr, false);
    return out.is_object();
}

/// The library at `dir`, loaded; the usual statuses, kUnsupported when `writable` and read-only.
Status openLibrary(const fs::path& dir, bool writable, SymbolLibrary& out) {
    const Status loaded = loadLibrary(dir, out);
    if (loaded != Status::kOk) {
        return loaded == Status::kInvalidArgument || loaded == Status::kUnsupported ? Status::kNotFound
                                                                                    : loaded;
    }
    if (writable && out.readOnly) {
        return Status::kUnsupported;
    }
    return Status::kOk;
}

/// Copies the tree `from` to `to` (created), skipping symbolic links. False on an I/O failure.
bool copyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        return false;
    }
    for (fs::recursive_directory_iterator it(from, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(from);
        if (it->is_symlink(ec)) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_directory(ec)) {
            fs::create_directories(to / rel, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories((to / rel).parent_path(), ec);
            fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            return false;
        }
    }
    return !ec;
}

Status renameGroupImpl(const fs::path& libraryDir, std::string_view groupId, std::string_view name) {
    if (!isGroupId(groupId) || trimmed(name).empty()) {
        return Status::kInvalidArgument;
    }
    SymbolLibrary library;
    if (const Status opened = openLibrary(libraryDir, true, library); opened != Status::kOk) {
        return opened;
    }
    const SymbolGroup* group = groupOf(library, groupId);
    if (group == nullptr || group->dir.empty()) {
        return Status::kNotFound;
    }
    const fs::path file = pathFromUtf8(group->dir) / kGroupFile;
    nlohmann::ordered_json meta;
    bool missing = false;
    if (!readOrderedObject(file, meta, missing)) {
        if (!missing) {
            return Status::kIoError;
        }
        meta = nlohmann::ordered_json::object();
        meta["version"] = 1;
        meta["id"] = std::string(groupId);
    }
    meta["name"] = trimmed(name);
    return writeFile(genericPathToUtf8(file), dumpJson(meta)) ? Status::kOk : Status::kIoError;
}

Status deleteGroupImpl(const fs::path& libraryDir, std::string_view groupId, std::string* trashed) {
    if (!isGroupId(groupId)) {
        return Status::kInvalidArgument;
    }
    SymbolLibrary library;
    if (const Status opened = openLibrary(libraryDir, true, library); opened != Status::kOk) {
        return opened;
    }
    const SymbolGroup* group = groupOf(library, groupId);
    if (group == nullptr) {
        return Status::kNotFound;
    }
    const fs::path root = pathFromUtf8(library.dir);
    std::error_code ec;
    if (!group->dir.empty()) {
        std::string name = std::string(groupId) + "-" + nowSeconds();
        for (int n = 2; fs::exists(root / kTrashDir / pathFromUtf8(name), ec); ++n) {
            name = std::string(groupId) + "-" + nowSeconds() + "-" + std::to_string(n);
        }
        fs::create_directories(root / kTrashDir, ec);
        fs::rename(pathFromUtf8(group->dir), root / kTrashDir / pathFromUtf8(name), ec);
        if (ec) {
            return Status::kIoError;
        }
        if (trashed != nullptr) {
            *trashed = std::string(kTrashDir) + "/" + name;
        }
    } else if (trashed != nullptr) {
        trashed->clear();
    }
    library.groups.erase(std::remove_if(library.groups.begin(), library.groups.end(),
                                        [&](const SymbolGroup& g) { return g.id == groupId; }),
                         library.groups.end());
    if (library.defaultGroup == groupId) {
        library.defaultGroup.clear();
    }
    return saveLibraryJson(library);
}

Status copyGroupImpl(const fs::path& srcLibraryDir, std::string_view groupId, const fs::path& dstLibraryDir,
                     std::string* targetId) {
    if (!isGroupId(groupId)) {
        return Status::kInvalidArgument;
    }
    SymbolLibrary source;
    if (const Status opened = openLibrary(srcLibraryDir, false, source); opened != Status::kOk) {
        return opened;
    }
    const SymbolGroup* group = groupOf(source, groupId);
    if (group == nullptr || group->dir.empty()) {
        return Status::kNotFound;
    }
    SymbolLibrary target;
    if (const Status opened = openLibrary(dstLibraryDir, true, target); opened != Status::kOk) {
        return opened;
    }
    const fs::path root = pathFromUtf8(target.dir);
    std::error_code ec;
    auto occupied = [&](const std::string& id) {
        return groupOf(target, id) != nullptr || fs::exists(root / kGroupsDir / pathFromUtf8(id), ec);
    };
    std::string id(groupId);
    for (int n = 2; occupied(id); ++n) {
        id = std::string(groupId) + "-" + std::to_string(n);
    }
    const fs::path to = root / kGroupsDir / pathFromUtf8(id);
    if (!copyTree(pathFromUtf8(group->dir), to)) {
        fs::remove_all(to, ec);
        return Status::kIoError;
    }
    if (id != groupId) {
        nlohmann::ordered_json meta;
        bool missing = false;
        if (readOrderedObject(to / kGroupFile, meta, missing)) {
            meta["id"] = id;
            writeFile(genericPathToUtf8(to / kGroupFile), dumpJson(meta));
        }
    }
    SymbolGroup added;
    added.id = id;
    added.lint = group->lint;
    target.groups.push_back(std::move(added));
    if (const Status saved = saveLibraryJson(target); saved != Status::kOk) {
        return saved;
    }
    if (targetId != nullptr) {
        *targetId = id;
    }
    return Status::kOk;
}

Status setGroupProblemsImpl(const fs::path& libraryJson, std::string_view groupId,
                            const LintSummary& summary) {
    if (!isGroupId(groupId)) {
        return Status::kInvalidArgument;
    }
    std::error_code ec;
    const fs::path file = fs::is_directory(libraryJson, ec) ? libraryJson / kLibraryFile : libraryJson;
    nlohmann::ordered_json root;
    bool missing = false;
    if (!readOrderedObject(file, root, missing)) {
        return missing ? Status::kNotFound : Status::kInvalidArgument;
    }
    nlohmann::ordered_json problems = root.contains("problems") && root["problems"].is_object()
                                          ? root["problems"]
                                          : nlohmann::ordered_json::object();
    const std::string id(groupId);
    if (summary.errors == 0 && summary.warnings == 0) {
        problems.erase(id);
    } else {
        nlohmann::ordered_json entry;
        entry["errors"] = summary.errors;
        entry["warnings"] = summary.warnings;
        problems[id] = std::move(entry);
    }
    if (problems.empty()) {
        root.erase("problems");
    } else {
        root["problems"] = std::move(problems);
    }
    return writeFile(genericPathToUtf8(file), dumpJson(root)) ? Status::kOk : Status::kIoError;
}

// --- snapshotForUse / changesOfUse ------------------------------------------------------------

/// The project-relative paths useSymbols() / updateCopy() may rewrite in place.
std::vector<std::string> inPlacePaths(const UiProject& project, std::span<const std::string_view> names) {
    std::vector<std::string> paths;
    const std::string symbols =
        project.shared.symbols.empty() ? std::string("shared/symbols") : project.shared.symbols;
    if (!project.shared.tokens.empty()) {
        paths.push_back(project.shared.tokens);
    }
    const std::string themes =
        project.shared.themes.empty() ? std::string("shared/themes") : project.shared.themes;
    fs::path themesDir;
    if (resolveInside(pathFromUtf8(project.rootDir), themes, themesDir)) {
        std::error_code ec;
        std::vector<std::string> files;
        for (fs::directory_iterator it(themesDir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".xts") {
                files.push_back(themes + "/" + pathToUtf8(it->path().filename()));
            }
        }
        std::sort(files.begin(), files.end());
        paths.insert(paths.end(), files.begin(), files.end());
    }
    paths.push_back(symbols + "/" + kIndexFile);
    for (const std::string_view name : names) {
        if (isPlainSymbolName(name)) {
            paths.push_back(symbols + "/" + std::string(name) + ".xt");
        }
    }
    return paths;
}

std::vector<ProjectFileState> snapshotForUseImpl(const UiProject& project,
                                                 std::span<const std::string_view> names) {
    std::vector<ProjectFileState> out;
    if (project.rootDir.empty()) {
        return out;
    }
    const fs::path root = pathFromUtf8(project.rootDir);
    for (const std::string& relative : inPlacePaths(project, names)) {
        fs::path file;
        if (!resolveInside(root, relative, file)) {
            continue;
        }
        ProjectFileState state;
        state.path = relative;
        state.existed = readFile(genericPathToUtf8(file), state.content);
        if (!state.existed) {
            state.content.clear();
        }
        out.push_back(std::move(state));
    }
    return out;
}

std::vector<FileChange> changesOfUseImpl(const std::vector<ProjectFileState>& snapshot,
                                         const UiProject& project, const UseReport& report) {
    std::vector<FileChange> out;
    if (project.rootDir.empty()) {
        return out;
    }
    const fs::path root = pathFromUtf8(project.rootDir);
    for (const std::string& written : report.written) {
        fs::path file;
        if (!resolveInside(root, written, file)) {
            continue;
        }
        FileChange change;
        change.path = written;
        if (!readFile(genericPathToUtf8(file), change.after)) {
            continue;
        }
        for (const ProjectFileState& state : snapshot) {
            if (state.path == written) {
                change.existed = state.existed;
                change.before = state.content;
                break;
            }
        }
        out.push_back(std::move(change));
    }
    return out;
}

// --- saveAsGroup -------------------------------------------------------------------------------

/// One parsed `.xts` of the project (or a mounted group), with the file it came from.
struct Sheet {
    fs::path file;
    compile::RuleSet rules;
    std::map<std::string, std::string> values;
    TokenSheet summary;
};

bool readSheet(const fs::path& file, Sheet& out) {
    std::string text;
    if (!readFile(genericPathToUtf8(file), text)) {
        return false;
    }
    out.file = file;
    compile::DiagnosticSink sink;
    compile::parseXts(text, genericPathToUtf8(file), 1, sink, out.rules);
    for (const compile::TokenDecl& token : out.rules.tokens) {
        out.values[token.name] = trimmed(token.value);  // The last declaration wins (the cascade).
    }
    out.summary = readTokenSheet(file);
    return true;
}

/// `<Name>.xt` of the project copy's source group as `<shared.symbols>/index.json` records it.
std::string recordedSourceGroup(const fs::path& symbolsDir, const std::string& name) {
    nlohmann::ordered_json index;
    bool missing = false;
    if (!readOrderedObject(symbolsDir / kIndexFile, index, missing) || !index.contains("symbols") ||
        !index["symbols"].is_array()) {
        return std::string();
    }
    for (const nlohmann::ordered_json& entry : index["symbols"]) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
            entry["name"].get<std::string>() != name) {
            continue;
        }
        if (entry.contains("source") && entry["source"].is_object()) {
            return getString(json(entry["source"]), "group");
        }
    }
    return std::string();
}

std::string recordedDescription(const fs::path& symbolsDir, const std::string& name, std::string& thumbnail) {
    nlohmann::ordered_json index;
    bool missing = false;
    thumbnail.clear();
    if (!readOrderedObject(symbolsDir / kIndexFile, index, missing) || !index.contains("symbols") ||
        !index["symbols"].is_array()) {
        return std::string();
    }
    for (const nlohmann::ordered_json& entry : index["symbols"]) {
        if (entry.is_object() && entry.contains("name") && entry["name"].is_string() &&
            entry["name"].get<std::string>() == name) {
            thumbnail = getString(json(entry), "thumbnail");
            return getString(json(entry), "description");
        }
    }
    return std::string();
}

/// Replaces every `from` of `rewrites` in `text` with its `to`, longest `from` first.
std::string rewriteText(std::string text, const std::vector<std::pair<std::string, std::string>>& rewrites) {
    std::vector<const std::pair<std::string, std::string>*> ordered;
    for (const auto& r : rewrites) {
        ordered.push_back(&r);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto* a, const auto* b) { return a->first.size() > b->first.size(); });
    for (const auto* r : ordered) {
        if (r->first.empty() || r->first == r->second) {
            continue;
        }
        size_t at = 0;
        while ((at = text.find(r->first, at)) != std::string::npos) {
            text.replace(at, r->first.size(), r->second);
            at += r->second.size();
        }
    }
    return text;
}

struct SaveContext {
    const UiProject& project;
    fs::path root;
    fs::path groupDir;
    std::string* error;
    /// The project sheets: `shared.tokens` first, then the theme files (`themeNameOf()` -> sheet).
    Sheet base;
    bool hasBase = false;
    std::vector<std::pair<std::string, Sheet>> themes;
    /// The implied default layer (T-498b): sheets of the mounted groups, in library order.
    std::vector<Sheet> implied;
    std::vector<std::string> impliedDirs;
    /// Asset copies made: `<sheet-relative path>` -> `<group-relative path>`, per sheet file.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> rewrites;
    std::set<std::string> copied;

    Status fail(Status status, std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return status;
    }
};

/// Copies the asset `sheetRelative` (a path as `sheet` writes it) into `<group>/<kind>/<file>`;
/// records the rewrite for `sheet`. False when the file is missing (reported as a skipped asset).
bool copyDependency(SaveContext& ctx, const fs::path& sheetFile, const std::string& sheetRelative,
                    const char* kind, std::vector<std::string>& listed) {
    const fs::path source = (sheetFile.parent_path() / pathFromUtf8(sheetRelative)).lexically_normal();
    // Inside the project (or the mounted group the sheet belongs to) only.
    fs::path checked;
    const std::string fromRoot = relativeText(ctx.root, source);
    bool inside = resolveInside(ctx.root, fromRoot, checked);
    for (const std::string& dir : ctx.impliedDirs) {
        inside = inside || resolveInside(pathFromUtf8(dir), relativeText(pathFromUtf8(dir), source), checked);
    }
    if (!inside) {
        return false;
    }
    std::string bytes;
    if (!readFile(genericPathToUtf8(source), bytes)) {
        return false;
    }
    const std::string fileName = pathToUtf8(source.filename());
    const std::string groupRelative = std::string(kind) + "/" + fileName;
    fs::path target;
    if (!resolveInside(ctx.groupDir, groupRelative, target)) {
        return false;
    }
    if (ctx.copied.insert(groupRelative).second) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (!writeFile(genericPathToUtf8(target), bytes)) {
            return false;
        }
    }
    ctx.rewrites[genericPathToUtf8(sheetFile)].emplace_back(sheetRelative, groupRelative);
    if (std::find(listed.begin(), listed.end(), groupRelative) == listed.end()) {
        listed.push_back(groupRelative);
    }
    return true;
}

/// The value of `--name` for the group's tokens.xts: the project's, else the implied layer's.
const Sheet* valueSheet(const SaveContext& ctx, const std::string& name) {
    if (ctx.hasBase && ctx.base.values.count(name) != 0) {
        return &ctx.base;
    }
    for (const Sheet& sheet : ctx.implied) {
        if (sheet.values.count(name) != 0) {
            return &sheet;
        }
    }
    return nullptr;
}

std::string fontFaceLine(const compile::FontFaceDef& face, const std::string& src) {
    std::string text = "@font-face { font-family: \"" + face.family + "\"; src: \"" + src +
                       "\"; font-weight: " + std::to_string(face.weight) + ";";
    if (face.italic) {
        text += " font-style: italic;";
    }
    return text + " }";
}

Status saveAsGroupImpl(const UiProject& project, std::span<const std::string_view> names,
                       const fs::path& libraryDir, std::string_view groupId, std::string_view name,
                       std::string* error, const LibraryMounts* mounts) {
    SaveContext ctx{project, {}, {}, error, {}, false, {}, {}, {}, {}, {}};
    for (const std::string_view symbol : names) {
        if (!isPlainSymbolName(symbol)) {
            return ctx.fail(Status::kInvalidArgument, "'" + std::string(symbol) + "' is not a symbol name");
        }
    }
    if (!isPlainGroupId(groupId)) {
        return ctx.fail(Status::kInvalidArgument,
                        "'" + std::string(groupId) + "' is not a group id ([a-z0-9-])");
    }
    if (project.rootDir.empty() && !names.empty()) {
        return ctx.fail(Status::kInvalidArgument, "the project has no directory");
    }
    SymbolLibrary library;
    if (const Status opened = openLibrary(libraryDir, true, library); opened != Status::kOk) {
        return ctx.fail(opened, opened == Status::kUnsupported
                                    ? "the library is read-only"
                                    : "not a symbol library: " + genericPathToUtf8(libraryDir));
    }
    const fs::path libraryRoot = pathFromUtf8(library.dir);
    std::error_code ec;
    if (groupOf(library, groupId) != nullptr ||
        fs::exists(libraryRoot / kGroupsDir / pathFromUtf8(groupId), ec)) {
        return ctx.fail(Status::kInvalidArgument,
                        "the library already has a group '" + std::string(groupId) + "'");
    }
    ctx.root = fs::absolute(pathFromUtf8(project.rootDir.empty() ? std::string(".") : project.rootDir), ec)
                   .lexically_normal();
    const std::string symbolsRel =
        project.shared.symbols.empty() ? std::string("shared/symbols") : project.shared.symbols;
    fs::path symbolsDir;
    if (!resolveInside(ctx.root, symbolsRel, symbolsDir)) {
        return ctx.fail(Status::kInvalidArgument, "shared.symbols leaves the project");
    }

    // The symbol files first: a missing one fails before anything is written.
    struct Source {
        std::string name;
        std::string text;
    };
    std::vector<Source> sources;
    for (const std::string_view symbol : names) {
        fs::path file;
        Source source;
        source.name = std::string(symbol);
        if (!resolveInside(symbolsDir, source.name + ".xt", file) ||
            !readFile(genericPathToUtf8(file), source.text)) {
            return ctx.fail(Status::kNotFound, "no " + symbolsRel + "/" + source.name + ".xt");
        }
        sources.push_back(std::move(source));
    }

    // The sheets: the project's tokens and themes, then the implied layer (the source group of each
    // copy, then every mounted group).
    if (!project.shared.tokens.empty() && !project.rootDir.empty()) {
        fs::path tokensFile;
        if (resolveInside(ctx.root, project.shared.tokens, tokensFile)) {
            ctx.hasBase = readSheet(tokensFile, ctx.base);
        }
    }
    if (!project.rootDir.empty()) {
        const std::string themesRel =
            project.shared.themes.empty() ? std::string("shared/themes") : project.shared.themes;
        fs::path themesDir;
        if (resolveInside(ctx.root, themesRel, themesDir)) {
            std::vector<fs::path> files;
            for (fs::directory_iterator it(themesDir, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && it->path().extension() == ".xts") {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                Sheet sheet;
                if (readSheet(file, sheet)) {
                    ctx.themes.emplace_back(themeNameOf(pathToUtf8(file.filename())), std::move(sheet));
                }
            }
        }
    }
    if (mounts != nullptr) {
        std::vector<const SymbolGroup*> groups;
        for (const Source& source : sources) {
            const std::string recorded = recordedSourceGroup(symbolsDir, source.name);
            if (recorded.empty()) {
                continue;
            }
            for (const SymbolLibrary& lib : mounts->libraries) {
                if (const SymbolGroup* g = groupOf(lib, recorded); g != nullptr && !g->dir.empty()) {
                    groups.push_back(g);
                }
            }
        }
        for (const SymbolLibrary& lib : mounts->libraries) {
            for (const SymbolGroup& g : lib.groups) {
                if (!g.dir.empty()) {
                    groups.push_back(&g);
                }
            }
        }
        for (const SymbolGroup* g : groups) {
            if (g->tokensFile.empty() ||
                std::find(ctx.impliedDirs.begin(), ctx.impliedDirs.end(), g->dir) != ctx.impliedDirs.end()) {
                continue;
            }
            fs::path tokensFile;
            Sheet sheet;
            if (resolveInside(pathFromUtf8(g->dir), g->tokensFile, tokensFile) &&
                readSheet(tokensFile, sheet)) {
                ctx.implied.push_back(std::move(sheet));
                ctx.impliedDirs.push_back(g->dir);
            }
        }
    }

    // Write under a staging name, then rename into place.
    const fs::path place = libraryRoot / kGroupsDir / pathFromUtf8(groupId);
    ctx.groupDir = libraryRoot / (".save-" + std::string(groupId) + "-" + nowSeconds());
    fs::remove_all(ctx.groupDir, ec);
    fs::create_directories(ctx.groupDir, ec);
    if (ec) {
        return ctx.fail(Status::kIoError, "cannot create " + genericPathToUtf8(ctx.groupDir));
    }
    auto abort = [&](Status status, std::string message) {
        fs::remove_all(ctx.groupDir, ec);
        return ctx.fail(status, std::move(message));
    };

    nlohmann::ordered_json index;
    index["version"] = 1;
    index["tokens"] = kTokensFile;
    nlohmann::ordered_json entries = nlohmann::ordered_json::array();
    std::map<std::string, std::string> groupTokens;  // --name -> value (rewritten) for tokens.xts
    std::map<std::string, const Sheet*> tokenSheet;  // --name -> the sheet the value came from
    std::vector<std::string> fontFamilies;           // families the --font-* tokens name, in order
    std::map<std::string, std::map<std::string, std::string>> themeTokens;  // theme -> --name -> value
    std::set<std::string> allUsed;

    for (const Source& source : sources) {
        const std::vector<std::string> used = lint::varNames(source.text);
        nlohmann::ordered_json entry;
        entry["name"] = source.name;
        entry["file"] = source.name + ".xt";
        entry["thumbnail"] = "thumbnails/" + source.name + ".png";
        std::string thumbnail;
        std::string description = recordedDescription(symbolsDir, source.name, thumbnail);
        const std::string recordedGroup = recordedSourceGroup(symbolsDir, source.name);
        if (description.empty() && mounts != nullptr) {
            // The project index holds only what the user wrote: a copy keeps its source's notes.
            if (const SymbolEntry* origin = findSymbol(*mounts, source.name, recordedGroup);
                origin != nullptr) {
                description = origin->description;
            }
        }
        entry["description"] = description;
        entry["category"] = "";
        nlohmann::ordered_json states = nlohmann::ordered_json::array();
        nlohmann::ordered_json slots = nlohmann::ordered_json::array();
        nlohmann::ordered_json ports = nlohmann::ordered_json::array();
        for (const SymbolContract& contract : readSymbolContracts(source.text, source.name + ".xt")) {
            if (contract.name != source.name) {
                continue;
            }
            for (const std::string& s : contract.requiredStates) {
                states.push_back(s);
            }
            for (const std::string& s : contract.requiredSlots) {
                slots.push_back(s);
            }
            for (const SymbolPortInfo& port : contract.ports) {
                nlohmann::ordered_json p;
                p["name"] = port.name;
                p["type"] = port.type;
                p["dir"] = port.dir;
                ports.push_back(std::move(p));
            }
            break;
        }
        entry["requires"] = nlohmann::ordered_json{{"states", states}, {"slots", slots}};
        entry["ports"] = std::move(ports);
        nlohmann::ordered_json timelines = nlohmann::ordered_json::array();
        for (const TimelineScan& timeline : scanTimelines(source.text)) {
            timelines.push_back(timeline.name);
        }
        entry["timelines"] = std::move(timelines);

        // Tokens and their values; dependencies from the sheets that declare them.
        nlohmann::ordered_json tokens = nlohmann::ordered_json::array();
        std::vector<std::string> fonts;
        std::vector<std::string> textures;
        for (const std::string& bare : used) {
            const std::string full = "--" + bare;
            allUsed.insert(full);
            const Sheet* sheet = valueSheet(ctx, full);
            std::string value;
            if (sheet != nullptr) {
                value = sheet->values.at(full);
                std::vector<std::string> sheetFonts;
                std::vector<std::string> sheetTextures;
                lint::dependencies({bare}, {sheet->summary}, sheetFonts, sheetTextures);
                for (const std::string& font : sheetFonts) {
                    copyDependency(ctx, sheet->file, font, "fonts", fonts);
                }
                for (const std::string& texture : sheetTextures) {
                    copyDependency(ctx, sheet->file, texture, "textures", textures);
                }
                if (bare.rfind("font-", 0) == 0) {
                    for (const compile::FontFaceDef& face : sheet->rules.fontFaces) {
                        if (value.find("\"" + face.family + "\"") != std::string::npos &&
                            std::find(fontFamilies.begin(), fontFamilies.end(), face.family) ==
                                fontFamilies.end()) {
                            fontFamilies.push_back(face.family);
                        }
                    }
                }
                if (groupTokens.count(full) == 0) {
                    groupTokens[full] = rewriteText(value, ctx.rewrites[genericPathToUtf8(sheet->file)]);
                    tokenSheet[full] = sheet;
                }
            }
            // Theme values of this token, their textures copied too.
            for (auto& [themeName, theme] : ctx.themes) {
                const auto it = theme.values.find(full);
                if (it == theme.values.end()) {
                    continue;
                }
                std::vector<std::string> sheetFonts;
                std::vector<std::string> sheetTextures;
                lint::dependencies({bare}, {theme.summary}, sheetFonts, sheetTextures);
                for (const std::string& font : sheetFonts) {
                    copyDependency(ctx, theme.file, font, "fonts", fonts);
                }
                for (const std::string& texture : sheetTextures) {
                    copyDependency(ctx, theme.file, texture, "textures", textures);
                }
                themeTokens[themeName][full] =
                    rewriteText(it->second, ctx.rewrites[genericPathToUtf8(theme.file)]);
            }
            nlohmann::ordered_json token;
            token["name"] = full;
            token["default"] = groupTokens.count(full) != 0 ? groupTokens[full] : std::string();
            tokens.push_back(std::move(token));
        }
        entry["tokens"] = std::move(tokens);
        entry["fonts"] = fonts;
        entry["textures"] = textures;
        entry["content"] = "sha256:" + util::sha256Hex(lfText(source.text));
        entry["previousContent"] = nlohmann::ordered_json::array();
        entries.push_back(std::move(entry));

        // The symbol file, as it is.
        if (!writeFile(genericPathToUtf8(ctx.groupDir / pathFromUtf8(source.name + ".xt")), source.text)) {
            return abort(Status::kIoError, "cannot write " + source.name + ".xt");
        }
        // The thumbnail: the project's, else the recorded source group's.
        fs::path thumbnailFrom;
        bool haveThumbnail = false;
        if (!thumbnail.empty() && resolveInside(symbolsDir, thumbnail, thumbnailFrom) &&
            fs::is_regular_file(thumbnailFrom, ec)) {
            haveThumbnail = true;
        } else if (mounts != nullptr) {
            const SymbolGroup* group = nullptr;
            const SymbolEntry* found = findSymbol(*mounts, source.name, recordedGroup, nullptr, &group);
            if (found == nullptr) {
                found = findSymbol(*mounts, source.name, {}, nullptr, &group);
            }
            if (found != nullptr && group != nullptr && !found->thumbnail.empty() &&
                resolveInside(pathFromUtf8(group->dir), found->thumbnail, thumbnailFrom) &&
                fs::is_regular_file(thumbnailFrom, ec)) {
                haveThumbnail = true;
            }
        }
        if (haveThumbnail) {
            fs::create_directories(ctx.groupDir / "thumbnails", ec);
            fs::copy_file(thumbnailFrom, ctx.groupDir / "thumbnails" / pathFromUtf8(source.name + ".png"),
                          fs::copy_options::overwrite_existing, ec);
        }
    }
    index["symbols"] = std::move(entries);

    // tokens.xts: the @font-face blocks of the named families (their sources rewritten), then
    // one :root with every token that has a value.
    std::string tokensText =
        "/* Tokens of the group \"" + std::string(name) + "\" (saved from a project). */\n";
    for (const std::string& family : fontFamilies) {
        const Sheet* from = nullptr;
        for (const auto& [token, sheet] : tokenSheet) {
            if (sheet->values.at(token).find("\"" + family + "\"") != std::string::npos) {
                from = sheet;
                break;
            }
        }
        if (from == nullptr) {
            continue;
        }
        for (const compile::FontFaceDef& face : from->rules.fontFaces) {
            if (face.family != family) {
                continue;
            }
            std::string src = face.src;
            for (const auto& r : ctx.rewrites[genericPathToUtf8(from->file)]) {
                if (r.first == face.src) {
                    src = r.second;
                }
            }
            tokensText += fontFaceLine(face, src) + "\n";
        }
    }
    if (!groupTokens.empty()) {
        tokensText += ":root {\n";
        for (const auto& [token, value] : groupTokens) {
            tokensText += "  " + token + ": " + value + ";\n";
        }
        tokensText += "}\n";
    }
    if (!writeFile(genericPathToUtf8(ctx.groupDir / kTokensFile), tokensText)) {
        return abort(Status::kIoError, "cannot write tokens.xts");
    }
    std::vector<std::string> themeFiles;
    for (const auto& [themeName, values] : themeTokens) {
        if (values.empty() || themeName.empty()) {
            continue;
        }
        const std::string file = "tokens-" + themeName + ".xts";
        std::string text = "/* Theme \"" + themeName + "\": token overrides only. */\n:root {\n";
        for (const auto& [token, value] : values) {
            text += "  " + token + ": " + value + ";\n";
        }
        text += "}\n";
        if (!writeFile(genericPathToUtf8(ctx.groupDir / pathFromUtf8(file)), text)) {
            return abort(Status::kIoError, "cannot write " + file);
        }
        themeFiles.push_back(file);
    }

    nlohmann::ordered_json meta;
    meta["version"] = 1;
    meta["id"] = std::string(groupId);
    meta["name"] = trimmed(name).empty() ? std::string(groupId) : trimmed(name);
    meta["description"] = "";
    meta["source"] = nlohmann::ordered_json{{"kind", "manual"}};
    meta["tokens"] = kTokensFile;
    meta["themes"] = themeFiles;
    if (!writeFile(genericPathToUtf8(ctx.groupDir / kGroupFile), dumpJson(meta)) ||
        !writeFile(genericPathToUtf8(ctx.groupDir / kIndexFile), dumpJson(index))) {
        return abort(Status::kIoError, "cannot write group.json / index.json");
    }

    fs::create_directories(place.parent_path(), ec);
    fs::rename(ctx.groupDir, place, ec);
    if (ec) {
        return abort(Status::kIoError, "cannot place the group '" + std::string(groupId) + "'");
    }
    SymbolGroup added;
    added.id = std::string(groupId);
    LintOptions options;
    options.indexOnly = true;
    const LintReport lint = lintGroup(place, options);
    added.lint.errors = lint.errors();
    added.lint.warnings = lint.warnings();
    library.groups.push_back(std::move(added));
    if (const Status saved = saveLibraryJson(library); saved != Status::kOk) {
        return ctx.fail(saved, "cannot write library.json");
    }
    return Status::kOk;
}

}  // namespace detail::groups

std::vector<ProjectFileState> snapshotForUse(const UiProject& project,
                                             std::span<const std::string_view> names) {
    std::vector<ProjectFileState> out;
    detail::catchAll([&]() {
        out = detail::groups::snapshotForUseImpl(project, names);
        return Status::kOk;
    });
    return out;
}

std::vector<FileChange> changesOfUse(const std::vector<ProjectFileState>& snapshot, const UiProject& project,
                                     const UseReport& report) {
    std::vector<FileChange> out;
    detail::catchAll([&]() {
        out = detail::groups::changesOfUseImpl(snapshot, project, report);
        return Status::kOk;
    });
    return out;
}

Status saveAsGroup(const UiProject& project, std::span<const std::string_view> names,
                   const fs::path& libraryDir, std::string_view groupId, std::string_view name,
                   std::string* error, const LibraryMounts* mounts) {
    if (error != nullptr) {
        error->clear();
    }
    return detail::catchAll([&]() {
        return detail::groups::saveAsGroupImpl(project, names, libraryDir, groupId, name, error, mounts);
    });
}

Status renameGroup(const fs::path& libraryDir, std::string_view groupId, std::string_view name) {
    return detail::catchAll([&]() { return detail::groups::renameGroupImpl(libraryDir, groupId, name); });
}

Status deleteGroup(const fs::path& libraryDir, std::string_view groupId, std::string* trashed) {
    return detail::catchAll([&]() { return detail::groups::deleteGroupImpl(libraryDir, groupId, trashed); });
}

Status copyGroup(const fs::path& srcLibraryDir, std::string_view groupId, const fs::path& dstLibraryDir,
                 std::string* targetId) {
    return detail::catchAll(
        [&]() { return detail::groups::copyGroupImpl(srcLibraryDir, groupId, dstLibraryDir, targetId); });
}

Status setGroupProblems(const fs::path& libraryJson, std::string_view groupId, const LintSummary& summary) {
    return detail::catchAll(
        [&]() { return detail::groups::setGroupProblemsImpl(libraryJson, groupId, summary); });
}

}  // namespace xt::bundle
