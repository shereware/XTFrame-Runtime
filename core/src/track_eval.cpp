#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/timeline.h"

/// Track evaluation (docs/design/04-timeline.md §4.3) and Timeline <- IR conversion
/// (docs/tasks/T-06.md "timelineFromIr").

namespace xt {

namespace {

/// Cubic Bezier Bernstein coefficients at parameter e, shared by the Vec2 and Vec3 spatial-bezier
/// overloads below so the cubic itself is defined once.
struct BezierCoeffs {
    float a, b, c, d;
};
BezierCoeffs cubicBezierCoeffs(float e) {
    float mt = 1.0f - e;
    return BezierCoeffs{mt * mt * mt, 3.0f * mt * mt * e, 3.0f * mt * e * e, e * e * e};
}

/// Cubic Bezier at parameter e with explicit control points (Lottie spatial interpolation).
Vec2 cubicBezierPoint(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float e) {
    BezierCoeffs c = cubicBezierCoeffs(e);
    return Vec2{c.a * p0.x + c.b * p1.x + c.c * p2.x + c.d * p3.x,
                c.a * p0.y + c.b * p1.y + c.c * p2.y + c.d * p3.y};
}

/// Cubic Bezier at parameter e with explicit control points (Vec3 spatial interpolation).
Vec3 cubicBezierPoint(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float e) {
    BezierCoeffs c = cubicBezierCoeffs(e);
    return Vec3{c.a * p0.x + c.b * p1.x + c.c * p2.x + c.d * p3.x,
                c.a * p0.y + c.b * p1.y + c.c * p2.y + c.d * p3.y,
                c.a * p0.z + c.b * p1.z + c.c * p2.z + c.d * p3.z};
}

/// Finds i such that keyframes[i].time <= t < keyframes[i+1].time, within the half-open range
/// [track.kfOffset, track.kfOffset + track.kfCount). Assumes t is strictly inside that range (the
/// boundary cases t <= first.time / t >= last.time are handled by the caller before this runs).
/// Checks the track's cached lastIndex first, falling back to a std::upper_bound binary search;
/// either way, track.lastIndex is updated to the result (04.3's evaluation cache).
uint32_t findSegmentIndex(const Track& track, const std::vector<Keyframe>& keyframes, float t) {
    const Keyframe* base = keyframes.data() + track.kfOffset;
    uint32_t cached = track.lastIndex;
    if (cached + 1 < track.kfCount && base[cached].time <= t && t < base[cached + 1].time) {
        return cached;
    }
    const Keyframe* first = base;
    const Keyframe* last = base + track.kfCount;
    const Keyframe* it =
        std::upper_bound(first, last, t, [](float value, const Keyframe& kf) { return value < kf.time; });
    uint32_t idx = static_cast<uint32_t>((it - first)) - 1;
    track.lastIndex = idx;
    return idx;
}

}  // namespace

Value evalTrack(const Timeline& timeline, const Track& track, float t, float* scratchFloats,
                GradientStop* scratchStops, const Easing* overrideLinear,
                std::span<const EasingPoint> easingPoints) {
    if (track.kfCount == 0) {
        XT_ASSERT(false && "evalTrack: empty track");
        return Value::ofFloat(0);
    }

    const Keyframe* base = timeline.keyframes.data() + track.kfOffset;
    const Keyframe& first = base[0];
    const Keyframe& last = base[track.kfCount - 1];

    // T-255: a NaN would pass both boundary tests and the segment search would overrun. T-255b: NaN
    // and -Inf read as 0; +Inf is the end (the `t >= last.time` test below returns the last key).
    if (std::isnan(t) || t == -std::numeric_limits<float>::infinity()) {
        t = 0.0f;
    }
    if (t <= first.time) {
        return first.value;
    }
    if (t >= last.time) {
        return last.value;
    }

    uint32_t i = findSegmentIndex(track, timeline.keyframes, t);
    const Keyframe& kfA = base[i];
    const Keyframe& kfB = base[i + 1];

    // Neutral markers (docs/design/12-compiler.md §12.3) have no real value to interpolate
    // to/from (their type is Enum, unrelated to the track's real ValueType), so any segment
    // touching one stays neutral for its whole span; the caller (Animator) then keeps the
    // property's current base value instead of writing.
    if (kfA.value.isNeutral() || kfB.value.isNeutral()) {
        // Exactly on a declared stop, that stop's own value still applies: the neutral span covers
        // the interval leading to the marker, not the instant the real keyframe sits at. Without
        // this, seeking to the boundary of a trailing neutral span (e.g. t == 0.5 for
        // `0% {} 50% {} <neutral 100%>`) would write nothing at all (docs/tasks/T-70.md B).
        // findSegmentIndex() guarantees kfA.time <= t < kfB.time, so kfA is the only such stop.
        if (!kfA.value.isNeutral() && t == kfA.time) {
            return kfA.value;
        }
        return Value::neutral();
    }

    // See docs/design/04-timeline.md §4.3.
    if (kfA.easing.kind == EasingKind::Hold) {
        return kfA.value;
    }

    float p = (t - kfA.time) / (kfB.time - kfA.time);
    float e;
    switch (kfA.easing.kind) {
        case EasingKind::Linear:
            if (overrideLinear != nullptr && overrideLinear->kind != EasingKind::Linear) {
                e = evalEasing(*overrideLinear, p, easingPoints);
            } else {
                e = p;
            }
            break;
        case EasingKind::CubicBezier:
        case EasingKind::Steps:
        case EasingKind::LinearPoints:  // T-335.
            e = evalEasing(kfA.easing, p, easingPoints);
            break;
        case EasingKind::Hold:
            // Unreachable: handled above.
            e = 0;
            break;
        default:
            e = p;
            break;
    }

    if (track.type == ValueType::Vec2 && kfA.hasSpatial) {
        Vec2 tanOut2 = Vec2{kfA.tanOut.x, kfA.tanOut.y};
        Vec2 tanIn2 = Vec2{kfA.tanIn.x, kfA.tanIn.y};
        Vec2 p0 = kfA.value.v2;
        Vec2 p1 = p0 + tanOut2;
        Vec2 p3 = kfB.value.v2;
        Vec2 p2 = p3 + tanIn2;
        return Value::ofVec2(cubicBezierPoint(p0, p1, p2, p3, e));
    }
    if (track.type == ValueType::Vec3 && kfA.hasSpatial) {
        Vec3 p0 = kfA.value.v3;
        Vec3 p1 = Vec3{p0.x + kfA.tanOut.x, p0.y + kfA.tanOut.y, p0.z + kfA.tanOut.z};
        Vec3 p3 = kfB.value.v3;
        Vec3 p2 = Vec3{p3.x + kfA.tanIn.x, p3.y + kfA.tanIn.y, p3.z + kfA.tanIn.z};
        return Value::ofVec3(cubicBezierPoint(p0, p1, p2, p3, e));
    }
    return lerpValue(kfA.value, kfB.value, e, scratchFloats, scratchStops);
}

bool evalTrackDirection(const Timeline& timeline, const Track& track, float t, Vec2& out,
                        std::span<const EasingPoint> easingPoints) {
    if (track.kfCount < 2 || (track.type != ValueType::Vec2 && track.type != ValueType::Float)) {
        return false;
    }
    const Keyframe* base = timeline.keyframes.data() + track.kfOffset;
    uint32_t i;
    float e;
    if (t <= base[0].time) {
        i = 0;
        e = 0;
    } else if (t >= base[track.kfCount - 1].time) {
        i = track.kfCount - 2;
        e = 1;
    } else {
        i = findSegmentIndex(track, timeline.keyframes, t);
        const float span = base[i + 1].time - base[i].time;
        const float p = span > 0 ? (t - base[i].time) / span : 0.0f;
        e = base[i].easing.kind == EasingKind::Linear ? p : evalEasing(base[i].easing, p, easingPoints);
    }
    const Keyframe& kfA = base[i];
    const Keyframe& kfB = base[i + 1];
    if (kfA.value.isNeutral() || kfB.value.isNeutral() || kfA.easing.kind == EasingKind::Hold) {
        return false;
    }
    constexpr float kEps = 1e-6f;
    if (track.type == ValueType::Float) {
        const float span = kfB.time - kfA.time;
        const float slope = span > 0 ? (kfB.value.f - kfA.value.f) / span : 0.0f;
        if (std::fabs(slope) <= kEps) {
            return false;
        }
        out = Vec2{slope, 0};
        return true;
    }
    const Vec2 p0 = kfA.value.v2;
    const Vec2 p3 = kfB.value.v2;
    const Vec2 chord = p3 - p0;
    if (kfA.hasSpatial) {
        // The cubic's derivative, divided by 3: (1-e)^2 (P1-P0) + 2(1-e)e (P2-P1) + e^2 (P3-P2).
        const Vec2 p1 = p0 + Vec2{kfA.tanOut.x, kfA.tanOut.y};
        const Vec2 p2 = p3 + Vec2{kfA.tanIn.x, kfA.tanIn.y};
        const float mt = 1.0f - e;
        const Vec2 d{mt * mt * (p1.x - p0.x) + 2 * mt * e * (p2.x - p1.x) + e * e * (p3.x - p2.x),
                     mt * mt * (p1.y - p0.y) + 2 * mt * e * (p2.y - p1.y) + e * e * (p3.y - p2.y)};
        if (d.x * d.x + d.y * d.y > kEps) {
            out = d;
            return true;
        }
    }
    if (chord.x * chord.x + chord.y * chord.y <= kEps) {
        return false;
    }
    out = chord;
    return true;
}

namespace {

/// Decodes one Keyframe::value[16] blob (same 16-byte encoding as Decl's value, see xt/ir_pod.h)
/// into a runtime Value, per the field's ValueType. Path/FloatArray/Stops decode to an (offset,
/// count) pair into floatPool/stopPool; out-of-range offset/count returns kInvalidIr.
Status decodeKeyframeValue(const uint8_t value[16], ValueType type, const std::vector<float>& floatPool,
                           const std::vector<GradientStop>& stopPool, Value& out) {
    // Neutral marker (docs/design/12-compiler.md §12.3): the compiler writes the u32 0xFFFFFFFF
    // into the keyframe's value blob whatever the track's ValueType is (style_emit.cpp's
    // makeNeutralKeyframe), and IrKeyframe carries no type of its own, so it has to be recognized
    // before the per-type decode below. Decoding it as the track's type instead turned it into a
    // real value whose payload is the all-ones bit pattern -- a NaN Float/Length/Color that
    // isNeutral() does not match and that the Animator therefore wrote straight into the NodeArena
    // (docs/tasks/T-70.md B). 0xFFFFFFFF is not a legal payload for any other keyframe: it is NaN
    // for the float-backed types, the neutral marker itself for Enum, and an out-of-range pool
    // offset for Path/FloatArray/Stops.
    if (irValueU32(value, 0) == 0xFFFFFFFFu) {
        out = Value::neutral();
        return Status::kOk;
    }
    switch (type) {
        case ValueType::Float:
            out = Value::ofFloat(irValueF32(value, 0));
            return Status::kOk;
        case ValueType::Vec2:
            out = Value::ofVec2(Vec2{irValueF32(value, 0), irValueF32(value, 1)});
            return Status::kOk;
        // docs/design/19-3d-space.md §19.3/03.10: Vec3 (Translate3/Anchor3/Orientation/
        // CameraTarget tracks). Found missing while implementing T-48 (any animated Vec3 track,
        // e.g. an animated camera position, failed to load with kInvalidIr because this switch
        // fell through to the `default:` "not part of the Keyframe value encoding" branch below);
        // fixed here since T-48's lottie_3d_camera_anim golden fixture depends on it.
        case ValueType::Vec3:
            out = Value::ofVec3(Vec3{irValueF32(value, 0), irValueF32(value, 1), irValueF32(value, 2)});
            return Status::kOk;
        case ValueType::Color:
            out = Value::ofColor(Color{irValueF32(value, 0), irValueF32(value, 1), irValueF32(value, 2),
                                       irValueF32(value, 3)});
            return Status::kOk;
        case ValueType::Length: {
            float v = irValueF32(value, 0);
            LengthUnit unit = static_cast<LengthUnit>(value[4]);
            out = Value::ofLength(v, unit);
            return Status::kOk;
        }
        case ValueType::Enum:
            out = Value::ofEnum(irValueU32(value, 0));
            return Status::kOk;
        case ValueType::Path:
        case ValueType::FloatArray: {
            uint32_t offset = irValueU32(value, 0);
            uint32_t count = irValueU32(value, 1);
            if (offset > floatPool.size() || count > floatPool.size() - offset) {
                return Status::kInvalidIr;
            }
            const float* ptr = floatPool.data() + offset;
            if (type != ValueType::Path) {
                out = Value::ofFloatArray(ptr, count);
                return Status::kOk;
            }
            // T-190 (03 §3.12.6): slots 2/3 are an optional (offset, count) of the keyframe's own
            // verbs (PathVerb codes as floats) in the same pool; count 0 = the node's verbs (every
            // IR written before T-190 has zeros there).
            uint32_t verbOffset = irValueU32(value, 2);
            uint32_t verbCount = irValueU32(value, 3);
            if (verbCount == 0) {
                out = Value::ofPath(ptr, count);
                return Status::kOk;
            }
            if (verbOffset > floatPool.size() || verbCount > floatPool.size() - verbOffset) {
                return Status::kInvalidIr;
            }
            for (uint32_t i = 0; i < verbCount; ++i) {
                float v = floatPool[verbOffset + i];
                if (!(v >= 0.0f && v <= 3.0f) || v != static_cast<float>(static_cast<int>(v))) {
                    return Status::kInvalidIr;
                }
            }
            out = Value::ofPathWithVerbs(ptr, count, floatPool.data() + verbOffset, verbCount);
            return Status::kOk;
        }
        case ValueType::Stops: {
            uint32_t offset = irValueU32(value, 0);
            uint32_t count = irValueU32(value, 1);
            if (offset > stopPool.size() || count > stopPool.size() - offset) {
                return Status::kInvalidIr;
            }
            out = Value::ofStops(stopPool.data() + offset, count);
            return Status::kOk;
        }
        case ValueType::StringRef:
        case ValueType::ListRef:
        default:
            // Not part of the Keyframe value encoding (03.10 / ir_pod.h: StringRef/ListRef are
            // Decl-only). Conservatively treated as malformed IR.
            return Status::kInvalidIr;
    }
}

}  // namespace

Status timelineFromIr(const TimelineChunk& chunk, Timeline& out) {
    out = Timeline{};
    out.name = chunk.name();
    out.duration = chunk.duration();
    out.frameRate = chunk.frameRate();

    // Pools first: keyframe values below point into these, so they must exist and not be resized
    // again afterwards.
    std::span<const float> fp = chunk.floatPool();
    out.floatPool.assign(fp.begin(), fp.end());
    std::span<const IrGradientStop> sp = chunk.stopPool();
    out.stopPool.reserve(sp.size());
    for (const IrGradientStop& s : sp) {
        out.stopPool.push_back(GradientStop{s.offset, s.color});
    }

    std::span<const IrMarker> irMarkers = chunk.markers();
    out.markers.reserve(irMarkers.size());
    for (const IrMarker& m : irMarkers) {
        out.markers.push_back(Marker{m.name, m.start, m.duration});
    }

    std::span<const IrTrack> irTracks = chunk.tracks();
    std::span<const IrKeyframe> irKeyframes = chunk.keyframes();

    out.tracks.reserve(irTracks.size());
    for (const IrTrack& it : irTracks) {
        Track track;
        track.target = it.target;
        track.prop = it.prop;
        track.type = static_cast<ValueType>(it.type);
        track.kfOffset = it.kfOffset;
        track.kfCount = it.kfCount;
        track.lastIndex = 0;
        out.tracks.push_back(track);
    }

    out.keyframes.resize(irKeyframes.size());
    for (size_t ti = 0; ti < irTracks.size(); ++ti) {
        const IrTrack& it = irTracks[ti];
        // T-03 already bounds-checks kfOffset/kfCount against the keyframe array; re-checked here
        // per docs/tasks/T-06.md.
        if (it.kfOffset > irKeyframes.size() || it.kfCount > irKeyframes.size() - it.kfOffset) {
            return Status::kInvalidIr;
        }
        for (uint32_t k = 0; k < it.kfCount; ++k) {
            const IrKeyframe& ikf = irKeyframes[it.kfOffset + k];
            Keyframe& kf = out.keyframes[it.kfOffset + k];
            kf.time = ikf.time;
            kf.easing = easingFromIr(ikf.easing);  // T-335: keeps a LinearPoints pool range.
            kf.tanOut = ikf.tanOut;
            kf.tanIn = ikf.tanIn;
            kf.hasSpatial = ikf.hasSpatial;
            Value v;
            Status st = decodeKeyframeValue(ikf.value, out.tracks[ti].type, out.floatPool, out.stopPool, v);
            if (st != Status::kOk) {
                return st;
            }
            kf.value = v;
        }
    }

    return Status::kOk;
}

}  // namespace xt
