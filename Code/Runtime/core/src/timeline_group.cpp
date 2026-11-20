#include "timeline_group.h"

#include <algorithm>
#include <cmath>

#include "dispatch_guard.h"
#include "event_dispatcher.h"
#include "trigger_runtime.h"
#include "xt/document.h"
#include "xt/node_arena.h"
#include "xt/property.h"

/// `@timeline` group runtime: Document's TimelineGroup API and its per-frame step. See
/// Docs/design/04-timeline.md §4.10.1-4.10.2, 13-api.md §13.8.2, 08-events.md §8.6,
/// Docs/tasks/T-172.md.
///
/// Model: a group owns one shared clock (`t`, seconds). While a group is active every member is an
/// Animator instance of its compiled `@keyframes <timeline>__<path>` timeline, created paused and
/// seeked to `(t - delay) / duration` every frame (clamped, so a member shows its first key before
/// its delay and its last key after its end). Members are played with `PlayParams::group = 1`, so
/// they are evaluated after the element's own CSS animations and win on shared properties.

namespace xt {

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// Whether a marker at `t` is crossed going from `from` to `to` (either direction).
bool markerHit(float t, float from, float to, bool inclusiveFrom) {
    if (to >= from) {
        return (t > from || (inclusiveFrom && t == from)) && t <= to;
    }
    return (t < from || (inclusiveFrom && t == from)) && t >= to;
}

/// T-774: at most this many self-control cuts per group and update (a jump back that is crossed
/// again in the same step); the rest of the step then plays through as before.
constexpr int kMaxSelfControlCuts = 64;

NodeId groupScope(const std::vector<NodeId>& docNodes, const TimelineGroupRuntime& g) {
    return g.scopeElement < docNodes.size() ? docNodes[g.scopeElement] : kInvalidNode;
}

/// T-774: the first marker (in the travel direction) crossed from `from` to `to` that a trigger
/// controls the group from; false when none is.
bool firstSelfMarker(TriggerRuntime& triggers, const TimelineGroupRuntime& g, NodeId scope, float from,
                     float to, bool inclusiveFrom, float& at) {
    bool found = false;
    for (const IrTimelineGroupMarker& m : g.markers) {
        if (!markerHit(m.time, from, to, inclusiveFrom) ||
            (found && (to >= from ? m.time >= at : m.time <= at)) ||
            !triggers.selfControls(g.name, TimelineEventKind::Marker, m.name, scope)) {
            continue;
        }
        at = m.time;
        found = true;
    }
    return found;
}

/// T-774: flags the last queued event of that marker (or end) of group `index` as self-applied.
void markSelfApplied(std::vector<TimelinePendingEvent>& pending, size_t index, TimelineEventKind kind,
                     StringId marker) {
    for (size_t i = pending.size(); i-- > 0;) {
        TimelinePendingEvent& ev = pending[i];
        if (ev.group == index && ev.kind == kind && ev.marker == marker) {
            ev.selfApplied = true;
            return;
        }
    }
}

/// T-776 (58 §58.5): gives every member instance of `g` (group `index`) a new per-play seed.
void reseedGroup(Animator& animator, TimelineExprSet& exprs, const TimelineGroupRuntime& g, size_t index) {
    if (exprs.empty()) {
        return;
    }
    const uint32_t seed = exprs.drawPlaySeed(static_cast<uint32_t>(index), g.name);
    for (uint32_t id : g.memberInstances) {
        if (id != 0) {
            animator.setPlaySeed(id, seed);
        }
    }
}

/// T-776: attaches the TEXP programs of member `member` of group `index` to its instance `id`.
void attachMemberExprs(Animator& animator, TimelineExprSet& exprs, size_t index, size_t member, uint32_t id,
                       bool scene) {
    if (exprs.empty()) {
        return;
    }
    exprs.setMemberScene(static_cast<uint32_t>(index), static_cast<uint32_t>(member), scene);
    for (uint32_t e : exprs.exprsOf(static_cast<uint32_t>(index), static_cast<uint32_t>(member))) {
        animator.attachExpression(id, e);
    }
}

}  // namespace

void Document::loadTimelineGroups() {
    m_timelineGroups->groups.clear();
    // T-776: the TEXP programs (none without TGRP) and what their inputs read.
    TimelineExprSet& exprs = m_timelineGroups->exprs;
    exprs.load(m_ir);
    exprs.bind(&m_arena, &m_docElementNodes, m_bindings.get(), &m_animator);
    m_animator.setExpressions(exprs.empty() ? nullptr : &exprs);
    const TimelineGroupChunk& chunk = m_ir.timelineGroups();
    if (!chunk.present()) {
        return;
    }
    for (const IrTimelineGroup& g : chunk.groups()) {
        TimelineGroupRuntime rt;
        rt.name = g.name;
        rt.scopeElement = g.scopeElement;
        rt.duration = g.duration;
        rt.markers = chunk.markers().subspan(g.markerOffset, g.markerCount);
        rt.sounds = chunk.sounds().subspan(g.soundOffset, g.soundCount);
        rt.members = chunk.members().subspan(g.memberOffset, g.memberCount);
        m_timelineGroups->groups.push_back(std::move(rt));
    }
}

int Document::findTimelineGroup(std::string_view scopePath, std::string_view name) const {
    NodeId scope = kInvalidNode;
    if (scopePath.empty()) {
        scope = m_root;
    } else {
        scope = byId(scopePath).node;
    }
    if (scope == kInvalidNode) {
        return -1;
    }
    const std::vector<TimelineGroupRuntime>& groups = m_timelineGroups->groups;
    for (size_t i = 0; i < groups.size(); ++i) {
        const TimelineGroupRuntime& g = groups[i];
        if (g.scopeElement >= m_docElementNodes.size() || m_docElementNodes[g.scopeElement] != scope) {
            continue;
        }
        if (m_ir.string(g.name) == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Document::activateTimelineGroup(size_t index) {
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    if (g.active) {
        return;
    }
    g.active = true;
    g.memberInstances.assign(g.members.size(), 0);
    g.memberIsScene.assign(g.members.size(), 0);
    for (size_t i = 0; i < g.members.size(); ++i) {
        const IrTimelineGroupMember& m = g.members[i];
        NodeId node = m.element < m_docElementNodes.size() ? m_docElementNodes[m.element] : kInvalidNode;
        if (node == kInvalidNode || !m_arena.alive(node) || m_animator.timeline(m.timeline) == nullptr) {
            continue;
        }
        if (m_arena.kind(node) == NodeKind::Element) {
            // T-190 (11.9): an inline <svg>'s own scene timeline plays through its node table, in
            // seconds of group time.
            const ElementData& data = m_arena.element(node);
            if (data.scene != kInvalidScene && data.sceneInstanceIndex < m_instances.size() &&
                sceneTimeline(data.scene) == m.timeline) {
                const SceneInstance& inst = m_instances[data.sceneInstanceIndex];
                PlayParams params;
                params.timeline = m.timeline;
                params.localToArena = inst.localToArena.data();
                params.localCount = static_cast<uint32_t>(inst.localToArena.size());
                params.selfNode = node;
                params.fill = FillMode::Both;
                params.loop = LoopMode::Once;
                params.iterations = 1;
                params.group = 1;
                uint32_t id = m_animator.play(params, m_clock);
                m_animator.pause(id, m_clock);
                g.memberInstances[i] = id;
                g.memberIsScene[i] = 1;
                attachMemberExprs(m_animator, m_timelineGroups->exprs, index, i, id, true);  // T-776
                continue;
            }
        }
        PlayParams params;
        params.timeline = m.timeline;
        params.selfNode = node;
        params.durationScale = m.duration > 0 ? m.duration : 1.0f;
        params.fill = FillMode::Both;
        params.loop = LoopMode::Once;
        params.iterations = 1;
        params.group = 1;
        uint32_t id = m_animator.play(params, m_clock);
        m_animator.pause(id, m_clock);
        g.memberInstances[i] = id;
        attachMemberExprs(m_animator, m_timelineGroups->exprs, index, i, id, false);  // T-776
    }
    reseedGroup(m_animator, m_timelineGroups->exprs, g, index);  // T-776: the activation's draw.
    g.restores.clear();
    g.restoresCaptured = false;

    // 4.10.2: `<anim sync>` clips inside the scope follow the group's time; T-190: so do the
    // timeline groups of a `<use sync>` symbol instance inside it. The walk does not enter another
    // group's scope (a nested instance's clips follow that instance's own groups).
    g.syncClips.clear();
    NodeId scope =
        g.scopeElement < m_docElementNodes.size() ? m_docElementNodes[g.scopeElement] : kInvalidNode;
    if (scope == kInvalidNode || !m_arena.alive(scope)) {
        return;
    }
    g.mediaClips.clear();
    std::vector<TimelineSyncClip> clips;
    std::vector<NodeId> mediaClips;
    m_arena.traversePreorder(scope, [this, scope, &clips, &mediaClips](NodeId n) {
        if (m_arena.kind(n) != NodeKind::Element) {
            return true;
        }
        ElementData& data = m_arena.element(n);
        // T-191 (04 §4.10.2): a `sync` video / sequence follows the group too.
        if (data.externalNode != kInvalidNode) {
            const ExternalNodeData& x = m_arena.external(data.externalNode);
            if (x.sync != 0 &&
                (x.source == ExternalSourceKind::Video || x.source == ExternalSourceKind::Sequence)) {
                mediaClips.push_back(data.externalNode);
            }
            return true;
        }
        if (n != scope) {
            std::vector<uint32_t> nested;
            const std::vector<TimelineGroupRuntime>& all = m_timelineGroups->groups;
            for (size_t k = 0; k < all.size(); ++k) {
                if (all[k].scopeElement != 0 && all[k].scopeElement < m_docElementNodes.size() &&
                    m_docElementNodes[all[k].scopeElement] == n) {
                    nested.push_back(static_cast<uint32_t>(k));
                }
            }
            if (!nested.empty()) {
                bool nestedSync = false;
                for (const auto& entry : data.attributes) {
                    if (string(entry.first) == "sync") {
                        nestedSync = string(entry.second) != "false";
                        break;
                    }
                }
                if (nestedSync) {
                    TimelineSyncClip clip;
                    clip.element = n;
                    clip.nestedGroups = std::move(nested);
                    clips.push_back(std::move(clip));
                }
                return false;  // Another scope: its own groups drive what is inside.
            }
        }
        if (data.scene == kInvalidScene || data.sceneInstanceIndex >= m_instances.size() ||
            string(data.tag) != "anim") {
            return true;
        }
        bool sync = false;
        for (const auto& entry : data.attributes) {
            if (string(entry.first) == "sync") {
                sync = string(entry.second) != "false";
                break;
            }
        }
        if (!sync) {
            return true;
        }
        TimelineSyncClip clip;
        clip.element = n;
        if (data.defaultAnimation != 0 && m_animator.isActive(data.defaultAnimation)) {
            clip.instance = data.defaultAnimation;
            m_animator.pause(clip.instance, m_clock);
        } else {
            const SceneInstance& inst = m_instances[data.sceneInstanceIndex];
            PlayParams params;
            params.timeline = sceneTimeline(data.scene);
            if (m_animator.timeline(params.timeline) == nullptr) {
                return true;
            }
            params.localToArena = inst.localToArena.data();
            params.localCount = static_cast<uint32_t>(inst.localToArena.size());
            params.selfNode = n;
            params.fill = FillMode::Both;
            clip.instance = m_animator.play(params, m_clock);
            m_animator.pause(clip.instance, m_clock);
            clip.created = true;
        }
        clips.push_back(clip);
        return true;
    });
    m_timelineGroups->groups[index].syncClips = std::move(clips);
    m_timelineGroups->groups[index].mediaClips = std::move(mediaClips);
}

void Document::releaseTimelineGroup(size_t index) {
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    for (size_t i = 0; i < g.memberInstances.size(); ++i) {
        uint32_t id = g.memberInstances[i];
        if (id == 0) {
            continue;
        }
        const IrTimelineGroupMember& m = g.members[i];
        m_animator.stop(id);
        NodeId node = m_docElementNodes[m.element];
        if (node == kInvalidNode || !m_arena.alive(node) || m_arena.kind(node) != NodeKind::Element ||
            (i < g.memberIsScene.size() && g.memberIsScene[i] != 0)) {
            continue;  // T-190: a scene member keeps its last frame (like an `<anim>` clip).
        }
        // The member wrote the element's properties directly and the cascade only rewrites a
        // property whose styled value changes, so put the styled value back explicitly.
        const ElementData& data = m_arena.element(node);
        if (const Timeline* tl = m_animator.timeline(m.timeline)) {
            for (const Track& track : tl->tracks) {
                if (track.prop < prop::kElementPropCount && data.styleResolvedOnce != 0) {
                    setProperty(m_arena, node, track.prop, data.style.base[track.prop], DirtyCause::Timeline);
                }
            }
        }
        // 19.3: a 3D PropId track may have forced is3D; a re-resolve restores it.
        m_arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle), DirtyCause::Timeline);
    }
    for (const TimelineRestore& r : g.restores) {
        uint32_t effectIndex = (static_cast<uint32_t>(r.prop) >> 8) & 0xFu;
        if (m_arena.alive(r.node) && effectIndex < m_arena.common(r.node).effectCount) {
            setProperty(m_arena, r.node, r.prop, r.value, DirtyCause::Timeline);
        }
    }
    g.restores.clear();
    g.memberInstances.clear();
    g.memberIsScene.clear();
    std::vector<TimelineSyncClip> clips = std::move(g.syncClips);
    g.syncClips.clear();
    for (const TimelineSyncClip& clip : clips) {
        for (uint32_t nested : clip.nestedGroups) {
            if (nested != index && nested < m_timelineGroups->groups.size() &&
                m_timelineGroups->groups[nested].active && !m_timelineGroups->groups[nested].playing) {
                releaseTimelineGroup(nested);
            }
        }
        if (clip.instance == 0) {
            continue;
        }
        if (clip.created) {
            m_animator.stop(clip.instance);
        } else {
            m_animator.resume(clip.instance, m_clock);
        }
    }
    for (NodeId ext : g.mediaClips) {
        if (m_arena.alive(ext) && m_arena.kind(ext) == NodeKind::External) {
            releaseExternalClip(ext);  // T-191: a video the group started is paused.
        }
    }
    g.mediaClips.clear();
    g.active = false;
    g.playing = false;
}

void Document::applyTimelineGroupTime(size_t index) {
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    for (size_t i = 0; i < g.memberInstances.size(); ++i) {
        uint32_t id = g.memberInstances[i];
        if (id == 0) {
            continue;
        }
        const IrTimelineGroupMember& m = g.members[i];
        if (i < g.memberIsScene.size() && g.memberIsScene[i] != 0) {
            // T-190: a scene member runs in seconds; past its last key it holds (fill both).
            const Timeline* tl = m_animator.timeline(m.timeline);
            float end = tl != nullptr ? tl->duration : 0.0f;
            m_animator.seek(id, clampf(g.t, 0.0f, end), m_clock);
            m_animator.setGroupTime(id, g.t);  // T-776
            continue;
        }
        float u;
        if (m.duration > 0) {
            u = clampf((g.t - m.delay) / m.duration, 0.0f, 1.0f);
        } else {
            u = g.t >= m.delay ? 1.0f : 0.0f;
        }
        m_animator.seek(id, u, m_clock);
        m_animator.setGroupTime(id, g.t);  // T-776: the expressions sample the group's time.
    }
    // 4.10.2: local clip time = (t - clipStart) * clipRate (T-190: `clip-start`/`clip-rate` on the
    // clip element, 04 §4.10.2 / 28 §28.2).
    std::vector<TimelineSyncClip> clips = g.syncClips;  // A nested group's activation may reallocate.
    const float groupT = g.t;
    for (const TimelineSyncClip& clip : clips) {
        float clipStart = 0.0f;
        float clipRate = 1.0f;
        if (clip.element != kInvalidNode && m_arena.alive(clip.element) &&
            m_arena.kind(clip.element) == NodeKind::Element) {
            const ElementData& data = m_arena.element(clip.element);
            clipStart = data.style.clipStart;
            clipRate = data.style.clipRate > 0.0f ? data.style.clipRate : 1.0f;
        }
        const float localT = (groupT - clipStart) * clipRate;
        for (uint32_t nested : clip.nestedGroups) {
            if (nested == index || nested >= m_timelineGroups->groups.size()) {
                continue;
            }
            if (!m_timelineGroups->groups[nested].active) {
                activateTimelineGroup(nested);
            }
            TimelineGroupRuntime& ng = m_timelineGroups->groups[nested];
            ng.playing = false;
            ng.t = clampf(localT, 0.0f, ng.duration);
            ng.lastClock = m_clock;
            applyTimelineGroupTime(nested);
        }
        if (clip.instance == 0) {
            continue;
        }
        const Timeline* tl = nullptr;
        for (const Animator::InstanceInfo& info : m_animator.debugInstances(m_clock)) {
            if (info.id == clip.instance) {
                tl = m_animator.timeline(info.timeline);
                break;
            }
        }
        float local = tl != nullptr ? clampf(localT, 0.0f, tl->duration) : localT;
        m_animator.seek(clip.instance, local, m_clock);
    }
    for (NodeId ext : g.mediaClips) {
        if (m_arena.alive(ext) && m_arena.kind(ext) == NodeKind::External) {
            syncExternalClip(ext, g.t, g.playing);  // T-191.
        }
    }
}

void Document::queueTimelineMarkers(size_t index, float from, float to, bool inclusiveFrom) {
    const TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    for (const IrTimelineGroupMarker& m : g.markers) {
        if (markerHit(m.time, from, to, inclusiveFrom)) {
            m_timelineGroups->pending.push_back(
                TimelinePendingEvent{static_cast<uint32_t>(index), TimelineEventKind::Marker, m.name});
        }
    }
}

Status Document::playTimeline(std::string_view scopePath, std::string_view name, const PlayOptions& options) {
    DispatchGuard dispatch(m_context);  // T-260a: `sync` video clips send host requests.
    int found = findTimelineGroup(scopePath, name);
    if (found < 0) {
        return Status::kNotFound;
    }
    if (!(options.rate > 0.0f)) {
        return Status::kInvalidArgument;
    }
    size_t index = static_cast<size_t>(found);
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    float start = options.reverse ? g.duration : 0.0f;
    bool fromMarker = !options.fromMarker.empty();
    if (fromMarker) {
        bool markerFound = false;
        for (const IrTimelineGroupMarker& m : g.markers) {
            if (m_ir.string(m.name) == options.fromMarker) {
                start = clampf(m.time, 0.0f, g.duration);
                markerFound = true;
                break;
            }
        }
        if (!markerFound) {
            return Status::kNotFound;
        }
    }
    const bool wasActive = g.active;
    activateTimelineGroup(index);
    TimelineGroupRuntime& grp = m_timelineGroups->groups[index];
    if (wasActive) {
        reseedGroup(m_animator, m_timelineGroups->exprs, grp, index);  // T-776: a new play, a new draw.
    }
    grp.reverse = options.reverse;
    grp.loop = options.loop && !options.once;
    grp.hold = options.hold;
    grp.rate = options.rate;
    grp.t = start;
    grp.lastClock = m_clock;
    grp.playing = true;
    grp.startInclusive = !fromMarker;
    applyTimelineGroupTime(index);
    return Status::kOk;
}

Status Document::stopTimeline(std::string_view scopePath, std::string_view name) {
    DispatchGuard dispatch(m_context);  // T-260a: as playTimeline().
    int found = findTimelineGroup(scopePath, name);
    if (found < 0) {
        return Status::kNotFound;
    }
    releaseTimelineGroup(static_cast<size_t>(found));
    return Status::kOk;
}

Status Document::seekTimeline(std::string_view scopePath, std::string_view name, float t) {
    DispatchGuard dispatch(m_context);  // T-260a: as playTimeline().
    int found = findTimelineGroup(scopePath, name);
    if (found < 0) {
        return Status::kNotFound;
    }
    size_t index = static_cast<size_t>(found);
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    // T-255 / T-255b: a NaN time (clampf passes NaN through) seeks to the start; +Inf clamps to the
    // group's end and -Inf to 0.
    float target = std::isnan(t) ? 0.0f : clampf(t, 0.0f, g.duration);
    if (!g.active) {
        activateTimelineGroup(index);
        TimelineGroupRuntime& fresh = m_timelineGroups->groups[index];
        fresh.t = target;
        fresh.playing = false;
        fresh.lastClock = m_clock;
        applyTimelineGroupTime(index);
        return Status::kOk;
    }
    // Markers crossed in the play direction fire; a seek against it is silent.
    bool forward = g.reverse ? target < g.t : target > g.t;
    if (forward) {
        queueTimelineMarkers(index, g.t, target, g.startInclusive);
    }
    g.startInclusive = false;
    g.t = target;
    g.lastClock = m_clock;
    applyTimelineGroupTime(index);
    return Status::kOk;
}

void Document::setTimelineGroupPaused(size_t index, bool paused) {
    DispatchGuard dispatch(m_context);  // A `sync` video clip is paused / played with the group.
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    if (!g.active) {
        return;  // Nothing playing or held: nothing to pause or resume.
    }
    if (paused) {
        if (!g.playing) {
            return;
        }
        g.playing = false;
    } else {
        // A group held at its end (or not looping at the start in reverse) stays where it is.
        const bool atEnd = !g.loop && (g.reverse ? g.t <= 0.0f : g.t >= g.duration);
        if (g.playing || atEnd) {
            return;
        }
        g.playing = true;
        g.lastClock = m_clock;
    }
    applyTimelineGroupTime(index);
}

float Document::timelineTime(std::string_view scopePath, std::string_view name) const {
    int found = findTimelineGroup(scopePath, name);
    return found < 0 ? -1.0f : m_timelineGroups->groups[static_cast<size_t>(found)].t;
}

bool Document::isTimelinePlaying(std::string_view scopePath, std::string_view name) const {
    int found = findTimelineGroup(scopePath, name);
    return found >= 0 && m_timelineGroups->groups[static_cast<size_t>(found)].playing;
}

void Document::setTimelineListener(TimelineListener listener) {
    m_timelineGroups->listener = std::move(listener);
}

void Document::captureTimelineRestores(size_t index) {
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    g.restoresCaptured = true;
    // Effect parameters have no ComputedStyle base to restore from on release: remember the value
    // they had before the members first wrote them. Taken on the first step after activation,
    // i.e. after the style resolve, so a group played before the first update() still sees the
    // element's resolved effect stack.
    for (size_t i = 0; i < g.memberInstances.size(); ++i) {
        const IrTimelineGroupMember& m = g.members[i];
        const Timeline* tl = m_animator.timeline(m.timeline);
        NodeId node = m_docElementNodes[m.element];
        if (g.memberInstances[i] == 0 || tl == nullptr || node == kInvalidNode || !m_arena.alive(node)) {
            continue;
        }
        for (const Track& track : tl->tracks) {
            uint32_t effectIndex = (static_cast<uint32_t>(track.prop) >> 8) & 0xFu;
            if (isEffectProp(track.prop) && effectIndex < m_arena.common(node).effectCount) {
                g.restores.push_back(
                    TimelineRestore{node, track.prop, getProperty(m_arena, node, track.prop)});
            }
        }
    }
}

void Document::stepTimelineGroups(float clock) {
    std::vector<TimelineGroupRuntime>& groups = m_timelineGroups->groups;
    // T-776 (58 §58.5): one port-history record per update, while a timeline reading it plays.
    m_timelineGroups->exprs.recordPortHistory(clock, groups);
    const bool selfControl = m_triggers->hasSelfControl();  // T-774
    for (size_t index = 0; index < groups.size(); ++index) {
        TimelineGroupRuntime& g = groups[index];
        if (g.active && !g.restoresCaptured) {
            captureTimelineRestores(index);
        }
        if (!g.active || !g.playing) {
            continue;
        }
        float dt = (clock - g.lastClock) * g.rate;
        g.lastClock = clock;
        if (dt < 0) {
            dt = 0;
        }
        bool inclusive = g.startInclusive;
        g.startInclusive = false;
        // T-774: a step cut at a self-controlled marker / end goes on from where its actions left
        // the group, with the rest of the step, while the group still plays.
        for (int pass = 0;
             advanceTimelineGroup(index, dt, inclusive, selfControl && pass < kMaxSelfControlCuts); ++pass) {
            TimelineGroupRuntime& cur = groups[index];
            if (!cur.active || !cur.playing) {
                break;
            }
            inclusive = cur.startInclusive;
            cur.startInclusive = false;
            cur.lastClock = clock;
        }
    }
}

bool Document::advanceTimelineGroup(size_t index, float& dt, bool inclusive, bool cut) {
    TimelineGroupRuntime& g = m_timelineGroups->groups[index];
    const NodeId scope = groupScope(m_docElementNodes, g);
    const float rate = g.rate;
    float travelled = 0;  // Group seconds up to the cut.
    float at = 0;         // The cut's time.
    // Queues the markers crossed from `a` to `b`; true when a self-controlled one cuts the step at `at`.
    const auto segment = [&](float a, float b, bool incl) {
        if (cut && firstSelfMarker(*m_triggers, g, scope, a, b, incl, at)) {
            queueTimelineMarkers(index, a, at, incl);
            travelled += std::fabs(at - a);
            return true;
        }
        queueTimelineMarkers(index, a, b, incl);
        travelled += std::fabs(b - a);
        return false;
    };
    // T-783: a self-control seek() jumping forward over other markers queued them; the ones that
    // control the group are applied now, where the jump landed, in time order -- not after the frame.
    const auto applyJumped = [&](size_t queuedFrom) {
        std::vector<TimelinePendingEvent>& pending = m_timelineGroups->pending;
        for (int n = 0; n < kMaxSelfControlCuts; ++n) {
            const TimelineGroupRuntime& cur = m_timelineGroups->groups[index];
            const NodeId curScope = groupScope(m_docElementNodes, cur);
            size_t best = pending.size();
            float bestAt = 0;
            for (size_t i = queuedFrom; i < pending.size(); ++i) {
                const TimelinePendingEvent& ev = pending[i];
                if (ev.group != index || ev.kind != TimelineEventKind::Marker || ev.selfApplied) {
                    continue;
                }
                for (const IrTimelineGroupMarker& m : cur.markers) {
                    if (m.name == ev.marker &&
                        (best == pending.size() || (cur.reverse ? m.time > bestAt : m.time < bestAt)) &&
                        m_triggers->selfControls(cur.name, TimelineEventKind::Marker, m.name, curScope)) {
                        best = i;
                        bestAt = m.time;
                        break;
                    }
                }
            }
            if (best == pending.size()) {
                return;
            }
            pending[best].selfApplied = true;
            const StringId marker = pending[best].marker;
            m_triggers->applySelfControl(cur.name, TimelineEventKind::Marker, marker, curScope);
        }
    };
    bool ended = false;
    bool stopped = false;
    float from = g.t;
    if (!g.reverse) {
        float to = from + dt;
        if (g.loop && g.duration > 0) {
            while (!stopped && to > g.duration) {
                stopped = segment(from, g.duration, inclusive);
                to -= g.duration;
                from = 0;
                inclusive = true;
            }
            stopped = stopped || segment(from, to, inclusive);
            g.t = to;
        } else if (to >= g.duration) {
            stopped = segment(from, g.duration, inclusive);
            g.t = g.duration;
            ended = true;
        } else {
            stopped = segment(from, to, inclusive);
            g.t = to;
        }
    } else {
        float to = from - dt;
        if (g.loop && g.duration > 0) {
            while (!stopped && to < 0) {
                stopped = segment(from, 0, inclusive);
                to += g.duration;
                from = g.duration;
                inclusive = true;
            }
            stopped = stopped || segment(from, to, inclusive);
            g.t = to;
        } else if (to <= 0) {
            stopped = segment(from, 0, inclusive);
            g.t = 0;
            ended = true;
        } else {
            stopped = segment(from, to, inclusive);
            g.t = to;
        }
    }
    if (stopped) {
        // T-774: the group stands at the marker while its self-control actions run.
        g.t = at;
        const size_t queued = m_timelineGroups->pending.size();
        for (const IrTimelineGroupMarker& m : g.markers) {
            if (m.time == at && m_triggers->selfControls(g.name, TimelineEventKind::Marker, m.name, scope)) {
                markSelfApplied(m_timelineGroups->pending, index, TimelineEventKind::Marker, m.name);
                m_triggers->applySelfControl(g.name, TimelineEventKind::Marker, m.name, scope);
            }
        }
        applyJumped(queued);
    } else {
        applyTimelineGroupTime(index);
        if (!ended) {
            return false;
        }
        g.playing = false;
        TimelinePendingEvent end{static_cast<uint32_t>(index), TimelineEventKind::End, kInvalidString};
        const bool self =
            cut && m_triggers->selfControls(g.name, TimelineEventKind::End, kInvalidString, scope);
        end.selfApplied = self;
        m_timelineGroups->pending.push_back(end);
        if (!g.hold) {
            releaseTimelineGroup(index);
        }
        if (!self) {
            return false;
        }
        const size_t queued = m_timelineGroups->pending.size();
        m_triggers->applySelfControl(g.name, TimelineEventKind::End, kInvalidString, scope);
        applyJumped(queued);
    }
    // The rest of the step, in clock time, at the rate the group plays at now.
    TimelineGroupRuntime& now = m_timelineGroups->groups[index];
    dt = std::max(dt - travelled, 0.0f);
    if (now.rate != rate) {
        dt = dt / rate * now.rate;
    }
    return true;
}

void Document::flushTimelineEvents() {
    std::vector<TimelinePendingEvent>& pending = m_timelineGroups->pending;
    if (pending.empty()) {
        return;
    }
    // Listeners may play/seek/stop groups, which may queue more events: take the batch first.
    std::vector<TimelinePendingEvent> batch;
    batch.swap(pending);
    for (const TimelinePendingEvent& ev : batch) {
        if (ev.group >= m_timelineGroups->groups.size()) {
            continue;
        }
        const TimelineGroupRuntime& g = m_timelineGroups->groups[ev.group];
        NodeId scope =
            g.scopeElement < m_docElementNodes.size() ? m_docElementNodes[g.scopeElement] : kInvalidNode;
        StringId name = g.name;
        if (scope != kInvalidNode && m_arena.alive(scope) && m_arena.kind(scope) == NodeKind::Element) {
            Event event;
            event.type =
                ev.kind == TimelineEventKind::End ? EventType::TimelineEnd : EventType::TimelineMarker;
            event.target = scope;
            event.markerName = ev.marker;
            event.timelineName = name;
            m_events->dispatch(m_arena, event);
        }
        // T-174: resume `then` chains waiting on this timeline, fire timeline(<n>).end/.marker().
        m_triggers->onTimelineEvent(name, ev.kind, ev.marker, scope, ev.selfApplied);  // T-774
        if (m_timelineGroups->listener && !m_unloading) {  // T-260a: none from an unloading Document.
            TimelineEvent te;
            te.kind = ev.kind;
            te.timeline = m_ir.string(name);
            te.marker = ev.marker == kInvalidString ? std::string_view{} : m_ir.string(ev.marker);
            te.scope = scope;
            m_timelineGroups->listener(te);
        }
    }
    m_triggers->dropSelfApplied();  // T-774: outcomes of trigger runs that did not happen.
}

void Document::forgetTimelineNode(NodeId node) {
    for (size_t index = 0; index < m_timelineGroups->groups.size(); ++index) {
        TimelineGroupRuntime& g = m_timelineGroups->groups[index];
        if (!g.active) {
            continue;
        }
        NodeId scope =
            g.scopeElement < m_docElementNodes.size() ? m_docElementNodes[g.scopeElement] : kInvalidNode;
        if (scope == node) {
            releaseTimelineGroup(index);
            continue;
        }
        for (size_t i = 0; i < g.memberInstances.size(); ++i) {
            if (g.memberInstances[i] != 0 && m_docElementNodes[g.members[i].element] == node) {
                m_animator.stop(g.memberInstances[i]);
                g.memberInstances[i] = 0;
            }
        }
        for (NodeId& ext : g.mediaClips) {
            if (ext != kInvalidNode && m_arena.alive(ext) && m_arena.parent(ext) == node) {
                ext = kInvalidNode;  // T-191: the clip element is going away.
            }
        }
        for (TimelineSyncClip& clip : g.syncClips) {
            if (clip.element == node) {
                if (clip.created && clip.instance != 0) {
                    m_animator.stop(clip.instance);
                }
                clip.instance = 0;
            }
        }
    }
}

}  // namespace xt
