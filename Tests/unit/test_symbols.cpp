#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/validate.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Symbols: `<symbol>`/`<use>`/`<slot>` expansion, `symbol(Name){}` rules, overrides, variants,
/// contracts, cycles and the `SYMB` IR block. See Docs/design/26-layers-symbols-library.md §26.2,
/// 12-compiler.md §12.7, Docs/tasks/T-171.md.

using namespace xt;
using namespace xt::compile;

namespace {

constexpr std::string_view kButtonDoc = R"(<xt>
  <head>
    <symbol name="Button" origin="center" variants="Kind=Primary|Secondary"
            requires="states: Hover; slots: label">
      <div id="Bg" class="btn-bg"></div>
      <span id="Label"><slot name="label">Button</slot></span>
    </symbol>
    <style>
      symbol(Button) {
        #Bg { width: 80px; height: 20px; background-color: #333333; }
        :hover div#Bg { background-color: #555555; }
        .v-Kind-Primary div#Bg { background-color: #0000ff; }
      }
    </style>
  </head>
  <body>
    <use symbol="Button" id="A"/>
    <use symbol="Button" id="B" prop:Bg.background-color="#33aa88"/>
    <use symbol="Button" id="C" variant:Kind="Primary" slot:label="Play"/>
  </body>
</xt>)";

CompileResult compileOne(std::string_view text) {
    std::array<SourceFile, 1> files{SourceFile{"doc.xt", text}};
    return compileDocumentFromStrings(files, "doc.xt", CompileOptions{});
}

bool hasErrorContaining(const CompileResult& result, const std::string& needle) {
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void requireNoErrors(const CompileResult& result) {
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
}

IrView openView(const CompileResult& result) {
    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    return r.value();
}

/// DOCT element index of the element whose id is `id`, or kInvalidNode.
uint32_t elementWithId(const IrView& view, std::string_view id) {
    std::span<const IrDocElement> els = view.doc().elements();
    for (uint32_t i = 0; i < els.size(); ++i) {
        if (els[i].id != kInvalidString && view.string(els[i].id) == id) {
            return i;
        }
    }
    return kInvalidNode;
}

std::string textOf(const IrView& view, uint32_t element) {
    for (const IrDocText& t : view.doc().texts()) {
        if (t.parent == element) {
            return std::string(view.string(t.text));
        }
    }
    return {};
}

bool hasClass(const IrView& view, uint32_t element, std::string_view cls) {
    const IrDocElement& el = view.doc().elements()[element];
    for (uint32_t i = 0; i < el.classCount; ++i) {
        if (view.string(view.doc().classPool()[el.classOffset + i]) == cls) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("symbols: xts symbol(Name){} block tags its rules") {
    DiagnosticSink sink;
    RuleSet rules;
    REQUIRE(
        parseXts("#x { opacity: 1; } symbol(Button) { #Bg { opacity: 0.5; } :hover #Bg { opacity: 1; } } "
                 "#y { opacity: 1; }",
                 "t.xts", 1, sink, rules));
    REQUIRE(rules.rules.size() == 4);
    CHECK(rules.rules[0].symbol.empty());
    CHECK(rules.rules[1].symbol == "Button");
    CHECK(rules.rules[2].symbol == "Button");
    CHECK(rules.rules[3].symbol.empty());
}

TEST_CASE("symbols: three instances expand to path ids, slots, variants and SYMB") {
    CompileResult result = compileOne(kButtonDoc);
    requireNoErrors(result);
    IrView view = openView(result);

    for (std::string_view id : {"A", "A/Bg", "A/Label", "B", "B/Bg", "B/Label", "C", "C/Bg", "C/Label"}) {
        CAPTURE(id);
        CHECK(elementWithId(view, id) != kInvalidNode);
    }
    // No <symbol>/<use>/<slot> survives, and inner ids are never left unprefixed.
    for (const IrDocElement& el : view.doc().elements()) {
        std::string_view tag = view.string(el.tag);
        CHECK(tag != "symbol");
        CHECK(tag != "use");
        CHECK(tag != "slot");
    }
    CHECK(elementWithId(view, "Bg") == kInvalidNode);

    uint32_t a = elementWithId(view, "A");
    uint32_t aBg = elementWithId(view, "A/Bg");
    CHECK(view.string(view.doc().elements()[a].tag) == "div");
    CHECK(view.doc().elements()[aBg].parent == a);
    CHECK(textOf(view, elementWithId(view, "A/Label")) == "Button");  // slot default
    CHECK(textOf(view, elementWithId(view, "C/Label")) == "Play");    // slot:label
    CHECK(hasClass(view, elementWithId(view, "C"), "v-Kind-Primary"));
    CHECK_FALSE(hasClass(view, a, "v-Kind-Primary"));
    CHECK(hasClass(view, aBg, "btn-bg"));

    const SymbolChunk& symb = view.symbols();
    REQUIRE(symb.present());
    REQUIRE(symb.instances().size() == 3);
    const char* roots[] = {"A", "B", "C"};
    for (size_t i = 0; i < 3; ++i) {
        const IrSymbolInstance& inst = symb.instances()[i];
        CHECK(view.string(inst.symbol) == "Button");
        CHECK(inst.instanceElement == elementWithId(view, roots[i]));
        REQUIRE(inst.paths.size() == 2);
        CHECK(view.string(inst.paths[0].path) == std::string(roots[i]) + "/Bg");
        CHECK(inst.paths[0].element == elementWithId(view, std::string(roots[i]) + "/Bg"));
        CHECK(view.string(inst.paths[1].path) == std::string(roots[i]) + "/Label");
    }
}

TEST_CASE("symbols: path ids resolve through Document::byId; overrides and variants style the instance") {
    CompileResult result = compileOne(kButtonDoc);
    requireNoErrors(result);
    NullTextShaper shaper;
    FileAssetResolver assets(".", nullptr);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, &assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);

    auto bgOf = [&](std::string_view id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return doc->arena().element(ref.node).style.base[prop::BackgroundColor].c;
    };
    auto near = [](Color c, float r, float g, float b) {
        return std::abs(c.r - r) < 0.01f && std::abs(c.g - g) < 0.01f && std::abs(c.b - b) < 0.01f;
    };
    CHECK(near(bgOf("A/Bg"), 0x33 / 255.0f, 0x33 / 255.0f, 0x33 / 255.0f));
    CHECK(near(bgOf("B/Bg"), 0x33 / 255.0f, 0xaa / 255.0f, 0x88 / 255.0f));  // prop: override
    CHECK(near(bgOf("C/Bg"), 0.0f, 0.0f, 1.0f));                             // variant class
    ElementRef aBg = doc->byId("A/Bg");
    CHECK(doc->arena().element(aBg.node).box.width == doctest::Approx(80.0f));
}

TEST_CASE("symbols: SYMB round-trips through IrWriter::fromView") {
    CompileResult result = compileOne(kButtonDoc);
    requireNoErrors(result);
    IrView view = openView(result);
    std::vector<uint8_t> again = IrWriter::fromView(view).finish();
    CHECK(again == result.ir);
}

TEST_CASE("symbols: a document without symbols has no SYMB chunk") {
    CompileResult result = compileOne("<xt><body><div id=\"a\"></div></body></xt>");
    requireNoErrors(result);
    CHECK_FALSE(openView(result).symbols().present());
}

TEST_CASE("symbols: structural prop override is an error") {
    CompileResult result = compileOne(R"(<xt><head>
      <symbol name="Button"><div id="Bg"></div></symbol></head>
      <body><use symbol="Button" id="P" prop:Bg.left="10px"/></body></xt>)");
    CHECK_FALSE(result.ok());
    CHECK(hasErrorContaining(result, "structural override not allowed"));
}

TEST_CASE("symbols: override errors (unknown path, attr, variant, slot)") {
    CHECK(hasErrorContaining(compileOne(R"(<xt><head><symbol name="S"><div id="Bg"></div></symbol></head>
      <body><use symbol="S" id="P" prop:Nope.color="#fff"/></body></xt>)"),
                             "unknown internal path"));
    CHECK(hasErrorContaining(compileOne(R"(<xt><head><symbol name="S"><div id="Bg"></div></symbol></head>
      <body><use symbol="S" id="P" attr:Bg.class="x"/></body></xt>)"),
                             "structural override not allowed"));
    CHECK(hasErrorContaining(
        compileOne(R"(<xt><head><symbol name="S" variants="Kind=A|B"><div id="Bg"></div></symbol></head>
      <body><use symbol="S" id="P" variant:Kind="C"/></body></xt>)"),
        "unknown variant"));
    CHECK(hasErrorContaining(compileOne(R"(<xt><head><symbol name="S"><div id="Bg"></div></symbol></head>
      <body><use symbol="S" id="P" slot:label="x"/></body></xt>)"),
                             "no slot \"label\""));
    CHECK(hasErrorContaining(compileOne(R"(<xt><body><use symbol="Missing" id="P"/></body></xt>)"),
                             "unknown symbol \"Missing\""));
    CHECK(hasErrorContaining(compileOne(R"(<xt><head><symbol name="S"><div id="Bg"></div></symbol></head>
      <body><use symbol="S"/></body></xt>)"),
                             "requires an id"));
}

TEST_CASE("symbols: attr: override sets the copied element's attribute") {
    CompileResult result =
        compileOne(R"(<xt><head><symbol name="S"><img id="Icon" src="a.png"/></symbol></head>
      <body><use symbol="S" id="P" attr:Icon.src="engine://b.png"/></body></xt>)");
    requireNoErrors(result);
    IrView view = openView(result);
    uint32_t icon = elementWithId(view, "P/Icon");
    REQUIRE(icon != kInvalidNode);
    const IrDocElement& el = view.doc().elements()[icon];
    bool found = false;
    for (uint32_t i = 0; i < el.attrCount; ++i) {
        const IrAttr& a = view.doc().attrPool()[el.attrOffset + i];
        if (view.string(a.name) == "src") {
            CHECK(view.string(a.value) == "engine://b.png");
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("symbols: requires contract -- missing state rule or slot is an error") {
    CompileResult noHover = compileOne(R"(<xt><head>
      <symbol name="Button" requires="states: Hover"><div id="Bg"></div></symbol>
      <style>symbol(Button) { #Bg { opacity: 1; } }</style></head>
      <body><use symbol="Button" id="P"/></body></xt>)");
    CHECK_FALSE(noHover.ok());
    CHECK(hasErrorContaining(noHover, "Button"));
    CHECK(hasErrorContaining(noHover, "requires state \"Hover\""));

    CompileResult noSlot = compileOne(R"(<xt><head>
      <symbol name="Button" requires="slots: label"><div id="Bg"></div></symbol></head>
      <body><div></div></body></xt>)");
    CHECK_FALSE(noSlot.ok());
    CHECK(hasErrorContaining(noSlot, "requires slot \"label\""));
}

TEST_CASE("T-392: requires checks each selector of a group; :focus-within and :state(Focused) count") {
    // XF-076: `#Box:focus-within, :state(Focused) #Box` -- either selector satisfies Focused.
    CompileResult grouped = compileOne(R"(<xt><head>
      <symbol name="Field" requires="states: Focused"><div id="Box"></div></symbol>
      <style>symbol(Field) { #Box:focus-within, :state(Focused) #Box { opacity: 0.5; } }</style></head>
      <body><use symbol="Field" id="F"/></body></xt>)");
    requireNoErrors(grouped);
    // Only the second selector: :state(Focused) is the built-in :focus, so no custom state slot.
    CompileResult stateOnly = compileOne(R"(<xt><head>
      <symbol name="Field" requires="states: Focused"><div id="Box"></div></symbol>
      <style>symbol(Field) { #Box.x, :state(Focused) #Box { opacity: 0.5; } }</style></head>
      <body><use symbol="Field" id="F"/></body></xt>)");
    requireNoErrors(stateOnly);
    CHECK(openView(stateOnly).customStates().names().empty());
    // Only :focus-within.
    CompileResult withinOnly = compileOne(R"(<xt><head>
      <symbol name="Field" requires="states: Focused"><div id="Box"></div></symbol>
      <style>symbol(Field) { #Box.x, #Box:focus-within { opacity: 0.5; } }</style></head>
      <body><use symbol="Field" id="F"/></body></xt>)");
    requireNoErrors(withinOnly);
    // Neither selector of the group: still an error.
    CompileResult neither = compileOne(R"(<xt><head>
      <symbol name="Field" requires="states: Focused"><div id="Box"></div></symbol>
      <style>symbol(Field) { #Box.x, #Box:hover { opacity: 0.5; } }</style></head>
      <body><use symbol="Field" id="F"/></body></xt>)");
    CHECK(hasErrorContaining(neither, "requires state \"Focused\""));
}

TEST_CASE("symbols: a cycle between symbols is an error") {
    CompileResult result = compileOne(R"(<xt><head>
      <symbol name="A"><use symbol="B" id="b"/></symbol>
      <symbol name="B"><use symbol="A" id="a"/></symbol></head>
      <body><use symbol="A" id="X"/></body></xt>)");
    CHECK_FALSE(result.ok());
    CHECK(hasErrorContaining(result, "symbol cycle"));
}

TEST_CASE("symbols: nested symbols, element slot fills and a symbol defined in <body>") {
    CompileResult result = compileOne(R"(<xt><head>
      <symbol name="Icon"><img id="Img" src="a.png"/></symbol>
      <symbol name="Button">
        <use symbol="Icon" id="Ic" prop:Img.opacity="0.5"/>
        <div id="Body"><slot name="content"><span id="Default">x</span></slot></div>
      </symbol></head>
      <body>
        <symbol name="Unused"><div id="Z"></div></symbol>
        <use symbol="Button" id="P"><slot name="content"><b id="Outer">Go</b></slot></use>
        <use symbol="Button" id="Q"/>
      </body></xt>)");
    requireNoErrors(result);
    IrView view = openView(result);
    CHECK(elementWithId(view, "P/Ic") != kInvalidNode);
    CHECK(elementWithId(view, "P/Ic/Img") != kInvalidNode);
    // The fill belongs to the using document: its id is not prefixed.
    uint32_t outer = elementWithId(view, "Outer");
    REQUIRE(outer != kInvalidNode);
    CHECK(view.doc().elements()[outer].parent == elementWithId(view, "P/Body"));
    CHECK(elementWithId(view, "P/Default") == kInvalidNode);
    CHECK(elementWithId(view, "Q/Default") != kInvalidNode);
    CHECK(elementWithId(view, "Z") == kInvalidNode);

    const SymbolChunk& symb = view.symbols();
    REQUIRE(symb.instances().size() == 4);  // P, P/Ic, Q, Q/Ic -- outer before nested.
    CHECK(view.string(symb.instances()[0].symbol) == "Button");
    CHECK(symb.instances()[0].instanceElement == elementWithId(view, "P"));
    CHECK(view.string(symb.instances()[1].symbol) == "Icon");
    CHECK(symb.instances()[1].instanceElement == elementWithId(view, "P/Ic"));
    REQUIRE(symb.instances()[1].paths.size() == 1);
    CHECK(view.string(symb.instances()[1].paths[0].path) == "P/Ic/Img");
    // P's internal paths: P/Ic, P/Ic/Img, P/Body (not the fill's "Outer").
    CHECK(symb.instances()[0].paths.size() == 3);
}

TEST_CASE("symbols: <link rel=\"symbols\"> loads definitions and their symbol() rules from a library") {
    constexpr std::string_view lib = R"(<xt><head>
      <symbol name="Chip"><div id="Bg"></div></symbol>
      <style>symbol(Chip) { #Bg { width: 30px; height: 10px; } }</style></head></xt>)";
    constexpr std::string_view main = R"(<xt><head><link rel="symbols" href="shared/symbols/chips.xt"/></head>
      <body><use symbol="Chip" id="K"/></body></xt>)";
    std::array<SourceFile, 2> files{SourceFile{"doc.xt", main}, SourceFile{"shared/symbols/chips.xt", lib}};
    CompileResult result = compileDocumentFromStrings(files, "doc.xt", CompileOptions{});
    requireNoErrors(result);
    IrView view = openView(result);
    CHECK(elementWithId(view, "K/Bg") != kInvalidNode);
    CHECK(view.symbols().instances().size() == 1);
    // The library is not a stylesheet link.
    CHECK(view.style().rules().size() == 1);
}

// T-258b (Docs/reviews/2026-10-02-full-audit-raw/D.md D7): expansion stops past 1000000 nodes.
TEST_CASE("symbols: T-258b expansion past 1000000 nodes is an error") {
    // Fan-out: Outer holds 1000 Inner instances of 1000 elements each, about 1001000 nodes. The
    // second <use> has no id, an error the rebuild reports after the expansion, so the compile
    // stops right after it either way.
    std::string text = "<xt><head><symbol name=\"Inner\">";
    for (int i = 0; i < 1000; ++i) {
        text += "<div></div>";
    }
    text += "</symbol><symbol name=\"Outer\">";
    for (int i = 0; i < 1000; ++i) {
        text += "<use symbol=\"Inner\" id=\"u" + std::to_string(i) + "\"/>";
    }
    text += "</symbol></head><body><use symbol=\"Outer\" id=\"X\"/><use symbol=\"Inner\"/></body></xt>";
    CompileResult result = compileOne(text);
    CHECK_FALSE(result.ok());
    CHECK(hasErrorContaining(result, "symbol expansion exceeds 1000000 nodes"));
}

// Deep nesting: every finished instance keeps the path id of each node in it, about 20 per node at
// this depth, so the 1000000 instance path budget stops it long before the node budget (with the
// node budget alone this took 110 s and 7 GB in a Debug build).
TEST_CASE("symbols: T-258b S0..S30, each using the next twice, is an error") {
    // 2^30 instances without the budgets.
    std::string text = "<xt><head>";
    for (int k = 0; k < 30; ++k) {
        const std::string next = "S" + std::to_string(k + 1);
        text += "<symbol name=\"S" + std::to_string(k) + "\"><use symbol=\"" + next +
                "\" id=\"a\"/><use symbol=\"" + next + "\" id=\"b\"/></symbol>";
    }
    text += "<symbol name=\"S30\"><div></div></symbol></head><body><use symbol=\"S0\" id=\"X\"/></body></xt>";
    CompileResult result = compileOne(text);
    CHECK_FALSE(result.ok());
    CHECK(hasErrorContaining(result, "symbol expansion exceeds 1000000 instance paths"));
    CHECK_FALSE(hasErrorContaining(result, "symbol expansion exceeds 1000000 nodes"));  // One error.
}

// -- T-382 (Aux Duty XF-026): a symbol rule that starts with an ancestor class never matches ------

namespace {

size_t ancestorClassWarnings(const CompileResult& result) {
    size_t n = 0;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning &&
            d.message.find("never matches at compile time (runtime addClass is not visible here): "
                           "symbol-scoped rules cannot see ancestor classes; put the class on a <use "
                           "symbol=") != std::string::npos) {
            ++n;
        }
    }
    return n;
}

std::string cardDoc(std::string_view rules, std::string_view useAttrs, std::string_view inner = "") {
    return std::string(R"(<xt><head>
    <symbol name="Card" variants="Size=S|L"><div id="Box" class="box">)") +
           std::string(inner) + R"(</div></symbol>
    <style> symbol(Card) { )" +
           std::string(rules) + R"( } </style>
  </head><body><div class="layout-tall"><use symbol="Card" id="C" )" +
           std::string(useAttrs) + R"(/></div></body></xt>)";
}

}  // namespace

TEST_CASE("symbols: T-382 a symbol rule starting with an ancestor class warns that it never matches") {
    // T-383: an inner element (#In); `#Box` is the symbol's root, which the class may be on.
    const std::string doc = cardDoc(".layout-tall #In { width: 10px; }", "", "<div id=\"In\"></div>");
    const CompileResult result = compileOne(doc);
    REQUIRE(result.ok());
    CHECK(ancestorClassWarnings(result) == 1);
    bool named = false;
    for (const Diagnostic& d : result.diagnostics) {
        named = named || d.message.find("symbol(Card) rule starting with .layout-tall") != std::string::npos;
    }
    CHECK(named);
}

TEST_CASE("symbols: T-382 the ancestor-class lint skips classes the symbol or its instances can carry") {
    // A variant class, a class of an element inside the symbol, a class on the <use>, a class bound
    // on the <use>, a class a trigger inside the symbol sets, a one-part rule and a class compound
    // with a state: none of them warns.
    CHECK(ancestorClassWarnings(compileOne(cardDoc(".v-Size-L #Box { width: 10px; }", ""))) == 0);
    CHECK(ancestorClassWarnings(compileOne(cardDoc(".box #Box { width: 10px; }", ""))) == 0);
    CHECK(ancestorClassWarnings(compileOne(cardDoc(".wide #Box { width: 10px; }", "class=\"wide\""))) == 0);
    CHECK(ancestorClassWarnings(compileOne(
              cardDoc(".wide #Box { width: 10px; }", "port:w=\"bool in\" bind:class.wide=\"w\""))) == 0);
    CHECK(ancestorClassWarnings(compileOne(cardDoc(
              ".open #Box { width: 10px; }", "", "<div id=\"T\" on:click=\"class(open, toggle)\"></div>"))) ==
          0);
    CHECK(ancestorClassWarnings(compileOne(cardDoc(".layout-tall { width: 10px; }", ""))) == 0);
    CHECK(ancestorClassWarnings(compileOne(cardDoc(".layout-tall:hover #Box { width: 10px; }", ""))) == 0);
}

// -- T-382: validateBindingTarget knows a <use>'s in-ports (27 §27.3.4, the T-380 ruling) ---------

TEST_CASE("symbols: T-382 symbolInPorts lists a symbol's in-ports and validateBindingTarget accepts them") {
    constexpr std::string_view kLib = R"(<xt><head>
    <link rel="symbols" href="more.xt"/>
    <symbol name="Row">
      <div id="Box" port:title="string in" port:count="number" port:pick="event out" port:sel="bool out"
           port:hit="event in" port:width="number in = 3">
        <use symbol="Badge" id="B"/>
      </div>
    </symbol>
    <symbol name="Badge"><div id="Dot" port:label="string in"></div></symbol>
  </head></xt>)";
    std::vector<std::string> ports;
    std::vector<std::string> hrefs;
    REQUIRE(symbolInPorts(kLib, "Row", ports, &hrefs));
    // In (explicit or default) and not an event; the nested Badge's `label` is its own.
    CHECK(ports == std::vector<std::string>{"title", "count", "width"});
    CHECK(hrefs == std::vector<std::string>{"more.xt"});
    CHECK_FALSE(symbolInPorts(kLib, "Nope", ports));
    CHECK(ports.empty());
    CHECK_FALSE(symbolInPorts("<xt><head", "Row", ports));

    REQUIRE(symbolInPorts(kLib, "Row", ports));
    std::string error;
    CHECK(validateBindingTarget("title", ports, error));
    CHECK(validateBindingTarget("width", ports, error));  // The port, not the property.
    CHECK(validateBindingTarget("opacity", ports, error));
    CHECK_FALSE(validateBindingTarget("title", error));        // Context-free: an unknown property.
    CHECK_FALSE(validateBindingTarget("pick", ports, error));  // An event out port is no target.
    CHECK(error.find("pick") != std::string::npos);
}

// -- T-383 (Aux Duty XF-042): the ancestor-class lint skips rules on the symbol's root -------------

TEST_CASE("symbols: T-383 the ancestor-class lint skips `.cls #Root` when #Root is the symbol's root") {
    // #Box is Card's top-level element: the instance root may get the class at run time.
    CHECK(ancestorClassWarnings(compileOne(cardDoc(".layout-tall #Box { width: 10px; }", ""))) == 0);
    CHECK(ancestorClassWarnings(compileOne(
              cardDoc(".layout-tall #Box #In { width: 10px; }", "", "<div id=\"In\"></div>"))) == 0);
    // An inner element still warns, with the new message naming the symbol.
    const CompileResult r =
        compileOne(cardDoc(".layout-tall #In { width: 10px; }", "", "<div id=\"In\"></div>"));
    CHECK(ancestorClassWarnings(r) == 1);
    bool named = false;
    for (const Diagnostic& d : r.diagnostics) {
        named =
            named || d.message.find("put the class on a <use symbol=Card> to silence") != std::string::npos;
    }
    CHECK(named);
}
