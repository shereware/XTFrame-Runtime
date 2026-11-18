#include "style_state_deps.h"

#include "selector_match.h"
#include "xt/ir.h"

namespace xt {

namespace {

/// The IrSelCond codes a "rest" compound keeps: plain element facts that need no match context.
bool keptCondition(IrSelCond code) {
    return code == IrSelCond::Tag || code == IrSelCond::Id || code == IrSelCond::Class;
}

}  // namespace

void buildStateDependencies(const IrView& ir, StateDependencyTable& out) {
    out.entries.clear();
    out.parts.clear();
    out.classIds = ir.style().classIds();
    std::span<const IrSelPart> parts = ir.style().selectorParts();
    for (const IrRule& rule : ir.style().rules()) {
        if (uint64_t{rule.selOffset} + rule.selCount > parts.size()) {
            continue;  // IrView::validate rejects this; defensive.
        }
        std::span<const IrSelPart> chain = parts.subspan(rule.selOffset, rule.selCount);
        uint32_t start = 0;
        while (start < chain.size()) {
            uint32_t end = start + 1;
            while (end < chain.size() &&
                   (chain[end].combinator == kIrSelCondition || chain[end].combinator == kIrSelSceneTarget)) {
                ++end;
            }
            uint16_t mask = chain[start].stateMask;
            for (uint32_t i = start + 1; i < end; ++i) {
                const IrSelPart& c = chain[i];
                if (c.combinator == kIrSelCondition &&
                    static_cast<IrSelCond>(c.classCount & ~kIrSelCondNegated) == IrSelCond::State) {
                    mask = static_cast<uint16_t>(mask | c.stateMask);
                }
            }
            if (mask != 0) {
                StateDependency dep;
                dep.stateMask = mask;
                dep.subject = start == 0;
                dep.partOffset = static_cast<uint32_t>(out.parts.size());
                IrSelPart head = chain[start];
                head.combinator = 0;
                head.stateMask = 0;
                out.parts.push_back(head);
                for (uint32_t i = start + 1; i < end; ++i) {
                    const IrSelPart& c = chain[i];
                    if (c.combinator != kIrSelCondition) {
                        continue;  // A scene-target part does not filter the element.
                    }
                    const auto code = static_cast<IrSelCond>(c.classCount & ~kIrSelCondNegated);
                    const bool negated = (c.classCount & kIrSelCondNegated) != 0;
                    if (!negated && keptCondition(code)) {
                        out.parts.push_back(c);
                    }
                    // Everything else (state, `:not()`, attributes, structure, focus-within, `:lang()`,
                    // `:state()`, `:focus-player()`) is dropped: "always true".
                }
                dep.partCount = static_cast<uint32_t>(out.parts.size()) - dep.partOffset;
                out.entries.push_back(dep);
            }
            start = end;
        }
    }
}

StateRestyle stateChangeRestyle(const StateDependencyTable& table, const NodeArena& arena, NodeId node,
                                uint16_t changedBits) {
    StateRestyle result = StateRestyle::None;
    const SelectorMatchContext ctx;
    for (const StateDependency& dep : table.entries) {
        if ((dep.stateMask & changedBits) == 0 || (dep.subject && result == StateRestyle::Self)) {
            continue;
        }
        std::span<const IrSelPart> compound(table.parts.data() + dep.partOffset, dep.partCount);
        uint32_t end = 0;
        if (!matchCompound(arena, node, compound, 0, table.classIds, ctx, end)) {
            continue;
        }
        if (!dep.subject) {
            return StateRestyle::Subtree;
        }
        result = StateRestyle::Self;
    }
    return result;
}

}  // namespace xt
