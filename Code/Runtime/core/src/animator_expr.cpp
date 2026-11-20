#include <algorithm>

#include "expr_op_info.h"
#include "timeline_expr.h"
#include "xt/animator.h"

/// T-776: the Animator's part of the timeline expressions (Docs/design/58-value-functions-and-
/// drivers.md §58.5): programs attached to `@timeline` member instances, when they run and how
/// their outputs replace track components. evaluate() (animator.cpp) calls these; see
/// Animator::setExpressions() in xt/animator.h.

namespace xt {

bool Animator::attachExpression(uint32_t id, uint32_t expr) {
    m_changedSinceEvaluate = true;  // T-749
    Instance* inst = findInstance(id);
    if (inst == nullptr || m_exprSet == nullptr) {
        return false;
    }
    const IrTimelineExpr& e = m_exprSet->record(expr);
    ExprSlot slot;
    slot.expr = expr;
    slot.track = e.track;
    slot.component = e.component;
    const auto at = std::upper_bound(inst->exprs.begin(), inst->exprs.end(), slot.track,
                                     [](uint16_t track, const ExprSlot& s) { return track < s.track; });
    inst->exprs.insert(at, slot);
    inst->exprMask |= m_exprSet->inputMask(expr);
    inst->exprDirty = 1;
    return true;
}

void Animator::setGroupTime(uint32_t id, float groupT) {
    Instance* inst = findInstance(id);
    if (inst != nullptr && !(inst->groupT == groupT)) {
        inst->groupT = groupT;
        inst->exprDirty = 1;
    }
}

void Animator::setPlaySeed(uint32_t id, uint32_t seed) {
    Instance* inst = findInstance(id);
    if (inst != nullptr && inst->playSeed != seed) {
        inst->playSeed = seed;
        inst->exprDirty = 1;
    }
}

bool Animator::exprsStale(const Instance& inst) const {
    if (inst.exprDirty != 0 || (inst.exprMask & kExprInputProp) != 0) {
        return true;  // Another element's property is read every update (outputs compared later).
    }
    const uint16_t mask = inst.exprMask;
    return ((mask & (kExprInputPort | kExprInputPortHistory)) != 0 &&
            m_exprSet->portGeneration() != inst.exprPortGen) ||
           ((mask & kExprInputLayout) != 0 && m_exprSet->layoutGeneration() != inst.exprLayoutGen) ||
           // T-781: a property read with a delay: only when a history recorded (never while
           // stopped), not every update as PushProp.
           ((mask & (kExprInputPortHistory | kExprInputPropHistory)) != 0 &&
            m_exprSet->portHistoryGeneration() != inst.exprHistoryGen);
}

void Animator::exprsRan(Instance& inst) const {
    inst.exprDirty = 0;
    inst.exprPortGen = m_exprSet->portGeneration();
    inst.exprLayoutGen = m_exprSet->layoutGeneration();
    inst.exprHistoryGen = m_exprSet->portHistoryGeneration();
}

bool Animator::applyExprs(Instance& inst, size_t trackIndex, Value& v, bool stale, size_t& cursor) {
    while (cursor < inst.exprs.size() && inst.exprs[cursor].track < trackIndex) {
        ++cursor;
    }
    bool changed = false;
    for (; cursor < inst.exprs.size() && inst.exprs[cursor].track == trackIndex; ++cursor) {
        ExprSlot& slot = inst.exprs[cursor];
        const float in = exprValueComponent(v, slot.component);
        if (stale || slot.valid == 0 || !(slot.in == in)) {
            const float out = m_exprSet->evaluate(slot.expr, inst.groupT, in, inst.playSeed);
            ++m_exprEvaluated;
            changed = changed || slot.valid == 0 || !(slot.out == out);
            slot.in = in;
            slot.out = out;
            slot.valid = 1;
        }
        setExprValueComponent(v, slot.component, slot.out);
    }
    return changed;
}

bool Animator::probeHeldExprs(Instance& inst, const Timeline& tl, float localT) {
    bool changed = false;
    size_t cursor = 0;
    for (const ExprSlot& first : inst.exprs) {
        // Each track with a program once (the slots are sorted by track).
        if (&first != inst.exprs.data() && (&first - 1)->track == first.track) {
            continue;
        }
        if (first.track >= tl.tracks.size()) {
            continue;
        }
        const Track& track = tl.tracks[first.track];
        if (track.kfCount == 0 || exprTrackComponents(static_cast<uint8_t>(track.type)) == 0) {
            continue;
        }
        Value v = evalTrack(tl, track, localT, nullptr, nullptr, &inst.overrideEasing, m_easingPoints);
        if (v.isNeutral()) {
            continue;
        }
        changed = applyExprs(inst, first.track, v, /*stale=*/true, cursor) || changed;
    }
    return changed;
}

}  // namespace xt
