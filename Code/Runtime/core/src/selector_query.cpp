#include <algorithm>
#include <cctype>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "selector_match.h"
#include "xt/document.h"

/// Document::query: a runtime parser for the 05.3 selector subset (no groups) plus a preorder
/// walk matching every Element against it through selector_match.h's shared matcher.
/// See Docs/design/05-style-and-css.md §5.3, Docs/design/13-api.md §13.2, Docs/tasks/T-35.md.

namespace xt {

namespace {

/// One compound selector (`div.a#x:hover`) as parsed, left to right.
struct Compound {
    StringId tag = kInvalidString;
    StringId id = kInvalidString;
    uint16_t stateMask = 0;
    uint32_t classOffset = 0;
    uint32_t classCount = 0;
    uint8_t combinator = kCombinatorSubject;  // Relation to the compound on its LEFT.
};

bool isNameStart(char c) { return c == '-' || c == '_' || std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isNameCont(char c) { return c == '-' || c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/// 05.3's five pseudo-classes. Returns 0 for anything else (including the unsupported
/// `:not()`/`:nth-*`/`::before`, which the caller then rejects).
uint16_t stateBitFor(std::string_view name) {
    if (name == "hover") {
        return static_cast<uint16_t>(kStateHover);
    }
    if (name == "active") {
        return static_cast<uint16_t>(kStateActive);
    }
    if (name == "focus") {
        return static_cast<uint16_t>(kStateFocus);
    }
    if (name == "disabled") {
        return static_cast<uint16_t>(kStateDisabled);
    }
    if (name == "checked") {
        return static_cast<uint16_t>(kStateChecked);
    }
    return 0;
}

}  // namespace

Status Document::query(std::string_view selector, std::vector<ElementRef>& out) {
    out.clear();

    std::vector<Compound> compounds;
    std::vector<uint32_t> classIds;
    size_t i = 0;
    const size_t n = selector.size();
    uint8_t pendingCombinator = kCombinatorSubject;  // For the first compound.
    // T-270a (C6): names are looked up, never interned; one that no string carries (no element can
    // have it) makes the whole selector match nothing, once its syntax has been checked.
    // T-270b: sound only because this subset has no negation (`:not(...)` is rejected below as a
    // functional pseudo-class). Adding a negation must change this: an unknown name inside one
    // makes that negation always true (`:not(.nope)` matches every element), not the selector
    // unmatchable.
    bool unmatchable = false;

    while (i < n) {
        while (i < n && isSpace(selector[i])) {
            if (!compounds.empty() && pendingCombinator == kCombinatorSubject) {
                pendingCombinator = kCombinatorDescendant;
            }
            ++i;
        }
        if (i >= n) {
            break;
        }
        if (selector[i] == '>') {
            if (compounds.empty()) {
                return Status::kInvalidArgument;  // Leading combinator.
            }
            pendingCombinator = kCombinatorChild;
            ++i;
            continue;
        }
        if (!compounds.empty() && pendingCombinator == kCombinatorSubject) {
            return Status::kInvalidArgument;  // Two compounds with nothing between them.
        }

        Compound compound;
        compound.combinator = pendingCombinator;
        compound.classOffset = static_cast<uint32_t>(classIds.size());
        pendingCombinator = kCombinatorSubject;

        bool sawAnything = false;
        while (i < n && !isSpace(selector[i]) && selector[i] != '>') {
            char c = selector[i];
            if (c == '*') {
                if (sawAnything) {
                    return Status::kInvalidArgument;
                }
                ++i;
                sawAnything = true;
                continue;  // Matches everything: no tag/id/class/state constraint.
            }
            if (c == '#' || c == '.' || c == ':') {
                ++i;
                if (i < n && c == ':' && selector[i] == ':') {
                    return Status::kInvalidArgument;  // `::before`/`::after` (05.3: unsupported).
                }
                size_t start = i;
                while (i < n && isNameCont(selector[i])) {
                    ++i;
                }
                if (i == start) {
                    return Status::kInvalidArgument;
                }
                std::string_view name = selector.substr(start, i - start);
                if (c == '#') {
                    if (compound.id != kInvalidString) {
                        return Status::kInvalidArgument;
                    }
                    compound.id = findString(name);
                    unmatchable = unmatchable || compound.id == kInvalidString;
                } else if (c == '.') {
                    classIds.push_back(findString(name));
                    unmatchable = unmatchable || classIds.back() == kInvalidString;
                    compound.classCount += 1;
                } else {
                    uint16_t bit = stateBitFor(name);
                    if (bit == 0 || (i < n && selector[i] == '(')) {
                        return Status::kInvalidArgument;  // Unknown or functional pseudo-class.
                    }
                    compound.stateMask = static_cast<uint16_t>(compound.stateMask | bit);
                }
                sawAnything = true;
                continue;
            }
            if (isNameStart(c)) {
                if (sawAnything) {
                    return Status::kInvalidArgument;  // A tag name must come first.
                }
                size_t start = i;
                while (i < n && isNameCont(selector[i])) {
                    ++i;
                }
                compound.tag = findString(selector.substr(start, i - start));
                unmatchable = unmatchable || compound.tag == kInvalidString;
                sawAnything = true;
                continue;
            }
            // `,` (groups), `[`, `+`, `~` and anything else: not in this task's subset.
            return Status::kInvalidArgument;
        }
        if (!sawAnything) {
            return Status::kInvalidArgument;
        }
        compounds.push_back(compound);
    }
    if (compounds.empty() || pendingCombinator == kCombinatorChild) {
        return Status::kInvalidArgument;
    }
    if (unmatchable) {
        return Status::kOk;
    }

    // Right-to-left part chain: part 0 is the subject, and each ancestor part carries the
    // combinator that linked it to the part on its right (selector_match.h's convention).
    std::vector<RuntimeSelPart> parts;
    parts.reserve(compounds.size());
    for (size_t k = compounds.size(); k > 0; --k) {
        const Compound& compound = compounds[k - 1];
        RuntimeSelPart part;
        part.combinator = k == compounds.size() ? kCombinatorSubject : compounds[k].combinator;
        part.stateMask = compound.stateMask;
        part.tag = compound.tag;
        part.id = compound.id;
        part.classOffset = compound.classOffset;
        part.classCount = compound.classCount;
        parts.push_back(part);
    }

    if (m_root == kInvalidNode || !m_arena.alive(m_root)) {
        return Status::kOk;
    }
    std::span<const RuntimeSelPart> partSpan(parts);
    std::span<const uint32_t> classSpan(classIds);
    // Own stack rather than NodeArena::traversePreorder: matching never marks anything dirty, but
    // keeping the walk local also lets it prune non-Element subtrees (Text nodes and instantiated
    // `anim` scenes never contain Elements).
    std::vector<NodeId> stack;
    stack.push_back(m_root);
    while (!stack.empty()) {
        NodeId node = stack.back();
        stack.pop_back();
        if (m_arena.kind(node) != NodeKind::Element) {
            continue;
        }
        if (matchChain(m_arena, node, partSpan, classSpan)) {
            out.push_back(ElementRef{this, node, m_arena.generation(node)});
        }
        // Push children reversed so the pop order stays document (preorder) order.
        size_t first = stack.size();
        for (NodeId c = m_arena.firstChild(node); c != kInvalidNode; c = m_arena.nextSibling(c)) {
            stack.push_back(c);
        }
        std::reverse(stack.begin() + static_cast<ptrdiff_t>(first), stack.end());
    }
    return Status::kOk;
}

}  // namespace xt
