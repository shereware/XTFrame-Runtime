// T-718 (Docs/design/10-backends.md §10.16 W1, Docs/tasks/T-718.md "測試"): the frame builder's
// analytic rounded-rect clip levels against a device that answers supportsAnalyticClip() either
// way -- no GPU needed (the pixel checks are in Tests/wgpu/test_wgpu_analytic_clip.cpp).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "camera.h"  // Code/Runtime/core/src: cssPerspectiveViewProj
#include "doctest.h"
#include "rounded_rect.h"  // Code/Runtime/core/src: appendRoundedRectPath
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/types.h"

using namespace xt;
using namespace xt::gpu;

namespace {

/// Hands out handles; executes nothing. Coverage draws and box shadows as xt_wgpu / xt_gl; the
/// analytic clip capability as asked.
class ClipDevice : public GpuDevice {
public:
    explicit ClipDevice(bool analytic) : m_analytic(analytic) {}
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return m_analytic; }
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
    TextureHandle acquireCachedTarget(uint16_t, uint16_t, TextureFormat) override { return m_next++; }
    void releaseCachedTarget(TextureHandle) override {}

private:
    bool m_analytic;
    TextureHandle m_next = 1;
};

struct Built {
    explicit Built(bool analytic, int w = 128, int h = 128, bool fringe = true) : device(analytic) {
        builder.init(device);
        builder.resize(w, h);
        builder.setEdgeFringe(fringe);
    }
    void build(const CommandList& cl) { builder.build(cl, nullptr, nullptr, nullptr, frame); }
    ClipDevice device;
    FrameBuilder builder;
    GpuFrame frame;
};

CommandList makeList(int w = 128, int h = 128) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    return cl;
}

PathRef beginPath(const CommandList& cl) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    return ref;
}

void endPath(const CommandList& cl, PathRef& ref) {
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
}

PathRef polygonPath(CommandList& cl, const std::vector<Vec2>& pts) {
    PathRef ref = beginPath(cl);
    for (size_t i = 0; i < pts.size(); ++i) {
        cl.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(pts[i].x);
        cl.points.push_back(pts[i].y);
    }
    cl.verbs.push_back(PathVerb::Close);
    endPath(cl, ref);
    return ref;
}

void pushClip(CommandList& cl, PathRef path, Mat2x3 xf = Mat2x3{}) {
    ClipPathCmd c{};
    c.xf = xf;
    c.path = path;
    c.rule = FillRule::NonZero;
    cl.clipPaths.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushClipPath, {}, static_cast<uint32_t>(cl.clipPaths.size() - 1)});
}

/// A rounded clip exactly as the Painter writes it (appendRoundedRectPath).
void pushRoundedClip(CommandList& cl, Rect box, float tl, float tr, float br, float bl,
                     Mat2x3 xf = Mat2x3{}) {
    PathRef ref = beginPath(cl);
    appendRoundedRectPath(box, tl, tr, br, bl, cl.verbs, cl.points);
    endPath(cl, ref);
    pushClip(cl, ref, xf);
}

void pushRoundedClip(CommandList& cl, Rect box, float r, Mat2x3 xf = Mat2x3{}) {
    pushRoundedClip(cl, box, r, r, r, r, xf);
}

/// The generator's layout with elliptical corners (rx != ry): `inset(... round a / b)`.
void pushEllipticalClip(CommandList& cl, Rect b, float rx, float ry) {
    const float k = kRoundedRectKappa;
    const float x = b.x, y = b.y, w = b.w, h = b.h;
    PathRef ref = beginPath(cl);
    const PathVerb verbs[] = {PathVerb::Move,  PathVerb::Line, PathVerb::Cubic, PathVerb::Line,
                              PathVerb::Cubic, PathVerb::Line, PathVerb::Cubic, PathVerb::Line,
                              PathVerb::Cubic, PathVerb::Close};
    cl.verbs.insert(cl.verbs.end(), verbs, verbs + sizeof(verbs) / sizeof(verbs[0]));
    const float pts[] = {x + rx,
                         y,
                         x + w - rx,
                         y,
                         x + w - rx + rx * k,
                         y,
                         x + w,
                         y + ry - ry * k,
                         x + w,
                         y + ry,
                         x + w,
                         y + h - ry,
                         x + w,
                         y + h - ry + ry * k,
                         x + w - rx + rx * k,
                         y + h,
                         x + w - rx,
                         y + h,
                         x + rx,
                         y + h,
                         x + rx - rx * k,
                         y + h,
                         x,
                         y + h - ry + ry * k,
                         x,
                         y + h - ry,
                         x,
                         y + ry,
                         x,
                         y + ry - ry * k,
                         x + rx - rx * k,
                         y,
                         x + rx,
                         y};
    cl.points.insert(cl.points.end(), pts, pts + sizeof(pts) / sizeof(pts[0]));
    endPath(cl, ref);
    pushClip(cl, ref);
}

void popClip(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopClip, {}, 0}); }

void fillRect(CommandList& cl, Rect r, Color color) {
    FillCmd f{};
    f.path = polygonPath(
        cl, {Vec2{r.x, r.y}, Vec2{r.x + r.w, r.y}, Vec2{r.x + r.w, r.y + r.h}, Vec2{r.x, r.y + r.h}});
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

void pushLayer(CommandList& cl, float opacity) {
    LayerCmd l{};
    l.opacity = opacity;
    l.blend = BlendMode::Normal;
    cl.layers.push_back(l);
    cl.commands.push_back(Command{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)});
}

void popLayer(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0}); }

void pushBlur(CommandList& cl, float radius, Rect hint, Rect content) {
    EffectInstance e{};
    e.kind = EffectKind::Blur;
    e.params[0] = radius;
    e.params[1] = radius;
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = 1;
    cl.effects.push_back(e);
    c.boundsHint = hint;
    c.contentBounds = content;
    cl.effectCmds.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)});
}

void popEffect(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0}); }

bool isLeaf(Shading s) {
    return s != Shading::StencilOnly && s != Shading::Composite && s != Shading::Effect;
}

uint32_t maskApplies(const GpuFrame& f) {
    uint32_t n = 0;
    for (const DrawCmd& c : f.cmds) {
        n += c.shading == Shading::Effect && c.program == EffectProgram::MaskApply ? 1u : 0u;
    }
    return n;
}

uint32_t clippedCmds(const GpuFrame& f) {
    uint32_t n = 0;
    for (const DrawCmd& c : f.cmds) {
        n += c.clipParamIndex != 0 ? 1u : 0u;
    }
    return n;
}

AnalyticClipParams blockOf(const GpuFrame& f, const DrawCmd& c) {
    AnalyticClipParams p;
    REQUIRE(c.clipParamIndex != 0);
    REQUIRE(size_t(c.clipParamIndex) + sizeof(p) / sizeof(float) <= f.params.size());
    std::memcpy(static_cast<void*>(&p), f.params.data() + c.clipParamIndex, sizeof(p));
    return p;
}

/// Field by field (DrawCmd has padding), and every pool byte for byte.
bool sameFrame(const GpuFrame& a, const GpuFrame& b) {
    if (a.cmds.size() != b.cmds.size() || a.passes.size() != b.passes.size() ||
        a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size() ||
        a.params.size() != b.params.size() || a.transients.size() != b.transients.size() ||
        a.cacheStores.size() != b.cacheStores.size()) {
        return false;
    }
    std::string da, db;
    dumpGpuFrame(a, da);
    dumpGpuFrame(b, db);
    if (da != db) {
        return false;
    }
    for (size_t i = 0; i < a.cmds.size(); ++i) {
        const DrawCmd& x = a.cmds[i];
        const DrawCmd& y = b.cmds[i];
        if (x.firstIndex != y.firstIndex || x.indexCount != y.indexCount || x.baseVertex != y.baseVertex ||
            x.vertexCount != y.vertexCount || x.texture != y.texture || x.src != y.src || x.aux != y.aux ||
            x.paramIndex != y.paramIndex || x.shading != y.shading || x.blend != y.blend ||
            x.stencilOp != y.stencilOp || x.stencilRef != y.stencilRef || x.sampler != y.sampler ||
            x.program != y.program || x.mipLod != y.mipLod || x.boxShadowMulti != y.boxShadowMulti ||
            x.clipX != y.clipX || x.clipY != y.clipY || x.clipW != y.clipW || x.clipH != y.clipH ||
            x.kind != y.kind || x.firstInstance != y.firstInstance || x.instanceCount != y.instanceCount ||
            x.coverage != y.coverage || x.clipParamIndex != y.clipParamIndex) {
            return false;
        }
    }
    return std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) == 0 &&
           std::memcmp(a.indices.data(), b.indices.data(), a.indices.size() * sizeof(uint32_t)) == 0 &&
           std::memcmp(a.params.data(), b.params.data(), a.params.size() * sizeof(float)) == 0;
}

/// A rounded card whose content runs edge to edge (the corners are hit: no skip, no scissor).
CommandList cardList(float radius = 16.0f) {
    CommandList cl = makeList();
    pushRoundedClip(cl, Rect{10, 10, 100, 80}, radius);
    fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
    fillRect(cl, Rect{20, 20, 40, 40}, Color{1, 0, 0, 1});
    popClip(cl);
    return cl;
}

}  // namespace

TEST_CASE("gpu analytic clip: one rounded clip is an analytic level, not a mask layer (T-718)") {
    const CommandList cl = cardList();
    Built b(true);
    b.build(cl);
    const GpuFrame& f = b.frame;
    CHECK(f.transients.empty());
    CHECK(f.passes.size() == 1);
    CHECK(maskApplies(f) == 0);
    CHECK(b.builder.lastClipStats().maskLayers == 0);
    CHECK(b.builder.lastBuildStats().analyticClips == 1);
    CHECK(b.builder.lastBuildStats().analyticClipFallbacks == 0);
    REQUIRE(!f.cmds.empty());
    for (const DrawCmd& c : f.cmds) {
        CHECK(c.stencilOp == StencilOp::TestEqualRef);  // no stencil level either
        CHECK(c.stencilRef == 0);
        REQUIRE(c.clipParamIndex != 0);
        const AnalyticClipParams p = blockOf(f, c);
        CHECK(p.count == 1);
        const float identity[6] = {1, 0, 0, 1, 0, 0};
        for (int i = 0; i < 6; ++i) {
            CHECK(p.clips[0].localFromDevice[i] == doctest::Approx(identity[i]));
        }
        CHECK(p.clips[0].rect[0] == doctest::Approx(10.0f));
        CHECK(p.clips[0].rect[1] == doctest::Approx(10.0f));
        CHECK(p.clips[0].rect[2] == doctest::Approx(100.0f));
        CHECK(p.clips[0].rect[3] == doctest::Approx(80.0f));
        for (const float r : p.clips[0].radii) {
            CHECK(r == doctest::Approx(16.0f));
        }
        // T-720 (W1 item 1 as revised): the scissor is the one outside the clip, not the shape's box.
        CHECK(c.clipX == 0);
        CHECK(c.clipY == 0);
        CHECK(c.clipW == 128);
        CHECK(c.clipH == 128);
    }
    std::string dump;
    dumpGpuFrame(f, dump);
    CHECK(dump.find(" aclip=1") != std::string::npos);

    SUBCASE("a draw after the PopClip carries nothing and has the scissor of before") {
        CommandList after = cardList();
        fillRect(after, Rect{0, 0, 30, 30}, Color{0, 1, 0, 1});
        Built a(true);
        a.build(after);
        REQUIRE(!a.frame.cmds.empty());
        const DrawCmd& last = a.frame.cmds.back();
        CHECK(last.clipParamIndex == 0);
        CHECK(last.clipW == 128);
        CHECK(last.clipH == 128);
    }
    SUBCASE("a transformed clip: localFromDevice is the inverse of the device transform (dpr 2)") {
        CommandList rot = makeList();
        rot.devicePixelRatio = 2;
        const Mat2x3 xf = mul(translation(Vec2{30, 30}), rotationDeg(20.0f));
        pushRoundedClip(rot, Rect{-20, -10, 40, 20}, 4, 6, 0, 8, xf);
        fillRect(rot, Rect{0, 0, 64, 64}, Color{0, 0, 1, 1});
        popClip(rot);
        Built r(true);
        r.build(rot);
        CHECK(r.builder.lastBuildStats().analyticClips == 1);
        REQUIRE(!r.frame.cmds.empty());
        const AnalyticClipParams p = blockOf(r.frame, r.frame.cmds.front());
        Mat2x3 inv;
        REQUIRE(invert(mul(scaling(Vec2{2, 2}), xf), inv));
        const float want[6] = {inv.a, inv.b, inv.c, inv.d, inv.e, inv.f};
        for (int i = 0; i < 6; ++i) {
            CHECK(p.clips[0].localFromDevice[i] == doctest::Approx(want[i]).epsilon(1e-4));
        }
        CHECK(p.clips[0].radii[0] == doctest::Approx(4.0f));
        CHECK(p.clips[0].radii[1] == doctest::Approx(6.0f));
        CHECK(p.clips[0].radii[2] == doctest::Approx(0.0f));
        CHECK(p.clips[0].radii[3] == doctest::Approx(8.0f));
    }
}

TEST_CASE("gpu analytic clip: nested two and three are analytic, the fourth is a mask layer (T-718)") {
    auto nested = [](int levels) {
        CommandList cl = makeList();
        for (int i = 0; i < levels; ++i) {
            const float inset = 4.0f * static_cast<float>(i);
            pushRoundedClip(cl, Rect{8 + inset, 8 + inset, 112 - 2 * inset, 112 - 2 * inset}, 20);
            fillRect(cl, Rect{0, 0, 128, 128}, Color{0.1f * static_cast<float>(i + 1), 0, 1, 1});
        }
        for (int i = 0; i < levels; ++i) {
            popClip(cl);
        }
        return cl;
    };
    for (int levels = 2; levels <= 3; ++levels) {
        CAPTURE(levels);
        Built b(true);
        b.build(nested(levels));
        CHECK(b.frame.transients.empty());
        CHECK(b.builder.lastClipStats().maskLayers == 0);
        CHECK(b.builder.lastBuildStats().analyticClips == static_cast<uint32_t>(levels));
        CHECK(b.builder.lastBuildStats().analyticClipFallbacks == 0);
        REQUIRE(b.frame.cmds.size() == static_cast<size_t>(levels));
        for (int i = 0; i < levels; ++i) {
            const AnalyticClipParams p = blockOf(b.frame, b.frame.cmds[static_cast<size_t>(i)]);
            CHECK(p.count == static_cast<uint32_t>(i + 1));
            CHECK(p.clips[i].rect[0] == doctest::Approx(8.0f + 4.0f * static_cast<float>(i)));
        }
    }
    Built b(true);
    b.build(nested(4));
    CHECK(b.builder.lastBuildStats().analyticClips == 3);
    CHECK(b.builder.lastBuildStats().analyticClipFallbacks == 1);
    CHECK(b.builder.lastClipStats().maskLayers == 1);
    CHECK(maskApplies(b.frame) == 1);
    // The fourth level's content is drawn into the mask layer under the three analytic clips; the
    // layer's white shape, its MaskApply and its composite carry none.
    uint32_t leavesWith3 = 0, leavesWithout = 0;
    for (const Pass& pass : b.frame.passes) {
        for (uint32_t i = pass.firstCmd; i < pass.firstCmd + pass.cmdCount; ++i) {
            const DrawCmd& c = b.frame.cmds[i];
            if (!isLeaf(c.shading)) {
                CHECK(c.clipParamIndex == 0);
                continue;
            }
            if (c.clipParamIndex == 0) {
                ++leavesWithout;
                CHECK(pass.target != kSurfaceTarget);  // the white shape
            } else if (blockOf(b.frame, c).count == 3) {
                ++leavesWith3;
            }
        }
    }
    CHECK(leavesWithout == 1);
    CHECK(leavesWith3 >= 2);  // the third level's fill and the fourth level's content
}

TEST_CASE("gpu analytic clip: other shapes, projections and 4x keep their path (T-718)") {
    auto framesOf = [](const CommandList& cl, bool fringe, Built& on, Built& off) {
        on.builder.setEdgeFringe(fringe);
        off.builder.setEdgeFringe(fringe);
        on.build(cl);
        off.build(cl);
    };
    SUBCASE("elliptical corner radii: a mask layer, counted as a fallback") {
        CommandList cl = makeList();
        pushEllipticalClip(cl, Rect{10, 10, 100, 80}, 30, 12);
        fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
        fillRect(cl, Rect{20, 20, 40, 40}, Color{1, 0, 0, 1});
        popClip(cl);
        Built on(true), off(false);
        framesOf(cl, true, on, off);
        CHECK(sameFrame(on.frame, off.frame));
        CHECK(clippedCmds(on.frame) == 0);
        CHECK(on.builder.lastClipStats().maskLayers == 1);
        CHECK(on.builder.lastBuildStats().analyticClips == 0);
        CHECK(on.builder.lastBuildStats().analyticClipFallbacks == 1);
        CHECK(off.builder.lastBuildStats().analyticClipFallbacks == 0);
    }
    SUBCASE("a polygon (a rotated square as a path) and a concave path") {
        CommandList cl = makeList();
        pushClip(cl, polygonPath(cl, {Vec2{64, 8}, Vec2{120, 64}, Vec2{64, 120}, Vec2{8, 64}}));
        fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
        fillRect(cl, Rect{20, 20, 40, 40}, Color{1, 0, 0, 1});
        popClip(cl);
        pushClip(cl, polygonPath(cl, {Vec2{10, 10}, Vec2{110, 10}, Vec2{110, 60}, Vec2{60, 60}, Vec2{60, 110},
                                      Vec2{10, 110}}));
        fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 1, 0, 1});
        fillRect(cl, Rect{20, 20, 40, 40}, Color{1, 0, 0, 1});
        popClip(cl);
        Built on(true), off(false);
        framesOf(cl, true, on, off);
        CHECK(sameFrame(on.frame, off.frame));
        CHECK(clippedCmds(on.frame) == 0);
        CHECK(on.builder.lastClipStats().maskLayers == 2);
        CHECK(on.builder.lastBuildStats().analyticClipFallbacks == 2);
    }
    SUBCASE("a rotated plain rectangle (PushClipRect and a zero-radius path)") {
        CommandList cl = makeList();
        const Mat2x3 xf = mul(translation(Vec2{64, 64}), rotationDeg(12.0f));
        ClipRectCmd c{};
        c.xf = xf;
        c.rect = Rect{-40, -20, 80, 40};
        cl.clipRects.push_back(c);
        cl.commands.push_back(Command{CmdKind::PushClipRect, {}, 0});
        fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
        fillRect(cl, Rect{20, 20, 40, 40}, Color{1, 0, 0, 1});
        popClip(cl);
        pushRoundedClip(cl, Rect{-40, -20, 80, 40}, 0, xf);
        fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
        fillRect(cl, Rect{20, 20, 40, 40}, Color{1, 0, 0, 1});
        popClip(cl);
        Built on(true), off(false);
        framesOf(cl, true, on, off);
        CHECK(sameFrame(on.frame, off.frame));
        CHECK(on.builder.lastClipStats().maskLayers == 2);
        CHECK(on.builder.lastBuildStats().analyticClips == 0);
        CHECK(on.builder.lastBuildStats().analyticClipFallbacks == 2);
    }
    SUBCASE("inside a perspective projection: the stencil level of before") {
        CommandList cl = makeList();
        ProjectionCmd pc{};
        pc.viewProj = cssPerspectiveViewProj(300.0f, Vec2{64, 64}).viewProj;
        pc.boxRect = Rect{0, 0, 128, 128};
        cl.projections.push_back(pc);
        cl.commands.push_back(Command{CmdKind::PushProjection, {}, 0});
        Transform3DCmd t{};
        t.world3 = mat4Mul(mat4Translate(Vec3{64, 64, 0}),
                           mat4Mul(mat4RotateY(35.0f), mat4Translate(Vec3{-64, -64, 0})));
        cl.transforms3.push_back(t);
        cl.commands.push_back(Command{CmdKind::PushTransform3D, {}, 0});
        pushRoundedClip(cl, Rect{10, 10, 100, 80}, 16);
        fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
        popClip(cl);
        cl.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
        cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
        Built on(true), off(false);
        framesOf(cl, true, on, off);
        CHECK(sameFrame(on.frame, off.frame));
        CHECK(clippedCmds(on.frame) == 0);
        CHECK(on.builder.lastBuildStats().analyticClips == 0);
        CHECK(on.builder.lastBuildStats().analyticClipFallbacks == 0);
    }
    SUBCASE("4x (no fringe): the stencil level of before") {
        const CommandList cl = cardList();
        Built on(true), off(false);
        framesOf(cl, false, on, off);
        CHECK(sameFrame(on.frame, off.frame));
        CHECK(clippedCmds(on.frame) == 0);
        CHECK(on.builder.lastBuildStats().analyticClips == 0);
        CHECK(on.builder.lastBuildStats().analyticClipFallbacks == 0);
        bool clipPush = false;
        for (const DrawCmd& c : on.frame.cmds) {
            clipPush = clipPush || c.stencilOp == StencilOp::ClipPush;
        }
        CHECK(clipPush);
    }
}

TEST_CASE("gpu analytic clip: a device without the capability gets the mask layer of before (T-718)") {
    const CommandList cl = cardList();
    Built off(false);
    off.build(cl);
    CHECK(clippedCmds(off.frame) == 0);
    CHECK(maskApplies(off.frame) == 1);
    CHECK(off.builder.lastClipStats().maskLayers == 1);
    CHECK(off.builder.lastBuildStats().analyticClips == 0);
    CHECK(off.builder.lastBuildStats().analyticClipFallbacks == 0);
    std::string dump;
    dumpGpuFrame(off.frame, dump);
    CHECK(dump.find("aclip") == std::string::npos);
    const GpuDevice& base = off.device;
    CHECK(!base.GpuDevice::supportsAnalyticClip());  // the default
    // Without a rounded clip the capability changes nothing at all.
    CommandList plain = makeList();
    fillRect(plain, Rect{10, 10, 50, 50}, Color{1, 0, 0, 1});
    pushLayer(plain, 0.5f);
    fillRect(plain, Rect{30, 30, 50, 50}, Color{0, 1, 0, 1});
    popLayer(plain);
    Built a(true), b(false);
    a.build(plain);
    b.build(plain);
    CHECK(sameFrame(a.frame, b.frame));
}

TEST_CASE("gpu analytic clip: leaves inside layers and effects carry the clip, group draws do not (T-718)") {
    CommandList cl = makeList();
    pushRoundedClip(cl, Rect{10, 10, 100, 80}, 16);
    fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
    pushLayer(cl, 0.5f);
    fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 1, 0, 1});
    popLayer(cl);
    pushBlur(cl, 4, Rect{0, 0, 128, 128}, Rect{20, 20, 60, 60});
    fillRect(cl, Rect{20, 20, 60, 60}, Color{1, 0, 0, 1});
    popEffect(cl);
    popClip(cl);
    Built b(true);
    b.build(cl);
    CHECK(b.builder.lastBuildStats().analyticClips == 1);
    CHECK(b.builder.lastClipStats().maskLayers == 0);
    uint32_t leavesOnSurface = 0, leavesOffscreen = 0, composites = 0, effects = 0;
    for (const Pass& pass : b.frame.passes) {
        for (uint32_t i = pass.firstCmd; i < pass.firstCmd + pass.cmdCount; ++i) {
            const DrawCmd& c = b.frame.cmds[i];
            if (c.shading == Shading::Composite) {
                ++composites;
                CHECK(c.clipParamIndex == 0);
            } else if (c.shading == Shading::Effect) {
                ++effects;
                CHECK(c.clipParamIndex == 0);
            } else if (c.shading == Shading::StencilOnly) {
                CHECK(c.clipParamIndex == 0);
            } else {
                CHECK(c.clipParamIndex != 0);
                (pass.target == kSurfaceTarget ? leavesOnSurface : leavesOffscreen) += 1;
            }
        }
    }
    CHECK(leavesOnSurface == 1);
    CHECK(leavesOffscreen == 2);  // the layer's fill and the effect's content
    CHECK(composites == 2);
    CHECK(effects == 2);  // the two blur passes

    // Stored on the second build, served on the third: the cached result is a group draw too.
    b.build(cl);
    b.build(cl);
    CHECK(b.builder.lastEffectCacheStats().hits == 1);
    uint32_t cachedQuads = 0;
    for (const DrawCmd& c : b.frame.cmds) {
        if (c.shading == Shading::Texture) {
            ++cachedQuads;
            CHECK(c.clipParamIndex == 0);
        }
    }
    CHECK(cachedQuads == 1);
}

TEST_CASE("gpu analytic clip: the level leaves the scissor alone; group windows follow their hint (T-720)") {
    // Under an axis-aligned rect clip (the scissor 4,4 .. 124,124) a rotated rounded clip; inside
    // it a hinted effect running out of the shape's box, an unhinted layer, then a leaf.
    CommandList cl = makeList(256, 256);
    ClipRectCmd rc{};
    rc.rect = Rect{4, 4, 248, 248};
    cl.clipRects.push_back(rc);
    cl.commands.push_back(Command{CmdKind::PushClipRect, {}, 0});
    const Mat2x3 rot = mul(translation(Vec2{60, 60}), rotationDeg(20.0f));
    pushRoundedClip(cl, Rect{-30, -20, 60, 40}, 8, rot);  // its box is about 70 x 58 px
    pushBlur(cl, 4, Rect{30, 30, 200, 200}, Rect{40, 40, 60, 60});
    fillRect(cl, Rect{40, 40, 60, 60}, Color{1, 0, 0, 1});
    popEffect(cl);
    pushLayer(cl, 0.5f);  // no boundsHint
    fillRect(cl, Rect{0, 0, 256, 256}, Color{0, 1, 0, 1});
    popLayer(cl);
    fillRect(cl, Rect{0, 0, 256, 256}, Color{0, 0, 1, 1});
    popClip(cl);
    popClip(cl);
    Built b(true, 256, 256);
    b.build(cl);
    CHECK(b.builder.lastBuildStats().analyticClips == 1);
    CHECK(b.builder.lastClipStats().maskLayers == 0);
    uint32_t composites = 0, leaves = 0;
    for (const Pass& pass : b.frame.passes) {
        for (uint32_t i = pass.firstCmd; i < pass.firstCmd + pass.cmdCount; ++i) {
            const DrawCmd& c = b.frame.cmds[i];
            if (pass.target != kSurfaceTarget) {
                continue;  // inside a group target
            }
            // On the surface every draw has the rect clip's scissor, not the shape's box.
            CHECK(c.clipX == 4);
            CHECK(c.clipY == 4);
            CHECK(c.clipW == 248);
            CHECK(c.clipH == 248);
            if (c.shading == Shading::Composite) {
                ++composites;
                CHECK(c.clipParamIndex == 0);
            } else if (isLeaf(c.shading)) {
                ++leaves;
                CHECK(c.clipParamIndex != 0);
            }
        }
    }
    CHECK(composites == 2);  // the effect's result and the layer
    CHECK(leaves == 1);
    // The group targets (stencil-capable): the hinted effect's is its 200 px hint, not cut at the
    // shape's box; the unhinted layer's is the shape's box (padded to 96 x 64), not the surface.
    uint32_t hinted = 0, atBox = 0, groups = 0;
    for (const TransientTargetDesc& t : b.frame.transients) {
        if (t.needStencil == 0) {
            continue;  // the blur's own passes
        }
        ++groups;
        hinted += t.width >= 200 && t.height >= 200 && t.width < 256 && t.height < 256 ? 1u : 0u;
        atBox += t.width <= 96 && t.height <= 96 ? 1u : 0u;
    }
    CHECK(groups == 2);
    CHECK(hinted == 1);
    CHECK(atBox == 1);
}

TEST_CASE("gpu analytic clip: leaves outside the scissor and box are skipped, composites not (T-720)") {
    // The rounded clip's box (150..250) lies right of the rect clip's scissor (0..128): its leaves
    // cannot show and are not drawn (as with T-718's empty scissor); the effect's result, whose hint
    // reaches into the scissor, is still composited.
    CommandList cl = makeList(256, 256);
    ClipRectCmd rc{};
    rc.rect = Rect{0, 0, 128, 256};
    cl.clipRects.push_back(rc);
    cl.commands.push_back(Command{CmdKind::PushClipRect, {}, 0});
    pushRoundedClip(cl, Rect{150, 20, 100, 100}, 12);
    fillRect(cl, Rect{0, 0, 256, 256}, Color{0, 0, 1, 1});
    pushBlur(cl, 4, Rect{100, 0, 156, 256}, Rect{150, 20, 100, 100});
    fillRect(cl, Rect{150, 20, 100, 100}, Color{1, 0, 0, 1});
    popEffect(cl);
    fillRect(cl, Rect{0, 0, 256, 256}, Color{0, 1, 0, 1});
    popClip(cl);
    popClip(cl);
    Built b(true, 256, 256);
    b.build(cl);
    CHECK(b.builder.lastBuildStats().analyticClips == 1);
    uint32_t surfaceLeaves = 0, surfaceComposites = 0, offscreenLeaves = 0;
    for (const Pass& pass : b.frame.passes) {
        for (uint32_t i = pass.firstCmd; i < pass.firstCmd + pass.cmdCount; ++i) {
            const DrawCmd& c = b.frame.cmds[i];
            if (pass.target == kSurfaceTarget) {
                surfaceLeaves += isLeaf(c.shading) ? 1u : 0u;
                surfaceComposites += c.shading == Shading::Composite ? 1u : 0u;
            } else {
                offscreenLeaves += isLeaf(c.shading) ? 1u : 0u;
            }
        }
    }
    CHECK(surfaceLeaves == 0);
    CHECK(offscreenLeaves == 0);
    CHECK(surfaceComposites == 1);
}

TEST_CASE("gpu analytic clip: an effect under another analytic clip is another cache entry (T-718)") {
    auto list = [](float radius) {
        CommandList cl = makeList();
        pushRoundedClip(cl, Rect{10, 10, 100, 80}, radius);
        fillRect(cl, Rect{0, 0, 128, 128}, Color{0, 0, 1, 1});
        pushBlur(cl, 4, Rect{0, 0, 128, 128}, Rect{20, 20, 60, 60});
        fillRect(cl, Rect{0, 0, 128, 128}, Color{1, 0, 0, 1});
        popEffect(cl);
        popClip(cl);
        return cl;
    };
    Built b(true);
    const CommandList a = list(16), other = list(24);
    b.build(a);
    b.build(a);
    CHECK(b.builder.lastEffectCacheStats().stores == 1);
    b.build(a);
    CHECK(b.builder.lastEffectCacheStats().hits == 1);
    b.build(other);  // same commands inside the effect, same scissor: only the clip's radius differs
    CHECK(b.builder.lastEffectCacheStats().hits == 0);
    CHECK(b.builder.lastEffectCacheStats().misses == 1);
    b.build(a);  // the first clip's entry went idle for one build and was released
    b.build(a);
    b.build(a);
    CHECK(b.builder.lastEffectCacheStats().hits == 1);
}
