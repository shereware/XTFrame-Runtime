#include <algorithm>
#include <vector>

#include "doctest.h"
#include "selector_match.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// Tests for selector_match.h's matchRule(): tag/class/id/wildcard/state part matching, child and
/// descendant combinators, and descendant-combinator backtracking. See Docs/design/
/// 05-style-and-css.md §5.3, §5.7, Docs/tasks/T-24.md "test_selector_match.cpp".
///
/// matchRule() never dereferences string content (tag/id/class equality is plain StringId
/// equality) and the IrView parameter is unused by the implementation, so these tests use a bare
/// NodeArena with hand-picked StringId constants instead of a full compiled document; `emptyIr()`
/// supplies the (otherwise-unused) IrView the signature requires.

using namespace xt;
using namespace xt::compile;

namespace {

// Arbitrary distinct "StringId" constants; no string table involved (see file comment).
constexpr StringId kTagA = 1, kTagB = 2, kTagC = 3, kTagX = 4, kTagM = 5, kTagOther = 6;
constexpr StringId kIdRoot = 100, kIdOther = 101;
constexpr StringId kClassFoo = 200, kClassBar = 201, kClassBaz = 202;

const IrView& emptyIr() {
    static std::vector<uint8_t> bytes = IrWriter{}.finish();
    static Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    return view.value();
}

NodeId makeElement(NodeArena& arena, NodeId parent, StringId tag, StringId id = kInvalidString,
                   std::vector<StringId> classes = {}, uint16_t stateBits = 0) {
    NodeId n = arena.create(NodeKind::Element);
    ElementData& data = arena.element(n);
    data.tag = tag;
    data.id = id;
    std::sort(classes.begin(), classes.end());
    data.classes = std::move(classes);
    data.stateBits = stateBits;
    if (parent != kInvalidNode) {
        arena.appendChild(parent, n);
    }
    return n;
}

IrSelPart part(uint8_t combinator, StringId tag = kInvalidString, StringId id = kInvalidString,
               uint16_t stateMask = 0, uint8_t classCount = 0, uint32_t classOffset = 0) {
    IrSelPart p{};
    p.combinator = combinator;
    p.tag = tag;
    p.id = id;
    p.stateMask = stateMask;
    p.classCount = classCount;
    p.classOffset = classOffset;
    return p;
}

IrRule ruleOf(uint32_t selOffset, uint32_t selCount) {
    IrRule r{};
    r.selOffset = selOffset;
    r.selCount = selCount;
    r.declOffset = 0;
    r.declCount = 0;
    r.specificity = 0;
    return r;
}

}  // namespace

TEST_CASE("selector_match: tag, wildcard, id, class, state single-part matching") {
    NodeArena arena;
    NodeId div = makeElement(arena, kInvalidNode, kTagA, kIdRoot, {kClassFoo, kClassBar}, kStateHover);

    std::vector<uint32_t> classIds = {kClassFoo, kClassBar, kClassBaz};

    SUBCASE("tag matches") {
        std::vector<IrSelPart> parts = {part(0, kTagA)};
        IrRule r = ruleOf(0, 1);
        CHECK(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("wrong tag fails") {
        std::vector<IrSelPart> parts = {part(0, kTagB)};
        IrRule r = ruleOf(0, 1);
        CHECK_FALSE(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("wildcard tag matches anything") {
        std::vector<IrSelPart> parts = {part(0, kInvalidString)};
        IrRule r = ruleOf(0, 1);
        CHECK(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("id matches") {
        std::vector<IrSelPart> parts = {part(0, kInvalidString, kIdRoot)};
        IrRule r = ruleOf(0, 1);
        CHECK(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("wrong id fails") {
        std::vector<IrSelPart> parts = {part(0, kInvalidString, kIdOther)};
        IrRule r = ruleOf(0, 1);
        CHECK_FALSE(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("both classes present matches") {
        std::vector<IrSelPart> parts = {part(0, kInvalidString, kInvalidString, 0, 2, 0)};
        IrRule r = ruleOf(0, 1);
        CHECK(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("missing class fails") {
        std::vector<IrSelPart> parts = {part(0, kInvalidString, kInvalidString, 0, 1, 2)};  // .baz
        IrRule r = ruleOf(0, 1);
        CHECK_FALSE(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("state subset matches") {
        std::vector<IrSelPart> parts = {part(0, kInvalidString, kInvalidString, kStateHover)};
        IrRule r = ruleOf(0, 1);
        CHECK(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("state not set fails") {
        std::vector<IrSelPart> parts = {part(0, kInvalidString, kInvalidString, kStateFocus)};
        IrRule r = ruleOf(0, 1);
        CHECK_FALSE(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
    SUBCASE("state requiring hover+focus fails when only hover set") {
        std::vector<IrSelPart> parts = {
            part(0, kInvalidString, kInvalidString, static_cast<uint16_t>(kStateHover | kStateFocus))};
        IrRule r = ruleOf(0, 1);
        CHECK_FALSE(matchRule(arena, emptyIr(), div, r, parts, classIds));
    }
}

TEST_CASE("selector_match: descendant combinator through intervening ancestors") {
    // a > x > b > y > c ("a b c" should match: every part just needs SOME ancestor of the
    // matching type, regardless of what's between).
    NodeArena arena;
    NodeId a = makeElement(arena, kInvalidNode, kTagA);
    NodeId x = makeElement(arena, a, kTagX);
    NodeId b = makeElement(arena, x, kTagB);
    NodeId y = makeElement(arena, b, kTagX);
    NodeId c = makeElement(arena, y, kTagC);
    (void)b;

    std::vector<uint32_t> classIds;
    std::vector<IrSelPart> parts = {part(0, kTagC), part(1, kTagB), part(1, kTagA)};
    IrRule r = ruleOf(0, 3);
    CHECK(matchRule(arena, emptyIr(), c, r, parts, classIds));
}

TEST_CASE("selector_match: descendant combinator, two candidate ancestors of the same type") {
    // a > b > x > b > c ("a b c" should still match).
    NodeArena arena;
    NodeId a = makeElement(arena, kInvalidNode, kTagA);
    NodeId b1 = makeElement(arena, a, kTagB);
    NodeId x = makeElement(arena, b1, kTagX);
    NodeId b2 = makeElement(arena, x, kTagB);
    NodeId c = makeElement(arena, b2, kTagC);

    std::vector<uint32_t> classIds;
    std::vector<IrSelPart> parts = {part(0, kTagC), part(1, kTagB), part(1, kTagA)};
    IrRule r = ruleOf(0, 3);
    CHECK(matchRule(arena, emptyIr(), c, r, parts, classIds));
}

TEST_CASE("selector_match: child combinator requires the direct parent") {
    NodeArena arena;
    NodeId a = makeElement(arena, kInvalidNode, kTagA);
    NodeId b = makeElement(arena, a, kTagB);
    NodeId x = makeElement(arena, a, kTagX);
    NodeId bIndirect = makeElement(arena, x, kTagB);

    std::vector<uint32_t> classIds;
    std::vector<IrSelPart> parts = {part(0, kTagB), part(2, kTagA)};  // "a > b"
    IrRule r = ruleOf(0, 2);
    CHECK(matchRule(arena, emptyIr(), b, r, parts, classIds));
    CHECK_FALSE(matchRule(arena, emptyIr(), bIndirect, r, parts, classIds));
}

TEST_CASE("selector_match: mixed child+descendant backtracks over descendant choices") {
    // Selector "a > b c" (rightmost-first parts: c subject, descendant b, child a).
    std::vector<uint32_t> classIds;
    std::vector<IrSelPart> parts = {part(0, kTagC), part(1, kTagB), part(2, kTagA)};
    IrRule r = ruleOf(0, 3);

    SUBCASE("succeeds: the farther b (not the nearest) is a's direct child") {
        // a -> b1(child of a) -> m -> b2 -> c. Nearest b to c is b2 (parent(b2) = m, not a);
        // backtracking must retry with b1, whose direct parent IS a.
        NodeArena arena;
        NodeId a = makeElement(arena, kInvalidNode, kTagA);
        NodeId b1 = makeElement(arena, a, kTagB);
        NodeId m = makeElement(arena, b1, kTagM);
        NodeId b2 = makeElement(arena, m, kTagB);
        NodeId c = makeElement(arena, b2, kTagC);
        CHECK(matchRule(arena, emptyIr(), c, r, parts, classIds));
    }
    SUBCASE("fails: neither candidate b is a's direct child, backtracking exhausts all choices") {
        // a -> m -> b1 -> x -> b2 -> c. Neither b1 nor b2's direct parent is a.
        NodeArena arena;
        NodeId a = makeElement(arena, kInvalidNode, kTagA);
        NodeId m = makeElement(arena, a, kTagM);
        NodeId b1 = makeElement(arena, m, kTagB);
        NodeId x = makeElement(arena, b1, kTagX);
        NodeId b2 = makeElement(arena, x, kTagB);
        NodeId c = makeElement(arena, b2, kTagC);
        CHECK_FALSE(matchRule(arena, emptyIr(), c, r, parts, classIds));
    }
}

TEST_CASE("selector_match: subject part must itself match") {
    NodeArena arena;
    NodeId a = makeElement(arena, kInvalidNode, kTagA);
    NodeId b = makeElement(arena, a, kTagB);
    std::vector<uint32_t> classIds;
    // Subject part requires kTagOther; `b` is kTagB, so the rule never even reaches the ancestor
    // parts, regardless of what's above it.
    std::vector<IrSelPart> parts = {part(0, kTagOther), part(1, kTagA)};
    IrRule r = ruleOf(0, 2);
    CHECK_FALSE(matchRule(arena, emptyIr(), b, r, parts, classIds));
}
