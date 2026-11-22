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
/// T-869: `inverse` divides (an element's scaled base value into a keyframe's design px).
void scaleElementPx(const NodeArena& arena, NodeId node, PropId p, Value& v, bool inverse = false) {
    if (arena.kind(node) != NodeKind::Element) {
        return;
    }
    const float factor = arena.element(node).style.uiScaleEff;
    if (factor == 1.0f || (inverse && !(factor > 0.0f))) {
        return;
    }
    const float s = inverse ? 1.0f / factor : factor;
    if (p == prop::TranslateX || p == prop::TranslateY) {
        if (v.type == ValueType::Float) {
            v.f *= s;
        } else if (v.type == ValueType::Length && v.len.unit == LengthUnit::Px) {
            v.len.value *= s;
        } else if (v.type == ValueType::Vec2) {
            v.v2.x *= s;  // T-869: the (px, %) form of a base value met by a keyframe; px part only.
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

/// The PropIds 16-22 (TranslateZ..Orientation) whose write makes an Element a 3D layer (19 §19.3).
bool isProp3D(PropId p) { return p >= prop::TranslateZ && p <= prop::Orientation; }

/// T-725: whether `v`, written to `p` (PropId 16-22), holds a 3D component other than zero: TranslateZ,
/// AnchorZ, RotationX/Y; the z of Translate3/Anchor3 (their x/y are the 2D translate and anchor);
/// any component of Orientation. A neutral value writes nothing; a type this does not know counts as 3D.
bool value3DNonZero(PropId p, const Value& v) {
    if (v.isNeutral()) {
        return false;
    }
    if (p == prop::Translate3 || p == prop::Anchor3) {
        return v.type != ValueType::Vec3 || v.v3.z != 0.0f;
    }
    if (p == prop::Orientation) {
        return v.type != ValueType::Vec3 || v.v3.x != 0.0f || v.v3.y != 0.0f || v.v3.z != 0.0f;
    }
    if (v.type == ValueType::Float) {
        return v.f != 0.0f;
    }
    if (v.type == ValueType::Length) {
        return v.len.value != 0.0f;
    }
    return true;
}

/// T-725: whether a PropId 16-22 track can write a 3D component other than zero at any time. A track
/// whose every keyframe is zero (or neutral: the element's own base value, which the style resolve
/// already judges) interpolates to zero under any easing -- e.g. the TranslateZ / RotationX / RotationY
/// tracks of `transform: translateX(..) translateY(..)` keyframes, which 05.5's compose path writes as
/// all ten components. A spatial segment can leave the zero plane through its tangents, so a Vec3
/// track's tangents count too.
bool trackMayWrite3D(const Timeline& tl, const Track& track) {
    if (track.kfOffset > tl.keyframes.size() || track.kfCount > tl.keyframes.size() - track.kfOffset) {
        return true;
    }
    for (uint32_t i = 0; i < track.kfCount; ++i) {
        const Keyframe& kf = tl.keyframes[track.kfOffset + i];
        if (value3DNonZero(track.prop, kf.value)) {
            return true;
        }
        if (track.type == ValueType::Vec3 && kf.hasSpatial != 0) {
            const bool all = track.prop == prop::Orientation;
            if (kf.tanOut.z != 0.0f || kf.tanIn.z != 0.0f ||
                (all &&
                 (kf.tanOut.x != 0.0f || kf.tanOut.y != 0.0f || kf.tanIn.x != 0.0f || kf.tanIn.y != 0.0f))) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TimelineId Animator::registerTimeline(const Timeline* timelinePtr) {
    size_t id = m_timelines.size();
    for (size_t i = 0; i < m_timelines.size(); ++i) {
        if (m_timelines[i] == timelinePtr) {
            id = i;
            break;
        }
    }
    if (id == m_timelines.size()) {
        m_timelines.push_back(timelinePtr);
        m_trackForces3D.emplace_back();
    }
    // T-725: decided once per track (again when a timeline is registered again, e.g. after its
    // owner rebuilt it in place).
    std::vector<uint8_t>& forces = m_trackForces3D[id];
    forces.assign(timelinePtr != nullptr ? timelinePtr->tracks.size() : 0, 0);
    for (size_t k = 0; k < forces.size(); ++k) {
        const Track& track = timelinePtr->tracks[k];
        forces[k] = (isProp3D(track.prop) && trackMayWrite3D(*timelinePtr, track)) ? 1 : 0;
    }
    return static_cast<TimelineId>(id);
}

bool Animator::trackForces3D(TimelineId id, const Timeline& tl, const Track& track) const {
    const size_t k = static_cast<size_t>(&track - tl.tracks.data());
    if (id >= m_trackForces3D.size() || k >= m_trackForces3D[id].size()) {
        return trackMayWrite3D(tl, track);  // not decided at registration: judge it here
    }
    return m_trackForces3D[id][k] != 0;
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

uint32_t Animator::play(const PlayParams& params, double clock) {
    m_changedSinceEvaluate = true;  // T-749
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
    inst.css = params.css;
    inst.reverse = params.reverse;  // T-872
    inst.overrideEasing = params.overrideEasing;

    m_instances.push_back(inst);
    growOrderCapacity();
    growScratchForTimeline(*tl);
    growPendingCapacity(tl->tracks.size());
    return inst.id;
}

void Animator::stop(uint32_t id) {
    m_changedSinceEvaluate = true;  // T-749
    for (size_t i = 0; i < m_instances.size(); ++i) {
        if (m_instances[i].id == id) {
            m_instances[i] = m_instances.back();
            m_instances.pop_back();
            return;
        }
    }
}

void Animator::pause(uint32_t id, double clock) {
    m_changedSinceEvaluate = true;  // T-749
    Instance* inst = findInstance(id);
    if (inst == nullptr) {
        return;
    }
    inst->pausedAt = (clock - inst->startClock - inst->delay) * inst->speed;  // T-870: in double
    inst->paused = 1;
    inst->held = 0;     // T-717
    inst->settled = 0;  // T-783
}

void Animator::resume(uint32_t id, double clock) {
    m_changedSinceEvaluate = true;  // T-749
    Instance* inst = findInstance(id);
    if (inst == nullptr || inst->paused == 0) {
        return;
    }
    if (inst->speed != 0.0f) {
        inst->startClock = clock - inst->delay - inst->pausedAt / inst->speed;
    }
    inst->paused = 0;
    inst->held = 0;     // T-717
    inst->settled = 0;  // T-783
}

void Animator::seek(uint32_t id, float localT, double clock) {
    m_changedSinceEvaluate = true;  // T-749
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
    // Treated as iteration 0: elapsed == frac (T-872: a reversed one runs from segEnd).
    float elapsed = (inst->reverse ? inst->segEnd - localT : localT - inst->segStart) * inst->durationScale;
    // T-783: a paused instance seeked onto the very position it was last evaluated at (a group whose
    // time did not move re-applies it every step) keeps what it applied there.
    const bool samePlace = inst->paused != 0 && inst->everEvaluated != 0 && inst->pausedAt == elapsed &&
                           inst->iterCount == 0 && inst->prevIter == 0 && inst->finished == 0 &&
                           inst->finishedNotified == 0;
    if (!samePlace) {
        inst->settled = 0;
    }
    if (inst->paused) {
        inst->pausedAt = elapsed;
    } else if (inst->speed != 0.0f) {
        inst->startClock = clock - inst->delay - elapsed / inst->speed;
    }
    inst->iterCount = 0;
    inst->prevIter = 0;
    inst->finished = 0;
    inst->finishedNotified = 0;
    inst->held = 0;  // T-717: applied once more even when seeked onto the same stop.
    // Avoid a spurious marker crossing across the jump on the next evaluate().
    inst->prevLocalT = localT;
}

void Animator::setSpeed(uint32_t id, float speed, double clock) {
    m_changedSinceEvaluate = true;  // T-749
    Instance* inst = findInstance(id);
    if (inst == nullptr) {
        return;
    }
    if (!std::isfinite(speed)) {
        speed = 0.0f;  // T-255b: a NaN / infinite rate stops the animation instead of poisoning its clock.
    }
    const double elapsedOld = elapsedAt(*inst, clock);
    inst->speed = speed;
    inst->held = 0;     // T-717
    inst->settled = 0;  // T-783
    if (inst->paused) {
        inst->pausedAt = elapsedOld;
    } else if (speed != 0.0f) {
        inst->startClock = clock - inst->delay - elapsedOld / speed;
    }
}

double Animator::activeElapsed(const Instance& inst, double clock) const {
    if (inst.speed == 0.0f) {
        return 0.0;
    }
    const double elapsed = elapsedAt(inst, clock);
    const float origin = inst.speed < 0.0f ? (inst.segEnd - inst.segStart) * inst.durationScale : 0.0f;
    return (elapsed - origin) / inst.speed;
}

void Animator::setActiveElapsed(Instance& inst, double seconds, double clock) {
    if (inst.speed == 0.0f || !std::isfinite(seconds)) {
        return;
    }
    const float origin = inst.speed < 0.0f ? (inst.segEnd - inst.segStart) * inst.durationScale : 0.0f;
    const double elapsed = origin + seconds * inst.speed;
    if (inst.paused != 0) {
        inst.pausedAt = elapsed;
    } else {
        inst.startClock = clock - inst.delay - elapsed / inst.speed;
    }
}

void Animator::resyncAfterJump(Instance& inst, double clock, bool quietFinish) {
    inst.held = 0;     // T-717: updateParams() / setElapsedSeconds() changed the instance.
    inst.settled = 0;  // T-783
    const LocalState st = computeLocalState(inst, clock);
    if (st.finished) {
        if (quietFinish) {
            inst.finished = 1;
            inst.finishedNotified = 1;
        }
    } else {
        inst.finished = 0;
        inst.finishedNotified = 0;
    }
    // No iteration or marker note for what the jump skipped over.
    inst.prevIter = st.iter;
    inst.iterCount = st.iter;
    inst.prevLocalT = st.localT;
}

bool Animator::updateParams(uint32_t id, const PlayParams& params, double clock) {
    m_changedSinceEvaluate = true;  // T-749
    Instance* inst = findInstance(id);
    const Timeline* tl = inst != nullptr ? timeline(inst->timeline) : nullptr;
    if (tl == nullptr) {
        return false;
    }
    const double seconds = activeElapsed(*inst, clock);
    const bool wasFinished = inst->finished != 0;
    inst->delay = params.delay;
    inst->speed = std::isfinite(params.speed) ? params.speed : 0.0f;
    inst->durationScale = params.durationScale;
    inst->segStart = params.segStart;
    inst->segEnd = params.segEnd < 0.0f ? tl->duration : params.segEnd;
    inst->loop = params.loop;
    inst->iterations = params.loop == LoopMode::Once ? uint16_t{1} : params.iterations;  // As play().
    inst->fill = params.fill;
    inst->layer = params.layer;
    inst->reverse = params.reverse;  // T-872
    inst->overrideEasing = params.overrideEasing;
    setActiveElapsed(*inst, seconds, clock);
    // Already finished and still past the end: no second onFinished.
    resyncAfterJump(*inst, clock, /*quietFinish=*/wasFinished);
    return true;
}

bool Animator::retarget(uint32_t id, TimelineId timelineId) {
    m_changedSinceEvaluate = true;  // T-749
    Instance* inst = findInstance(id);
    const Timeline* next = timeline(timelineId);
    if (inst == nullptr || next == nullptr) {
        return false;
    }
    const Timeline* prev = timeline(inst->timeline);
    if (prev != nullptr && inst->segEnd == prev->duration) {
        inst->segEnd = next->duration;
    }
    inst->timeline = timelineId;
    inst->held = 0;        // T-717
    inst->settled = 0;     // T-783
    dropNeutralBases(id);  // T-869: they name tracks of the previous timeline.
    growScratchForTimeline(*next);
    growPendingCapacity(next->tracks.size());
    return true;
}

double Animator::elapsedSeconds(uint32_t id, double clock) const {
    const Instance* inst = findInstance(id);
    if (inst == nullptr || inst->speed == 0.0f) {
        return 0.0;
    }
    // activeElapsed() in double (T-651): sums and differences of a few floats are exact there.
    const double speed = inst->speed;
    const double elapsed = elapsedAt(*inst, clock);
    const double origin =
        speed < 0.0 ? static_cast<double>((inst->segEnd - inst->segStart) * inst->durationScale) : 0.0;
    return (elapsed - origin) / speed;
}

void Animator::setElapsedSeconds(uint32_t id, double seconds, double clock) {
    m_changedSinceEvaluate = true;  // T-749
    Instance* inst = findInstance(id);
    if (inst == nullptr || inst->speed == 0.0f || !std::isfinite(seconds)) {
        return;
    }
    // setActiveElapsed() in double (T-651): the inverse of elapsedSeconds() at the same clock gives
    // back the saved startClock / pausedAt exactly.
    const double speed = inst->speed;
    const double origin =
        speed < 0.0 ? static_cast<double>((inst->segEnd - inst->segStart) * inst->durationScale) : 0.0;
    const double elapsed = origin + seconds * speed;
    if (inst->paused != 0) {
        inst->pausedAt = elapsed;
    } else {
        inst->startClock = clock - inst->delay - elapsed / speed;
    }
    resyncAfterJump(*inst, clock, /*quietFinish=*/true);
}

bool Animator::isPaused(uint32_t id) const {
    const Instance* inst = findInstance(id);
    return inst != nullptr && inst->paused != 0;
}

bool Animator::isActive(uint32_t id) const {
    const Instance* inst = findInstance(id);
    return inst != nullptr && inst->finished == 0;
}

bool Animator::isFinished(uint32_t id) const {
    const Instance* inst = findInstance(id);
    return inst != nullptr && inst->finished != 0;
}

float Animator::localTime(uint32_t id, double clock) const {
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
    m_changedSinceEvaluate = true;  // T-749
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

namespace {

bool overrideTweenable(ValueType t) {
    return t == ValueType::Float || t == ValueType::Length || t == ValueType::Color || t == ValueType::Vec2;
}

bool sameTweenShape(const Value& a, const Value& b) {
    return a.type == b.type && (a.type != ValueType::Length || a.len.unit == b.len.unit);
}

}  // namespace

void Animator::setOverride(NodeId node, PropId prop, const Value& value, float duration, Easing easing) {
    m_changedSinceEvaluate = true;  // T-749
    uint64_t key = packKey(node, prop);
    auto it = std::lower_bound(m_overrides.begin(), m_overrides.end(), key,
                               [](const Override& o, uint64_t k) { return packKey(o.node, o.prop) < k; });
    const bool existed = it != m_overrides.end() && it->node == node && it->prop == prop;
    if (!existed) {
        it = m_overrides.insert(it, Override{});
        it->node = node;
        it->prop = prop;
        growPendingCapacity(size_t{1});
    }
    Override& ov = *it;
    const bool tween = duration > 0.0f && std::isfinite(duration) && overrideTweenable(value.type);
    if (tween) {
        // Restart from what was last shown (the tweened value mid-tween); with nothing shown yet
        // keep the pending start; with no entry at all the live value is read at evaluate().
        if (existed && ov.shownValid) {
            ov.from = ov.shown;
            ov.fromKnown = true;
        } else if (!existed) {
            ov.fromKnown = false;
        }
        ov.duration = duration;
        ov.easing = easing;
        ov.tweenState = 1;
    } else {
        ov.tweenState = 0;
        ov.fromKnown = false;
        ov.shown = value;
        ov.shownValid = true;
    }
    ov.value = value;
    ov.applied = false;  // T-748: written again at the next evaluate().
}

void Animator::clearOverride(NodeId node, PropId prop) {
    m_changedSinceEvaluate = true;  // T-749
    uint64_t key = packKey(node, prop);
    auto it = std::lower_bound(m_overrides.begin(), m_overrides.end(), key,
                               [](const Override& o, uint64_t k) { return packKey(o.node, o.prop) < k; });
    if (it != m_overrides.end() && it->node == node && it->prop == prop) {
        m_overrides.erase(it);
    }
}

void Animator::clearNode(NodeId node) {
    m_changedSinceEvaluate = true;  // T-749
    if (m_dispatchingNotes) {       // T-260a: drops this node's later TransitionEnd notes of the batch.
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
    m_changedSinceEvaluate = true;  // T-749
    if (m_dispatchingNotes) {       // T-260a: as clearNode().
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

bool Animator::needsEvaluate(const NodeArena& arena) const {
    if (m_changedSinceEvaluate || !m_lastEvaluateIdle || !m_transitions.empty() || !m_notes.empty()) {
        return true;
    }
    // The tests evaluate() makes before leaving a settled instance or override alone.
    for (const Instance& inst : m_instances) {
        if (inst.settled == 0 || inst.heldSkipped == 0) {
            return true;
        }
        const Timeline* tl = timeline(inst.timeline);
        if ((tl != nullptr && heldTargetsTouched(inst, *tl, arena)) ||
            (m_exprSet != nullptr && !inst.exprs.empty() && exprsStale(inst))) {
            return true;
        }
    }
    for (const Override& ov : m_overrides) {
        if (!ov.skipped || ov.tweenState != 0 || targetTouched(ov.node, ov.prop, arena)) {
            return true;
        }
    }
    return false;
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

std::vector<Animator::InstanceInfo> Animator::debugInstances(double clock) const {
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

Animator::LocalState Animator::computeLocalState(const Instance& inst, double clock) const {
    LocalState st;
    // T-870 (59 §59.7): the elapsed time and the iteration split in double; `frac` is rounded.
    const double elapsed = elapsedAt(inst, clock);
    float segLen = (inst.segEnd - inst.segStart) * inst.durationScale;

    if (elapsed < 0.0) {
        bool hasBackward = fillHas(inst.fill, FillMode::Backwards);
        st.localT = (inst.speed < 0.0f) != (inst.reverse != 0) ? inst.segEnd : inst.segStart;  // T-872
        st.iter = 0;
        st.finished = false;
        st.skip = !hasBackward;
        st.beforeStart = hasBackward;  // T-869
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
    const double ratio = elapsed / static_cast<double>(segLen);
    // T-255b: saturated, not a plain cast (undefined past the uint32 range, e.g. a huge clock).
    uint32_t iter = toUint32Sat(std::floor(ratio));
    if (iter > 0 && ratio == static_cast<double>(iter)) {
        iter -= 1;
    }
    st.iter = iter;

    if (inst.iterations != 0 && iter >= inst.iterations) {
        st.finished = true;
        const bool lastForward = inst.loop != LoopMode::PingPong || ((inst.iterations - 1u) % 2u) == 0u;
        bool effectiveForward = (lastForward != (inst.speed < 0.0f)) != (inst.reverse != 0);  // T-872
        st.localT = effectiveForward ? inst.segEnd : inst.segStart;
        return st;
    }

    float frac = static_cast<float>(elapsed - static_cast<double>(iter) * static_cast<double>(segLen));
    if ((inst.loop == LoopMode::PingPong && (iter % 2) == 1) != (inst.reverse != 0)) {  // T-872: reverse
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
                                   const Easing* overrideLinear, size_t& floatCursor, size_t& stopCursor,
                                   const Value* base) {
    float* sf = m_scratchFloats.empty() ? nullptr : m_scratchFloats.data() + floatCursor;
    GradientStop* sg = m_scratchStops.empty() ? nullptr : m_scratchStops.data() + stopCursor;
    Value v = base != nullptr ? evalTrackWithBase(tl, track, t, *base, sf, sg, overrideLinear, m_easingPoints)
                              : evalTrack(tl, track, t, sf, sg, overrideLinear, m_easingPoints);
    if (sf != nullptr && v.floats == sf && (v.type == ValueType::Path || v.type == ValueType::FloatArray)) {
        floatCursor += v.floatCount;
    }
    if (sg != nullptr && v.stops == sg && v.type == ValueType::Stops) {
        stopCursor += v.stopCount;
    }
    return v;
}

void Animator::gatherOrient(const Timeline& tl, const Track& track, float t, NodeId node, DirtyCause cause) {
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
        m_orient.push_back(OrientDir{node, Vec2{0, 0}, 0, cause});
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
            prevForward = prevForward != (inst.reverse != 0);  // T-872
            curForward = curForward != (inst.reverse != 0);
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

/// §19.3: an Element a PropId 16-22 is written to is a 3D layer while the write lasts (T-725: a write
/// whose source can only ever write zero to the 3D components leaves it on its plane; Pending::may3D).
void forceElement3D(NodeArena& arena, NodeId node, DirtyCause cause) {
    NodeCommon& common = arena.common(node);
    if (common.is3D == 0) {
        common.is3D = 1;
        arena.markDirty(node, static_cast<uint8_t>(kDirtyTransform | kDirtyTree), cause);
        arena.markDirtyAncestors(node, static_cast<uint8_t>(kDirtyTree),
                                 nearestSpaceRootOrDocRoot(arena, node), cause);
    }
}

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

void Animator::reassertElement3D(NodeArena& arena) {
    for (const Pending& p : m_pending) {
        if (!p.ended && p.may3D && isProp3D(p.prop) && arena.alive(p.node) &&
            arena.kind(p.node) == NodeKind::Element) {
            forceElement3D(arena, p.node, p.cause);
        }
    }
    // T-717: a held member the last evaluate() left alone wrote nothing this frame, but its 3D
    // values are still on its targets.
    for (const Instance& inst : m_instances) {
        const Timeline* tl = inst.heldSkipped != 0 ? timeline(inst.timeline) : nullptr;
        if (tl == nullptr) {
            continue;
        }
        for (const Track& track : tl->tracks) {
            const NodeId node = trackNode(inst, track);
            if (isProp3D(track.prop) && node != kInvalidNode && arena.alive(node) &&
                arena.kind(node) == NodeKind::Element && trackForces3D(inst.timeline, *tl, track)) {
                forceElement3D(arena, node, inst.css != 0 ? DirtyCause::Animation : DirtyCause::Timeline);
            }
        }
    }
    // T-748: so does a settled override the last evaluate() left alone.
    for (const Override& ov : m_overrides) {
        if (ov.skipped && isProp3D(ov.prop) && value3DNonZero(ov.prop, ov.value) && arena.alive(ov.node) &&
            arena.kind(ov.node) == NodeKind::Element) {
            forceElement3D(arena, ov.node, DirtyCause::HostApi);
        }
    }
}

void Animator::noteHeldMotion(const Instance& inst, const Timeline& tl, float localT, NodeArena& arena) {
    for (const Track& track : tl.tracks) {
        const NodeId node = trackNode(inst, track);
        if (node == kInvalidNode || !isMotionProp(track.prop) || track.kfCount == 0 ||
            track.kfOffset > tl.keyframes.size() || track.kfCount > tl.keyframes.size() - track.kfOffset) {
            continue;
        }
        // Only a track with a neutral key can write nothing here (then it was not "in motion").
        bool neutralKey = false;
        for (uint32_t k = 0; k < track.kfCount && !neutralKey; ++k) {
            neutralKey = tl.keyframes[track.kfOffset + k].value.isNeutral();
        }
        if (neutralKey && evalTrack(tl, track, localT, nullptr, nullptr, &inst.overrideEasing, m_easingPoints)
                              .isNeutral()) {
            continue;
        }
        noteMotion(arena, node, track.prop);
    }
}

NodeId Animator::trackNode(const Instance& inst, const Track& track) {
    if (track.target == kSelfNode) {
        return inst.selfNode;
    }
    return track.target < inst.localCount ? inst.localToArena[track.target] : kInvalidNode;
}

namespace {

/// T-717: the general transform components a PropId writes (one bit per component PropId), so
/// that Translate overlaps TranslateX and Translate3 overlaps TranslateZ; 0 outside 1-22.
uint32_t transformComponents(PropId p) {
    switch (p) {
        case prop::Anchor:
            return (1u << prop::AnchorX) | (1u << prop::AnchorY);
        case prop::Anchor3:
            return (1u << prop::AnchorX) | (1u << prop::AnchorY) | (1u << prop::AnchorZ);
        case prop::Translate:
            return (1u << prop::TranslateX) | (1u << prop::TranslateY);
        case prop::Translate3:
            return (1u << prop::TranslateX) | (1u << prop::TranslateY) | (1u << prop::TranslateZ);
        case prop::Scale:
            return (1u << prop::ScaleX) | (1u << prop::ScaleY);
        default:
            return p >= prop::AnchorX && p <= prop::Orientation ? 1u << p : 0u;
    }
}

/// T-717: whether writing `a` can change what writing `b` set.
bool propsOverlap(PropId a, PropId b) {
    return a == b || (transformComponents(a) & transformComponents(b)) != 0;
}

}  // namespace

bool Animator::targetTouched(NodeId node, PropId prop, const NodeArena& arena) const {
    if (!arena.alive(node)) {
        return true;
    }
    const uint32_t flags = arena.common(node).flags2;
    if ((flags & kAnimValueStale) != 0) {
        return true;
    }
    if ((flags & kAnimWriteQueued) != 0) {
        // A write queued before this one would have been replaced by it.
        for (const Pending& p : m_pending) {
            if (p.node == node && propsOverlap(p.prop, prop)) {
                return true;
            }
        }
    }
    return false;
}

bool Animator::heldTargetsTouched(const Instance& inst, const Timeline& tl, const NodeArena& arena) const {
    for (const Track& track : tl.tracks) {
        const NodeId node = trackNode(inst, track);
        if (node != kInvalidNode && targetTouched(node, track.prop, arena)) {
            return true;
        }
    }
    return false;
}

void Animator::evaluate(double clock, NodeArena& arena) {
    // T-749 (needsEvaluate()): a call a callback of this evaluate() makes sets the flag again.
    m_changedSinceEvaluate = false;
    bool idle = m_transitions.empty();
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
        float p = static_cast<float>(clock - tr.startClock - tr.delay) / tr.duration;  // T-870
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
        // T-725: a transition between two values without a 3D component never makes its node 3D.
        const bool may3D = !isProp3D(prop) || value3DNonZero(prop, tr.from) || value3DNonZero(prop, tr.to);
        m_pending.push_back(Pending{node, prop, value, done, may3D, DirtyCause::Transition});
        arena.common(node).flags2 |= kAnimWriteQueued;  // T-717: a later held member writes over it.
        noteMotion(arena, node, prop);                  // T-345
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
    m_heldTargets.clear();
    m_evaluatedInstances = 0;
    m_exprEvaluated = 0;  // T-776
    for (uint32_t idx : m_order) {
        Instance& inst = m_instances[idx];
        LocalState st = computeLocalState(inst, clock);
        if (st.skip) {
            idle = false;  // T-749: in its delay -- it starts with time alone.
            continue;
        }
        inst.finished = st.finished ? 1 : 0;
        bool forwardsFill = fillHas(inst.fill, FillMode::Forwards);
        bool doTrackEval = !(st.finished && !forwardsFill);
        // T-717 (10 §10.16 W4): a `@timeline` member paused at its end stop -- the segment end (a
        // forward play's end) or start (a reverse play's end) -- whose fill keeps that value is a
        // static hold: its values are applied on the frame it gets there and then only again when a
        // target node was written or restyled since (kAnimValueStale), never "in motion".
        // T-748 (23 §23.ag): so is a plain instance (a CSS animation) that finished and holds its
        // last value through a `forwards` fill.
        const bool atStop = (inst.group == 0 && st.finished && forwardsFill) ||
                            (inst.group != 0 && inst.paused != 0 &&
                             ((st.localT == inst.segEnd && forwardsFill) ||
                              (st.localT == inst.segStart && fillHas(inst.fill, FillMode::Backwards))));
        const bool wasHeld = atStop && inst.held != 0;
        inst.held = atStop ? 1 : 0;
        // T-783: a paused instance -- a group member wherever the group stands (paused mid-way by a
        // marker, held after a seekTimeline(), a step that moved no time), a paused `<anim sync>`
        // clip or CSS animation -- shows the same values for as long as nothing moves or changes it
        // (every such call clears `settled`) and nothing writes or restyles a target: the same hold.
        // `held` keeps deciding "in motion" (T-717), so pixel snapping is exactly as before.
        const bool stationary = atStop || inst.paused != 0;
        const bool wasSettled = stationary && inst.settled != 0;
        inst.settled = stationary ? 1 : 0;
        inst.heldSkipped = 0;
        // T-776 (58 §58.5): the member's programs run when its time, seed or a read input moved.
        bool exprStale = m_exprSet != nullptr && !inst.exprs.empty() && exprsStale(inst);
        const Timeline* heldTl = wasSettled && doTrackEval ? timeline(inst.timeline) : nullptr;
        if (heldTl != nullptr && !heldTargetsTouched(inst, *heldTl, arena)) {
            // T-776: a held member whose programs must run runs them and writes only when an output
            // changed (an unchanged value written again would move the visual generation, 23 §23.ag).
            const bool exprChanged = exprStale && probeHeldExprs(inst, *heldTl, st.localT);
            if (exprStale) {
                exprsRan(inst);
                exprStale = false;
            }
            if (!exprChanged) {
                doTrackEval = false;
                inst.heldSkipped = 1;
                if (!st.finished && !wasHeld) {
                    noteHeldMotion(inst, *heldTl, st.localT, arena);  // T-783: "in motion" as when it wrote.
                }
            }
        }

        // T-749: an instance whose end was already reported (a `forwards` hold) crosses no more
        // iterations -- its raw iteration index keeps growing with the clock, which queued an
        // AnimationIteration for every segment length of time after the end.
        const bool endReported = st.finished && inst.finishedNotified != 0;
        if (st.iter > inst.iterCount && !endReported) {
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
            const DirtyCause instCause =
                inst.css != 0 ? DirtyCause::Animation : DirtyCause::Timeline;  // T-747
            const Timeline* tl = timeline(inst.timeline);
            if (tl != nullptr) {
                ++m_evaluatedInstances;
                const bool withExprs = m_exprSet != nullptr && !inst.exprs.empty();  // T-776
                size_t exprCursor = 0;
                for (const Track& track : tl->tracks) {
                    const NodeId node = trackNode(inst, track);
                    if (node == kInvalidNode) {
                        continue;
                    }
                    if (stationary) {
                        m_heldTargets.push_back(node);  // T-717: its stale bit is cleared after step 5.
                    }
                    const size_t trackIndex = static_cast<size_t>(&track - tl->tracks.data());
                    // T-869 (12 §12.3): Neutral keys read the base value (none in a Backwards delay).
                    Value base =
                        st.beforeStart ? Value::neutral() : neutralBase(inst, *tl, track, node, arena);
                    scaleElementPx(arena, node, track.prop, base, /*inverse=*/true);
                    Value v = evalTrackAdvancing(*tl, track, st.localT, &inst.overrideEasing, floatCursor,
                                                 stopCursor, base.isNeutral() ? nullptr : &base);
                    if (v.isNeutral()) {
                        continue;
                    }
                    // T-776: a program's output replaces its component, before the px scaling.
                    bool exprWrote = false;
                    if (withExprs) {
                        applyExprs(inst, trackIndex, v, exprStale, exprCursor);
                        exprWrote = exprCursor > 0 && inst.exprs[exprCursor - 1].track == trackIndex;
                    }
                    scaleElementPx(arena, node, track.prop, v);  // T-345
                    const bool may3D = !isProp3D(track.prop) || exprWrote ||
                                       trackForces3D(inst.timeline, *tl, track);  // T-725
                    m_pending.push_back(Pending{node, track.prop, v, false, may3D, instCause});
                    arena.common(node).flags2 |= kAnimWriteQueued;  // T-717, as in step 2.
                    // T-345: a held final frame is static; T-717: so is a held member re-applying.
                    if (!st.finished && !wasHeld) {
                        noteMotion(arena, node, track.prop);
                    }
                    if ((track.prop == prop::Translate || track.prop == prop::TranslateX ||
                         track.prop == prop::TranslateY) &&
                        arena.common(node).autoOrient != 0) {
                        gatherOrient(*tl, track, st.localT, node, instCause);
                    }
                }
                if (withExprs && exprStale) {
                    exprsRan(inst);  // T-776
                }
            }
        }

        inst.prevLocalT = st.localT;
        inst.prevIter = st.iter;
        inst.everEvaluated = 1;
        idle = idle && inst.heldSkipped != 0;  // T-749: left alone (settled, untouched)

        if (st.finished && inst.finishedNotified == 0) {
            inst.finishedNotified = 1;
            m_toNotify.push_back(inst.id);
        }
        if (st.finished && !forwardsFill) {
            m_toRemove.push_back(inst.id);
            restoreCascadeValues(inst, arena);  // T-869: a CSS animation's general properties.
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
            arena.markDirty(o.node, static_cast<uint8_t>(kDirtyTransform), o.cause);
        }
    }

    // Step 4: Overrides (T-545: a tweening one writes the interpolated value).
    for (Override& ov : m_overrides) {
        // T-748 (23 §23.ag): a sticky override that settled (no tween) and was applied writes
        // nothing more while its target is not written or restyled: the same value written again
        // would mark the node dirty and move the visual generation every frame. Its node stays "in
        // motion" as when it writes (no pixel snapping change).
        ov.skipped = ov.applied && ov.tweenState == 0 && !targetTouched(ov.node, ov.prop, arena);
        if (ov.skipped) {
            noteMotion(arena, ov.node, ov.prop);
            continue;
        }
        idle = false;  // T-749
        Value value = ov.value;
        if (ov.tweenState == 1) {
            ov.startClock = clock;
            if (!ov.fromKnown && arena.alive(ov.node) && propertyApplies(arena.kind(ov.node), ov.prop) &&
                (!isEffectProp(ov.prop) ||
                 ((static_cast<uint32_t>(ov.prop) >> 8) & 0xFu) < arena.common(ov.node).effectCount)) {
                ov.from = getProperty(arena, ov.node, ov.prop);
                ov.fromKnown = true;
            }
            ov.tweenState = ov.fromKnown && sameTweenShape(ov.from, ov.value) ? 2 : 0;
        }
        if (ov.tweenState == 2) {
            const float p = static_cast<float>(clock - ov.startClock) / ov.duration;  // T-870
            if (p >= 1.0f) {
                ov.tweenState = 0;
            } else if (p > 0.0f) {
                value =
                    lerpValue(ov.from, ov.value, evalEasing(ov.easing, p, m_easingPoints), nullptr, nullptr);
            } else {
                value = ov.from;
            }
        }
        ov.shown = value;
        ov.shownValid = true;
        scaleElementPx(arena, ov.node, ov.prop, value);                             // T-345
        const bool may3D = !isProp3D(ov.prop) || value3DNonZero(ov.prop, value) ||  // T-725
                           value3DNonZero(ov.prop, ov.value) ||
                           (ov.fromKnown && value3DNonZero(ov.prop, ov.from));
        m_pending.push_back(Pending{ov.node, ov.prop, value, false, may3D, DirtyCause::HostApi});
        noteMotion(arena, ov.node, ov.prop);
        ov.applied = ov.tweenState == 0;  // T-748
    }
    for (NodeId n : wasMoving) {
        if (arena.alive(n) && (arena.common(n).flags2 & kInMotion) == 0) {
            arena.markDirty(n, static_cast<uint8_t>(kDirtyTransform),
                            arena.lastCause());  // T-345: snap it now.
            idle = false;                        // T-749
        }
    }
    // T-749: the same nodes in motion as last time (m_motion holds each node once); nothing finished,
    // removed or notified below.
    m_lastEvaluateIdle = idle && m_motion.size() == wasMoving.size() && m_toRemove.empty() &&
                         m_toNotify.empty() && m_notes.empty();

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
        // T-725: unless its source can only write zero to the 3D components (Pending::may3D).
        if (kind == NodeKind::Element && p.may3D && isProp3D(p.prop)) {
            forceElement3D(arena, p.node, p.cause);
        }
        setProperty(arena, p.node, p.prop, p.value, p.cause);
    }
    // T-717: what was written here is what the Animator applied; a held member re-applies only
    // after a later write or restyle marks its target again.
    constexpr uint32_t kAnimBits = kAnimValueStale | kAnimWriteQueued;
    for (const Pending& p : m_pending) {
        arena.common(p.node).flags2 &= ~kAnimBits;
    }
    for (NodeId n : m_heldTargets) {
        if (arena.alive(n)) {
            arena.common(n).flags2 &= ~kAnimBits;
        }
    }
    neutralBasesApplied(arena);  // T-869

    // Step 6: Remove finished (non-Forwards-fill) instances, then fire onFinished once for every
    // instance that just became finished (Forwards-fill instances stay in the pool).
    for (uint32_t id : m_toRemove) {
        stop(id);
    }
    dropNeutralBases(0);  // T-869: those of instances no longer in the pool.
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
