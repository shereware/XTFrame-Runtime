#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/style_model.h"

/// Compile-time symbol expansion: `<symbol>` / `<use>` / `<slot>` and `symbol(Name) { ... }`
/// rules. See docs/design/26-layers-symbols-library.md §26.2, 12-compiler.md §12.7,
/// docs/tasks/T-171.md.

namespace xt::compile {

/// One parsed symbol library (`<link rel="symbols" href>`), parsed with `parseXtSymbolLibrary`.
struct SymbolLibraryFile {
    /// The href as written (used as the file name in diagnostics).
    std::string name;
    /// The library's tree; only its `symbolDefs` are used.
    DomTree tree;
};

/// One expanded instance, in document order (an outer instance before the ones nested in it).
struct ExpandedSymbolInstance {
    /// `<symbol name>`.
    std::string symbol;
    /// Index into the expanded `DomTree::nodes` of the instance root.
    uint32_t instanceNode = 0;
    /// Every element inside the instance whose id is an internal path of it
    /// (`<instance id>/...`): full path id and node index, in document order.
    std::vector<std::pair<std::string, uint32_t>> paths;
    /// T-385 (27 §27.3.2): the `<use>`'s plain attributes (no `prefix:`, not `symbol`, `id`,
    /// `class`, `style` or `state`), in source order, as carried onto the instance root: the
    /// candidates for literal port values (applyUseLiterals()).
    std::vector<DomAttr> useAttrs;
};

/// Expands every `<use>` of `dom` in place (after parsing, before style compilation):
///   - each `<use symbol id>` becomes a `div` carrying `id` (prefixed by the enclosing instance's
///     path when nested), the `<use>`'s other plain attributes, and `v-<Prop>-<Value>` classes for
///     its `variant:` attributes (T-385: `state="A B"` becomes `data-xt-state`, after checking
///     each name is a custom state the symbol declares or uses); the definition's children are copied under
///     it with every id rewritten to `<instance path>/<id>`; `<slot>`s are replaced by the instance's fill or
///     their own default content;
///   - `symbol(Name)` rules in `rules` are replaced, in place, by one rewritten copy per instance;
///     `prop:` overrides are appended as `#<path>` rules with the highest non-inline specificity;
///     `attr:` overrides are applied to the copied element; `origin` becomes a `transform-origin`
///     rule on the instance root, placed first (lowest priority);
///   - `<symbol>` definitions are removed.
/// Validates names, `requires` contracts, cycles and the override allow-list. `libraries` supply
/// additional definitions. Returns false if any error was reported (then `dom`/`rules` are left
/// in an unspecified state). `instances` receives the SYMB table rows.
bool expandSymbols(DomTree& dom, const std::vector<SymbolLibraryFile>& libraries, RuleSet& rules,
                   std::string_view fileName, DiagnosticSink& sink,
                   std::vector<ExpandedSymbolInstance>& instances);

}  // namespace xt::compile
