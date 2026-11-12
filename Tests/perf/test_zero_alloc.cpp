#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "alloc_counter.h"
#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Per-frame zero-allocation tests: static UI, a playing Lottie, and 10 concurrent transitions.
/// See Docs/design/15-testing.md §15.4, Docs/tasks/T-38.md "零配置測試".
///
/// Not covered by Docs/tasks/T-38.md (conservative choices made here):
///   - None of Tests/lottie/*.json has >=50 layers (the largest, mask_matte.json, has 3), so the
///     Lottie case generates one: shape_rect_fill.json's single static shape layer, duplicated 50
///     times with distinct `ind`/`nm`, written to a temp .json file compileLottie can read.
///   - The transitions_10 case's very first update() after setStyle() is the one that actually
///     detects the changed base value and calls Animator::addTransition() 10 times; that growth
///     (Animator::m_transitions/m_pending/m_scratchFloats going from empty to holding 10 entries)
///     is a one-time cost of *starting* the transitions, not a steady-state per-frame cost, so it
///     runs once, uncounted, between setStyle() and startCounting() -- matching the other two
///     cases, where startCounting() also only begins once the scene has reached steady state.

using namespace xt;
using namespace xt::compile;

namespace {

std::string fixturePath(const char* name) { return std::string(XT_PERF_FIXTURES_DIR) + "/" + name; }

void requireNoErrors(const CompileResult& result) {
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
}

/// Prints up to kMaxRecordedSizes allocation sizes recorded by the most recent
/// xt::perf::startCounting()/stopCounting() window, tagged with `phase` and `frame` so a failure
/// says which of update()/render() allocated and on which of the counted frames. Builds a plain
/// std::string with snprintf first, rather than a MESSAGE(... << ... << ...) chain mixing a
/// runtime `const char*` parameter with other types, to sidestep an unrelated crash -- see the
/// T-38 report's "uncovered cases".
void reportAllocSizes(const char* phase, int frame) {
    uint32_t n = xt::perf::recordedCount();
    char header[160];
    std::snprintf(header, sizeof(header),
                  "%s allocated on frame %d: %llu call(s), first %u size(s) recorded below.", phase, frame,
                  static_cast<unsigned long long>(xt::perf::count()), n);
    MESSAGE(header);
    for (uint32_t i = 0; i < n; ++i) {
        char line[64];
        std::snprintf(line, sizeof(line), "  alloc[%u] = %u bytes", i,
                      xt::perf::recordedSize(static_cast<int>(i)));
        MESSAGE(line);
    }
}

/// Runs `frames` counted update()+render() pairs, checking each phase of each frame allocates
/// nothing. Must be called only once the scene has already reached steady state (every warm-up /
/// triggering side effect done beforehand, uncounted).
void checkZeroAllocFrames(Context& ctx, CommandList& cmds, float dt, int frames) {
    for (int i = 0; i < frames; ++i) {
        xt::perf::startCounting();
        ctx.update(dt);
        uint64_t updateAllocs = xt::perf::count();
        xt::perf::stopCounting();
        if (updateAllocs != 0) {
            reportAllocSizes("update()", i);
        }
        CHECK(updateAllocs == 0);

        xt::perf::startCounting();
        ctx.render(cmds);
        uint64_t renderAllocs = xt::perf::count();
        xt::perf::stopCounting();
        if (renderAllocs != 0) {
            reportAllocSizes("render()", i);
        }
        CHECK(renderAllocs == 0);
    }
}

/// T-692: renders `cmds` into a 1280x720 SW target 3 times uncounted (the warm-up: the layer pool
/// and the effect and mask scratch grow), then `frames` times counted; none may allocate.
void checkZeroAllocSwFrames(const CommandList& cmds, TextShaper& shaper, int frames, const char* phase) {
    std::vector<uint8_t> rgba(static_cast<size_t>(1280) * 720 * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{rgba.data(), 1280, 720, 1280 * 4});
    for (int i = 0; i < 3; ++i) {
        renderer.render(cmds, &shaper, nullptr);
    }
    uint64_t total = 0;
    for (int i = 0; i < frames; ++i) {
        xt::perf::startCounting();
        renderer.render(cmds, &shaper, nullptr);
        const uint64_t n = xt::perf::count();
        xt::perf::stopCounting();
        if (n != 0) {
            reportAllocSizes(phase, i);
        }
        total += n;
    }
    CHECK(total == 0);
}

/// Tests/lottie/shape_rect_fill.json's single static (non-animated) rectangle+fill layer,
/// duplicated `count` times with distinct `ind`/`nm`, wrapped in one Lottie composition. See the
/// file header for why this is generated rather than read from a fixture.
std::string buildLottieManyLayersJson(int count) {
    std::string json = "{\"v\":\"5.6.5\",\"fr\":30,\"ip\":0,\"op\":30,\"w\":800,\"h\":800,\"layers\":[";
    for (int i = 0; i < count; ++i) {
        if (i != 0) {
            json += ",";
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"ty\":4,\"ind\":%d,\"nm\":\"Layer%d\",\"ip\":0,\"op\":30,\"st\":0,\"sr\":1,"
                      "\"ks\":{\"a\":{\"a\":0,\"k\":[0,0,0]},\"p\":{\"a\":0,\"k\":[%d,%d,0]},"
                      "\"s\":{\"a\":0,\"k\":[100,100,100]},\"r\":{\"a\":0,\"k\":0},"
                      "\"o\":{\"a\":0,\"k\":100}},"
                      "\"shapes\":[{\"ty\":\"rc\",\"nm\":\"Rect\",\"p\":{\"a\":0,\"k\":[50,50]},"
                      "\"s\":{\"a\":0,\"k\":[40,20]},\"r\":{\"a\":0,\"k\":0},\"d\":1},"
                      "{\"ty\":\"fl\",\"nm\":\"Fill\",\"c\":{\"a\":0,\"k\":[1,0,0,1]},"
                      "\"o\":{\"a\":0,\"k\":100},\"r\":1}]}",
                      i, i, (i % 10) * 10, (i / 10) * 10);
        json += buf;
    }
    json += "]}";
    return json;
}

}  // namespace

TEST_CASE("perf: static UI (static_100.xt, 100 elements) update+render allocate nothing in steady state") {
    CompileOptions options;
    options.rootDir = std::string(XT_PERF_FIXTURES_DIR);
    CompileResult result = compileDocument(fixturePath("static_100.xt"), options);
    requireNoErrors(result);

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());

    CommandList cmds;
    const float dt = 1.0f / 60.0f;
    ctx.update(dt);  // Warm-up: 2 frames, uncounted (initial style/layout resolve, scratch growth).
    ctx.render(cmds);
    ctx.update(dt);
    ctx.render(cmds);

    checkZeroAllocFrames(ctx, cmds, dt, 10);
}

TEST_CASE("perf: Lottie, 50 playing layers, update+render allocate nothing in steady state") {
    // See the file header: no Tests/lottie/*.json fixture has >=50 layers, so one is generated.
    const int kLayerCount = 50;
    std::string json = buildLottieManyLayersJson(kLayerCount);
    // Written to the current working directory (ctest's run directory, not the fixtures/ source
    // tree) and removed right after compileLottie() reads it.
    std::string jsonPath = "xt_perf_generated_lottie_50_layers.json";
    {
        std::ofstream out(jsonPath, std::ios::binary);
        REQUIRE(out.good());
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
    }
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "Lottie zero-alloc case: generated %d duplicated shape_rect_fill.json layers "
                      "(no existing fixture has >= 50 layers)",
                      kLayerCount);
        MESSAGE(msg);
    }

    CompileOptions options;
    CompileResult result = compileLottie(jsonPath, options);
    std::remove(jsonPath.c_str());
    requireNoErrors(result);

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());

    CommandList cmds;
    const float dt = 1.0f / 60.0f;
    ctx.update(dt);  // Warm-up: 2 frames, uncounted.
    ctx.render(cmds);
    ctx.update(dt);
    ctx.render(cmds);

    checkZeroAllocFrames(ctx, cmds, dt, 10);
}

TEST_CASE(
    "perf: 10 concurrent transitions (transitions_10.xt) update+render allocate nothing "
    "once running") {
    CompileOptions options;
    options.rootDir = std::string(XT_PERF_FIXTURES_DIR);
    CompileResult result = compileDocument(fixturePath("transitions_10.xt"), options);
    requireNoErrors(result);

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);

    CommandList cmds;
    const float dt = 1.0f / 60.0f;
    ctx.update(dt);  // Warm-up: 2 frames, uncounted.
    ctx.render(cmds);
    ctx.update(dt);
    ctx.render(cmds);

    std::vector<ElementRef> boxes;
    Status st = doc->query(".box", boxes);
    REQUIRE(st == Status::kOk);
    REQUIRE(boxes.size() == 10);
    for (ElementRef box : boxes) {
        // setStyle() itself is allowed to allocate (Docs/tasks/T-38.md); not counted.
        Status setSt = box.setStyle("opacity: 0");
        CHECK(setSt == Status::kOk);
    }

    // The style resolve that detects the 10 changed base values and registers the matching
    // Animator transitions runs here -- a one-time cost, deliberately left uncounted (see the file
    // header).
    ctx.update(dt);
    ctx.render(cmds);
    CHECK(doc->animator().transitionCount() == 10);

    checkZeroAllocFrames(ctx, cmds, dt, 10);
}

// T-641 (Docs/design/50-c-abi.md §50.aa item 3): the SW layer pool's ageing must not churn. A static
// scene of nested opacity groups of different sizes (the panel holding three cards, a halo: two
// layers deep), fully rendered every frame at 1280x720, keeps
// hitting its pool slots: after the warm-up, 300 frames of SwRenderer::render allocate nothing --
// across two ageing windows (120 frames each). Effects have their own case below (T-643).
TEST_CASE("perf: T-641 static nested layers, 300 full SW frames allocate nothing (layer pool ageing)") {
    CompileOptions options;
    options.rootDir = std::string(XT_PERF_FIXTURES_DIR);
    CompileResult result = compileDocument(fixturePath("layers_static.xt"), options);
    requireNoErrors(result);

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());

    CommandList cmds;
    const float dt = 1.0f / 60.0f;
    ctx.update(dt);
    ctx.render(cmds);
    int layers = 0;
    for (const Command& c : cmds.commands) {
        layers += c.kind == CmdKind::PushLayer ? 1 : 0;
    }
    CHECK(layers >= 5);  // #panel, the 3 cards, #halo (a childless node folds its opacity into its paint)
    checkZeroAllocSwFrames(cmds, shaper, 300, "SwRenderer::render()");
}

// T-643: the effect passes' working buffers (blur levels, max-filter rows and columns, the distance
// transform, the backdrop pyramid) and the per-PushEffect state are renderer-owned scratch now. A
// static 1280x720 scene holding every SW effect kind -- drop shadow (plain, spread, inset), glow,
// blur, outline, ColorAdjust, GradientMask, Shine, a stack of three, a backdrop blur -- and a
// translucent Multiply layer, fully rendered every frame: after the warm-up, 300 frames allocate
// nothing.
// T-692: since T-668 / T-680 the painter turns the drop shadows of #shadow and #spread (plain opaque
// boxes) into DrawBoxShadow commands by default, which would leave this case with 9 effect layers
// and no off-screen drop-shadow pass at all. That pass still runs for hosts that turn the
// conversion off (xtframe-bake, `xtframe-view --no-box-shadow`, custom backends), so this case keeps
// it: the Context paints with setBoxShadowCommands(false) and all 11 effect layers stay effects.
// The default (converted) frame is covered by the case below.
TEST_CASE("perf: T-643 static effects, 300 full SW frames allocate nothing (effect scratch)") {
    CompileOptions options;
    options.rootDir = std::string(XT_PERF_FIXTURES_DIR);
    CompileResult result = compileDocument(fixturePath("effects_static.xt"), options);
    requireNoErrors(result);

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setBoxShadowCommands(false);
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());

    CommandList cmds;
    ctx.update(1.0f / 60.0f);
    ctx.render(cmds);
    int effects = 0;
    int boxShadows = 0;
    for (const Command& c : cmds.commands) {
        effects += c.kind == CmdKind::PushEffect ? 1 : 0;
        boxShadows += c.kind == CmdKind::DrawBoxShadow ? 1 : 0;
    }
    CHECK(effects >= 11);  // #multiply is a PushLayer, not an effect
    CHECK(boxShadows == 0);
    checkZeroAllocSwFrames(cmds, shaper, 300, "SwRenderer::render() with effects");
}

// T-692: the same scene with the default painter output (T-668 / T-680): #shadow and #spread are
// drawn as DrawBoxShadow commands, the other 9 effect layers (inset, glow, blur, outline,
// ColorAdjust, GradientMask, Shine, the stack, the backdrop blur) stay PushEffects. In steady state
// neither the painter (Context::update + render, which makes the conversion) nor the SW renderer
// (which draws the analytic shadows) allocates.
TEST_CASE("perf: T-692 static effects with converted box shadows allocate nothing in steady state") {
    CompileOptions options;
    options.rootDir = std::string(XT_PERF_FIXTURES_DIR);
    CompileResult result = compileDocument(fixturePath("effects_static.xt"), options);
    requireNoErrors(result);

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    REQUIRE(ctx.boxShadowCommands());  // The default.
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());

    CommandList cmds;
    const float dt = 1.0f / 60.0f;
    ctx.update(dt);  // Warm-up: 2 frames, uncounted.
    ctx.render(cmds);
    ctx.update(dt);
    ctx.render(cmds);
    checkZeroAllocFrames(ctx, cmds, dt, 10);

    int effects = 0;
    int boxShadows = 0;
    for (const Command& c : cmds.commands) {
        effects += c.kind == CmdKind::PushEffect ? 1 : 0;
        boxShadows += c.kind == CmdKind::DrawBoxShadow ? 1 : 0;
    }
    CHECK(boxShadows >= 2);  // #shadow, #spread
    CHECK(effects >= 9);
    checkZeroAllocSwFrames(cmds, shaper, 100, "SwRenderer::render() with box shadows");
}
