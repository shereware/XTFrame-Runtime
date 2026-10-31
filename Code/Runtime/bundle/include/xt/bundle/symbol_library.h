#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/bundle/bundle.h"  // SymbolPortInfo (T-200).
#include "xt/bundle/ui_project.h"
#include "xt/compile/diagnostics.h"
#include "xt/types.h"

/// T-498 (Docs/design/49-symbol-sets.md 49.1, 49.2, 49.7, 49.8): symbol libraries. A library is a
/// directory holding a `library.json` (`id`, `name`, `readOnly`, `default`, `groups[]`); each group
/// is `<library>/groups/<group-id>/` with its `group.json`, `index.json`, symbol files,
/// `tokens.xts`, theme files, `fonts/`, `textures/` and `thumbnails/`. The presets ship in
/// `Assets/Symbols/` (copied to `<applicationDirPath>/symbols`) and are read-only; the user's own
/// library is `<user data>/XTFrame/symbols/`, where the default preset group is copied on the
/// first mount and can be edited, and reloaded from the presets when wanted.
///
/// Paths are UTF-8 (14 §14.10); the relative paths of a group (`tokens`, `themes[]`, each symbol's
/// `file`, `thumbnail`, `fonts[]`, `textures[]`) are kept as written, '/' separated, and are only
/// accepted when they stay inside the group directory. Every file read here is untrusted input
/// (14 §14.9): a member of the wrong type reads as absent, a broken file is a `problems` entry of
/// its group, and no exception leaves these functions.

namespace xt::bundle {

/// One token a symbol reads and its default value (`index.json` `tokens[]`, `{name, default}`).
struct TokenDefault {
    /// `--name`.
    std::string name;
    /// The value `tokens.xts` declares; empty when it declares none.
    std::string value;
};

/// Where a group came from (`group.json` `source`).
struct GroupSource {
    /// `preset`, `import`, `manual`, ... (empty when the group says nothing).
    std::string kind;
    /// The set id (`<author>/<id>`), empty for a hand-made group.
    std::string set;
    /// The set's semver, empty when unknown.
    std::string setVersion;
};

/// One `index.json` entry of a group.
struct SymbolEntry {
    std::string name;
    /// `<Name>.xt`, relative to the group directory (`mobile/`, `list/` subdirectories allowed).
    std::string file;
    /// `thumbnails/<Name>.png`, relative to the group directory; empty when the entry has none.
    std::string thumbnail;
    std::string description;
    std::string category;
    /// `requires.states` and `requires.slots`.
    std::vector<std::string> requiredStates;
    std::vector<std::string> requiredSlots;
    std::vector<SymbolPortInfo> ports;
    std::vector<std::string> timelines;
    /// `sha256:<hex>` of the symbol file as the index recorded it (T-408); empty when absent.
    std::string content;
    std::vector<TokenDefault> tokens;
    /// Font and texture files the symbol needs, relative to the group directory.
    std::vector<std::string> fonts;
    std::vector<std::string> textures;
};

/// T-501 (49.6): the persisted lint state of a group, `library.json` `problems.<group id>`
/// (`{errors, warnings}`; setGroupProblems()). A group with `errors > 0` is "has problems": listed,
/// but to be fixed before its symbols are used in a project.
struct LintSummary {
    size_t errors = 0;
    size_t warnings = 0;
};

/// One group of a library.
struct SymbolGroup {
    /// The directory name under `groups/` (`[a-z0-9-]+`, or `<id>@<version>` for a kept copy).
    std::string id;
    std::string name;
    std::string description;
    /// The group directory, UTF-8, '/' separated, no trailing slash; empty when it does not exist.
    std::string dir;
    GroupSource source;
    /// `tokens.xts` (relative to `dir`); empty when the group names none or an invalid one.
    std::string tokensFile;
    /// Theme files relative to `dir` (`tokens-night.xts`, `themes/*.xts`).
    std::vector<std::string> themeFiles;
    /// The group thumbnail relative to `dir`; empty when none.
    std::string thumbnail;
    std::vector<SymbolEntry> symbols;
    /// What was wrong with the group's files (49.6: a group with problems is listed, not hidden).
    std::vector<compile::Diagnostic> problems;
    /// T-501: the lint state `library.json` records for the group ({0, 0} when none recorded).
    LintSummary lint;
};

/// T-498b (49.2): how a library got mounted.
enum class LibraryScope : uint8_t {
    /// The presets next to the executable (mountDefaults(), read-only).
    Preset,
    /// The user's own library under the user data directory (mountDefaults()).
    User,
    /// `<project>/shared/symbol-library/` (mountProjectLibrary()); mounted with the project.
    Project,
    /// Any other directory (mount(), loadMounts()).
    Extra,
};

/// One mounted library.
struct SymbolLibrary {
    std::string id;
    std::string name;
    /// The library directory, UTF-8, '/' separated, no trailing slash.
    std::string dir;
    bool readOnly = false;
    /// The group the standard symbols resolve in (`library.json` `default`, else the first group).
    std::string defaultGroup;
    /// In `library.json` order.
    std::vector<SymbolGroup> groups;
    /// T-498b: how it was mounted.
    LibraryScope scope = LibraryScope::Extra;
};

/// T-498b: one line of the persisted mount list (`symbol-libraries.json`).
struct MountEntry {
    /// The library directory as the user gave it, UTF-8.
    std::string path;
    /// false: listed but not mounted.
    bool enabled = true;
};

/// The libraries the editor has mounted, in preference order (49.2 `symbolLibraries[]`).
struct LibraryMounts {
    std::vector<SymbolLibrary> libraries;
    /// T-498b: the extra libraries the user mounted, in order, as `symbol-libraries.json` lists
    /// them (loadMounts() / saveMounts()). The presets, the user library and a project library are
    /// never listed here.
    std::vector<MountEntry> entries;
};

/// The directory of `libraryDir`'s default group (`library.json`'s `default`, else its first
/// group): where every "standard symbol" path resolves -- `xtframe-pack`'s and the editor's
/// standard symbol directory and the `xt-standard:<Name>` link alias (49.8). UTF-8, '/'
/// separators, without a trailing slash. Empty when `libraryDir` holds no readable
/// `library.json`, the default group's id is not a group id, or its directory does not exist.
std::string defaultGroupDir(std::string_view libraryDir);

/// Reads `<dir>/library.json` and, for each listed group, its `group.json` and `index.json` into
/// `out` (reset first). kNotFound without a `library.json`; kInvalidArgument when it is not a JSON
/// object, lacks a valid `id`, or `dir` is empty; kUnsupported for a `version` above 1. A group
/// whose directory or files are missing or broken is still listed, with its `problems` filled and
/// what could be read kept; kOk then. kInternal when something threw (`out` reset).
Status loadLibrary(const std::filesystem::path& dir, SymbolLibrary& out);

/// Writes `<library.dir>/library.json` (`version`, `id`, `name`, `readOnly`, `default`, the group
/// ids in order, and `problems` for every group whose `lint` is not {0, 0}). kUnsupported for a read-only
/// library, kInvalidArgument for an empty `dir` or `id`, kIoError when the file cannot be written.
Status saveLibraryJson(const SymbolLibrary& library);

/// T-498b: reads one group directory (`group.json`, `index.json`) into `out` (reset first), the
/// way loadLibrary() reads each listed group; `out.id` is the directory name. kNotFound when `dir`
/// is not a directory, kInvalidArgument when its name is not a group id; kOk otherwise, with the
/// group's `problems` saying what was wrong with its files.
Status loadGroup(const std::filesystem::path& dir, SymbolGroup& out);

/// The entry named `name` in the group `group` (any group when `group` is empty, in library
/// order); nullptr when none.
const SymbolEntry* findSymbol(const SymbolLibrary& library, std::string_view name,
                              std::string_view group = {});

/// T-498b (49.2): findSymbol() across every mounted library, in mount order (the first library
/// with the symbol wins; `group` restricts the search to that group id). `*library` and `*group`
/// (may be null) receive where it was found. nullptr when no library has it.
const SymbolEntry* findSymbol(const LibraryMounts& mounts, std::string_view name, std::string_view group = {},
                              const SymbolLibrary** libraryOut = nullptr,
                              const SymbolGroup** groupOut = nullptr);

/// T-498b (49.8): the group every `xt-standard:<Name>` link resolves in -- always the preset
/// library's default group (LibraryScope::Preset), whatever else is mounted. nullptr when no
/// preset library is mounted or its default group has no directory.
const SymbolGroup* standardGroup(const LibraryMounts& mounts);

/// Mounts the two default libraries (49.2) into `mounts`, ahead of everything else there: the
/// presets at `presetDir` (read-only) and the user's own library at
/// `<userDataDir>/XTFrame/symbols/`. A missing user library is created (id `mine`, writable) and,
/// at that first mount only, the presets' default group is copied into it as its default group; a
/// user library that exists is loaded as it is (a group the user deleted does not come back:
/// reloadPreset() does that). A library already in `mounts` with the same directory is replaced.
/// kNotFound when `presetDir` is not a library; kInvalidArgument for an empty `userDataDir`;
/// kIoError when the user library cannot be created or written.
Status mountDefaults(LibraryMounts& mounts, const std::filesystem::path& presetDir,
                     const std::filesystem::path& userDataDir);

/// 49.7 `symbols.loadPreset`: copies the preset group `groupId` (from the first read-only library
/// of `mounts` that has it) into the first writable library of `mounts`, replacing a group of the
/// same id there. The replaced group is moved to `<library>/.trash/<id>-<seconds since epoch>/`,
/// or, with `keepOld`, kept as the group `<id>@<its setVersion>` (a suffix `-<seconds>` added when
/// that exists too). The writable library's `library.json` is rewritten and the library reloaded
/// in place. kInvalidArgument for a bad `groupId`; kNotFound when no mounted library has the preset
/// or none is writable; kIoError when a move or copy fails.
Status reloadPreset(LibraryMounts& mounts, std::string_view groupId, bool keepOld);

/// T-498b (49.2): mounts the library at `dir` (LibraryScope::Extra) after everything already in
/// `mounts` (a library with the same directory is replaced in place) and lists it in
/// `mounts.entries` when it is not there yet. loadLibrary()'s statuses.
Status mount(LibraryMounts& mounts, const std::filesystem::path& dir);

/// T-498b: removes the mounted library with id `id` (the first one) and its `entries` line. false
/// when no mounted library has that id.
bool unmount(LibraryMounts& mounts, std::string_view id);

/// T-498b (49.2 "project library"): mounts `<project root>/shared/symbol-library/` when that
/// directory holds a library (LibraryScope::Project, after everything else, never listed in
/// `entries`); kNotFound when the project has no such directory, else loadLibrary()'s statuses.
Status mountProjectLibrary(LibraryMounts& mounts, const UiProject& project);

/// T-498b: reads `<userDataDir>/XTFrame/symbol-libraries.json` (`version` 1, `libraries[]` of
/// `{path, enabled}`) into `mounts.entries` (replaced) and mounts every enabled entry whose
/// directory is a library (LibraryScope::Extra, in list order, after what is mounted already; one
/// that fails to load stays listed, unmounted). kNotFound without the file, kInvalidArgument when
/// it is not a JSON object or `userDataDir` is empty, kUnsupported for a `version` above 1.
Status loadMounts(const std::filesystem::path& userDataDir, LibraryMounts& mounts);

/// T-498b: writes `mounts.entries` as `<userDataDir>/XTFrame/symbol-libraries.json`.
/// kInvalidArgument for an empty `userDataDir`, kIoError when the file cannot be written.
Status saveMounts(const std::filesystem::path& userDataDir, const LibraryMounts& mounts);

/// T-498b (49.3 "copy rule"): what useSymbols() did. Paths are project-relative, '/' separated.
struct UseReport {
    /// kOk, or kInvalidArgument when a name is not a plain symbol name (nothing written then),
    /// kIoError when a file could not be written (what was written before stays).
    Status status = Status::kOk;
    /// Files created or appended to, in the order written.
    std::vector<std::string> written;
    /// Files left alone, each `<path>: <why>` (a symbol the project already has, a font or texture
    /// already there with the same content).
    std::vector<std::string> skipped;
    /// Fonts / textures copied under another name because the project had a different file of
    /// that name, each `<library-relative> -> <project-relative>`.
    std::vector<std::string> renamed;
    /// Tokens appended: `--name` for `shared.tokens`, `<theme>:--name` for a theme file.
    std::vector<std::string> tokensAdded;
    /// Names no mounted library (or the given group) has.
    std::vector<std::string> notFound;
};

/// T-498b (49.3, the editor's drag-in and the AI's `symbols.use`): copies the symbols `names` (the
/// first mounted library that has each, or the group `groupId` when given) and what they need into
/// `project`:
///   1. `<group>/<file>` -> `<shared.symbols>/<Name>.xt`, never over an existing file (skipped:
///      the project's copy is used);
///   2. the entry's `fonts[]` / `textures[]` -> `<shared.assetRoot>/fonts/`, `.../textures/`; a
///      file already there with the same sha256 is skipped, one with other content makes the copy
///      `<stem>-<first 8 hex of its sha256><ext>` and every reference in the copied symbol and the
///      appended token lines is rewritten to it;
///   3. of the entry's `tokens[]`, the names `shared.tokens` does not declare are appended at its
///      end as one `/* added with <Name> from <group> */` section (the file is created when
///      missing; an existing name is left as it is), the group's `@font-face` blocks for the copied
///      fonts the project does not declare (same family, weight and style) go in the same section,
///      and each group theme file (`tokens-night.xts` -> `<shared.themes>/night.xts`, see
///      themeNameOf()) gets the entry's tokens it sets and the project theme lacks the same way;
///   4. `<shared.symbols>/index.json` (`version` 1, `symbols[]`) records the copy's source
///      (`library`, `group`, `set`, `setVersion`, `sha256`) for isCopyStale().
/// Nothing is undone (the editor wraps the call in a snapshot command, T-501). Every path from the
/// library is only used when it stays inside its group directory and every path written only when
/// it stays inside the project root (14 §14.9 rule 5). A `name` with a separator, `..`, `.` or `:`
/// is refused as a whole (status kInvalidArgument, nothing written).
UseReport useSymbols(const LibraryMounts& mounts, const UiProject& project,
                     std::span<const std::string_view> names, std::string_view groupId = {});

/// T-498b (49.1 "older than the library"): true when the project's copy
/// `<shared.symbols>/<name>.xt` exists, a mounted library has `name`, and the two files differ
/// (sha256 of the bytes, CRLF read as LF). false when the project has no copy or no library has
/// the symbol.
bool isCopyStale(const LibraryMounts& mounts, const UiProject& project, std::string_view name);

/// T-498b: the theme name of a group theme file (`themes[]` of group.json, or a project theme
/// file): the file name stem, minus a leading `tokens-` (`tokens-night.xts` -> `night`,
/// `themes/dark.xts` -> `dark`). Two files with different names are two themes.
std::string themeNameOf(std::string_view themeFile);

/// T-498b (lint `token.sameAsDefault`): the tokens `projectTokensFile` (a `shared/tokens.xts`)
/// declares with exactly the value `groupTokensFile` (the group's `tokens.xts`) declares for the
/// same name (value text compared after trimming), in the project file's order. Such a
/// declaration only repeats the implied default layer and should be deleted so the project follows
/// the group. Empty when either file cannot be read.
std::vector<TokenDefault> sameAsDefaultTokens(const std::filesystem::path& projectTokensFile,
                                              const std::filesystem::path& groupTokensFile);

/// T-498b (49 §49.3 ruling) / T-519: the implied default layer rule pack() applies
/// (PackReport::impliedGroupDir), shared with the Editor Service's token queries. When `standardSymbolDir`
/// (UTF-8) is a group directory (loadGroup()) and a UI of the project -- `uis[].doc`, relative to `rootDir`
/// -- links a standard symbol (`xt-standard:<Name>`, or a file whose name the group also has), fills `out`
/// with that group and returns true; otherwise returns false and leaves `out` unspecified. An unreadable or
/// unparsable UI counts as linking nothing.
bool detectImpliedGroup(const std::string& rootDir, const std::vector<ProjectUi>& uis,
                        const std::string& standardSymbolDir, SymbolGroup& out);

// --- T-501 (Docs/design/49-symbol-sets.md 49.1, 49.5; Docs/tasks/T-501.md item 4) ---------------

/// 49.1 "update the copy" (the editor's "Update copy" button, the AI's `symbols.updateCopy`):
/// useSymbols() for the one symbol `name` with its symbol file written over the project's copy
/// (`<shared.symbols>/<Name>.xt`, and `index.json`'s source entry replaced) and its fonts and
/// textures copied by the same rules (an existing file of the same content is skipped, other
/// content makes a hashed copy); step 3 is left out: no token or theme line is appended, the
/// project's token values stay as they are. Nothing is undone here: the editor snapshots the
/// files first (snapshotForUse()) and wraps the call in one undo step (ReplaceSharedFile).
/// `report.notFound` names it when no mounted library has the symbol.
UseReport updateCopy(const LibraryMounts& mounts, const UiProject& project, std::string_view name);

/// One project file as it was before a useSymbols() / updateCopy() call.
struct ProjectFileState {
    /// Project-relative, '/' separated.
    std::string path;
    bool existed = false;
    /// The content when it existed (bytes as read).
    std::string content;
};

/// The files a useSymbols() / updateCopy() call for `names` may change in place -- the shared
/// token file, every theme file of `shared.themes`, `<shared.symbols>/index.json` and the
/// project copy of each name -- read now (one entry each; absent files are listed with
/// `existed == false`). Every other file those calls write is a new file. Empty for an unnamed
/// project.
std::vector<ProjectFileState> snapshotForUse(const UiProject& project,
                                             std::span<const std::string_view> names);

/// One file a useSymbols() / updateCopy() call wrote, with its content before and after: what one
/// undo step restores (the editor's `ReplaceSharedFile`: `before` written back, or the file
/// removed when it did not exist).
struct FileChange {
    /// Project-relative, '/' separated.
    std::string path;
    bool existed = false;
    std::string before;
    std::string after;
};

/// The changes of the call that produced `report`: one entry per `report.written` path (in that
/// order), `before`/`existed` from `snapshot` (a path not in the snapshot is a new file), `after`
/// read from the project now. A written file that cannot be read back is left out.
std::vector<FileChange> changesOfUse(const std::vector<ProjectFileState>& snapshot, const UiProject& project,
                                     const UseReport& report);

/// 49.5 "save as group" (the editor's "Save as group..." dialog, the AI's `symbols.saveAsGroup`):
/// the project symbols `names` (`<shared.symbols>/<Name>.xt`) become the new group
/// `<libraryDir>/groups/<groupId>/` named `name`, the dependencies collected the reverse way of
/// 49.3: the `var(--x)` each symbol reads are its `tokens[]`, their values from `shared.tokens`
/// (a value the project does not declare is taken from the group `<shared.symbols>/index.json`
/// records as the copy's source, or any group of `mounts` that declares it, when `mounts` is
/// given -- the implied default layer of T-498b), written as the group's `tokens.xts`; the
/// `--font-*` families' `@font-face` sources and the `--tex-*` `url()` files are copied to
/// `fonts/` and `textures/` (references rewritten); each project theme file setting one of the
/// tokens becomes `tokens-<theme>.xts`; `group.json` (`source.kind` "manual") and `index.json`
/// (contract, ports, timelines, tokens with defaults, fonts, textures, content hash, the
/// project's description) are written; a thumbnail the project index names (else the source
/// group's) is copied to `thumbnails/<Name>.png`. `library.json` lists the group; its index-only
/// lint (49.6) is recorded with setGroupProblems(). An empty `names` makes an empty group (the
/// editor's "New group"; `project` may then be unnamed). kInvalidArgument (with `error`) for a
/// name that is not a plain symbol name, a `groupId` that is not `[a-z0-9-]+` or already a group
/// of the library; kNotFound when a symbol file or the library is missing;
/// kUnsupported for a read-only library; kIoError when a write fails (the group directory is
/// removed again).
Status saveAsGroup(const UiProject& project, std::span<const std::string_view> names,
                   const std::filesystem::path& libraryDir, std::string_view groupId, std::string_view name,
                   std::string* error = nullptr, const LibraryMounts* mounts = nullptr);

/// Renames the group `groupId` of `libraryDir` (its `group.json` `name`; the directory and id stay).
/// kInvalidArgument for an empty `name` or a bad id; kNotFound when the library or the group is
/// missing; kUnsupported for a read-only library; kIoError.
Status renameGroup(const std::filesystem::path& libraryDir, std::string_view groupId, std::string_view name);

/// Deletes the group `groupId` of `libraryDir`: its directory moves to
/// `<libraryDir>/.trash/<id>-<seconds since epoch>/` (`*trashed`, may be null, receives that
/// library-relative path) and `library.json` drops it (a `default` naming it is cleared).
/// kInvalidArgument for a bad id; kNotFound when the library or the group is missing;
/// kUnsupported for a read-only library; kIoError.
Status deleteGroup(const std::filesystem::path& libraryDir, std::string_view groupId,
                   std::string* trashed = nullptr);

/// Copies the group `groupId` of `srcLibraryDir` into `dstLibraryDir` as `groups/<id>/` -- or
/// `<id>-2`, `<id>-3`, ... when the destination has a group of that id (`*targetId`, may be
/// null, receives the id used; `group.json` `id` is rewritten to it, everything else, the source
/// among it, is kept) -- and lists it in the destination's `library.json`. The "Copy to my
/// library" of a preset (read-only) group. kInvalidArgument for a bad id; kNotFound when a
/// library or the group is missing; kUnsupported for a read-only destination; kIoError.
Status copyGroup(const std::filesystem::path& srcLibraryDir, std::string_view groupId,
                 const std::filesystem::path& dstLibraryDir, std::string* targetId = nullptr);

/// Records `summary` as `problems.<groupId>` of the library file `libraryJson` (a `library.json`,
/// or the directory holding one), every other key kept; a {0, 0} summary removes the entry. The
/// persisted "has problems" mark of 49.6 (an import's lint, saveAsGroup()'s). kInvalidArgument
/// for a bad id or a file that is not a JSON object; kNotFound without the file; kIoError.
Status setGroupProblems(const std::filesystem::path& libraryJson, std::string_view groupId,
                        const LintSummary& summary);

}  // namespace xt::bundle
