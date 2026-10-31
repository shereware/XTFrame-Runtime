#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/types.h"

/// T-499 (Docs/design/49-symbol-sets.md 49.6): lint for a symbol group, the checks `Tests/symbols`
/// ran on the preset group moved into the library so `xtframe-symbols lint`, the import (49.4) and
/// the tests run the same code. Every file read here is untrusted input (14 §14.9): a broken file
/// is an issue of the report, never a failure of the call, and no exception leaves these functions.
///
/// Codes (`LintIssue::code`; error unless marked W):
///   group.notFound          the directory is not a group directory (also: a listed group has none)
///   group.id                the directory name is not a group id
///   group.problem           what loadGroup() found wrong with `group.json` / `index.json` (W for
///                           the warnings it records)
///   group.tokens            the token file is missing or unreadable
///   group.theme             a theme file `group.json` lists is missing
///   group.thumbnail         W: `group.json` names no group thumbnail
///   index.unlisted          a `.xt` file of the group root, `mobile/` or `list/` is not in index.json
///   index.contract          the `requires="..."` of the symbol file is not the index entry's contract
///   index.port              a port of the entry is not declared by `port:<name>="<type> <dir>"`
///   index.timeline          a timeline of the entry has no `@timeline <name>` in the file
///   index.tokens            the entry's token names are not the `var(--name)` set the file reads
///   index.fonts             the entry's fonts are not the files its font tokens name
///   index.textures          the entry's textures are not the files its texture tokens name
///   index.thumbnail         the entry's thumbnail is not `thumbnails/<Name>.png`
///   index.content           the entry's `content` is not the sha256 of the file (CRLF read as LF)
///   index.previousContent   W: the entry has no `previousContent` list
///   token.undeclared        `var(--name)` is not declared by the group's token file
///   token.default           the entry's default of a token is not the value the token file declares
///   asset.missing           a font or texture of the entry (or the tokens' names) is not in the group
///   thumbnail.missing       a symbol thumbnail (or the group thumbnail) is not in the group
///   symbol.fileMissing      the entry's `.xt` file is missing
///   symbol.name             the file has no `<symbol name="<Name>"`
/// and, unless `LintOptions::indexOnly`:
///   compile.error           the symbol's preview does not compile with --strict (`W` when it
///                           compiles and only a deprecation notice is left: compile.warning)
///   contract.state          the symbol has no `symbol(<Name>)` rule for a state it `requires`
///   contract.slot           the symbol defines no `<slot>` it `requires`
///   preview.noInstance      the preview does not instantiate the symbol
///   preview.spec            `previews/<Name>.preview.json` is malformed or has no `.preview.xt`
///   preview.json            W: a `.preview.xt` without a `.preview.json` (no thumbnail can be made)
///   style.literalColor      a literal colour (`#hex`, `rgb[a]()`, `hsl[a]()`, a named colour) in the
///                           symbol's styles: colours come from tokens
///   timeline.duration       a timeline longer than 500 ms (the host-driven `cooldown` is exempt)
///   timeline.property       a timeline animating more than transform, opacity and text-reveal
///   timeline.name           W: a timeline name that is not lower-case kebab-case (28 §28.1.3)
/// A symbol without `previews/<Name>.preview.xt` is compiled through a synthetic minimal preview
/// (one `<use>` of it under the group's token file).

namespace xt::bundle {

enum class LintLevel : uint8_t { Warning, Error };

/// One finding.
struct LintIssue {
    LintLevel level = LintLevel::Error;
    /// The symbol name; empty for a finding about the group.
    std::string symbol;
    /// A code from the list above.
    std::string code;
    std::string message;
};

struct LintOptions {
    /// Only what `index.json` says about the files (no compile, no previews, no style or timeline
    /// scan): what `Tools/Scripts/check_symbols_index.sh` runs.
    bool indexOnly = false;
};

struct LintReport {
    /// The group id (directory name); empty when the directory is not a group.
    std::string groupId;
    /// The group directory, UTF-8, '/' separated.
    std::string dir;
    /// How many symbols the index lists.
    size_t symbols = 0;
    std::vector<LintIssue> issues;

    size_t errors() const;
    size_t warnings() const;
};

/// Lints the group directory `dir`. Never throws; a directory that is not a group is one
/// `group.notFound` / `group.id` error.
LintReport lintGroup(const std::filesystem::path& dir, const LintOptions& options = {});

/// `dir` as a library (`library.json`): one report per listed group, in library order (a listed
/// group without a directory is a `group.notFound` report); else `dir` as one group directory.
std::vector<LintReport> lintLibrary(const std::filesystem::path& dir, const LintOptions& options = {});

/// The declarations of the style regions `findLiteralColors()` reports: `<name>: <value>` for every
/// declaration of `css` whose value holds a literal colour (`#hex` of 3..8 digits, `rgb(`, `rgba(`,
/// `hsl(`, `hsla(`, or one of 05 §5.3's named colours), comments ignored.
std::vector<std::string> findLiteralColors(std::string_view css);

/// The text the colour scan reads from a file: for a `.xt` (`isXts` false) the bodies of its
/// `<style>` elements and its `style="..."` attributes, for an `.xts` the whole text.
std::vector<std::string> styleRegions(std::string_view text, bool isXts);

/// One `@timeline <name> { ... }` block of a stylesheet text.
struct TimelineScan {
    std::string name;
    /// The latest key time in the block, seconds.
    float lastTime = 0;
    /// The animated properties (`name:` words other than `duration` and `marker`), sorted, unique.
    std::vector<std::string> props;
};
std::vector<TimelineScan> scanTimelines(std::string_view text);

/// True for a lower-case kebab-case name (`intro`, `hover-in`, `item-out`): the shape of the
/// conventional names of 28 §28.1.3.
bool isConventionalTimelineName(std::string_view name);

/// `previews/<Name>.preview.json` of a group (T-499): what to render for the symbol's thumbnail and
/// what the normal-state golden of `Tests/symbols` runs.
struct PreviewSpec {
    /// `<Name>`.
    std::string name;
    /// `previews/<Name>.preview.xt` and `.preview.json`, relative to the group directory.
    std::string xtFile;
    std::string jsonFile;
    /// Output size in pixels (1..4096).
    int width = 0;
    int height = 0;
    /// Extra frames of 1/60 s run after the events (0..600; usually 0).
    int frames = 0;
    /// `ports{}`: name and text, in file order; each is a `port <name> <text>` line before `events`.
    std::vector<std::pair<std::string, std::string>> ports;
    /// `events[]`: lines of the event script of `Tests/golden` (`update 0.6`, `placeholders`, ...).
    std::vector<std::string> events;
};

/// Reads one `*.preview.json` into `out` (reset first). kNotFound when the file is missing,
/// kInvalidArgument (with `error`) when it is not valid.
Status readPreviewSpec(const std::filesystem::path& jsonFile, PreviewSpec& out, std::string& error);

/// The specs of `<groupDir>/previews/*.preview.json` (not subdirectories), sorted by name; with
/// `names` not empty only those. A file that is not valid, or whose `.preview.xt` is missing, is
/// left out and noted in `problems` (when not null).
std::vector<PreviewSpec> thumbnailSpecs(const std::filesystem::path& groupDir,
                                        const std::vector<std::string>& names = {},
                                        std::vector<compile::Diagnostic>* problems = nullptr);

/// The event script of `spec`: `port <name> <text>` for each of `ports`, then each of `events`,
/// one per line, '\n' terminated.
std::string previewEventScript(const PreviewSpec& spec);

}  // namespace xt::bundle
