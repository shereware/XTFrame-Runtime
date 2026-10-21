#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "camera.h"
#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/command_list.h"
#include "xt/command_simplifier.h"
#include "xt/node.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// CommandListSimplifier tests, one case per docs/design/09-command-list.md §9.5 rule, plus
/// §19.8's flatten3D rule (docs/tasks/T-49.md "測試"). See docs/tasks/T-37.md "測試" /
/// "test_command_simplifier.cpp".

using namespace xt;
using namespace xt::sw;

namespace {

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

void pushCmd(CommandList& cl, CmdKind kind, uint32_t index) {
    Command cmd{};
    cmd.kind = kind;
    cmd.index = index;
    cl.commands.push_back(cmd);
}

/// A horizontal open 2-point line (a single "Line" segment), for StrokePath tests.
PathRef addLinePath(CommandList& cl, Vec2 a, Vec2 b) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    float pts[4] = {a.x, a.y, b.x, b.y};
    cl.points.insert(cl.points.end(), pts, pts + 4);
    ref.verbCount = 2;
    ref.pointCount = 4;
    return ref;
}

std::vector<uint8_t> renderRgba(const CommandList& cl, int w, int h) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 0});
    renderer.render(cl, nullptr, nullptr);
    return buf;
}

}  // namespace

TEST_CASE("CommandListSimplifier: StrokePath degrades to FillPath, matches SW render within tolerance") {
    constexpr int w = 40, h = 20;

    CommandList original;
    original.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    StrokeCmd stroke{};
    stroke.xf = Mat2x3{};
    stroke.path = addLinePath(original, Vec2{4, 10}, Vec2{36, 10});
    stroke.paint.kind = PaintKind::Solid;
    stroke.paint.color = Color{1, 1, 1, 1};
    stroke.width = 6;
    stroke.cap = LineCap::Butt;
    stroke.join = LineJoin::Miter;
    stroke.miterLimit = 4;
    original.strokes.push_back(stroke);
    pushCmd(original, CmdKind::StrokePath, 0);

    CommandList simplified = original;
    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(simplified, options, nullptr);

    REQUIRE(simplified.strokes.empty());
    REQUIRE(simplified.fills.size() == 1);
    CHECK(simplified.fills[0].rule == FillRule::NonZero);

    std::vector<uint8_t> before = renderRgba(original, w, h);
    std::vector<uint8_t> after = renderRgba(simplified, w, h);

    size_t mismatches = 0;
    size_t total = before.size();
    for (size_t i = 0; i < total; ++i) {
        int diff = std::abs(static_cast<int>(before[i]) - static_cast<int>(after[i]));
        if (diff > 8) {
            ++mismatches;
        }
    }
    CHECK(static_cast<double>(mismatches) / static_cast<double>(total) <= 0.02);
}

TEST_CASE("CommandListSimplifier: gradient paint degrades to Solid") {
    CommandList list;
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{0, 0, 0, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 1}},
    };
    list.stops = stops;

    FillCmd fill{};
    fill.path = addRectPath(list, 0, 0, 10, 10);
    fill.paint.kind = PaintKind::Linear;
    fill.paint.color = Color{1, 1, 1, 1};
    fill.paint.p0 = Vec2{0, 0};
    fill.paint.p1 = Vec2{10, 0};
    fill.paint.stopOffset = 0;
    fill.paint.stopCount = 2;
    fill.rule = FillRule::NonZero;
    fill.antialias = 1;
    list.fills.push_back(fill);
    pushCmd(list, CmdKind::FillPath, 0);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, nullptr);

    REQUIRE(list.fills.size() == 1);
    CHECK(list.fills[0].paint.kind == PaintKind::Solid);
    // t=0.5 between black and white -> mid gray, non-empty (non-zero alpha).
    CHECK(list.fills[0].paint.color.a > 0.0f);
    CHECK(list.fills[0].paint.color.r == doctest::Approx(0.5f).epsilon(0.05));

    std::vector<uint8_t> pixels = renderRgba(list, 10, 10);
    bool anyOpaque = false;
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) {
            anyOpaque = true;
            break;
        }
    }
    CHECK(anyOpaque);
}

TEST_CASE("CommandListSimplifier: PushLayer/PopLayer opacity is baked into paint alpha") {
    CommandList list;
    LayerCmd layer{};
    layer.opacity = 0.5f;
    layer.blend = BlendMode::Normal;
    list.layers.push_back(layer);
    pushCmd(list, CmdKind::PushLayer, 0);

    FillCmd fillA{};
    fillA.path = addRectPath(list, 0, 0, 10, 10);
    fillA.paint.kind = PaintKind::Solid;
    fillA.paint.color = Color{1, 0, 0, 1};
    fillA.rule = FillRule::NonZero;
    fillA.antialias = 1;
    list.fills.push_back(fillA);
    pushCmd(list, CmdKind::FillPath, 0);

    FillCmd fillB{};
    fillB.path = addRectPath(list, 5, 5, 10, 10);
    fillB.paint.kind = PaintKind::Solid;
    fillB.paint.color = Color{0, 0, 1, 1};
    fillB.rule = FillRule::NonZero;
    fillB.antialias = 1;
    list.fills.push_back(fillB);
    pushCmd(list, CmdKind::FillPath, 1);

    pushCmd(list, CmdKind::PopLayer, 0);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, nullptr);

    // PushLayer/PopLayer commands are gone; both fills remain with opacity baked in.
    for (const Command& cmd : list.commands) {
        CHECK(cmd.kind != CmdKind::PushLayer);
        CHECK(cmd.kind != CmdKind::PopLayer);
    }
    REQUIRE(list.fills.size() == 2);
    CHECK(list.fills[0].paint.color.a == doctest::Approx(0.5f));
    CHECK(list.fills[1].paint.color.a == doctest::Approx(0.5f));
    // Overlap region (5..10, 5..10) differs from an un-simplified render since the backend no
    // longer composites the two fills through a single 0.5-opacity layer; recorded here rather
    // than asserted (docs/tasks/T-37.md "重疊區允許差異（記錄）").
}

TEST_CASE("CommandListSimplifier: mask content removed, masked content kept") {
    CommandList list;
    MaskCmd mask{};
    mask.mode = MaskMode::Alpha;
    list.masks.push_back(mask);
    pushCmd(list, CmdKind::PushMask, 0);

    // Mask shape (dropped).
    FillCmd shape{};
    shape.path = addRectPath(list, 0, 0, 5, 5);
    shape.paint.kind = PaintKind::Solid;
    shape.paint.color = Color{1, 1, 1, 1};
    shape.rule = FillRule::NonZero;
    shape.antialias = 1;
    list.fills.push_back(shape);
    pushCmd(list, CmdKind::FillPath, 0);

    pushCmd(list, CmdKind::EndMaskContent, 0);

    // Masked content (kept).
    FillCmd content{};
    content.path = addRectPath(list, 0, 0, 10, 10);
    content.paint.kind = PaintKind::Solid;
    content.paint.color = Color{0, 1, 0, 1};
    content.rule = FillRule::NonZero;
    content.antialias = 1;
    list.fills.push_back(content);
    pushCmd(list, CmdKind::FillPath, 1);

    pushCmd(list, CmdKind::PopMask, 0);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, nullptr);

    for (const Command& cmd : list.commands) {
        CHECK(cmd.kind != CmdKind::PushMask);
        CHECK(cmd.kind != CmdKind::EndMaskContent);
        CHECK(cmd.kind != CmdKind::PopMask);
    }
    REQUIRE(list.fills.size() == 1);
    CHECK(list.fills[0].paint.color == Color{0, 1, 0, 1});
}

TEST_CASE("CommandListSimplifier: PushClipPath degrades to PushClipRect (AABB)") {
    CommandList list;
    ClipPathCmd clip{};
    clip.path = addRectPath(list, 2, 3, 4, 5);  // AABB: x=2,y=3,w=4,h=5.
    clip.rule = FillRule::NonZero;
    list.clipPaths.push_back(clip);
    pushCmd(list, CmdKind::PushClipPath, 0);
    pushCmd(list, CmdKind::PopClip, 0);  // T-263a: simplify() leaves an unbalanced list untouched.

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, nullptr);

    REQUIRE(list.clipPaths.empty());
    REQUIRE(list.clipRects.size() == 1);
    CHECK(list.commands[0].kind == CmdKind::PushClipRect);
    CHECK(list.clipRects[0].rect.x == doctest::Approx(2.0f));
    CHECK(list.clipRects[0].rect.y == doctest::Approx(3.0f));
    CHECK(list.clipRects[0].rect.w == doctest::Approx(4.0f));
    CHECK(list.clipRects[0].rect.h == doctest::Approx(5.0f));
}

TEST_CASE("CommandListSimplifier: DrawGlyphs degrades to one FillPath per glyph (FakeTextShaper square)") {
    CommandList list;
    GlyphsCmd glyphs{};
    glyphs.font = 1;
    glyphs.size = 10;
    glyphs.color = Color{1, 1, 1, 1};
    glyphs.glyphOffset = 0;
    glyphs.glyphCount = 2;
    list.glyphs = {65, 66};
    list.glyphPos = {Vec2{0, 0}, Vec2{20, 0}};
    list.glyphRuns.push_back(glyphs);
    pushCmd(list, CmdKind::DrawGlyphs, 0);

    FakeTextShaper shaper;
    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, &shaper);

    REQUIRE(list.glyphRuns.empty());
    REQUIRE(list.fills.size() == 2);
    // FakeTextShaper::glyphOutline is a fixed 1x1 em square; scaled by size=10, translated by
    // glyphPos[0] = (0, 0) -> (0,0)-(10,10).
    REQUIRE(list.fills[0].path.pointCount == 8);
    const std::vector<float>& pts0 = list.points;
    uint32_t off0 = list.fills[0].path.pointOffset;
    CHECK(pts0[off0] == doctest::Approx(0.0f));
    CHECK(pts0[off0 + 1] == doctest::Approx(0.0f));
    CHECK(pts0[off0 + 2] == doctest::Approx(10.0f));
    // Second glyph translated by glyphPos[1] = (20, 0).
    uint32_t off1 = list.fills[1].path.pointOffset;
    CHECK(pts0[off1] == doctest::Approx(20.0f));
    CHECK(list.fills[0].paint.kind == PaintKind::Solid);
    CHECK(list.fills[0].paint.color == Color{1, 1, 1, 1});
}

TEST_CASE("CommandListSimplifier: null shaper drops DrawGlyphs entirely") {
    CommandList list;
    GlyphsCmd glyphs{};
    glyphs.font = 1;
    glyphs.size = 10;
    glyphs.color = Color{1, 1, 1, 1};
    glyphs.glyphOffset = 0;
    glyphs.glyphCount = 1;
    // assign() rather than `= {65}`: GCC 12 -O2 turns the one-element initializer_list
    // assignment into a bogus -Werror=array-bounds (runtime mirror CI, g++-12 Release).
    list.glyphs.assign(1, 65u);
    list.glyphPos.assign(1, Vec2{0, 0});
    list.glyphRuns.push_back(glyphs);
    pushCmd(list, CmdKind::DrawGlyphs, 0);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, nullptr);

    CHECK(list.commands.empty());
    CHECK(list.fills.empty());
    CHECK(list.glyphRuns.empty());
}

TEST_CASE("CommandListSimplifier: capacity is reused across calls on a stable-shape input") {
    auto buildFlat = [](CommandList& cl) {
        cl.commands.clear();
        cl.fills.clear();
        cl.verbs.clear();
        cl.points.clear();
        for (int i = 0; i < 3; ++i) {
            FillCmd fill{};
            fill.path = addRectPath(cl, static_cast<float>(i) * 5.0f, 0, 4, 4);
            fill.paint.kind = PaintKind::Solid;
            fill.paint.color = Color{1, 1, 1, 1};
            fill.rule = FillRule::NonZero;
            fill.antialias = 1;
            cl.fills.push_back(fill);
            pushCmd(cl, CmdKind::FillPath, static_cast<uint32_t>(cl.fills.size() - 1));
        }
    };

    CommandList list;
    buildFlat(list);

    CommandListSimplifier simplifier;
    SimplifyOptions options;

    // simplify() ping-pongs `list`'s buffers with the Simplifier's internal scratch member on
    // every call (docs/tasks/T-37.md "產生新 vector 後 swap；容量跨呼叫保留於 Simplifier 成
    // 員"). The two buffers were originally built by different code paths (this test's
    // range-insert vs. copyPath's element-by-element push_back), so they may each stabilize at a
    // *different* capacity; `list`'s reported capacity legitimately alternates between those two
    // values forever. Two calls are enough to observe both stabilized capacities; a call beyond
    // that must not exceed the max of what's already been seen (no further reallocation).
    simplifier.simplify(list, options, nullptr);
    REQUIRE(list.fills.size() == 3);
    size_t capFills1 = list.fills.capacity();
    size_t capCommands1 = list.commands.capacity();
    size_t capVerbs1 = list.verbs.capacity();
    size_t capPoints1 = list.points.capacity();

    simplifier.simplify(list, options, nullptr);
    REQUIRE(list.fills.size() == 3);
    size_t capFills2 = list.fills.capacity();
    size_t capCommands2 = list.commands.capacity();
    size_t capVerbs2 = list.verbs.capacity();
    size_t capPoints2 = list.points.capacity();

    simplifier.simplify(list, options, nullptr);
    REQUIRE(list.fills.size() == 3);
    CHECK(list.fills.capacity() <= std::max(capFills1, capFills2));
    CHECK(list.commands.capacity() <= std::max(capCommands1, capCommands2));
    CHECK(list.verbs.capacity() <= std::max(capVerbs1, capVerbs2));
    CHECK(list.points.capacity() <= std::max(capPoints1, capPoints2));
}

// --- docs/design/19-3d-space.md §19.8 "flatten3D" (docs/tasks/T-49.md) ---

namespace {

void pushProjection(CommandList& cl, const Mat4& viewProj, const Mat2x3& rootXf, Rect box) {
    ProjectionCmd proj{};
    proj.viewProj = viewProj;
    proj.rootXf = rootXf;
    proj.boxRect = box;
    proj.depthTest = 0;
    cl.projections.push_back(proj);
    pushCmd(cl, CmdKind::PushProjection, static_cast<uint32_t>(cl.projections.size() - 1));
}

void popProjection(CommandList& cl) { pushCmd(cl, CmdKind::PopProjection, 0); }

void pushTransform3D(CommandList& cl, const Mat4& world3) {
    Transform3DCmd xf3{};
    xf3.world3 = world3;
    cl.transforms3.push_back(xf3);
    pushCmd(cl, CmdKind::PushTransform3D, static_cast<uint32_t>(cl.transforms3.size() - 1));
}

void popTransform3D(CommandList& cl) { pushCmd(cl, CmdKind::PopTransform3D, 0); }

uint32_t pushFill(CommandList& cl, PathRef path) {
    FillCmd fill{};
    fill.xf = Mat2x3{};
    fill.path = path;
    fill.paint.kind = PaintKind::Solid;
    fill.paint.color = Color{1, 0, 0, 1};
    fill.rule = FillRule::NonZero;
    fill.antialias = 1;
    cl.fills.push_back(fill);
    pushCmd(cl, CmdKind::FillPath, static_cast<uint32_t>(cl.fills.size() - 1));
    return static_cast<uint32_t>(cl.fills.size() - 1);
}

}  // namespace

TEST_CASE("CommandListSimplifier: flatten3D projects a FillPath (translateZ perspective scale)") {
    // docs/tasks/T-49.md "測試": PushProjection{cssPerspective(200, (32,32))} +
    // PushTransform3D{translateZ = 100} (scale = 1 / (1 - 100/200) = 2 about the perspective
    // origin) over a 10x10 square centered on that origin (27,27)-(37,37) -> projects to the
    // task sheet's asserted (22,22)-(42,42) 20x20 result. (10x10, not literally "20x20", is the
    // *pre*-projection size that produces the sheet's stated *post*-projection numbers under this
    // scale=2 setup -- see the T-49 report's "uncovered cases".)
    CameraSetup camera = cssPerspectiveViewProj(200.0f, Vec2{32, 32});
    Mat4 world3 = mat4Translate(Vec3{0, 0, 100});

    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    pushProjection(list, camera.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    pushTransform3D(list, world3);
    pushFill(list, addRectPath(list, 27, 27, 10, 10));
    popTransform3D(list);
    popProjection(list);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    SimplifyStats stats = simplifier.simplify(list, options, nullptr);

    for (const Command& cmd : list.commands) {
        CHECK(cmd.kind != CmdKind::PushProjection);
        CHECK(cmd.kind != CmdKind::PopProjection);
        CHECK(cmd.kind != CmdKind::PushTransform3D);
        CHECK(cmd.kind != CmdKind::PopTransform3D);
    }
    REQUIRE(list.fills.size() == 1);
    CHECK(stats.flattenedCommands >= 1);

    const FillCmd& fill = list.fills[0];
    CHECK(fill.xf.a == doctest::Approx(1.0f));
    CHECK(fill.xf.e == doctest::Approx(0.0f));
    REQUIRE(fill.path.pointCount == 8);
    float minX = list.points[fill.path.pointOffset];
    float minY = list.points[fill.path.pointOffset + 1];
    float maxX = minX, maxY = minY;
    for (uint32_t i = 0; i < fill.path.pointCount; i += 2) {
        minX = std::min(minX, list.points[fill.path.pointOffset + i]);
        maxX = std::max(maxX, list.points[fill.path.pointOffset + i]);
        minY = std::min(minY, list.points[fill.path.pointOffset + i + 1]);
        maxY = std::max(maxY, list.points[fill.path.pointOffset + i + 1]);
    }
    CHECK(minX == doctest::Approx(22.0f).epsilon(0.5 / 22.0));
    CHECK(minY == doctest::Approx(22.0f).epsilon(0.5 / 22.0));
    CHECK(maxX == doctest::Approx(42.0f).epsilon(0.5 / 42.0));
    CHECK(maxY == doctest::Approx(42.0f).epsilon(0.5 / 42.0));

    // SW render of the *simplified* list matches the hand-computed (22,22)-(42,42) region
    // (docs/tasks/T-49.md's verification note: render only the simplified list here, since the
    // SW backend's own projection support (T-45) is not part of this task).
    std::vector<uint8_t> pixels = renderRgba(list, 64, 64);
    auto opaqueAt = [&](int x, int y) {
        return pixels[(static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4 + 3] > 0;
    };
    CHECK(opaqueAt(32, 32));
    CHECK(opaqueAt(23, 23));
    CHECK(opaqueAt(41, 41));
    CHECK_FALSE(opaqueAt(10, 10));
    CHECK_FALSE(opaqueAt(50, 50));
}

TEST_CASE("CommandListSimplifier: flatten3D DrawImage stays DrawImage under an orthographic camera") {
    // perspective: none -> cssPerspectiveViewProj returns the identity (orthographicIdentity ==
    // true); every quad projects to an exact (unrotated) rectangle, i.e. a parallelogram.
    CameraSetup camera = cssPerspectiveViewProj(0.0f, Vec2{0, 0});

    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    pushProjection(list, camera.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    ImageCmd image{};
    image.xf = Mat2x3{};
    image.asset = 3;
    image.dst = Rect{10, 10, 20, 20};
    image.srcUv = Rect{0, 0, 1, 1};
    image.opacity = 1.0f;
    image.smooth = 1;
    list.images.push_back(image);
    pushCmd(list, CmdKind::DrawImage, 0);
    popProjection(list);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    SimplifyStats stats = simplifier.simplify(list, options, nullptr);

    REQUIRE(list.commands.size() == 1);
    CHECK(list.commands[0].kind == CmdKind::DrawImage);
    REQUIRE(list.images.size() == 1);
    CHECK(list.imageQuads.empty());
    CHECK(stats.flattenedCommands == 1);
}

TEST_CASE("CommandListSimplifier: flatten3D DrawImage becomes DrawImageQuad under rotateY perspective") {
    CameraSetup camera = cssPerspectiveViewProj(200.0f, Vec2{0, 0});
    Mat4 world3 = mat4RotateY(60.0f);

    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    pushProjection(list, camera.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    pushTransform3D(list, world3);
    ImageCmd image{};
    image.xf = Mat2x3{};
    image.asset = 5;
    image.dst = Rect{-20, -20, 40, 40};
    image.srcUv = Rect{0, 0, 1, 1};
    image.opacity = 1.0f;
    image.smooth = 0;
    // T-263d: the image is the second source's (two addresses, never dereferenced).
    static const char storage[2] = {0, 0};
    list.sources = {CommandSource{reinterpret_cast<const IrView*>(&storage[0]), 1},
                    CommandSource{reinterpret_cast<const IrView*>(&storage[1]), 2}};
    image.source = 1;
    list.images.push_back(image);
    pushCmd(list, CmdKind::DrawImage, 0);
    popTransform3D(list);
    popProjection(list);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, nullptr);

    REQUIRE(list.commands.size() == 1);
    CHECK(list.commands[0].kind == CmdKind::DrawImageQuad);
    REQUIRE(list.imageQuads.size() == 1);
    CHECK(list.images.empty());
    CHECK(list.imageQuads[0].asset == 5);
    // T-263d: the quad inherits the image's source, and the table comes through both passes.
    CHECK(list.imageQuads[0].source == 1);
    REQUIRE(list.sources.size() == 2);
    CHECK(list.sources[1].loadSerial == 2);
}

TEST_CASE("CommandListSimplifier: flatten3D drops DrawGlyphs (no shaper) and counts droppedGlyphRuns") {
    CameraSetup camera = cssPerspectiveViewProj(200.0f, Vec2{32, 32});

    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    pushProjection(list, camera.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    GlyphsCmd glyphs{};
    glyphs.font = 1;
    glyphs.size = 10;
    glyphs.color = Color{1, 1, 1, 1};
    glyphs.glyphOffset = 0;
    glyphs.glyphCount = 1;
    // assign() rather than `= {65}`: GCC 12 -O2 turns the one-element initializer_list
    // assignment into a bogus -Werror=array-bounds (runtime mirror CI, g++-12 Release).
    list.glyphs.assign(1, 65u);
    list.glyphPos.assign(1, Vec2{0, 0});
    list.glyphRuns.push_back(glyphs);
    pushCmd(list, CmdKind::DrawGlyphs, 0);
    popProjection(list);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    SimplifyStats stats = simplifier.simplify(list, options, nullptr);

    CHECK(list.glyphRuns.empty());
    CHECK(list.fills.empty());
    CHECK(stats.droppedGlyphRuns == 1);
}

TEST_CASE("CommandListSimplifier: flatten3D degrades PushClipRect to PushClipPath under projection") {
    CameraSetup camera = cssPerspectiveViewProj(0.0f, Vec2{0, 0});  // Identity: exact rect corners.

    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    pushProjection(list, camera.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    ClipRectCmd clipRect{};
    clipRect.xf = Mat2x3{};
    clipRect.rect = Rect{2, 3, 4, 5};
    list.clipRects.push_back(clipRect);
    pushCmd(list, CmdKind::PushClipRect, 0);
    pushCmd(list, CmdKind::PopClip, 0);
    popProjection(list);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    // Disable the (also-default-on) 2D clipPaths rule, which would otherwise degrade flatten3D's
    // PushClipPath output straight back to a PushClipRect AABB -- this test checks flatten3D's own
    // output specifically.
    options.clipPaths = false;
    simplifier.simplify(list, options, nullptr);

    REQUIRE(list.commands.size() == 2);
    CHECK(list.commands[0].kind == CmdKind::PushClipPath);
    CHECK(list.commands[1].kind == CmdKind::PopClip);
    REQUIRE(list.clipPaths.size() == 1);
    CHECK(list.clipRects.empty());
    REQUIRE(list.clipPaths[0].path.pointCount == 8);
    // Identity camera + identity rootXf/xf -> corners project back to the same rect.
    float minX = list.points[list.clipPaths[0].path.pointOffset];
    float maxX = minX;
    for (uint32_t i = 0; i < list.clipPaths[0].path.pointCount; i += 2) {
        minX = std::min(minX, list.points[list.clipPaths[0].path.pointOffset + i]);
        maxX = std::max(maxX, list.points[list.clipPaths[0].path.pointOffset + i]);
    }
    CHECK(minX == doctest::Approx(2.0f));
    CHECK(maxX == doctest::Approx(6.0f));
}

TEST_CASE("CommandListSimplifier: flatten3D leaves non-projected commands untouched") {
    CommandList list;
    pushFill(list, addRectPath(list, 0, 0, 5, 5));

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    options.gradients = false;
    options.layers = false;
    options.masks = false;
    options.clipPaths = false;
    options.strokes = false;
    options.glyphsToPaths = false;
    SimplifyStats stats = simplifier.simplify(list, options, nullptr);

    CHECK(stats.flattenedCommands == 0);
    REQUIRE(list.fills.size() == 1);
    CHECK(list.fills[0].path.pointCount == 8);
}

TEST_CASE("CommandListSimplifier: options.effects (default) drops PushEffect/PopEffect, keeps content") {
    // docs/tasks/T-52.md: `options.effects` (default true) removes PushEffect/PopEffect and
    // accumulates `SimplifyStats::droppedEffects` from EffectCmd::effectCount -- the content in
    // between is left untouched (processed by this same loop's other rules as usual).
    CommandList list;
    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    blur.enabled = 1;
    blur.params[0] = 4;
    blur.params[1] = 4;
    list.effects.push_back(blur);
    EffectCmd ec{};
    ec.effectOffset = 0;
    ec.effectCount = 1;
    ec.xf = Mat2x3{};
    ec.boundsHint = Rect{0, 0, 20, 20};
    ec.contentBounds = Rect{0, 0, 10, 10};
    list.effectCmds.push_back(ec);
    pushCmd(list, CmdKind::PushEffect, 0);
    pushFill(list, addRectPath(list, 0, 0, 5, 5));
    pushCmd(list, CmdKind::PopEffect, 0);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    options.flatten3D = false;
    SimplifyStats stats = simplifier.simplify(list, options, nullptr);

    CHECK(stats.droppedEffects == 1);
    REQUIRE(list.fills.size() == 1);
    for (const Command& cmd : list.commands) {
        CHECK(cmd.kind != CmdKind::PushEffect);
        CHECK(cmd.kind != CmdKind::PopEffect);
    }
}

TEST_CASE("CommandListSimplifier: options.effects = false keeps PushEffect/PopEffect, copies pools") {
    CommandList list;
    list.stops.push_back(GradientStop{0.0f, Color{0, 0, 0, 1}});
    list.stops.push_back(GradientStop{1.0f, Color{1, 1, 1, 1}});
    EffectInstance mask{};
    mask.kind = EffectKind::GradientMask;
    mask.enabled = 1;
    mask.stopOffset = 0;
    mask.stopCount = 2;
    list.effects.push_back(mask);
    EffectCmd ec{};
    ec.effectOffset = 0;
    ec.effectCount = 1;
    ec.xf = Mat2x3{};
    ec.boundsHint = Rect{0, 0, 20, 20};
    ec.contentBounds = Rect{0, 0, 10, 10};
    list.effectCmds.push_back(ec);
    pushCmd(list, CmdKind::PushEffect, 0);
    pushFill(list, addRectPath(list, 0, 0, 5, 5));
    pushCmd(list, CmdKind::PopEffect, 0);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    options.flatten3D = false;
    options.effects = false;
    SimplifyStats stats = simplifier.simplify(list, options, nullptr);

    CHECK(stats.droppedEffects == 0);
    REQUIRE(list.effectCmds.size() == 1);
    REQUIRE(list.effectCmds[0].effectCount == 1);
    REQUIRE(list.effects.size() == 1);
    CHECK(list.effects[0].stopCount == 2);
    REQUIRE(list.stops.size() >= list.effects[0].stopOffset + 2);
}

TEST_CASE("CommandListSimplifier: SW render before vs after flatten3D") {
    // docs/tasks/T-49.md's verification note: the SW backend's own projection rendering (T-45) is
    // being merged separately and is not available in this worktree, so a *before* (unsimplified,
    // still carrying PushProjection/PushTransform3D) SW render cannot be produced or compared yet.
    // This test documents the intended comparison (>= 4/8ths tolerance per channel, <= 2% mismatch
    // ratio, matching the rest of this file's simplifier tolerance) so it can be enabled once T-45
    // lands; it is compiled (so it cannot silently bit-rot) but skipped.
    CameraSetup camera = cssPerspectiveViewProj(200.0f, Vec2{32, 32});
    Mat4 world3 = mat4Translate(Vec3{0, 0, 100});

    CommandList original;
    original.viewport = Rect{0, 0, 64, 64};
    pushProjection(original, camera.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    pushTransform3D(original, world3);
    pushFill(original, addRectPath(original, 27, 27, 10, 10));
    popTransform3D(original);
    popProjection(original);

    CommandList simplified = original;
    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(simplified, options, nullptr);

    std::vector<uint8_t> before = renderRgba(original, 64, 64);  // Requires T-45 to render correctly.
    std::vector<uint8_t> after = renderRgba(simplified, 64, 64);

    size_t mismatches = 0;
    size_t total = before.size();
    for (size_t i = 0; i < total; ++i) {
        int diff = std::abs(static_cast<int>(before[i]) - static_cast<int>(after[i]));
        if (diff > 8) {
            ++mismatches;
        }
    }
    CHECK(static_cast<double>(mismatches) / static_cast<double>(total) <= 0.02);
}

namespace {

/// T-245: appends a DrawImageMesh of `count` vertices (a fan) whose positions start at `x0`.
void addTestMesh(CommandList& cl, AssetId asset, float x0, uint32_t count, float opacity) {
    ImageMeshCmd mc{};
    mc.asset = asset;
    mc.vertexOffset = static_cast<uint32_t>(cl.meshVertices.size());
    mc.vertexCount = count;
    mc.indexOffset = static_cast<uint32_t>(cl.meshIndices.size());
    for (uint32_t i = 0; i < count; ++i) {
        const float a = static_cast<float>(i);
        cl.meshVertices.push_back(
            MeshVertex{Vec2{x0 + a * 3.0f, 10.0f + a * a}, Vec2{a * 0.1f, 1.0f - a * 0.1f}});
    }
    for (uint32_t i = 1; i + 1 < count; ++i) {
        cl.meshIndices.push_back(0);
        cl.meshIndices.push_back(static_cast<uint16_t>(i));
        cl.meshIndices.push_back(static_cast<uint16_t>(i + 1));
    }
    mc.indexCount = static_cast<uint32_t>(cl.meshIndices.size()) - mc.indexOffset;
    mc.opacity = opacity;
    mc.smooth = 1;
    cl.imageMeshes.push_back(mc);
    pushCmd(cl, CmdKind::DrawImageMesh, static_cast<uint32_t>(cl.imageMeshes.size() - 1));
}

}  // namespace

TEST_CASE(
    "CommandListSimplifier: DrawImageMesh round trip re-pools vertices/indices and folds layer opacity") {
    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    addTestMesh(list, 1, 0.0f, 4, 1.0f);  // outside the layer
    LayerCmd layer{};
    layer.opacity = 0.5f;
    layer.blend = BlendMode::Normal;
    layer.boundsHint = Rect{0, 0, 64, 64};
    list.layers.push_back(layer);
    pushCmd(list, CmdKind::PushLayer, 0);
    addTestMesh(list, 2, 20.0f, 5, 0.8f);
    pushCmd(list, CmdKind::PopLayer, 0);
    const CommandList original = list;

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    simplifier.simplify(list, options, nullptr);

    REQUIRE(list.commands.size() == 2);
    REQUIRE(list.imageMeshes.size() == 2);
    for (size_t m = 0; m < 2; ++m) {
        CAPTURE(m);
        CHECK(list.commands[m].kind == CmdKind::DrawImageMesh);
        const ImageMeshCmd& out = list.imageMeshes[list.commands[m].index];
        const ImageMeshCmd& in = original.imageMeshes[m];
        CHECK(out.asset == in.asset);
        CHECK(out.smooth == in.smooth);
        REQUIRE(out.vertexCount == in.vertexCount);
        REQUIRE(out.indexCount == in.indexCount);
        for (uint32_t i = 0; i < in.vertexCount; ++i) {
            CHECK(list.meshVertices[out.vertexOffset + i].p.x ==
                  original.meshVertices[in.vertexOffset + i].p.x);
            CHECK(list.meshVertices[out.vertexOffset + i].p.y ==
                  original.meshVertices[in.vertexOffset + i].p.y);
            CHECK(list.meshVertices[out.vertexOffset + i].uv.x ==
                  original.meshVertices[in.vertexOffset + i].uv.x);
            CHECK(list.meshVertices[out.vertexOffset + i].uv.y ==
                  original.meshVertices[in.vertexOffset + i].uv.y);
        }
        for (uint32_t i = 0; i < in.indexCount; ++i) {
            CHECK(list.meshIndices[out.indexOffset + i] == original.meshIndices[in.indexOffset + i]);
        }
    }
    CHECK(list.imageMeshes[0].opacity == doctest::Approx(1.0f));
    CHECK(list.imageMeshes[1].opacity == doctest::Approx(0.4f));  // 0.8 x the dropped layer's 0.5
    CHECK(list.meshVertices.size() == 9);
    CHECK(list.meshIndices.size() == 15);
}

TEST_CASE("CommandListSimplifier: flatten3D projects DrawImageMesh vertices and keeps the triangles") {
    // Orthographic (perspective: none): every vertex projects onto itself.
    CameraSetup camera = cssPerspectiveViewProj(0.0f, Vec2{0, 0});
    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    pushProjection(list, camera.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    addTestMesh(list, 3, 5.0f, 4, 1.0f);
    popProjection(list);
    const CommandList original = list;

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    SimplifyStats stats = simplifier.simplify(list, options, nullptr);
    REQUIRE(list.commands.size() == 1);
    CHECK(list.commands[0].kind == CmdKind::DrawImageMesh);
    CHECK(stats.flattenedCommands == 1);
    REQUIRE(list.meshVertices.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(list.meshVertices[i].p.x == doctest::Approx(original.meshVertices[i].p.x));
        CHECK(list.meshVertices[i].p.y == doctest::Approx(original.meshVertices[i].p.y));
    }
    CHECK(list.meshIndices.size() == 6);

    // A tilted perspective moves the vertices but keeps the mesh a DrawImageMesh.
    CameraSetup persp = cssPerspectiveViewProj(200.0f, Vec2{32, 32});
    CommandList tilted;
    tilted.viewport = Rect{0, 0, 64, 64};
    pushProjection(tilted, persp.viewProj, Mat2x3{}, Rect{0, 0, 64, 64});
    pushTransform3D(tilted, mat4RotateY(40.0f));
    addTestMesh(tilted, 3, 5.0f, 4, 1.0f);
    popTransform3D(tilted);
    popProjection(tilted);
    simplifier.simplify(tilted, options, nullptr);
    REQUIRE(tilted.commands.size() == 1);
    CHECK(tilted.commands[0].kind == CmdKind::DrawImageMesh);
    REQUIRE(tilted.meshVertices.size() == 4);
    CHECK(tilted.meshVertices[3].p.x != doctest::Approx(original.meshVertices[3].p.x));
}

TEST_CASE("CommandListSimplifier: the result's structureChecked is simplify()'s own decision") {
    // T-263b: the simplified list is a new list that simplify() built from a valid one, so it is
    // marked checked whatever the input's flag said; a list that fails validation is handed back
    // untouched and stays unmarked.
    for (bool flatten : {false, true}) {
        for (bool inputChecked : {false, true}) {
            CAPTURE(flatten);
            CAPTURE(inputChecked);
            CommandList list;
            list.viewport = Rect{0, 0, 16, 16};
            FillCmd fill{};
            fill.path = addRectPath(list, 0, 0, 10, 10);
            fill.paint.kind = PaintKind::Solid;
            fill.paint.color = Color{1, 0, 0, 1};
            fill.rule = FillRule::NonZero;
            list.fills.push_back(fill);
            pushCmd(list, CmdKind::FillPath, 0);
            list.structureChecked = inputChecked;

            CommandListSimplifier simplifier;
            SimplifyOptions options;
            options.flatten3D = flatten;
            simplifier.simplify(list, options, nullptr);
            CHECK(list.fills.size() == 1);
            CHECK(list.structureChecked);
        }
    }
    CommandList broken;
    broken.viewport = Rect{0, 0, 16, 16};
    pushCmd(broken, CmdKind::FillPath, 7);  // No such fill.
    CommandListSimplifier simplifier;
    simplifier.simplify(broken, SimplifyOptions{}, nullptr);
    CHECK(broken.commands.size() == 1);
    CHECK_FALSE(broken.structureChecked);
}

TEST_CASE("dumpCommandList: DrawImageMesh header, vertices and triangles") {
    CommandList list;
    addTestMesh(list, 7, 1.0f, 3, 0.5f);
    std::string out;
    dumpCommandList(list, out);
    CHECK(out ==
          "DrawImageMesh asset=7 vertices=3 triangles=1 opacity=0.500 smooth=1\n"
          "  vertex p=(1.000,10.000) uv=(0.000,1.000)\n"
          "  vertex p=(4.000,11.000) uv=(0.100,0.900)\n"
          "  vertex p=(7.000,14.000) uv=(0.200,0.800)\n"
          "  tri 0 1 2\n");
}
