#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"
#include "xt/timeline.h"

using namespace xt;
using namespace xt::compile;

namespace {

/// A 3-keyframe Float track: (t=0, v=0) -[firstSegmentEasing]-> (t=1, v=10) -[Linear]-> (t=2, v=0).
Timeline makeFloatTimeline(Easing firstSegmentEasing) {
    Timeline tl;
    tl.duration = 2;

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofFloat(0);
    kf0.easing = firstSegmentEasing;
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofFloat(10);
    kf1.easing = Easing::linear();
    tl.keyframes.push_back(kf1);

    Keyframe kf2;
    kf2.time = 2;
    kf2.value = Value::ofFloat(0);
    kf2.easing = Easing::linear();  // ignored: last keyframe.
    tl.keyframes.push_back(kf2);

    Track track;
    track.type = ValueType::Float;
    track.kfOffset = 0;
    track.kfCount = 3;
    tl.tracks.push_back(track);

    return tl;
}

}  // namespace

TEST_CASE("evalTrack: Float track, Linear, boundary and interior samples") {
    Timeline tl = makeFloatTimeline(Easing::linear());
    const Track& track = tl.tracks[0];

    CHECK(evalTrack(tl, track, -1.0f, nullptr, nullptr).f == doctest::Approx(0.0f));
    CHECK(evalTrack(tl, track, 0.5f, nullptr, nullptr).f == doctest::Approx(5.0f));
    CHECK(evalTrack(tl, track, 1.5f, nullptr, nullptr).f == doctest::Approx(5.0f));
    CHECK(evalTrack(tl, track, 3.0f, nullptr, nullptr).f == doctest::Approx(0.0f));
}

TEST_CASE("evalTrack: Float track, first segment Hold") {
    Timeline tl = makeFloatTimeline(Easing::hold());
    const Track& track = tl.tracks[0];
    CHECK(evalTrack(tl, track, 0.5f, nullptr, nullptr).f == doctest::Approx(0.0f));
}

TEST_CASE("evalTrack: Float track, first segment CubicBezier ease-in") {
    Timeline tl = makeFloatTimeline(Easing::cssEaseIn());
    const Track& track = tl.tracks[0];
    float actual = evalTrack(tl, track, 0.5f, nullptr, nullptr).f;
    CHECK(std::fabs(actual - 10.0f * 0.3154f) <= 2e-3f);
}

TEST_CASE("evalTrack: lastIndex cache updates correctly across a sequence of samples") {
    Timeline tl = makeFloatTimeline(Easing::linear());
    const Track& track = tl.tracks[0];

    CHECK(evalTrack(tl, track, 0.1f, nullptr, nullptr).f == doctest::Approx(1.0f));
    CHECK(track.lastIndex == 0);
    CHECK(evalTrack(tl, track, 0.2f, nullptr, nullptr).f == doctest::Approx(2.0f));
    CHECK(track.lastIndex == 0);
    CHECK(evalTrack(tl, track, 1.5f, nullptr, nullptr).f == doctest::Approx(5.0f));
    CHECK(track.lastIndex == 1);
    CHECK(evalTrack(tl, track, 0.3f, nullptr, nullptr).f == doctest::Approx(3.0f));
    CHECK(track.lastIndex == 0);
}

TEST_CASE("evalTrack: Vec2 spatial bezier (Lottie-style tangents)") {
    Timeline tl;
    tl.duration = 1;

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofVec2(Vec2{0, 0});
    kf0.easing = Easing::linear();
    kf0.tanOut = Vec3{0, 10, 0};
    kf0.tanIn = Vec3{0, 10, 0};
    kf0.hasSpatial = 1;
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofVec2(Vec2{10, 0});
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Vec2;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Value out = evalTrack(tl, tl.tracks[0], 0.5f, nullptr, nullptr);
    CHECK(out.v2.x == doctest::Approx(5.0f));
    CHECK(out.v2.y == doctest::Approx(7.5f));
}

TEST_CASE("evalTrack: Vec3 track, Linear midpoint") {
    Timeline tl;
    tl.duration = 1;

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofVec3(Vec3{0, 0, 0});
    kf0.easing = Easing::linear();
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofVec3(Vec3{10, 20, -10});
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Vec3;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Value out = evalTrack(tl, tl.tracks[0], 0.5f, nullptr, nullptr);
    CHECK(out.v3.x == doctest::Approx(5.0f));
    CHECK(out.v3.y == doctest::Approx(10.0f));
    CHECK(out.v3.z == doctest::Approx(-5.0f));
}

TEST_CASE("evalTrack: Vec3 spatial bezier (Lottie-style tangents)") {
    Timeline tl;
    tl.duration = 1;

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofVec3(Vec3{0, 0, 0});
    kf0.easing = Easing::linear();
    // NOTE (deviation from Docs/tasks/T-42b.md's literal example): the sheet's test data uses
    // tanOut = (0, 10, 0) and tanIn = (0, -10, 0). With the documented control-point rule (P1 = a
    // + tanOut, P2 = b + tanIn, same rule the pre-existing Vec2 spatial-bezier path uses), those
    // opposite-signed y tangents exactly cancel at the symmetric t = 0.5 Bernstein weights (both
    // 0.375): y(0.5) = 0.375*10 + 0.375*(kf1.y-10) + ... reduces to exactly the linear midpoint,
    // not above it. Using matching-sign tangents (as the existing Vec2 spatial-bezier test does)
    // reproduces the intended "spatial bezier bulges above linear" behavior instead.
    kf0.tanOut = Vec3{0, 10, 0};
    kf0.tanIn = Vec3{0, 10, 0};
    kf0.hasSpatial = 1;
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofVec3(Vec3{10, 0, 10});
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Vec3;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Value out = evalTrack(tl, tl.tracks[0], 0.5f, nullptr, nullptr);
    // x/z are linear between endpoints (tangents are 0 on those axes): 0 -> 10 each, midpoint 5.
    CHECK(out.v3.x == doctest::Approx(5.0f));
    CHECK(out.v3.z == doctest::Approx(5.0f));
    // y bulges above the linear midpoint (0, both endpoints' y == 0) because of the tanOut/tanIn
    // handles (mirrors the Vec2 spatial-bezier test above).
    CHECK(out.v3.y > 0.0f);
    CHECK(out.v3.y == doctest::Approx(7.5f));
}

TEST_CASE("evalTrack: Path track interpolates each point, scratch is used") {
    Timeline tl;
    tl.duration = 1;
    tl.floatPool = {0, 0, 10, 10, 100, 100, 110, 110};

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofPath(tl.floatPool.data(), 4);
    kf0.easing = Easing::linear();
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofPath(tl.floatPool.data() + 4, 4);
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Path;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    float scratch[4] = {};
    Value out = evalTrack(tl, tl.tracks[0], 0.5f, scratch, nullptr);
    CHECK(out.floats == scratch);
    REQUIRE(out.floatCount == 4);
    CHECK(scratch[0] == doctest::Approx(50.0f));
    CHECK(scratch[1] == doctest::Approx(50.0f));
    CHECK(scratch[2] == doctest::Approx(60.0f));
    CHECK(scratch[3] == doctest::Approx(60.0f));
}

TEST_CASE("evalTrack: Stops track interpolates each stop, scratch is used") {
    Timeline tl;
    tl.duration = 1;
    tl.stopPool = {GradientStop{0, Color{1, 0, 0, 1}}, GradientStop{1, Color{0, 0, 1, 1}},
                   GradientStop{0, Color{0, 1, 0, 1}}, GradientStop{1, Color{1, 1, 0, 1}}};

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofStops(tl.stopPool.data(), 2);
    kf0.easing = Easing::linear();
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofStops(tl.stopPool.data() + 2, 2);
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Stops;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    GradientStop scratch[2] = {};
    Value out = evalTrack(tl, tl.tracks[0], 0.5f, nullptr, scratch);
    CHECK(out.stops == scratch);
    REQUIRE(out.stopCount == 2);
    CHECK(scratch[0].offset == doctest::Approx(0.0f));
    CHECK(scratch[0].color.r == doctest::Approx(0.5f));
    CHECK(scratch[0].color.g == doctest::Approx(0.5f));
    CHECK(scratch[1].offset == doctest::Approx(1.0f));
    CHECK(scratch[1].color.r == doctest::Approx(0.5f));
    CHECK(scratch[1].color.b == doctest::Approx(0.5f));
}

TEST_CASE("evalTrack: Length track, same unit linear, different unit discrete") {
    Timeline tl;
    tl.duration = 1;

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofLength(0, LengthUnit::Px);
    kf0.easing = Easing::linear();
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofLength(10, LengthUnit::Percent);
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Length;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Value below = evalTrack(tl, tl.tracks[0], 0.3f, nullptr, nullptr);
    CHECK(below.len.unit == LengthUnit::Px);
    CHECK(below.len.value == doctest::Approx(0.0f));

    Value above = evalTrack(tl, tl.tracks[0], 0.7f, nullptr, nullptr);
    CHECK(above.len.unit == LengthUnit::Percent);
    CHECK(above.len.value == doctest::Approx(10.0f));
}

TEST_CASE("evalTrack: Enum track is discrete") {
    Timeline tl;
    tl.duration = 1;

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofEnum(1);
    kf0.easing = Easing::linear();
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofEnum(2);
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Enum;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    CHECK(evalTrack(tl, tl.tracks[0], 0.3f, nullptr, nullptr).e == 1u);
    CHECK(evalTrack(tl, tl.tracks[0], 0.7f, nullptr, nullptr).e == 2u);
}

TEST_CASE("evalTrack: Color track linear") {
    Timeline tl;
    tl.duration = 1;

    Keyframe kf0;
    kf0.time = 0;
    kf0.value = Value::ofColor(Color{0, 0, 0, 0});
    kf0.easing = Easing::linear();
    tl.keyframes.push_back(kf0);

    Keyframe kf1;
    kf1.time = 1;
    kf1.value = Value::ofColor(Color{1, 1, 1, 1});
    tl.keyframes.push_back(kf1);

    Track track;
    track.type = ValueType::Color;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Value out = evalTrack(tl, tl.tracks[0], 0.25f, nullptr, nullptr);
    CHECK(out.c.r == doctest::Approx(0.25f));
    CHECK(out.c.g == doctest::Approx(0.25f));
    CHECK(out.c.b == doctest::Approx(0.25f));
    CHECK(out.c.a == doctest::Approx(0.25f));
}

namespace {

IrEasing linearIrEasing() { return IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0}; }

}  // namespace

TEST_CASE("timelineFromIr: build via IrWriter, round-trip through IrView, evaluate") {
    IrWriter w;
    StringId timelineName = w.intern("tl1");
    StringId markerName = w.intern("m1");

    TimelineBuilder tb;
    tb.name = timelineName;
    tb.duration = 1.0f;
    tb.frameRate = 24.0f;

    IrTrack floatTrack{};
    floatTrack.target = 0;
    floatTrack.prop = 0;
    floatTrack.type = static_cast<uint8_t>(ValueType::Float);
    floatTrack.kfOffset = 0;
    floatTrack.kfCount = 2;
    tb.tracks.push_back(floatTrack);

    IrTrack pathTrack{};
    pathTrack.target = 0;
    pathTrack.prop = 32;  // PathData
    pathTrack.type = static_cast<uint8_t>(ValueType::Path);
    pathTrack.kfOffset = 2;
    pathTrack.kfCount = 2;
    tb.tracks.push_back(pathTrack);

    IrTrack stopsTrack{};
    stopsTrack.target = 0;
    stopsTrack.prop = 43;  // FillGradientStops
    stopsTrack.type = static_cast<uint8_t>(ValueType::Stops);
    stopsTrack.kfOffset = 4;
    stopsTrack.kfCount = 2;
    tb.tracks.push_back(stopsTrack);

    IrKeyframe kf0{};
    kf0.time = 0.0f;
    kf0.easing = linearIrEasing();
    irSetValueF32(kf0.value, 0, 0.0f);
    tb.keyframes.push_back(kf0);

    IrKeyframe kf1{};
    kf1.time = 1.0f;
    kf1.easing = linearIrEasing();
    irSetValueF32(kf1.value, 0, 1.0f);
    tb.keyframes.push_back(kf1);

    IrKeyframe kf2{};
    kf2.time = 0.0f;
    kf2.easing = linearIrEasing();
    irSetValueU32(kf2.value, 0, 0);
    irSetValueU32(kf2.value, 1, 4);
    tb.keyframes.push_back(kf2);

    IrKeyframe kf3{};
    kf3.time = 1.0f;
    kf3.easing = linearIrEasing();
    irSetValueU32(kf3.value, 0, 4);
    irSetValueU32(kf3.value, 1, 4);
    tb.keyframes.push_back(kf3);

    IrKeyframe kf4{};
    kf4.time = 0.0f;
    kf4.easing = linearIrEasing();
    irSetValueU32(kf4.value, 0, 0);
    irSetValueU32(kf4.value, 1, 2);
    tb.keyframes.push_back(kf4);

    IrKeyframe kf5{};
    kf5.time = 1.0f;
    kf5.easing = linearIrEasing();
    irSetValueU32(kf5.value, 0, 2);
    irSetValueU32(kf5.value, 1, 2);
    tb.keyframes.push_back(kf5);

    tb.floatPool = {0, 0, 10, 10, 100, 100, 110, 110};
    tb.stopPool = {IrGradientStop{0.0f, Color{1, 0, 0, 1}}, IrGradientStop{1.0f, Color{0, 0, 1, 1}},
                   IrGradientStop{0.0f, Color{0, 1, 0, 1}}, IrGradientStop{1.0f, Color{1, 1, 0, 1}}};

    IrMarker marker{};
    marker.name = markerName;
    marker.start = 0.5f;
    marker.duration = 0.1f;
    tb.markers.push_back(marker);

    TimelineId timelineId = w.addTimeline(tb);

    std::vector<uint8_t> bytes = w.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{});
    REQUIRE(r.ok());
    IrView view = r.value();

    Timeline rt;
    Status st = timelineFromIr(view.timeline(timelineId), rt);
    REQUIRE(st == Status::kOk);

    CHECK(rt.name == timelineName);
    CHECK(rt.duration == doctest::Approx(1.0f));
    CHECK(rt.frameRate == doctest::Approx(24.0f));
    REQUIRE(rt.tracks.size() == 3);
    REQUIRE(rt.keyframes.size() == 6);
    REQUIRE(rt.floatPool.size() == 8);
    REQUIRE(rt.stopPool.size() == 4);
    REQUIRE(rt.markers.size() == 1);
    CHECK(rt.markers[0].name == markerName);
    CHECK(rt.markers[0].start == doctest::Approx(0.5f));
    CHECK(rt.markers[0].duration == doctest::Approx(0.1f));

    CHECK(rt.tracks[0].type == ValueType::Float);
    CHECK(rt.tracks[1].type == ValueType::Path);
    CHECK(rt.tracks[2].type == ValueType::Stops);
    CHECK(rt.tracks[1].prop == 32);

    Value floatVal = evalTrack(rt, rt.tracks[0], 0.5f, nullptr, nullptr);
    CHECK(floatVal.f == doctest::Approx(0.5f));

    float scratchFloats[4];
    Value pathVal = evalTrack(rt, rt.tracks[1], 0.5f, scratchFloats, nullptr);
    REQUIRE(pathVal.floatCount == 4);
    CHECK(scratchFloats[0] == doctest::Approx(50.0f));
    CHECK(scratchFloats[1] == doctest::Approx(50.0f));
    CHECK(scratchFloats[2] == doctest::Approx(60.0f));
    CHECK(scratchFloats[3] == doctest::Approx(60.0f));

    GradientStop scratchStops[2];
    Value stopsVal = evalTrack(rt, rt.tracks[2], 0.5f, nullptr, scratchStops);
    REQUIRE(stopsVal.stopCount == 2);
    CHECK(scratchStops[0].offset == doctest::Approx(0.0f));
    CHECK(scratchStops[0].color.r == doctest::Approx(0.5f));
    CHECK(scratchStops[0].color.g == doctest::Approx(0.5f));
    CHECK(scratchStops[1].offset == doctest::Approx(1.0f));
    CHECK(scratchStops[1].color.r == doctest::Approx(0.5f));
    CHECK(scratchStops[1].color.b == doctest::Approx(0.5f));

    // Out-of-range offset -> kInvalidIr. Docs/tasks/T-03.md's IrView::open() already rejects an
    // out-of-pool keyframe value offset (validated at open() time), so the only way to exercise
    // timelineFromIr's own defensive re-check is to corrupt an already-*validated* IrView's
    // backing bytes in place (the view is zero-copy over `bytes`) and re-run timelineFromIr on it.
    const IrKeyframe& pathKf0 = view.timeline(timelineId).keyframes()[2];
    auto byteOffset = static_cast<size_t>(reinterpret_cast<const uint8_t*>(&pathKf0) - bytes.data());
    uint8_t* valuePtr = bytes.data() + byteOffset + offsetof(IrKeyframe, value);
    irSetValueU32(valuePtr, 0, 999999u);  // offset now far past floatPool.size() == 8

    Timeline rt2;
    Status st2 = timelineFromIr(view.timeline(timelineId), rt2);
    CHECK(st2 == Status::kInvalidIr);
}
