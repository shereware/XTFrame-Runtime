#include "xt/animator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "camera.h"
#include "xt/assert.h"
#include "xt/node_arena.h"
#include "xt/num_guard.h"
#include "xt/property.h"

/// Animator implementation. See Docs/design/04-timeline.md §4.5-4.9, Docs/tasks/T-07.md
/// ("evaluate 精確流程" for the exact, authoritative step order).

namespace xt {

namespace {

/// T-345 (05 §5.9.9): a keyframe or override value of an Element's px property is design px, so
/// it is multiplied by the element's `ui-scale` factor (ComputedStyle::uiScaleEff), as the cascade
/// does with its own px lengths. Transition ends come from the cascade and are already scaled.
void scaleElementPx(const NodeArena& arena, NodeId node, PropId p, Value& v) {
    if (arena.kind(node) != NodeKind::Element) {
        return;
    }
    const float s = arena.element(node).style.uiScaleEff;
    if (s == 1.0f) {
        return;
    }
    if (p == prop::TranslateX || p == prop::TranslateY) {
        if (v.type == ValueType::Float) {
            v.f *= s;
        } else if (v.type == ValueType::Length && v.len.unit == LengthUnit::Px) {
            v.len.value *= s;
        }
        return;
    }
    if (p == prop::Translate) {
        if (v.type == ValueType::Vec2) {
            v.v2.x *= s;
            v.v2.y *= s;
        }
        return;
    }
    if (p >= prop::Width && p <= prop::Order && p != prop::ScrollX && p != prop::ScrollY &&
        v.type == ValueType::Length && v.len.unit == LengthUnit::Px) {
        v.len.value *= s;
    }
}

uint64_t packKey(NodeId node, PropId prop) {
    return (static_cast<uint64_t>(node) << 16) | static_cast<uint64_t>(prop);
}

uint32_t maxFloatLen(const Timeline& tl, const Track& track) {
    uint32_t maxLen = 0;
    for (uint32_t k = 0; k < track.kfCount; ++k) {
        maxLen = std::max(maxLen, tl.keyframes[track.kfOffset + k].value.floatCount);
    }
    return maxLen;
}

uint32_t maxStopLen(const Timeline& tl, const Track& track) {
    uint32_t maxLen = 0;
    for (uint32_t k = 0; k < track.kfCount; ++k) {
        maxLen = std::max(maxLen, tl.keyframes[track.kfOffset + k].value.stopCount);
    }
    return maxLen;
}

/// True if `point` was crossed going from `from` to `to` (half-open on the `from` side): forward
/// (to > from) checks (from, to], backward (to < from) checks [to, from). Equal endpoints never
/// match (zero-length segment).
bool segmentContains(float from, float to, float point) {
    if (to > from) {
        return point > from && point <= to;
    }
    if (to < from) {
        return point < from && point >= to;
    }
    return false;
}

bool fillHas(FillMode fill, FillMode bit) {
    return (static_cast<uint8_t>(fill) & static_cast<uint8_t>(bit)) != 0;
}

/// T-260a: `node`'s generation for a queued notification; 0 when it is not alive (or none).
uint32_t liveGeneration(const NodeArena& arena, NodeId node) {
    return arena.alive(node) ? arena.generation(node) : 0u;
}

}  // namespace

TimelineId Animator::registerTimeline(const Timeline* timelinePtr) {
    for (size_t i = 0; i < m_timelines.size(); ++i) {
        if (m_timelines[i] == timelinePtr) {
            return static_cast<TimelineId>(i);
        }
    }
    m_timelines.push_back(timelinePtr);
    return static_cast<TimelineId>(m_timelines.size() - 1);
}

const Timeline* Animator::timeline(TimelineId id) const {
    return id < m_timelines.size() ? m_timelines[id] : nullptr;
}

void Animator::growScratchForTimeline(const Timeline& tl) {
    size_t addFloats = 0;
    size_t addStops = 0;
    for (const Track& track : tl.tracks) {
        if (track.type == ValueType::Path || track.type == ValueType::FloatArray) {
            addFloats += maxFloatLen(tl, track);
        } else if (track.type == ValueType::Stops) {
            addStops += maxStopLen(tl, track);
        }
    }
    if (addFloats > 0) {
        m_scratchFloatsNeeded += addFloats;
        if (m_scratchFloats.size() < m_scratchFloatsNeeded) {
            m_scratchFloats.resize(m_scratchFloatsNeeded);
        }
    }
    if (addStops > 0) {
        m_scratchStopsNeeded += addStops;
        if (m_scratchStops.size() < m_scratchStopsNeeded) {
            m_scratchStops.resize(m_scratchStopsNeeded);
        }
    }
}

void Animator::growScratchFor(const Value& value) {
    size_t addFloats = 0;
    size_t addStops = 0;
    if (value.type == ValueType::Path || value.type == ValueType::FloatArray) {
        addFloats = value.floatCount;
    } else if (value.type == ValueType::Stops) {
        addStops = value.stopCount;
    }
    if (addFloats > 0) {
        m_scratchFloatsNeeded += addFloats;
        if (m_scratchFloats.size() < m_scratchFloatsNeeded) {
            m_scratchFloats.resize(m_scratchFloatsNeeded);
        }
    }
    if (addStops > 0) {
        m_scratchStopsNeeded += addStops;
        if (m_scratchStops.size() < m_scratchStopsNeeded) {
            m_scratchStops.resize(m_scratchStopsNeeded);
        }
    }
}

void Animator::growPendingCapacity(size_t additionalSlots) {
    if (additionalSlots == 0) {
        return;
    }
    m_pendingCapacityNeeded += additionalSlots;
    if (m_pending.capacity() < m_pendingCapacityNeeded) {
        m_pending.reserve(m_pendingCapacityNeeded);
    }
    if (m_orient.capacity() < m_pendingCapacityNeeded) {
        m_orient.reserve(m_pendingCapacityNeeded);
    }
}

void Animator::growOrderCapacity() {
    if (m_order.capacity() < m_instances.size()) {
        m_order.reserve(m_instances.size());
    }
}

Animator::Instance* Animator::findInstance(uint32_t id) {
    for (Instance& inst : m_instances) {
        if (inst.id == id) {
            return &inst;
        }
    }
    return nullptr;
}

const Animator::Instance* Animator::findInstance(uint32_t id) const {
    for (const Instance& inst : m_instances) {
        if (inst.id == id) {
            return &inst;
        }
    }
    return nullptr;
}

uint32_t Animator::play(const PlayParams& params, float clock) {
    const Timeline* tl = timeline(params.timeline);
    if (tl == nullptr) {
        // T-257b: IrView::validate rejects IR naming a timeline it lacks; an id that still does
        // not resolve (an API caller's) starts nothing, the same in every build.
        return 0;
    }

    Instance inst;
    inst.id = m_nextId++;
    inst.timeline = params.timeline;
    inst.localToArena = params.localToArena;
    inst.localCount = params.localCount;
    inst.selfNode = params.selfNode;
    inst.startClock = clock;
    inst.delay = params.delay;
    inst.speed = params.speed;
    inst.durationScale = params.durationScale;
    inst.segStart = params.segStart;
    inst.segEnd = params.segEnd < 0.0f ? tl->duration : params.segEnd;
    inst.loop = params.loop;
    // §4.5: iterations is forced to 1 when loop == Once, regardless of the given value.
    inst.iterations = params.loop == LoopMode::Once ? uint16_t{1} : params.iterations;
    inst.fill = params.fill;
    inst.layer = params.layer;
    inst.group = params.group;
    inst.overrideEasing = params.overrideEasing;

    m_instances.push_back(inst);
    growOrderCapacity();
    growScratchForTimeline(*tl);
    growPendingCapacity(tl->tracks.size());
    return inst.id;
}

void Animator::stop(uint32_t id) {
    for (size_t i = 0; i < m_instances.size(); ++i) {
        if (m_instances[i].id == id) {
            m_instances[i] = m_instances.back();
            m_instances.pop_back();
            return;
        }
    }
}

void Animator::pause(uint32_t id, float clock) {
    Instance* inst = findInstance(id);
    if (inst == nullptr) {
        return;
    }
    float elapsed = (clock - inst->startClock - inst->delay) * inst->speed;
    inst->pausedAt = elapsed;
    inst->paused = 1;
}

void Animator::resume(uint32_t id, float clock) {
    Instance* inst = findInstance(id);
    if (inst == nullptr || inst->paused == 0) {
        return;
    }
    if (inst->speed != 0.0f) {
        inst->startClock = clock - inst->delay - inst->pausedAt / inst->speed;
    }
    inst->paused = 0;
}

void Animator::seek(uint32_t id, float localT, float clock) {
    Instance* inst = findInstance(id);
    if (inst == nullptr) {
        return;
    }
    // T-255 / T-255b: a host or IR time of +Inf seeks to the animation's end, NaN or -Inf to 0.
    if (localT == std::numeric_limits<float>::infinity()) {
        localT = inst->segEnd;
    } else if (!std::isfinite(localT)) {
        localT = 0.0f;
    }
    // Treated as iteration 0, forward segment: elapsed == frac.
    float elapsed = (localT - inst->segStart) * inst->durationScale;
    if (inst->paused) {
        inst->pausedAt = elapsed;
    } else if (inst->speed != 0.0f) {
        inst->startClock = clock - inst->delay - elapsed / inst->speed;
    }
    inst->iterCount = 0;
    inst->prevIter = 0;
    inst->finished = 0;
    inst->finishedNotified = 0;
    // Avoid a spurious marker crossing across the jump on the next evaluate().
    inst->prevLocalT = localT;
}

void Animator::setSpeed(uint32_t id, float speed, float clock) {
    Instance* inst = findInstance(id);
    if (inst == nullptr) {
        return;
    }
    if (!std::isfinite(speed)) {
        speed = 0.0f;  // T-255b: a NaN / infinite rate stops the animation instead of poisoning its clock.
    }
    float elapsedOld = inst->paused ? inst->pausedAt : (clock - inst->startClock - inst->delay) * inst->speed;
    inst->speed = speed;
    if (inst->paused) {
        inst->pausedAt = elapsedOld;
    } else if (speed != 0.0f) {
        inst->startClock = clock - inst->delay - elapsedOld / speed;
    }
}

bool Animator::isActive(uint32_t id) const {
    const Instance* inst = findInstance(id);
    return inst != nullptr && inst->finished == 0;
}

bool Animator::isFinished(uint32_t id) const {
    const Instance* inst = findInstance(id);
    return inst != nullptr && inst->finished != 0;
}

float Animator::localTime(uint32_t id, float clock) const {
    const Instance* inst = findInstance(id);
    if (inst == nullptr) {
        return 0.0f;
    }
    return computeLocalState(*inst, clock).localT;
}

float Animator::segmentDuration(uint32_t id) const {
    const Instance* inst = findInstance(id);
    if (inst == nullptr) {
        return 0.0f;
    }
    // Same quantity computeLocalState() calls segLen: the seconds one pass over [segStart, segEnd]
    // takes at durationScale == the instance's time scale.
    return (inst->segEnd - inst->segStart) * inst->durationScale;
}

float Animator::speedOf(uint32_t id) const {
    const Instance* inst = findInstance(id);
    return inst == nullptr ? 0.0f : inst->speed;
}

void Animator::addTransition(const Transition& transition) {
    for (Transition& existing : m_transitions) {
        if (existing.written() == transition.written() && existing.prop == transition.prop) {
            existing = transition;
            growScratchFor(transition.from);
            return;
        }
    }
    m_transitions.push_back(transition);
    growPendingCapacity(size_t{1});
    growScratchFor(transition.from);
}

bool Animator::hasTransition(NodeId node, PropId prop) const {
    for (const Transition& t : m_transitions) {
        if (t.written() == node && t.prop == prop) {
            return true;
        }
    }
    return false;
}

bool Animator::hasOverride(NodeId node, PropId prop) const {
    uint64_t key = packKey(node, prop);
    auto it = std::lower_bound(m_overrides.begin(), m_overrides.end(), key,
                               [](const Override& o, uint64_t k) { return packKey(o.node, o.prop) < k; });
    return it != m_overrides.end() && it->node == node && it->prop == prop;
}

void Animator::setOverride(NodeId node, PropId prop, const Value& value) {
    uint64_t key = packKey(node, prop);
    auto it = std::lower_bound(m_overrides.begin(), m_overrides.end(), key,
                               [](const Override& o, uint64_t k) { return packKey(o.node, o.prop) < k; });
    if (it != m_overrides.end() && it->node == node && it->prop == prop) {
        it->value = value;
        return;
    }
    m_overrides.insert(it, Override{node, prop, value});
    growPendingCapacity(size_t{1});
}

void Animator::clearOverride(NodeId node, PropId prop) {
    uint64_t key = packKey(node, prop);
    auto it = std::lower_bound(m_overrides.begin(), m_overrides.end(), key,
                               [](const Override& o, uint64_t k) { return packKey(o.node, o.prop) < k; });
    if (it != m_overrides.end() && it->node == node && it->prop == prop) {
        m_overrides.erase(it);
    }
}

void Animator::clearNode(NodeId node) {
    if (m_dispatchingNotes) {  // T-260a: drops this node's later TransitionEnd notes of the batch.
        m_clearedWhileDispatching.push_back(
            ClearedTransitions{.node = node, .propLo = 0, .propHi = static_cast<PropId>(~PropId{0})});
    }
    for (size_t i = m_transitions.size(); i > 0; --i) {
        // T-218: an element's transitions include the ones it runs on its <svg> scene nodes.
        if (m_transitions[i - 1].node == node || m_transitions[i - 1].written() == node) {
            m_transitions[i - 1] = m_transitions.back();
            m_transitions.pop_back();
        }
    }
    for (size_t i = m_overrides.size(); i > 0; --i) {
        if (m_overrides[i - 1].node == node) {
            m_overrides.erase(m_overrides.begin() + static_cast<ptrdiff_t>(i - 1));
        }
    }
}

void Animator::cancelTransitionsForNodeRange(NodeId node, PropId propLo, PropId propHi) {
    if (m_dispatchingNotes) {  // T-260a: as clearNode().
        m_clearedWhileDispatching.push_back(
            ClearedTransitions{.node = node, .propLo = propLo, .propHi = propHi});
    }
    for (size_t i = m_transitions.size(); i > 0; --i) {
        const Transition& t = m_transitions[i - 1];
        if (t.written() == node && t.prop >= propLo && t.prop <= propHi) {
            m_transitions[i - 1] = m_transitions.back();
            m_transitions.pop_back();
        }
    }
}

uint32_t Animator::activeInstanceCount() const { return static_cast<uint32_t>(m_instances.size()); }

uint32_t Animator::transitionCount() const { return static_cast<uint32_t>(m_transitions.size()); }

Animator::DebugCapacities Animator::debugCapacities() const {
    DebugCapacities caps;
    caps.pending = m_pending.capacity();
    caps.scratchFloats = m_scratchFloats.capacity();
    caps.scratchStops = m_scratchStops.capacity();
    return caps;
}

std::vector<Animator::InstanceInfo> Animator::debugInstances(float clock) const {
    std::vector<InstanceInfo> out;
    out.reserve(m_instances.size());
    for (const Instance& inst : m_instances) {
        InstanceInfo info;
        info.id = inst.id;
        info.timeline = inst.timeline;
        info.selfNode = inst.selfNode;
        info.localToArena = inst.localToArena;
        info.localCount = inst.localCount;
        info.localT = computeLocalState(inst, clock).localT;
        info.delay = inst.delay;
        info.speed = inst.speed;
        info.durationScale = inst.durationScale;
        info.segStart = inst.segStart;
        info.segEnd = inst.segEnd;
        info.loop = inst.loop;
        info.iterations = inst.iterations;
        info.fill = inst.fill;
        info.layer = inst.layer;
        info.paused = inst.paused != 0;
        info.finished = inst.finished != 0;
        out.push_back(info);
    }
    return out;
}

Animator::LocalState Animator::computeLocalState(const Instance& inst, float clock) const {
    LocalState st;
    float elapsed = inst.paused != 0 ? inst.pausedAt : (clock - inst.startClock - inst.delay) * inst.speed;
    float segLen = (inst.segEnd - inst.segStart) * inst.durationScale;

    if (elapsed < 0.0f) {
        bool hasBackward = fillHas(inst.fill, FillMode::Backwards);
        st.localT = inst.speed < 0.0f ? inst.segEnd : inst.segStart;
        st.iter = 0;
        st.finished = false;
        st.skip = !hasBackward;
        return st;
    }

    if (segLen <= 0.0f) {
        // Degenerate segment (segEnd <= segStart): not covered by the spec. Conservative
        // fallback: treat as immediately finished at segStart.
        st.localT = inst.segStart;
        st.iter = 0;
        st.finished = true;
        return st;
    }

    // Exact multiples of segLen are treated as the closing instant of the PREVIOUS iteration
    // (half-open interval on the right) rather than the opening instant of the next one -- e.g.
    // for a 1-iteration, 1-second segment, elapsed == 1.0 is still iteration 0's final frame
    // (localT == segEnd), not the (already out-of-range) start of iteration 1.
    float ratio = elapsed / segLen;
    // T-255b: saturated, not a plain cast (undefined past the uint32 range, e.g. a huge clock).
    uint32_t iter = toUint32Sat(std::floor(ratio));
    if (iter > 0 && ratio == static_cast<float>(iter)) {
        iter -= 1;
    }
    st.iter = iter;

    if (inst.iterations != 0 && iter >= inst.iterations) {
        st.finished = true;
        bool lastForward = true;
        if (inst.loop == LoopMode::PingPong) {
            uint32_t lastIter = static_cast<uint32_t>(inst.iterations - 1);
            lastForward = (lastIter % 2) == 0;
        }
        bool effectiveForward = lastForward != (inst.speed < 0.0f);
        st.localT = effectiveForward ? inst.segEnd : inst.segStart;
        return st;
    }

    float frac = elapsed - static_cast<float>(iter) * segLen;
    if (inst.loop == LoopMode::PingPong && (iter % 2) == 1) {
        frac = segLen - frac;
    }
    st.localT = inst.segStart + frac / inst.durationScale;
    return st;
}

void Animator::setEasingPoints(std::span<const EasingPoint> points) {
    m_easingPoints.assign(points.begin(), points.end());
}

bool Animator::internEasingPoints(std::span<const EasingPoint> points, uint16_t& offset) {
    if (points.size() < 2) {
        return false;
    }
    const auto same = [](const EasingPoint& a, const EasingPoint& b) {
        return a.in == b.in && a.out == b.out;
    };
    auto it = std::search(m_easingPoints.begin(), m_easingPoints.end(), points.begin(), points.end(), same);
    if (it != m_easingPoints.end()) {
        const size_t at = static_cast<size_t>(it - m_easingPoints.begin());
        if (at <= 0xFFFFu) {
            offset = static_cast<uint16_t>(at);
            return true;
        }
    }
    if (m_easingPoints.size() > 0xFFFFu || points.size() > 0xFFFFu - m_easingPoints.size()) {
        return false;
    }
    offset = static_cast<uint16_t>(m_easingPoints.size());
    m_easingPoints.insert(m_easingPoints.end(), points.begin(), points.end());
    return true;
}

Value Animator::evalTrackAdvancing(const Timeline& tl, const Track& track, float t,
                                   const Easing* overrideLinear, size_t& floatCursor, size_t& stopCursor) {
    float* sf = m_scratchFloats.empty() ? nullptr : m_scratchFloats.data() + floatCursor;
    GradientStop* sg = m_scratchStops.empty() ? nullptr : m_scratchStops.data() + stopCursor;
    Value v = evalTrack(tl, track, t, sf, sg, overrideLinear, m_easingPoints);
    if (sf != nullptr && v.floats == sf && (v.type == ValueType::Path || v.type == ValueType::FloatArray)) {
        floatCursor += v.floatCount;
    }
    if (sg != nullptr && v.stops == sg && v.type == ValueType::Stops) {
        stopCursor += v.stopCount;
    }
    return v;
}

void Animator::gatherOrient(const Timeline& tl, const Track& track, float t, NodeId node) {
    Vec2 d{};
    if (!evalTrackDirection(tl, track, t, d, m_easingPoints)) {
        return;
    }
    OrientDir* entry = nullptr;
    for (OrientDir& o : m_orient) {
        if (o.node == node) {
            entry = &o;
            break;
        }
    }
    if (entry == nullptr) {
        m_orient.push_back(OrientDir{node, Vec2{0, 0}, 0});
        entry = &m_orient.back();
    }
    if (track.prop == prop::Translate) {
        entry->dir = d;
        entry->has = 3;
    } else if (track.prop == prop::TranslateX) {
        entry->dir.x = d.x;
        entry->has = static_cast<uint8_t>(entry->has | 1);
    } else {
        entry->dir.y = d.x;
        entry->has = static_cast<uint8_t>(entry->has | 2);
    }
}

void Animator::checkMarkers(Instance& inst, const LocalState& st, const NodeArena& arena) {
    if (inst.everEvaluated == 0) {
        return;
    }
    const Timeline* tl = timeline(inst.timeline);
    if (tl == nullptr || tl->markers.empty()) {
        return;
    }
    bool wrapped = st.iter != inst.prevIter;
    for (const Marker& marker : tl->markers) {
        bool hit;
        if (!wrapped) {
            hit = segmentContains(inst.prevLocalT, st.localT, marker.start);
        } else {
            bool prevForward = inst.loop == LoopMode::PingPong
                                   ? ((inst.prevIter % 2 == 0) != (inst.speed < 0.0f))
                                   : (inst.speed >= 0.0f);
            bool curForward = inst.loop == LoopMode::PingPong ? ((st.iter % 2 == 0) != (inst.speed < 0.0f))
                                                              : (inst.speed >= 0.0f);
            float tailBoundary = prevForward ? inst.segEnd : inst.segStart;
            float headBoundary = curForward ? inst.segStart : inst.segEnd;
            hit = segmentContains(inst.prevLocalT, tailBoundary, marker.start) ||
                  segmentContains(headBoundary, st.localT, marker.start);
        }
        if (hit && onMarker) {
            // T-260a: called after step 6 (dispatchNotes), not while m_instances is walked.
            m_notes.push_back(Note{.kind = NoteKind::Marker,
                                   .node = inst.selfNode,
                                   .generation = liveGeneration(arena, inst.selfNode),
                                   .prop = prop::kInvalidProp,
                                   .id = inst.id,
                                   .iteration = 0,
                                   .marker = marker.name,
                                   .skipped = 0});
        }
    }
}

void Animator::dispatchNotes(const NodeArena& arena) {
    if (m_dispatchingNotes || m_notes.empty()) {
        return;  // A nested evaluate() leaves its notes to the next one.
    }
    m_dispatchingNotes = true;
    m_clearedWhileDispatching.clear();
    m_notesOut.clear();
    m_notesOut.swap(m_notes);
    for (size_t i = 0; i < m_notesOut.size(); ++i) {
        const Note note = m_notesOut[i];
        if (!noteStillValid(note, arena)) {
            continue;  // Destroyed, stopped or cleared by an earlier callback of this batch.
        }
        switch (note.kind) {
            case NoteKind::TransitionEnd:
                if (onTransitionEnd) {
                    onTransitionEnd(note.node, note.prop);
                }
                break;
            case NoteKind::Iteration:
                if (onIteration) {
                    onIteration(note.id, note.iteration);
                }
                break;
            case NoteKind::Marker:
                if (onMarker) {
                    onMarker(note.id, note.marker);
                }
                break;
        }
    }
    m_dispatchingNotes = false;
}

bool Animator::noteStillValid(const Note& note, const NodeArena& arena) const {
    if (note.node != kInvalidNode &&
        (!arena.alive(note.node) || arena.generation(note.node) != note.generation)) {
        return false;  // The node was destroyed (or its slot reused).
    }
    if (note.kind == NoteKind::TransitionEnd) {
        for (const ClearedTransitions& c : m_clearedWhileDispatching) {
            if (c.node == note.node && note.prop >= c.propLo && note.prop <= c.propHi) {
                return false;  // A callback cleared this node's transitions.
            }
        }
        return true;
    }
    // Iteration / Marker: the instance must still exist, unless step 6 itself just removed it as
    // finished (m_toRemove) -- only a callback's stop() makes the notification stale.
    if (findInstance(note.id) != nullptr) {
        return true;
    }
    return std::find(m_toRemove.begin(), m_toRemove.end(), note.id) != m_toRemove.end();
}

namespace {

/// T-345: the properties whose animation moves a node (transform, position, inset, margins).
bool isMotionProp(PropId p) {
    return (p <= prop::Orientation && p != prop::Opacity && p != prop::Visible && p != prop::AutoOrient) ||
           (p >= prop::MarginTop && p <= prop::MarginLeft) || (p >= prop::Left && p <= prop::Bottom);
}

}  // namespace

bool inMotion(const NodeArena& arena, NodeId node) {
    for (NodeId n = node; n != kInvalidNode; n = arena.parent(n)) {
        if ((arena.common(n).flags2 & kInMotion) != 0) {
            return true;
        }
    }
    return false;
}

void Animator::noteMotion(NodeArena& arena, NodeId node, PropId p) {
    if (!isMotionProp(p) || !arena.alive(node)) {
        return;
    }
    NodeCommon& c = arena.common(node);
    if ((c.flags2 & kInMotion) == 0) {
        c.flags2 |= kInMotion;
        m_motion.push_back(node);
    }
}

void Animator::evaluate(float clock, NodeArena& arena) {
    m_pending.clear();
    m_orient.clear();
    // T-345: last update's moving nodes are static again unless this update moves them; a node
    // that stops is repainted so the Painter can align it to whole pixels.
    m_motionPrev.swap(m_motion);
    m_motion.clear();
    const std::vector<NodeId>& wasMoving = m_motionPrev;
    for (NodeId n : wasMoving) {
        if (arena.alive(n)) {
            arena.common(n).flags2 &= ~static_cast<uint32_t>(kInMotion);
        }
    }
    size_t floatCursor = 0;
    size_t stopCursor = 0;

    // Step 2: Transitions.
    for (size_t i = 0; i < m_transitions.size();) {
        Transition& tr = m_transitions[i];
        float p = (clock - tr.startClock - tr.delay) / tr.duration;
        if (p < 0.0f) {
            ++i;
            continue;
        }
        NodeId node = tr.written();
        PropId prop = tr.prop;
        Value value;
        bool done = false;
        if (p >= 1.0f) {
            value = tr.to;
            done = true;
        } else {
            float e = evalEasing(tr.easing, p, m_easingPoints);
            float* sf = m_scratchFloats.empty() ? nullptr : m_scratchFloats.data() + floatCursor;
            GradientStop* sg = m_scratchStops.empty() ? nullptr : m_scratchStops.data() + stopCursor;
            value = lerpValue(tr.from, tr.to, e, sf, sg);
            if (sf != nullptr && value.floats == sf &&
                (value.type == ValueType::Path || value.type == ValueType::FloatArray)) {
                floatCursor += value.floatCount;
            }
            if (sg != nullptr && value.stops == sg && value.type == ValueType::Stops) {
                stopCursor += value.stopCount;
            }
        }
        m_pending.push_back(Pending{node, prop, value});
        noteMotion(arena, node, prop);  // T-345
        if (done) {
            if (onTransitionEnd) {
                // The owning element (T-218). T-260a: called after step 6 (dispatchNotes).
                m_notes.push_back(Note{.kind = NoteKind::TransitionEnd,
                                       .node = tr.node,
                                       .generation = liveGeneration(arena, tr.node),
                                       .prop = prop,
                                       .id = 0,
                                       .iteration = 0,
                                       .marker = kInvalidString,
                                       .skipped = 0});
            }
            m_transitions[i] = m_transitions.back();
            m_transitions.pop_back();
            continue;
        }
        ++i;
    }

    // Step 3: Instances, ordered by (layer, group, startClock, id) ascending. TimelineGroup members
    // (04.10.1, T-172) sort after the plain instances of their layer so the group wins.
    m_order.resize(m_instances.size());
    for (size_t i = 0; i < m_order.size(); ++i) {
        m_order[i] = static_cast<uint32_t>(i);
    }
    std::sort(m_order.begin(), m_order.end(), [this](uint32_t a, uint32_t b) {
        const Instance& ia = m_instances[a];
        const Instance& ib = m_instances[b];
        if (ia.layer != ib.layer) {
            return ia.layer < ib.layer;
        }
        if (ia.group != ib.group) {
            return ia.group < ib.group;
        }
        if (ia.startClock != ib.startClock) {
            return ia.startClock < ib.startClock;
        }
        return ia.id < ib.id;
    });

    m_toRemove.clear();
    m_toNotify.clear();
    for (uint32_t idx : m_order) {
        Instance& inst = m_instances[idx];
        LocalState st = computeLocalState(inst, clock);
        if (st.skip) {
            continue;
        }
        inst.finished = st.finished ? 1 : 0;
        bool forwardsFill = fillHas(inst.fill, FillMode::Forwards);
        bool doTrackEval = !(st.finished && !forwardsFill);

        if (st.iter > inst.iterCount) {
            // T-255b: a 64-bit counter, so an iteration index saturated at UINT32_MAX (a clock past
            // the uint32 iteration range) ends the loop instead of wrapping it forever; nothing to
            // do per iteration without a listener.
            // T-257b (fuzz_ir out-of-memory finding): a tiny or zero duration or a huge dt crossed
            // hundreds of millions of iterations in one evaluate() and queued a note for each. At
            // most kMaxIterationNotesPerUpdate get a note of their own; the rest of this update's
            // iterations collapse into one note for the last one reached, carrying `skipped`.
            const uint64_t lastIndividual =
                std::min(uint64_t{st.iter}, uint64_t{inst.iterCount} + kMaxIterationNotesPerUpdate);
            for (uint64_t j = uint64_t{inst.iterCount} + 1; onIteration && j <= lastIndividual; ++j) {
                // T-260a: called after step 6 (dispatchNotes).
                m_notes.push_back(Note{.kind = NoteKind::Iteration,
                                       .node = inst.selfNode,
                                       .generation = liveGeneration(arena, inst.selfNode),
                                       .prop = prop::kInvalidProp,
                                       .id = inst.id,
                                       .iteration = static_cast<uint32_t>(j),
                                       .marker = kInvalidString,
                                       .skipped = 0});
            }
            if (onIteration && uint64_t{st.iter} > lastIndividual) {
                const uint64_t skipped = uint64_t{st.iter} - lastIndividual - 1;
                m_notes.push_back(Note{.kind = NoteKind::Iteration,
                                       .node = inst.selfNode,
                                       .generation = liveGeneration(arena, inst.selfNode),
                                       .prop = prop::kInvalidProp,
                                       .id = inst.id,
                                       .iteration = static_cast<uint32_t>(st.iter),
                                       .marker = kInvalidString,
                                       .skipped = static_cast<uint32_t>(std::min<uint64_t>(
                                           skipped, std::numeric_limits<uint32_t>::max()))});
            }
            inst.iterCount = st.iter;
        }

        checkMarkers(inst, st, arena);

        if (doTrackEval) {
            const Timeline* tl = timeline(inst.timeline);
            if (tl != nullptr) {
                for (const Track& track : tl->tracks) {
                    NodeId node;
                    if (track.target == kSelfNode) {
                        node = inst.selfNode;
                    } else if (track.target < inst.localCount) {
                        node = inst.localToArena[track.target];
                    } else {
                        node = kInvalidNode;
                    }
                    if (node == kInvalidNode) {
                        continue;
                    }
                    Value v = evalTrackAdvancing(*tl, track, st.localT, &inst.overrideEasing, floatCursor,
                                                 stopCursor);
                    if (v.isNeutral()) {
                        continue;
                    }
                    scaleElementPx(arena, node, track.prop, v);  // T-345
                    m_pending.push_back(Pending{node, track.prop, v});
                    if (!st.finished) {
                        noteMotion(arena, node, track.prop);  // T-345: a held final frame is static.
                    }
                    if ((track.prop == prop::Translate || track.prop == prop::TranslateX ||
                         track.prop == prop::TranslateY) &&
                        arena.common(node).autoOrient != 0) {
                        gatherOrient(*tl, track, st.localT, node);
                    }
                }
            }
        }

        inst.prevLocalT = st.localT;
        inst.prevIter = st.iter;
        inst.everEvaluated = 1;

        if (st.finished && inst.finishedNotified == 0) {
            inst.finishedNotified = 1;
            m_toNotify.push_back(inst.id);
        }
        if (st.finished && !forwardsFill) {
            m_toRemove.push_back(inst.id);
        }
    }

    // T-228 (31 §31.7.2): auto-orient -- the tangent angle of the node's motion, kept as is while
    // the node does not move.
    for (const OrientDir& o : m_orient) {
        if (o.dir.x * o.dir.x + o.dir.y * o.dir.y <= 1e-12f) {
            continue;
        }
        NodeCommon& c = arena.common(o.node);
        const float deg = std::atan2(o.dir.y, o.dir.x) * (180.0f / 3.14159265358979f);
        if (c.orientPathDeg != deg) {
            c.orientPathDeg = deg;
            arena.markDirty(o.node, static_cast<uint8_t>(kDirtyTransform));
        }
    }

    // Step 4: Overrides.
    for (const Override& ov : m_overrides) {
        Value value = ov.value;
        scaleElementPx(arena, ov.node, ov.prop, value);  // T-345
        m_pending.push_back(Pending{ov.node, ov.prop, value});
        noteMotion(arena, ov.node, ov.prop);
    }
    for (NodeId n : wasMoving) {
        if (arena.alive(n) && (arena.common(n).flags2 & kInMotion) == 0) {
            arena.markDirty(n, static_cast<uint8_t>(kDirtyTransform));  // T-345: snap it now.
        }
    }

    // Step 5: Apply.
    for (const Pending& p : m_pending) {
        NodeKind kind = arena.kind(p.node);
        if (!propertyApplies(kind, p.prop)) {
            // T-257b: IrView::validate rejects a track whose property does not apply to its target
            // (the gate); one that still gets here (an API caller's timeline) is skipped, the same
            // in every build.
            continue;
        }
        // §19.3: a live Track/transition/override on an Element's PropId 16-22 forces is3D = 1
        // (Docs/tasks/T-43c.md). Not cleared here; the next style resolve restores it (19.3).
        if (kind == NodeKind::Element && p.prop >= prop::TranslateZ && p.prop <= prop::Orientation) {
            NodeCommon& common = arena.common(p.node);
            if (common.is3D == 0) {
                common.is3D = 1;
                arena.markDirty(p.node, static_cast<uint8_t>(kDirtyTransform | kDirtyTree));
                arena.markDirtyAncestors(p.node, static_cast<uint8_t>(kDirtyTree),
                                         nearestSpaceRootOrDocRoot(arena, p.node));
            }
        }
        setProperty(arena, p.node, p.prop, p.value);
    }

    // Step 6: Remove finished (non-Forwards-fill) instances, then fire onFinished once for every
    // instance that just became finished (Forwards-fill instances stay in the pool).
    for (uint32_t id : m_toRemove) {
        stop(id);
    }
    // T-260a: the transition-end / iteration / marker notifications steps 2-3 queued, in order.
    dispatchNotes(arena);
    // T-260a: by index, the count taken now and the size re-checked: a callback that re-enters
    // evaluate() (unsupported) refills m_toNotify, which must not make this read out of bounds.
    const size_t notifyCount = m_toNotify.size();
    for (size_t i = 0; i < notifyCount && i < m_toNotify.size(); ++i) {
        if (onFinished) {
            onFinished(m_toNotify[i]);
        }
    }
}

}  // namespace xt
