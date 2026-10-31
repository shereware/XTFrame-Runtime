#include "xt/bundle/symbol_set.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "symbol_internal.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/zip_archive.h"
#include "xt/charconv.h"
#include "xt/compile/utf8_path.h"
#include "xt/version.h"

/// T-499 (Docs/design/49-symbol-sets.md 49.4, 49.7), see the header; T-500: the `.xtsymbols`
/// archive form (exportSet / importSet) through xt/bundle/zip_archive.h. Helpers live in
/// `xt::bundle::detail::sets` so a unity build of xt_bundle never sees two files' helpers under one
/// name.

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace detail::sets {

constexpr int kSetVersion = 1;
constexpr size_t kMaxSetFiles = 5000;
constexpr uint64_t kMaxSetBytes = 256ull * 1024ull * 1024ull;
constexpr size_t kMaxSetGroups = 64;
constexpr const char* kSetFile = "set.json";
constexpr const char* kSetGroupsDir = "groups";

struct Semver {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
};

/// `X.Y.Z` (each 1..9 digits), optionally followed by `-` or `+` and `[0-9A-Za-z.-]` characters.
bool parseSemver(std::string_view text, Semver& out) {
    uint32_t parts[3] = {0, 0, 0};
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        const size_t start = i;
        uint64_t value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 9) {
            value = value * 10 + static_cast<uint64_t>(text[i] - '0');
            ++i;
        }
        if (i == start || (i < text.size() && text[i] >= '0' && text[i] <= '9')) {
            return false;
        }
        parts[part] = static_cast<uint32_t>(value);
        if (part < 2) {
            if (i >= text.size() || text[i] != '.') {
                return false;
            }
            ++i;
        }
    }
    if (i < text.size()) {
        if (text[i] != '-' && text[i] != '+') {
            return false;
        }
        for (++i; i < text.size(); ++i) {
            const char c = text[i];
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            c == '.' || c == '-';
            if (!ok) {
                return false;
            }
        }
    }
    out = Semver{parts[0], parts[1], parts[2]};
    return true;
}

int compareSemver(const Semver& a, const Semver& b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    return 0;
}

bool validAuthor(std::string_view author) {
    if (author.empty() || author.size() > 64) {
        return false;
    }
    for (const char c : author) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

Status fail(std::string& error, Status status, std::string message) {
    error = std::move(message);
    return status;
}

/// `requires.xtframe` against this build: kOk, or kUnsupported with `error`.
Status checkRequires(const std::string& text, std::string& error) {
    if (text.empty()) {
        return Status::kOk;
    }
    Semver need;
    std::string version = text.rfind(">=", 0) == 0 ? text.substr(2) : std::string();
    version = trimmed(version);
    // `>=0.6` is `>=0.6.0`.
    if (!parseSemver(version, need)) {
        if (!parseSemver(version + ".0", need)) {
            return fail(error, Status::kUnsupported,
                        "requires.xtframe '" + text + "' is not of the form >=<major>.<minor>");
        }
    }
    const Semver have{XT_VERSION_MAJOR, XT_VERSION_MINOR, XT_VERSION_PATCH};
    if (compareSemver(have, need) < 0) {
        return fail(error, Status::kUnsupported,
                    "the set requires XTFrame " + text + ", this is " + versionString());
    }
    return Status::kOk;
}

Status readSetInfoImpl(const fs::path& setDir, SetInfo& out, std::string& error) {
    std::string text;
    if (!readFile(genericPathToUtf8(setDir / kSetFile), text)) {
        return fail(error, Status::kNotFound, "no set.json");
    }
    std::string depthError;
    const json root = parseBounded(text, kMaxJsonDepth, depthError);
    if (!depthError.empty()) {
        return fail(error, Status::kInvalidArgument, "set.json: " + depthError);
    }
    if (root.is_discarded() || !root.is_object()) {
        return fail(error, Status::kInvalidArgument, "set.json is not a JSON object");
    }
    out.version = static_cast<int>(getFloat(root, "version", static_cast<float>(kSetVersion)));
    if (out.version > kSetVersion) {
        return fail(error, Status::kUnsupported,
                    "set.json version " + std::to_string(out.version) + " is newer than this build reads");
    }
    out.id = getString(root, "id");
    if (!isValidId(out.id)) {
        return fail(error, Status::kInvalidArgument, "set.json: 'id' must be [a-z0-9-]+");
    }
    out.author = getString(root, "author");
    if (!validAuthor(out.author)) {
        return fail(error, Status::kInvalidArgument,
                    "set.json: 'author' is missing or has characters outside [A-Za-z0-9._-]");
    }
    out.name = getString(root, "name");
    if (out.name.empty()) {
        out.name = out.id;
    }
    out.setVersion = getString(root, "setVersion");
    Semver semver;
    if (!parseSemver(out.setVersion, semver)) {
        return fail(error, Status::kInvalidArgument, "set.json: 'setVersion' is not a semver (X.Y.Z)");
    }
    out.license = getString(root, "license");
    if (out.license.empty() || out.license.size() > 128) {
        return fail(error, Status::kInvalidArgument, "set.json: 'license' is required");
    }
    out.fontsLicense = getString(root, "fontsLicense");
    out.description = getString(root, "description");
    out.homepage = getString(root, "homepage");
    out.thumbnail = getString(root, "thumbnail");
    if (!out.thumbnail.empty()) {
        fs::path inside;
        if (!resolveInside(setDir, out.thumbnail, inside)) {
            return fail(error, Status::kInvalidArgument, "set.json: 'thumbnail' leaves the set directory");
        }
    }
    out.groups = getStringArray(root, "groups");
    if (out.groups.empty() || out.groups.size() > kMaxSetGroups) {
        return fail(error, Status::kInvalidArgument,
                    "set.json: 'groups' must list 1.." + std::to_string(kMaxSetGroups) + " group ids");
    }
    std::set<std::string> seen;
    for (const std::string& id : out.groups) {
        if (!isGroupId(id) || !seen.insert(id).second) {
            return fail(error, Status::kInvalidArgument, "set.json: '" + id + "' is not a (unique) group id");
        }
    }
    if (const auto req = root.find("requires"); req != root.end() && req->is_object()) {
        out.requiresXtframe = getString(*req, "xtframe");
    }
    return checkRequires(out.requiresXtframe, error);
}

/// Every file of the tree under `root` (a directory), relative '/' paths, sorted; `error` set and
/// false when a symbolic link, a special file, too many files or too many bytes are met.
bool walkChecked(const fs::path& root, std::vector<std::string>& files, std::string& error) {
    std::error_code ec;
    uint64_t bytes = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(root);
        if (it->is_symlink(ec)) {
            error = "'" + genericPathToUtf8(rel) + "' is a symbolic link";
            return false;
        }
        if (it->is_directory(ec)) {
            continue;
        }
        if (!it->is_regular_file(ec)) {
            error = "'" + genericPathToUtf8(rel) + "' is not a regular file";
            return false;
        }
        bytes += static_cast<uint64_t>(it->file_size(ec));
        files.push_back(genericPathToUtf8(rel));
        if (files.size() > kMaxSetFiles || bytes > kMaxSetBytes) {
            error = "the set holds more than " + std::to_string(kMaxSetFiles) + " files or " +
                    std::to_string(kMaxSetBytes / (1024 * 1024)) + " MB";
            return false;
        }
    }
    if (ec) {
        error = "cannot read the directory tree: " + ec.message();
        return false;
    }
    std::sort(files.begin(), files.end());
    return true;
}

/// Copies the tree `from` to `to`, leaving out hidden entries (a name starting with '.'), symbolic
/// links and, with `skipFonts`, the top-level `fonts/` directory. False on an I/O failure.
bool copyTree(const fs::path& from, const fs::path& to, bool skipFonts, std::string& error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        error = "cannot create " + genericPathToUtf8(to) + ": " + ec.message();
        return false;
    }
    for (fs::recursive_directory_iterator it(from, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(from);
        const std::string first = pathToUtf8(*rel.begin());
        const bool hidden = !first.empty() && first.front() == '.';
        const bool leaf = !rel.empty() && pathToUtf8(rel.filename()).front() == '.';
        if (hidden || leaf || it->is_symlink(ec) || (skipFonts && first == "fonts")) {
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
            error = "cannot copy " + genericPathToUtf8(rel) + ": " + ec.message();
            return false;
        }
    }
    if (ec) {
        error = "cannot read " + genericPathToUtf8(from) + ": " + ec.message();
        return false;
    }
    return true;
}

/// `YYYY-MM-DDTHH:MM:SSZ` of `seconds` since the epoch (UTC; the civil-from-days algorithm).
std::string isoUtc(int64_t seconds) {
    int64_t days = seconds / 86400;
    int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", static_cast<int>(year),
                  static_cast<int>(month), static_cast<int>(day), static_cast<int>(rest / 3600),
                  static_cast<int>(rest % 3600 / 60), static_cast<int>(rest % 60));
    return buffer;
}

std::string dumpJson(const nlohmann::ordered_json& value) {
    return value.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

/// The first line `SPDX-License-Identifier: <id>` of the file `path`; empty when none.
std::string spdxOf(const fs::path& path) {
    std::string text;
    if (!readFile(genericPathToUtf8(path), text)) {
        return std::string();
    }
    constexpr std::string_view kKey = "SPDX-License-Identifier:";
    const size_t at = text.find(kKey);
    if (at == std::string::npos) {
        return std::string();
    }
    size_t end = text.find_first_of("\r\n", at);
    return trimmed(std::string_view(text).substr(
        at + kKey.size(), end == std::string::npos ? std::string::npos : end - at - kKey.size()));
}

ExportReport exportImpl(const fs::path& groupDir, const fs::path& outDir, const ExportOptions& options) {
    ExportReport report;
    auto failWith = [&](Status status, std::string message) {
        report.status = status;
        report.error = std::move(message);
        return report;
    };
    SymbolGroup group;
    const Status loaded = loadGroup(groupDir, group);
    if (loaded == Status::kNotFound) {
        return failWith(Status::kNotFound, "not a group directory: " + genericPathToUtf8(groupDir));
    }
    if (loaded != Status::kOk) {
        return failWith(Status::kInvalidArgument, "the directory name is not a group id");
    }
    SetInfo& set = report.set;
    set.version = kSetVersion;
    const std::string& sourceSet = group.source.set;
    const size_t slash = sourceSet.find('/');
    set.id = slash == std::string::npos ? group.id : sourceSet.substr(slash + 1);
    if (!isValidId(set.id)) {
        return failWith(Status::kInvalidArgument,
                        "the group id '" + group.id + "' is not a set id ([a-z0-9-])");
    }
    set.author = !options.author.empty()
                     ? options.author
                     : (slash == std::string::npos ? std::string() : sourceSet.substr(0, slash));
    if (set.author.empty()) {
        return failWith(Status::kInvalidArgument, "no author: the group names no source set; pass --author");
    }
    if (!validAuthor(set.author)) {
        return failWith(Status::kInvalidArgument, "the author must be 1..64 characters of [A-Za-z0-9._-]");
    }
    set.license = options.license;
    if (set.license.empty()) {
        std::string text;
        if (readFile(genericPathToUtf8(groupDir / "group.json"), text)) {
            std::string depthError;
            const json meta = parseBounded(text, kMaxJsonDepth, depthError);
            if (meta.is_object()) {
                if (const auto source = meta.find("source"); source != meta.end() && source->is_object()) {
                    set.license = getString(*source, "license");
                }
            }
        }
    }
    if (set.license.empty()) {
        set.license = spdxOf(groupDir / "LICENSE");
    }
    if (set.license.empty()) {
        return failWith(Status::kInvalidArgument,
                        "no license: group.json source.license and an SPDX-License-Identifier line in "
                        "LICENSE are missing; pass --license");
    }
    Semver semver;
    set.setVersion =
        parseSemver(group.source.setVersion, semver) ? group.source.setVersion : std::string("1.0.0");
    set.name = group.name;
    set.description = group.description;
    set.groups = {group.id};
    {
        char version[32];
        std::snprintf(version, sizeof(version), ">=%d.%d", XT_VERSION_MAJOR, XT_VERSION_MINOR);
        set.requiresXtframe = version;
    }
    if (options.fonts) {
        std::error_code ec;
        for (fs::directory_iterator it(groupDir / "fonts", ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = pathToUtf8(it->path().filename());
            if (name.rfind("OFL", 0) == 0) {
                set.fontsLicense = "OFL-1.1";
            }
        }
    }

    std::error_code ec;
    if (fs::exists(outDir, ec)) {
        if (!fs::is_directory(outDir, ec) || !fs::is_empty(outDir, ec)) {
            return failWith(Status::kInvalidArgument,
                            genericPathToUtf8(outDir) + " exists and is not an empty directory");
        }
    }
    std::string error;
    if (!copyTree(groupDir, outDir / kSetGroupsDir / pathFromUtf8(group.id), !options.fonts, error)) {
        return failWith(Status::kIoError, error);
    }
    if (!group.thumbnail.empty()) {
        fs::path thumbnail;
        if (resolveInside(groupDir, group.thumbnail, thumbnail) && fs::is_regular_file(thumbnail, ec)) {
            fs::copy_file(thumbnail, outDir / "thumbnail.png", fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                set.thumbnail = "thumbnail.png";
            }
        }
    }
    nlohmann::ordered_json root;
    root["version"] = set.version;
    root["id"] = set.id;
    root["author"] = set.author;
    root["name"] = set.name;
    root["setVersion"] = set.setVersion;
    root["license"] = set.license;
    if (!set.fontsLicense.empty()) {
        root["fontsLicense"] = set.fontsLicense;
    }
    root["description"] = set.description;
    if (!set.homepage.empty()) {
        root["homepage"] = set.homepage;
    }
    if (!set.thumbnail.empty()) {
        root["thumbnail"] = set.thumbnail;
    }
    root["groups"] = set.groups;
    root["requires"] = nlohmann::ordered_json{{"xtframe", set.requiresXtframe}};
    if (!writeFile(genericPathToUtf8(outDir / kSetFile), dumpJson(root))) {
        return failWith(Status::kIoError, "cannot write set.json");
    }
    std::string walkError;
    walkChecked(outDir, report.files, walkError);
    return report;
}

bool isSuffixedId(const std::string& id, const std::string& base) {
    if (id.size() <= base.size() + 1 || id.compare(0, base.size(), base) != 0 || id[base.size()] != '-') {
        return false;
    }
    for (size_t i = base.size() + 1; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') {
            return false;
        }
    }
    return true;
}

struct Plan {
    ImportedGroup group;
    bool write = false;
    /// Where in the library the group goes; the old group there (if any) moves to `.trash/`.
    bool replacesExisting = false;
};

/// Rewrites `file` (a group.json, created when missing) for the group imported as `targetId`.
bool writeImportedGroupJson(const fs::path& file, const std::string& targetId, const SetInfo& set,
                            const std::string& setVersion) {
    nlohmann::ordered_json meta = nlohmann::ordered_json::object();
    std::string text;
    if (readFile(genericPathToUtf8(file), text)) {
        std::string depthError;
        const json checked = parseBounded(text, kMaxJsonDepth, depthError);
        if (depthError.empty() && checked.is_object()) {
            meta = nlohmann::ordered_json::parse(text, nullptr, false);
        }
    }
    if (!meta.is_object()) {
        meta = nlohmann::ordered_json::object();
    }
    if (!meta.contains("version")) {
        meta["version"] = 1;
    }
    meta["id"] = targetId;
    if (!meta.contains("name")) {
        meta["name"] = targetId;
    }
    nlohmann::ordered_json source = meta.contains("source") && meta["source"].is_object()
                                        ? meta["source"]
                                        : nlohmann::ordered_json::object();
    source["kind"] = "import";
    source["set"] = set.author + "/" + set.id;
    source["setVersion"] = setVersion;
    meta["source"] = std::move(source);
    meta["importedAt"] = isoUtc(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    return writeFile(genericPathToUtf8(file), dumpJson(meta));
}

ImportReport importImpl(const fs::path& setDir, const fs::path& libraryDir, const ImportOptions& options) {
    ImportReport report;
    auto failWith = [&](Status status, std::string message) {
        report.status = status;
        report.error = std::move(message);
        report.groups.clear();
        return report;
    };
    std::string error;
    if (const Status status = readSetInfoImpl(setDir, report.set, error); status != Status::kOk) {
        return failWith(status, error);
    }
    const SetInfo& set = report.set;
    if (!options.asGroup.empty() && (set.groups.size() != 1 || !isGroupId(options.asGroup))) {
        return failWith(Status::kInvalidArgument,
                        "--as needs a set with one group and a group id ([a-z0-9-])");
    }
    std::vector<std::string> files;
    if (!walkChecked(setDir, files, error)) {
        return failWith(Status::kInvalidArgument, error);
    }

    // The set's groups: directories inside the set, every path of their files inside the group.
    std::vector<fs::path> sourceDirs;
    for (const std::string& id : set.groups) {
        fs::path dir;
        std::error_code ec;
        if (!resolveInside(setDir, std::string(kSetGroupsDir) + "/" + id, dir) ||
            !fs::is_directory(dir, ec)) {
            return failWith(Status::kInvalidArgument,
                            "set.json lists the group '" + id + "' but groups/" + id + " is not a directory");
        }
        SymbolGroup group;
        if (loadGroup(dir, group) != Status::kOk) {
            return failWith(Status::kInvalidArgument, "groups/" + id + " cannot be read as a group");
        }
        for (const compile::Diagnostic& d : group.problems) {
            if (d.message.find("leaves the group directory") != std::string::npos) {
                return failWith(Status::kInvalidArgument, "groups/" + id + ": " + d.file + ": " + d.message);
            }
        }
        sourceDirs.push_back(dir);
    }
    std::vector<LintReport> lints;
    for (const fs::path& dir : sourceDirs) {
        lints.push_back(lintGroup(dir));
    }

    SymbolLibrary library;
    if (const Status loaded = loadLibrary(libraryDir, library); loaded != Status::kOk) {
        return failWith(loaded == Status::kNotFound ? Status::kNotFound : Status::kInvalidArgument,
                        "not a symbol library: " + genericPathToUtf8(libraryDir));
    }
    if (library.readOnly) {
        return failWith(Status::kUnsupported, "the library '" + library.id + "' is read-only");
    }
    const fs::path libraryRoot = pathFromUtf8(library.dir);

    // The id rules, per group.
    const std::string identity = set.author + "/" + set.id;
    Semver incoming;
    parseSemver(set.setVersion, incoming);
    std::set<std::string> taken;
    for (const SymbolGroup& g : library.groups) {
        taken.insert(g.id);
    }
    std::vector<Plan> plans;
    for (size_t i = 0; i < set.groups.size(); ++i) {
        Plan plan;
        plan.group.sourceId = set.groups[i];
        plan.group.lint = std::move(lints[i]);
        const std::string want = options.asGroup.empty() ? set.groups[i] : options.asGroup;
        const SymbolGroup* same = nullptr;
        for (const SymbolGroup& g : library.groups) {
            const bool idMatches = g.id == want || (options.asGroup.empty() && isSuffixedId(g.id, want));
            if (g.source.set == identity && idMatches && (same == nullptr || g.id == want)) {
                same = &g;
            }
        }
        if (same != nullptr) {
            Semver existing;
            parseSemver(same->source.setVersion, existing);
            const int order = compareSemver(incoming, existing);
            plan.group.targetId = same->id;
            if (order > 0) {
                plan.group.action = ImportAction::Updated;
                plan.write = plan.replacesExisting = true;
            } else if (order == 0) {
                plan.group.action = ImportAction::Unchanged;
            } else if (options.replaceOlder) {
                plan.group.action = ImportAction::OlderReplaced;
                plan.write = plan.replacesExisting = true;
            } else {
                plan.group.action = ImportAction::OlderSkipped;
            }
        } else {
            std::error_code ec;
            auto occupied = [&](const std::string& id) {
                return taken.count(id) != 0 || fs::exists(libraryRoot / kSetGroupsDir / pathFromUtf8(id), ec);
            };
            std::string target = want;
            if (occupied(target)) {
                if (!options.asGroup.empty()) {
                    return failWith(Status::kInvalidArgument,
                                    "the library has a group '" + want + "' of another set");
                }
                for (int n = 2; occupied(target = want + "-" + std::to_string(n)); ++n) {
                }
            }
            plan.group.targetId = target;
            plan.group.action = ImportAction::Added;
            plan.write = true;
            taken.insert(target);
        }
        plans.push_back(std::move(plan));
    }

    // Copy into a staging directory first; nothing of the library changes until every copy worked.
    const std::string stamp = nowSeconds();
    fs::path staging = libraryRoot / (".import-" + stamp);
    std::error_code ec;
    for (int n = 2; fs::exists(staging, ec); ++n) {
        staging = libraryRoot / (".import-" + stamp + "-" + std::to_string(n));
    }
    auto cleanup = [&]() { fs::remove_all(staging, ec); };
    for (size_t i = 0; i < plans.size(); ++i) {
        if (!plans[i].write) {
            continue;
        }
        const fs::path to = staging / pathFromUtf8(plans[i].group.targetId);
        if (!copyTree(sourceDirs[i], to, false, error) ||
            !writeImportedGroupJson(to / "group.json", plans[i].group.targetId, set, set.setVersion)) {
            cleanup();
            return failWith(Status::kIoError, error.empty() ? "cannot write group.json" : error);
        }
    }
    const fs::path trash = libraryRoot / ".trash";
    std::vector<std::pair<fs::path, fs::path>> moved;  // (trash place, group place) of the old groups
    for (Plan& plan : plans) {
        if (!plan.write) {
            continue;
        }
        const fs::path place = libraryRoot / kSetGroupsDir / pathFromUtf8(plan.group.targetId);
        if (plan.replacesExisting && fs::exists(place, ec)) {
            std::string name = plan.group.targetId + "-" + stamp;
            for (int n = 2; fs::exists(trash / pathFromUtf8(name), ec); ++n) {
                name = plan.group.targetId + "-" + stamp + "-" + std::to_string(n);
            }
            fs::create_directories(trash, ec);
            fs::rename(place, trash / pathFromUtf8(name), ec);
            if (ec) {
                for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
                    fs::rename(it->first, it->second, ec);
                }
                cleanup();
                return failWith(Status::kIoError, "cannot move " + genericPathToUtf8(place) + " to .trash");
            }
            moved.emplace_back(trash / pathFromUtf8(name), place);
            plan.group.trashed = ".trash/" + name;
        }
        fs::create_directories(place.parent_path(), ec);
        fs::rename(staging / pathFromUtf8(plan.group.targetId), place, ec);
        if (ec) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
                fs::remove_all(it->second, ec);
                fs::rename(it->first, it->second, ec);
            }
            cleanup();
            return failWith(Status::kIoError, "cannot place the group '" + plan.group.targetId + "'");
        }
    }
    cleanup();

    bool listChanged = false;
    for (const Plan& plan : plans) {
        if (plan.write && std::none_of(library.groups.begin(), library.groups.end(),
                                       [&](const SymbolGroup& g) { return g.id == plan.group.targetId; })) {
            SymbolGroup added;
            added.id = plan.group.targetId;
            library.groups.push_back(std::move(added));
            listChanged = true;
        }
    }
    if (listChanged) {
        if (const Status saved = saveLibraryJson(library); saved != Status::kOk) {
            return failWith(Status::kIoError, "cannot write library.json");
        }
    }
    for (Plan& plan : plans) {
        report.groups.push_back(std::move(plan.group));
    }
    return report;
}

// --- tokens.json --------------------------------------------------------------------------------

std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

/// `value` (`12px`, `0`, `1.5`) as a number text; `fallback` when it is none.
std::string numberText(const std::map<std::string, std::string>& values, const char* name,
                       const char* fallback) {
    const auto it = values.find(name);
    if (it == values.end()) {
        return fallback;
    }
    std::string text = it->second;
    if (text.size() > 2 && text.compare(text.size() - 2, 2, "px") == 0) {
        text.erase(text.size() - 2);
    }
    double number = 0;
    const std::from_chars_result parsed = fromChars(text.data(), text.data() + text.size(), number);
    if (text.empty() || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
        return fallback;
    }
    char buffer[64];
    const size_t length = formatFloat(buffer, sizeof(buffer), number, 6);
    return std::string(buffer, length);
}

Status tokensJsonImpl(const fs::path& groupDir, std::string& out, std::string& error) {
    SymbolGroup group;
    const Status loaded = loadGroup(groupDir, group);
    if (loaded != Status::kOk) {
        return fail(error, loaded == Status::kNotFound ? Status::kNotFound : Status::kInvalidArgument,
                    "not a group directory: " + genericPathToUtf8(groupDir));
    }
    fs::path tokensPath;
    TokenSheet base;
    if (!group.tokensFile.empty() && resolveInside(groupDir, group.tokensFile, tokensPath)) {
        base = readTokenSheet(tokensPath);
    }
    if (!base.ok) {
        return fail(error, Status::kNotFound, "the group has no readable token file");
    }
    static const std::pair<const char*, const char*> kColors[] = {
        {"bg", "--color-bg"},
        {"panel", "--color-surface"},
        {"panelBorder", "--color-border"},
        {"text", "--color-text"},
        {"textDim", "--color-text-dim"},
        {"accent", "--color-accent"},
        {"accentText", "--color-text-on-accent"},
        {"success", "--color-success"},
        {"danger", "--color-danger"},
        {"warning", "--color-warning"},
    };
    if (base.values.count("--color-bg") == 0 || base.values.count("--color-text") == 0) {
        return fail(error, Status::kInvalidArgument, "the token file declares no --color-bg / --color-text");
    }
    // The themes: the token file, then each theme file (a token a theme leaves out keeps the base value).
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> themes;
    themes.emplace_back("base", base.values);
    for (const std::string& themeFile : group.themeFiles) {
        fs::path path;
        if (!resolveInside(groupDir, themeFile, path)) {
            continue;
        }
        const TokenSheet sheet = readTokenSheet(path);
        if (!sheet.ok) {
            continue;
        }
        std::string name = themeNameOf(themeFile);
        if (name == "night") {
            name = "dark";  // The skill's (and projects') name for the dark theme.
        }
        std::map<std::string, std::string> values = base.values;
        for (const auto& [token, value] : sheet.values) {
            values[token] = value;
        }
        themes.emplace_back(std::move(name), std::move(values));
    }

    std::string text = "{\n  \"name\": \"XTFrame game UI\",\n  \"version\": 1,\n  \"themes\": {\n";
    for (size_t t = 0; t < themes.size(); ++t) {
        text += "    " + jsonString(themes[t].first) + ": {\n";
        std::vector<std::string> lines;
        for (const auto& [key, token] : kColors) {
            const auto it = themes[t].second.find(token);
            if (it != themes[t].second.end()) {
                lines.push_back(std::string("      \"") + key + "\": " + jsonString(it->second));
            }
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            text += lines[i] + (i + 1 < lines.size() ? ",\n" : "\n");
        }
        text += std::string("    }") + (t + 1 < themes.size() ? ",\n" : "\n");
    }
    text += "  },\n  \"font\": {\n";
    const auto& values = base.values;
    auto family = [&](const char* first, const char* second) -> std::string {
        for (const char* name : {first, second}) {
            const auto it = values.find(name);
            if (it != values.end()) {
                return it->second;
            }
        }
        return std::string();
    };
    const std::string body = family("--font-body", "--font-family");
    const std::string display = family("--font-display", "--font-display-family");
    if (!body.empty()) {
        text += "    \"family\": " + jsonString(body) + ",\n";
    }
    if (!display.empty()) {
        text += "    \"displayFamily\": " + jsonString(display) + ",\n";
    }
    // The role metrics the skill's dialect fixes (title, display and the label's tracking are not
    // in a token file); the label and body sizes follow the group's scale.
    text += "    \"label\": { \"size\": " + numberText(values, "--font-size-xs", "12") +
            ", \"weight\": 700, \"letterSpacing\": 1, \"transform\": \"uppercase\" },\n";
    text +=
        "    \"body\": { \"size\": " + numberText(values, "--font-size-s", "14") + ", \"weight\": 400 },\n";
    text += "    \"title\": { \"size\": 22, \"weight\": 700 },\n";
    text += "    \"display\": { \"size\": 48, \"weight\": 700 }\n  },\n";
    // The spacing scale of the dialect, plus whatever the group's --space-* tokens add.
    std::set<int> space{4, 8, 12, 16, 20, 24, 32, 48};
    for (const auto& [token, value] : values) {
        if (token.rfind("--space-", 0) == 0) {
            const std::string number = numberText(values, token.c_str(), "");
            int parsed = 0;
            const std::from_chars_result result =
                std::from_chars(number.data(), number.data() + number.size(), parsed);
            if (!number.empty() && result.ec == std::errc() && result.ptr == number.data() + number.size() &&
                parsed > 0 && parsed < 1000) {
                space.insert(parsed);
            }
        }
    }
    text += "  \"space\": [";
    for (auto it = space.begin(); it != space.end(); ++it) {
        text += (it == space.begin() ? "" : ", ") + std::to_string(*it);
    }
    text += "],\n";
    text += "  \"radius\": { \"chip\": " + numberText(values, "--radius-s", "0") +
            ", \"button\": " + numberText(values, "--radius-m", "0") +
            ", \"panel\": " + numberText(values, "--radius-l", "0") +
            ", \"tile\": " + numberText(values, "--radius-m", "0") + " },\n";
    text += "  \"shadow\": { \"panel\": \"0 0 0 0 #00000000\" },\n";
    text += "  \"board\": { \"width\": 1280, \"height\": 720 }\n}\n";
    out = std::move(text);
    return Status::kOk;
}

/// T-500: a fresh, empty staging directory `<tempDir or system temp>/xtframe-symbols/<kind>-<seconds>[-n]`;
/// empty when none can be made.
fs::path makeStaging(const std::string& tempDir, const char* kind) {
    std::error_code ec;
    const fs::path base =
        (tempDir.empty() ? fs::temp_directory_path(ec) : pathFromUtf8(tempDir)) / "xtframe-symbols";
    if (ec) {
        return fs::path();
    }
    fs::create_directories(base, ec);
    if (ec) {
        return fs::path();
    }
    const std::string stamp = std::string(kind) + "-" + nowSeconds();
    for (int n = 1; n < 1000; ++n) {
        const fs::path dir = base / pathFromUtf8(n == 1 ? stamp : stamp + "-" + std::to_string(n));
        if (fs::create_directory(dir, ec)) {
            return dir;
        }
        if (ec) {
            return fs::path();
        }
    }
    return fs::path();
}

/// The set limits as zip limits (the set's own checks, walkChecked, apply to the unpacked tree too).
ZipReadOptions setReadLimits() {
    ZipReadOptions limits;
    limits.maxBytes = kMaxSetBytes;
    limits.maxEntries = kMaxSetFiles;
    return limits;
}

ExportReport exportSetImpl(const fs::path& groupDir, const fs::path& file, const ExportOptions& options) {
    ExportReport report;
    const fs::path staging = makeStaging(options.tempDir, "export");
    if (staging.empty()) {
        report.status = Status::kIoError;
        report.error = "cannot create a temporary directory";
        return report;
    }
    std::error_code ec;
    const fs::path setDir = staging / "set";
    report = exportImpl(groupDir, setDir, options);
    if (report.status == Status::kOk) {
        if (file.has_parent_path()) {
            fs::create_directories(file.parent_path(), ec);  // as exportGroup() creates its directory
        }
        ZipWriteOptions zipOptions;
        zipOptions.store = true;
        zipOptions.maxBytes = kMaxSetBytes;
        zipOptions.maxEntries = kMaxSetFiles;
        std::string error;
        const Status zipped = zipDirectory(setDir, file, zipOptions, &error);
        if (zipped != Status::kOk) {
            report = ExportReport();
            report.status = zipped == Status::kInvalidArgument ? Status::kInvalidArgument : Status::kIoError;
            report.error = "cannot write " + genericPathToUtf8(file) + ": " + error;
        }
    }
    fs::remove_all(staging, ec);
    return report;
}

ImportReport importSetImpl(const fs::path& file, const fs::path& libraryDir, const ImportOptions& options) {
    ImportReport report;
    const fs::path staging = makeStaging(options.tempDir, "import");
    if (staging.empty()) {
        report.status = Status::kIoError;
        report.error = "cannot create a temporary directory";
        return report;
    }
    std::error_code ec;
    const fs::path setDir = staging / "set";
    std::string error;
    const Status unzipped = unzipToDirectory(file, setDir, setReadLimits(), &error);
    if (unzipped != Status::kOk) {
        report.status = unzipped;
        report.error = genericPathToUtf8(file) + ": " + error;
    } else {
        report = importImpl(setDir, libraryDir, options);
    }
    fs::remove_all(staging, ec);
    return report;
}

}  // namespace detail::sets

Status readSetInfo(const fs::path& setDir, SetInfo& out, std::string& error) {
    out = SetInfo();
    error.clear();
    const Status status =
        detail::catchAll([&]() { return detail::sets::readSetInfoImpl(setDir, out, error); });
    if (status == Status::kInternal) {
        error = "unexpected failure while reading set.json";
    }
    return status;
}

ExportReport exportGroup(const fs::path& groupDir, const fs::path& outDir, const ExportOptions& options) {
    ExportReport report;
    const Status status = detail::catchAll([&]() {
        report = detail::sets::exportImpl(groupDir, outDir, options);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report = ExportReport();
        report.status = status;
        report.error = "unexpected failure while exporting";
    }
    return report;
}

ExportReport exportSet(const fs::path& groupDir, const fs::path& file, const ExportOptions& options) {
    ExportReport report;
    const Status status = detail::catchAll([&]() {
        report = detail::sets::exportSetImpl(groupDir, file, options);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report = ExportReport();
        report.status = status;
        report.error = "unexpected failure while exporting";
    }
    return report;
}

ImportReport importSet(const fs::path& file, const fs::path& libraryDir, const ImportOptions& options) {
    ImportReport report;
    const Status status = detail::catchAll([&]() {
        report = detail::sets::importSetImpl(file, libraryDir, options);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report = ImportReport();
        report.status = status;
        report.error = "unexpected failure while importing";
    }
    return report;
}

ImportReport importGroupDir(const fs::path& setDir, const fs::path& libraryDir,
                            const ImportOptions& options) {
    ImportReport report;
    const Status status = detail::catchAll([&]() {
        report = detail::sets::importImpl(setDir, libraryDir, options);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report = ImportReport();
        report.status = status;
        report.error = "unexpected failure while importing";
    }
    return report;
}

Status tokensJson(const fs::path& groupDir, std::string& out, std::string& error) {
    out.clear();
    error.clear();
    const Status status =
        detail::catchAll([&]() { return detail::sets::tokensJsonImpl(groupDir, out, error); });
    if (status == Status::kInternal) {
        out.clear();
        error = "unexpected failure while reading the group";
    }
    return status;
}

}  // namespace xt::bundle
