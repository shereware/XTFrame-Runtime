#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/host_ports.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// HostPorts (Docs/tasks/T-144.md): a host drives and observes a Document's §17.6 connection
/// points directly, through the same document endpoint code FlowRuntime uses.

using namespace xt;
using namespace xt::flow;

namespace {

const std::string kFixturePath = std::string(XT_TEST_FLOW_DIR) + "/../xt/host_ports.xt";

std::string readFixture() {
    std::ifstream file(kFixturePath, std::ios::binary);
    REQUIRE(file.good());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::vector<uint8_t> compileText(const std::string& text) {
    const compile::SourceFile file{"host_ports.xt", text};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "host_ports.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

/// A Context with the fixture loaded, laid out in a 400x400 viewport.
struct Fixture {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    DocumentHandle handle = 0;

    Fixture() {
        std::vector<uint8_t> ir = compileText(readFixture());
        Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(loaded.ok());
        handle = loaded.value();
        ctx.setViewport(Rect{0, 0, 400, 400}, 1.0f);
        ctx.update(0.0f);
    }

    Document& doc() { return *ctx.document(handle); }

    Vec2 center(const char* id) {
        ElementRef ref = doc().byId(id);
        REQUIRE(ref.valid());
        const Rect box = ref.boundingBox();
        return Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    }

    void pointer(InputEvent::Kind kind, Vec2 pos) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = pos;
        ctx.pushInput(ev);
    }
};

PortValue stringValue(const char* s) {
    PortValue v;
    v.type = PortType::String;
    v.s = s;
    return v;
}

PortValue numberValue(double n) {
    PortValue v;
    v.type = PortType::Number;
    v.n = n;
    return v;
}

}  // namespace

TEST_CASE("HostPorts: setInput drives element input ports, rejects unknown ports and wrong types") {
    Fixture f;
    HostPorts ports;
    REQUIRE(ports.bind(f.doc()) == Status::kOk);
    CHECK(!ports.ports().empty());

    CHECK(ports.setInput("#title.setText", stringValue("hello")) == Status::kOk);
    CHECK(f.doc().byId("title").text() == "hello");

    CHECK(ports.setInput("#title.addClass", PortValue{}, "hot") == Status::kOk);
    CHECK(f.doc().byId("title").hasClass("hot"));

    CHECK(ports.setInput("#title.bogus", PortValue{}) == Status::kInvalidArgument);
    CHECK(ports.setInput("#nobody.setText", stringValue("x")) == Status::kInvalidArgument);
    CHECK(ports.setInput("document.clock", numberValue(1)) == Status::kInvalidArgument);

    // No conversion: setText takes a string, exactly as a `.xtflow` edge.
    CHECK(ports.setInput("#title.setText", numberValue(3)) == Status::kInvalidArgument);
    CHECK(ports.lastError().find("#title.setText") != std::string::npos);
    CHECK(ports.lastError().find("string") != std::string::npos);
    CHECK(f.doc().byId("title").text() == "hello");

    // addClass needs its `class` parameter.
    CHECK(ports.setInput("#title.addClass", PortValue{}) == Status::kInvalidArgument);
}

TEST_CASE("HostPorts: a pulse output fires once per event") {
    Fixture f;
    HostPorts ports;
    REQUIRE(ports.bind(f.doc()) == Status::kOk);
    int clicks = 0;
    Result<uint32_t> id = ports.addOutputListener("#btn.Click", [&](const PortValue& v) {
        CHECK(v.type == PortType::Pulse);
        ++clicks;
    });
    REQUIRE(id.ok());
    CHECK(id.value() != 0);
    CHECK(!ports.addOutputListener("#btn.setText", [](const PortValue&) {}).ok());  // An input.

    const Vec2 pos = f.center("btn");
    f.pointer(InputEvent::PointerDown, pos);
    f.pointer(InputEvent::PointerUp, pos);
    f.ctx.update(0.016f);
    CHECK(clicks == 1);

    ports.removeOutputListener(id.value());
    f.pointer(InputEvent::PointerDown, pos);
    f.pointer(InputEvent::PointerUp, pos);
    f.ctx.update(0.016f);
    CHECK(clicks == 1);
}

TEST_CASE("HostPorts: a value output only fires on change") {
    Fixture f;
    HostPorts ports;
    REQUIRE(ports.bind(f.doc()) == Status::kOk);
    std::vector<bool> seen;
    REQUIRE(ports
                .addOutputListener("#btn.hover",
                                   [&](const PortValue& v) {
                                       CHECK(v.type == PortType::Bool);
                                       seen.push_back(v.b);
                                   })
                .ok());

    f.pointer(InputEvent::PointerMove, f.center("btn"));
    f.ctx.update(0.016f);
    ports.tick(0.016f);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == true);

    f.pointer(InputEvent::PointerMove, Vec2{390, 390});
    f.ctx.update(0.016f);
    ports.tick(0.016f);
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == false);

    f.pointer(InputEvent::PointerMove, Vec2{391, 391});
    f.ctx.update(0.016f);
    ports.tick(0.016f);
    CHECK(seen.size() == 2);
}

TEST_CASE("HostPorts: document.clock advances with tick()") {
    Fixture f;
    HostPorts ports;
    REQUIRE(ports.bind(f.doc()) == Status::kOk);
    std::vector<double> clock;
    REQUIRE(
        ports.addOutputListener("document.clock", [&](const PortValue& v) { clock.push_back(v.n); }).ok());
    ports.tick(0.5f);
    ports.tick(0.5f);
    REQUIRE(clock.size() == 2);
    CHECK(clock[0] == doctest::Approx(0.5));
    CHECK(clock[1] == doctest::Approx(1.0));
    CHECK(clock[1] > clock[0]);
}

TEST_CASE("HostPorts: rebind keeps listeners whose ports survive a reload, warns about the rest") {
    Fixture f;
    HostPorts ports;
    REQUIRE(ports.bind(f.doc()) == Status::kOk);
    int clicks = 0;
    int goneClicks = 0;
    REQUIRE(ports.addOutputListener("#btn.Click", [&](const PortValue&) { ++clicks; }).ok());
    REQUIRE(ports.addOutputListener("#gone.Click", [&](const PortValue&) { ++goneClicks; }).ok());

    std::string text = readFixture();
    const std::string goneElement = "<div id=\"gone\"></div>";
    const size_t at = text.find(goneElement);
    REQUIRE(at != std::string::npos);
    text.erase(at, goneElement.size());
    std::vector<uint8_t> ir = compileText(text);
    REQUIRE(f.ctx.reloadDocument(f.handle, IrBlob{ir.data(), ir.size()}, true).ok());
    f.ctx.update(0.0f);

    std::vector<std::string> warnings;
    REQUIRE(ports.rebind(f.doc(), warnings) == Status::kOk);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "#gone.Click: element not found");

    const Vec2 pos = f.center("btn");
    f.pointer(InputEvent::PointerDown, pos);
    f.pointer(InputEvent::PointerUp, pos);
    f.ctx.update(0.016f);
    CHECK(clicks == 1);
    CHECK(goneClicks == 0);

    // Ports of the new document work too.
    CHECK(ports.setInput("#title.setText", stringValue("after")) == Status::kOk);
    CHECK(f.doc().byId("title").text() == "after");
}

TEST_CASE("HostPorts: calls before bind() are rejected") {
    HostPorts ports;
    CHECK(ports.ports().empty());
    CHECK(ports.setInput("#title.setText", stringValue("x")) == Status::kInvalidArgument);
    CHECK(!ports.addOutputListener("#btn.Click", [](const PortValue&) {}).ok());
    ports.tick(0.1f);  // No-op.
}
