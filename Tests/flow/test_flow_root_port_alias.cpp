#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/text_shaper.h"

/// T-604 (Docs/design/53-flow-language.md, "v2 document port addressing"): a UI-flow endpoint
/// `#<id>.<port>` whose element is a direct child of `<body>` (the UI root), declares no such port
/// itself, while the document declares one, binds to the document port; the Document-aware
/// lintFlowGraph() reports it as info `flow.rootPortAlias`.

using namespace xt;
using namespace xt::flow;

namespace {

/// A v2 document: its ports live in `<ports>` (lowered onto `<body>`); `#Screen` is the UI root.
constexpr const char* kV2Doc = R"X(<xt version="2">
  <ports>
    hp: number in = 0.5
    score: number out
  </ports>
  <body id="Root"><div id="Screen"><div id="Inner"></div></div></body>
</xt>)X";

/// The UI root `#Screen` is an instance of a symbol declaring its own `hp`.
constexpr const char* kOwnPortDoc = R"X(<xt version="2">
  <symbol name="Panel">
    <ports>
      hp: number in = 0.25
    </ports>
    <div id="Box"></div>
  </symbol>
  <ports>
    hp: number in = 0.5
  </ports>
  <body id="Root"><use symbol="Panel" id="Screen"/></body>
</xt>)X";

std::string joinedLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line + "\n";
    }
    return out;
}

std::unique_ptr<Document> loadDoc(TextShaper& shaper, const char* source) {
    const compile::SourceFile file{"alias.xt", source};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "alias.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 200, 200});
    doc->update(0.0f);
    return doc;
}

FlowGraph parseGraph(const std::string& json) {
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(FlowGraph::parse(json, graph, errors) == Status::kOk, joinedLines(errors));
    return graph;
}

/// One constant edge into `target`.
std::string constantInto(const std::string& target) {
    return R"({"version":1,"nodes":[{"id":"k","type":"constant","params":{"value":42}}],
        "edges":[{"from":"k.out","to":")" +
           target + R"("}]})";
}

size_t countCode(const std::vector<FlowLintFinding>& findings, const std::string& code) {
    size_t n = 0;
    for (const FlowLintFinding& finding : findings) {
        n += finding.code == code ? 1u : 0u;
    }
    return n;
}

}  // namespace

TEST_CASE("T-604: #<UI root>.<port> binds the v2 document port and reports one flow.rootPortAlias") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, kV2Doc);
    REQUIRE(doc->lookupPort("hp") != kInvalidPort);
    CHECK(doc->lookupPort("Screen.hp") == kInvalidPort);

    const FlowGraph graph = parseGraph(constantInto("#Screen.hp"));
    FlowRuntime rt;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(rt.load(graph, *doc, errors) == Status::kOk, joinedLines(errors));
    CHECK(doc->portText(doc->lookupPort("hp")) == "42");

    std::vector<FlowLintFinding> findings;
    lintFlowGraph(graph, *doc, findings);
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].code == "flow.rootPortAlias");
    CHECK(findings[0].severity == FlowLintFinding::Severity::Info);
    REQUIRE(findings[0].targets.size() == 1);
    CHECK(findings[0].targets[0] == "#Screen.hp");
    CHECK(findings[0].fix == "rewrite as #document.hp");
    CHECK(findings[0].message.find("document port 'hp'") != std::string::npos);

    // The graph-only lint knows nothing about the Document.
    std::vector<FlowLintFinding> graphOnly;
    lintFlowGraph(graph, graphOnly);
    CHECK(graphOnly.empty());

    // `#document.hp` is the canonical spelling: same binding, no finding.
    const FlowGraph canonical = parseGraph(constantInto("#document.hp"));
    FlowRuntime rt2;
    REQUIRE_MESSAGE(rt2.load(canonical, *doc, errors) == Status::kOk, joinedLines(errors));
    findings.clear();
    lintFlowGraph(canonical, *doc, findings);
    CHECK(findings.empty());
}

TEST_CASE("T-604: an output port of the document is reachable through the UI root too") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, kV2Doc);
    const FlowGraph graph = parseGraph(R"({"version":1,"nodes":[{"id":"a","type":"counter","params":{}},
        {"id":"b","type":"counter","params":{}}],
        "edges":[{"from":"#Screen.score","to":"a.set"},{"from":"#Screen.score","to":"b.set"}]})");
    FlowRuntime rt;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(rt.load(graph, *doc, errors) == Status::kOk, joinedLines(errors));

    std::vector<FlowLintFinding> findings;
    lintFlowGraph(graph, *doc, findings);
    REQUIRE(findings.size() == 1);  // One per distinct endpoint text.
    CHECK(findings[0].targets[0] == "#Screen.score");
    CHECK(findings[0].fix == "rewrite as #document.score");
}

TEST_CASE("T-604: an element that is not a direct child of <body> keeps the unknown-port error") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, kV2Doc);
    const FlowGraph graph = parseGraph(constantInto("#Inner.hp"));
    FlowRuntime rt;
    std::vector<std::string> errors;
    CHECK(rt.load(graph, *doc, errors) != Status::kOk);
    CHECK(joinedLines(errors).find("unknown input port 'hp' on '#Inner'") != std::string::npos);
    CHECK(doc->portText(doc->lookupPort("hp")) == "0.5");

    std::vector<FlowLintFinding> findings;
    lintFlowGraph(graph, *doc, findings);
    CHECK(countCode(findings, "flow.rootPortAlias") == 0);
}

TEST_CASE("T-604: a port the UI root declares itself wins over the document port") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, kOwnPortDoc);
    const PortId own = doc->lookupPort("Screen.hp");
    const PortId document = doc->lookupPort("hp");
    REQUIRE(own != kInvalidPort);
    REQUIRE(document != kInvalidPort);
    REQUIRE(own != document);

    const FlowGraph graph = parseGraph(constantInto("#Screen.hp"));
    FlowRuntime rt;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(rt.load(graph, *doc, errors) == Status::kOk, joinedLines(errors));
    CHECK(doc->portText(own) == "42");
    CHECK(doc->portText(document) == "0.5");

    std::vector<FlowLintFinding> findings;
    lintFlowGraph(graph, *doc, findings);
    CHECK(countCode(findings, "flow.rootPortAlias") == 0);
}
