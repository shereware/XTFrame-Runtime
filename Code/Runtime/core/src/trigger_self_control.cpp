#include <algorithm>

#include "trigger_runtime.h"
#include "xt/document.h"
#include "xt/ir.h"

/// T-774 (Docs/design/58-value-functions-and-drivers.md §58.6 item 2, 27 §27.8, Docs/tasks/T-774.md):
/// a timeline's own marker (or end) pausing, stopping, seeking or replaying it from a marker takes
/// effect at the marker's time. TriggerRuntime finds those "self-control" actions when it loads;
/// Document::stepTimelineGroups() cuts its step at such a marker and calls applySelfControl(), which
/// performs them through perform() (the same code as a trigger run) and records their outcomes; the
/// trigger itself still runs after the frame's evaluation, in the usual order, and takes those
/// outcomes instead of performing the actions again (runChain()).
/// T-783 (Docs/tasks/T-783.md): an `if` in front of such an action is decided at the marker's time
/// too (its result is recorded and the trigger run takes it), and the end of a `<repeat>` instance's
/// copy is cut the same way (RepeatController::step()).

namespace xt {

namespace {

/// True for an action that acts on the timeline `timeline` itself and is performed by the step.
bool isSelfControl(const IrTriggerAction& action, StringId timeline) {
    if ((action.flags & kActionMedia) != 0 || action.arg0 != timeline) {
        return false;
    }
    switch (static_cast<TriggerAction>(action.kind)) {
        case TriggerAction::Pause:
        case TriggerAction::Stop:
        case TriggerAction::Seek:
            return true;
        case TriggerAction::Play:
            // `play(x, from=m)`; an explicit `hold` leaves a playing timeline alone.
            return action.arg1 != kInvalidString && (action.flags & kPlayHold) == 0;
        default:
            return false;
    }
}

}  // namespace

void TriggerRuntime::indexSelfActions() {
    m_selfActions.clear();
    m_guardedSelf.clear();
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    std::vector<std::pair<uint16_t, bool>> stack;  // (action, an earlier action was not self-control)
    for (size_t index = 0; index < tc.triggers().size(); ++index) {
        const IrTrigger& trig = tc.triggers()[index];
        const TriggerEvent ev = static_cast<TriggerEvent>(trig.event);
        if (ev != TriggerEvent::TimelineEnd && ev != TriggerEvent::TimelineMarker) {
            continue;
        }
        // Each statement head's chain, as runChain() follows it, up to the first action whose
        // outcome is only known when the trigger runs. T-783: an `if` reached through self-control
        // actions only is decided at the marker's time (selfChain()), so both its branches count;
        // one after any other action stays the trigger run's (its condition may read that action's
        // effect).
        bool guarded = false;
        for (uint16_t head = 1; head <= trig.headCount; ++head) {
            stack.assign(1, {head, false});
            uint32_t steps = 0;
            while (!stack.empty() && steps++ <= 2u * trig.actionCount) {
                const auto [at, other] = stack.back();
                stack.pop_back();
                if (at == 0 || at > trig.actionCount) {
                    continue;
                }
                const IrTriggerAction& action = tc.actions()[trig.actionOffset + at - 1];
                const TriggerAction kind = static_cast<TriggerAction>(action.kind);
                if (kind == TriggerAction::Remove || (kind == TriggerAction::If && other)) {
                    continue;
                }
                if (kind == TriggerAction::If) {
                    guarded = true;
                    stack.emplace_back(action.alt, false);
                    stack.emplace_back(action.next, false);
                    continue;
                }
                const bool self = isSelfControl(action, trig.arg0);
                if (self) {
                    m_selfActions.emplace_back(static_cast<uint32_t>(index), at);
                }
                if (kind != TriggerAction::Play) {  // A play waits for an end (or never continues).
                    stack.emplace_back(action.next, other || !self);
                }
            }
        }
        if (guarded) {
            m_guardedSelf.push_back(static_cast<uint32_t>(index));
        }
    }
    std::sort(m_selfActions.begin(), m_selfActions.end());
    m_selfActions.erase(std::unique(m_selfActions.begin(), m_selfActions.end()), m_selfActions.end());
}

bool TriggerRuntime::selfChain(NodeId node, uint32_t index, bool apply, bool instance) {
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    const IrTrigger& trig = tc.triggers()[index];
    // As a timeline trigger's run: no pointer button, value 0, no port.
    struct ButtonScope {
        uint8_t& slot;
        uint8_t saved;
        ~ButtonScope() { slot = saved; }
    } buttonScope{m_runButton, m_runButton};
    m_runButton = 0;
    bool reached = false;
    for (uint16_t head = 1; head <= trig.headCount; ++head) {
        uint16_t at = head;
        bool other = false;
        uint32_t steps = 0;
        while (at != 0 && at <= trig.actionCount && steps++ <= trig.actionCount) {
            if (!m_doc.m_arena.alive(node)) {
                return reached;
            }
            const IrTriggerAction& action = tc.actions()[trig.actionOffset + at - 1];
            const TriggerAction kind = static_cast<TriggerAction>(action.kind);
            if (kind == TriggerAction::Remove || (kind == TriggerAction::If && other)) {
                break;
            }
            SelfApplied done;
            done.node = node;
            done.generation = m_doc.m_arena.generation(node);
            done.trigger = index;
            done.action = at;
            done.instance = instance;
            if (kind == TriggerAction::If) {
                // T-783: decided at the marker's time; the trigger run takes this result (runChain()).
                const bool holds = condition(node, action, 0.0f, 0xFFFFFFFFu);
                if (apply) {
                    done.outcome = holds ? Outcome::Done : Outcome::Stop;
                    m_selfApplied.push_back(done);
                }
                at = holds ? action.next : action.alt;
                continue;
            }
            const bool self = isSelfControl(action, trig.arg0);
            if (self) {
                reached = true;
                if (!apply) {
                    return true;
                }
                struct CurrentTrigger {  // As runChain(): the source a surface hide would record.
                    uint32_t& slot;
                    uint32_t saved;
                    ~CurrentTrigger() { slot = saved; }
                } current{m_currentTrigger, m_currentTrigger};
                m_currentTrigger = index;
                done.outcome =
                    perform(node, action, done.waitFor, done.waitScope, done.waitGroup, 0.0f, 0xFFFFFFFFu, 0);
                m_selfApplied.push_back(done);
            }
            if (kind == TriggerAction::Play) {
                break;  // Waits for an end (or never continues).
            }
            other = other || !self;
            at = action.next;
        }
    }
    return reached;
}

bool TriggerRuntime::selfControls(StringId timeline, TimelineEventKind kind, StringId marker, NodeId scope) {
    for (const auto& [node, index] : m_timelineTriggers) {
        const auto it = std::lower_bound(m_selfActions.begin(), m_selfActions.end(),
                                         std::make_pair(index, static_cast<uint16_t>(0)));
        if (it == m_selfActions.end() || it->first != index ||
            !matchesTimelineEvent(node, index, timeline, kind, marker, scope)) {
            continue;
        }
        // T-783: behind an `if`, only when its condition lets the chain reach one now.
        if (!std::binary_search(m_guardedSelf.begin(), m_guardedSelf.end(), index) ||
            selfChain(node, index, /*apply=*/false)) {
            return true;
        }
    }
    return false;
}

void TriggerRuntime::applySelfControl(StringId timeline, TimelineEventKind kind, StringId marker,
                                      NodeId scope) {
    // By index: an action may change the list (a stopped timeline's instance copy, T-177).
    for (size_t i = 0; i < m_timelineTriggers.size(); ++i) {
        const NodeId node = m_timelineTriggers[i].first;
        const uint32_t index = m_timelineTriggers[i].second;
        const auto it = std::lower_bound(m_selfActions.begin(), m_selfActions.end(),
                                         std::make_pair(index, static_cast<uint16_t>(0)));
        if (it == m_selfActions.end() || it->first != index ||
            !matchesTimelineEvent(node, index, timeline, kind, marker, scope)) {
            continue;
        }
        selfChain(node, index, /*apply=*/true);
    }
}

bool TriggerRuntime::instanceEndTrigger(NodeId node, uint32_t index, NodeId scope, StringId timeline) const {
    const NodeArena& arena = m_doc.m_arena;
    const IrTrigger& trig = m_doc.m_ir.triggers().triggers()[index];
    if (trig.arg0 != timeline || static_cast<TriggerEvent>(trig.event) != TriggerEvent::TimelineEnd ||
        !arena.alive(node) || !arena.alive(scope) || (node != scope && !arena.isAncestorOf(scope, node))) {
        return false;
    }
    const auto it = std::lower_bound(m_selfActions.begin(), m_selfActions.end(),
                                     std::make_pair(index, static_cast<uint16_t>(0)));
    return it != m_selfActions.end() && it->first == index;
}

bool TriggerRuntime::instanceSelfControls(NodeId scope, StringId timeline) {
    for (const auto& [node, index] : m_timelineTriggers) {
        if (instanceEndTrigger(node, index, scope, timeline) &&
            (!std::binary_search(m_guardedSelf.begin(), m_guardedSelf.end(), index) ||
             selfChain(node, index, /*apply=*/false))) {
            return true;
        }
    }
    return false;
}

void TriggerRuntime::applyInstanceSelfControl(NodeId scope, StringId timeline) {
    for (size_t i = 0; i < m_timelineTriggers.size(); ++i) {  // By index, as applySelfControl().
        const NodeId node = m_timelineTriggers[i].first;
        const uint32_t index = m_timelineTriggers[i].second;
        if (instanceEndTrigger(node, index, scope, timeline)) {
            selfChain(node, index, /*apply=*/true, /*instance=*/true);
        }
    }
}

void TriggerRuntime::dropSelfAppliedOf(bool instance) {
    m_selfApplied.erase(std::remove_if(m_selfApplied.begin(), m_selfApplied.end(),
                                       [instance](const SelfApplied& s) { return s.instance == instance; }),
                        m_selfApplied.end());
}

bool TriggerRuntime::takeSelfApplied(NodeId node, uint32_t trigger, uint16_t action, Outcome& outcome,
                                     StringId& waitFor, NodeId& waitScope, NodeId& waitGroup) {
    const uint32_t generation = m_doc.m_arena.generation(node);
    for (size_t i = 0; i < m_selfApplied.size(); ++i) {
        const SelfApplied& s = m_selfApplied[i];
        if (s.node == node && s.generation == generation && s.trigger == trigger && s.action == action) {
            outcome = s.outcome;
            waitFor = s.waitFor;
            waitScope = s.waitScope;
            waitGroup = s.waitGroup;
            m_selfApplied.erase(m_selfApplied.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

}  // namespace xt
