#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/animator.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Animator tests. See Docs/tasks/T-07.md.

using namespace xt;

namespace {

constexpr StringId kHitMarker = 1;

/// Fixture: two tracks over a 1-second timeline.
///   - Float track, target = 1 (scene-local index), Opacity: 0 -> 1, linear.
///   - Vec2 track, target = 0, Translate: (0,0) -> (10,20), linear.
/// Also carries a single marker "hit" at t = 0.5 for the marker-crossing tests.
Timeline makeTimeline() {
    Timeline tl;
    tl.duration = 1.0f;

    Keyframe f0;
    f0.time = 0.0f;
    f0.value = Value::ofFloat(0.0f);
    f0.easing = Easing::linear();
    Keyframe f1;
    f1.time = 1.0f;
    f1.value = Value::ofFloat(1.0f);
    f1.easing = Easing::linear();
    tl.keyframes.push_back(f0);
    tl.keyframes.push_back(f1);

    Keyframe v0;
    v0.time = 0.0f;
    v0.value = Value::ofVec2(Vec2{0.0f, 0.0f});
    v0.easing = Easing::linear();
    Keyframe v1;
    v1.time = 1.0f;
    v1.value = Value::ofVec2(Vec2{10.0f, 20.0f});
    v1.easing = Easing::linear();
    tl.keyframes.push_back(v0);
    tl.keyframes.push_back(v1);

    Track opacityTrack;
    opacityTrack.target = 1;
    opacityTrack.prop = prop::Opacity;
    opacityTrack.type = ValueType::Float;
    opacityTrack.kfOffset = 0;
    opacityTrack.kfCount = 2;
    tl.tracks.push_back(opacityTrack);

    Track translateTrack;
    translateTrack.target = 0;
    translateTrack.prop = prop::Translate;
    translateTrack.type = ValueType::Vec2;
    translateTrack.kfOffset = 2;
    translateTrack.kfCount = 2;
    tl.tracks.push_back(translateTrack);

    tl.markers.push_back(Marker{kHitMarker, 0.5f, 0.0f});

    return tl;
}

/// Float Opacity track, target = 1: first keyframe neutral, second = 1.0 at t = 1.
Timeline makeNeutralTimeline() {
    Timeline tl;
    tl.duration = 1.0f;

    Keyframe k0;
    k0.time = 0.0f;
    k0.value = Value::neutral();
    k0.easing = Easing::linear();
    Keyframe k1;
    k1.time = 1.0f;
    k1.value = Value::ofFloat(1.0f);
    k1.easing = Easing::linear();
    tl.keyframes.push_back(k0);
    tl.keyframes.push_back(k1);

    Track track;
    track.target = 1;
    track.prop = prop::Opacity;
    track.type = ValueType::Float;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    return tl;
}

/// Float Opacity track, target = kSelfNode: 0 -> 1, linear over 1 second.
Timeline makeSelfTimeline() {
    Timeline tl;
    tl.duration = 1.0f;

    Keyframe k0;
    k0.time = 0.0f;
    k0.value = Value::ofFloat(0.0f);
    k0.easing = Easing::linear();
    Keyframe k1;
    k1.time = 1.0f;
    k1.value = Value::ofFloat(1.0f);
    k1.easing = Easing::linear();
    tl.keyframes.push_back(k0);
    tl.keyframes.push_back(k1);

    Track track;
    track.target = kSelfNode;
    track.prop = prop::Opacity;
    track.type = ValueType::Float;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    return tl;
}

struct Fixture {
    NodeArena arena;
    NodeId n0, n1, n2;
    NodeId localToArena[3];
    uint32_t localCount = 3;

    Fixture() {
        n0 = arena.create(NodeKind::Group);
        n1 = arena.create(NodeKind::Group);
        n2 = arena.create(NodeKind::Group);
        localToArena[0] = n0;
        localToArena[1] = n1;
        localToArena[2] = n2;
    }
};

PlayParams basicParams(TimelineId tid, const Fixture& fx) {
    PlayParams p;
    p.timeline = tid;
    p.localToArena = fx.localToArena;
    p.localCount = fx.localCount;
    p.selfNode = kInvalidNode;
    return p;
}

}  // namespace

TEST_CASE("Animator: basic playback, Once + Forwards fill") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    uint32_t id = anim.play(p, 0.0f);

    anim.evaluate(0.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));

    anim.evaluate(1.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));

    anim.evaluate(1.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));
    CHECK(anim.isFinished(id));
    CHECK(anim.activeInstanceCount() == 1);
}

TEST_CASE("Animator: fill None removes the instance and does not re-apply") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    p.fill = FillMode::None;
    uint32_t finishedId = 0;
    anim.onFinished = [&](uint32_t id) { finishedId = id; };
    uint32_t id = anim.play(p, 0.0f);

    anim.evaluate(0.5f, fx.arena);
    anim.evaluate(1.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));

    anim.evaluate(1.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));
    CHECK(finishedId == id);
    CHECK(anim.activeInstanceCount() == 0);
}

TEST_CASE("Animator: delay") {
    Fixture fx;
    Timeline tl = makeTimeline();

    SUBCASE("no backward fill: not written before delay elapses") {
        Animator anim;
        TimelineId tid = anim.registerTimeline(&tl);
        setProperty(fx.arena, fx.n1, prop::Opacity, Value::ofFloat(0.25f));

        PlayParams p = basicParams(tid, fx);
        p.delay = 1.0f;
        anim.play(p, 0.0f);
        anim.evaluate(0.5f, fx.arena);
        CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.25f));
    }

    SUBCASE("Backwards fill writes the start value before delay elapses") {
        Animator anim;
        TimelineId tid = anim.registerTimeline(&tl);
        setProperty(fx.arena, fx.n1, prop::Opacity, Value::ofFloat(0.25f));

        PlayParams p = basicParams(tid, fx);
        p.delay = 1.0f;
        p.fill = FillMode::Backwards;
        anim.play(p, 0.0f);
        anim.evaluate(0.5f, fx.arena);
        CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.0f));
    }
}

TEST_CASE("Animator: speed") {
    Fixture fx;
    Timeline tl = makeTimeline();

    SUBCASE("speed 2 doubles progress") {
        Animator anim;
        TimelineId tid = anim.registerTimeline(&tl);
        PlayParams p = basicParams(tid, fx);
        p.speed = 2.0f;
        anim.play(p, 0.0f);
        anim.evaluate(0.25f, fx.arena);
        CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
    }

    SUBCASE("speed -1 with seek plays backward") {
        Animator anim;
        TimelineId tid = anim.registerTimeline(&tl);
        PlayParams p = basicParams(tid, fx);
        p.speed = -1.0f;
        uint32_t id = anim.play(p, 0.0f);
        anim.seek(id, 1.0f, 0.0f);
        anim.evaluate(0.5f, fx.arena);
        CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
    }
}

TEST_CASE("Animator: Loop with infinite iterations") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    std::vector<uint32_t> iterations;
    anim.onIteration = [&](uint32_t, uint32_t iter) { iterations.push_back(iter); };

    PlayParams p = basicParams(tid, fx);
    p.loop = LoopMode::Loop;
    p.iterations = 0;
    anim.play(p, 0.0f);

    anim.evaluate(2.3f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.3f));
    REQUIRE(iterations.size() == 2);
    CHECK(iterations[0] == 1);
    CHECK(iterations[1] == 2);
}

TEST_CASE("Animator: PingPong") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    p.loop = LoopMode::PingPong;
    p.iterations = 0;
    anim.play(p, 0.0f);

    anim.evaluate(1.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));

    anim.evaluate(2.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
}

TEST_CASE("Animator: iterations forced to 1 when loop == Once") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    p.loop = LoopMode::Once;
    p.iterations = 2;  // Should be forced to 1.
    uint32_t id = anim.play(p, 0.0f);

    anim.evaluate(2.5f, fx.arena);
    CHECK(anim.isFinished(id));
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));
}

TEST_CASE("Animator: segment") {
    Fixture fx;
    Timeline tl = makeTimeline();

    SUBCASE("segStart/segEnd remap local time") {
        Animator anim;
        TimelineId tid = anim.registerTimeline(&tl);
        PlayParams p = basicParams(tid, fx);
        p.segStart = 0.25f;
        p.segEnd = 0.75f;
        anim.play(p, 0.0f);
        anim.evaluate(0.25f, fx.arena);
        CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
    }

    SUBCASE("segment with Loop") {
        Animator anim;
        TimelineId tid = anim.registerTimeline(&tl);
        PlayParams p = basicParams(tid, fx);
        p.segStart = 0.25f;
        p.segEnd = 0.75f;
        p.loop = LoopMode::Loop;
        p.iterations = 0;
        anim.play(p, 0.0f);
        anim.evaluate(0.6f, fx.arena);
        CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.35f));
    }
}

TEST_CASE("Animator: durationScale") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    p.durationScale = 0.5f;
    anim.play(p, 0.0f);
    anim.evaluate(0.25f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
}

TEST_CASE("Animator: pause/resume") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    uint32_t id = anim.play(p, 0.0f);

    anim.evaluate(0.3f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.3f));

    anim.pause(id, 0.3f);
    anim.evaluate(0.8f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.3f));

    anim.resume(id, 0.8f);
    anim.evaluate(1.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
}

TEST_CASE("Animator: layering order (transition < layer0 < layer1 < override)") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams pa = basicParams(tid, fx);
    pa.layer = 0;
    anim.play(pa, 0.0f);  // A: starts at 0.

    PlayParams pb = basicParams(tid, fx);
    pb.layer = 0;
    anim.play(pb, 0.1f);  // B: starts later on the same layer -> should win over A.

    anim.evaluate(0.5f, fx.arena);
    // A localT = 0.5 -> opacity 0.5; B localT = 0.4 -> opacity 0.4; B started later -> wins.
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.4f));

    PlayParams pc = basicParams(tid, fx);
    pc.layer = 1;
    anim.play(pc, 0.0f);  // C: layer 1, started earliest -> still wins over A/B.

    anim.evaluate(0.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));

    anim.setOverride(fx.n1, prop::Opacity, Value::ofFloat(0.9f));
    anim.evaluate(0.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.9f));

    anim.clearOverride(fx.n1, prop::Opacity);
    anim.evaluate(0.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));

    Transition tr;
    tr.node = fx.n1;
    tr.prop = prop::Opacity;
    tr.from = Value::ofFloat(0.0f);
    tr.to = Value::ofFloat(0.2f);
    tr.startClock = 0.0f;
    tr.delay = 0.0f;
    tr.duration = 10.0f;
    tr.easing = Easing::linear();
    anim.addTransition(tr);
    anim.evaluate(0.5f, fx.arena);
    // Layer-1 instance still wins over the transition.
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
}

TEST_CASE("Animator: Transition evaluation and replace") {
    Fixture fx;
    Animator anim;

    Transition tr;
    tr.node = fx.n1;
    tr.prop = prop::Opacity;
    tr.from = Value::ofFloat(0.0f);
    tr.to = Value::ofFloat(10.0f);
    tr.startClock = 0.0f;
    tr.delay = 0.0f;
    tr.duration = 1.0f;
    tr.easing = Easing::linear();
    anim.addTransition(tr);
    CHECK(anim.transitionCount() == 1);

    anim.evaluate(0.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(5.0f));
    CHECK(anim.transitionCount() == 1);

    std::pair<NodeId, PropId> ended{kInvalidNode, prop::kInvalidProp};
    anim.onTransitionEnd = [&](NodeId n, PropId p) { ended = {n, p}; };
    anim.evaluate(1.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(10.0f));
    CHECK(ended.first == fx.n1);
    CHECK(ended.second == prop::Opacity);
    CHECK(anim.transitionCount() == 0);

    // Replace: adding a second transition for the same (node, prop) before it completes replaces
    // the first, rather than adding a second entry.
    Transition trA;
    trA.node = fx.n1;
    trA.prop = prop::Opacity;
    trA.from = Value::ofFloat(0.0f);
    trA.to = Value::ofFloat(100.0f);
    trA.startClock = 2.0f;
    trA.delay = 0.0f;
    trA.duration = 1.0f;
    trA.easing = Easing::linear();
    anim.addTransition(trA);
    CHECK(anim.hasTransition(fx.n1, prop::Opacity));
    CHECK(anim.transitionCount() == 1);

    Transition trB = trA;
    trB.to = Value::ofFloat(20.0f);
    anim.addTransition(trB);
    CHECK(anim.transitionCount() == 1);
    anim.evaluate(2.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(10.0f));
}

TEST_CASE("Animator: Neutral keyframe keeps the base value until a real keyframe is reached") {
    Fixture fx;
    Timeline tl = makeNeutralTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    setProperty(fx.arena, fx.n1, prop::Opacity, Value::ofFloat(0.42f));

    PlayParams p = basicParams(tid, fx);
    anim.play(p, 0.0f);

    anim.evaluate(0.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.42f));

    anim.evaluate(1.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));
}

TEST_CASE("Animator: overrideEasing replaces Linear keyframe easings") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    p.overrideEasing = Easing::cssEaseIn();
    anim.play(p, 0.0f);

    anim.evaluate(0.5f, fx.arena);
    float actual = getProperty(fx.arena, fx.n1, prop::Opacity).f;
    CHECK(std::fabs(actual - 0.3154f) <= 2e-3f);
}

TEST_CASE("Animator: marker crossing, including across a loop wrap") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    std::vector<std::pair<uint32_t, StringId>> events;
    anim.onMarker = [&](uint32_t id, StringId marker) { events.emplace_back(id, marker); };

    PlayParams p = basicParams(tid, fx);
    p.loop = LoopMode::Loop;
    p.iterations = 0;
    anim.play(p, 0.0f);

    anim.evaluate(0.4f, fx.arena);
    CHECK(events.empty());

    anim.evaluate(0.6f, fx.arena);
    CHECK(events.size() == 1);

    anim.evaluate(1.6f, fx.arena);
    CHECK(events.size() == 2);
}

TEST_CASE("Animator: kSelfNode resolves to PlayParams.selfNode") {
    Fixture fx;
    Timeline tl = makeSelfTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    p.selfNode = fx.n2;
    anim.play(p, 0.0f);

    anim.evaluate(0.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n2, prop::Opacity).f == doctest::Approx(0.5f));
}

TEST_CASE("Animator: evaluate() performs no allocations in steady state") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    PlayParams p = basicParams(tid, fx);
    p.loop = LoopMode::Loop;
    p.iterations = 0;
    anim.play(p, 0.0f);

    Transition tr;
    tr.node = fx.n0;
    tr.prop = prop::Opacity;
    tr.from = Value::ofFloat(0.0f);
    tr.to = Value::ofFloat(1.0f);
    tr.startClock = 0.0f;
    tr.delay = 0.0f;
    tr.duration = 100.0f;
    tr.easing = Easing::linear();
    anim.addTransition(tr);
    anim.setOverride(fx.n2, prop::Opacity, Value::ofFloat(0.1f));

    anim.evaluate(0.1f, fx.arena);
    Animator::DebugCapacities caps1 = anim.debugCapacities();
    anim.evaluate(0.2f, fx.arena);
    Animator::DebugCapacities caps2 = anim.debugCapacities();
    anim.evaluate(0.3f, fx.arena);
    Animator::DebugCapacities caps3 = anim.debugCapacities();

    CHECK(caps2.pending == caps3.pending);
    CHECK(caps2.scratchFloats == caps3.scratchFloats);
    CHECK(caps2.scratchStops == caps3.scratchStops);
    CHECK(caps1.pending == caps2.pending);
    CHECK(caps1.scratchFloats == caps2.scratchFloats);
    CHECK(caps1.scratchStops == caps2.scratchStops);
}

TEST_CASE("Animator: onFinished fires once for a Forwards-fill instance that stays in the pool") {
    NodeArena arena;
    const NodeId n0 = arena.create(NodeKind::Group);
    const NodeId n1 = arena.create(NodeKind::Group);
    const NodeId map[] = {n0, n1};
    Timeline tl;
    tl.duration = 1.0f;
    tl.keyframes.push_back(Keyframe{0.0f, Value::ofFloat(0.0f), Easing::linear(), {}, {}, 0});
    tl.keyframes.push_back(Keyframe{1.0f, Value::ofFloat(1.0f), Easing::linear(), {}, {}, 0});
    tl.tracks.push_back(Track{1, prop::Opacity, ValueType::Float, 0, 2, 0});
    Animator anim;
    const TimelineId tid = anim.registerTimeline(&tl);
    int fired = 0;
    anim.onFinished = [&](uint32_t) { ++fired; };
    PlayParams pp;
    pp.timeline = tid;
    pp.localToArena = map;
    pp.localCount = 2;
    pp.fill = FillMode::Forwards;
    const uint32_t id = anim.play(pp, 0.0f);
    anim.evaluate(0.5f, arena);
    CHECK(fired == 0);
    anim.evaluate(1.5f, arena);
    CHECK(fired == 1);
    CHECK(anim.isFinished(id));
    anim.evaluate(2.5f, arena);
    CHECK(fired == 1);
    anim.seek(id, 0.0f, 2.5f);
    anim.evaluate(4.0f, arena);
    CHECK(fired == 2);
}

// §19.3, Docs/tasks/T-43c.md: a live Track on an Element's PropId 16-22 forces is3D = 1 for as
// long as it is active. evaluate() does not clear is3D on its own when the track ends (19.3: "全
// 部結束後由下一次樣式解析恢復") -- that half is covered by
// test_document_3d.cpp's "a finished PropId 16-22 animation restores is3D on the next resolve",
// since restoring it is StyleEngine::resolve's job, not Animator's.
TEST_CASE("Animator: a live Track on an Element's PropId 16-22 forces is3D = 1") {
    NodeArena arena;
    const NodeId el = arena.create(NodeKind::Element);
    Timeline tl;
    tl.duration = 1.0f;
    tl.keyframes.push_back(Keyframe{0.0f, Value::ofFloat(0.0f), Easing::linear(), {}, {}, 0});
    tl.keyframes.push_back(Keyframe{1.0f, Value::ofFloat(45.0f), Easing::linear(), {}, {}, 0});
    tl.tracks.push_back(Track{kSelfNode, prop::RotationY, ValueType::Float, 0, 2, 0});  // PropId 19.
    Animator anim;
    const TimelineId tid = anim.registerTimeline(&tl);
    PlayParams pp;
    pp.timeline = tid;
    pp.selfNode = el;
    anim.play(pp, 0.0f);

    CHECK(arena.common(el).is3D == 0);
    anim.evaluate(0.0f, arena);
    CHECK(arena.common(el).is3D == 1);
    CHECK((arena.dirty(el) & static_cast<uint8_t>(kDirtyTree)) != 0);
    CHECK(getProperty(arena, el, prop::RotationY).f == doctest::Approx(0.0f));  // Even at value 0.
}

// Docs/tasks/T-50.md: effect-parameter PropIds (18.2) go through the exact same Track/evaluate
// path as any other prop -- no special-casing needed in animator.cpp itself.

TEST_CASE("Animator: Float track targets effectParam(0, 3) -> the effect's params[2] changes") {
    Fixture fx;
    EffectInstance e0{};
    e0.kind = EffectKind::DropShadow;
    EffectInstance seq[] = {e0};
    REQUIRE(fx.arena.assignEffects(fx.n1, seq, {}) == Status::kOk);
    uint32_t effectOffset = fx.arena.common(fx.n1).effectOffset;

    Timeline tl;
    tl.duration = 1.0f;
    Keyframe k0;
    k0.time = 0.0f;
    k0.value = Value::ofFloat(0.0f);
    k0.easing = Easing::linear();
    Keyframe k1;
    k1.time = 1.0f;
    k1.value = Value::ofFloat(8.0f);
    k1.easing = Easing::linear();
    tl.keyframes.push_back(k0);
    tl.keyframes.push_back(k1);

    Track track;
    track.target = 1;  // Fixture's n1 (localToArena[1]).
    track.prop = effectParam(0, 3);
    track.type = ValueType::Float;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);
    PlayParams p = basicParams(tid, fx);
    anim.play(p, 0.0f);

    anim.evaluate(0.5f, fx.arena);
    CHECK(fx.arena.effects()[effectOffset].params[2] == doctest::Approx(4.0f));
    CHECK(getProperty(fx.arena, fx.n1, effectParam(0, 3)).f == doctest::Approx(4.0f));
}

TEST_CASE("Animator: Vec2 track targets effectParam(0, kEffectParamOffset) -> params[0..1] change") {
    Fixture fx;
    EffectInstance e0{};
    e0.kind = EffectKind::DropShadow;
    EffectInstance seq[] = {e0};
    REQUIRE(fx.arena.assignEffects(fx.n1, seq, {}) == Status::kOk);
    uint32_t effectOffset = fx.arena.common(fx.n1).effectOffset;

    Timeline tl;
    tl.duration = 1.0f;
    Keyframe k0;
    k0.time = 0.0f;
    k0.value = Value::ofVec2(Vec2{0.0f, 0.0f});
    k0.easing = Easing::linear();
    Keyframe k1;
    k1.time = 1.0f;
    k1.value = Value::ofVec2(Vec2{10.0f, 20.0f});
    k1.easing = Easing::linear();
    tl.keyframes.push_back(k0);
    tl.keyframes.push_back(k1);

    Track track;
    track.target = 1;
    track.prop = effectParam(0, prop::kEffectParamOffset);
    track.type = ValueType::Vec2;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);
    PlayParams p = basicParams(tid, fx);
    anim.play(p, 0.0f);

    anim.evaluate(0.5f, fx.arena);
    CHECK(fx.arena.effects()[effectOffset].params[0] == doctest::Approx(5.0f));
    CHECK(fx.arena.effects()[effectOffset].params[1] == doctest::Approx(10.0f));
}

// T-255b: seek(+Inf) goes to the animation's end, seek(NaN / -Inf) to 0; a NaN / infinite speed
// reads as 0; a clock far past the uint32 iteration range saturates instead of casting out of range.
TEST_CASE("Animator: T-255b non-finite seek times and speeds") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);
    uint32_t id = anim.play(basicParams(tid, fx), 0.0f);

    anim.seek(id, std::numeric_limits<float>::infinity(), 0.0f);
    anim.evaluate(0.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));
    anim.seek(id, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    anim.evaluate(0.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.0f));
    anim.seek(id, 1.0f, 0.0f);
    anim.seek(id, -std::numeric_limits<float>::infinity(), 0.0f);
    anim.evaluate(0.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.0f));

    anim.setSpeed(id, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    anim.evaluate(0.5f, fx.arena);
    CHECK(std::isfinite(getProperty(fx.arena, fx.n1, prop::Opacity).f));

    Animator loopAnim;
    PlayParams looping = basicParams(loopAnim.registerTimeline(&tl), fx);
    looping.loop = LoopMode::Loop;
    looping.iterations = 0;  // Infinite.
    loopAnim.play(looping, 0.0f);
    loopAnim.evaluate(1e12f, fx.arena);
    const float far = getProperty(fx.arena, fx.n1, prop::Opacity).f;
    CHECK(std::isfinite(far));
    CHECK(far >= 0.0f);
    CHECK(far <= 1.0f);
}

// T-257b (fuzz_ir out-of-memory finding): one evaluate() that crosses millions of iterations of a
// tiny timeline queued one note per iteration. At most kMaxIterationNotesPerUpdate are queued
// per instance; the rest collapse into one note for the last iteration reached.
TEST_CASE("T-257b: one evaluate queues at most kMaxIterationNotesPerUpdate + 1 iteration notes") {
    Fixture fx;
    Timeline tl;
    tl.duration = 0.001f;
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    std::vector<uint32_t> iterations;
    anim.onIteration = [&](uint32_t, uint32_t iter) { iterations.push_back(iter); };

    PlayParams p = basicParams(tid, fx);
    p.loop = LoopMode::Loop;
    p.iterations = 65535;
    anim.play(p, 0.0f);

    const float clock = 3600.0f;  // One update with dt = 1 h.
    anim.evaluate(clock, fx.arena);

    // The iteration index computeLocalState reaches (not clamped to `iterations`).
    const float ratio = clock / 0.001f;
    uint32_t last = static_cast<uint32_t>(std::floor(ratio));
    if (last > 0 && ratio == static_cast<float>(last)) {
        last -= 1;
    }
    REQUIRE(last > kMaxIterationNotesPerUpdate + 1);
    REQUIRE(iterations.size() == kMaxIterationNotesPerUpdate + 1);
    CHECK(iterations.size() < 2 * kMaxIterationNotesPerUpdate);
    for (uint32_t i = 0; i < kMaxIterationNotesPerUpdate; ++i) {
        CHECK(iterations[i] == i + 1);
    }
    CHECK(iterations.back() == last);
}

TEST_CASE("T-257b: crossing fewer iterations than the bound reports every one of them") {
    Fixture fx;
    Timeline tl = makeTimeline();
    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);

    std::vector<uint32_t> iterations;
    anim.onIteration = [&](uint32_t, uint32_t iter) { iterations.push_back(iter); };

    PlayParams p = basicParams(tid, fx);
    p.loop = LoopMode::Loop;
    p.iterations = 10;
    anim.play(p, 0.0f);

    anim.evaluate(10.5f, fx.arena);
    REQUIRE(iterations.size() == 10);
    for (uint32_t i = 0; i < 10; ++i) {
        CHECK(iterations[i] == i + 1);
    }
}

TEST_CASE(
    "Animator: T-545 an override can tween, restart from the tweened value, and clears with its tween") {
    Fixture fx;
    Animator anim;
    setProperty(fx.arena, fx.n1, prop::Opacity, Value::ofFloat(0.0f));

    anim.setOverride(fx.n1, prop::Opacity, Value::ofFloat(1.0f), 0.5f, Easing::linear());
    CHECK(anim.hasOverride(fx.n1, prop::Opacity));
    anim.evaluate(0.0f, fx.arena);  // The tween starts at this clock, from the live value.
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.0f));
    anim.evaluate(0.25f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));

    // A new target mid-tween restarts from the tweened value (0.5), at the next evaluate().
    anim.setOverride(fx.n1, prop::Opacity, Value::ofFloat(0.0f), 1.0f, Easing::linear());
    anim.evaluate(0.25f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.5f));
    anim.evaluate(0.75f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.25f));
    anim.evaluate(1.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.0f));

    // The finished value is the target and stays.
    anim.setOverride(fx.n1, prop::Opacity, Value::ofFloat(1.0f), 0.5f, Easing::linear());
    anim.evaluate(2.0f, fx.arena);
    anim.evaluate(2.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));
    anim.evaluate(9.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(1.0f));

    // duration 0 writes at once; clearOverride drops the override (and a running tween with it).
    anim.setOverride(fx.n1, prop::Opacity, Value::ofFloat(0.3f));
    anim.evaluate(9.0f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.3f));
    anim.setOverride(fx.n1, prop::Opacity, Value::ofFloat(0.9f), 1.0f, Easing::linear());
    anim.clearOverride(fx.n1, prop::Opacity);
    CHECK(!anim.hasOverride(fx.n1, prop::Opacity));
    setProperty(fx.arena, fx.n1, prop::Opacity, Value::ofFloat(0.7f));  // What the restyle restores.
    anim.evaluate(9.5f, fx.arena);
    CHECK(getProperty(fx.arena, fx.n1, prop::Opacity).f == doctest::Approx(0.7f));
}
