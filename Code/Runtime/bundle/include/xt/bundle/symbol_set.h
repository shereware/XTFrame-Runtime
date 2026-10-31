#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xt/bundle/symbol_lint.h"
#include "xt/types.h"

/// T-499 (Docs/design/49-symbol-sets.md 49.4, 49.7): symbol sets in directory form and the skill's
/// `tokens.json`. A set directory is
///
///     <set>/set.json
///     <set>/thumbnail.png                 (the group thumbnail, when the group has one)
///     <set>/groups/<group id>/...         (each group exactly as in a library)
///
/// the `.xtsymbols` zip of T-500 holds the same tree (store mode, xt/bundle/zip_archive.h):
/// exportSet() / importSet() go through a temporary directory and the directory-form functions, so
/// both forms get the same checks and the same result. Every file read here is untrusted input (14
/// §14.9): paths are only used after resolveInside(), a symbolic link in a set is refused, and no
/// exception leaves these functions.

namespace xt::bundle {

/// `set.json` (49.4).
struct SetInfo {
    int version = 1;
    /// With `author`, the global identity of the set (`<author>/<id>`).
    std::string id;
    std::string author;
    std::string name;
    /// Semver `X.Y.Z[-pre]`.
    std::string setVersion;
    /// SPDX id; required.
    std::string license;
    std::string fontsLicense;
    std::string description;
    std::string homepage;
    /// Relative to the set directory; empty when none.
    std::string thumbnail;
    /// Group ids, each a directory `groups/<id>`.
    std::vector<std::string> groups;
    /// `requires.xtframe`, `>=<major>.<minor>[.<patch>]`; empty = any.
    std::string requiresXtframe;
};

/// Reads `<setDir>/set.json` and validates it (`version`, a valid `id`, `author`, `setVersion` and
/// `license`, `groups` of group ids, `requires.xtframe` against this build). kNotFound without the
/// file; kInvalidArgument (with `error`) for a malformed one; kUnsupported for a newer `version` or
/// a `requires.xtframe` this build does not meet (or does not understand).
Status readSetInfo(const std::filesystem::path& setDir, SetInfo& out, std::string& error);

struct ExportOptions {
    /// Overrides the author (else the part of the group's `source.set` before '/').
    std::string author;
    /// Overrides the license (else `source.license` of group.json, else the `SPDX-License-Identifier:`
    /// line of the group's `LICENSE`).
    std::string license;
    /// false: the `fonts/` directory is left out (`--no-fonts`).
    bool fonts = true;
    /// T-500, exportSet() only: the directory the staging directory is created under (UTF-8); empty =
    /// the system temp directory. The staging directory is always removed again.
    std::string tempDir;
};

struct ExportReport {
    /// kOk; kNotFound (no such group); kInvalidArgument (`error`: no author or license to take, a
    /// group id that is not a set id, an output directory that is not empty); kIoError.
    Status status = Status::kOk;
    std::string error;
    SetInfo set;
    /// The files written, relative to the set directory, sorted.
    std::vector<std::string> files;
};

/// Writes the group `groupDir` as a directory-form set into `outDir` (created; must be missing or
/// empty): the group tree (hidden entries, `.trash` among them, and symbolic links left out; no
/// `fonts/` without `options.fonts`), `thumbnail.png` and `set.json`.
ExportReport exportGroup(const std::filesystem::path& groupDir, const std::filesystem::path& outDir,
                         const ExportOptions& options = {});

/// T-500 (49.4): exportGroup() into a staging directory, then the `.xtsymbols` archive `file`
/// (store mode, zipDirectory(); its directory is created, an existing file is replaced). The report is
/// exportGroup()'s
/// (`files` relative to the set root = the archive entries); kIoError when the archive cannot be
/// written. The staging directory is removed whatever happens.
ExportReport exportSet(const std::filesystem::path& groupDir, const std::filesystem::path& file,
                       const ExportOptions& options = {});

struct ImportOptions {
    /// Imports the set's only group under this id (kInvalidArgument for a set with several groups,
    /// or when the id belongs to a group of another set).
    std::string asGroup;
    /// An older version of a set the library has: false leaves the library alone and reports
    /// `OlderSkipped` (49.4: the caller asks); true replaces it (the newer group goes to `.trash/`).
    bool replaceOlder = false;
    /// T-500, importSet() only: the directory the archive is unpacked under (UTF-8); empty = the
    /// system temp directory. The unpacked copy is always removed again.
    std::string tempDir;
};

enum class ImportAction : uint8_t {
    /// A new group.
    Added,
    /// The library had an older version of this set's group: replaced, the old one is in `.trash/`.
    Updated,
    /// The library has this version already.
    Unchanged,
    /// The library has a newer version; nothing written.
    OlderSkipped,
    /// The library had a newer version and `replaceOlder` replaced it.
    OlderReplaced,
};

struct ImportedGroup {
    /// The group id in the set.
    std::string sourceId;
    /// The group id in the library (`<id>-2` ... when another set's group had the id).
    std::string targetId;
    ImportAction action = ImportAction::Added;
    /// The old group's place in `.trash/` for Updated and OlderReplaced, relative to the library.
    std::string trashed;
    /// The lint of the set's group (49.6): errors do not stop the import.
    LintReport lint;
};

struct ImportReport {
    /// kOk; kNotFound (no `set.json`, no library); kInvalidArgument (`error`: a malformed set, a path
    /// leaving its group, a symbolic link, too many files, a bad `--as`); kUnsupported (a newer
    /// `set.json` version, `requires.xtframe` not met, a read-only library); kIoError. Nothing is
    /// written to the library unless kOk.
    Status status = Status::kOk;
    std::string error;
    SetInfo set;
    std::vector<ImportedGroup> groups;
};

/// 49.4 for a directory-form set: the checks (`set.json`, every path inside its group, no symbolic
/// link, lint), the id rules per group, then the groups are copied into `libraryDir`'s
/// `groups/<id>/` (group.json gets `id`, `source {kind: "import", set: "<author>/<id>", setVersion}`
/// and `importedAt`) and `library.json` lists them. `libraryDir` must be a writable library.
///
/// Id rules, per group `g` of the set with identity `<author>/<id>`: a library group with the same
/// identity (`group.json` `source.set`) and the id `g` or `g-<n>` is the same group: a newer
/// `setVersion` replaces it (the old one moves to `.trash/<id>-<seconds>/`), the same is skipped, an
/// older one is `OlderSkipped` unless `replaceOlder`. Any other group of the id `g` (another author,
/// or a hand-made one) makes the new group `g-2`, `g-3`, ... (the first free).
ImportReport importGroupDir(const std::filesystem::path& setDir, const std::filesystem::path& libraryDir,
                            const ImportOptions& options = {});

/// T-500 (49.4): the `.xtsymbols` archive `file` unpacked (unzipToDirectory(): zip integrity, every
/// entry name inside the staging directory, no symbolic link, the set's file and byte limits) into
/// a staging directory, then importGroupDir() from there (all of 49.4's checks), so the library
/// only changes when both passed. kNotFound when `file` cannot be opened; kInvalidArgument
/// (`error`) for a bad archive or entry. The staging directory is removed whatever happens.
ImportReport importSet(const std::filesystem::path& file, const std::filesystem::path& libraryDir,
                       const ImportOptions& options = {});

/// The xtui skill's `references/tokens.json` for the group `groupDir`: `themes.base` from its token
/// file and one theme per theme file (`tokens-night.xts` -> `dark`), the font families, sizes,
/// spacing scale, radii, the shadow and the board size, in the layout of the hand-written file
/// `Skills/skills/xtui/references/tokens.json` had (Docs/design/49-symbol-sets.md 49.7). kNotFound
/// when the group has no readable token file; kInvalidArgument (with `error`) when the token file
/// lacks the colour tokens the skill's themes need.
Status tokensJson(const std::filesystem::path& groupDir, std::string& out, std::string& error);

}  // namespace xt::bundle
