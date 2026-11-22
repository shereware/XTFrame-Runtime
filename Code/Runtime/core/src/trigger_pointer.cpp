#include <cmath>
#include <cstring>

#include "trigger_runtime.h"
#include "xt/document.h"
#include "xt/ir.h"

/// T-871 (Docs/design/60-input-channels.md §60.3.2-§60.3.3, Docs/tasks/T-871.md): the values set()
/// and add() read from the firing event when the action runs -- the pointer quantities
/// (`pointer.delta`, `.fromPress`, `.angle`, `.turn`, `.distance`, `.grab`, plus T-386's `.fraction`
/// / `.local`) and a wheel trigger's `event.dx` / `event.dy` -- and the per-press state behind
/// `pointer.turn` / `pointer.grab`: every press starts tracking the elements those values name
/// (load() collects their ids), every pointer move adds the angle turned around each one's centre
/// (wrapped to -180..180 per move, so the sum crosses +-180 without a jump), the release stops it.

namespace xt {

namespace {

constexpr float kDegreesPerRadian = 57.29577951308232f;

/// The bearing of `v` in degrees: 0 = right, clockwise (y down) positive, -180..180.
float bearing(Vec2 v) { return std::atan2(v.y, v.x) * kDegreesPerRadian; }

/// `d` (a difference of two bearings) wrapped to -180..180.
float wrapDegrees(float d) {
    while (d > 180.0f) {
        d -= 360.0f;
    }
    while (d < -180.0f) {
        d += 360.0f;
    }
    return d;
}

}  // namespace

TriggerRuntime::Job TriggerRuntime::resumeJob(const Waiter& w) {
    Job job{w.node, w.generation, w.trigger, w.next, w.value, w.port, w.player, w.pointer, w.button};
    job.delta = w.delta;
    job.press = w.press;
    job.wheel = w.wheel;
    return job;
}

Vec2 TriggerRuntime::centreOf(NodeId node) const {
    const LayoutBox& box = m_doc.m_arena.element(node).box;
    return m_doc.nodeToScreen(node, Vec2{box.width * 0.5f, box.height * 0.5f});
}

const TriggerRuntime::Tracker* TriggerRuntime::trackerOf(StringId id) const {
    for (const Tracker& t : m_trackers) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

TriggerRuntime::Tracker TriggerRuntime::startTracker(StringId id, NodeId node, Vec2 pos, Vec2 press) const {
    Tracker t;
    t.id = id;
    t.node = node;
    t.generation = m_doc.m_arena.generation(node);
    t.lastAngle = bearing(pos - centreOf(node));
    t.grab = m_doc.fractionIn(node, press);
    return t;
}

void TriggerRuntime::onPointerSample(PointerSample kind, Vec2 pos) {
    const NodeArena& arena = m_doc.m_arena;
    if (kind == PointerSample::Press) {
        m_trackers.clear();
        for (const StringId id : m_trackIds) {
            const ElementRef target = m_doc.byId(m_doc.m_ir.string(id));
            if (target.valid() && arena.kind(target.node) == NodeKind::Element) {
                m_trackers.push_back(startTracker(id, target.node, pos, pos));
            }
        }
        m_tracking = !m_trackers.empty();
        return;
    }
    if (!m_tracking) {
        return;
    }
    for (Tracker& t : m_trackers) {
        if (!arena.alive(t.node) || arena.generation(t.node) != t.generation) {
            continue;
        }
        const Vec2 v = pos - centreOf(t.node);
        if (v.x * v.x + v.y * v.y < 1e-6f) {
            continue;  // On the centre: no bearing; the next move measures from the last one.
        }
        const float angle = bearing(v);
        t.turn += wrapDegrees(angle - t.lastAngle);
        t.lastAngle = angle;
    }
    if (kind == PointerSample::Release) {
        m_tracking = false;  // The values stay as the release left them until the next press.
    }
}

bool TriggerRuntime::consumesWheel(NodeId node) {
    // 60 §60.3.2: only a `consume()` heading the first statement counts (the compiler allows it
    // nowhere else in an `on:wheel`); decided at dispatch, as consumesOutsidePress().
    const NodeArena& arena = m_doc.m_arena;
    if (node == kInvalidNode || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return false;
    }
    const uint32_t table = arena.common(node).triggerTable;
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (table == 0 || table > tc.tables().size()) {
        return false;
    }
    const IrTriggerTable& t = tc.tables()[table - 1];
    for (uint32_t k = 0; k < t.triggerCount; ++k) {
        const IrTrigger& trig = tc.triggers()[t.triggerOffset + k];
        if (static_cast<TriggerEvent>(trig.event) == TriggerEvent::Wheel && trig.actionCount > 0 &&
            trig.actionOffset < tc.actions().size() &&
            tc.actions()[trig.actionOffset].kind == static_cast<uint8_t>(TriggerAction::Consume)) {
            return true;
        }
    }
    return false;
}

std::vector<TriggerRuntime::TrackState> TriggerRuntime::trackStates() const {
    std::vector<TrackState> out;
    if (!m_tracking) {
        return out;
    }
    for (const Tracker& t : m_trackers) {
        out.push_back(TrackState{std::string(m_doc.m_ir.string(t.id)), t.turn, t.grab});
    }
    return out;
}

void TriggerRuntime::restoreTracks(const std::vector<TrackState>& states, bool pressed, Vec2 pos,
                                   Vec2 press) {
    m_trackers.clear();
    m_tracking = false;
    if (!pressed) {
        return;
    }
    for (const StringId id : m_trackIds) {
        const std::string_view name = m_doc.m_ir.string(id);
        const ElementRef target = m_doc.byId(name);
        if (!target.valid() || m_doc.m_arena.kind(target.node) != NodeKind::Element) {
            continue;
        }
        Tracker t = startTracker(id, target.node, pos, press);
        for (const TrackState& s : states) {
            if (s.id == name) {
                t.turn = s.turn;
                t.grab = s.grab;
            }
        }
        m_trackers.push_back(t);
    }
    m_tracking = !m_trackers.empty();
}

bool TriggerRuntime::runtimeSourceValue(uint8_t source, float arg2, float& out) const {
    const SetSource src = static_cast<SetSource>(source);
    switch (src) {
        case SetSource::EventDx:
            out = m_runWheel.x;
            return true;
        case SetSource::EventDy:
            out = m_runWheel.y;
            return true;
        case SetSource::PointerFractionX:
        case SetSource::PointerFractionY:
        case SetSource::PointerLocalX:
        case SetSource::PointerLocalY:
        case SetSource::PointerDeltaX:
        case SetSource::PointerDeltaY:
        case SetSource::PointerFromPressX:
        case SetSource::PointerFromPressY:
        case SetSource::PointerAngle:
        case SetSource::PointerTurn:
        case SetSource::PointerDistance:
        case SetSource::PointerGrabX:
        case SetSource::PointerGrabY:
            break;
        default:
            return false;
    }
    out = 0.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &arg2, sizeof(bits));
    NodeId target = kInvalidNode;
    if (bits != kInvalidString) {
        const ElementRef ref = m_doc.byId(m_doc.m_ir.string(bits));
        if (!ref.valid() || m_doc.m_arena.kind(ref.node) != NodeKind::Element) {
            return true;  // A named element that is not there: 0.
        }
        target = ref.node;
    }
    const Vec2 p = m_runPointer;
    // A movement in root coordinates, or along `target`'s local axes (localPosIn()'s mapping, so
    // its rotation, scale and 3D plane count).
    const auto movement = [&](Vec2 from) {
        return target == kInvalidNode ? p - from
                                      : m_doc.localPosIn(target, p) - m_doc.localPosIn(target, from);
    };
    switch (src) {
        case SetSource::PointerFractionX:
        case SetSource::PointerFractionY: {
            const Vec2 f = m_doc.fractionIn(target, p);
            out = src == SetSource::PointerFractionY ? f.y : f.x;
            break;
        }
        case SetSource::PointerLocalX:
        case SetSource::PointerLocalY: {
            const Vec2 l = m_doc.localPosIn(target, p);
            out = src == SetSource::PointerLocalY ? l.y : l.x;
            break;
        }
        case SetSource::PointerDeltaX:
        case SetSource::PointerDeltaY: {
            const Vec2 d = movement(p - m_runDelta);
            out = src == SetSource::PointerDeltaY ? d.y : d.x;
            break;
        }
        case SetSource::PointerFromPressX:
        case SetSource::PointerFromPressY: {
            const Vec2 d = movement(m_runPress);
            out = src == SetSource::PointerFromPressY ? d.y : d.x;
            break;
        }
        case SetSource::PointerAngle:
        case SetSource::PointerDistance: {
            if (target == kInvalidNode) {
                break;
            }
            const Vec2 v = p - centreOf(target);
            out = src == SetSource::PointerAngle ? bearing(v) : std::sqrt(v.x * v.x + v.y * v.y);
            break;
        }
        case SetSource::PointerTurn:
        case SetSource::PointerGrabX:
        case SetSource::PointerGrabY: {
            const Tracker* t = trackerOf(bits);
            if (t != nullptr) {
                out = src == SetSource::PointerTurn
                          ? t->turn
                          : (src == SetSource::PointerGrabY ? t->grab.y : t->grab.x);
            }
            break;
        }
        default:
            break;
    }
    return true;
}

}  // namespace xt
