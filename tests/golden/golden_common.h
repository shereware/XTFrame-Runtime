#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/types.h"

/// Golden-image test framework shared infrastructure: the comparison rule, fixture registration,
/// diff-image output, and the small scene-building helpers every `fixtures/scene_*.cpp` file
/// needs to construct its SceneBuilder without repeating boilerplate. `xt_golden` is a standalone
/// executable independent of doctest -- do not include doctest headers here. See
/// docs/tasks/T-13.md, docs/design/15-testing.md §15.3.

namespace xt::golden {

// ---------------------------------------------------------------------------------------------
// Comparison / diff output (docs/tasks/T-13.md "比對規則").
// ---------------------------------------------------------------------------------------------

struct GoldenResult {
    bool pass = false;
    uint32_t diffPixels = 0;
    uint32_t maxChannelDiff = 0;
};

/// T-267a (review L22): how many manifest problems loadLottieManifest() / loadUiManifest() have met
/// in this process -- a manifest that cannot be opened or a malformed line, each of which used to
/// drop fixtures silently. Callers count a non-zero value as a failure.
int manifestErrors();

/// Records one manifest problem (the loaders call it next to their message).
void noteManifestError();

/// Compares two tightly-packed RGBA8 buffers of size `w * h * 4`. A pixel counts as different if
/// any channel's absolute delta exceeds `channelTolerance`; the comparison passes when
/// `diffPixels <= w * h * maxDiffRatio`.
GoldenResult compareRgba(const uint8_t* a, const uint8_t* b, int w, int h, int channelTolerance,
                         float maxDiffRatio);

/// Writes a diff PNG to `path`: pixels that differ (per compareRgba's per-channel rule) are opaque
/// red; every other pixel is `a`'s grayscale luminance. Returns false if the PNG could not be
/// written.
bool writeDiffPng(const std::string& path, const uint8_t* a, const uint8_t* b, int w, int h,
                  int channelTolerance);

/// T-267d: the SW-against-SW-golden tolerance of this run. Strict unless the environment says
/// otherwise: XT_GOLDEN_CHANNEL (integer, default 2) is the per-channel delta a pixel may have,
/// XT_GOLDEN_RATIO (float, default 0) the share of pixels allowed beyond it. xt_golden and
/// xt_symbols_tests both read it; xt_golden's three exception fixtures keep their own 0.5% ratio.
struct SwTolerance {
    int channel = 2;
    float ratio = 0.0f;
};
SwTolerance swTolerance();

/// T-267d: XT_GOLDEN_VERBOSE=1 -- also report fixtures that pass with differing pixels ([DIFF]).
bool goldenVerbose();

/// T-267d: the one-line report of a compare, `<tag> <name>: <n> px differ, max channel diff <d>
/// (allowed: ratio <r>, channel <c>)`, with tag "[FAIL]" or "[DIFF]".
std::string describeDiff(const char* tag, const std::string& name, const GoldenResult& result, float ratio,
                         int channel);

/// T-267d: every xt_golden line (results and error details) goes to stdout through this, flushed
/// after each line, so a [FAIL] line can no longer be cut into a [PASS] line of the other stream.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void report(const char* format, ...);

// ---------------------------------------------------------------------------------------------
// Fixture registration (docs/tasks/T-13.md "Fixture 介面").
// ---------------------------------------------------------------------------------------------

struct GoldenFixture {
    const char* name;
    int width;
    int height;
    float dpr;
    float time;  // Reserved for future animated fixtures; every T-13 fixture uses 0.
    void (*build)(xt::compile::IrWriter&);
    xt::Color clearColor;
};

/// Appends `fixture` to the process-wide fixture registry. Called by XT_GOLDEN_FIXTURE's static
/// registration object; not normally called directly.
void registerFixture(const GoldenFixture& fixture);

/// All fixtures registered so far, in registration order.
const std::vector<GoldenFixture>& fixtures();

/// Defines `buildFnName` as a `void(xt::compile::IrWriter&)` function and registers a fixture
/// named `fixtureName` (a bare identifier, stringized) that uses it, at static-init time.
#define XT_GOLDEN_FIXTURE(fixtureName, width, height, dpr, time, clearColor, buildFnName)     \
    static void buildFnName(xt::compile::IrWriter&);                                          \
    namespace {                                                                               \
    struct XtGoldenReg_##fixtureName {                                                        \
        XtGoldenReg_##fixtureName() {                                                         \
            ::xt::golden::registerFixture(::xt::golden::GoldenFixture{                        \
                #fixtureName, (width), (height), (dpr), (time), &buildFnName, (clearColor)}); \
        }                                                                                     \
    };                                                                                        \
    const XtGoldenReg_##fixtureName g_xtGoldenReg_##fixtureName;                              \
    }                                                                                         \
    static void buildFnName(xt::compile::IrWriter& writer)

// ---------------------------------------------------------------------------------------------
// Scene-building helpers shared by fixtures/scene_*.cpp.
// ---------------------------------------------------------------------------------------------

/// IrSceneNode::parent marker for the scene root (matches tests/unit/test_painter.cpp).
inline constexpr uint32_t kRootParent = 0xFFFFFFFFu;

/// Appends a circle (center `c`, radius `r`) as one closed subpath (Move + 4 Cubic + Close, the
/// standard 4-segment kappa approximation) to `verbs`/`points`.
void appendCircle(std::vector<xt::PathVerb>& verbs, std::vector<float>& points, xt::Vec2 c, float r);

/// Describes one IrPathData to add to a SceneBuilder. Mirrors PathNodeData/IrPathData's fields;
/// see tests/unit/test_painter.cpp's PathSpec for the pattern this follows.
struct PathSpec {
    std::vector<xt::PathVerb> verbs;
    std::vector<float> points;
    xt::FillRule fillRule = xt::FillRule::NonZero;

    xt::PaintKind fillKind = xt::PaintKind::Solid;
    xt::Color fillColor{1, 1, 1, 1};
    xt::Vec2 fillP0{};
    xt::Vec2 fillP1{};
    std::vector<xt::IrGradientStop> fillStops;

    xt::PaintKind strokeKind = xt::PaintKind::None;
    xt::Color strokeColor{0, 0, 0, 1};
    xt::Vec2 strokeP0{};
    xt::Vec2 strokeP1{};
    std::vector<xt::IrGradientStop> strokeStops;
    float strokeWidth = 0;
    xt::LineCap cap = xt::LineCap::Butt;
    xt::LineJoin join = xt::LineJoin::Miter;
    float miterLimit = 4;
    std::vector<float> dashes;
    float dashPhase = 0;

    float trimStart = 0;
    float trimEnd = 1;
    float trimOffset = 0;

    /// T-233: stroke taper (fractions of arc length) and width profile as (t, w) pairs.
    float taperStart = 0;
    float taperEnd = 0;
    std::vector<float> widthProfile;
};

/// Appends one path's verb/point/stop data to `scene`'s pools and pushes the matching IrPathData.
/// Returns the new path's index (for use as a later IrSceneNode::kindIndex).
uint32_t addPath(xt::compile::SceneBuilder& scene, const PathSpec& spec);

/// NodeCommon fields an IrSceneNode carries, other than kind/parent/kindIndex. Defaults match
/// NodeCommon's own defaults.
struct NodeOpts {
    uint8_t visible = 1;
    float opacity = 1;
    xt::Vec2 anchor{0, 0};
    xt::Vec2 translate{0, 0};
    xt::Vec2 scale{1, 1};
    float rotation = 0;
    xt::Vec2 skew{0, 0};
    uint8_t blend = 0;
    uint8_t maskMode = 0;
};

/// Appends an IrSceneNode and returns its local index (for use as a later node's `parent`).
/// `parent` must be `kRootParent` for the first node in the scene, or a strictly smaller index.
uint32_t addNode(xt::compile::SceneBuilder& scene, xt::NodeKind kind, uint32_t parent, uint32_t kindIndex = 0,
                 const NodeOpts& opts = {});

// ---------------------------------------------------------------------------------------------
// Lottie fixture loader (docs/tasks/T-17.md "黃金圖 Lottie fixture 流程"). Implemented in
// golden_lottie.cpp. Unlike the SceneBuilder-based fixtures above, a Lottie fixture runs the full
// compileLottie -> Context -> update(time) -> render -> SwRenderer pipeline against a hand-written
// Lottie JSON file.
// ---------------------------------------------------------------------------------------------

struct LottieFixture {
    std::string name;
    std::string jsonPath;  // Relative to the lottie fixtures directory (manifest's own directory).
    int width = 0;
    int height = 0;
    float time = 0;
    xt::Color clearColor{0, 0, 0, 1};
};

/// Parses `tests/golden/lottie/manifest.txt`: one fixture per non-blank, non-'#'-comment line,
/// whitespace-separated `<name> <json relative path> <width> <height> <time> [clearColor hex]`.
/// `clearColor` defaults to "000000" (opaque black) when omitted. A malformed line is skipped with
/// a message to stdout and counted in manifestErrors().
std::vector<LottieFixture> loadLottieManifest(const std::string& manifestPath);

/// Runs one Lottie fixture end-to-end (compileLottie -> Context -> update(time) -> render ->
/// SwRenderer) and returns its rendered RGBA8 buffer (tightly packed, width*height*4 bytes).
/// Returns an empty vector on failure (already reported to stdout). `lottieDir` is the directory
/// `fx.jsonPath` is relative to (also used as compileLottie's/FileAssetResolver's rootDir).
std::vector<uint8_t> renderLottieFixture(const LottieFixture& fx, const std::string& lottieDir);

// ---------------------------------------------------------------------------------------------
// UI (`.xt` document) golden fixture loader (docs/tasks/T-27.md). Like the Lottie loader above,
// but runs compileDocument -> Context(NullTextShaper, FileAssetResolver) -> update(time) ->
// render -> SwRenderer against a hand-written `.xt` fixture, exercising the Element Painter path
// (background/border/img/overflow-clip/z-index/visibility) end to end. Implemented in
// golden_ui.cpp.
// ---------------------------------------------------------------------------------------------

struct UiFixture {
    std::string name;
    std::string xtPath;  // Relative to the ui fixtures directory (manifest's own directory).
    int width = 0;
    int height = 0;
    // Seconds passed to the Context::update() that runs *after* the event script (0 = no such
    // extra update; docs/tasks/T-33b.md "事件腳本文法").
    float time = 0;
    xt::Color clearColor{1, 1, 1, 1};
    // Optional `.events` script, relative to the ui fixtures directory. Empty = none.
    std::string eventsPath;
    // T-203 (docs/design/18-effects.md §18.7): the manifest line carries `gl-only` -- the fixture
    // uses a `.xtfx` custom effect, which SW never runs (it applies the fallback), so xt_golden
    // skips it and xt_gl_tests compares it through xt::gl instead.
    bool glOnly = false;
};

/// Parses `tests/golden/ui/manifest.txt`: one fixture per non-blank, non-'#'-comment line,
/// whitespace-separated
/// `<name> <xt relative path> <width> <height> <time> [clearColor hex] [events file]`.
/// `clearColor` defaults to "ffffff" (opaque white, matching docs/tasks/T-27.md's fixture table)
/// when omitted. The optional trailing `.events` script (docs/tasks/T-33b.md) may take either
/// of the last two columns -- a token ending in ".events" is always the script, anything else
/// is the clear color. A malformed line is skipped with a message to stdout and counted in
/// manifestErrors().
std::vector<UiFixture> loadUiManifest(const std::string& manifestPath);

/// Runs one UI fixture end-to-end (compileDocument -> Context -> update(time) -> render ->
/// SwRenderer) and returns its rendered RGBA8 buffer (tightly packed, width*height*4 bytes).
/// Returns an empty vector on failure (already reported to stdout). `uiDir` is the directory
/// `fx.xtPath` is relative to (also used as compileDocument's/FileAssetResolver's rootDir).
std::vector<uint8_t> renderUiFixture(const UiFixture& fx, const std::string& uiDir);

/// T-203: draws a fixture's CommandList (`fx.width` x `fx.height`, cleared to `fx.clearColor`)
/// and returns top-down RGBA8; the default backend of renderUiFixture() is xt::sw.
using UiBackend = std::function<std::vector<uint8_t>(const UiFixture& fx, const xt::CommandList& cmds,
                                                     xt::TextShaper* shaper, xt::AssetResolver* assets,
                                                     const xt::IrView* ir)>;

/// T-203: renderUiFixture() with `backend` drawing the final CommandList (xt_gl_tests' gl-only
/// fixtures).
std::vector<uint8_t> renderUiFixture(const UiFixture& fx, const std::string& uiDir, const UiBackend& backend);

/// Runs a UI fixture's `.events` script (docs/tasks/T-33b.md "事件腳本文法") against an already
/// loaded Context/Document: each non-`update` line pushes one input event and runs `update(0)` so
/// it is dispatched in its own frame. Returns false on the first bad line, with `err` set to
/// "<file>:<line>: <message>" (which fails the fixture). Implemented in golden_ui.cpp.
bool runEventScript(xt::Context& ctx, xt::Document& doc, const std::string& path, std::string& err);

// ---------------------------------------------------------------------------------------------
// Partial-redraw equivalence check (docs/design/23-host-integration.md §23.3, docs/tasks/T-99.md
// test 1). Implemented in golden_partial.cpp.
// ---------------------------------------------------------------------------------------------

/// Runs every animated `.xt` fixture of the partial-redraw check for 60 frames down both a
/// full-redraw and a damage-driven path and compares the final buffers byte for byte (tolerance
/// 0). `uiDir` is the UI fixture directory; `filter` matches fixture names the same way xt_golden's
/// --filter does. Adds to `ran`/`failed` and prints one [PASS]/[FAIL] line per case.
void runPartialRedrawChecks(const std::string& uiDir, const std::string& filter, int& ran, int& failed);

}  // namespace xt::golden
