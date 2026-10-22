#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// SwRenderer integration tests, built by hand-filling a CommandList. See Docs/tasks/T-11.md
/// "測試" / "test_sw_renderer.cpp".

using namespace xt;
using namespace xt::sw;

namespace {

/// Appends a closed rectangle path (Move, Line x3, Close) to `cl`'s pools; returns its PathRef.
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

void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color,
                 FillRule rule = FillRule::NonZero, Mat2x3 xf = Mat2x3{}) {
    PathRef path = addRectPath(cl, x, y, w, h);
    FillCmd f{};
    f.xf = xf;
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
    cl.commands.push_back(cmd);
}

void addPushClipRect(CommandList& cl, Rect rect, Mat2x3 xf = Mat2x3{}) {
    ClipRectCmd c{};
    c.xf = xf;
    c.rect = rect;
    cl.clipRects.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushClipRect;
    cmd.index = static_cast<uint32_t>(cl.clipRects.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopClip(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopClip;
    cmd.index = 0;
    cl.commands.push_back(cmd);
}

struct Rgba {
    uint8_t r, g, b, a;
};

Rgba pixelAt(const std::vector<uint8_t>& buf, int width, int x, int y) {
    size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return Rgba{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};
}

bool approxByte(uint8_t a, int expected, int tol = 1) {
    return std::abs(static_cast<int>(a) - expected) <= tol;
}

}  // namespace

TEST_CASE("SwRenderer: fill a rect on a black target, exact edges, no AA") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    renderer.render(cl, nullptr, nullptr);

    Rgba inside = pixelAt(buf, w, 32, 32);
    CHECK(inside.r == 255);
    CHECK(inside.g == 0);
    CHECK(inside.b == 0);
    CHECK(inside.a == 255);

    Rgba outside = pixelAt(buf, w, 0, 0);
    CHECK(outside.r == 0);
    CHECK(outside.a == 255);

    Rgba topLeftCorner = pixelAt(buf, w, 16, 16);  // inside, exact integer boundary
    CHECK(topLeftCorner.r == 255);

    Rgba rightBoundary = pixelAt(buf, w, 48, 32);  // rect spans [16,48): x=48 is outside
    CHECK(rightBoundary.r == 0);

    Rgba leftJustOutside = pixelAt(buf, w, 15, 32);
    CHECK(leftJustOutside.r == 0);
}

TEST_CASE("SwRenderer: semi-transparent blue over opaque red, non-premultiplied src-over") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    addFillRect(cl, 16, 16, 32, 32, Color{0, 0, 1, 0.5f});
    renderer.render(cl, nullptr, nullptr);

    Rgba p = pixelAt(buf, w, 32, 32);
    CHECK(approxByte(p.r, 128));
    CHECK(p.g == 0);
    CHECK(approxByte(p.b, 128));
    CHECK(p.a == 255);
}

TEST_CASE("SwRenderer: semi-transparent red over a transparent target stays non-premultiplied") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 0});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 0.5f});
    renderer.render(cl, nullptr, nullptr);

    Rgba p = pixelAt(buf, w, 32, 32);
    CHECK(p.r == 255);
    CHECK(p.g == 0);
    CHECK(p.b == 0);
    CHECK(approxByte(p.a, 128));
}

TEST_CASE("SwRenderer: ClipRect restricts a fill; PopClip restores full coverage") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addPushClipRect(cl, Rect{0, 0, 32, 64});
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    addPopClip(cl);
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    renderer.render(cl, nullptr, nullptr);

    // After the whole sequence, the unconditional blue fill (post-PopClip) covers everything.
    Rgba left = pixelAt(buf, w, 8, 32);
    Rgba right = pixelAt(buf, w, 48, 32);
    CHECK(left.b == 255);
    CHECK(right.b == 255);
}

TEST_CASE("SwRenderer: ClipRect left half only, before PopClip") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addPushClipRect(cl, Rect{0, 0, 32, 64});
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    addPopClip(cl);  // T-263a: render() rejects a list that leaves a clip open.
    renderer.render(cl, nullptr, nullptr);

    Rgba left = pixelAt(buf, w, 8, 32);
    Rgba right = pixelAt(buf, w, 48, 32);
    CHECK(left.r == 255);
    CHECK(right.r == 0);  // outside the clip, fill did not reach here
}

TEST_CASE("SwRenderer: nested ClipRect intersects") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addPushClipRect(cl, Rect{0, 0, 48, 64});
    addPushClipRect(cl, Rect{16, 0, 32, 64});  // intersection: x in [16, 48)
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    addPopClip(cl);
    addPopClip(cl);
    renderer.render(cl, nullptr, nullptr);

    CHECK(pixelAt(buf, w, 8, 32).r == 0);
    CHECK(pixelAt(buf, w, 24, 32).r == 255);
    CHECK(pixelAt(buf, w, 40, 32).r == 255);
    CHECK(pixelAt(buf, w, 56, 32).r == 0);
}

// T-263a E8: SW has no clip depth limit (xt::kMaxClipDepth is the GPU stencil's): a clip nested
// deeper than that still applies, nothing asserts, and every PopClip pairs.
TEST_CASE("SwRenderer: T-263a clips nested deeper than kMaxClipDepth all apply") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    // Clips 1..15 all keep x in [0, 48); the 16th and 17th cut that down to [0, 8).
    for (uint32_t i = 0; i < kMaxClipDepth; ++i) {
        addPushClipRect(cl, Rect{0, 0, 48, 64});
    }
    addPushClipRect(cl, Rect{0, 0, 8, 64});
    addPushClipRect(cl, Rect{0, 0, 8, 64});
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    addPopClip(cl);
    addPopClip(cl);
    addFillRect(cl, 0, 32, 64, 32, Color{0, 1, 0, 1});  // still inside clips 1..15
    for (uint32_t i = 0; i < kMaxClipDepth; ++i) {
        addPopClip(cl);
    }
    addFillRect(cl, 56, 0, 8, 8, Color{0, 0, 1, 1});  // every clip popped
    renderer.render(cl, nullptr, nullptr);

    CHECK(pixelAt(buf, w, 4, 8).r == 255);
    CHECK(pixelAt(buf, w, 24, 8).r == 0);  // cut by the 16th clip
    CHECK(pixelAt(buf, w, 52, 8).r == 0);  // outside clips 1..15
    CHECK(pixelAt(buf, w, 24, 40).g == 255);
    CHECK(pixelAt(buf, w, 52, 40).g == 0);
    CHECK(pixelAt(buf, w, 60, 4).b == 255);
}

// T-263a E1: render() validates the list first; an invalid one draws nothing and never reads a
// pool out of range.
TEST_CASE("SwRenderer: T-263a an invalid command list draws nothing and does not crash") {
    const int w = 32, h = 32;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{buf.data(), w, h, w * 4});
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addFillRect(cl, 0, 0, 32, 32, Color{1, 0, 0, 1});

    SUBCASE("a command index past its pool") {
        cl.commands.push_back(Command{CmdKind::StrokePath, {}, 1000});
    }
    SUBCASE("a path whose verbs need more points than its range holds") { cl.fills[0].path.pointCount = 6; }
    SUBCASE("effectCount = 0xFFFFFFFF") {
        EffectCmd ec{};
        ec.effectCount = 0xFFFFFFFFu;
        ec.boundsHint = Rect{0, 0, 32, 32};
        cl.effectCmds.push_back(ec);
        cl.commands.push_back(Command{CmdKind::PushEffect, {}, 0});
        cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
    }
    SUBCASE("a pop of an empty stack") { cl.commands.push_back(Command{CmdKind::PopMask, {}, 0}); }
    SUBCASE("a layer left open") {
        LayerCmd layer{};
        layer.opacity = 1;
        layer.boundsHint = cl.viewport;
        cl.layers.push_back(layer);
        cl.commands.insert(cl.commands.begin(), Command{CmdKind::PushLayer, {}, 0});
    }
    renderer.render(cl, nullptr, nullptr);
    CHECK(renderer.lastExecutedDrawCommands() == 0);
    CHECK(pixelAt(buf, w, 16, 16).r == 0);

    // The renderer is still usable afterwards.
    CommandList good;
    good.viewport = cl.viewport;
    good.devicePixelRatio = 1;
    addFillRect(good, 0, 0, 32, 32, Color{0, 0, 1, 1});
    renderer.render(good, nullptr, nullptr);
    CHECK(pixelAt(buf, w, 16, 16).b == 255);
}

TEST_CASE("SwRenderer: rotated square, center colored, far corner untouched, area matches") {
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 0});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    const float side = 20;
    Mat2x3 xf = mul(translation(Vec2{32, 32}), rotationDeg(45));
    addFillRect(cl, -side / 2, -side / 2, side, side, Color{1, 1, 1, 1}, FillRule::NonZero, xf);
    renderer.render(cl, nullptr, nullptr);

    CHECK(pixelAt(buf, w, 32, 32).a == 255);
    CHECK(pixelAt(buf, w, 10, 10).a == 0);

    int64_t alphaSum = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            alphaSum += pixelAt(buf, w, x, y).a;
        }
    }
    double expected = static_cast<double>(side) * static_cast<double>(side) * 255.0;
    CHECK(static_cast<double>(alphaSum) >= expected * 0.97);
    CHECK(static_cast<double>(alphaSum) <= expected * 1.03);
}

TEST_CASE("SwRenderer: devicePixelRatio 2 scales into the target") {
    const int viewportSize = 32;
    const int w = 64, h = 64;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(viewportSize), static_cast<float>(viewportSize)};
    cl.devicePixelRatio = 2;
    addFillRect(cl, 8, 8, 8, 8, Color{0, 1, 0, 1});
    renderer.render(cl, nullptr, nullptr);

    CHECK(pixelAt(buf, w, 20, 20).g == 255);  // inside device-space (16,16)-(32,32)
    CHECK(pixelAt(buf, w, 10, 10).g == 0);
    CHECK(pixelAt(buf, w, 40, 40).g == 0);
}

TEST_CASE("SwRenderer: a CommandList carrying the five 19.7 3D commands does not crash render()") {
    const int w = 32, h = 32;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;

    ProjectionCmd proj{};
    proj.boxRect = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.projections.push_back(proj);
    cl.commands.push_back(Command{CmdKind::PushProjection, {}, 0});

    Transform3DCmd xf3{};
    cl.transforms3.push_back(xf3);
    cl.commands.push_back(Command{CmdKind::PushTransform3D, {}, 0});

    // A normal 2D fill in between, to confirm the surrounding commands still render normally.
    addFillRect(cl, 8, 8, 16, 16, Color{0, 1, 0, 1});

    ImageQuadCmd quad{};
    quad.p[0] = Vec2{0, 0};
    quad.p[1] = Vec2{1, 0};
    quad.p[2] = Vec2{1, 1};
    quad.p[3] = Vec2{0, 1};
    quad.srcUv = Rect{0, 0, 1, 1};
    quad.opacity = 1;
    cl.imageQuads.push_back(quad);
    cl.commands.push_back(Command{CmdKind::DrawImageQuad, {}, 0});

    cl.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
    cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});

    renderer.render(cl, nullptr, nullptr);

    // The 3D commands are skipped, so the fill in between still covers its rect.
    CHECK(pixelAt(buf, w, 16, 16).g == 255);
}

TEST_CASE("SwRenderer: writePng/readPng round trip") {
    const int w = 8, h = 8;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            buf[i + 0] = static_cast<uint8_t>(x * 30);
            buf[i + 1] = static_cast<uint8_t>(y * 30);
            buf[i + 2] = 128;
            buf[i + 3] = 255;
        }
    }

    std::filesystem::path path = xt::test::tempDir() / "xt_sw_test_roundtrip.png";
    CHECK(writePng(path.string(), buf.data(), w, h, w * 4));

    std::vector<uint8_t> readBack;
    int readW = 0, readH = 0;
    CHECK(readPng(path.string(), readBack, readW, readH));
    CHECK(readW == w);
    CHECK(readH == h);
    REQUIRE(readBack.size() == buf.size());
    CHECK(readBack == buf);

    std::error_code ec;
    std::filesystem::remove(path, ec);

    // T-261e: a UTF-8 path whose name the ANSI code page cannot spell (xt_sw_test_アイコン.png).
    const std::string utf8 = path.parent_path().generic_string() +
                             "/xt_sw_test_\xE3\x82\xA2\xE3\x82\xA4\xE3\x82\xB3\xE3\x83\xB3.png";
    const std::filesystem::path wide(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    CHECK(writePng(utf8, buf.data(), w, h, w * 4));
    CHECK(std::filesystem::is_regular_file(wide, ec));
    readBack.clear();
    CHECK(readPng(utf8, readBack, readW, readH));
    CHECK(readBack == buf);
    std::filesystem::remove(wide, ec);
}

// =================================================================================================
// T-245: DrawImageMesh (Docs/design/32-rigging-and-deformation.md §32.2.4).
// =================================================================================================

namespace {

/// Looks decoded images up by AssetId, ignoring the IrView (as test_sw_stroke_image.cpp's).
class MeshTestResolver : public AssetResolver {
public:
    AssetId add(ImageData data) {
        AssetId id = static_cast<AssetId>(m_images.size());
        m_images.push_back(std::move(data));
        return id;
    }
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId asset, const IrView&, ImageData& out) override {
        if (asset >= m_images.size()) {
            return false;
        }
        out = m_images[asset];
        return true;
    }
    Vec2 imageSize(AssetId asset, const IrView&) override {
        if (asset >= m_images.size()) {
            return Vec2{0, 0};
        }
        return Vec2{static_cast<float>(m_images[asset].width), static_cast<float>(m_images[asset].height)};
    }

private:
    std::vector<ImageData> m_images;
};

/// 16x12 pseudo-random texels, a few of them translucent.
ImageData meshPatternImage() {
    ImageData img;
    img.width = 16;
    img.height = 12;
    img.rgba.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4);
    uint32_t seed = 777u;
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        seed = seed * 1664525u + 1013904223u;
        img.rgba[i + 0] = static_cast<uint8_t>(seed >> 24);
        img.rgba[i + 1] = static_cast<uint8_t>((seed >> 16) & 0xFF);
        img.rgba[i + 2] = static_cast<uint8_t>((seed >> 8) & 0xFF);
        img.rgba[i + 3] = static_cast<uint8_t>((i / 4) % 5 == 0 ? 120 : 255);
    }
    return img;
}

/// A uniform 4x4 image of one color (non-premultiplied).
ImageData solidImage(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ImageData img;
    img.width = 4;
    img.height = 4;
    img.rgba.resize(4 * 4 * 4);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i + 0] = r;
        img.rgba[i + 1] = g;
        img.rgba[i + 2] = b;
        img.rgba[i + 3] = a;
    }
    return img;
}

/// Appends a DrawImageMesh of `verts` / `indices` to `cl`.
void addMesh(CommandList& cl, AssetId asset, const std::vector<MeshVertex>& verts,
             const std::vector<uint16_t>& indices, float opacity, bool smooth) {
    ImageMeshCmd mc{};
    mc.asset = asset;
    mc.vertexOffset = static_cast<uint32_t>(cl.meshVertices.size());
    mc.vertexCount = static_cast<uint32_t>(verts.size());
    mc.indexOffset = static_cast<uint32_t>(cl.meshIndices.size());
    mc.indexCount = static_cast<uint32_t>(indices.size());
    mc.opacity = opacity;
    mc.smooth = smooth ? 1 : 0;
    cl.meshVertices.insert(cl.meshVertices.end(), verts.begin(), verts.end());
    cl.meshIndices.insert(cl.meshIndices.end(), indices.begin(), indices.end());
    cl.imageMeshes.push_back(mc);
    cl.commands.push_back(
        Command{CmdKind::DrawImageMesh, {}, static_cast<uint32_t>(cl.imageMeshes.size() - 1)});
}

/// The four corners of `r` with uvs (0,0)-(1,1): TL, TR, BR, BL.
std::vector<MeshVertex> rectVertices(Rect r) {
    return {MeshVertex{{r.x, r.y}, {0, 0}}, MeshVertex{{r.x + r.w, r.y}, {1, 0}},
            MeshVertex{{r.x + r.w, r.y + r.h}, {1, 1}}, MeshVertex{{r.x, r.y + r.h}, {0, 1}}};
}

/// Renders `cl` over `backdrop` into a fresh w x h canvas through `resolver`.
std::vector<uint8_t> renderMeshList(const CommandList& cl, MeshTestResolver& resolver, int w, int h,
                                    Color backdrop) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(backdrop);
    IrView view{};
    renderer.render(cl, nullptr, &resolver, &view);
    return buf;
}

CommandList meshList(int w, int h) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    return cl;
}

}  // namespace

TEST_CASE("DrawImageMesh: a rectangle of two triangles draws DrawImage's bytes exactly (no AA)") {
    const int w = 64, h = 64;
    MeshTestResolver resolver;
    const AssetId asset = resolver.add(meshPatternImage());
    // 16x12 texels magnified 3x: no pixel center maps exactly onto a texel boundary, where nearest
    // sampling is a tie the two paths may break differently (DrawImage restarts its fixed-point
    // source column at every rasterizer span, the mesh at every triangle).
    const Rect dst{5, 7, 48, 36};
    const Color backdrop{0.5f, 0.5f, 0.5f, 1};
    for (bool smooth : {false, true}) {
        for (float opacity : {1.0f, 0.6f}) {
            CommandList ref = meshList(w, h);
            ImageCmd ic{};
            ic.xf = Mat2x3{};
            ic.asset = asset;
            ic.dst = dst;
            ic.srcUv = Rect{0, 0, 1, 1};
            ic.opacity = opacity;
            ic.smooth = smooth ? 1 : 0;
            ref.images.push_back(ic);
            ref.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
            const std::vector<uint8_t> expected = renderMeshList(ref, resolver, w, h, backdrop);
            // Both diagonals.
            const std::vector<std::vector<uint16_t>> splits = {{0, 1, 2, 0, 2, 3}, {0, 1, 3, 1, 2, 3}};
            for (const std::vector<uint16_t>& split : splits) {
                CAPTURE(smooth);
                CAPTURE(opacity);
                CAPTURE(split[2]);
                CommandList cl = meshList(w, h);
                addMesh(cl, asset, rectVertices(dst), split, opacity, smooth);
                const std::vector<uint8_t> actual = renderMeshList(cl, resolver, w, h, backdrop);
                CHECK(actual == expected);
            }
        }
    }
}

TEST_CASE("DrawImageMesh: triangles sharing edges cover exactly the pixel centers inside, once") {
    // A translucent white image over opaque black: a pixel blended twice would come out brighter
    // than one blended once, and a seam would leave a black pixel inside.
    const int w = 48, h = 48;
    MeshTestResolver resolver;
    const AssetId asset = resolver.add(solidImage(255, 255, 255, 128));
    const Rect r{3.3f, 2.7f, 37.3f, 32.5f};  // centers inside: x 3..40, y 3..34
    struct Case {
        const char* name;
        std::vector<MeshVertex> verts;
        std::vector<uint16_t> indices;
    };
    std::vector<MeshVertex> fan = rectVertices(r);
    fan.push_back(MeshVertex{{20.13f, 17.91f}, {0.45f, 0.47f}});
    const Case cases[] = {
        {"two triangles", rectVertices(r), {0, 1, 2, 0, 2, 3}},
        {"four-triangle fan around an inner pin", fan, {0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4}},
        {"mixed winding", rectVertices(r), {0, 2, 1, 0, 2, 3}},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        CommandList cl = meshList(w, h);
        addMesh(cl, asset, c.verts, c.indices, 1.0f, false);
        const std::vector<uint8_t> buf = renderMeshList(cl, resolver, w, h, Color{0, 0, 0, 1});
        const Rgba once = pixelAt(buf, w, 20, 20);
        CHECK(approxByte(once.r, 128, 1));
        int bad = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float cx = static_cast<float>(x) + 0.5f;
                const float cy = static_cast<float>(y) + 0.5f;
                const bool inside = cx > r.x && cx < r.x + r.w && cy > r.y && cy < r.y + r.h;
                const Rgba p = pixelAt(buf, w, x, y);
                const uint8_t want = inside ? once.r : 0;
                if (p.r != want || p.g != want || p.b != want || p.a != 255) {
                    ++bad;
                }
            }
        }
        CHECK(bad == 0);
    }
}

TEST_CASE("DrawImageMesh: degenerate triangles and bad input draw nothing and do not crash") {
    const int w = 32, h = 32;
    MeshTestResolver resolver;
    const AssetId asset = resolver.add(solidImage(255, 0, 0, 255));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<MeshVertex> verts = {
        MeshVertex{{2, 2}, {0, 0}},   MeshVertex{{20, 20}, {1, 1}}, MeshVertex{{11, 11}, {0.5f, 0.5f}},
        MeshVertex{{5, 5}, {0, 0}},   MeshVertex{{nan, 4}, {0, 0}}, MeshVertex{{3e7f, 1}, {1, 0}},
        MeshVertex{{9, 2}, {nan, 0}}, MeshVertex{{2, 20}, {0, 1}},
    };
    // Collinear, coincident, NaN position, out-of-range coordinate, NaN uv, out-of-range index,
    // and a trailing partial triangle.
    const std::vector<uint16_t> indices = {0, 1, 2, 3, 3, 3, 0, 4, 7, 0, 5, 7, 0, 6, 7, 0, 7, 99, 0, 1};
    CommandList cl = meshList(w, h);
    addMesh(cl, asset, verts, indices, 1.0f, true);
    // A command pointing past the pools, and one without a resolvable asset.
    ImageMeshCmd broken{};
    broken.asset = asset;
    broken.vertexOffset = 1000;
    broken.vertexCount = 3;
    broken.indexCount = 3;
    broken.opacity = 1;
    cl.imageMeshes.push_back(broken);
    cl.commands.push_back(Command{CmdKind::DrawImageMesh, {}, 1});
    addMesh(cl, 42, rectVertices(Rect{0, 0, 32, 32}), {0, 1, 2}, 1.0f, false);
    const std::vector<uint8_t> buf = renderMeshList(cl, resolver, w, h, Color{0, 0, 0, 1});
    int touched = 0;
    for (size_t i = 0; i < buf.size(); i += 4) {
        touched += buf[i] != 0 ? 1 : 0;
    }
    CHECK(touched == 0);
}

TEST_CASE("DrawImageMesh: a sheared mesh samples affinely and the clip applies") {
    const int w = 32, h = 32;
    MeshTestResolver resolver;
    const AssetId asset = resolver.add(solidImage(0, 255, 0, 255));
    CommandList cl = meshList(w, h);
    addPushClipRect(cl, Rect{0, 0, 16, 32});
    const std::vector<MeshVertex> verts = {MeshVertex{{4, 4}, {0, 0}}, MeshVertex{{28, 8}, {1, 0}},
                                           MeshVertex{{24, 28}, {1, 1}}, MeshVertex{{2, 26}, {0, 1}}};
    addMesh(cl, asset, verts, {0, 1, 2, 0, 2, 3}, 1.0f, true);
    addPopClip(cl);
    const std::vector<uint8_t> buf = renderMeshList(cl, resolver, w, h, Color{0, 0, 0, 1});
    CHECK(pixelAt(buf, w, 10, 16).g == 255);  // inside the mesh and the clip
    CHECK(pixelAt(buf, w, 20, 16).g == 0);    // inside the mesh, clipped out
    CHECK(pixelAt(buf, w, 1, 1).g == 0);      // outside the mesh
}

TEST_CASE("DrawImageMesh: 1k triangles over a 1920x1080 target (timing, informational)") {
    const int w = 1920, h = 1080;
    MeshTestResolver resolver;
    const AssetId asset = resolver.add(meshPatternImage());
    // A 25 x 20 grid of quads (1000 triangles) covering the whole target, every inner pin moved.
    const int cols = 25, rows = 20;
    std::vector<MeshVertex> verts;
    for (int j = 0; j <= rows; ++j) {
        for (int i = 0; i <= cols; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(cols);
            const float v = static_cast<float>(j) / static_cast<float>(rows);
            const bool inner = i > 0 && i < cols && j > 0 && j < rows;
            const float dx = inner ? 9.0f * std::sin(static_cast<float>(j) * 1.3f) : 0.0f;
            const float dy = inner ? 7.0f * std::cos(static_cast<float>(i) * 0.7f) : 0.0f;
            verts.push_back(
                MeshVertex{{u * static_cast<float>(w) + dx, v * static_cast<float>(h) + dy}, {u, v}});
        }
    }
    std::vector<uint16_t> indices;
    const auto at = [&](int x, int y) { return static_cast<uint16_t>(y * (cols + 1) + x); };
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            indices.insert(indices.end(), {at(i, j), at(i + 1, j), at(i + 1, j + 1)});
            indices.insert(indices.end(), {at(i, j), at(i + 1, j + 1), at(i, j + 1)});
        }
    }
    REQUIRE(indices.size() == 3000);
    CommandList cl = meshList(w, h);
    addMesh(cl, asset, verts, indices, 1.0f, true);
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer renderer;
    SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    IrView view{};
    renderer.render(cl, nullptr, &resolver, &view);  // warm-up (decode)
    const auto t0 = std::chrono::steady_clock::now();
    const int frames = 5;
    for (int f = 0; f < frames; ++f) {
        renderer.render(cl, nullptr, &resolver, &view);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
    MESSAGE("DrawImageMesh 1000 triangles, 1920x1080, smooth: " << ms << " ms/frame");
    CHECK(pixelAt(buf, w, w / 2, h / 2).a == 255);

    // Reference: one full-target DrawImage through the general (per-pixel inverse) path.
    CommandList ref = meshList(w, h);
    ImageCmd ic{};
    ic.xf = Mat2x3{1, 1e-7f, 1e-7f, 1, 0, 0};
    ic.asset = asset;
    ic.dst = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    ic.srcUv = Rect{0, 0, 1, 1};
    ic.opacity = 1;
    ic.smooth = 1;
    ref.images.push_back(ic);
    ref.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
    const auto r0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        renderer.render(ref, nullptr, &resolver, &view);
    }
    const auto r1 = std::chrono::steady_clock::now();
    MESSAGE("reference DrawImage (general path), 1920x1080, smooth: "
            << std::chrono::duration<double, std::milli>(r1 - r0).count() / frames << " ms/frame");
}
