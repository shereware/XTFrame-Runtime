#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/compile/canonical_writer.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostic_report.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt_v2_test_util.h"

/// T-587 (Docs/design/54-source-format-v2.md): the source format v2 reader -- sections, the
/// `<layout>` / `<style>` / `<timelines>` whitelists and their diagnostics, `src` fragments, file
/// columns, `on:<event>@<path>`, the canonical writer, and the §54.2 one-page example against the
/// same content written in v1 (§54.11 layer two: computed styles).

using namespace xt;
using namespace xt::compile;
using xt::test::v2::chunkPayload;
using xt::test::v2::compileFiles;
using xt::test::v2::findCode;
using xt::test::v2::requireCompiled;

namespace {

/// A v2 document with `sections` after a `<body>` holding `body`.
std::string v2Doc(const std::string& body, const std::string& sections) {
    return "<xt version=\"2\">\n<body>" + body + "</body>\n" + sections + "\n</xt>\n";
}

CompileResult compileV2(const std::string& body, const std::string& sections, CompileOptions options = {}) {
    return compileFiles({SourceFile{"v2.xt", v2Doc(body, sections)}}, options);
}

/// One Context with one loaded Document, laid out once.
struct LoadedDoc {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;

    explicit LoadedDoc(const std::vector<uint8_t>& ir) {
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
        ctx.update(0.0f);
    }
};

bool sameValue(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case ValueType::Float:
            return a.f == b.f;
        case ValueType::Vec2:
            return a.v2 == b.v2;
        case ValueType::Vec3:
            return a.v3.x == b.v3.x && a.v3.y == b.v3.y && a.v3.z == b.v3.z;
        case ValueType::Color:
            return a.c == b.c;
        case ValueType::Length:
            return a.len.value == b.len.value && a.len.unit == b.len.unit;
        case ValueType::Enum:
            return a.e == b.e;
        default:
            return true;  // String / list ids belong to each document's own string table.
    }
}

/// §54.11 layer two: every element's computed style (animatable properties, non-animatable
/// StyleProps) and layout box are the same in both documents.
void checkSameComputedStyle(const std::vector<uint8_t>& irA, const std::vector<uint8_t>& irB) {
    LoadedDoc a(irA);
    LoadedDoc b(irB);
    uint32_t count = 0;
    for (; a.doc->elementAt(count).valid(); ++count) {
        CAPTURE(count);
        const ElementRef ea = a.doc->elementAt(count);
        const ElementRef eb = b.doc->elementAt(count);
        REQUIRE(eb.valid());
        for (PropId p = 0; p < prop::kElementPropCount; ++p) {
            CAPTURE(p);
            CHECK(sameValue(ea.property(p), eb.property(p)));
        }
        for (uint16_t sp = 160; sp <= kStylePropLast; ++sp) {
            CAPTURE(sp);
            CHECK(sameValue(ea.computedStyleValue(sp), eb.computedStyleValue(sp)));
        }
        const LayoutBox ba = ea.layoutBox();
        const LayoutBox bb = eb.layoutBox();
        CHECK(ba.x == bb.x);
        CHECK(ba.y == bb.y);
        CHECK(ba.width == bb.width);
        CHECK(ba.height == bb.height);
        CHECK(ea.effectCount() == eb.effectCount());
    }
    CHECK_FALSE(b.doc->elementAt(count).valid());
    CHECK(count > 2);
}

/// Subtree equality without source positions (the round trip moves lines and reorders the
/// top-level parts): tag, attributes, text and children, recursively.
bool sameSubtree(const DomTree& a, uint32_t ia, const DomTree& b, uint32_t ib) {
    const DomNode& x = a.nodes[ia];
    const DomNode& y = b.nodes[ib];
    if (x.tag != y.tag || x.text != y.text || x.children.size() != y.children.size() ||
        x.attrs.size() != y.attrs.size()) {
        return false;
    }
    for (size_t k = 0; k < x.attrs.size(); ++k) {
        if (x.attrs[k].name != y.attrs[k].name || x.attrs[k].value != y.attrs[k].value ||
            x.attrs[k].portHidden != y.attrs[k].portHidden) {
            return false;
        }
    }
    for (size_t k = 0; k < x.children.size(); ++k) {
        if (!sameSubtree(a, x.children[k], b, y.children[k])) {
            return false;
        }
    }
    return true;
}

/// The body and every symbol definition are the same in both trees.
bool sameElements(const DomTree& a, const DomTree& b) {
    if (a.symbolDefs.size() != b.symbolDefs.size() || (a.body == 0xFFFFFFFFu) != (b.body == 0xFFFFFFFFu)) {
        return false;
    }
    for (size_t k = 0; k < a.symbolDefs.size(); ++k) {
        if (!sameSubtree(a, a.symbolDefs[k], b, b.symbolDefs[k])) {
            return false;
        }
    }
    return a.body == 0xFFFFFFFFu || sameSubtree(a, a.body, b, b.body);
}

}  // namespace

TEST_CASE("T-587 v2 reader: sections are direct children of <xt>, in any order, with file positions") {
    const std::string text =
        "<xt version=\"2\">\n"
        "  <style>#A { color: red; }</style>\n"
        "  <body><div id=\"A\"></div></body>\n"
        "  <layout src=\"a.layout.xts\"/>\n"
        "  <timelines>@timeline t { #A { 0s { opacity: 0 } } }</timelines>\n"
        "  <layout>\n    #A { width: 10px; }\n  </layout>\n"
        "</xt>\n";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "s.xt", sink, tree));
    CHECK(tree.formatVersion == 2);
    // No section is an element: the tree is <xt>, <body>, <div>.
    REQUIRE(tree.nodes.size() == 3);
    REQUIRE(tree.styleSheets.size() == 1);
    CHECK(tree.styleSheets[0].text == "#A { color: red; }");
    CHECK(tree.styleSheets[0].lineOffset == 2);
    CHECK(tree.styleSheets[0].colOffset == 10);
    CHECK(tree.styleSheets[0].order == 0);
    REQUIRE(tree.layoutSheets.size() == 2);
    CHECK(tree.layoutSheets[0].src == "a.layout.xts");
    CHECK(tree.layoutSheets[0].order == 1);
    CHECK(tree.layoutSheets[0].lineOffset == 4);
    CHECK(tree.layoutSheets[0].colOffset == 3);
    CHECK(tree.layoutSheets[1].order == 3);
    CHECK(tree.layoutSheets[1].lineOffset == 6);
    REQUIRE(tree.timelineSheets.size() == 1);
    CHECK(tree.timelineSheets[0].order == 2);
    CHECK(tree.nodes[1].sourceBegin == text.find("<body>"));
    CHECK(tree.nodes[1].sourceEnd == text.find("</body>") + 7);

    DomTree v1;
    DiagnosticSink v1Sink;
    REQUIRE(parseXt("<xt><body><div id=\"A\"></div></body></xt>", "v1.xt", v1Sink, v1));
    CHECK(v1.formatVersion == 1);
    CHECK(v1.layoutSheets.empty());
}

TEST_CASE("T-587 v2 reader: <head> takes <title> and <symbols src> only (head.legacyChild)") {
    for (const char* legacy : {"<link href=\"a.xts\"/>", "<style>#a { color: red; }</style>",
                               "<symbol name=\"S\"><div id=\"x\"></div></symbol>", "<meta/>"}) {
        CAPTURE(legacy);
        const std::string text = std::string("<xt version=\"2\"><head><title>T</title>") + legacy +
                                 "<symbols src=\"lib.xt\"/></head><body></body></xt>";
        DiagnosticSink sink;
        DomTree tree;
        CHECK_FALSE(parseXt(text, "h.xt", sink, tree));
        const Diagnostic* d = findCode(sink.all(), "head.legacyChild");
        REQUIRE(d != nullptr);
        CHECK(d->message.find("--migrate") != std::string::npos);
        CHECK_FALSE(d->fix.empty());
        CHECK(tree.title == "T");
        REQUIRE(tree.symbolHrefs.size() == 1);
        CHECK(tree.symbolHrefs[0] == "lib.xt");
        CHECK(tree.linkHrefs.empty());
        CHECK(tree.inlineStyles.empty());
        CHECK(tree.symbolDefs.empty());
    }
}

TEST_CASE("T-587 v2 reader: section.srcWithContent, duplicate <ports>, ports.onElement") {
    {
        DiagnosticSink sink;
        DomTree tree;
        CHECK_FALSE(parseXt("<xt version=\"2\"><body></body><style src=\"a.xts\">#a{}</style></xt>", "s.xt",
                            sink, tree));
        const Diagnostic* d = findCode(sink.all(), "section.srcWithContent");
        REQUIRE(d != nullptr);
        CHECK_FALSE(d->fix.empty());
    }
    {
        DiagnosticSink sink;
        DomTree tree;
        CHECK_FALSE(
            parseXt("<xt version=\"2\"><ports>a: number</ports><ports>b: number</ports><body></body></xt>",
                    "p.xt", sink, tree));
    }
    {
        DiagnosticSink sink;
        DomTree tree;
        CHECK_FALSE(parseXt(
            "<xt version=\"2\"><body port:hp=\"number in\"><div id=\"a\" port:x=\"bool\"></div></body></xt>",
            "p.xt", sink, tree));
        size_t count = 0;
        for (const Diagnostic& d : sink.all()) {
            if (d.code == "ports.onElement") {
                ++count;
                CHECK(d.fix.find("<ports>") != std::string::npos);
            }
        }
        CHECK(count == 2);
        // The same attributes are fine in v1.
        DiagnosticSink v1Sink;
        CHECK(parseXt("<xt><body port:hp=\"number in\"></body></xt>", "p.xt", v1Sink, tree));
    }
    {
        // The fix travels into the shared diagnostics format as the `fix` detail.
        Diagnostic d{Diagnostic::Level::Error, "a.xt", 1, 2, "m", "ports.onElement", "move it"};
        const ReportDiagnostic item = fromCompileDiagnostic(d);
        REQUIRE(item.detail.size() == 1);
        CHECK(item.detail[0].first == "fix");
        CHECK(item.detail[0].second == "move it");
    }
}

TEST_CASE("T-587 v2 reader: on:<event>@<path> is an attribute name in v2 only") {
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(
        parseXt("<xt version=\"2\"><body><use symbol=\"S\" id=\"P\" on:press@T1=\"sound(a)\" "
                "on:key(Enter)@Row/Label=\"sound(b)\"/></body></xt>",
                "t.xt", sink, tree));
    const DomNode& use = tree.nodes[2];
    CHECK(use.attr("on:press@T1") != nullptr);
    CHECK(use.attr("on:key(enter)@row/label") == nullptr);
    CHECK(use.attr("on:key(Enter)@Row/Label") != nullptr);
    DiagnosticSink v1Sink;
    CHECK_FALSE(parseXt("<xt><body><use symbol=\"S\" id=\"P\" on:press@T1=\"sound(a)\"/></body></xt>", "t.xt",
                        v1Sink, tree));
}

TEST_CASE("T-587 whitelists: <layout> takes geometry only (layout.notGeometry)") {
    const char* bad[] = {"#A { color: red; }",
                         "#A { transition: width 1s; }",
                         "#A { cursor: pointer; }",
                         "#A { on-press: sound(a); }",
                         "#A { --space: 4px; }",
                         "@keyframes k { from { opacity: 0 } to { opacity: 1 } }",
                         "@timeline t { #A { 0s { opacity: 0 } } }"};
    for (const char* css : bad) {
        CAPTURE(css);
        const CompileResult r =
            compileV2("<div id=\"A\"></div>", std::string("<layout>") + css + "</layout>");
        CHECK_FALSE(r.ok());
        const Diagnostic* d = findCode(r.diagnostics, "layout.notGeometry");
        REQUIRE(d != nullptr);
        CHECK_FALSE(d->fix.empty());
    }
    const CompileResult ok = compileV2(
        "<div id=\"A\"></div>",
        "<layout>#A { position: absolute; left: 10px; width: 20px; display: flex; gap: 4px; padding: 2px; "
        "z-index: 3; overflow: hidden; perspective: 400px; layer: overlay; ui-scale: none; transform: "
        "rotate(5deg); }"
        "@media (max-width: 1000px) { #A { left: 2px; } }</layout>");
    requireCompiled(ok);
}

TEST_CASE("T-587 whitelists: <style> takes no geometry but display: none, no @timeline, no triggers") {
    struct Case {
        const char* css;
        const char* code;
    };
    const Case cases[] = {{"#A { width: 10px; }", "style.geometryInStyle"},
                          {"#A { display: flex; }", "style.geometryInStyle"},
                          {"#A { margin: 4px; }", "style.geometryInStyle"},
                          {"@timeline t { #A { 0s { opacity: 0 } } }", "style.timelineInStyle"},
                          {"#A { on-press: sound(a); }", "style.triggerInStyle"},
                          {"#A { state-enter: Low sound(a); }", "style.triggerInStyle"}};
    for (const Case& c : cases) {
        CAPTURE(c.css);
        const CompileResult r =
            compileV2("<div id=\"A\"></div>", std::string("<style>") + c.css + "</style>");
        CHECK_FALSE(r.ok());
        const Diagnostic* d = findCode(r.diagnostics, c.code);
        REQUIRE(d != nullptr);
        CHECK_FALSE(d->fix.empty());
    }
    const CompileResult ok =
        compileV2("<div id=\"A\"></div>",
                  "<style>#A { display: none; color: red; cursor: pointer; transition: width 1s; "
                  "--accent: #f00; } @keyframes k { from { opacity: 0 } to { opacity: 1 } }"
                  "</style>");
    requireCompiled(ok);
    // A v1 document keeps the whole language in <style>.
    requireCompiled(compileFiles({SourceFile{
        "v1.xt",
        "<xt><head><style>#A { width: 10px; on-press: sound(a); } @timeline t { #A { 0s { opacity: 0 } } }"
        "</style></head><body><div id=\"A\"></div></body></xt>"}}));
}

TEST_CASE("T-587 whitelists: <timelines> takes @timeline blocks only") {
    requireCompiled(
        compileV2("<div id=\"A\"></div>",
                  "<timelines>@timeline t { #A { 0s { opacity: 0 } 1s { opacity: 1 } } }</timelines>"));
    for (const char* css : {"#A { opacity: 0; }", "@keyframes k { from { opacity: 0 } }"}) {
        CAPTURE(css);
        const CompileResult r =
            compileV2("<div id=\"A\"></div>", std::string("<timelines>") + css + "</timelines>");
        CHECK_FALSE(r.ok());
    }
}

TEST_CASE("T-587 <layout> selectors are not restricted to #id subjects (54 §54.1 item 3)") {
    const CompileResult r =
        compileV2("<div id=\"Row\" class=\"row\"><div id=\"A\"></div><div id=\"B\"></div></div>",
                  "<layout>.row { display: flex; } div div { width: 30px; } * { height: 10px; }"
                  "</layout>");
    requireCompiled(r);
    CHECK(findCode(r.diagnostics, "layout.subjectNotId") == nullptr);  // The lint is off by default.
    LoadedDoc loaded(r.ir);
    CHECK(loaded.doc->byId("B").layoutBox().x == 30.0f);  // `.row` is a flex row.
    CHECK(loaded.doc->byId("A").layoutBox().width == 30.0f);
    CHECK(loaded.doc->byId("Row").layoutBox().height == 10.0f);

    CompileOptions lint;
    lint.lintLayoutSubjectId = true;
    const CompileResult linted =
        compileV2("<div id=\"Row\" class=\"row\"></div>",
                  "<layout>.row { display: flex; } #Row { gap: 2px; }</layout>", lint);
    REQUIRE(linted.ok());
    size_t count = 0;
    for (const Diagnostic& d : linted.diagnostics) {
        count += d.code == "layout.subjectNotId" ? size_t{1} : size_t{0};
    }
    CHECK(count == 1);
}

TEST_CASE("T-587 layout.duplicateSubject: the same #id in two layout sections warns once") {
    const CompileResult r =
        compileV2("<div id=\"A\"></div><div id=\"B\"></div>",
                  "<layout>#A { width: 1px; } #B { width: 1px; }</layout>\n<layout>#A { height: 1px; } "
                  "#A { left: 0; }</layout>\n<layout>#A { top: 0; }</layout>");
    requireCompiled(r);
    size_t count = 0;
    for (const Diagnostic& d : r.diagnostics) {
        if (d.code == "layout.duplicateSubject") {
            ++count;
            CHECK(d.level == Diagnostic::Level::Warning);
            CHECK(d.message.find("#A") != std::string::npos);
            CHECK(d.line == 4);
        }
    }
    CHECK(count == 1);
}

TEST_CASE("T-587 diagnostics of an inline section report file lines and columns") {
    const std::string text =
        "<xt version=\"2\">\n<body><div id=\"A\"></div></body>\n"
        "  <style>#A { width: 1px; }\n"
        "    #A { left: 2px; }</style>\n</xt>\n";
    const CompileResult r = compileFiles({SourceFile{"cols.xt", text}});
    std::vector<std::pair<int, int>> where;
    for (const Diagnostic& d : r.diagnostics) {
        if (d.code == "style.geometryInStyle") {
            where.emplace_back(d.line, d.col);
        }
    }
    REQUIRE(where.size() == 2);
    CHECK(where[0] == std::make_pair(3, 15));  // `width` on the section's first line: a file column.
    CHECK(where[1] == std::make_pair(4, 10));
}

TEST_CASE(
    "T-587 src fragments: always .xts, parsed in the referencing section's mode, relative to the owner") {
    const std::string sheet = "#A { width: 10px; }";
    {
        const CompileResult r = compileFiles(
            {SourceFile{"ui/v2.xt", v2Doc("<div id=\"A\"></div>", "<layout src=\"parts/a.xts\"/>")},
             SourceFile{"parts/a.xts", sheet}});
        requireCompiled(r);
        LoadedDoc loaded(r.ir);
        CHECK(loaded.doc->byId("A").layoutBox().width == 10.0f);
    }
    {
        // The same fragment referenced by <style> is checked with <style>'s whitelist.
        const CompileResult r =
            compileFiles({SourceFile{"v2.xt", v2Doc("<div id=\"A\"></div>", "<style src=\"a.xts\"/>")},
                          SourceFile{"a.xts", sheet}});
        CHECK_FALSE(r.ok());
        const Diagnostic* d = findCode(r.diagnostics, "style.geometryInStyle");
        REQUIRE(d != nullptr);
        CHECK(d->file == "a.xts");
        CHECK(d->line == 1);
        CHECK(d->col == 6);
    }
    {
        const CompileResult r =
            compileFiles({SourceFile{"v2.xt", v2Doc("<div id=\"A\"></div>", "<layout src=\"a.css\"/>")},
                          SourceFile{"a.css", sheet}});
        CHECK_FALSE(r.ok());
        const CompileResult missing = compileV2("<div id=\"A\"></div>", "<style src=\"nowhere.xts\"/>");
        CHECK_FALSE(missing.ok());
    }
    {
        // A v2 library's own <style src> and <symbols src> resolve against the library file.
        const std::string lib =
            "<xt version=\"2\"><head><symbols src=\"more/Inner.xt\"/></head>"
            "<symbol name=\"Outer\"><div id=\"Box\"><use symbol=\"Inner\" id=\"In\"/></div>"
            "<layout src=\"outer.layout.xts\"/></symbol></xt>";
        const std::string inner =
            "<xt version=\"2\"><symbol name=\"Inner\"><div id=\"Dot\"></div>"
            "<layout>#Dot { width: 7px; height: 7px; }</layout></symbol></xt>";
        const std::string doc =
            "<xt version=\"2\"><head><symbols src=\"lib/Outer.xt\"/></head>"
            "<body><use symbol=\"Outer\" id=\"O\"/></body></xt>";
        const CompileResult r = compileFiles({SourceFile{"main.xt", doc}, SourceFile{"lib/Outer.xt", lib},
                                              SourceFile{"lib/outer.layout.xts", "#Box { width: 50px; }"},
                                              SourceFile{"lib/more/Inner.xt", inner}});
        requireCompiled(r);
        LoadedDoc loaded(r.ir);
        CHECK(loaded.doc->byId("O/Box").layoutBox().width == 50.0f);
        CHECK(loaded.doc->byId("O/In/Dot").layoutBox().width == 7.0f);
    }
}

TEST_CASE("T-587 a <symbol>'s own sections are scoped to the symbol") {
    const std::string text = R"X(<xt version="2">
  <symbol name="Card">
    <div id="Box"><span id="Title"></span></div>
    <layout>
      #Box { width: 300px; height: 40px; }
    </layout>
    <style>
      #Box { background-color: #102030; }
    </style>
    <timelines>
      @timeline pop { #Box { 0s { opacity: 0 } 0.5s { opacity: 1 } } }
    </timelines>
  </symbol>
  <body><use symbol="Card" id="C1"/><div id="Box"></div></body>
</xt>)X";
    const CompileResult r = compileFiles({SourceFile{"sym.xt", text}});
    requireCompiled(r);
    LoadedDoc loaded(r.ir);
    CHECK(loaded.doc->byId("C1/Box").layoutBox().width == 300.0f);
    CHECK(loaded.doc->byId("Box").layoutBox().width != 300.0f);  // The document's own #Box.
}

TEST_CASE("T-587 on:<event>@<path> compiles to the TRIG of v1's .xts rule trigger; merge order kept") {
    // The symbol's inner #T1 has its own on:press and on:release; the instance adds press (same
    // event: the element's own attribute wins, in the rule's slot) and hover (a new event).
    const std::string symbol =
        R"X(<div id="Tabs"><div id="T1" on:press="sound(inner)" on:release="sound(innerUp)"></div>
      <div id="T2"></div></div>)X";
    const std::string v1 =
        "<xt><head><symbol name=\"Tabs\">" + symbol +
        "</symbol><style>#Pages/T1 { on-press: sound(outer); on-hover: sound(outerHover); }\n"
        "#Pages/T2 { on-press: sound(two); }</style></head>"
        "<body><use symbol=\"Tabs\" id=\"Pages\"/></body></xt>";
    const std::string v2 = "<xt version=\"2\"><symbol name=\"Tabs\">" + symbol +
                           "</symbol><body><use symbol=\"Tabs\" id=\"Pages\" on:press@T1=\"sound(outer)\" "
                           "on:hover@T1=\"sound(outerHover)\" on:press@T2=\"sound(two)\"/></body></xt>";
    const CompileResult a = compileFiles({SourceFile{"t.xt", v1}});
    const CompileResult b = compileFiles({SourceFile{"t.xt", v2}});
    requireCompiled(a);
    requireCompiled(b);
    const std::vector<uint8_t> trigA = chunkPayload(a.ir, "TRIG");
    REQUIRE_FALSE(trigA.empty());
    CHECK(trigA == chunkPayload(b.ir, "TRIG"));

    // Inside a symbol definition the trigger is scoped to that symbol like `symbol(X) { ... }`.
    const std::string nestedV1 =
        "<xt><head><symbol name=\"Tabs\">" + symbol +
        "</symbol><symbol name=\"Panel\"><div id=\"P\"><use symbol=\"Tabs\" id=\"Pages\"/>"
        "</div></symbol><style>symbol(Panel) { #Pages/T2 { on-press: sound(two); } }</style>"
        "</head><body><use symbol=\"Panel\" id=\"A\"/><use symbol=\"Panel\" id=\"B\"/></body></xt>";
    const std::string nestedV2 =
        "<xt version=\"2\"><symbol name=\"Tabs\">" + symbol +
        "</symbol><symbol name=\"Panel\"><div id=\"P\"><use symbol=\"Tabs\" id=\"Pages\" "
        "on:press@T2=\"sound(two)\"/></div></symbol>"
        "<body><use symbol=\"Panel\" id=\"A\"/><use symbol=\"Panel\" id=\"B\"/></body></xt>";
    const CompileResult c = compileFiles({SourceFile{"n.xt", nestedV1}});
    const CompileResult d = compileFiles({SourceFile{"n.xt", nestedV2}});
    requireCompiled(c);
    requireCompiled(d);
    CHECK(chunkPayload(c.ir, "TRIG") == chunkPayload(d.ir, "TRIG"));

    const CompileResult notUse = compileV2("<div id=\"X\" on:press@T1=\"sound(a)\"></div>", "");
    CHECK_FALSE(notUse.ok());
}

TEST_CASE("T-587 canonical writer: section order, indentation, timelines by name, round trip") {
    const std::string text = R"X(<xt version="2">
  <timelines>
    @timeline zoom { #A { 0s { opacity: 0 } 1s { opacity: 1 } } }
    /* the intro */
    @timeline intro { #A { 0s { opacity: 0 } } }
  </timelines>
  <style>
    /* surface */
    #A, .b { color: red; background-color: rgba(0, 0, 0, 0.5) }
    #A { display: none; }
  </style>
  <body on:show="play(intro)"><div id="A" class="b">Hi &amp; bye</div></body>
  <layout>#A { width: 10px; height: 20px; }
    @media (max-width: 1000px) { #A { width: 5px; } }
    /* end */
  </layout>
  <style src="extra.xts"/>
  <ports>
    // the score
    score: number in = 0
    secret: string in hidden
  </ports>
  <symbol name="Dot" requires="slots: x">
    <div id="Dot"></div>
    <ports>on: bool in</ports>
    <layout>#Dot { width: 4px; }</layout>
  </symbol>
  <head><title>A &lt; B</title><symbols src="lib.xt"/></head>
</xt>
)X";
    DiagnosticSink sink;
    CanonicalXt first;
    REQUIRE(readCanonicalXt(text, "c.xt", sink, first));
    CHECK(first.title == "A < B");
    REQUIRE(first.symbols.size() == 1);
    CHECK(first.symbols[0].ports.size() == 1);
    REQUIRE(first.ports.size() == 2);
    CHECK(first.ports[0].comments == std::vector<std::string>{"// the score"});
    CHECK(first.ports[1].hidden);
    REQUIRE(first.style.size() == 2);
    CHECK(first.style[1].src == "extra.xts");
    REQUIRE(first.layout.size() == 1);
    CHECK(first.layout[0].trailingComments == std::vector<std::string>{"/* end */"});

    const std::string written = writeCanonicalXt(first);
    const std::string expected = R"X(<xt version="2">
  <head>
    <title>A &lt; B</title>
    <symbols src="lib.xt"/>
  </head>

  <symbol name="Dot" requires="slots: x">
    <ports>
      on: bool in
    </ports>
    <div id="Dot"></div>
    <layout>
      #Dot {
        width: 4px;
      }
    </layout>
  </symbol>

  <ports>
    // the score
    score: number in = 0
    secret: string in hidden
  </ports>

  <body on:show="play(intro)"><div id="A" class="b">Hi &amp; bye</div></body>

  <layout>
    #A {
      width: 10px;
      height: 20px;
    }
    @media (max-width: 1000px) {
      #A {
        width: 5px;
      }
    }
    /* end */
  </layout>
  <style>
    /* surface */
    #A,
    .b {
      color: red;
      background-color: rgba(0, 0, 0, 0.5);
    }
    #A {
      display: none;
    }
  </style>
  <style src="extra.xts"/>
  <timelines>
    /* the intro */
    @timeline intro {
      #A {
        0s {
          opacity: 0;
        }
      }
    }
    @timeline zoom {
      #A {
        0s {
          opacity: 0;
        }
        1s {
          opacity: 1;
        }
      }
    }
  </timelines>
</xt>
)X";
    CHECK(written == expected);

    // Round trip: read -> write -> read gives the same structure and the same elements.
    CanonicalXt second;
    REQUIRE(readCanonicalXt(written, "c.xt", sink, second));
    first.timelines[0].items = second.timelines[0].items;  // The writer sorts timelines by name.
    CHECK(first == second);
    CHECK(writeCanonicalXt(second) == written);
    DomTree treeA;
    DomTree treeB;
    REQUIRE(parseXt(text, "c.xt", sink, treeA));
    REQUIRE(parseXt(written, "c.xt", sink, treeB));
    CHECK(sameElements(treeA, treeB));
    CHECK_FALSE(sink.hasErrors());

    DiagnosticSink v1Sink;
    CanonicalXt v1;
    CHECK_FALSE(readCanonicalXt("<xt><body></body></xt>", "v1.xt", v1Sink, v1));
}

// ---- §54.2: the one-page example against the same content written in v1 --------------------
// `anchor` / `offset` / `size` (54 §54.5.1) arrive with T-588; until then both versions write
// their §54.5.1 expansion by hand.

namespace {

constexpr const char* kOnePageV2 = R"X(<xt version="2">
  <head>
    <title>HUD</title>
    <symbols src="../../shared/symbols/HealthBar.xt"/>
  </head>

  <ports>
    hp:        number in = 0.8
    objective: string in = "@hud_objective_default"
    hit:       event in
  </ports>

  <body on:show="play(intro)" on:port(hit)="play(hurt)">
    <div id="Screen">
      <div id="Hurt"></div>
      <div id="TopLeft">
        <use symbol="HealthBar" id="Health" slot:label="@hud_hp" bind:hp="hp | scale(100) | round" hp_max="100"/>
      </div>
    </div>
  </body>

  <layout>
    #Screen  { width: 100vw; height: 100vh; }
    #Hurt    { position: absolute; left: 0; right: 0; top: 0; bottom: 0; }
    #TopLeft { position: absolute; left: calc(1safe-left + 24px); top: calc(1safe-top + 24px); display: flex; flex-direction: column; gap: 8px; }
    @media (max-width: 1000px) { #TopLeft { left: calc(1safe-left + 12px); top: calc(1safe-top + 12px); } }
  </layout>

  <style src="../../shared/base.xts"/>
  <style>
    #Hurt { background-color: rgba(200, 30, 30, 0.6); opacity: 0; }
  </style>

  <timelines>
    @timeline intro { #TopLeft { 0s { opacity: 0.25; translate: -24px 0 } 0.3s ease-out { opacity: 1; translate: 0 0 } } }
    @timeline hurt  { #Hurt { 0s { opacity: 0 } 0.06s linear { opacity: 0.85 } 0.3s ease-out { opacity: 0 } } }
  </timelines>
</xt>
)X";

constexpr const char* kOnePageV1 = R"X(<xt>
  <head>
    <title>HUD</title>
    <link rel="symbols" href="../../shared/symbols/HealthBar_v1.xt"/>
    <link href="../../shared/base.xts"/>
    <style>
      #Screen  { width: 100vw; height: 100vh; }
      #Hurt    { position: absolute; left: 0; right: 0; top: 0; bottom: 0; }
      #TopLeft { position: absolute; left: calc(1safe-left + 24px); top: calc(1safe-top + 24px); display: flex; flex-direction: column; gap: 8px; }
      @media (max-width: 1000px) { #TopLeft { left: calc(1safe-left + 12px); top: calc(1safe-top + 12px); } }
      #Hurt { background-color: rgba(200, 30, 30, 0.6); opacity: 0; }
      @timeline intro { #TopLeft { 0s { opacity: 0.25; translate: -24px 0 } 0.3s ease-out { opacity: 1; translate: 0 0 } } }
      @timeline hurt  { #Hurt { 0s { opacity: 0 } 0.06s linear { opacity: 0.85 } 0.3s ease-out { opacity: 0 } } }
    </style>
  </head>
  <body on:show="play(intro)" on:port(hit)="play(hurt)" port:hp="number in = 0.8"
        port:objective="string in = &quot;@hud_objective_default&quot;" port:hit="event in">
    <div id="Screen">
      <div id="Hurt"></div>
      <div id="TopLeft">
        <use symbol="HealthBar" id="Health" slot:label="@hud_hp" bind:hp="hp | scale(100) | round" hp_max="100"/>
      </div>
    </div>
  </body>
</xt>
)X";

constexpr const char* kHealthBarV2 = R"X(<xt version="2">
  <symbol name="HealthBar" requires="slots: label">
    <ports>
      hp:     number in = 0
      hp_max: number in = 100
    </ports>
    <div id="Bar">
      <span id="Label"><slot name="label">HP</slot></span>
      <div id="Track"><div id="Fill"></div></div>
    </div>
    <layout>
      #Bar   { display: flex; flex-direction: column; gap: 4px; width: 240px; }
      #Track { height: 12px; }
      #Fill  { width: 50%; height: 100%; }
    </layout>
    <style src="HealthBar.style.xts"/>
  </symbol>
</xt>
)X";

constexpr const char* kHealthBarV1 = R"X(<xt>
  <head>
    <symbol name="HealthBar" requires="slots: label">
      <div id="Bar" port:hp="number in = 0" port:hp_max="number in = 100">
        <span id="Label"><slot name="label">HP</slot></span>
        <div id="Track"><div id="Fill"></div></div>
      </div>
    </symbol>
    <style>
      symbol(HealthBar) {
        #Bar   { display: flex; flex-direction: column; gap: 4px; width: 240px; }
        #Track { height: 12px; }
        #Fill  { width: 50%; height: 100%; }
        #Track { background-color: #222222; }
        #Fill  { background-color: #33cc33; }
      }
    </style>
  </head>
</xt>
)X";

}  // namespace

TEST_CASE("T-587 §54.2 one-page example: v2 and the same v1 content compute the same styles") {
    const std::vector<SourceFile> shared = {
        SourceFile{"../../shared/symbols/HealthBar.xt", kHealthBarV2},
        SourceFile{"../../shared/symbols/HealthBar_v1.xt", kHealthBarV1},
        SourceFile{"../../shared/symbols/HealthBar.style.xts",
                   "#Track { background-color: #222222; }\n#Fill { background-color: #33cc33; }\n"},
        SourceFile{"../../shared/base.xts", ":root { --hud-gap: 8px; }\n#Screen { color: #f0f0f0; }\n"}};
    std::vector<SourceFile> v2Files{SourceFile{"hud.xt", kOnePageV2}};
    std::vector<SourceFile> v1Files{SourceFile{"hud.xt", kOnePageV1}};
    v2Files.insert(v2Files.end(), shared.begin(), shared.end());
    v1Files.insert(v1Files.end(), shared.begin(), shared.end());
    const CompileResult v2 = compileFiles(v2Files);
    const CompileResult v1 = compileFiles(v1Files);
    requireCompiled(v2);
    requireCompiled(v1);
    checkSameComputedStyle(v1.ir, v2.ir);
    // Layer one (report only, §54.11): v1 puts every <link> before every <style> and v2 merges the
    // sections in file order, so the rule order -- and with it the IR bytes -- may differ.
    MESSAGE("54.2 layer one (IR bytes equal): " << std::string(v1.ir == v2.ir ? "yes" : "no"));
}
