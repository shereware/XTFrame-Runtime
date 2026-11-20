#include <vector>

#include "doctest.h"
#include "layer_collect.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// layer_collect.h tests. See Docs/tasks/T-44a.md "測試" / "test_layer_collect.cpp".

using namespace xt;
using namespace xt::layer_collect;

namespace {

/// Collects `node`'s direct children in document order, matching Painter's ordering contract for
/// this test (no z-index reordering needed here).
std::vector<NodeId> children(const NodeArena& arena, NodeId node) {
    std::vector<NodeId> out;
    arena.forEachChild(node, [&](NodeId c) { out.push_back(c); });
    return out;
}

}  // namespace

TEST_CASE("layer_collect: collectSections splits 3D/2D/3D children into sorted/plain/sorted") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId c0 = arena.create(NodeKind::Group);  // is3D
    NodeId c1 = arena.create(NodeKind::Group);  // plain 2D
    NodeId c2 = arena.create(NodeKind::Group);  // is3D
    arena.appendChild(root, c0);
    arena.appendChild(root, c1);
    arena.appendChild(root, c2);
    arena.common(c0).is3D = 1;
    arena.common(c2).is3D = 1;

    std::vector<uint8_t> has3D;
    computeHas3D(arena, root, has3D);
    CHECK(has3D[c0] == 1);
    CHECK(has3D[c1] == 0);
    CHECK(has3D[c2] == 1);

    std::vector<NodeId> ordered = children(arena, root);
    std::vector<Section> sections;
    collectSections(arena, ordered, has3D, sections);

    REQUIRE(sections.size() == 3);
    CHECK(sections[0].sorted == true);
    CHECK(sections[0].firstChild == 0);
    CHECK(sections[0].childCount == 1);
    CHECK(sections[1].sorted == false);
    CHECK(sections[1].firstChild == 1);
    CHECK(sections[1].childCount == 1);
    CHECK(sections[2].sorted == true);
    CHECK(sections[2].firstChild == 2);
    CHECK(sections[2].childCount == 1);
}

TEST_CASE("layer_collect: collectSections merges consecutive 3D siblings into one sorted section") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId c0 = arena.create(NodeKind::Group);  // is3D
    NodeId c1 = arena.create(NodeKind::Group);  // is3D
    NodeId c2 = arena.create(NodeKind::Group);  // plain 2D
    arena.appendChild(root, c0);
    arena.appendChild(root, c1);
    arena.appendChild(root, c2);
    arena.common(c0).is3D = 1;
    arena.common(c1).is3D = 1;

    std::vector<uint8_t> has3D;
    computeHas3D(arena, root, has3D);
    std::vector<NodeId> ordered = children(arena, root);
    std::vector<Section> sections;
    collectSections(arena, ordered, has3D, sections);

    REQUIRE(sections.size() == 2);
    CHECK(sections[0].sorted == true);
    CHECK(sections[0].firstChild == 0);
    CHECK(sections[0].childCount == 2);
    CHECK(sections[1].sorted == false);
    CHECK(sections[1].firstChild == 2);
    CHECK(sections[1].childCount == 1);
}

TEST_CASE("layer_collect: collectLayers gathers a 3D layer's own 3D descendants as separate layers") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId layer = arena.create(NodeKind::Group);   // is3D
    NodeId twoD = arena.create(NodeKind::Group);    // plain 2D child of layer
    NodeId nested = arena.create(NodeKind::Group);  // is3D grandchild -- a separate layer
    arena.appendChild(root, layer);
    arena.appendChild(layer, twoD);
    arena.appendChild(twoD, nested);
    arena.common(layer).is3D = 1;
    arena.common(nested).is3D = 1;

    Mat4 view = mat4Identity();
    std::vector<LayerEntry> layers;
    uint32_t docOrder = 0;
    collectLayers(arena, layer, view, layers, docOrder);

    REQUIRE(layers.size() == 2);
    CHECK(layers[0].node == layer);
    CHECK(layers[0].docOrder == 0);
    CHECK(layers[1].node == nested);
    CHECK(layers[1].docOrder == 1);
    CHECK(docOrder == 2);
}

TEST_CASE("layer_collect: collectLayers does not descend into a nested 3D space root's subtree") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId layer = arena.create(NodeKind::Group);         // is3D
    NodeId nestedRoot = arena.create(NodeKind::Group);    // is3D + isSpaceRoot
    NodeId insideNested = arena.create(NodeKind::Group);  // is3D, inside nestedRoot's subtree
    arena.appendChild(root, layer);
    arena.appendChild(layer, nestedRoot);
    arena.appendChild(nestedRoot, insideNested);
    arena.common(layer).is3D = 1;
    arena.common(nestedRoot).is3D = 1;
    arena.common(nestedRoot).isSpaceRoot = 1;
    arena.common(insideNested).is3D = 1;

    Mat4 view = mat4Identity();
    std::vector<LayerEntry> layers;
    uint32_t docOrder = 0;
    collectLayers(arena, layer, view, layers, docOrder);

    // layer and nestedRoot itself are collected (nestedRoot's own is3D still counts -- it's the
    // layer that hosts the nested projection); insideNested is not, since it belongs to
    // nestedRoot's own, separate projection.
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].node == layer);
    CHECK(layers[1].node == nestedRoot);
}

TEST_CASE("layer_collect: sortLayers orders by depth ascending, then docOrder ascending") {
    std::vector<LayerEntry> layers = {
        LayerEntry{NodeId{1}, 5.0f, 0, 1.0f},
        LayerEntry{NodeId{2}, -3.0f, 1, 1.0f},
        LayerEntry{NodeId{3}, 5.0f, 2, 1.0f},  // same depth as entry 0, later docOrder
        LayerEntry{NodeId{4}, -3.0f, 3, 1.0f},
    };
    sortLayers(layers);

    REQUIRE(layers.size() == 4);
    CHECK(layers[0].node == NodeId{2});
    CHECK(layers[1].node == NodeId{4});
    CHECK(layers[2].node == NodeId{1});
    CHECK(layers[3].node == NodeId{3});
}

TEST_CASE("layer_collect: collectLayers computes depth from view * world3 * (anchor.xyz, 1)") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId near = arena.create(NodeKind::Group);
    NodeId far = arena.create(NodeKind::Group);
    arena.appendChild(root, near);
    arena.appendChild(root, far);
    arena.common(near).is3D = 1;
    arena.common(near).world3 = mat4Translate(Vec3{0, 0, 10});
    arena.common(far).is3D = 1;
    arena.common(far).world3 = mat4Translate(Vec3{0, 0, -50});

    Mat4 view = mat4Identity();
    std::vector<LayerEntry> layers;
    uint32_t docOrder = 0;
    collectLayers(arena, root, view, layers, docOrder);

    REQUIRE(layers.size() == 2);
    CHECK(layers[0].node == near);
    CHECK(layers[0].depth == doctest::Approx(10.0f));
    CHECK(layers[1].node == far);
    CHECK(layers[1].depth == doctest::Approx(-50.0f));
}

TEST_CASE("layer_collect: computeHas3D is all zero for a tree with no 3D nodes") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId a = arena.create(NodeKind::Group);
    NodeId b = arena.create(NodeKind::Group);
    arena.appendChild(root, a);
    arena.appendChild(a, b);

    std::vector<uint8_t> has3D;
    computeHas3D(arena, root, has3D);

    REQUIRE(has3D.size() == arena.capacity());
    for (uint8_t v : has3D) {
        CHECK(v == 0);
    }
}

TEST_CASE("layer_collect: collectSegmentItems splits a flat child's content at its 3D layers (T-727)") {
    // child B (flat) = { b1 (flat), L (3D layer) = { inL (flat, inside the layer) }, b3 (flat) = { b3c } }
    NodeArena arena;
    NodeId b = arena.create(NodeKind::Group);
    NodeId b1 = arena.create(NodeKind::Group);
    NodeId layer = arena.create(NodeKind::Group);
    NodeId inL = arena.create(NodeKind::Group);
    NodeId b3 = arena.create(NodeKind::Group);
    NodeId b3c = arena.create(NodeKind::Group);
    arena.appendChild(b, b1);
    arena.appendChild(b, layer);
    arena.appendChild(layer, inL);
    arena.appendChild(b, b3);
    arena.appendChild(b3, b3c);
    arena.common(layer).is3D = 1;
    arena.common(layer).world3 = mat4Translate(Vec3{0, 0, 5});

    std::vector<LayerEntry> items;
    std::vector<PieceRange> ranges(arena.capacity());
    uint32_t docOrder = 0;
    collectSegmentItems(arena, b, mat4Identity(), items, docOrder, ranges);

    // Document order: piece 0 (B, b1), the layer, piece 1 (b3, b3c).
    REQUIRE(items.size() == 3);
    CHECK(items[0].node == b);
    CHECK(items[0].piece == 0u);
    CHECK(items[0].depth == doctest::Approx(0.0f));
    CHECK(items[1].node == layer);
    CHECK(items[1].piece == kLayerItem);
    CHECK(items[1].depth == doctest::Approx(5.0f));
    CHECK(items[2].node == b);
    CHECK(items[2].piece == 1u);
    CHECK(items[0].docOrder < items[1].docOrder);
    CHECK(items[1].docOrder < items[2].docOrder);

    CHECK(ranges[b].first == 0u);
    CHECK(ranges[b].last == 1u);
    CHECK(ranges[b1].first == 0u);
    CHECK(ranges[b1].last == 0u);
    CHECK(ranges[b3].first == 1u);
    CHECK(ranges[b3c].first == 1u);
    CHECK_FALSE(pieceInRange(ranges[layer], 0));
    CHECK_FALSE(pieceInRange(ranges[layer], 1));
}

TEST_CASE("layer_collect: a flat group keeps its 3D descendants in its own piece (T-762)") {
    // child B (flat) = { b1, G (flat, the group) = { L (3D layer) = { inL }, g2 }, b3 }
    NodeArena arena;
    NodeId b = arena.create(NodeKind::Group);
    NodeId b1 = arena.create(NodeKind::Group);
    NodeId g = arena.create(NodeKind::Group);
    NodeId layer = arena.create(NodeKind::Group);
    NodeId inL = arena.create(NodeKind::Group);
    NodeId g2 = arena.create(NodeKind::Group);
    NodeId b3 = arena.create(NodeKind::Group);
    arena.appendChild(b, b1);
    arena.appendChild(b, g);
    arena.appendChild(g, layer);
    arena.appendChild(layer, inL);
    arena.appendChild(g, g2);
    arena.appendChild(b, b3);
    arena.common(layer).is3D = 1;
    arena.common(layer).world3 = mat4Translate(Vec3{0, 0, 5});

    std::vector<LayerEntry> items;
    std::vector<PieceRange> ranges(arena.capacity());
    uint32_t docOrder = 0;

    SUBCASE("no grouping property: the layer is an item of its own (T-727)") {
        CHECK_FALSE(flattens3D(arena, g));
        collectSegmentItems(arena, b, mat4Identity(), items, docOrder, ranges);
        REQUIRE(items.size() == 3);
        CHECK(items[1].node == layer);
        CHECK(items[1].piece == kLayerItem);
        CHECK_FALSE(pieceInRange(ranges[layer], 0));
    }
    SUBCASE("opacity < 1: one item, the layer and everything below G in piece 0") {
        arena.common(g).opacity = 0.5f;
        CHECK(flattens3D(arena, g));
        collectSegmentItems(arena, b, mat4Identity(), items, docOrder, ranges);
        REQUIRE(items.size() == 1);
        CHECK(items[0].node == b);
        CHECK(items[0].piece == 0u);
        CHECK(items[0].depth == doctest::Approx(0.0f));
        for (NodeId n : {b1, g, layer, inL, g2, b3}) {
            CAPTURE(n);
            CHECK(ranges[n].first == 0u);
            CHECK(ranges[n].last == 0u);
        }
        CHECK(ranges[b].first == 0u);
        CHECK(ranges[b].last == 0u);
    }
    SUBCASE("a blend mode, an effect, a Mask child: the same") {
        SUBCASE("blend") { arena.common(g).blend = static_cast<uint8_t>(BlendMode::Multiply); }
        SUBCASE("mask") {
            NodeId mask = arena.create(NodeKind::Mask);
            arena.insertBefore(g, mask, layer);
            ranges.resize(arena.capacity());
        }
        CHECK(flattens3D(arena, g));
        collectSegmentItems(arena, b, mat4Identity(), items, docOrder, ranges);
        REQUIRE(items.size() == 1);
        CHECK(pieceInRange(ranges[layer], 0));
        CHECK(pieceInRange(ranges[inL], 0));
    }
    SUBCASE("the group is the segment child itself") {
        arena.common(b).opacity = 0.5f;
        collectSegmentItems(arena, b, mat4Identity(), items, docOrder, ranges);
        REQUIRE(items.size() == 1);
        CHECK(items[0].piece == 0u);
        CHECK(pieceInRange(ranges[layer], 0));
    }
}
