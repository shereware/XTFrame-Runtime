#include <algorithm>
#include <cmath>

#include "camera.h"
#include "painter_action.h"
#include "xt/effect_utils.h"
#include "xt/painter.h"
#include "xt/project3d.h"

/// Painter in world-layer output (Docs/design/33-world-layers.md §33.2-§33.5, Docs/tasks/T-701.md):
/// the list becomes one self-contained command range per layer (CommandList::worldLayers).
///
/// The base layer's range is the ordinary traversal from the root; Painter::enterNode() queues
/// every world layer it meets (NodeCommon::worldLayerActive, set by updateWorldTransforms())
/// instead of painting it. Each queued layer then gets a range of its own: its content is in the
/// layer plane's coordinates already (the transform pass made the layer's box a plane origin), so
/// the range only needs a wrapper projection that moves the layer's rectangle to (0, 0) -- an
/// affine plane, drawn through the ordinary 2D paths (10 §10.15) -- and the list's viewport set to
/// the rectangle's size while it is emitted, so every bounds hint is clamped to the layer's own
/// target instead of the screen.

namespace xt {

// Defined in painter.cpp.
Rect nodeOwnBounds(const NodeArena& arena, NodeId n, bool& hasShape);

namespace {

/// What projectionBoundsHint() is handed as its "viewport" here: large enough never to clip, and
/// recognizable when it comes back as the "cannot be bounded" answer.
constexpr float kUnboundedExtent = 1.0e7f;

void uniteRect(Rect& acc, bool& any, const Rect& r) {
    if (!any) {
        acc = r;
        any = true;
        return;
    }
    const float x0 = std::min(acc.x, r.x);
    const float y0 = std::min(acc.y, r.y);
    const float x1 = std::max(acc.x + acc.w, r.x + r.w);
    const float y1 = std::max(acc.y + acc.h, r.y + r.h);
    acc = Rect{x0, y0, x1 - x0, y1 - y0};
}

/// T-703 (33 §33.13 item 1, revising §33.4 item 2): the largest a layer's rectangle may be, as a
/// multiple of the root box's width / height. Checked on the layer's own plane only, so it does not
/// depend on how the layer is moved, tilted or turned.
constexpr float kWorldLayerMaxRootMultiple = 3.0f;

/// Cuts `lo` .. `lo + size` to at most `limit` about its centre; true when it was cut.
bool cutAboutCentre(float& lo, float& size, float limit) {
    if (!(size > limit)) {
        return false;
    }
    lo += 0.5f * (size - limit);
    size = limit;
    return true;
}

}  // namespace

Rect Painter::worldLayerBounds(const NodeArena& arena, NodeId layer, bool& any, bool& unbounded) {
    any = false;
    unbounded = false;
    Rect acc{};
    m_layerBoundsStack.clear();
    m_layerBoundsStack.emplace_back(layer, 0.0f);
    const std::vector<EffectInstance>& pool = arena.effects();
    while (!m_layerBoundsStack.empty()) {
        const NodeId n = m_layerBoundsStack.back().first;
        float expand = m_layerBoundsStack.back().second;
        m_layerBoundsStack.pop_back();
        if (arena.kind(n) == NodeKind::Camera) {
            continue;
        }
        const NodeCommon& c = arena.common(n);
        if (n != layer && (c.visible == 0 || c.worldOpacity <= 0.0f || c.worldLayerActive != 0)) {
            continue;  // not painted, or a layer of its own
        }
        // The node's effects grow what it and its subtree draw (18 §18.5), as pushEffectCmd() does.
        for (uint32_t i = 0; i < c.effectCount; ++i) {
            const EffectInstance& e = pool[c.effectOffset + i];
            if (e.enabled == 0) {
                continue;
            }
            expand +=
                e.kind == EffectKind::Custom
                    ? customEffectExpand(m_shaders != nullptr ? m_shaders->find(e.shader) : nullptr, c.world)
                    : effectExpand(effectToScreen(e, c.world));
        }
        bool hasShape = false;
        const Rect own = nodeOwnBounds(arena, n, hasShape);
        if (hasShape) {
            const Rect r = transformBounds(c.world, own);
            uniteRect(acc, any, Rect{r.x - expand, r.y - expand, r.w + 2 * expand, r.h + 2 * expand});
        }
        if (c.isSpaceRoot != 0) {
            // A 3D space inside the layer (or the layer itself, flattening its 3D descendants):
            // the projected extent of its subtree, as the PushProjection's own bounds hint.
            const CameraSetup setup = rootCameraSetup(arena, n);
            project3d::Mapping mapping;
            mapping.hasProjection = true;
            mapping.viewProj = setup.viewProj;
            mapping.rootXf = c.world;
            mapping.planeWorld3 = mat4Identity();
            m_mappingStack.push_back(mapping);
            const Rect farBox{-kUnboundedExtent, -kUnboundedExtent, 2 * kUnboundedExtent,
                              2 * kUnboundedExtent};
            const Rect hint = projectionBoundsHint(arena, n, farBox);
            m_mappingStack.pop_back();
            if (hint.w >= 2 * kUnboundedExtent || hint.h >= 2 * kUnboundedExtent) {
                unbounded = true;
            } else if (hint.w > 0.0f && hint.h > 0.0f) {
                uniteRect(acc, any,
                          Rect{hint.x - expand, hint.y - expand, hint.w + 2 * expand, hint.h + 2 * expand});
            }
            continue;
        }
        arena.forEachChild(n, [&](NodeId child) { m_layerBoundsStack.emplace_back(child, expand); });
    }
    return acc;
}

bool Painter::paintWorldLayer(const NodeArena& arena, size_t index, uint32_t order, CommandList& out) {
    const NodeId node = m_layerQueue[index].node;
    if (!arena.alive(node) || m_drawableCount[node] == 0) {
        return false;
    }
    const NodeCommon& c = arena.common(node);
    bool any = false;
    bool unbounded = false;
    Rect bounds = worldLayerBounds(arena, node, any, unbounded);
    if (!any && !unbounded) {
        return false;
    }
    // 33 §33.4 item 2: one logical px of slack for antialiased edges. T-703 (33 §33.13 item 1): then
    // the size limit on the layer's own plane -- each side at most three times the root box's,
    // cut about the content's centre; `world3` plays no part.
    bounds = Rect{bounds.x - 1.0f, bounds.y - 1.0f, bounds.w + 2.0f, bounds.h + 2.0f};
    const Rect viewport = out.viewport;
    const float maxW = kWorldLayerMaxRootMultiple * viewport.w;
    const float maxH = kWorldLayerMaxRootMultiple * viewport.h;
    if (unbounded) {
        // Content that cannot be bounded: the largest rectangle allowed, about the centre of the
        // bounded part (or of the layer's own box when there is none).
        const LayoutBox& box = arena.element(node).box;
        const Rect around = any ? bounds : Rect{0, 0, box.width, box.height};
        bounds = Rect{around.x + 0.5f * (around.w - maxW), around.y + 0.5f * (around.h - maxH), maxW, maxH};
        m_worldLayerClamped = true;
    } else {
        const bool cutW = cutAboutCentre(bounds.x, bounds.w, maxW);
        const bool cutH = cutAboutCentre(bounds.y, bounds.h, maxH);
        if (cutW || cutH) {
            m_worldLayerClamped = true;
        }
    }
    if (!(bounds.w > 0.0f) || !(bounds.h > 0.0f)) {
        return false;
    }
    // Outward to the step grid, so small movements of the content keep the target's size.
    const float step = kWorldLayerRectStep;
    const float x0 = std::floor(bounds.x / step) * step;
    const float y0 = std::floor(bounds.y / step) * step;
    const float x1 = std::ceil((bounds.x + bounds.w) / step) * step;
    const float y1 = std::ceil((bounds.y + bounds.h) / step) * step;
    Rect rect{x0, y0, x1 - x0, y1 - y0};
    // T-706 (33 §33.14 item 1): hysteresis. The previous frame's rectangle stays while the content
    // lies wholly inside it and it is no more than two steps larger than the new one on each side
    // (width, height); the rectangle so changes only when the content grows or shrinks past that.
    const uint32_t generation = arena.generation(node);
    if (m_layerRects != nullptr) {
        for (const NodeArena::WorldLayerState::LayerRect& previous : *m_layerRects) {
            if (previous.id != node || previous.generation != generation) {
                continue;
            }
            const Rect& p = previous.rect;
            const bool inside = bounds.x >= p.x && bounds.y >= p.y && bounds.x + bounds.w <= p.x + p.w &&
                                bounds.y + bounds.h <= p.y + p.h;
            if (inside && p.w - rect.w <= 2.0f * step && p.h - rect.h <= 2.0f * step) {
                rect = p;
            }
            break;
        }
        m_layerRectsNext.push_back(NodeArena::WorldLayerState::LayerRect{node, generation, rect});
    }

    const uint32_t first = static_cast<uint32_t>(out.commands.size());
    // The range's own frame: logical (0, 0) is the rectangle's corner, the viewport its size.
    out.viewport = Rect{0, 0, rect.w, rect.h};
    ProjectionCmd proj{};
    proj.viewProj = mat4Identity();
    proj.rootXf = translation(Vec2{-rect.x, -rect.y});
    proj.boxRect = rect;
    proj.depthTest = 0;
    proj.boundsHint = out.viewport;
    const uint32_t projIdx = static_cast<uint32_t>(out.projections.size());
    out.projections.push_back(proj);
    out.commands.push_back(Command{CmdKind::PushProjection, {}, projIdx});
    project3d::Mapping mapping;
    mapping.hasProjection = true;
    mapping.viewProj = proj.viewProj;
    mapping.rootXf = proj.rootXf;
    mapping.planeWorld3 = mat4Identity();
    m_mappingStack.push_back(mapping);
    m_layerWrapDepth = 1;

    m_layerEntering = node;
    m_currentLayer = kInvalidNode;
    m_layerInsertPos = index + 1;  // layers met inside this one come right after it
    m_stack.push_back(Action{Action::Kind::EnterNode, node, 1.0f, MaskMode::Alpha});
    runStack(arena, out);
    m_layerEntering = kInvalidNode;

    m_mappingStack.pop_back();
    m_layerWrapDepth = 0;
    out.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
    out.viewport = viewport;

    WorldLayerRange range;
    range.id = node;
    range.order = order;
    range.firstCommand = first;
    range.commandCount = static_cast<uint32_t>(out.commands.size()) - first;
    range.world3 = c.world3;
    range.rect = rect;
    range.opacity = c.worldOpacity;
    range.backfaceHidden = c.backfaceHidden;
    range.source = m_source;
    out.worldLayers.push_back(range);
    return true;
}

void Painter::paintWorldLayers(const NodeArena& arena, NodeId root, CommandList& out) {
    // The base layer: everything no switch covers, painted exactly as outside world-layer output
    // (no wrapper: its rectangle is the viewport).
    const uint32_t first = static_cast<uint32_t>(out.commands.size());
    m_layerInsertPos = 0;
    m_stack.push_back(Action{Action::Kind::EnterNode, root, 1.0f, MaskMode::Alpha});
    runStack(arena, out);
    paintOverlays(arena, out);
    paintGhost(arena, out);
    WorldLayerRange base;
    base.id = 0;
    base.order = 0;
    base.firstCommand = first;
    base.commandCount = static_cast<uint32_t>(out.commands.size()) - first;
    base.rect = out.viewport;
    base.source = m_source;
    out.worldLayers.push_back(base);

    // The queue grows while it is walked: a layer met inside layer i is inserted at i + 1.
    m_layerRectsNext.clear();
    uint32_t order = 1;
    for (size_t i = 0; i < m_layerQueue.size(); ++i) {
        if (paintWorldLayer(arena, i, order, out)) {
            ++order;
        }
    }
    // T-706: this frame's rectangles become the previous ones; a layer not emitted is dropped.
    // (Swapped, not copied: both vectors keep their storage from frame to frame.)
    if (m_layerRects != nullptr) {
        m_layerRects->swap(m_layerRectsNext);
    }
}

}  // namespace xt
