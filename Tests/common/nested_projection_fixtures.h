// T-761 (Docs/design/19-3d-space.md §19.y.1): CommandLists with nested projections, shared by the
// GL (Tests/gl/test_gl_3d.cpp) and wgpu (Tests/wgpu/test_wgpu_nested_projection.cpp) cross-checks
// against xt::sw. The GPU vertices must carry the whole chain's homogeneous w, so an image whose
// perspective sits in an inner level is sampled perspective-correctly even when the outer level is
// flat (w == 1 on its plane). Needs Code/Runtime/core/src on the include path (camera.h).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include "camera.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/types.h"

namespace xt::test::nested {

inline constexpr int kW = 128, kH = 128;

/// Asset 0: a 32x32 checkerboard of single texels (high frequency), opaque.
inline ImageData fineChecker() {
    ImageData img;
    img.width = 32;
    img.height = 32;
    img.rgba.resize(32 * 32 * 4);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const bool light = ((x + y) & 1) == 0;
            const size_t i = (static_cast<size_t>(y) * 32 + static_cast<size_t>(x)) * 4;
            img.rgba[i + 0] = light ? 255 : 20;
            img.rgba[i + 1] = light ? 230 : 40;
            img.rgba[i + 2] = light ? 60 : 200;
            img.rgba[i + 3] = 255;
        }
    }
    return img;
}

/// Serves fineChecker() as asset 0.
class CheckerResolver : public AssetResolver {
public:
    CheckerResolver() : m_image(fineChecker()) {}
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId asset, const IrView&, ImageData& out) override {
        if (asset != 0) {
            return false;
        }
        out = m_image;
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override {
        return Vec2{static_cast<float>(m_image.width), static_cast<float>(m_image.height)};
    }
    const ImageData* imageView(AssetId asset, const IrView&) override {
        return asset == 0 ? &m_image : nullptr;
    }

private:
    ImageData m_image;
};

/// Mean absolute difference over every RGBA channel of two equally sized buffers (0..255 units).
inline double meanChannelDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double sum = 0.0;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return n == 0 ? 0.0 : sum / static_cast<double>(n);
}

/// Pixels whose channels differ by more than `tolerance` although `reference` is opaque over their
/// whole 3x3 neighbourhood: differences away from the shape's edges (the edges keep the projected
/// GPU path's hard coverage against xt::sw's analytic one, as for a single projection level).
inline int interiorDiffs(const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& reference,
                         int tolerance) {
    auto at = [](const std::vector<uint8_t>& px, int x, int y) {
        return px.data() + (static_cast<size_t>(y) * static_cast<size_t>(kW) + static_cast<size_t>(x)) * 4;
    };
    int count = 0;
    for (int y = 1; y + 1 < kH; ++y) {
        for (int x = 1; x + 1 < kW; ++x) {
            bool interior = true;
            for (int dy = -1; dy <= 1 && interior; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    interior = interior && at(reference, x + dx, y + dy)[3] == 255;
                }
            }
            if (!interior) {
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                if (std::abs(static_cast<int>(at(gpu, x, y)[c]) - static_cast<int>(at(reference, x, y)[c])) >
                    tolerance) {
                    ++count;
                    break;
                }
            }
        }
    }
    return count;
}

/// Builds the lists; every command goes through these.
struct Builder {
    CommandList cl;
    Builder() {
        cl.viewport = Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)};
        cl.devicePixelRatio = 1;
    }
    void push(Command c) { cl.commands.push_back(c); }
    void pushProjection(float perspective, Vec2 origin, Mat2x3 rootXf) {
        ProjectionCmd c{};
        c.viewProj = cssPerspectiveViewProj(perspective, origin).viewProj;
        c.rootXf = rootXf;
        c.boxRect = Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)};
        cl.projections.push_back(c);
        push(Command{CmdKind::PushProjection, {}, static_cast<uint32_t>(cl.projections.size() - 1)});
    }
    void popProjection() { push(Command{CmdKind::PopProjection, {}, 0}); }
    void pushTransform3D(const Mat4& world3) {
        cl.transforms3.push_back(Transform3DCmd{world3});
        push(Command{CmdKind::PushTransform3D, {}, static_cast<uint32_t>(cl.transforms3.size() - 1)});
    }
    void popTransform3D() { push(Command{CmdKind::PopTransform3D, {}, 0}); }
    void image(Rect dst) {
        ImageCmd c{};
        c.xf = Mat2x3{};
        c.asset = 0;
        c.dst = dst;
        c.srcUv = Rect{0, 0, 1, 1};
        c.opacity = 1;
        c.smooth = 1;
        cl.images.push_back(c);
        push(Command{CmdKind::DrawImage, {}, static_cast<uint32_t>(cl.images.size() - 1)});
    }
    void imageQuad(const Vec2 (&p)[4]) {
        ImageQuadCmd c{};
        c.asset = 0;
        for (int i = 0; i < 4; ++i) {
            c.p[i] = p[i];
        }
        c.srcUv = Rect{0, 0, 1, 1};
        c.opacity = 1;
        c.smooth = 1;
        cl.imageQuads.push_back(c);
        push(Command{CmdKind::DrawImageQuad, {}, static_cast<uint32_t>(cl.imageQuads.size() - 1)});
    }
    void gradientRect(Rect r, Color a, Color b) {
        PathRef ref{};
        ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
        ref.pointOffset = static_cast<uint32_t>(cl.points.size());
        const float pts[8] = {r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h};
        cl.verbs.insert(cl.verbs.end(),
                        {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close});
        cl.points.insert(cl.points.end(), pts, pts + 8);
        ref.verbCount = 5;
        ref.pointCount = 8;
        FillCmd f{};
        f.xf = Mat2x3{};
        f.path = ref;
        f.paint.kind = PaintKind::Linear;
        f.paint.color = Color{1, 1, 1, 1};
        f.paint.p0 = Vec2{r.x, r.y};
        f.paint.p1 = Vec2{r.x + r.w, r.y + r.h};
        f.paint.stopOffset = static_cast<uint32_t>(cl.stops.size());
        cl.stops.push_back(GradientStop{0.0f, a});
        cl.stops.push_back(GradientStop{1.0f, b});
        f.paint.stopCount = 2;
        f.rule = FillRule::NonZero;
        f.antialias = 1;
        cl.fills.push_back(f);
        push(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
    }
};

/// A flat outer perspective root (its plane wrapped in PushTransform3D{I}, as the Painter does:
/// w == 1 there) around an inner root whose layer is rotateX(55deg) under perspective 150 -- all
/// the perspective is in the inner level.
inline CommandList flatOuterTiltedInner() {
    Builder b;
    b.pushProjection(400.0f, Vec2{64, 64}, Mat2x3{});
    b.pushTransform3D(mat4Identity());
    b.pushProjection(150.0f, Vec2{0, 0}, translation(Vec2{64, 64}));
    b.pushTransform3D(mat4RotateX(55.0f));
    b.image(Rect{-48, -48, 96, 96});
    b.popTransform3D();
    b.popProjection();
    b.popTransform3D();
    b.popProjection();
    return std::move(b.cl);
}

/// Both levels carry perspective: outer rotateY(25deg) under perspective 300, inner rotateX(50deg)
/// under perspective 150.
inline CommandList bothLevelsTilted() {
    Builder b;
    b.pushProjection(300.0f, Vec2{64, 64}, Mat2x3{});
    b.pushTransform3D(mat4RotateY(25.0f));
    b.pushProjection(150.0f, Vec2{0, 0}, translation(Vec2{64, 64}));
    b.pushTransform3D(mat4RotateX(50.0f));
    b.image(Rect{-44, -44, 88, 88});
    b.popTransform3D();
    b.popProjection();
    b.popTransform3D();
    b.popProjection();
    return std::move(b.cl);
}

/// Three levels: a flat outer root, a middle root tilted rotateY(-20deg) under perspective 400 and
/// an inner root tilted rotateX(45deg) under perspective 200, drawing a DrawImageQuad.
inline CommandList threeLevels() {
    Builder b;
    b.pushProjection(500.0f, Vec2{64, 64}, Mat2x3{});
    b.pushProjection(400.0f, Vec2{0, 0}, translation(Vec2{64, 64}));
    b.pushTransform3D(mat4RotateY(-20.0f));
    b.pushProjection(200.0f, Vec2{0, 0}, translation(Vec2{4, -2}));
    b.pushTransform3D(mat4RotateX(45.0f));
    const Vec2 quad[4] = {Vec2{-46, -46}, Vec2{46, -46}, Vec2{46, 46}, Vec2{-46, 46}};
    b.imageQuad(quad);
    b.popTransform3D();
    b.popProjection();
    b.popTransform3D();
    b.popProjection();
    b.popProjection();
    return std::move(b.cl);
}

/// A linear gradient fill (a path: points projected on the CPU, paint through a homography fitted
/// to the projected box) in the flat-outer nesting -- already perspective-correct before T-761.
inline CommandList nestedGradientFill() {
    Builder b;
    b.pushProjection(400.0f, Vec2{64, 64}, Mat2x3{});
    b.pushProjection(150.0f, Vec2{0, 0}, translation(Vec2{64, 64}));
    b.pushTransform3D(mat4RotateX(55.0f));
    b.gradientRect(Rect{-48, -48, 96, 96}, Color{1, 0, 0, 1}, Color{0, 0, 1, 1});
    b.popTransform3D();
    b.popProjection();
    b.popProjection();
    return std::move(b.cl);
}

/// A single projection level (the inner level of flatOuterTiltedInner alone): the pre-T-761 path,
/// whose vertices must stay byte-identical.
inline CommandList singleLevelTilt() {
    Builder b;
    b.pushProjection(150.0f, Vec2{0, 0}, translation(Vec2{64, 64}));
    b.pushTransform3D(mat4RotateX(55.0f));
    b.image(Rect{-48, -48, 96, 96});
    b.popTransform3D();
    b.popProjection();
    return std::move(b.cl);
}

}  // namespace xt::test::nested
