#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// T-873 (Docs/design/60-input-channels.md §60.3.5): the flow node `input` (param `channel`) and its
/// outputs `value`, `pressed`, `released` (pulses) and `delta`, read once per tick from the channel
/// state of the frame the Document committed.

using namespace xt;
using namespace xt::flow;

namespace {

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line + "\n";
    }
    return out;
}

double valueOf(const FlowRuntime& runtime, const std::string& name) {
    const std::vector<std::string>& names = runtime.valueNames();
    std::span<const PortValue> values = runtime.values();
    for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
        if (names[i] == name) {
            return values[i].n;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("T-873: the input node type, its ports and its channel check") {
    std::span<const NodeTypeDesc> table = nodeTypeTable();
    REQUIRE(table.size() == 25);
    const NodeTypeDesc& input = table[24];
    CHECK(input.type == "input");
    REQUIRE(input.params.size() == 1);
    CHECK(input.params[0] == "channel");
    CHECK(input.inputs.empty());
    REQUIRE(input.outputs.size() == 4);
    CHECK(input.outputs[0].name == "value");
    CHECK(input.outputs[0].type == PortType::Number);
    CHECK(input.outputs[1].name == "pressed");
    CHECK(input.outputs[1].type == PortType::Pulse);
    CHECK(input.outputs[2].name == "released");
    CHECK(input.outputs[3].name == "delta");
    CHECK(input.outputs[3].type == PortType::Number);

    FlowGraph graph;
    std::vector<std::string> errors;
    CHECK(FlowGraph::parse(R"({"nodes":[{"id":"i","type":"input"}],"edges":[]})", graph, errors) !=
          Status::kOk);
    errors.clear();
    CHECK(
        FlowGraph::parse(R"({"nodes":[{"id":"i","type":"input","params":{"channel":"pad.zz"}}],"edges":[]})",
                         graph, errors) != Status::kOk);
    CHECK(joined(errors).find("pad.zz") != std::string::npos);
}

TEST_CASE("T-873: input node outputs value / delta on a move and pulses on the edges") {
    const compile::SourceFile file{"ui.xt", "<xt><body id=\"Root\"></body></xt>"};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "ui.xt", compile::CompileOptions{});
    REQUIRE(result.ok());
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    const DocumentHandle h = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false).value();
    ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
    Document& doc = *ctx.document(h);
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(FlowGraph::parse(R"({"version":1,"nodes":[
        {"id":"stick","type":"input","params":{"channel":"pad.rx"}},
        {"id":"btn","type":"input","params":{"channel":"player(1).pad.a"}},
        {"id":"v","type":"math","params":{"op":"add"}},
        {"id":"d","type":"math","params":{"op":"add"}},
        {"id":"down","type":"counter"},{"id":"up","type":"counter"}],
      "edges":[
        {"from":"stick.value","to":"v.a"},
        {"from":"stick.delta","to":"d.a"},
        {"from":"btn.pressed","to":"down.increment"},
        {"from":"btn.released","to":"up.increment"}]})",
                                     graph, errors) == Status::kOk,
                    joined(errors));
    FlowRuntime runtime;
    REQUIRE_MESSAGE(runtime.load(graph, doc, errors) == Status::kOk, joined(errors));
    ctx.update(0.016f);
    runtime.tick(0.016f);

    InputEvent axis;
    axis.kind = InputEvent::GamepadAxisMotion;
    axis.gamepadAxis = GamepadAxis::RightX;
    axis.axisValue = 0.5f;
    ctx.pushInput(axis);
    ctx.update(0.016f);
    runtime.tick(0.016f);
    CHECK(valueOf(runtime, "v.out") == doctest::Approx(0.5));
    CHECK(valueOf(runtime, "d.out") == doctest::Approx(0.5));
    axis.axisValue = 0.75f;
    ctx.pushInput(axis);
    ctx.update(0.016f);
    runtime.tick(0.016f);
    CHECK(valueOf(runtime, "v.out") == doctest::Approx(0.75));
    CHECK(valueOf(runtime, "d.out") == doctest::Approx(0.25));

    InputEvent press;
    press.kind = InputEvent::GamepadButtonDown;
    press.gamepadButton = GamepadButton::A;
    press.player = 1;
    ctx.pushInput(press);
    ctx.update(0.016f);
    runtime.tick(0.016f);
    CHECK(valueOf(runtime, "down.value") == 1);
    CHECK(valueOf(runtime, "up.value") == 0);
    press.kind = InputEvent::GamepadButtonUp;
    ctx.pushInput(press);
    ctx.update(0.016f);
    runtime.tick(0.016f);
    CHECK(valueOf(runtime, "up.value") == 1);
    // Player 0's A is another channel state.
    press.kind = InputEvent::GamepadButtonDown;
    press.player = 0;
    ctx.pushInput(press);
    ctx.update(0.016f);
    runtime.tick(0.016f);
    CHECK(valueOf(runtime, "down.value") == 1);
}
