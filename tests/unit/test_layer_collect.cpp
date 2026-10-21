#include <vector>

#include "doctest.h"
#include "layer_collect.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// layer_collect.h tests. See docs/tasks/T-44a.md "測試" / "test_layer_collect.cpp".

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
