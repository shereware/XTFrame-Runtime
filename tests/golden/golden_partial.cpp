#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "golden_common.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#ifdef XT_GOLDEN_TEXT
#include "xt/text/ft_text_shaper.h"
#endif
#include "xt/types.h"

/// Partial-redraw equivalence check (docs/design/23-host-integration.md §23.3, docs/tasks/T-99.md
/// test 1). For each animated `.xt` fixture below, 60 frames are driven through one Context and
/// rasterized twice from the *same* CommandList:
///   A: clear() + SwRenderer::render(list) every frame (the full-redraw reference);
///   B: full for the first frame (and any frame whose DamageList comes back `full`), otherwise
///      clearRects(damage.rects) + render(list, ..., &damage).
/// After frame 60 the two buffers must be byte-identical -- both went through the same rasterizer,
/// so the tolerance is 0. A difference means the damage rectangles or the write mask are wrong,
/// not that the images are "close enough".
///
/// Not covered by docs/tasks/T-99.md (choices made here):
///   - One Context feeds both paths, so A and B can never diverge because of a different
///     CommandList; the DamageList comes from that single Context::render(list, &damage).
///   - The frames run at a fixed 1/60 s, with frame 0 at dt == 0 (what every other UI fixture and
///     the examples do).
///   - A failing case writes `build/golden-out/<name>.partial.{full,partial,diff}.png` next to the
///     other golden failure artifacts.
///   - The per-path render times are printed (not asserted): a timing threshold would be flaky on
///     a shared CI machine, and the sheet only asks for the numbers in the report.

namespace xt::golden {

namespace {

namespace fs = std::filesystem;

/// The animated fixtures of tests/golden/ui/ this check runs; two of them are effect-driven
/// (docs/tasks/T-99.md "挑 3 個，含一個帶 `effect:` 的", plus T-130's backdrop-blur case).
struct PartialCase {
    const char* name;
    const char* xtFile;
    int width;
    int height;
    xt::Color clearColor;
};

constexpr PartialCase kPartialCases[] = {
    {"partial_ui_css_animation_mid", "ui_css_animation_mid.xt", 256, 256, xt::Color{1, 1, 1, 1}},
    {"partial_ui_anim_scene_time", "ui_anim_scene_time.xt", 256, 256, xt::Color{1, 1, 1, 1}},
    {"partial_ui_fx_shadow_anim", "ui_fx_shadow_anim.xt", 256, 256, xt::Color{1, 1, 1, 1}},
    // T-130: a glass panel sliding over a colour grid. BackdropBlur reads what lies below it, so
    // this case only stays byte-identical if computeDamage unions the panel's rectangle with the
    // dirty rectangles it meets (docs/design/18-effects.md §18.6).
    {"partial_ui_fx_backdrop_anim", "ui_fx_backdrop_anim.xt", 256, 256, xt::Color{1, 1, 1, 1}},
};

constexpr uint32_t kFrames = 60;

double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

/// Index of the first differing byte of two equally sized buffers, or `a.size()` when equal.
size_t firstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return a.size();
}

}  // namespace

void runPartialRedrawChecks(const std::string& uiDir, const std::string& filter, int& ran, int& failed) {
    for (const PartialCase& pc : kPartialCases) {
        const std::string name = pc.name;
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            continue;
        }
        ++ran;

        const fs::path xtPath = fs::path(uiDir) / pc.xtFile;
        xt::compile::CompileOptions options;
        options.rootDir = xtPath.parent_path().string();
        options.strict = false;
        xt::compile::CompileResult result = xt::compile::compileDocument(xtPath.string(), options);
        if (!result.ok()) {
            report("[FAIL] %s: could not compile '%s'\n", pc.name, xtPath.string().c_str());
            ++failed;
            continue;
        }

#ifdef XT_GOLDEN_TEXT
        xt::text::FtTextShaper shaper;
#else
        xt::NullTextShaper shaper;
#endif
        xt::FileAssetResolver resolver(xtPath.parent_path().string(), xt::sw::decodeImageBytes);
        xt::Context ctx(xt::ContextConfig{&shaper, &resolver});
        ctx.setViewport(xt::Rect{0, 0, static_cast<float>(pc.width), static_cast<float>(pc.height)}, 1.0f);
        xt::Result<xt::DocumentHandle> handle =
            ctx.loadDocument(xt::IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true);
        if (!handle.ok()) {
            report("[FAIL] %s: failed to load (status=%s)\n", pc.name, xt::statusName(handle.status()));
            ++failed;
            continue;
        }
        const xt::Document* doc = ctx.document(handle.value());
        const xt::IrView* irView = doc != nullptr ? &doc->ir() : nullptr;

        const size_t byteCount = static_cast<size_t>(pc.width) * static_cast<size_t>(pc.height) * 4;
        std::vector<uint8_t> fullBuf(byteCount, 0);
        std::vector<uint8_t> partialBuf(byteCount, 0);
        const int strideBytes = pc.width * 4;
        xt::sw::SwRenderer fullRenderer;
        xt::sw::SwRenderer partialRenderer;
        fullRenderer.setTarget(xt::sw::SwTarget{fullBuf.data(), pc.width, pc.height, strideBytes});
        partialRenderer.setTarget(xt::sw::SwTarget{partialBuf.data(), pc.width, pc.height, strideBytes});

        xt::CommandList list;
        xt::DamageList damage;
        const float dt = 1.0f / 60.0f;
        double fullMs = 0;
        double partialMs = 0;
        uint32_t fullFrames = 0;
        uint32_t executed = 0;
        uint32_t skipped = 0;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            ctx.update(frame == 0 ? 0.0f : dt);
            // No list.clear() here: Context::render() diffs against whatever `out` still holds
            // from the previous frame (§23.2 step 1) and clears it itself.
            ctx.render(list, &damage);

            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            fullRenderer.clear(pc.clearColor);
            fullRenderer.render(list, &shaper, &resolver, irView);
            fullMs += elapsedMs(start);

            const bool partial = frame != 0 && !damage.full;
            start = std::chrono::steady_clock::now();
            if (partial) {
                partialRenderer.clearRects(pc.clearColor, damage.rects);
                partialRenderer.render(list, &shaper, &resolver, irView, nullptr, &damage);
            } else {
                ++fullFrames;
                partialRenderer.clear(pc.clearColor);
                partialRenderer.render(list, &shaper, &resolver, irView);
            }
            partialMs += elapsedMs(start);
            executed += partialRenderer.lastExecutedDrawCommands();
            skipped += partialRenderer.lastSkippedDrawCommands();
        }

        const size_t diff = firstDifference(fullBuf, partialBuf);
        if (diff == fullBuf.size()) {
            report(
                "[PASS] %s (%u frames, %u full, draws executed=%u skipped=%u, full %.3f ms/frame, "
                "partial %.3f ms/frame)\n",
                pc.name, kFrames, fullFrames, executed, skipped, fullMs / static_cast<double>(kFrames),
                partialMs / static_cast<double>(kFrames));
            continue;
        }

        const size_t pixel = diff / 4;
        report(
            "[FAIL] %s: partial redraw differs from a full redraw at byte %zu "
            "(pixel %zu,%zu channel %zu: full=%u partial=%u)\n",
            pc.name, diff, pixel % static_cast<size_t>(pc.width), pixel / static_cast<size_t>(pc.width),
            diff % 4, fullBuf[diff], partialBuf[diff]);
        std::error_code ec;
        const fs::path outDir = fs::path("build") / "golden-out";
        fs::create_directories(outDir, ec);
        xt::sw::writePng((outDir / (name + ".full.png")).string(), fullBuf.data(), pc.width, pc.height,
                         strideBytes);
        xt::sw::writePng((outDir / (name + ".partial.png")).string(), partialBuf.data(), pc.width, pc.height,
                         strideBytes);
        writeDiffPng((outDir / (name + ".diff.png")).string(), fullBuf.data(), partialBuf.data(), pc.width,
                     pc.height, /*channelTolerance=*/0);
        ++failed;
    }
}

}  // namespace xt::golden
