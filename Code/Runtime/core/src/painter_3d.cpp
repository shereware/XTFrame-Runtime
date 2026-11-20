#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

#include "camera.h"
#include "layer_collect.h"
#include "painter_action.h"
#include "xt/effect_utils.h"
#include "xt/geom.h"
#include "xt/layout.h"
#include "xt/painter.h"
#include "xt/project3d.h"
#include "xt/property.h"

/// Painter::emitRoot / Painter::flushSection: entering a 3D space root (Docs/design/19-3d-space.md
/// §19.5 "Painter" and §19.6 "深度排序與背面", step 3 as amended by §19.x ruling 1). See
/// Docs/tasks/T-44b.md and Docs/tasks/T-727.md.
///
/// Both functions run entirely synchronously when the root's/segment's EnterNode action is
/// dequeued -- they compute the camera setup, section the root's children, and collect/sort/
/// backface-cull each sorted segment's layers all up front. Only the actual *painting* of that
/// content is deferred, by appending Actions to Painter::m_planScratch (reusing pushPlan()'s
/// existing "build a plan, then push it reversed onto m_stack" pattern) so it interleaves
/// correctly with everything else already on the explicit-stack traversal (T-10's "no recursion").

namespace xt {

// Defined in painter_element.cpp (see painter.cpp's matching forward declarations).
/// T-327: returns true when `background-repeat` needed more than 4096 tiles and the image was drawn
/// stretched instead (Painter::backgroundTilesCapped()).
bool emitElementBackground(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                           std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints);
/// T-737: `dashCache` (the Painter's; nullptr computes every side) serves the dashed / dotted sides.
void emitElementBorder(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                       std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints,
                       bool allowSnap = true, BorderDashCache* dashCache = nullptr);
void emitElementImage(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out);
/// T-199 (26 §26.3.4): the `border-image` nine-slice, drawn instead of emitElementBorder when
/// `style.borderImage` is set (painter_element.cpp).
void emitElementBorderImage(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out);
bool emitExternalPlaceholder(const ExternalNodeData& ext, const ElementData& data, const Mat2x3& world,
                             float alpha, CommandList& out, std::vector<PathVerb>& scratchVerbs,
                             std::vector<float>& scratchPoints);
/// T-332 (05 §5.9.8): the element's `clip-path` push (painter_element.cpp); see painter.cpp.
bool pushElementClipShape(const ElementData& data, const Mat2x3& world, CommandList& out,
                          std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints);
/// T-572 (18 §18.13): the element's overflow clip (painter_element.cpp); see painter.cpp.
void pushOverflowClip(const ElementData& data, const Mat2x3& world, CommandList& out,
                      std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints);

void sortedElementChildren(const NodeArena& arena, NodeId node, std::vector<NodeId>& out);

// Defined in painter.cpp: `n`'s own shape bounds in its own plane (no descendants); `hasShape` is
// false for a node kind that has none (Group/Mask/Camera, or a Path with no points).
Rect nodeOwnBounds(const NodeArena& arena, NodeId n, bool& hasShape);

Rect Painter::projectionBoundsHint(const NodeArena& arena, NodeId root, Rect viewport, bool* bounded) {
    if (bounded != nullptr) {
        *bounded = false;
    }
    Rect bounds{};
    if (!projectedSubtreeBounds(arena, root, bounds)) {
        return viewport;
    }
    if (viewport.w > 0.0f && viewport.h > 0.0f) {
        float x0 = std::max(bounds.x, viewport.x);
        float y0 = std::max(bounds.y, viewport.y);
        float x1 = std::min(bounds.x + bounds.w, viewport.x + viewport.w);
        float y1 = std::min(bounds.y + bounds.h, viewport.y + viewport.h);
        bounds = Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
        if (bounded != nullptr) {
            // T-795: a degenerate projection (no finite extent) is not a bound (as nestedSpaceReach()).
            *bounded = std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.w) &&
                       std::isfinite(bounds.h);
        }
    }
    return bounds;
}

bool Painter::projectedSubtreeBounds(const NodeArena& arena, NodeId root, Rect& bounds) {
    return projectedBoundsFrom(arena, root, mat4Identity(), /*startIsRoot=*/true, bounds);
}

bool Painter::projectedBoundsFrom(const NodeArena& arena, NodeId root, const Mat4& startPlane,
                                  bool startIsRoot, Rect& bounds) {
    // The walk mirrors updateWorldTransforms()'s plane context (world_transform.cpp): a node's
    // `world` is relative to its nearest enclosing 3D layer's plane (identity on the root's own
    // plane), which is exactly the mapping stack's planeWorld3 the emitted commands are drawn
    // under, so projecting each node's own shape corners with its own `world` reproduces the
    // screen extent of the commands this projection will contain.
    bool ok = true;
    bool any = false;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    float expand = 0.0f;
    Mat4 savedPlane = m_mappingStack.back().planeWorld3;

    m_boundsStack.clear();
    m_boundsStack.push_back({root, startPlane});
    while (ok && !m_boundsStack.empty()) {
        NodeId n = m_boundsStack.back().first;
        Mat4 plane = m_boundsStack.back().second;
        m_boundsStack.pop_back();
        const NodeCommon& c = arena.common(n);
        if (n != root) {
            if (c.visible == 0 || c.worldOpacity <= 0.0f || arena.kind(n) == NodeKind::Camera) {
                continue;  // Not painted; same skip as subtreeBounds().
            }
            if (c.worldLayerActive != 0) {
                continue;  // T-701 (33 §33.2): a world layer is drawn in a range of its own.
            }
            if (c.isSpaceRoot != 0) {
                // A nested space root paints under its own viewProj, which this mapping stack
                // does not yet carry; bounding it would mean running its camera setup here. Give
                // up and report the whole viewport, the conservative pre-T-105 answer.
                ok = false;
                break;
            }
            if (c.is3D != 0) {
                plane = c.world3;
            }
        }
        m_mappingStack.back().planeWorld3 = plane;

        // T-760: the root's own box lies on its own plane at the origin (emitRoot() draws it with the
        // identity inside its projection, whose rootXf is the root's `world`).
        // T-762: any other start node lies on `startPlane` through its own `world`.
        const Mat2x3 world = n == root && startIsRoot ? Mat2x3{} : c.world;
        bool hasShape = false;
        Rect local = nodeOwnBounds(arena, n, hasShape);
        if (hasShape) {
            Vec2 corners[4] = {
                Vec2{local.x, local.y},
                Vec2{local.x + local.w, local.y},
                Vec2{local.x, local.y + local.h},
                Vec2{local.x + local.w, local.y + local.h},
            };
            for (Vec2 corner : corners) {
                bool cornerOk = false;
                Vec2 sp = project3d::projectPoint(m_mappingStack, world, corner, cornerOk);
                if (!cornerOk) {
                    ok = false;  // §19.6 step 1: any corner behind the camera -> whole viewport.
                    break;
                }
                if (!any) {
                    minX = maxX = sp.x;
                    minY = maxY = sp.y;
                    any = true;
                } else {
                    minX = std::min(minX, sp.x);
                    maxX = std::max(maxX, sp.x);
                    minY = std::min(minY, sp.y);
                    maxY = std::max(maxY, sp.y);
                }
            }
        }
        if (!ok) {
            break;
        }

        if (c.effectCount > 0 && (n != root || startIsRoot)) {  // T-762: a group's own reach is its caller's
            // Per-node expansion in screen pixels, exactly as pushEffectCmd() computes it; the
            // hint is outset by the largest one in the subtree (§19.6 step 1).
            Mat2x3 xf = project3d::jacobianAt(m_mappingStack, world, c.anchor);
            const std::vector<EffectInstance>& pool = arena.effects();
            float sum = 0.0f;
            for (uint32_t i = 0; i < c.effectCount; ++i) {
                const EffectInstance& e = pool[c.effectOffset + i];
                if (e.enabled == 0) {
                    continue;
                }
                sum +=
                    e.kind == EffectKind::Custom
                        ? customEffectExpand(m_shaders != nullptr ? m_shaders->find(e.shader) : nullptr, xf)
                        : effectExpand(effectToScreen(e, xf));
            }
            expand = std::max(expand, sum);
        }

        arena.forEachChild(n, [&](NodeId child) { m_boundsStack.push_back({child, plane}); });
    }

    m_mappingStack.back().planeWorld3 = savedPlane;
    if (!ok || !any) {
        return false;
    }
    bounds = Rect{minX - expand, minY - expand, maxX - minX + 2 * expand, maxY - minY + 2 * expand};
    return true;
}

bool Painter::nestedSpaceReach(const NodeArena& arena, NodeId root, Rect& bounds) {
    // T-779: the screen extent emitRoot() will report for `root` (projectedSubtreeBounds(), its effects'
    // reach included), computed before the root is reached. The levels already open are taken on the
    // plane of their current layer, as the list's consumers have them when the root's PushProjection
    // runs (planeIsAffine()'s swap); a level without a known layer is not judged.
    if (!m_mappingStack.empty() && m_levelLayers.size() != m_mappingStack.size()) {
        return false;
    }
    const NodeCommon& c = arena.common(root);
    const CameraSetup setup = rootCameraSetup(arena, root);
    project3d::Mapping mapping;
    mapping.hasProjection = true;
    mapping.viewProj = setup.viewProj;
    mapping.rootXf = c.world;
    mapping.planeWorld3 = mat4Identity();
    swapInLevelPlanes(arena);
    m_mappingStack.push_back(mapping);
    bool ok = projectedSubtreeBounds(arena, root, bounds);
    m_mappingStack.pop_back();
    restoreLevelPlanes();
    // A degenerate projection (a camera setup or plane that yields no finite extent) is not a bound.
    ok = ok && std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.w) &&
         std::isfinite(bounds.h);
    return ok;
}

bool Painter::unionNestedSpaceReach(const NodeArena& arena, NodeId n, Rect& acc, bool& any) {
    bool ok = true;
    arena.forEachChild(n, [&](NodeId ch) {
        const NodeCommon& cc = arena.common(ch);
        if (!ok || (cc.isSpaceRoot == 0 && (m_deferredBelow[ch] & kBelowSpace) == 0)) {
            return;  // no space root in this child's subtree
        }
        if (cc.visible == 0 || cc.worldOpacity <= 0.0f || arena.kind(ch) == NodeKind::Camera) {
            return;  // not painted (enterNode()'s first test), nor anything below it
        }
        if (cc.isSpaceRoot != 0) {
            Rect bounds{};
            ok = nestedSpaceReach(arena, ch, bounds);
            if (ok) {
                // Same union as unionRect(): the first adopts.
                if (!any) {
                    acc = bounds;
                    any = true;
                } else {
                    const float x0 = std::min(acc.x, bounds.x);
                    const float y0 = std::min(acc.y, bounds.y);
                    const float x1 = std::max(acc.x + acc.w, bounds.x + bounds.w);
                    const float y1 = std::max(acc.y + acc.h, bounds.y + bounds.h);
                    acc = Rect{x0, y0, x1 - x0, y1 - y0};
                }
            }
            return;
        }
        if (cc.is3D != 0 && !m_mappingStack.empty()) {
            ok = false;  // a 3D layer of the space being painted: its own plane (not met when alone)
            return;
        }
        ok = unionNestedSpaceReach(arena, ch, acc, any);
    });
    return ok;
}

void Painter::swapInLevelPlanes(const NodeArena& arena) {
    m_planeSaveScratch.clear();
    for (size_t i = 0; i < m_mappingStack.size(); ++i) {
        m_planeSaveScratch.push_back(m_mappingStack[i].planeWorld3);
        if (m_levelLayers[i] != kInvalidNode) {
            m_mappingStack[i].planeWorld3 = arena.common(m_levelLayers[i]).world3;
        }
    }
}

void Painter::restoreLevelPlanes() {
    for (size_t i = 0; i < m_planeSaveScratch.size(); ++i) {
        m_mappingStack[i].planeWorld3 = m_planeSaveScratch[i];
    }
}

bool Painter::planeIsAffine(const NodeArena& arena, const CommandList& out) {
    const size_t depth = m_mappingStack.size();
    if (depth <= m_layerWrapDepth) {
        return true;  // no 3D space (T-701: a world layer's wrapper is a translation)
    }
    if (m_levelLayers.size() != depth) {
        return false;  // an entry pushed by something else than emitRoot(): its plane is not known
    }
    PlaneMemo& memo = m_planeMemo;
    if (memo.valid && memo.serial == m_spaceSerial && memo.depth == depth &&
        memo.layer == m_levelLayers.back()) {
        return memo.affine;
    }
    // The viewport and tolerance of the list's consumers (project3d::planeTest). In world-layer output
    // every range is built at a pixel ratio of its own and a layer's range is painted with its
    // rectangle as the list's viewport, so only a plane with a constant w is taken there.
    const bool known = !arena.worldLayerState().output;
    const project3d::PlaneTest test =
        project3d::planeTest(known ? out.viewport : Rect{}, project3d::kUnboundedPlaneViewport,
                             known ? out.devicePixelRatio : 1.0f);
    // The entries hold their roots' own planes; each one's current layer's world3 is swapped in around
    // the queries (as pushEffectCmd() does for the top one). The consumers judge a level when its plane
    // is set and never take a level under a projected one, hence every prefix of the stack.
    swapInLevelPlanes(arena);
    bool affine = true;
    Mat2x3 fold;
    for (size_t d = 1; affine && d <= depth; ++d) {
        affine = project3d::stackPlaneAffine(std::span<const project3d::Mapping>(m_mappingStack.data(), d),
                                             test, fold);
    }
    restoreLevelPlanes();
    memo.valid = true;
    memo.affine = affine;
    memo.serial = m_spaceSerial;
    memo.depth = depth;
    memo.layer = m_levelLayers.back();
    return affine;
}

bool Painter::enterEmbeddedLayer(const NodeArena& arena, NodeId n, CommandList& out) {
    // T-762 (19 §19.y.2): a 3D layer met while drawing a flat group's piece keeps its transform and
    // perspective but is drawn here, in document order: its own plane for its subtree, then back to
    // the plane it was met on. §19.6's backface test as flushSection() makes it for a sorted layer.
    const NodeCommon& c = arena.common(n);
    if (c.backfaceHidden != 0) {
        const Mat4 savedPlane = m_mappingStack.back().planeWorld3;
        m_mappingStack.back().planeWorld3 = c.world3;
        const bool backface = project3d::isBackface(m_mappingStack, Mat2x3{}, Vec2{c.anchor.x, c.anchor.y});
        m_mappingStack.back().planeWorld3 = savedPlane;
        if (backface) {
            return false;
        }
    }
    const NodeId outer = m_currentLayer;
    const Mat4 outerPlane = outer != kInvalidNode ? arena.common(outer).world3 : mat4Identity();
    m_stack.push_back(Action{Action::Kind::LeaveEmbeddedLayer, outer, 0.0f, MaskMode::Alpha, outerPlane});
    out.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
    const uint32_t idx = static_cast<uint32_t>(out.transforms3.size());
    out.transforms3.push_back(Transform3DCmd{c.world3});
    out.commands.push_back(Command{CmdKind::PushTransform3D, {}, idx});
    m_currentLayer = n;
    if (!m_levelLayers.empty()) {
        m_levelLayers.back() = n;  // T-716: the plane the subtree is judged on
    }
    return true;
}

bool Painter::holdsEmbeddedLayers(const NodeArena& arena, NodeId n) const {
    // Only a piece of a sorted segment can hold them (collectSegmentItems()); a node there that gets a
    // Layer or an effect is a group (layer_collect::flattens3D()), so any 3D layer below it is embedded.
    if (m_piece == layer_collect::kNoPiece || m_mappingStack.empty()) {
        return false;
    }
    bool any = false;
    arena.forEachChild(n, [&](NodeId ch) {
        const NodeCommon& cc = arena.common(ch);
        any = any || (cc.isSpaceRoot == 0 && cc.worldLayerActive == 0 && m_has3D[ch] != 0);
    });
    return any;
}

Rect Painter::embeddedSubtreeBounds(const NodeArena& arena, NodeId n, Rect viewport) {
    // T-762: projectedSubtreeBounds()'s walk from `n` on the plane it is drawn on (each embedded layer
    // switching to its own), the outer levels on their current layers' planes; the viewport when the
    // extent is not known (a nested space root, a corner behind the eye, no finite result).
    const Mat4 plane = m_currentLayer != kInvalidNode ? arena.common(m_currentLayer).world3 : mat4Identity();
    const bool swap = m_levelLayers.size() == m_mappingStack.size();
    if (swap) {
        swapInLevelPlanes(arena);
    }
    Rect bounds{};
    bool ok = projectedBoundsFrom(arena, n, plane, /*startIsRoot=*/false, bounds);
    if (swap) {
        restoreLevelPlanes();
    }
    ok = ok && std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.w) &&
         std::isfinite(bounds.h);
    return ok ? bounds : viewport;
}

void Painter::flushSection(const NodeArena& arena, std::span<const NodeId> orderedChildren,
                           const layer_collect::Section& section, const Mat4& view, float alpha) {
    // T-727 (19 §19.x ruling 1, amending §19.6 step 3): collect every 3D layer across the segment's
    // sibling subtrees together with the segment's flat content -- one item per run of a child's
    // flat content between two of its layers, on the root plane -- (docOrder is shared across the
    // whole segment, so ties break by segment-wide document order, not per-child), then depth-sort
    // (farthest first) and backface-cull the layers. A flat item is drawn by EnterNode2D with its
    // piece: enterNode() skips what belongs to other pieces and, via the is3D-skip branch
    // (m_currentLayer == kInvalidNode), the layers, which are items of their own.
    // T-762 (19 §19.y.2): a flat group holds its 3D descendants; they are no items of their own and
    // are drawn in place while its piece is drawn (enterEmbeddedLayer()).
    m_layerScratch.clear();
    if (m_pieceRanges.size() < arena.capacity()) {
        m_pieceRanges.resize(arena.capacity());
    }
    uint32_t docOrder = 0;
    for (uint32_t i = 0; i < section.childCount; ++i) {
        NodeId child = orderedChildren[section.firstChild + i];
        layer_collect::collectSegmentItems(arena, child, view, m_layerScratch, docOrder, m_pieceRanges);
    }
    layer_collect::sortLayers(m_layerScratch);

    for (const layer_collect::LayerEntry& layer : m_layerScratch) {
        if (layer.piece != layer_collect::kLayerItem) {
            m_planScratch.push_back(
                Action{Action::Kind::PushTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
            m_planScratch.push_back(Action{Action::Kind::EnterNode2D, layer.node, alpha, MaskMode::Alpha,
                                           mat4Identity(), layer.piece});
            m_planScratch.push_back(
                Action{Action::Kind::PopTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
            continue;
        }
        const NodeCommon& lc = arena.common(layer.node);
        if (lc.backfaceHidden != 0) {
            // §19.6's backface test needs the full (outer-roots-included) mapping stack, with the
            // innermost (this root's) entry's planeWorld3 set to this specific layer's world3 --
            // m_mappingStack's top currently holds identity (the root's own plane) from
            // emitRoot(); swap it in and back out around the one call that needs it.
            Mat4 savedPlane = m_mappingStack.back().planeWorld3;
            m_mappingStack.back().planeWorld3 = lc.world3;
            Vec2 anchor{lc.anchor.x, lc.anchor.y};
            bool backface = project3d::isBackface(m_mappingStack, Mat2x3{}, anchor);
            m_mappingStack.back().planeWorld3 = savedPlane;
            if (backface) {
                continue;
            }
        }
        m_planScratch.push_back(
            Action{Action::Kind::PushTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, lc.world3});
        m_planScratch.push_back(Action{Action::Kind::EnterNodeLayer, layer.node, alpha * layer.inheritedAlpha,
                                       MaskMode::Alpha, mat4Identity()});
        m_planScratch.push_back(
            Action{Action::Kind::PopTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
    }
}

void Painter::emitRoot(const NodeArena& arena, NodeId root, float inheritedAlpha, CommandList& out) {
    const NodeCommon& c = arena.common(root);
    CameraSetup setup = rootCameraSetup(arena, root);
    Rect box = spaceRootBoxOf(arena, root);

    // Docs/tasks/T-48.md "T-44b 遺留修正" 1: the root's own opacity < 1 (or a non-Normal blend)
    // wraps the whole PushProjection..PopProjection run in a PushLayer/PopLayer pair -- like
    // enterNode()'s needsLayer branch -- so the root's alpha is composited once instead of being
    // folded into every descendant's own paint alpha (09.3's existing rule).
    // T-701: a world layer's own opacity is the host's to apply (ownOpacity()).
    const float opacity = ownOpacity(root, c);
    bool needsLayer = c.blend != static_cast<uint8_t>(BlendMode::Normal) || opacity < 1.0f;
    const uint32_t layerIdx = static_cast<uint32_t>(out.layers.size());  // (T-795: bounded below)
    if (needsLayer) {
        out.layers.push_back(LayerCmd{opacity, static_cast<BlendMode>(c.blend), out.viewport});
        out.commands.push_back(Command{CmdKind::PushLayer, {}, layerIdx});
    }

    ProjectionCmd proj{};
    proj.viewProj = setup.viewProj;
    proj.rootXf = c.world;
    proj.boxRect = box;
    proj.depthTest = 0;
    uint32_t projIdx = static_cast<uint32_t>(out.projections.size());
    out.projections.push_back(proj);
    out.commands.push_back(Command{CmdKind::PushProjection, {}, projIdx});

    project3d::Mapping mapping;
    mapping.hasProjection = true;
    mapping.viewProj = setup.viewProj;
    mapping.rootXf = c.world;
    mapping.planeWorld3 = mat4Identity();
    ++m_spaceSerial;  // T-716: planeIsAffine()'s memo is per push
    m_levelLayers.resize(m_mappingStack.size(), kInvalidNode);
    m_levelLayers.push_back(kInvalidNode);
    m_mappingStack.push_back(mapping);
    // T-727: this root may sit in a flat piece of an outer root; its PopProjection restores it.
    m_pieceSave.push_back(m_piece);
    m_piece = layer_collect::kNoPiece;

    // §19.6 step 1 (T-105): the hint needs this root's mapping-stack entry, so it is computed
    // after the push and written back into the command emitted above.
    // T-795 (10 §10.20, T-779 supplement): the root's own layer above holds only this projection, so
    // its window is the same extent when that is known (every corner in front of the eye, finite) --
    // not in the drag ghost, whose commands move afterwards; otherwise the whole viewport, unknown.
    bool bounded = false;
    out.projections[projIdx].boundsHint = projectionBoundsHint(arena, root, out.viewport, &bounded);
    if (needsLayer && bounded && !m_ghostPainting) {
        out.layers[layerIdx].boundsHint = out.projections[projIdx].boundsHint;
        out.layers[layerIdx].boundsKnown = 1;
    }

    // When needsLayer, the root's own opacity is folded into the PushLayer above instead of into
    // `alpha` (matching enterNode()'s needsLayer branch); otherwise it is folded into `alpha` as
    // before (pushPlan()'s non-layer path).
    float alpha = needsLayer ? inheritedAlpha : inheritedAlpha * opacity;

    NodeKind kind = arena.kind(root);
    bool clip = false;
    bool shapeClip = false;
    if (kind == NodeKind::Element) {
        const ElementData& data = arena.element(root);
        // T-760 (19 §19.5 Painter step 4): the root's own box is root-plane content, drawn inside its
        // projection whose rootXf is already c.world -- so its transform to that plane is the
        // identity (the plane's origin is the root's box), like its children's. Drawing it with
        // c.world here put it at twice the root's offset.
        const Mat2x3 own{};
        // T-332: the root's `clip-path`, before its background (as paintElement()).
        shapeClip = pushElementClipShape(data, own, out, m_elemVerbsScratch, m_elemPointsScratch);
        // Root's own background/border/image (T-27 rule 1), output directly -- its children are
        // handled by the sectioned loop below instead of pushPlan().
        if (data.style.visibility != Visibility::Hidden) {
            if (emitElementBackground(data, own, alpha, out, m_elemVerbsScratch, m_elemPointsScratch)) {
                m_backgroundTilesCapped = true;  // T-327: Document warns once.
            }
            if (data.style.borderImage != kInvalidAsset) {
                emitElementBorderImage(data, own, alpha, out);  // T-199: replaces the plain border.
            } else {
                emitElementBorder(data, own, alpha, out, m_elemVerbsScratch, m_elemPointsScratch, true,
                                  m_borderDashCache.get());
            }
            if (data.externalNode == kInvalidNode ||
                !emitExternalPlaceholder(arena.external(data.externalNode), data, own, alpha, out,
                                         m_elemVerbsScratch, m_elemPointsScratch)) {
                emitElementImage(data, own, alpha, out);  // T-191: or its placeholder.
            }
        }
        clip = clipsOverflow(data);  // T-336.
        if (clip) {
            pushOverflowClip(data, own, out, m_elemVerbsScratch, m_elemPointsScratch);  // T-572.
        }
    }

    // §19.4: the root's own chosen camera child (if any) is not itself scene content -- excluded
    // from both sectioning and painting (it never reaches enterNode() as a section child, so the
    // "no root-plane 2D content" case emits nothing for it, not an empty PushTransform3D pair).
    NodeId cam = findCameraChild(arena, root);
    sortedElementChildren(arena, root, m_childScratch);
    if (cam != kInvalidNode) {
        m_childScratch.erase(std::remove(m_childScratch.begin(), m_childScratch.end(), cam),
                             m_childScratch.end());
    }

    layer_collect::collectSections(arena, m_childScratch, m_has3D, m_sectionScratch);

    m_planScratch.clear();
    for (const layer_collect::Section& section : m_sectionScratch) {
        if (section.sorted) {
            flushSection(arena, m_childScratch, section, setup.view, alpha);
            continue;
        }
        // A plain (all-2D, no is3D anywhere inside) section is always exactly one child (see
        // layer_collect::collectSections); ordinary enterNode() suffices (nothing to skip), but
        // EnterNode2D (which also resets m_currentLayer) keeps this uniform with flushSection()'s
        // own 2D-content quanta.
        NodeId child = m_childScratch[section.firstChild];
        m_planScratch.push_back(
            Action{Action::Kind::PushTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
        m_planScratch.push_back(
            Action{Action::Kind::EnterNode2D, child, alpha, MaskMode::Alpha, mat4Identity()});
        m_planScratch.push_back(
            Action{Action::Kind::PopTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
    }

    if (clip) {
        m_planScratch.push_back(
            Action{Action::Kind::PopClip, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
    }
    if (shapeClip) {  // T-332: the clip-path pops after the overflow clip.
        m_planScratch.push_back(
            Action{Action::Kind::PopClip, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
    }
    m_planScratch.push_back(
        Action{Action::Kind::PopProjection, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
    if (needsLayer) {
        m_planScratch.push_back(
            Action{Action::Kind::PopLayer, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
    }

    // Push in reverse so m_planScratch[0] ends on top of m_stack (pops, i.e. executes, first) --
    // same pattern as pushPlan().
    for (size_t k = m_planScratch.size(); k > 0; --k) {
        m_stack.push_back(m_planScratch[k - 1]);
    }
}

}  // namespace xt
