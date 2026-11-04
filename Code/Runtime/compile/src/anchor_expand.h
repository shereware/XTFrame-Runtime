#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/style_model.h"
#include "xts/xts_lexer.h"

/// T-588 (Docs/design/54-source-format-v2.md §54.5.1): the anchor shorthands `anchor` / `offset` /
/// `offset-end` / `size` of a v2 `<layout>` section, expanded at compile time into the longhand
/// declarations a writer would otherwise type (`position`, `left` / `right` / `top` / `bottom`,
/// `width` / `height`, and for `size: fill` in a stack `flex-grow` / `flex-basis` / `align-self`).
/// The IR never sees the shorthands.
///
/// Two stages:
///   1. expandAnchorShorthands(), called by the `.xts` parser at the end of each declaration block
///      (so inside `@media` and `symbol()` blocks too): validates the block's shorthands, reports
///      every error with its 54 §54.13 code and a fix, and returns the longhands, which take the
///      place of the first shorthand of the block. An axis of `size: fill` becomes
///      `width: 100%` / `height: 100%` marked with StyleDecl::anchorFill.
///   2. resolveAnchorFill(), called by compileDocument() after symbol expansion and `<repeat>`
///      preparation (the element tree is final) and before tokens and style emission: rewrites each
///      marked declaration for what the rule's elements are -- a UI root (a direct child of
///      `<body>`) gets `100vw` / `100vh`, a stack child (its parent's static `display` is `flex`,
///      and it is not absolutely positioned) gets `flex-grow: 1; flex-basis: 0` on the main axis
///      and `align-self: stretch` on the cross axis, every other element keeps `100%`.

namespace xt::compile {

class DiagnosticSink;
struct DomTree;

/// StyleDecl::anchorFill bits: which axis of `size: fill` the declaration is, and whether the rule
/// also had `anchor` (an absolutely positioned element is never a stack child).
inline constexpr uint8_t kAnchorFillX = 1;
inline constexpr uint8_t kAnchorFillY = 2;
inline constexpr uint8_t kAnchorFillAnchored = 4;

/// One shorthand declaration as written in a block.
struct AnchorShorthandDecl {
    std::string name;                       ///< `anchor`, `offset`, `offset-end` or `size`.
    std::vector<std::vector<Token>> parts;  ///< Top-level space-separated components.
    bool important = false;
    int line = 0;  ///< Reported (file) line.
    int col = 0;
    size_t declIndex = 0;  ///< The block's declaration count when it was read.
};

/// One generated longhand: `name: value` with the importance and source line of the shorthand it
/// comes from (`anchor` for position and edges, `size` for the sizes).
struct AnchorLonghand {
    std::string name;
    std::vector<Token> value;
    bool important = false;
    int line = 0;
    uint8_t fill = 0;  ///< StyleDecl::anchorFill for this declaration.
};

/// Splits a declaration value into its top-level components (separated by whitespace outside
/// parentheses): `calc(1safe-left + 4px) 8px` -> two components.
std::vector<std::vector<Token>> splitAnchorComponents(const std::vector<Token>& value);

/// Stage 1 (see the file comment). `decls` are the block's shorthands in source order (a repeated
/// shorthand: the last one wins); `conflict` names a `position` / `left` / `right` / `top` /
/// `bottom` declared in the same block (empty: none), which `anchor` may not be combined with.
/// Returns false, with every problem reported and `out` empty, when the shorthands are invalid.
bool expandAnchorShorthands(const std::vector<AnchorShorthandDecl>& decls, std::string_view conflict,
                            std::string_view fileName, DiagnosticSink& sink,
                            std::vector<AnchorLonghand>& out);

/// Stage 2 (see the file comment): every StyleDecl with a non-zero anchorFill in `ruleSet` is
/// rewritten for the elements of `dom` its rule matches statically (pseudo-class-free compounds) and
/// its mark cleared. A rule whose elements need different forms is an error (the `100%` form stays).
void resolveAnchorFill(RuleSet& ruleSet, const DomTree& dom, std::string_view fileName, DiagnosticSink& sink);

}  // namespace xt::compile
