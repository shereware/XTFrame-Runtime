#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "doctest.h"
#include "perf_clock.h"
#include "xt/animator.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Performance threshold tests: Animator::evaluate with 1000 tracks, and the SW rasterizer with
/// 200 filled circles. See Docs/design/15-testing.md §15.4, Docs/tasks/T-38.md "效能門檻".
///
/// Debug only prints the measured value (MESSAGE, no assertion -- Debug builds are far from
/// representative of steady-state performance); Release asserts. Defining XT_PERF_RELAXED (see
/// Tests/perf/CMakeLists.txt's XT_PERF_RELAXED option) triples both thresholds, for a slower CI
/// runner.
///
/// Not covered by Docs/tasks/T-38.md (conservative choice made here): the sheet gives an explicit
/// sample count (100, median) only for the Animator case; the SW raster case just says "< 16 ms".
/// This file times the SW case the same way (a handful of untimed warm-up runs, then the median
/// of several timed runs) for the same reason -- a single sample is noisy -- rather than
/// introducing a second methodology.

using namespace xt;
using namespace xt::compile;

namespace {

/// Absolute path of a file under Tests/perf/fixtures (Docs/tasks/T-128.md "測試").
std::string fixturePath(const char* name) { return std::string(XT_PERF_FIXTURES_DIR) + "/" + name; }

double medianOf(std::vector<double>& samplesMs) {
    std::sort(samplesMs.begin(), samplesMs.end());
    return samplesMs[samplesMs.size() / 2];
}

#if defined(NDEBUG)
constexpr bool kIsReleaseBuild = true;
#else
constexpr bool kIsReleaseBuild = false;
#endif

#if defined(XT_PERF_RELAXED)
constexpr double kThresholdScale = 3.0;
#else
constexpr double kThresholdScale = 1.0;
#endif

/// Prints the measurement; also CHECKs it against `thresholdMs` in a Release build. Builds one
/// plain std::string with snprintf first (rather than a MESSAGE(... << ... << ...) chain mixing
/// ternaries and string literals) to sidestep an unrelated crash, so this file's own diagnostics
/// stay simple regardless of the cause -- see the T-38 report's "uncovered cases".
void reportAndCheck(const char* what, double medianMs, double thresholdMs) {
    const char* kind = kIsReleaseBuild ? "Release, asserted" : "Debug, message only";
    const char* relaxed = (kThresholdScale != 1.0) ? ", XT_PERF_RELAXED x3" : "";
    char line[256];
    std::snprintf(line, sizeof(line), "%s: median = %.4f ms (threshold %.4f ms, %s%s)", what, medianMs,
                  thresholdMs, kind, relaxed);
    MESSAGE(line);
    if (kIsReleaseBuild) {
        CHECK(medianMs < thresholdMs);
    }
}

/// `reportAndCheck` without the assertion: prints the measurement and the threshold it is being
/// judged against, in both configurations. Docs/tasks/T-128.md asks for the gradient-heavy board
/// benchmark to be reported this way ("門檻 <= 25 ms，Debug 不斷言、Release 印出") -- the board is
/// a regression *watch*, not a gate: T-128 brought it from 213 ms to well under half that but not
/// below 25 ms, and the remaining cost is in phases (layer composite, clip masks, effects) that
/// T-128 was not allowed to touch. Asserting here would only make the suite red on every run.
void reportOnly(const char* what, double medianMs, double thresholdMs) {
    const char* kind = kIsReleaseBuild ? "Release" : "Debug";
    char line[256];
    std::snprintf(line, sizeof(line), "%s: median = %.4f ms (watch threshold %.4f ms, %s, not asserted)",
                  what, medianMs, thresholdMs, kind);
    MESSAGE(line);
}

/// Appends a closed path approximating a circle (center `c`, radius `r`) as Move + 8 Cubic
/// segments + Close, per Docs/tasks/T-38.md "每個 8 段 Cubic 圓形". Each segment covers a 45deg
/// arc; the cubic-bezier-arc control-point formula is k = 4/3 * tan(theta/4) (theta = arc angle).
void appendCircle8(std::vector<PathVerb>& verbs, std::vector<float>& points, Vec2 c, float r) {
    constexpr int kSegments = 8;
    constexpr float kPi = 3.14159265358979323846f;
    const float theta = 2.0f * kPi / static_cast<float>(kSegments);
    const float k = (4.0f / 3.0f) * std::tan(theta / 4.0f) * r;

    auto pointAt = [&](float angle) { return Vec2{c.x + r * std::cos(angle), c.y + r * std::sin(angle)}; };
    auto tangentAt = [&](float angle) { return Vec2{-std::sin(angle), std::cos(angle)}; };
    auto pushPoint = [&](Vec2 p) {
        points.push_back(p.x);
        points.push_back(p.y);
    };

    Vec2 prev = pointAt(0.0f);
    verbs.push_back(PathVerb::Move);
    pushPoint(prev);

    float prevAngle = 0.0f;
    for (int i = 1; i <= kSegments; ++i) {
        float angle = theta * static_cast<float>(i);
        Vec2 cur = pointAt(angle);
        Vec2 tPrev = tangentAt(prevAngle);
        Vec2 tCur = tangentAt(angle);
        Vec2 c1{prev.x + tPrev.x * k, prev.y + tPrev.y * k};
        Vec2 c2{cur.x - tCur.x * k, cur.y - tCur.y * k};
        verbs.push_back(PathVerb::Cubic);
        pushPoint(c1);
        pushPoint(c2);
        pushPoint(cur);
        prev = cur;
        prevAngle = angle;
    }
    verbs.push_back(PathVerb::Close);
}

PathRef addCirclePath(CommandList& cl, Vec2 c, float r) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    appendCircle8(cl.verbs, cl.points, c, r);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    return ref;
}

}  // namespace

TEST_CASE("perf: Animator::evaluate, 1000 float tracks, median of 100 samples") {
    constexpr uint32_t kTrackCount = 1000;

    NodeArena arena;
    std::vector<NodeId> nodes;
    nodes.reserve(kTrackCount);
    for (uint32_t i = 0; i < kTrackCount; ++i) {
        nodes.push_back(arena.create(NodeKind::Group));
    }

    Timeline tl;
    tl.duration = 1.0f;
    tl.keyframes.reserve(static_cast<size_t>(kTrackCount) * 2);
    tl.tracks.reserve(kTrackCount);
    for (uint32_t i = 0; i < kTrackCount; ++i) {
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
        track.target = static_cast<NodeId>(i);  // Scene-local index i.
        track.prop = prop::Opacity;
        track.type = ValueType::Float;
        track.kfOffset = i * 2;
        track.kfCount = 2;
        tl.tracks.push_back(track);
    }

    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);
    PlayParams p;
    p.timeline = tid;
    p.localToArena = nodes.data();
    p.localCount = static_cast<uint32_t>(nodes.size());
    p.selfNode = kInvalidNode;
    p.loop = LoopMode::Loop;
    p.iterations = 0;
    anim.play(p, 0.0f);

    // Untimed warm-up: lets the Animator's scratch vectors grow to their steady-state capacity
    // before any timed sample (Docs/design/01-architecture.md §1.7).
    anim.evaluate(0.01f, arena);
    anim.evaluate(0.02f, arena);

    std::vector<double> samplesMs;
    samplesMs.reserve(100);
    for (int i = 0; i < 100; ++i) {
        float t = 0.03f + static_cast<float>(i) * 0.001f;
        double start = xt::perf::nowSeconds();
        anim.evaluate(t, arena);
        samplesMs.push_back((xt::perf::nowSeconds() - start) * 1000.0);
    }

    double medianMs = medianOf(samplesMs);
    double thresholdMs = (kIsReleaseBuild ? 0.2 : 1.0) * kThresholdScale;
    reportAndCheck("Animator::evaluate(1000 tracks)", medianMs, thresholdMs);
}

TEST_CASE("perf: SW raster, 1920x1080, 200 filled circles") {
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    constexpr int kPathCount = 200;

    std::mt19937 rng(12345u);  // Fixed seed, per Docs/tasks/T-38.md "固定種子隨機位置".
    std::uniform_real_distribution<float> xDist(0.0f, static_cast<float>(kWidth));
    std::uniform_real_distribution<float> yDist(0.0f, static_cast<float>(kHeight));
    std::uniform_real_distribution<float> rDist(8.0f, 40.0f);
    std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)};
    cl.devicePixelRatio = 1;
    cl.fills.reserve(kPathCount);
    cl.commands.reserve(kPathCount);
    for (int i = 0; i < kPathCount; ++i) {
        Vec2 center{xDist(rng), yDist(rng)};
        float radius = rDist(rng);
        FillCmd fc{};
        fc.xf = Mat2x3{};
        fc.path = addCirclePath(cl, center, radius);
        fc.paint.kind = PaintKind::Solid;
        fc.paint.color = Color{colorDist(rng), colorDist(rng), colorDist(rng), 1.0f};
        fc.rule = FillRule::NonZero;
        fc.antialias = 1;
        cl.fills.push_back(fc);
        cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
    }

    std::vector<uint8_t> buf(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0);
    xt::sw::SwTarget target{buf.data(), kWidth, kHeight, kWidth * 4};
    xt::sw::SwRenderer renderer;

    // Untimed warm-up.
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});
    renderer.render(cl, nullptr, nullptr);

    std::vector<double> samplesMs;
    constexpr int kSamples = 10;
    samplesMs.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        renderer.setTarget(target);
        renderer.clear(Color{0, 0, 0, 1});
        double start = xt::perf::nowSeconds();
        renderer.render(cl, nullptr, nullptr);
        samplesMs.push_back((xt::perf::nowSeconds() - start) * 1000.0);
    }

    double medianMs = medianOf(samplesMs);
    double thresholdMs = 16.0 * kThresholdScale;
    reportAndCheck("SW raster (1920x1080, 200 filled circles)", medianMs, thresholdMs);
}

TEST_CASE("perf: SW render, difficulty-select (gradient-heavy board) at 1280x720") {
    // Docs/tasks/T-128.md "測試": the fixture is the `xtframe-import dc` result for
    // Tests/import/fixtures/dc/difficulty-select -- many full-viewport gradient layers, large
    // translucent panels, 40 droplets and 26 rain streaks. Text is shaped by a NullTextShaper
    // (xt_perf links no font stack), so this benchmark measures the fill/span path, not text.
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    CompileOptions options;
    options.rootDir = std::string(XT_PERF_FIXTURES_DIR);
    CompileResult compiled = compileDocument(fixturePath("difficulty-select/difficulty-select.xt"), options);
    for (const Diagnostic& d : compiled.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(compiled.ok());

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{compiled.ir.data(), compiled.ir.size()}, true);
    REQUIRE(handle.ok());

    std::vector<uint8_t> buf(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0);
    xt::sw::SwTarget target{buf.data(), kWidth, kHeight, kWidth * 4};
    xt::sw::SwRenderer renderer;
    CommandList cl;
    const float dt = 1.0f / 60.0f;

    // Untimed warm-up: first style/layout resolve, scratch growth, gradient ramps. Few samples
    // on purpose: one frame of this board costs seconds in a Debug build, and the median of three
    // is already stable enough for a regression watch.
    for (int i = 0; i < 2; ++i) {
        ctx.update(dt);
        ctx.render(cl);
        renderer.setTarget(target);
        renderer.clear(Color{0, 0, 0, 1});
        renderer.render(cl, &shaper, nullptr, nullptr);
    }

    std::vector<double> samplesMs;
    constexpr int kSamples = 3;
    samplesMs.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        ctx.update(dt);
        ctx.render(cl);
        renderer.setTarget(target);
        renderer.clear(Color{0, 0, 0, 1});
        double start = xt::perf::nowSeconds();
        renderer.render(cl, &shaper, nullptr, nullptr);
        samplesMs.push_back((xt::perf::nowSeconds() - start) * 1000.0);
    }

    double medianMs = medianOf(samplesMs);
    reportOnly("SW render (difficulty-select, 1280x720)", medianMs, 25.0 * kThresholdScale);
}
