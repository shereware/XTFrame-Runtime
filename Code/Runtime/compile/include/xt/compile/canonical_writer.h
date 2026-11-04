#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/diagnostics.h"

/// T-587 (Docs/design/54-source-format-v2.md §54.9): the canonical writer of source format v2,
/// shared by the editor, the importer and the migrator (T-589 / T-591 / T-592). The writer takes
/// the document as sections (CanonicalXt) and writes them in the canonical order `<head>` ->
/// `<symbol>`s -> `<ports>` -> `<body>` -> `<layout>` -> `<style>` -> `<timelines>`, two-space
/// indentation, one selector per line and one declaration per line.
///
/// Order inside a section: `<layout>` keeps the caller's order (the caller sorts its rules by layer
/// order and writes `#id` subjects, 54 §54.9 writer convention); `<style>` keeps source order (the
/// cascade depends on it); `<timelines>` is sorted by timeline name (`symbol(X) { ... }` blocks
/// after the top-level timelines, by prelude, their own timelines sorted the same way); `<ports>`
/// keeps declaration order (= PORT order = contract hash order).
///
/// Element markup (`<body>` and a symbol's elements) is the caller's text, written as it is (the
/// first line indented to its place). Comments: a comment between rules or declarations (or
/// between `<ports>` declarations, on its own line) is kept and written above the item that follows
/// it, and a section's trailing comments at its end; a comment inside a declaration, a selector or
/// at the end of a block is not kept.

namespace xt::compile {

/// One item of a section: a block (`<prelude> { <children> }` -- a rule, `@media`, `@timeline`,
/// a timeline track or key, `symbol(X)`) or a statement (`<prelude>;` -- a declaration, or an
/// `@timeline <name> from "<file>#<name>"`).
struct CanonicalItem {
    /// The comments directly above the item, verbatim (`/* ... */`).
    std::vector<std::string> comments;
    /// Selector list, at-rule prelude or key time (block), or the declaration / statement text
    /// without its ';' (statement). Whitespace runs outside strings are one space.
    std::string prelude;
    /// A block (`{ ... }`) rather than a statement.
    bool block = false;
    /// A block's items, in order.
    std::vector<CanonicalItem> children;

    bool operator==(const CanonicalItem&) const = default;
};

/// One `<layout>`, `<style>` or `<timelines>` section.
struct CanonicalSection {
    /// `src` fragment path (the section then has no items).
    std::string src;
    /// The section's items, in the order they are written.
    std::vector<CanonicalItem> items;
    /// Comments after the last item, verbatim.
    std::vector<std::string> trailingComments;

    bool operator==(const CanonicalSection&) const = default;
};

/// One `<ports>` declaration: `<name>: <value>[ hidden]`.
struct CanonicalPort {
    std::string name;
    /// `<type> [in|out|call] [= <default>]`, as a v1 `port:<name>` attribute value.
    std::string value;
    /// `hidden` (the port is not exposed).
    bool hidden = false;
    /// The comment lines directly above the declaration, verbatim (`//` or `/* */`).
    std::vector<std::string> comments;

    bool operator==(const CanonicalPort&) const = default;
};

/// One `<symbol>` (a direct child of `<xt>`).
struct CanonicalSymbol {
    /// The `<symbol>` attributes (`name` first by convention), unescaped, in order.
    std::vector<std::pair<std::string, std::string>> attrs;
    /// The symbol's `<ports>` (none written when empty).
    std::vector<CanonicalPort> ports;
    /// The symbol's elements, each one element's markup.
    std::vector<std::string> elements;
    std::vector<CanonicalSection> layout;
    std::vector<CanonicalSection> style;
    std::vector<CanonicalSection> timelines;

    bool operator==(const CanonicalSymbol&) const = default;
};

/// A whole v2 `.xt` file (a UI, or a symbol library when `body` is empty).
struct CanonicalXt {
    /// `<title>` text (unescaped); none written when empty.
    std::string title;
    /// `<symbols src>` values, in order.
    std::vector<std::string> symbolsSrc;
    std::vector<CanonicalSymbol> symbols;
    /// The document's `<ports>` (none written when empty).
    std::vector<CanonicalPort> ports;
    /// The `<body ...>...</body>` markup; empty for a symbol library.
    std::string body;
    std::vector<CanonicalSection> layout;
    std::vector<CanonicalSection> style;
    std::vector<CanonicalSection> timelines;

    bool operator==(const CanonicalXt&) const = default;
};

/// Splits a section's CSS text into items (rules, at-rules, declarations, with their comments).
/// Comments after the last top-level item go to `trailingComments` when it is given.
std::vector<CanonicalItem> splitSheetItems(std::string_view css,
                                           std::vector<std::string>* trailingComments = nullptr);

/// Reads a version 2 `.xt` text into the writer's structure (sections split into items, element
/// markup copied from the source). False, with the errors in `sink`, when the text does not parse
/// or is not version 2.
bool readCanonicalXt(std::string_view text, std::string_view fileName, DiagnosticSink& sink,
                     CanonicalXt& out);

/// The canonical v2 text of `doc` (LF line endings, a newline at the end).
std::string writeCanonicalXt(const CanonicalXt& doc);

}  // namespace xt::compile
