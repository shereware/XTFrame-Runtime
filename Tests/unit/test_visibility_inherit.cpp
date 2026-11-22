#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-867 item 3: `visibility` is inherited (Docs/design/05-style-and-css.md §5.4, the `visibility`
/// row: 繼承 = 是), like CSS: a `visibility: hidden` element hides its descendants unless one of them
/// declares `visibility: visible`; hit testing follows (08 §8.2: a hidden element is skipped, its
/// descendants are still tested). Source: a conversion report on 0.19.2 (an overlay with
/// `visibility: hidden` whose children still showed). The style cases held already on main and stay
/// as guards; what failed was an animated `visibility: hidden` (the node's `Visible`), which the
/// Painter skipped with its subtree while hit testing still found the subtree.

using namespace xt;
using namespace xt::compile;

namespace {

constexpr uint16_t kVisibility = 170;  // StyleProp `visibility` (05 §5.4)
constexpr int kW = 200;
constexpr int kH = 100;

std::vector<uint8_t> compileOk(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"vis.xt", xt}};
    CompileResult r = compileDocumentFromStrings(files, "vis.xt", options);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    return r.ir;
}

std::unique_ptr<Document> loadDoc(const std::vector<uint8_t>& ir) {
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, true, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, kW, kH});
    doc->update(0.0f);
    return doc;
}

struct Frame {
    std::vector<uint8_t> px;
    /// True when the pixel at (x, y) is within 24 per channel of (r, g, b).
    bool near(int x, int y, int r, int g, int b) const {
        const size_t o = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4;
        auto close = [](int a, int e) { return a - e <= 24 && e - a <= 24; };
        return close(px[o], r) && close(px[o + 1], g) && close(px[o + 2], b);
    }
};

Frame render(Document& doc) {
    CommandList list;
    doc.paint(list);
    Frame f;
    f.px.assign(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{f.px.data(), kW, kH, kW * 4};
    renderer.setTarget(target);
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);
    return f;
}

bool white(const Frame& f, int x, int y) { return f.near(x, y, 255, 255, 255); }
bool red(const Frame& f, int x, int y) { return f.near(x, y, 255, 0, 0); }
bool blue(const Frame& f, int x, int y) { return f.near(x, y, 0, 0, 255); }
bool green(const Frame& f, int x, int y) { return f.near(x, y, 0, 128, 0); }

// #Over (0,0)-(200,100), no background of its own. #A (10,10)-(50,50) red with a red grandchild
// #A2; #B (60,10)-(100,50) blue, re-declares `visible`; #C (110,10)-(150,50) green with a transform
// (its own matrix, the bars' case in the report). The pixel probes sit at each box's center; a
// point on no visible element hits the body (the document root).
std::string doc(const std::string& overRule, const std::string& extraCss = "") {
    return R"(<xt><head><style>
  #Over { position: absolute; left: 0px; top: 0px; width: 200px; height: 100px; )" +
           overRule + R"( }
  #A { position: absolute; left: 10px; top: 10px; width: 40px; height: 40px; background-color: #ff0000; }
  #A2 { position: absolute; left: 0px; top: 30px; width: 40px; height: 10px; background-color: #ff0000; }
  #B { position: absolute; left: 60px; top: 10px; width: 40px; height: 40px; background-color: #0000ff;
       visibility: visible; }
  #C { position: absolute; left: 110px; top: 10px; width: 40px; height: 40px; background-color: #008000;
       transform: rotate(0.5deg); }
  #Over.on { visibility: visible; }
)" + extraCss +
           R"(</style></head><body>
  <div id="Over"><div id="A"><div id="A2"></div></div><div id="B"></div><div id="C"></div></div>
</body></xt>)";
}

}  // namespace

TEST_CASE("T-867 visibility: a hidden ancestor hides its descendants; `visible` re-shows one") {
    std::unique_ptr<Document> d = loadDoc(compileOk(doc("visibility: hidden;")));
    CHECK(d->byId("A").computedStyleValue(kVisibility).e == 1);
    CHECK(d->byId("A2").computedStyleValue(kVisibility).e == 1);
    CHECK(d->byId("B").computedStyleValue(kVisibility).e == 0);
    const Frame f = render(*d);
    CHECK(white(f, 30, 30));   // #A inherited hidden
    CHECK(white(f, 30, 45));   // #A2 (grandchild) too
    CHECK(blue(f, 80, 30));    // #B re-declared visible
    CHECK(white(f, 130, 30));  // #C with its own transform
}

TEST_CASE("T-867 visibility: hit testing skips hidden descendants, finds a re-shown one") {
    std::unique_ptr<Document> d = loadDoc(compileOk(doc("visibility: hidden;")));
    CHECK(d->hitTest(Vec2{30, 30}) == d->root().node);
    CHECK(d->hitTest(Vec2{130, 30}) == d->root().node);
    CHECK(d->hitTest(Vec2{80, 30}) == d->byId("B").node);
}

TEST_CASE("T-867 visibility: switching the ancestor at run time re-styles the descendants") {
    std::unique_ptr<Document> d = loadDoc(compileOk(doc("")));
    Frame f = render(*d);
    CHECK(red(f, 30, 30));
    CHECK(green(f, 130, 30));

    REQUIRE(d->byId("Over").setStyle("visibility: hidden") == Status::kOk);
    d->update(0.016f);
    f = render(*d);
    CHECK(white(f, 30, 30));
    CHECK(white(f, 30, 45));
    CHECK(blue(f, 80, 30));
    CHECK(white(f, 130, 30));
    CHECK(d->hitTest(Vec2{30, 30}) == d->root().node);

    REQUIRE(d->byId("Over").setStyle("visibility: visible") == Status::kOk);
    d->update(0.032f);
    f = render(*d);
    CHECK(red(f, 30, 30));
    CHECK(green(f, 130, 30));
    CHECK(d->hitTest(Vec2{30, 30}) == d->byId("A").node);
}

TEST_CASE("T-867 visibility: a class toggled on the ancestor (the flow addClass case)") {
    std::unique_ptr<Document> d = loadDoc(compileOk(doc("visibility: hidden;")));
    Frame f = render(*d);
    CHECK(white(f, 30, 30));
    REQUIRE(d->byId("Over").addClass("on"));
    d->update(0.016f);
    f = render(*d);
    CHECK(red(f, 30, 30));
    CHECK(red(f, 30, 45));
    CHECK(green(f, 130, 30));
    REQUIRE(d->byId("Over").removeClass("on"));
    d->update(0.032f);
    f = render(*d);
    CHECK(white(f, 30, 30));
    CHECK(white(f, 130, 30));
    CHECK(blue(f, 80, 30));
}

TEST_CASE("T-867 visibility: an animation switching the ancestor hides the descendants") {
    // `visibility` in @keyframes compiles to the node's Visible (05 §5.4 T-229 note, a Hold key).
    std::unique_ptr<Document> d =
        loadDoc(compileOk(doc("animation: blink 1s linear forwards;",
                              "@keyframes blink { 0% { visibility: visible } 50% { visibility: hidden } "
                              "100% { visibility: hidden } }")));
    Frame f = render(*d);
    CHECK(red(f, 30, 30));
    d->update(0.75f);
    f = render(*d);
    CHECK(white(f, 30, 30));
    CHECK(white(f, 130, 30));
    CHECK(d->hitTest(Vec2{30, 30}) == d->root().node);
}
