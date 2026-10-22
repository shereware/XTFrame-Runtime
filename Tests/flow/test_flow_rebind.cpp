#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// FlowRuntime::rebind(): re-resolving a loaded graph against a reloaded Document -- listeners are
/// re-added, connection points whose element is gone are warned about and dropped, node state is
/// kept. See Docs/design/22-bundle.md §22.5, Docs/tasks/T-93.md "測試".

using namespace xt;
using namespace xt::flow;

namespace {

/// Compiles `<xt><body>...</body></xt>` in memory into a laid-out Document.
std::unique_ptr<Document> loadDoc(TextShaper& shaper, const std::string& body) {
    const std::string text =
        "<xt><head><style>button { width: 60px; height: 24px; } span { width: 60px; height: 24px; }"
        "</style></head><body>" +
        body + "</body></xt>";
    const compile::SourceFile file{"rebind.xt", text};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "rebind.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 400});
    doc->update(0.0f);
    return doc;
}

void click(Document& doc, const char* id) {
    ElementRef ref = doc.byId(id);
    REQUIRE(ref.valid());
    const Rect box = ref.boundingBox();
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    doc.dispatchInput(down);
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    doc.dispatchInput(up);
}

/// `#inc.Click` and `#gone.Click` both drive one counter, whose value is written into `#count`.
FlowGraph buildGraph() {
    const std::string json = R"({
        "version": 1,
        "nodes": [
            { "id": "cnt", "type": "counter", "params": { "step": 1, "initial": 0 } },
            { "id": "ts", "type": "toString", "params": { "format": "{}" } }
        ],
        "edges": [
            { "from": "#inc.Click", "to": "cnt.increment" },
            { "from": "#gone.Click", "to": "cnt.increment" },
            { "from": "cnt.value", "to": "ts.in" },
            { "from": "ts.out", "to": "#count.setText" }
        ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    const Status status = FlowGraph::parse(json, graph, errors);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
    REQUIRE(status == Status::kOk);
    return graph;
}

}  // namespace

TEST_CASE("FlowRuntime::rebind: keeps node state, warns about connection points that are gone") {
    NullTextShaper shaper;
    std::unique_ptr<Document> before = loadDoc(
        shaper, R"(<button id="inc">+1</button><button id="gone">x</button><span id="count">0</span>)");
    // The reloaded document, which lost `#gone`. Both Documents are created before the runtime so
    // they outlive it: ~FlowRuntime() takes its listeners back off the Document it is bound to.
    std::unique_ptr<Document> after =
        loadDoc(shaper, R"(<button id="inc">+1</button><span id="count">0</span>)");

    FlowRuntime rt;
    std::vector<std::string> errors;
    REQUIRE(rt.load(buildGraph(), *before, errors) == Status::kOk);

    click(*before, "inc");
    CHECK(before->byId("count").text() == "1");
    CHECK(rt.values()[0].n == doctest::Approx(1));  // cnt.value

    std::vector<std::string> warnings;
    REQUIRE(rt.rebind(*after, warnings) == Status::kOk);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "#gone.Click: element not found");

    // Node state survived the rebind...
    CHECK(rt.values()[0].n == doctest::Approx(1));
    // ...and the still-present connection points work on the new Document.
    click(*after, "inc");
    CHECK(rt.values()[0].n == doctest::Approx(2));
    CHECK(after->byId("count").text() == "2");
}

TEST_CASE("FlowRuntime::rebind: without a successful load it is kInvalidArgument") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, R"(<span id="count">0</span>)");
    FlowRuntime rt;
    std::vector<std::string> warnings;
    CHECK(rt.rebind(*doc, warnings) == Status::kInvalidArgument);
    CHECK(warnings.empty());
}
