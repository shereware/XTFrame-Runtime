#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "env_flag.h"
#include "layout_common.h"
#include "xt/assert.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/perf.h"
#include "xt/property.h"

/// T-755 (Docs/design/06-layout.md §6.x "排版邊界與局部重排", revising §6.3): the layout boundary
/// predicate (item 1), LayoutEngine::layout()'s choice between the full pass and laying out only the
/// subtrees of the layout roots NodeArena::markDirty() recorded (items 2-3), and the verify mode
/// that compares every partial pass with a full one (item 5). See Docs/tasks/T-755.md.

namespace xt {

namespace {

/// 06 §6.x item 1: `len` (a width or a height) fixes the size whatever the content -- a length (a
/// `calc()` without a percentage included), or a percentage (or a `calc()` with one) when
/// `percentDefinite` (the containing block's matching size has that property).
bool definiteSize(Length len, bool percentDefinite) {
    if (len.unit == LengthUnit::Auto) {
        return false;
    }
    return lengthUsesPercent(len) ? percentDefinite : true;
}

/// Whether a full pass lays `node` out (an Element that is not `display: none`, a `<camera>` or a
/// `<repeat>` template; LayoutEngine::layoutChild()'s and the passes' own tests).
bool laidOut(const NodeArena& arena, NodeId node) {
    if (arena.kind(node) != NodeKind::Element) {
        return false;
    }
    const ElementData& d = arena.element(node);
    return d.style.display != Display::None && d.cameraNode == kInvalidNode &&
           (arena.common(node).flags2 & kTemplate) == 0;
}

constexpr uint8_t kDefiniteBoth =
    static_cast<uint8_t>(kLayoutDefiniteW) | static_cast<uint8_t>(kLayoutDefiniteH);

// The verify mode's field names (VerifyRecord::v), per record kind.
constexpr const char* kElementFields[34] = {"x",
                                            "y",
                                            "width",
                                            "height",
                                            "margin.top",
                                            "margin.right",
                                            "margin.bottom",
                                            "margin.left",
                                            "padding.top",
                                            "padding.right",
                                            "padding.bottom",
                                            "padding.left",
                                            "border.top",
                                            "border.right",
                                            "border.bottom",
                                            "border.left",
                                            "intrinsicWidth",
                                            "intrinsicHeight",
                                            "scrollWidth",
                                            "scrollHeight",
                                            "translate.x",
                                            "translate.y",
                                            "anchor.x",
                                            "anchor.y",
                                            "visible",
                                            "perspective",
                                            "perspectiveOrigin.x",
                                            "perspectiveOrigin.y",
                                            "sceneScale.x",
                                            "sceneScale.y",
                                            "sceneTranslate.x",
                                            "sceneTranslate.y",
                                            "",
                                            ""};
constexpr const char* kTextFields[34] = {"width",       "height",      "intrinsicMain", "lines",
                                         "translate.x", "translate.y", "visible"};

}  // namespace

LayoutEngine::LayoutEngine() {
    if (envFlag("XT_PARTIAL_LAYOUT") == '0') {
        m_partialEnabled = false;
    }
    if (envFlag("XT_VERIFY_VISUAL_GENERATION") == '1') {
        m_verifyPartial = true;
        m_verifyAbort = true;
    }
}

bool LayoutEngine::isLayoutBoundary(const ElementData& data, uint8_t flags, bool flexItem, bool gridItem) {
    const ComputedStyle& style = data.style;
    if ((flags & kDefiniteBoth) != kDefiniteBoth) {
        return false;
    }
    // T-782: `overflow` takes no part. A scroll extent comes from an element's direct children's
    // boxes only (storeScrollSize()), and a boundary's box does not depend on its content, so what
    // overflows it changes no ancestor's scroll extent.
    if (flexItem) {
        // First version (06 §6.x item 1): only `flex: none` or its equivalent -- the flexible
        // lengths then never depend on the line's free space, nor the basis on the content.
        const Length basis = style.base[prop::FlexBasis].len;
        if (style.base[prop::FlexGrow].f != 0.0f || style.base[prop::FlexShrink].f != 0.0f ||
            (basis.unit != LengthUnit::Auto && lengthUsesPercent(basis))) {
            return false;
        }
    }
    if (gridItem && (flags & kLayoutGridFixedTracks) == 0) {
        return false;
    }
    return true;
}

void LayoutEngine::updateInFlowLayoutFlags(NodeArena& arena, NodeId node) {
    ElementData& data = arena.element(node);
    if (!laidOut(arena, node)) {
        data.layoutFlags = 0;
        return;
    }
    const NodeId parent = arena.parent(node);
    uint8_t parentFlags = 0;
    Display parentDisplay = Display::Block;
    if (parent != kInvalidNode && arena.kind(parent) == NodeKind::Element) {
        parentFlags = arena.element(parent).layoutFlags;
        parentDisplay = arena.element(parent).style.display;
    }
    const bool flexItem = parentDisplay == Display::Flex;
    const bool gridItem = parentDisplay == Display::Grid;
    // A percentage counts only against a block container's definite size: a flex or grid item's
    // percentages resolve against a line or an area that the first version does not judge.
    const bool blockParent = !flexItem && !gridItem;
    uint8_t flags = static_cast<uint8_t>(data.layoutFlags & kLayoutGridFixedTracks);
    if (!gridItem) {
        flags = 0;
    }
    if (definiteSize(data.style.base[prop::Width].len,
                     blockParent && (parentFlags & kLayoutDefiniteW) != 0)) {
        flags |= static_cast<uint8_t>(kLayoutDefiniteW);
    }
    if (definiteSize(data.style.base[prop::Height].len,
                     blockParent && (parentFlags & kLayoutDefiniteH) != 0)) {
        flags |= static_cast<uint8_t>(kLayoutDefiniteH);
    }
    if (isLayoutBoundary(data, flags, flexItem, gridItem)) {
        flags |= static_cast<uint8_t>(kLayoutBoundary);
    }
    data.layoutFlags = flags;
}

void LayoutEngine::updateAbsoluteLayoutFlags(NodeArena& arena, NodeId node) {
    ElementData& data = arena.element(node);
    const ComputedStyle& style = data.style;
    if (!laidOut(arena, node)) {
        data.layoutFlags = 0;
        return;
    }
    // 06 §6.x item 1: each axis by its size or by both insets. The containing block (a padding box
    // sized before the absolute pass runs, never by its absolutely positioned descendants) is
    // definite, so a percentage of it counts.
    uint8_t flags = 0;
    if (definiteSize(style.base[prop::Width].len, true) ||
        (style.base[prop::Width].len.unit == LengthUnit::Auto &&
         style.base[prop::Left].len.unit != LengthUnit::Auto &&
         style.base[prop::Right].len.unit != LengthUnit::Auto)) {
        flags |= static_cast<uint8_t>(kLayoutDefiniteW);
    }
    if (definiteSize(style.base[prop::Height].len, true) ||
        (style.base[prop::Height].len.unit == LengthUnit::Auto &&
         style.base[prop::Top].len.unit != LengthUnit::Auto &&
         style.base[prop::Bottom].len.unit != LengthUnit::Auto)) {
        flags |= static_cast<uint8_t>(kLayoutDefiniteH);
    }
    if (isLayoutBoundary(data, flags, false, false)) {
        flags |= static_cast<uint8_t>(kLayoutBoundary);
    }
    data.layoutFlags = flags;
}

void LayoutEngine::layout(NodeArena& arena, NodeId root, Rect viewport) {
    XT_ASSERT(m_ir != nullptr);
    // Docs/design/21-perf-tracing.md §21.1's laidOutElements; stays 0 on either early return below.
    m_laidOutElements = 0;
    m_lastLayoutRoots = 0;
    m_lastLayoutFull = false;
    if (root == kInvalidNode || !arena.alive(root) || arena.kind(root) != NodeKind::Element) {
        return;
    }
    bool full = (arena.dirty(root) & static_cast<uint8_t>(kLayoutRoot)) != 0;
    if (!full && arena.layoutRoots().empty()) {
        // T-782: no layout write since the last clearAllDirty() -- no root to look for (T-160).
        refreshDirtyTransforms(arena, root);
        return;
    }
    if (!full) {
        const bool partialOk = collectLayoutRoots(arena, root);
        if (partialOk && m_layoutRootScratch.empty()) {
            refreshDirtyTransforms(arena, root);  // Nothing to lay out (T-160).
            return;
        }
        full = !partialOk || !m_partialEnabled || !m_haveFullPass || root != m_fullRoot ||
               viewport.x != m_fullViewport.x || viewport.y != m_fullViewport.y ||
               viewport.w != m_fullViewport.w || viewport.h != m_fullViewport.h;
    }
    beginRedundantCount(arena);  // T-753: detail level >= 1 only
    if (full) {
        layoutFull(arena, root, viewport, true);
        endRedundantCount(arena);
        m_lastLayoutFull = true;
        ++m_counts.fullPasses;
        m_haveFullPass = true;
        m_fullRoot = root;
        m_fullViewport = viewport;
        return;
    }
    // A §6.7 input written outside the subtrees (an animated translate) is folded first; the
    // subtrees are folded again below with their new boxes.
    refreshDirtyTransforms(arena, root);
    for (NodeId sub : m_layoutRootScratch) {
        relayoutSubtree(arena, root, sub);
    }
    endRedundantCount(arena);  // T-753: before the verify mode's full pass
    m_lastLayoutRoots = static_cast<uint32_t>(m_layoutRootScratch.size());
    m_counts.layoutRoots += m_lastLayoutRoots;
    if (m_verifyPartial) {
        verifyAgainstFull(arena, root, viewport);
    }
}

void LayoutEngine::beginRedundantCount(const NodeArena& arena) {
    m_trackLaidOut = m_perf != nullptr && m_perf->countsRedundantWork();
    if (!m_trackLaidOut) {
        return;
    }
    const size_t capacity = arena.capacity();
    if (m_laidOutEpoch.size() < capacity) {
        m_laidOutEpoch.resize(capacity, 0);
    }
    if (++m_passEpoch == 0) {  // wrapped: no stale mark may equal the new epoch
        std::fill(m_laidOutEpoch.begin(), m_laidOutEpoch.end(), 0u);
        m_passEpoch = 1;
    }
    m_boxBefore.resize(capacity);
    for (NodeId n = 0; n < capacity; ++n) {
        if (arena.alive(n) && arena.kind(n) == NodeKind::Element) {
            const LayoutBox& b = arena.element(n).box;
            m_boxBefore[n] = BoxBefore{b.x, b.y, b.width, b.height};
        }
    }
    m_laidOutList.clear();
}

void LayoutEngine::endRedundantCount(const NodeArena& arena) {
    if (!m_trackLaidOut) {
        return;
    }
    m_trackLaidOut = false;
    uint32_t same = 0;
    for (NodeId n : m_laidOutList) {
        if (n >= m_boxBefore.size() || !arena.alive(n) || arena.kind(n) != NodeKind::Element) {
            continue;
        }
        const LayoutBox& b = arena.element(n).box;
        const BoxBefore& o = m_boxBefore[n];
        same += (b.x == o.x && b.y == o.y && b.width == o.w && b.height == o.h) ? 1u : 0u;
    }
    m_counts.redundantBoxes += same;
}

bool LayoutEngine::collectLayoutRoots(NodeArena& arena, NodeId root) {
    m_layoutRootScratch.clear();
    for (NodeId r : arena.layoutRoots()) {
        if (!arena.alive(r) || (arena.dirty(r) & static_cast<uint8_t>(kLayoutRoot)) == 0) {
            continue;  // Destroyed, or its slot reused, since.
        }
        // Inside `root`'s laid-out tree (a full pass reaches it), below no other layout root (that
        // one's subtree holds it). A subtree the full pass does not lay out (detached, under a
        // `display: none` ancestor, itself hidden) needs nothing: its boxes stay as they are.
        if (r == root || !laidOut(arena, r)) {
            continue;
        }
        bool inside = false;
        for (NodeId a = arena.parent(r); a != kInvalidNode; a = arena.parent(a)) {
            if ((arena.dirty(a) & static_cast<uint8_t>(kLayoutRoot)) != 0 || !laidOut(arena, a)) {
                break;
            }
            if (a == root) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            continue;
        }
        // Recorded as a root by a boundary flag of the last pass that sized it; one sized as a
        // boundary has its replay arguments. Anything else cannot be laid out on its own.
        const ElementData& d = arena.element(r);
        if ((d.layoutFlags & static_cast<uint8_t>(kLayoutBoundary)) == 0 ||
            (d.style.position != Position::Absolute && d.layoutDepth < 0)) {
            return false;
        }
        m_layoutRootScratch.push_back(r);
    }
    std::sort(m_layoutRootScratch.begin(), m_layoutRootScratch.end());
    m_layoutRootScratch.erase(std::unique(m_layoutRootScratch.begin(), m_layoutRootScratch.end()),
                              m_layoutRootScratch.end());
    return true;
}

void LayoutEngine::relayoutSubtree(NodeArena& arena, NodeId root, NodeId sub) {
    // The absolute pass's arguments for `sub`'s parent, recomputed top-down from the root with
    // layoutAbsolutePass() / layoutAbsoluteChild()'s own float operations in their order (every box
    // on the way is outside the subtree, hence final): its containing block and content origin.
    m_chainScratch.clear();
    for (NodeId a = arena.parent(sub); a != kInvalidNode; a = arena.parent(a)) {
        m_chainScratch.push_back(a);
        if (a == root) {
            break;
        }
    }
    std::reverse(m_chainScratch.begin(), m_chainScratch.end());
    const ElementData& rootData = arena.element(root);
    const Rect pb = rootData.box.paddingBox();
    Rect cbPad{pb.x, pb.y, pb.w, pb.h};
    NodeId cbNode = root;
    const Rect rootContent = rootData.box.contentBox();
    float contentAbsX = rootContent.x;
    float contentAbsY = rootContent.y;
    int depth = 0;
    for (size_t i = 1; i < m_chainScratch.size(); ++i) {
        const NodeId c = m_chainScratch[i];
        const ElementData& cd = arena.element(c);
        const float cAbsX = contentAbsX + cd.box.x;
        const float cAbsY = contentAbsY + cd.box.y;
        const Rect cContent = cd.box.contentBox();
        if (cd.style.position != Position::Static) {
            const Rect cPad = cd.box.paddingBox();
            cbNode = c;
            cbPad = Rect{cAbsX + cPad.x, cAbsY + cPad.y, cPad.w, cPad.h};
        }
        contentAbsX = cAbsX + cContent.x;
        contentAbsY = cAbsY + cContent.y;
        ++depth;
    }

    ElementData& data = arena.element(sub);
    if (data.style.position == Position::Absolute) {
        // Sized and placed by the absolute pass, against its (unchanged) containing block.
        ++m_laidOutElements;
        noteLaidOut(sub);  // T-753
        layoutAbsoluteChild(arena, sub, cbNode, cbPad, contentAbsX, contentAbsY, depth);
    } else {
        // In flow: the last layoutChild() call again. Its parent placed it and may have forced its
        // size (a flex item's main size); a boundary's box does not depend on its content, so those
        // are kept as they are.
        const float x = data.box.x;
        const float y = data.box.y;
        const float w = data.box.width;
        const float h = data.box.height;
        layoutChild(arena, sub, data.layoutAvailW, data.layoutAvailH, data.layoutImposedH, data.layoutDepth);
        data.box.x = x;
        data.box.y = y;
        data.box.width = w;
        data.box.height = h;
        // Its absolutely positioned descendants, against the containing blocks inside the subtree
        // or, for those with none, the one above it.
        layoutAbsoluteChild(arena, sub, cbNode, cbPad, contentAbsX, contentAbsY, depth);
    }
    applyTransforms(arena, sub);
}

void LayoutEngine::snapshotForVerify(const NodeArena& arena, NodeId root,
                                     std::vector<VerifyRecord>& out) const {
    out.clear();
    arena.traversePreorder(root, [&](NodeId n) {
        const NodeKind kind = arena.kind(n);
        if (kind != NodeKind::Element && kind != NodeKind::Text) {
            return true;
        }
        VerifyRecord r;
        r.node = n;
        const NodeCommon& common = arena.common(n);
        if (kind == NodeKind::Element) {
            const ElementData& d = arena.element(n);
            const LayoutBox& b = d.box;
            const float values[] = {b.x,
                                    b.y,
                                    b.width,
                                    b.height,
                                    b.margin.top,
                                    b.margin.right,
                                    b.margin.bottom,
                                    b.margin.left,
                                    b.padding.top,
                                    b.padding.right,
                                    b.padding.bottom,
                                    b.padding.left,
                                    b.border.top,
                                    b.border.right,
                                    b.border.bottom,
                                    b.border.left,
                                    b.intrinsicWidth,
                                    b.intrinsicHeight,
                                    b.scrollWidth,
                                    b.scrollHeight,
                                    common.translate.x,
                                    common.translate.y,
                                    common.anchor.x,
                                    common.anchor.y,
                                    static_cast<float>(common.visible),
                                    d.perspectivePx,
                                    d.perspectiveOriginPx.x,
                                    d.perspectiveOriginPx.y};
            std::memcpy(r.v, values, sizeof(values));
            if (d.sceneRoot != kInvalidNode && arena.alive(d.sceneRoot)) {
                const NodeCommon& g = arena.common(d.sceneRoot);
                r.v[28] = g.scale.x;
                r.v[29] = g.scale.y;
                r.v[30] = g.translate.x;
                r.v[31] = g.translate.y;
            }
        } else {
            const TextNodeData& t = arena.text(n);
            r.kind = 1;
            r.v[0] = t.width;
            r.v[1] = t.height;
            r.v[2] = t.intrinsicMain;
            r.v[3] = static_cast<float>(t.lines.size());
            r.v[4] = common.translate.x;
            r.v[5] = common.translate.y;
            r.v[6] = static_cast<float>(common.visible);
        }
        out.push_back(r);
        return true;
    });
}

void LayoutEngine::verifyAgainstFull(NodeArena& arena, NodeId root, Rect viewport) {
    snapshotForVerify(arena, root, m_verifyPartialScratch);
    const uint32_t laidOutCount = m_laidOutElements;
    layoutFull(arena, root, viewport, false);  // Its result stands either way.
    m_laidOutElements = laidOutCount;
    snapshotForVerify(arena, root, m_verifyFullScratch);
    ++m_check.verified;
    const size_t count = std::min(m_verifyPartialScratch.size(), m_verifyFullScratch.size());
    size_t at = count;
    int field = -1;
    for (size_t i = 0; i < count && field < 0; ++i) {
        const VerifyRecord& a = m_verifyPartialScratch[i];
        const VerifyRecord& b = m_verifyFullScratch[i];
        for (int k = 0; k < 34; ++k) {
            if (std::memcmp(&a.v[k], &b.v[k], sizeof(float)) != 0) {
                at = i;
                field = k;
                break;
            }
        }
    }
    if (field < 0 && m_verifyPartialScratch.size() == m_verifyFullScratch.size()) {
        return;
    }
    ++m_check.mismatches;
    ++m_counts.mismatches;
    // The full pass changed boxes the partial one had folded: every transform is recomputed.
    arena.markDirty(root, static_cast<uint8_t>(kDirtyTransform), arena.lastCause());
    if (!m_check.first.empty() && !m_verifyAbort) {
        return;
    }
    char buf[384];
    if (field < 0) {
        std::snprintf(buf, sizeof(buf),
                      "partial layout: the tree changed during the comparison (%zu vs %zu nodes)",
                      m_verifyPartialScratch.size(), m_verifyFullScratch.size());
        if (m_check.first.empty()) {
            m_check.first = buf;
        }
    } else {
        const VerifyRecord& a = m_verifyPartialScratch[at];
        const VerifyRecord& b = m_verifyFullScratch[at];
        std::string id;
        if (a.kind == 0 && arena.element(a.node).id != kInvalidString) {
            id = std::string(string(arena.element(a.node).id));
        }
        std::string roots;
        for (NodeId r : m_layoutRootScratch) {
            roots += (roots.empty() ? "" : ",") + std::to_string(r);
        }
        std::snprintf(
            buf, sizeof(buf),
            "partial layout differs from a full layout: %s node %u%s%s, %s: partial %.9g, full %.9g "
            "(layout roots %s)",
            a.kind == 0 ? "element" : "text", static_cast<unsigned>(a.node), id.empty() ? "" : " #",
            id.c_str(), a.kind == 0 ? kElementFields[field] : kTextFields[field],
            static_cast<double>(a.v[field]), static_cast<double>(b.v[field]), roots.c_str());
        if (m_check.first.empty()) {
            m_check.first = buf;
            m_check.firstNode = a.node;
        }
    }
    if (m_verifyAbort) {
        detail::assertFail(buf, __FILE__, __LINE__);
    }
}

}  // namespace xt
