#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../Common/console_utf8.h"
#include "../Common/print_version.h"
#include "xt/bundle/asset_meta.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/design_doc.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/gen_ports.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/utf8_path.h"

/// xtframe-pack: turns a bundle's source form into its published form. See
/// Docs/design/22-bundle.md §22.3/§22.4, Docs/tasks/T-92.md. All the work happens in
/// xt::bundle::pack(); this file only parses arguments and prints diagnostics.
///
/// Usage: xtframe-pack <project-dir | name.xtui | ui-project.json | bundle.xtproj | file.xt> [-o <out-dir>]
///                   [--ui <id>]... [--strict] [--no-thumbs] [--embed] [--prune] [--lang <list>]
///                   [--allow-placeholders]
///        xtframe-pack --upgrade <bundle-dir>
///        xtframe-pack --version
///
/// --version (T-420) prints the version lines (Code/Tools/Common/print_version.h) to stdout and exits 0,
/// whatever else is on the command line.
///
/// T-183 (Docs/design/30-ui-project.md, 22-bundle.md §22.10, 12-compiler.md §12.7): a directory
/// is read as a UI Project (T-214: its `<name>.xtui`, else a legacy `ui-project.json`, else
/// `bundle.xtproj`, see xt::bundle::UiProject). `-o` may be omitted when the `.xtui`'s
/// `publish.outDir` is set (relative to the project
/// root); an absent `--lang` falls back to `publish.languages`. `--upgrade <dir>` turns a
/// `bundle.xtproj` directory into a UI Project in place (upgradeToUiProject()) and packs nothing.
///
/// --bake-vectors (T-226) moved to the editor in T-274 (Docs/RELEASE-MAP.md §6.1: xtframe-pack is
/// runtime layer and does not link the editor core). The option is kept only to say so: it packs
/// nothing and exits 2. Bake with `xtframe-editor --bake-vectors <project> -o <dir>` and pack that
/// copy, or save the project in the editor. An unbaked `<xt-offset>` / `<xt-merge>` is packed
/// disabled, with the compiler's `vector.unbakedModifier` warning (a failure under --strict), which
/// this tool prints with a bake hint appended.
///
/// --lang (T-179, Docs/design/12-compiler.md §12.7): a comma-separated list of languages; only
/// their `strings/<lang>.csv` tables are packed (and linted). May be repeated.
///
/// --gen-ports cpp|cs <out> (T-193, Docs/design/23-host-integration.md §23.10 item 1): after a
/// successful pack, writes a strongly typed port header for every UI's declared ports
/// (xt::bundle::generatePortHeader()) to `<out>`. Needs a full pack: combined with `--ui` it exits
/// 2, since only the re-packed UIs' tables would be known. A global flow (`bundle.xtflow` at the
/// project root or under `shared/`) is validated and packed automatically (manifest `flows.global`).
///
/// T-198 (Docs/design/30-ui-project.md 30.4): the standard symbol library is read from
/// `<executable dir>/symbols` (copied there by the build); a UI's `<link rel="symbols">` to a
/// missing `<shared.symbols>/<Name>.xt` gets that standard file copied into the project first
/// (PackOptions::standardSymbolDir), one `copied standard symbol` line each.
///
/// A lone `.xt` file is packed as the implied single-UI bundle of §22.2
/// (ProjectFile::implied), so the same command works before a project has a `bundle.xtproj`.
///
/// Exit codes: 0 success, 1 a pack/compile error, 2 argument error (usage printed to stderr).
/// Diagnostics use xtframe-compile's `file:line:col: level: message` format, with the UI id
/// prefixed to `file` by pack() itself.
///
/// Not covered by Docs/tasks/T-92.md (conservative choices made here):
///   - `-o` is required unless the UI Project names `publish.outDir`; an unknown option, a missing
///     output directory, more than one input, or a `--ui` id the project does not list all exit 2.
///   - `--upgrade` takes no other option; a failed upgrade exits 1.
///   - The input may name the `.xtui`, `ui-project.json` or `bundle.xtproj` directly; its parent
///     directory is used as the project root, so tab-completing the file works.
///
/// T-309 (Docs/design/35-design-system.md §35.4, §35.10): when the project has a design directory,
/// `design.draftShipped` and `design.docStale` of xt::bundle::design::lintDocument() are printed as
/// `xtframe-pack: warning: ...` lines. They are checked right after pack() has pulled the standard
/// symbols into the project (the state DESIGN.md is generated in, so a fresh checkout of the starter
/// does not report it stale), before the compile diagnostics. They never stop the pack, change the
/// exit code or add a manifest field; a design directory that does not load is one warning line too.
///
/// T-312a (Docs/design/35-design-system.md §35.15.1, 40 §40.2.1): when the UI Project has asset
/// identity (xt::bundle::assets::assetsUpgraded(): a design directory, or a `.meta` under the asset
/// root), `manifest/assets.yaml` is regenerated from the asset metas before the pack
/// (regenerateAssetsManifest(); metas are created for assets without one). A project without either,
/// a legacy `bundle.xtproj` and a lone `.xt` keep the old behaviour. A failed regeneration exits 1
/// before anything is packed. An asset with status `placeholder` is an error
/// (`asset.placeholderShipped`, exit 1) unless `--allow-placeholders` is given, which packs it with a
/// warning.

namespace {

namespace fs = std::filesystem;
using xt::compile::genericPathToUtf8;
using xt::compile::pathFromUtf8;
using xt::compile::pathToUtf8;

void printUsage() {
    std::fprintf(stderr,
                 "usage: xtframe-pack <project-dir | name.xtui | ui-project.json | bundle.xtproj | file.xt> "
                 "[-o <out-dir>] "
                 "[--ui <id>]... [--strict] [--no-thumbs] [--embed] [--prune] [--lang <list>] "
                 "[--gen-ports cpp|cs <out>] [--atlas] [--atlas-scope ui|bundle] "
                 "[--compress bc7|bc3|bc1|none] [--font-subset] [--allow-placeholders]\n"
                 "       xtframe-pack --upgrade <bundle-dir>\n"
                 "       xtframe-pack --version\n");
}

/// T-274: appended to the compiler's `vector.unbakedModifier` message, since the pack does not bake.
constexpr const char* kBakeHint =
    " (bake it: save the project in the editor, or xtframe-editor --bake-vectors)";

void printDiagnostic(const xt::compile::Diagnostic& d) {
    const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
    const std::string& file = d.file.empty() ? std::string("<input>") : d.file;
    std::string message = d.message;
    if (message.rfind("vector.unbakedModifier", 0) == 0) {
        message += kBakeHint;
    }
    if (d.line == 0 && d.col == 0) {
        std::fprintf(stderr, "%s: %s: %s\n", file.c_str(), level, message.c_str());
    } else {
        std::fprintf(stderr, "%s:%d:%d: %s: %s\n", file.c_str(), d.line, d.col, level, message.c_str());
    }
}

/// T-309: the design lint a pack reports (see the file comment).
void designWarnings(const xt::bundle::UiProject& project, const std::string& standardSymbolDir) {
    namespace design = xt::bundle::design;
    std::error_code ec;
    if (project.isUnnamed() || !fs::is_directory(pathFromUtf8(design::designDir(project)), ec)) {
        return;
    }
    design::DesignSystem ds;
    design::Graph graph;
    std::string error;
    if (design::load(project, ds, nullptr, &error) != xt::Status::kOk ||
        design::buildGraph(project, ds, graph, &error) != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-pack: warning: design: %s\n", error.c_str());
        return;
    }
    design::BuildOptions options;
    options.standardSymbolDir = standardSymbolDir;
    for (const design::Issue& issue : design::lintDocument(project, ds, graph, options)) {
        if (issue.id != "design.draftShipped" && issue.id != "design.docStale") {
            continue;
        }
        std::fprintf(stderr, "xtframe-pack: warning: %s: %s%s%s\n", issue.id.c_str(), issue.entry.c_str(),
                     issue.entry.empty() ? "" : ": ", issue.message.c_str());
    }
}

}  // namespace

/// `--upgrade <dir>`: see the file comment.
int runUpgrade(const std::string& dir) {
    xt::bundle::UpgradeReport report;
    std::string error;
    const xt::Status status = xt::bundle::upgradeToUiProject(dir, &report, &error);
    if (status != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-pack: --upgrade: %s\n", error.c_str());
        return 1;
    }
    for (const std::string& created : report.created) {
        std::printf("xtframe-pack: created %s\n", created.c_str());
    }
    std::printf("xtframe-pack: upgraded '%s' to a UI Project (bundle.xtproj kept)\n", dir.c_str());
    return 0;
}

int packMain(int argc, char** argv) {
    std::string input;
    xt::bundle::PackOptions options;
    bool sawInput = false;
    bool genPorts = false;
    xt::bundle::PortsLanguage portsLanguage = xt::bundle::PortsLanguage::Cpp;
    std::string portsOut;

    // T-420: --version wins over every other argument.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            xt::tools::printVersion("xtframe-pack", stdout);
            return 0;
        }
    }

    if (argc >= 2 && std::string(argv[1]) == "--upgrade") {
        if (argc != 3) {
            printUsage();
            return 2;
        }
        return runUpgrade(argv[2]);
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--upgrade") {
            std::fprintf(stderr, "xtframe-pack: --upgrade takes no other option\n");
            printUsage();
            return 2;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-pack: -o requires an argument\n");
                printUsage();
                return 2;
            }
            options.outDir = argv[++i];
        } else if (arg == "--ui") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-pack: --ui requires an argument\n");
                printUsage();
                return 2;
            }
            options.onlyUis.emplace_back(argv[++i]);
        } else if (arg == "--lang") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-pack: --lang requires an argument\n");
                printUsage();
                return 2;
            }
            const std::string list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const std::string lang =
                    list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (lang.empty()) {
                    std::fprintf(stderr, "xtframe-pack: --lang: empty language in '%s'\n", list.c_str());
                    printUsage();
                    return 2;
                }
                options.languages.push_back(lang);
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (arg == "--gen-ports") {
            if (i + 2 >= argc) {
                std::fprintf(stderr,
                             "xtframe-pack: --gen-ports requires a language (cpp|cs) and an output file\n");
                printUsage();
                return 2;
            }
            if (!xt::bundle::parsePortsLanguage(argv[i + 1], portsLanguage)) {
                std::fprintf(stderr, "xtframe-pack: --gen-ports: unknown language '%s' (cpp|cs)\n",
                             argv[i + 1]);
                printUsage();
                return 2;
            }
            portsOut = argv[i + 2];
            genPorts = true;
            i += 2;
        } else if (arg == "--atlas") {
            options.atlas = true;  // T-199 (26.3.4, 12.7)
        } else if (arg == "--atlas-scope") {
            if (i + 1 >= argc || (std::string(argv[i + 1]) != "ui" && std::string(argv[i + 1]) != "bundle")) {
                std::fprintf(stderr, "xtframe-pack: --atlas-scope requires ui or bundle\n");
                printUsage();
                return 2;
            }
            options.atlasScope = argv[++i];
        } else if (arg == "--compress") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-pack: --compress requires bc7, bc3, bc1 or none\n");
                printUsage();
                return 2;
            }
            const std::string format = argv[++i];
            if (format != "bc7" && format != "bc3" && format != "bc1" && format != "none") {
                std::fprintf(stderr, "xtframe-pack: --compress: unknown format '%s' (bc7|bc3|bc1|none)\n",
                             format.c_str());
                printUsage();
                return 2;
            }
            options.compress = format;
        } else if (arg == "--font-subset") {
            options.fontSubset = true;
        } else if (arg == "--allow-placeholders") {
            options.allowPlaceholders = true;  // T-312a
        } else if (arg == "--strict") {
            options.strict = true;
        } else if (arg == "--no-thumbs") {
            options.thumbnails = false;
        } else if (arg == "--embed") {
            options.embed = true;
        } else if (arg == "--prune") {
            options.prune = true;
        } else if (arg == "--bake-vectors") {
            // T-274: moved to the editor; nothing is packed.
            std::fprintf(
                stderr,
                "xtframe-pack: --bake-vectors moved to the editor: run \"xtframe-editor --bake-vectors "
                "<project> -o <dir>\" and pack that copy, or save the project in the editor (it bakes "
                "on save)\n");
            return 2;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "xtframe-pack: unknown option '%s'\n", arg.c_str());
            printUsage();
            return 2;
        } else if (sawInput) {
            std::fprintf(stderr, "xtframe-pack: more than one input given\n");
            printUsage();
            return 2;
        } else {
            input = arg;
            sawInput = true;
        }
    }

    if (!sawInput) {
        printUsage();
        return 2;
    }
    if (genPorts && !options.onlyUis.empty()) {
        std::fprintf(stderr, "xtframe-pack: --gen-ports needs a full pack (no --ui)\n");
        printUsage();
        return 2;
    }
    if ((options.atlas || options.fontSubset) && !options.onlyUis.empty()) {
        std::fprintf(stderr, "xtframe-pack: --atlas / --font-subset need a full pack (no --ui)\n");
        printUsage();
        return 2;
    }

    const fs::path inputPath = pathFromUtf8(input);
    xt::bundle::UiProject project;
    if (inputPath.extension() == ".xt") {
        project = xt::bundle::UiProject::implied(input);
    } else {
        // T-214: UiProject::load() takes the directory or the root file itself.
        std::string error;
        xt::Result<xt::bundle::UiProject> loaded = xt::bundle::UiProject::load(input, &error);
        if (!loaded.ok()) {
            std::fprintf(stderr, "xtframe-pack: %s\n", error.c_str());
            return 2;
        }
        project = std::move(loaded.value());
    }

    if (options.outDir.empty() && !project.publish.outDir.empty()) {
        options.outDir =
            genericPathToUtf8(pathFromUtf8(project.rootDir) / pathFromUtf8(project.publish.outDir));
    }
    if (options.outDir.empty()) {
        std::fprintf(stderr, "xtframe-pack: no output directory (-o, or publish.outDir in the .xtui)\n");
        printUsage();
        return 2;
    }

    for (const std::string& id : options.onlyUis) {
        if (project.ui(id) == nullptr) {
            std::fprintf(stderr, "xtframe-pack: no UI with id '%s'\n", id.c_str());
            return 2;
        }
    }

    // T-198 (Docs/design/30-ui-project.md 30.4, 30.8): the standard symbol library ships next to
    // the executable (`<applicationDirPath>/symbols`).
    {
        std::error_code ec;
        const fs::path symbols = fs::absolute(pathFromUtf8(argv[0]), ec).parent_path() / "symbols";
        if (fs::is_directory(symbols, ec)) {
            options.standardSymbolDir = pathToUtf8(symbols);
        }
    }
    // T-312a: the asset manifest is generated from the metas once the project has asset identity.
    if (inputPath.extension() != ".xt" && !project.legacy && xt::bundle::assets::assetsUpgraded(project)) {
        xt::bundle::AssetsManifest generated;
        xt::bundle::assets::RegenerateReport regenerated;
        std::string error;
        if (xt::bundle::assets::regenerateAssetsManifest(project, generated, &regenerated, &error) !=
            xt::Status::kOk) {
            std::fprintf(stderr, "xtframe-pack: error: assets: %s\n", error.c_str());
            std::fprintf(stderr, "xtframe-pack: failed to pack '%s'\n", input.c_str());
            return 1;
        }
        for (const std::string& warning : regenerated.warnings) {
            std::fprintf(stderr, "xtframe-pack: warning: assets: %s\n", warning.c_str());
        }
        if (regenerated.written) {
            std::printf("xtframe-pack: regenerated %s (%zu asset(s), %d new meta(s))\n",
                        xt::bundle::kAssetsManifestPath, generated.assets.size(), regenerated.metasCreated);
        }
    }
    const xt::bundle::PackReport report = xt::bundle::pack(project, options);
    for (const std::string& copied : report.standardSymbolsCopied) {
        std::printf("xtframe-pack: copied standard symbol %s\n", copied.c_str());
    }
    designWarnings(project, options.standardSymbolDir);
    for (const xt::compile::Diagnostic& d : report.diagnostics) {
        printDiagnostic(d);
    }
    if (!report.ok) {
        std::fprintf(stderr, "xtframe-pack: failed to pack '%s'\n", input.c_str());
        return 1;
    }

    std::printf("xtframe-pack: wrote %zu UI(s) to %s (%u asset(s), %u deduplicated, %u pruned)\n",
                report.packedUis.size(), options.outDir.c_str(), report.assetsWritten,
                report.assetsDeduplicated, report.assetsPruned);
    if (report.atlasPages != 0) {  // T-199
        const double efficiency = report.atlasPixelsTotal == 0
                                      ? 0.0
                                      : 100.0 * static_cast<double>(report.atlasPixelsUsed) /
                                            static_cast<double>(report.atlasPixelsTotal);
        std::printf("xtframe-pack: atlas: %u page(s), %u entr%s, %.1f%% of the page pixels used\n",
                    report.atlasPages, report.atlasEntries, report.atlasEntries == 1 ? "y" : "ies",
                    efficiency);
    }
    if (report.assetsCompressed != 0) {
        std::printf("xtframe-pack: compressed %u image(s) to DDS (%s)\n", report.assetsCompressed,
                    options.compress.c_str());
    }
    if (report.variants3x != 0) {
        std::printf("xtframe-pack: packed %u @3x variant(s)\n", report.variants3x);  // T-345
    }
    if (report.variants2x != 0) {
        std::printf("xtframe-pack: packed %u @2x variant(s)\n", report.variants2x);
    }
    if (report.fontsSubset != 0) {
        std::printf("xtframe-pack: subset %u font(s)\n", report.fontsSubset);
    }
    if (report.assetsManifestUpdated) {
        std::printf("xtframe-pack: filled measured fields in %s/manifest/assets.yaml\n",
                    project.rootDir.c_str());
    }
    if (report.globalFlowWritten) {
        std::printf("xtframe-pack: packed global flow bundle.xtflow\n");
    }
    if (genPorts) {
        const std::string text = xt::bundle::generatePortHeader(portsLanguage, report.portTables);
        std::ofstream file(pathFromUtf8(portsOut), std::ios::binary);
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
            std::fprintf(stderr, "xtframe-pack: --gen-ports: cannot write '%s'\n", portsOut.c_str());
            return 1;
        }
        std::printf("xtframe-pack: wrote port header %s\n", portsOut.c_str());
    }
    return 0;
}

// MSVC only: MinGW calls `wmain` only when linked with `-municode`, which this build does not pass,
// so there the narrow `main` below stays the entry point.
#if defined(_MSC_VER)
/// T-237: the arguments as UTF-8 (the narrow `argv` of `main` is in the ANSI code page, which
/// cannot carry every project path). T-261e: a plain UTF-16 -> UTF-8 text conversion, not a path
/// one, since not every argument is a path.
int wmain(int argc, wchar_t** wargv) {
    xt::tools::useUtf8Console();
    xt::tools::Utf8Args args(argc, wargv);  // T-261f
    return packMain(args.argc(), args.argv());
}
#else
int main(int argc, char** argv) {
    xt::tools::useUtf8Console();
    return packMain(argc, argv);
}
#endif
