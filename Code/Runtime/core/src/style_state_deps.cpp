#include "style_state_deps.h"

#include <algorithm>

#include "selector_match.h"
#include "style_decode.h"
#include "xt/ir.h"
#include "xt/property.h"

namespace xt {

namespace {

/// The IrSelCond codes a "rest" compound keeps: plain element facts that need no match context.
bool keptCondition(IrSelCond code) {
    return code == IrSelCond::Tag || code == IrSelCond::Id || code == IrSelCond::Class;
}

/// T-763: the custom state bit of a `:state()` condition's name, or 0 when STAT has no such name.
uint32_t customStateBit(std::span<const uint32_t> names, uint32_t name) {
    for (size_t bit = 0; bit < names.size() && bit < kMaxCustomStates; ++bit) {
        if (names[bit] == name) {
            return 1u << bit;
        }
    }
    return 0;
}

}  // namespace

void buildStateDependencies(const IrView& ir, StateDependencyTable& out) {
    out.entries.clear();
    out.parts.clear();
    out.classes.clear();
    out.classIds = ir.style().classIds();
    const std::span<const uint32_t> customNames = ir.customStates().names();
    std::span<const IrSelPart> parts = ir.style().selectorParts();
    auto noteClass = [&out](uint32_t cls, bool subject) {
        ClassDependency dep;
        dep.cls = cls;
        dep.subject = subject;
        dep.ancestor = !subject;
        out.classes.push_back(dep);
    };
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
            const bool subject = start == 0;
            // T-763 (item 3): every class the compound names, negated or not.
            const IrSelPart& head = chain[start];
            for (uint32_t c = 0; c < head.classCount; ++c) {
                if (uint64_t{head.classOffset} + c < out.classIds.size()) {
                    noteClass(out.classIds[head.classOffset + c], subject);
                }
            }
            uint16_t mask = head.stateMask;
            uint32_t custom = 0;
            uint8_t players = 0;
            for (uint32_t i = start + 1; i < end; ++i) {
                const IrSelPart& c = chain[i];
                if (c.combinator != kIrSelCondition) {
                    continue;
                }
                switch (static_cast<IrSelCond>(c.classCount & ~kIrSelCondNegated)) {
                    case IrSelCond::State:
                        mask = static_cast<uint16_t>(mask | c.stateMask);
                        break;
                    case IrSelCond::CustomState:
                        custom |= customStateBit(customNames, c.tag);
                        break;
                    case IrSelCond::FocusPlayer:
                        if (c.tag < 8) {
                            players = static_cast<uint8_t>(players | (1u << c.tag));
                        }
                        break;
                    case IrSelCond::Class:
                        noteClass(c.tag, subject);
                        break;
                    default:
                        break;
                }
            }
            if (mask != 0 || custom != 0 || players != 0) {
                StateDependency dep;
                dep.stateMask = mask;
                dep.customMask = custom;
                dep.focusPlayerMask = players;
                dep.subject = subject;
                dep.partOffset = static_cast<uint32_t>(out.parts.size());
                IrSelPart rest = head;
                rest.combinator = 0;
                rest.stateMask = 0;
                out.parts.push_back(rest);
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
    // One entry per class, positions merged.
    std::sort(out.classes.begin(), out.classes.end(),
              [](const ClassDependency& a, const ClassDependency& b) { return a.cls < b.cls; });
    size_t kept = 0;
    for (size_t i = 0; i < out.classes.size(); ++i) {
        if (kept > 0 && out.classes[kept - 1].cls == out.classes[i].cls) {
            out.classes[kept - 1].subject = out.classes[kept - 1].subject || out.classes[i].subject;
            out.classes[kept - 1].ancestor = out.classes[kept - 1].ancestor || out.classes[i].ancestor;
            continue;
        }
        out.classes[kept++] = out.classes[i];
    }
    out.classes.resize(kept);
}

StateRestyle stateChangeRestyle(const StateDependencyTable& table, const NodeArena& arena, NodeId node,
                                const StateChange& change) {
    StateRestyle result = StateRestyle::None;
    const SelectorMatchContext ctx;
    for (const StateDependency& dep : table.entries) {
        if (((dep.stateMask & change.bits) == 0 && (dep.customMask & change.custom) == 0 &&
             (dep.focusPlayerMask & change.focusPlayers) == 0) ||
            (dep.subject && result == StateRestyle::Self)) {
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

StateRestyle classChangeRestyle(const StateDependencyTable& table, std::span<const StringId> before,
                                std::span<const StringId> after) {
    StateRestyle result = StateRestyle::None;
    auto note = [&](StringId cls) {
        auto it = std::lower_bound(table.classes.begin(), table.classes.end(), cls,
                                   [](const ClassDependency& d, StringId c) { return d.cls < c; });
        if (it == table.classes.end() || it->cls != cls) {
            return;  // No rule names it.
        }
        if (it->ancestor) {
            result = StateRestyle::Subtree;
        } else if (it->subject && result == StateRestyle::None) {
            result = StateRestyle::Self;
        }
    };
    // The symmetric difference of the two sorted lists.
    size_t i = 0;
    size_t j = 0;
    while ((i < before.size() || j < after.size()) && result != StateRestyle::Subtree) {
        if (j == after.size() || (i < before.size() && before[i] < after[j])) {
            note(before[i++]);
        } else if (i == before.size() || after[j] < before[i]) {
            note(after[j++]);
        } else {
            ++i;
            ++j;
        }
    }
    return result;
}

void markStateChange(NodeArena& arena, NodeId node, StateRestyle scope, const StateChange& change,
                     DirtyCause cause) {
    markRestyle(arena, node, scope, cause);
    if ((change.bits & static_cast<uint16_t>(kStateFocus)) != 0) {
        // The painter reads the bit itself (a focused editable input draws its selection and
        // caret, painter.cpp), whatever the style rules test.
        arena.markDirty(node, static_cast<uint8_t>(kDirtyPaint), cause);
    }
}

void markRestyle(NodeArena& arena, NodeId node, StateRestyle scope, DirtyCause cause) {
    switch (scope) {
        case StateRestyle::None:
            break;
        case StateRestyle::Self:
            arena.markStyleDirtySelf(node, cause);
            break;
        case StateRestyle::Subtree:
            arena.markStyleDirtySelf(node, cause);
            arena.traversePreorder(node, [&arena, cause](NodeId n) {
                arena.markStyleDirtySelf(n, cause);
                return true;
            });
            break;
    }
}

void markStyleDirtyFullRange(NodeArena& arena, NodeId node, DirtyCause cause) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle), cause);
    arena.traversePreorder(node, [&arena, cause](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle), cause);
        return true;
    });
}

void markRuntimeDeclRestyle(NodeArena& arena, NodeId node, uint16_t propId, bool legacy, DirtyCause cause) {
    if (legacy) {
        markStyleDirtyFullRange(arena, node, cause);
        return;
    }
    const bool reachesDescendants = style_decode::isInherited(propId) ||
                                    propId == prop::TextDecorationColor ||
                                    propId == prop::TextDecorationThickness;
    markRestyle(arena, node, reachesDescendants ? StateRestyle::Subtree : StateRestyle::Self, cause);
}

}  // namespace xt
