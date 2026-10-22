#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "golden_common.h"
#include "xt/asset_resolver.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/scene_instance.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// xt_golden: standalone executable running the "Painter -> SwRenderer -> PNG -> compare against
/// expected" pipeline for every registered SceneBuilder fixture (Docs/tasks/T-13.md), the
/// "compileLottie -> Context -> update(time) -> render -> SwRenderer -> PNG -> compare" pipeline
/// for every fixture listed in Tests/golden/lottie/manifest.txt (Docs/tasks/T-17.md), and the
/// "compileDocument -> Context -> update(time) -> render -> SwRenderer -> PNG -> compare" pipeline
/// for every fixture listed in Tests/golden/ui/manifest.txt (Docs/tasks/T-27.md).
///
/// Usage: xt_golden [--update] [--filter <substring>]
///   --update           Write the rendered image to expected/<name>.png instead of comparing.
///   --filter <substr>  Only run fixtures whose name contains <substr>. Applies to both the
///                      SceneBuilder fixtures and the Lottie manifest fixtures.
/// Environment (T-267d): XT_GOLDEN_CHANNEL (default 2) and XT_GOLDEN_RATIO (default 0) set the
/// compare tolerance for one run; XT_GOLDEN_VERBOSE=1 adds a [DIFF] line for every fixture that
/// passes with differing pixels. Every line goes to stdout.
///
/// Not covered by Docs/tasks/T-13.md (conservative choices made here):
///   - Working directory for build/golden-out/: interpreted relative to the process's current
///     working directory (matches running `xt_golden` from the repository root or from CTest's
///     working directory; the directory is created on demand either way).
///   - An expected PNG whose dimensions do not match the fixture's width/height is treated as a
///     failure rather than attempting a partial comparison.
///   - Diff-image grayscale channel: computed from the *actual* (freshly rendered) image, not the
///     expected one.
///   - --filter matching no fixture at all, or an IR/instantiation failure inside a fixture, exits
///     non-zero (2) rather than being silently reported as zero failures.

namespace {

namespace fs = std::filesystem;
using namespace xt;
using namespace xt::golden;

/// Runs one fixture end-to-end and returns its rendered RGBA8 buffer (tightly packed,
/// width*height*4 bytes, sized to the fixture's own width/height -- every T-13 fixture uses
/// dpr == 1, so device pixels equal logical pixels). Returns an empty vector on failure (already
/// reported).
std::vector<uint8_t> renderFixture(const GoldenFixture& fx) {
    compile::IrWriter writer;
    fx.build(writer);
    std::vector<uint8_t> bytes = writer.finish();

    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    if (!viewResult.ok()) {
        report("xt_golden: fixture '%s' produced an invalid IR blob (status=%s)\n", fx.name,
               statusName(viewResult.status()));
        return {};
    }
    const IrView& view = viewResult.value();

    NodeArena arena;
    NodeId container = arena.create(NodeKind::Group);
    SceneInstance inst;
    if (instantiateScene(view, /*scene=*/0, arena, container, inst) != Status::kOk) {
        report("xt_golden: fixture '%s' failed to instantiate scene 0\n", fx.name);
        return {};
    }
    updateWorldTransforms(arena, inst.root, /*force=*/true);

    Painter painter;
    CommandList cmds;
    painter.paint(arena, inst.root, cmds);
    cmds.viewport = Rect{0, 0, static_cast<float>(fx.width), static_cast<float>(fx.height)};
    cmds.devicePixelRatio = fx.dpr;

    const int pixelWidth = static_cast<int>(static_cast<float>(fx.width) * fx.dpr);
    const int pixelHeight = static_cast<int>(static_cast<float>(fx.height) * fx.dpr);
    std::vector<uint8_t> buf(static_cast<size_t>(pixelWidth) * static_cast<size_t>(pixelHeight) * 4, 0);

    sw::SwRenderer renderer;
    sw::SwTarget target{buf.data(), pixelWidth, pixelHeight, pixelWidth * 4};
    renderer.setTarget(target);
    renderer.clear(fx.clearColor);
    // T-245: embedded image assets (scene_image_mesh) resolve through the IR; a fixture without
    // images never asks the resolver anything, so its output is unchanged.
    FileAssetResolver resolver(std::string(XT_GOLDEN_DIR), sw::decodeImageBytes);
    renderer.render(cmds, nullptr, &resolver, &view);
    return buf;
}

bool matchesFilter(const std::string& name, const std::string& filter) {
    return filter.empty() || name.find(filter) != std::string::npos;
}

/// T-267a (review 2026-10-02 L6): every comparison here is SW against an SW-rendered golden (the
/// Lottie expected images are SW output too, Docs/tasks/T-17.md; lottie-web is only compared by
/// Tools/LottieReference). SW is deterministic, so no pixel may differ by more than 2/255 per
/// channel. The 0.5% ratio of 15 §15.3 was letting a caret or a 1 px focus ring vanish unnoticed.
/// T-267d: the channel tolerance (2) and the ratio (0) are the defaults of swTolerance(), which
/// XT_GOLDEN_CHANNEL / XT_GOLDEN_RATIO override for one run.
///
/// The allowed share of pixels beyond the channel tolerance: the run's ratio (XT_GOLDEN_RATIO, strict 0
/// when unset; T-267d). The three Lottie fixtures that T-267a had exempted were regenerated at the
/// 2026-10-07 merge (stale since 211fd93e, T-236), so no fixture is exempt any more.
float swMaxDiffRatio(const std::string& /*name*/, const SwTolerance& tolerance) {
    return tolerance.ratio;
}

/// Compares one rendered fixture against its golden and reports the result: [PASS] (plus a [DIFF]
/// line under XT_GOLDEN_VERBOSE=1 when some pixel differs at all), or [FAIL] with the pixel count
/// and the largest channel delta, writing <name>.actual.png and <name>.diff.png to `outDir`.
/// Returns whether the fixture passed.
bool compareAndReport(const std::string& name, const std::vector<uint8_t>& actual,
                      const std::vector<uint8_t>& expected, int width, int height, const fs::path& outDir) {
    const SwTolerance tolerance = swTolerance();
    const float ratio = swMaxDiffRatio(name, tolerance);
    const GoldenResult result =
        compareRgba(actual.data(), expected.data(), width, height, tolerance.channel, ratio);
    if (result.pass) {
        report("[PASS] %s (diffPixels=%u maxChannelDiff=%u)\n", name.c_str(), result.diffPixels,
               result.maxChannelDiff);
        if (goldenVerbose() && result.maxChannelDiff > 0) {
            report("%s\n", describeDiff("[DIFF]", name, result, ratio, tolerance.channel).c_str());
        }
        return true;
    }
    report("%s\n", describeDiff("[FAIL]", name, result, ratio, tolerance.channel).c_str());
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const fs::path actualPath = outDir / (name + ".actual.png");
    const fs::path diffPath = outDir / (name + ".diff.png");
    sw::writePng(actualPath.string(), actual.data(), width, height, width * 4);
    writeDiffPng(diffPath.string(), actual.data(), expected.data(), width, height, tolerance.channel);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    bool update = false;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--update") {
            update = true;
        } else if (arg == "--filter") {
            if (i + 1 >= argc) {
                report("xt_golden: --filter requires an argument\n");
                return 2;
            }
            filter = argv[++i];
        } else {
            report("xt_golden: unknown argument '%s'\n", arg.c_str());
            return 2;
        }
    }

    const fs::path expectedDir(XT_GOLDEN_DIR);
    const fs::path outDir = fs::path("build") / "golden-out";

    int ran = 0;
    int failed = 0;
    for (const GoldenFixture& fx : fixtures()) {
        const std::string name = fx.name;
        if (!matchesFilter(name, filter)) {
            continue;
        }
        ++ran;

        std::vector<uint8_t> actual = renderFixture(fx);
        if (actual.empty()) {
            report("[FAIL] %s: fixture could not be rendered\n", fx.name);
            ++failed;
            continue;
        }

        const fs::path expectedPath = expectedDir / (name + ".png");

        if (update) {
            std::error_code ec;
            fs::create_directories(expectedDir, ec);
            if (!sw::writePng(expectedPath.string(), actual.data(), fx.width, fx.height, fx.width * 4)) {
                report("[FAIL] %s: could not write expected image to %s\n", fx.name,
                       expectedPath.string().c_str());
                ++failed;
                continue;
            }
            report("[UPDATED] %s -> %s\n", fx.name, expectedPath.string().c_str());
            continue;
        }

        std::vector<uint8_t> expected;
        int expectedWidth = 0;
        int expectedHeight = 0;
        if (!sw::readPng(expectedPath.string(), expected, expectedWidth, expectedHeight)) {
            report("[FAIL] %s: expected image missing at %s (run 'xt_golden --update' to generate it)\n",
                   fx.name, expectedPath.string().c_str());
            ++failed;
            continue;
        }
        if (expectedWidth != fx.width || expectedHeight != fx.height) {
            report("[FAIL] %s: expected image is %dx%d, fixture renders %dx%d\n", fx.name, expectedWidth,
                   expectedHeight, fx.width, fx.height);
            ++failed;
            continue;
        }

        if (!compareAndReport(fx.name, actual, expected, fx.width, fx.height, outDir)) {
            ++failed;
        }
    }

    const fs::path lottieDir(XT_GOLDEN_LOTTIE_DIR);
    const std::vector<LottieFixture> lottieFixtures =
        loadLottieManifest((lottieDir / "manifest.txt").string());
    for (const LottieFixture& fx : lottieFixtures) {
        if (!matchesFilter(fx.name, filter)) {
            continue;
        }
        ++ran;

        std::vector<uint8_t> actual = renderLottieFixture(fx, lottieDir.string());
        if (actual.empty()) {
            report("[FAIL] %s: fixture could not be rendered\n", fx.name.c_str());
            ++failed;
            continue;
        }

        const fs::path expectedPath = expectedDir / (fx.name + ".png");

        if (update) {
            std::error_code ec;
            fs::create_directories(expectedDir, ec);
            if (!sw::writePng(expectedPath.string(), actual.data(), fx.width, fx.height, fx.width * 4)) {
                report("[FAIL] %s: could not write expected image to %s\n", fx.name.c_str(),
                       expectedPath.string().c_str());
                ++failed;
                continue;
            }
            report("[UPDATED] %s -> %s\n", fx.name.c_str(), expectedPath.string().c_str());
            continue;
        }

        std::vector<uint8_t> expected;
        int expectedWidth = 0;
        int expectedHeight = 0;
        if (!sw::readPng(expectedPath.string(), expected, expectedWidth, expectedHeight)) {
            report("[FAIL] %s: expected image missing at %s (run 'xt_golden --update' to generate it)\n",
                   fx.name.c_str(), expectedPath.string().c_str());
            ++failed;
            continue;
        }
        if (expectedWidth != fx.width || expectedHeight != fx.height) {
            report("[FAIL] %s: expected image is %dx%d, fixture renders %dx%d\n", fx.name.c_str(),
                   expectedWidth, expectedHeight, fx.width, fx.height);
            ++failed;
            continue;
        }

        if (!compareAndReport(fx.name, actual, expected, fx.width, fx.height, outDir)) {
            ++failed;
        }
    }

    const fs::path uiDir(XT_GOLDEN_UI_DIR);
    const std::vector<UiFixture> uiFixtures = loadUiManifest((uiDir / "manifest.txt").string());
    for (const UiFixture& fx : uiFixtures) {
        if (!matchesFilter(fx.name, filter)) {
            continue;
        }
        if (fx.glOnly) {
            // T-203 (18.7): a custom effect's shader only runs on GL; xt_gl_tests compares it.
            report("[SKIP] %s (gl-only)\n", fx.name.c_str());
            continue;
        }
        ++ran;

        std::vector<uint8_t> actual = renderUiFixture(fx, uiDir.string());
        if (actual.empty()) {
            report("[FAIL] %s: fixture could not be rendered\n", fx.name.c_str());
            ++failed;
            continue;
        }

        const fs::path expectedPath = expectedDir / (fx.name + ".png");

        if (update) {
            std::error_code ec;
            fs::create_directories(expectedDir, ec);
            if (!sw::writePng(expectedPath.string(), actual.data(), fx.width, fx.height, fx.width * 4)) {
                report("[FAIL] %s: could not write expected image to %s\n", fx.name.c_str(),
                       expectedPath.string().c_str());
                ++failed;
                continue;
            }
            report("[UPDATED] %s -> %s\n", fx.name.c_str(), expectedPath.string().c_str());
            continue;
        }

        std::vector<uint8_t> expected;
        int expectedWidth = 0;
        int expectedHeight = 0;
        if (!sw::readPng(expectedPath.string(), expected, expectedWidth, expectedHeight)) {
            report("[FAIL] %s: expected image missing at %s (run 'xt_golden --update' to generate it)\n",
                   fx.name.c_str(), expectedPath.string().c_str());
            ++failed;
            continue;
        }
        if (expectedWidth != fx.width || expectedHeight != fx.height) {
            report("[FAIL] %s: expected image is %dx%d, fixture renders %dx%d\n", fx.name.c_str(),
                   expectedWidth, expectedHeight, fx.width, fx.height);
            ++failed;
            continue;
        }

        if (!compareAndReport(fx.name, actual, expected, fx.width, fx.height, outDir)) {
            ++failed;
        }
    }

    // Docs/tasks/T-99.md test 1: partial redraw must match a full redraw byte for byte. Skipped
    // in --update mode, which regenerates expected images and has nothing to compare against.
    if (!update) {
        runPartialRedrawChecks(uiDir.string(), filter, ran, failed);
    }

    // T-267a (review L22): an unreadable manifest or a malformed line drops fixtures; that is a failure.
    if (manifestErrors() > 0) {
        report("[FAIL] %d manifest problem(s): unreadable manifest or malformed line (see above)\n",
               manifestErrors());
        failed += manifestErrors();
    }
    if (ran == 0) {
        report("xt_golden: no fixtures matched filter '%s'\n", filter.c_str());
        return 2;
    }
    if (failed > 0) {
        report("xt_golden: %d/%d fixture(s) failed\n", failed, ran);
        return 1;
    }
    if (update) {
        report("xt_golden: updated %d fixture(s)\n", ran);
    } else {
        report("xt_golden: all %d fixture(s) passed\n", ran);
    }
    return 0;
}
