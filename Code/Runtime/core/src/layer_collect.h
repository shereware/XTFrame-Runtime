#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// Pure functions used by Painter (T-44b) to sort a 3D space root's direct children into
/// segments and collect/sort the 3D layers inside a "sorted" segment. See
/// Docs/design/19-3d-space.md §19.6.

namespace xt::layer_collect {

/// LayerEntry::piece of a 3D layer (T-727); any other value is a flat-content item's piece index.
inline constexpr uint32_t kLayerItem = 0xFFFFFFFFu;
/// "No piece filter": a flat child's content is drawn / hit tested whole (T-727).
inline constexpr uint32_t kNoPiece = 0xFFFFFFFFu;

/// One 3D layer collected inside a sorted segment: the layer's node, its camera-space depth
/// (19.6), and its document-order index (used as sortLayers' tiebreaker).
///
/// T-727 (19 §19.x ruling 1): the same entry also describes a flat-content item of the segment --
/// `node` is then the segment's direct child whose flat content it is, `piece` the index of the run
/// of that content between two of the child's 3D layers (PieceRange), `depth` the depth of the
/// segment's plane at the child's anchor and `inheritedAlpha` 1.
struct LayerEntry {
    NodeId node;
    float depth;
    uint32_t docOrder;
    /// Docs/tasks/T-48.md "T-44b 遺留修正" 2: the product of every 2D ancestor's `opacity`
    /// between the sorted segment's direct child and this layer (the layer's own opacity is not
    /// included -- it is applied separately, when the layer itself is painted/entered). 1.0 when
    /// no 2D ancestor sits between the segment child and the layer (e.g. the segment child is
    /// itself the layer).
    float inheritedAlpha;
    /// kLayerItem for a 3D layer; otherwise the flat piece index (T-727).
    uint32_t piece = kLayerItem;
};

/// T-727: the pieces of a sorted segment child's flat content a node takes part in. A flat node's
/// own content belongs to piece `first` (the number of the child's 3D layers before it in document
/// order); `last` is the piece of the last flat node of its subtree, so the node is entered when
/// pieces `first`..`last` are drawn and draws its own content only in `first`. A node that is not
/// flat content (a 3D layer, a camera, anything inside a layer) gets an empty range.
struct PieceRange {
    uint32_t first = 0xFFFFFFFFu;
    uint32_t last = 0;
};

/// Does `range` admit `piece`?
inline bool pieceInRange(const PieceRange& range, uint32_t piece) {
    return piece >= range.first && piece <= range.last;
}

/// One contiguous run of a 3D space root's direct children (indices into the `orderedChildren`
/// span passed to collectSections), per Docs/design/19-3d-space.md §19.6 step 1. `sorted == true`
/// means every child in the run has a subtree containing at least one 3D layer (`has3D`); such a
/// run's 3D layers are collected and globally depth-sorted together (step 3). `sorted == false`
/// segments are always a single child, drawn in the root plane in document order (step 2).
struct Section {
    uint32_t firstChild;  ///< Index into `orderedChildren` of the run's first child.
    uint32_t childCount;  ///< Number of children in the run.
    bool sorted;
};

/// Splits `orderedChildren` (the root's direct children, already in Painter's z-index-sorted
/// child order) into Sections: a maximal run of consecutive children whose subtree has any 3D
/// layer (`has3D[child] != 0`) becomes one `sorted` Section; every other child becomes its own
/// (unsorted) Section. `has3D.size()` must equal the owning NodeArena's `capacity()`.
void collectSections(const NodeArena& arena, std::span<const NodeId> orderedChildren,
                     std::span<const uint8_t> has3D, std::vector<Section>& out);

/// Collects every 3D layer (a node with `is3D == 1`) in `child`'s subtree (child included),
/// preorder, appending to `out`. Does not descend into a nested 3D space root's subtree
/// (`isSpaceRoot == 1`), since that subtree's own layers belong to its own, separate projection.
/// `docOrderCounter` is incremented once per collected layer (shared across sibling calls, so the
/// resulting `docOrder` values are consistent document order across a whole sorted segment).
/// Depth is computed per §19.6: `(view * node.world3 * (anchor.xyz, 1)).z`.
void collectLayers(const NodeArena& arena, NodeId child, const Mat4& view, std::vector<LayerEntry>& out,
                   uint32_t& docOrderCounter);

/// T-727 (Docs/design/19-3d-space.md §19.x ruling 1): collectLayers() for one sorted segment child
/// plus the child's flat content as items of the same list. Appends, in document order, every 3D
/// layer of `child`'s subtree (exactly as collectLayers() does) and one item (piece != kLayerItem)
/// per run of the child's flat content between two of those layers, all sharing `docOrderCounter`;
/// a flat item's depth is §19.6's depth of the root plane at the child's anchor
/// (`(view * mat4FromMat2x3(child.world) * (anchor, 0, 1)).z`, 0 under a CSS root). Writes the
/// PieceRange of every node of the child's subtree into `ranges` (sized arena.capacity()).
///
/// Returns false -- after removing the child's flat items again -- when the child's flat content
/// cannot be split: a flat node with 3D layers inside forms a group (opacity < 1, a blend mode,
/// enabled effects, Mask or Modifier children) whose flat content spans more than one piece. The
/// ruling leaves that combination undecided; the caller then keeps the pre-T-727 order for this
/// child (its whole flat content before every item of the segment). A child that is itself a 3D
/// layer has no flat content and returns true.
bool collectSegmentItems(const NodeArena& arena, NodeId child, const Mat4& view, std::vector<LayerEntry>& out,
                         uint32_t& docOrderCounter, std::vector<PieceRange>& ranges);

/// Sorts `entries` by (depth ascending, docOrder ascending) -- farthest from the camera first,
/// per §19.6's "由遠到近（depth 小者先）畫". Uses std::sort (not stable_sort, per 01.7's no-per-
/// frame-allocation rule); the explicit docOrder tiebreaker makes the result deterministic.
void sortLayers(std::vector<LayerEntry>& entries);

/// Computes, for every node in `root`'s subtree (root included), whether its subtree contains any
/// `is3D` node -- postorder, not counting content inside a nested 3D space root's subtree (that
/// subtree is opaque to this computation, per collectLayers' rule above). `has3D` is resized to
/// `arena.capacity()` and indexable by NodeId.
void computeHas3D(const NodeArena& arena, NodeId root, std::vector<uint8_t>& has3D);

}  // namespace xt::layer_collect
