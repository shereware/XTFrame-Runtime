#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/ir_pod.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// T-723 (Docs/design/10-backends.md §10.16 W5, Docs/tasks/T-723.md B.1-B.2): the state dependency
/// table -- which style rules can change their match when an element's ElementState bit (hover,
/// active, focus, checked, disabled, ...) changes -- and the restyle scope it gives a state change.
/// T-763 (Docs/tasks/T-763.md items 1-3): the same for custom states (`:state()`), the
/// `:focus-player()` bits and classes. Built from the IR's STYL rules when the Document loads
/// (EventDispatcher::init); read by every state-bit, custom-state and class write. Internal to
/// xt_core.

namespace xt {

class IrView;

/// One compound of one rule that tests an ElementState bit, a custom state or a `:focus-player()`
/// bit (in its own part or in a `:not()` / condition part): what it tests, the compound with every
/// state, attribute, structural and other context-dependent condition replaced by "always true"
/// (a superset of the elements it can match, so filtering by it never misses one), and whether it
/// is the selector's subject.
struct StateDependency {
    uint16_t stateMask = 0;
    uint8_t focusPlayerMask = 0;  // T-763: `:focus-player(n)` bit n.
    bool subject = false;
    uint32_t customMask = 0;  // T-763: custom state bit i = `:state(<STAT name i>)`.
    uint32_t partOffset = 0;  // Into StateDependencyTable::parts.
    uint32_t partCount = 0;
};

/// T-763 (item 3): a class some compound names (a class selector or a `.x` / `:not(.x)`
/// condition), and in which positions.
struct ClassDependency {
    uint32_t cls = 0;       // The class's StringId (the IR's, which ElementData::classes share).
    bool subject = false;   // Named by a subject compound.
    bool ancestor = false;  // Named by a non-subject (ancestor) compound.
};

struct StateDependencyTable {
    std::vector<StateDependency> entries;
    std::vector<IrSelPart> parts;
    std::span<const uint32_t> classIds;    // The IR's STYL class-id pool.
    std::vector<ClassDependency> classes;  // T-763: sorted by `cls`, one entry per class.
};

/// Rebuilds `out` from `ir`'s STYL rules (every rule, `@media` ones included).
void buildStateDependencies(const IrView& ir, StateDependencyTable& out);

/// What a state or class change on `node` re-resolves (B.2). The selector engine has no sibling
/// combinators, so no change reaches another element than the node and its subtree.
enum class StateRestyle : uint8_t {
    None = 0,     ///< No compound testing what changed can match the element.
    Self = 1,     ///< Only subject compounds match: the element itself.
    Subtree = 2,  ///< A non-subject (ancestor) compound matches: the element and its subtree.
};

/// T-763: the bits one write changed, each kind in its own mask.
struct StateChange {
    uint16_t bits = 0;         // ElementState bits.
    uint32_t custom = 0;       // Custom state bits.
    uint8_t focusPlayers = 0;  // `:focus-player()` bits.
};

StateRestyle stateChangeRestyle(const StateDependencyTable& table, const NodeArena& arena, NodeId node,
                                const StateChange& change);
inline StateRestyle stateChangeRestyle(const StateDependencyTable& table, const NodeArena& arena, NodeId node,
                                       uint16_t changedBits) {
    return stateChangeRestyle(table, arena, node, StateChange{changedBits, 0, 0});
}

/// T-763 (item 3): the scope of a class list change from `before` to `after` (both sorted):
/// the widest position any added or removed class has in the table.
StateRestyle classChangeRestyle(const StateDependencyTable& table, std::span<const StringId> before,
                                std::span<const StringId> after);

/// Marks what `scope` says: nothing, `node` (NodeArena::markStyleDirtySelf), or `node` and every
/// descendant. Never an ancestor.
void markRestyle(NodeArena& arena, NodeId node, StateRestyle scope, DirtyCause cause);

/// markRestyle(), plus kDirtyPaint on `node` when `change` holds kStateFocus (the painter draws a
/// focused editable input's selection and caret from the bit, not from the style).
void markStateChange(NodeArena& arena, NodeId node, StateRestyle scope, const StateChange& change,
                     DirtyCause cause);

/// The old full range (05.8): `node`, its ancestors (NodeArena::markDirty) and every descendant.
/// The equivalence check's legacy mode, and the writes no table covers (an `id` change, any other
/// attribute).
void markStyleDirtyFullRange(NodeArena& arena, NodeId node, DirtyCause cause);

/// T-723 (10 §10.16 W5) / T-763: what a changed runtime declaration of `propId` (a style binding,
/// ElementRef::setStyle / setStyleTemplate / setEffects) re-resolves -- the element alone, plus its
/// subtree when `propId` reaches the descendants' computed style (an inherited property, or the
/// text-decoration color / thickness a decorating ancestor passes down); ancestors never.
/// `legacy`: the old full range, for the equivalence check only.
void markRuntimeDeclRestyle(NodeArena& arena, NodeId node, uint16_t propId, bool legacy, DirtyCause cause);

}  // namespace xt
