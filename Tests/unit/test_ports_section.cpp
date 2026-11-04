#include <string>
#include <vector>

#include "doctest.h"
#include "ports_section.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt_v2_test_util.h"

/// T-587 (Docs/design/54-source-format-v2.md §54.4): the `<ports>` section grammar, its lowering
/// into the v1 port table, and the PORT chunk compared byte for byte with v1 `port:` attributes.

using namespace xt;
using namespace xt::compile;
using xt::test::v2::chunkPayload;
using xt::test::v2::compileFiles;
using xt::test::v2::requireCompiled;

TEST_CASE("T-587 <ports>: one declaration per line, file positions, hidden") {
    const std::string text =
        "\n  hp:        number in = 0.8\n  objective: string in = \"@hud_objective_default\"\n"
        "  hit:       event in\n  internal: number in hidden\n";
    DiagnosticSink sink;
    std::vector<PortsDecl> decls;
    REQUIRE(parsePortsSection(text, 10, 10, "p.xt", sink, decls));
    CHECK_FALSE(sink.hasErrors());
    REQUIRE(decls.size() == 4);
    CHECK(decls[0].name == "hp");
    CHECK(decls[0].value == "number in = 0.8");
    CHECK(decls[0].line == 11);
    CHECK(decls[0].col == 3);
    CHECK(decls[1].value == "string in = \"@hud_objective_default\"");
    CHECK(decls[2].value == "event in");
    CHECK_FALSE(decls[2].hidden);
    CHECK(decls[3].name == "internal");
    CHECK(decls[3].value == "number in");
    CHECK(decls[3].hidden);
}

TEST_CASE("T-587 <ports>: comments, a type over several lines, quoted text, an enum default named hidden") {
    const std::string text =
        "hp: number in /* inline */ = 1\n"
        "  /* host values */\n"
        "  items: list<struct{\n"
        "      title: string,\n"
        "      subtitle: string}> in = []\n"
        "  url: string in = \"http://x/y\"  // trailing\n"
        "  vis: enum(shown|hidden) in = hidden\n"
        "  tab: enum(low|mid|high) in = mid hidden\n";
    DiagnosticSink sink;
    std::vector<PortsDecl> decls;
    REQUIRE(parsePortsSection(text, 5, 20, "p.xt", sink, decls));
    REQUIRE(decls.size() == 5);
    CHECK(decls[0].value == "number in = 1");
    CHECK(decls[0].col == 20);  // The first line starts at the section's column.
    CHECK(decls[1].name == "items");
    CHECK(decls[1].value == "list<struct{ title: string, subtitle: string}> in = []");
    REQUIRE(decls[1].comments.size() == 1);
    CHECK(decls[1].comments[0] == "/* host values */");
    CHECK(decls[2].value == "string in = \"http://x/y\"");
    CHECK(decls[3].value == "enum(shown|hidden) in = hidden");
    CHECK_FALSE(decls[3].hidden);
    CHECK(decls[4].value == "enum(low|mid|high) in = mid");
    CHECK(decls[4].hidden);

    std::string type;
    std::string dir;
    std::string defaultText;
    bool hasDefault = false;
    splitPortsValue(decls[1].value, type, dir, defaultText, hasDefault);
    CHECK(type == "list<struct{ title: string, subtitle: string}>");
    CHECK(dir == "in");
    CHECK(hasDefault);
    CHECK(defaultText == "[]");
    splitPortsValue("event", type, dir, defaultText, hasDefault);
    CHECK(type == "event");
    CHECK(dir.empty());
    CHECK_FALSE(hasDefault);
}

TEST_CASE("T-587 <ports>: text that is no declaration is an error") {
    DiagnosticSink sink;
    std::vector<PortsDecl> decls;
    CHECK_FALSE(parsePortsSection("  number in\n", 3, 8, "p.xt", sink, decls));
    REQUIRE(sink.all().size() == 1);
    CHECK(sink.all()[0].line == 3);
    CHECK(sink.all()[0].col == 10);
}

TEST_CASE("T-587 <ports> lowers onto <body> and the symbol's first element; hidden is per port") {
    const std::string text = R"(<xt version="2">
  <symbol name="Bar">
    <ports>
      value: bool in
      secret: number in hidden
    </ports>
    <div id="Box"><span id="L"></span></div>
  </symbol>
  <ports>
    hp: number in = 0.8
  </ports>
  <body><use symbol="Bar" id="B1"/></body>
</xt>)";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "p.xt", sink, tree));
    CHECK(tree.formatVersion == 2);
    REQUIRE(tree.portSections.size() == 2);
    CHECK(tree.portSections[0].symbol == "Bar");
    CHECK(tree.portSections[1].symbol.empty());
    const DomNode& body = tree.nodes[tree.body];
    REQUIRE(body.attr("port:hp") != nullptr);
    CHECK(body.attr("port:hp")->value == "number in = 0.8");
    CHECK(body.attr("port:hp")->line == 10);
    REQUIRE(tree.symbolDefs.size() == 1);
    const DomNode& box = tree.nodes[tree.nodes[tree.symbolDefs[0]].children.front()];
    CHECK(box.attr("id")->value == "Box");
    REQUIRE(box.attr("port:secret") != nullptr);
    CHECK(box.attr("port:secret")->portHidden);
    CHECK_FALSE(box.attr("port:value")->portHidden);
}

TEST_CASE("T-587 <ports>: the PORT chunk is byte-identical to v1 port: attributes") {
    // Document ports with every type form, then symbol ports (all hidden = v1 expose:false).
    const std::string v1 = R"(<xt>
  <head>
    <symbol name="Bar"><div id="Box" expose:false port:value="bool in" port:hp="number in = 1"><span id="L"></span></div></symbol>
  </head>
  <body port:hp="number in = 0.8" port:objective="string in = &quot;@hud_objective_default&quot;"
        port:hit="event in" port:choose="event out" port:open="event call"
        port:tab="enum(low|mid|high) in = mid" port:items="list&lt;struct{title: string, n: number}&gt; in = []">
    <use symbol="Bar" id="B1"/>
  </body>
</xt>)";
    const std::string v2 = R"(<xt version="2">
  <symbol name="Bar">
    <ports>
      value: bool in hidden
      hp: number in = 1 hidden
    </ports>
    <div id="Box" expose:false><span id="L"></span></div>
  </symbol>
  <ports>
    hp: number in = 0.8
    objective: string in = "@hud_objective_default"
    hit: event in
    choose: event out
    open: event call
    tab: enum(low|mid|high) in = mid
    items: list<struct{title: string,
                       n: number}> in = []
  </ports>
  <body>
    <use symbol="Bar" id="B1"/>
  </body>
</xt>)";
    const CompileResult a = compileFiles({SourceFile{"ports.xt", v1}});
    const CompileResult b = compileFiles({SourceFile{"ports.xt", v2}});
    requireCompiled(a);
    requireCompiled(b);
    const std::vector<uint8_t> portsA = chunkPayload(a.ir, "PORT");
    REQUIRE_FALSE(portsA.empty());
    CHECK(portsA == chunkPayload(b.ir, "PORT"));
    CHECK(chunkPayload(a.ir, "STRS") == chunkPayload(b.ir, "STRS"));
}

TEST_CASE("T-587 <ports>: a per-port hidden matches expose:false on a single-port element") {
    const std::string v1 = R"(<xt><body><div id="A" expose:false port:x="number in"></div></body></xt>)";
    const std::string v2 =
        R"(<xt version="2"><ports>x: number in hidden</ports><body><div id="A"></div></body></xt>)";
    const CompileResult a = compileFiles({SourceFile{"h.xt", v1}});
    const CompileResult b = compileFiles({SourceFile{"h.xt", v2}});
    requireCompiled(a);
    requireCompiled(b);
    // v1 declares the port on #A, v2 on <body>: only the flags are compared.
    std::vector<uint8_t> portsA = chunkPayload(a.ir, "PORT");
    std::vector<uint8_t> portsB = chunkPayload(b.ir, "PORT");
    REQUIRE(portsA.size() >= 4 + sizeof(IrPort));
    REQUIRE(portsB.size() >= 4 + sizeof(IrPort));
    IrPort pa{};
    IrPort pb{};
    std::memcpy(&pa, portsA.data() + 4, sizeof(pa));
    std::memcpy(&pb, portsB.data() + 4, sizeof(pb));
    CHECK(pa.flags == pb.flags);
    CHECK((pb.flags & kPortExpose) == 0);
}
