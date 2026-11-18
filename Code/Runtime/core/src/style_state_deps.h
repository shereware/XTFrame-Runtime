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
/// Built from the IR's STYL rules when the Document loads (EventDispatcher::init); read by the
/// event dispatcher's state-bit writes. Internal to xt_core.

namespace xt {

class IrView;

/// One compound of one rule that tests an ElementState bit (in its own part or in a `:not()` /
/// condition part): the bits it tests, the compound with every state, attribute, structural and
/// other context-dependent condition replaced by "always true" (a superset of the elements it
/// can match, so filtering by it never misses one), and whether it is the selector's subject.
struct StateDependency {
    uint16_t stateMask = 0;
    bool subject = false;
    uint32_t partOffset = 0;  // Into StateDependencyTable::parts.
    uint32_t partCount = 0;
};

struct StateDependencyTable {
    std::vector<StateDependency> entries;
    std::vector<IrSelPart> parts;
    std::span<const uint32_t> classIds;  // The IR's STYL class-id pool.
};

/// Rebuilds `out` from `ir`'s STYL rules (every rule, `@media` ones included).
void buildStateDependencies(const IrView& ir, StateDependencyTable& out);

/// What a change of ElementState bit(s) `changedBits` on `node` re-resolves (B.2).
enum class StateRestyle : uint8_t {
    None = 0,     ///< No compound testing the bits can match the element.
    Self = 1,     ///< Only subject compounds match: the element itself.
    Subtree = 2,  ///< A non-subject (ancestor) compound matches: the element and its subtree.
};
StateRestyle stateChangeRestyle(const StateDependencyTable& table, const NodeArena& arena, NodeId node,
                                uint16_t changedBits);

}  // namespace xt
