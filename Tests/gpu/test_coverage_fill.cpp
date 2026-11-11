#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/coverage_bitmap.h"
#include "xt/geom.h"
#include "xt/gpu/coverage_atlas.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

#ifndef XT_GPU_NO_TEXT
#include "xt/text/ft_text_shaper.h"
#include "xt/text/glyph_atlas.h"
#endif

/// T-660b: 1x concave fills drawn from the coverage atlas -- the cache across frames, the frame
/// budget, page recycling (checked against a CPU mirror of the device pages) and the 4x path.
/// See Docs/design/10-backends.md §10.11 item 2, Docs/tasks/T-660b.md item 6.

using namespace xt;
using namespace xt::gpu;

namespace {

/// A device that keeps a CPU copy of every R8 texture so a coverage quad's texels can be read back.
class MirrorDevice : public GpuDevice {
public:
    struct Tex {
        uint16_t w = 0, h = 0;
        TextureFormat fmt = TextureFormat::Rgba8;
        bool alive = true;
        std::vector<uint8_t> r8;
    };
    std::map<TextureHandle, Tex> textures;
    TextureHandle next = 1;

    TextureHandle createTexture(const char*, uint16_t w, uint16_t h, TextureFormat fmt,
                                const void*) override {
        Tex t;
        t.w = w;
        t.h = h;
        t.fmt = fmt;
        if (fmt == TextureFormat::R8) {
            t.r8.assign(static_cast<size_t>(w) * h, 0xCD);  // garbage until uploaded
        }
        textures[next] = t;
        return next++;
    }
    void updateTexture(TextureHandle tex, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* data,
                       size_t rowPitch) override {
        REQUIRE(textures.count(tex) == 1);
        Tex& t = textures[tex];
        REQUIRE(t.alive);
        if (t.fmt != TextureFormat::R8) return;
        REQUIRE(x + w <= t.w);
        REQUIRE(y + h <= t.h);
        const auto* src = static_cast<const uint8_t*>(data);
        for (uint16_t row = 0; row < h; ++row) {
            std::memcpy(t.r8.data() + static_cast<size_t>(y + row) * t.w + x, src + row * rowPitch, w);
        }
    }
    void destroyTexture(TextureHandle tex) override {
        if (tex == 0) return;
        REQUIRE(textures.count(tex) == 1);
        textures[tex].alive = false;
    }
    void submit(const GpuFrame&) override {}
};

/// A concave L (side `s`, arm `s / 3`) drawn under `xf`, solid.
void addConcave(CommandList& cl, float s, const Mat2x3& xf) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    const float a = s / 3.0f;
    const float pts[12] = {0, 0, a, 0, a, s - a, s, s - a, s, s, 0, s};
    cl.verbs.push_back(PathVerb::Move);
    for (int i = 0; i < 5; ++i) cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    cl.points.insert(cl.points.end(), pts, pts + 12);
    ref.verbCount = 7;
    ref.pointCount = 12;
    FillCmd f{};
    f.xf = xf;
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = Color{0.2f, 0.4f, 0.8f, 1.0f};
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

CommandList makeList(int w, int h) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    return cl;
}

Mat2x3 rotatedAt(float deg, float x, float y) { return mul(translation(Vec2{x, y}), rotationDeg(deg)); }

size_t countCoverageCmds(const GpuFrame& f) {
    size_t n = 0;
    for (const DrawCmd& c : f.cmds) n += c.coverage != 0 ? 1 : 0;
    return n;
}

/// Coverage quads (4 vertices each) across every coverage command.
size_t countCoverageQuads(const GpuFrame& f) {
    size_t n = 0;
    for (const DrawCmd& c : f.cmds) n += c.coverage != 0 ? c.vertexCount / 4 : 0;
    return n;
}

size_t countWindingCmds(const GpuFrame& f) {
    size_t n = 0;
    for (const DrawCmd& c : f.cmds) n += c.stencilOp == StencilOp::WriteIncrWrap ? 1 : 0;
    return n;
}

/// Checks that the (only) coverage quad of `f` sits at the shape's whole-pixel position and samples
/// exactly the bitmap rasterizeCoverage gives `cl`'s fill `fill` -- from the mirrored device page.
void checkQuadAgainstRaster(const MirrorDevice& dev, const GpuFrame& f, const CommandList& cl, size_t fill) {
    const DrawCmd* quad = nullptr;
    for (const DrawCmd& c : f.cmds) {
        if (c.coverage != 0) {
            REQUIRE(quad == nullptr);
            quad = &c;
        }
    }
    REQUIRE(quad != nullptr);
    REQUIRE(quad->vertexCount == 4);
    const FillCmd& fc = cl.fills[fill];
    int32_t wholeX = 0, wholeY = 0;
    uint8_t subX = 0, subY = 0;
    REQUIRE(splitCoverageTranslation(fc.xf.e, wholeX, subX));
    REQUIRE(splitCoverageTranslation(fc.xf.f, wholeY, subY));
    Mat2x3 rasterXf = fc.xf;
    rasterXf.e = subX * 0.25f;
    rasterXf.f = subY * 0.25f;
    CoverageBitmap bm;
    REQUIRE(
        rasterizeCoverage(std::span<const PathVerb>(cl.verbs.data() + fc.path.verbOffset, fc.path.verbCount),
                          std::span<const float>(cl.points.data() + fc.path.pointOffset, fc.path.pointCount),
                          rasterXf, fc.rule, bm));
    const Vertex& v0 = f.vertices[quad->baseVertex];
    const Vertex& v2 = f.vertices[quad->baseVertex + 2];
    CHECK(v0.x == static_cast<float>(wholeX + bm.originX));
    CHECK(v0.y == static_cast<float>(wholeY + bm.originY));
    CHECK(v2.x - v0.x == static_cast<float>(bm.width));
    CHECK(v2.y - v0.y == static_cast<float>(bm.height));
    const auto it = dev.textures.find(quad->coverage);
    REQUIRE(it != dev.textures.end());
    const MirrorDevice::Tex& page = it->second;
    REQUIRE(page.alive);
    const int tx = static_cast<int>(std::lround(v0.u * page.w));
    const int ty = static_cast<int>(std::lround(v0.v * page.h));
    CHECK(std::lround(v2.u * page.w) - tx == bm.width);
    size_t mismatches = 0;
    for (int y = 0; y < bm.height; ++y) {
        for (int x = 0; x < bm.width; ++x) {
            const uint8_t got = page.r8[static_cast<size_t>(ty + y) * page.w + static_cast<size_t>(tx + x)];
            mismatches += got != bm.pixels[static_cast<size_t>(y) * bm.width + x] ? 1 : 0;
        }
    }
    CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("coverage fill: the second frame of a static screen hits every bitmap and rasterizes nothing") {
    CommandList cl = makeList(256, 256);
    addConcave(cl, 60, Mat2x3{1, 0, 0, 1, 10, 10});
    addConcave(cl, 60, rotatedAt(17, 140, 20));
    addConcave(cl, 60, Mat2x3{1, 0, 0, 1, 20.3f, 140});
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(256, 256);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.misses == 3);
    CHECK(s.hits == 0);
    CHECK(s.fallbacks == 0);
    CHECK(s.rasterizedPixels > 0);
    CHECK(s.atlasBytes == 512u * 512u);
    CHECK(countWindingCmds(frame) == 0);
    CHECK(countCoverageCmds(frame) >= 1);
    CHECK(builder.lastFrameReusable(nullptr, nullptr));

    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.misses == 0);
    CHECK(s.rasterizedPixels == 0);
    CHECK(s.hits == 3);
    CHECK(s.fallbacks == 0);
    builder.shutdown();
    CHECK(builder.lastCoverageStats().atlasBytes == 0);
}

TEST_CASE("coverage fill: a shape rotating every frame is rasterized every frame within the budget") {
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(256, 256);
    GpuFrame frame;
    for (int i = 0; i < 8; ++i) {
        CommandList cl = makeList(256, 256);
        addConcave(cl, 80, rotatedAt(5.0f * static_cast<float>(i), 128, 40));
        builder.build(cl, nullptr, nullptr, nullptr, frame);
        INFO("frame ", i);
        const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
        // A miss, or a hit when page 0 grew this frame and the frame was built a second time.
        CHECK(s.hits + s.misses == 1);
        CHECK(s.rasterizedPixels > 0);
        CHECK(s.fallbacks == 0);
        checkQuadAgainstRaster(dev, frame, cl, 0);
    }
}

TEST_CASE("coverage fill: a shape over 4096 px a side always falls back and asks for no other frame") {
    // T-672 raised the limit; T-662: a size-limit fallback is not a budget one (another build would
    // draw it the same), so the builder does not ask for another frame.
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1024, 1024);
    CHECK_FALSE(builder.wantsAnotherFrame());
    GpuFrame frame;
    CommandList big = makeList(1024, 1024);
    addConcave(big, 4200, Mat2x3{1, 0, 0, 1, -200, -200});
    builder.build(big, nullptr, nullptr, nullptr, frame);
    const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 1);
    CHECK(s.rasterizedPixels == 0);
    CHECK(countWindingCmds(frame) == 1);
    CHECK(countCoverageCmds(frame) == 0);
    CHECK_FALSE(builder.wantsAnotherFrame());
}

TEST_CASE("coverage fill: after pages are recycled every quad still samples its own bitmap") {
    // One shape per frame whose bitmap is 511..512 px (the largest kept whole, T-672), a page each:
    // two 513+ px boxes never share a 1024 page. Frames 0..3 fill the four pages (page 0 grows from
    // 512 on the way: that frame is built twice); frames 4..6 recycle the least recently used
    // pages, and shape 0 comes back in frame 6.
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1024, 1024);
    GpuFrame frame;
    const int order[] = {0, 1, 2, 3, 4, 5, 0, 4};
    const bool expectRaster[] = {true, true, true, true, true, true, true, false};
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); ++k) {
        INFO("frame ", k, " shape ", order[k]);
        CommandList cl = makeList(1024, 1024);
        addConcave(cl, 508.5f + 0.1f * static_cast<float>(order[k]), Mat2x3{1, 0, 0, 1, 300.25f, 10.5f});
        builder.build(cl, nullptr, nullptr, nullptr, frame);
        const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
        CHECK((s.rasterizedPixels > 0) == expectRaster[k]);
        CHECK(s.fallbacks == 0);
        CHECK(s.atlasBytes <= 4u * 1024u * 1024u);
        checkQuadAgainstRaster(dev, frame, cl, 0);
    }
}

TEST_CASE("coverage fill: 4x (edge fringe off), antialias 0 and convex fills never use the atlas") {
    CommandList cl = makeList(256, 256);
    addConcave(cl, 60, Mat2x3{1, 0, 0, 1, 10, 10});
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(256, 256);
    builder.setEdgeFringe(false);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.hits + s.misses + s.fallbacks == 0);
    CHECK(countCoverageCmds(frame) == 0);
    CHECK(countWindingCmds(frame) == 1);

    builder.setEdgeFringe(true);
    cl.fills[0].antialias = 0;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits + s.misses + s.fallbacks == 0);
    CHECK(countCoverageCmds(frame) == 0);
}

// ---------------------------------------------------------------------------------------------
// T-672: large shapes are tiled (Docs/design/10-backends.md §10.11 item 2, tiling supplement).
// ---------------------------------------------------------------------------------------------

namespace {

/// Adds a FillPath of the closed polygons `contours` (x, y pairs) under `xf`.
void addPolygons(CommandList& cl, const std::vector<std::vector<float>>& contours, const Mat2x3& xf,
                 FillRule rule, const PaintRef& paint) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    for (const std::vector<float>& c : contours) {
        cl.verbs.push_back(PathVerb::Move);
        for (size_t i = 2; i < c.size(); i += 2) cl.verbs.push_back(PathVerb::Line);
        cl.verbs.push_back(PathVerb::Close);
        cl.points.insert(cl.points.end(), c.begin(), c.end());
    }
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    FillCmd f{};
    f.xf = xf;
    f.path = ref;
    f.paint = paint;
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

PaintRef solidPaint() {
    PaintRef p{};
    p.kind = PaintKind::Solid;
    p.color = Color{0.9f, 0.3f, 0.1f, 1.0f};
    return p;
}

/// A 1600 x 900 rectangle with a notch in its top edge and a diamond hole in the middle (EvenOdd):
/// concave, two contours (a convex outline with a convex hole would be a direct ring fill).
std::vector<std::vector<float>> holedRect() {
    return {{0, 0, 700, 0, 800, 60, 900, 0, 1600, 0, 1600, 900, 0, 900},
            {800, 150, 1100, 450, 800, 750, 500, 450}};
}

/// A 1600 x 900 cross with a diamond hole in its centre (EvenOdd). At a whole-pixel offset its arms
/// (one 256 px tile thick) lie exactly on tile rows / columns of its bitmap (bitmap = local + 1), so
/// only the four arm ends and the hole tile are edge tiles: 5 edge tiles, 5 full, 18 empty.
std::vector<std::vector<float>> holedCross() {
    return {{0,    255, 767,  255, 767,  0,   1023, 0,   1023, 255, 1600, 255,
             1600, 511, 1023, 511, 1023, 900, 767,  900, 767,  511, 0,    511},
            {895, 303, 975, 383, 895, 463, 815, 383}};
}

/// The device-space coverage the frame's coverage quads (4 vertices each; consecutive quads are
/// batched into one command) put on a w x h target, read from the mirrored pages: a quad with
/// u0 != u1 maps its texels 1:1 on its pixels; one with u0 == u1 (a run of full tiles) samples the
/// single texel under that uv. `fullRuns` counts the latter.
std::vector<uint8_t> coverageImage(const MirrorDevice& dev, const GpuFrame& f, int w, int h,
                                   size_t& fullRuns) {
    std::vector<uint8_t> img(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    fullRuns = 0;
    for (const DrawCmd& c : f.cmds) {
        if (c.coverage == 0) continue;
        REQUIRE(c.vertexCount % 4 == 0);
        for (uint32_t q = 0; q < c.vertexCount; q += 4) {
            const Vertex& v0 = f.vertices[c.baseVertex + q];
            const Vertex& v2 = f.vertices[c.baseVertex + q + 2];
            const auto it = dev.textures.find(c.coverage);
            REQUIRE(it != dev.textures.end());
            const MirrorDevice::Tex& page = it->second;
            REQUIRE(page.alive);
            const int x0 = static_cast<int>(v0.x), y0 = static_cast<int>(v0.y);
            const int x1 = static_cast<int>(v2.x), y1 = static_cast<int>(v2.y);
            const bool full = v0.u == v2.u;
            fullRuns += full ? 1 : 0;
            const int tx =
                full ? static_cast<int>(v0.u * page.w) : static_cast<int>(std::lround(v0.u * page.w));
            const int ty =
                full ? static_cast<int>(v0.v * page.h) : static_cast<int>(std::lround(v0.v * page.h));
            for (int y = std::max(y0, 0); y < std::min(y1, h); ++y) {
                for (int x = std::max(x0, 0); x < std::min(x1, w); ++x) {
                    const int sx = full ? tx : tx + (x - x0);
                    const int sy = full ? ty : ty + (y - y0);
                    img[static_cast<size_t>(y) * w + x] =
                        page.r8[static_cast<size_t>(sy) * page.w + static_cast<size_t>(sx)];
                }
            }
        }
    }
    return img;
}

/// Pixels where coverageImage() differs from rasterizeCoverage (= SW's alpha, T-660a) of fill
/// `fill` placed at its whole-pixel translation.
size_t coverageMismatches(const MirrorDevice& dev, const GpuFrame& f, const CommandList& cl, size_t fill,
                          int w, int h, size_t& fullRuns) {
    const std::vector<uint8_t> img = coverageImage(dev, f, w, h, fullRuns);
    const FillCmd& fc = cl.fills[fill];
    int32_t wholeX = 0, wholeY = 0;
    uint8_t subX = 0, subY = 0;
    REQUIRE(splitCoverageTranslation(fc.xf.e, wholeX, subX));
    REQUIRE(splitCoverageTranslation(fc.xf.f, wholeY, subY));
    Mat2x3 rasterXf = fc.xf;
    rasterXf.e = subX * 0.25f;
    rasterXf.f = subY * 0.25f;
    CoverageBitmap bm;
    REQUIRE(
        rasterizeCoverage(std::span<const PathVerb>(cl.verbs.data() + fc.path.verbOffset, fc.path.verbCount),
                          std::span<const float>(cl.points.data() + fc.path.pointOffset, fc.path.pointCount),
                          rasterXf, fc.rule, bm));
    std::vector<uint8_t> want(img.size(), 0);
    for (int y = 0; y < bm.height; ++y) {
        for (int x = 0; x < bm.width; ++x) {
            const int dx = wholeX + bm.originX + x, dy = wholeY + bm.originY + y;
            REQUIRE(dx >= 0);
            REQUIRE(dy >= 0);
            REQUIRE(dx < w);
            REQUIRE(dy < h);
            want[static_cast<size_t>(dy) * w + dx] = bm.pixels[static_cast<size_t>(y) * bm.width + x];
        }
    }
    size_t mismatches = 0;
    for (size_t i = 0; i < img.size(); ++i) mismatches += img[i] != want[i] ? 1 : 0;
    return mismatches;
}

}  // namespace

TEST_CASE("coverage fill: a 1600x900 shape with a hole is tiled, not a fallback, and matches SW coverage") {
    CommandList cl = makeList(1700, 1000);
    addPolygons(cl, holedCross(), Mat2x3{1, 0, 0, 1, 20, 30}, FillRule::EvenOdd, solidPaint());
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1700, 1000);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(s.hits + s.misses == 1);
    CHECK(countWindingCmds(frame) == 0);
    CHECK(s.tiles > 0);
    CHECK(s.uniformTiles > s.tiles);  // the edge tiles are far fewer than all of them
    CHECK(s.tiles == 5);
    CHECK(s.tiles + s.uniformTiles == 7u * 4u);
    size_t fullRuns = 0;
    CHECK(coverageMismatches(dev, frame, cl, 0, 1700, 1000, fullRuns) == 0);
    // Horizontally adjacent full tiles are one quad: (1,1)-(2,1), (4,1)-(5,1) and (3,2).
    CHECK(fullRuns == 3);
    CHECK(countCoverageQuads(frame) == s.tiles + fullRuns);

    // Static: the second frame hits every tile and rasterizes nothing.
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits == 1);
    CHECK(s.misses == 0);
    CHECK(s.rasterizedPixels == 0);
    CHECK(s.fallbacks == 0);
    CHECK(coverageMismatches(dev, frame, cl, 0, 1700, 1000, fullRuns) == 0);
}

TEST_CASE("coverage fill: a 1600x900 notched shape with a hole at a fractional offset matches SW coverage") {
    CommandList cl = makeList(1700, 1000);
    addPolygons(cl, holedRect(), Mat2x3{1, 0, 0, 1, 20.3f, 30.6f}, FillRule::EvenOdd, solidPaint());
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1700, 1000);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
    CHECK(s.tiles + s.uniformTiles == 7u * 4u);
    size_t fullRuns = 0;
    CHECK(coverageMismatches(dev, frame, cl, 0, 1700, 1000, fullRuns) == 0);
    CHECK(fullRuns > 0);
}

TEST_CASE(
    "coverage fill: full tiles of a gradient fill are plain gradient quads, edge tiles coverage quads") {
    CommandList cl = makeList(1700, 1000);
    cl.stops.push_back(GradientStop{0.0f, Color{1, 0, 0, 1}});
    cl.stops.push_back(GradientStop{1.0f, Color{0, 0, 1, 1}});
    PaintRef paint{};
    paint.kind = PaintKind::Linear;
    paint.color = Color{1, 1, 1, 1};
    paint.p0 = Vec2{0, 0};
    paint.p1 = Vec2{1600, 0};
    paint.stopOffset = 0;
    paint.stopCount = 2;
    addPolygons(cl, holedCross(), Mat2x3{1, 0, 0, 1, 20, 30}, FillRule::EvenOdd, paint);
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1700, 1000);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
    size_t coverageQuads = 0, plainGradient = 0;
    for (const DrawCmd& c : frame.cmds) {
        if (c.shading != Shading::Gradient) continue;
        CHECK(c.stencilOp == StencilOp::TestEqualRef);
        REQUIRE(c.vertexCount % 4 == 0);
        for (uint32_t q = 0; q < c.vertexCount; q += 4) {
            if (c.coverage != 0) {
                ++coverageQuads;
                const Vertex& v0 = frame.vertices[c.baseVertex + q];
                const Vertex& v2 = frame.vertices[c.baseVertex + q + 2];
                CHECK(v0.u != v2.u);  // never a constant-uv quad
            } else {
                ++plainGradient;
            }
        }
    }
    CHECK(coverageQuads == s.tiles);
    CHECK(plainGradient == 3);  // the three runs of full tiles
}

TEST_CASE("coverage fill: a 3000x4 thin concave shape is tiled and drawn") {
    CommandList cl = makeList(3100, 64);
    addPolygons(cl, {{0, 0, 3000, 0, 3000, 4, 1500, 1.5f, 0, 4}}, Mat2x3{1, 0, 0, 1, 40.5f, 20.25f},
                FillRule::NonZero, solidPaint());
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(3100, 64);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
    CHECK(s.tiles + s.uniformTiles == 12);  // 3003 x 7 -> 12 x 1 tiles
    size_t fullRuns = 0;
    CHECK(coverageMismatches(dev, frame, cl, 0, 3100, 64, fullRuns) == 0);
}

TEST_CASE("coverage fill: the first shape over the frame budget is drawn; the next waits a frame") {
    // Shape A: a 1453 px square with a notch, bitmap 1455 x 1455 = 2.1 Mpx > the 2 Mpx budget,
    // first in the frame: rasterized anyway. Shape B (small) comes after it: held back this frame,
    // drawn from the atlas the next.
    CommandList cl = makeList(1600, 1600);
    addPolygons(cl, {{0, 0, 1453, 0, 1453, 1453, 800, 1453, 726.5f, 1300, 653, 1453, 0, 1453}},
                Mat2x3{1, 0, 0, 1, 10, 10}, FillRule::NonZero, solidPaint());
    addConcave(cl, 60, Mat2x3{1, 0, 0, 1, 1500.25f, 1500});
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1600, 1600);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.rasterizedPixels > CoverageAtlas::kDefaultFrameBudget);
    CHECK(s.fallbacks == 1);  // B
    CHECK(countWindingCmds(frame) == 1);
    CHECK(s.tiles + s.uniformTiles == 36);  // A was drawn: 6 x 6 tiles

    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(s.hits == 1);    // A
    CHECK(s.misses == 1);  // B, rasterized now
    CHECK(s.rasterizedPixels > 0);
    CHECK(s.rasterizedPixels < 10000u);
    CHECK(countWindingCmds(frame) == 0);

    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits == 2);
    CHECK(s.rasterizedPixels == 0);
}

// ---------------------------------------------------------------------------------------------
// T-661: strokes the direct paths do not take use the coverage atlas (Docs/design/10-backends.md
// §10.11 item 3, Docs/tasks/T-661.md).
// ---------------------------------------------------------------------------------------------

namespace {

/// Adds a StrokePath of the open polyline `pts` (x, y pairs) under `xf`; `dashes` empty = solid.
void addPolylineStroke(CommandList& cl, const std::vector<float>& pts, const Mat2x3& xf, float width,
                       LineJoin join = LineJoin::Miter, LineCap cap = LineCap::Butt, float miterLimit = 10.0f,
                       const std::vector<float>& dashes = {}, float dashPhase = 0.0f) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    for (size_t i = 2; i < pts.size(); i += 2) cl.verbs.push_back(PathVerb::Line);
    cl.points.insert(cl.points.end(), pts.begin(), pts.end());
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    StrokeCmd s{};
    s.xf = xf;
    s.path = ref;
    s.paint.kind = PaintKind::Solid;
    s.paint.color = Color{0.1f, 0.2f, 0.3f, 1.0f};
    s.width = width;
    s.cap = cap;
    s.join = join;
    s.miterLimit = miterLimit;
    s.dashOffset = static_cast<uint32_t>(cl.dashes.size());
    s.dashCount = static_cast<uint32_t>(dashes.size());
    s.dashPhase = dashPhase;
    cl.dashes.insert(cl.dashes.end(), dashes.begin(), dashes.end());
    cl.strokes.push_back(s);
    cl.commands.push_back(Command{CmdKind::StrokePath, {}, static_cast<uint32_t>(cl.strokes.size() - 1)});
}

const std::vector<float> kZigzag = {0, 40, 20, 0, 40, 40, 60, 10};

/// Checks that the (only) coverage quad of `f` samples exactly what SW makes of stroke `index` of
/// `cl` -- the path flattened under the raster transform, strokeToFill at width x sqrt|det xf|, a
/// NonZero fill of that outline -- read back from the mirrored device page.
void checkStrokeQuadAgainstRaster(const MirrorDevice& dev, const GpuFrame& f, const CommandList& cl,
                                  size_t index) {
    const DrawCmd* quad = nullptr;
    for (const DrawCmd& c : f.cmds) {
        if (c.coverage != 0) {
            REQUIRE(quad == nullptr);
            quad = &c;
        }
    }
    REQUIRE(quad != nullptr);
    REQUIRE(quad->vertexCount == 4);
    const StrokeCmd& sc = cl.strokes[index];
    int32_t wholeX = 0, wholeY = 0;
    uint8_t subX = 0, subY = 0;
    REQUIRE(splitCoverageTranslation(sc.xf.e, wholeX, subX));
    REQUIRE(splitCoverageTranslation(sc.xf.f, wholeY, subY));
    Mat2x3 rasterXf = sc.xf;
    rasterXf.e = subX * 0.25f;
    rasterXf.f = subY * 0.25f;
    std::vector<Vec2> flat;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closed;
    flattenPath(std::span<const PathVerb>(cl.verbs.data() + sc.path.verbOffset, sc.path.verbCount),
                std::span<const float>(cl.points.data() + sc.path.pointOffset, sc.path.pointCount), rasterXf,
                kFlattenTolerance, flat, starts, closed);
    std::vector<Vec2> outline;
    std::vector<uint32_t> outlineStarts;
    strokeToFill(flat, starts, closed, sc.width * std::sqrt(std::fabs(determinant(sc.xf))), sc.cap, sc.join,
                 sc.miterLimit, kFlattenTolerance, outline, outlineStarts);
    CoverageBitmap bm;
    REQUIRE(rasterizeCoverageContours(outline, outlineStarts, FillRule::NonZero, bm));
    const Vertex& v0 = f.vertices[quad->baseVertex];
    const Vertex& v2 = f.vertices[quad->baseVertex + 2];
    CHECK(v0.x == static_cast<float>(wholeX + bm.originX));
    CHECK(v0.y == static_cast<float>(wholeY + bm.originY));
    CHECK(v2.x - v0.x == static_cast<float>(bm.width));
    CHECK(v2.y - v0.y == static_cast<float>(bm.height));
    const auto it = dev.textures.find(quad->coverage);
    REQUIRE(it != dev.textures.end());
    const MirrorDevice::Tex& page = it->second;
    const int tx = static_cast<int>(std::lround(v0.u * page.w));
    const int ty = static_cast<int>(std::lround(v0.v * page.h));
    size_t mismatches = 0;
    for (int y = 0; y < bm.height; ++y) {
        for (int x = 0; x < bm.width; ++x) {
            const uint8_t got = page.r8[static_cast<size_t>(ty + y) * page.w + static_cast<size_t>(tx + x)];
            mismatches += got != bm.pixels[static_cast<size_t>(y) * bm.width + x] ? 1 : 0;
        }
    }
    CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("coverage stroke: a polyline is one coverage quad sampling the SW stroke coverage") {
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(256, 256);
    GpuFrame frame;
    const Mat2x3 xfs[] = {Mat2x3{1, 0, 0, 1, 20, 30}, Mat2x3{1, 0, 0, 1, 20.3f, 30.6f},
                          rotatedAt(13, 100, 60),
                          mul(Mat2x3{1, 0, 0, 1, 40.25f, 40}, scaling(Vec2{1.5f, 1.5f}))};
    for (const Mat2x3& xf : xfs) {
        CommandList cl = makeList(256, 256);
        addPolylineStroke(cl, kZigzag, xf, 2.0f);
        builder.build(cl, nullptr, nullptr, nullptr, frame);
        const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
        CHECK(s.fallbacks == 0);
        CHECK(countWindingCmds(frame) == 0);
        checkStrokeQuadAgainstRaster(dev, frame, cl, 0);
    }
}

TEST_CASE("coverage stroke: strokes differing in any geometry parameter never share an atlas entry") {
    // The same path, each stroke differing from the first in one parameter: width, join, cap,
    // miter limit, dash array, dash phase. Every one is its own miss.
    CommandList cl = makeList(256, 256);
    const Mat2x3 at{1, 0, 0, 1, 20, 20};
    addPolylineStroke(cl, kZigzag, at, 2.0f);
    addPolylineStroke(cl, kZigzag, at, 2.5f);
    addPolylineStroke(cl, kZigzag, at, 2.0f, LineJoin::Round);
    addPolylineStroke(cl, kZigzag, at, 2.0f, LineJoin::Bevel);
    addPolylineStroke(cl, kZigzag, at, 2.0f, LineJoin::Miter, LineCap::Square);
    addPolylineStroke(cl, kZigzag, at, 2.0f, LineJoin::Miter, LineCap::Butt, 1.5f);
    addPolylineStroke(cl, kZigzag, at, 2.0f, LineJoin::Miter, LineCap::Butt, 10.0f, {9, 1});
    addPolylineStroke(cl, kZigzag, at, 2.0f, LineJoin::Miter, LineCap::Butt, 10.0f, {9, 2});
    addPolylineStroke(cl, kZigzag, at, 2.0f, LineJoin::Miter, LineCap::Butt, 10.0f, {9, 1}, 3.0f);
    constexpr uint32_t kStrokes = 9;
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(256, 256);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
    CHECK(s.misses == kStrokes);
    CHECK(s.hits == 0);

    // The same strokes again, moved by whole pixels: all hits (the key holds no whole translation).
    for (StrokeCmd& sc : cl.strokes) {
        sc.xf.e += 30;
        sc.xf.f += 50;
    }
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits == kStrokes);
    CHECK(s.misses == 0);

    // The fill of the same path does not take the stroke's entry, and the stroke under a 2x scale
    // at half the width (the same device width) is not the unscaled stroke's either.
    CommandList other = makeList(256, 256);
    other.verbs = cl.verbs;
    other.points = cl.points;
    FillCmd fc{};
    fc.xf = at;
    fc.path = cl.strokes[0].path;
    fc.paint.kind = PaintKind::Solid;
    fc.paint.color = Color{1, 0, 0, 1};
    fc.rule = FillRule::NonZero;
    fc.antialias = 1;
    other.fills.push_back(fc);
    other.commands.push_back(Command{CmdKind::FillPath, {}, 0});
    StrokeCmd scaled = cl.strokes[0];
    scaled.xf = mul(at, scaling(Vec2{2, 2}));
    scaled.width = 1.0f;
    other.strokes.push_back(scaled);
    other.commands.push_back(Command{CmdKind::StrokePath, {}, 0});
    builder.build(other, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits == 0);
    CHECK(s.misses == 2);
}

TEST_CASE("coverage stroke: the second frame of a static screen hits every stroke and rasterizes nothing") {
    CommandList cl = makeList(256, 256);
    addPolylineStroke(cl, kZigzag, Mat2x3{1, 0, 0, 1, 10, 10}, 2.0f, LineJoin::Round);
    addPolylineStroke(cl, kZigzag, rotatedAt(13, 140, 20), 3.0f, LineJoin::Miter, LineCap::Round, 10.0f,
                      {8, 2});
    addPolylineStroke(cl, kZigzag, Mat2x3{1, 0, 0, 1, 20.3f, 140}, 0.25f);
    // About 2450 px long and 1 px wide: tiled.
    addPolylineStroke(cl, {-2200, 240, 120, 250, 250, 244}, Mat2x3{}, 1.0f);
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(256, 256);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(s.hits + s.misses == 4);
    CHECK(s.tiles + s.uniformTiles > 0);
    CHECK(countWindingCmds(frame) == 0);

    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.misses == 0);
    CHECK(s.rasterizedPixels == 0);
    CHECK(s.hits == 4);
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
}

TEST_CASE("coverage stroke: direct convex strokes and 4x strokes never use the atlas") {
    // A single straight segment and a dash whose pieces do not touch are direct convex fans
    // (unchanged); a polyline at 4x (edge fringe off) is stencil-then-cover as before.
    CommandList cl = makeList(256, 256);
    addPolylineStroke(cl, {10, 10, 200, 60}, Mat2x3{}, 2.0f);
    addPolylineStroke(cl, {10, 100, 240, 100}, Mat2x3{}, 2.0f, LineJoin::Miter, LineCap::Butt, 10.0f,
                      {10, 10});
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(256, 256);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.hits + s.misses + s.fallbacks == 0);
    CHECK(countCoverageCmds(frame) == 0);
    CHECK(countWindingCmds(frame) == 0);

    CommandList poly = makeList(256, 256);
    addPolylineStroke(poly, kZigzag, Mat2x3{1, 0, 0, 1, 10, 10}, 2.0f);
    builder.setEdgeFringe(false);
    builder.build(poly, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits + s.misses + s.fallbacks == 0);
    CHECK(countCoverageCmds(frame) == 0);
    CHECK(countWindingCmds(frame) == 1);
}

// ---------------------------------------------------------------------------------------------
// T-662: the frame budget counts covered pixels after each shape, and a frame whose budget held a
// shape back asks for another one (Docs/design/10-backends.md §10.11 item 2, supplement 2).
// ---------------------------------------------------------------------------------------------

namespace {

/// A `side` px square with a 20 % wide, 20 % deep notch in its bottom edge (concave).
std::vector<std::vector<float>> notchedSquare(float side) {
    const float k = side / 100.0f;
    return {
        {0, 0, 100 * k, 0, 100 * k, 100 * k, 60 * k, 100 * k, 50 * k, 80 * k, 40 * k, 100 * k, 0, 100 * k}};
}

/// Pixels where the frame's coverage image differs from rasterizeCoverage (SW's alpha) of every
/// fill of `cl` (which must not overlap), each at its whole-pixel translation.
size_t coverageMismatchesAll(const MirrorDevice& dev, const GpuFrame& f, const CommandList& cl, int w,
                             int h) {
    size_t fullRuns = 0;
    const std::vector<uint8_t> img = coverageImage(dev, f, w, h, fullRuns);
    std::vector<uint8_t> want(img.size(), 0);
    for (const FillCmd& fc : cl.fills) {
        int32_t wholeX = 0, wholeY = 0;
        uint8_t subX = 0, subY = 0;
        REQUIRE(splitCoverageTranslation(fc.xf.e, wholeX, subX));
        REQUIRE(splitCoverageTranslation(fc.xf.f, wholeY, subY));
        Mat2x3 rasterXf = fc.xf;
        rasterXf.e = subX * 0.25f;
        rasterXf.f = subY * 0.25f;
        CoverageBitmap bm;
        REQUIRE(rasterizeCoverage(
            std::span<const PathVerb>(cl.verbs.data() + fc.path.verbOffset, fc.path.verbCount),
            std::span<const float>(cl.points.data() + fc.path.pointOffset, fc.path.pointCount), rasterXf,
            fc.rule, bm));
        for (int y = 0; y < bm.height; ++y) {
            for (int x = 0; x < bm.width; ++x) {
                const int dx = wholeX + bm.originX + x, dy = wholeY + bm.originY + y;
                REQUIRE(dx >= 0);
                REQUIRE(dy >= 0);
                REQUIRE(dx < w);
                REQUIRE(dy < h);
                const uint8_t v = bm.pixels[static_cast<size_t>(y) * bm.width + x];
                if (v != 0) want[static_cast<size_t>(dy) * w + dx] = v;
            }
        }
    }
    size_t mismatches = 0;
    for (size_t i = 0; i < img.size(); ++i) mismatches += img[i] != want[i] ? 1 : 0;
    return mismatches;
}

}  // namespace

TEST_CASE("coverage budget: rasterizeCoverage reports the pixels it covered, not its box") {
    // A 2400 px long, 1.4 px wide diagonal (45 deg): the box is ~1700 x 1700 = 2.9 Mpx, the covered
    // pixels a few thousand.
    CommandList cl = makeList(2000, 2000);
    const float d = 2400.0f / std::sqrt(2.0f);
    addPolygons(cl, {{0, 0, 1.4f, 0, d + 1.4f, d, d, d}}, Mat2x3{1, 0, 0, 1, 50, 50}, FillRule::NonZero,
                solidPaint());
    const FillCmd& fc = cl.fills[0];
    CoverageBitmap bm;
    REQUIRE(rasterizeCoverage(std::span<const PathVerb>(cl.verbs.data(), fc.path.verbCount),
                              std::span<const float>(cl.points.data(), fc.path.pointCount), fc.xf, fc.rule,
                              bm));
    CHECK(static_cast<uint64_t>(bm.width) * static_cast<uint64_t>(bm.height) > 2000000u);
    CHECK(bm.coveredPixels > 0u);
    CHECK(bm.coveredPixels < 20000u);
    uint64_t nonzero = 0;
    for (uint8_t v : bm.pixels) nonzero += v != 0 ? 1 : 0;
    CHECK(bm.coveredPixels == nonzero);
}

TEST_CASE("coverage budget: six 0.5 Mpx shapes over the 2 Mpx budget are completed by the next frames") {
    // Six notched squares of 720..725 px (about 0.51 Mpx covered each, 3 Mpx in all), side by side.
    constexpr int kW = 2300, kH = 1600;
    CommandList cl = makeList(kW, kH);
    for (int i = 0; i < 6; ++i) {
        const float x = 10.0f + static_cast<float>(i % 3) * 760.0f;
        const float y = 10.0f + static_cast<float>(i / 3) * 760.0f;
        addPolygons(cl, notchedSquare(720.0f + static_cast<float>(i)), Mat2x3{1, 0, 0, 1, x, y},
                    FillRule::NonZero, solidPaint());
    }
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(kW, kH);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    // 4 x 0.51 Mpx reach the budget: shapes 5 and 6 are held back (stencil-then-cover) this frame.
    CHECK(s.fallbacks == 2);
    CHECK(countWindingCmds(frame) == 2);
    CHECK(s.rasterizedPixels >= CoverageAtlas::kDefaultFrameBudget);
    CHECK(builder.wantsAnotherFrame());
    CHECK_FALSE(builder.lastFrameReusable(nullptr, nullptr));

    int frames = 1;
    while (builder.wantsAnotherFrame() && frames < 8) {
        builder.build(cl, nullptr, nullptr, nullptr, frame);
        ++frames;
    }
    CHECK(frames == 2);
    s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(s.misses == 2);
    CHECK(s.hits == 4);
    CHECK(countWindingCmds(frame) == 0);
    CHECK_FALSE(builder.wantsAnotherFrame());
    CHECK(coverageMismatchesAll(dev, frame, cl, kW, kH) == 0);

    // The completed frame is reusable and a further build rasterizes nothing.
    CHECK(builder.lastFrameReusable(nullptr, nullptr));
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits == 6);
    CHECK(s.rasterizedPixels == 0);
    CHECK_FALSE(builder.wantsAnotherFrame());
}

TEST_CASE("coverage budget: a 2400 px 45 deg thin stroke no longer uses up the budget") {
    // Before T-662 the stroke's ~1700 x 1700 box (2.9 Mpx) counted against the budget and held back
    // the shape after it; its covered pixels are a few thousand.
    CommandList cl = makeList(2000, 2000);
    const float d = 2400.0f / std::sqrt(2.0f);
    addPolylineStroke(cl, {0, 0, d * 0.5f, d * 0.5f + 0.5f, d, d}, Mat2x3{1, 0, 0, 1, 50, 50}, 1.5f);
    addConcave(cl, 300.0f, Mat2x3{1, 0, 0, 1, 1500, 100});
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(2000, 2000);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
    CHECK(s.rasterizedPixels < 100000u);
    CHECK_FALSE(builder.wantsAnotherFrame());
}

// ---------------------------------------------------------------------------------------------
// T-662: outline glyphs use the coverage atlas at 1x (Docs/design/10-backends.md §10.11 item 4,
// Docs/tasks/T-662.md item 9).
// ---------------------------------------------------------------------------------------------

#ifndef XT_GPU_NO_TEXT

namespace {

std::string glyphFontPath() { return std::string(XT_TEST_FONTS_DIR) + "/NotoSans-Regular.subset.ttf"; }

/// One DrawGlyphs run of `text` shaped with Noto Sans at `size`, pen at `origin`, under `xf`.
CommandList glyphRunList(text::FtTextShaper& shaper, const std::string& textStr, float size, Vec2 origin,
                         const Mat2x3& xf, int w, int h) {
    static const std::vector<std::string_view> families = {"Noto Sans"};
    ShapeParams params;
    params.font = FontRequest{std::span<const std::string_view>(families), 400, false};
    params.size = size;
    ShapeResult shaped;
    shaper.shape(textStr, params, shaped);
    REQUIRE(!shaped.runs.empty());
    CommandList list = makeList(w, h);
    GlyphsCmd cmd{};
    cmd.xf = xf;
    cmd.font = shaped.runs[0].font;
    cmd.size = size;
    cmd.color = Color{0.1f, 0.1f, 0.1f, 1.0f};
    cmd.glyphOffset = 0;
    float penX = origin.x;
    for (const ShapedGlyph& g : shaped.glyphs) {
        list.glyphs.push_back(g.glyphId);
        list.glyphPos.push_back(Vec2{penX + g.xOffset, origin.y + g.yOffset});
        penX += g.xAdvance;
    }
    cmd.glyphCount = static_cast<uint32_t>(list.glyphs.size());
    list.glyphRuns.push_back(cmd);
    list.commands.push_back(Command{CmdKind::DrawGlyphs, {}, 0});
    return list;
}

size_t countGlyphCmds(const GpuFrame& f) {
    size_t n = 0;
    for (const DrawCmd& c : f.cmds) n += c.shading == Shading::Glyph ? 1 : 0;
    return n;
}

}  // namespace

TEST_CASE("coverage text: 120 px glyphs, convex ones too, are coverage quads; the second frame hits") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(glyphFontPath()));
    text::GlyphAtlas atlas(shaper, 256);
    // "H" is concave; "I" and "l" are convex (they took the fringed direct path before T-662).
    CommandList cl = glyphRunList(shaper, "HIl", 120.0f, Vec2{10, 130}, Mat2x3{}, 400, 200);
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(400, 200);
    builder.setGlyphAtlas(&atlas);
    GpuFrame frame;
    builder.build(cl, &shaper, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.hits + s.misses == 3);
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
    CHECK(countGlyphCmds(frame) == 0);
    CHECK(countCoverageQuads(frame) == 3);
    size_t solid = 0;
    for (const DrawCmd& c : frame.cmds) solid += c.coverage == 0 && c.shading == Shading::Solid ? 1 : 0;
    CHECK(solid == 0);  // no glyph on the direct (fringed) path

    builder.build(cl, &shaper, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.misses == 0);
    CHECK(s.hits == 3);
    CHECK(s.rasterizedPixels == 0);
    CHECK_FALSE(builder.wantsAnotherFrame());

    // 4x (no edge fringe) keeps stencil-then-cover and the direct paths, and never the atlas.
    builder.setEdgeFringe(false);
    builder.build(cl, &shaper, nullptr, nullptr, frame);
    CHECK(countCoverageCmds(frame) == 0);
    CHECK(countWindingCmds(frame) >= 1);
}

TEST_CASE("coverage text: the same glyph twice in a run is one atlas entry; 96 px atlas text is unchanged") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(glyphFontPath()));
    text::GlyphAtlas atlas(shaper, 512);
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(600, 200);
    builder.setGlyphAtlas(&atlas);
    GpuFrame frame;
    // Whole-pixel pen positions: both "o"s are at the same quarter-pixel phase.
    CommandList cl = glyphRunList(shaper, "oo", 128.0f, Vec2{10, 140}, Mat2x3{}, 600, 200);
    cl.glyphPos[1].x = std::floor(cl.glyphPos[1].x);
    builder.build(cl, &shaper, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.misses + s.hits == 2);
    CHECK(s.misses <= 1);  // the second "o" hits the first one's bitmap (or both hit after a regrow)

    CommandList small = glyphRunList(shaper, "Hello", 96.0f, Vec2{10, 140}, Mat2x3{}, 600, 200);
    builder.build(small, &shaper, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits + s.misses + s.fallbacks == 0);
    CHECK(countGlyphCmds(frame) == 1);
}

TEST_CASE("coverage text: a run scaled every frame is rasterized within the budget, then completed") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(glyphFontPath()));
    text::GlyphAtlas atlas(shaper, 512);
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1024, 512);
    builder.setGlyphAtlas(&atlas);
    GpuFrame frame;

    // 48 px text scaled every frame (runIsBeingScaled: drawn from outlines): within the budget it
    // is rasterized every frame and nothing falls back.
    for (int i = 0; i < 4; ++i) {
        INFO("small frame ", i);
        const float k = 1.0f + 0.07f * static_cast<float>(i);
        CommandList cl = glyphRunList(shaper, "Scale", 48.0f, Vec2{0, 0},
                                      mul(translation(Vec2{20, 120}), scaling(Vec2{k, k})), 1024, 512);
        builder.build(cl, &shaper, nullptr, nullptr, frame);
        const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
        if (i > 0) {
            CHECK(s.misses + s.hits > 0);
            CHECK(s.rasterizedPixels > 0);
        }
        CHECK(s.fallbacks == 0);
        CHECK_FALSE(builder.wantsAnotherFrame());
    }

    // Over the budget: a notched 1453 px square (about 2.1 Mpx covered, the frame's first shape,
    // so always drawn) whose scale moves every frame takes the whole budget, so the scaled run after
    // it is held back -- drawn stencil-then-cover -- and another frame is asked for.
    auto overBudget = [&](float fillScale, float k) {
        CommandList cl = glyphRunList(shaper, "Scale", 48.0f, Vec2{0, 0},
                                      mul(translation(Vec2{20, 1550}), scaling(Vec2{k, k})), 1600, 1600);
        const Command glyphs = cl.commands[0];
        cl.commands.clear();
        addPolygons(cl, {{0, 0, 1453, 0, 1453, 1453, 800, 1453, 726.5f, 1300, 653, 1453, 0, 1453}},
                    mul(translation(Vec2{10, 10}), scaling(Vec2{fillScale, fillScale})), FillRule::NonZero,
                    solidPaint());
        cl.commands.push_back(glyphs);
        return cl;
    };
    builder.resize(1600, 1600);
    float k = 1.3f;
    for (int i = 0; i < 3; ++i) {
        INFO("over-budget frame ", i);
        k = 1.3f + 0.05f * static_cast<float>(i);
        CommandList cl = overBudget(1.0f - 0.001f * static_cast<float>(i), k);
        builder.build(cl, &shaper, nullptr, nullptr, frame);
        const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
        CHECK(s.fallbacks == 5);  // the five glyphs of "Scale"
        CHECK(countWindingCmds(frame) >= 1);
        CHECK(builder.wantsAnotherFrame());
        CHECK_FALSE(builder.lastFrameReusable(nullptr, nullptr));
    }
    // The animation stops (the same list again): the square hits and the glyphs are rasterized --
    // complete in one frame.
    CommandList last = overBudget(1.0f - 0.002f, k);
    builder.build(last, &shaper, nullptr, nullptr, frame);
    const FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(countWindingCmds(frame) == 0);
    CHECK_FALSE(builder.wantsAnotherFrame());
}

// T-675 (Docs/design/10-backends.md §10.11, T-662 record): the coverage key of an outline glyph holds the
// shaper's generation, not its address. A second shaper made in the storage of the first one (so at the
// same address, as a bundle reload may do) gets the same FontId and glyph ids, and must not hit the
// first one's bitmaps.
TEST_CASE("coverage text: a new shaper at the same address does not hit the old shaper's glyphs") {
    alignas(text::FtTextShaper) unsigned char storage[sizeof(text::FtTextShaper)];
    auto* first = new (storage) text::FtTextShaper;
    REQUIRE(first->addFontFile(glyphFontPath()));
    const uint64_t firstGeneration = first->generation();
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(400, 200);
    GpuFrame frame;
    // No glyph atlas: every glyph is an outline glyph, drawn as a coverage quad (edge fringe on).
    CommandList cl = glyphRunList(*first, "HIl", 120.0f, Vec2{10, 130}, Mat2x3{}, 400, 200);
    builder.build(cl, first, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    REQUIRE(s.misses == 3);
    builder.build(cl, first, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    REQUIRE(s.hits == 3);  // the same shaper hits its own bitmaps

    first->~FtTextShaper();
    auto* second = new (storage) text::FtTextShaper;
    REQUIRE(static_cast<void*>(second) == static_cast<void*>(storage));
    REQUIRE(second->addFontFile(glyphFontPath()));
    CHECK(second->generation() != firstGeneration);
    CommandList again = glyphRunList(*second, "HIl", 120.0f, Vec2{10, 130}, Mat2x3{}, 400, 200);
    REQUIRE(again.glyphRuns[0].font == cl.glyphRuns[0].font);  // the same FontId ...
    REQUIRE(again.glyphs == cl.glyphs);                        // ... and glyph ids
    builder.build(again, second, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.hits == 0);  // nothing of the destroyed shaper is taken for this one's
    CHECK(s.misses == 3);
    builder.build(again, second, nullptr, nullptr, frame);
    CHECK(builder.lastCoverageStats().hits == 3);
    second->~FtTextShaper();
}

namespace {

/// `glyphs` (one DrawGlyphs run) drawn after a notched 1453 px square (about 2.1 Mpx covered, the
/// frame's first coverage shape, so always rasterized), which uses up the frame budget on its miss.
CommandList afterBudgetHog(CommandList glyphs) {
    const Command run = glyphs.commands[0];
    glyphs.commands.clear();
    addPolygons(glyphs, {{0, 0, 1453, 0, 1453, 1453, 800, 1453, 726.5f, 1300, 653, 1453, 0, 1453}},
                Mat2x3{1, 0, 0, 1, 10, 10}, FillRule::NonZero, solidPaint());
    glyphs.commands.push_back(run);
    return glyphs;
}

}  // namespace

// T-676 (Docs/tasks/T-676.md item 3): a rotated / skewed run <= 96 px is drawn as SW draws it, from
// its outlines through the coverage atlas (T-662's glyph key), not from resampled hinted bitmaps.
TEST_CASE("coverage text: T-676 rotated and skewed runs are outline coverage quads; static frames hit") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(glyphFontPath()));
    const Mat2x3 skew{1.0f, 0.0f, -0.105104f, 1.0f, 30.0f, 60.0f};  // skewX(-6deg)
    for (const Mat2x3& xf : {rotatedAt(-9, 30, 60), skew}) {
        text::GlyphAtlas atlas(shaper, 512);
        MirrorDevice dev;
        FrameBuilder builder;
        builder.init(dev);
        builder.resize(400, 200);
        builder.setGlyphAtlas(&atlas);
        GpuFrame frame;
        const CommandList cl = glyphRunList(shaper, "Hello", 24.0f, Vec2{0, 0}, xf, 400, 200);
        builder.build(cl, &shaper, nullptr, nullptr, frame);
        FrameBuilder::CoverageStats s = builder.lastCoverageStats();
        CHECK(s.misses + s.hits == 5);
        CHECK(s.misses >= 4);  // the two "l"s share a bitmap when their quarter-pixel phases match
        CHECK(s.fallbacks == 0);
        CHECK(countGlyphCmds(frame) == 0);
        CHECK(countWindingCmds(frame) == 0);
        CHECK(countCoverageQuads(frame) == 5);
        CHECK_FALSE(builder.wantsAnotherFrame());
        builder.build(cl, &shaper, nullptr, nullptr, frame);
        s = builder.lastCoverageStats();
        CHECK(s.hits == 5);
        CHECK(s.misses == 0);
        CHECK(s.rasterizedPixels == 0);
    }
}

// T-676: a glyph of such a run whose coverage quad the frame budget refuses is drawn by the T-653
// atlas path (never stencil-then-cover), and the refusal asks for another frame (T-662); the next
// frame draws it as a coverage quad.
TEST_CASE("coverage text: T-676 a rotated run over the budget falls back to the atlas, then completes") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(glyphFontPath()));
    text::GlyphAtlas atlas(shaper, 512);
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1600, 1600);
    builder.setGlyphAtlas(&atlas);
    GpuFrame frame;
    const CommandList cl =
        afterBudgetHog(glyphRunList(shaper, "Scale", 24.0f, Vec2{0, 0}, rotatedAt(12, 40, 1540), 1600, 1600));
    builder.build(cl, &shaper, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 5);  // the five glyphs of "Scale"
    CHECK(countWindingCmds(frame) == 0);
    CHECK(countGlyphCmds(frame) == 1);  // one batch of T-653 atlas quads
    CHECK(builder.wantsAnotherFrame());
    CHECK_FALSE(builder.lastFrameReusable(nullptr, nullptr));

    builder.build(cl, &shaper, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(countGlyphCmds(frame) == 0);
    CHECK(countWindingCmds(frame) == 0);
    CHECK_FALSE(builder.wantsAnotherFrame());
}

#endif  // XT_GPU_NO_TEXT

// ---------------------------------------------------------------------------------------------
// T-677 (Docs/tasks/T-677.md item 4): same input -> same output, whatever the builder drew before.
// ---------------------------------------------------------------------------------------------

namespace {

/// What `f`'s coverage quads put on the target, independent of where their bitmaps sit in the
/// atlas: per quad its device rect, colour and every texel it samples (read from the mirrored
/// page; a quad with u0 == u1 samples one texel, a full tile), then the number of stencil
/// fallbacks. Two frames with equal signatures draw the same pixels.
std::vector<int64_t> coverageSignature(const MirrorDevice& dev, const GpuFrame& f) {
    std::vector<int64_t> sig;
    for (const DrawCmd& c : f.cmds) {
        if (c.coverage == 0) {
            continue;
        }
        const auto it = dev.textures.find(c.coverage);
        REQUIRE(it != dev.textures.end());
        const MirrorDevice::Tex& page = it->second;
        REQUIRE(page.alive);
        for (uint32_t q = 0; q + 4 <= c.vertexCount; q += 4) {
            const Vertex& v0 = f.vertices[c.baseVertex + q];
            const Vertex& v2 = f.vertices[c.baseVertex + q + 2];
            const long x0 = std::lround(v0.x), y0 = std::lround(v0.y);
            const long x1 = std::lround(v2.x), y1 = std::lround(v2.y);
            sig.insert(sig.end(), {x0, y0, x1, y1, static_cast<int64_t>(v0.rgba)});
            const bool fullU = v0.u == v2.u;
            const bool fullV = v0.v == v2.v;
            const long tx = std::lround(v0.u * page.w - (fullU ? 0.5f : 0.0f));
            const long ty = std::lround(v0.v * page.h - (fullV ? 0.5f : 0.0f));
            const long tw = fullU ? 1 : x1 - x0;
            const long th = fullV ? 1 : y1 - y0;
            REQUIRE(tx >= 0);
            REQUIRE(ty >= 0);
            REQUIRE(tx + tw <= page.w);
            REQUIRE(ty + th <= page.h);
            for (long y = 0; y < th; ++y) {
                for (long x = 0; x < tw; ++x) {
                    sig.push_back(
                        page.r8[static_cast<size_t>(ty + y) * page.w + static_cast<size_t>(tx + x)]);
                }
            }
        }
    }
    sig.push_back(static_cast<int64_t>(countWindingCmds(f)));
    return sig;
}

/// The screen redrawn by the T-677 recycling test: concave shapes at several sizes, rotations and
/// quarter-pixel phases, and a 600 px L stored as tiles (edge, empty and full ones).
CommandList orderScreen() {
    CommandList cl = makeList(1024, 1024);
    addConcave(cl, 60, Mat2x3{1, 0, 0, 1, 10, 10});
    addConcave(cl, 90, rotatedAt(17, 160, 30));
    addConcave(cl, 45, Mat2x3{1, 0, 0, 1, 260.25f, 20.75f});
    addConcave(cl, 120, rotatedAt(-33, 420.5f, 60.25f));
    addConcave(cl, 600, Mat2x3{1, 0, 0, 1, 20.5f, 300.25f});
    return cl;
}

}  // namespace

TEST_CASE("coverage order: a screen redrawn after the atlas grew and every page was recycled is identical") {
    MirrorDevice dev;
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1024, 1024);
    GpuFrame frame;
    const CommandList screen = orderScreen();
    // Fresh builder: page 0 starts at 512 and must grow for the L's 258 px tile boxes (the frame is
    // built a second time then).
    builder.build(screen, nullptr, nullptr, nullptr, frame);
    FrameBuilder::CoverageStats s = builder.lastCoverageStats();
    REQUIRE(s.fallbacks == 0);
    REQUIRE(s.tiles > 0);
    REQUIRE(s.uniformTiles > 0);
    CHECK(s.atlasBytes >= 1024u * 1024u);  // page 0 grew
    const std::vector<int64_t> first = coverageSignature(dev, frame);

    // One 511..512 px shape per frame: every such box takes a 1024 page of its own, so six of them
    // fill the four pages and recycle each, the screen's pages included (least recently used first).
    for (int k = 0; k < 6; ++k) {
        INFO("filler ", k);
        CommandList filler = makeList(1024, 1024);
        addConcave(filler, 508.5f + 0.1f * static_cast<float>(k), Mat2x3{1, 0, 0, 1, 300.25f, 10.5f});
        builder.build(filler, nullptr, nullptr, nullptr, frame);
        CHECK(builder.lastCoverageStats().fallbacks == 0);
    }

    builder.build(screen, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.fallbacks == 0);
    CHECK(s.rasterizedPixels > 0);  // its bitmaps were recycled and are rasterized again
    CHECK(coverageSignature(dev, frame) == first);

    builder.build(screen, nullptr, nullptr, nullptr, frame);
    s = builder.lastCoverageStats();
    CHECK(s.rasterizedPixels == 0);  // all cached now
    CHECK(coverageSignature(dev, frame) == first);
}

#ifndef XT_GPU_NO_TEXT

namespace {

/// Checks the texel ring around every glyph quad of `f` (the texels a linear sample at the quad's
/// edge reads besides the glyph's own: the 1 px margin the atlas keeps empty) is 0 on the mirrored
/// device page.
void checkGlyphMarginsZero(const MirrorDevice& dev, const GpuFrame& f) {
    size_t quads = 0;
    size_t nonZero = 0;
    for (const DrawCmd& c : f.cmds) {
        if (c.shading != Shading::Glyph || c.texture == 0) {
            continue;
        }
        const auto it = dev.textures.find(c.texture);
        REQUIRE(it != dev.textures.end());
        const MirrorDevice::Tex& page = it->second;
        REQUIRE(page.alive);
        REQUIRE(page.fmt == TextureFormat::R8);
        for (uint32_t q = 0; q + 4 <= c.vertexCount; q += 4) {
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (uint32_t k = 0; k < 4; ++k) {
                const Vertex& v = f.vertices[c.baseVertex + q + k];
                u0 = std::min(u0, v.u);
                u1 = std::max(u1, v.u);
                v0 = std::min(v0, v.v);
                v1 = std::max(v1, v.v);
            }
            // Bilinear taps over [u0, u1] x [v0, v1]: texels lo..hi; the outermost ones are the ring.
            const long lx = static_cast<long>(std::floor(u0 * page.w - 0.5f));
            const long hx = static_cast<long>(std::ceil(u1 * page.w - 0.5f));
            const long ly = static_cast<long>(std::floor(v0 * page.h - 0.5f));
            const long hy = static_cast<long>(std::ceil(v1 * page.h - 0.5f));
            ++quads;
            for (long y = ly; y <= hy; ++y) {
                for (long x = lx; x <= hx; ++x) {
                    const bool ring = x == lx || x == hx || y == ly || y == hy;
                    if (!ring || x < 0 || y < 0 || x >= page.w || y >= page.h) {
                        continue;
                    }
                    nonZero += page.r8[static_cast<size_t>(y) * page.w + static_cast<size_t>(x)] != 0 ? 1 : 0;
                }
            }
        }
    }
    CHECK(quads > 0);
    CHECK(nonZero == 0);
}

}  // namespace

// T-677: rotated atlas glyphs (T-653) grow their quads half a texel onto the glyph's margin. The
// device page there must be 0 -- not the garbage of a page created without data (GL), nor the ink of
// glyphs another atlas (a new document's, a bundle reload's) or a pre-reset packing left on a page
// the builder keeps.
TEST_CASE("glyph atlas: rotated atlas glyphs sample an empty margin, also after the atlas is replaced") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(glyphFontPath()));
    MirrorDevice dev;  // pages start as 0xCD, like a GL texture made without data
    FrameBuilder builder;
    builder.init(dev);
    builder.resize(1600, 1600);
    GpuFrame frame;
    // T-676: a rotated run takes the T-653 atlas path only when its coverage quads are refused: each
    // list starts with a shape that misses and uses up the frame budget.
    const Mat2x3 rot = rotatedAt(12, 30, 1540);
    {
        text::GlyphAtlas first(shaper, 256);
        builder.setGlyphAtlas(&first);
        const CommandList ink =
            afterBudgetHog(glyphRunList(shaper, "WMWMWMWMW", 24.0f, Vec2{0, 24}, rot, 1600, 1600));
        builder.build(ink, &shaper, nullptr, nullptr, frame);
        REQUIRE(countGlyphCmds(frame) >= 1);
        checkGlyphMarginsZero(dev, frame);
        builder.setGlyphAtlas(nullptr);
    }
    // A second atlas of the same size: the builder keeps its device pages, which hold the first
    // atlas's glyphs where the second one packs its margins.
    text::GlyphAtlas second(shaper, 256);
    builder.setGlyphAtlas(&second);
    // The square's bitmap is cached now: a slightly different square misses and hogs the budget again.
    CommandList thin = afterBudgetHog(glyphRunList(shaper, "il.lil.il", 24.0f, Vec2{0, 24}, rot, 1600, 1600));
    thin.fills[0].xf.e += 0.5f;
    builder.build(thin, &shaper, nullptr, nullptr, frame);
    REQUIRE(countGlyphCmds(frame) >= 1);
    checkGlyphMarginsZero(dev, frame);
    builder.setGlyphAtlas(nullptr);
}

#endif  // XT_GPU_NO_TEXT
