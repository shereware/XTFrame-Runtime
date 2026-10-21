#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/ir_pod.h"
#include "xt/timeline.h"
#include "xt/types.h"
#include "xt/value.h"

/// Runtime state of a Document's `@timeline` groups (docs/design/04-timeline.md §4.10.1-4.10.2,
/// docs/tasks/T-172.md). Private to core; Document owns one TimelineGroupSet behind a unique_ptr
/// and implements the behavior in timeline_group.cpp.

namespace xt {

/// One `sync` clip driven by a group (4.10.2): an `<anim sync>` (the Animator instance the group
/// seeks) or, T-190, a `<use sync>` symbol instance root (the instance's own timeline groups,
/// which the group seeks in the clip's local time).
struct TimelineSyncClip {
    /// The `anim` element or the instance root.
    NodeId element = kInvalidNode;
    /// The instance being driven (0 once released or the element is gone; always 0 for a
    /// `<use sync>` clip).
    uint32_t instance = 0;
    /// True if the group created the instance (stopped on release); false if it borrowed the
    /// element's autoplay instance (paused while driven, resumed on release).
    bool created = false;
    /// T-190: a `<use sync>` clip's timeline groups (indices into TimelineGroupSet::groups).
    std::vector<uint32_t> nestedGroups;
};

/// A property value to write back when a group releases its members.
struct TimelineRestore {
    NodeId node = kInvalidNode;
    PropId prop = 0;
    Value value;
};

/// One TGRP group plus its playback state.
struct TimelineGroupRuntime {
    /// Timeline name (IR StringId).
    StringId name = kInvalidString;
    /// DOCT element index of the scope element.
    uint32_t scopeElement = 0;
    /// Group duration, seconds.
    float duration = 0;
    /// The IR's markers/sounds/members for this group (the IR outlives the Document's use of it).
    std::span<const IrTimelineGroupMarker> markers;
    std::span<const IrTimelineGroupSound> sounds;
    std::span<const IrTimelineGroupMember> members;

    /// Members are instantiated (playing, paused by a seek, or held after the end).
    bool active = false;
    /// The shared clock advances.
    bool playing = false;
    bool reverse = false;
    bool loop = false;
    bool hold = false;
    /// The first step after play() also fires a marker sitting exactly on the start time.
    bool startInclusive = false;
    /// Group time, seconds.
    float t = 0;
    float rate = 1;
    /// Document clock of the last step.
    float lastClock = 0;
    /// Animator instance per member (parallel to `members`; 0 = none).
    std::vector<uint32_t> memberInstances;
    /// T-190 (11.9): 1 for a member that is an inline `<svg>`'s scene timeline (seconds, played
    /// through the scene instance's node table), parallel to `members`.
    std::vector<uint8_t> memberIsScene;
    /// `<anim sync>` clips in the scope.
    std::vector<TimelineSyncClip> syncClips;
    /// T-191: `<video sync>` / `<sequence sync>` clips in the scope (their External nodes).
    std::vector<NodeId> mediaClips;
    /// Effect-parameter values captured on the first step after activation, written back on
    /// release.
    std::vector<TimelineRestore> restores;
    bool restoresCaptured = false;
};

/// A queued End/Marker event, flushed after the Animator has evaluated the frame.
struct TimelinePendingEvent {
    uint32_t group = 0;
    TimelineEventKind kind = TimelineEventKind::End;
    StringId marker = kInvalidString;
};

/// Every group of one Document.
struct TimelineGroupSet {
    std::vector<TimelineGroupRuntime> groups;
    std::vector<TimelinePendingEvent> pending;
    TimelineListener listener;
};

}  // namespace xt
