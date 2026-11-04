#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/bundle_context.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// T-601 (Docs/design/53-flow-language.md §53.102): the UI flow's `lifecycle` node (`shown` /
/// `hidden`, delivered before the first update of the frame the UI is shown in) and the static lint
/// `flow.introByTimer`.

using namespace xt;
using namespace xt::flow;

namespace {

/// `#Title` ends opaque; the intro fades it in from 0 over 0.5 s.
constexpr const char* kIntroUi = R"X(<xt><head><style>
#Title { width: 100px; height: 20px; opacity: 1; }
@timeline intro { duration: 0.5s; #Title { 0s { opacity: 0 } 0.5s { opacity: 1 } } }
</style></head><body id="Root"><span id="Title">t</span></body></xt>)X";

/// Counts `shown` and `hidden` of one lifecycle node and plays the intro on `shown`.
constexpr const char* kCountingFlow = R"({"version":1,"nodes":[
    {"id":"life","type":"lifecycle"},
    {"id":"shown","type":"counter"},{"id":"hidden","type":"counter"},
    {"id":"intro","type":"timeline","params":{"name":"intro"}}],
  "edges":[
    {"from":"life.shown","to":"shown.increment"},
    {"from":"life.hidden","to":"hidden.increment"},
    {"from":"life.shown","to":"intro.play"}]})";

std::string joinedLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line + "\n";
    }
    return out;
}

DocumentHandle loadIntroUi(Context& ctx) {
    const compile::SourceFile file{"intro.xt", kIntroUi};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "intro.xt", options);
    REQUIRE(result.ok());
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    return handle.value();
}

void loadFlow(FlowRuntime& runtime, Document& doc, const char* json) {
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(FlowGraph::parse(json, graph, errors) == Status::kOk, joinedLines(errors));
    REQUIRE_MESSAGE(runtime.load(graph, doc, errors) == Status::kOk, joinedLines(errors));
}

double count(const FlowRuntime& runtime, const std::string& name) {
    const std::vector<std::string>& names = runtime.valueNames();
    std::span<const PortValue> values = runtime.values();
    for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
        if (names[i] == name) {
            return values[i].n;
        }
    }
    return -1;
}

float titleOpacity(Document& doc) { return doc.byId("Title").property(prop::Opacity).f; }

std::vector<FlowLintFinding> lint(const char* json) {
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(FlowGraph::parse(json, graph, errors) == Status::kOk, joinedLines(errors));
    std::vector<FlowLintFinding> findings;
    lintFlowGraph(graph, findings);
    return findings;
}

}  // namespace

TEST_CASE("lifecycle: the node type, its ports and the .xtflow round-trip") {
    std::span<const NodeTypeDesc> table = nodeTypeTable();
    REQUIRE(table.size() == 24);
    CHECK(table[23].type == "lifecycle");
    CHECK(table[23].params.empty());
    CHECK(table[23].inputs.empty());
    REQUIRE(table[23].outputs.size() == 2);
    CHECK(table[23].outputs[0].name == "shown");
    CHECK(table[23].outputs[0].type == PortType::Pulse);
    CHECK(table[23].outputs[1].name == "hidden");
    CHECK(table[23].outputs[1].type == PortType::Pulse);

    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(kCountingFlow, graph, errors) == Status::kOk);
    FlowGraph again;
    REQUIRE(FlowGraph::parse(graph.toJson(), again, errors) == Status::kOk);
    CHECK(again.toJson() == graph.toJson());
    CHECK(graph.toJson().find("\"version\": " + std::to_string(kFlowFormatVersion)) != std::string::npos);
    // No inputs: an edge into it is an error.
    CHECK(FlowGraph::parse(R"({"nodes":[{"id":"l","type":"lifecycle"},{"id":"t","type":"timer",
        "params":{"interval":1}}],"edges":[{"from":"t.tick","to":"l.shown"}]})",
                           graph, errors) != Status::kOk);
}

TEST_CASE("lifecycle: shown fires before the first update of the frame the UI is shown in") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    const DocumentHandle h = loadIntroUi(ctx);
    Document& doc = *ctx.document(h);
    FlowRuntime runtime;
    loadFlow(runtime, doc, kCountingFlow);
    CHECK(count(runtime, "shown.value") == 0);  // Loading the graph shows nothing.

    ctx.update(0.016f);  // Frame 0.
    CHECK(count(runtime, "shown.value") == 1);
    CHECK(doc.isTimelinePlaying("", "intro"));
    CHECK(titleOpacity(doc) == doctest::Approx(0.0f));  // The intro's frame 0, not the final state.
    for (int i = 0; i < 5; ++i) {
        ctx.update(0.016f);
    }
    CHECK(count(runtime, "shown.value") == 1);  // Once, not per frame.
    CHECK(titleOpacity(doc) > 0.0f);

    // What an autostart timer does instead: frame 0 is the final state (the flash 53 §53.102 cures).
    Context timerCtx{ContextConfig{&shaper, nullptr}};
    const DocumentHandle t = loadIntroUi(timerCtx);
    FlowRuntime timerRuntime;
    loadFlow(timerRuntime, *timerCtx.document(t), R"({"nodes":[
        {"id":"start","type":"timer","params":{"interval":0.05,"autostart":true,"repeat":false}},
        {"id":"intro","type":"timeline","params":{"name":"intro"}}],
      "edges":[{"from":"start.tick","to":"intro.play"}]})");
    timerCtx.update(0.016f);
    timerRuntime.tick(0.016f);
    CHECK(titleOpacity(*timerCtx.document(t)) == doctest::Approx(1.0f));
}

TEST_CASE("lifecycle: hidden on OnHidden, shown again on requestShow") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    const DocumentHandle h = loadIntroUi(ctx);
    FlowRuntime runtime;
    loadFlow(runtime, *ctx.document(h), kCountingFlow);
    ctx.update(0.016f);
    REQUIRE(count(runtime, "shown.value") == 1);

    REQUIRE(ctx.requestHide(h) == Status::kOk);  // No outro: hidden at once (OnHidden).
    CHECK_FALSE(ctx.isVisible(h));
    CHECK(count(runtime, "hidden.value") == 1);
    REQUIRE(ctx.requestShow(h) == Status::kOk);
    CHECK(count(runtime, "shown.value") == 2);
    REQUIRE(ctx.requestShow(h) == Status::kOk);  // Already visible: no OnShown, no shown.
    CHECK(count(runtime, "shown.value") == 2);
    CHECK(count(runtime, "hidden.value") == 1);
}

TEST_CASE("lifecycle: setVisible is silent; setVisible(false) then requestShow fires shown") {
    NullTextShaper shaper;
    SUBCASE("parked before the first update, shown by requestShow") {
        Context ctx{ContextConfig{&shaper, nullptr}};
        const DocumentHandle h = loadIntroUi(ctx);
        Document& doc = *ctx.document(h);
        FlowRuntime runtime;
        loadFlow(runtime, doc, kCountingFlow);
        REQUIRE(ctx.setVisible(h, false) == Status::kOk);
        ctx.update(0.016f);
        ctx.update(0.016f);
        CHECK(count(runtime, "shown.value") == 0);  // Hidden at its first update: no first show.
        REQUIRE(ctx.requestShow(h) == Status::kOk);
        CHECK(count(runtime, "shown.value") == 1);
        // Started at the show (the Document's clock then, as an `on:show` play() is): the first
        // frame drawn is the intro's start, one frame in, not the final state.
        ctx.update(0.016f);
        CHECK(titleOpacity(doc) < 0.1f);
        CHECK(count(runtime, "shown.value") == 1);
    }
    SUBCASE("setVisible(true) fires nothing") {
        Context ctx{ContextConfig{&shaper, nullptr}};
        const DocumentHandle h = loadIntroUi(ctx);
        FlowRuntime runtime;
        loadFlow(runtime, *ctx.document(h), kCountingFlow);
        ctx.update(0.016f);
        REQUIRE(count(runtime, "shown.value") == 1);
        REQUIRE(ctx.setVisible(h, false) == Status::kOk);
        CHECK(count(runtime, "hidden.value") == 0);
        REQUIRE(ctx.setVisible(h, true) == Status::kOk);
        ctx.update(0.016f);
        CHECK(count(runtime, "shown.value") == 1);
        CHECK(count(runtime, "hidden.value") == 0);
    }
    SUBCASE("setVisible(true) before the first update counts as visible") {
        Context ctx{ContextConfig{&shaper, nullptr}};
        const DocumentHandle h = loadIntroUi(ctx);
        FlowRuntime runtime;
        loadFlow(runtime, *ctx.document(h), kCountingFlow);
        REQUIRE(ctx.setVisible(h, true) == Status::kOk);
        CHECK(count(runtime, "shown.value") == 0);
        ctx.update(0.016f);
        CHECK(count(runtime, "shown.value") == 1);
    }
}

TEST_CASE("lifecycle: every lifecycle node fires; each instance fires its own; a disabled flow does not") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    const char* twoNodes = R"({"nodes":[{"id":"a","type":"lifecycle"},{"id":"b","type":"lifecycle"},
        {"id":"ca","type":"counter"},{"id":"cb","type":"counter"}],
      "edges":[{"from":"a.shown","to":"ca.increment"},{"from":"b.shown","to":"cb.increment"}]})";
    const DocumentHandle first = loadIntroUi(ctx);
    const DocumentHandle second = loadIntroUi(ctx);
    const DocumentHandle third = loadIntroUi(ctx);
    BundleContext bundle(ctx);
    REQUIRE(bundle.addUi("intro", first) == Status::kOk);
    REQUIRE(bundle.addUi("intro", second) == Status::kOk);
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(bundle.loadUiFlow("intro", twoNodes, errors) == Status::kOk, joinedLines(errors));
    FlowRuntime disabled;
    loadFlow(disabled, *ctx.document(third), twoNodes);
    disabled.setEnabled(false);
    ctx.update(0.016f);
    for (DocumentHandle h : {first, second}) {
        FlowRuntime* runtime = bundle.uiFlowRuntime(h);
        REQUIRE(runtime != nullptr);
        CHECK(count(*runtime, "ca.value") == 1);
        CHECK(count(*runtime, "cb.value") == 1);
    }
    CHECK(count(disabled, "ca.value") == 0);
    // An unloaded runtime is off the Document: a later show reaches nothing.
    disabled.setEnabled(true);
    disabled.unload();
    REQUIRE(ctx.requestHide(third) == Status::kOk);
    REQUIRE(ctx.requestShow(third) == Status::kOk);
    CHECK(disabled.values().empty());
}

TEST_CASE("lifecycle: the global flow rejects it (ui#<id>.shown is its form there)") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    const DocumentHandle h = loadIntroUi(ctx);
    BundleContext bundle(ctx);
    REQUIRE(bundle.addUi("intro", h) == Status::kOk);
    std::vector<std::string> errors;
    CHECK(bundle.loadGlobalFlow(R"({"nodes":[{"id":"l","type":"lifecycle"}],"edges":[]})", errors) !=
          Status::kOk);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("node 'l': lifecycle is only valid in a UI's own flow") == 0);
}

TEST_CASE("lint flow.introByTimer: autostart timer into timeline.play warns; lifecycle.shown does not") {
    std::vector<FlowLintFinding> findings = lint(R"({"nodes":[
        {"id":"start","type":"timer","params":{"interval":0.05,"autostart":true,"repeat":false}},
        {"id":"intro","type":"timeline","params":{"name":"intro"}},
        {"id":"rain","type":"timeline","params":{"name":"rain","loop":true}}],
      "edges":[{"from":"start.tick","to":"intro.play"},{"from":"start.tick","to":"rain.play"}]})");
    REQUIRE(findings.size() == 1);  // One per timer.
    CHECK(findings[0].code == "flow.introByTimer");
    CHECK(findings[0].nodeId == "start");
    CHECK(findings[0].targets == std::vector<std::string>{"intro.play", "rain.play"});
    CHECK(findings[0].timeline == "intro");
    const std::string message =
        "intro starts from an autostart timer; the first frames show the final state "
        "(use lifecycle.shown or on:show)";
    CHECK(findings[0].message == message);
    CHECK(findings[0].fix ==
          "drive intro.play from lifecycle.shown instead of start.tick (or on:show=\"play(intro)\" on the "
          "board)");

    CHECK(lint(kCountingFlow).empty());
    // Through pulse-only relays (merge, a pulse throttle), to seek and to a state node.
    findings = lint(R"({"nodes":[
        {"id":"t","type":"timer","params":{"interval":0.1,"autostart":1,"repeat":false}},
        {"id":"m","type":"merge"},{"id":"th","type":"throttle","params":{"unit":"frames","rate":1}},
        {"id":"st","type":"state","params":{"states":["A","B"],"transitions":[{"from":"A","on":"go","to":"B"}]}}],
      "edges":[{"from":"t.tick","to":"m.in"},{"from":"m.out","to":"th.in"},{"from":"th.passed","to":"st.go"}]})");
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].targets == std::vector<std::string>{"st.go"});
    CHECK(findings[0].timeline.empty());
    CHECK(findings[0].fix == "drive st.go from lifecycle.shown instead of t.tick");
    // A repeating timer, one not started by itself, or a value node in between: no warning.
    CHECK(lint(R"({"nodes":[{"id":"t","type":"timer","params":{"interval":1,"autostart":true}},
        {"id":"i","type":"timeline","params":{"name":"intro"}}],"edges":[{"from":"t.tick","to":"i.play"}]})")
              .empty());
    CHECK(lint(R"({"nodes":[{"id":"t","type":"timer","params":{"interval":1,"repeat":false}},
        {"id":"i","type":"timeline","params":{"name":"intro"}}],"edges":[{"from":"t.tick","to":"i.play"}]})")
              .empty());
    CHECK(lint(R"({"nodes":[{"id":"t","type":"timer","params":{"interval":1,"autostart":true,"repeat":false}},
        {"id":"c","type":"counter"},{"id":"i","type":"timeline","params":{"name":"intro"}}],
      "edges":[{"from":"t.tick","to":"c.increment"},{"from":"c.value","to":"i.seek"}]})")
              .empty());
}
