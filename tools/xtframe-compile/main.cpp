#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "../common/console_utf8.h"
#include "../common/standard_symbols_note.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/utf8_path.h"
#include "xt/ir.h"

/// xtframe-compile: CLI front-end for xt::compile::compileLottie (`.json`) / compileDocument (`.xt`).
/// See docs/tasks/T-17.md "xtframe-compile", docs/tasks/T-23.md "xtframe-compile".
///
/// Usage: xtframe-compile <input.json | input.xt> -o <out.xtir> [--root DIR] [--embed-images]
///                       [--embed-fonts] [--strict] [--no-meta] [--standard-symbols DIR]
///
/// T-235: `--standard-symbols DIR` is where `<link rel="symbols" href="xt-standard:<Name>">`
/// resolves (CompileOptions::standardSymbolDir); default `<executable dir>/symbols` when present.
///
/// Exit codes: 0 success, 1 a compile error occurred, 2 argument error (usage printed to stderr).
///
/// T-263f: when a `.xt` inside a UI Project fails because a file is missing from the project's
/// symbol directory (`shared.symbols` of its `.xtui`; those files are not checked in), one `note:`
/// line after the diagnostics says how they are fetched (tools/common/standard_symbols_note.h).
/// Nothing is written into the project and the exit code stays 1.
///
/// Not covered by docs/tasks/T-17.md/T-23.md (conservative choices made here):
///   - Diagnostics are printed in the order compileLottie/compileDocument returned them (warnings
///     and errors interleaved as produced), one per line, to stderr.
///   - `--root` defaults to the input file's parent directory when omitted, matching the golden
///     Lottie fixture flow's `rootDir = json 所在目錄` rule. compileDocument itself does not use
///     `--root`/`options.rootDir` at all (it always resolves relative paths against the `.xt`
///     file's own directory, per docs/tasks/T-23.md); `--root` is accepted for `.xt` input too
///     (for CLI uniformity) but has no effect on the compile.
///   - An `-o` value that fails to open for writing is reported as an `error:` diagnostic (no
///     file/line) and exits 1, not 2 (argument parsing itself succeeded).
///   - The success message's `<rules> rules, <elements> elements` suffix (T-23) is printed only
///     for `.xt` input; `.json` input keeps T-17's original `<scenes> scenes, <timelines>
///     timelines` message unchanged.
///   - An input extension other than `.json`/`.xt` is treated as Lottie JSON (compileLottie),
///     matching T-17's original behavior for any non-`.xt` path.

namespace {

namespace fs = std::filesystem;

void printUsage() {
    std::fprintf(stderr,
                 "usage: xtframe-compile <input.json | input.xt> -o <out.xtir> [--root DIR] "
                 "[--embed-images] [--embed-fonts] [--strict] [--no-meta] [--standard-symbols DIR]\n");
}

/// Reads the `.xtir` header's major/minor version directly from the serialized bytes (no IrView
/// dependency needed: the header's layout/offset is fixed, see xt/ir_pod.h's IrHeader).
void readIrVersion(const std::vector<uint8_t>& ir, uint16_t& major, uint16_t& minor) {
    major = 0;
    minor = 0;
    if (ir.size() < sizeof(xt::IrHeader)) {
        return;
    }
    std::memcpy(&major, ir.data() + 4, sizeof(major));
    std::memcpy(&minor, ir.data() + 6, sizeof(minor));
}

void printDiagnostic(const xt::compile::Diagnostic& d) {
    const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
    const std::string& file = d.file.empty() ? std::string("<input>") : d.file;
    if (d.line == 0 && d.col == 0) {
        std::fprintf(stderr, "%s: %s: %s\n", file.c_str(), level, d.message.c_str());
    } else {
        std::fprintf(stderr, "%s:%d:%d: %s: %s\n", file.c_str(), d.line, d.col, level, d.message.c_str());
    }
}

}  // namespace

static int run(int argc, char** argv) {
    xt::tools::useUtf8Console();  // T-261e
    std::string input;
    std::string output;
    std::string root;
    std::string standardSymbols;
    bool embedImages = false;
    bool embedFonts = false;
    bool strict = false;
    bool emitMeta = true;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-compile: -o requires an argument\n");
                printUsage();
                return 2;
            }
            output = argv[++i];
        } else if (arg == "--root") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-compile: --root requires an argument\n");
                printUsage();
                return 2;
            }
            root = argv[++i];
        } else if (arg == "--standard-symbols") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-compile: --standard-symbols requires an argument\n");
                printUsage();
                return 2;
            }
            standardSymbols = argv[++i];
        } else if (arg == "--embed-images") {
            embedImages = true;
        } else if (arg == "--embed-fonts") {
            embedFonts = true;
        } else if (arg == "--strict") {
            strict = true;
        } else if (arg == "--no-meta") {
            emitMeta = false;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "xtframe-compile: unknown option '%s'\n", arg.c_str());
            printUsage();
            return 2;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() != 1 || output.empty()) {
        printUsage();
        return 2;
    }
    input = positional[0];

    const fs::path inputPath = xt::compile::pathFromUtf8(input);  // T-261f: every argument is UTF-8
    bool isXt = inputPath.extension() == ".xt";

    xt::compile::CompileOptions options;
    options.rootDir = root.empty() ? xt::compile::pathToUtf8(inputPath.parent_path()) : root;
    options.embedImages = embedImages;
    options.embedFonts = embedFonts;
    options.strict = strict;
    options.emitMeta = emitMeta;
    options.standardSymbolDir = standardSymbols;
    if (standardSymbols.empty()) {
        std::error_code ec;
        const fs::path symbols =
            fs::absolute(xt::compile::pathFromUtf8(argv[0]), ec).parent_path() / "symbols";
        if (fs::is_directory(symbols, ec)) {
            options.standardSymbolDir = xt::compile::pathToUtf8(symbols);
        }
    }

    xt::compile::CompileResult result =
        isXt ? xt::compile::compileDocument(input, options) : xt::compile::compileLottie(input, options);
    for (const xt::compile::Diagnostic& d : result.diagnostics) {
        printDiagnostic(d);
    }

    if (!result.ok()) {
        // T-263f: a UI Project's document whose standard symbols were never fetched (they are not
        // checked in) fails on the missing files; say what fetches them. Nothing is written here.
        xt::bundle::UiProject project;
        if (isXt && xt::tools::enclosingUiProject(inputPath, project)) {
            std::error_code ec;
            const xt::tools::NoteDocument document{fs::absolute(inputPath, ec).parent_path(), std::string()};
            xt::tools::noteMissingStandardSymbols(project, {&document, 1}, result.diagnostics);
        }
        return 1;
    }

    const fs::path outputPath = xt::compile::pathFromUtf8(output);
    if (outputPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(outputPath.parent_path(), ec);
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out.good()) {
        std::fprintf(stderr, "error: could not open '%s' for writing\n", output.c_str());
        return 1;
    }
    out.write(reinterpret_cast<const char*>(result.ir.data()),
              static_cast<std::streamsize>(result.ir.size()));
    out.close();
    if (!out.good()) {
        std::fprintf(stderr, "error: failed writing '%s'\n", output.c_str());
        return 1;
    }

    xt::Result<xt::IrView> view = xt::IrView::open(xt::IrBlob{result.ir.data(), result.ir.size()});
    uint32_t scenes = view.ok() ? view.value().sceneCount() : 0;
    uint32_t timelines = view.ok() ? view.value().timelineCount() : 0;
    uint16_t irMajor = 0;
    uint16_t irMinor = 0;
    readIrVersion(result.ir, irMajor, irMinor);
    if (isXt) {
        // T-23: ".xt 輸入 ... 成功訊息加 <rules> rules, <elements> elements" -- appended to T-17's
        // original "bytes, scenes, timelines" message rather than replacing it (both counts are
        // meaningful for a `.xt` compile: scenes/timelines come from `<anim>`/`@keyframes`).
        uint32_t rules = view.ok() ? static_cast<uint32_t>(view.value().style().rules().size()) : 0;
        uint32_t elements = view.ok() ? static_cast<uint32_t>(view.value().doc().elements().size()) : 0;
        std::printf("wrote %s (ir %u.%u, %zu bytes, %u scenes, %u timelines, %u rules, %u elements)\n",
                    output.c_str(), irMajor, irMinor, result.ir.size(), scenes, timelines, rules, elements);
    } else {
        std::printf("wrote %s (ir %u.%u, %zu bytes, %u scenes, %u timelines)\n", output.c_str(), irMajor,
                    irMinor, result.ir.size(), scenes, timelines);
    }
    return 0;
}

// MSVC only: MinGW calls `wmain` only when linked with `-municode`, which this build does not pass,
// so there the narrow `main` below stays the entry point.
#if defined(_WIN32) && defined(_MSC_VER)
/// T-261f: the arguments as UTF-8 (xt::tools::Utf8Args); the narrow `argv` of `main` is in the ANSI
/// code page, which cannot carry every path.
int wmain(int argc, wchar_t** wargv) {
    xt::tools::Utf8Args args(argc, wargv);
    return run(args.argc(), args.argv());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
