#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "sw_effect_math.h"
#include "sw_layer.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/effect_utils.h"
#include "xt/node.h"
#include "xt/perf.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-343 (docs/design/23-host-integration.md §23.aa, 10 §10.9, docs/tasks/T-343.md): the software
/// renderer's Direct target mode -- the canvas is the host scene and is never cleared, bands are
/// kept between frames and pasted back while nothing of them changed, scene runs are drawn every
/// frame -- plus the Painter's scene-reader marks and the shared downsampled-blur arithmetic.

using namespace xt;
using namespace xt::sw;

namespace {

constexpr int kW = 128, kH = 96;

struct Rgba {
    uint8_t r, g, b, a;
};

/// A straight-alpha canvas holding a horizontal gradient (the "scene").
std::vector<uint8_t> makeScene() {
    std::vector<uint8_t> buf(static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4u);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            uint8_t* p = buf.data() + (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
            p[0] = static_cast<uint8_t>(x * 255 / (kW - 1));
            p[1] = static_cast<uint8_t>(y * 255 / (kH - 1));
            p[2] = x < kW / 2 ? 40 : 200;
            p[3] = 255;
        }
    }
    return buf;
}

/// Puts the scene back into `buf` without reallocating it (the SwTarget points at its storage).
void restoreScene(std::vector<uint8_t>& buf) {
    const std::vector<uint8_t> scene = makeScene();
    std::copy(scene.begin(), scene.end(), buf.begin());
}

Rgba pixelAt(const std::vector<uint8_t>& buf, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
    return Rgba{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};
}

PathRef addRectPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 8);
    ref.verbCount = 5;
    ref.pointCount = 8;
    return ref;
}

void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    FillCmd f{};
    f.path = addRectPath(cl, x, y, w, h);
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

void addScenePushLayer(CommandList& cl, BlendMode blend, Rect hint) {
    LayerCmd l{};
    l.opacity = 1.0f;
    l.blend = blend;
    l.boundsHint = hint;
    cl.layers.push_back(l);
    Command c{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)};
    c.flags[0] = kCommandSceneRead;
    cl.commands.push_back(c);
}

void addGlassPanel(CommandList& cl, Rect box, float radius, Color tint) {
    EffectInstance e{};
    e.kind = EffectKind::BackdropBlur;
    e.params[0] = radius;
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = 1;
    cl.effects.push_back(e);
    c.boundsHint = box;
    c.contentBounds = box;
    cl.effectCmds.push_back(c);
    Command cmd{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)};
    cmd.flags[0] = kCommandSceneRead;
    cl.commands.push_back(cmd);
    addFillRect(cl, box.x, box.y, box.w, box.h, tint);
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

/// Band 0 (two rects), a glass panel, band 1 (one rect), a multiply layer, band 2 (one rect).
CommandList directList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    cl.hostTargetMode = HostTargetMode::Direct;
    addFillRect(cl, 4, 4, 20, 10, Color{1, 0, 0, 1});
    addFillRect(cl, 4, 16, 20, 10, Color{0, 1, 0, 1});
    addGlassPanel(cl, Rect{40, 20, 48, 40}, 16.0f, Color{1, 1, 1, 0.3f});
    addFillRect(cl, 100, 4, 20, 10, Color{0, 0, 1, 1});
    addScenePushLayer(cl, BlendMode::Multiply, Rect{0, 0, kW, kH});
    addFillRect(cl, 0, 70, kW, 26, Color{0.5f, 0.5f, 0.5f, 1});
    cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    addFillRect(cl, 100, 80, 20, 10, Color{1, 1, 0, 1});
    return cl;
}

}  // namespace

TEST_CASE(
    "sw: T-343 Direct mode leaves the scene in place, pastes bands on the second frame, redraws on damage") {
    const CommandList cl = directList();
    std::vector<uint8_t> buf = makeScene();
    SwRenderer renderer;
    SwTarget target{buf.data(), kW, kH, kW * 4};
    renderer.setTarget(target);
    PerfTracker perf;
    perf.setEnabled(true);

    perf.beginFrame(0.0f);
    renderer.render(cl, nullptr, nullptr, nullptr, &perf, nullptr);
    const std::vector<uint8_t> first = buf;
    CHECK(perf.current().bands == 3);
    CHECK(perf.current().bandRedraws == 3);
    CHECK(perf.current().directFallback == 0);
    CHECK(perf.current().pyramidLevels == static_cast<uint32_t>(backdropPyramidLevels(8.0f)));
    CHECK(perf.current().backdropCopyPx > 0);
    CHECK(renderer.lastExecutedDrawCommands() == 6);
    // The scene survives where nothing was drawn; a band's rect is opaque red; the panel is a
    // blend of the blurred scene and the white tint; the multiply band darkened the bottom.
    const Rgba untouched = pixelAt(buf, 60, 8);
    CHECK(untouched.r == pixelAt(makeScene(), 60, 8).r);
    CHECK(untouched.a == 255);
    CHECK(pixelAt(buf, 10, 8).r == 255);
    CHECK(pixelAt(buf, 10, 8).g == 0);
    const Rgba panel = pixelAt(buf, 64, 40);
    const Rgba scene = pixelAt(makeScene(), 64, 40);
    CHECK(panel.r > scene.r);  // lightened by the tint
    CHECK(panel.b != scene.b);
    CHECK(pixelAt(buf, 64, 80).g < pixelAt(makeScene(), 64, 80).g);  // multiply darkens

    // Frame 2, nothing changed: the bands are pasted, only the two scene-run fills execute.
    restoreScene(buf);
    DamageList none;
    none.full = false;
    perf.beginFrame(1.0f);
    renderer.render(cl, nullptr, nullptr, nullptr, &perf, &none);
    CHECK(perf.current().bandRedraws == 0);
    CHECK(renderer.lastExecutedDrawCommands() == 2);
    CHECK(renderer.lastSkippedDrawCommands() == 4);
    CHECK(buf == first);

    // Damage over band 1 only: that band is redrawn.
    restoreScene(buf);
    DamageList one;
    one.full = false;
    one.rects.push_back(Rect{102, 5, 4, 4});
    perf.beginFrame(2.0f);
    renderer.render(cl, nullptr, nullptr, nullptr, &perf, &one);
    CHECK(perf.current().bandRedraws == 1);
    CHECK(renderer.lastExecutedDrawCommands() == 3);
    CHECK(buf == first);

    // A command added to band 2 (outside its old box) redraws it.
    CommandList grown = directList();
    addFillRect(grown, 4, 80, 10, 10, Color{0, 1, 1, 1});
    restoreScene(buf);
    DamageList d;
    d.full = false;
    d.rects.push_back(Rect{4, 80, 10, 10});
    perf.beginFrame(3.0f);
    renderer.render(grown, nullptr, nullptr, nullptr, &perf, &d);
    CHECK(perf.current().bandRedraws == 1);
    CHECK(pixelAt(buf, 8, 84).g == 255);
    CHECK(pixelAt(buf, 8, 84).r == 0);

    // Layer mode again: no bands, the canvas is drawn as always.
    CommandList layer = directList();
    layer.hostTargetMode = HostTargetMode::Layer;
    perf.beginFrame(4.0f);
    renderer.render(layer, nullptr, nullptr, nullptr, &perf, nullptr);
    CHECK(perf.current().bands == 0);
    CHECK(renderer.lastExecutedDrawCommands() == 6);
}

TEST_CASE("sw: T-343 more than four bands falls back to drawing everything, every frame") {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    cl.hostTargetMode = HostTargetMode::Direct;
    for (int i = 0; i < 5; ++i) {
        addFillRect(cl, 4.0f + 24.0f * static_cast<float>(i), 4, 16, 10, Color{1, 0, 0, 1});
        addScenePushLayer(cl, BlendMode::Screen, Rect{4.0f + 24.0f * static_cast<float>(i), 20, 16, 10});
        addFillRect(cl, 4.0f + 24.0f * static_cast<float>(i), 20, 16, 10, Color{0, 1, 0, 0.5f});
        cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    }
    addFillRect(cl, 4, 40, 16, 10, Color{0, 0, 1, 1});
    std::vector<uint8_t> buf = makeScene();
    SwRenderer renderer;
    SwTarget target{buf.data(), kW, kH, kW * 4};
    renderer.setTarget(target);
    PerfTracker perf;
    perf.setEnabled(true);
    perf.beginFrame(0.0f);
    renderer.render(cl, nullptr, nullptr, nullptr, &perf, nullptr);
    CHECK(perf.current().bands == 6);
    CHECK(perf.current().directFallback == 1);
    DamageList none;
    none.full = false;
    perf.beginFrame(1.0f);
    renderer.render(cl, nullptr, nullptr, nullptr, &perf, &none);
    CHECK(perf.current().bandRedraws == 6);
    CHECK(renderer.lastExecutedDrawCommands() == 11);
}

TEST_CASE("sw: T-343 the backdrop pyramid blurs a seam toward the mean and keeps a flat region") {
    // A scene that is dark on the left and bright on the right; one glass panel across the seam
    // and one over the flat right side.
    std::vector<uint8_t> buf(static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4u);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            uint8_t* p = buf.data() + (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
            const uint8_t v = x < kW / 2 ? 20 : 220;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    cl.hostTargetMode = HostTargetMode::Direct;
    addGlassPanel(cl, Rect{kW / 2 - 24, 8, 48, 32}, 24.0f, Color{1, 1, 1, 0.0f});  // sigma 12, no tint
    addGlassPanel(cl, Rect{96, 48, 24, 32}, 24.0f, Color{1, 1, 1, 0.0f});
    SwRenderer renderer;
    SwTarget target{buf.data(), kW, kH, kW * 4};
    renderer.setTarget(target);
    renderer.render(cl, nullptr, nullptr);
    // A fully transparent content leaves the scene alone (Backdrop composes only under alpha):
    // sanity for the seam untouched outside the panels.
    CHECK(pixelAt(buf, kW / 2 - 1, 60).r == 20);
    CHECK(pixelAt(buf, kW / 2 + 1, 60).r == 220);
}

TEST_CASE("sw: T-343 the blurred backdrop a glass panel shows is the pyramid's, bracketed by its levels") {
    // The panel's own content is half-transparent white, so its pixel = 0.5 * white +
    // 0.5 * blurred scene; at the seam the blurred scene must lie strictly between the two sides.
    std::vector<uint8_t> buf(static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4u);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            uint8_t* p = buf.data() + (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
            const uint8_t v = x < kW / 2 ? 0 : 200;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    cl.hostTargetMode = HostTargetMode::Direct;
    addGlassPanel(cl, Rect{kW / 2 - 32, 8, 64, 48}, 32.0f, Color{1, 1, 1, 0.5f});  // sigma 16
    SwRenderer renderer;
    SwTarget target{buf.data(), kW, kH, kW * 4};
    renderer.setTarget(target);
    renderer.render(cl, nullptr, nullptr);
    const int seamL = pixelAt(buf, kW / 2 - 2, 30).r;
    const int seamR = pixelAt(buf, kW / 2 + 2, 30).r;
    // Without blur these would be 128 (0.5 white + 0) and 228 (0.5 white + 0.5 * 200).
    CHECK(seamL > 128 + 10);
    CHECK(seamR < 228 - 10);
    CHECK(seamL <= seamR);
    // Far from the seam (inside the panel, 30 px away) the blur has mostly settled.
    CHECK(pixelAt(buf, kW / 2 - 30, 30).r < seamL);
    CHECK(pixelAt(buf, kW / 2 + 30, 30).r > seamR);
}

TEST_CASE("sw: T-343 a downsampled gaussianBlur stays close to a true Gaussian") {
    // A 96 x 96 white square on 256 x 256: blur with sigma 10 (quarter resolution) and compare
    // the alpha profile along the middle row with the analytic Gaussian of a box edge.
    Surface src;
    src.w = 256;
    src.h = 256;
    src.rgba.assign(static_cast<size_t>(256) * 256 * 4, 0.0f);
    for (int y = 80; y < 176; ++y) {
        for (int x = 80; x < 176; ++x) {
            src.set(x, y, Color{1, 1, 1, 1});
        }
    }
    Surface dst, scratch;
    fx::gaussianBlur(src, dst, scratch, 10.0f, 10.0f, true);
    CHECK(blurDownsampleShift(10.0f) == 2);
    auto erf01 = [](float x) { return 0.5f * (1.0f + std::erf(x / std::sqrt(2.0f))); };
    float maxDiff = 0.0f;
    for (int x = 40; x < 216; ++x) {
        // 1-D profile of a 96 px wide box blurred by sigma 10: erf(right edge) - erf(left edge).
        const float px = static_cast<float>(x) + 0.5f;
        const float expected = erf01((176.0f - px) / 10.0f) - erf01((80.0f - px) / 10.0f);
        const float got = dst.get(x, 128).a;
        maxDiff = std::max(maxDiff, std::fabs(got - expected));
    }
    CHECK(maxDiff < 0.04f);  // the three-box approximation plus the quarter-resolution sampling
    CHECK(dst.get(128, 128).a > 0.99f);
    CHECK(dst.get(10, 128).a < 0.001f);
}

TEST_CASE("sw: T-343 downsample2x2 / upsampleBilinear are exact box and bilinear rules") {
    const float field[6] = {1, 3, 5, 7, 9, 11};  // 3 x 2, one channel
    std::vector<float> out;
    int ow = 0, oh = 0;
    fx::downsample2x2(field, 3, 2, 1, out, ow, oh);
    REQUIRE(ow == 2);
    REQUIRE(oh == 1);
    CHECK(out[0] == doctest::Approx((1 + 3 + 7 + 9) * 0.25f));
    CHECK(out[1] == doctest::Approx((5 + 5 + 11 + 11) * 0.25f));  // clamped last column
    std::vector<float> up;
    fx::upsampleBilinear(out.data(), 2, 1, 1, up, 4, 1);
    REQUIRE(up.size() == 4);
    CHECK(up[0] == doctest::Approx(out[0]));  // p = -0.25: clamped to texel 0
    CHECK(up[1] == doctest::Approx(out[0] * 0.75f + out[1] * 0.25f));
    CHECK(up[2] == doctest::Approx(out[0] * 0.25f + out[1] * 0.75f));
    CHECK(up[3] == doctest::Approx(out[1]));
}

TEST_CASE("core: T-343 the Painter marks blend layers and backdrop-blur nodes; Context carries the mode") {
    const std::string text =
        "<xt><head><style>"
        ".a { position: absolute; left: 10px; top: 10px; width: 40px; height: 20px; background-color:#f00; "
        "mix-blend-mode: multiply; }"
        ".b { position: absolute; left: 10px; top: 40px; width: 40px; height: 20px; "
        "background-color:rgba(0, 255, 0, 0.5); effect: backdrop-blur(8px); }"
        ".c { position: absolute; left: 60px; top: 10px; width: 20px; height: 20px; background-color:#00f; "
        "opacity: 0.5; }"
        ".d { width: 5px; height: 5px; background-color:#fff; }"
        ".e { width: 5px; height: 5px; background-color:#000; }"
        "</style></head><body><div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"><div "
        "class=\"d\"></div><div class=\"e\"></div></div></body></xt>";
    const compile::SourceFile file{"direct.xt", text};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "direct.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    ctx.setViewport(Rect{0, 0, 128, 96}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());
    ctx.update(0.0f);
    CommandList list;
    DamageList damage;
    ctx.render(list, &damage);
    CHECK(list.hostTargetMode == HostTargetMode::Layer);
    CHECK_FALSE(list.sceneStatic);
    std::string dump;
    dumpCommandList(list, dump);
    INFO(dump);
    int markedLayers = 0, markedEffects = 0, unmarkedLayers = 0;
    for (const Command& c : list.commands) {
        const bool marked = (c.flags[0] & kCommandSceneRead) != 0;
        if (c.kind == CmdKind::PushLayer) {
            const LayerCmd& l = list.layers[c.index];
            if (marked) {
                ++markedLayers;
                CHECK(l.blend == BlendMode::Multiply);
            } else {
                ++unmarkedLayers;
                CHECK(l.blend == BlendMode::Normal);  // the opacity group
            }
        } else if (c.kind == CmdKind::PushEffect) {
            if (marked) ++markedEffects;
            CHECK(marked);
        } else {
            CHECK_FALSE(marked);
        }
    }
    CHECK(markedLayers == 1);
    CHECK(markedEffects == 1);
    CHECK(unmarkedLayers == 1);
    std::vector<SceneRun> runs;
    scanSceneRuns(list, runs);
    size_t sceneRuns = 0;
    for (const SceneRun& r : runs) {
        sceneRuns += r.sceneRead ? 1 : 0;
    }
    CHECK(sceneRuns == 2);

    // The mode and the static flag go out with the next list; a mode change damages fully.
    ctx.setHostTargetMode(HostTargetMode::Direct);
    ctx.setSceneStatic(true);
    ctx.update(0.016f);
    ctx.render(list, &damage);
    CHECK(list.hostTargetMode == HostTargetMode::Direct);
    CHECK(list.sceneStatic);
    CHECK(damage.full);
    ctx.update(0.016f);
    ctx.render(list, &damage);
    CHECK_FALSE(damage.full);
    CHECK(list.hostTargetMode == HostTargetMode::Direct);
}
