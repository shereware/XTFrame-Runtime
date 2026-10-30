#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/perf.h"
#include "xt/text_shaper.h"

/// T-475 (Docs/design/21-perf-tracing.md §21.6, 27-interaction.md §27.4.2 item 6): the flow stage
/// times and counters FlowRuntime writes into the PerfTracker setPerfTracker() gave it, checked on
/// one frame of the Tests/perf/flow_big large fixture (500 nodes, 100 sinks) at behavior 1.

using namespace xt;
using namespace xt::flow;

namespace {

const std::string kFixtureDir = std::string(XT_TEST_FLOW_DIR) + "/../perf/flow_big/large";

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// The large fixture's Document and its flow, loaded and settled.
struct BigFlow {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc;
    FlowRuntime flow;
    std::vector<PortId> ports;

    void load() {
        compile::CompileOptions options;
        options.rootDir = kFixtureDir;
        options.behavior = 1;  // Coalesced value signals (43 §43.3), so flowSignalsCoalesced moves.
        compile::CompileResult compiled =
            compile::compileDocument(kFixtureDir + "/ui/flow-big/flow-big.xt", options);
        REQUIRE(compiled.ok());
        REQUIRE(Document::load(IrBlob{compiled.ir.data(), compiled.ir.size()}, true, &shaper, nullptr, doc) ==
                Status::kOk);
        doc->setViewport(Rect{0, 0, 1280, 720});
        doc->update(0.0f);
        FlowGraph graph;
        std::vector<std::string> errors;
        REQUIRE(FlowGraph::parse(readText(kFixtureDir + "/ui/flow-big/flow-big.xtflow"), graph, errors) ==
                Status::kOk);
        REQUIRE(flow.load(graph, *doc, errors) == Status::kOk);
        for (const PortRecord& port : doc->ports()) {
            if (ports.size() < 20 && port.documentLevel && port.type == PortValueType::Number) {
                ports.push_back(port.id);
            }
        }
    }

    /// One frame of xtframe-view's --synth-pointer 16 --synth-ports 20 traffic, then update + tick.
    void frame(uint32_t index) {
        for (uint32_t i = 0; i < 16; ++i) {
            InputEvent ev;
            ev.kind = InputEvent::PointerMove;
            const float t = static_cast<float>(index * 16 + i) / 600.0f;
            ev.pos = Vec2{1280.0f * t, 720.0f * t};
            doc->dispatchInput(ev);
        }
        for (size_t p = 0; p < ports.size(); ++p) {
            (void)doc->setPort(ports[p], Value::ofFloat(static_cast<float>(index * 3 + p * 7)));
        }
        doc->update(1.0f / 60.0f);
        flow.tick(1.0f / 60.0f);
    }
};

}  // namespace

TEST_CASE("T-475: one frame of the large fixture fills every flow counter") {
    BigFlow big;
    big.load();
    big.frame(1);  // Settles whatever load() left pending.

    PerfTracker tracker;
    tracker.setEnabled(true);
    big.flow.setPerfTracker(&tracker);
    tracker.beginFrame(0.0f);
    big.frame(2);
    const FrameStats& f = tracker.current();
    CHECK(f.flowSignals > 0u);
    CHECK(f.flowSignalsCoalesced > 0u);
    CHECK(f.flowNodeEvals > 0u);
    CHECK(f.flowSinkWrites > 0u);
    CHECK(f.flowFanOutEdges > 0u);
    CHECK(f.flowEvalMs > 0.0f);
    CHECK(f.flowDeliverMs > 0.0f);
    CHECK(f.flowSinkMs > 0.0f);
    CHECK(f.flowPollMs >= 0.0f);
    // Coalescing never drops more signals than came in.
    CHECK(f.flowSignalsCoalesced <= f.flowSignals);
    // xt_flow_tests does not replace operator new (only xt_perf_flow does): nothing to count.
    CHECK(f.flowAllocs == 0u);

    // A disabled tracker, or none, records nothing.
    tracker.setEnabled(false);
    big.frame(3);
    big.flow.setPerfTracker(nullptr);
    tracker.setEnabled(true);
    tracker.beginFrame(1.0f);
    big.frame(4);
    const FrameStats& g = tracker.current();
    CHECK(g.flowSignals == 0u);
    CHECK(g.flowNodeEvals == 0u);
    CHECK(g.flowSinkWrites == 0u);
    CHECK(g.flowFanOutEdges == 0u);
    CHECK(g.flowEvalMs == 0.0f);
}
