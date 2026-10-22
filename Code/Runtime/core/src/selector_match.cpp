#include "selector_match.h"

namespace xt {

bool matchRule(const NodeArena& arena, const IrView& /*ir*/, NodeId element, const IrRule& rule,
               std::span<const IrSelPart> selectorParts, std::span<const uint32_t> classIds,
               const SelectorMatchContext& ctx) {
    if (rule.selCount == 0 || rule.selOffset + rule.selCount > selectorParts.size()) {
        return false;
    }
    return matchChain(arena, element, selectorParts.subspan(rule.selOffset, rule.selCount), classIds, ctx);
}

}  // namespace xt
