#pragma once

#include <array>
#include <json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "xt/bundle/ui_project.h"
#include "xt/types.h"

/// The writers of the UI Project's own files that the editor's panels and the Editor Service ops
/// share (docs/design/37-work-model.md §37.8.4: one implementation per write; docs/tasks/T-371.md):
/// the UI list (`project.addUi` / `renameUi` / `removeUi`, the UI list panel and the project tab),
/// the project settings (`project.set`, the project tab's form), `AGENTS.md` (`project.setAgents`),
/// the conventions (`conventions.put`, the conventions tab), the linked source folders
/// (`asset.linkFolder`, the source folders dialog) and an asset's nine-slice (`asset.slice`, the
/// asset workbench). Every function that changes `UiProject` writes the project file
/// (`UiProject::save()`) and restores the previous value when that fails. Paths are UTF-8.

namespace xt::bundle {

/// A stable UI id: `[a-z0-9-]+` (22 §22.2).
bool isValidUiId(std::string_view id);

/// The `.xt` text a new blank UI starts from.
extern const char* const kBlankUiDocument;

/// Where a new UI `uiId` goes, relative to the project root: when every UI lives at
/// `<prefix>/<id>/<file>.xt` with one common `<prefix>` (the starter keeps its UIs under `ui/`),
/// `<prefix>/<uiId>/<uiId>.xt`, else `<uiId>/<uiId>.xt`.
std::string defaultUiDocPath(const UiProject& project, std::string_view uiId);

/// Adds UI `id` whose document is `docRel` (relative to the root; '\\' read as '/'), writing
/// `text` there first. kInvalidArgument (with `*error`) for an unnamed project, an invalid or
/// taken id, an empty path, one leaving the project or a file that already exists; kIoError
/// when the document or the project file cannot be written (the new document is removed again).
Status addProjectUi(UiProject& project, std::string_view id, std::string_view docRel, std::string_view text,
                    std::string* error = nullptr);

/// Renames UI `oldId` to `newId` and rewrites what binds to the old id in the global flow graph
/// (`<root>/bundle.xtflow`, else `<root>/shared/bundle.xtflow`): edge endpoints `ui#<oldId>.…`
/// and the `ui` parameter of timeline nodes. `*rewritten` (may be null) receives the
/// project-relative files it rewrote. kOk without change when the ids are equal; kInvalidArgument
/// for an unnamed project, an unknown `oldId`, an invalid or taken `newId`; kIoError when the
/// project file cannot be written (nothing changed). A global flow that does not parse is left
/// alone (a line in `*notes`, may be null).
Status renameProjectUi(UiProject& project, std::string_view oldId, std::string_view newId,
                       std::vector<std::string>* rewritten = nullptr,
                       std::vector<std::string>* notes = nullptr, std::string* error = nullptr);

/// Removes UI `id` from the list (its files stay on disk). kInvalidArgument for an unnamed
/// project, an unknown id or the last UI; kIoError when the project file cannot be written.
Status removeProjectUi(UiProject& project, std::string_view id, std::string* error = nullptr);

/// The project settings the project tab's form and `project.set` edit.
struct ProjectSettings {
    std::string name;
    std::string id;
    std::string game;
    ProjectSpec spec;
};

/// The project's current settings.
ProjectSettings projectSettings(const UiProject& project);

/// Checks `settings` field by field; "" when they can be written, else the first problem
/// ("id: ...", "spec.safeArea.default: ...", ...). Rules: id `[a-z0-9-]+`; safe areas in [0, 0.5];
/// design height in [1, 16384]; clamp min >= 0 and max >= min (or unclamped), DPI floor in
/// [0, 16]; responsive scale `auto` | `none` | `clamp(<min>, <max>)` with 0 < min <= max, position
/// `follow` | `fixed`, visibility `auto` | `show` | `hide`; players in [1, 64]; budgets >= 0 and
/// finite; every platform resolution in [1, 16384]; every language tag and input
/// method non-empty; every number finite.
std::string validateProjectSettings(const ProjectSettings& settings);

/// Writes `settings` into the project (and its project file). kInvalidArgument (with `*error`
/// from validateProjectSettings()) for an unnamed or legacy project or invalid settings; kIoError
/// when the project file cannot be written (nothing changed).
Status setProjectSettings(UiProject& project, const ProjectSettings& settings, std::string* error = nullptr);

/// Replaces the linked source folders (`.xtui:assets.sourceFolders`). kInvalidArgument for an
/// unnamed or legacy project or a folder whose collection is not a collection name; kIoError when
/// the project file cannot be written.
Status setAssetSourceFolders(UiProject& project, std::vector<AssetSourceFolder> folders,
                             std::string* error = nullptr);

/// The project's `AGENTS.md` (absolute, '/' separators); "" for an unnamed project.
std::string agentsPath(const UiProject& project);
/// Writes `AGENTS.md` (atomic). kInvalidArgument for an unnamed project, kIoError on failure.
Status writeAgents(const UiProject& project, std::string_view text, std::string* error = nullptr);

/// The motion spec, relative to the project root.
inline constexpr const char* kMotionSpecPath = "contracts/motion-spec.yaml";
/// The layout templates' directory, relative to the project root.
inline constexpr const char* kLayoutTemplateDir = "conventions/layouts";

/// The YAML text of `doc` as the conventions tab writes a convention file back: the leading
/// comment block of `originalText`, then `doc` with every mapping's keys in the order they appear
/// in `originalText` (unknown keys last). Throws nothing; "" when `doc` cannot be written.
std::string conventionYamlText(std::string_view originalText, const nlohmann::json& doc);

/// Writes the YAML convention file `relFile` (project-relative) from `doc`, as
/// conventionYamlText() against the file's current text. kInvalidArgument for an unnamed project
/// or a path leaving it, kIoError when it cannot be written.
Status writeConventionYaml(const UiProject& project, std::string_view relFile, const nlohmann::json& doc,
                           std::string* error = nullptr);

/// `conventions.put {kind: "motion"}`: sets the top-level key `id` of the motion spec to `value`
/// (a missing file is created). kInvalidArgument for an unnamed project, an empty id, a file that
/// does not parse as a mapping; kIoError when it cannot be written.
Status putMotionSpecEntry(const UiProject& project, std::string_view id, const nlohmann::json& value,
                          std::string* error = nullptr);

/// A layout template name: `[A-Za-z0-9][A-Za-z0-9 _-]*`.
bool isValidLayoutTemplateName(std::string_view name);

/// Writes the layout template `<root>/conventions/layouts/<name>.xt` with `text`. An existing
/// template is replaced only when `replace`. kInvalidArgument for an unnamed project, an invalid
/// name, empty text or an existing template without `replace`; kIoError when it cannot be written.
/// `*relPath` (may be null) receives the project-relative path.
Status saveLayoutTemplate(const UiProject& project, std::string_view name, std::string_view text,
                          bool replace, std::string* relPath = nullptr, std::string* error = nullptr);

/// Sets the nine-slice of the asset `relFile` (`<file>.meta:import.slice`, [left, right, top,
/// bottom] in px; all zero removes it), creating the meta when missing. kInvalidArgument for an
/// unnamed project or a negative inset; else as `assets::ensureMeta()` / `assets::writeMeta()`.
Status setAssetSlice(const UiProject& project, std::string_view relFile, const std::array<int, 4>& slice,
                     std::string* error = nullptr);

}  // namespace xt::bundle
