#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/bundle/bundle.h"
#include "xt/bundle/project_file.h"
#include "xt/types.h"
#include "xt/version.h"

/// UiProject: the editing-side container of Docs/design/30-ui-project.md (§30.1, §30.2, §30.5)
/// and its asset manifest (Docs/design/26-layers-symbols-library.md §26.3.3). Docs/tasks/T-183.md.
///
/// A UI Project is a directory whose root file is `<name>.xtui` (T-214, Docs/design/22-bundle.md
/// §22.11.1; JSON, one `.xtui` per directory). It holds the source form of a bundle (`uis`,
/// `shared`, `publish`, exactly the fields of `bundle.xtproj` v2) plus the project's identity
/// (`name`, `game`, `version`) and its `spec`, and next to it the `conventions/`, `references/`,
/// `contracts/` and `manifest/` directories (each may be missing). `xtframe-pack` turns it into a
/// published bundle through toBundleProject(). A directory with no `.xtui` but a `ui-project.json`
/// (the T-198..T-213 name) or only a `bundle.xtproj` is read as a UI Project too (`legacyKind`
/// says which; the old file is never written) and `upgradeToUiProject()` converts a
/// `bundle.xtproj` in place.
///
/// Paths stored here use '/' separators and are relative to `rootDir`. JSON never leaks into this
/// header; it lives in ui_project.cpp.

namespace xt::bundle {

/// One target platform of `spec.platforms`: a name and the resolutions the UI must fit.
struct SpecPlatform {
    /// Free-form platform name, e.g. "pc", "ps5", "switch".
    std::string name;
    /// Resolutions, `[w, h]` each.
    std::vector<Vec2> resolutions;
};

/// T-345 (25 §25.4.2): `<name>.xtui:spec.responsive { scale, position, visibility }`, the defaults
/// the editor gives new elements (`ui-scale`, the fixed-position action, `media-override`). Kept
/// verbatim: a value is whatever string the file holds.
struct SpecResponsive {
    /// True when the file had a `responsive` object; it is then written back even when every
    /// value is the default.
    bool present = false;
    /// `auto` | `none` | `clamp(<min>, <max>)`.
    std::string scale = "auto";
    /// `follow` | `fixed`.
    std::string position = "follow";
    /// `auto` | `show` | `hide`.
    std::string visibility = "auto";
};

/// `<name>.xtui:spec` (§30.2 row 1). Every field is optional in the file; missing keys read as
/// these defaults.
struct ProjectSpec {
    /// T-407 (43 §43.3): `spec.behavior`, the behavior version (kLatestBehavior in xt/version.h is
    /// the newest; 1 = `flow-signal-coalescing`). Missing = 0 (the pre-beta behavior); a new
    /// project gets kLatestBehavior. Compiled into every UI's IR META and the bundle manifest's
    /// `spec.behavior`. Written back only when it is not 0.
    uint32_t behavior = 0;
    /// Platforms and their resolution sets.
    std::vector<SpecPlatform> platforms;
    /// Safe-area margin as a fraction of the viewport, desktop default (§30.2: 2.5%).
    float safeAreaDefault = 0.025f;
    /// Safe-area margin on consoles (§30.2: 5%).
    float safeAreaConsole = 0.05f;
    /// T-210 (25 §25.4.1 item 2): the design height (`spec.logicalHeight`, default 1080). The
    /// editor canvas is this tall (width = height x aspect) unless a platform resolution says
    /// otherwise, uiscale = output height / logicalHeight, and the manifest's `spec.logicalHeight`
    /// is written from it. Written back only when it is not 1080.
    float logicalHeight = 1080.0f;
    /// T-241 (23 §23.11.2): `spec.uiscaleRule` ("height" | "width" | "shortEdge" | T-345 "fit"); a
    /// missing or unknown value reads as Height, which is not written back.
    UiScaleRule uiscaleRule = UiScaleRule::Height;
    /// T-345 (25 §25.4.2 item 1): `spec.uiscaleClamp: [min, max]` (max `null` = none) and
    /// `spec.uiscaleDpiFloor`; each written only when set.
    UiScaleLimits uiscaleLimits;
    /// T-345 (25 §25.4.2 "專案預設"): `spec.responsive`, the editor's defaults for new elements.
    /// The core never reads it; the three values are kept verbatim.
    SpecResponsive responsive;
    /// Input methods, e.g. "mouse", "gamepad", "touch".
    std::vector<std::string> inputMethods;
    /// Local players the UI must serve.
    int players = 1;
    /// Languages (BCP 47 tags) the project ships.
    std::vector<std::string> languages;
    /// Per-language font fallback chains.
    std::vector<FontFallbackInfo> fontFallback;
    /// Performance budget: UI frame time in milliseconds; 0 = unset.
    float budgetFrameMs = 0.0f;
    /// Performance budget: effect layers per screen; 0 = unset.
    int budgetEffectLayers = 0;
    /// Performance budget: fill rate (screen areas drawn per frame); 0 = unset.
    float budgetFillRate = 0.0f;
    /// T-202: performance budget, particle caps summed over one UI (`budget.particles`); 0 = unset
    /// (the lint's default of 2048 applies).
    int budgetParticles = 0;
    /// T-312a (35 §35.15.1 item 5): performance budget, the largest asset file in bytes
    /// (`budget.assetBytes`); 0 = unset (assets::assetHygiene() does not check). Written only when set.
    uint64_t budgetAssetBytes = 0;
    /// Target engine name, e.g. "example-engine"; empty = unset.
    std::string engine;
    /// Target engine version, e.g. "5.7.1"; empty = unset.
    std::string engineVersion;
};

/// `<name>.xtui:shared`: `bundle.xtproj`'s `shared` plus the UI Project's directories (§30.1).
struct UiProjectShared {
    /// Directory searched after a UI's own directory when resolving assets (§22.2).
    std::string assetRoot = "shared";
    /// Directories scanned for fonts.
    std::vector<std::string> fontDirs;
    /// Stylesheets implicitly loaded before every UI's own `<link>`s.
    std::vector<std::string> styles;
    /// Directories scanned for templates/effects.
    std::vector<std::string> templateDirs;
    /// Symbol library directory (`*.xt` holding `<symbol>`s).
    std::string symbols = "shared/symbols";
    /// The design-token stylesheet; loaded before `styles` when the file exists. Empty = none.
    std::string tokens = "shared/tokens.xts";
    /// Theme directory (`*.xts`, packed to the bundle's `themes/`).
    std::string themes = "shared/themes";
    /// String-table directory (`<lang>.csv`, packed to the bundle's `strings/`).
    std::string strings = "shared/strings";
    /// Motion recipe directory (editor/AI only; never packed).
    std::string recipes = "shared/recipes";
    /// T-305 (Docs/design/35-design-system.md §35.2): the design system directory
    /// (`xt::bundle::design`; editor/AI only, never packed).
    std::string design = "design";
};

/// `<name>.xtui:publish.atlas` (T-199, Docs/design/30-ui-project.md §30.7, 26 §26.3.4): the
/// defaults of `xtframe-pack --atlas`.
struct AtlasOptions {
    bool enabled = false;
    int maxSize = 4096;
    int padding = 2;
    bool mipmaps = false;
    /// "ui" (one page set per UI, the default) or "bundle" (one set for the whole bundle).
    std::string scope = "ui";
};

/// T-212 (Docs/design/22-bundle.md §22.11.2): one place a UI Project was published to,
/// `<name>.xtui:publish.targets[]`.
struct PublishTarget {
    /// Currently only kPublishKindDir.
    std::string kind;
    /// The bundle directory: relative to the project root when it lies inside it, else absolute;
    /// '/' separators.
    std::string path;
    /// UTC time of the last successful publish, ISO 8601 (`2026-09-27T08:00:00Z`); empty = never.
    std::string lastPublished;
};

/// A bundle directory anywhere.
inline constexpr const char* kPublishKindDir = "dir";

/// `<name>.xtui:publish`: `bundle.xtproj`'s `publish` plus the output directory, the
/// languages and the atlas options.
struct UiProjectPublish {
    /// Default output directory for `xtframe-pack` (relative to the project root); empty = `-o` is
    /// required.
    std::string outDir;
    /// Default `--lang` list; empty = every string table.
    std::vector<std::string> languages;
    bool embedImages = false;
    bool embedFonts = false;
    Vec2 thumbnail{320, 180};
    AtlasOptions atlas;
    /// T-212: where the project was published, least recent first (the last entry is the "last
    /// target" Publish on save and Publish Current UI use). Written only when non-empty.
    std::vector<PublishTarget> targets;
};

/// T-312a (35 §35.15.1 item 3): one linked source folder, `<name>.xtui:assets.sourceFolders[]`.
/// xt::bundle::assets::sourceFolders() gives the parsed form.
struct AssetSourceFolder {
    /// Absolute directory, '/' separators.
    std::string path;
    /// The collection its files are imported into.
    std::string collection;
    /// The `import` settings as one JSON object text; empty when there are none.
    std::string importJson;
};

/// T-312a: `<name>.xtui:assets` (a new key; written only when it has content).
struct UiProjectAssets {
    std::vector<AssetSourceFolder> sourceFolders;
    /// Keys of `assets` this reader does not know, as one JSON object text; empty when none.
    std::string extraJson;
};

/// T-214 (§22.11.1): which old root file a UiProject was read from. Both are read-only: save()
/// always writes `<name>.xtui` and leaves the old file as it is.
enum class LegacyKind {
    /// Read from a `.xtui` (or created in memory).
    kNone,
    /// Read from `ui-project.json` (the T-198..T-213 name; same content as `.xtui`).
    kUiProjectJson,
    /// Read from `bundle.xtproj` (`legacy` is then true as well).
    kBundleXtproj,
};

/// A parsed `<name>.xtui` (or a `ui-project.json` / `bundle.xtproj` read as one).
struct UiProject {
    /// Format version of the `.xtui` content: always the latest (kUiProjectFormatVersion) after
    /// load(), which migrates older files (T-406, 43 §43.2).
    int version = 1;
    /// T-406: the version the file had when load() migrated it to the latest; 0 when it already was
    /// the latest (or for a project not read from a `.xtui`). Not stored in the JSON.
    int migratedFrom = 0;
    /// Human-readable project (and bundle) name.
    std::string name;
    /// Stable bundle id; `[a-z0-9-]+` (§22.2).
    std::string id;
    /// The game this UI belongs to.
    std::string game;
    /// §30.2 row 1.
    ProjectSpec spec;
    /// The UIs, same fields as `bundle.xtproj` v2.
    std::vector<ProjectUi> uis;
    UiProjectShared shared;
    /// Editor preview device pixel ratio.
    float previewDpr = 1.0f;
    /// T-344 (Docs/design/17-editor.md §17.31): `preview.scene`, what the editor's Demo space
    /// draws the UI over -- empty (the default, not written): the document's `editor-only`
    /// content; "black", "grey" or "white": that color; anything else: an image file, relative to
    /// the project root.
    std::string previewScene;
    /// "desktop" or "engine".
    std::string target = "engine";
    UiProjectPublish publish;
    /// T-312a: `assets` (source folders).
    UiProjectAssets assets;
    /// The project directory (no trailing '/'). Not stored in the JSON.
    std::string rootDir;
    /// True when this was read from a `bundle.xtproj` (no `.xtui` nor `ui-project.json` in
    /// `rootDir`): the content is `bundle.xtproj`-shaped and pack() takes the ProjectFile path.
    bool legacy = false;
    /// T-214: the old root file this was read from, for the UI to show; kNone after a save.
    LegacyKind legacyKind = LegacyKind::kNone;
    /// Top-level keys load() did not recognise, kept as one JSON object text so save() writes them
    /// back unchanged; empty when there were none.
    std::string extraJson;
    /// Likewise for unrecognised `spec` keys.
    std::string extraSpecJson;
    /// T-406 (43 §43.2): likewise for unrecognised `shared`, `preview` and `publish` keys.
    std::string extraSharedJson;
    std::string extraPreviewJson;
    std::string extraPublishJson;
    /// T-212 (§22.11.1): an in-memory project around a lone `.xt` (unnamedFor()). It has no
    /// `.xtui`; save() refuses it and path()/dir() are empty.
    bool unnamed = false;

    /// True for the in-memory project of a lone `.xt` (and for a default-constructed one).
    bool isUnnamed() const { return unnamed || rootDir.empty(); }
    /// T-214: the root file save() writes, `<rootDir>/<fileName()>`; empty while unnamed.
    std::string path() const;
    /// The project directory; empty while unnamed.
    std::string dir() const { return isUnnamed() ? std::string() : rootDir; }
    /// T-214: the root file's name, uiProjectFileName(name, id).
    std::string fileName() const;
    /// The last entry of `publish.targets`, or nullptr.
    const PublishTarget* lastTarget() const;

    /// T-214: reads a UI Project. `path` is a directory or a root file:
    ///   - a directory: its one `*.xtui` (more than one -> kInvalidArgument "more than one .xtui");
    ///     none -> `ui-project.json` (legacyKind kUiProjectJson); none -> `bundle.xtproj`
    ///     (legacyKind kBundleXtproj, `legacy` = true; `shared.themes`/`shared.strings` are then
    ///     "themes"/"strings", the bundle root's directories, and `shared.tokens` is empty);
    ///   - a `*.xtui`, `ui-project.json` or `bundle.xtproj` file: that file (`rootDir` = its
    ///     directory); any other file -> kInvalidArgument.
    /// Errors as ProjectFile::load(): kNotFound when no root file can be read, kIoError for invalid
    /// JSON, kUnsupported for a `version` newer than kUiProjectFormatVersion (T-406: a missing one
    /// reads as 1, an older one is migrated and migratedFrom records it), kInvalidArgument for a
    /// missing/malformed `id`, a missing `uis`, a UI without `id`/`doc`, a malformed or repeated UI
    /// id.
    static Result<UiProject> load(std::string_view path, std::string* error = nullptr);

    /// The UI Project that `upgradeToUiProject()` writes for `project`: every `bundle.xtproj` field
    /// carried over, `shared.themes`/`strings` = "themes"/"strings", `shared.tokens` empty (so the
    /// pack result is unchanged), default `spec`, `legacy` = false.
    static UiProject fromBundleProject(const ProjectFile& project);

    /// The implied UI Project of a lone `.xt` file (§30.1): ProjectFile::implied() plus defaults.
    static UiProject implied(std::string_view xtPath);

    /// T-212 (§22.11.1): the unnamed UI Project of a lone `.xt`: `id` and the one UI's id are the
    /// file stem made a valid id (ProjectFile::implied()), `name` is empty, `rootDir` is the file's
    /// directory, `shared` keeps the `.xtui` defaults (relative to that directory),
    /// `unnamed` = true, `legacy` = false.
    static UiProject unnamedFor(std::string_view xtPath);

    /// Writes path() (`<rootDir>/<name>.xtui`, see uiProjectFileName()): the key order of the
    /// report in Docs/tasks/T-183.md, 2-space indent, UTF-8, LF, trailing newline; byte-stable.
    /// T-214: every other `*.xtui` in `rootDir` is then deleted (one per directory, so a renamed
    /// project leaves no old `.xtui` behind; a case-only rename renames the file). A
    /// `ui-project.json` or `bundle.xtproj` next to it is never touched. The caller clears
    /// `legacyKind`/`legacy` after a successful save. kInvalidArgument without a `rootDir` or for an
    /// unnamed project, kIoError when the file cannot be written.
    Status save(std::string* error = nullptr) const;

    /// The `bundle.xtproj`-shaped view pack() and the editor work on. `shared.styles` gets
    /// `shared.tokens` prepended when that file exists and is not already listed.
    ProjectFile toBundleProject() const;

    /// The UI with this id, or nullptr.
    const ProjectUi* ui(std::string_view id) const;
};

/// T-214 (§22.11.1): extension of the UI Project root file `<name>.xtui`.
inline constexpr const char* kUiProjectExtension = ".xtui";
/// T-214: the T-198..T-213 root file name, read-only (in-memory upgrade).
inline constexpr const char* kLegacyUiProjectFileName = "ui-project.json";

/// T-214: the root file name for a project: `<name>.xtui`, or `<id>.xtui` when `name` is empty or
/// unusable as a file name (a path separator, `<>:"|?*`, a control character, text that is not
/// well-formed UTF-8, a leading/trailing space, a trailing '.', "." / "..", or a Windows device
/// name such as CON). T-237: UTF-8 letters ("測試專案") are kept.
std::string uiProjectFileName(std::string_view name, std::string_view id);

/// T-214: true when `path`'s file name is a UI Project root file: `*.xtui` (any case),
/// `ui-project.json` or `bundle.xtproj`. Does not touch the disk.
bool isUiProjectFilePath(std::string_view path);

/// T-214: the root file load() would read in directory `dir` ('/' separators): its one `*.xtui`,
/// else `ui-project.json`, else `bundle.xtproj`; empty when there is none or (with `*error` set)
/// when there is more than one `.xtui`. `*kind` (may be null) receives the file's LegacyKind.
std::string findUiProjectFile(std::string_view dir, LegacyKind* kind = nullptr, std::string* error = nullptr);

/// True when `dir` holds a `*.xtui` or a `ui-project.json` (a UI Project, not just a
/// `bundle.xtproj`).
bool hasUiProjectFile(std::string_view dir);

/// 29.6's calibrated defaults, the content `upgradeToUiProject()` writes to
/// `contracts/motion-spec.yaml`.
std::string_view defaultMotionSpecYaml();

/// The `AGENTS.md` template `upgradeToUiProject()` writes.
std::string_view agentsTemplate();

// --- T-212: creating, copying and adopting UI Projects (Docs/design/22-bundle.md §22.11) ---

/// `name` as a UI Project / UI id: ASCII letters lowercased, digits kept, every run of other
/// characters becomes one '-', leading/trailing '-' dropped. Empty when nothing is left.
std::string slugifyId(std::string_view name);

/// The text of an empty UI document (`<head>` + `<body>`), what a blank project's first UI holds.
std::string_view blankUiDocument();

/// Creates a UI Project in `dir`, which must not exist or be an empty directory.
///   - `templateDir` non-empty: that directory (a UI Project, e.g. `Examples/ui-project-starter`)
///     is copied, except its `out/` and `publish/` directories and its root files (`*.xtui`,
///     `ui-project.json`, `bundle.xtproj`); then `name` and `id` are set and `<name>.xtui`
///     written.
///   - `templateDir` empty: `<name>.xtui` with one UI `main` at `main/main.xt`
///     (blankUiDocument()) and an empty `shared/`.
/// T-305: then design::upgradeDesign() runs (a `design/` directory, the template's `references/`
/// moved into it). `*out` (may be null) receives the loaded project. kInvalidArgument for a
/// malformed `id`, a non-empty `dir` or a template without `.xtui`/`ui-project.json`; kIoError
/// when something cannot be written.
Status createUiProject(std::string_view dir, std::string_view name, std::string_view id,
                       std::string_view templateDir, UiProject* out, std::string* error = nullptr);

/// Copies the standard symbols `project`'s UIs link but the project does not hold yet: for every
/// `<link rel="symbols" href>` of every UI document that resolves to a missing
/// `<shared.symbols>/<Name>.xt`, `<standardSymbolDir>/<Name>.xt` (then `mobile/<Name>.xt`, then
/// `list/<Name>.xt`) is copied there -- what `xtframe-pack` does on a first pack
/// (Docs/design/30-ui-project.md 30.4), done up front so a project made from the starter compiles the moment
/// it is opened. Existing files are never overwritten; `xt-standard:` links need no copy. `*copied` (may be
/// null) receives the project-relative paths written. kIoError when a copy cannot be written.
Status pullStandardSymbols(const UiProject& project, std::string_view standardSymbolDir,
                           std::vector<std::string>* copied = nullptr);

/// T-408 (Docs/design/43-compatibility-and-versions.md §43.6, 30 §30.4): one project copy of a
/// standard symbol, as diffStandardSymbols() reports it.
struct StandardSymbolDiff {
    /// The symbol's index name (`Button`).
    std::string name;
    /// The copy, project-relative (`shared/symbols/Button.xt`).
    std::string file;
    /// The library file, relative to the library (`Button.xt`, `mobile/TouchButton.xt`).
    std::string standard;
    /// `current` (same text as the library), `outdated` (the text of an earlier library version:
    /// a newer one is available and the copy was not edited) or `modified` (none of the known
    /// library versions: edited in the project, maybe also behind the library).
    std::string status;
    /// `outdated` / `modified`: a unified line diff from the library file (`--- standard/<file>`)
    /// to the copy (`+++ <file>`), 2 lines of context; empty for `current` or when either file is
    /// longer than 4000 lines.
    std::string diff;
};

/// T-408: compares every file of `<shared.symbols>/` named like a symbol of
/// `<standardSymbolDir>/index.json` (root, `mobile/`, `list/`) with that library file. Texts are
/// compared with CRLF read as LF; a symbol's earlier versions are the index's `previousContent`
/// hashes (`sha256:<hex>`, kept by Tools/Scripts/gen_symbols_index.py). Reads only -- nothing is copied or
/// overwritten (the project holds copies, never live links to the library). Rows are sorted by
/// `file`. kNotFound when `standardSymbolDir` has no readable index.json.
Status diffStandardSymbols(const UiProject& project, std::string_view standardSymbolDir,
                           std::vector<StandardSymbolDiff>& out);

/// Copies the project directory `project.rootDir` to `dir` (every file, except a `dir` nested
/// inside it) and returns the copy in `*out` (same content, `rootDir` = `dir`, `legacy` kept).
/// kInvalidArgument for an unnamed project, a non-empty `dir` or `dir` = the project directory;
/// kIoError when a file cannot be copied.
Status copyUiProject(const UiProject& project, std::string_view dir, UiProject* out,
                     std::string* error = nullptr);

/// Turns the unnamed project `unnamed` into a UI Project in `dir` (must not exist or be empty):
/// creates `dir`, `dir/shared/` and `dir/<ui id>/`, moves every file of `files` (absolute paths
/// under `unnamed.rootDir`: the `.xt` first, then its `.xts`, `.xtflow`, sidecar, ...) to the same
/// relative place under `dir/<ui id>/`, and writes `<name>.xtui` (`id` = the unnamed id,
/// `name` = `dir`'s base name, the unnamed project's one UI with `doc` = `<ui id>/<file name>`).
/// Missing files are skipped; files outside `unnamed.rootDir` are left alone. `*out` (may be
/// null) receives the project. kInvalidArgument for a named project, a non-empty `dir` or an
/// empty `files`; kIoError when a file cannot be moved or written.
Status adoptAsUiProject(const UiProject& unnamed, const std::vector<std::string>& files, std::string_view dir,
                        UiProject* out, std::string* error = nullptr);

/// Records a successful publish in `project.publish`: `path` (absolute) is stored relative to the
/// project root when it lies inside it; an entry of the same kind and path is moved to the end,
/// else one is appended; its `lastPublished` becomes `timestamp`. Kind kPublishKindDir also sets
/// `publish.outDir` to the stored path.
void recordPublish(UiProject& project, std::string_view kind, std::string_view path,
                   std::string_view timestamp);

/// A stored target path made absolute (a relative one resolves against `project.rootDir`), '/'
/// separators.
std::string resolveProjectPath(const UiProject& project, std::string_view path);

/// What upgradeToUiProject() created, as paths relative to the project root.
struct UpgradeReport {
    std::vector<std::string> created;
};

/// `xtframe-pack --upgrade <dir>` (§30.1, 12.7): reads `<dir>/bundle.xtproj`, writes
/// `<name>.xtui` (fromBundleProject()), creates `conventions/`, `contracts/` and `manifest/`,
/// writes `AGENTS.md` and `contracts/motion-spec.yaml` unless they exist, and then runs
/// design::upgradeDesign() (T-305: `design/` replaces the top-level `references/`).
/// Deletes nothing. kInvalidArgument when a `.xtui` or `ui-project.json` already exists (T-214:
/// open a `ui-project.json` and save to upgrade it), ProjectFile::load()'s
/// status when `bundle.xtproj` cannot be read, kIoError when a file or directory cannot be
/// written.
Status upgradeToUiProject(std::string_view dir, UpgradeReport* report = nullptr,
                          std::string* error = nullptr);

// --- Asset manifest (Docs/design/26-layers-symbols-library.md §26.3.3) ---

/// An asset's measured fields (`measured` in `manifest/assets.yaml`). Each `has*` flag says the
/// field is present; xtframe-pack fills the missing ones from the file.
struct AssetMeasured {
    bool hasSize = false;
    int w = 0;
    int h = 0;
    bool hasAlpha = false;
    bool alpha = false;
    bool hasSlice = false;
    /// Nine-slice margins L, R, T, B in pixels (§26.3.4 `slice: L R T B`).
    int slice[4] = {0, 0, 0, 0};
};

/// One entry of `manifest/assets.yaml`.
struct AssetEntry {
    /// File stem, kebab-case (§26.3.3).
    std::string id;
    /// "image", "vector", "font", "anim", "video", ...
    std::string type;
    /// The file, relative to the project root.
    std::string file;
    /// Provenance: "manual", "import:<design>", "ai:<scenario>", "external:<where>".
    std::string source;
    /// "draft", "approved", "stale" or "pending"; missing reads as "draft".
    std::string status = "draft";
    /// License; required when `source` is "ai..." or "external..." (§26.3.3).
    std::string license;
    AssetMeasured measured;
    /// Scalar keys this reader does not know, kept for save() as (key, YAML scalar text).
    std::vector<std::pair<std::string, std::string>> extra;
};

/// `manifest/assets.yaml`.
struct AssetsManifest {
    std::vector<AssetEntry> assets;
    /// T-262b (F10): set by parse() when the text holds something save() cannot reproduce -- a
    /// comment (other than the header line toYaml() writes), a non-scalar value of a key this
    /// reader does not know, or an unknown top-level key. The packer then leaves the file alone.
    bool hasUnsavableContent = false;

    /// Parses the YAML text. false (with `error` set) on malformed YAML or when `assets` is not a
    /// list of mappings. Non-scalar keys this reader does not know are dropped (and flagged in
    /// `hasUnsavableContent`).
    static bool parse(std::string_view text, AssetsManifest& out, std::string* error = nullptr);
    /// The YAML text save() writes: a header comment, `version: 1`, then one block mapping per
    /// asset (`id, type, file, source, status, license, measured, <extra>`); `measured` is a flow
    /// mapping `{ w, h, alpha, slice: [L, R, T, B] }` of its present fields.
    std::string toYaml() const;
    /// Reads and parses a file. kNotFound when it cannot be read, kInvalidArgument when malformed.
    static Result<AssetsManifest> load(std::string_view path, std::string* error = nullptr);
    /// Writes toYaml() to `path`. kIoError on failure.
    Status save(std::string_view path) const;
    /// The entry with this id, or nullptr.
    const AssetEntry* find(std::string_view id) const;
};

/// Path of the asset manifest relative to a project root.
inline constexpr const char* kAssetsManifestPath = "manifest/assets.yaml";

/// What measureImage() found.
struct ImageMeasure {
    /// False when the bytes are not a PNG/JPEG/SVG this reader understands.
    bool ok = false;
    int w = 0;
    int h = 0;
    /// PNG: an alpha channel (color type 4/6) or a `tRNS` chunk. JPEG: never. SVG: always (areas
    /// it does not paint are transparent).
    bool alpha = false;
};

/// Reads the size/alpha of PNG, JPEG or SVG bytes (by content, not by `ext`, except that an SVG
/// is only recognised when `ext` is ".svg"). SVG size: the root's `width`/`height` (px or unitless
/// numbers), else its `viewBox`.
ImageMeasure measureImage(std::string_view bytes, std::string_view ext);

/// One asset-manifest lint finding (12.7 lint category `assets`).
struct AssetIssue {
    bool isError = false;
    /// The entry's id ("" for file-level findings).
    std::string id;
    /// Message without the "assets: " prefix.
    std::string message;
};

/// Validates `manifest` against the files under `rootDir` and fills the missing `measured` fields
/// of measurable (PNG/JPEG/SVG) files (§26.3.3). Rules: an entry needs an `id` (unique) and a
/// `file`; `status` is one of draft/approved/stale/pending; the file must exist; declared `w`/`h`
/// must equal the file's; declared `alpha: true` needs an alpha channel; `slice` needs a size and
/// must leave a stretchable centre (L+R < w, T+B < h, no negative margin); `source` "ai..." or
/// "external..." needs a `license`. When `referenced` is non-null, each entry whose file (as a
/// lexically normal absolute generic path) is not in it gets an "unreferenced" warning. Sets
/// `*filled` when a field was filled in.
std::vector<AssetIssue> checkAssets(AssetsManifest& manifest, std::string_view rootDir,
                                    const std::vector<std::string>* referenced, bool* filled);

/// The canonical form checkAssets() compares `referenced` paths in: absolute, lexically normal,
/// '/' separators.
std::string canonicalAssetPath(std::string_view path);

}  // namespace xt::bundle
