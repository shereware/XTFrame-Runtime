#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "xt/compile/compile.h"

/// T-589 (Docs/design/54-source-format-v2.md §54.10-§54.12): the source format 1 -> 2 migrator
/// and the computed-style dump its self-check compares. R layer (xt_compile); `xtframe-compile
/// --migrate`, `xtframe-compile --dump-computed` and `xtframe-pack --upgrade --format 2` call it.
///
/// The migration takes a UI Project directory (every `.xt` under it; hidden, `build`, `dist`,
/// `bin` and `node_modules` directories left out) or a single `.xt`, together with every `.xts`
/// they `<link>` and every symbol library they `<link rel="symbols">` (followed outside the
/// directory too; `xt-standard:` libraries are not followed), and returns the version 2 text of
/// each file (written with the canonical writer, xt/compile/canonical_writer.h) plus a report. It
/// writes nothing; writeMigratedFiles() does, without backup files (54 §54.10 D10).
///
/// The eight steps of §54.10, per `.xt`:
///   1. `<link href>` -> `<style src>` (plus `<layout src>` / `<timelines src>` for the parts of
///      the sheet that move, written to `<stem>.layout.xts` / `<stem>.timelines.xts` next to it);
///      `<link rel="symbols">` -> `<symbols src>`; an inline `<style>` -> inline sections; every
///      `<symbol>` -> a direct child of `<xt>`. A symbol library's own `<link>`s, which version 1
///      never loaded (XF-017), are dropped with a note, so the library keeps its behavior.
///   2. `port:` attributes -> `<ports>`: the `<body>`'s and the UI root element's (a direct element
///      child of `<body>`) into the document's `<ports>`, a symbol's first element's into the
///      symbol's `<ports>`; declaration order. For a UI root port, every `"#<root id>.<port>"`
///      endpoint of the document's sibling `<stem>.xtflow` becomes `"#document.<port>"` (ruling of
///      53 "v2 document port addressing", T-604). A port declared on any other element is a
///      migration error: 54 §54.4 has no place for it, and moving it would change its
///      `#<id>.<port>` name.
///   3. `@timeline` (with `from`, and inside `symbol(X) {}`) -> `<timelines>`.
///   4. `on-<event>` / `state-enter` / `state-exit` rule triggers -> `on:<event>` on the element
///      (`#<id>` of a document element, or a simple selector matched statically against the
///      document's elements), `on:<event>@<path>` on the `<use>` (`#<use id>/<path>`, or a
///      `symbol(X) { #<id> }` trigger of a library symbol: on every `<use symbol="X">` with an id),
///      or on the symbol's own element (`symbol(X) { #<id> }` of a symbol defined in the same
///      file). An element that already has `on:<event>` keeps its own (version 1's rule).
///      Anything else is a migration error.
///   5. Every rule's declarations are split by the section table (xts_section_table.inc,
///      46 §46.14): the layout ones into a `<layout>` rule, the rest into a `<style>` rule, same
///      selector, same `@media` / `symbol(X)` wrapper; nothing is flattened. `display` always goes
///      to `<layout>` (so every declaration of one property stays in one kind of section).
///   6. The layout declarations of an inline `style="..."` move to a `#<id>` rule in the file's
///      last `<layout>` section (the symbol's own `<layout>` inside a `<symbol>`); an element
///      without an id gets one by the rule of the editor's `Document::autoName()`
///      (Code/Editor/Core/src/document_names.cpp, reimplemented in migrate_markup.cpp).
///   7. With MigrateOptions::anchors, a `<layout>` rule whose `position: absolute` box is written
///      in the anchor formulas of 54 §54.5.1 (`1safe-left`, `calc(1safe-left + X)`, the `right` /
///      `top` / `bottom` forms, the `stretch` pair and `calc(50% - W / 2 + X)` for `center`) is
///      rewritten into `anchor` / `offset` / `offset-end` / `size`; any other form is left as is.
///   8. The canonical v2 text.

namespace xt::compile {

/// Options of migrateToV2().
struct MigrateOptions {
    /// Step 7 (`--anchors`).
    bool anchors = false;
    /// Compile every migrated UI document before and after (both from memory) and compare: layer
    /// one (the IR bytes) and layer two (dumpComputedStyle()), 54 §54.11.
    bool selfCheck = true;
    /// The compile options of the self-check for the document at this absolute path (root
    /// directory, implied stylesheets...); unset: default options. `emitMeta` and `xtDir` are set
    /// by the migrator; `standardSymbolDir` below is used when the returned one is empty.
    std::function<CompileOptions(const std::string& xtPath)> compileOptions;
    /// Where `xt-standard:<Name>` resolves in the self-check.
    std::string standardSymbolDir;
};

/// One file the migration writes.
struct MigratedFile {
    /// Absolute path (generic separators, lexically normal).
    std::string path;
    /// The new text (LF line endings).
    std::string text;
    /// A new file (a split-off `<stem>.layout.xts` / `<stem>.timelines.xts`).
    bool created = false;
};

/// Layer one / layer two outcome of one document.
enum class MigrateCheck : uint8_t {
    NotChecked = 0,  ///< Not a UI document, selfCheck off, or version 1 does not compile.
    Equal = 1,
    Differ = 2,
};

/// What the migration did to one file.
struct MigrateFileReport {
    /// Absolute path (generic separators).
    std::string path;
    /// "ui", "library", "fragment" or "flow" (a rewritten sibling `.xtflow`).
    std::string kind;
    /// "migrated", "skipped" (`reason` says why: already version 2, does not parse) or "error".
    std::string status;
    std::string reason;
    /// Step 5: rules that became a layout rule and a style rule.
    uint32_t rulesSplit = 0;
    /// Step 7: rules rewritten into `anchor` / `offset` / `size`.
    uint32_t anchors = 0;
    /// Step 4: rule triggers written onto elements.
    uint32_t triggersMoved = 0;
    /// Step 2: `port:` attributes moved into `<ports>`.
    uint32_t portsMoved = 0;
    /// Step 6: inline styles whose geometry moved into a `#id` layout rule.
    uint32_t inlineMoved = 0;
    /// Step 6: `<tag> line <n> -> <id>` of every element that got an id.
    std::vector<std::string> idsAssigned;
    MigrateCheck layerOne = MigrateCheck::NotChecked;
    /// Layer one without the `SRCM` chunk (its element lines move with the rewritten text).
    MigrateCheck layerOneWithoutSourceMap = MigrateCheck::NotChecked;
    MigrateCheck layerTwo = MigrateCheck::NotChecked;
    /// Layer two differs: the first differing line of the two dumps; not checked: why.
    std::string checkDetail;
    /// Informational notes and migration errors, one line each.
    std::vector<std::string> notes;
    std::vector<std::string> errors;
};

/// The whole run.
struct MigrateReport {
    /// The directory every output path is relative to (the common ancestor of the input and every
    /// file touched).
    std::string root;
    std::vector<MigrateFileReport> files;
    /// True when no file has an error and no checked document differs in layer two.
    bool ok() const;
    /// The report as JSON (paths relative to `root`).
    std::string json() const;
    /// A short human summary: one line per file with something to say, then the totals.
    std::string summary() const;
};

/// Migrates `input` (a UI Project directory or a `.xt`), see the file comment. `out` gets every
/// file to write, `report` one entry per file seen. Returns report.ok().
bool migrateToV2(const std::string& input, const MigrateOptions& options, std::vector<MigratedFile>& out,
                 MigrateReport& report);

/// Writes `files` in place (`outDir` empty) or under `outDir` at their path relative to
/// `report.root`. No backup files. False with `error` set when a file cannot be written.
bool writeMigratedFiles(const std::vector<MigratedFile>& files, const MigrateReport& report,
                        const std::string& outDir, std::string& error);

/// The computed-style dump of an `.xtir` (54 §54.11 layer two; `xtframe-compile --dump-computed`):
/// for every DOCT element (by index and tag path; ids are left out, so an id the migration assigns
/// does not count), every pseudo-class combination the rule set uses (applied to the element and
/// its ancestors) and every `@media` condition (none holding, then each distinct one alone), the
/// cascade winner of every property -- STYL rules and inline declarations, by importance,
/// specificity and order -- with its value decoded independently of pool offsets and string ids;
/// plus the design tokens. Deterministic JSON. False (and `error`) for an IR that does not open.
bool dumpComputedStyle(std::span<const uint8_t> ir, std::string& json, std::string& error);

}  // namespace xt::compile
