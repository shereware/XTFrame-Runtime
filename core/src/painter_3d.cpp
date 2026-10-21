#include <algorithm>
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

/// Painter::emitRoot / Painter::flushSection: entering a 3D space root (docs/design/19-3d-space.md
/// §19.5 "Painter" and §19.6 "深度排序與背面"). See docs/tasks/T-44b.md.
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
void emitElementBorder(const ElementData& data, const Mat2x3& world, float alpha, CommandList& out,
                       std::vector<PathVerb>& scratchVerbs, std::vector<float>& scratchPoints,
                       bool allowSnap = true);
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

void sortedElementChildren(const NodeArena& arena, NodeId node, std::vector<NodeId>& out);

// Defined in painter.cpp: `n`'s own shape bounds in its own plane (no descendants); `hasShape` is
// false for a node kind that has none (Group/Mask/Camera, or a Path with no points).
Rect nodeOwnBounds(const NodeArena& arena, NodeId n, bool& hasShape);

Rect Painter::projectionBoundsHint(const NodeArena& arena, NodeId root, Rect viewport) {
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
    m_boundsStack.push_back({root, mat4Identity()});
    while (ok && !m_boundsStack.empty()) {
        NodeId n = m_boundsStack.back().first;
        Mat4 plane = m_boundsStack.back().second;
        m_boundsStack.pop_back();
        const NodeCommon& c = arena.common(n);
        if (n != root) {
            if (c.visible == 0 || c.worldOpacity <= 0.0f || arena.kind(n) == NodeKind::Camera) {
                continue;  // Not painted; same skip as subtreeBounds().
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
                Vec2 sp = project3d::projectPoint(m_mappingStack, c.world, corner, cornerOk);
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

        if (c.effectCount > 0) {
            // Per-node expansion in screen pixels, exactly as pushEffectCmd() computes it; the
            // hint is outset by the largest one in the subtree (§19.6 step 1).
            Mat2x3 xf = project3d::jacobianAt(m_mappingStack, c.world, c.anchor);
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
        return viewport;
    }

    Rect bounds{minX - expand, minY - expand, maxX - minX + 2 * expand, maxY - minY + 2 * expand};
    if (viewport.w > 0.0f && viewport.h > 0.0f) {
        float x0 = std::max(bounds.x, viewport.x);
        float y0 = std::max(bounds.y, viewport.y);
        float x1 = std::min(bounds.x + bounds.w, viewport.x + viewport.w);
        float y1 = std::min(bounds.y + bounds.h, viewport.y + viewport.h);
        bounds = Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    return bounds;
}

void Painter::flushSection(const NodeArena& arena, std::span<const NodeId> orderedChildren,
                           const layer_collect::Section& section, const Mat4& view, float alpha) {
    // §19.6 step 3: the segment's own-plane 2D content is output before its layers. A child is
    // "own-plane content" here iff it is not itself a 3D layer; a non-is3D child may still have
    // is3D descendants deeper down (that's why this section is "sorted" at all) -- enterNode()'s
    // is3D-skip branch (via the EnterNode2D action, m_currentLayer == kInvalidNode) leaves those
    // to the layer list below instead of painting them here.
    for (uint32_t i = 0; i < section.childCount; ++i) {
        NodeId child = orderedChildren[section.firstChild + i];
        if (arena.common(child).is3D != 0) {
            continue;
        }
        m_planScratch.push_back(
            Action{Action::Kind::PushTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
        m_planScratch.push_back(
            Action{Action::Kind::EnterNode2D, child, alpha, MaskMode::Alpha, mat4Identity()});
        m_planScratch.push_back(
            Action{Action::Kind::PopTransform3D, kInvalidNode, 0.0f, MaskMode::Alpha, mat4Identity()});
    }

    // Collect every 3D layer across the segment's sibling subtrees (docOrder is shared across the
    // whole segment, per collectLayers()'s contract, so ties break by segment-wide document order,
    // not per-child), then depth-sort (farthest first) and backface-cull.
    m_layerScratch.clear();
    uint32_t docOrder = 0;
    for (uint32_t i = 0; i < section.childCount; ++i) {
        NodeId child = orderedChildren[section.firstChild + i];
        layer_collect::collectLayers(arena, child, view, m_layerScratch, docOrder);
    }
    layer_collect::sortLayers(m_layerScratch);

    for (const layer_collect::LayerEntry& layer : m_layerScratch) {
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

    // docs/tasks/T-48.md "T-44b 遺留修正" 1: the root's own opacity < 1 (or a non-Normal blend)
    // wraps the whole PushProjection..PopProjection run in a PushLayer/PopLayer pair -- like
    // enterNode()'s needsLayer branch -- so the root's alpha is composited once instead of being
    // folded into every descendant's own paint alpha (09.3's existing rule).
    bool needsLayer = c.blend != static_cast<uint8_t>(BlendMode::Normal) || c.opacity < 1.0f;
    if (needsLayer) {
        uint32_t layerIdx = static_cast<uint32_t>(out.layers.size());
        out.layers.push_back(LayerCmd{c.opacity, static_cast<BlendMode>(c.blend), out.viewport});
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
    m_mappingStack.push_back(mapping);

    // §19.6 step 1 (T-105): the hint needs this root's mapping-stack entry, so it is computed
    // after the push and written back into the command emitted above.
    out.projections[projIdx].boundsHint = projectionBoundsHint(arena, root, out.viewport);

    // When needsLayer, the root's own opacity is folded into the PushLayer above instead of into
    // `alpha` (matching enterNode()'s needsLayer branch); otherwise it is folded into `alpha` as
    // before (pushPlan()'s non-layer path).
    float alpha = needsLayer ? inheritedAlpha : inheritedAlpha * c.opacity;

    NodeKind kind = arena.kind(root);
    bool clip = false;
    bool shapeClip = false;
    if (kind == NodeKind::Element) {
        const ElementData& data = arena.element(root);
        // T-332: the root's `clip-path`, before its background (as paintElement()).
        shapeClip = pushElementClipShape(data, c.world, out, m_elemVerbsScratch, m_elemPointsScratch);
        // Root's own background/border/image (T-27 rule 1), output directly -- its children are
        // handled by the sectioned loop below instead of pushPlan().
        if (data.style.visibility != Visibility::Hidden) {
            if (emitElementBackground(data, c.world, alpha, out, m_elemVerbsScratch, m_elemPointsScratch)) {
                m_backgroundTilesCapped = true;  // T-327: Document warns once.
            }
            if (data.style.borderImage != kInvalidAsset) {
                emitElementBorderImage(data, c.world, alpha, out);  // T-199: replaces the plain border.
            } else {
                emitElementBorder(data, c.world, alpha, out, m_elemVerbsScratch, m_elemPointsScratch);
            }
            if (data.externalNode == kInvalidNode ||
                !emitExternalPlaceholder(arena.external(data.externalNode), data, c.world, alpha, out,
                                         m_elemVerbsScratch, m_elemPointsScratch)) {
                emitElementImage(data, c.world, alpha, out);  // T-191: or its placeholder.
            }
        }
        clip = clipsOverflow(data);  // T-336.
        if (clip) {
            uint32_t clipIdx = static_cast<uint32_t>(out.clipRects.size());
            out.clipRects.push_back(ClipRectCmd{c.world, data.box.paddingBox()});
            out.commands.push_back(Command{CmdKind::PushClipRect, {}, clipIdx});
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
