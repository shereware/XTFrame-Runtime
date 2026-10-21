#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/bundle/design_system.h"
#include "xt/bundle/ui_project.h"
#include "xt/types.h"

/// The Claude Design design-system exchange (docs/design/35-design-system.md §35.4, §35.7.1
/// `design.export {format: claude-design}`, §35.12 (24); docs/design/24-design-import.md §24.8;
/// docs/tasks/T-316.md items 1 and 2). Files only: nothing here talks to a network or an API.
///
/// The exchange folder holds:
///   - `README.md`: sections 1 Identity, 2 Voice, 3 Color, 4 Type, 5 Space and shape, 6 Motion,
///     8 Components and 11 Decisions of buildDocument() (images, status marks, Deprecated lists and
///     the "UIs" columns left out; empty sections dropped), headed `## <Title>`, after a short
///     paragraph saying the values are the project's tokens.
///   - `tokens.json`: the shape of `integrations/claude-design/skills/xtui/references/tokens.json`
///     (`name`, `version`, `themes{<theme>{<key>: value}}`, `font{family, <role>{size, weight, ...}}`,
///     `space[]`, `radius{}`, `shadow{}`, `board{}`). `themes.base` holds every colour variable of
///     `shared.tokens` (`--color-` dropped, kebab-case to camelCase); every `shared.themes/*.xts`
///     is one more theme listing only the colour variables it overrides.
///   - `palette.json` (an XTFrame addition): the colour library's groups and swatches.
///   - `components.md`: the symbol library (name, states, slots, ports, usage notes).
///
/// The import reads the same files back (only `README.md` is required). Everything it writes is a
/// draft with `source: {kind: import:claude-design, ref: <file>#<anchor>}`; it never writes
/// `approved` and never overwrites an existing variable value (a value is written only where the
/// stylesheet does not declare that variable yet). Importing the same folder again updates the
/// entries with the same `source.ref` (their status goes back to draft) instead of adding new ones.

namespace xt::bundle::design {

/// The `source.kind` of every entry the import writes.
inline constexpr std::string_view kClaudeDesignSourceKind = "import:claude-design";

/// What exportClaudeDesign() wrote.
struct ExportReport {
    /// The files written, relative to the output directory, in writing order.
    std::vector<std::string> files;
    /// The README section titles, in order.
    std::vector<std::string> sections;
    /// `tokens.json` theme keys (`base` first).
    std::vector<std::string> themes;
    /// Colour variables in `themes.base`.
    int colorTokens = 0;
    /// Swatches in `palette.json`.
    int swatches = 0;
    /// Symbols in `components.md`.
    int symbols = 0;
    /// What did not fit the exchange shape (one line each).
    std::vector<std::string> notes;
    /// Why the export failed (empty on success).
    std::string error;
};

/// Writes the exchange folder of `ds` (loaded from `project`; `graph` = buildGraph() of the two)
/// into `outDir` (created when missing; only the four files above are written, each atomically).
/// `standardSymbolDir` (T-316 addition, may be empty) is the standard symbol library (the directory
/// holding its `index.json`), read for the Components section and `components.md` as by
/// BuildOptions::standardSymbolDir. kInvalidArgument for an unnamed project or an empty `outDir`,
/// kIoError when a file cannot be written (`report->error` says which; `report` may be null).
Status exportClaudeDesign(const UiProject& project, const DesignSystem& ds, const Graph& graph,
                          std::string_view outDir, ExportReport* report,
                          std::string_view standardSymbolDir = std::string_view());

/// T-316 addition: how importClaudeDesign() writes variable values.
struct ImportOptions {
    /// True: base variable values missing from `shared.tokens` are written into that stylesheet.
    /// False: they are only listed in ImportReport::baseValues, for a caller that writes them
    /// through its own document (the Editor Service, one undo step). Theme files are always
    /// written.
    bool writeBaseValues = true;
};

/// What importClaudeDesign() did. "Updated" counts only entries whose content or status changed.
struct ImportReport {
    int textsAdded = 0;
    int textsUpdated = 0;
    int tokensAdded = 0;
    int tokensUpdated = 0;
    int swatchesAdded = 0;
    int swatchesUpdated = 0;
    int groupsAdded = 0;
    /// Variables declared in `shared.tokens` by this import (or listed in `baseValues`).
    int valuesWritten = 0;
    /// Variables declared in theme files by this import.
    int themeValuesWritten = 0;
    /// Entries left alone because an entry with the same id exists that this import did not write.
    int skipped = 0;
    /// `--name` -> value: the base values the stylesheet did not declare (written or, with
    /// `writeBaseValues == false`, to be written by the caller), in `tokens.json` order.
    std::vector<std::pair<std::string, std::string>> baseValues;
    /// The files read, relative to the folder.
    std::vector<std::string> files;
    /// What was not imported and why (one line each).
    std::vector<std::string> notes;
    /// Why the import failed (empty on success).
    std::string error;

    int added() const { return textsAdded + tokensAdded + swatchesAdded + groupsAdded; }
    int updated() const { return textsUpdated + tokensUpdated + swatchesUpdated; }
};

/// Reads the exchange folder `dir` into `project`'s design system (loaded, changed and saved here):
///   - `README.md` (required): every `##` section (`#` when the file has no `##` heading) becomes a
///     text entry `text/imported/<slug>.md` whose `section` follows its title (Identity -> identity,
///     Voice / Tone -> voice, Color / Colours / Palette -> color, Type / Typography -> type, Space
///     and shape / Spacing -> space, Motion -> motion, Layout -> layout, Components -> components,
///     Imagery / Iconography -> imagery, Screens -> screens, Decisions -> decisions; an unknown
///     title -> identity), `ref: README.md#<anchor>`. Text before the first section is not imported.
///   - `tokens.json`: `themes.base` -> draft variable metadata (category color) and, for variables
///     the stylesheet lacks, values; every other theme -> `<shared.themes>/<theme>.xts` (an existing
///     file only gains the variables it lacks). Keys go back as `--color-<kebab-case>`.
///   - `palette.json`: groups and swatches (drafts).
///   - `components.md`: one text entry (`section: components`, `ref: components.md`).
/// The project needs a design directory (kNotFound otherwise: run upgradeDesign() first).
/// kNotFound when `dir` or its `README.md` does not exist, kInvalidArgument for an unnamed project,
/// a file over its size limit or malformed JSON, kIoError when a file cannot be written.
/// Nothing is saved unless everything was read.
Status importClaudeDesign(const UiProject& project, std::string_view dir, ImportReport* report,
                          const ImportOptions& options = ImportOptions());

}  // namespace xt::bundle::design
