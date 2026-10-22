#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "clip_shape.h"
#include "rounded_rect.h"
#include "xt/command_list.h"
#include "xt/geom.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/num_guard.h"
#include "xt/particles.h"
#include "xt/property.h"

/// emitElementBackground / emitElementBorder / emitElementImage / emitElementOutline (T-325): the
/// per-Element visual emitters Painter::paintElement calls (painter.cpp). See Docs/tasks/T-27.md rules 2-4,
/// Docs/design/09-command-list.md §9.3's Element branch.
///
/// Not covered by Docs/tasks/T-27.md (conservative choices made here):
///   - border-radius Length percentages resolve against min(border-box width, border-box height)
///     (there is a single circular radius per corner in this model, not CSS's per-axis pair, so
///     there is no natural width-vs-height split to mirror CSS's own rule).
///   - The border ring's inner radius per corner subtracts the *average* of that corner's two
///     adjacent border widths (top/left for TL, etc.) rather than a single "corresponding" width,
///     since a corner touches two edges that may have different widths.
///   - An `img` with no cached intrinsic size (data.hasIntrinsic == 0, or a degenerate 0-sized
///     one) falls back to `object-fit: fill` behavior (stretch to the content box, full 0..1
///     srcUv) rather than leaving it undrawn.

namespace xt {

namespace {

/// border-image `repeat` / `round` (T-258a): a tile smaller than this (layout px) draws its
/// region stretched.
constexpr float kMinBorderImageTile = 1.0f;
/// border-image `repeat` / `round` (T-258a): a region needing more tiles than this along one axis
/// draws stretched.
constexpr uint32_t kMaxBorderImageTiles = 1024;
/// border-image `repeat` / `round` (T-258a): a region needing more tiles than this in all (both
/// axes multiplied) draws stretched.
constexpr uint32_t kMaxBorderImageRegionTiles = 65536;
/// T-327 (05 §5.9.8): `background-repeat` draws at most this many tiles (columns x rows); more
/// fall back to the stretched image and the Painter warns once.
constexpr uint32_t kMaxBackgroundTiles = 4096;

void appendPathToCommandList(const std::vector<PathVerb>& verbs, const std::vector<float>& points,
                             CommandList& out, PathRef& ref) {
    ref.verbOffset = static_cast<uint32_t>(out.verbs.size());
    out.verbs.insert(out.verbs.end(), verbs.begin(), verbs.end());
    ref.verbCount = static_cast<uint32_t>(verbs.size());
    ref.pointOffset = static_cast<uint32_t>(out.points.size());
    out.points.insert(out.points.end(), points.begin(), points.end());
    ref.pointCount = static_cast<uint32_t>(points.size());
}

/// Resolves the four corner-radius Lengths (percentages against min(w, h)) and clamps them per
/// the CSS overlap rule (Docs/tasks/T-27.md rule 2).
void resolveRadii(const ComputedStyle& style, float w, float h, float& tl, float& tr, float& br, float& bl) {
    float ref = std::min(w, h);
    tl = resolveLength(style.base[prop::BorderRadiusTL].len, ref, 0.0f);
    tr = resolveLength(style.base[prop::BorderRadiusTR].len, ref, 0.0f);
    br = resolveLength(style.base[prop::BorderRadiusBR].len, ref, 0.0f);
    bl = resolveLength(style.base[prop::BorderRadiusBL].len, ref, 0.0f);
    clampRoundedRectRadii(w, h, tl, tr, br, bl);
}

/// object-fit dst/srcUv computation for the `img` element (rule 4). `content` is the element's
/// content box (local coordinates); `assetW`/`assetH` is the image's natural size.
void computeImgFit(ObjectFit fit, Rect content, float assetW, float assetH, Rect& dst, Rect& srcUv) {
    srcUv = Rect{0, 0, 1, 1};
    if (fit == ObjectFit::Fill || assetW <= 0.0f || assetH <= 0.0f || content.w <= 0.0f ||
        content.h <= 0.0f) {
        dst = content;
        return;
    }
    float contentAspect = content.w / content.h;
    float assetAspect = assetW / assetH;
    if (fit == ObjectFit::Contain) {
        if (assetAspect > contentAspect) {
            float h = content.w / assetAspect;
            dst = Rect{content.x, content.y + (content.h - h) * 0.5f, content.w, h};
        } else {
            float w = content.h * assetAspect;
            dst = Rect{content.x + (content.w - w) * 0.5f, content.y, w, content.h};
        }
        return;
    }
    // ObjectFit::Cover: dst fills the content box exactly; crop the source via srcUv.
    dst = content;
    if (assetAspect > contentAspect) {
        float visibleFrac = contentAspect / assetAspect;
        float u0 = (1.0f - visibleFrac) * 0.5f;
        srcUv = Rect{u0, 0, visibleFrac, 1};
    } else {
        float visibleFrac = assetAspect / contentAspect;
        float v0 = (1.0f - visibleFrac) * 0.5f;
        srcUv = Rect{0, v0, 1, visibleFrac};
    }
}

/// Docs/design/05-style-and-css.md §5.5c (T-115): turns the element's resolved
/// `background-gradient` into a PaintRef in the box's local coordinates, appending its stops to
/// `out.stops`. `w`/`h` is the filled box's size and `alpha` the inherited opacity multiplier.
/// Returns false when there is nothing to paint (no gradient, or no stops).
///
/// Linear: the gradient line runs through the box center along CSS's angle convention (0deg points
/// towards the top, angles grow clockwise) and is |w sin a| + |h cos a| long, so p0/p1 are the
/// center plus/minus half that along the direction.
/// Radial (T-116): p0 is the center, p1 the point one horizontal radius (rx) to its right, and p2
/// the point one vertical radius (ry) below it -- an axis-aligned ellipse (see xt/command_list.h).
bool buildGradientPaint(const ElementGradient& gradient, float w, float h, float alpha, CommandList& out,
                        PaintRef& paint) {
    if (gradient.kind == GradientKind::None || gradient.stops.empty()) {
        return false;
    }
    Vec2 center{w * 0.5f, h * 0.5f};
    if (gradient.kind == GradientKind::Linear) {
        float rad = gradient.angle * (3.14159265358979323846f / 180.0f);
        float sinA = std::sin(rad);
        float cosA = std::cos(rad);
        Vec2 dir{sinA, -cosA};  // y grows downwards, so 0deg (towards the top) is (0, -1).
        float length = std::fabs(w * sinA) + std::fabs(h * cosA);
        paint.kind = PaintKind::Linear;
        paint.p0 = Vec2{center.x - dir.x * length * 0.5f, center.y - dir.y * length * 0.5f};
        paint.p1 = Vec2{center.x + dir.x * length * 0.5f, center.y + dir.y * length * 0.5f};
    } else if (gradient.kind == GradientKind::Conic) {
        // T-201: p0 is the centre, p1 one unit along the start angle's ray (0deg = up, clockwise);
        // t is the clockwise angle from that ray over a full turn.
        float rad = gradient.angle * (3.14159265358979323846f / 180.0f);
        float cx = resolveLength(gradient.cx, w, 0.0f);
        float cy = resolveLength(gradient.cy, h, 0.0f);
        paint.kind = PaintKind::Conic;
        paint.p0 = Vec2{cx, cy};
        paint.p1 = Vec2{cx + std::sin(rad), cy - std::cos(rad)};
        paint.p2 = paint.p0;
    } else {
        float rx = resolveLength(gradient.rx, w, 0.0f);
        float ry = resolveLength(gradient.ry, h, 0.0f);
        float cx = resolveLength(gradient.cx, w, 0.0f);
        float cy = resolveLength(gradient.cy, h, 0.0f);
        paint.kind = PaintKind::Radial;
        paint.p0 = Vec2{cx, cy};
        paint.p1 = Vec2{cx + rx, cy};
        paint.p2 = Vec2{cx, cy + ry};
    }
    paint.color = Color{1, 1, 1, alpha};  // 09.2: a gradient's paint color is an alpha multiplier.
    paint.stopOffset = static_cast<uint32_t>(out.stops.size());
    paint.stopCount = static_cast<uint32_t>(gradient.stops.size());
    out.stops.insert(out.stops.end(), gradient.stops.begin(), gradient.stops.end());
    return true;
}

/// T-327: the padding box's corner radii -- the border box's radii less the adjacent border
/// widths' average, as emitElementBorder's inner edge (rule 2) -- clamped to the padding box.
void paddingBoxRadii(const ElementData& data, float r[4]) {
    const LayoutBox& box = data.box;
    const Edges& bd = box.border;
    float tl, tr, br, bl;
    resolveRadii(data.style, box.width, box.height, tl, tr, br, bl);
    const Rect inner = box.paddingBox();
    r[0] = std::max(0.0f, tl - (bd.top + bd.left) * 0.5f);
    r[1] = std::max(0.0f, tr - (bd.top + bd.right) * 0.5f);
    r[2] = std::max(0.0f, br - (bd.bottom + bd.right) * 0.5f);
    r[3] = std::max(0.0f, bl - (bd.bottom + bd.left) * 0.5f);
    clampRoundedRectRadii(inner.w, inner.h, r[0], r[1], r[2], r[3]);
}

/// T-327: a `background-position` component in px from the padding box's edge: a percentage is
/// taken of (box - image) as CSS defines it (negative when the image is the larger), any other
/// length resolves as usual.
float backgroundOffset(const Length& len, float boxSize, float imageSize) {
    if (len.unit == LengthUnit::Percent) {
        return len.value * 0.01f * (boxSize - imageSize);
    }
    return resolveLength(len, std::max(0.0f, boxSize - imageSize), 0.0f);
}

/// T-327: the size of one background image tile from `background-size` (05 §5.9.8). `iw`/`ih`
/// is the image's own size (> 0) and `box` the padding box. `auto` on one axis keeps the image's
/// aspect ratio, on both it is the image's own size.
void backgroundTileSize(const ComputedStyle& style, Rect box, float iw, float ih, float& tw, float& th) {
    const Length& sx = style.base[prop::BackgroundSizeX].len;
    const Length& sy = style.base[prop::BackgroundSizeY].len;
    if (sx.unit == LengthUnit::Auto &&
        (sx.value == kBackgroundSizeCover || sx.value == kBackgroundSizeContain)) {
        const float fx = box.w / iw;
        const float fy = box.h / ih;
        const float s = sx.value == kBackgroundSizeCover ? std::max(fx, fy) : std::min(fx, fy);
        tw = iw * s;
        th = ih * s;
        return;
    }
    const bool autoX = sx.unit == LengthUnit::Auto;
    const bool autoY = sy.unit == LengthUnit::Auto;
    const float w = autoX ? 0.0f : resolveLength(sx, box.w, 0.0f);
    const float h = autoY ? 0.0f : resolveLength(sy, box.h, 0.0f);
    if (autoX && autoY) {
        tw = iw;
        th = ih;
    } else if (autoX) {
        th = h;
        tw = h * iw / ih;
    } else if (autoY) {
        tw = w;
        th = w * ih / iw;
    } else {
        tw = w;
        th = h;
    }
}

/// T-327: one axis of the background tiling: the first tile's start and how many tiles are
/// drawn. A repeated axis starts at or before the box's edge and covers the box; an unrepeated
/// one draws the single tile at `origin` (0 tiles when it misses the box).
void backgroundAxis(bool repeat, float origin, float tile, float boxStart, float boxSize, float& start,
                    double& count) {
    if (!repeat) {
        start = origin;
        count = (origin < boxStart + boxSize && origin + tile > boxStart) ? 1.0 : 0.0;
        return;
    }
    start = origin - static_cast<float>(std::ceil((origin - boxStart) / tile)) * tile;
    count = std::ceil((static_cast<double>(boxStart) + boxSize - start) / tile);
}

/// T-327: the background image as one ImageCmd covering `dst`, cropped to `clip` (the padding
/// box): the part of `dst` outside it is cut off through srcUv, never with a clip command.
void pushBackgroundTile(const ElementData& data, const Mat2x3& world, float alpha, Rect dst, Rect clip,
                        CommandList& out) {
    const float x0 = std::max(dst.x, clip.x);
    const float y0 = std::max(dst.y, clip.y);
    const float x1 = std::min(dst.x + dst.w, clip.x + clip.w);
    const float y1 = std::min(dst.y + dst.h, clip.y + clip.h);
    if (!(x1 > x0) || !(y1 > y0)) {
        return;
    }
    const Rect& sub = data.backgroundSubUv;  // T-199: the atlas cell.
    const float u0 = (x0 - dst.x) / dst.w;
    const float u1 = (x1 - dst.x) / dst.w;
    const float v0 = (y0 - dst.y) / dst.h;
    const float v1 = (y1 - dst.y) / dst.h;
    ImageCmd ic{};
    ic.xf = world;
    ic.asset = data.style.backgroundImage;
    ic.dst = Rect{x0, y0, x1 - x0, y1 - y0};
    ic.srcUv = Rect{sub.x + u0 * sub.w, sub.y + v0 * sub.h, (u1 - u0) * sub.w, (v1 - v0) * sub.h};
    ic.opacity = alpha;
    ic.smooth = data.style.imageRendering == 1 ? 0 : 1;
    ic.variantScale = data.style.uiScaleEff;  // T-345: the `@2x` / `@3x` choice.
    uint32_t idx = static_cast<uint32_t>(out.images.size());
    out.images.push_back(ic);
    out.commands.push_back(Command{CmdKind::DrawImage, {}, idx});
}

/// T-327 (05 §5.9.8): `background-image` with its size / position / repeat; true when the tile
/// cap forced the stretched fallback.
bool emitElementBackgroundImage(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                                std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints) {
    const ComputedStyle& style = data.style;
    if (style.backgroundImage == kInvalidAsset) {
        return false;
    }
    const Rect pb = data.box.paddingBox();
    const uint8_t mode = style.backgroundRepeat;
    bool capped = false;

    // Placement (05 §5.9.8): `stretch` keeps the pre-T-327 single quad over the padding box.
    float tw = pb.w, th = pb.h, ox = pb.x, oy = pb.y;
    float startX = pb.x, startY = pb.y;
    double nx = 1.0, ny = 1.0;
    bool stretch = mode == kBackgroundRepeatStretch;
    if (!stretch) {
        float iw = data.backgroundImageSize.x;
        float ih = data.backgroundImageSize.y;
        if (!(iw > 0.0f) || !(ih > 0.0f) || !std::isfinite(iw) || !std::isfinite(ih)) {
            iw = pb.w;  // No intrinsic size: CSS sizes such an image as the positioning area.
            ih = pb.h;
        }
        if (!(iw > 0.0f) || !(ih > 0.0f)) {
            return false;
        }
        backgroundTileSize(style, pb, iw, ih, tw, th);
        if (!std::isfinite(tw) || !std::isfinite(th) || !(tw > 0.0f) || !(th > 0.0f)) {
            return false;  // A zero-sized image draws nothing, as in CSS.
        }
        ox = pb.x + backgroundOffset(style.base[prop::BackgroundPositionX].len, pb.w, tw);
        oy = pb.y + backgroundOffset(style.base[prop::BackgroundPositionY].len, pb.h, th);
        if (!std::isfinite(ox) || !std::isfinite(oy)) {
            return false;
        }
        const bool repeatX = mode == kBackgroundRepeatBoth || mode == kBackgroundRepeatX;
        const bool repeatY = mode == kBackgroundRepeatBoth || mode == kBackgroundRepeatY;
        backgroundAxis(repeatX, ox, tw, pb.x, pb.w, startX, nx);
        backgroundAxis(repeatY, oy, th, pb.y, pb.h, startY, ny);
        if (!(nx * ny <= static_cast<double>(kMaxBackgroundTiles))) {
            // Too many quads: draw the pre-T-327 stretched image instead (the Painter warns once).
            capped = true;
            stretch = true;
            nx = ny = 1.0;
        }
    }
    if (nx < 1.0 || ny < 1.0) {
        return capped;  // An unrepeated tile entirely outside the box.
    }

    // A rounded padding box clips the whole image set with one PushClipPath; square corners need
    // no clip command (the tiles are cropped to the box through srcUv).
    float r[4];
    paddingBoxRadii(data, r);
    const bool rounded = r[0] > 0.0f || r[1] > 0.0f || r[2] > 0.0f || r[3] > 0.0f;
    if (rounded) {
        scratchVerbs.clear();
        scratchPoints.clear();
        appendRoundedRectPath(pb, r[0], r[1], r[2], r[3], scratchVerbs, scratchPoints);
        ClipPathCmd cp{};
        cp.xf = world;
        appendPathToCommandList(scratchVerbs, scratchPoints, out, cp.path);
        cp.rule = FillRule::NonZero;
        uint32_t idx = static_cast<uint32_t>(out.clipPaths.size());
        out.clipPaths.push_back(cp);
        out.commands.push_back(Command{CmdKind::PushClipPath, {}, idx});
    }

    if (stretch) {
        ImageCmd ic{};
        ic.xf = world;
        ic.asset = style.backgroundImage;
        ic.dst = pb;
        ic.srcUv = data.backgroundSubUv;  // T-199: the atlas cell when the asset is atlased.
        ic.opacity = alpha;
        ic.smooth = style.imageRendering == 1 ? 0 : 1;
        ic.variantScale = style.uiScaleEff;  // T-345
        uint32_t idx = static_cast<uint32_t>(out.images.size());
        out.images.push_back(ic);
        out.commands.push_back(Command{CmdKind::DrawImage, {}, idx});
    } else {
        const auto cols = static_cast<uint32_t>(nx);
        const auto rows = static_cast<uint32_t>(ny);
        for (uint32_t j = 0; j < rows; ++j) {
            for (uint32_t i = 0; i < cols; ++i) {
                const Rect dst{startX + static_cast<float>(i) * tw, startY + static_cast<float>(j) * th, tw,
                               th};
                pushBackgroundTile(data, world, alpha, dst, pb, out);
            }
        }
    }

    if (rounded) {
        out.commands.push_back(Command{CmdKind::PopClip, {}, 0});
    }
    return capped;
}

}  // namespace

bool pushElementClipShape(const ElementData& data, const Mat2x3& world, CommandList& out,
                          std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints) {
    if (data.clipShape.kind == ClipShapeKind::None) {
        return false;
    }
    scratchVerbs.clear();
    scratchPoints.clear();
    FillRule rule = FillRule::NonZero;
    const Rect borderBox{0, 0, data.box.width, data.box.height};
    if (!clip_shape::appendPath(data.clipShape, borderBox, scratchVerbs, scratchPoints, rule)) {
        // An empty shape (zero radius, a degenerate inset, ...) shows nothing: an empty rect clip.
        uint32_t idx = static_cast<uint32_t>(out.clipRects.size());
        out.clipRects.push_back(ClipRectCmd{world, Rect{0, 0, 0, 0}});
        out.commands.push_back(Command{CmdKind::PushClipRect, {}, idx});
        return true;
    }
    ClipPathCmd cp{};
    cp.xf = world;
    appendPathToCommandList(scratchVerbs, scratchPoints, out, cp.path);
    cp.rule = rule;
    uint32_t idx = static_cast<uint32_t>(out.clipPaths.size());
    out.clipPaths.push_back(cp);
    out.commands.push_back(Command{CmdKind::PushClipPath, {}, idx});
    return true;
}

bool emitElementBackground(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                           std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints) {
    const ComputedStyle& style = data.style;
    const LayoutBox& box = data.box;
    Color bg = style.base[prop::BackgroundColor].c;
    if (bg.a > 0.0f) {
        float tl, tr, br, bl;
        resolveRadii(style, box.width, box.height, tl, tr, br, bl);

        scratchVerbs.clear();
        scratchPoints.clear();
        appendRoundedRectPath(Rect{0, 0, box.width, box.height}, tl, tr, br, bl, scratchVerbs, scratchPoints);

        FillCmd fc{};
        fc.xf = world;
        appendPathToCommandList(scratchVerbs, scratchPoints, out, fc.path);
        fc.paint.kind = PaintKind::Solid;
        fc.paint.color = Color{bg.r, bg.g, bg.b, bg.a * alpha};
        fc.rule = FillRule::NonZero;
        fc.antialias = 1;
        uint32_t idx = static_cast<uint32_t>(out.fills.size());
        out.fills.push_back(fc);
        out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
    }

    // 05.5c (T-115): the gradient is painted over `background-color` and under
    // `background-image`, on the same rounded-rect path.
    {
        PaintRef paint{};
        if (buildGradientPaint(data.backgroundGradient, box.width, box.height, alpha, out, paint)) {
            float tl, tr, br, bl;
            resolveRadii(style, box.width, box.height, tl, tr, br, bl);

            scratchVerbs.clear();
            scratchPoints.clear();
            appendRoundedRectPath(Rect{0, 0, box.width, box.height}, tl, tr, br, bl, scratchVerbs,
                                  scratchPoints);

            FillCmd fc{};
            fc.xf = world;
            appendPathToCommandList(scratchVerbs, scratchPoints, out, fc.path);
            fc.paint = paint;
            fc.rule = FillRule::NonZero;
            fc.antialias = 1;
            uint32_t idx = static_cast<uint32_t>(out.fills.size());
            out.fills.push_back(fc);
            out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
        }
    }

    // T-327 (05 §5.9.8): `background-image` with its size / position / repeat; true when the
    // tile cap forced the stretched fallback.
    return emitElementBackgroundImage(data, world, alpha, out, scratchVerbs, scratchPoints);
}

namespace {

/// T-201: appendRoundedRectPath()'s outline traced the other way round (counter-clockwise: TL
/// corner, left edge, BL, bottom, BR, right edge, TR, top edge), so a ring of an outer and this
/// inner outline fills with the nonzero rule and a side of zero width -- where the two outlines
/// coincide -- cancels exactly instead of leaving an antialiasing seam (even-odd adds the two
/// coincident edges' coverage).
void appendRoundedRectPathReversed(Rect box, float rTL, float rTR, float rBR, float rBL,
                                   std::vector<PathVerb>& verbs, std::vector<float>& points) {
    const float x = box.x;
    const float y = box.y;
    const float w = box.w;
    const float h = box.h;
    const float k = kRoundedRectKappa;
    auto point = [&points](float px, float py) {
        points.push_back(px);
        points.push_back(py);
    };
    auto line = [&](float px, float py) {
        verbs.push_back(PathVerb::Line);
        point(px, py);
    };
    auto cubic = [&](float x1, float y1, float x2, float y2, float x3, float y3) {
        verbs.push_back(PathVerb::Cubic);
        point(x1, y1);
        point(x2, y2);
        point(x3, y3);
    };
    verbs.push_back(PathVerb::Move);
    point(x + rTL, y);
    if (rTL > 0.0f) {
        cubic(x + rTL - rTL * k, y, x, y + rTL - rTL * k, x, y + rTL);
    }
    line(x, y + h - rBL);
    if (rBL > 0.0f) {
        cubic(x, y + h - rBL + rBL * k, x + rBL - rBL * k, y + h, x + rBL, y + h);
    }
    line(x + w - rBR, y + h);
    if (rBR > 0.0f) {
        cubic(x + w - rBR + rBR * k, y + h, x + w, y + h - rBR + rBR * k, x + w, y + h - rBR);
    }
    line(x + w, y + rTR);
    if (rTR > 0.0f) {
        cubic(x + w, y + rTR - rTR * k, x + w - rTR + rTR * k, y, x + w - rTR, y);
    }
    line(x + rTL, y);
    verbs.push_back(PathVerb::Close);
}

}  // namespace

namespace {

/// T-333 (05 §5.9.8): side `side`'s color (0 top, 1 right, 2 bottom, 3 left): its own
/// base[BorderTopColor + side] when ComputedStyle::borderColorSides says so, else the legacy
/// single BorderColor.
Color borderSideColor(const ComputedStyle& style, int side) {
    if ((style.borderColorSides & (1u << side)) != 0) {
        return style.base[prop::BorderTopColor + side].c;
    }
    return style.base[prop::BorderColor].c;
}

float borderSideWidth(const Edges& bd, int side) {
    switch (side) {
        case 0:
            return bd.top;
        case 1:
            return bd.right;
        case 2:
            return bd.bottom;
        default:
            return bd.left;
    }
}

bool sameColorBits(const Color& a, const Color& b) { return std::memcmp(&a, &b, sizeof(Color)) == 0; }

/// T-333: true when the border is one solid color -- every side with a width is `solid` and all of
/// them share one color (`color`) -- the case drawn as the T-201 ring exactly as before T-333.
bool borderIsUniform(const ComputedStyle& style, const Edges& bd, Color& color) {
    bool first = true;
    color = style.base[prop::BorderColor].c;
    for (int side = 0; side < 4; ++side) {
        if (!(borderSideWidth(bd, side) > 0.0f)) {
            continue;
        }
        if (style.borderStyle[side] != kBorderStyleSolid) {
            return false;
        }
        const Color c = borderSideColor(style, side);
        if (first) {
            color = c;
            first = false;
        } else if (!sameColorBits(c, color)) {
            return false;
        }
    }
    return true;
}

/// T-333: true when side `side` draws something in the per-side mode.
bool borderSideDrawn(const ComputedStyle& style, const Edges& bd, int side) {
    return borderSideWidth(bd, side) > 0.0f && style.borderStyle[side] != kBorderStyleNone &&
           borderSideColor(style, side).a > 0.0f;
}

constexpr float kHalfPi = 1.57079632679f;
constexpr float kPi = 3.14159265359f;

/// The corner geometry of the per-side border (T-333). Corner c is 0 TL, 1 TR, 2 BR, 3 BL; its
/// arc runs clockwise (y down) from angle startAngle(c) on side c-1 to startAngle(c) + pi/2 on
/// side c, with its 45-degree midpoint at startAngle(c) + pi/4. Side s runs from corner s to
/// corner (s + 1) % 4.
struct BorderCorners {
    Vec2 outerC[4];  ///< Outer (border-box) arc centers.
    float outerR[4];
    Vec2 innerC[4];  ///< Inner (padding-box) arc centers.
    float innerR[4];
    Vec2 midC[4];  ///< Centerline arc centers (the dash / dot path, halfway through the ring).
    float midR[4];
};

float cornerStartAngle(int corner) { return kPi + static_cast<float>(corner) * kHalfPi; }

Vec2 arcPoint(Vec2 c, float r, float angle) {
    return Vec2{c.x + r * std::cos(angle), c.y + r * std::sin(angle)};
}

void buildBorderCorners(const ElementData& data, BorderCorners& k) {
    const LayoutBox& box = data.box;
    const Edges& bd = box.border;
    const float w = box.width;
    const float h = box.height;
    float tl, tr, br, bl;
    resolveRadii(data.style, w, h, tl, tr, br, bl);
    // Inner radii: the same rule (and clamp) as the T-201 ring of emitElementBorder.
    const Rect inner = box.paddingBox();
    float itl = std::max(0.0f, tl - (bd.top + bd.left) * 0.5f);
    float itr = std::max(0.0f, tr - (bd.top + bd.right) * 0.5f);
    float ibr = std::max(0.0f, br - (bd.bottom + bd.right) * 0.5f);
    float ibl = std::max(0.0f, bl - (bd.bottom + bd.left) * 0.5f);
    clampRoundedRectRadii(inner.w, inner.h, itl, itr, ibr, ibl);
    // The centerline radius sits halfway between the outer and the T-201 inner radius.
    const float mtl = std::max(0.0f, tl - (bd.top + bd.left) * 0.25f);
    const float mtr = std::max(0.0f, tr - (bd.top + bd.right) * 0.25f);
    const float mbr = std::max(0.0f, br - (bd.bottom + bd.right) * 0.25f);
    const float mbl = std::max(0.0f, bl - (bd.bottom + bd.left) * 0.25f);
    const float mx0 = bd.left * 0.5f;
    const float my0 = bd.top * 0.5f;
    const float mx1 = w - bd.right * 0.5f;
    const float my1 = h - bd.bottom * 0.5f;

    k.outerC[0] = Vec2{tl, tl};
    k.outerC[1] = Vec2{w - tr, tr};
    k.outerC[2] = Vec2{w - br, h - br};
    k.outerC[3] = Vec2{bl, h - bl};
    k.outerR[0] = tl;
    k.outerR[1] = tr;
    k.outerR[2] = br;
    k.outerR[3] = bl;
    k.innerC[0] = Vec2{inner.x + itl, inner.y + itl};
    k.innerC[1] = Vec2{inner.x + inner.w - itr, inner.y + itr};
    k.innerC[2] = Vec2{inner.x + inner.w - ibr, inner.y + inner.h - ibr};
    k.innerC[3] = Vec2{inner.x + ibl, inner.y + inner.h - ibl};
    k.innerR[0] = itl;
    k.innerR[1] = itr;
    k.innerR[2] = ibr;
    k.innerR[3] = ibl;
    k.midC[0] = Vec2{mx0 + mtl, my0 + mtl};
    k.midC[1] = Vec2{mx1 - mtr, my0 + mtr};
    k.midC[2] = Vec2{mx1 - mbr, my1 - mbr};
    k.midC[3] = Vec2{mx0 + mbl, my1 - mbl};
    k.midR[0] = mtl;
    k.midR[1] = mtr;
    k.midR[2] = mbr;
    k.midR[3] = mbl;
}

/// Appends the circular arc (center `c`, radius `r`) from `a0` to `a1` (at most pi/2 apart) as one
/// cubic, starting from the current point (which must be the arc's start). Nothing for r <= 0.
void appendArcCubic(Vec2 c, float r, float a0, float a1, std::vector<PathVerb>& verbs,
                    std::vector<float>& points) {
    if (!(r > 0.0f)) {
        return;
    }
    const float kk = (4.0f / 3.0f) * std::tan((a1 - a0) * 0.25f) * r;
    const Vec2 p0 = arcPoint(c, r, a0);
    const Vec2 p3 = arcPoint(c, r, a1);
    const Vec2 p1{p0.x - kk * std::sin(a0), p0.y + kk * std::cos(a0)};
    const Vec2 p2{p3.x + kk * std::sin(a1), p3.y - kk * std::cos(a1)};
    verbs.push_back(PathVerb::Cubic);
    points.insert(points.end(), {p1.x, p1.y, p2.x, p2.y, p3.x, p3.y});
}

void appendPoint(PathVerb verb, Vec2 p, std::vector<PathVerb>& verbs, std::vector<float>& points) {
    verbs.push_back(verb);
    points.push_back(p.x);
    points.push_back(p.y);
}

/// T-333: side `side`'s trapezoid -- the ring between the border box and the padding box, cut at
/// both ends along the corner split (the line from the outer to the inner corner, i.e. the 45-degree
/// diagonal of a square corner, or through both arcs' midpoints of a rounded one), clockwise.
void appendBorderTrapezoid(const BorderCorners& k, int side, std::vector<PathVerb>& verbs,
                           std::vector<float>& points) {
    const int s = side;
    const int e = (side + 1) % 4;
    const float sMid = cornerStartAngle(s) + kHalfPi * 0.5f;
    const float sEnd = cornerStartAngle(s) + kHalfPi;
    const float eStart = cornerStartAngle(e);
    const float eMid = eStart + kHalfPi * 0.5f;
    appendPoint(PathVerb::Move, arcPoint(k.outerC[s], k.outerR[s], sMid), verbs, points);
    appendArcCubic(k.outerC[s], k.outerR[s], sMid, sEnd, verbs, points);
    appendPoint(PathVerb::Line, arcPoint(k.outerC[e], k.outerR[e], eStart), verbs, points);
    appendArcCubic(k.outerC[e], k.outerR[e], eStart, eMid, verbs, points);
    appendPoint(PathVerb::Line, arcPoint(k.innerC[e], k.innerR[e], eMid), verbs, points);
    appendArcCubic(k.innerC[e], k.innerR[e], eMid, eStart, verbs, points);
    appendPoint(PathVerb::Line, arcPoint(k.innerC[s], k.innerR[s], sEnd), verbs, points);
    appendArcCubic(k.innerC[s], k.innerR[s], sEnd, sMid, verbs, points);
    verbs.push_back(PathVerb::Close);
}

/// One end of a side's region: the corner split line through `p` along `dir`; the side keeps the
/// points on the same side of it as its centerline's middle.
struct SplitPlane {
    Vec2 p;
    Vec2 dir;
    float sign;  ///< Sign of cross(dir, x - p) inside the side's region.
};

float splitCross(const SplitPlane& plane, Vec2 x) {
    return plane.dir.x * (x.y - plane.p.y) - plane.dir.y * (x.x - plane.p.x);
}

SplitPlane makeSplitPlane(const BorderCorners& k, int corner, Vec2 inside) {
    const float mid = cornerStartAngle(corner) + kHalfPi * 0.5f;
    SplitPlane plane{};
    plane.p = arcPoint(k.outerC[corner], k.outerR[corner], mid);
    const Vec2 q = arcPoint(k.innerC[corner], k.innerR[corner], mid);
    plane.dir = q - plane.p;
    if (!(std::fabs(plane.dir.x) + std::fabs(plane.dir.y) > 1e-6f)) {
        plane.dir = Vec2{-std::cos(mid), -std::sin(mid)};  // Degenerate: the 45-degree diagonal.
    }
    plane.sign = splitCross(plane, inside) >= 0.0f ? 1.0f : -1.0f;
    return plane;
}

/// Sutherland-Hodgman: clips the closed polygon `in` to the kept side of `plane` into `out`.
void clipToSplit(const std::vector<Vec2>& in, const SplitPlane& plane, std::vector<Vec2>& out) {
    out.clear();
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2 a = in[i];
        const Vec2 b = in[(i + 1) % n];
        const float da = splitCross(plane, a) * plane.sign;
        const float db = splitCross(plane, b) * plane.sign;
        if (da >= 0.0f) {
            out.push_back(a);
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            out.push_back(Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
    }
}

/// T-333: a `dashed` / `dotted` side: dashes (3 x width on, 2 x width off; CSS's convention) or
/// dots (round, one width across, one width apart) laid by applyDash (geom_dash.cpp) along the
/// side's centerline -- through the rounded corners' centerline arcs up to their midpoints --
/// turned into fills by strokeToFill, and cut at the corner splits like the solid trapezoid.
/// Appends Move/Line/Close contours to `verbs` / `points`.
void appendBorderDashes(const BorderCorners& k, int side, float width, bool dotted, float tolerance,
                        float extension, std::vector<PathVerb>& verbs, std::vector<float>& points) {
    const int s = side;
    const int e = (side + 1) % 4;
    const float sMid = cornerStartAngle(s) + kHalfPi * 0.5f;
    const float sEnd = cornerStartAngle(s) + kHalfPi;
    const float eStart = cornerStartAngle(e);
    const float eMid = eStart + kHalfPi * 0.5f;

    std::vector<PathVerb> lineVerbs;
    std::vector<float> linePoints;
    appendPoint(PathVerb::Move, arcPoint(k.midC[s], k.midR[s], sMid), lineVerbs, linePoints);
    appendArcCubic(k.midC[s], k.midR[s], sMid, sEnd, lineVerbs, linePoints);
    appendPoint(PathVerb::Line, arcPoint(k.midC[e], k.midR[e], eStart), lineVerbs, linePoints);
    appendArcCubic(k.midC[e], k.midR[e], eStart, eMid, lineVerbs, linePoints);

    std::vector<Vec2> flat;
    std::vector<uint32_t> flatStarts;
    std::vector<uint8_t> flatClosed;
    flattenPath(lineVerbs, linePoints, Mat2x3{}, tolerance, flat, flatStarts, flatClosed);
    if (flat.size() < 2) {
        return;
    }
    const Vec2 lineStart = flat.front();
    const Vec2 lineEnd = flat.back();
    const Vec2 inside =
        arcPoint(k.midC[e], k.midR[e], eStart) * 0.5f + arcPoint(k.midC[s], k.midR[s], sEnd) * 0.5f;

    // Dots: zero-length dashes with round caps, so every dot is one width across and the visible
    // gap between two is one width (a period of two widths).
    const float pattern[2] = {dotted ? 0.0f : 3.0f * width, dotted ? 2.0f * width : 2.0f * width};
    std::vector<Vec2> dashed;
    std::vector<uint32_t> dashedStarts;
    std::vector<uint8_t> dashedClosed;
    // applyDash starts a pattern whose first dash is empty in its gap, so the dot pattern is
    // shifted back by a hair: the first dot then sits on the corner split (half of it each side).
    const float phase = dotted ? -1e-3f * width : 0.0f;
    applyDash(flat, flatStarts, flatClosed, pattern, phase, dashed, dashedStarts, dashedClosed);

    // A butt-capped dash touching an end of the centerline is lengthened past it (its end point
    // moved out along its own first / last segment, so it stays one segment), so that the corner
    // split -- not the cap -- ends it: a square corner's dash then fills its half of the corner as
    // the solid trapezoid does.
    std::vector<uint8_t> dashOpen(dashedStarts.size(), 0);
    if (!dotted) {
        const auto samePoint = [](Vec2 p, Vec2 q) {
            return std::fabs(p.x - q.x) + std::fabs(p.y - q.y) < 1e-4f;
        };
        const auto lengthen = [extension](Vec2& end, Vec2 from) {
            const Vec2 t = end - from;
            const float len = std::sqrt(t.x * t.x + t.y * t.y);
            if (len > 1e-6f) {
                end = end + t * (extension / len);
            }
        };
        for (size_t ci = 0; ci < dashedStarts.size(); ++ci) {
            const uint32_t a = dashedStarts[ci];
            const uint32_t b =
                ci + 1 < dashedStarts.size() ? dashedStarts[ci + 1] : static_cast<uint32_t>(dashed.size());
            if (b - a < 2) {
                continue;
            }
            const Vec2 second = dashed[a + 1];
            const Vec2 secondLast = dashed[b - 2];
            if (samePoint(dashed[a], lineStart)) {
                lengthen(dashed[a], second);
            }
            if (samePoint(dashed[b - 1], lineEnd)) {
                lengthen(dashed[b - 1], secondLast);
            }
        }
    }

    std::vector<Vec2> fill;
    std::vector<uint32_t> fillStarts;
    strokeToFill(dashed, dashedStarts, dashOpen, width, dotted ? LineCap::Round : LineCap::Butt,
                 LineJoin::Round, 4.0f, tolerance, fill, fillStarts);

    const SplitPlane startPlane = makeSplitPlane(k, s, inside);
    const SplitPlane endPlane = makeSplitPlane(k, e, inside);
    std::vector<Vec2> poly;
    std::vector<Vec2> half;
    std::vector<Vec2> clipped;
    for (size_t ci = 0; ci < fillStarts.size(); ++ci) {
        const uint32_t a = fillStarts[ci];
        const uint32_t b =
            ci + 1 < fillStarts.size() ? fillStarts[ci + 1] : static_cast<uint32_t>(fill.size());
        poly.assign(fill.begin() + a, fill.begin() + b);
        clipToSplit(poly, startPlane, half);
        clipToSplit(half, endPlane, clipped);
        if (clipped.size() < 3) {
            continue;
        }
        appendPoint(PathVerb::Move, clipped[0], verbs, points);
        for (size_t i = 1; i < clipped.size(); ++i) {
            appendPoint(PathVerb::Line, clipped[i], verbs, points);
        }
        verbs.push_back(PathVerb::Close);
    }
}

void pushBorderFill(const Mat2x3& world, Color color, float alpha, const std::vector<PathVerb>& verbs,
                    const std::vector<float>& points, CommandList& out) {
    FillCmd fc{};
    fc.xf = world;
    appendPathToCommandList(verbs, points, out, fc.path);
    fc.paint.kind = PaintKind::Solid;
    fc.paint.color = Color{color.r, color.g, color.b, color.a * alpha};
    fc.rule = FillRule::NonZero;
    fc.antialias = 1;
    uint32_t idx = static_cast<uint32_t>(out.fills.size());
    out.fills.push_back(fc);
    out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
}

/// T-333: the per-side border: one FillPath per drawn side, top, right, bottom, left.
void emitElementBorderSides(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                            std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints) {
    const Edges& bd = data.box.border;
    BorderCorners k{};
    buildBorderCorners(data, k);
    // Flatten in local space finely enough for the world scale.
    const float scale = std::sqrt(std::fabs(world.a * world.d - world.b * world.c));
    const float tolerance = kFlattenTolerance / std::max(scale, 1e-3f);
    const float extension = std::max(std::max(bd.top, bd.right), std::max(bd.bottom, bd.left)) * 2.0f + 1.0f;
    for (int side = 0; side < 4; ++side) {
        if (!borderSideDrawn(data.style, bd, side)) {
            continue;
        }
        scratchVerbs.clear();
        scratchPoints.clear();
        const uint8_t style = data.style.borderStyle[side];
        if (style == kBorderStyleSolid) {
            appendBorderTrapezoid(k, side, scratchVerbs, scratchPoints);
        } else {
            appendBorderDashes(k, side, borderSideWidth(bd, side), style == kBorderStyleDotted, tolerance,
                               extension, scratchVerbs, scratchPoints);
        }
        if (scratchVerbs.empty()) {
            continue;
        }
        pushBorderFill(world, borderSideColor(data.style, side), alpha, scratchVerbs, scratchPoints, out);
    }
}

}  // namespace

uint32_t elementBorderFillCount(const ElementData& data) {
    const Edges& bd = data.box.border;
    const bool anyWidth = bd.top > 0.0f || bd.right > 0.0f || bd.bottom > 0.0f || bd.left > 0.0f;
    if (!anyWidth) {
        return 0;
    }
    Color color{};
    if (borderIsUniform(data.style, bd, color)) {
        return color.a > 0.0f ? 1u : 0u;
    }
    uint32_t count = 0;
    for (int side = 0; side < 4; ++side) {
        count += borderSideDrawn(data.style, bd, side) ? 1u : 0u;
    }
    return count;
}

void emitElementBorder(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                       std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints,
                       bool allowSnap) {
    const LayoutBox& box = data.box;
    const Edges& bd = box.border;
    bool anyWidth = bd.top > 0.0f || bd.right > 0.0f || bd.bottom > 0.0f || bd.left > 0.0f;
    if (!anyWidth) {
        return;
    }
    // T-333 (05 §5.9.8): sides of different colors or a non-solid style are drawn one by one; one
    // solid color stays the single T-201 ring below, unchanged.
    Color borderColor{};
    if (!borderIsUniform(data.style, bd, borderColor)) {
        emitElementBorderSides(data, world, alpha, out, scratchVerbs, scratchPoints);
        return;
    }
    if (borderColor.a <= 0.0f) {
        return;
    }

    float tl, tr, br, bl;
    resolveRadii(data.style, box.width, box.height, tl, tr, br, bl);

    // T-345 (25 §25.4.2 item 4): a ring of 1 px or thinner on an unrotated, unscaled element is
    // aligned to whole pixels -- its edges rounded to the nearest pixel -- so a hairline does not
    // smear over two pixel rows. Anything else is drawn where layout put it.
    Mat2x3 xf = world;
    Rect outer{0, 0, box.width, box.height};
    Rect inner = box.paddingBox();
    // Not while the element or an ancestor is being moved (it would step pixel by pixel).
    const bool thin = bd.top <= 1.0f && bd.right <= 1.0f && bd.bottom <= 1.0f && bd.left <= 1.0f && allowSnap;
    if (thin && world.a == 1.0f && world.b == 0.0f && world.c == 0.0f && world.d == 1.0f) {
        const float x0 = std::round(world.e);
        const float y0 = std::round(world.f);
        xf.e = x0;
        xf.f = y0;
        outer.w = std::round(world.e + box.width) - x0;
        outer.h = std::round(world.f + box.height) - y0;
        inner = Rect{bd.left, bd.top, std::max(0.0f, outer.w - bd.left - bd.right),
                     std::max(0.0f, outer.h - bd.top - bd.bottom)};
    }

    scratchVerbs.clear();
    scratchPoints.clear();
    appendRoundedRectPath(outer, tl, tr, br, bl, scratchVerbs, scratchPoints);

    float itl = std::max(0.0f, tl - (bd.top + bd.left) * 0.5f);
    float itr = std::max(0.0f, tr - (bd.top + bd.right) * 0.5f);
    float ibr = std::max(0.0f, br - (bd.bottom + bd.right) * 0.5f);
    float ibl = std::max(0.0f, bl - (bd.bottom + bd.left) * 0.5f);
    clampRoundedRectRadii(inner.w, inner.h, itl, itr, ibr, ibl);
    // T-201: the inner outline runs the other way and the ring fills nonzero, so a side without
    // border (where outer and inner coincide) leaves no antialiased seam.
    appendRoundedRectPathReversed(inner, itl, itr, ibr, ibl, scratchVerbs, scratchPoints);

    FillCmd fc{};
    fc.xf = xf;
    appendPathToCommandList(scratchVerbs, scratchPoints, out, fc.path);
    fc.paint.kind = PaintKind::Solid;
    fc.paint.color = Color{borderColor.r, borderColor.g, borderColor.b, borderColor.a * alpha};
    fc.rule = FillRule::NonZero;
    fc.antialias = 1;
    uint32_t idx = static_cast<uint32_t>(out.fills.size());
    out.fills.push_back(fc);
    out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
}

namespace {

/// T-325 (05 §5.9.8): the outline ring's width and offset in px, read from the cascaded style
/// (`em` was already resolved by the cascade; a percentage, which `outline-*` does not take,
/// resolves against 0). Returns false when no ring is drawn: `outline-style: none`, a width <= 0
/// or a non-finite value.
bool outlineMetrics(const ComputedStyle& style, float& width, float& offset) {
    if (style.outlineStyle == 0) {
        return false;
    }
    width = resolveLength(style.base[prop::OutlineWidth].len, 0.0f, 0.0f);
    offset = resolveLength(style.base[prop::OutlineOffset].len, 0.0f, 0.0f);
    return std::isfinite(width) && std::isfinite(offset) && width > 0.0f;
}

/// `box` grown by `amount` on every side (shrunk when negative), never below zero size: a side
/// pair that would cross collapses onto the box's center line.
Rect growRect(Rect box, float amount) {
    const float w = std::max(0.0f, box.w + 2.0f * amount);
    const float h = std::max(0.0f, box.h + 2.0f * amount);
    return Rect{box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h};
}

}  // namespace

float elementOutlineExtent(const ElementData& data) {
    float width = 0.0f;
    float offset = 0.0f;
    if (!outlineMetrics(data.style, width, offset)) {
        return 0.0f;
    }
    return std::max(0.0f, offset + width);
}

void emitElementOutline(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                        std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints) {
    float width = 0.0f;
    float offset = 0.0f;
    if (!outlineMetrics(data.style, width, offset)) {
        return;
    }
    const Color color = data.style.base[prop::OutlineColor].c;
    if (color.a <= 0.0f) {
        return;
    }
    const LayoutBox& box = data.box;
    const Rect borderBox{0, 0, box.width, box.height};
    const Rect outer = growRect(borderBox, offset + width);
    if (outer.w <= 0.0f || outer.h <= 0.0f) {
        return;
    }
    const Rect inner = growRect(borderBox, offset);

    // Inner edge: the border radius plus the offset, clamped at 0; a square corner (radius 0)
    // stays square, as CSS draws it. The outer edge keeps the ring's width round a rounded corner
    // and stays square where the inner corner is square.
    float r[4];
    resolveRadii(data.style, box.width, box.height, r[0], r[1], r[2], r[3]);
    float ri[4];
    float ro[4];
    for (int i = 0; i < 4; ++i) {
        ri[i] = r[i] > 0.0f ? std::max(0.0f, r[i] + offset) : 0.0f;
        ro[i] = ri[i] > 0.0f ? ri[i] + width : 0.0f;
    }
    clampRoundedRectRadii(outer.w, outer.h, ro[0], ro[1], ro[2], ro[3]);
    clampRoundedRectRadii(inner.w, inner.h, ri[0], ri[1], ri[2], ri[3]);

    scratchVerbs.clear();
    scratchPoints.clear();
    appendRoundedRectPath(outer, ro[0], ro[1], ro[2], ro[3], scratchVerbs, scratchPoints);
    if (inner.w > 0.0f && inner.h > 0.0f) {
        // T-201's ring: the inner outline runs the other way and the ring fills nonzero.
        appendRoundedRectPathReversed(inner, ri[0], ri[1], ri[2], ri[3], scratchVerbs, scratchPoints);
    }

    FillCmd fc{};
    fc.xf = world;
    appendPathToCommandList(scratchVerbs, scratchPoints, out, fc.path);
    fc.paint.kind = PaintKind::Solid;
    fc.paint.color = Color{color.r, color.g, color.b, color.a * alpha};
    fc.rule = FillRule::NonZero;
    fc.antialias = 1;
    uint32_t idx = static_cast<uint32_t>(out.fills.size());
    out.fills.push_back(fc);
    out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
}

void emitElementImage(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out) {
    if (data.imageAsset == kInvalidAsset) {
        return;
    }
    Rect content = data.box.contentBox();
    float assetW = data.hasIntrinsic != 0 ? data.intrinsic.x : 0.0f;
    float assetH = data.hasIntrinsic != 0 ? data.intrinsic.y : 0.0f;
    Rect dst, srcUv;
    computeImgFit(data.style.objectFit, content, assetW, assetH, dst, srcUv);
    // T-191: an atlas sequence draws one cell of its image; object-fit works inside the cell.
    const Rect& sub = data.imageSubUv;
    srcUv = Rect{sub.x + srcUv.x * sub.w, sub.y + srcUv.y * sub.h, srcUv.w * sub.w, srcUv.h * sub.h};

    ImageCmd ic{};
    ic.xf = world;
    ic.asset = data.imageAsset;
    ic.dst = dst;
    ic.srcUv = srcUv;
    ic.opacity = data.style.base[prop::ImageOpacity].f * alpha;
    ic.smooth = data.style.imageRendering == 1 ? 0 : 1;  // T-327: `image-rendering: pixelated`.
    ic.variantScale = data.style.uiScaleEff;             // T-345
    uint32_t idx = static_cast<uint32_t>(out.images.size());
    out.images.push_back(ic);
    out.commands.push_back(Command{CmdKind::DrawImage, {}, idx});
}

/// T-202 (03 §3.12.5, 18 §18.12): the live particles as one DrawParticles command in the element's
/// coordinates (`world` is the element's). Each instance is the particle's center, its current
/// size as a square, its rotation in radians and its current color premultiplied with `alpha`
/// folded in; invisible ones (size or alpha <= 0) are left out, and no command is emitted when
/// nothing remains.
void emitParticles(const ParticleNodeData& particles, const Mat2x3& world, float alpha, CommandList& out) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const uint32_t offset = static_cast<uint32_t>(out.particleInstances.size());
    const Rect& sub = particles.textureSubUv;
    for (const Particle& p : particles.particles) {
        const float size = particleSize(p);
        const Color c = particleColor(particles, p);
        const float a = std::clamp(c.a, 0.0f, 1.0f) * alpha;
        if (!(size > 0.0f) || !(a > 0.0f)) {
            continue;
        }
        ParticleInstance inst{};
        inst.x = p.pos.x;
        inst.y = p.pos.y;
        inst.w = size;
        inst.h = size;
        inst.rot = p.rot * kDegToRad;
        inst.rgba = Color{c.r * a, c.g * a, c.b * a, a};
        inst.uv0 = Vec2{sub.x, sub.y};
        inst.uv1 = Vec2{sub.x + sub.w, sub.y + sub.h};
        out.particleInstances.push_back(inst);
    }
    const uint32_t count = static_cast<uint32_t>(out.particleInstances.size()) - offset;
    if (count == 0) {
        return;
    }
    ParticlesCmd pc{};
    pc.xf = world;
    pc.texture = particles.texture;
    pc.blend = particles.blend;
    pc.instanceOffset = offset;
    pc.count = count;
    const uint32_t idx = static_cast<uint32_t>(out.particles.size());
    out.particles.push_back(pc);
    out.commands.push_back(Command{CmdKind::DrawParticles, {}, idx});
}

bool particlesBounds(const ParticleNodeData& particles, Rect& out) {
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const Particle& p : particles.particles) {
        // A rotated square of side s fits in the square of its half-diagonal.
        const float r = 0.5f * std::abs(particleSize(p)) * 1.41421356f;
        if (!any) {
            x0 = p.pos.x - r, y0 = p.pos.y - r, x1 = p.pos.x + r, y1 = p.pos.y + r;
            any = true;
        } else {
            x0 = std::min(x0, p.pos.x - r);
            y0 = std::min(y0, p.pos.y - r);
            x1 = std::max(x1, p.pos.x + r);
            y1 = std::max(y1, p.pos.y + r);
        }
    }
    out = Rect{x0, y0, x1 - x0, y1 - y0};
    return any;
}

/// T-191 (25.3.5b): an unbound video / external texture drawn as a placeholder while the Document
/// has placeholders on -- the content box filled with a 16 px checkerboard, then the label the
/// Document shaped after layout (name and, for a video, duration). Returns false (nothing drawn)
/// when `ext` does not show a placeholder, so the caller draws the image as usual.
bool emitExternalPlaceholder(const ExternalNodeData& ext, const ElementData& data, const Mat2x3& world,
                             float alpha, CommandList& out, std::vector<PathVerb>& scratchVerbs,
                             std::vector<float>& scratchPoints) {
    if (ext.showPlaceholder == 0) {
        return false;
    }
    const Rect content = data.box.contentBox();
    if (content.w <= 0.0f || content.h <= 0.0f) {
        return true;
    }
    auto fillRects = [&](bool checker, Color color) {
        scratchVerbs.clear();
        scratchPoints.clear();
        constexpr float kCell = 16.0f;
        const int cols = static_cast<int>(std::ceil(content.w / kCell));
        const int rows = static_cast<int>(std::ceil(content.h / kCell));
        for (int r = 0; r < (checker ? rows : 1); ++r) {
            for (int c = 0; c < (checker ? cols : 1); ++c) {
                if (checker && ((r + c) & 1) == 0) {
                    continue;
                }
                const float x0 = checker ? content.x + static_cast<float>(c) * kCell : content.x;
                const float y0 = checker ? content.y + static_cast<float>(r) * kCell : content.y;
                const float x1 =
                    checker ? std::min(x0 + kCell, content.x + content.w) : content.x + content.w;
                const float y1 =
                    checker ? std::min(y0 + kCell, content.y + content.h) : content.y + content.h;
                scratchVerbs.push_back(PathVerb::Move);
                scratchVerbs.push_back(PathVerb::Line);
                scratchVerbs.push_back(PathVerb::Line);
                scratchVerbs.push_back(PathVerb::Line);
                scratchVerbs.push_back(PathVerb::Close);
                const float pts[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
                scratchPoints.insert(scratchPoints.end(), pts, pts + 8);
            }
        }
        if (scratchVerbs.empty()) {
            return;
        }
        FillCmd fc{};
        fc.xf = world;
        appendPathToCommandList(scratchVerbs, scratchPoints, out, fc.path);
        fc.paint.kind = PaintKind::Solid;
        fc.paint.color = Color{color.r, color.g, color.b, color.a * alpha};
        fc.rule = FillRule::NonZero;
        fc.antialias = 0;
        uint32_t idx = static_cast<uint32_t>(out.fills.size());
        out.fills.push_back(fc);
        out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
    };
    fillRects(false, Color{0.16f, 0.16f, 0.18f, 1.0f});
    fillRects(true, Color{0.24f, 0.24f, 0.27f, 1.0f});

    for (const GlyphRun& run : ext.label) {
        if (run.font == kInvalidFont || run.glyphs.empty()) {
            continue;
        }
        GlyphsCmd cmd{};
        cmd.xf = world;
        cmd.font = run.font;
        cmd.size = run.fontSize;
        cmd.color = Color{ext.labelColor.r, ext.labelColor.g, ext.labelColor.b, ext.labelColor.a * alpha};
        cmd.rendering = data.style.textRendering;
        cmd.glyphOffset = static_cast<uint32_t>(out.glyphs.size());
        for (size_t i = 0; i < run.glyphs.size() && i < run.positions.size(); ++i) {
            out.glyphs.push_back(run.glyphs[i]);
            out.glyphPos.push_back(run.positions[i]);
        }
        cmd.glyphCount = static_cast<uint32_t>(out.glyphs.size()) - cmd.glyphOffset;
        cmd.effect = GlyphEffect::None;
        uint32_t index = static_cast<uint32_t>(out.glyphRuns.size());
        out.glyphRuns.push_back(cmd);
        out.commands.push_back(Command{CmdKind::DrawGlyphs, {}, index});
    }
    return true;
}

/// T-199 (Docs/design/26-layers-symbols-library.md §26.3.4, 05 §5.9.2): the `border-image`
/// nine-slice. The source image (its atlas cell, `data.borderImageSubUv`) is cut at
/// `data.borderImageSlicePx` (L, R, T, B, source pixels) into corners, edges and a middle, and
/// drawn over the element's border box as textured quads: the four corners unscaled-proportion
/// (stretched to the border widths), the four edges stretched / repeated / rounded along their
/// axis (`border-image-repeat`), the middle only with `border-image-fill`. Every tile is one
/// DrawImage with a sub-rectangle `srcUv`, so both backends draw it through their existing image
/// path; `repeat` tiles are separate quads (partial tiles at the ends use a partial `srcUv`).
///
/// Not covered by the sheet (conservative choices made here):
///   - The destination widths are the element's border widths (CSS `border-image-width: 1`); an
///     edge whose border width is 0 uses its slice in pixels instead, so `border-image` works
///     without a `border-width` declaration.
///   - Slices that overlap (L + R > width) are scaled down proportionally, as CSS does; so are
///     destination widths that would not fit the box.
///   - Tile scale: edges keep the ratio of their border width to their slice (CSS); the middle
///     uses the top (else bottom) edge's horizontal scale and the left (else right) edge's
///     vertical scale, 1 when there is no such edge.
///   - A `repeat` run is centered in its area (CSS); `round` uses n = max(1, round(len / tile)).
void emitElementBorderImage(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out) {
    const ComputedStyle& style = data.style;
    if (style.borderImage == kInvalidAsset) {
        return;
    }
    const LayoutBox& box = data.box;
    const float w = box.width;
    const float h = box.height;
    const Vec2 src = data.borderImageSize;
    if (w <= 0.0f || h <= 0.0f || src.x <= 0.0f || src.y <= 0.0f) {
        return;
    }
    float sl = std::clamp(data.borderImageSlicePx[0], 0.0f, src.x);
    float sr = std::clamp(data.borderImageSlicePx[1], 0.0f, src.x);
    float st = std::clamp(data.borderImageSlicePx[2], 0.0f, src.y);
    float sb = std::clamp(data.borderImageSlicePx[3], 0.0f, src.y);
    if (sl + sr > src.x) {
        const float f = src.x / (sl + sr);
        sl *= f;
        sr *= f;
    }
    if (st + sb > src.y) {
        const float f = src.y / (st + sb);
        st *= f;
        sb *= f;
    }
    float dl = box.border.left > 0.0f ? box.border.left : sl;
    float dr = box.border.right > 0.0f ? box.border.right : sr;
    float dt = box.border.top > 0.0f ? box.border.top : st;
    float db = box.border.bottom > 0.0f ? box.border.bottom : sb;
    if (dl + dr > w) {
        const float f = w / (dl + dr);
        dl *= f;
        dr *= f;
    }
    if (dt + db > h) {
        const float f = h / (dt + db);
        dt *= f;
        db *= f;
    }
    const float scaleX = st > 0.0f ? dt / st : (sb > 0.0f ? db / sb : 1.0f);
    const float scaleY = sl > 0.0f ? dl / sl : (sr > 0.0f ? dr / sr : 1.0f);
    const Rect sub = data.borderImageSubUv;
    const uint8_t mode = style.borderImageRepeat;

    auto uvOf = [&](float x0, float y0, float x1, float y1) {
        return Rect{sub.x + x0 / src.x * sub.w, sub.y + y0 / src.y * sub.h, (x1 - x0) / src.x * sub.w,
                    (y1 - y0) / src.y * sub.h};
    };
    auto draw = [&](Rect dst, Rect srcUv) {
        if (dst.w <= 0.0f || dst.h <= 0.0f || srcUv.w <= 0.0f || srcUv.h <= 0.0f) {
            return;
        }
        ImageCmd ic{};
        ic.xf = world;
        ic.asset = style.borderImage;
        ic.dst = dst;
        ic.srcUv = srcUv;
        ic.opacity = alpha;
        ic.smooth = 1;
        ic.variantScale = style.uiScaleEff;  // T-345
        uint32_t idx = static_cast<uint32_t>(out.images.size());
        out.images.push_back(ic);
        out.commands.push_back(Command{CmdKind::DrawImage, {}, idx});
    };
    /// One axis of a tiled region: the tile origins and sizes along `len` for a `tile`-sized tile
    /// (stretch: one tile spanning `len`; repeat: centered run, partial ends; round: whole tiles).
    struct Span {
        float pos;
        float size;
        float srcFrom;  // 0..1 of the source extent along this axis
        float srcTo;
    };
    auto spansOf = [&](float origin, float len, float tile, bool tiled, std::vector<Span>& spans) {
        spans.clear();
        if (len <= 0.0f) {
            return;
        }
        if (!tiled || mode == 0 || tile <= 0.0f) {
            spans.push_back(Span{origin, len, 0.0f, 1.0f});
            return;
        }
        if (mode == 2) {  // round
            const int n = std::max(1, toInt32Sat(std::floor(len / tile + 0.5f)));
            const float size = len / static_cast<float>(n);
            for (int i = 0; i < n; ++i) {
                spans.push_back(Span{origin + size * static_cast<float>(i), size, 0.0f, 1.0f});
            }
            return;
        }
        // repeat: centered; the first tile starts at or before `origin`. The guards only matter
        // when `-= tile` / `+= tile` stop moving a huge coordinate (T-258a).
        float start = origin + (len - tile) * 0.5f;
        for (uint32_t i = 0; start > origin && i < 2 * kMaxBorderImageTiles; ++i) {
            start -= tile;
        }
        uint32_t guard = 0;
        for (float x = start; x < origin + len && guard++ < 2 * kMaxBorderImageTiles; x += tile) {
            const float x0 = std::max(x, origin);
            const float x1 = std::min(x + tile, origin + len);
            if (x1 <= x0) {
                continue;
            }
            spans.push_back(Span{x0, x1 - x0, (x0 - x) / tile, (x1 - x) / tile});
        }
    };
    /// Upper bound of the tiles spansOf() makes along one axis; NaN for a tile that is too small
    /// or not finite (the region is then stretched).
    auto tileCount = [&](float len, float tile, bool tiled) -> double {
        if (!tiled || mode == 0 || tile <= 0.0f || len <= 0.0f) {
            return 1.0;  // Stretched along this axis anyway.
        }
        if (!std::isfinite(tile) || tile < kMinBorderImageTile) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double ratio = static_cast<double>(len) / static_cast<double>(tile);
        return mode == 2 ? std::max(1.0, std::floor(ratio + 0.5)) : std::ceil(ratio) + 1.0;
    };
    std::vector<Span> xs;
    std::vector<Span> ys;
    auto region = [&](Rect dst, float sx0, float sy0, float sx1, float sy1, bool tileX, bool tileY) {
        const float sw = sx1 - sx0;
        const float sh = sy1 - sy0;
        if (dst.w <= 0.0f || dst.h <= 0.0f || sw <= 0.0f || sh <= 0.0f) {
            return;
        }
        // T-258a: a tile under kMinBorderImageTile px (or non-finite) on either axis, more than
        // kMaxBorderImageTiles tiles along one axis, or more than kMaxBorderImageRegionTiles in
        // the region draws the whole region stretched instead.
        const float tileW = sw * scaleX;
        const float tileH = sh * scaleY;
        const double nx = tileCount(dst.w, tileW, tileX);
        const double ny = tileCount(dst.h, tileH, tileY);
        const auto perAxis = static_cast<double>(kMaxBorderImageTiles);
        if (!(nx <= perAxis) || !(ny <= perAxis) ||
            !(nx * ny <= static_cast<double>(kMaxBorderImageRegionTiles))) {
            tileX = false;
            tileY = false;
        }
        spansOf(dst.x, dst.w, tileW, tileX, xs);
        spansOf(dst.y, dst.h, tileH, tileY, ys);
        for (const Span& y : ys) {
            for (const Span& x : xs) {
                draw(Rect{x.pos, y.pos, x.size, y.size}, uvOf(sx0 + sw * x.srcFrom, sy0 + sh * y.srcFrom,
                                                              sx0 + sw * x.srcTo, sy0 + sh * y.srcTo));
            }
        }
    };

    // Corners.
    region(Rect{0, 0, dl, dt}, 0, 0, sl, st, false, false);
    region(Rect{w - dr, 0, dr, dt}, src.x - sr, 0, src.x, st, false, false);
    region(Rect{0, h - db, dl, db}, 0, src.y - sb, sl, src.y, false, false);
    region(Rect{w - dr, h - db, dr, db}, src.x - sr, src.y - sb, src.x, src.y, false, false);
    // Edges.
    region(Rect{dl, 0, w - dl - dr, dt}, sl, 0, src.x - sr, st, true, false);
    region(Rect{dl, h - db, w - dl - dr, db}, sl, src.y - sb, src.x - sr, src.y, true, false);
    region(Rect{0, dt, dl, h - dt - db}, 0, st, sl, src.y - sb, false, true);
    region(Rect{w - dr, dt, dr, h - dt - db}, src.x - sr, st, src.x, src.y - sb, false, true);
    // Middle.
    if (style.borderImageFill != 0) {
        region(Rect{dl, dt, w - dl - dr, h - dt - db}, sl, st, src.x - sr, src.y - sb, true, true);
    }
}

}  // namespace xt
