#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_ports.h"
#include "xt/text_shaper.h"

/// connectionPoints(): docs/design/17-editor.md §17.6's connection-point table reflected off
/// tests/flow/runtime_doc.xt (button#inc, span#count, input#name, span#echo, button#darkToggle,
/// div#bar, `<anim id="anim">`), docs/tasks/T-65.md's "test_flow_ports".

using namespace xt;
using namespace xt::flow;

namespace {

std::string flowPath(const std::string& name) { return std::string(XT_TEST_FLOW_DIR) + "/" + name; }

std::unique_ptr<Document> loadRuntimeDoc(TextShaper& shaper) {
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocument(flowPath("runtime_doc.xt"), options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 400});
    doc->update(0.0f);
    return doc;
}

bool has(const std::vector<PortDesc>& points, const std::string& owner, const std::string& port,
         PortType type, bool isInput) {
    for (const PortDesc& p : points) {
        if (p.owner == owner && p.port == port && p.type == type && p.isInput == isInput) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("connectionPoints: runtime_doc.xt's ids, <anim> and document ports") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);

    std::vector<PortDesc> points = connectionPoints(*doc);

    // #inc: a plain id'd element's row-1 output port.
    CHECK(has(points, "#inc", "Click", PortType::Pulse, false));
    // #count: a plain id'd element's row-1 input port.
    CHECK(has(points, "#count", "setText", PortType::String, true));
    // #root: row-1 input toggleClass, and row-1 output Change is NOT one of #root's ports
    // (Change is only listed for elements that actually fire it; row-1 lists every port for
    // every id'd element regardless of tag, per docs/design/17-editor.md §17.6 -- see the
    // connectionPoints() doc comment/report for this conservative choice).
    CHECK(has(points, "#root", "toggleClass", PortType::Pulse, true));
    // #anim: row-1 ports (any id'd element) plus row-2 <anim>-only ports.
    CHECK(has(points, "#anim", "Click", PortType::Pulse, false));
    CHECK(has(points, "#anim", "Marker", PortType::String, false));
    CHECK(has(points, "#anim", "localTime", PortType::Number, false));
    CHECK(has(points, "#anim", "Finished", PortType::Pulse, false));
    CHECK(has(points, "#anim", "play", PortType::Pulse, true));
    CHECK(has(points, "#anim", "seek", PortType::Number, true));
    // A plain (non-<anim>) element does NOT get row-2 ports.
    CHECK_FALSE(has(points, "#inc", "play", PortType::Pulse, true));
    CHECK_FALSE(has(points, "#inc", "Marker", PortType::String, false));
    // T-123's two new row-1 input ports.
    CHECK(has(points, "#spin", "playAnimation", PortType::String, true));
    CHECK(has(points, "#spin", "stopAnimation", PortType::Pulse, true));
    // document: row-3 output ports.
    CHECK(has(points, "document", "clock", PortType::Number, false));
    CHECK(has(points, "document", "viewportWidth", PortType::Number, false));
    CHECK(has(points, "document", "viewportHeight", PortType::Number, false));
    // document: the T-77 pointer ports (row 3).
    CHECK(has(points, "document", "pointerX", PortType::Number, false));
    CHECK(has(points, "document", "pointerY", PortType::Number, false));
    CHECK(has(points, "document", "pointerNX", PortType::Number, false));
    CHECK(has(points, "document", "pointerNY", PortType::Number, false));
    CHECK(has(points, "document", "pointerDown", PortType::Bool, false));
    // The document has no input ports.
    for (const PortDesc& p : points) {
        if (p.owner == "document") {
            CHECK_FALSE(p.isInput);
        }
    }
}
