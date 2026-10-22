#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/perf.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Built-in performance tracing. See Docs/design/21-perf-tracing.md §21.1-21.2,
/// Docs/tasks/T-76a.md "Tests/unit/test_perf.cpp".

using namespace xt;

namespace {

/// A document with enough boxes that a frame's style/layout/paint phases all do real work.
const char* kSource =
    "<xt><head><style>"
    "  body { background-color: #101010; }"
    "  .box { width: 120px; height: 40px; background-color: #ff8800;"
    "         border-radius: 4px; opacity: 0.8; }"
    "  @keyframes pulse { from { opacity: 0.2 } to { opacity: 1 } }"
    "  .spin { animation: pulse 1s linear 0s infinite; }"
    "</style></head><body>"
    "  <div class=\"box\"></div><div class=\"box spin\"></div><div class=\"box\"></div>"
    "  <div class=\"box\"></div><div class=\"box spin\"></div><div class=\"box\"></div>"
    "</body></xt>";

/// Compiles kSource and loads it into `ctx` (takeOwnership, so the IR buffer need not outlive this
/// call); fails the test when either step fails.
void loadTestDocument(Context& ctx) {
    DocumentHandle handle = 0;
    compile::SourceFile files[1] = {compile::SourceFile{"perf.xt", kSource}};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "perf.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    Result<DocumentHandle> loaded =
        ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true);
    REQUIRE(loaded.ok());
    handle = loaded.value();
    CHECK(handle != 0);
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
}

/// One frame: update + render, exactly what a host's loop does.
void runFrame(Context& ctx, CommandList& list, float dt) {
    ctx.update(dt);
    list.clear();
    ctx.render(list);
}

}  // namespace

TEST_CASE("PerfTracker: enabled records one entry per frame with non-zero phase times") {
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    loadTestDocument(ctx);

    ctx.perf().setEnabled(true);
    CommandList list;
    for (uint32_t i = 0; i < 10; ++i) {
        runFrame(ctx, list, i == 0 ? 0.0f : 1.0f / 60.0f);
    }

    std::span<const FrameStats> history = ctx.perf().history();
    REQUIRE(history.size() == 10);
    for (uint32_t i = 0; i < history.size(); ++i) {
        CHECK(history[i].frameIndex == i);
    }

    const FrameStats& last = history.back();
    CHECK(last.updateMs > 0.0f);
    // §21.1: the sub-phases are all measured inside Context::update, so their sum cannot exceed it.
    float phases = last.styleMs + last.layoutMs + last.animateMs + last.transformMs;
    CHECK(phases <= last.updateMs);
    CHECK(last.commands > 0);
    CHECK(last.paintMs >= 0.0f);
    CHECK(last.commandBytes > 0);
    CHECK(last.arenaNodes > 0);
    // The first frame styles and lays out every element; a later frame may have nothing dirty left
    // (LayoutEngine::layout() returns early then, so laidOutElements is legitimately 0).
    CHECK(history[0].styledElements > 0);
    CHECK(history[0].laidOutElements > 0);
}

TEST_CASE("PerfTracker: disabled records nothing and current() stays writable") {
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    loadTestDocument(ctx);

    CHECK(!ctx.perf().enabled());
    CommandList list;
    for (uint32_t i = 0; i < 10; ++i) {
        runFrame(ctx, list, 1.0f / 60.0f);
    }
    CHECK(ctx.perf().history().empty());
    CHECK(ctx.perf().summary().frames == 0);
    // current() is a scratch record while disabled: writing to it is safe and never stored.
    ctx.perf().current().updateMs += 1.0f;
    CHECK(ctx.perf().history().empty());
}

TEST_CASE("PerfTracker: reset drops the history and restarts frameIndex") {
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    loadTestDocument(ctx);

    ctx.perf().setEnabled(true);
    CommandList list;
    for (uint32_t i = 0; i < 5; ++i) {
        runFrame(ctx, list, 1.0f / 60.0f);
    }
    CHECK(ctx.perf().history().size() == 5);

    ctx.perf().reset();
    CHECK(ctx.perf().history().empty());
    CHECK(ctx.perf().enabled());  // reset() does not change enabled().

    runFrame(ctx, list, 1.0f / 60.0f);
    REQUIRE(ctx.perf().history().size() == 1);
    CHECK(ctx.perf().history()[0].frameIndex == 0);
}

TEST_CASE("PerfTracker: summary averages and maxima over the requested window") {
    PerfTracker tracker;
    tracker.setEnabled(true);
    // Frames 0..3 with updateMs 1, 2, 3, 4 and commands 10, 20, 30, 40, one clock second apart.
    for (uint32_t i = 0; i < 4; ++i) {
        tracker.beginFrame(static_cast<float>(i));
        tracker.current().updateMs = static_cast<float>(i + 1);
        tracker.current().commands = (i + 1) * 10;
        tracker.endFrame();
    }

    PerfSummary all = tracker.summary(120);
    CHECK(all.frames == 4);
    CHECK(all.avg.updateMs == doctest::Approx(2.5f));
    CHECK(all.max.updateMs == doctest::Approx(4.0f));
    CHECK(all.avg.commands == 25);
    CHECK(all.max.commands == 40);
    // 4 frames spanning 3 clock seconds.
    CHECK(all.fpsAvg == doctest::Approx(1.0f));

    PerfSummary lastTwo = tracker.summary(2);
    CHECK(lastTwo.frames == 2);
    CHECK(lastTwo.avg.updateMs == doctest::Approx(3.5f));
    CHECK(lastTwo.max.updateMs == doctest::Approx(4.0f));
    CHECK(lastTwo.avg.commands == 35);

    CHECK(tracker.summary(0).frames == 0);
}

TEST_CASE("PerfTracker: the ring buffer overwrites the oldest frame once full") {
    PerfTracker tracker;
    tracker.setEnabled(true);
    const uint32_t capacity = PerfTracker::kHistoryCapacity;
    for (uint32_t i = 0; i < capacity; ++i) {
        tracker.beginFrame(static_cast<float>(i));
        tracker.current().updateMs = static_cast<float>(i);
        tracker.endFrame();
    }
    REQUIRE(tracker.history().size() == capacity);
    CHECK(tracker.history().front().frameIndex == 0);
    CHECK(tracker.history().back().frameIndex == capacity - 1);

    // 10 more frames: the history stays at capacity and starts 10 frames later.
    for (uint32_t i = 0; i < 10; ++i) {
        tracker.beginFrame(static_cast<float>(capacity + i));
        tracker.current().updateMs = static_cast<float>(capacity + i);
        tracker.endFrame();
    }
    std::span<const FrameStats> history = tracker.history();
    REQUIRE(history.size() == capacity);
    CHECK(history.front().frameIndex == 10);
    CHECK(history.back().frameIndex == capacity + 9);
    for (uint32_t i = 0; i < capacity; ++i) {
        CHECK(history[i].frameIndex == 10 + i);
        CHECK(history[i].updateMs == doctest::Approx(static_cast<float>(10 + i)));
    }
}

TEST_CASE("PerfTracker: a backend may report into the frame after endFrame()") {
    // §21.2: Context::render() ends the frame, but the backend's render() call happens after it and
    // must still land in that frame's record.
    PerfTracker tracker;
    tracker.setEnabled(true);
    tracker.beginFrame(0.0f);
    tracker.endFrame();
    tracker.current().renderMs += 2.0f;
    REQUIRE(tracker.history().size() == 1);
    CHECK(tracker.history()[0].renderMs == doctest::Approx(2.0f));

    tracker.beginFrame(1.0f);
    CHECK(tracker.current().renderMs == doctest::Approx(0.0f));
    CHECK(tracker.history()[0].renderMs == doctest::Approx(2.0f));
}

TEST_CASE("PerfScope: costs nothing and writes nothing while the tracker is disabled") {
    PerfTracker tracker;
    tracker.beginFrame(0.0f);  // No-op while disabled.
    {
        PerfScope scope(tracker, &FrameStats::styleMs);
    }
    CHECK(tracker.current().styleMs == doctest::Approx(0.0f));
    CHECK(tracker.history().empty());

    tracker.setEnabled(true);
    tracker.beginFrame(0.0f);
    {
        PerfScope scope(tracker, &FrameStats::styleMs);
        // Busy work so the elapsed time is measurably positive.
        volatile double sink = 0;
        for (int i = 0; i < 200000; ++i) {
            sink = sink + static_cast<double>(i);
        }
        (void)sink;
    }
    CHECK(tracker.current().styleMs > 0.0f);
}
