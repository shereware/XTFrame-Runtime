#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// T-336 (Docs/design/05-style-and-css.md §5.9.8, 06 §6.5 / §6.9, 08 §8.4): `overflow: auto`,
/// `overflow-x` / `overflow-y` (clip, scroll sizes, per-axis wheel), flex `wrap-reverse` and
/// `order` (layout, paint order, hit order, the order cache), against the hand-computed snapshots
/// next to each fixture (Tests/layout/overflow_axes.json, flex_wrap_reverse.json,
/// flex_order.json).

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string s = text.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

void checkFixture(const std::string& stem) {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(stem + ".xt", &shaper, &assets);
    const std::string expected = readFile(layoutPath(stem + ".json"));
    REQUIRE(!expected.empty());
    LayoutEngine engine;
    engine.init(doc->ir(), &assets);
    CHECK(engine.snapshot(doc->arena(), doc->root().node) == expected);
}

InputEvent wheel(Vec2 pos, Vec2 delta) {
    InputEvent ev;
    ev.kind = InputEvent::Wheel;
    ev.pos = pos;
    ev.wheelDelta = delta;
    return ev;
}

const ElementData& elementOf(Document& doc, const char* id) {
    ElementRef ref = doc.byId(id);
    REQUIRE(ref.valid());
    return doc.arena().element(ref.node);
}

float scrollOf(Document& doc, const char* id, PropId axis) { return elementOf(doc, id).style.base[axis].f; }

}  // namespace

TEST_CASE("T-336: overflow snapshot -- auto fits / overflows, x and y apart, two-value overflow") {
    checkFixture("overflow_axes");
}

TEST_CASE("T-336: wrap-reverse stacks the lines from the cross end; align-content and margins flip") {
    checkFixture("flex_wrap_reverse");
}

TEST_CASE("T-336: order reorders three items stably; an absolute child keeps its place") {
    checkFixture("flex_order");
}

TEST_CASE("T-336: used overflow, scroll sizes and the clip per axis") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("overflow_axes.xt", &shaper, &assets);

    const ElementData& fits = elementOf(*doc, "fits");
    CHECK(fits.style.overflowX == Overflow::Auto);
    CHECK(fits.style.overflowY == Overflow::Auto);
    CHECK(maxScrollX(fits.box) == doctest::Approx(0));
    CHECK(maxScrollY(fits.box) == doctest::Approx(0));
    CHECK_FALSE(clipsOverflow(fits));  // auto that fits: no clip.

    const ElementData& tall = elementOf(*doc, "tall");
    CHECK(maxScrollY(tall.box) == doctest::Approx(220));  // 300 - the 80 px content box.
    CHECK(clipsOverflow(tall));

    const ElementData& yOnly = elementOf(*doc, "y-only");
    CHECK(usedOverflowX(yOnly.style) == Overflow::Hidden);
    CHECK(usedOverflowY(yOnly.style) == Overflow::Scroll);
    CHECK(maxScrollX(yOnly.box) == doctest::Approx(100));
    CHECK(clipsOverflow(yOnly));

    // A `visible` axis beside a non-visible one is `auto`.
    const ElementData& xOnly = elementOf(*doc, "x-only");
    CHECK(xOnly.style.overflowY == Overflow::Visible);
    CHECK(usedOverflowY(xOnly.style) == Overflow::Auto);
    CHECK(maxScrollX(xOnly.box) == doctest::Approx(150));
    CHECK(maxScrollY(xOnly.box) == doctest::Approx(0));
    CHECK(clipsOverflow(xOnly));

    // A flex container's scroll size is its items' extent.
    const ElementData& pair = elementOf(*doc, "pair");
    CHECK(pair.style.overflowX == Overflow::Scroll);
    CHECK(pair.style.overflowY == Overflow::Hidden);
    CHECK(maxScrollX(pair.box) == doctest::Approx(60));
    CHECK(maxScrollY(pair.box) == doctest::Approx(50));

    // The Painter clips every box but `fits`, each to its whole padding box.
    CommandList cmds;
    doc->paint(cmds);
    CHECK(cmds.clipRects.size() == 4);
    for (const ClipRectCmd& clip : cmds.clipRects) {
        CHECK(clip.rect.w == doctest::Approx(100));
        CHECK(clip.rect.h == doctest::Approx(100));
    }
}

TEST_CASE("T-336: the wheel moves only the live axes, each clamped on its own") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("overflow_axes.xt", &shaper, &assets);

    // overflow-x: hidden, overflow-y: scroll -- x never takes the wheel.
    doc->dispatchInput(wheel(Vec2{270, 10}, Vec2{1, 1}));
    CHECK(scrollOf(*doc, "y-only", prop::ScrollY) == doctest::Approx(40));
    CHECK(scrollOf(*doc, "y-only", prop::ScrollX) == doctest::Approx(0));

    // overflow-x: auto (overflowing), y visible -> auto that fits: only x moves.
    const float childX = doc->arena().common(doc->byId("x-only-c").node).world.e;
    doc->dispatchInput(wheel(Vec2{10, 160}, Vec2{1, 1}));
    doc->update(0.0f);
    CHECK(scrollOf(*doc, "x-only", prop::ScrollX) == doctest::Approx(40));
    CHECK(scrollOf(*doc, "x-only", prop::ScrollY) == doctest::Approx(0));
    CHECK(doc->arena().common(doc->byId("x-only-c").node).world.e == doctest::Approx(childX - 40));

    // overflow: scroll hidden -- x clamps at 60, y stays.
    doc->dispatchInput(wheel(Vec2{160, 160}, Vec2{10, 10}));
    CHECK(scrollOf(*doc, "pair", prop::ScrollX) == doctest::Approx(60));
    CHECK(scrollOf(*doc, "pair", prop::ScrollY) == doctest::Approx(0));

    // A hidden axis can still be written through the API; it does not offset the children.
    const float pairChildY = doc->arena().common(doc->byId("pair-a").node).world.f;
    doc->byId("pair").setProperty(prop::ScrollY, Value::ofFloat(30));
    doc->update(0.0f);
    CHECK(scrollOf(*doc, "pair", prop::ScrollY) == doctest::Approx(30));
    CHECK(doc->arena().common(doc->byId("pair-a").node).world.f == doctest::Approx(pairChildY));

    // auto with padding: clamped to the content past the content box.
    doc->dispatchInput(wheel(Vec2{130, 10}, Vec2{0, 100}));
    CHECK(scrollOf(*doc, "tall", prop::ScrollY) == doctest::Approx(220));
}

TEST_CASE("T-336: overflow: auto takes the wheel only once its content overflows") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("overflow_wheel.xt", &shaper, &assets);

    doc->dispatchInput(wheel(Vec2{20, 20}, Vec2{0, 1}));  // Over #inner, which fits.
    doc->update(0.0f);
    CHECK(scrollOf(*doc, "inner", prop::ScrollY) == doctest::Approx(0));
    CHECK(scrollOf(*doc, "outer", prop::ScrollY) == doctest::Approx(40));
    CHECK_FALSE(clipsOverflow(elementOf(*doc, "inner")));

    REQUIRE(doc->byId("inner-c").setStyle("height: 200px") == Status::kOk);
    doc->update(0.0f);
    CHECK(clipsOverflow(elementOf(*doc, "inner")));
    doc->dispatchInput(wheel(Vec2{20, 20}, Vec2{0, 1}));
    CHECK(scrollOf(*doc, "inner", prop::ScrollY) == doctest::Approx(40));
    CHECK(scrollOf(*doc, "outer", prop::ScrollY) == doctest::Approx(40));
}

TEST_CASE("T-336: order drives paint order and hit order") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("flex_order.xt", &shaper, &assets);
    ElementRef p1 = doc->byId("p1");  // red, order 1, x 50-150
    ElementRef p2 = doc->byId("p2");  // green, order 0, x 0-100
    REQUIRE(p1.valid());
    REQUIRE(p2.valid());

    CommandList cmds;
    doc->paint(cmds);
    int red = -1;
    int green = -1;
    for (size_t i = 0; i < cmds.fills.size(); ++i) {
        const Color c = cmds.fills[i].paint.color;
        if (c.r > 0.9f && c.g < 0.1f) {
            red = static_cast<int>(i);
        } else if (c.g > 0.9f && c.r < 0.1f) {
            green = static_cast<int>(i);
        }
    }
    REQUIRE(red >= 0);
    REQUIRE(green >= 0);
    CHECK(green < red);  // Order-modified document order: p2 first, p1 on top.

    CHECK(doc->hitTest(Vec2{75, 75}) == p1.node);  // The overlap goes to the later item.
    CHECK(doc->hitTest(Vec2{25, 75}) == p2.node);

    // Back to document order: p1 is laid out and drawn first again (p2's negative margin now
    // overlaps nothing).
    REQUIRE(p1.setStyle("order: 0") == Status::kOk);
    doc->update(0.0f);
    CommandList again;
    doc->paint(again);
    red = -1;
    green = -1;
    for (size_t i = 0; i < again.fills.size(); ++i) {
        const Color c = again.fills[i].paint.color;
        if (c.r > 0.9f && c.g < 0.1f) {
            red = static_cast<int>(i);
        } else if (c.g > 0.9f && c.r < 0.1f) {
            green = static_cast<int>(i);
        }
    }
    CHECK(red < green);
    CHECK(doc->hitTest(Vec2{75, 75}) == p1.node);
    CHECK(doc->hitTest(Vec2{150, 75}) == p2.node);
}

TEST_CASE("T-336: the order cache follows order and child changes") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("flex_order.xt", &shaper, &assets);

    const ElementData& row = elementOf(*doc, "row");
    // Items a(2) b(0) c(-1) d(2); the absolute child is not an item.
    CHECK(row.flexOrder.items.size() == 4);
    CHECK(row.flexOrder.permutation == std::vector<uint32_t>{2, 1, 0, 3});

    ElementRef c = doc->byId("o-c");
    REQUIRE(c.setStyle("order: 3") == Status::kOk);
    CHECK(c.setStyle("order: 1.5") == Status::kInvalidArgument);
    doc->update(0.0f);
    CHECK(row.flexOrder.permutation == std::vector<uint32_t>{1, 0, 3, 2});
    CHECK(elementOf(*doc, "o-b").box.x == doctest::Approx(0));
    CHECK(elementOf(*doc, "o-c").box.x == doctest::Approx(150));

    REQUIRE(doc->byId("o-b").setStyle("display: none") == Status::kOk);
    doc->update(0.0f);
    CHECK(row.flexOrder.items.size() == 3);
    CHECK(row.flexOrder.permutation == std::vector<uint32_t>{0, 2, 1});  // a, d, c.
    CHECK(elementOf(*doc, "o-a").box.x == doctest::Approx(0));

    // Every item back at order 0: nothing is sorted, the cache is empty.
    const ElementData& pRow = elementOf(*doc, "p-row");
    CHECK(pRow.flexOrder.permutation.size() == 2);
    REQUIRE(doc->byId("p1").setStyle("order: 0") == Status::kOk);
    doc->update(0.0f);
    CHECK(pRow.flexOrder.permutation.empty());
    CHECK(elementOf(*doc, "p1").box.x == doctest::Approx(0));
}

TEST_CASE("T-336: setStyle overflow / overflow-x / overflow-y / flex-wrap values") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("overflow_axes.xt", &shaper, &assets);
    ElementRef fits = doc->byId("fits");
    REQUIRE(fits.setStyle("overflow: hidden scroll") == Status::kOk);
    doc->update(0.0f);
    CHECK(elementOf(*doc, "fits").style.overflowX == Overflow::Hidden);
    CHECK(elementOf(*doc, "fits").style.overflowY == Overflow::Scroll);
    REQUIRE(fits.setStyle("overflow-y: auto") == Status::kOk);
    doc->update(0.0f);
    CHECK(elementOf(*doc, "fits").style.overflowY == Overflow::Auto);
    CHECK(fits.setStyle("overflow: clip") == Status::kInvalidArgument);
    REQUIRE(fits.setStyle("flex-wrap: wrap-reverse") == Status::kOk);
    doc->update(0.0f);
    CHECK(elementOf(*doc, "fits").style.flexWrap == FlexWrap::WrapReverse);
}
