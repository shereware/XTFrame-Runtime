#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// DOM tree produced by parsing a `.xt` document. See Docs/design/05-style-and-css.md §5.1-5.2,
/// Docs/tasks/T-21.md.

namespace xt::compile {

/// One attribute of a `DomNode`, in source order.
struct DomAttr {
    /// Attribute name, as written in the source (case preserved).
    std::string name;
    /// Attribute value, unescaped.
    std::string value;
    /// 1-based source line of the attribute.
    int line = 0;
    /// 1-based source column of the attribute.
    int col = 0;
};

/// Kinds of DomTextRun::inlineKind (T-178).
enum class DomInlineKind : uint8_t {
    None = 0,   ///< Ordinary text.
    Glyph = 1,  ///< `<glyph key>`; the run's text is one U+FFFC.
    Image = 2,  ///< `<img inline src>`; the run's text is one U+FFFC.
};

/// T-178 (Docs/design/07-text.md §7.8): one rich-text run of a DomNode's `text`, produced when
/// an element's content mixes text with inline markup (`<span>`, `<b>`, `<i>`, `<a>`, `<glyph>`,
/// `<img inline>`). Runs are ascending and non-overlapping; bytes outside every run carry the
/// element's own style.
struct DomTextRun {
    /// UTF-8 byte range `[start, end)` into DomNode::text.
    uint32_t start = 0;
    uint32_t end = 0;  ///< One past the run's last byte.
    /// The accumulated `style` declarations of the enclosing inline elements, outermost first
    /// (`<b>`/`<i>` contribute `font-weight: bold;`/`font-style: italic;`).
    std::string style;
    /// The accumulated `class` names of the enclosing inline elements.
    std::vector<std::string> classes;
    /// Inline object kind.
    DomInlineKind inlineKind = DomInlineKind::None;
    /// Glyph: the `key` attribute; image: the `src` attribute.
    std::string inlineRef;
    /// The inline object's `height` attribute text; empty = the run's font size.
    std::string inlineHeight;
    /// Index into DomNode::textLinks of the enclosing `<a>`, or -1.
    int32_t link = -1;
    /// Source position of the innermost inline element.
    int line = 0;
    int col = 0;  ///< 1-based source column.
};

/// One element (or text-bearing element) of a parsed `.xt` document tree.
struct DomNode {
    /// Tag name, lower-cased.
    std::string tag;
    /// Attributes in source order; duplicate names warn, and the last one wins.
    std::vector<DomAttr> attrs;
    /// Whitespace-folded text content, valid only when `hasText` is true.
    std::string text;
    /// True if `text` holds this node's folded text content.
    bool hasText = false;
    /// Indices into `DomTree::nodes` of this node's children, in document order.
    std::vector<uint32_t> children;
    /// Index into `DomTree::nodes`, or `0xFFFFFFFFu` for the root node.
    uint32_t parent = 0xFFFFFFFFu;
    /// Start-tag position (1-based line).
    int line = 0;
    /// Start-tag position (1-based column).
    int col = 0;
    /// T-178: rich-text runs of `text` (empty for plain text).
    std::vector<DomTextRun> textRuns;
    /// T-178: the raw `on:click` value of every `<a>` in `text`, indexed by DomTextRun::link.
    std::vector<std::string> textLinks;
    /// T-178 (parser scratch): the element's entity-decoded, unfolded direct text, and the byte
    /// position in it at which each of `children` appeared.
    std::string rawText;
    std::vector<uint32_t> rawChildPos;  ///< Parallel to `children`.
    /// T-190 (11 §11.9): an inline `<svg>`'s whole source (`<svg ...>...</svg>`, attributes other
    /// than id/class/style/sync), compiled by parseSvg() into the element's scene. Empty for every
    /// other tag.
    std::string svgSource;
    /// 1-based line of the `<svg>` start tag in its source file (for SVG diagnostics).
    int svgLine = 0;

    /// Returns the first attribute named `name`, or nullptr if none exists.
    const DomAttr* attr(std::string_view name) const {
        for (const DomAttr& a : attrs) {
            if (a.name == name) {
                return &a;
            }
        }
        return nullptr;
    }
};

/// The full tree produced by `parseXt()`, plus the document-level references gathered while
/// walking it.
struct DomTree {
    /// All nodes in the tree; `nodes[0]` is the `<xt>` root.
    std::vector<DomNode> nodes;
    /// Index into `nodes` of the `<body>` element, or `0xFFFFFFFFu` if absent.
    uint32_t body = 0xFFFFFFFFu;
    /// `<link href>` values, in appearance order.
    std::vector<std::string> linkHrefs;
    /// `<style>` block contents paired with their 1-based start line, in appearance order.
    std::vector<std::pair<std::string, int>> inlineStyles;
    /// `<title>` element's text content, or empty if absent.
    std::string title;
    /// T-171: indices into `nodes` of every `<symbol>` definition, in appearance order. One in
    /// `<head>` has no parent (`parent == 0xFFFFFFFFu`, not listed in any `children`); one
    /// elsewhere is an ordinary child of its parent. symbol_expand.cpp removes all of them.
    std::vector<uint32_t> symbolDefs;
    /// T-171: `<link rel="symbols" href>` values (symbol libraries), in appearance order. These
    /// are not stylesheets and are not in `linkHrefs`.
    std::vector<std::string> symbolHrefs;
};

}  // namespace xt::compile
