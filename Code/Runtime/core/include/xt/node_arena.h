#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/assert.h"
#include "xt/node.h"
#include "xt/types.h"

/// Structure-of-arrays node storage: topology, tombstone/free-list deletion, generation counters,
/// and dirty-bit propagation. Contains no property semantics (T-05) and no scene instantiation (T-08).
/// See Docs/design/01-architecture.md §1.3-1.4, Docs/tasks/T-04.md

namespace xt {

/// T-244 (Docs/design/32-rigging-and-deformation.md §32.3): the skinned geometry of one skinned
/// Path (`points`, laid out as PathNodeData::points) or Mesh (`positions`), in the skeleton group's
/// space, cached beside the node data (NodeArena::skinned()) so a static frame skins nothing and
/// the IR / PathNodeData::points stay untouched. Written by the Document's rig step, read by the
/// Painter.
struct SkinnedGeometry {
    std::vector<float> points;    ///< Path: the skinned points.
    std::vector<Vec2> positions;  ///< Mesh: the skinned positions.
    /// RigState::version the geometry was skinned at; 0xFFFFFFFF = must be re-skinned.
    uint32_t skinnedVersion = 0xFFFFFFFFu;
    uint32_t generation = 0;  ///< The node's generation when the entry was made.
    uint8_t valid = 0;        ///< 1 once skinned at least once.
};

/// Owns all nodes of a Document. NodeId indices are stable across create/destroy (subject to
/// generation reuse); NodeId 0 is a regular valid index like any other.
class NodeArena {
public:
    // Creation / destruction.

    /// Creates a new node of the given kind. Reuses a free slot (bumping its generation) when one
    /// is available. kind must not be NodeKind::Free.
    NodeId create(NodeKind kind);

    /// Destroys the subtree rooted at id (postorder). Every destroyed node's kind becomes Free, its
    /// slot joins the free list, and its *Data is reset to a default value (vector capacity kept).
    /// Fixes up the former parent's firstChild/lastChild/nextSibling chain and marks it
    /// kDirtyTree | kDirtyLayout.
    void destroy(NodeId id);

    // Topology.

    /// Appends child as the last child of parent. child must currently have no parent. Marks parent
    /// kDirtyTree | kDirtyLayout.
    void appendChild(NodeId parent, NodeId child);

    /// Inserts child as parent's child immediately before "before". before must be a current child of
    /// parent; before == kInvalidNode behaves like appendChild. Marks parent kDirtyTree | kDirtyLayout.
    void insertBefore(NodeId parent, NodeId child, NodeId before);

    /// Removes child from its parent's child list without destroying it. Marks the former parent
    /// kDirtyTree | kDirtyLayout.
    void detach(NodeId child);

    /// Returns id's parent, or kInvalidNode if it has none.
    NodeId parent(NodeId id) const;
    /// Returns id's first child, or kInvalidNode if it has none.
    NodeId firstChild(NodeId id) const;
    /// Returns id's last child, or kInvalidNode if it has none.
    NodeId lastChild(NodeId id) const;
    /// Returns id's next sibling, or kInvalidNode if it is the last child.
    NodeId nextSibling(NodeId id) const;
    /// O(siblings): walks from the parent's firstChild to find the sibling preceding id.
    NodeId prevSibling(NodeId id) const;
    /// Returns id's number of children.
    uint32_t childCount(NodeId id) const;
    /// True if ancestor is a strict ancestor of node (node itself does not count).
    bool isAncestorOf(NodeId ancestor, NodeId node) const;

    // Access.

    /// Bounds-checked; returns NodeKind::Free for an out-of-range id instead of asserting.
    NodeKind kind(NodeId id) const;
    /// Bounds-checked; returns false for an out-of-range id instead of asserting.
    bool alive(NodeId id) const;
    /// Returns id's current generation counter, used to detect stale handles after slot reuse.
    uint32_t generation(NodeId id) const;
    /// Returns id's kind-independent common fields (transform, dirty flags, etc.).
    NodeCommon& common(NodeId id);
    /// Returns id's kind-independent common fields (transform, dirty flags, etc.).
    const NodeCommon& common(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Path).
    PathNodeData& path(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Path).
    const PathNodeData& path(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Image).
    ImageNodeData& image(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Image).
    const ImageNodeData& image(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Text).
    TextNodeData& text(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Text).
    const TextNodeData& text(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Element).
    ElementData& element(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Element).
    const ElementData& element(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Camera).
    CameraData& camera(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Camera).
    const CameraData& camera(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::External) (T-191).
    ExternalNodeData& external(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::External) (T-191).
    const ExternalNodeData& external(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Particles) (T-202).
    ParticleNodeData& particles(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Particles) (T-202).
    const ParticleNodeData& particles(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Modifier) (T-225).
    ModifierNodeData& modifier(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Modifier) (T-225).
    const ModifierNodeData& modifier(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Shape) (T-225).
    ShapeNodeData& shape(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Shape) (T-225).
    const ShapeNodeData& shape(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Bone) (T-243).
    BoneNodeData& bone(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Bone) (T-243).
    const BoneNodeData& bone(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Constraint) (T-243).
    ConstraintNodeData& constraint(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Constraint) (T-243).
    const ConstraintNodeData& constraint(NodeId id) const;
    /// XT_ASSERT(kind(id) == NodeKind::Mesh) (T-243).
    MeshNodeData& mesh(NodeId id);
    /// XT_ASSERT(kind(id) == NodeKind::Mesh) (T-243).
    const MeshNodeData& mesh(NodeId id) const;

    // Effect pools (Docs/design/18-effects.md §18.2). Global, not per-node: node-owned ranges are
    // NodeCommon::effectOffset/effectCount. Only grow; destroy() does not reclaim slots (18.2's
    // pool policy), so entries referenced by a destroyed node's former range simply become holes
    // until a future Document::compactEffects() (out of scope for this task).
    /// Returns the effect instance pool.
    std::vector<EffectInstance>& effects();
    /// Returns the effect instance pool.
    const std::vector<EffectInstance>& effects() const;
    /// Returns the gradient-stop pool referenced by `effects()`' GradientMask entries.
    std::vector<GradientStop>& effectStops();
    /// Returns the gradient-stop pool referenced by `effects()`' GradientMask entries.
    const std::vector<GradientStop>& effectStops() const;

    /// Reserves `effectCount`/`stopCount` slots up front (typically 2x the load-time total; see
    /// Docs/tasks/T-50.md), so ordinary effect reassignment doesn't reallocate mid-frame (01.7).
    void reserveEffects(uint32_t effectCount, uint32_t stopCount);
    /// Replaces `node`'s effect stack (18.2). `effects.size() > 16` -> kInvalidArgument, node
    /// left untouched. Otherwise: when the new sequence's EffectKind-per-slot and count exactly
    /// match `node`'s current one, overwrites the existing pool segment's params/colors/shader in
    /// place; each slot's gradient stops are rebased in place too when the new stop count matches
    /// the old one, else that slot's stops are appended at effectStops()'s end and its
    /// stopOffset/stopCount rewritten. When the sequence differs (kind or count), the whole new
    /// segment is appended at effects()'s end and `node`'s effectOffset/effectCount rewritten; the
    /// old segment becomes an unreachable hole until compactEffects() (T-270a: Document::update()
    /// calls it once most of a large pool is holes). `effects[i].stopOffset`/
    /// `stopCount` index into the caller-supplied `stops` span (not effectStops() positions) and
    /// are rewritten to effectStops() positions as they're copied. If either pool reallocates
    /// during the call, effectPoolGrew() latches true (NodeArena has no warning list of its own;
    /// Document::update() surfaces this as a one-shot warning). Always returns kOk otherwise.
    Status assignEffects(NodeId node, std::span<const EffectInstance> effects,
                         std::span<const GradientStop> stops);
    /// Rebuilds effects()/effectStops() from scratch by walking every alive node in ascending
    /// NodeId order and copying its current segment contiguously (discarding the holes left by
    /// assignEffects()'s append path), rewriting every node's effectOffset as it goes. O(alive
    /// node count + total effect/stop count). See Document::compactEffects().
    void compactEffects();
    /// T-270a: the number of effects()/effectStops() entries alive nodes' segments use (the rest
    /// are holes). T-270b: O(1) -- kept up to date by assignEffects(), setEffectRange() and
    /// destroy() (a Debug build checks it against a full scan).
    void liveEffectCounts(size_t& effects, size_t& stops) const;
    /// T-270b: points `node`'s effect range at effects()[offset, offset + count), entries the caller
    /// has already appended to effects()/effectStops() (scene instantiation), keeping the live
    /// counts above. Writing NodeCommon::effectOffset/effectCount directly bypasses them.
    void setEffectRange(NodeId node, uint32_t offset, uint32_t count);
    /// True since the last clearEffectPoolGrew() call if assignEffects() caused effects() or
    /// effectStops() to reallocate. See assignEffects()'s doc comment.
    bool effectPoolGrew() const { return m_effectPoolGrew; }
    /// Clears the flag set by effectPoolGrew(), so a caller (Document::update()) only surfaces one
    /// warning per growth event.
    void clearEffectPoolGrew() { m_effectPoolGrew = false; }
    /// T-211: latched by setProperty() when a write addresses `effect.<n>.<param>` on a node with
    /// no n-th effect (a keyframe animating an effect the element's stack lacks); the write is a
    /// no-op. Document::update() turns it into a one-shot kWarnEffectMissing warning.
    bool effectMissingWritten() const { return m_effectMissingWritten; }
    void noteEffectMissingWritten() { m_effectMissingWritten = true; }
    void clearEffectMissingWritten() { m_effectMissingWritten = false; }

    /// T-244: `id`'s skinned geometry side-table entry, or nullptr when it has none (never skinned,
    /// or the slot was reused by another node since).
    const SkinnedGeometry* skinned(NodeId id) const;
    /// T-244: `id`'s side-table entry, created (stale, empty) when missing.
    SkinnedGeometry& skinnedSlot(NodeId id);
    /// T-244: marks `id`'s skinned geometry stale (e.g. its weights changed); no-op without one.
    void invalidateSkinned(NodeId id);

    /// Node name (e.g. a Lottie layer name or scene node name), for API lookup. Defaults to
    /// kInvalidString for every newly created node. See Docs/tasks/T-08.md.
    StringId name(NodeId id) const;
    /// Sets id's name, used for by-name lookup (e.g. a Lottie layer/scene node name).
    void setName(NodeId id, StringId name);

    // Dirty bits.

    /// Returns id's current dirty-bit mask.
    uint8_t dirty(NodeId id) const;
    /// Sets bits on id and propagates per Docs/design/01-architecture.md §1.4:
    ///   kDirtyStyle     -> self | all ancestors (to root)
    ///   kDirtyLayout    -> self | ancestors up to and including the first node where
    ///                      stopsLayoutPropagation() is true (currently always false, so to root)
    ///   kDirtyTransform -> self | all descendants (preorder)
    ///   kDirtyPaint, kDirtyTree -> self only
    void markDirty(NodeId id, uint8_t bits);
    /// Calls markDirty(n, bits) for id and each ancestor on the path from id up to and including
    /// stopAt; if stopAt is not found on that path (e.g. kInvalidNode), walks all the way to the
    /// document root. See Docs/design/19-3d-space.md §19.3, Docs/tasks/T-43c.md "T-43b 遺留修正"
    /// 2 (isSpaceRoot cache invalidation when a descendant's is3D changes).
    void markDirtyAncestors(NodeId id, uint8_t bits, NodeId stopAt);
    /// Clears every dirty bit on id, without propagation.
    void clearDirty(NodeId id);
    /// Clears exactly `bits` on id, leaving any other set dirty bits untouched (no propagation --
    /// propagation only ever happens on set). See Docs/tasks/T-24.md step 7 (StyleEngine::resolve
    /// clears only kDirtyStyle per element after recomputing it, keeping kDirtyLayout/kDirtyPaint
    /// set by that same resolve).
    void clearDirtyBits(NodeId id, uint8_t bits);
    /// Clears every node's dirty bits (e.g. after a frame's dirty processing is fully flushed).
    void clearAllDirty();
    /// True if any alive node carries any of `bits` (O(n) scan; used once per frame).
    bool anyDirty(uint8_t bits) const;

    // Traversal. Non-recursive (bounded by an internal, capacity-retaining stack member).

    /// Invokes `f(NodeId)` once per direct child of id, left to right; safe if `f` detaches or
    /// destroys the current child (the next sibling is captured before calling `f`).
    template <class F>
    void forEachChild(NodeId id, F&& f) const {
        XT_ASSERT(alive(id));
        NodeId c = firstChild(id);
        while (c != kInvalidNode) {
            NodeId next = nextSibling(c);
            f(c);
            c = next;
        }
    }

    /// Preorder DFS including root. F(NodeId) returns bool; returning false skips root's subtree
    /// (siblings are still visited).
    template <class F>
    void traversePreorder(NodeId root, F&& f) const {
        XT_ASSERT(alive(root));
        m_stack.clear();
        m_stack.push_back(root);
        while (!m_stack.empty()) {
            NodeId n = m_stack.back();
            m_stack.pop_back();
            bool descend = f(n);
            if (descend) {
                pushChildrenReversed(n);
            }
        }
    }

    /// Postorder DFS: children visited before their parent, siblings left-to-right, includes root.
    template <class F>
    void traversePostorder(NodeId root, F&& f) const {
        XT_ASSERT(alive(root));
        collectSubtreePostorder(root, m_postBuf);
        for (NodeId n : m_postBuf) {
            f(n);
        }
    }

    // Stats.

    /// Returns the total number of node slots (alive + free).
    uint32_t capacity() const;
    /// Returns the number of currently alive nodes.
    uint32_t aliveCount() const;

private:
    void checkAlive(NodeId id) const;
    /// Extension point for kDirtyLayout propagation (e.g. stopping at position:absolute elements).
    /// ComputedStyle is empty in this task, so this always returns false.
    bool stopsLayoutPropagation(NodeId id) const;
    uint32_t allocateData(NodeKind kind);
    void freeNodeData(NodeId id);
    void pushChildrenReversed(NodeId id) const;
    void collectSubtreePostorder(NodeId root, std::vector<NodeId>& out) const;

    // SoA topology, See Docs/design/01-architecture.md §1.3.
    std::vector<NodeKind> m_kind;
    std::vector<NodeId> m_parent;
    std::vector<NodeId> m_firstChild;
    std::vector<NodeId> m_nextSibling;
    std::vector<NodeId> m_lastChild;
    std::vector<NodeCommon> m_common;
    std::vector<uint32_t> m_kindIndex;  // index into m_paths / m_elements / m_texts / m_images
    std::vector<PathNodeData> m_paths;
    std::vector<ElementData> m_elements;
    std::vector<TextNodeData> m_texts;
    std::vector<ImageNodeData> m_images;
    std::vector<CameraData> m_cameras;
    std::vector<ExternalNodeData> m_externals;
    std::vector<ParticleNodeData> m_particles;
    std::vector<ModifierNodeData> m_modifiers;
    std::vector<ShapeNodeData> m_shapes;
    std::vector<BoneNodeData> m_bones;              // T-243
    std::vector<ConstraintNodeData> m_constraints;  // T-243
    std::vector<MeshNodeData> m_meshes;             // T-243
    std::vector<uint8_t> m_dirty;
    std::vector<StringId> m_name;

    std::vector<uint32_t> m_generation;
    std::vector<NodeId> m_freeList;
    std::vector<uint32_t> m_pathFreeList;
    std::vector<uint32_t> m_elementFreeList;
    std::vector<uint32_t> m_textFreeList;
    std::vector<uint32_t> m_imageFreeList;
    std::vector<uint32_t> m_cameraFreeList;
    std::vector<uint32_t> m_externalFreeList;
    std::vector<uint32_t> m_particleFreeList;
    std::vector<uint32_t> m_modifierFreeList;
    std::vector<uint32_t> m_shapeFreeList;
    std::vector<uint32_t> m_boneFreeList;        // T-243
    std::vector<uint32_t> m_constraintFreeList;  // T-243
    std::vector<uint32_t> m_meshFreeList;        // T-243
    uint32_t m_alive = 0;

    // Effect pools (Docs/design/18-effects.md §18.2); arena-global, not per-node kind data (see
    // the accessor comments above), so they are not touched by allocateData()/freeNodeData().
    std::vector<EffectInstance> m_effects;
    std::vector<GradientStop> m_effectStops;
    // T-270b: the entries alive nodes' segments use (liveEffectCounts()); countEffects() adds or
    // removes one node's segment.
    size_t m_liveEffects = 0;
    size_t m_liveEffectStops = 0;
    void countEffects(NodeId node, bool add);
    // Set by assignEffects() when m_effects/m_effectStops reallocated; see effectPoolGrew().
    bool m_effectPoolGrew = false;
    bool m_effectMissingWritten = false;  // T-211, see effectMissingWritten().
    // T-244: skinned geometry side table; m_skinSlot is indexed by NodeId (0xFFFFFFFF = none).
    std::vector<uint32_t> m_skinSlot;
    std::vector<SkinnedGeometry> m_skinned;

    // Traversal / destroy scratch buffers; capacity retained across calls.
    mutable std::vector<NodeId> m_stack;
    mutable std::vector<NodeId> m_childScratch;
    mutable std::vector<NodeId> m_postBuf;
    std::vector<NodeId> m_destroyBuf;
};

/// T-345 (25 §25.4.2 item 4): true when `node` or one of its ancestors carries kInMotion (a
/// transition, keyframe track or override is moving it this update).
bool inMotion(const NodeArena& arena, NodeId node);

}  // namespace xt
