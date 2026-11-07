#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-200 core follow-ups (Docs/tasks/T-200.md): world anchors (13.8.3), component converters,
/// system-event ports and Context::hotReload (22.10).

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\">" +
           std::string(body) + "</body></xt>";
}

CompileResult compileSrc(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasError(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A Context with one compiled document loaded (the IR bytes stay owned by `ir`).
struct Host {
    Context ctx{ContextConfig{}};
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;
    float clock = 0;

    Document& doc() { return *ctx.document(handle); }
    void step(float dt = 0.016f) {
        clock += dt;
        ctx.update(dt);
    }
    PortId port(std::string_view name) {
        PortId p = doc().lookupPort(name);
        REQUIRE(p != kInvalidPort);
        return p;
    }
    Vec2 origin(std::string_view id) {
        ElementRef e = doc().byId(id);
        REQUIRE(e.valid());
        return doc().nodeToScreen(e.node, Vec2{0, 0});
    }
};

std::unique_ptr<Host> open(const std::string& xt) {
    auto host = std::make_unique<Host>();
    CompileResult r = compileSrc(xt);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    host->ir = r.ir;
    host->ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    Result<DocumentHandle> h = host->ctx.loadDocument(IrBlob{host->ir.data(), host->ir.size()}, false);
    REQUIRE(h.ok());
    host->handle = h.value();
    host->step(0);
    return host;
}

}  // namespace

// -- 1. World anchors (13.8.3) -------------------------------------------------------------------

TEST_CASE("t200: world-anchor moves the element to the anchor with a translate") {
    auto h =
        open(page("#Tag { position: absolute; left: 10px; top: 20px; width: 30px; height: 10px; }"
                  "#Box { margin-left: 40px; margin-top: 5px; width: 50px; height: 50px; }",
                  "<div id=\"Hud\" port:pos=\"vec2\" port:pos3=\"vec3\"></div>"
                  "<div id=\"Box\"><div id=\"Tag\" world-anchor=\"pos\"></div></div>"
                  "<div id=\"Tag3\" style=\"margin-left: 4px\" world-anchor=\"pos3\"></div>"));
    const PortId pos = h->port("pos");
    REQUIRE(h->ctx.setWorldAnchor(h->handle, pos, 120, 80) == Status::kOk);
    h->step();
    Vec2 o = h->origin("Tag");
    CHECK(o.x == doctest::Approx(120));
    CHECK(o.y == doctest::Approx(80));
    // The layout of the siblings is untouched.
    CHECK(h->doc().byId("Box").layoutBox().width == doctest::Approx(50));

    REQUIRE(h->doc().setWorldAnchor(pos, 7, 9) == Status::kOk);
    h->step();
    o = h->origin("Tag");
    CHECK(o.x == doctest::Approx(7));
    CHECK(o.y == doctest::Approx(9));

    // vec3: (x, y, depth); `left`/`top` semantics put the margin box on the point.
    const PortId pos3 = h->port("pos3");
    REQUIRE(h->doc().setWorldAnchor(pos3, 50, 60, 12.5f) == Status::kOk);
    h->step();
    Value v = h->doc().getPort(pos3);
    REQUIRE(v.type == ValueType::Vec3);
    CHECK(v.v3.z == doctest::Approx(12.5f));
    o = h->origin("Tag3");
    CHECK(o.x == doctest::Approx(54));
    CHECK(o.y == doctest::Approx(60));

    // Setting the port directly drives the anchor too.
    REQUIRE(h->doc().setPort(pos, Value::ofVec2(Vec2{30, 40})) == Status::kOk);
    h->step();
    o = h->origin("Tag");
    CHECK(o.x == doctest::Approx(30));
    CHECK(o.y == doctest::Approx(40));
}

TEST_CASE("t200: setWorldAnchor toggles Offscreen and rejects bad ports") {
    auto h = open(page("#Tag:state(Offscreen) { opacity: 0.25; }",
                       "<div id=\"Hud\" port:pos=\"vec2\" port:hp=\"number\"></div>"
                       "<div id=\"Tag\" world-anchor=\"pos\"></div>"));
    const PortId pos = h->port("pos");
    ElementRef tag = h->doc().byId("Tag");
    REQUIRE(h->doc().setWorldAnchor(pos, -50, 10, 0, /*onScreen=*/false) == Status::kOk);
    CHECK(h->doc().hasCustomState(tag, "Offscreen"));
    REQUIRE(h->doc().setWorldAnchor(pos, 50, 10, 0, true) == Status::kOk);
    CHECK_FALSE(h->doc().hasCustomState(tag, "Offscreen"));

    CHECK(h->doc().setWorldAnchor(h->port("hp"), 1, 2) == Status::kInvalidArgument);
    CHECK(h->doc().setWorldAnchor(kInvalidPort, 1, 2) == Status::kNotFound);
    CHECK(h->ctx.setWorldAnchor(99, pos, 1, 2) == Status::kInvalidArgument);
}

TEST_CASE("t200: world-anchor needs a vec2/vec3 port") {
    CHECK(hasError(compileSrc(page("", "<div id=\"A\" port:hp=\"number\" world-anchor=\"hp\"></div>")),
                   "must be vec2 or vec3"));
    CHECK(hasError(compileSrc(page("", "<div id=\"A\" world-anchor=\"nope\"></div>")), "unknown port"));
}

// -- 2. Component converters (ops 37-41) ---------------------------------------------------------

TEST_CASE("t200: x/y/z/w and vec2/vec3 converters") {
    auto h = open(page("",
                       "<div id=\"Hud\" port:pos=\"vec2\" port:p3=\"vec3\" port:tint=\"color\" "
                       "port:hp=\"number\" port:mp=\"number\"></div>"
                       "<div id=\"A\" bind:width=\"pos | x\" bind:height=\"pos | y\"></div>"
                       "<span id=\"Z\" bind:text=\"p3 | z\"></span>"
                       "<span id=\"W\" bind:text=\"tint | w\"></span>"
                       "<span id=\"G\" bind:text=\"tint | y\"></span>"
                       "<span id=\"N\" bind:text=\"hp | x\"></span>"
                       "<span id=\"NY\" bind:text=\"hp | y\"></span>"
                       "<div id=\"V\" port:out=\"vec2\" bind:text=\"vec2(hp, mp) | y\"></div>"
                       "<span id=\"V1\" bind:text=\"hp | vec2(7) | y\"></span>"
                       "<span id=\"V3\" bind:text=\"hp | vec3(2, 3) | x\"></span>"
                       "<span id=\"C\" bind:text=\"vec3(1, 2, 5) | z\"></span>"));
    Document& d = h->doc();
    REQUIRE(d.setPortText(h->port("pos"), "[12, 34]") == Status::kOk);
    REQUIRE(d.setPortText(h->port("p3"), "1,2,3") == Status::kOk);
    REQUIRE(d.setPortText(h->port("tint"), "#ff800080") == Status::kOk);
    REQUIRE(d.setPort(h->port("hp"), Value::ofFloat(4)) == Status::kOk);
    REQUIRE(d.setPort(h->port("mp"), Value::ofFloat(9)) == Status::kOk);
    h->step();
    CHECK(d.byId("A").layoutBox().width == doctest::Approx(12));
    CHECK(d.byId("A").layoutBox().height == doctest::Approx(34));
    CHECK(d.byId("Z").text() == "3");
    CHECK(std::string(d.byId("W").text()).rfind("0.50", 0) == 0);
    CHECK(std::string(d.byId("G").text()).rfind("0.50", 0) == 0);
    CHECK(d.byId("N").text() == "4");
    CHECK(d.byId("NY").text() == "0");
    CHECK(d.byId("V").text() == "9");
    CHECK(d.byId("V1").text() == "7");
    CHECK(d.byId("V3").text() == "4");
    CHECK(d.byId("C").text() == "5");
    CHECK(d.setPortText(h->port("pos"), "[1, 2") == Status::kInvalidArgument);
    CHECK(d.setPortText(h->port("pos"), "[1, 2, 3]") == Status::kInvalidArgument);
}

TEST_CASE("t200: component converters are catalog ops 37-41") {
    CompileResult r =
        compileSrc(page("",
                        "<div id=\"A\" port:pos=\"vec2\" bind:left=\"pos | x\" "
                        "bind:top=\"pos | vec2(1) | y\" bind:width=\"vec3(pos, 1, 2) | z\"></div>"));
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    std::vector<uint8_t> ops;
    for (const IrConverterStep& s : view.value().bindings().steps()) {
        ops.push_back(s.op);
    }
    CHECK(ops == std::vector<uint8_t>{37, 41, 38, 41, 39});
    CHECK(hasError(
        compileSrc(page("", "<div id=\"A\" port:pos=\"vec2\" bind:left=\"pos | vec2(1, 2, 3)\"></div>")),
        "vec2() takes"));
    CHECK(hasError(compileSrc(page("", "<div id=\"A\" port:pos=\"vec2\" bind:left=\"pos | x(1)\"></div>")),
                   "x() takes"));
}

// -- 3. System-event ports (27.12, 23.10 item 4) -------------------------------------------------

namespace {

struct SystemListener : ContextListener {
    std::vector<std::string> events;
    void onSystemEvent(std::string_view kind, std::string_view value) override {
        events.push_back(std::string(kind) + "=" + std::string(value));
    }
};

}  // namespace

TEST_CASE("t200: system.* ports are declared implicitly and fed by Context::postSystemEvent") {
    auto h = open(page("",
                       "<div id=\"P\" on:port(system.paused)=\"class(paused)\" "
                       "on:port(system.resumed)=\"class(paused, off)\"></div>"
                       "<span id=\"L\" bind:text=\"system.language\"></span>"
                       "<span id=\"D\" bind:text=\"system.inputDevice | upper\"></span>"));
    Document& d = h->doc();
    CHECK(d.lookupPort("system.paused") != kInvalidPort);
    CHECK(d.lookupPort("system.language") != kInvalidPort);
    CHECK(d.lookupPort("system.loading") == kInvalidPort);  // Not referenced, not declared.
    CHECK(d.portInfo(d.lookupPort("system.paused")).type == PortValueType::Event);
    CHECK(d.portInfo(d.lookupPort("system.language")).type == PortValueType::String);

    SystemListener listener;
    h->ctx.addListener(&listener);
    REQUIRE(h->ctx.postSystemEvent("paused") == Status::kOk);
    CHECK(d.byId("P").hasClass("paused"));
    REQUIRE(h->ctx.postSystemEvent("resumed") == Status::kOk);
    CHECK_FALSE(d.byId("P").hasClass("paused"));
    // T-389: the bindings of a port set by postSystemEvent() are evaluated by the next update().
    REQUIRE(h->ctx.postSystemEvent("languageChanged", "fr") == Status::kOk);
    h->ctx.update(0.0f);
    CHECK(d.byId("L").text() == "fr");
    REQUIRE(h->ctx.postSystemEvent("language", "de") == Status::kOk);  // Alias.
    h->ctx.update(0.0f);
    CHECK(d.byId("L").text() == "de");
    REQUIRE(h->ctx.postSystemEvent("inputDevice", "gamepad") == Status::kOk);
    h->ctx.update(0.0f);
    CHECK(d.byId("D").text() == "GAMEPAD");
    REQUIRE(h->ctx.postSystemEvent("loading") == Status::kOk);  // No port: listeners only.
    CHECK(h->ctx.postSystemEvent("reboot") == Status::kInvalidArgument);
    CHECK(listener.events == std::vector<std::string>{"paused=", "resumed=", "languageChanged=fr",
                                                      "languageChanged=de", "inputDevice=gamepad",
                                                      "loading="});
    h->ctx.removeListener(&listener);
}

TEST_CASE("t240: system.back is an implicit event port fed by postSystemEvent(\"back\")") {
    auto h = open(page("", "<div id=\"B\" on:port(system.back)=\"class(backed)\"></div>"));
    Document& d = h->doc();
    REQUIRE(d.lookupPort("system.back") != kInvalidPort);
    CHECK(d.portInfo(d.lookupPort("system.back")).type == PortValueType::Event);
    SystemListener listener;
    h->ctx.addListener(&listener);
    CHECK_FALSE(d.byId("B").hasClass("backed"));
    REQUIRE(h->ctx.postSystemEvent("back") == Status::kOk);
    CHECK(d.byId("B").hasClass("backed"));
    CHECK(listener.events == std::vector<std::string>{"back="});
    h->ctx.removeListener(&listener);
}

// -- 4. Context::hotReload (13.8.3, 22.10) -------------------------------------------------------

namespace {

struct Strings : ILocalizationProvider {
    std::string text = "Hello";
    std::string_view resolve(std::string_view key, std::string_view) override {
        return key == "greet" ? std::string_view(text) : std::string_view{};
    }
};

struct Glyphs : IGlyphProvider {
    int calls = 0;
    AssetRef resolve(std::string_view, InputKind) override {
        ++calls;
        return AssetRef{};
    }
};

Color bgColor(Document& d, std::string_view id) {
    Value v = d.byId(id).computedStyleValue(prop::BackgroundColor);
    return v.type == ValueType::Color ? v.c : Color{};
}

constexpr const char* kUiV1 = R"X(<xt><head><style>
:root { --c: #ff0000; }
#A { width: 40px; height: 20px; background-color: var(--c); }
#A:state(Warn) { opacity: 0.5; }
@timeline intro { duration: 2s; #A { 0s { opacity: 0 } 2s { opacity: 1 } } }
</style></head><body id="Root">
<div id="Hud" port:hp="number" port:name="string" port:mode="enum(Normal|Danger)" port:pos="vec2"
     port:items="list<struct{name: string, n: number}>" port:hit="event"></div>
<div id="A" state-groups="Level: Warn|Ok"><span id="T" bind:text="name">x</span></div>
<button id="B">b</button><p id="G">@greet</p><div id="C"><glyph key="jump"/></div>
</body></xt>)X";

std::vector<uint8_t> compileOrFail(const std::string& xt) {
    CompileResult r = compileSrc(xt);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    return r.ir;
}

}  // namespace

TEST_CASE("t200: hotReload applies tokens, themes, strings, fonts, glyphs and symbols in place") {
    auto h = open(kUiV1);
    Document* before = &h->doc();
    CHECK(bgColor(*before, "A").r == doctest::Approx(1));

    BundleDelta delta;
    delta.tokens = std::vector<TokenDef>{{"--c", "#00ff00"}};
    REQUIRE(h->ctx.hotReload(delta) == Status::kOk);
    h->step();
    CHECK(&h->doc() == before);  // No Document was rebuilt.
    CHECK(bgColor(h->doc(), "A").g == doctest::Approx(1));
    h->ctx.clearTokens();

    BundleDelta themes;
    themes.themes.push_back(ThemeDef{"cold", {{"--c", "#0000ff"}}});
    themes.theme = "cold";
    REQUIRE(h->ctx.hotReload(themes) == Status::kOk);
    h->step();
    CHECK(h->ctx.theme() == "cold");
    CHECK(bgColor(h->doc(), "A").b == doctest::Approx(1));

    Strings strings;
    BundleDelta s1;
    s1.strings = &strings;
    REQUIRE(h->ctx.hotReload(s1) == Status::kOk);
    h->step();
    CHECK(h->doc().byId("G").text() == "Hello");
    strings.text = "Bonjour";  // The host reloaded its tables: the same provider again.
    REQUIRE(h->ctx.hotReload(s1) == Status::kOk);
    h->step();
    CHECK(h->doc().byId("G").text() == "Bonjour");

    Glyphs glyphs;
    BundleDelta g;
    g.glyphs = &glyphs;
    REQUIRE(h->ctx.hotReload(g) == Status::kOk);
    CHECK(glyphs.calls > 0);

    BundleDelta f;
    f.fonts = true;
    f.symbols = true;
    REQUIRE(h->ctx.hotReload(f) == Status::kOk);
    h->step();
    CHECK(&h->doc() == before);
    CHECK(h->doc().byId("A").layoutBox().width == doctest::Approx(40));

    BundleDelta bad;
    bad.tokens = std::vector<TokenDef>{{"--c", "not a value("}};
    CHECK(h->ctx.hotReload(bad) == Status::kInvalidArgument);
    CHECK(h->ctx.hotReload(BundleDelta{}) == Status::kOk);
}

TEST_CASE("t200: hotReload of a recompiled Document keeps ports, states, focus and playheads") {
    auto h = open(kUiV1);
    Document& d = h->doc();
    REQUIRE(d.setPort(h->port("hp"), Value::ofFloat(42)) == Status::kOk);
    REQUIRE(d.setPortText(h->port("name"), "Ann") == Status::kOk);
    REQUIRE(d.setPortText(h->port("mode"), "Danger") == Status::kOk);
    REQUIRE(d.setPortText(h->port("pos"), "3,4") == Status::kOk);
    REQUIRE(d.setPortText(h->port("items"), R"([{"name":"a","n":1},{"name":"b","n":2}])") == Status::kOk);
    REQUIRE(d.setCustomState(d.byId("A"), "Warn", true) == Status::kOk);
    d.setFocus(d.byId("B"), 0);
    REQUIRE(d.playTimeline("", "intro") == Status::kOk);
    h->step(0.5f);
    const float t0 = d.timelineTime("", "intro");
    CHECK(t0 > 0.3f);

    // v2: another style, a new element and a new port; every id the state uses is still there.
    std::string v2 = kUiV1;
    v2.replace(v2.find("width: 40px"), 11, "width: 60px");
    v2.replace(v2.find("<button id=\"B\">"), 15, "<span id=\"New\">n</span><button id=\"B\">");
    v2.replace(v2.find("port:hit=\"event\""), 16, "port:hit=\"event\" port:extra=\"number\"");
    std::vector<uint8_t> ir2 = compileOrFail(v2);

    BundleDelta delta;
    delta.ir.push_back(IrReload{h->handle, IrBlob{ir2.data(), ir2.size()}, false});
    HotReloadReport report;
    REQUIRE(h->ctx.hotReload(delta, &report) == Status::kOk);
    REQUIRE(report.documents.size() == 1);
    CHECK(report.documents[0].status == Status::kOk);
    CHECK(report.documents[0].missing.empty());

    Document& n = h->doc();
    CHECK(n.getPort(n.lookupPort("hp")).f == doctest::Approx(42));
    CHECK(n.portText(n.lookupPort("name")) == "Ann");
    CHECK(n.portText(n.lookupPort("mode")) == "Danger");
    CHECK(n.getPort(n.lookupPort("pos")).v2.y == doctest::Approx(4));
    Value items = n.getPort(n.lookupPort("items"));
    REQUIRE(items.itemCount == 2);
    CHECK(n.string(items.items[1].items[0].sid) == "b");
    CHECK(items.items[1].items[1].f == doctest::Approx(2));
    CHECK(n.byId("T").text() == "Ann");
    CHECK(n.hasCustomState(n.byId("A"), "Warn"));
    CHECK(n.focus(0).id() == "B");
    CHECK(n.isTimelinePlaying("", "intro"));
    CHECK(n.timelineTime("", "intro") == doctest::Approx(t0));
    h->step(0.5f);
    CHECK(n.timelineTime("", "intro") == doctest::Approx(t0 + 0.5f).epsilon(0.01));
    CHECK(n.byId("A").layoutBox().width == doctest::Approx(60));  // The new style applies.
    CHECK(n.byId("New").valid());
}

TEST_CASE("T-546: the first write of a tween binding after hotReload applies directly") {
    const std::string v1 = page("#Bar { height: 10px; }",
                                R"X(<div id="P" port:hp="number = 30"></div>
<div id="Bar" bind:width="hp | tween(1s)"></div>)X");
    auto h = open(v1);
    h->step(0.016f);
    CHECK(h->doc().byId("Bar").layoutBox().width == doctest::Approx(30));
    REQUIRE(h->doc().setPort(h->port("hp"), Value::ofFloat(200)) == Status::kOk);
    h->step(2.0f);
    CHECK(h->doc().byId("Bar").layoutBox().width == doctest::Approx(200));
    std::vector<uint8_t> ir2 = compileOrFail(v1);
    BundleDelta delta;
    delta.ir.push_back(IrReload{h->handle, IrBlob{ir2.data(), ir2.size()}, false});
    REQUIRE(h->ctx.hotReload(delta) == Status::kOk);
    h->step(0.016f);
    CHECK(h->doc().byId("Bar").layoutBox().width == doctest::Approx(200));  // Carried port, no 30 -> 200.
}

TEST_CASE("t391: hotReload carries a list port's nested lists and rebuilds the nested repeats") {
    const std::string v1 = R"X(<xt><body id="Root">
<div id="Hud" port:groups="list<struct{name: string, rows: list<struct{t: string}>}>"></div>
<repeat port="groups" id="Rows"><div id="Row">
  <repeat port="item(rows)" id="Sub"><span id="Cell" bind:text="item(t)"></span></repeat>
</div></repeat></body></xt>)X";
    auto h = open(v1);
    Document& d = h->doc();
    REQUIRE(d.setPortText(h->port("groups"), R"([{"name":"A","rows":[{"t":"a0"},{"t":"a1"}]}])") ==
            Status::kOk);
    h->step(0.016f);
    std::string v2 = v1;
    v2.replace(v2.find("<div id=\"Row\">"), 14, "<div id=\"Row\" class=\"r\">");
    std::vector<uint8_t> ir2 = compileOrFail(v2);
    BundleDelta delta;
    delta.ir.push_back(IrReload{h->handle, IrBlob{ir2.data(), ir2.size()}, false});
    REQUIRE(h->ctx.hotReload(delta) == Status::kOk);
    Document& n = h->doc();
    h->step(0.016f);
    Value groups = n.getPort(n.lookupPort("groups"));
    REQUIRE(groups.itemCount == 1);
    REQUIRE(groups.items[0].items[1].itemCount == 2);
    CHECK(n.string(groups.items[0].items[1].items[1].items[0].sid) == "a1");
    CHECK(n.byId("Rows/0/Sub/1/Cell").text() == "a1");
}

TEST_CASE("t200: hotReload reports kIncompatible and reloads fresh when an id is gone") {
    auto h = open(kUiV1);
    Document& d = h->doc();
    REQUIRE(d.setPort(h->port("hp"), Value::ofFloat(42)) == Status::kOk);
    REQUIRE(d.setCustomState(d.byId("A"), "Warn", true) == Status::kOk);
    std::string v2 = kUiV1;  // Element A renamed to Z (its rules and timeline track follow).
    for (size_t at = v2.find("#A "); at != std::string::npos; at = v2.find("#A ")) {
        v2[at + 1] = 'Z';
    }
    v2.replace(v2.find("<div id=\"A\""), 11, "<div id=\"Z\"");
    v2.replace(v2.find("#A:state(Warn)"), 14, "#Z:state(Warn)");
    std::vector<uint8_t> ir2 = compileOrFail(v2);
    BundleDelta delta;
    delta.ir.push_back(IrReload{h->handle, IrBlob{ir2.data(), ir2.size()}, false});
    HotReloadReport report;
    CHECK(h->ctx.hotReload(delta, &report) == Status::kIncompatible);
    REQUIRE(report.documents.size() == 1);
    CHECK(report.documents[0].status == Status::kIncompatible);
    CHECK(report.documents[0].missing == std::vector<std::string>{"element A"});
    Document& n = h->doc();
    CHECK(n.byId("Z").valid());
    CHECK(n.getPort(n.lookupPort("hp")).f == doctest::Approx(0));  // Fresh.

    // A port that changes type is incompatible too.
    std::string v3 = kUiV1;
    v3.replace(v3.find("port:hp=\"number\""), 16, "port:hp=\"string\"");
    std::vector<uint8_t> ir3 = compileOrFail(v3);
    REQUIRE(h->doc().setPort(h->port("hp"), Value::ofFloat(5)) == Status::kOk);
    BundleDelta d3;
    d3.ir.push_back(IrReload{h->handle, IrBlob{ir3.data(), ir3.size()}, false});
    HotReloadReport r3;
    CHECK(h->ctx.hotReload(d3, &r3) == Status::kIncompatible);
    REQUIRE(r3.documents.size() == 1);
    CHECK(r3.documents[0].missing == std::vector<std::string>{"port hp"});

    // A broken blob keeps the old Document; an unknown handle is an invalid argument.
    Document* kept = &h->doc();
    const uint8_t junk[16] = {1, 2, 3};
    BundleDelta broken;
    broken.ir.push_back(IrReload{h->handle, IrBlob{junk, sizeof(junk)}, false});
    CHECK(h->ctx.hotReload(broken) != Status::kOk);
    CHECK(&h->doc() == kept);
    BundleDelta unknown;
    unknown.ir.push_back(IrReload{99, IrBlob{ir3.data(), ir3.size()}, false});
    CHECK(h->ctx.hotReload(unknown) == Status::kInvalidArgument);
}

TEST_CASE("t330: system.reducedMotion is an implicit string port fed by postSystemEvent") {
    auto h = open(page("", "<span id=\"M\" bind:text=\"system.reducedMotion\"></span>"));
    Document& d = h->doc();
    REQUIRE(d.lookupPort("system.reducedMotion") != kInvalidPort);
    CHECK(d.portInfo(d.lookupPort("system.reducedMotion")).type == PortValueType::String);
    REQUIRE(h->ctx.postSystemEvent("reducedMotion", "reduce") == Status::kOk);
    h->ctx.update(0.0f);  // T-389.
    CHECK(d.byId("M").text() == "reduce");
    CHECK(h->ctx.device().reducedMotion);
    REQUIRE(h->ctx.postSystemEvent("reducedMotion", "0") == Status::kOk);
    h->ctx.update(0.0f);
    CHECK(d.byId("M").text() == "0");
    CHECK_FALSE(h->ctx.device().reducedMotion);
}

// -- T-398 (23 §23.ad.1): Document::snapshotState / restoreState ------------------------------------

namespace {

constexpr const char* kStateUi = R"X(<xt><head><style>
#A { width: 40px; height: 20px; }
#A:state(Warn) { opacity: 0.5; }
#F { width: 200px; height: 30px; }
#Box { width: 100px; height: 50px; overflow: scroll; }
#Tall { width: 80px; height: 400px; }
@timeline intro { duration: 2s; #A { 0s { opacity: 0 } 2s { opacity: 1 } } }
</style></head><body id="Root">
<div id="Hud" port:hp="number" port:name="string" port:mode="enum(Normal|Danger)" port:pos="vec2"
     port:tint="color" port:on="bool" port:items="list<struct{name: string, n: number}>"
     port:tags="list<string>" port:hit="event"></div>
<div id="A" state-groups="Level: Warn|Ok"></div>
<input type="text" id="F"/>
<div id="Box"><div id="Tall"></div></div>
</body></xt>)X";

/// T-650: a snapshot without its `clock` member (the update() count differs after one more frame).
std::string withoutClock(std::string snap) {
    const size_t at = snap.find(",\"clock\":{");
    if (at != std::string::npos) {
        snap.erase(at, snap.find('}', at) + 1 - at);
    }
    return snap;
}

InputEvent stateKey(KeyCode key, uint8_t modifiers = 0) {
    InputEvent ev;
    ev.kind = InputEvent::KeyDown;
    ev.key = key;
    ev.modifiers = modifiers;
    return ev;
}

}  // namespace

TEST_CASE("t398: snapshotState in one Context, restoreState in another, before its first frame") {
    auto h = open(kStateUi);
    Document& d = h->doc();
    REQUIRE(d.setPortText(h->port("hp"), "42.125") == Status::kOk);
    REQUIRE(d.setPortText(h->port("name"), "Ann \"Q\"\n") == Status::kOk);
    REQUIRE(d.setPortText(h->port("mode"), "Danger") == Status::kOk);
    REQUIRE(d.setPortText(h->port("pos"), "0.1,-2.5") == Status::kOk);
    REQUIRE(d.setPortText(h->port("tint"), "#11223344") == Status::kOk);
    REQUIRE(d.setPortText(h->port("on"), "true") == Status::kOk);
    REQUIRE(d.setPortText(h->port("items"), R"([{"name":"a","n":1.5},{"name":"b","n":2}])") == Status::kOk);
    REQUIRE(d.setPortText(h->port("tags"), R"(["x","y"])") == Status::kOk);
    REQUIRE(d.setCustomState(d.byId("A"), "Warn", true) == Status::kOk);
    ElementRef field = d.byId("F");
    REQUIRE(field.valid());
    field.setValue("hello");
    d.setFocus(field, 0);
    d.dispatchInput(stateKey(KeyCode::Left, kShift));  // Selection: anchor 5, cursor 4.
    d.dispatchInput(stateKey(KeyCode::Left, kShift));  // cursor 3.
    REQUIRE(d.playTimeline("", "intro") == Status::kOk);
    h->step(0.5f);
    REQUIRE(d.byId("Box").scrollTo(0, 120) == Status::kOk);
    CHECK(d.byId("Box").scrollPos().y == doctest::Approx(120));
    const float t0 = d.timelineTime("", "intro");
    CHECK(t0 > 0.3f);

    const std::string snap = d.snapshotState();
    CAPTURE(snap);
    CHECK(snap.find(R"("F":{"value":"hello","cursor":3,"anchor":5})") != std::string::npos);
    CHECK(snap.find(R"("focus":{"0":"F"})") != std::string::npos);
    CHECK(snap.find(R"("Box":[0,120])") != std::string::npos);

    // Another Context, the same IR; restored after load, before the first update().
    Context ctx2{ContextConfig{}};
    ctx2.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    Result<DocumentHandle> h2 = ctx2.loadDocument(IrBlob{h->ir.data(), h->ir.size()}, false);
    REQUIRE(h2.ok());
    Document& n = *ctx2.document(h2.value());
    REQUIRE(n.restoreState(snap) == Status::kOk);
    ctx2.update(0.0f);

    CHECK(n.getPort(n.lookupPort("hp")).f == doctest::Approx(42.125f));
    CHECK(n.portText(n.lookupPort("name")) == "Ann \"Q\"\n");
    CHECK(n.portText(n.lookupPort("mode")) == "Danger");
    CHECK(n.getPort(n.lookupPort("pos")).v2.x == 0.1f);
    CHECK(n.portText(n.lookupPort("tint")) == d.portText(d.lookupPort("tint")));
    CHECK(n.portText(n.lookupPort("on")) == "true");
    Value items = n.getPort(n.lookupPort("items"));
    REQUIRE(items.itemCount == 2);
    CHECK(n.string(items.items[1].items[0].sid) == "b");
    CHECK(items.items[0].items[1].f == doctest::Approx(1.5f));
    CHECK(n.getPort(n.lookupPort("tags")).itemCount == 2);
    CHECK(n.hasCustomState(n.byId("A"), "Warn"));
    CHECK(n.byId("F").value() == "hello");
    CHECK(n.focus(0).id() == "F");
    CHECK(n.isTimelinePlaying("", "intro"));
    CHECK(n.timelineTime("", "intro") == doctest::Approx(t0));
    CHECK(n.byId("Box").scrollPos().y == doctest::Approx(120));
    // Everything the snapshot covers, the input caret and selection included, is the same (T-650:
    // but the update() count, one more here).
    CHECK(withoutClock(n.snapshotState()) == withoutClock(snap));

    // Applied again at any time: the snapshot is authoritative (state cleared, focus moved).
    REQUIRE(n.setCustomState(n.byId("A"), "Warn", false) == Status::kOk);
    REQUIRE(n.setCustomState(n.byId("A"), "Ok", true) == Status::kOk);
    n.setFocus(ElementRef{}, 0);
    REQUIRE(n.byId("Box").scrollTo(0, 10) == Status::kOk);
    REQUIRE(n.restoreState(snap) == Status::kOk);
    ctx2.update(0.0f);
    CHECK(n.hasCustomState(n.byId("A"), "Warn"));
    CHECK_FALSE(n.hasCustomState(n.byId("A"), "Ok"));
    CHECK(n.focus(0).id() == "F");
    CHECK(n.byId("Box").scrollPos().y == doctest::Approx(120));

    // Not a snapshot: nothing applied.
    CHECK(n.restoreState("{") == Status::kInvalidArgument);
    CHECK(n.restoreState("[]") == Status::kInvalidArgument);
    CHECK(n.restoreState(R"({"version":4})") == Status::kInvalidArgument);  // T-651: 1, 2 and 3 load.
    // Unknown elements, ports, states and timelines are skipped.
    CHECK(n.restoreState(R"({"version":1,"ports":{"nope":"1","hp":"7"},"states":{"Zz":["Warn"]},)"
                         R"("timelines":[{"name":"none"}]})") == Status::kOk);
    CHECK(n.getPort(n.lookupPort("hp")).f == doctest::Approx(7));
}

TEST_CASE("t398: hotReload carries input values and scroll offsets too") {
    auto h = open(kStateUi);
    Document& d = h->doc();
    d.byId("F").setValue("abc");
    REQUIRE(d.byId("Box").scrollTo(0, 64) == Status::kOk);
    std::string v2 = kStateUi;
    v2.replace(v2.find("width: 40px"), 11, "width: 60px");
    std::vector<uint8_t> ir2 = compileOrFail(v2);
    BundleDelta delta;
    delta.ir.push_back(IrReload{h->handle, IrBlob{ir2.data(), ir2.size()}, false});
    REQUIRE(h->ctx.hotReload(delta) == Status::kOk);
    Document& n = h->doc();
    h->step();
    CHECK(n.byId("F").value() == "abc");
    CHECK(n.byId("Box").scrollPos().y == doctest::Approx(64));
}

// -- T-650 (23 XF-097 version 2): runtime classes, the document clock and `shown` -----------------

namespace {

constexpr const char* kV2Ui = R"X(<xt><head><style>
#A { width: 40px; height: 20px; opacity: 1; }
#B, #C { width: 40px; height: 20px; }
.big { width: 80px; }
.red { opacity: 0.5; }
@timeline intro { duration: 2s; #A { 0s { opacity: 0 } 2s { opacity: 1 } } }
</style></head><body id="Root" on:show="play(intro)">
<div id="A" class="big"></div><div id="B"></div><div id="C"></div>
</body></xt>)X";

/// Counts the OnShown calls of a Context.
struct ShownCounter : ContextListener {
    int shown = 0;
    void onShown(DocumentHandle /*document*/) override { ++shown; }
};

/// A second Context with the same IR loaded, not updated yet.
struct Fresh {
    Context ctx{ContextConfig{}};
    DocumentHandle handle = 0;
    ShownCounter counter;
    explicit Fresh(const std::vector<uint8_t>& ir) {
        ctx.addListener(&counter);
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        Result<DocumentHandle> h = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(h.ok());
        handle = h.value();
    }
    ~Fresh() { ctx.removeListener(&counter); }
    Fresh(const Fresh&) = delete;
    Fresh& operator=(const Fresh&) = delete;
    Document& doc() { return *ctx.document(handle); }
};

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

}  // namespace

TEST_CASE("t650: runtime classes round-trip as add / remove deltas against the IR") {
    auto h = open(kV2Ui);
    Document& d = h->doc();
    REQUIRE(d.byId("A").removeClass("big"));
    REQUIRE(d.byId("A").addClass("red"));
    REQUIRE(d.byId("B").addClass("zeta"));
    REQUIRE(d.byId("B").addClass("red"));
    h->step();
    const std::string snap = d.snapshotState();
    CAPTURE(snap);
    CHECK(snap.find(R"({"version":3,)") == 0);
    CHECK(snap.find(
              R"("classes":{"A":{"add":["red"],"remove":["big"]},"B":{"add":["red","zeta"],"remove":[]}})") !=
          std::string::npos);
    CHECK(snap.find(R"("shown":true})") != std::string::npos);

    Fresh f(h->ir);
    Document& n = f.doc();
    REQUIRE(n.byId("C").addClass("stray"));  // The snapshot is authoritative: C gets its IR classes back.
    REQUIRE(n.restoreState(snap) == Status::kOk);
    f.ctx.update(0.0f);
    CHECK(n.byId("A").hasClass("red"));
    CHECK_FALSE(n.byId("A").hasClass("big"));
    CHECK(n.byId("B").hasClass("red"));
    CHECK(n.byId("B").hasClass("zeta"));
    CHECK_FALSE(n.byId("C").hasClass("stray"));
    CHECK(n.byId("B").property(prop::Opacity).f == doctest::Approx(0.5f));  // Restyled.
    CHECK(n.byId("A").boundingBox().w == doctest::Approx(40.0f));           // `.big` is gone.
    CHECK(withoutClock(n.snapshotState()) == withoutClock(snap));

    // A version 1 snapshot (no `classes`) leaves the classes alone.
    REQUIRE(n.byId("C").addClass("stray"));
    REQUIRE(n.restoreState(R"({"version":1})") == Status::kOk);
    CHECK(n.byId("C").hasClass("stray"));
    CHECK(n.byId("A").hasClass("red"));
}

TEST_CASE("t650: the clock round-trips bit for bit; restored before the first update, no initial show") {
    auto h = open(kV2Ui);  // Shown at its first update: `on:show` plays the intro.
    Document& d = h->doc();
    for (int i = 0; i < 50; ++i) {
        h->step(1.0f / 60.0f);
    }
    REQUIRE(d.isTimelinePlaying("", "intro"));
    const std::string snap = d.snapshotState();
    CAPTURE(snap);
    CHECK(snap.find(R"("frame":51})") != std::string::npos);

    Fresh f(h->ir);
    Document& n = f.doc();
    REQUIRE(n.restoreState(snap) == Status::kOk);
    CHECK(sameBits(f.ctx.clock(), h->ctx.clock()));  // The only document: the Context's clock too.
    for (int i = 0; i < 30; ++i) {
        h->step(1.0f / 60.0f);
        f.ctx.update(1.0f / 60.0f);
        CAPTURE(i);
        REQUIRE(sameBits(n.timelineTime("", "intro"), d.timelineTime("", "intro")));
        REQUIRE(sameBits(n.byId("A").property(prop::Opacity).f, d.byId("A").property(prop::Opacity).f));
    }
    CHECK(f.counter.shown == 0);  // No OnShown, no `show` trigger (the intro was not restarted).
    CHECK(n.snapshotState() == d.snapshotState());

    // A snapshot taken before the first update says shown=false: the restored document shows as usual.
    Fresh early(h->ir);
    const std::string unshown = early.doc().snapshotState();
    CHECK(unshown.find(R"("shown":false})") != std::string::npos);
    Fresh g(h->ir);
    REQUIRE(g.doc().restoreState(unshown) == Status::kOk);
    g.ctx.update(0.0f);
    CHECK(g.counter.shown == 1);
    CHECK(g.doc().isTimelinePlaying("", "intro"));
    CHECK(g.doc().timelineTime("", "intro") == 0.0f);
}

TEST_CASE("t650: a version 1 snapshot still loads: clock kept, initial show as before") {
    auto h = open(kV2Ui);
    Fresh f(h->ir);
    Document& n = f.doc();
    REQUIRE(n.byId("C").addClass("stray"));
    REQUIRE(
        n.restoreState(R"({"version":1,"states":{},"timelines":[{"name":"intro","t":1,"playing":true}]})") ==
        Status::kOk);
    CHECK(f.ctx.clock() == 0.0f);
    CHECK(n.timelineTime("", "intro") == doctest::Approx(1.0f));
    f.ctx.update(0.0f);
    CHECK(f.counter.shown == 1);                 // The initial show, as before T-650.
    CHECK(n.timelineTime("", "intro") == 0.0f);  // Its `show` trigger restarts the intro.
    CHECK(n.byId("C").hasClass("stray"));        // No `classes`: untouched.
    CHECK(n.snapshotState().find(R"("frame":1})") != std::string::npos);  // Counted from the load.
}
