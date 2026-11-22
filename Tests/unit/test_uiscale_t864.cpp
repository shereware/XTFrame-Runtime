#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-864: two run-time writes of logical px measured under uiscale != 1, following T-863. The `px`
/// of a style is design px that the cascade multiplies by the element's factor
/// (Docs/design/25-editor-blueprint.md §25.4.2): a `spawn()` instance's top-left goes to `at`, a
/// viewport (logical) point (03 §3.12.8, 27 §27.2.3), and a virtualized `<repeat>`'s spacers stand
/// in for the rows that are not instantiated (27 §27.3.2), so both are measured in logical px.

using namespace xt;

namespace {

std::vector<uint8_t> compileDoc(const std::string& head, const std::string& body,
                                const std::string& bodyAttrs) {
    const std::string text = "<xt><head>" + head + "</head><body" + bodyAttrs + ">" + body + "</body></xt>";
    const compile::SourceFile file{"t864.xt", text};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "t864.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

/// One Context + one Document in a 1000 x 800 viewport at uiscale `scale`.
struct Scaled {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    DocumentHandle handle = 0;
    Document* doc = nullptr;

    Scaled(const std::vector<uint8_t>& ir, float scale) {
        Result<DocumentHandle> h = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(h.ok());
        handle = h.value();
        doc = ctx.document(handle);
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 1000, 800}, 1.0f);
        REQUIRE(ctx.setUiScale(scale) == Status::kOk);
        ctx.update(0.0f);
    }

    void step() {
        for (int i = 0; i < 4; ++i) {
            ctx.update(0.016f);
        }
    }
    NodeId node(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
    const LayoutBox& box(const char* id) { return doc->arena().element(node(id)).box; }
    Vec2 screen(const char* id) { return doc->nodeToScreen(node(id), Vec2{0, 0}); }
    PortId port(const char* name) {
        const PortId p = doc->lookupPort(name);
        REQUIRE(p != kInvalidPort);
        return p;
    }
    void scrollTo(float y) {
        setProperty(doc->arena(), node("Scroll"), prop::ScrollY, Value::ofFloat(y), DirtyCause::HostApi);
        step();
    }
};

constexpr float kScales[] = {1.0f, 0.888f, 1.5f};

// -------------------------------------------------------------------------------------------
// 1. spawn(): the instance's top-left lands on `at` (a viewport point).
// -------------------------------------------------------------------------------------------

const char* kSpawnHead = R"X(
      <symbol name="Pop"><div id="Box"></div></symbol>
      <style>
        #Field { position: absolute; left: 20px; top: 30px; width: 100px; height: 50px; }
        #Mark { position: absolute; left: 300px; top: 200px; width: 40px; height: 60px; }
        symbol(Pop) { #Box { width: 10px; height: 10px; } }
      </style>)X";

const char* kSpawnBody = R"X(
      <div id="Field" on:port(hit)="spawn(Pop, at=self, lifetime=5s)"
                      on:port(far)="spawn(Pop, at=#Mark, lifetime=5s)"></div>
      <div id="Mark"></div>)X";

const char* kSpawnAttrs = R"X( id="Root" port:hit="event in" port:far="event in")X";

}  // namespace

TEST_CASE("T-864: spawn() puts the instance's top-left on `at` at any uiscale") {
    const std::vector<uint8_t> ir = compileDoc(kSpawnHead, kSpawnBody, kSpawnAttrs);
    for (const float s : kScales) {
        CAPTURE(s);
        Scaled l(ir, s);
        REQUIRE(l.doc->setPort(l.port("hit"), Value::ofFloat(0)) == Status::kOk);
        l.step();
        // at=self: the centre of #Field's box, (20 + 50, 30 + 25) design px.
        Vec2 o = l.screen("spawn-0/0/spawn-0-Pop");
        CHECK(o.x == doctest::Approx(70 * s));
        CHECK(o.y == doctest::Approx(55 * s));
        // at=#Mark: the centre of another element, (300 + 20, 200 + 30) design px.
        REQUIRE(l.doc->setPort(l.port("far"), Value::ofFloat(0)) == Status::kOk);
        l.step();
        o = l.screen("spawn-0/1/spawn-0-Pop");
        CHECK(o.x == doctest::Approx(320 * s));
        CHECK(o.y == doctest::Approx(230 * s));
    }
}

TEST_CASE("T-864: a recycled spawn instance lands on `at` too") {
    const std::vector<uint8_t> ir = compileDoc(kSpawnHead, kSpawnBody, kSpawnAttrs);
    Scaled l(ir, 1.5f);
    REQUIRE(l.doc->setPort(l.port("hit"), Value::ofFloat(0)) == Status::kOk);
    l.step();
    for (int i = 0; i < 400; ++i) {
        l.ctx.update(0.016f);  // Past the 5 s lifetime: back into the pool.
    }
    REQUIRE(l.doc->repeatInstances("spawn-0").empty());
    REQUIRE(l.doc->setPort(l.port("far"), Value::ofFloat(0)) == Status::kOk);
    l.step();
    const Vec2 o = l.screen("spawn-0/1/spawn-0-Pop");
    CHECK(o.x == doctest::Approx(320 * 1.5f));
    CHECK(o.y == doctest::Approx(230 * 1.5f));
}

// -------------------------------------------------------------------------------------------
// 2. A virtualized <repeat>: the spacers are (rows not instantiated) x the row's actual height.
// -------------------------------------------------------------------------------------------

namespace {

const char* kVirtualBody = R"X(
    <div id="P" port:items="list<struct{name: string}>"></div>
    <div id="Scroll" style="position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: scroll">
      <repeat port="items" id="Item" virtualize>
        <div id="Row" style="height: 20px"><span id="Name" bind:text="item(name)"></span></div>
      </repeat>
    </div>)X";

std::string manyEntries(int n) {
    std::string json = "[";
    for (int i = 0; i < n; ++i) {
        json += (i > 0 ? "," : "") + std::string("{\"name\":\"R") + std::to_string(i) + "\"}";
    }
    return json + "]";
}

/// The spacer heights and the instantiated window of the virtualized repeat "Item".
struct Window {
    float before = 0;
    float after = 0;
    uint32_t first = 0;
    uint32_t count = 0;
};

Window window(Scaled& l) {
    Window w;
    const NodeArena& arena = l.doc->arena();
    const NodeId rep = l.node("Item");
    const NodeId first = arena.firstChild(rep);
    const NodeId last = arena.lastChild(rep);
    REQUIRE(first != kInvalidNode);
    REQUIRE(last != kInvalidNode);
    w.before = arena.element(first).box.height;
    w.after = arena.element(last).box.height;
    const std::span<const ElementRef> inst = l.doc->repeatInstances("Item");
    w.count = static_cast<uint32_t>(inst.size());
    if (!inst.empty()) {
        const std::string id = std::string(inst.front().id());  // "Item/<n>/Row"
        w.first = static_cast<uint32_t>(std::stoul(id.substr(5)));
    }
    return w;
}

void checkSpacers(Scaled& l, float s, uint32_t n) {
    const Window w = window(l);
    CAPTURE(w.first);
    CAPTURE(w.count);
    const float row = 20 * s;
    CHECK(w.before == doctest::Approx(static_cast<float>(w.first) * row));
    CHECK(w.after == doctest::Approx(static_cast<float>(n - w.first - w.count) * row));
    CHECK(l.box("Scroll").scrollHeight == doctest::Approx(static_cast<float>(n) * row));
}

}  // namespace

TEST_CASE("T-864: a virtualized repeat's spacers follow the actual row height at any uiscale") {
    const std::vector<uint8_t> ir = compileDoc("", kVirtualBody, "");
    for (const float s : kScales) {
        CAPTURE(s);
        Scaled l(ir, s);
        REQUIRE(l.doc->setPortText(l.port("items"), manyEntries(200)) == Status::kOk);
        l.step();
        checkSpacers(l, s, 200);
        l.scrollTo(2000 * s);  // Rows 100..110 visible.
        checkSpacers(l, s, 200);
        CHECK(l.doc->byId("Item/100/Row").valid());
        CHECK(l.box("Item/100/Row").y == doctest::Approx(2000 * s));
        // Scrolled to the bottom: the last row ends where the content ends.
        const float maxScroll = l.box("Scroll").scrollHeight - l.box("Scroll").paddingBox().h;
        CHECK(maxScroll == doctest::Approx((4000 - 200) * s));
        l.scrollTo(maxScroll);
        checkSpacers(l, s, 200);
        REQUIRE(l.doc->byId("Item/199/Row").valid());
        const LayoutBox& lastRow = l.box("Item/199/Row");
        CHECK(lastRow.y + lastRow.height == doctest::Approx(4000 * s));
        CHECK(window(l).after == doctest::Approx(0));
    }
}

TEST_CASE("T-864: a uiscale change while running re-measures the virtualized rows") {
    const std::vector<uint8_t> ir = compileDoc("", kVirtualBody, "");
    Scaled l(ir, 1.0f);
    REQUIRE(l.doc->setPortText(l.port("items"), manyEntries(200)) == Status::kOk);
    l.step();
    l.scrollTo(1000);  // Rows 50..60 visible at 20 px.
    checkSpacers(l, 1.0f, 200);
    for (const float s : {1.5f, 0.888f}) {
        CAPTURE(s);
        REQUIRE(l.ctx.setUiScale(s) == Status::kOk);
        l.step();
        checkSpacers(l, s, 200);
        // The window covers what the viewport shows at the new row height.
        const float scrollY = l.doc->arena().element(l.node("Scroll")).style.base[prop::ScrollY].f;
        const Window w = window(l);
        const float vpH = l.box("Scroll").paddingBox().h;
        CHECK(static_cast<float>(w.first) * 20 * s <= scrollY + 0.01f);
        CHECK(static_cast<float>(w.first + w.count) * 20 * s >= scrollY + vpH - 0.01f);
    }
}
