#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/flow/host_ports.h"
#include "xt/text_shaper.h"

/// T-867 item 2: a declared `out` port with a default value reads that default before its first
/// `set`, at all three read points -- a binding, a flow edge from the port, the host `getPort` --
/// (Docs/design/27-interaction.md §27.3, the `port:` row: "執行期、畫布、截圖、展示一律以預設值
/// 起始"; in and out alike). Source: a conversion report on 0.19.2 (`d_tmid.out = -0.45`: a flow
/// `math sub` fed by `#document.mid_o`, declared `number out = 0.7`, read 0).

using namespace xt;
using namespace xt::flow;

namespace {

constexpr const char* kDoc = R"X(<xt><body id="Root" port:mid_o="number out = 0.7">
  <div id="Hud" port:hp_o="number out = 42" bind:text="format('{0} {1}', mid_o, hp_o)"></div>
</body></xt>)X";

std::unique_ptr<Document> loadDoc(TextShaper& shaper) {
    const compile::SourceFile file{"outdef.xt", kDoc};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "outdef.xt", options);
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

struct Values {
    const FlowRuntime& rt;
    double num(const std::string& name) const {
        const std::vector<std::string>& names = rt.valueNames();
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return rt.values()[i].n;
            }
        }
        FAIL("no flow value " << name);
        return -1.0;
    }
};

constexpr const char* kGraph = R"({
    "version": 1,
    "nodes": [
        { "id": "k", "type": "constant", "params": { "value": 0.45 } },
        { "id": "d", "type": "math", "params": { "op": "sub" } },
        { "id": "h", "type": "math", "params": { "op": "sub" } },
        { "id": "z", "type": "constant", "params": { "value": 0 } }
    ],
    "edges": [
        { "from": "#document.mid_o", "to": "d.a" },
        { "from": "k.out", "to": "d.b" },
        { "from": "#Hud.hp_o", "to": "h.a" },
        { "from": "z.out", "to": "h.b" }
    ]
})";

}  // namespace

TEST_CASE("T-867 out port default: binding, host getPort and flow all read the declared default") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper);

    // (c) the host.
    const PortId mid = doc->lookupPort("mid_o");
    REQUIRE(mid != kInvalidPort);
    CHECK(doc->getPort(mid).f == doctest::Approx(0.7f));
    CHECK(doc->getPort(doc->lookupPort("Hud.hp_o")).f == doctest::Approx(42.0f));
    HostPorts ports;
    REQUIRE(ports.bind(*doc) == Status::kOk);
    CHECK(ports.get(ports.lookup("mid_o")).f == doctest::Approx(0.7f));

    // (a) a binding.
    CHECK(std::string(doc->byId("Hud").text()) == "0.7 42");

    // (b) flow edges out of the ports, before any set().
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(kGraph, graph, errors) == Status::kOk);
    FlowRuntime rt;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
    const Values v{rt};
    CHECK(v.num("d.out") == doctest::Approx(0.25));  // 0.7 - 0.45, not 0 - 0.45
    CHECK(v.num("h.out") == doctest::Approx(42.0));
    doc->update(0.016f);
    CHECK(v.num("d.out") == doctest::Approx(0.25));

    // A later set still flows through (the default only seeds).
    REQUIRE(doc->setPort(mid, Value::ofFloat(1.0f)) == Status::kOk);
    doc->update(0.032f);
    CHECK(v.num("d.out") == doctest::Approx(0.55));
}
