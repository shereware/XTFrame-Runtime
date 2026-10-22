#include <vector>

#include "doctest.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/types.h"

using namespace xt;

TEST_CASE("NodeArena: appendChild builds sibling chain") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId c0 = arena.create(NodeKind::Path);
    NodeId c1 = arena.create(NodeKind::Path);
    NodeId c2 = arena.create(NodeKind::Path);
    arena.appendChild(root, c0);
    arena.appendChild(root, c1);
    arena.appendChild(root, c2);

    CHECK(arena.childCount(root) == 3);
    CHECK(arena.firstChild(root) == c0);
    CHECK(arena.lastChild(root) == c2);
    CHECK(arena.nextSibling(c0) == c1);
    CHECK(arena.nextSibling(c1) == c2);
    CHECK(arena.nextSibling(c2) == kInvalidNode);
    CHECK(arena.prevSibling(c0) == kInvalidNode);
    CHECK(arena.prevSibling(c1) == c0);
    CHECK(arena.prevSibling(c2) == c1);
    CHECK(arena.parent(c0) == root);
    CHECK(arena.parent(c1) == root);
    CHECK(arena.parent(c2) == root);
}

TEST_CASE("NodeArena: insertBefore") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId c0 = arena.create(NodeKind::Path);
    NodeId c1 = arena.create(NodeKind::Path);
    arena.appendChild(root, c0);
    arena.appendChild(root, c1);

    // Insert in the middle.
    NodeId mid = arena.create(NodeKind::Path);
    arena.insertBefore(root, mid, c1);
    CHECK(arena.childCount(root) == 3);
    CHECK(arena.nextSibling(c0) == mid);
    CHECK(arena.nextSibling(mid) == c1);
    CHECK(arena.parent(mid) == root);

    // Insert at the very front.
    NodeId front = arena.create(NodeKind::Path);
    arena.insertBefore(root, front, c0);
    CHECK(arena.firstChild(root) == front);
    CHECK(arena.nextSibling(front) == c0);
    CHECK(arena.childCount(root) == 4);

    // before == kInvalidNode behaves like append.
    NodeId tail = arena.create(NodeKind::Path);
    arena.insertBefore(root, tail, kInvalidNode);
    CHECK(arena.lastChild(root) == tail);
    CHECK(arena.childCount(root) == 5);
}

TEST_CASE("NodeArena: detach fixes sibling chain and clears parent") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId c0 = arena.create(NodeKind::Path);
    NodeId c1 = arena.create(NodeKind::Path);
    NodeId c2 = arena.create(NodeKind::Path);
    arena.appendChild(root, c0);
    arena.appendChild(root, c1);
    arena.appendChild(root, c2);

    arena.detach(c1);
    CHECK(arena.parent(c1) == kInvalidNode);
    CHECK(arena.childCount(root) == 2);
    CHECK(arena.nextSibling(c0) == c2);
    CHECK(arena.firstChild(root) == c0);
    CHECK(arena.lastChild(root) == c2);
}

TEST_CASE("NodeArena: destroy removes subtree") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId child = arena.create(NodeKind::Group);
    NodeId grandchild = arena.create(NodeKind::Path);
    arena.appendChild(root, child);
    arena.appendChild(child, grandchild);

    uint32_t before = arena.aliveCount();
    arena.destroy(child);
    CHECK(arena.aliveCount() == before - 2);
    CHECK_FALSE(arena.alive(child));
    CHECK_FALSE(arena.alive(grandchild));
    CHECK(arena.kind(child) == NodeKind::Free);
    CHECK(arena.kind(grandchild) == NodeKind::Free);
    CHECK(arena.childCount(root) == 0);
}

TEST_CASE("NodeArena: slot reuse bumps generation and resets data") {
    NodeArena arena;
    NodeId a = arena.create(NodeKind::Path);
    arena.path(a).verbs.push_back(PathVerb::Move);
    arena.path(a).points.push_back(1.0f);
    uint32_t gen0 = arena.generation(a);

    arena.destroy(a);
    NodeId b = arena.create(NodeKind::Path);

    CHECK(b == a);
    CHECK(arena.generation(b) == gen0 + 1);
    CHECK(arena.path(b).verbs.empty());
    CHECK(arena.path(b).points.empty());
}

TEST_CASE("NodeArena: isAncestorOf") {
    NodeArena arena;
    NodeId grandparent = arena.create(NodeKind::Group);
    NodeId parent = arena.create(NodeKind::Group);
    NodeId self = arena.create(NodeKind::Path);
    NodeId sibling = arena.create(NodeKind::Path);
    arena.appendChild(grandparent, parent);
    arena.appendChild(parent, self);
    arena.appendChild(parent, sibling);

    CHECK(arena.isAncestorOf(grandparent, self));
    CHECK(arena.isAncestorOf(parent, self));
    CHECK_FALSE(arena.isAncestorOf(self, self));
    CHECK_FALSE(arena.isAncestorOf(sibling, self));
}

TEST_CASE("NodeArena: dirty bit propagation") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId mid = arena.create(NodeKind::Group);
    NodeId leaf = arena.create(NodeKind::Path);
    arena.appendChild(root, mid);
    arena.appendChild(mid, leaf);
    arena.clearAllDirty();

    arena.markDirty(leaf, kDirtyStyle);
    CHECK((arena.dirty(leaf) & kDirtyStyle) != 0);
    CHECK((arena.dirty(mid) & kDirtyStyle) != 0);
    CHECK((arena.dirty(root) & kDirtyStyle) != 0);
    arena.clearAllDirty();

    arena.markDirty(root, kDirtyTransform);
    CHECK((arena.dirty(root) & kDirtyTransform) != 0);
    CHECK((arena.dirty(mid) & kDirtyTransform) != 0);
    CHECK((arena.dirty(leaf) & kDirtyTransform) != 0);
    arena.clearAllDirty();

    arena.markDirty(leaf, kDirtyPaint);
    CHECK((arena.dirty(leaf) & kDirtyPaint) != 0);
    CHECK(arena.dirty(mid) == 0);
    CHECK(arena.dirty(root) == 0);
    arena.clearAllDirty();

    arena.markDirty(leaf, kDirtyLayout);
    CHECK((arena.dirty(leaf) & kDirtyLayout) != 0);
    CHECK((arena.dirty(mid) & kDirtyLayout) != 0);
    CHECK((arena.dirty(root) & kDirtyLayout) != 0);

    arena.clearAllDirty();
    CHECK(arena.dirty(root) == 0);
    CHECK(arena.dirty(mid) == 0);
    CHECK(arena.dirty(leaf) == 0);

    NodeId extra = arena.create(NodeKind::Path);
    arena.clearAllDirty();
    arena.appendChild(root, extra);
    CHECK((arena.dirty(root) & (kDirtyTree | kDirtyLayout)) == (kDirtyTree | kDirtyLayout));
}

TEST_CASE("NodeArena: preorder traversal matches creation-order DFS") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId a = arena.create(NodeKind::Group);
    NodeId a0 = arena.create(NodeKind::Path);
    NodeId a1 = arena.create(NodeKind::Path);
    NodeId b = arena.create(NodeKind::Path);
    arena.appendChild(root, a);
    arena.appendChild(a, a0);
    arena.appendChild(a, a1);
    arena.appendChild(root, b);

    std::vector<NodeId> order;
    arena.traversePreorder(root, [&](NodeId id) {
        order.push_back(id);
        return true;
    });
    std::vector<NodeId> expected{root, a, a0, a1, b};
    CHECK(order == expected);
}

TEST_CASE("NodeArena: postorder traversal visits children before parent") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId a = arena.create(NodeKind::Group);
    NodeId a0 = arena.create(NodeKind::Path);
    NodeId b = arena.create(NodeKind::Path);
    arena.appendChild(root, a);
    arena.appendChild(a, a0);
    arena.appendChild(root, b);

    std::vector<NodeId> order;
    arena.traversePostorder(root, [&](NodeId id) { order.push_back(id); });
    std::vector<NodeId> expected{a0, a, b, root};
    CHECK(order == expected);
}

TEST_CASE("NodeArena: preorder false return prunes subtree") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId a = arena.create(NodeKind::Group);
    NodeId a0 = arena.create(NodeKind::Path);
    NodeId b = arena.create(NodeKind::Path);
    arena.appendChild(root, a);
    arena.appendChild(a, a0);
    arena.appendChild(root, b);

    std::vector<NodeId> order;
    arena.traversePreorder(root, [&](NodeId id) {
        order.push_back(id);
        return id != a;  // don't descend into a's children
    });
    std::vector<NodeId> expected{root, a, b};
    CHECK(order == expected);
}

TEST_CASE("NodeArena: Element data defaults") {
    NodeArena arena;
    NodeId elem = arena.create(NodeKind::Element);
    const ElementData& data = arena.element(elem);
    CHECK(data.id == kInvalidString);
    CHECK(data.scene == kInvalidScene);
    CHECK(data.sceneRoot == kInvalidNode);
    CHECK(data.inlineStyle == kInvalidString);
    CHECK(data.classes.empty());
}

TEST_CASE("NodeArena: Camera node create/destroy, default CameraData") {
    NodeArena arena;
    NodeId cam = arena.create(NodeKind::Camera);
    const CameraData& data = arena.camera(cam);
    CHECK(data.kind == 0);
    CHECK(data.useTarget == 1);
    CHECK(data.zoom == doctest::Approx(0));
    CHECK(data.target.x == doctest::Approx(0));
    CHECK(data.target.y == doctest::Approx(0));
    CHECK(data.target.z == doctest::Approx(0));
    CHECK(data.nearZ == doctest::Approx(1));
    CHECK(data.farZ == doctest::Approx(100000));

    arena.destroy(cam);
    CHECK_FALSE(arena.alive(cam));
    CHECK(arena.kind(cam) == NodeKind::Free);

    // Slot reuse: a fresh Camera created afterwards gets default data again.
    NodeId cam2 = arena.create(NodeKind::Camera);
    CHECK(arena.camera(cam2).useTarget == 1);
}

TEST_CASE("NodeArena: effects/effectStops pools are empty by default and only grow") {
    NodeArena arena;
    CHECK(arena.effects().empty());
    CHECK(arena.effectStops().empty());

    EffectInstance fx{};
    fx.kind = EffectKind::DropShadow;
    arena.effects().push_back(fx);
    arena.effectStops().push_back(GradientStop{0.5f, Color{1, 1, 1, 1}});
    CHECK(arena.effects().size() == 1u);
    CHECK(arena.effectStops().size() == 1u);
    CHECK(arena.effects()[0].kind == EffectKind::DropShadow);
}

// Docs/tasks/T-50.md: NodeArena::assignEffects/compactEffects (Docs/design/18-effects.md §18.2).

TEST_CASE("NodeArena::assignEffects: in-place overwrite when kind sequence/count match") {
    NodeArena arena;
    NodeId n = arena.create(NodeKind::Group);

    EffectInstance e0{};
    e0.kind = EffectKind::DropShadow;
    e0.params[0] = 1;
    EffectInstance seq1[] = {e0};
    REQUIRE(arena.assignEffects(n, seq1, {}) == Status::kOk);
    uint32_t offset1 = arena.common(n).effectOffset;
    CHECK(arena.common(n).effectCount == 1u);
    CHECK(arena.effects()[offset1].params[0] == doctest::Approx(1));

    // Same kind sequence, same count, different param -> overwritten in place (offset unchanged).
    EffectInstance e0b{};
    e0b.kind = EffectKind::DropShadow;
    e0b.params[0] = 42;
    EffectInstance seq1b[] = {e0b};
    CHECK(arena.assignEffects(n, seq1b, {}) == Status::kOk);
    CHECK(arena.common(n).effectOffset == offset1);
    CHECK(arena.common(n).effectCount == 1u);
    CHECK(arena.effects()[offset1].params[0] == doctest::Approx(42));
}

TEST_CASE("NodeArena::assignEffects: kind or count change appends a new segment") {
    NodeArena arena;
    NodeId n = arena.create(NodeKind::Group);

    EffectInstance e0{};
    e0.kind = EffectKind::DropShadow;
    EffectInstance seq1[] = {e0};
    REQUIRE(arena.assignEffects(n, seq1, {}) == Status::kOk);
    uint32_t offset1 = arena.common(n).effectOffset;

    // Different kind sequence (Glow instead of DropShadow) -> appended, offset advances.
    EffectInstance e1{};
    e1.kind = EffectKind::Glow;
    EffectInstance seq2[] = {e1};
    REQUIRE(arena.assignEffects(n, seq2, {}) == Status::kOk);
    CHECK(arena.common(n).effectOffset > offset1);
    CHECK(arena.common(n).effectCount == 1u);
    CHECK(arena.effects()[arena.common(n).effectOffset].kind == EffectKind::Glow);

    // Different count (2 vs 1) -> also appended.
    uint32_t offset2 = arena.common(n).effectOffset;
    EffectInstance seq3[] = {e1, e1};
    REQUIRE(arena.assignEffects(n, seq3, {}) == Status::kOk);
    CHECK(arena.common(n).effectOffset > offset2);
    CHECK(arena.common(n).effectCount == 2u);
}

TEST_CASE("NodeArena::assignEffects: more than 16 effects is kInvalidArgument, node untouched") {
    NodeArena arena;
    NodeId n = arena.create(NodeKind::Group);
    EffectInstance many[17]{};
    for (EffectInstance& e : many) {
        e.kind = EffectKind::Blur;
    }
    CHECK(arena.assignEffects(n, many, {}) == Status::kInvalidArgument);
    CHECK(arena.common(n).effectCount == 0u);
}

TEST_CASE("NodeArena::assignEffects: GradientMask stop rebasing, in place and on append") {
    NodeArena arena;
    NodeId n = arena.create(NodeKind::Path);

    GradientStop stopsA[] = {{0, Color{1, 0, 0, 1}}, {1, Color{0, 1, 0, 1}}};
    EffectInstance gm{};
    gm.kind = EffectKind::GradientMask;
    gm.stopOffset = 0;
    gm.stopCount = 2;
    EffectInstance seq[] = {gm};
    REQUIRE(arena.assignEffects(n, seq, stopsA) == Status::kOk);
    uint32_t stopOffset1 = arena.effects()[arena.common(n).effectOffset].stopOffset;
    CHECK(arena.effectStops()[stopOffset1].offset == doctest::Approx(0));
    CHECK(arena.effectStops()[stopOffset1 + 1].offset == doctest::Approx(1));

    // Same kind/count, same stop count -> in place: effect offset AND stop offset unchanged,
    // stops overwritten.
    GradientStop stopsB[] = {{0.25f, Color{0, 0, 1, 1}}, {0.75f, Color{1, 1, 0, 1}}};
    EffectInstance gmB = gm;
    EffectInstance seqB[] = {gmB};
    uint32_t offsetBefore = arena.common(n).effectOffset;
    REQUIRE(arena.assignEffects(n, seqB, stopsB) == Status::kOk);
    CHECK(arena.common(n).effectOffset == offsetBefore);
    uint32_t stopOffset2 = arena.effects()[arena.common(n).effectOffset].stopOffset;
    CHECK(stopOffset2 == stopOffset1);
    CHECK(arena.effectStops()[stopOffset2].offset == doctest::Approx(0.25f));

    // Different stop count -> that slot's stops are appended instead (the effect slot itself
    // stays in place, since the kind sequence/count are unchanged).
    GradientStop stopsC[] = {{0, Color{}}, {0.5f, Color{}}, {1, Color{}}};
    EffectInstance gmC = gm;
    gmC.stopCount = 3;
    EffectInstance seqC[] = {gmC};
    uint32_t stopPoolSizeBefore = static_cast<uint32_t>(arena.effectStops().size());
    REQUIRE(arena.assignEffects(n, seqC, stopsC) == Status::kOk);
    CHECK(arena.common(n).effectOffset == offsetBefore);
    const EffectInstance& e = arena.effects()[arena.common(n).effectOffset];
    CHECK(e.stopCount == 3u);
    CHECK(e.stopOffset == stopPoolSizeBefore);
    CHECK(arena.effectStops()[e.stopOffset + 1].offset == doctest::Approx(0.5f));
}

TEST_CASE("NodeArena::compactEffects: rebuilds the pool to exactly the alive nodes' segments") {
    NodeArena arena;
    NodeId a = arena.create(NodeKind::Group);
    NodeId b = arena.create(NodeKind::Group);

    EffectInstance e0{};
    e0.kind = EffectKind::DropShadow;
    e0.params[0] = 1;
    EffectInstance seqA1[] = {e0};
    REQUIRE(arena.assignEffects(a, seqA1, {}) == Status::kOk);

    EffectInstance e1{};
    e1.kind = EffectKind::Glow;
    e1.params[0] = 2;
    EffectInstance seqB[] = {e1};
    REQUIRE(arena.assignEffects(b, seqB, {}) == Status::kOk);

    // Reassign a's sequence to a different kind -> appends, leaving a's old segment as a hole.
    EffectInstance e2{};
    e2.kind = EffectKind::Blur;
    e2.params[0] = 3;
    EffectInstance seqA2[] = {e2};
    REQUIRE(arena.assignEffects(a, seqA2, {}) == Status::kOk);
    CHECK(arena.effects().size() == 3u);  // seqA1 + seqB + seqA2: one hole left by seqA1.

    arena.compactEffects();
    CHECK(arena.effects().size() == 2u);  // Exactly a's + b's live segments.
    CHECK(arena.effects()[arena.common(a).effectOffset].kind == EffectKind::Blur);
    CHECK(arena.effects()[arena.common(a).effectOffset].params[0] == doctest::Approx(3));
    CHECK(arena.effects()[arena.common(b).effectOffset].kind == EffectKind::Glow);
    CHECK(arena.effects()[arena.common(b).effectOffset].params[0] == doctest::Approx(2));
}

TEST_CASE("LayoutBox: paddingBox and contentBox") {
    LayoutBox box;
    box.width = 10;
    box.height = 10;
    box.border = Edges{1, 1, 1, 1};
    box.padding = Edges{2, 2, 2, 2};

    Rect pb = box.paddingBox();
    CHECK(pb.x == 1);
    CHECK(pb.y == 1);
    CHECK(pb.w == 8);
    CHECK(pb.h == 8);

    Rect cb = box.contentBox();
    CHECK(cb.x == 3);
    CHECK(cb.y == 3);
    CHECK(cb.w == 4);
    CHECK(cb.h == 4);
}

TEST_CASE("NodeArena: deep chain preorder traversal does not recurse/crash") {
    NodeArena arena;
    constexpr int kDepth = 10000;
    NodeId root = arena.create(NodeKind::Group);
    NodeId cur = root;
    for (int i = 0; i < kDepth; ++i) {
        NodeId child = arena.create(NodeKind::Group);
        arena.appendChild(cur, child);
        cur = child;
    }

    uint32_t count = 0;
    arena.traversePreorder(root, [&](NodeId) {
        ++count;
        return true;
    });
    CHECK(count == static_cast<uint32_t>(kDepth + 1));
}
