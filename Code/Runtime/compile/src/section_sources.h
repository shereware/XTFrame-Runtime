#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/style_model.h"

/// T-587 (Docs/design/54-source-format-v2.md §54.3, §54.5, §54.6, §54.8): the compile step of a
/// v2 file's sections -- `<layout>`, `<style>` and `<timelines>` (inline or `src` fragments)
/// parsed in file order with their section's whitelist, the layout lints, and `on:<event>@<path>`
/// lowered into the rule triggers v1 wrote in `.xts`.

namespace xt::compile {

/// Reads a section fragment: `href` (already joined with the owning file's directory) -> its text
/// and its directory (empty when in memory). False when it cannot be read; `message` is the error.
using FragmentReader =
    std::function<bool(const std::string& href, std::string& text, std::string& dir, std::string& message)>;

/// Where a v2 file's sections come from.
struct SectionOwner {
    /// The file's name as diagnostics and StyleRule::file show it.
    std::string fileName;
    /// The directory a relative `src` is joined with (relative to the document directory, or
    /// absolute); empty for the document itself (its `src` resolve like its `<link href>`).
    std::string baseHref;
    /// StyleRule::sheetDir of the inline sections (empty for the document, as for its `<style>`).
    std::string inlineSheetDir;
};

/// `href` joined with `base` (lexically normal, generic separators) unless `base` is empty or
/// `href` absolute.
std::string joinSectionHref(std::string_view base, std::string_view href);

/// Parses `tree`'s layout / style / timelines sections into `ruleSet` in file order (the merge
/// order, 54 §54.3): `<layout>` in XtsSectionMode::LayoutOnly, `<style>` StyleOnly, `<timelines>`
/// TimelinesOnly, a `src` fragment (always `.xts`) in its section's mode; a symbol's section as the
/// body of `symbol(<name>) {}`. Inline sections report file lines and columns. Then the layout
/// lints: `layout.duplicateSubject` (warning: the same `#id` subject in more than one layout
/// section of the file) and, with `lintSubjectId`, `layout.subjectNotId` (a layout rule whose
/// subject has no `#id`).
void parseSheetSections(const DomTree& tree, const SectionOwner& owner, const FragmentReader& read,
                        bool lintSubjectId, DiagnosticSink& sink, RuleSet& ruleSet);

/// Lowers every `on:<event>[(<arg>)]@<path>="<actions>"` attribute of `tree` (54 §54.6) into a
/// rule `#<use id>/<path> { on-<event>: <actions> }` appended to `ruleSet` (scoped to the
/// enclosing `<symbol>`, if any) and removes the attribute, so trigger_emit.cpp compiles it
/// exactly as the v1 `.xts` rule trigger it replaces. Only valid on a `<use>` with an `id`.
void lowerPathTriggers(DomTree& tree, std::string_view fileName, DiagnosticSink& sink, RuleSet& ruleSet);

}  // namespace xt::compile
