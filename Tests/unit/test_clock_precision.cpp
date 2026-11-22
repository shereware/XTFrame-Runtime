#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-870 (Docs/design/59-visual-feedback.md §59.7): the Context clock is kept and accumulated in
/// double, so a UI left running for hours does not drift; animation local time is the double
/// difference of two clocks; `xtTime` (CommandList::time) is the clock modulo 3600 s.

using namespace xt;
using namespace xt::compile;

namespace {

/// `#x` fades opacity 0 -> 1 linearly over a 2 s period forever, so its opacity is the phase.
constexpr const char* kPulse =
    "<xt><head><style>"
    "@keyframes ramp { from { opacity: 0 } to { opacity: 1 } }"
    "#x { width: 40px; height: 20px; background-color: #336699;"
    " animation: ramp 2s linear 0s infinite normal both; }"
    "</style></head>\n<body id=\"Root\"><div id=\"x\">x</div></body></xt>";

struct Host {
    std::unique_ptr<Context> ctx;
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;

    Document& doc() { return *ctx->document(handle); }
    float phase() { return doc().byId("x").property(prop::Opacity).f; }
};

std::unique_ptr<Host> openPulse() {
    auto host = std::make_unique<Host>();
    CompileOptions options;
    options.emitMeta = false;
    host->ir = test::compileMainOk(kPulse, options);
    host->ctx = std::make_unique<Context>(ContextConfig{nullptr, nullptr});
    host->ctx->setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> handle = host->ctx->loadDocument(IrBlob{host->ir.data(), host->ir.size()}, false);
    REQUIRE(handle.ok());
    host->handle = handle.value();
    host->ctx->update(0.0f);  // The animation starts at clock 0.
    return host;
}

/// The phase error, in seconds of the 2 s period, between the animation and `clock` (wrapped so a
/// phase just below 1 and one just above 0 count as close).
double phaseErrorSeconds(float phase, double clock) {
    const double expected = std::fmod(clock, 2.0) / 2.0;
    double d = std::fabs(static_cast<double>(phase) - expected);
    d = std::fmin(d, 1.0 - d);
    return d * 2.0;
}

/// Advances `host` by `frames` updates of 1/60 s and returns the clock the host asked for (the sum of
/// the float steps, in double).
double stepFrames(Host& host, long frames) {
    const float dt = 1.0f / 60.0f;
    for (long i = 0; i < frames; ++i) {
        host.ctx->update(dt);
    }
    return static_cast<double>(frames) * static_cast<double>(dt);
}

}  // namespace

TEST_CASE("T-870 clock: a 2 s CSS animation keeps its phase at 3600.5 s stepped in 1/60 s frames") {
    auto host = openPulse();
    const double clock = stepFrames(*host, 216030);  // 3600.5 s
    const double err = phaseErrorSeconds(host->phase(), clock);
    std::printf("T-870 drift at %.4f s (1/60 steps): %.3g s\n", clock, err);
    CHECK(err < 0.001);
}

TEST_CASE("T-870 clock: a 2 s CSS animation keeps its phase at 36000.5 s stepped in 1/60 s frames") {
    // 2.16 million updates: about 11 s in Debug.
    auto host = openPulse();
    const double clock = stepFrames(*host, 2160030);  // 36000.5 s
    const double err = phaseErrorSeconds(host->phase(), clock);
    std::printf("T-870 drift at %.4f s (1/60 steps): %.3g s\n", clock, err);
    CHECK(err < 0.001);
}

TEST_CASE("T-870 clock: one large step to 3600.5 s and to 36000.5 s keeps the phase") {
    for (const float target : {3600.5f, 36000.5f}) {
        auto host = openPulse();
        host->ctx->update(target);
        const double err = phaseErrorSeconds(host->phase(), static_cast<double>(target));
        std::printf("T-870 drift at %.4f s (one step): %.3g s\n", static_cast<double>(target), err);
        CHECK(err < 0.001);
    }
}

TEST_CASE("T-870 clock: a large clock followed by 1/60 s frames keeps the phase") {
    auto host = openPulse();
    host->ctx->update(36000.0f);
    const double clock = 36000.0 + stepFrames(*host, 30);  // 36000.5 s
    const double err = phaseErrorSeconds(host->phase(), clock);
    CHECK(err < 0.001);
}

TEST_CASE("T-870 clock: setClock() sets the clock without stepping; update() continues from it") {
    auto host = openPulse();
    host->ctx->setClock(36000.25);
    CHECK(host->ctx->clockSeconds() == 36000.25);
    CHECK(host->ctx->clock() == 36000.25f);
    host->ctx->update(0.25f);
    CHECK(host->ctx->clockSeconds() == 36000.5);
    // The animation started at clock 0 (openPulse), so its phase follows the absolute clock.
    CHECK(phaseErrorSeconds(host->phase(), 36000.5) < 0.001);
    host->ctx->setClock(-1.0);  // Never negative or non-finite.
    CHECK(host->ctx->clockSeconds() == 36000.5);
}

TEST_CASE("T-870 xtTime: CommandList::time is the clock modulo 3600 s") {
    auto host = openPulse();
    CommandList list;
    host->ctx->update(1.5f);
    host->ctx->render(list, nullptr);
    CHECK(list.time == 1.5f);
    host->ctx->update(3600.0f);  // 3601.5 s
    host->ctx->render(list, nullptr);
    CHECK(list.time == 1.5f);
    host->ctx->setClock(36000.25);
    host->ctx->update(0.0f);
    host->ctx->render(list, nullptr);
    CHECK(list.time == 0.25f);
    CHECK(kEffectTimePeriod == 3600.0);
}

TEST_CASE("T-870 clock: a Document loaded after setClock() starts at that clock") {
    // `xtframe-view --clock`: a timeline played before the first update starts at the set clock,
    // not 100 s in (the Document clock used to be 0 until its first update).
    constexpr const char* kTimeline =
        "<xt version=\"2\"><body><div id=\"x\"></div></body>"
        "<layout>#x { position: absolute; width: 10px; height: 10px; }</layout>"
        "<timelines>@timeline move { duration: 10s;"
        " #x { 0s { opacity: 0; } 10s { opacity: 1; } } }</timelines>"
        "</xt>";
    CompileOptions options;
    options.emitMeta = false;
    const std::vector<uint8_t> ir = test::compileMainOk(kTimeline, options);
    Context ctx(ContextConfig{nullptr, nullptr});
    ctx.setViewport(Rect{0, 0, 100, 100}, 1.0f);
    ctx.setClock(100.0);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
    REQUIRE(handle.ok());
    Document& doc = *ctx.document(handle.value());
    REQUIRE(doc.playTimeline("", "move") == Status::kOk);
    ctx.update(0.0f);
    CHECK(doc.timelineTime("", "move") == 0.0f);
    ctx.update(2.5f);
    CHECK(doc.timelineTime("", "move") == doctest::Approx(2.5f));
}

TEST_CASE("T-874 clock: an <anim autoplay> of a Document loaded after setClock() starts at that clock") {
    // `xtframe-view --clock 100`: the autoplay instance used to start at clock 0 inside
    // Document::load(), so the first frame showed the clip 100 s in (here: its last frame).
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_DATA_DIR;  // Tests/lottie: min_solid_anim.json is 2 s long
    const std::vector<uint8_t> ir = test::compileMainOk(
        "<xt><body><anim id=\"clip\" src=\"min_solid_anim.json\"></anim></body></xt>", options);
    Context ctx(ContextConfig{nullptr, nullptr});
    ctx.setViewport(Rect{0, 0, 100, 100}, 1.0f);
    ctx.setClock(100.0);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
    REQUIRE(handle.ok());
    Document& doc = *ctx.document(handle.value());
    const uint32_t inst = doc.defaultAnimation(doc.byId("clip"));
    REQUIRE(inst != 0);
    ctx.update(0.0f);
    CHECK(doc.animationTime(inst) == 0.0f);
    ctx.update(0.5f);
    CHECK(doc.animationTime(inst) == doctest::Approx(0.5f));
}
