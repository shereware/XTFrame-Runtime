#include "symbol_expand.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "xt/compile/parse_xts.h"
#include "xt/node.h"  // ElementState bits (kStateHover etc.), for `requires="states: ..."`.
#include "xt/util/text.h"
#include "xts/xts_props.h"

/// See Docs/design/26-layers-symbols-library.md §26.2, 12-compiler.md §12.7, Docs/tasks/T-171.md.
///
/// Not covered by the design/task sheet (conservative choices made here):
///   - The instance root is a `div` (a symbol may have several top-level children, so it needs a
///     wrapper that carries the instance id); the `<use>`'s plain attributes (`class`, `style`,
///     ...) are carried onto it, `sync` included (T-190: the runtime reads it on the root to
///     make the instance's timeline groups follow the parent group's clock, 04 §4.10.2).
///   - A `symbol(Name)` selector whose outermost compound has no `#id` is scoped to the instance
///     as two selectors: that compound on the instance root itself (`#Play:hover #Play/Bg`) and on
///     any element inside it (`#Play :hover #Play/Bg`). One whose outermost compound has an id
///     needs no scoping (the rewritten path id is already unique).
///   - `prop:` override rules get specificity `255 << 16` (above any stylesheet selector, below
///     the `style` attribute's `1 << 24`), so they win over the symbol's own `:hover` rules too.
///   - An unspecified `variant:` adds no class (no implicit default variant).
///   - `requires="states: ..."` maps Hover -> :hover, Pressed/Active -> :active, Focus/Focused ->
///     :focus, Disabled -> :disabled, Checked -> :checked; any other name is a custom state (27.1,
///     T-174). A built-in state counts as present when some `symbol(Name)` rule's selector uses
///     that pseudo-class anywhere; a custom one when some `symbol(Name)` rule uses
///     `:state(<Name>)` or the definition's `state-groups` declares it.
///   - T-174: the definition's `state-groups` attribute is copied onto every instance root (unless
///     the `<use>` carries its own), so the compiler's document-wide state-group table sees it.
///   - `origin` nine-grid words (`top-left` ... `bottom-right`, `center`) map to `transform-origin`
///     keywords; any other value is passed to `transform-origin` verbatim.
///   - Elements copied from a symbol library keep the `<use>` site's line/col (the library file is
///     not in the source map), while elements from a same-file definition keep their own.
///   - At-rules inside a `symbol()` block (`@keyframes` ...) are document-wide, not scoped --
///     except `@timeline` (T-190): it is rewritten per instance like the rules (track paths
///     prefixed with the instance path, scope = the instance root); a symbol with no instance
///     drops its timelines.

namespace xt::compile {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
/// `prop:` override rules: "highest specificity" (12.7) below the inline `style` attribute.
constexpr uint32_t kOverrideSpecificity = 255u << 16;

/// 26.2.1's overridable groups: color, font, image, effect, opacity, text. Anything else
/// (position, size, layout, constraints, ...) is a structural override and is rejected.
constexpr std::array<std::string_view, 24> kOverridableProps = {
    "color", "background-color", "background-image", "background-gradient", "border-color", "opacity",
    "image-opacity", "effect", "font-family", "font-size", "font-weight", "font-style", "letter-spacing",
    "line-height", "text-align", "white-space", "text-overflow", "text-rendering", "object-fit",
    // T-199 (26.3.4): the nine-slice image is a visual override like background-image.
    "border-image", "border-image-source", "border-image-slice", "border-image-repeat", "border-image-fill"};
/// `attr:` overrides allowed by the same rule (image source and text-like attributes).
constexpr std::array<std::string_view, 3> kOverridableAttrs = {"src", "alt", "placeholder"};

template <size_t N>
bool inList(const std::array<std::string_view, N>& list, std::string_view s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

using util::trim;

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t pos = s.find(sep, start);
        std::string part =
            trim(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (!part.empty()) {
            out.push_back(std::move(part));
        }
        if (pos == std::string_view::npos) {
            return out;
        }
        start = pos + 1;
    }
}

bool stateBitFor(std::string_view name, uint16_t& bit) {
    if (name == "Hover") {
        bit = kStateHover;
    } else if (name == "Pressed" || name == "Active") {
        bit = kStateActive;
    } else if (name == "Focus" || name == "Focused") {
        bit = kStateFocus;
    } else if (name == "Disabled") {
        bit = kStateDisabled;
    } else if (name == "Checked") {
        bit = kStateChecked;
    } else {
        return false;
    }
    return true;
}

std::string mapOrigin(const std::string& v) {
    static const std::array<std::pair<std::string_view, std::string_view>, 9> kGrid = {{
        {"top-left", "left top"},
        {"top", "center top"},
        {"top-right", "right top"},
        {"left", "left center"},
        {"center", "center center"},
        {"right", "right center"},
        {"bottom-left", "left bottom"},
        {"bottom", "center bottom"},
        {"bottom-right", "right bottom"},
    }};
    for (const auto& [word, css] : kGrid) {
        if (v == word) {
            return std::string(css);
        }
    }
    return v;
}

/// T-258b (D7): the most nodes the expanded tree holds. Nested `<use>`s multiply (a cycle check
/// does not bound them), so the rebuild stops with an error at this size.
constexpr size_t kMaxExpandedNodes = 1000000;
/// T-258b (D7): the most instance path ids the expansion keeps (ExpandedSymbolInstance::paths, one
/// per emitted node with an id per enclosing instance). Deep nesting reaches this long before
/// kMaxExpandedNodes, so it is the bound on the expander's memory there.
constexpr size_t kMaxExpansionPathEntries = 1000000;

/// One `<symbol>` definition.
struct SymbolDef {
    std::string name;
    const DomTree* tree = nullptr;
    uint32_t node = 0;
    std::string file;
    std::vector<std::pair<std::string, std::vector<std::string>>> variants;
    std::vector<std::string> requiredStates;
    std::vector<std::string> requiredSlots;
    std::vector<std::string> slots;  // Slot names the definition declares.
    std::vector<std::string> uses;   // Symbol names its body instantiates (for cycle detection).
    std::string origin;
    bool hasOrigin = false;
    /// T-174: the definition's `state-groups` attribute (empty when absent).
    std::string stateGroups;
    int stateGroupsLine = 0;
    int stateGroupsCol = 0;
};

/// What an instance puts into one slot: text (`slot:<n>="..."`) or a `<slot name>` child of the
/// `<use>` (its text and child elements).
struct SlotFill {
    bool isText = false;
    std::string text;
    const DomTree* tree = nullptr;
    uint32_t node = 0;
};

/// Where nodes are being copied from.
struct Ctx {
    const DomTree* tree = nullptr;
    std::string file;
    std::string prefix;  // "" at document level, "<instance path>/" inside an instance.
    const std::map<std::string, SlotFill>* fills = nullptr;
    const Ctx* outer = nullptr;  // The context the `<use>` appeared in (slot fills copy from there).
    bool inSymbol = false;
    bool remapLines = false;
    int siteLine = 0;
    int siteCol = 0;
};

class Expander {
public:
    Expander(DomTree& dom, const std::vector<SymbolLibraryFile>& libraries, RuleSet& rules,
             std::string_view fileName, DiagnosticSink& sink, std::vector<ExpandedSymbolInstance>& instances)
        : m_dom(dom),
          m_libraries(libraries),
          m_rules(rules),
          m_file(fileName),
          m_sink(sink),
          m_instances(instances) {}

    bool run() {
        collectDefs(m_dom, m_file);
        for (const SymbolLibraryFile& lib : m_libraries) {
            collectDefs(lib.tree, lib.name);
        }
        if (!m_ok) {
            return false;
        }
        checkCycles();
        checkContracts();
        lintAncestorClassRules();
        if (!m_ok) {
            return false;
        }
        if (needsTreeRebuild()) {
            rebuildTree();
        }
        rewriteRules();
        rewriteTimelines();
        return m_ok;
    }

private:
    DomTree& m_dom;
    const std::vector<SymbolLibraryFile>& m_libraries;
    RuleSet& m_rules;
    std::string m_file;
    DiagnosticSink& m_sink;
    std::vector<ExpandedSymbolInstance>& m_instances;
    bool m_ok = true;

    std::vector<SymbolDef> m_defs;
    std::map<std::string, size_t> m_defIndex;
    DomTree m_out;
    std::map<std::string, std::vector<std::string>> m_instanceIds;  // symbol -> instance paths
    std::map<std::string, std::string> m_instanceSites;             // T-235: instance path -> site
    std::vector<StyleRule> m_originRules;
    std::vector<StyleRule> m_overrideRules;
    bool m_tooLarge = false;   // T-258b: the expansion reached one of its two budgets.
    size_t m_pathEntries = 0;  // T-258b: instance path ids recorded so far (all instances).

    void error(const std::string& file, const std::string& msg, int line = 0, int col = 0) {
        m_ok = false;
        m_sink.error(file, msg, line, col);
    }

    /// T-258b (D7): false once the expanded tree holds kMaxExpandedNodes nodes (or the path budget
    /// below was reached); the first call that finds the tree full reports the error at `n` (the
    /// element that would have grown it).
    bool withinBudget(const Ctx& ctx, const DomNode& n) {
        if (!m_tooLarge && m_out.nodes.size() < kMaxExpandedNodes) {
            return true;
        }
        if (!m_tooLarge) {
            m_tooLarge = true;
            error(ctx.file, "symbol expansion exceeds " + std::to_string(kMaxExpandedNodes) + " nodes",
                  n.line, n.col);
        }
        return false;
    }

    /// T-258b (D7): false (after one error at `n`, the `<use>` being recorded) once
    /// kMaxExpansionPathEntries instance path ids have been recorded; the expansion then stops.
    bool takePathEntry(const Ctx& ctx, const DomNode& n) {
        if (m_pathEntries < kMaxExpansionPathEntries) {
            ++m_pathEntries;
            return true;
        }
        if (!m_tooLarge) {
            m_tooLarge = true;
            error(ctx.file,
                  "symbol expansion exceeds " + std::to_string(kMaxExpansionPathEntries) + " instance paths",
                  n.line, n.col);
        }
        return false;
    }

    // -- Definitions ---------------------------------------------------------------------------

    /// Walks `tree` below `node`, recording slot names and instantiated symbol names. Children of
    /// a `<use>` are that instance's fills: their `<slot>` wrappers are not slots of this
    /// definition, but what is inside them is walked.
    void scanDefBody(const DomTree& tree, uint32_t node, SymbolDef& def) {
        for (uint32_t child : tree.nodes[node].children) {
            const DomNode& n = tree.nodes[child];
            if (n.tag == "symbol") {
                continue;
            }
            if (n.tag == "use") {
                if (const DomAttr* s = n.attr("symbol")) {
                    def.uses.push_back(s->value);
                }
                for (uint32_t fill : n.children) {
                    scanDefBody(tree, fill, def);
                }
                continue;
            }
            if (n.tag == "slot") {
                if (const DomAttr* name = n.attr("name")) {
                    def.slots.push_back(name->value);
                }
            }
            scanDefBody(tree, child, def);
        }
    }

    void collectDefs(const DomTree& tree, const std::string& file) {
        for (uint32_t idx : tree.symbolDefs) {
            if (idx >= tree.nodes.size()) {
                continue;  // T-257: never index past the tree.
            }
            const DomNode& n = tree.nodes[idx];
            const DomAttr* name = n.attr("name");
            if (name == nullptr || name->value.empty()) {
                error(file, "<symbol> requires a name", n.line, n.col);
                continue;
            }
            if (m_defIndex.count(name->value) != 0) {
                error(file, "duplicate symbol \"" + name->value + "\"", n.line, n.col);
                continue;
            }
            SymbolDef def;
            def.name = name->value;
            def.tree = &tree;
            def.node = idx;
            def.file = file;
            if (const DomAttr* origin = n.attr("origin")) {
                def.origin = trim(origin->value);
                def.hasOrigin = !def.origin.empty();
            }
            if (const DomAttr* groups = n.attr("state-groups")) {
                def.stateGroups = groups->value;
                def.stateGroupsLine = groups->line;
                def.stateGroupsCol = groups->col;
            }
            if (const DomAttr* variants = n.attr("variants")) {
                for (const std::string& group : split(variants->value, ',')) {
                    size_t eq = group.find('=');
                    std::string prop = eq == std::string::npos ? std::string() : trim(group.substr(0, eq));
                    std::vector<std::string> values = eq == std::string::npos
                                                          ? std::vector<std::string>()
                                                          : split(group.substr(eq + 1), '|');
                    if (prop.empty() || values.empty()) {
                        error(file, "symbol \"" + def.name + "\": invalid variants \"" + group + "\"", n.line,
                              n.col);
                        continue;
                    }
                    def.variants.emplace_back(prop, std::move(values));
                }
            }
            if (const DomAttr* req = n.attr("requires")) {
                for (const std::string& section : split(req->value, ';')) {
                    size_t colon = section.find(':');
                    std::string key = colon == std::string::npos ? section : trim(section.substr(0, colon));
                    std::vector<std::string> items = colon == std::string::npos
                                                         ? std::vector<std::string>()
                                                         : split(section.substr(colon + 1), ',');
                    if (key == "states") {
                        def.requiredStates.insert(def.requiredStates.end(), items.begin(), items.end());
                    } else if (key == "slots") {
                        def.requiredSlots.insert(def.requiredSlots.end(), items.begin(), items.end());
                    } else {
                        error(file, "symbol \"" + def.name + "\": unknown requires section \"" + key + "\"",
                              n.line, n.col);
                    }
                }
            }
            scanDefBody(tree, idx, def);
            m_defIndex.emplace(def.name, m_defs.size());
            m_defs.push_back(std::move(def));
        }
    }

    const SymbolDef* findDef(const std::string& name) const {
        auto it = m_defIndex.find(name);
        return it == m_defIndex.end() ? nullptr : &m_defs[it->second];
    }

    void checkCycles() {
        // 0 = unvisited, 1 = on the current path, 2 = done.
        std::vector<int> color(m_defs.size(), 0);
        std::vector<std::string> path;
        for (size_t i = 0; i < m_defs.size(); ++i) {
            if (color[i] == 0) {
                visit(i, color, path);
            }
        }
    }

    void visit(size_t i, std::vector<int>& color, std::vector<std::string>& path) {
        color[i] = 1;
        path.push_back(m_defs[i].name);
        for (const std::string& used : m_defs[i].uses) {
            auto it = m_defIndex.find(used);
            if (it == m_defIndex.end()) {
                const DomNode& n = m_defs[i].tree->nodes[m_defs[i].node];
                error(m_defs[i].file,
                      "symbol \"" + m_defs[i].name + "\" uses unknown symbol \"" + used + "\"", n.line,
                      n.col);
                continue;
            }
            size_t j = it->second;
            if (color[j] == 1) {
                std::string chain;
                auto start = std::find(path.begin(), path.end(), m_defs[j].name);
                for (auto p = start; p != path.end(); ++p) {
                    chain += *p + " -> ";
                }
                chain += m_defs[j].name;
                const DomNode& n = m_defs[i].tree->nodes[m_defs[i].node];
                error(m_defs[i].file, "symbol cycle: " + chain, n.line, n.col);
                continue;
            }
            if (color[j] == 0) {
                visit(j, color, path);
            }
        }
        path.pop_back();
        color[i] = 2;
    }

    void checkContracts() {
        for (const SymbolDef& def : m_defs) {
            const DomNode& n = def.tree->nodes[def.node];
            for (const std::string& slot : def.requiredSlots) {
                if (std::find(def.slots.begin(), def.slots.end(), slot) == def.slots.end()) {
                    error(def.file,
                          "symbol \"" + def.name + "\" requires slot \"" + slot +
                              "\" but defines no <slot name=\"" + slot + "\">",
                          n.line, n.col);
                }
            }
            for (const std::string& state : def.requiredStates) {
                uint16_t bit = 0;
                // T-174: a name that is not a built-in state is a custom state (27.1).
                const bool builtIn = stateBitFor(state, bit);
                bool found = false;
                if (!builtIn) {
                    for (const std::string& group : split(def.stateGroups, ';')) {
                        size_t colon = group.find(':');
                        for (const std::string& member :
                             split(colon == std::string::npos ? std::string_view(group)
                                                              : std::string_view(group).substr(colon + 1),
                                   '|')) {
                            found = found || member == state;
                        }
                    }
                }
                for (const StyleRule& rule : m_rules.rules) {
                    if (rule.symbol != def.name) {
                        continue;
                    }
                    // T-392 (27.3.5): each selector of the group (`a, b`) is checked on its own;
                    // one that uses the state is enough. `:focus-within` counts for Focused.
                    for (const Selector& sel : rule.selectors) {
                        for (const SelectorPart& part : sel.parts) {
                            found = found || (builtIn && (part.stateMask & bit) != 0);
                            for (const SelectorCond& cond : part.conds) {
                                found = found ||
                                        (!builtIn && !cond.negated &&
                                         cond.kind == SelectorCondKind::CustomState && cond.name == state) ||
                                        (builtIn && bit == kStateFocus && !cond.negated &&
                                         cond.kind == SelectorCondKind::FocusWithin);
                            }
                        }
                    }
                }
                if (!found) {
                    error(def.file,
                          "symbol \"" + def.name + "\" requires state \"" + state + "\" but has no symbol(" +
                              def.name + ") rule for it",
                          n.line, n.col);
                }
            }
        }
    }

    // -- T-382 (Aux Duty XF-026): symbol rules that start with an ancestor class ------------------

    /// True when `cls` is a class some element of `tree`'s subtree at `node` may carry: in its
    /// `class` attribute, a `bind:class.<cls>` binding, or anywhere in an attribute value (a
    /// trigger's `class(<cls>, on)`). With `uses` not empty only the `<use symbol="<uses>">`
    /// elements of the subtree count.
    static bool classMentioned(const DomTree& tree, uint32_t node, const std::string& cls,
                               std::string_view uses) {
        std::vector<uint32_t> stack{node};
        while (!stack.empty()) {
            const uint32_t idx = stack.back();
            stack.pop_back();
            if (idx >= tree.nodes.size()) {
                continue;
            }
            const DomNode& n = tree.nodes[idx];
            const DomAttr* use = n.attr("symbol");
            const bool match = uses.empty() || (n.tag == "use" && use != nullptr && use->value == uses);
            if (match) {
                for (const DomAttr& a : n.attrs) {
                    if (a.name == "bind:class." + cls) {
                        return true;
                    }
                    if (a.name == "class") {
                        for (const std::string& token : split(a.value, ' ')) {
                            if (token == cls) {
                                return true;
                            }
                        }
                    } else if (a.value.find(cls) != std::string::npos) {
                        return true;
                    }
                }
            }
            for (uint32_t child : n.children) {
                stack.push_back(child);
            }
        }
        return false;
    }

    /// T-383: whether `id` (not empty) is the id of a top-level element of symbol `def`.
    static bool isTopLevelId(const SymbolDef& def, const std::string& id) {
        if (id.empty() || def.node >= def.tree->nodes.size()) {
            return false;
        }
        for (uint32_t child : def.tree->nodes[def.node].children) {
            const DomAttr* a = child < def.tree->nodes.size() ? def.tree->nodes[child].attr("id") : nullptr;
            if (a != nullptr && a->value == id) {
                return true;
            }
        }
        return false;
    }

    /// 26 §26.6: the outermost compound of a `symbol(X)` selector matches the instance root or an
    /// element inside the instance, never an ancestor. A selector whose outermost compound is only
    /// classes (`symbol(X) { .layout-tall #Box { } }`) therefore never matches when neither the
    /// symbol's elements nor any `<use symbol="X">` carry that class and it is no variant class:
    /// one warning per selector. T-383 (Aux Duty XF-042): not when the next compound's id is a
    /// top-level element of the symbol (`.card-s #Card`, #Card the symbol's root: the class
    /// compound then matches the instance root, which the host may give the class at run time).
    void lintAncestorClassRules() {
        for (const StyleRule& rule : m_rules.rules) {
            const SymbolDef* def = rule.symbol.empty() ? nullptr : findDef(rule.symbol);
            if (def == nullptr) {
                continue;
            }
            for (const Selector& sel : rule.selectors) {
                if (sel.parts.size() < 2) {
                    continue;
                }
                const SelectorPart& outer = sel.parts.back();
                if (outer.classes.empty() || !outer.id.empty() || !outer.tag.empty() ||
                    outer.stateMask != 0 || !outer.conds.empty()) {
                    continue;
                }
                if (isTopLevelId(*def, sel.parts[sel.parts.size() - 2].id)) {
                    continue;  // T-383
                }
                for (const std::string& cls : outer.classes) {
                    bool variant = false;
                    for (const auto& [prop, values] : def->variants) {
                        for (const std::string& value : values) {
                            variant = variant || cls == "v-" + prop + "-" + value;
                        }
                    }
                    if (variant || classMentioned(*def->tree, def->node, cls, {})) {
                        continue;
                    }
                    // The body and every definition (a `<head>` one is in no `children`).
                    auto onUse = [&](const DomTree& tree) {
                        bool found = classMentioned(tree, 0, cls, def->name);
                        for (uint32_t d : tree.symbolDefs) {
                            found = found || classMentioned(tree, d, cls, def->name);
                        }
                        return found;
                    };
                    bool onInstance = onUse(m_dom);
                    for (const SymbolLibraryFile& lib : m_libraries) {
                        onInstance = onInstance || onUse(lib.tree);
                    }
                    if (onInstance) {
                        continue;
                    }
                    m_sink.warn(rule.file.empty() ? m_file : rule.file,
                                "symbol(" + def->name + ") rule starting with ." + cls +
                                    " never matches at compile time (runtime addClass is not visible "
                                    "here): symbol-scoped rules cannot see ancestor classes; put the class "
                                    "on a <use symbol=" +
                                    def->name + "> to silence",
                                rule.line);
                    break;
                }
            }
        }
    }

    // -- Tree rebuild --------------------------------------------------------------------------

    bool needsTreeRebuild() const {
        if (!m_dom.symbolDefs.empty()) {
            return true;
        }
        for (const DomNode& n : m_dom.nodes) {
            if (n.tag == "use" || n.tag == "slot") {
                return true;
            }
        }
        return false;
    }

    uint32_t push(DomNode node, uint32_t parent) {
        node.children.clear();
        node.parent = parent;
        uint32_t idx = static_cast<uint32_t>(m_out.nodes.size());
        m_out.nodes.push_back(std::move(node));
        if (parent != kNone) {
            m_out.nodes[parent].children.push_back(idx);
        }
        return idx;
    }

    void appendText(uint32_t parent, const std::string& text) {
        if (text.empty()) {
            return;
        }
        DomNode& p = m_out.nodes[parent];
        if (p.hasText && !p.text.empty()) {
            p.text += " ";
        }
        p.text += text;
        p.hasText = true;
    }

    void rebuildTree() {
        m_out = DomTree{};
        m_out.linkHrefs = m_dom.linkHrefs;
        m_out.inlineStyles = m_dom.inlineStyles;
        m_out.title = m_dom.title;
        m_out.symbolHrefs = m_dom.symbolHrefs;
        Ctx ctx;
        ctx.tree = &m_dom;
        ctx.file = m_file;
        uint32_t root = push(m_dom.nodes[0], kNone);
        emitChildren(ctx, 0, root);
        for (const DomNode& n : m_out.nodes) {
            if (n.hasText && !n.children.empty()) {
                error(m_file, "element <" + n.tag + "> mixes text and child elements after symbol expansion",
                      n.line, n.col);
            }
        }
        m_dom = std::move(m_out);
    }

    void emitChildren(const Ctx& ctx, uint32_t srcNode, uint32_t outParent) {
        for (uint32_t child : ctx.tree->nodes[srcNode].children) {
            emitNode(ctx, child, outParent);
        }
    }

    void emitNode(const Ctx& ctx, uint32_t idx, uint32_t outParent) {
        const DomNode& n = ctx.tree->nodes[idx];
        if (n.tag == "symbol") {
            return;
        }
        if (n.tag == "slot") {
            emitSlot(ctx, n, outParent);
            return;
        }
        if (n.tag == "use") {
            emitUse(ctx, n, outParent);
            return;
        }
        if (!withinBudget(ctx, n)) {
            return;
        }
        DomNode copy = n;
        if (!ctx.prefix.empty()) {
            for (DomAttr& a : copy.attrs) {
                if (a.name == "id") {
                    a.value = ctx.prefix + a.value;
                }
            }
        }
        if (ctx.remapLines) {
            copy.line = ctx.siteLine;
            copy.col = ctx.siteCol;
        }
        uint32_t out = push(std::move(copy), outParent);
        if (ctx.tree == &m_dom && idx == m_dom.body) {
            m_out.body = out;
        }
        emitChildren(ctx, idx, out);
    }

    void emitSlot(const Ctx& ctx, const DomNode& n, uint32_t outParent) {
        if (!ctx.inSymbol) {
            error(ctx.file, "<slot> outside <symbol>", n.line, n.col);
            return;
        }
        const DomAttr* name = n.attr("name");
        if (name == nullptr) {
            error(ctx.file, "<slot> requires a name", n.line, n.col);
            return;
        }
        auto it = ctx.fills->find(name->value);
        if (it == ctx.fills->end()) {
            // Default content: the slot's own text and children, in the definition's context.
            if (n.hasText) {
                appendText(outParent, n.text);
            }
            for (uint32_t child : n.children) {
                emitNode(ctx, child, outParent);
            }
            return;
        }
        const SlotFill& fill = it->second;
        if (fill.isText) {
            appendText(outParent, fill.text);
            return;
        }
        const DomNode& s = fill.tree->nodes[fill.node];
        if (s.hasText) {
            appendText(outParent, s.text);
        }
        for (uint32_t child : s.children) {
            emitNode(*ctx.outer, child, outParent);
        }
    }

    uint32_t findInInstance(uint32_t root, const std::string& fullPath) const {
        for (uint32_t i = root + 1; i < m_out.nodes.size(); ++i) {
            const DomAttr* id = m_out.nodes[i].attr("id");
            if (id != nullptr && id->value == fullPath) {
                return i;
            }
        }
        return kNone;
    }

    /// T-385: the custom state names symbol `def` declares or uses: `requires="states: ..."`,
    /// its `state-groups`, `:state(<Name>)` in its `symbol(Name)` rules, and `bind:state.<Name>` /
    /// `state(<Name>, ...)` actions anywhere in its body. Built-in names are left in; the caller
    /// rejects them first.
    std::vector<std::string> symbolStates(const SymbolDef& def) const {
        std::vector<std::string> out = def.requiredStates;
        for (const std::string& group : split(def.stateGroups, ';')) {
            const size_t colon = group.find(':');
            for (const std::string& member :
                 split(colon == std::string::npos ? std::string_view(group)
                                                  : std::string_view(group).substr(colon + 1),
                       '|')) {
                out.push_back(member);
            }
        }
        for (const StyleRule& rule : m_rules.rules) {
            if (rule.symbol != def.name) {
                continue;
            }
            for (const Selector& sel : rule.selectors) {
                for (const SelectorPart& part : sel.parts) {
                    for (const SelectorCond& cond : part.conds) {
                        if (cond.kind == SelectorCondKind::CustomState) {
                            out.push_back(cond.name);
                        }
                    }
                }
            }
        }
        std::vector<uint32_t> stack{def.node};
        while (!stack.empty()) {
            const uint32_t idx = stack.back();
            stack.pop_back();
            if (idx >= def.tree->nodes.size()) {
                continue;
            }
            const DomNode& node = def.tree->nodes[idx];
            for (const DomAttr& a : node.attrs) {
                if (a.name.rfind("bind:state.", 0) == 0) {
                    out.push_back(a.name.substr(11));
                }
                if (a.name.rfind("on:", 0) != 0) {
                    continue;
                }
                for (size_t pos = a.value.find("state("); pos != std::string::npos;
                     pos = a.value.find("state(", pos + 6)) {
                    const size_t start = pos + 6;
                    const size_t end = a.value.find_first_of(",)", start);
                    if (end != std::string::npos) {
                        out.push_back(trim(std::string_view(a.value).substr(start, end - start)));
                    }
                }
            }
            stack.insert(stack.end(), node.children.begin(), node.children.end());
        }
        return out;
    }

    void emitUse(const Ctx& ctx, const DomNode& n, uint32_t outParent) {
        if (!withinBudget(ctx, n)) {
            return;
        }
        const DomAttr* symAttr = n.attr("symbol");
        const DomAttr* idAttr = n.attr("id");
        if (symAttr == nullptr) {
            error(ctx.file, "<use> requires a symbol attribute", n.line, n.col);
            return;
        }
        if (idAttr == nullptr || idAttr->value.empty()) {
            error(ctx.file, "<use symbol=\"" + symAttr->value + "\"> requires an id", n.line, n.col);
            return;
        }
        const SymbolDef* def = findDef(symAttr->value);
        if (def == nullptr) {
            error(ctx.file, "unknown symbol \"" + symAttr->value + "\"", n.line, n.col);
            return;
        }
        const std::string fullId = ctx.prefix + idAttr->value;
        auto where = [&](const std::string& what) {
            return what + " (symbol \"" + def->name + "\", use \"" + fullId + "\")";
        };

        // Split the <use>'s attributes.
        std::map<std::string, SlotFill> fills;
        std::vector<std::pair<std::string, const DomAttr*>> propOverrides;
        std::vector<std::pair<std::string, const DomAttr*>> attrOverrides;
        std::vector<std::string> variantClasses;
        std::string classList;
        std::vector<DomAttr> rootAttrs;
        std::vector<DomAttr> useAttrs;       // T-385: literal port value candidates.
        const DomAttr* stateAttr = nullptr;  // T-385: `state="A B"`.
        for (const DomAttr& a : n.attrs) {
            if (a.name == "symbol" || a.name == "id") {
                continue;
            }
            if (a.name == "state") {
                stateAttr = &a;
                continue;
            }
            if (a.name != "style" && a.name != "class" && a.name.find(':') == std::string::npos) {
                useAttrs.push_back(a);
            }
            if (a.name == "class") {
                classList = a.value;
                continue;
            }
            if (a.name.rfind("slot:", 0) == 0) {
                SlotFill fill;
                fill.isText = true;
                fill.text = a.value;
                fills[a.name.substr(5)] = std::move(fill);
                continue;
            }
            if (a.name.rfind("prop:", 0) == 0) {
                propOverrides.emplace_back(a.name.substr(5), &a);
                continue;
            }
            if (a.name.rfind("attr:", 0) == 0) {
                attrOverrides.emplace_back(a.name.substr(5), &a);
                continue;
            }
            if (a.name.rfind("variant:", 0) == 0) {
                std::string prop = a.name.substr(8);
                std::string value = trim(a.value);
                bool known = false;
                for (const auto& [vp, values] : def->variants) {
                    if (vp == prop) {
                        known = std::find(values.begin(), values.end(), value) != values.end();
                        break;
                    }
                }
                if (!known) {
                    error(ctx.file, where("unknown variant " + prop + "=\"" + value + "\""), a.line, a.col);
                    continue;
                }
                variantClasses.push_back("v-" + prop + "-" + value);
                continue;
            }
            rootAttrs.push_back(a);
        }
        if (n.hasText) {
            error(ctx.file, where("<use> may only contain <slot> children"), n.line, n.col);
        }
        for (uint32_t child : n.children) {
            const DomNode& c = ctx.tree->nodes[child];
            const DomAttr* slotName = c.attr("name");
            if (c.tag != "slot" || slotName == nullptr) {
                error(ctx.file, where("<use> may only contain <slot name> children"), c.line, c.col);
                continue;
            }
            SlotFill fill;
            fill.tree = ctx.tree;
            fill.node = child;
            fills[slotName->value] = fill;
        }
        for (const auto& [slotName, fill] : fills) {
            if (std::find(def->slots.begin(), def->slots.end(), slotName) == def->slots.end()) {
                error(ctx.file, where("no slot \"" + slotName + "\""), n.line, n.col);
            }
        }

        // The instance root.
        for (const std::string& vc : variantClasses) {
            classList += (classList.empty() ? "" : " ") + vc;
        }
        DomNode root;
        root.tag = "div";
        root.attrs.push_back(DomAttr{"id", fullId, idAttr->line, idAttr->col});
        if (!classList.empty()) {
            root.attrs.push_back(DomAttr{"class", classList, n.line, n.col});
        }
        root.attrs.insert(root.attrs.end(), rootAttrs.begin(), rootAttrs.end());
        if (stateAttr != nullptr) {  // T-385 (27 §27.3.2): the instance's initial custom states.
            const std::vector<std::string> known = symbolStates(*def);
            std::string seeded;
            for (const std::string& name : split(stateAttr->value, ' ')) {
                uint16_t bit = 0;
                if (stateBitFor(name, bit)) {
                    error(ctx.file,
                          where("state=\"" + name + "\" is a built-in state; state= sets custom states only"),
                          stateAttr->line, stateAttr->col);
                    continue;
                }
                if (std::find(known.begin(), known.end(), name) == known.end()) {
                    error(ctx.file,
                          where("unknown state \"" + name +
                                "\" in state= (not declared or used by symbol \"" + def->name + "\")"),
                          stateAttr->line, stateAttr->col);
                    continue;
                }
                seeded += (seeded.empty() ? "" : " ") + name;
            }
            if (!seeded.empty()) {
                root.attrs.push_back(DomAttr{"data-xt-state", seeded, stateAttr->line, stateAttr->col});
            }
        }
        if (!def->stateGroups.empty() && root.attr("state-groups") == nullptr) {
            root.attrs.push_back(
                DomAttr{"state-groups", def->stateGroups, def->stateGroupsLine, def->stateGroupsCol});
        }
        root.line = ctx.remapLines ? ctx.siteLine : n.line;
        root.col = ctx.remapLines ? ctx.siteCol : n.col;
        uint32_t rootIdx = push(std::move(root), outParent);

        size_t instanceSlot = m_instances.size();
        m_instances.push_back(ExpandedSymbolInstance{def->name, rootIdx, {}, std::move(useAttrs)});
        m_instanceIds[def->name].push_back(fullId);
        m_instanceSites[fullId] = ctx.file + ":" + std::to_string(ctx.remapLines ? ctx.siteLine : n.line) +
                                  " <use id=\"" + idAttr->value + "\">";  // T-235

        Ctx inner;
        inner.tree = def->tree;
        inner.file = def->file;
        inner.prefix = fullId + "/";
        inner.fills = &fills;
        inner.outer = &ctx;
        inner.inSymbol = true;
        inner.remapLines = ctx.remapLines || def->tree != &m_dom;
        inner.siteLine = ctx.remapLines ? ctx.siteLine : n.line;
        inner.siteCol = ctx.remapLines ? ctx.siteCol : n.col;
        emitChildren(inner, def->node, rootIdx);

        const std::string pathPrefix = fullId + "/";
        for (uint32_t i = rootIdx + 1; i < m_out.nodes.size(); ++i) {
            const DomAttr* id = m_out.nodes[i].attr("id");
            if (id != nullptr && id->value.rfind(pathPrefix, 0) == 0) {
                if (!takePathEntry(ctx, n)) {
                    break;  // T-258b (D7): the expansion stops.
                }
                m_instances[instanceSlot].paths.emplace_back(id->value, i);
            }
        }

        if (def->hasOrigin) {
            addRule(m_originRules, fullId, "transform-origin", mapOrigin(def->origin), 0, ctx.file, n.line);
        }

        for (const auto& [spec, attr] : attrOverrides) {
            size_t dot = spec.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 == spec.size()) {
                error(ctx.file, where("invalid override attr:" + spec), attr->line, attr->col);
                continue;
            }
            std::string path = spec.substr(0, dot);
            std::string name = spec.substr(dot + 1);
            if (!inList(kOverridableAttrs, name)) {
                error(ctx.file, where("structural override not allowed: attr:" + spec), attr->line,
                      attr->col);
                continue;
            }
            uint32_t target = findInInstance(rootIdx, pathPrefix + path);
            if (target == kNone) {
                error(ctx.file, where("unknown internal path \"" + path + "\" in attr:" + spec), attr->line,
                      attr->col);
                continue;
            }
            bool replaced = false;
            for (DomAttr& a : m_out.nodes[target].attrs) {
                if (a.name == name) {
                    a.value = attr->value;
                    replaced = true;
                }
            }
            if (!replaced) {
                m_out.nodes[target].attrs.push_back(DomAttr{name, attr->value, attr->line, attr->col});
            }
        }

        for (const auto& [spec, attr] : propOverrides) {
            size_t dot = spec.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 == spec.size()) {
                error(ctx.file, where("invalid override prop:" + spec), attr->line, attr->col);
                continue;
            }
            std::string path = spec.substr(0, dot);
            std::string css = spec.substr(dot + 1);
            if (!inList(kOverridableProps, css)) {
                if (findProp(css) == nullptr) {
                    error(ctx.file, where("unknown property in prop:" + spec), attr->line, attr->col);
                } else {
                    error(ctx.file, where("structural override not allowed: prop:" + spec), attr->line,
                          attr->col);
                }
                continue;
            }
            if (findInInstance(rootIdx, pathPrefix + path) == kNone) {
                error(ctx.file, where("unknown internal path \"" + path + "\" in prop:" + spec), attr->line,
                      attr->col);
                continue;
            }
            addRule(m_overrideRules, pathPrefix + path, css, attr->value, kOverrideSpecificity, ctx.file,
                    attr->line);
        }
    }

    /// Appends `#<id> { <css>: <value> }` to `out`; `specificity == 0` means "computed".
    void addRule(std::vector<StyleRule>& out, const std::string& id, const std::string& css,
                 const std::string& value, uint32_t specificity, const std::string& file, int line) {
        StyleRule rule;
        rule.line = line;
        DiagnosticSink local;
        if (!parseDeclarations(css + ": " + value, file, line, local, rule.decls) || local.hasErrors() ||
            local.warningCount() != 0 || rule.decls.empty()) {
            std::string detail = local.all().empty() ? std::string() : ": " + local.all().front().message;
            error(file, "invalid value \"" + value + "\" for " + css + " on \"" + id + "\"" + detail, line);
            return;
        }
        Selector sel;
        SelectorPart part;
        part.id = id;
        sel.parts.push_back(std::move(part));
        sel.specificity = specificity != 0 ? specificity : selectorSpecificity(sel);
        rule.selectors.push_back(std::move(sel));
        out.push_back(std::move(rule));
    }

    // -- Rules ---------------------------------------------------------------------------------

    void rewriteSelector(const Selector& sel, const std::string& instanceId,
                         std::vector<Selector>& out) const {
        Selector base = sel;
        for (SelectorPart& part : base.parts) {
            if (!part.id.empty()) {
                part.id = instanceId + "/" + part.id;
            }
        }
        if (base.parts.empty()) {
            return;
        }
        if (!base.parts.back().id.empty()) {
            base.specificity = selectorSpecificity(base);
            out.push_back(std::move(base));
            return;
        }
        Selector onRoot = base;
        onRoot.parts.back().id = instanceId;
        onRoot.specificity = selectorSpecificity(onRoot);
        out.push_back(std::move(onRoot));

        Selector inside = std::move(base);
        SelectorPart scope;
        scope.combinator = 0;  // descendant
        scope.id = instanceId;
        inside.parts.push_back(std::move(scope));
        inside.specificity = selectorSpecificity(inside);
        out.push_back(std::move(inside));
    }

    /// T-190 (26.2.1, 04 §4.10.1, 12 §12.7): `symbol(Name) { @timeline ... }` -> one copy per
    /// instance, scoped to the instance root, with every track path `<instance path>/<path>`.
    void rewriteTimelines() {
        std::vector<TimelineDef> result;
        for (TimelineDef& def : m_rules.timelines) {
            if (def.symbol.empty()) {
                result.push_back(std::move(def));
                continue;
            }
            if (findDef(def.symbol) == nullptr) {
                m_sink.warn(m_file,
                            "symbol(" + def.symbol + ") @timeline refers to an unknown symbol, ignored",
                            def.line);
                continue;
            }
            auto it = m_instanceIds.find(def.symbol);
            if (it == m_instanceIds.end()) {
                continue;  // No instance: nothing to animate.
            }
            for (const std::string& instanceId : it->second) {
                TimelineDef copy = def;
                copy.symbol.clear();
                copy.scopePath = instanceId;
                for (TimelineTrackDef& track : copy.tracks) {
                    track.path = instanceId + "/" + track.path;
                }
                result.push_back(std::move(copy));
            }
        }
        m_rules.timelines = std::move(result);
    }

    void rewriteRules() {
        std::vector<StyleRule> result;
        result.reserve(m_originRules.size() + m_rules.rules.size() + m_overrideRules.size());
        for (StyleRule& r : m_originRules) {
            result.push_back(std::move(r));
        }
        for (StyleRule& rule : m_rules.rules) {
            if (rule.symbol.empty()) {
                result.push_back(std::move(rule));
                continue;
            }
            if (findDef(rule.symbol) == nullptr) {
                m_sink.warn(m_file, "symbol(" + rule.symbol + ") block refers to an unknown symbol, ignored",
                            rule.line);
                continue;
            }
            auto it = m_instanceIds.find(rule.symbol);
            if (it == m_instanceIds.end()) {
                continue;  // No instance: nothing to style.
            }
            for (const std::string& instanceId : it->second) {
                StyleRule copy;
                copy.decls = rule.decls;
                copy.line = rule.line;
                copy.file = rule.file;  // T-235: diagnostics point at the symbol's own file.
                // T-497 (05 §5.9.12): the copy keeps the library file's directory, so a
                // `background-image` / `border-image-source` inside `symbol(...)` still resolves against it.
                copy.sheetDir = rule.sheetDir;
                copy.instanceSite = m_instanceSites[instanceId];
                for (const Selector& sel : rule.selectors) {
                    rewriteSelector(sel, instanceId, copy.selectors);
                }
                result.push_back(std::move(copy));
            }
        }
        for (StyleRule& r : m_overrideRules) {
            result.push_back(std::move(r));
        }
        m_rules.rules = std::move(result);
    }
};

}  // namespace

bool expandSymbols(DomTree& dom, const std::vector<SymbolLibraryFile>& libraries, RuleSet& rules,
                   std::string_view fileName, DiagnosticSink& sink,
                   std::vector<ExpandedSymbolInstance>& instances) {
    instances.clear();
    Expander expander(dom, libraries, rules, fileName, sink, instances);
    return expander.run();
}

}  // namespace xt::compile
