#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/layout.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-755 (Docs/design/06-layout.md §6.x "排版邊界與局部重排"): a kDirtyLayout write stops at the
/// nearest layout boundary above it and LayoutEngine::layout() lays out only that subtree again. Each
/// case runs the same writes in two Contexts -- partial relayout on and off -- and checks the frame's
/// counts (FrameStats::laidOutElements / layoutRoots / layoutFullPasses), that the boxes outside the
/// subtree did not move, and that the layout snapshot and the painted command list are byte for byte
/// those of the full relayout. The verify mode (a full pass after every partial one) is on in both,
/// counting: partialLayoutMismatch must stay 0.

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>body { margin: 0; font-size: 10px; line-height: 10px; } " + std::string(style) +
           "</style></head>\n<body id=\"Root\">" + std::string(body) + "</body></xt>";
}

std::string dump(const CommandList& list) {
    std::string out;
    dumpCommandList(list, out);
    return out;
}

/// One Context with one Document, a fixed-metrics shaper (10 px glyphs 6 px wide) so text has a size.
struct Ui {
    FakeTextShaper shaper;
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    std::unique_ptr<Context> ctx;
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;
    CommandList list;

    Document& doc() { return *ctx->document(handle); }
    const LayoutEngine& engine() { return *ctx->layoutEngine(handle); }
    ElementRef el(std::string_view id) { return doc().byId(id); }
    const ElementData& data(std::string_view id) { return doc().arena().element(el(id).node); }
    const LayoutBox& box(std::string_view id) { return data(id).box; }
    bool boundary(std::string_view id) { return (data(id).layoutFlags & kLayoutBoundary) != 0; }
    FrameStats frame() {
        ctx->update(1.0f / 60.0f);
        ctx->render(list, nullptr);
        return ctx->perf().current();
    }
    std::string snapshot() {
        LayoutEngine engine;
        engine.init(doc().ir(), &assets);
        return engine.snapshot(doc().arena(), doc().root().node);
    }
    std::string paint() {
        CommandList out;
        doc().paint(out);
        return dump(out);
    }
};

std::unique_ptr<Ui> openUi(const std::string& source, bool partial) {
    auto ui = std::make_unique<Ui>();
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", source}};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    test::requireCompiled(result);
    ui->ir = result.ir;
    ContextConfig config;
    config.shaper = &ui->shaper;
    config.assets = &ui->assets;
    ui->ctx = std::make_unique<Context>(config);
    ui->ctx->setPartialLayout(partial);
    ui->ctx->setVerifyPartialLayout(true);  // Counting (the suite's environment switch would abort).
    ui->ctx->perf().setEnabled(true);
    ui->ctx->setViewport(Rect{0, 0, 400, 300}, 1.0f);
    Result<DocumentHandle> handle = ui->ctx->loadDocument(IrBlob{ui->ir.data(), ui->ir.size()}, false);
    REQUIRE(handle.ok());
    ui->handle = handle.value();
    for (int i = 0; i < 3; ++i) {
        ui->frame();
    }
    return ui;
}

/// The same page with partial relayout on (`on`) and off (`off`).
struct Pair {
    std::unique_ptr<Ui> on;
    std::unique_ptr<Ui> off;
};

Pair openPair(std::string_view style, std::string_view body) {
    const std::string source = page(style, body);
    return Pair{openUi(source, true), openUi(source, false)};
}

/// Applies `write` to both, runs one frame each; checks the outputs agree and no verify mismatch, and
/// returns the partial side's frame record.
FrameStats step(Pair& p, const std::function<void(Ui&)>& write) {
    write(*p.on);
    write(*p.off);
    const FrameStats f = p.on->frame();
    const FrameStats g = p.off->frame();
    CHECK(f.partialLayoutMismatch == 0);
    CHECK(p.on->engine().partialCheck().mismatches == 0);
    CAPTURE(p.on->engine().partialCheck().first);
    CHECK(g.layoutRoots == 0);
    CHECK(p.on->snapshot() == p.off->snapshot());
    CHECK(p.on->paint() == p.off->paint());
    CHECK(dump(p.on->list) == dump(p.off->list));
    return f;
}

/// The boxes of `ids` as text (to check they did not move).
std::string boxes(Ui& ui, const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) {
        const LayoutBox& b = ui.box(id);
        out += id + ":" + std::to_string(b.x) + "," + std::to_string(b.y) + "," + std::to_string(b.width) +
               "," + std::to_string(b.height) + "," + std::to_string(b.scrollWidth) + "," +
               std::to_string(b.scrollHeight) + ";";
    }
    return out;
}

constexpr std::string_view kFixedBox =
    "#outer { width: 300px; height: 200px; overflow: hidden; } #sib { height: 20px; } "
    "#box { width: 200px; height: 40px; overflow: hidden; } #after { height: 10px; }";
constexpr std::string_view kFixedBody =
    "<div id=\"outer\"><div id=\"sib\">s</div><div id=\"box\"><div id=\"t\">hello</div></div>"
    "<div id=\"after\">x</div></div>";

}  // namespace

TEST_CASE("partial layout: a text change inside a fixed, clipping box lays out that box only") {
    Pair p = openPair(kFixedBox, kFixedBody);
    REQUIRE(p.on->boundary("box"));
    const std::vector<std::string> outside{"Root", "outer", "sib", "after"};
    const std::string before = boxes(*p.on, outside);
    const FrameStats f = step(p, [](Ui& ui) { ui.el("t").setText("hello world, longer"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.layoutFullPasses == 0);
    CHECK(f.laidOutElements == 2);  // #box and #t.
    CHECK(boxes(*p.on, outside) == before);
    CHECK(p.on->engine().lastLayoutRoots() == 1);
    // The full side relaid everything out.
    CHECK(p.off->engine().lastLayoutFull());
}

TEST_CASE("partial layout: the text growing past the box (wrapping, then overflowing) stays inside") {
    Pair p = openPair(
        "#outer { width: 300px; height: 200px; overflow: hidden; } "
        "#box { width: 60px; height: 20px; overflow: hidden; } #after { height: 10px; }",
        "<div id=\"outer\"><div id=\"box\"><div id=\"t\">ab</div></div><div id=\"after\">x</div></div>");
    const std::vector<std::string> outside{"outer", "after"};
    const std::string before = boxes(*p.on, outside);
    const float scrollBefore = p.on->box("box").scrollHeight;
    FrameStats f = step(p, [](Ui& ui) { ui.el("t").setText("abc defgh ijk"); });  // Two lines.
    CHECK(f.layoutRoots == 1);
    f = step(p, [](Ui& ui) { ui.el("t").setText("abc defgh ijk lmnop qrs tuv wxyz"); });  // Overflows.
    CHECK(f.layoutRoots == 1);
    CHECK(p.on->box("box").scrollHeight > scrollBefore);
    CHECK(p.on->box("t").height > 20.0f);
    CHECK(boxes(*p.on, outside) == before);
}

TEST_CASE("partial layout: what is not a boundary passes the write up to the next one") {
    struct Variant {
        const char* name;
        const char* style;    // #box's declarations (and its parent's, as needed)
        const char* wrapper;  // #mid's declarations
    };
    const Variant variants[] = {
        {"auto width", "#box { height: 40px; overflow: hidden; }", ""},
        {"auto height", "#box { width: 200px; overflow: hidden; }", ""},
        {"percent width against an auto width", "#box { width: 50%; height: 40px; overflow: hidden; }", ""},
        {"percent height against an auto height", "#box { width: 50px; height: 50%; overflow: hidden; }", ""},
        {"flex item that grows",
         "#box { width: 200px; height: 40px; overflow: hidden; flex-grow: 1; flex-shrink: 0; }",
         "#mid { display: flex; width: 280px; }"},
        {"flex item that shrinks (the default)", "#box { width: 200px; height: 40px; overflow: hidden; }",
         "#mid { display: flex; width: 280px; }"},
        {"flex item with a percent basis",
         "#box { width: 200px; height: 40px; overflow: hidden; flex: 0 0 50%; }",
         "#mid { display: flex; width: 280px; }"},
        {"grid item in an auto track", "#box { width: 200px; height: 40px; overflow: hidden; }",
         "#mid { display: grid; grid-template-columns: auto; grid-template-rows: 40px; }"},
        {"grid item in a percent track", "#box { width: 200px; height: 40px; overflow: hidden; }",
         "#mid { display: grid; grid-template-columns: 50%; grid-template-rows: 40px; }"},
    };
    for (const Variant& v : variants) {
        if (v.style == nullptr) {
            continue;
        }
        CAPTURE(v.name);
        const std::string style = std::string("#outer { width: 300px; height: 200px; overflow: hidden; } ") +
                                  v.wrapper + " " + v.style;
        Pair p = openPair(style,
                          "<div id=\"outer\"><div id=\"mid\"><div id=\"box\"><div id=\"t\">hello</div></div>"
                          "</div><div id=\"after\">x</div></div>");
        CHECK_FALSE(p.on->boundary("box"));
        CHECK(p.on->boundary("outer"));
        const FrameStats f = step(p, [](Ui& ui) { ui.el("t").setText("hello there, a longer text"); });
        CHECK(f.layoutRoots == 1);
        CHECK(f.layoutFullPasses == 0);
        CHECK(f.laidOutElements == 5);  // #outer's subtree: outer, mid, box, t, after.
    }
}

TEST_CASE("partial layout: the boundary predicate, condition by condition") {
    struct Case {
        const char* name;
        const char* style;
        const char* wrapper;
        bool boundary;
    };
    const Case cases[] = {
        {"px both axes, overflow hidden", "#box { width: 200px; height: 40px; overflow: hidden; }", "", true},
        {"overflow scroll", "#box { width: 200px; height: 40px; overflow: scroll; }", "", true},
        {"overflow auto", "#box { width: 200px; height: 40px; overflow: auto; }", "", true},
        {"overflow-y hidden only (x computes to auto)",
         "#box { width: 200px; height: 40px; overflow-y: hidden; }", "", true},
        {"overflow visible (T-782: a boundary too)", "#box { width: 200px; height: 40px; }", "", true},
        {"calc without a percentage", "#box { width: calc(100px + 20px); height: 40px; overflow: hidden; }",
         "", true},
        {"calc with a percentage, definite parent",
         "#box { width: calc(50% + 2px); height: 40px; overflow: hidden; }", "#mid { width: 280px; }", true},
        {"calc with a percentage, auto parent",
         "#box { width: calc(50% + 2px); height: 40px; overflow: hidden; }", "", false},
        {"percent width, definite parent", "#box { width: 50%; height: 40px; overflow: hidden; }",
         "#mid { width: 280px; }", true},
        {"percent height, definite parent", "#box { width: 50px; height: 50%; overflow: hidden; }",
         "#mid { height: 100px; }", true},
        {"percent height, auto parent", "#box { width: 50px; height: 50%; overflow: hidden; }", "", false},
        {"auto width", "#box { height: 40px; overflow: hidden; }", "", false},
        {"auto height", "#box { width: 40px; overflow: hidden; }", "", false},
        {"display none", "#box { width: 200px; height: 40px; overflow: hidden; display: none; }", "", false},
        {"flex: none", "#box { width: 200px; height: 40px; overflow: hidden; flex: none; }",
         "#mid { display: flex; width: 280px; }", true},
        {"flex 0 0 px basis", "#box { width: 200px; height: 40px; overflow: hidden; flex: 0 0 120px; }",
         "#mid { display: flex; width: 280px; }", true},
        {"flex-grow 1", "#box { width: 200px; height: 40px; overflow: hidden; flex: 1 0 auto; }",
         "#mid { display: flex; width: 280px; }", false},
        {"flex-shrink 1", "#box { width: 200px; height: 40px; overflow: hidden; flex: 0 1 auto; }",
         "#mid { display: flex; width: 280px; }", false},
        {"flex item with a percent width", "#box { width: 50%; height: 40px; overflow: hidden; flex: none; }",
         "#mid { display: flex; width: 280px; }", false},
        {"grid item in px tracks", "#box { width: 200px; height: 40px; overflow: hidden; }",
         "#mid { display: grid; grid-template-columns: 250px; grid-template-rows: 40px; }", true},
        {"grid item in fr tracks", "#box { width: 200px; height: 40px; overflow: hidden; }",
         "#mid { display: grid; width: 280px; grid-template-columns: 1fr; grid-template-rows: 1fr; height: "
         "60px; }",
         true},
        {"grid item in an auto row", "#box { width: 200px; height: 40px; overflow: hidden; }",
         "#mid { display: grid; grid-template-columns: 250px; grid-template-rows: auto; }", false},
        {"absolute, px size", "#box { position: absolute; width: 200px; height: 40px; overflow: hidden; }",
         "#mid { position: relative; }", true},
        {"absolute, percent size", "#box { position: absolute; width: 50%; height: 50%; overflow: hidden; }",
         "#mid { position: relative; }", true},
        {"absolute, both insets",
         "#box { position: absolute; left: 5px; right: 5px; top: 2px; bottom: 2px; "
         "overflow: hidden; }",
         "#mid { position: relative; height: 80px; }", true},
        {"absolute, one inset per axis",
         "#box { position: absolute; left: 5px; top: 2px; overflow: hidden; }",
         "#mid { position: relative; }", false},
        {"absolute, overflow visible (T-782: a boundary too)",
         "#box { position: absolute; width: 200px; height: 40px; }", "#mid { position: relative; }", true},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const std::string style = std::string("#outer { width: 300px; height: 200px; overflow: hidden; } ") +
                                  c.wrapper + " " + c.style;
        auto ui = openUi(page(style,
                              "<div id=\"outer\"><div id=\"mid\"><div id=\"box\"><div id=\"t\">hi</div></div>"
                              "</div></div>"),
                         true);
        CHECK(ui->boundary("box") == c.boundary);
        CHECK(ui->boundary("Root") == false);  // body: auto width and height.
        CHECK((ui->doc().arena().element(ui->doc().root().node).layoutFlags & kLayoutBoundary) != 0);
    }
}

TEST_CASE("partial layout: absolutely positioned descendants, containing block inside and outside") {
    // Inside: #box is positioned, #abs inside it (auto size, not a boundary itself).
    Pair in = openPair(std::string(kFixedBox) +
                           " #box { position: relative; } #abs { position: absolute; left: 3px; "
                           "top: 4px; }",
                       "<div id=\"outer\"><div id=\"sib\">s</div><div id=\"box\"><div id=\"t\">hello</div>"
                       "<div id=\"abs\">a</div></div><div id=\"after\">x</div></div>");
    FrameStats f = step(in, [](Ui& ui) { ui.el("abs").setText("abs grows wider"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 2);  // #box and #t in flow; #abs in the absolute pass, uncounted.
    // Outside: #box is static, #abs's containing block is #outer; the text above #abs's parent wraps
    // and moves it down, so #abs (relative to its parent) moves too.
    Pair out = openPair(
        "#outer { width: 300px; height: 200px; overflow: hidden; position: relative; } "
        "#box { width: 60px; height: 100px; overflow: hidden; } "
        "#abs { position: absolute; left: 3px; top: 4px; }",
        "<div id=\"outer\"><div id=\"box\"><div id=\"t\">ab</div><div id=\"p\"><div id=\"abs\">a</div>"
        "</div></div></div>");
    const float yBefore = out.on->box("p").y;
    f = step(out, [](Ui& ui) { ui.el("t").setText("abc defgh ijk lmn"); });
    CHECK(f.layoutRoots == 1);
    CHECK(out.on->box("p").y > yBefore);
    // An absolutely positioned layout root of its own.
    Pair self = openPair(
        "#outer { width: 300px; height: 200px; overflow: hidden; position: relative; } "
        "#abs { position: absolute; left: 10px; top: 10px; width: 80px; height: 30px; overflow: hidden; }",
        "<div id=\"outer\"><div id=\"sib\">s</div><div id=\"abs\"><div id=\"t\">a</div></div></div>");
    REQUIRE(self.on->boundary("abs"));
    f = step(self, [](Ui& ui) { ui.el("t").setText("a longer text here"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 2);  // #abs (counted once) and #t.
}

TEST_CASE("partial layout: nested boundaries and several layout roots in one frame") {
    Pair p = openPair(
        "#outer { width: 300px; height: 200px; overflow: hidden; } "
        ".b { width: 120px; height: 40px; overflow: hidden; } #inner { width: 100px; height: 20px; "
        "overflow: hidden; }",
        "<div id=\"outer\"><div id=\"b1\" class=\"b\"><div id=\"t1\">one</div><div id=\"inner\">"
        "<div id=\"t2\">two</div></div></div><div id=\"b2\" class=\"b\"><div id=\"t3\">three</div></div>"
        "</div>");
    FrameStats f = step(p, [](Ui& ui) { ui.el("t2").setText("two, longer"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 2);  // #inner and #t2.
    f = step(p, [](Ui& ui) { ui.el("t1").setText("one, longer"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 4);  // #b1's subtree.
    f = step(p, [](Ui& ui) {
        ui.el("t2").setText("two again");
        ui.el("t1").setText("one again");  // Covers the first.
        ui.el("t3").setText("three again");
    });
    CHECK(f.layoutRoots == 2);
    CHECK(f.laidOutElements == 6);  // #b1's four and #b2's two.
}

TEST_CASE("partial layout: a style change that flips the boundary goes past it") {
    Pair p = openPair(kFixedBox, kFixedBody);
    REQUIRE(p.on->boundary("box"));
    // No longer a fixed height, and its text changes in the same frame: #outer lays out.
    FrameStats f = step(p, [](Ui& ui) {
        REQUIRE(ui.el("box").setStyle("height: auto") == Status::kOk);
        ui.el("t").setText("changed");
    });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 5);
    CHECK_FALSE(p.on->boundary("box"));
    f = step(p, [](Ui& ui) { ui.el("t").setText("changed again"); });
    CHECK(f.laidOutElements == 5);
    // A boundary again; its own width changes (its box moves its siblings? no: same flow), then text.
    f = step(p, [](Ui& ui) { REQUIRE(ui.el("box").setStyle("height: 40px; width: 150px") == Status::kOk); });
    CHECK(f.laidOutElements == 5);
    CHECK(p.on->boundary("box"));
    f = step(p, [](Ui& ui) { ui.el("t").setText("back inside"); });
    CHECK(f.laidOutElements == 2);
}

TEST_CASE("partial layout: <repeat> entries added and removed") {
    Pair p = openPair(
        "#outer { width: 300px; height: 200px; overflow: hidden; } "
        "#L { width: 200px; height: 120px; overflow: hidden; } .cell { height: 10px; }",
        "<div id=\"P\" port:items=\"list<struct{name: string}>\"></div>"
        "<div id=\"outer\"><div id=\"L\"><repeat port=\"items\" id=\"Item\"><div class=\"cell\" "
        "bind:text=\"item(name)\"></div></repeat></div><div id=\"after\">x</div></div>");
    const auto set = [](std::string_view json) {
        return [json](Ui& ui) {
            REQUIRE(ui.doc().setPortText(ui.doc().lookupPort("items"), json) == Status::kOk);
        };
    };
    step(p, set(R"([{"name":"A"},{"name":"B"}])"));
    step(p, set(R"([{"name":"A"},{"name":"B"},{"name":"C"}])"));
    step(p, set(R"([{"name":"C"}])"));
    const FrameStats f = step(p, set(R"([{"name":"D"}])"));  // Same count: the text only.
    CHECK(f.layoutFullPasses == 0);
}

TEST_CASE("partial layout: off, every write is a full pass; the verify mode catches a wrong boundary") {
    auto off = openUi(page(kFixedBox, kFixedBody), false);
    off->el("t").setText("changed");
    FrameStats f = off->frame();
    CHECK(f.layoutFullPasses == 1);
    CHECK(f.layoutRoots == 0);

    // A boundary whose own width is changed behind the layout engine's back (no dirty bit): the
    // partial pass keeps its old box, the full pass of the verify mode does not.
    auto ui = openUi(page(kFixedBox, kFixedBody), true);
    REQUIRE(ui->boundary("box"));
    NodeArena& arena = ui->doc().arena();
    arena.element(ui->el("box").node).style.base[prop::Width].len = Length{150, LengthUnit::Px};
    ui->el("t").setText("changed");
    f = ui->frame();
    CHECK(f.layoutRoots == 1);
    CHECK(f.partialLayoutMismatch == 1);
    const LayoutEngine::PartialCheck& check = ui->engine().partialCheck();
    CHECK(check.mismatches == 1);
    CHECK(check.first.find("#box") != std::string::npos);
    CHECK(check.first.find("width") != std::string::npos);
    CHECK(ui->box("box").width == doctest::Approx(150));  // The full pass's result stands.
}

// T-782 (06 §6.x, Docs/tasks/T-782.md): `overflow` no longer takes part in the boundary predicate. A
// scroll extent comes from an element's direct children's boxes only (storeScrollSize), and a
// boundary's box does not depend on its content, so no ancestor's scroll extent can change.

TEST_CASE("partial layout: a fixed, overflow:visible absolutely positioned box is a layout root") {
    Pair p = openPair(
        "#stage { position: relative; width: 300px; height: 200px; } "
        "#box { position: absolute; left: 10px; top: 10px; width: 80px; height: 30px; }",
        "<div id=\"stage\"><div id=\"sib\">s</div><div id=\"box\"><div id=\"t\">round 1</div></div></div>");
    REQUIRE(p.on->boundary("box"));
    const std::vector<std::string> outside{"Root", "stage", "sib"};
    const std::string before = boxes(*p.on, outside);
    FrameStats f = step(p, [](Ui& ui) { ui.el("t").setText("round 2, a much longer text that wraps"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.layoutFullPasses == 0);
    CHECK(f.laidOutElements == 2);         // #box (counted once) and #t.
    CHECK(p.on->box("t").height > 30.0f);  // Overflows the box.
    CHECK(boxes(*p.on, outside) == before);
    f = step(p, [](Ui& ui) { ui.el("t").setText("r3"); });
    CHECK(f.laidOutElements == 2);
}

TEST_CASE("partial layout: text overflowing a visible box leaves an overflow:auto ancestor's extent") {
    // Absolutely positioned (the scroller does not count it at all) and in flow (it counts the box's
    // margin box, which does not change).
    const char* const variants[] = {
        "#box { position: absolute; left: 4px; top: 4px; width: 60px; height: 20px; }",
        "#box { width: 60px; height: 20px; margin: 3px; }",
    };
    for (const char* v : variants) {
        CAPTURE(v);
        Pair p = openPair(std::string("#scroller { position: relative; width: 200px; height: 100px; "
                                      "overflow: auto; } #pad { height: 30px; } ") +
                              v,
                          "<div id=\"scroller\"><div id=\"pad\">p</div><div id=\"box\"><div id=\"t\">ab</div>"
                          "</div></div>");
        REQUIRE(p.on->boundary("box"));
        const std::vector<std::string> outside{"Root", "scroller", "pad"};
        const std::string before = boxes(*p.on, outside);
        // Longer: wraps past the box's height (and the scroller's), a long word past its width.
        FrameStats f = step(p, [](Ui& ui) {
            ui.el("t").setText(
                "abcdefghijklmnopqrstuvwxyz0123456789 abc def ghi jkl mno pqr stu vwx yz abc def ghi jkl "
                "mno pqr stu vwx yz abc def ghi jkl mno pqr stu vwx yz");
        });
        CHECK(f.layoutRoots == 1);
        CHECK(f.laidOutElements == 2);
        CHECK(p.on->box("t").height > 100.0f);
        CHECK(p.on->box("box").scrollHeight > 20.0f);
        CHECK(boxes(*p.on, outside) == before);  // scrollWidth / scrollHeight included.
        // Shorter again.
        f = step(p, [](Ui& ui) { ui.el("t").setText("ab"); });
        CHECK(f.laidOutElements == 2);
        CHECK(boxes(*p.on, outside) == before);
    }
}

TEST_CASE("partial layout: a visible box in a visible box in a scroll container") {
    Pair p = openPair(
        "#scroller { width: 220px; height: 120px; overflow: scroll; } "
        "#a { width: 150px; height: 60px; } #b { width: 80px; height: 20px; } #after { height: 10px; }",
        "<div id=\"scroller\"><div id=\"a\"><div id=\"ta\">a</div><div id=\"b\"><div id=\"tb\">b</div>"
        "</div></div><div id=\"after\">x</div></div>");
    REQUIRE(p.on->boundary("a"));
    REQUIRE(p.on->boundary("b"));
    const std::vector<std::string> outside{"Root", "scroller", "after"};
    const std::string before = boxes(*p.on, outside);
    // The inner one's text overflows both boxes.
    FrameStats f = step(
        p, [](Ui& ui) { ui.el("tb").setText("b b b b b b b b b b b b b b b b b b b b b b b b b b b b"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 2);  // #b and #tb.
    CHECK(boxes(*p.on, outside) == before);
    // The outer one's own text: #a's subtree (a, ta, b, tb), #b moves down.
    const float bY = p.on->box("b").y;
    f = step(p,
             [](Ui& ui) { ui.el("ta").setText("a a a a a a a a a a a a a a a a a a a a a a a a a a a a"); });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 4);
    CHECK(p.on->box("b").y > bY);
    CHECK(boxes(*p.on, outside) == before);
    // Both in one frame: the outer root covers the inner one.
    f = step(p, [](Ui& ui) {
        ui.el("tb").setText("b");
        ui.el("ta").setText("a");
    });
    CHECK(f.layoutRoots == 1);
    CHECK(f.laidOutElements == 4);
    CHECK(boxes(*p.on, outside) == before);
}
