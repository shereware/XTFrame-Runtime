#include "xt/painter.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

#include "camera.h"
#include "clip_shape.h"
#include "composition.h"
#include "layer_collect.h"
#include "painter_action.h"
#include "shape_modifiers.h"
#include "text_edit.h"
#include "xt/effect_utils.h"
#include "xt/geom.h"
#include "xt/layout.h"
#include "xt/num_guard.h"
#include "xt/project3d.h"
#include "xt/property.h"

/// Painter traversal for scene nodes (Group/Path/Image/Mask) and Element (T-27: background,
/// border, img, overflow clip, z-index, visibility). See Docs/design/09-command-list.md §9.3-9.4,
/// Docs/tasks/T-10.md "Painter", Docs/tasks/T-27.md, Docs/tasks/T-30.md (Text -> DrawGlyphs).

namespace xt {

// Defined in painter_element.cpp.
/// T-327: returns true when `background-repeat` needed more than 4096 tiles and the image was drawn
/// stretched instead (Painter::backgroundTilesCapped()).
bool emitElementBackground(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                           std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints);
void emitElementBorder(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                       std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints,
                       bool allowSnap = true);
/// T-333 (05 §5.9.8): how many FillPaths emitElementBorder draws (0, 1 for the single-color ring, or
/// one per drawn side).
uint32_t elementBorderFillCount(const ElementData& data);
void emitElementImage(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out);
/// T-325 (05 §5.9.8): the `outline-*` ring outside the border box (painter_element.cpp), and how far
/// it reaches past the border box on every side (0 when no ring is drawn).
void emitElementOutline(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                        std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints);
float elementOutlineExtent(const ElementData& data);
/// T-199 (26 §26.3.4): the `border-image` nine-slice, drawn instead of emitElementBorder when
/// `style.borderImage` is set (painter_element.cpp).
void emitElementBorderImage(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out);
bool emitExternalPlaceholder(const ExternalNodeData& ext, const ElementData& data, const Mat2x3& world,
                             float alpha, CommandList& out, std::vector<PathVerb>& scratchVerbs,
                             std::vector<float>& scratchPoints);
/// T-332 (05 §5.9.8): pushes the element's `clip-path` shape as a PushClipPath (an empty shape as
/// an empty PushClipRect) and returns true; false (nothing emitted) when the element has no clip.
/// The caller pops it after the children (painter_element.cpp).
bool pushElementClipShape(const ElementData& data, const Mat2x3& world, CommandList& out,
                          std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints);
/// T-202: one DrawParticles command for the live particles of `particles` (painter_element.cpp).
void emitParticles(const ParticleNodeData& particles, const Mat2x3& world, float alpha, CommandList& out);
/// T-202: the local AABB of `particles`' live quads; false when there are none.
bool particlesBounds(const ParticleNodeData& particles, Rect& out);

// Defined in painter_text.cpp.
void emitTextGlyphs(const TextNodeData& text, const ComputedStyle& style, const Mat2x3& world, float alpha,
                    const IrView* ir, CommandList& out);
uint32_t textDrawableCount(const TextNodeData& text, const ComputedStyle& style, const IrView* ir);
void emitPlaceholderGlyphs(const TextNodeData& text, const ComputedStyle& style, Color color,
                           const Mat2x3& world, float alpha, CommandList& out);

// Defined in painter_caret.cpp (T-33, T-33b).
void emitTextSelection(const TextNodeData& text, const EditState& state, const Mat2x3& world, float alpha,
                       CommandList& out, std::vector<PathVerb>& verbs, std::vector<float>& points);
void emitTextCaret(const TextNodeData& text, const ComputedStyle& style, uint32_t caretOffset,
                   const Mat2x3& world, float alpha, float clock, CommandList& out,
                   std::vector<PathVerb>& verbs, std::vector<float>& points);
void emitCompositionDecoration(const TextNodeData& text, const ComputedStyle& style,
                               const CompositionState& comp, const Mat2x3& world, float alpha,
                               CommandList& out, std::vector<PathVerb>& verbs, std::vector<float>& points);

// Defined below, after this file's anonymous namespace (also used by painter_3d.cpp's
// projectionBoundsHint()): `n`'s own shape bounds in its own plane, per its node kind's rule
// (Docs/design/18-effects.md §18.1), without its descendants. `hasShape` is set to false for a
// node kind that has no shape of its own (Group/Mask/Camera, or a Path with no points), whose
// returned rect is then empty and must not be unioned in.
Rect nodeOwnBounds(const NodeArena& arena, NodeId n, bool& hasShape);

// Painter::Action's full definition lives in painter_action.h (shared with painter_3d.cpp).

namespace {

/// Add(4) -> Alpha, Subtract(5) -> AlphaInverted, Intersect(6) -> Alpha; the four track-matte
/// modes (Alpha/AlphaInverted/Luma/LumaInverted) pass through unchanged. See
/// Docs/design/09-command-list.md §9.4.
MaskMode mapMaskMode(MaskMode mode) {
    switch (mode) {
        case MaskMode::Add:
            return MaskMode::Alpha;
        case MaskMode::Subtract:
            return MaskMode::AlphaInverted;
        case MaskMode::Intersect:
            return MaskMode::Alpha;
        default:
            return mode;
    }
}

/// Builds a PaintRef for `paint` (a PathNodeData's fill or stroke), appending gradient stops to
/// `out.stops` when needed. `opacityProp` is PathNodeData::fillOpacity/strokeOpacity; `alpha` is
/// the inherited-from-ancestors multiplier (see Docs/design/09-command-list.md §9.3 rule 3-4).
PaintRef makePaintRef(const Paint& paint, float opacityProp, float alpha,
                      const std::vector<GradientStop>& stops, CommandList& out) {
    PaintRef ref{};
    ref.kind = paint.kind;
    float a = paint.color.a * opacityProp * alpha;
    if (paint.kind == PaintKind::Linear || paint.kind == PaintKind::Radial) {
        ref.color = Color{1, 1, 1, a};
        ref.p0 = paint.p0;
        ref.p1 = paint.p1;
        // T-116: Lottie/SVG scene paints have no second radial axis (Paint carries only p0/p1),
        // so p2 == p0 keeps the pre-T-116 circle (radius |p1 - p0|) for every scene radial paint.
        ref.p2 = paint.p0;
        ref.stopOffset = static_cast<uint32_t>(out.stops.size());
        ref.stopCount = paint.stopCount;
        out.stops.insert(out.stops.end(), stops.begin() + paint.stopOffset,
                         stops.begin() + paint.stopOffset + paint.stopCount);
    } else {
        ref.color = Color{paint.color.r, paint.color.g, paint.color.b, a};
    }
    return ref;
}

/// Docs/design/18-effects.md §18.1: "有效果" means effectCount > 0 and at least one of those
/// effects is enabled.
bool hasEnabledEffects(const NodeArena& arena, const NodeCommon& c) {
    if (c.effectCount == 0) {
        return false;
    }
    const std::vector<EffectInstance>& pool = arena.effects();
    for (uint32_t i = 0; i < c.effectCount; ++i) {
        if (pool[c.effectOffset + i].enabled != 0) {
            return true;
        }
    }
    return false;
}

/// Expands `acc` (tracked via `any`) to also cover `r`; the first call just adopts `r`.
void unionRect(Rect& acc, bool& any, Rect r) {
    if (!any) {
        acc = r;
        any = true;
        return;
    }
    float x0 = std::min(acc.x, r.x);
    float y0 = std::min(acc.y, r.y);
    float x1 = std::max(acc.x + acc.w, r.x + r.w);
    float y1 = std::max(acc.y + acc.h, r.y + r.h);
    acc = Rect{x0, y0, x1 - x0, y1 - y0};
}

/// Docs/design/18-effects.md §18.1's `subtreeBounds(n)`: `n`'s own shape bounds (per its kind's
/// rule) unioned with every child's subtreeBounds, each mapped into `n`'s local frame via that
/// child's `NodeCommon::local` (its transform relative to `n`, not the full world chain) --
/// entirely in `n`'s own plane, pre-`n`'s-own-transform; the caller (Painter::pushEffectCmd) maps
/// the result into screen space afterwards. Skips invisible/zero-opacity children and Camera
/// nodes, matching what Painter actually paints.
Rect subtreeBounds(const NodeArena& arena, NodeId n) {
    bool any = false;
    Rect acc = nodeOwnBounds(arena, n, any);
    arena.forEachChild(n, [&](NodeId c) {
        const NodeCommon& cc = arena.common(c);
        if (cc.visible == 0 || cc.worldOpacity <= 0.0f || arena.kind(c) == NodeKind::Camera) {
            return;
        }
        Rect childLocal = subtreeBounds(arena, c);
        unionRect(acc, any, transformBounds(cc.local, childLocal));
    });
    if (any && arena.kind(n) == NodeKind::Element) {
        // T-332 (05 §5.9.8): a `clip-path` cuts the element and its subtree down to the shape's
        // bounding box (paint bounds = shape bbox ∩ the unclipped bounds).
        const ElementData& data = arena.element(n);
        if (data.clipShape.kind != ClipShapeKind::None) {
            acc = intersect(acc,
                            clip_shape::bounds(data.clipShape, Rect{0, 0, data.box.width, data.box.height}));
        }
    }
    return any ? acc : Rect{0, 0, 0, 0};
}

/// T-133: intersects a screen-space rectangle with the command list's viewport. A degenerate
/// viewport (width or height <= 0) leaves `r` alone, matching what pushEffectCmd did before.
Rect clampToViewport(Rect r, Rect viewport) {
    if (viewport.w <= 0.0f || viewport.h <= 0.0f) {
        return r;
    }
    float x0 = std::max(r.x, viewport.x);
    float y0 = std::max(r.y, viewport.y);
    float x1 = std::min(r.x + r.w, viewport.x + viewport.w);
    float y1 = std::min(r.y + r.h, viewport.y + viewport.h);
    return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

/// T-233 (31 §31.4.7): whether `st` needs the variable-width path (a taper or a width profile).
bool hasVariableWidth(const Stroke& st) {
    return st.taperStart > 0.0f || st.taperEnd > 0.0f || st.profileCount > 0;
}

/// T-233: the width multiplier of `profile` (sorted by t) at arc-length fraction `u`: linear
/// between points, held beyond the first / last one; 1 for an empty profile.
float profileAt(std::span<const WidthPoint> profile, float u) {
    if (profile.empty()) {
        return 1.0f;
    }
    if (u <= profile.front().t) {
        return profile.front().w;
    }
    for (size_t i = 1; i < profile.size(); ++i) {
        const WidthPoint& b = profile[i];
        if (u <= b.t) {
            const WidthPoint& a = profile[i - 1];
            float span = b.t - a.t;
            float f = span > 0.0f ? (u - a.t) / span : 1.0f;
            return a.w + (b.w - a.w) * f;
        }
    }
    return profile.back().w;
}

/// T-233: the largest multiplier of `profile` (at least 1), for the stroke outset of bounds().
float profileMax(std::span<const WidthPoint> profile) {
    float m = 1.0f;
    for (const WidthPoint& p : profile) {
        m = std::max(m, p.w);
    }
    return m;
}

/// T-233: the width-profile points `st` refers to in `pool` (empty when none or out of range).
std::span<const WidthPoint> profileSpan(const Stroke& st, const std::vector<WidthPoint>& pool) {
    if (st.profileCount == 0 || static_cast<size_t>(st.profileOffset) + st.profileCount > pool.size()) {
        return {};
    }
    return std::span<const WidthPoint>(pool.data() + st.profileOffset, st.profileCount);
}

template <class PaintData>
std::span<const WidthPoint> profileOf(const PaintData& p) {
    return profileSpan(p.strokeStyle, p.widthProfile);
}

}  // namespace

/// T-244 (32 §32.3): a skinned Path's skinned points (NodeArena's side table, filled by the
/// Document's rig step), in its skeleton group's space; nullptr when the path is not skinned, its
/// skeleton is gone, or the cache does not match the path (the Painter then draws `points` as is).
const std::vector<float>* skinnedPointsOf(const NodeArena& arena, NodeId n) {
    if (arena.kind(n) != NodeKind::Path) {
        return nullptr;
    }
    const PathNodeData& p = arena.path(n);
    if (p.skin.weights.empty() || !arena.alive(p.skin.skeleton)) {
        return nullptr;
    }
    const SkinnedGeometry* g = arena.skinned(n);
    return g != nullptr && g->valid != 0 && g->points.size() == p.points.size() ? &g->points : nullptr;
}

/// T-244: a skinned Mesh's skinned positions, as skinnedPointsOf() (nullptr = draw `positions`).
const std::vector<Vec2>* skinnedPositionsOf(const NodeArena& arena, NodeId n) {
    if (arena.kind(n) != NodeKind::Mesh) {
        return nullptr;
    }
    const MeshNodeData& m = arena.mesh(n);
    if (m.skin.weights.empty() || !arena.alive(m.skin.skeleton)) {
        return nullptr;
    }
    const SkinnedGeometry* g = arena.skinned(n);
    return g != nullptr && g->valid != 0 && g->positions.size() == m.positions.size() ? &g->positions
                                                                                      : nullptr;
}

/// T-244: the world of `n`'s parent (identity for a root).
Mat2x3 parentWorldOf(const NodeArena& arena, NodeId n) {
    const NodeId parent = arena.parent(n);
    return parent != kInvalidNode ? arena.common(parent).world : Mat2x3{};
}

/// T-244: a skinned shape's own transform counts as identity (32 §32.2.2): its geometry is already
/// in its parent's space, so it draws with the parent's world, not its own.
Mat2x3 shapeWorld(const NodeArena& arena, NodeId n) {
    if (skinnedPointsOf(arena, n) == nullptr && skinnedPositionsOf(arena, n) == nullptr) {
        return arena.common(n).world;
    }
    return parentWorldOf(arena, n);
}

/// T-244: likewise for the local transform (identity for a skinned shape).
Mat2x3 shapeLocal(const NodeArena& arena, NodeId n) {
    return skinnedPointsOf(arena, n) == nullptr ? arena.common(n).local : Mat2x3{};
}

Rect nodeOwnBounds(const NodeArena& arena, NodeId n, bool& hasShape) {
    Rect acc{};
    bool any = false;
    switch (arena.kind(n)) {
        case NodeKind::Element: {
            const ElementData& data = arena.element(n);
            // T-325: the outline ring reaches past the border box (paint only, not layout).
            const float ex = elementOutlineExtent(data);
            unionRect(acc, any, Rect{-ex, -ex, data.box.width + 2 * ex, data.box.height + 2 * ex});
            break;
        }
        case NodeKind::Path: {
            const PathNodeData& p = arena.path(n);
            // T-244: a skinned path's bounds come from its skinned points (in its parent's space),
            // brought back into its own space so the callers' `local` mapping holds.
            const std::vector<float>* skinned = skinnedPointsOf(arena, n);
            const std::vector<float>& pts = skinned != nullptr ? *skinned : p.points;
            if (!pts.empty()) {
                float minX = pts[0], maxX = pts[0];
                float minY = pts[1], maxY = pts[1];
                for (size_t i = 0; i + 1 < pts.size(); i += 2) {
                    minX = std::min(minX, pts[i]);
                    maxX = std::max(maxX, pts[i]);
                    minY = std::min(minY, pts[i + 1]);
                    maxY = std::max(maxY, pts[i + 1]);
                }
                Rect r{minX, minY, maxX - minX, maxY - minY};
                Mat2x3 invLocal;
                if (skinned != nullptr && invert(arena.common(n).local, invLocal)) {
                    r = transformBounds(invLocal, r);
                }
                if (p.stroke.kind != PaintKind::None && p.strokeStyle.width > 0.0f) {
                    float ex = strokeOutset(p.strokeStyle.width, p.strokeStyle.cap, p.strokeStyle.join,
                                            p.strokeStyle.miterLimit) *
                               profileMax(profileOf(p));
                    r = Rect{r.x - ex, r.y - ex, r.w + 2 * ex, r.h + 2 * ex};
                }
                unionRect(acc, any, r);
            }
            break;
        }
        case NodeKind::Shape: {
            // T-225: from the parameters, stroke outset as for a Path.
            const ShapeNodeData& s = arena.shape(n);
            Rect r{};
            if (modifiers::shapeBounds(s.kind, s.params, r)) {
                if (s.stroke.kind != PaintKind::None && s.strokeStyle.width > 0.0f) {
                    float ex = strokeOutset(s.strokeStyle.width, s.strokeStyle.cap, s.strokeStyle.join,
                                            s.strokeStyle.miterLimit) *
                               profileMax(profileOf(s));
                    r = Rect{r.x - ex, r.y - ex, r.w + 2 * ex, r.h + 2 * ex};
                }
                unionRect(acc, any, r);
            }
            break;
        }
        case NodeKind::Group: {
            bool hasModifier = false;
            arena.forEachChild(
                n, [&](NodeId c) { hasModifier = hasModifier || arena.kind(c) == NodeKind::Modifier; });
            if (!hasModifier) {
                break;  // A plain Group: own bounds is empty; only its descendants have shape.
            }
            // T-225 (31 §31.6.1 bounds): a group with modifiers owns its modified output -- its
            // Path/Shape children's bounds (in its space) run through each modifier: a repeat as
            // the union of every copy, zigzag / wiggle outset by their size, pucker by |amount| of
            // the box, twist as the circle around its center through the farthest corner.
            Rect box{};
            bool have = false;
            arena.forEachChild(n, [&](NodeId c) {
                const NodeKind ck = arena.kind(c);
                const NodeCommon& cc = arena.common(c);
                if (cc.visible == 0) {
                    return;
                }
                if (ck == NodeKind::Path || ck == NodeKind::Shape) {
                    bool childShape = false;
                    Rect r = nodeOwnBounds(arena, c, childShape);
                    if (childShape) {
                        unionRect(box, have, transformBounds(cc.local, r));
                    }
                    return;
                }
                if (ck != NodeKind::Modifier || !have) {
                    return;
                }
                const ModifierNodeData& m = arena.modifier(c);
                if (m.enabled == 0) {
                    return;
                }
                switch (m.kind) {
                    case ModifierKind::Repeat: {
                        std::vector<modifiers::RepeatCopy> copies;
                        modifiers::repeatCopies(m, copies);
                        Rect u{};
                        bool anyCopy = false;
                        for (const modifiers::RepeatCopy& copy : copies) {
                            unionRect(u, anyCopy, transformBounds(copy.m, box));
                        }
                        if (anyCopy) {
                            unionRect(u, anyCopy, box);
                            box = u;
                        }
                        break;
                    }
                    case ModifierKind::ZigZag:
                    case ModifierKind::Wiggle: {
                        const float e = std::fabs(m.params[0]);
                        box = Rect{box.x - e, box.y - e, box.w + 2 * e, box.h + 2 * e};
                        break;
                    }
                    case ModifierKind::Pucker: {
                        const float e =
                            std::min(std::fabs(m.params[0]) / 100.0f, 1.0f) * std::max(box.w, box.h);
                        box = Rect{box.x - e, box.y - e, box.w + 2 * e, box.h + 2 * e};
                        break;
                    }
                    case ModifierKind::Twist: {
                        const Vec2 c0{m.params[1], m.params[2]};
                        float r = 0.0f;
                        const Vec2 corners[4] = {{box.x, box.y},
                                                 {box.x + box.w, box.y},
                                                 {box.x, box.y + box.h},
                                                 {box.x + box.w, box.y + box.h}};
                        for (Vec2 p : corners) {
                            const Vec2 d = p - c0;
                            r = std::max(r, std::sqrt(d.x * d.x + d.y * d.y));
                        }
                        unionRect(box, have, Rect{c0.x - r, c0.y - r, 2 * r, 2 * r});
                        break;
                    }
                    default:
                        break;  // Trim / round stay inside; offset / merge are ignored.
                }
            });
            if (have) {
                unionRect(acc, any, box);
            }
            break;
        }
        case NodeKind::Image: {
            const ImageNodeData& img = arena.image(n);
            unionRect(acc, any, Rect{0, 0, img.width, img.height});
            break;
        }
        case NodeKind::Mesh: {
            // T-245: the AABB of the mesh positions (T-244: skinned positions).
            const MeshNodeData& mesh = arena.mesh(n);
            const std::vector<Vec2>* skinnedPos = skinnedPositionsOf(arena, n);
            const std::vector<Vec2>& positions = skinnedPos != nullptr ? *skinnedPos : mesh.positions;
            if (!positions.empty()) {
                float minX = positions[0].x, maxX = minX;
                float minY = positions[0].y, maxY = minY;
                for (const Vec2& p : positions) {
                    minX = std::min(minX, p.x);
                    maxX = std::max(maxX, p.x);
                    minY = std::min(minY, p.y);
                    maxY = std::max(maxY, p.y);
                }
                Rect r{minX, minY, maxX - minX, maxY - minY};
                Mat2x3 invLocal;
                if (skinnedPos != nullptr && invert(arena.common(n).local, invLocal)) {
                    r = transformBounds(invLocal, r);  // Skinned: parent space -> own space.
                }
                unionRect(acc, any, r);
            }
            break;
        }
        case NodeKind::Particles: {
            Rect r{};
            if (particlesBounds(arena.particles(n), r)) {
                unionRect(acc, any, r);  // T-202: the live quads, in the element's coordinates.
            }
            break;
        }
        case NodeKind::Text: {
            const TextNodeData& text = arena.text(n);
            for (const TextLine& line : text.lines) {
                unionRect(acc, any, line.rect);
            }
            // T-178: glyph-level text effects draw outside the line boxes (radius <= 8 px, a
            // shadow also by its offset); a conservative fixed outset keeps layer bounds from
            // clipping them.
            NodeId parent = arena.parent(n);
            bool effects = !text.styleRuns.empty() ||
                           (parent != kInvalidNode && arena.kind(parent) == NodeKind::Element &&
                            arena.element(parent).style.textEffectCount > 0);
            if (any && effects) {
                acc = Rect{acc.x - 32.0f, acc.y - 32.0f, acc.w + 64.0f, acc.h + 64.0f};
            }
            break;
        }
        default:
            break;  // Group/Mask/Camera: own bounds is empty; only its descendants have shape.
    }
    hasShape = any;
    return any ? acc : Rect{0, 0, 0, 0};
}

void sortedElementChildren(const NodeArena& arena, NodeId node, std::vector<NodeId>& out) {
    out.clear();
    arena.forEachChild(node, [&](NodeId c) { out.push_back(c); });
    if (arena.kind(node) != NodeKind::Element) {
        return;  // Group/Mask children never carry z-index (T-27 rule 6): nothing to reorder.
    }

    // Collect the positions (within `out`) and z-index of every Element-kind child; every other
    // position (Text, an anim's scene-root Group, ...) is left untouched.
    //
    // T-38 (Docs/design/01-architecture.md §1.7): these used to be locals, reallocated on every
    // call -- and pushPlan() calls this once per Group/Mask/Element node with children on every
    // single paint(), so a tree of any size reallocated repeatedly every frame. A function-local
    // `static` keeps their capacity across calls; sortedElementChildren() is not re-entrant
    // (nothing here calls back into it) and Context is single-threaded (01.6), so a plain
    // (non-thread_local) static is safe here. hit_test.cpp's hitTest() also calls this function
    // but never concurrently with Painter.
    //
    // T-336 (05 §5.9.8, 06 §6.9): in a flex container the key is (z-index, `order`, document
    // order) -- CSS paints flex items in order-modified document order; an absolute child is not a
    // flex item and sorts as `order: 0`.
    struct SortKey {
        float z;
        float order;
        NodeId node;
    };
    static std::vector<size_t> positions;
    static std::vector<SortKey> keys;
    positions.clear();
    keys.clear();
    const bool flex = arena.element(node).style.display == Display::Flex;
    for (size_t i = 0; i < out.size(); ++i) {
        if (arena.kind(out[i]) == NodeKind::Element) {
            const ComputedStyle& style = arena.element(out[i]).style;
            const float order =
                flex && style.position != Position::Absolute ? style.base[prop::Order].f : 0.0f;
            positions.push_back(i);
            keys.push_back(SortKey{style.zIndex, order, out[i]});
        }
    }
    if (positions.size() < 2) {
        return;  // 0 or 1 Element child: nothing to reorder.
    }
    // Stable: ties keep their original relative (document) order (Docs/tasks/T-27.md rule 6).
    // Insertion sort instead of std::stable_sort: libstdc++'s stable_sort allocates a temporary
    // buffer on every call, which breaks the per-frame zero-allocation rule (01.7); child lists
    // are short, so O(n^2) is fine.
    for (size_t i = 1; i < keys.size(); ++i) {
        SortKey item = keys[i];
        size_t j = i;
        while (j > 0 &&
               (keys[j - 1].z > item.z || (keys[j - 1].z == item.z && keys[j - 1].order > item.order))) {
            keys[j] = keys[j - 1];
            --j;
        }
        keys[j] = item;
    }
    for (size_t k = 0; k < positions.size(); ++k) {
        out[positions[k]] = keys[k].node;
    }
}

void Painter::paint(const NodeArena& arena, NodeId root, CommandList& out) {
    if (!arena.alive(root)) {
        return;
    }
    m_modifierVertices = 0;
    m_backgroundTilesCapped = false;
    m_drawableCount.assign(arena.capacity(), 0);
    m_has3D.assign(arena.capacity(), 0);
    computeDrawableCount(arena, root);
    m_mappingStack.clear();
    m_currentLayer = kInvalidNode;
    m_overlayQueue.clear();
    m_overlayEntering = kInvalidNode;
    m_paintRoot = root;
    out.time = m_clock;  // T-203: `xtTime` of custom effects (18.7).
    const size_t firstImage = out.images.size();
    const size_t firstParticles = out.particles.size();
    const size_t firstMesh = out.imageMeshes.size();
    const size_t firstEffect = out.effectCmds.size();

    m_stack.clear();
    m_stack.push_back(Action{Action::Kind::EnterNode, root, 1.0f, MaskMode::Alpha});
    runStack(arena, out);
    paintOverlays(arena, out);  // T-194 (08.9): `layer: overlay` subtrees, after the root.
    paintGhost(arena, out);     // T-194 (27.7): the drag ghost on top of everything.

    // T-263d: every asset command appended above carries this document's ids; say whose they are.
    // (Source 0 is what the emitters' value-initialized commands already hold. ShaderRefs are
    // stamped where they are copied, pushEffectCmd().)
    if (m_source != 0) {
        for (size_t i = firstImage; i < out.images.size(); ++i) {
            out.images[i].source = m_source;
        }
        for (size_t i = firstParticles; i < out.particles.size(); ++i) {
            out.particles[i].source = m_source;
        }
        for (size_t i = firstMesh; i < out.imageMeshes.size(); ++i) {
            out.imageMeshes[i].source = m_source;
        }
        for (size_t i = firstEffect; i < out.effectCmds.size(); ++i) {
            out.effectCmds[i].source = m_source;
        }
    }
}

void Painter::runStack(const NodeArena& arena, CommandList& out) {
    while (!m_stack.empty()) {
        Action action = m_stack.back();
        m_stack.pop_back();
        switch (action.kind) {
            case Action::Kind::EnterNode:
                enterNode(arena, action.node, action.alpha, out);
                break;
            case Action::Kind::PushMask: {
                uint32_t idx = static_cast<uint32_t>(out.masks.size());
                out.masks.push_back(MaskCmd{action.mode, out.viewport});
                out.commands.push_back(Command{CmdKind::PushMask, {}, idx});
                break;
            }
            case Action::Kind::EndMaskContent:
                out.commands.push_back(Command{CmdKind::EndMaskContent, {}, 0});
                break;
            case Action::Kind::PopMask:
                out.commands.push_back(Command{CmdKind::PopMask, {}, 0});
                break;
            case Action::Kind::PopLayer:
                out.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
                break;
            case Action::Kind::PopClip:
                out.commands.push_back(Command{CmdKind::PopClip, {}, 0});
                break;
            // T-44b: see the Action comment above and painter_3d.cpp.
            case Action::Kind::EnterNode2D:
                m_currentLayer = kInvalidNode;
                enterNode(arena, action.node, action.alpha, out);
                break;
            case Action::Kind::EnterNodeLayer:
                m_currentLayer = action.node;
                enterNode(arena, action.node, action.alpha, out);
                break;
            case Action::Kind::PushTransform3D: {
                uint32_t idx = static_cast<uint32_t>(out.transforms3.size());
                out.transforms3.push_back(Transform3DCmd{action.xf3});
                out.commands.push_back(Command{CmdKind::PushTransform3D, {}, idx});
                break;
            }
            case Action::Kind::PopTransform3D:
                out.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
                break;
            case Action::Kind::PopProjection:
                m_mappingStack.pop_back();
                out.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
                break;
            case Action::Kind::PopEffect:
                out.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
                break;
        }
    }
}

void Painter::paintOverlays(const NodeArena& arena, CommandList& out) {
    // Nested overlays found while painting one are appended and painted after it.
    for (size_t i = 0; i < m_overlayQueue.size(); ++i) {
        const auto [node, alpha] = m_overlayQueue[i];
        if (!arena.alive(node)) {
            continue;
        }
        m_overlayEntering = node;
        m_currentLayer = kInvalidNode;
        m_stack.push_back(Action{Action::Kind::EnterNode, node, alpha, MaskMode::Alpha});
        runStack(arena, out);
    }
    m_overlayEntering = kInvalidNode;
    m_overlayQueue.clear();
}

void Painter::paintGhost(const NodeArena& arena, CommandList& out) {
    const NodeId source = m_ghostSource;
    if (source == kInvalidNode || !arena.alive(source) || arena.kind(source) != NodeKind::Element ||
        arena.common(source).is3D != 0 || m_drawableCount[source] == 0) {
        return;
    }
    for (NodeId a = arena.parent(source); a != kInvalidNode; a = arena.parent(a)) {
        if (arena.common(a).isSpaceRoot != 0) {
            return;  // Inside a 3D space root: no flat snapshot to move around.
        }
    }
    // The snapshot: the source subtree in an offscreen layer at 60 % opacity, every command then
    // moved by the ghost offset.
    const uint32_t layerIdx = static_cast<uint32_t>(out.layers.size());
    out.layers.push_back(LayerCmd{0.6f, BlendMode::Normal, out.viewport});
    out.commands.push_back(Command{CmdKind::PushLayer, {}, layerIdx});
    const size_t first = out.commands.size();
    const NodeId parent = arena.parent(source);
    const float alpha = parent != kInvalidNode ? arena.common(parent).worldOpacity : 1.0f;
    m_overlayEntering = source;
    m_currentLayer = kInvalidNode;
    m_stack.push_back(Action{Action::Kind::EnterNode, source, alpha, MaskMode::Alpha});
    runStack(arena, out);
    m_overlayEntering = kInvalidNode;
    m_overlayQueue.clear();  // Overlays inside the ghost are not repeated.
    const Mat2x3 shift = translation(m_ghostOffset);
    auto moveRect = [this](Rect& r) {
        r.x += m_ghostOffset.x;
        r.y += m_ghostOffset.y;
    };
    for (size_t i = first; i < out.commands.size(); ++i) {
        const Command& cmd = out.commands[i];
        switch (cmd.kind) {
            case CmdKind::FillPath:
                out.fills[cmd.index].xf = mul(shift, out.fills[cmd.index].xf);
                break;
            case CmdKind::StrokePath:
                out.strokes[cmd.index].xf = mul(shift, out.strokes[cmd.index].xf);
                break;
            case CmdKind::DrawImage:
                out.images[cmd.index].xf = mul(shift, out.images[cmd.index].xf);
                break;
            case CmdKind::DrawGlyphs:
                out.glyphRuns[cmd.index].xf = mul(shift, out.glyphRuns[cmd.index].xf);
                break;
            case CmdKind::PushClipRect:
                out.clipRects[cmd.index].xf = mul(shift, out.clipRects[cmd.index].xf);
                break;
            case CmdKind::PushClipPath:
                out.clipPaths[cmd.index].xf = mul(shift, out.clipPaths[cmd.index].xf);
                break;
            case CmdKind::PushLayer:
                moveRect(out.layers[cmd.index].boundsHint);
                break;
            case CmdKind::PushMask:
                moveRect(out.masks[cmd.index].boundsHint);
                break;
            case CmdKind::PushEffect: {
                EffectCmd& fx = out.effectCmds[cmd.index];
                fx.xf = mul(shift, fx.xf);
                moveRect(fx.boundsHint);
                moveRect(fx.contentBounds);
                break;
            }
            case CmdKind::DrawImageMesh: {
                // T-245: the vertices are already in world coordinates.
                const ImageMeshCmd& mc = out.imageMeshes[cmd.index];
                for (uint32_t v = 0; v < mc.vertexCount; ++v) {
                    out.meshVertices[mc.vertexOffset + v].p =
                        apply(shift, out.meshVertices[mc.vertexOffset + v].p);
                }
                break;
            }
            default:
                break;
        }
    }
    out.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
}

void Painter::computeDrawableCount(const NodeArena& arena, NodeId root) {
    arena.traversePostorder(root, [&](NodeId n) {
        NodeKind kind = arena.kind(n);
        // T-44b (Docs/design/19-3d-space.md §19.5-19.6): Camera nodes are never drawn/hit-tested
        // (19.4) and never carry is3D/have children of their own -- skip both counts outright.
        if (kind == NodeKind::Camera) {
            m_drawableCount[n] = 0;
            m_has3D[n] = 0;
            return;
        }
        uint32_t count = 0;
        // has3D[n]: does n's subtree contain an is3D node -- same rule as layer_collect::
        // computeHas3D (T-44a): a nested 3D space root's own subtree is opaque to this
        // computation (its layers belong to its own, separate projection; see collectLayers()).
        uint8_t has3D = arena.common(n).is3D != 0 ? 1 : 0;
        if (kind == NodeKind::Path) {
            const PathNodeData& p = arena.path(n);
            bool hasFill = p.fill.kind != PaintKind::None;
            bool hasStroke = p.stroke.kind != PaintKind::None && p.strokeStyle.width > 0.0f;
            if (hasFill || hasStroke) {
                count = 1;
            }
        } else if (kind == NodeKind::Shape) {
            // T-225: one drawable when it fills or strokes, like a Path (a Modifier counts 0).
            const ShapeNodeData& s = arena.shape(n);
            if (s.fill.kind != PaintKind::None ||
                (s.stroke.kind != PaintKind::None && s.strokeStyle.width > 0.0f)) {
                count = 1;
            }
        } else if (kind == NodeKind::Image) {
            count = 1;
        } else if (kind == NodeKind::Mesh) {
            count = 1;  // T-245: one DrawImageMesh.
        } else if (kind == NodeKind::Particles) {
            count = arena.particles(n).particles.empty() ? 0 : 1;  // T-202: one DrawParticles.
        } else if (kind == NodeKind::Text) {
            // T-30: each painted GlyphRun is one drawable item. T-178: so is each of its text
            // effects and each inline object.
            NodeId parent = arena.parent(n);
            if (parent != kInvalidNode && arena.kind(parent) == NodeKind::Element) {
                count = textDrawableCount(arena.text(n), arena.element(parent).style, m_ir);
            } else {
                for (const GlyphRun& run : arena.text(n).runs) {
                    if (run.font != kInvalidFont && !run.glyphs.empty()) {
                        count += 1;
                    }
                }
            }
        } else if (kind == NodeKind::Element) {
            // T-27 rule 7: an Element's own background/border/image each count as one drawable
            // item (in addition to its descendants), but only when actually drawn -- i.e. not
            // when visibility: hidden suppresses the Element's own visuals (rule 1).
            const ElementData& data = arena.element(n);
            if (data.style.visibility != Visibility::Hidden) {
                if (data.style.base[prop::BackgroundColor].c.a > 0.0f) {
                    count += 1;
                }
                if (data.style.backgroundImage != kInvalidAsset) {
                    count += 1;
                }
                // T-160: `background-gradient` (05.5c) is its own FillPath (emitElementBackground).
                // Left out, a gradient-only element counted 0 drawables and lost its effects.
                if (data.backgroundGradient.kind != GradientKind::None &&
                    !data.backgroundGradient.stops.empty()) {
                    count += 1;
                }
                if (data.style.borderImage != kInvalidAsset) {
                    count += 1;  // T-199: the nine-slice (drawn instead of the border).
                } else {
                    count += elementBorderFillCount(data);  // T-333: one per side when split.
                }
                if (data.imageAsset != kInvalidAsset) {
                    count += 1;
                }
                // T-191: a placeholder is a fill plus its label.
                if (data.externalNode != kInvalidNode &&
                    arena.external(data.externalNode).showPlaceholder != 0) {
                    count += 1;
                }
            }
        }
        arena.forEachChild(n, [&](NodeId c) {
            const NodeCommon& cc = arena.common(c);
            if (cc.visible != 0 && cc.worldOpacity > 0.0f) {
                count += m_drawableCount[c];
            }
            if (cc.isSpaceRoot == 0) {  // Opaque boundary: see the has3D comment above.
                has3D |= m_has3D[c];
            }
        });
        m_drawableCount[n] = count;
        m_has3D[n] = has3D;
    });
}

void Painter::enterNode(const NodeArena& arena, NodeId n, float inheritedAlpha, CommandList& out) {
    const NodeCommon& c = arena.common(n);
    if (c.visible == 0 || c.worldOpacity <= 0.0f) {
        return;
    }
    // T-44b (Docs/design/19-3d-space.md §19.5-19.6).
    if (c.isSpaceRoot != 0) {
        emitRoot(arena, n, inheritedAlpha, out);
        return;
    }
    if (!m_mappingStack.empty() && c.is3D != 0 && n != m_currentLayer) {
        // Inside a 3D space root's subtree, an is3D node other than "the layer currently being
        // entered" (m_currentLayer, set by the EnterNodeLayer/EnterNode2D action that dispatched
        // here) is a separate layer in the sorted-segment layer list -- not painted from here.
        return;
    }
    if (arena.kind(n) == NodeKind::Camera) {
        return;  // 19.4: never drawn/hit-tested.
    }
    // T-194 (08.9): a `layer: overlay` subtree is skipped here and painted after the root (outside
    // every ancestor clip and layer), unless it is the overlay being entered by that pass. Not
    // inside a 3D space root (its layers are depth-sorted there).
    if ((c.flags2 & kLayerOverlay) != 0 && n != m_overlayEntering && n != m_paintRoot &&
        m_mappingStack.empty()) {
        const NodeId parent = arena.parent(n);
        m_overlayQueue.emplace_back(n, parent != kInvalidNode ? arena.common(parent).worldOpacity : 1.0f);
        return;
    }
    NodeKind kind = arena.kind(n);
    uint32_t drawable = m_drawableCount[n];
    // Docs/design/18-effects.md §18.1: a node with effects always gets its own Layer when
    // opacity < 1 (or a non-Normal blend) -- unlike the plain (no-effect) rule below, the
    // T-326 (18 §18.12): `c.blend` is a scene node's blend or an Element's `mix-blend-mode`
    // (StyleEngine copies the style into NodeCommon::blend); both take the same Layer here, and
    // a Normal blend costs nothing.
    // drawable >= 2 "fold opacity into paint instead of a Layer" optimization does not apply,
    // since an effect must see the node's un-premultiplied content (its own opacity is folded
    // into the Layer, not into each paint's alpha).
    bool hasFx = hasEnabledEffects(arena, c) && drawable > 0;
    bool needsLayer =
        hasFx ? (c.blend != static_cast<uint8_t>(BlendMode::Normal) || c.opacity < 1.0f)
              : (c.blend != static_cast<uint8_t>(BlendMode::Normal) || (c.opacity < 1.0f && drawable >= 2));
    float alpha = needsLayer ? 1.0f : inheritedAlpha * c.opacity;
    if (needsLayer) {
        uint32_t idx = static_cast<uint32_t>(out.layers.size());
        // T-133: the tight content AABB, clamped to the viewport, so the SW backend can allocate a
        // bounded offscreen buffer instead of a full-viewport one. `c.world` is only read on the
        // unprojected path, where it is exactly this node's screen transform; inside a 3D space
        // root contentScreenBounds ignores it and projects the subtree's corners instead.
        Rect layerBounds =
            clampToViewport(contentScreenBounds(arena, n, c.world, out.viewport), out.viewport);
        out.layers.push_back(LayerCmd{c.opacity, static_cast<BlendMode>(c.blend), layerBounds});
        Command push{CmdKind::PushLayer, {}, idx};
        if (c.blend != static_cast<uint8_t>(BlendMode::Normal)) {
            // T-343 (23 §23.aa.2): a blend mode reads what lies below it (on the host scene in
            // Direct mode); its whole subtree is one scene-reading run.
            push.flags[0] = kCommandSceneRead;
        }
        out.commands.push_back(push);
        m_stack.push_back(Action{Action::Kind::PopLayer, kInvalidNode, 0.0f, MaskMode::Alpha});
    }
    if (hasFx) {
        pushEffectCmd(arena, n, out);
        m_stack.push_back(Action{Action::Kind::PopEffect, kInvalidNode, 0.0f, MaskMode::Alpha});
        if (needsLayer) {
            // T-160: the Layer wraps the Effect (18.1), so it must hold the effect's output too --
            // the content AABB alone clipped a shadow/glow to the element's own box (a 3 px line
            // with `shadow(0 0 24px 4px ...)` and opacity < 1 lost its glow). The effect's
            // boundsHint is that content AABB outset by the stack's expansion, same viewport clamp.
            out.layers.back().boundsHint = out.effectCmds.back().boundsHint;
        }
    }
    switch (kind) {
        case NodeKind::Path:
            emitPath(arena, n, alpha, out);
            break;
        case NodeKind::Shape:
            emitShape(arena, n, alpha, out);  // T-225
            break;
        case NodeKind::Image:
            emitImage(arena, n, alpha, out);
            break;
        case NodeKind::Mesh:
            emitMesh(arena, n, alpha, out);  // T-245
            break;
        case NodeKind::Group: {
            // T-225 (31 §31.6.1): a group with Modifier children is collected, modified and drawn.
            bool modified = false;
            arena.forEachChild(
                n, [&](NodeId ch) { modified = modified || arena.kind(ch) == NodeKind::Modifier; });
            if (modified) {
                emitModifiedGroup(arena, n, alpha, out);
            } else {
                pushPlan(arena, n, alpha);
            }
            break;
        }
        case NodeKind::Mask:
            pushPlan(arena, n, alpha);
            break;
        case NodeKind::Element:
            paintElement(arena, n, alpha, out);
            break;
        case NodeKind::Text:
            paintText(arena, n, alpha, out);
            break;
        case NodeKind::Particles: {
            // T-202: in the element's child order; `visibility: hidden` on the element hides them.
            const ParticleNodeData& particles = arena.particles(n);
            const NodeId owner = arena.parent(n);
            if (owner == kInvalidNode || arena.kind(owner) != NodeKind::Element ||
                arena.element(owner).style.visibility != Visibility::Hidden) {
                emitParticles(particles, c.world, alpha, out);
            }
            break;
        }
        default:
            break;  // Free is unreachable here
    }
}

void Painter::paintElement(const NodeArena& arena, NodeId n, float alpha, CommandList& out) {
    const ElementData& data = arena.element(n);
    const NodeCommon& c = arena.common(n);

    // Rule 1: visibility: hidden suppresses the Element's own background/border/image, but its
    // Element children are still painted below -- `visibility` is inherited (05.4), so a child
    // that re-declares `visibility: visible` shows through.
    bool hidden = data.style.visibility == Visibility::Hidden;
    // T-332 (05 §5.9.8): the `clip-path` shape wraps everything the element draws -- after its
    // own transform (c.world), before its background -- and its children; popped after them.
    // With `overflow: hidden` the two clips nest (the shape outside, rule 5's padding-box rect
    // inside, around the children only); effects (18 §18.1) stay outside both.
    if (pushElementClipShape(data, c.world, out, m_elemVerbsScratch, m_elemPointsScratch)) {
        m_stack.push_back(Action{Action::Kind::PopClip, kInvalidNode, 0.0f, MaskMode::Alpha});
    }
    if (!hidden) {
        if (emitElementBackground(data, c.world, alpha, out, m_elemVerbsScratch, m_elemPointsScratch)) {
            m_backgroundTilesCapped = true;  // T-327: Document warns once.
        }
        if (data.style.borderImage != kInvalidAsset) {
            emitElementBorderImage(data, c.world, alpha, out);  // T-199: replaces the plain border.
        } else {
            emitElementBorder(data, c.world, alpha, out, m_elemVerbsScratch, m_elemPointsScratch,
                              !inMotion(arena, n));  // T-345: no pixel alignment while moving.
        }
        // T-191: an unbound video / external texture may draw its placeholder instead.
        if (data.externalNode == kInvalidNode ||
            !emitExternalPlaceholder(arena.external(data.externalNode), data, c.world, alpha, out,
                                     m_elemVerbsScratch, m_elemPointsScratch)) {
            emitElementImage(data, c.world, alpha, out);
        }
        // T-325 (05 §5.9.8): the outline after the border and the element's own image, before the
        // overflow clip and the children; never through the effect chain.
        emitElementOutline(data, c.world, alpha, out, m_elemVerbsScratch, m_elemPointsScratch);
    }

    // Rule 5: overflow clip wraps the children only (padding-box, in this Element's own world
    // transform). PushClipRect is emitted immediately (it needs no deferred state); PopClip is
    // deferred via the stack (pushed before the children's actions, so it pops -- i.e. executes --
    // after every child has been processed).
    // T-336: `auto` clips only once its content overflows; one clipping axis clips the whole box.
    bool clip = clipsOverflow(data);
    if (clip) {
        uint32_t idx = static_cast<uint32_t>(out.clipRects.size());
        out.clipRects.push_back(ClipRectCmd{c.world, data.box.paddingBox()});
        out.commands.push_back(Command{CmdKind::PushClipRect, {}, idx});
        m_stack.push_back(Action{Action::Kind::PopClip, kInvalidNode, 0.0f, MaskMode::Alpha});
    }

    // T-119: an Element's non-Element children -- a compiled `<svg>`'s instantiated Group/Path/
    // Image scene nodes, an `anim`'s scene root, a Text node -- carry no ComputedStyle of their
    // own, so nothing re-derives the inherited `visibility` for them and they used to keep
    // painting under a hidden host. They belong to this Element's own visuals; skip them.
    pushPlan(arena, n, alpha, /*elementChildrenOnly=*/hidden);
}

void Painter::paintText(const NodeArena& arena, NodeId n, float alpha, CommandList& out) {
    NodeId parent = arena.parent(n);
    if (parent == kInvalidNode || arena.kind(parent) != NodeKind::Element) {
        return;  // A Text node always hangs off an Element (document_load.cpp); defensive.
    }
    const ElementData& parentData = arena.element(parent);
    if (parentData.style.visibility == Visibility::Hidden) {
        return;  // T-27 rule 1, extended to the element's text.
    }
    const TextNodeData& text = arena.text(n);
    const Mat2x3& world = arena.common(n).world;

    // T-33: a focused, editable `input` draws its selection behind the glyphs and its caret on
    // top of them. T-33b: while it is composing, the IME's clause highlight replaces the
    // selection (there is none -- CompositionStart deleted it), the caret sits at the
    // composition cursor, and a 1px underline runs below the composition.
    const EditState* state = nullptr;
    if (m_editStates != nullptr && text.editIndex < m_editStates->size() &&
        (parentData.stateBits & static_cast<uint16_t>(kStateFocus)) != 0) {
        state = &(*m_editStates)[text.editIndex];
    }
    bool composing = state != nullptr && m_composition != nullptr && m_composition->active &&
                     m_composition->element == parent;
    if (composing) {
        emitCompositionDecoration(text, parentData.style, *m_composition, world, alpha, out,
                                  m_elemVerbsScratch, m_elemPointsScratch);
    } else if (state != nullptr) {
        emitTextSelection(text, *state, world, alpha, out, m_elemVerbsScratch, m_elemPointsScratch);
    }
    // T-345 (25 §25.4.2 item 4): the glyphs of an unrotated, unscaled text node start on a whole
    // pixel (its translation rounded), so text is not resampled between pixel columns.
    // Not while it or an ancestor is being moved (it would step pixel by pixel).
    Mat2x3 glyphWorld = world;
    if (world.a == 1.0f && world.b == 0.0f && world.c == 0.0f && world.d == 1.0f && !inMotion(arena, n)) {
        glyphWorld.e = std::round(world.e);
        glyphWorld.f = std::round(world.f);
    }
    emitTextGlyphs(text, parentData.style, glyphWorld, alpha, m_ir, out);
    if (!text.placeholderRuns.empty()) {
        // T-395 (08 §8.16): an empty input's placeholder, under the caret.
        Color color = parentData.style.base[prop::Color].c;
        if (m_hasPlaceholderColor) {
            color = m_placeholderColor;
        } else {
            color.a *= 0.5f;
        }
        emitPlaceholderGlyphs(text, parentData.style, color, glyphWorld, alpha, out);
    }
    if (state != nullptr) {
        uint32_t caretOffset = composing ? compositionCaret(*m_composition) : state->cursor;
        emitTextCaret(text, parentData.style, caretOffset, world, alpha, m_clock, out, m_elemVerbsScratch,
                      m_elemPointsScratch);
    }
}

void Painter::pushPlan(const NodeArena& arena, NodeId group, float alpha, bool elementChildrenOnly,
                       bool skipShapes) {
    // Rule 6: sortedElementChildren stable-sorts an Element's Element-kind children by (z-index,
    // document order) among themselves; non-Element children (Text, an `anim`'s instantiated scene
    // root Group) keep their original position. Group/Mask nodes (Lottie-compiled scenes) never
    // carry z-index, so it returns their child list unchanged.
    sortedElementChildren(arena, group, m_childScratch);
    if (elementChildrenOnly) {
        // T-119: paintElement passes this for a `visibility: hidden` host -- only Element children
        // (which re-derive the inherited `visibility` in their own ComputedStyle) are entered.
        size_t kept = 0;
        for (size_t i = 0; i < m_childScratch.size(); ++i) {
            if (arena.kind(m_childScratch[i]) == NodeKind::Element) {
                m_childScratch[kept++] = m_childScratch[i];
            }
        }
        m_childScratch.resize(kept);
    }
    if (skipShapes) {
        // T-225: emitModifiedGroup() has drawn the Path/Shape children; Modifiers never draw.
        size_t kept = 0;
        for (size_t i = 0; i < m_childScratch.size(); ++i) {
            const NodeKind k = arena.kind(m_childScratch[i]);
            if (k != NodeKind::Path && k != NodeKind::Shape && k != NodeKind::Modifier) {
                m_childScratch[kept++] = m_childScratch[i];
            }
        }
        m_childScratch.resize(kept);
    }

    m_planScratch.clear();
    size_t i = 0;
    size_t n = m_childScratch.size();
    while (i < n) {
        if (arena.kind(m_childScratch[i]) != NodeKind::Mask) {
            m_planScratch.push_back(
                Action{Action::Kind::EnterNode, m_childScratch[i], alpha, MaskMode::Alpha});
            ++i;
            continue;
        }

        // Collect the contiguous run of Mask siblings; an invisible Mask is dropped from the
        // active list but still consumed from the run (Docs/design/09-command-list.md §9.4,
        // Docs/tasks/T-10.md "Mask 節點本身的 visible == 0 -> 該 Mask 被忽略（不推）").
        m_maskRunScratch.clear();
        while (i < n && arena.kind(m_childScratch[i]) == NodeKind::Mask) {
            NodeId m = m_childScratch[i];
            if (arena.common(m).visible != 0) {
                m_maskRunScratch.push_back(m);
            }
            ++i;
        }
        size_t contentStart = i;
        while (i < n && arena.kind(m_childScratch[i]) != NodeKind::Mask) {
            ++i;
        }
        size_t contentEnd = i;

        if (m_maskRunScratch.empty()) {
            for (size_t k = contentStart; k < contentEnd; ++k) {
                m_planScratch.push_back(
                    Action{Action::Kind::EnterNode, m_childScratch[k], alpha, MaskMode::Alpha});
            }
            continue;
        }

        bool allAdd = true;
        for (NodeId m : m_maskRunScratch) {
            if (static_cast<MaskMode>(arena.common(m).maskMode) != MaskMode::Add) {
                allAdd = false;
                break;
            }
        }

        if (allAdd) {
            m_planScratch.push_back(Action{Action::Kind::PushMask, kInvalidNode, 0.0f, MaskMode::Alpha});
            for (NodeId m : m_maskRunScratch) {
                m_planScratch.push_back(Action{Action::Kind::EnterNode, m, alpha, MaskMode::Alpha});
            }
            m_planScratch.push_back(
                Action{Action::Kind::EndMaskContent, kInvalidNode, 0.0f, MaskMode::Alpha});
            for (size_t k = contentStart; k < contentEnd; ++k) {
                m_planScratch.push_back(
                    Action{Action::Kind::EnterNode, m_childScratch[k], alpha, MaskMode::Alpha});
            }
            m_planScratch.push_back(Action{Action::Kind::PopMask, kInvalidNode, 0.0f, MaskMode::Alpha});
        } else {
            for (NodeId m : m_maskRunScratch) {
                MaskMode mapped = mapMaskMode(static_cast<MaskMode>(arena.common(m).maskMode));
                m_planScratch.push_back(Action{Action::Kind::PushMask, kInvalidNode, 0.0f, mapped});
                m_planScratch.push_back(Action{Action::Kind::EnterNode, m, alpha, MaskMode::Alpha});
                m_planScratch.push_back(
                    Action{Action::Kind::EndMaskContent, kInvalidNode, 0.0f, MaskMode::Alpha});
            }
            for (size_t k = contentStart; k < contentEnd; ++k) {
                m_planScratch.push_back(
                    Action{Action::Kind::EnterNode, m_childScratch[k], alpha, MaskMode::Alpha});
            }
            for (size_t k = 0; k < m_maskRunScratch.size(); ++k) {
                m_planScratch.push_back(Action{Action::Kind::PopMask, kInvalidNode, 0.0f, MaskMode::Alpha});
            }
        }
    }

    // Push in reverse so m_planScratch[0] ends on top of m_stack (pops, i.e. executes, first).
    for (size_t k = m_planScratch.size(); k > 0; --k) {
        m_stack.push_back(m_planScratch[k - 1]);
    }
}

/// T-225: the paint of a Path or Shape node (both carry the same paint fields).
struct Painter::PaintSource {
    const Paint* fill = nullptr;
    const Paint* stroke = nullptr;
    const Stroke* strokeStyle = nullptr;
    FillRule fillRule = FillRule::NonZero;
    float fillOpacity = 1;
    float strokeOpacity = 1;
    const std::vector<GradientStop>* stops = nullptr;
    const std::vector<float>* dashes = nullptr;
    const std::vector<WidthPoint>* widthProfile = nullptr;  ///< T-233.
};

/// T-225 (31 §31.6.1): one path collected by emitModifiedGroup(): its range in m_modVerbs /
/// m_modPoints, the Path/Shape node it came from (paint and transform), an extra alpha (a repeat
/// copy's opacity), its draw-order rank and whether the pending (command-duplicating) repeat
/// applies to it.
struct Painter::ModPath {
    uint32_t verbOffset = 0;
    uint32_t verbCount = 0;
    uint32_t pointOffset = 0;
    uint32_t pointCount = 0;
    NodeId source = kInvalidNode;
    float alpha = 1;
    uint32_t rank = 0;
    uint8_t repeated = 0;
};

// Defined after ModPath: the member vectors of ModPath need the complete type here.
Painter::Painter() = default;
Painter::~Painter() = default;

namespace {

template <class PaintData>
Painter::PaintSource paintSourceOf(const PaintData& p);

/// Per-path trim (§3.8, 03): the source geometry when the interval covers the whole path, empty
/// when it is empty, else trimPath() into the scratch buffers (Docs/tasks/T-10.md "trim 為 (0,1,0)
/// 或區間 >= 1 時直接引用原路徑，不複製").
void trimmedGeometry(std::span<const PathVerb> srcVerbs, std::span<const float> srcPoints, float trimStart,
                     float trimEnd, float trimOffset, std::vector<PathVerb>& scratchVerbs,
                     std::vector<float>& scratchPoints, std::span<const PathVerb>& verbs,
                     std::span<const float>& points) {
    float start = trimStart < 0.0f ? 0.0f : (trimStart > 1.0f ? 1.0f : trimStart);
    float end = trimEnd < 0.0f ? 0.0f : (trimEnd > 1.0f ? 1.0f : trimEnd);
    if (end < start) {
        std::swap(start, end);
    }
    if ((end - start) >= 1.0f) {
        verbs = srcVerbs;
        points = srcPoints;
        return;
    }
    scratchVerbs.clear();
    scratchPoints.clear();
    if (start != end) {
        trimPath(srcVerbs, srcPoints, trimStart, trimEnd, trimOffset, scratchVerbs, scratchPoints);
    }
    verbs = std::span<const PathVerb>(scratchVerbs);
    points = std::span<const float>(scratchPoints);
}

PathRef appendGeometry(std::span<const PathVerb> verbs, std::span<const float> points, CommandList& out) {
    uint32_t verbOffset = static_cast<uint32_t>(out.verbs.size());
    out.verbs.insert(out.verbs.end(), verbs.begin(), verbs.end());
    uint32_t pointOffset = static_cast<uint32_t>(out.points.size());
    out.points.insert(out.points.end(), points.begin(), points.end());
    return PathRef{verbOffset, static_cast<uint32_t>(verbs.size()), pointOffset,
                   static_cast<uint32_t>(points.size())};
}

template <class PaintData>
Painter::PaintSource paintSourceOf(const PaintData& p) {
    Painter::PaintSource s;
    s.fill = &p.fill;
    s.stroke = &p.stroke;
    s.strokeStyle = &p.strokeStyle;
    s.fillRule = p.fillRule;
    s.fillOpacity = p.fillOpacity;
    s.strokeOpacity = p.strokeOpacity;
    s.stops = &p.stops;
    s.dashes = &p.dashes;
    s.widthProfile = &p.widthProfile;
    return s;
}

}  // namespace

void Painter::emitPathCommands(const PathRef& pathRef, const PaintSource& p, const Mat2x3& xf, float alpha,
                               CommandList& out) {
    if (p.fill->kind != PaintKind::None) {
        FillCmd fc{};
        fc.xf = xf;
        fc.path = pathRef;
        fc.paint = makePaintRef(*p.fill, p.fillOpacity, alpha, *p.stops, out);
        fc.rule = p.fillRule;
        fc.antialias = 1;
        uint32_t idx = static_cast<uint32_t>(out.fills.size());
        out.fills.push_back(fc);
        out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
    }
    const Stroke& st = *p.strokeStyle;
    if (p.stroke->kind != PaintKind::None && st.width > 0.0f && hasVariableWidth(st)) {
        emitVariableStroke(pathRef, p, xf, alpha, out);  // T-233
    } else if (p.stroke->kind != PaintKind::None && st.width > 0.0f) {
        StrokeCmd sc{};
        sc.xf = xf;
        sc.path = pathRef;
        sc.paint = makePaintRef(*p.stroke, p.strokeOpacity, alpha, *p.stops, out);
        sc.width = st.width;
        sc.cap = st.cap;
        sc.join = st.join;
        sc.miterLimit = st.miterLimit;
        sc.dashOffset = static_cast<uint32_t>(out.dashes.size());
        sc.dashCount = st.dashCount;
        out.dashes.insert(out.dashes.end(), p.dashes->begin() + st.dashOffset,
                          p.dashes->begin() + st.dashOffset + st.dashCount);
        sc.dashPhase = st.dashPhase;
        uint32_t idx = static_cast<uint32_t>(out.strokes.size());
        out.strokes.push_back(sc);
        out.commands.push_back(Command{CmdKind::StrokePath, {}, idx});
    }
}

void Painter::emitVariableStroke(const PathRef& pathRef, const PaintSource& p, const Mat2x3& xf, float alpha,
                                 CommandList& out) {
    const Stroke& st = *p.strokeStyle;
    // Flatten in the node's local space (the fill keeps `xf`, so the outline scales with the node
    // exactly like a backend stroke), at the device tolerance divided by the larger axis scale.
    float scale = std::max(std::sqrt(xf.a * xf.a + xf.b * xf.b), std::sqrt(xf.c * xf.c + xf.d * xf.d));
    if (!(scale > 1e-6f)) {
        return;
    }
    const float tolerance = kFlattenTolerance / scale;
    m_vsFlatPoints.clear();
    m_vsFlatStarts.clear();
    m_vsFlatClosed.clear();
    flattenPath(std::span<const PathVerb>(out.verbs.data() + pathRef.verbOffset, pathRef.verbCount),
                std::span<const float>(out.points.data() + pathRef.pointOffset, pathRef.pointCount), Mat2x3{},
                tolerance, m_vsFlatPoints, m_vsFlatStarts, m_vsFlatClosed);

    std::span<const WidthPoint> profile = profileSpan(st, *p.widthProfile);
    const float taperStart = std::clamp(st.taperStart, 0.0f, 1.0f);
    const float taperEnd = std::clamp(st.taperEnd, 0.0f, 1.0f);
    bool taperOn = true;  // Per contour: a closed contour ignores the taper (31 §31.4.7).
    auto widthAt = [&](float u) {
        float w = st.width * profileAt(profile, u);
        if (taperOn) {
            float f = 1.0f;
            if (taperStart > 0.0f && u < taperStart) {
                f = std::min(f, u / taperStart);
            }
            if (taperEnd > 0.0f && u > 1.0f - taperEnd) {
                f = std::min(f, (1.0f - u) / taperEnd);
            }
            w *= std::max(f, 0.0f);
        }
        return w;
    };
    // The dash pattern is not applied to a variable-width stroke (T-233: not specified by 31
    // §31.4.7; the taper / profile is laid along the whole contour).
    m_vsOutPoints.clear();
    m_vsOutStarts.clear();
    const size_t contourCount = m_vsFlatStarts.size();
    for (size_t ci = 0; ci < contourCount; ++ci) {
        const uint32_t start = m_vsFlatStarts[ci];
        const uint32_t end =
            ci + 1 < contourCount ? m_vsFlatStarts[ci + 1] : static_cast<uint32_t>(m_vsFlatPoints.size());
        if (end <= start) {
            continue;
        }
        taperOn = m_vsFlatClosed[ci] == 0;
        const uint32_t zero = 0;
        strokeToFill(std::span<const Vec2>(m_vsFlatPoints.data() + start, end - start),
                     std::span<const uint32_t>(&zero, 1), std::span<const uint8_t>(&m_vsFlatClosed[ci], 1),
                     WidthFn(widthAt), st.cap, st.join, st.miterLimit, tolerance, m_vsOutPoints,
                     m_vsOutStarts);
    }
    if (m_vsOutStarts.empty()) {
        return;
    }

    // One FillPath: every output contour as Move, Line..., Close.
    const uint32_t verbOffset = static_cast<uint32_t>(out.verbs.size());
    const uint32_t pointOffset = static_cast<uint32_t>(out.points.size());
    for (size_t ci = 0; ci < m_vsOutStarts.size(); ++ci) {
        const uint32_t start = m_vsOutStarts[ci];
        const uint32_t end = ci + 1 < m_vsOutStarts.size() ? m_vsOutStarts[ci + 1]
                                                           : static_cast<uint32_t>(m_vsOutPoints.size());
        for (uint32_t i = start; i < end; ++i) {
            out.verbs.push_back(i == start ? PathVerb::Move : PathVerb::Line);
            out.points.push_back(m_vsOutPoints[i].x);
            out.points.push_back(m_vsOutPoints[i].y);
        }
        if (end > start) {
            out.verbs.push_back(PathVerb::Close);
        }
    }
    FillCmd fc{};
    fc.xf = xf;
    fc.path = PathRef{verbOffset, static_cast<uint32_t>(out.verbs.size()) - verbOffset, pointOffset,
                      static_cast<uint32_t>(out.points.size()) - pointOffset};
    fc.paint = makePaintRef(*p.stroke, p.strokeOpacity, alpha, *p.stops, out);
    fc.rule = FillRule::NonZero;
    fc.antialias = 1;
    uint32_t idx = static_cast<uint32_t>(out.fills.size());
    out.fills.push_back(fc);
    out.commands.push_back(Command{CmdKind::FillPath, {}, idx});
}

bool Painter::emitSkinnedPath(const NodeArena& arena, NodeId n, float alpha, CommandList& out) {
    const std::vector<float>* skinned = skinnedPointsOf(arena, n);
    if (skinned == nullptr) {
        return false;
    }
    const PathNodeData& p = arena.path(n);
    std::span<const PathVerb> verbs;
    std::span<const float> points;
    trimmedGeometry(p.verbs, *skinned, p.trimStart, p.trimEnd, p.trimOffset, m_trimVerbsScratch,
                    m_trimPointsScratch, verbs, points);
    const PathRef pathRef = appendGeometry(verbs, points, out);
    emitPathCommands(pathRef, paintSourceOf(p), shapeWorld(arena, n), alpha, out);
    return true;
}

void Painter::emitPath(const NodeArena& arena, NodeId n, float alpha, CommandList& out) {
    if (emitSkinnedPath(arena, n, alpha, out)) {
        return;  // T-244 (32 §32.3): the skinned geometry instead.
    }
    const PathNodeData& p = arena.path(n);
    const NodeCommon& c = arena.common(n);
    std::span<const PathVerb> verbs;
    std::span<const float> points;
    trimmedGeometry(p.verbs, p.points, p.trimStart, p.trimEnd, p.trimOffset, m_trimVerbsScratch,
                    m_trimPointsScratch, verbs, points);
    const PathRef pathRef = appendGeometry(verbs, points, out);
    emitPathCommands(pathRef, paintSourceOf(p), c.world, alpha, out);
}

void Painter::emitShape(const NodeArena& arena, NodeId n, float alpha, CommandList& out) {
    const ShapeNodeData& s = arena.shape(n);
    const NodeCommon& c = arena.common(n);
    m_shapeVerbsScratch.clear();
    m_shapePointsScratch.clear();
    modifiers::buildShape(s.kind, s.params, m_shapeVerbsScratch, m_shapePointsScratch);
    std::span<const PathVerb> verbs;
    std::span<const float> points;
    trimmedGeometry(m_shapeVerbsScratch, m_shapePointsScratch, s.trimStart, s.trimEnd, s.trimOffset,
                    m_trimVerbsScratch, m_trimPointsScratch, verbs, points);
    const PathRef pathRef = appendGeometry(verbs, points, out);
    emitPathCommands(pathRef, paintSourceOf(s), c.world, alpha, out);
}

void Painter::emitModifiedGroup(const NodeArena& arena, NodeId group, float alpha, CommandList& out) {
    m_modVerbs.clear();
    m_modPoints.clear();
    m_modPaths.clear();
    m_pendingCopies.clear();
    bool pendingZeroOnTop = true;

    // The last modifier that does anything: a repeat there duplicates commands instead of
    // geometry (§31.6.1 repeat). Disabled / hidden modifiers and the baked offset / merge are inert.
    auto effective = [&](NodeId c) {
        if (arena.kind(c) != NodeKind::Modifier || arena.common(c).visible == 0) {
            return false;
        }
        const ModifierNodeData& m = arena.modifier(c);
        return m.enabled != 0 && m.kind != ModifierKind::Offset && m.kind != ModifierKind::Merge;
    };
    NodeId lastModifier = kInvalidNode;
    arena.forEachChild(group, [&](NodeId c) {
        if (effective(c)) {
            lastModifier = c;
        }
    });

    // Replaces every collected path's geometry with `apply(index, verbs, points)`'s output.
    auto rewrite = [&](auto&& apply) {
        m_modVerbsNext.clear();
        m_modPointsNext.clear();
        for (size_t i = 0; i < m_modPaths.size(); ++i) {
            ModPath& mp = m_modPaths[i];
            const uint32_t vo = static_cast<uint32_t>(m_modVerbsNext.size());
            const uint32_t po = static_cast<uint32_t>(m_modPointsNext.size());
            apply(i, std::span<const PathVerb>(m_modVerbs.data() + mp.verbOffset, mp.verbCount),
                  std::span<const float>(m_modPoints.data() + mp.pointOffset, mp.pointCount));
            mp.verbOffset = vo;
            mp.verbCount = static_cast<uint32_t>(m_modVerbsNext.size()) - vo;
            mp.pointOffset = po;
            mp.pointCount = static_cast<uint32_t>(m_modPointsNext.size()) - po;
        }
        m_modVerbs.swap(m_modVerbsNext);
        m_modPoints.swap(m_modPointsNext);
    };

    uint32_t wiggleOrdinal = 0;
    arena.forEachChild(group, [&](NodeId c) {
        const NodeKind kind = arena.kind(c);
        const NodeCommon& cc = arena.common(c);
        if (kind == NodeKind::Path || kind == NodeKind::Shape) {
            if (cc.visible == 0) {
                return;
            }
            std::span<const PathVerb> verbs;
            std::span<const float> points;
            if (kind == NodeKind::Path) {
                // T-244 (31 §31.8): skinning before the modifiers -- they see the skinned path.
                const PathNodeData& p = arena.path(c);
                const std::vector<float>* skinned = skinnedPointsOf(arena, c);
                trimmedGeometry(p.verbs, skinned != nullptr ? *skinned : p.points, p.trimStart, p.trimEnd,
                                p.trimOffset, m_trimVerbsScratch, m_trimPointsScratch, verbs, points);
            } else {
                const ShapeNodeData& s = arena.shape(c);
                m_shapeVerbsScratch.clear();
                m_shapePointsScratch.clear();
                modifiers::buildShape(s.kind, s.params, m_shapeVerbsScratch, m_shapePointsScratch);
                trimmedGeometry(m_shapeVerbsScratch, m_shapePointsScratch, s.trimStart, s.trimEnd,
                                s.trimOffset, m_trimVerbsScratch, m_trimPointsScratch, verbs, points);
            }
            ModPath mp;
            mp.verbOffset = static_cast<uint32_t>(m_modVerbs.size());
            mp.verbCount = static_cast<uint32_t>(verbs.size());
            mp.pointOffset = static_cast<uint32_t>(m_modPoints.size());
            mp.pointCount = static_cast<uint32_t>(points.size());
            mp.source = c;
            mp.alpha = cc.opacity;
            mp.rank = static_cast<uint32_t>(m_modPaths.size());
            m_modVerbs.insert(m_modVerbs.end(), verbs.begin(), verbs.end());
            m_modPoints.insert(m_modPoints.end(), points.begin(), points.end());
            m_modPaths.push_back(mp);
            return;
        }
        if (!effective(c) || m_modPaths.empty()) {
            return;
        }
        const ModifierNodeData& m = arena.modifier(c);
        switch (m.kind) {
            case ModifierKind::Trim: {
                std::vector<modifiers::PathSpan> spans(m_modPaths.size());
                for (size_t i = 0; i < m_modPaths.size(); ++i) {
                    const ModPath& mp = m_modPaths[i];
                    spans[i].verbs =
                        std::span<const PathVerb>(m_modVerbs.data() + mp.verbOffset, mp.verbCount);
                    spans[i].points =
                        std::span<const float>(m_modPoints.data() + mp.pointOffset, mp.pointCount);
                }
                std::vector<modifiers::PathRange> ranges;
                m_modVerbsNext.clear();
                m_modPointsNext.clear();
                modifiers::trimPaths(
                    spans, m.params[0], m.params[1], m.params[2],
                    m.mode == modifiers::kTrimSerial ? modifiers::kTrimSerial : modifiers::kTrimParallel,
                    m_modVerbsNext, m_modPointsNext, ranges);
                for (size_t i = 0; i < m_modPaths.size(); ++i) {
                    m_modPaths[i].verbOffset = ranges[i].verbOffset;
                    m_modPaths[i].verbCount = ranges[i].verbCount;
                    m_modPaths[i].pointOffset = ranges[i].pointOffset;
                    m_modPaths[i].pointCount = ranges[i].pointCount;
                }
                m_modVerbs.swap(m_modVerbsNext);
                m_modPoints.swap(m_modPointsNext);
                break;
            }
            case ModifierKind::Repeat: {
                if (c == lastModifier) {
                    // No modifier after it: duplicate commands, not geometry.
                    m_pendingCopies.clear();
                    modifiers::repeatCopies(m, m_pendingCopies);
                    pendingZeroOnTop = modifiers::repeatCopyZeroOnTop(m);
                    for (ModPath& mp : m_modPaths) {
                        mp.repeated = 1;
                    }
                    break;
                }
                // A later modifier needs the copies' geometry: expand, copy 0 first (so a later
                // serial trim runs through the copies in order); ranks keep the draw order.
                m_repeatCopies.clear();
                modifiers::repeatCopies(m, m_repeatCopies);
                const uint32_t pathCount = static_cast<uint32_t>(m_modPaths.size());
                const uint32_t copyCount = static_cast<uint32_t>(m_repeatCopies.size());
                const bool zeroOnTop = modifiers::repeatCopyZeroOnTop(m);
                m_modVerbsNext.clear();
                m_modPointsNext.clear();
                m_modPathsNext.clear();
                for (uint32_t k = 0; k < copyCount; ++k) {
                    const modifiers::RepeatCopy& copy = m_repeatCopies[k];
                    const uint32_t drawSlot = zeroOnTop ? copyCount - 1 - k : k;
                    for (const ModPath& src : m_modPaths) {
                        // The copy transform acts in the group's space; the geometry is in the
                        // source node's space: conjugate by its local transform.
                        const Mat2x3& local = arena.common(src.source).local;
                        Mat2x3 inv;
                        Mat2x3 xf = copy.m;
                        if (invert(local, inv)) {
                            xf = mul(inv, mul(copy.m, local));
                        }
                        ModPath mp = src;
                        mp.verbOffset = static_cast<uint32_t>(m_modVerbsNext.size());
                        mp.pointOffset = static_cast<uint32_t>(m_modPointsNext.size());
                        mp.alpha = src.alpha * copy.alpha;
                        mp.rank = drawSlot * pathCount + src.rank;
                        m_modVerbsNext.insert(m_modVerbsNext.end(), m_modVerbs.begin() + src.verbOffset,
                                              m_modVerbs.begin() + src.verbOffset + src.verbCount);
                        for (uint32_t q = 0; q + 1 < src.pointCount; q += 2) {
                            const Vec2 p = apply(xf, Vec2{m_modPoints[src.pointOffset + q],
                                                          m_modPoints[src.pointOffset + q + 1]});
                            m_modPointsNext.push_back(p.x);
                            m_modPointsNext.push_back(p.y);
                        }
                        m_modPathsNext.push_back(mp);
                    }
                }
                m_modVerbs.swap(m_modVerbsNext);
                m_modPoints.swap(m_modPointsNext);
                m_modPaths.swap(m_modPathsNext);
                break;
            }
            case ModifierKind::Round:
                rewrite([&](size_t, std::span<const PathVerb> v, std::span<const float> p) {
                    modifiers::roundCorners(v, p, m.params[0], m_modVerbsNext, m_modPointsNext);
                });
                break;
            case ModifierKind::Pucker:
                rewrite([&](size_t, std::span<const PathVerb> v, std::span<const float> p) {
                    modifiers::pucker(v, p, m.params[0], m_modVerbsNext, m_modPointsNext);
                });
                break;
            case ModifierKind::ZigZag:
                rewrite([&](size_t, std::span<const PathVerb> v, std::span<const float> p) {
                    modifiers::zigzag(v, p, m.params[0], m.params[1], m.mode == 1, m_modVerbsNext,
                                      m_modPointsNext);
                });
                break;
            case ModifierKind::Twist:
                rewrite([&](size_t, std::span<const PathVerb> v, std::span<const float> p) {
                    modifiers::twist(v, p, m.params[0], Vec2{m.params[1], m.params[2]}, m_modVerbsNext,
                                     m_modPointsNext);
                });
                break;
            case ModifierKind::Wiggle: {
                modifiers::WiggleParams wp;
                wp.size = m.params[0];
                wp.detail = m.params[1];
                wp.seed = static_cast<uint32_t>(toInt32Sat(std::round(m.params[2])));  // T-258a
                wp.speed = m.params[3];
                wp.correlation = m.params[4];
                wp.temporalPhase = m.params[5];
                wp.spatialPhase = m.params[6];
                wp.smooth = m.mode == 1;
                wiggleOrdinal = 0;
                rewrite([&](size_t, std::span<const PathVerb> v, std::span<const float> p) {
                    modifiers::wiggle(v, p, wp, m_time, wiggleOrdinal, m_modVerbsNext, m_modPointsNext);
                });
                break;
            }
            default:
                break;
        }
    });

    // Draw: every path's geometry once (a pending repeat shares its PathRef across the copies),
    // in rank order; the pending repeat's copies first (in their draw order), then the paths
    // collected after it.
    m_modOrder.resize(m_modPaths.size());
    for (size_t i = 0; i < m_modOrder.size(); ++i) {
        m_modOrder[i] = static_cast<uint32_t>(i);
    }
    std::stable_sort(m_modOrder.begin(), m_modOrder.end(),
                     [&](uint32_t a, uint32_t b) { return m_modPaths[a].rank < m_modPaths[b].rank; });
    m_modRefs.resize(m_modPaths.size());
    for (size_t i = 0; i < m_modPaths.size(); ++i) {
        const ModPath& mp = m_modPaths[i];
        const std::span<const PathVerb> verbs(m_modVerbs.data() + mp.verbOffset, mp.verbCount);
        m_modRefs[i] = appendGeometry(
            verbs, std::span<const float>(m_modPoints.data() + mp.pointOffset, mp.pointCount), out);
        // T-225b (21.1): a pending repeat draws a repeated path once per copy.
        const uint32_t times = mp.repeated != 0 ? static_cast<uint32_t>(m_pendingCopies.size()) : 1u;
        m_modifierVertices += modifiers::vertexCount(verbs) * times;
    }
    auto paintOf = [&](NodeId src) {
        return arena.kind(src) == NodeKind::Path ? paintSourceOf(arena.path(src))
                                                 : paintSourceOf(arena.shape(src));
    };
    if (!m_pendingCopies.empty()) {
        const Mat2x3& groupWorld = arena.common(group).world;
        const size_t copyCount = m_pendingCopies.size();
        for (size_t k = 0; k < copyCount; ++k) {
            const modifiers::RepeatCopy& copy = m_pendingCopies[pendingZeroOnTop ? copyCount - 1 - k : k];
            for (uint32_t i : m_modOrder) {
                const ModPath& mp = m_modPaths[i];
                if (mp.repeated == 0 || mp.verbCount == 0) {
                    continue;
                }
                const Mat2x3 xf = mul(groupWorld, mul(copy.m, shapeLocal(arena, mp.source)));
                emitPathCommands(m_modRefs[i], paintOf(mp.source), xf, alpha * mp.alpha * copy.alpha, out);
            }
        }
    }
    for (uint32_t i : m_modOrder) {
        const ModPath& mp = m_modPaths[i];
        if (mp.repeated != 0 || mp.verbCount == 0) {
            continue;
        }
        emitPathCommands(m_modRefs[i], paintOf(mp.source), shapeWorld(arena, mp.source), alpha * mp.alpha,
                         out);
    }

    // Nested groups, masks, images, text: after the modified output, in child order.
    pushPlan(arena, group, alpha, false, /*skipShapes=*/true);
}

void Painter::pushEffectCmd(const NodeArena& arena, NodeId n, CommandList& out) {
    const NodeCommon& c = arena.common(n);
    EffectCmd ec{};

    // Copy n's *enabled* effects (18.1's "複製 enabled 的效果到 CommandList::effects"); a
    // GradientMask's stops are copied into out.stops and its stopOffset rewritten.
    ec.effectOffset = static_cast<uint32_t>(out.effects.size());
    const std::vector<EffectInstance>& pool = arena.effects();
    const std::vector<GradientStop>& poolStops = arena.effectStops();
    uint32_t enabledCount = 0;
    for (uint32_t i = 0; i < c.effectCount; ++i) {
        const EffectInstance& e = pool[c.effectOffset + i];
        if (e.enabled == 0) {
            continue;
        }
        EffectInstance copy = e;
        if (e.stopCount > 0) {
            copy.stopOffset = static_cast<uint32_t>(out.stops.size());
            out.stops.insert(out.stops.end(), poolStops.begin() + e.stopOffset,
                             poolStops.begin() + e.stopOffset + e.stopCount);
        }
        if (e.kind == EffectKind::Custom && m_shaders != nullptr) {
            m_shaders->copyTo(e.shader, m_source, out);  // T-203: once per asset per frame.
        }
        out.effects.push_back(copy);
        ++enabledCount;
    }
    ec.effectCount = enabledCount;

    // xf: plain c.world outside any 3D space root; project3d::jacobianAt at n's own anchor
    // otherwise -- for a plain-2D node sitting directly on the root's (or a sorted segment's own)
    // plane, m_currentLayer is kInvalidNode and the mapping stack's top planeWorld3 is already
    // identity (root plane), so no swap is needed; for the 3D layer node itself or one of its 2D
    // descendants, swap in that layer's own world3 (matches flushSection()'s backface-test
    // pattern) around the query.
    Mat2x3 xf;
    if (m_mappingStack.empty()) {
        xf = c.world;
    } else {
        Mat4 saved = m_mappingStack.back().planeWorld3;
        bool swapped = m_currentLayer != kInvalidNode;
        if (swapped) {
            m_mappingStack.back().planeWorld3 = arena.common(m_currentLayer).world3;
        }
        xf = project3d::jacobianAt(m_mappingStack, c.world, c.anchor);
        if (swapped) {
            m_mappingStack.back().planeWorld3 = saved;
        }
    }
    Rect content = contentScreenBounds(arena, n, xf, out.viewport);
    ec.xf = xf;
    ec.contentBounds = content;

    float expand = 0.0f;
    for (uint32_t i = 0; i < ec.effectCount; ++i) {
        const EffectInstance& e = out.effects[ec.effectOffset + i];
        if (e.kind == EffectKind::Custom) {
            expand += customEffectExpand(m_shaders != nullptr ? m_shaders->find(e.shader) : nullptr, xf);
            continue;
        }
        EffectInstance screenParams = effectToScreen(e, xf);
        expand += effectExpand(screenParams);
    }
    Rect bounds{content.x - expand, content.y - expand, content.w + 2 * expand, content.h + 2 * expand};
    ec.boundsHint = clampToViewport(bounds, out.viewport);

    uint32_t idx = static_cast<uint32_t>(out.effectCmds.size());
    out.effectCmds.push_back(ec);
    Command push{CmdKind::PushEffect, {}, idx};
    if (ec.effectCount > 0 && out.effects[ec.effectOffset].kind == EffectKind::BackdropBlur) {
        // T-343 (23 §23.aa.2): a leading BackdropBlur reads what lies below the node (the
        // host scene in Direct mode); the node's subtree is one scene-reading run. Only enabled
        // effects are copied into the list, so the first entry is enabled.
        push.flags[0] = kCommandSceneRead;
    }
    out.commands.push_back(push);
}

Rect Painter::contentScreenBounds(const NodeArena& arena, NodeId n, const Mat2x3& xf, Rect viewport) {
    // T-133: shared by pushEffectCmd (EffectCmd::contentBounds) and enterNode (LayerCmd::
    // boundsHint), so a bounded SW layer and a bounded SW effect buffer agree on what a node's
    // subtree covers on screen.
    Rect local = subtreeBounds(arena, n);
    if (m_mappingStack.empty()) {
        return transformBounds(xf, local);
    }
    const NodeCommon& c = arena.common(n);
    Mat4 saved = m_mappingStack.back().planeWorld3;
    bool swapped = m_currentLayer != kInvalidNode;
    if (swapped) {
        m_mappingStack.back().planeWorld3 = arena.common(m_currentLayer).world3;
    }
    Vec2 corners[4] = {
        Vec2{local.x, local.y},
        Vec2{local.x + local.w, local.y},
        Vec2{local.x, local.y + local.h},
        Vec2{local.x + local.w, local.y + local.h},
    };
    bool any = false;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (Vec2 p : corners) {
        bool ok = false;
        Vec2 s = project3d::projectPoint(m_mappingStack, c.world, p, ok);
        if (!ok) {
            continue;
        }
        if (!any) {
            minX = maxX = s.x;
            minY = maxY = s.y;
            any = true;
        } else {
            minX = std::min(minX, s.x);
            maxX = std::max(maxX, s.x);
            minY = std::min(minY, s.y);
            maxY = std::max(maxY, s.y);
        }
    }
    if (swapped) {
        m_mappingStack.back().planeWorld3 = saved;
    }
    // Every corner failed to project (w <= 1e-6): fall back to the viewport, the same
    // conservative choice DrawImageQuad/FillPath make elsewhere for an unprojectable subject
    // (Docs/design/19-3d-space.md §19.7) -- not covered explicitly by Docs/tasks/T-52.md.
    return any ? Rect{minX, minY, maxX - minX, maxY - minY} : viewport;
}

void Painter::emitImage(const NodeArena& arena, NodeId n, float alpha, CommandList& out) {
    const ImageNodeData& img = arena.image(n);
    const NodeCommon& c = arena.common(n);
    ImageCmd ic{};
    ic.xf = c.world;
    ic.asset = img.asset;
    ic.dst = Rect{0, 0, img.width, img.height};
    ic.srcUv = Rect{0, 0, 1, 1};
    ic.opacity = alpha;
    ic.smooth = 1;
    uint32_t idx = static_cast<uint32_t>(out.images.size());
    out.images.push_back(ic);
    out.commands.push_back(Command{CmdKind::DrawImage, {}, idx});
}

void Painter::emitMesh(const NodeArena& arena, NodeId n, float alpha, CommandList& out) {
    // T-245 (32 32.2.4): positions x world -> meshVertices, uvs and indices copied, one
    // DrawImageMesh. A mesh with no triangle or mismatched pools (never produced by a validated
    // IR) draws nothing.
    const MeshNodeData& mesh = arena.mesh(n);
    // T-244 (32 §32.3): a skinned mesh draws its skinned positions (the side table) with its
    // parent's world (its own transform counts as identity).
    const std::vector<Vec2>* skinned = skinnedPositionsOf(arena, n);
    const Mat2x3 world = skinned != nullptr ? parentWorldOf(arena, n) : arena.common(n).world;
    const std::vector<Vec2>& positions = skinned != nullptr ? *skinned : mesh.positions;
    if (mesh.indices.size() < 3 || mesh.uvs.size() != positions.size() || positions.empty()) {
        return;
    }
    ImageMeshCmd mc{};
    mc.asset = mesh.asset;
    mc.vertexOffset = static_cast<uint32_t>(out.meshVertices.size());
    mc.vertexCount = static_cast<uint32_t>(positions.size());
    mc.indexOffset = static_cast<uint32_t>(out.meshIndices.size());
    mc.indexCount = static_cast<uint32_t>(mesh.indices.size() / 3 * 3);
    mc.opacity = mesh.opacity * alpha;
    mc.smooth = mesh.smooth;
    for (size_t i = 0; i < positions.size(); ++i) {
        out.meshVertices.push_back(MeshVertex{apply(world, positions[i]), mesh.uvs[i]});
    }
    for (uint32_t i = 0; i < mc.indexCount; ++i) {
        if (mesh.indices[i] >= positions.size()) {
            out.meshVertices.resize(mc.vertexOffset);
            out.meshIndices.resize(mc.indexOffset);
            return;
        }
        out.meshIndices.push_back(mesh.indices[i]);
    }
    uint32_t idx = static_cast<uint32_t>(out.imageMeshes.size());
    out.imageMeshes.push_back(mc);
    out.commands.push_back(Command{CmdKind::DrawImageMesh, {}, idx});
}

}  // namespace xt
