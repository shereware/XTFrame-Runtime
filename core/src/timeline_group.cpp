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
/// docs/design/04-timeline.md §4.10.1-4.10.2, 13-api.md §13.8.2, 08-events.md §8.6,
/// docs/tasks/T-172.md.
///
/// Model: a group owns one shared clock (`t`, seconds). While a group is active every member is an
/// Animator instance of its compiled `@keyframes <timeline>__<path>` timeline, created paused and
/// seeked to `(t - delay) / duration` every frame (clamped, so a member shows its first key before
/// its delay and its last key after its end). Members are played with `PlayParams::group = 1`, so
/// they are evaluated after the element's own CSS animations and win on shared properties.

namespace xt {

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

void Document::loadTimelineGroups() {
    m_timelineGroups->groups.clear();
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
    }
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
                    setProperty(m_arena, node, track.prop, data.style.base[track.prop]);
                }
            }
        }
        // 19.3: a 3D PropId track may have forced is3D; a re-resolve restores it.
        m_arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    }
    for (const TimelineRestore& r : g.restores) {
        uint32_t effectIndex = (static_cast<uint32_t>(r.prop) >> 8) & 0xFu;
        if (m_arena.alive(r.node) && effectIndex < m_arena.common(r.node).effectCount) {
            setProperty(m_arena, r.node, r.prop, r.value);
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
            continue;
        }
        float u;
        if (m.duration > 0) {
            u = clampf((g.t - m.delay) / m.duration, 0.0f, 1.0f);
        } else {
            u = g.t >= m.delay ? 1.0f : 0.0f;
        }
        m_animator.seek(id, u, m_clock);
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
        bool hit;
        if (to >= from) {
            hit = (m.time > from || (inclusiveFrom && m.time == from)) && m.time <= to;
        } else {
            hit = (m.time < from || (inclusiveFrom && m.time == from)) && m.time >= to;
        }
        if (hit) {
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
    activateTimelineGroup(index);
    TimelineGroupRuntime& grp = m_timelineGroups->groups[index];
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
        bool ended = false;
        float from = g.t;
        if (!g.reverse) {
            float to = from + dt;
            if (g.loop && g.duration > 0) {
                while (to > g.duration) {
                    queueTimelineMarkers(index, from, g.duration, inclusive);
                    to -= g.duration;
                    from = 0;
                    inclusive = true;
                }
                queueTimelineMarkers(index, from, to, inclusive);
                g.t = to;
            } else if (to >= g.duration) {
                queueTimelineMarkers(index, from, g.duration, inclusive);
                g.t = g.duration;
                ended = true;
            } else {
                queueTimelineMarkers(index, from, to, inclusive);
                g.t = to;
            }
        } else {
            float to = from - dt;
            if (g.loop && g.duration > 0) {
                while (to < 0) {
                    queueTimelineMarkers(index, from, 0, inclusive);
                    to += g.duration;
                    from = g.duration;
                    inclusive = true;
                }
                queueTimelineMarkers(index, from, to, inclusive);
                g.t = to;
            } else if (to <= 0) {
                queueTimelineMarkers(index, from, 0, inclusive);
                g.t = 0;
                ended = true;
            } else {
                queueTimelineMarkers(index, from, to, inclusive);
                g.t = to;
            }
        }
        applyTimelineGroupTime(index);
        if (ended) {
            g.playing = false;
            m_timelineGroups->pending.push_back(
                TimelinePendingEvent{static_cast<uint32_t>(index), TimelineEventKind::End, kInvalidString});
            if (!g.hold) {
                releaseTimelineGroup(index);
            }
        }
    }
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
        m_triggers->onTimelineEvent(name, ev.kind, ev.marker, scope);
        if (m_timelineGroups->listener && !m_unloading) {  // T-260a: none from an unloading Document.
            TimelineEvent te;
            te.kind = ev.kind;
            te.timeline = m_ir.string(name);
            te.marker = ev.marker == kInvalidString ? std::string_view{} : m_ir.string(ev.marker);
            te.scope = scope;
            m_timelineGroups->listener(te);
        }
    }
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
