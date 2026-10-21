#pragma once

#include <algorithm>
#include <span>
#include <string_view>
#include <vector>

#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// Selector matching against IR rules (05.7 step 1) and against runtime-parsed selectors
/// (Document::query, T-35). A private (core/src) header used by style_engine.cpp,
/// selector_query.cpp and their own tests. See docs/design/05-style-and-css.md §5.3, §5.7,
/// docs/tasks/T-24.md, docs/tasks/T-35.md.

namespace xt {

/// A runtime-parsed selector part (selector_query.cpp). Field-for-field the same shape as
/// IrSelPart (xt/ir_pod.h) -- same combinator encoding (0 = subject, 1 = descendant, 2 = child),
/// same classOffset/classCount slicing into a separate class-id pool -- so both can drive
/// matchChain() below.
struct RuntimeSelPart {
    uint8_t combinator = 0;
    uint16_t stateMask = 0;
    StringId tag = kInvalidString;
    StringId id = kInvalidString;
    uint32_t classOffset = 0;
    uint32_t classCount = 0;
};

inline constexpr uint8_t kCombinatorSubject = 0;
inline constexpr uint8_t kCombinatorDescendant = 1;
inline constexpr uint8_t kCombinatorChild = 2;

/// One selector part (tag/id/classes/state) against a single element node. `Part` is IrSelPart or
/// RuntimeSelPart. See docs/design/05-style-and-css.md §5.3.
template <class Part>
bool matchPart(const NodeArena& arena, NodeId node, const Part& part, std::span<const uint32_t> classIds) {
    if (node == kInvalidNode || arena.kind(node) != NodeKind::Element) {
        return false;
    }
    const ElementData& data = arena.element(node);
    if (part.tag != kInvalidString && part.tag != data.tag) {
        return false;
    }
    // T-177: a `<repeat>` instance element also answers to its template-relative id.
    if (part.id != kInvalidString && part.id != data.id && part.id != data.templateId) {
        return false;
    }
    for (uint32_t i = 0; i < part.classCount; ++i) {
        StringId want = classIds[part.classOffset + i];
        if (!std::binary_search(data.classes.begin(), data.classes.end(), want)) {
            return false;
        }
    }
    if ((static_cast<uint16_t>(part.stateMask) & ~data.stateBits) != 0) {
        return false;
    }
    return true;
}

/// T-173: what a condition part (kIrSelCondition, xt/ir_pod.h) may need beyond the node itself.
/// Every member is optional; a null one falls back to a direct (slower) computation.
struct SelectorMatchContext {
    /// Resolves an attribute value / `lang` attribute StringId (IR or runtime) to its text;
    /// `stringsCtx` is passed back. Null: attribute prefix/suffix/substring and `:lang()` never
    /// match.
    std::string_view (*string)(const void* stringsCtx, StringId id) = nullptr;
    const void* stringsCtx = nullptr;
    /// Per NodeId: 1-based position among the parent's Element children, valid for every Element
    /// StyleEngine::resolve() has reached in this pass (0 = unknown, compute by walking).
    const std::vector<uint32_t>* childIndex = nullptr;
    /// Per NodeId: 1 when the element or a descendant has kStateFocus (`:focus-within`).
    const std::vector<uint8_t>* focusWithin = nullptr;
    /// T-174: the IR's STAT state names (bit i = names[i]) for IrSelCond::CustomState. Empty:
    /// `:state()` never matches.
    std::span<const uint32_t> customStateNames;
    /// T-179: the document language (Context::setLanguage); when non-empty, `:lang()` matches it
    /// instead of the element's (inherited) `lang` attribute.
    std::string_view documentLang;
};

namespace selector_detail {

inline bool isElementNode(const NodeArena& arena, NodeId n) {
    return n != kInvalidNode && arena.kind(n) == NodeKind::Element;
}

inline uint32_t elementIndex(const NodeArena& arena, NodeId node, const SelectorMatchContext& ctx) {
    if (ctx.childIndex != nullptr && node < ctx.childIndex->size() && (*ctx.childIndex)[node] != 0) {
        return (*ctx.childIndex)[node];
    }
    uint32_t index = 1;
    for (NodeId s = arena.prevSibling(node); s != kInvalidNode; s = arena.prevSibling(s)) {
        if (arena.kind(s) == NodeKind::Element) {
            ++index;
        }
    }
    return index;
}

inline bool isLastElementChild(const NodeArena& arena, NodeId node) {
    for (NodeId s = arena.nextSibling(node); s != kInvalidNode; s = arena.nextSibling(s)) {
        if (arena.kind(s) == NodeKind::Element) {
            return false;
        }
    }
    return true;
}

inline bool hasFocusWithin(const NodeArena& arena, NodeId node, const SelectorMatchContext& ctx) {
    if (ctx.focusWithin != nullptr && node < ctx.focusWithin->size()) {
        return (*ctx.focusWithin)[node] != 0;
    }
    // Fallback: search the subtree (only without a StyleEngine-provided cache, e.g. in tests).
    std::vector<NodeId> stack{node};
    while (!stack.empty()) {
        NodeId n = stack.back();
        stack.pop_back();
        if (!isElementNode(arena, n)) {
            continue;
        }
        if ((arena.element(n).stateBits & static_cast<uint16_t>(kStateFocus)) != 0) {
            return true;
        }
        for (NodeId c = arena.firstChild(n); c != kInvalidNode; c = arena.nextSibling(c)) {
            stack.push_back(c);
        }
    }
    return false;
}

inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// BCP 47 prefix match (`:lang(en)` matches `en` and `en-US`), ASCII case-insensitive.
inline bool langMatches(std::string_view lang, std::string_view prefix) {
    if (prefix.empty() || lang.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLower(lang[i]) != asciiLower(prefix[i])) {
            return false;
        }
    }
    return lang.size() == prefix.size() || lang[prefix.size()] == '-';
}

/// The element's effective `lang`: the document language when one is set (T-179), otherwise
/// 05.4's attribute, inherited when absent.
inline std::string_view effectiveLang(const NodeArena& arena, NodeId node, const SelectorMatchContext& ctx) {
    if (!ctx.documentLang.empty()) {
        return ctx.documentLang;
    }
    for (NodeId n = node; isElementNode(arena, n); n = arena.parent(n)) {
        for (const auto& attr : arena.element(n).attributes) {
            if (ctx.string(ctx.stringsCtx, attr.first) == "lang") {
                return ctx.string(ctx.stringsCtx, attr.second);
            }
        }
    }
    return std::string_view{};
}

/// The attribute `name`'s value on `data` (`id` included); false when absent.
inline bool findAttribute(const ElementData& data, StringId name, const SelectorMatchContext& ctx,
                          StringId& value) {
    for (const auto& attr : data.attributes) {
        if (attr.first == name) {
            value = attr.second;
            return true;
        }
    }
    if (data.id != kInvalidString && ctx.string != nullptr && ctx.string(ctx.stringsCtx, name) == "id") {
        value = data.id;
        return true;
    }
    return false;
}

/// One condition part (combinator kIrSelCondition) against `node`, negation included.
template <class Part>
bool matchCondition(const NodeArena& arena, NodeId node, const Part& part, const SelectorMatchContext& ctx) {
    const uint8_t raw = static_cast<uint8_t>(part.classCount);
    const bool negated = (raw & kIrSelCondNegated) != 0;
    const auto code = static_cast<IrSelCond>(raw & ~kIrSelCondNegated);
    const ElementData& data = arena.element(node);
    bool result = false;
    switch (code) {
        case IrSelCond::Tag:
            result = data.tag == part.tag;
            break;
        case IrSelCond::Id:
            result = data.id == part.id;
            break;
        case IrSelCond::Class:
            result =
                std::binary_search(data.classes.begin(), data.classes.end(), static_cast<StringId>(part.tag));
            break;
        case IrSelCond::State:
            result = part.stateMask != 0 && (static_cast<uint16_t>(part.stateMask) & ~data.stateBits) == 0;
            break;
        case IrSelCond::Any:
            result = true;
            break;
        case IrSelCond::AttrExists:
        case IrSelCond::AttrEquals:
        case IrSelCond::AttrPrefix:
        case IrSelCond::AttrSuffix:
        case IrSelCond::AttrSubstring: {
            StringId value = kInvalidString;
            if (!findAttribute(data, part.tag, ctx, value)) {
                break;
            }
            if (code == IrSelCond::AttrExists) {
                result = true;
                break;
            }
            if (code == IrSelCond::AttrEquals && value == part.id) {
                result = true;
                break;
            }
            if (ctx.string == nullptr) {
                break;
            }
            std::string_view have = ctx.string(ctx.stringsCtx, value);
            std::string_view want = ctx.string(ctx.stringsCtx, part.id);
            if (code == IrSelCond::AttrEquals) {
                result = have == want;
            } else if (want.empty()) {
                result = false;  // CSS: an empty ^= $= *= value never matches.
            } else if (code == IrSelCond::AttrPrefix) {
                result = have.substr(0, want.size()) == want;
            } else if (code == IrSelCond::AttrSuffix) {
                result = have.size() >= want.size() && have.substr(have.size() - want.size()) == want;
            } else {
                result = have.find(want) != std::string_view::npos;
            }
            break;
        }
        case IrSelCond::NthChild: {
            const int64_t a = static_cast<int32_t>(part.tag);
            const int64_t b = static_cast<int32_t>(part.id);
            const int64_t index = elementIndex(arena, node, ctx);
            // index = a*n + b for some integer n >= 0.
            if (a == 0) {
                result = index == b;
            } else {
                const int64_t diff = index - b;
                result = diff % a == 0 && diff / a >= 0;
            }
            break;
        }
        case IrSelCond::FirstChild:
            result = elementIndex(arena, node, ctx) == 1;
            break;
        case IrSelCond::LastChild:
            result = isLastElementChild(arena, node);
            break;
        case IrSelCond::FocusWithin:
            result = hasFocusWithin(arena, node, ctx);
            break;
        case IrSelCond::Lang:
            result = ctx.string != nullptr &&
                     langMatches(effectiveLang(arena, node, ctx), ctx.string(ctx.stringsCtx, part.tag));
            break;
        case IrSelCond::CustomState:
            // T-174 (27.1): the state's bit is its index among STAT's names.
            for (size_t bit = 0; bit < ctx.customStateNames.size() && bit < 32; ++bit) {
                if (ctx.customStateNames[bit] == part.tag) {
                    result = (arena.common(node).customStates & (1u << bit)) != 0;
                    break;
                }
            }
            break;
        case IrSelCond::FocusPlayer:
            // T-194 (08.9): `:focus-player(<n>)`; `tag` is the player index.
            result = part.tag < 8 && (data.focusPlayers & (1u << part.tag)) != 0;
            break;
    }
    return result != negated;
}

}  // namespace selector_detail

/// One compound -- `parts[idx]` plus the kIrSelCondition parts right after it -- against `node`.
/// `end` receives the index just past the compound.
template <class Part>
bool matchCompound(const NodeArena& arena, NodeId node, std::span<const Part> parts, uint32_t idx,
                   std::span<const uint32_t> classIds, const SelectorMatchContext& ctx, uint32_t& end) {
    end = idx + 1;
    // T-218: a kIrSelSceneTarget part (right after the subject) is not a condition on the node;
    // it only routes the rule's declarations (StyleEngine), so matching skips it.
    while (end < parts.size() &&
           (parts[end].combinator == kIrSelCondition || parts[end].combinator == kIrSelSceneTarget)) {
        ++end;
    }
    if (!matchPart(arena, node, parts[idx], classIds)) {
        return false;
    }
    for (uint32_t i = idx + 1; i < end; ++i) {
        if (parts[i].combinator == kIrSelSceneTarget) {
            continue;
        }
        if (!selector_detail::matchCondition(arena, node, parts[i], ctx)) {
            return false;
        }
    }
    return true;
}

/// Matches `element` against a whole selector part chain. `parts[0]` (combinator == 0) is the
/// subject (checked against `element` itself); subsequent compounds are ancestors, right-to-left,
/// each combined with the previous match via its own combinator (1 = descendant, any ancestor;
/// 2 = child, the direct parent only). A compound's kIrSelCondition parts (T-173) follow it and
/// are checked against the same node. Descendant combinators backtrack (iteratively, no
/// recursion): if a compound matches some ancestor but a later one then fails, it is retried
/// against a farther-up ancestor before giving up. `classIds` is the class-id pool `classOffset`/
/// `classCount` slice into.
template <class Part>
bool matchChain(const NodeArena& arena, NodeId element, std::span<const Part> parts,
                std::span<const uint32_t> classIds,
                const SelectorMatchContext& ctx = SelectorMatchContext{}) {
    if (parts.empty()) {
        return false;
    }
    uint32_t idx = 0;
    if (!matchCompound(arena, element, parts, 0, classIds, ctx, idx)) {
        return false;
    }

    // `choices` records, for each descendant compound matched so far, the node the search started
    // from (beforeNode) and the ancestor it matched (matchedNode); backtracking resumes the search
    // for that same compound one level above matchedNode.
    struct Choice {
        NodeId beforeNode;
        NodeId matchedNode;
        uint32_t partIdx;
    };
    std::vector<Choice> choices;

    NodeId cur = element;
    NodeId forcedSearchFrom = kInvalidNode;
    bool haveForcedSearchFrom = false;

    while (true) {
        if (idx >= parts.size()) {
            return true;
        }
        const Part& part = parts[idx];
        bool matched = false;
        uint32_t next = idx + 1;
        if (part.combinator == kCombinatorChild) {
            NodeId p = arena.parent(cur);
            if (p != kInvalidNode && matchCompound(arena, p, parts, idx, classIds, ctx, next)) {
                cur = p;
                idx = next;
                matched = true;
            }
            haveForcedSearchFrom = false;
        } else {
            // Anything that isn't kCombinatorChild here is descendant (a condition part never
            // starts a compound: matchCompound consumed it).
            NodeId search = haveForcedSearchFrom ? forcedSearchFrom : arena.parent(cur);
            haveForcedSearchFrom = false;
            NodeId found = kInvalidNode;
            while (search != kInvalidNode) {
                if (matchCompound(arena, search, parts, idx, classIds, ctx, next)) {
                    found = search;
                    break;
                }
                search = arena.parent(search);
            }
            if (found != kInvalidNode) {
                choices.push_back(Choice{cur, found, idx});
                cur = found;
                idx = next;
                matched = true;
            }
        }
        if (matched) {
            continue;
        }
        // Backtrack: retry the most recent descendant choice one level further up.
        if (choices.empty()) {
            return false;
        }
        Choice c = choices.back();
        choices.pop_back();
        cur = c.beforeNode;
        idx = c.partIdx;
        forcedSearchFrom = arena.parent(c.matchedNode);
        haveForcedSearchFrom = true;
    }
}

/// Matches `element` against `rule`'s selector chain. `selectorParts`/`classIds` are the whole
/// STYL selectorParts/classIds pools (StyleChunk::selectorParts()/classIds()); `rule.selOffset`/
/// `selCount` slice into them. Thin wrapper over matchChain(); a rule with `:state()` parts (the
/// IR's STAT selector states, T-174) is matched through RuntimeSelPart copies carrying the masks.
bool matchRule(const NodeArena& arena, const IrView& ir, NodeId element, const IrRule& rule,
               std::span<const IrSelPart> selectorParts, std::span<const uint32_t> classIds,
               const SelectorMatchContext& ctx = SelectorMatchContext{});

}  // namespace xt
