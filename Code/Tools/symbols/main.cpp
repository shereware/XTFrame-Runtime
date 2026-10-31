#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../Common/console_utf8.h"
#include "../Common/print_version.h"
#include "preview_render.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/bundle/symbol_set.h"
#include "xt/compile/utf8_path.h"

/// xtframe-symbols: symbol libraries, groups and sets on the command line (Docs/design/
/// 49-symbol-sets.md 49.4 to 49.7; Docs/tasks/T-499.md). Runtime layer: xt_bundle plus the headless
/// render of preview_render.cpp. The work happens in xt::bundle (the same code the editor service
/// will use); this file parses arguments and prints.
///
/// Usage:
///   xtframe-symbols lint <group dir | library dir> [--index-only] [--json]
///   xtframe-symbols thumbnails <group dir> [--names A,B] [--json]
///   xtframe-symbols tokens-json <group dir> [-o <file>]
///   xtframe-symbols export <group dir | group id> -o <dir | file.xtsymbols> [--author <name>]
///                          [--license <spdx>] [--no-fonts] [--library <dir>] [--json]
///   xtframe-symbols import <set dir | file.xtsymbols> [--library <dir>] [--as <group id>]
///                          [--replace-older] [--json]
///   xtframe-symbols libraries [--json]
///   xtframe-symbols mount <library dir>
///   xtframe-symbols unmount <library id>
/// Everywhere: `--user-data <dir>` (the user data directory: `%APPDATA%`, `~/Library/Application
/// Support`, `$XDG_DATA_HOME` or `~/.local/share`) and `--presets <dir>` (the preset library, default
/// `<executable dir>/symbols`).
///
/// `lint` prints one `<level> <symbol>: <code>: <message>` line per finding (`-` for the group)
/// under a `== <group id>` line per group; the codes are listed in xt/bundle/symbol_lint.h.
/// `thumbnails` renders `<group>/thumbnails/<Name>.png` from `previews/<Name>.preview.json` and
/// `thumbnails/group.png` (Button, Toggle, Slider and Modal in four 160x100 cells). `export` writes
/// a directory-form set (`set.json`, `groups/<id>/`) or, when `-o` ends in `.xtsymbols`, the
/// store-mode zip of that tree (T-500, exportSet()); `import` reads either form into a library (the
/// user library by default, which a first use creates; importSet() for a `.xtsymbols` file). Without
/// `--library` an `export <group id>` looks the group up in the preset library and the user library.
///
/// `import --url https://...` (49.7): this tool does not download. The option is validated (https
/// only) and answered with exit 2 and "use the editor or download the file first": the runtime layer
/// has no HTTP client and must not use a system tool; the editor service downloads (T-502).
///
/// Exit codes: 0 success (lint: no error, warnings are fine); 1 failure (lint errors, an unreadable
/// or invalid input, a write error); 2 argument error (usage on stderr).

namespace {

namespace fs = std::filesystem;
using nlohmann::ordered_json;
using xt::compile::genericPathToUtf8;
using xt::compile::pathFromUtf8;
using xt::compile::pathToUtf8;

void printUsage() {
    std::fprintf(stderr,
                 "usage: xtframe-symbols lint <group dir | library dir> [--index-only] [--json]\n"
                 "       xtframe-symbols thumbnails <group dir> [--names A,B] [--json]\n"
                 "       xtframe-symbols tokens-json <group dir> [-o <file>]\n"
                 "       xtframe-symbols export <group dir | group id> -o <dir | file.xtsymbols> "
                 "[--author <name>] [--license <spdx>] [--no-fonts] [--library <dir>] [--json]\n"
                 "       xtframe-symbols import <set dir | file.xtsymbols> [--library <dir>] "
                 "[--as <group id>] [--replace-older] [--json]\n"
                 "       xtframe-symbols libraries [--json]\n"
                 "       xtframe-symbols mount <library dir>\n"
                 "       xtframe-symbols unmount <library id>\n"
                 "options: --user-data <dir>  --presets <dir>\n");
}

struct Args {
    std::string command;
    std::vector<std::string> positional;
    bool json = false;
    bool indexOnly = false;
    bool noFonts = false;
    bool replaceOlder = false;
    std::string out;
    std::string names;
    std::string author;
    std::string license;
    std::string library;
    std::string asGroup;
    std::string userData;
    std::string presets;
    std::string url;  // T-500: validated only, see the file comment.
};

/// 0 when parsed; 2 on an argument error (message printed).
int parseArgs(int argc, char** argv, Args& args) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&](std::string& target) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-symbols: %s requires an argument\n", a.c_str());
                return false;
            }
            target = argv[++i];
            return true;
        };
        if (a == "--json") {
            args.json = true;
        } else if (a == "--index-only") {
            args.indexOnly = true;
        } else if (a == "--no-fonts") {
            args.noFonts = true;
        } else if (a == "--replace-older") {
            args.replaceOlder = true;
        } else if (a == "-o" || a == "--out") {
            if (!value(args.out)) {
                return 2;
            }
        } else if (a == "--names") {
            if (!value(args.names)) {
                return 2;
            }
        } else if (a == "--author") {
            if (!value(args.author)) {
                return 2;
            }
        } else if (a == "--license") {
            if (!value(args.license)) {
                return 2;
            }
        } else if (a == "--library") {
            if (!value(args.library)) {
                return 2;
            }
        } else if (a == "--as") {
            if (!value(args.asGroup)) {
                return 2;
            }
        } else if (a == "--user-data") {
            if (!value(args.userData)) {
                return 2;
            }
        } else if (a == "--presets") {
            if (!value(args.presets)) {
                return 2;
            }
        } else if (a == "--url") {
            if (!value(args.url)) {
                return 2;
            }
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "xtframe-symbols: unknown option '%s'\n", a.c_str());
            return 2;
        } else {
            args.positional.push_back(a);
        }
    }
    return 0;
}

bool needPositional(const Args& args, size_t n) {
    if (args.positional.size() != n) {
        std::fprintf(stderr, "xtframe-symbols: %s takes %zu argument(s)\n", args.command.c_str(), n);
        printUsage();
        return false;
    }
    return true;
}

void printJson(const ordered_json& j) {
    std::printf("%s\n", j.dump(2, ' ', false, ordered_json::error_handler_t::replace).c_str());
}

std::string getEnv(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return "";
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
#endif
}

/// The user data directory the user library lives under (`<dir>/XTFrame/symbols`).
fs::path userDataDir(const Args& args) {
    if (!args.userData.empty()) {
        return pathFromUtf8(args.userData);
    }
#if defined(_WIN32)
    return pathFromUtf8(getEnv("APPDATA"));
#elif defined(__APPLE__)
    return pathFromUtf8(getEnv("HOME")) / "Library" / "Application Support";
#else
    if (const std::string xdg = getEnv("XDG_DATA_HOME"); !xdg.empty()) {
        return pathFromUtf8(xdg);
    }
    return pathFromUtf8(getEnv("HOME")) / ".local" / "share";
#endif
}

/// The preset library: `--presets`, else `<executable dir>/symbols` (copied there by the build).
fs::path presetDir(const Args& args, const char* argv0) {
    if (!args.presets.empty()) {
        return pathFromUtf8(args.presets);
    }
    std::error_code ec;
    return fs::absolute(pathFromUtf8(argv0), ec).parent_path() / "symbols";
}

bool isGroupDir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "group.json", ec) || fs::is_regular_file(dir / "index.json", ec);
}

const char* levelName(xt::bundle::LintLevel level) {
    return level == xt::bundle::LintLevel::Error ? "error" : "warning";
}

ordered_json reportJson(const xt::bundle::LintReport& report) {
    ordered_json j;
    j["id"] = report.groupId;
    j["dir"] = report.dir;
    j["symbols"] = report.symbols;
    j["errors"] = report.errors();
    j["warnings"] = report.warnings();
    ordered_json issues = ordered_json::array();
    for (const xt::bundle::LintIssue& issue : report.issues) {
        ordered_json item;
        item["level"] = levelName(issue.level);
        item["symbol"] = issue.symbol;
        item["code"] = issue.code;
        item["message"] = issue.message;
        issues.push_back(std::move(item));
    }
    j["issues"] = std::move(issues);
    return j;
}

void printIssues(const xt::bundle::LintReport& report) {
    for (const xt::bundle::LintIssue& issue : report.issues) {
        std::printf("%s %s: %s: %s\n", levelName(issue.level),
                    issue.symbol.empty() ? "-" : issue.symbol.c_str(), issue.code.c_str(),
                    issue.message.c_str());
    }
}

int runLint(const Args& args) {
    xt::bundle::LintOptions options;
    options.indexOnly = args.indexOnly;
    const std::vector<xt::bundle::LintReport> reports =
        xt::bundle::lintLibrary(pathFromUtf8(args.positional[0]), options);
    size_t errors = 0;
    size_t warnings = 0;
    for (const xt::bundle::LintReport& report : reports) {
        errors += report.errors();
        warnings += report.warnings();
    }
    if (args.json) {
        ordered_json j;
        j["errors"] = errors;
        j["warnings"] = warnings;
        ordered_json groups = ordered_json::array();
        for (const xt::bundle::LintReport& report : reports) {
            groups.push_back(reportJson(report));
        }
        j["groups"] = std::move(groups);
        printJson(j);
    } else {
        for (const xt::bundle::LintReport& report : reports) {
            std::printf("== %s (%zu symbols)\n",
                        report.groupId.empty() ? report.dir.c_str() : report.groupId.c_str(), report.symbols);
            printIssues(report);
        }
        std::printf("lint: %zu error(s), %zu warning(s) in %zu group(s)\n", errors, warnings, reports.size());
    }
    return errors == 0 ? 0 : 1;
}

std::vector<std::string> splitNames(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string name =
            text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!name.empty()) {
            out.push_back(name);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

int runThumbnails(const Args& args) {
    const fs::path dir = pathFromUtf8(args.positional[0]);
    xt::bundle::SymbolGroup group;
    if (const xt::Status status = xt::bundle::loadGroup(dir, group); status != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: '%s' is not a group directory\n", args.positional[0].c_str());
        return 1;
    }
    const std::vector<std::string> names = splitNames(args.names);
    std::vector<xt::compile::Diagnostic> problems;
    const std::vector<xt::bundle::PreviewSpec> specs = xt::bundle::thumbnailSpecs(dir, names, &problems);
    int failures = 0;
    ordered_json written = ordered_json::array();
    ordered_json failed = ordered_json::array();
    auto fail = [&](const std::string& what, const std::string& why) {
        ++failures;
        failed.push_back(what + ": " + why);
        if (!args.json) {
            std::fprintf(stderr, "xtframe-symbols: %s: %s\n", what.c_str(), why.c_str());
        }
    };
    for (const xt::compile::Diagnostic& d : problems) {
        fail(d.file, d.message);
    }
    for (const std::string& name : names) {
        if (std::none_of(specs.begin(), specs.end(), [&](const auto& spec) { return spec.name == name; })) {
            fail(name, "no previews/" + name + ".preview.json");
        }
    }
    for (const xt::bundle::PreviewSpec& spec : specs) {
        xt::tools::symbols::Image image;
        std::string error;
        if (!xt::tools::symbols::renderPreview(dir, spec, image, error)) {
            fail(spec.name, error);
            continue;
        }
        const std::string relative = "thumbnails/" + spec.name + ".png";
        if (!xt::tools::symbols::writePng(dir / pathFromUtf8(relative), image)) {
            fail(spec.name, "cannot write " + relative);
            continue;
        }
        written.push_back(relative);
        if (!args.json) {
            std::printf("wrote %s (%dx%d)\n", relative.c_str(), image.width, image.height);
        }
    }

    // The group thumbnail: Button, Toggle, Slider and Modal (then the first symbols of the index
    // when the group lacks one of them), from the thumbnails on disk.
    std::vector<std::string> cellNames;
    for (const char* preferred : {"Button", "Toggle", "Slider", "Modal"}) {
        cellNames.emplace_back(preferred);
    }
    std::vector<xt::tools::symbols::Image> cells;
    std::vector<std::string> used;
    auto addCell = [&](const std::string& name) {
        xt::tools::symbols::Image image;
        if (cells.size() < 4 && std::find(used.begin(), used.end(), name) == used.end() &&
            xt::tools::symbols::readPng(dir / "thumbnails" / pathFromUtf8(name + ".png"), image)) {
            cells.push_back(std::move(image));
            used.push_back(name);
        }
    };
    for (const std::string& name : cellNames) {
        addCell(name);
    }
    for (const xt::bundle::SymbolEntry& entry : group.symbols) {
        addCell(entry.name);
    }
    xt::tools::symbols::Image composite;
    if (cells.empty()) {
        fail("group.png", "no symbol thumbnail to compose");
    } else if (!xt::tools::symbols::composeGroupThumbnail(cells, 320, 200, composite) ||
               !xt::tools::symbols::writePng(dir / "thumbnails" / "group.png", composite)) {
        fail("group.png", "cannot write thumbnails/group.png");
    } else {
        written.push_back("thumbnails/group.png");
        if (!args.json) {
            std::printf("wrote thumbnails/group.png (320x200) from %zu thumbnail(s)\n", cells.size());
        }
    }
    if (args.json) {
        ordered_json j;
        j["written"] = std::move(written);
        j["failed"] = std::move(failed);
        printJson(j);
    }
    return failures == 0 ? 0 : 1;
}

int runTokensJson(const Args& args) {
    std::string text;
    std::string error;
    if (const xt::Status status = xt::bundle::tokensJson(pathFromUtf8(args.positional[0]), text, error);
        status != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: tokens-json: %s\n", error.c_str());
        return 1;
    }
    if (args.out.empty()) {
#ifdef _WIN32
        // The same bytes as -o writes: bare LF line ends on every platform.
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    std::error_code ec;
    const fs::path out = pathFromUtf8(args.out);
    if (out.has_parent_path()) {
        fs::create_directories(out.parent_path(), ec);
    }
    std::ofstream file(out, std::ios::binary);
    if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        std::fprintf(stderr, "xtframe-symbols: cannot write '%s'\n", args.out.c_str());
        return 1;
    }
    std::printf("wrote %s\n", args.out.c_str());
    return 0;
}

/// The directory of the group `spec` names: a group directory, or a group id of `--library`, else of
/// the preset library and the user library. Empty when there is none.
fs::path resolveGroup(const Args& args, const char* argv0, const std::string& spec) {
    const fs::path asPath = pathFromUtf8(spec);
    if (isGroupDir(asPath)) {
        return asPath;
    }
    std::vector<fs::path> libraries;
    if (!args.library.empty()) {
        libraries.push_back(pathFromUtf8(args.library));
    } else {
        libraries.push_back(presetDir(args, argv0));
        libraries.push_back(userDataDir(args) / "XTFrame" / "symbols");
    }
    for (const fs::path& dir : libraries) {
        xt::bundle::SymbolLibrary library;
        if (xt::bundle::loadLibrary(dir, library) != xt::Status::kOk) {
            continue;
        }
        for (const xt::bundle::SymbolGroup& group : library.groups) {
            if (group.id == spec && !group.dir.empty()) {
                return pathFromUtf8(group.dir);
            }
        }
    }
    return fs::path();
}

/// T-500: `path` names a `.xtsymbols` archive (by extension).
bool isSetArchivePath(const std::string& path) { return pathFromUtf8(path).extension() == ".xtsymbols"; }

int runExport(const Args& args, const char* argv0) {
    if (args.out.empty()) {
        std::fprintf(stderr, "xtframe-symbols: export needs -o <dir | file.xtsymbols>\n");
        printUsage();
        return 2;
    }
    const fs::path group = resolveGroup(args, argv0, args.positional[0]);
    if (group.empty()) {
        std::fprintf(stderr, "xtframe-symbols: no group '%s'\n", args.positional[0].c_str());
        return 1;
    }
    xt::bundle::ExportOptions options;
    options.author = args.author;
    options.license = args.license;
    options.fonts = !args.noFonts;
    const xt::bundle::ExportReport report =
        isSetArchivePath(args.out) ? xt::bundle::exportSet(group, pathFromUtf8(args.out), options)
                                   : xt::bundle::exportGroup(group, pathFromUtf8(args.out), options);
    if (report.status != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: export: %s\n", report.error.c_str());
        return 1;
    }
    if (args.json) {
        ordered_json j;
        j["id"] = report.set.id;
        j["author"] = report.set.author;
        j["setVersion"] = report.set.setVersion;
        j["license"] = report.set.license;
        j["fontsLicense"] = report.set.fontsLicense;
        j[isSetArchivePath(args.out) ? "file" : "dir"] = args.out;
        j["files"] = report.files;
        printJson(j);
    } else {
        std::printf("exported %s/%s %s (license %s) to %s: %zu file(s)\n", report.set.author.c_str(),
                    report.set.id.c_str(), report.set.setVersion.c_str(), report.set.license.c_str(),
                    args.out.c_str(), report.files.size());
    }
    return 0;
}

/// The library `import` writes to: `--library`, else the user library, created (with a copy of the
/// presets' default group, 49.2) at the first use.
fs::path importTarget(const Args& args, const char* argv0) {
    if (!args.library.empty()) {
        return pathFromUtf8(args.library);
    }
    const fs::path userData = userDataDir(args);
    const fs::path mine = userData / "XTFrame" / "symbols";
    std::error_code ec;
    if (!fs::is_regular_file(mine / "library.json", ec)) {
        xt::bundle::LibraryMounts mounts;
        xt::bundle::mountDefaults(mounts, presetDir(args, argv0), userData);
        if (!fs::is_regular_file(mine / "library.json", ec)) {
            fs::create_directories(mine / "groups", ec);
            xt::bundle::SymbolLibrary library;
            library.id = "mine";
            library.name = "My symbols";
            library.dir = genericPathToUtf8(mine);
            xt::bundle::saveLibraryJson(library);
        }
    }
    return mine;
}

const char* actionName(xt::bundle::ImportAction action) {
    switch (action) {
        case xt::bundle::ImportAction::Added:
            return "added";
        case xt::bundle::ImportAction::Updated:
            return "updated";
        case xt::bundle::ImportAction::Unchanged:
            return "unchanged";
        case xt::bundle::ImportAction::OlderSkipped:
            return "older-skipped";
        case xt::bundle::ImportAction::OlderReplaced:
            return "older-replaced";
    }
    return "added";
}

int runImport(const Args& args, const char* argv0) {
    const fs::path target = importTarget(args, argv0);
    xt::bundle::ImportOptions options;
    options.asGroup = args.asGroup;
    options.replaceOlder = args.replaceOlder;
    const fs::path input = pathFromUtf8(args.positional[0]);
    std::error_code ec;
    const xt::bundle::ImportReport report =
        fs::is_regular_file(input, ec) && isSetArchivePath(args.positional[0])
            ? xt::bundle::importSet(input, target, options)
            : xt::bundle::importGroupDir(input, target, options);
    if (report.status != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: import: %s\n", report.error.c_str());
        return 1;
    }
    size_t lintErrors = 0;
    if (args.json) {
        ordered_json j;
        j["set"] = report.set.author + "/" + report.set.id;
        j["setVersion"] = report.set.setVersion;
        j["license"] = report.set.license;
        j["fontsLicense"] = report.set.fontsLicense;
        j["library"] = genericPathToUtf8(target);
        ordered_json groups = ordered_json::array();
        for (const xt::bundle::ImportedGroup& group : report.groups) {
            ordered_json item;
            item["source"] = group.sourceId;
            item["target"] = group.targetId;
            item["action"] = actionName(group.action);
            item["trashed"] = group.trashed;
            item["lint"] = reportJson(group.lint);
            groups.push_back(std::move(item));
            lintErrors += group.lint.errors();
        }
        j["groups"] = std::move(groups);
        printJson(j);
        return 0;
    }
    std::printf("set %s/%s %s, license %s%s%s\n", report.set.author.c_str(), report.set.id.c_str(),
                report.set.setVersion.c_str(), report.set.license.c_str(),
                report.set.fontsLicense.empty() ? "" : ", fonts ", report.set.fontsLicense.c_str());
    for (const xt::bundle::ImportedGroup& group : report.groups) {
        std::printf("%s %s -> %s%s%s\n", actionName(group.action), group.sourceId.c_str(),
                    group.targetId.c_str(), group.trashed.empty() ? "" : " (old one in ",
                    group.trashed.empty() ? "" : (group.trashed + ")").c_str());
        printIssues(group.lint);
        lintErrors += group.lint.errors();
        if (group.action == xt::bundle::ImportAction::OlderSkipped) {
            std::printf("note: the library has a newer version of %s; --replace-older replaces it\n",
                        group.sourceId.c_str());
        }
    }
    if (lintErrors != 0) {
        std::printf(
            "imported with %zu lint error(s): fix them before dragging these symbols into a project\n",
            lintErrors);
    }
    return 0;
}

void mountAll(const Args& args, const char* argv0, xt::bundle::LibraryMounts& mounts) {
    xt::bundle::mountDefaults(mounts, presetDir(args, argv0), userDataDir(args));
    xt::bundle::loadMounts(userDataDir(args), mounts);
}

const char* scopeName(xt::bundle::LibraryScope scope) {
    switch (scope) {
        case xt::bundle::LibraryScope::Preset:
            return "preset";
        case xt::bundle::LibraryScope::User:
            return "user";
        case xt::bundle::LibraryScope::Project:
            return "project";
        case xt::bundle::LibraryScope::Extra:
            break;
    }
    return "extra";
}

int runLibraries(const Args& args, const char* argv0) {
    xt::bundle::LibraryMounts mounts;
    mountAll(args, argv0, mounts);
    if (args.json) {
        ordered_json j = ordered_json::array();
        for (const xt::bundle::SymbolLibrary& library : mounts.libraries) {
            ordered_json item;
            item["id"] = library.id;
            item["name"] = library.name;
            item["dir"] = library.dir;
            item["scope"] = scopeName(library.scope);
            item["readOnly"] = library.readOnly;
            item["default"] = library.defaultGroup;
            ordered_json groups = ordered_json::array();
            for (const xt::bundle::SymbolGroup& group : library.groups) {
                ordered_json g;
                g["id"] = group.id;
                g["name"] = group.name;
                g["symbols"] = group.symbols.size();
                g["problems"] = group.problems.size();
                groups.push_back(std::move(g));
            }
            item["groups"] = std::move(groups);
            j.push_back(std::move(item));
        }
        printJson(j);
        return 0;
    }
    for (const xt::bundle::SymbolLibrary& library : mounts.libraries) {
        std::printf("%s\t%s\t%s\t%s\n", library.id.c_str(), scopeName(library.scope),
                    library.readOnly ? "read-only" : "writable", library.dir.c_str());
        for (const xt::bundle::SymbolGroup& group : library.groups) {
            std::printf("  %s\t%s\t%zu symbols%s\n", group.id.c_str(), group.name.c_str(),
                        group.symbols.size(), group.problems.empty() ? "" : "\t(has problems)");
        }
    }
    for (const xt::bundle::MountEntry& entry : mounts.entries) {
        const bool mounted = std::any_of(
            mounts.libraries.begin(), mounts.libraries.end(), [&](const xt::bundle::SymbolLibrary& library) {
                return library.dir == genericPathToUtf8(pathFromUtf8(entry.path).lexically_normal());
            });
        if (!mounted) {
            std::printf("not mounted\t%s%s\n", entry.path.c_str(), entry.enabled ? "" : "\t(disabled)");
        }
    }
    return 0;
}

int runMount(const Args& args, const char* argv0) {
    (void)argv0;
    xt::bundle::LibraryMounts mounts;
    xt::bundle::loadMounts(userDataDir(args), mounts);  // kNotFound: the first mount.
    const fs::path dir = pathFromUtf8(args.positional[0]);
    if (const xt::Status status = xt::bundle::mount(mounts, dir); status != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: '%s' is not a symbol library (library.json %s)\n",
                     args.positional[0].c_str(), status == xt::Status::kNotFound ? "missing" : "invalid");
        return 1;
    }
    if (xt::bundle::saveMounts(userDataDir(args), mounts) != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: cannot write the mount list\n");
        return 1;
    }
    std::printf("mounted %s\n", args.positional[0].c_str());
    return 0;
}

int runUnmount(const Args& args) {
    xt::bundle::LibraryMounts mounts;
    if (xt::bundle::loadMounts(userDataDir(args), mounts) != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: no library '%s' is mounted\n", args.positional[0].c_str());
        return 1;
    }
    if (!xt::bundle::unmount(mounts, args.positional[0])) {
        std::fprintf(stderr, "xtframe-symbols: no library '%s' is mounted\n", args.positional[0].c_str());
        return 1;
    }
    if (xt::bundle::saveMounts(userDataDir(args), mounts) != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-symbols: cannot write the mount list\n");
        return 1;
    }
    std::printf("unmounted %s\n", args.positional[0].c_str());
    return 0;
}

int symbolsMain(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--version") {
        xt::tools::printVersion("xtframe-symbols", stdout);
        return 0;
    }
    if (argc >= 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        printUsage();
        return 0;
    }
    Args args;
    if (const int rc = parseArgs(argc, argv, args); rc != 0) {
        return rc;
    }
    const std::string& c = args.command;
    if (!args.url.empty()) {  // T-500: validated, never downloaded here (see the file comment).
        if (c != "import") {
            std::fprintf(stderr, "xtframe-symbols: --url belongs to import\n");
            return 2;
        }
        if (args.url.rfind("https://", 0) != 0) {
            std::fprintf(stderr, "xtframe-symbols: --url accepts https:// addresses only\n");
            return 2;
        }
        std::fprintf(stderr,
                     "xtframe-symbols: import --url: this tool does not download; use the editor or download "
                     "the file first, then import the .xtsymbols file\n");
        return 2;
    }
    if (c == "lint" || c == "thumbnails" || c == "tokens-json" || c == "export" || c == "import" ||
        c == "mount" || c == "unmount") {
        if (!needPositional(args, 1)) {
            return 2;
        }
    } else if (c == "libraries") {
        if (!needPositional(args, 0)) {
            return 2;
        }
    } else {
        std::fprintf(stderr, "xtframe-symbols: unknown command '%s'\n", c.c_str());
        printUsage();
        return 2;
    }
    if (c == "lint") {
        return runLint(args);
    }
    if (c == "thumbnails") {
        return runThumbnails(args);
    }
    if (c == "tokens-json") {
        return runTokensJson(args);
    }
    if (c == "export") {
        return runExport(args, argv[0]);
    }
    if (c == "import") {
        return runImport(args, argv[0]);
    }
    if (c == "libraries") {
        return runLibraries(args, argv[0]);
    }
    if (c == "mount") {
        return runMount(args, argv[0]);
    }
    return runUnmount(args);
}

}  // namespace

// MSVC only, as xtframe-pack: the arguments as UTF-8 through wmain (T-237, T-261f).
#if defined(_MSC_VER)
int wmain(int argc, wchar_t** wargv) {
    xt::tools::useUtf8Console();
    xt::tools::Utf8Args args(argc, wargv);
    return symbolsMain(args.argc(), args.argv());
}
#else
int main(int argc, char** argv) {
    xt::tools::useUtf8Console();
    return symbolsMain(argc, argv);
}
#endif
