#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/canonical_writer.h"
#include "xt/compile/dom.h"

/// T-589: the pieces of the source format v2 migrator (xt/compile/migrate.h) shared by
/// migrate_v2.cpp (the driver), migrate_sheet.cpp (steps 3, 4's collection, 5 and 7 on CSS text)
/// and migrate_markup.cpp (steps 2, 4's placement and 6 on element markup).

namespace xt::compile::migrate {

// ---- CSS (migrate_sheet.cpp) ---------------------------------------------------------------

/// One `on-<event>` / `state-enter` / `state-exit` declaration of a version 1 rule (step 4).
struct TriggerRule {
    /// The enclosing `symbol(<name>)`, or empty.
    std::string symbol;
    /// One selector of the rule's selector list (whitespace folded).
    std::string selector;
    /// The event as an `on:` attribute names it (`press`, `state(Low).enter`).
    std::string event;
    /// The action list.
    std::string actions;
    /// Inside an `@media` block (cannot be written on an element).
    bool inMedia = false;
};

/// A version 1 sheet split into the three v2 sections (step 5) and its triggers.
struct SplitSheet {
    std::vector<CanonicalItem> layout;
    std::vector<CanonicalItem> style;
    std::vector<CanonicalItem> timelines;
    std::vector<std::string> trailingComments;
    std::vector<TriggerRule> triggers;
    uint32_t rulesSplit = 0;
    uint32_t anchors = 0;
};

/// Splits version 1 CSS text; `anchors` runs step 7 on the layout rules.
SplitSheet splitV1Sheet(std::string_view css, bool anchors);

/// True when the declaration `prelude` (`name: value`) belongs to `<layout>` (the section table,
/// after property alias resolution).
bool isLayoutDeclaration(std::string_view prelude);

/// The CSS name of a declaration prelude, lower-cased.
std::string declarationName(std::string_view prelude);

/// Step 7 on one rule's declarations: rewrites the anchor formulas into the shorthands. True when
/// it did.
bool reverseAnchors(std::vector<CanonicalItem>& decls);

/// Writes items as a `.xts` fragment (two-space indentation, one declaration per line).
std::string writeFragment(const std::vector<CanonicalItem>& items, const std::vector<std::string>& trailing);

/// Splits a selector list at its top-level commas (whitespace folded).
std::vector<std::string> splitSelectors(std::string_view list);

/// Appends `s` as a JSON string (quotes, escapes) to `out` (the report and the computed-style dump).
void appendJsonString(std::string& out, std::string_view s);

/// Splits an inline `style` attribute value into declarations (`name: value`, trimmed).
std::vector<std::string> splitDeclarations(std::string_view style);

// ---- markup (migrate_markup.cpp) -----------------------------------------------------------

/// One text edit of a source: `length` bytes at `pos` replaced by `text`.
struct TextEdit {
    size_t pos = 0;
    size_t length = 0;
    std::string text;
};

/// One attribute of a start tag, as offsets into the source.
struct TagAttr {
    std::string name;  ///< As written.
    size_t begin = 0;  ///< The whitespace before the name.
    size_t end = 0;    ///< One past the value's closing quote (or the name).
};

/// Scans the start tag whose '<' is at `lt`: its attributes, the end of its name and the offset of
/// its `>` / `/>`. False when it is not a well-formed start tag.
bool scanStartTag(std::string_view text, size_t lt, std::vector<TagAttr>& attrs, size_t& nameEnd,
                  size_t& close);

/// The text of [begin, end) with every edit inside it applied (an edit inside an earlier edit's
/// replaced range is dropped).
std::string applyEdits(std::string_view text, size_t begin, size_t end, std::vector<TextEdit> edits);

/// `text` re-indented: every line after the first moves by `delta` columns (spaces only; a line
/// never loses more leading spaces than it has).
std::string reindent(std::string_view text, int delta);

/// The column (0-based) of `pos` on its line, counting leading spaces and tabs only.
int indentAt(std::string_view text, size_t pos);

/// `value` escaped for a double-quoted attribute.
std::string escapeAttr(std::string_view value);

/// The editor's id for an id-less element (Document::autoName(),
/// Code/Editor/Core/src/document_names.cpp), not in `used`.
std::string autoElementId(const DomTree& tree, uint32_t node, const std::set<std::string>& used);

/// True when element `node` matches the simple selector `selector` (tag / `#id` / `.class`
/// compounds with descendant and `>` combinators, no pseudo-classes) statically. `ok` false when
/// the selector is not of that kind.
bool matchSimpleSelector(const DomTree& tree, uint32_t node, std::string_view selector, bool& ok);

}  // namespace xt::compile::migrate
