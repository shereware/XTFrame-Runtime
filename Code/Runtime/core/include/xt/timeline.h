#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "xt/types.h"
#include "xt/value.h"

/// Runtime timeline structures (Easing, Keyframe, Track, Marker, Timeline) and single-track
/// evaluation. See Docs/design/04-timeline.md §4.1-4.4, Docs/tasks/T-06.md.

namespace xt {

// Forward declaration only: timelineFromIr() takes a TimelineChunk (defined in xt/ir.h) by const
// reference. xt/ir.h -> xt/ir_pod.h -> xt/timeline.h (for EasingKind), so this header must not
// include xt/ir.h back (that would cycle); the .cpp implementing timelineFromIr includes it.
class TimelineChunk;

/// See Docs/design/04-timeline.md §4.1.
enum class EasingKind : uint8_t {
    Linear = 0,       ///< Constant-rate interpolation.
    Hold = 1,         ///< Stays at the segment's start value for the whole segment.
    CubicBezier = 2,  ///< Interpolates along a cubic bezier timing curve (x1,y1,x2,y2).
    Steps = 3,        ///< Discrete jumps (CSS `steps()`).
    /// T-335: CSS `linear(<stop>, ...)` -- piecewise-linear through `pointCount` EasingPoints at
    /// `pointOffset` of the easing point pool (IR STYL `easingPoints[]`).
    LinearPoints = 4
};

/// T-335: one point of a `linear()` easing (CSS Easing 1): input progress -> output progress. The
/// points of one easing have non-decreasing `in`.
struct EasingPoint {
    float in = 0;   ///< Input progress (0 and 1 for a canonical first / last point).
    float out = 0;  ///< Output progress.
};

/// T-335: one `linear()` stop as written: an output and zero, one or two input percentages (as
/// fractions, 50% = 0.5).
struct LinearStop {
    float out = 0;           ///< The stop's `<number>`.
    uint8_t inputCount = 0;  ///< How many of `in` are set (0..2).
    float in[2] = {0, 0};    ///< The stop's input positions.
};

/// Describes the easing of the segment from one keyframe to the next. The last keyframe's easing
/// is ignored. See Docs/design/04-timeline.md §4.1.
struct Easing {
    EasingKind kind = EasingKind::Linear;  ///< Which interpolation formula applies to this segment.
    uint8_t stepsJumpEnd = 1;  ///< Steps only: 1 = jump-end (CSS default steps(n, end)), 0 = jump-start.
    uint16_t stepCount = 1;    ///< Steps only.
    float x1 = 0;              ///< CubicBezier only: first control point x.
    float y1 = 0;              ///< CubicBezier only: first control point y.
    float x2 = 1;              ///< CubicBezier only: second control point x.
    float y2 = 1;              ///< CubicBezier only: second control point y.
    /// T-335: LinearPoints only: the first point's index in the easing point pool.
    uint16_t pointOffset = 0;
    /// T-335: LinearPoints only: the number of points (at least 2).
    uint16_t pointCount = 0;

    /// Builds a linear easing (CSS `linear`).
    static Easing linear();
    /// Builds a hold easing: evalEasing always returns 0 (the segment stays at the start value).
    static Easing hold();
    /// Builds a CubicBezier easing from the given control points.
    static Easing bezier(float x1, float y1, float x2, float y2);
    /// Builds a Steps easing with `n` steps, jumping at the end (true) or the start (false) of
    /// each step.
    static Easing steps(uint16_t n, bool jumpEnd);
    // CSS keyword table, Docs/design/04-timeline.md §4.1.
    /// Builds the CSS `ease` keyword's bezier easing.
    static Easing cssEase();
    /// Builds the CSS `ease-in` keyword's bezier easing.
    static Easing cssEaseIn();
    /// Builds the CSS `ease-out` keyword's bezier easing.
    static Easing cssEaseOut();
    /// Builds the CSS `ease-in-out` keyword's bezier easing.
    static Easing cssEaseInOut();
};

/// Evaluates `easing` at progress `p` (clamped to [0,1]). Hold always returns 0. CubicBezier solves
/// for t via Newton then bisection (Docs/design/04-timeline.md §4.1, exact algorithm). Steps follows
/// §4.3's formula. T-335: LinearPoints reads its points from `points` (the easing point pool) and
/// interpolates piecewise-linearly (CSS Easing 1's algorithm, binary search for the segment); a
/// range outside `points` evaluates as Linear.
float evalEasing(const Easing& easing, float p, std::span<const EasingPoint> points = {});

/// T-335: CSS Easing 1's canonical points of `linear(<stops>)`: a missing first / last input
/// becomes 0 / 1, an input below an earlier one is raised to it, a run of stops without one is
/// spread evenly between its neighbours, and a stop with two inputs is two points. Appends to
/// `out`; returns false (appending nothing) for fewer than two stops or a non-finite value.
bool linearEasingPoints(std::span<const LinearStop> stops, std::vector<EasingPoint>& out);

/// A single animated sample. See Docs/design/04-timeline.md §4.2.
struct Keyframe {
    float time = 0;  ///< Seconds, relative to the Timeline's start; strictly increasing.
    /// The value sampled at `time`. See Docs/design/03-scene-and-properties.md §3.10.
    Value value;
    Easing easing;  ///< Segment to the next keyframe.
    // Vec2/Vec3-only (Lottie spatial bezier); ignored for other types. The IR still stores Vec2
    // tangents only (T-42c adds Vec3 IR support); Vec3 tracks loaded from IR get z = 0.
    Vec3 tanOut;  ///< This keyframe's out-tangent, relative to this keyframe's value.
    Vec3 tanIn;   ///< Next keyframe's in-tangent, relative to the NEXT keyframe's value.
    /// 1 = interpolate this segment along the tanOut/tanIn spatial bezier (Vec2 or Vec3); 0 = plain
    /// component-wise interpolation.
    uint8_t hasSpatial = 0;
};

/// One animated (node, prop) pair over a Timeline. See Docs/design/04-timeline.md §4.4.
struct Track {
    NodeId target = kInvalidNode;  ///< Scene-local index; kSelfNode = the element the track applies to.
    PropId prop = 0;               ///< The animated property.
    /// Value type of `prop`; every keyframe on this track must carry a Value of this type.
    ValueType type = ValueType::Float;
    uint32_t kfOffset = 0;           ///< Offset into Timeline::keyframes.
    uint32_t kfCount = 0;            ///< Count into Timeline::keyframes.
    mutable uint32_t lastIndex = 0;  ///< Evaluation cache (last segment start index).
};

/// See Docs/design/04-timeline.md §4.4.
struct Marker {
    /// Marker name.
    StringId name = kInvalidString;
    /// Start time, in the Timeline's own seconds.
    float start = 0;
    /// Duration in seconds.
    float duration = 0;
};

/// A full timeline: tracks plus the pools their keyframe values point into. See
/// Docs/design/04-timeline.md §4.4.
///
/// Keyframe::value's Path/Stops/FloatArray pointers point into THIS Timeline's floatPool/stopPool.
/// Those pointers stay valid across a move (the vectors' buffers don't move), but are invalidated
/// by a copy -- so Timeline disallows copy and only allows move.
struct Timeline {
    /// Timeline name (e.g. the Lottie file name or the `@keyframes` name).
    StringId name = kInvalidString;
    /// Duration in seconds.
    float duration = 0;
    /// Hint only: the Lottie source's original fps, or 0 for a CSS-derived Timeline.
    float frameRate = 0;
    /// All animated (node, prop) tracks.
    std::vector<Track> tracks;
    /// Keyframe pool that Track::kfOffset/kfCount index into.
    std::vector<Keyframe> keyframes;
    std::vector<float> floatPool;        ///< Backing storage for Keyframe::value's Path/FloatArray data.
    std::vector<GradientStop> stopPool;  ///< Backing storage for Keyframe::value's Stops data.
    std::vector<Marker> markers;         ///< Named time ranges on this timeline.

    Timeline() = default;
    /// Not copyable: a copy would invalidate the keyframe pool pointers described above.
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;
    /// Movable: the keyframe pools' buffers don't move, so value-pointers stay valid.
    Timeline(Timeline&&) = default;
    /// Movable: the keyframe pools' buffers don't move, so value-pointers stay valid.
    Timeline& operator=(Timeline&&) = default;
};

/// Options for playing a named `@timeline` group (Docs/design/04-timeline.md §4.10.1,
/// 13-api.md §13.8.2, Docs/tasks/T-172.md).
struct PlayOptions {
    /// Play from the end (or `fromMarker`) towards 0.
    bool reverse = false;
    /// Repeat forever; `End` is never fired. Ignored when `once` is also set.
    bool loop = false;
    /// Play once (the default when `loop` is false).
    bool once = false;
    /// Keep the members at their final frame after the end until stopTimeline(); without it the
    /// members are released at the end and the elements return to their styled values.
    bool hold = false;
    /// Start at this marker's time instead of the start (or the end when `reverse`); empty = none.
    /// Only read during the call.
    std::string_view fromMarker;
    /// Playback rate multiplier (> 0).
    float rate = 1;
};

/// Kind of a TimelineEvent (Docs/design/08-events.md §8.6, 13-api.md §13.8.2).
enum class TimelineEventKind : uint8_t {
    End = 0,     ///< The group played to its end (not fired while looping, nor on stop).
    Marker = 1,  ///< Playback (or a seek in the play direction) crossed a named marker.
};

/// A timeline group event delivered to the host's timeline listeners. The string views stay valid
/// only for the duration of the callback.
struct TimelineEvent {
    /// End or Marker.
    TimelineEventKind kind = TimelineEventKind::End;
    /// The timeline's name.
    std::string_view timeline;
    /// The marker's name (Marker only).
    std::string_view marker;
    /// The group's scope element (the document root for a root-scope timeline).
    NodeId scope = kInvalidNode;
};

/// Host callback for TimelineEvents (13.8.2's `timelineEvents(listener)`).
using TimelineListener = std::function<void(const TimelineEvent&)>;

/// Evaluates `track` (which must belong to `timeline`) at local time `t`. See
/// Docs/design/04-timeline.md §4.3. `track.kfCount == 0` is a logic error (the compiler never
/// produces empty tracks): asserts and returns Value::ofFloat(0).
/// Path/Stops/FloatArray interpolation results are written into scratchFloats/scratchStops, which
/// the caller must size to at least the data length of any keyframe on this track.
/// `overrideLinear`: when non-null and its kind != EasingKind::Linear, replaces a Linear keyframe
/// easing with it (Docs/design/12-compiler.md §12.3, used by Animator's PlayParams.overrideEasing).
/// `easingPoints`: T-335's easing point pool a LinearPoints keyframe or override easing reads.
Value evalTrack(const Timeline& timeline, const Track& track, float t, float* scratchFloats,
                GradientStop* scratchStops, const Easing* overrideLinear = nullptr,
                std::span<const EasingPoint> easingPoints = {});

/// T-228 (31 §31.7.2, auto-orient): the direction of motion of `track` at `t`. A Vec2 track
/// yields its spatial-bezier derivative (the chord for a plain segment, or when the derivative
/// vanishes); a Float track yields its per-second chord slope in `out.x` (`out.y` = 0). Before the
/// first / after the last keyframe the first / last segment's end tangent is used. Returns false
/// when the track does not move there (a Hold or neutral segment, coincident values, a single
/// keyframe, another value type), so the caller keeps its previous angle.
bool evalTrackDirection(const Timeline& timeline, const Track& track, float t, Vec2& out,
                        std::span<const EasingPoint> easingPoints = {});

/// Builds a runtime Timeline from an IR TimelineChunk (used by T-08). Copies the pools first, then
/// the keyframes (so their Path/Stops/FloatArray pointers, decoded from each owning track's type,
/// point at valid storage). Returns Status::kInvalidIr if any keyframe's (offset, count) pair is
/// out of range for its pool (re-checked here even though the IR reader already validated it).
Status timelineFromIr(const TimelineChunk& chunk, Timeline& out);

}  // namespace xt
