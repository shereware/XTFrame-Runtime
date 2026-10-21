#include "xt/command_simplifier.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "xt/assert.h"
#include "xt/geom.h"
#include "xt/project3d.h"

/// CommandListSimplifier implementation. See docs/design/09-command-list.md §9.5,
/// docs/tasks/T-37.md "CommandListSimplifier" for the exact rule table this follows.
///
/// Conservative choices not spelled out by the rule table (silent -> most conservative):
/// - The FillCmd generated from a degraded StrokePath uses antialias = 1 (always on).
/// - A gradient with zero stops degrades to transparent black (Color{0, 0, 0, 0}); a single-stop
///   gradient degrades to that stop's color.
/// - Gradient stops are assumed sorted ascending by `offset` (the only convention used
///   elsewhere in the codebase); the t=0.5 interpolation does a linear scan assuming this.
/// - PushClipPath's replacement rect is the AABB of the path's raw (unflattened) control points,
///   not the flattened curve -- safe because a cubic Bezier's curve is contained in the convex
///   hull of its control points, so this AABB never under-approximates the true bounds.
/// - PushLayer/PushMask/PushClipPath commands encountered while inside an active (being-dropped)
///   PushMask..EndMaskContent span are dropped entirely, including their bookkeeping (opacity
///   stack / mask depth), matching "移除 PushMask..EndMaskContent 之間的所有命令".

namespace xt {

namespace {

/// Copies one path's verb/point payload from `src` into `out`'s pools, returning the new PathRef.
PathRef copyPath(CommandList& out, const CommandList& src, const PathRef& path) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(out.verbs.size());
    for (uint32_t i = 0; i < path.verbCount; ++i) {
        out.verbs.push_back(src.verbs[path.verbOffset + i]);
    }
    ref.verbCount = path.verbCount;
    ref.pointOffset = static_cast<uint32_t>(out.points.size());
    for (uint32_t i = 0; i < path.pointCount; ++i) {
        out.points.push_back(src.points[path.pointOffset + i]);
    }
    ref.pointCount = path.pointCount;
    return ref;
}

/// Copies `count` GradientStops starting at `offset` from `src.stops` into `out.stops`.
uint32_t copyStops(CommandList& out, const CommandList& src, uint32_t offset, uint32_t count) {
    uint32_t newOffset = static_cast<uint32_t>(out.stops.size());
    for (uint32_t i = 0; i < count; ++i) {
        out.stops.push_back(src.stops[offset + i]);
    }
    return newOffset;
}

/// Copies `src`'s EffectCmd `srcCmd` (and the effect instances/GradientMask stops it references)
/// into `out`, rewriting offsets to point at `out`'s pools. Used when a pass round-trips
/// PushEffect unchanged (options.effects == false, or flatten3DPass's passthrough).
EffectCmd copyEffectCmd(CommandList& out, const CommandList& src, const EffectCmd& srcCmd) {
    EffectCmd dst = srcCmd;
    dst.effectOffset = static_cast<uint32_t>(out.effects.size());
    for (uint32_t i = 0; i < srcCmd.effectCount; ++i) {
        EffectInstance e = src.effects[srcCmd.effectOffset + i];
        if (e.stopCount > 0) {
            e.stopOffset = copyStops(out, src, e.stopOffset, e.stopCount);
        }
        out.effects.push_back(e);
    }
    return dst;
}

/// Interpolated stop color at t (docs/tasks/T-37.md: "顏色 = stops 依 offset 於 t=0.5 的內插
/// 色"), assuming stops are sorted ascending by offset.
Color interpolateStopsAt(std::span<const GradientStop> stops, float t) {
    if (stops.empty()) {
        return Color{0, 0, 0, 0};
    }
    if (stops.size() == 1 || t <= stops[0].offset) {
        return stops[0].color;
    }
    for (size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].offset) {
            const GradientStop& a = stops[i - 1];
            const GradientStop& b = stops[i];
            float span = b.offset - a.offset;
            float localT = span > 0 ? (t - a.offset) / span : 0.0f;
            return Color{
                a.color.r + (b.color.r - a.color.r) * localT,
                a.color.g + (b.color.g - a.color.g) * localT,
                a.color.b + (b.color.b - a.color.b) * localT,
                a.color.a + (b.color.a - a.color.a) * localT,
            };
        }
    }
    return stops.back().color;
}

/// Builds the output PaintRef for `paint` (from `src`), applying gradient-to-solid degradation
/// (if `options.gradients`) and multiplying alpha by `opacityMul` (layer opacity, §9.5).
PaintRef simplifyPaint(CommandList& out, const CommandList& src, const PaintRef& paint,
                       const SimplifyOptions& options, float opacityMul) {
    bool isGradient =
        paint.kind == PaintKind::Linear || paint.kind == PaintKind::Radial || paint.kind == PaintKind::Conic;
    if (isGradient && options.gradients) {
        std::span<const GradientStop> stops(src.stops.data() + paint.stopOffset, paint.stopCount);
        Color mid = interpolateStopsAt(stops, 0.5f);
        PaintRef out_{};
        out_.kind = PaintKind::Solid;
        out_.color = Color{mid.r, mid.g, mid.b, mid.a * paint.color.a * opacityMul};
        return out_;
    }
    PaintRef out_ = paint;
    if (isGradient) {
        out_.stopOffset = copyStops(out, src, paint.stopOffset, paint.stopCount);
    }
    out_.color.a = paint.color.a * opacityMul;
    return out_;
}

void pushCommand(CommandList& out, CmdKind kind, uint32_t index) {
    Command cmd{};
    cmd.kind = kind;
    cmd.index = index;
    out.commands.push_back(cmd);
}

/// T-202: appends a DrawParticles copy of `src` (its instances re-pooled into `out`), every
/// premultiplied instance color scaled by `opacityMul`.
void copyParticles(CommandList& out, const CommandList& list, const ParticlesCmd& src, float opacityMul) {
    ParticlesCmd pc = src;
    pc.instanceOffset = static_cast<uint32_t>(out.particleInstances.size());
    for (uint32_t i = 0; i < src.count; ++i) {
        ParticleInstance p = list.particleInstances[src.instanceOffset + i];
        p.rgba =
            Color{p.rgba.r * opacityMul, p.rgba.g * opacityMul, p.rgba.b * opacityMul, p.rgba.a * opacityMul};
        out.particleInstances.push_back(p);
    }
    out.particles.push_back(pc);
    pushCommand(out, CmdKind::DrawParticles, static_cast<uint32_t>(out.particles.size() - 1));
}

/// T-245: appends a DrawImageMesh copy of `src` (its vertices and indices re-pooled into `out`),
/// the opacity scaled by `opacityMul`. With `mappings` non-empty every vertex is projected through
/// them (flatten3D; the triangles stay affine, docs/design/32-rigging-and-deformation.md §32.2.4)
/// and the whole mesh is dropped when one vertex fails to project (w <= 1e-6), as for a quad.
void copyImageMesh(CommandList& out, const CommandList& list, const ImageMeshCmd& src, float opacityMul,
                   const std::vector<project3d::Mapping>* mappings) {
    ImageMeshCmd mc = src;
    mc.opacity = src.opacity * opacityMul;
    mc.vertexOffset = static_cast<uint32_t>(out.meshVertices.size());
    mc.indexOffset = static_cast<uint32_t>(out.meshIndices.size());
    for (uint32_t i = 0; i < src.vertexCount; ++i) {
        MeshVertex v = list.meshVertices[src.vertexOffset + i];
        if (mappings != nullptr) {
            bool ok = false;
            v.p = project3d::projectPoint(*mappings, Mat2x3{}, v.p, ok);
            if (!ok) {
                out.meshVertices.resize(mc.vertexOffset);
                return;
            }
        }
        out.meshVertices.push_back(v);
    }
    const auto first = list.meshIndices.begin() + static_cast<std::ptrdiff_t>(src.indexOffset);
    out.meshIndices.insert(out.meshIndices.end(), first, first + static_cast<std::ptrdiff_t>(src.indexCount));
    out.imageMeshes.push_back(mc);
    pushCommand(out, CmdKind::DrawImageMesh, static_cast<uint32_t>(out.imageMeshes.size() - 1));
}

/// Converts a flattened polyline (points/contourStarts/closed) into verbs/points appended to
/// `out`'s pools (Move, Line*, [Close]), returning the new PathRef.
PathRef polylineToPath(CommandList& out, std::span<const Vec2> points,
                       std::span<const uint32_t> contourStarts, std::span<const uint8_t> closed) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(out.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(out.points.size());
    for (size_t c = 0; c < contourStarts.size(); ++c) {
        uint32_t start = contourStarts[c];
        uint32_t end =
            (c + 1 < contourStarts.size()) ? contourStarts[c + 1] : static_cast<uint32_t>(points.size());
        if (start >= end) {
            continue;
        }
        out.verbs.push_back(PathVerb::Move);
        out.points.push_back(points[start].x);
        out.points.push_back(points[start].y);
        for (uint32_t i = start + 1; i < end; ++i) {
            out.verbs.push_back(PathVerb::Line);
            out.points.push_back(points[i].x);
            out.points.push_back(points[i].y);
        }
        if (closed[c] != 0) {
            out.verbs.push_back(PathVerb::Close);
        }
    }
    ref.verbCount = static_cast<uint32_t>(out.verbs.size() - ref.verbOffset);
    ref.pointCount = static_cast<uint32_t>(out.points.size() - ref.pointOffset);
    return ref;
}

/// Degrades one StrokePath into a FillPath: applyDash (if any) + strokeToFill, both done in the
/// command's own local coordinate system (xf is not applied here -- it stays attached to the
/// output FillCmd, docs/tasks/T-37.md "描邊在命令座標中做").
void degradeStroke(CommandList& out, const CommandList& src, const StrokeCmd& stroke,
                   const SimplifyOptions& options, float opacityMul) {
    std::vector<Vec2> flatPoints;
    std::vector<uint32_t> flatStarts;
    std::vector<uint8_t> flatClosed;
    flattenPath(std::span<const PathVerb>(src.verbs.data() + stroke.path.verbOffset, stroke.path.verbCount),
                std::span<const float>(src.points.data() + stroke.path.pointOffset, stroke.path.pointCount),
                Mat2x3{}, options.tolerance, flatPoints, flatStarts, flatClosed);

    std::vector<Vec2> dashPoints;
    std::vector<uint32_t> dashStarts;
    std::vector<uint8_t> dashClosed;
    std::span<const Vec2> strokeSrcPoints = flatPoints;
    std::span<const uint32_t> strokeSrcStarts = flatStarts;
    std::span<const uint8_t> strokeSrcClosed = flatClosed;
    if (stroke.dashCount > 0) {
        applyDash(flatPoints, flatStarts, flatClosed,
                  std::span<const float>(src.dashes.data() + stroke.dashOffset, stroke.dashCount),
                  stroke.dashPhase, dashPoints, dashStarts, dashClosed);
        strokeSrcPoints = dashPoints;
        strokeSrcStarts = dashStarts;
        strokeSrcClosed = dashClosed;
    }

    std::vector<Vec2> fillPoints;
    std::vector<uint32_t> fillStarts;
    strokeToFill(strokeSrcPoints, strokeSrcStarts, strokeSrcClosed, stroke.width, stroke.cap, stroke.join,
                 stroke.miterLimit, options.tolerance, fillPoints, fillStarts);
    // strokeToFill's output contours are all implicitly closed.
    std::vector<uint8_t> fillClosed(fillStarts.size(), 1);

    FillCmd fill{};
    fill.xf = stroke.xf;
    fill.path = polylineToPath(out, fillPoints, fillStarts, fillClosed);
    fill.paint = simplifyPaint(out, src, stroke.paint, options, opacityMul);
    fill.rule = FillRule::NonZero;
    fill.antialias = 1;
    out.fills.push_back(fill);
    pushCommand(out, CmdKind::FillPath, static_cast<uint32_t>(out.fills.size() - 1));
}

/// Rect AABB of a path's raw (unflattened) control points, in the path's own local coordinates.
Rect pathAabb(std::span<const float> points, const PathRef& path) {
    if (path.pointCount == 0) {
        return Rect{0, 0, 0, 0};
    }
    float minX = points[path.pointOffset];
    float minY = points[path.pointOffset + 1];
    float maxX = minX;
    float maxY = minY;
    for (uint32_t i = 0; i < path.pointCount; i += 2) {
        float x = points[path.pointOffset + i];
        float y = points[path.pointOffset + i + 1];
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

/// docs/design/19-3d-space.md §19.8's flatten3D constant chord-error tolerance (px, in the
/// projected/root-box space), independent of SimplifyOptions::tolerance.
constexpr float kFlatten3DTolerance = 0.25f;

/// Copies one PaintRef verbatim (including gradient stops), with no opacity/gradient-collapse
/// simplification -- flatten3D runs before the rules that do that (gradients/layers), so its
/// output paint must be untouched.
PaintRef copyPaintVerbatim(CommandList& out, const CommandList& src, const PaintRef& paint) {
    PaintRef out_ = paint;
    bool isGradient =
        paint.kind == PaintKind::Linear || paint.kind == PaintKind::Radial || paint.kind == PaintKind::Conic;
    if (isGradient) {
        out_.stopOffset = copyStops(out, src, paint.stopOffset, paint.stopCount);
    }
    return out_;
}

/// Converts a flattened polyline (points/contourStarts/closed) into local (not appended to any
/// CommandList) verb/point vectors -- the format project3d::projectPolyline expects. Mirrors
/// polylineToPath() above, minus the CommandList-pool bookkeeping.
void polylineToVerbsPoints(std::span<const Vec2> points, std::span<const uint32_t> contourStarts,
                           std::span<const uint8_t> closed, std::vector<PathVerb>& outVerbs,
                           std::vector<float>& outPoints) {
    outVerbs.clear();
    outPoints.clear();
    for (size_t c = 0; c < contourStarts.size(); ++c) {
        uint32_t start = contourStarts[c];
        uint32_t end =
            (c + 1 < contourStarts.size()) ? contourStarts[c + 1] : static_cast<uint32_t>(points.size());
        if (start >= end) {
            continue;
        }
        outVerbs.push_back(PathVerb::Move);
        outPoints.push_back(points[start].x);
        outPoints.push_back(points[start].y);
        for (uint32_t i = start + 1; i < end; ++i) {
            outVerbs.push_back(PathVerb::Line);
            outPoints.push_back(points[i].x);
            outPoints.push_back(points[i].y);
        }
        if (closed[c] != 0) {
            outVerbs.push_back(PathVerb::Close);
        }
    }
}

/// Appends an already-flattened (Move/Line/Close only) verb/point pair into `out`'s pools,
/// returning the new PathRef.
PathRef appendRawPath(CommandList& out, std::span<const PathVerb> verbs, std::span<const float> points) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(out.verbs.size());
    out.verbs.insert(out.verbs.end(), verbs.begin(), verbs.end());
    ref.verbCount = static_cast<uint32_t>(verbs.size());
    ref.pointOffset = static_cast<uint32_t>(out.points.size());
    out.points.insert(out.points.end(), points.begin(), points.end());
    ref.pointCount = static_cast<uint32_t>(points.size());
    return ref;
}

/// flatten3D's shared FillPath/StrokePath/glyph-outline/ClipPath step (docs/design/19-3d-space.md
/// §19.8): flattens `verbs`/`points` (in `xf`'s local coordinates) in plane space (tolerance =
/// 0.25 / projectedScaleMax, per §19.7's SW rule), then projects the result through `mappings`.
/// Returns false (leaving `outVerbs`/`outPoints` empty) if any point's projection fails --
/// the caller then drops the whole path, per §19.7's "任何點 ok == false -> 丟棄".
bool flattenAndProject(std::span<const project3d::Mapping> mappings, std::span<const PathVerb> verbs,
                       std::span<const float> points, const Mat2x3& xf, std::vector<PathVerb>& outVerbs,
                       std::vector<float>& outPoints) {
    PathRef wholePath{0, static_cast<uint32_t>(verbs.size()), 0, static_cast<uint32_t>(points.size())};
    float sMax = project3d::projectedScaleMax(mappings, xf, pathAabb(points, wholePath));
    float tol = kFlatten3DTolerance / sMax;

    std::vector<Vec2> flatPoints;
    std::vector<uint32_t> flatStarts;
    std::vector<uint8_t> flatClosed;
    flattenPath(verbs, points, xf, tol, flatPoints, flatStarts, flatClosed);

    std::vector<PathVerb> localVerbs;
    std::vector<float> localPoints;
    polylineToVerbsPoints(flatPoints, flatStarts, flatClosed, localVerbs, localPoints);

    return project3d::projectPolyline(mappings, Mat2x3{}, localVerbs, localPoints, outVerbs, outPoints);
}

/// The four corners of `r`, clockwise from top-left (docs/design/19-3d-space.md §19.7's
/// ImageQuadCmd::p convention).
void rectCorners(const Rect& r, Vec2 out[4]) {
    out[0] = Vec2{r.x, r.y};
    out[1] = Vec2{r.x + r.w, r.y};
    out[2] = Vec2{r.x + r.w, r.y + r.h};
    out[3] = Vec2{r.x, r.y + r.h};
}

/// Projects each of `dst`'s four corners (`xf`'s local coordinates) through `mappings`. Returns
/// false if any corner's projection fails.
bool projectQuad(std::span<const project3d::Mapping> mappings, const Mat2x3& xf, const Rect& dst,
                 Vec2 outQuad[4]) {
    Vec2 corners[4];
    rectCorners(dst, corners);
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        outQuad[i] = project3d::projectPoint(mappings, xf, corners[i], ok);
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// docs/design/19-3d-space.md §19.8's DrawImage parallelogram test: "投影後四角若仍為平行四邊形
/// (|p0 + p2 - p1 - p3| < 1e-3 * 最長邊)".
bool isParallelogram(const Vec2 quad[4]) {
    Vec2 diff{quad[0].x + quad[2].x - quad[1].x - quad[3].x, quad[0].y + quad[2].y - quad[1].y - quad[3].y};
    float diffLen = std::hypot(diff.x, diff.y);
    float maxEdge = 0.0f;
    for (int i = 0; i < 4; ++i) {
        Vec2 edge = quad[(i + 1) % 4] - quad[i];
        maxEdge = std::max(maxEdge, std::hypot(edge.x, edge.y));
    }
    return diffLen < 1e-3f * maxEdge;
}

/// Solves the affine Mat2x3 mapping `dst`'s top-left/top-right/bottom-left corners onto
/// `quad`[0]/[1]/[3] (docs/design/19-3d-space.md §19.8: "由 dst -> 四角求得的仿射"). Exact for a
/// true parallelogram (quad[2] then also matches, since it's the fourth parallelogram vertex);
/// callers only use this once isParallelogram() has confirmed that. `dst.w`/`dst.h` must be
/// non-zero (checked by the caller).
Mat2x3 affineFromRectToQuad(const Rect& dst, const Vec2 quad[4]) {
    Mat2x3 xf{};
    xf.a = (quad[1].x - quad[0].x) / dst.w;
    xf.b = (quad[1].y - quad[0].y) / dst.w;
    xf.c = (quad[3].x - quad[0].x) / dst.h;
    xf.d = (quad[3].y - quad[0].y) / dst.h;
    xf.e = quad[0].x - xf.a * dst.x - xf.c * dst.y;
    xf.f = quad[0].y - xf.b * dst.x - xf.d * dst.y;
    return xf;
}

/// docs/design/19-3d-space.md §19.8's flatten3D pass: projects every command inside an active
/// PushProjection..PopProjection span, replacing it with the engine-minimum equivalent (a Line-only
/// FillPath, DrawImage/DrawImageQuad, or PushClipPath), and removes the
/// PushProjection/PopProjection/PushTransform3D/PopTransform3D commands themselves. Commands
/// outside any projection span are copied through unchanged, as are PushLayer/PopLayer/PushMask/
/// EndMaskContent/PopMask at any depth (§19.8: "PushLayer/Mask 等原樣").
SimplifyStats flatten3DPass(CommandList& out, const CommandList& list, TextShaper* shaper) {
    SimplifyStats stats{};
    std::vector<project3d::Mapping> mappings;

    std::vector<PathVerb> projVerbs;
    std::vector<float> projPoints;
    std::vector<PathVerb> glyphVerbs;
    std::vector<float> glyphPoints;

    for (const Command& cmd : list.commands) {
        bool inProjection = !mappings.empty();
        switch (cmd.kind) {
            case CmdKind::PushProjection: {
                const ProjectionCmd& p = list.projections[cmd.index];
                project3d::Mapping m;
                m.hasProjection = true;
                m.viewProj = p.viewProj;
                m.rootXf = p.rootXf;
                m.planeWorld3 = mat4Identity();
                mappings.push_back(m);
                break;
            }
            case CmdKind::PopProjection: {
                if (!mappings.empty()) {
                    mappings.pop_back();
                }
                break;
            }
            case CmdKind::PushTransform3D: {
                if (mappings.empty()) {
                    XT_ASSERT(false && "flatten3D: PushTransform3D outside an active PushProjection");
                    break;
                }
                mappings.back().planeWorld3 = list.transforms3[cmd.index].world3;
                break;
            }
            case CmdKind::PopTransform3D: {
                if (!mappings.empty()) {
                    mappings.back().planeWorld3 = mat4Identity();
                }
                break;
            }
            case CmdKind::FillPath: {
                const FillCmd& src = list.fills[cmd.index];
                if (!inProjection) {
                    FillCmd fill{};
                    fill.xf = src.xf;
                    fill.path = copyPath(out, list, src.path);
                    fill.paint = copyPaintVerbatim(out, list, src.paint);
                    fill.rule = src.rule;
                    fill.antialias = src.antialias;
                    out.fills.push_back(fill);
                    pushCommand(out, CmdKind::FillPath, static_cast<uint32_t>(out.fills.size() - 1));
                    break;
                }
                ++stats.flattenedCommands;
                std::span<const PathVerb> verbs(list.verbs.data() + src.path.verbOffset, src.path.verbCount);
                std::span<const float> pts(list.points.data() + src.path.pointOffset, src.path.pointCount);
                if (!flattenAndProject(mappings, verbs, pts, src.xf, projVerbs, projPoints)) {
                    break;  // w <= 1e-6 somewhere -> drop the whole path.
                }
                FillCmd fill{};
                fill.xf = Mat2x3{};
                fill.path = appendRawPath(out, projVerbs, projPoints);
                fill.paint = copyPaintVerbatim(out, list, src.paint);
                fill.rule = src.rule;
                fill.antialias = src.antialias;
                out.fills.push_back(fill);
                pushCommand(out, CmdKind::FillPath, static_cast<uint32_t>(out.fills.size() - 1));
                break;
            }
            case CmdKind::StrokePath: {
                const StrokeCmd& src = list.strokes[cmd.index];
                if (!inProjection) {
                    StrokeCmd stroke{};
                    stroke.xf = src.xf;
                    stroke.path = copyPath(out, list, src.path);
                    stroke.paint = copyPaintVerbatim(out, list, src.paint);
                    stroke.width = src.width;
                    stroke.cap = src.cap;
                    stroke.join = src.join;
                    stroke.miterLimit = src.miterLimit;
                    stroke.dashOffset = static_cast<uint32_t>(out.dashes.size());
                    for (uint32_t i = 0; i < src.dashCount; ++i) {
                        out.dashes.push_back(list.dashes[src.dashOffset + i]);
                    }
                    stroke.dashCount = src.dashCount;
                    stroke.dashPhase = src.dashPhase;
                    out.strokes.push_back(stroke);
                    pushCommand(out, CmdKind::StrokePath, static_cast<uint32_t>(out.strokes.size() - 1));
                    break;
                }
                ++stats.flattenedCommands;
                // §19.8: "先 strokeToFill（平面空間）再同 FillPath（3D 下強制，不看 strokes
                // 選項）" -- flatten in plane space (xf applied), dash (if any), stroke-to-fill,
                // then project the resulting fill polygon exactly like FillPath.
                float sMax = project3d::projectedScaleMax(
                    mappings, src.xf,
                    pathAabb(std::span<const float>(list.points.data() + src.path.pointOffset,
                                                    src.path.pointCount),
                             PathRef{0, src.path.verbCount, 0, src.path.pointCount}));
                float tol = kFlatten3DTolerance / sMax;

                std::vector<Vec2> flatPoints;
                std::vector<uint32_t> flatStarts;
                std::vector<uint8_t> flatClosed;
                flattenPath(
                    std::span<const PathVerb>(list.verbs.data() + src.path.verbOffset, src.path.verbCount),
                    std::span<const float>(list.points.data() + src.path.pointOffset, src.path.pointCount),
                    src.xf, tol, flatPoints, flatStarts, flatClosed);

                std::vector<Vec2> dashPoints;
                std::vector<uint32_t> dashStarts;
                std::vector<uint8_t> dashClosed;
                std::span<const Vec2> strokeSrcPoints = flatPoints;
                std::span<const uint32_t> strokeSrcStarts = flatStarts;
                std::span<const uint8_t> strokeSrcClosed = flatClosed;
                if (src.dashCount > 0) {
                    applyDash(flatPoints, flatStarts, flatClosed,
                              std::span<const float>(list.dashes.data() + src.dashOffset, src.dashCount),
                              src.dashPhase, dashPoints, dashStarts, dashClosed);
                    strokeSrcPoints = dashPoints;
                    strokeSrcStarts = dashStarts;
                    strokeSrcClosed = dashClosed;
                }

                std::vector<Vec2> fillPoints;
                std::vector<uint32_t> fillStarts;
                strokeToFill(strokeSrcPoints, strokeSrcStarts, strokeSrcClosed, src.width, src.cap, src.join,
                             src.miterLimit, tol, fillPoints, fillStarts);
                std::vector<uint8_t> fillClosed(fillStarts.size(), 1);

                std::vector<PathVerb> localVerbs;
                std::vector<float> localPoints;
                polylineToVerbsPoints(fillPoints, fillStarts, fillClosed, localVerbs, localPoints);
                if (!project3d::projectPolyline(mappings, Mat2x3{}, localVerbs, localPoints, projVerbs,
                                                projPoints)) {
                    break;  // w <= 1e-6 somewhere -> drop the whole path.
                }
                FillCmd fill{};
                fill.xf = Mat2x3{};
                fill.path = appendRawPath(out, projVerbs, projPoints);
                fill.paint = copyPaintVerbatim(out, list, src.paint);
                fill.rule = FillRule::NonZero;
                fill.antialias = 1;
                out.fills.push_back(fill);
                pushCommand(out, CmdKind::FillPath, static_cast<uint32_t>(out.fills.size() - 1));
                break;
            }
            case CmdKind::DrawGlyphs: {
                const GlyphsCmd& src = list.glyphRuns[cmd.index];
                if (!inProjection) {
                    GlyphsCmd glyphs{};
                    glyphs.xf = src.xf;
                    glyphs.font = src.font;
                    glyphs.size = src.size;
                    glyphs.color = src.color;
                    glyphs.effect = src.effect;  // T-178.
                    glyphs.effectRadius = src.effectRadius;
                    glyphs.glyphOffset = static_cast<uint32_t>(out.glyphs.size());
                    for (uint32_t i = 0; i < src.glyphCount; ++i) {
                        out.glyphs.push_back(list.glyphs[src.glyphOffset + i]);
                    }
                    glyphs.glyphCount = src.glyphCount;
                    for (uint32_t i = 0; i < src.glyphCount; ++i) {
                        out.glyphPos.push_back(list.glyphPos[src.glyphOffset + i]);
                    }
                    out.glyphRuns.push_back(glyphs);
                    pushCommand(out, CmdKind::DrawGlyphs, static_cast<uint32_t>(out.glyphRuns.size() - 1));
                    break;
                }
                if (src.effect != GlyphEffect::None) {
                    break;  // T-178: a glyph-level effect has no outline-path form; dropped.
                }
                if (shaper == nullptr) {
                    ++stats.droppedGlyphRuns;
                    break;
                }
                for (uint32_t i = 0; i < src.glyphCount; ++i) {
                    uint32_t glyphId = list.glyphs[src.glyphOffset + i];
                    Vec2 pos = list.glyphPos[src.glyphOffset + i];
                    if (!shaper->glyphOutline(src.font, glyphId, glyphVerbs, glyphPoints)) {
                        continue;  // No outline for this glyph (e.g. space); emit nothing.
                    }
                    // p -> src.xf * (p * size + pos): scale/translate the glyph's unit outline
                    // into the run's local space, then the run's own xf into plane space.
                    Mat2x3 glyphXf = mul(src.xf, mul(translation(pos), scaling(Vec2{src.size, src.size})));
                    if (!flattenAndProject(mappings, glyphVerbs, glyphPoints, glyphXf, projVerbs,
                                           projPoints)) {
                        continue;  // w <= 1e-6 somewhere -> drop this glyph's outline.
                    }
                    ++stats.flattenedCommands;
                    FillCmd fill{};
                    fill.xf = Mat2x3{};
                    fill.path = appendRawPath(out, projVerbs, projPoints);
                    fill.paint.kind = PaintKind::Solid;
                    fill.paint.color = src.color;
                    fill.rule = FillRule::NonZero;
                    fill.antialias = 1;
                    out.fills.push_back(fill);
                    pushCommand(out, CmdKind::FillPath, static_cast<uint32_t>(out.fills.size() - 1));
                }
                break;
            }
            case CmdKind::DrawImage: {
                const ImageCmd& src = list.images[cmd.index];
                if (!inProjection) {
                    out.images.push_back(src);
                    pushCommand(out, CmdKind::DrawImage, static_cast<uint32_t>(out.images.size() - 1));
                    break;
                }
                ++stats.flattenedCommands;
                Vec2 quad[4];
                if (!projectQuad(mappings, src.xf, src.dst, quad)) {
                    break;  // w <= 1e-6 somewhere -> drop the whole image.
                }
                bool degenerateRect = std::fabs(src.dst.w) < 1e-6f || std::fabs(src.dst.h) < 1e-6f;
                if (!degenerateRect && isParallelogram(quad)) {
                    ImageCmd image{};
                    image.xf = affineFromRectToQuad(src.dst, quad);
                    image.asset = src.asset;
                    image.dst = src.dst;
                    image.srcUv = src.srcUv;
                    image.opacity = src.opacity;
                    image.smooth = src.smooth;
                    image.source = src.source;
                    image.variantScale = src.variantScale;  // T-345
                    out.images.push_back(image);
                    pushCommand(out, CmdKind::DrawImage, static_cast<uint32_t>(out.images.size() - 1));
                } else {
                    ImageQuadCmd q{};
                    q.asset = src.asset;
                    q.p[0] = quad[0];
                    q.p[1] = quad[1];
                    q.p[2] = quad[2];
                    q.p[3] = quad[3];
                    q.srcUv = src.srcUv;
                    q.opacity = src.opacity;
                    q.smooth = src.smooth;
                    q.source = src.source;              // T-263d: the quad draws the image's asset.
                    q.variantScale = src.variantScale;  // T-345
                    out.imageQuads.push_back(q);
                    pushCommand(out, CmdKind::DrawImageQuad,
                                static_cast<uint32_t>(out.imageQuads.size() - 1));
                }
                break;
            }
            case CmdKind::DrawImageQuad: {
                const ImageQuadCmd& src = list.imageQuads[cmd.index];
                if (!inProjection) {
                    out.imageQuads.push_back(src);
                    pushCommand(out, CmdKind::DrawImageQuad,
                                static_cast<uint32_t>(out.imageQuads.size() - 1));
                    break;
                }
                ++stats.flattenedCommands;
                ImageQuadCmd q = src;
                bool allOk = true;
                for (int i = 0; i < 4; ++i) {
                    bool ok = false;
                    q.p[i] = project3d::projectPoint(mappings, Mat2x3{}, src.p[i], ok);
                    allOk = allOk && ok;
                }
                if (!allOk) {
                    break;  // w <= 1e-6 somewhere -> drop the whole quad.
                }
                out.imageQuads.push_back(q);
                pushCommand(out, CmdKind::DrawImageQuad, static_cast<uint32_t>(out.imageQuads.size() - 1));
                break;
            }
            case CmdKind::PushClipRect: {
                const ClipRectCmd& src = list.clipRects[cmd.index];
                if (!inProjection) {
                    out.clipRects.push_back(src);
                    pushCommand(out, CmdKind::PushClipRect, static_cast<uint32_t>(out.clipRects.size() - 1));
                    break;
                }
                ++stats.flattenedCommands;
                Vec2 quad[4];
                ClipPathCmd clip{};
                clip.xf = Mat2x3{};
                clip.rule = FillRule::NonZero;
                if (projectQuad(mappings, src.xf, src.rect, quad)) {
                    PathVerb verbs[5] = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line,
                                         PathVerb::Close};
                    float pts[8] = {quad[0].x, quad[0].y, quad[1].x, quad[1].y,
                                    quad[2].x, quad[2].y, quad[3].x, quad[3].y};
                    clip.path = appendRawPath(out, verbs, pts);
                } else {
                    // w <= 1e-6 somewhere -> an empty clip path (clips everything out) keeps
                    // Push/PopClip balanced instead of dropping just the Push half.
                    clip.path = PathRef{};
                }
                out.clipPaths.push_back(clip);
                pushCommand(out, CmdKind::PushClipPath, static_cast<uint32_t>(out.clipPaths.size() - 1));
                break;
            }
            case CmdKind::PushClipPath: {
                const ClipPathCmd& src = list.clipPaths[cmd.index];
                if (!inProjection) {
                    ClipPathCmd clip{};
                    clip.xf = src.xf;
                    clip.path = copyPath(out, list, src.path);
                    clip.rule = src.rule;
                    out.clipPaths.push_back(clip);
                    pushCommand(out, CmdKind::PushClipPath, static_cast<uint32_t>(out.clipPaths.size() - 1));
                    break;
                }
                ++stats.flattenedCommands;
                ClipPathCmd clip{};
                clip.xf = Mat2x3{};
                clip.rule = src.rule;
                std::span<const PathVerb> verbs(list.verbs.data() + src.path.verbOffset, src.path.verbCount);
                std::span<const float> pts(list.points.data() + src.path.pointOffset, src.path.pointCount);
                if (flattenAndProject(mappings, verbs, pts, src.xf, projVerbs, projPoints)) {
                    clip.path = appendRawPath(out, projVerbs, projPoints);
                } else {
                    // Same Push/PopClip-balance reasoning as PushClipRect above.
                    clip.path = PathRef{};
                }
                out.clipPaths.push_back(clip);
                pushCommand(out, CmdKind::PushClipPath, static_cast<uint32_t>(out.clipPaths.size() - 1));
                break;
            }
            case CmdKind::PopClip: {
                pushCommand(out, CmdKind::PopClip, 0);
                break;
            }
            case CmdKind::PushLayer: {
                out.layers.push_back(list.layers[cmd.index]);
                pushCommand(out, CmdKind::PushLayer, static_cast<uint32_t>(out.layers.size() - 1));
                break;
            }
            case CmdKind::PopLayer: {
                pushCommand(out, CmdKind::PopLayer, 0);
                break;
            }
            case CmdKind::PushMask: {
                out.masks.push_back(list.masks[cmd.index]);
                pushCommand(out, CmdKind::PushMask, static_cast<uint32_t>(out.masks.size() - 1));
                break;
            }
            case CmdKind::EndMaskContent: {
                pushCommand(out, CmdKind::EndMaskContent, 0);
                break;
            }
            case CmdKind::PopMask: {
                pushCommand(out, CmdKind::PopMask, 0);
                break;
            }
            // docs/design/18-effects.md §18.1: effects execute in screen space (post-projection),
            // so -- like PushLayer/Mask -- they pass through flatten3D unchanged at any depth;
            // EffectCmd::xf already accounts for 3D (Painter computed it via
            // project3d::jacobianAt), so there is nothing left for this pass to project.
            case CmdKind::PushEffect: {
                EffectCmd ec = copyEffectCmd(out, list, list.effectCmds[cmd.index]);
                out.effectCmds.push_back(ec);
                pushCommand(out, CmdKind::PushEffect, static_cast<uint32_t>(out.effectCmds.size() - 1));
                break;
            }
            case CmdKind::PopEffect: {
                pushCommand(out, CmdKind::PopEffect, 0);
                break;
            }
            case CmdKind::DrawParticles: {
                // T-202: particles have no projected form; dropped inside a projection.
                const ParticlesCmd& src = list.particles[cmd.index];
                if (inProjection) {
                    stats.droppedParticles += src.count;
                    break;
                }
                copyParticles(out, list, src, 1.0f);
                break;
            }
            case CmdKind::DrawImageMesh: {
                // T-245: re-pooled; inside a projection every vertex is projected (affine triangles).
                const ImageMeshCmd& src = list.imageMeshes[cmd.index];
                if (inProjection) {
                    ++stats.flattenedCommands;
                }
                copyImageMesh(out, list, src, 1.0f, inProjection ? &mappings : nullptr);
                break;
            }
        }
    }
    return stats;
}

/// docs/tasks/T-263e.md ruling C: everything of a CommandList that is not a command or a pool,
/// carried from a pass's input to its (cleared) output -- the one place that says what happens to
/// each such field, so the next one added to CommandList is decided here and not forgotten.
void copyListHeader(const CommandList& in, CommandList& out) {
    out.viewport = in.viewport;
    out.devicePixelRatio = in.devicePixelRatio;
    out.time = in.time;  // Custom effects read it as xtTime.
    // T-263d: every asset command a pass copies or derives keeps its `source` (the ImageQuadCmd
    // made from a DrawImage takes the image's), so the table goes along as it is.
    out.sources.assign(in.sources.begin(), in.sources.end());
    // Not copied. `frameSerial`: a simplified list is not what Context::render() produced, so it
    // is never a damage base (T-263c). `structureChecked`: simplify() sets it once the result
    // is built (T-263b).
    out.frameSerial = 0;
    out.structureChecked = false;
}

/// docs/tasks/T-263e.md ruling C (T-263d review): the shader pools of a list, copied as they are.
/// The passes copy an EffectInstance without renumbering its `shader`, and a ShaderRef's `source`
/// indexes the table copyListHeader() copied, so a custom effect that survives simplification
/// still finds its ShaderRef, parameters and text. A pool added next to these three goes here.
void copyShaderPools(const CommandList& in, CommandList& out) {
    out.shaders.assign(in.shaders.begin(), in.shaders.end());
    out.shaderParams.assign(in.shaderParams.begin(), in.shaderParams.end());
    out.shaderText.assign(in.shaderText.begin(), in.shaderText.end());
}

}  // namespace

SimplifyStats CommandListSimplifier::simplify(CommandList& list, const SimplifyOptions& options,
                                              TextShaper* shaper) {
    // The out-of-place form into the scratch list, swapped in: the scratch keeps the previous
    // list's buffers for the next call. (An invalid list comes back as a copy of itself.)
    const SimplifyStats stats = simplify(list, m_scratch, options, shaper);
    std::swap(list, m_scratch);
    return stats;
}

SimplifyStats CommandListSimplifier::simplify(const CommandList& in, CommandList& out,
                                              const SimplifyOptions& options, TextShaper* shaper) {
    if (&in == &out) {
        return simplify(out, options, shaper);  // One list for both: the in-place form.
    }
    SimplifyStats stats{};
    // docs/tasks/T-263a.md B10: a list may come from a baked file or a host; one whose indices,
    // ranges or push/pop pairs are broken is handed on as it is, unsimplified (and unmarked: the
    // backend rejects it). The result of a valid one is marked `structureChecked` (T-263b).
    if (!in.structureChecked && !validateCommandList(in)) {
        out = in;
        return stats;
    }
    const CommandList* source = &in;
    if (options.flatten3D) {
        CommandList& flattened = m_flatten3DScratch;
        flattened.clear();
        copyListHeader(in, flattened);
        copyShaderPools(in, flattened);
        stats = flatten3DPass(flattened, in, shaper);
        source = &flattened;
    }
    const CommandList& list = *source;

    out.clear();
    copyListHeader(list, out);
    // `options.effects` drops every PushEffect below, so nothing in the result would refer to
    // the shader pools: they stay empty, as that result always was (a baked frame carries no
    // shader text nobody reads). Kept effects keep their pools.
    if (!options.effects) {
        copyShaderPools(list, out);
    }

    std::vector<float> opacityStack;
    opacityStack.push_back(1.0f);
    int maskShapeDepth = 0;  // > 0: inside an active PushMask..EndMaskContent span being dropped.

    std::vector<PathVerb> glyphVerbs;
    std::vector<float> glyphPoints;

    for (const Command& cmd : list.commands) {
        float opacity = opacityStack.back();
        switch (cmd.kind) {
            case CmdKind::FillPath: {
                if (maskShapeDepth > 0) {
                    break;
                }
                const FillCmd& src = list.fills[cmd.index];
                FillCmd fill{};
                fill.xf = src.xf;
                fill.path = copyPath(out, list, src.path);
                fill.paint = simplifyPaint(out, list, src.paint, options, opacity);
                fill.rule = src.rule;
                fill.antialias = src.antialias;
                out.fills.push_back(fill);
                pushCommand(out, CmdKind::FillPath, static_cast<uint32_t>(out.fills.size() - 1));
                break;
            }
            case CmdKind::StrokePath: {
                if (maskShapeDepth > 0) {
                    break;
                }
                const StrokeCmd& src = list.strokes[cmd.index];
                if (options.strokes) {
                    degradeStroke(out, list, src, options, opacity);
                } else {
                    StrokeCmd stroke{};
                    stroke.xf = src.xf;
                    stroke.path = copyPath(out, list, src.path);
                    stroke.paint = simplifyPaint(out, list, src.paint, options, opacity);
                    stroke.width = src.width;
                    stroke.cap = src.cap;
                    stroke.join = src.join;
                    stroke.miterLimit = src.miterLimit;
                    stroke.dashOffset = static_cast<uint32_t>(out.dashes.size());
                    for (uint32_t i = 0; i < src.dashCount; ++i) {
                        out.dashes.push_back(list.dashes[src.dashOffset + i]);
                    }
                    stroke.dashCount = src.dashCount;
                    stroke.dashPhase = src.dashPhase;
                    out.strokes.push_back(stroke);
                    pushCommand(out, CmdKind::StrokePath, static_cast<uint32_t>(out.strokes.size() - 1));
                }
                break;
            }
            case CmdKind::DrawImage: {
                if (maskShapeDepth > 0) {
                    break;
                }
                ImageCmd image = list.images[cmd.index];
                image.opacity *= opacity;
                out.images.push_back(image);
                pushCommand(out, CmdKind::DrawImage, static_cast<uint32_t>(out.images.size() - 1));
                break;
            }
            case CmdKind::DrawGlyphs: {
                if (maskShapeDepth > 0) {
                    break;
                }
                const GlyphsCmd& src = list.glyphRuns[cmd.index];
                if (!options.glyphsToPaths) {
                    GlyphsCmd glyphs{};
                    glyphs.xf = src.xf;
                    glyphs.font = src.font;
                    glyphs.size = src.size;
                    glyphs.color = src.color;
                    glyphs.color.a *= opacity;
                    glyphs.effect = src.effect;  // T-178.
                    glyphs.effectRadius = src.effectRadius;
                    glyphs.glyphOffset = static_cast<uint32_t>(out.glyphs.size());
                    for (uint32_t i = 0; i < src.glyphCount; ++i) {
                        out.glyphs.push_back(list.glyphs[src.glyphOffset + i]);
                    }
                    glyphs.glyphCount = src.glyphCount;
                    for (uint32_t i = 0; i < src.glyphCount; ++i) {
                        out.glyphPos.push_back(list.glyphPos[src.glyphOffset + i]);
                    }
                    out.glyphRuns.push_back(glyphs);
                    pushCommand(out, CmdKind::DrawGlyphs, static_cast<uint32_t>(out.glyphRuns.size() - 1));
                    break;
                }
                if (src.effect != GlyphEffect::None) {
                    break;  // T-178: a glyph-level effect has no outline-path form; dropped.
                }
                if (shaper == nullptr) {
                    ++stats.droppedGlyphRuns;
                    break;  // glyphsToPaths needs a shaper; no shaper -> drop the command.
                }
                Color color = src.color;
                color.a *= opacity;
                for (uint32_t i = 0; i < src.glyphCount; ++i) {
                    uint32_t glyphId = list.glyphs[src.glyphOffset + i];
                    Vec2 pos = list.glyphPos[src.glyphOffset + i];
                    if (!shaper->glyphOutline(src.font, glyphId, glyphVerbs, glyphPoints)) {
                        continue;  // No outline for this glyph (e.g. space); emit nothing.
                    }
                    FillCmd fill{};
                    fill.xf = src.xf;
                    fill.path.verbOffset = static_cast<uint32_t>(out.verbs.size());
                    for (PathVerb v : glyphVerbs) {
                        out.verbs.push_back(v);
                    }
                    fill.path.verbCount = static_cast<uint32_t>(glyphVerbs.size());
                    fill.path.pointOffset = static_cast<uint32_t>(out.points.size());
                    for (size_t p = 0; p + 1 < glyphPoints.size(); p += 2) {
                        out.points.push_back(glyphPoints[p] * src.size + pos.x);
                        out.points.push_back(glyphPoints[p + 1] * src.size + pos.y);
                    }
                    fill.path.pointCount = static_cast<uint32_t>(glyphPoints.size());
                    fill.paint.kind = PaintKind::Solid;
                    fill.paint.color = color;
                    fill.rule = FillRule::NonZero;
                    fill.antialias = 1;
                    out.fills.push_back(fill);
                    pushCommand(out, CmdKind::FillPath, static_cast<uint32_t>(out.fills.size() - 1));
                }
                break;
            }
            case CmdKind::PushClipRect: {
                if (maskShapeDepth > 0) {
                    break;
                }
                out.clipRects.push_back(list.clipRects[cmd.index]);
                pushCommand(out, CmdKind::PushClipRect, static_cast<uint32_t>(out.clipRects.size() - 1));
                break;
            }
            case CmdKind::PushClipPath: {
                if (maskShapeDepth > 0) {
                    break;
                }
                const ClipPathCmd& src = list.clipPaths[cmd.index];
                if (options.clipPaths) {
                    ClipRectCmd rect{};
                    rect.xf = src.xf;
                    rect.rect = pathAabb(list.points, src.path);
                    out.clipRects.push_back(rect);
                    pushCommand(out, CmdKind::PushClipRect, static_cast<uint32_t>(out.clipRects.size() - 1));
                } else {
                    ClipPathCmd clip{};
                    clip.xf = src.xf;
                    clip.path = copyPath(out, list, src.path);
                    clip.rule = src.rule;
                    out.clipPaths.push_back(clip);
                    pushCommand(out, CmdKind::PushClipPath, static_cast<uint32_t>(out.clipPaths.size() - 1));
                }
                break;
            }
            case CmdKind::PopClip: {
                if (maskShapeDepth > 0) {
                    break;
                }
                pushCommand(out, CmdKind::PopClip, 0);
                break;
            }
            case CmdKind::PushLayer: {
                const LayerCmd& src = list.layers[cmd.index];
                if (options.layers) {
                    opacityStack.push_back(opacity * src.opacity);
                } else {
                    opacityStack.push_back(opacity);
                    if (maskShapeDepth == 0) {
                        LayerCmd layer = src;
                        layer.blend = BlendMode::Normal;
                        out.layers.push_back(layer);
                        pushCommand(out, CmdKind::PushLayer, static_cast<uint32_t>(out.layers.size() - 1));
                    }
                }
                break;
            }
            case CmdKind::PopLayer: {
                if (opacityStack.size() <= 1) {
                    break;  // B10: an unmatched PopLayer pops nothing (and emits nothing).
                }
                opacityStack.pop_back();
                if (!options.layers && maskShapeDepth == 0) {
                    pushCommand(out, CmdKind::PopLayer, 0);
                }
                break;
            }
            case CmdKind::PushMask: {
                if (options.masks) {
                    ++maskShapeDepth;
                } else if (maskShapeDepth == 0) {
                    out.masks.push_back(list.masks[cmd.index]);
                    pushCommand(out, CmdKind::PushMask, static_cast<uint32_t>(out.masks.size() - 1));
                }
                break;
            }
            case CmdKind::EndMaskContent: {
                if (options.masks) {
                    if (maskShapeDepth == 0) {
                        break;  // B10: an EndMaskContent without its PushMask.
                    }
                    --maskShapeDepth;
                } else if (maskShapeDepth == 0) {
                    pushCommand(out, CmdKind::EndMaskContent, 0);
                }
                break;
            }
            case CmdKind::PopMask: {
                if (!options.masks && maskShapeDepth == 0) {
                    pushCommand(out, CmdKind::PopMask, 0);
                }
                break;
            }
            // docs/tasks/T-52.md: `options.effects` (default true) drops PushEffect/PopEffect,
            // leaving the wrapped content commands in place (they are processed individually by
            // this same loop, unaffected) -- the 09.5 engine-minimum-subset degradation for
            // effects.
            case CmdKind::PushEffect: {
                if (maskShapeDepth > 0) {
                    break;
                }
                const EffectCmd& src = list.effectCmds[cmd.index];
                if (options.effects) {
                    stats.droppedEffects += src.effectCount;
                    break;
                }
                EffectCmd ec = copyEffectCmd(out, list, src);
                out.effectCmds.push_back(ec);
                pushCommand(out, CmdKind::PushEffect, static_cast<uint32_t>(out.effectCmds.size() - 1));
                break;
            }
            case CmdKind::PopEffect: {
                if (maskShapeDepth > 0) {
                    break;
                }
                if (!options.effects) {
                    pushCommand(out, CmdKind::PopEffect, 0);
                }
                break;
            }
            // docs/design/19-3d-space.md §19.8: the `flatten3D` rule (this class's own dedicated
            // pass above, run first by simplify() when options.flatten3D) removes every
            // PushProjection/PopProjection/PushTransform3D/PopTransform3D and replaces DrawImage
            // (when non-parallelogram) with DrawImageQuad; this second pass only still sees these
            // command kinds when `options.flatten3D` was false, and just round-trips them
            // unchanged (they carry no opacity/gradient/mask state relevant to this pass's rules).
            case CmdKind::PushProjection: {
                if (maskShapeDepth > 0) {
                    break;
                }
                out.projections.push_back(list.projections[cmd.index]);
                pushCommand(out, CmdKind::PushProjection, static_cast<uint32_t>(out.projections.size() - 1));
                break;
            }
            case CmdKind::PopProjection: {
                if (maskShapeDepth > 0) {
                    break;
                }
                pushCommand(out, CmdKind::PopProjection, 0);
                break;
            }
            case CmdKind::PushTransform3D: {
                if (maskShapeDepth > 0) {
                    break;
                }
                out.transforms3.push_back(list.transforms3[cmd.index]);
                pushCommand(out, CmdKind::PushTransform3D, static_cast<uint32_t>(out.transforms3.size() - 1));
                break;
            }
            case CmdKind::PopTransform3D: {
                if (maskShapeDepth > 0) {
                    break;
                }
                pushCommand(out, CmdKind::PopTransform3D, 0);
                break;
            }
            case CmdKind::DrawImageQuad: {
                if (maskShapeDepth > 0) {
                    break;
                }
                out.imageQuads.push_back(list.imageQuads[cmd.index]);
                pushCommand(out, CmdKind::DrawImageQuad, static_cast<uint32_t>(out.imageQuads.size() - 1));
                break;
            }
            case CmdKind::DrawParticles: {
                // T-202 (18.12): not in the engine-minimum subset.
                const ParticlesCmd& src = list.particles[cmd.index];
                if (options.particles) {
                    stats.droppedParticles += src.count;
                    break;
                }
                if (maskShapeDepth > 0) {
                    break;
                }
                copyParticles(out, list, src, opacity);
                break;
            }
            case CmdKind::DrawImageMesh: {
                // T-245: the layer opacity folds into the command's own, like DrawImage.
                if (maskShapeDepth > 0) {
                    break;
                }
                copyImageMesh(out, list, list.imageMeshes[cmd.index], opacity, nullptr);
                break;
            }
        }
    }

    // docs/tasks/T-263b.md: built here from a valid list, rule by rule, and valid for every
    // tests/golden/ui document under every option combination (tests/gpu's corpus test): the
    // backend need not check it again.
    out.structureChecked = true;
    return stats;
}

}  // namespace xt
