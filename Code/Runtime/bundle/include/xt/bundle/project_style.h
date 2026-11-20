#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/bundle/symbol_library.h"
#include "xt/bundle/ui_project.h"
#include "xt/types.h"

/// T-743 (Docs/design/56-symbol-identity-style-scope.md 56.2; 47-symbol-visual-style.md, the core
/// token vocabulary): a UI Project has one style. Its token values live in `shared.tokens` and the
/// theme files of `shared.themes`; every symbol, whichever library it came from, reads them.
///
///   - The core vocabulary: the 52 token names every symbol library declares (coreTokens(); the data
///     file `Assets/Symbols/core-tokens.json` is kept equal to it and to chapter 47 by Tests/symbols).
///   - "Apply style" (applyGroupStyle()): the core tokens of another group's `tokens.xts` (and its
///     theme files) replace the project's values; everything replaced is reported, nothing else of
///     the project changes. The project records where its style came from in `<shared.symbols>/
///     index.json` `style` (readProjectStyle()) -- a project file, not part of any bundle.
///   - "Duplicate and detach" (duplicateDetached()): a copy of a project symbol under a new name that
///     reads its own tokens, so a later style change leaves it alone.
///   - The implied token layer of a project that links several groups (projectImpliedGroup(), 47
///     rule 5): the applied style's group, else the first linked group, else the standard group.
///
/// Paths are UTF-8 (14 §14.10). Files read here are untrusted input (14 §14.9); no exception leaves
/// these functions. Nothing here writes unless its documentation says so: the editor and the
/// Editor Service write the returned FileChange list as one undo step (`ReplaceSharedFile`).

namespace xt::bundle {

// --- the core vocabulary (47, "共同詞彙清單") -----------------------------------------------------

enum class CoreTokenType : uint8_t { Color, Length, Time, Family };

struct CoreToken {
    /// `--color-bg`, ...
    const char* name;
    CoreTokenType type;
};

/// The 52 core tokens in the order of chapter 47's list.
std::span<const CoreToken> coreTokens();

/// The core token `name` (`--x`), or null for a name outside the vocabulary.
const CoreToken* findCoreToken(std::string_view name);

/// `color`, `length`, `time`, `family` (the `type` of `core-tokens.json`).
const char* coreTokenTypeName(CoreTokenType type);

/// True when `value` (a token value text) can be a value of `type`: a colour (`#hex`, `rgb[a]()`,
/// `hsl[a]()`, `color-mix()`, a named colour, `transparent`), a length (a number with a unit, `0`),
/// a time (`<n>ms`, `<n>s`) or a font family list (not a number). A value built from `var()` or
/// `calc()` is accepted for every type (its type cannot be told without resolving it).
bool coreTokenValueFits(CoreTokenType type, std::string_view value);

// --- the project's style record ----------------------------------------------------------------

/// `<shared.symbols>/index.json` `style`: where the project's style came from and the value of each
/// core token as it was applied (to tell "custom (from <group>)" from "<group>").
struct ProjectStyle {
    std::string library;
    std::string group;
    /// The group's display name and set version when applied.
    std::string name;
    std::string set;
    std::string setVersion;
    /// The group theme applied as the base, empty for the group's base tokens.
    std::string theme;
    /// `--name` -> `sha256:<hex>` (styleValueHash()) of the value written for the base.
    std::map<std::string, std::string> tokens;
    /// Theme name -> the same, for each theme file written.
    std::map<std::string, std::map<std::string, std::string>> themes;
};

/// `sha256:<hex>` of `value` without its leading and trailing white space.
std::string styleValueHash(std::string_view value);

/// The project's `style` record into `out` (reset first). False -- `out` empty -- when the project
/// has no index, no `style` object, or one without `library` and `group`.
bool readProjectStyle(const UiProject& project, ProjectStyle& out);

/// What the design variables tab shows (56 §56.2 item 1): "Style: <group>" or "Style: custom (from
/// <group>)".
struct StyleStatus {
    bool recorded = false;
    ProjectStyle style;
    /// The recorded tokens whose value in the project is no longer the applied one (or gone):
    /// `--name` for the base, `<theme>:--name` for a theme file. Empty: the project is the style.
    std::vector<std::string> changed;
};
StyleStatus projectStyleStatus(const UiProject& project);

// --- apply style (56 §56.2 item 1, 47 rules 4, 6) ------------------------------------------------

/// One core token value applyGroupStyle() writes.
struct StyleTokenChange {
    /// Empty for `shared.tokens`, else the theme name (`<shared.themes>/<theme>.xts`).
    std::string theme;
    std::string name;
    /// The project's value before (for an added token: the implied layer's value, or empty).
    std::string before;
    std::string after;
    /// True when `before` is the project's own value: not the value the last applied style wrote
    /// (`style` record), or -- without a record -- not the implied layer's value.
    bool custom = false;
};

/// An extension token (not core) that symbols of the project read and the applied style does not
/// declare: it keeps its value, which may not match the new style (56 §56.2 item 6).
struct UncoveredToken {
    std::string name;
    /// Where its value comes from: `<group>` from the `/* added with <Name> from <group> */`
    /// section of `shared.tokens` that declares it, `project` for another declaration there,
    /// `implied` for the implied layer, empty when nothing declares it.
    std::string origin;
    /// The qualified names of the project symbols that read it.
    std::vector<std::string> usedBy;
};

struct StyleReport {
    /// kOk; kInvalidArgument (`error`) for an unnamed project, one without `shared.tokens` or a bad
    /// id; kNotFound when the library, the group, its token file or the theme is missing; kIoError
    /// when a write failed (applyGroupStyle() without dryRun).
    Status status = Status::kOk;
    std::string error;
    std::string library;
    std::string group;
    std::string groupName;
    std::string setVersion;
    std::string theme;
    /// Core tokens the project declared with another value: overwritten.
    std::vector<StyleTokenChange> overwritten;
    /// Core tokens the project (or its theme file) did not declare: appended.
    std::vector<StyleTokenChange> added;
    /// Core tokens the style itself does not declare: the project's values stay.
    std::vector<std::string> missingCore;
    std::vector<UncoveredToken> uncovered;
    /// Project themes that set core tokens and that the style has no theme file for (they keep the
    /// old style's values).
    std::vector<std::string> themesNotProvided;
    /// Font files copied (`<group relative> -> <project relative>`), as 49 §49.3 item 2.
    std::vector<std::string> copied;
    /// Every file the call writes (or would write, with dryRun), project-relative, with the content
    /// before and after; asset copies are new files. The `style` record is in `index.json`.
    std::vector<FileChange> changes;
};

/// Applies the style of the group `groupId` of the mounted library `libraryId` to `project`. Only
/// core tokens are written (47 rule 4): each one the group's `tokens.xts` declares is written into
/// `shared.tokens` -- an existing declaration's value replaced in place (every declaration of the
/// name), a missing one appended in one `/* style <group name> (<library>/<group>) */` section
/// together with the group's `@font-face` blocks of the families its core font tokens name whose
/// files are copied to `<shared.assetRoot>/fonts/` by the copy rule (same content skipped, other
/// content `<stem>-<8 hex><ext>`). With `theme` empty, each group theme file (`tokens-<name>.xts`)
/// is written the same way into `<shared.themes>/<name>.xts` (created when missing); with `theme`
/// (a theme name of the group) the group's base merged with that theme is written as the base and no
/// theme file is written. Extension tokens, symbols and documents are never touched. The `style`
/// record is (re)written. `impliedTokenFiles` (the implied layer's token files, as the preview
/// takes them) give the "before" of added tokens and, without a record, the reference for `custom`.
/// With `dryRun` nothing is written; the report is the same.
StyleReport applyGroupStyle(const LibraryMounts& mounts, const UiProject& project, std::string_view libraryId,
                            std::string_view groupId, std::string_view theme = {}, bool dryRun = false,
                            std::span<const std::string> impliedTokenFiles = {});

/// Writes `changes` under `project`'s root (directories created): `after` for each. kIoError at the
/// first file that cannot be written (the ones before stay written).
Status writeFileChanges(const UiProject& project, const std::vector<FileChange>& changes);

// --- duplicate and detach (56 §56.2 item 2) ------------------------------------------------------

struct DetachedToken {
    /// `--x` as the original reads it, and the new `--<new name lower-case>-<x without -->`.
    std::string from;
    std::string to;
    /// Its first value: the project's value now, `var()` references resolved.
    std::string value;
};

struct DetachReport {
    /// kOk; kInvalidArgument (`error`) for an unnamed project, a bad name or a taken new name;
    /// kNotFound when the symbol file is missing or does not define the symbol.
    Status status = Status::kOk;
    std::string error;
    /// The new symbol's name and project-relative file.
    std::string name;
    std::string file;
    std::vector<DetachedToken> tokens;
    /// Tokens the symbol reads that have no value the project can hold (none declared, or one whose
    /// `url()` lives outside the project): the copy keeps reading the shared name.
    std::vector<std::string> unresolved;
    std::vector<FileChange> changes;
};

/// Plans the independent copy `newName` (a plain symbol name, flat in `<shared.symbols>/`) of the
/// project symbol `qualified` (`Name` or `<ns>/<Name>`): the file is copied with its `<symbol
/// name>` and `symbol(<Name>)` rules renamed, each `var(--x)` it reads renamed to the new token, a
/// namespaced original's bare `<use symbol>` of its namespace siblings qualified and its relative
/// paths re-based; the new tokens are appended to `shared.tokens` (and to each theme file that sets
/// one of them) in one `/* detached: <newName> (from <qualified>) */` section; `index.json` gets an
/// entry with `derivedFrom` (the original's recorded source, or the project) and no `source`, so the
/// copy is the project's own and never compared for updates. `impliedTokenFiles` supply values the
/// project does not declare. Nothing is written.
DetachReport duplicateDetached(const UiProject& project, std::string_view qualified, std::string_view newName,
                               std::span<const std::string> impliedTokenFiles = {});

// --- the implied layer of a project with several groups (47 rule 5) ----------------------------

/// 47 rule 5 (T-741's open item): the group whose `tokens.xts` is the implied token layer of the
/// project (`rootDir`, `uis`; `symbolsDir` its `shared.symbols`). When some UI links symbols of a
/// group (linkedSymbolGroups()): the applied style's group (readProjectStyle()), else -- only when
/// the UIs link two groups or more -- the first linked one; each looked up as `<dir>/groups/<id>` of
/// the library with that id among the standard group's library, `<rootDir>/shared/symbol-library`
/// and `libraryDirs` (in that order). Otherwise, and when the group is not found, the T-498b rule
/// (detectImpliedGroup()), so a project that never applied a style and links one group is unchanged.
/// `*dir` (may be null) receives the group directory -- `standardSymbolDir` itself, as given, for the
/// standard group. False when no group is implied.
bool projectImpliedGroup(const std::string& rootDir, const std::vector<ProjectUi>& uis,
                         const std::string& standardSymbolDir, const std::string& symbolsDir,
                         std::span<const std::string> libraryDirs, SymbolGroup& out,
                         std::string* dir = nullptr);

/// The `tokens.xts` of projectImpliedGroup() (absolute, '/'), empty when none: what the preview
/// compiles take as `CompileOptions::impliedTokenFiles`.
std::string projectImpliedTokenFile(const UiProject& project, const std::string& standardSymbolDir,
                                    std::span<const std::string> libraryDirs);

}  // namespace xt::bundle
