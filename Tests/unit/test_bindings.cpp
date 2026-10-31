#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/aliases.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/validate.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/types.h"
#include "xt/version.h"

/// Declared ports, data bindings, the converter catalog and the port API (Docs/design/
/// 27-interaction.md §27.3, 03 §3.12.3, 04 §4.10.4, 12 §12.7, 13 §13.8.1, Docs/tasks/T-175.md).

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\">" +
           std::string(body) + "</body></xt>";
}

/// T-407: `behavior` > 0 emits META with that `behavior` (0 = no META, the pre-beta behavior).
CompileResult compileSrc(const std::string& xt, uint32_t behavior = 0) {
    CompileOptions options;
    options.emitMeta = behavior != 0;
    options.behavior = behavior;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool compileError(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;
    float clock = 0;

    void step(float dt) {
        clock += dt;
        doc->update(clock);
    }
    ElementRef el(std::string_view id) const { return doc->byId(id); }
    PortId port(std::string_view name) const {
        PortId p = doc->lookupPort(name);
        REQUIRE(p != kInvalidPort);
        return p;
    }
    void set(std::string_view name, float v) {
        REQUIRE(doc->setPort(port(name), Value::ofFloat(v)) == Status::kOk);
    }
    void setText(std::string_view name, std::string_view v) {
        REQUIRE(doc->setPortText(port(name), v) == Status::kOk);
    }
    /// T-389 (27 §27.4.1): a port change is evaluated by the next update(); the text is read after
    /// one (the clock unmoved).
    std::string text(std::string_view id) const {
        doc->update(clock);
        return std::string(el(id).text());
    }
};

Ui open(const std::string& xt, uint32_t behavior = 0) {
    Ui out;
    out.result = compileSrc(xt, behavior);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    Status status =
        Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr, out.doc);
    REQUIRE(status == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 200});
    out.doc->update(0.0f);
    return out;
}

/// One text binding `expr` over ports hp (number) and name (string), after `setup`.
std::string convert(const std::string& expr, float hp, std::string_view name = "") {
    Ui ui = open(page("",
                      "<div id=\"P\" port:hp=\"number\" port:hp_max=\"number\" port:name=\"string\" "
                      "port:mode=\"enum(Normal|Warn|Danger)\" port:flag=\"bool\"></div>"
                      "<span id=\"T\" bind:text=\"" +
                          expr + "\"></span>"));
    ui.set("hp_max", 200);
    ui.set("hp", hp);
    if (!name.empty()) {
        ui.setText("name", name);
    }
    return ui.text("T");
}

}  // namespace

// -- Port declarations ----------------------------------------------------------------------------

TEST_CASE("bindings: port declarations compile into PORT with types, directions and fields") {
    CompileResult r = compileSrc(page("", R"X(
        <div id="Hud" port:hp="number in" port:label="string" port:alive="bool out" port:tint="color"
             port:pos="vec2" port:dir="vec3 call" port:who="entity" port:mode="enum(<Normal|Warn|Danger>)"
             port:stats="struct{atk: number, name: string}" port:items="list<struct{icon: texture, count: number}>"
             port:portrait="texture" port:hit="event out"></div>)X"));
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const PortChunk& pc = view.value().ports();
    REQUIRE(pc.present());
    REQUIRE(pc.ports().size() == 12);
    auto type = [&](size_t i) { return static_cast<PortValueType>(pc.ports()[i].type); };
    auto dir = [&](size_t i) { return static_cast<PortDir>(pc.ports()[i].dir); };
    CHECK(view.value().string(pc.ports()[0].name) == "hp");
    CHECK(type(0) == PortValueType::Number);
    CHECK(dir(0) == PortDir::In);
    CHECK(type(1) == PortValueType::String);
    CHECK(dir(1) == PortDir::In);
    CHECK(type(2) == PortValueType::Bool);
    CHECK(dir(2) == PortDir::Out);
    CHECK(type(3) == PortValueType::Color);
    CHECK(type(4) == PortValueType::Vec2);
    CHECK(type(5) == PortValueType::Vec3);
    CHECK(dir(5) == PortDir::Call);
    CHECK(type(6) == PortValueType::Entity);
    CHECK(type(7) == PortValueType::Enum);
    CHECK(pc.ports()[7].fieldCount == 3);
    CHECK(type(8) == PortValueType::Struct);
    CHECK(pc.ports()[8].fieldCount == 2);
    CHECK(type(9) == PortValueType::List);
    CHECK(pc.ports()[9].fieldCount == 2);
    CHECK(static_cast<PortValueType>(pc.fields()[pc.ports()[9].fieldOffset].type) == PortValueType::Texture);
    CHECK(type(10) == PortValueType::Texture);
    CHECK(type(11) == PortValueType::Event);
    for (const IrPort& p : pc.ports()) {
        CHECK(p.scope == kNoPortScope);
        CHECK((p.flags & kPortExpose) != 0);
    }

    // Round trip: IrWriter::fromView re-emits identical bytes (PORT included).
    IrWriter again = IrWriter::fromView(view.value());
    CHECK(again.finish() == r.ir);
}

TEST_CASE("bindings: invalid port declarations and bindings are errors") {
    auto errs = [](const std::string& body, std::string_view needle) {
        CompileResult r = compileSrc(page("", body));
        CAPTURE(body);
        CHECK(!r.ok());
        CHECK(compileError(r, needle));
    };
    errs(R"X(<div port:hp="numbr"></div>)X", "unknown port type");
    errs(R"X(<div port:hp="number sideways"></div>)X", "unknown port type");
    errs(R"X(<div port:hp="number"></div><div port:hp="string"></div>)X", "declared twice");
    errs(R"X(<div port:s="struct{a: struct{b: number}}"></div>)X", "invalid struct field");
    errs(R"X(<div port:l="list<texture>"></div>)X", "struct");  // T-381: list<number> is valid.
    errs(R"X(<div port:e="enum(a|a)"></div>)X", "listed twice");
    errs(R"X(<div expose="maybe"></div>)X", "expose must be");
    errs(R"X(<div id="A" bind:width="nope"></div>)X", "unknown port");
    errs(R"X(<div id="A" port:hp="number" bind:width="hp | wiggle(1)"></div>)X", "unknown converter");
    errs(R"X(<div id="A" port:hp="number" bind:wibble="hp"></div>)X", "unknown or shorthand property");
    errs(R"X(<div id="A" port:hp="number" bind:margin="hp"></div>)X", "unknown or shorthand property");
    errs(R"X(<div id="A" port:hp="number" bind:width="hp | range(0, 1)"></div>)X", "takes 4");
    errs(R"X(<div id="A" port:hp="number" bind:text="hp | lt(20%)"></div>)X", "_max");
    errs(R"X(<div id="A" port:hp="number" bind:text="hp | scale(big)"></div>)X", "neither a number");
    errs(R"X(<div id="A" port:hp="number" bind:variant.Kind="hp"></div>)X", "symbol instance");
    errs(R"X(<div id="A" port:hp="number" bind:text="hp | tween(1s, wobbly)"></div>)X", "unknown easing");
    errs(R"X(<div id="A" port:hp="number" bind:text="format(hp)"></div>)X", "quoted pattern");
    errs(R"X(<div id="A" port:hp="number" bind:text="#Nobody.width"></div>)X", "does not name an element");
    errs(R"X(<div id="A" port:hp="number" on:port(hp)="if lt(20%) hide"></div>)X", "_max");
}

TEST_CASE("bindings: the port table, lookup forms and text values") {
    Ui ui = open(page("", R"X(
        <div id="Hud" port:hp="number" port:alive="bool" port:tint="color" port:pos="vec2"
             port:mode="enum(Normal|Warn)" port:who="entity" port:label="string" port:hit="event"></div>)X"));
    std::vector<PortRecord> table = ui.doc->ports();
    REQUIRE(table.size() == 8);
    CHECK(table[0].name == "hp");
    CHECK(table[0].qualifiedName == "hp");
    CHECK(table[0].elementId == "Hud");
    CHECK(table[0].documentLevel);
    CHECK(table[4].enumValues == std::vector<std::string>{"Normal", "Warn"});
    CHECK(ui.doc->lookupPort("hp") == table[0].id);
    CHECK(ui.doc->lookupPort("Hud.hp") == table[0].id);
    CHECK(ui.doc->lookupPort("#Hud.hp") == table[0].id);
    CHECK(ui.doc->lookupPort("nope") == kInvalidPort);
    CHECK(ui.doc->lookupPort("other#Hud.hp") == kInvalidPort);  // Another UI: not this task.

    PortId hp = ui.port("hp");
    CHECK(ui.doc->setPort(hp, Value::ofFloat(72)) == Status::kOk);
    CHECK(ui.doc->getPort(hp).f == 72.0f);
    CHECK(ui.doc->setPort(hp, Value::ofColor(Color{})) == Status::kInvalidArgument);
    CHECK(ui.doc->setPort(kInvalidPort, Value::ofFloat(1)) == Status::kNotFound);
    CHECK(ui.doc->portText(hp) == "72");
    ui.setText("alive", "true");
    CHECK(ui.doc->getPort(ui.port("alive")).f == 1.0f);
    ui.setText("tint", "#ff0000");
    CHECK(ui.doc->getPort(ui.port("tint")).c.r == doctest::Approx(1.0f));
    ui.setText("pos", "3, 4");
    CHECK(ui.doc->getPort(ui.port("pos")).v2.y == 4.0f);
    ui.setText("mode", "Warn");
    CHECK(ui.doc->getPort(ui.port("mode")).e == 1u);
    CHECK(ui.doc->portText(ui.port("mode")) == "Warn");
    CHECK(ui.doc->setPortText(ui.port("mode"), "Nope") == Status::kInvalidArgument);
    CHECK(ui.doc->setPort(ui.port("mode"), Value::ofStringId(ui.doc->intern("Normal"))) == Status::kOk);
    CHECK(ui.doc->getPort(ui.port("mode")).e == 0u);
    ui.setText("who", "123456789");
    CHECK(ui.doc->getPort(ui.port("who")).e == 123456789u);
    ui.setText("label", "hello world");
    CHECK(ui.doc->portText(ui.port("label")) == "hello world");
    CHECK(ui.doc->string(ui.doc->getPort(ui.port("label")).sid) == "hello world");
}

// -- Converters -----------------------------------------------------------------------------------

TEST_CASE("bindings: numeric converters") {
    CHECK(convert("hp | range(0, 100, 0, 10)", 50) == "5");
    CHECK(convert("hp | range(0, 100, 0, 10)", 150) == "10");  // Clamped.
    CHECK(convert("hp | range(0, hp_max, 0, 1)", 50) == "0.25");
    CHECK(convert("hp | range(0, 100, 0%, 100%)", 30) == "30%");
    CHECK(convert("hp | clamp(10, 20)", 50) == "20");
    CHECK(convert("hp | round", 2.6f) == "3");
    CHECK(convert("hp | round(1)", 2.64f) == "2.6");
    CHECK(convert("hp | abs", -4) == "4");
    CHECK(convert("hp | scale(3)", 4) == "12");
    CHECK(convert("hp | offset(-1)", 4) == "3");
    CHECK(convert("hp | scale(hp_max)", 2) == "400");
}

TEST_CASE("bindings: comparison and bool converters") {
    CHECK(convert("hp | lt(20)", 10) == "true");
    CHECK(convert("hp | le(10)", 10) == "true");
    CHECK(convert("hp | gt(20)", 10) == "false");
    CHECK(convert("hp | ge(10)", 10) == "true");
    CHECK(convert("hp | eq(10)", 10) == "true");
    CHECK(convert("hp | ne(10)", 10) == "false");
    CHECK(convert("hp | between(5, 15)", 10) == "true");
    CHECK(convert("hp | lt(20%)", 30) == "true");  // 20% of hp_max (200) = 40.
    CHECK(convert("hp | lt(20%)", 50) == "false");
    CHECK(convert("hp | lt(hp_max)", 50) == "true");
    CHECK(convert("hp | lt(20) | not", 10) == "false");
    CHECK(convert("hp | gt(5) | and(hp_max)", 10) == "true");
    CHECK(convert("hp | gt(50) | or(flag)", 10) == "false");
    CHECK(convert("name | eq(Bob)", 0, "Bob") == "true");
    CHECK(convert("name | eq('Bob')", 0, "Al") == "false");
}

TEST_CASE("bindings: format converters") {
    CHECK(convert("format('{0}/{1}', hp, hp_max)", 72) == "72/200");
    CHECK(convert("hp | format('HP {0} of {1}', hp_max)", 5) == "HP 5 of 200");
    CHECK(convert("format('Lv {0}: {1}', 3, name)", 0, "Ann") == "Lv 3: Ann");
    CHECK(convert("hp | thousands", 1234567) == "1,234,567");
    CHECK(convert("hp | thousands", -1234) == "-1,234");
    CHECK(convert("hp | fixed(2)", 3.14159f) == "3.14");
    CHECK(convert("hp | scale(0.01) | percent", 42) == "42%");
    CHECK(convert("hp | time", 125) == "02:05");
    CHECK(convert("hp | time(h:mm:ss)", 3725) == "1:02:05");
    CHECK(convert("hp | time(m:ss)", 61) == "1:01");
    CHECK(convert("name | upper", 0, "Abc") == "ABC");
    CHECK(convert("name | lower", 0, "Abc") == "abc");
    CHECK(convert("name | concat(' the ', hp)", 3, "Ann") == "Ann the 3");
    CHECK(convert("name | localize", 0, "@menu.start") == "menu.start");  // No provider: the key.
    CHECK(convert("name | glyph", 0, "jump") == "jump");
    CHECK(convert("name | fromToken(color-{0})", 0, "danger") == "var(--color-danger)");
}

namespace {

struct Strings : ILocalizationProvider {
    std::string_view resolve(std::string_view key, std::string_view lang) override {
        if (key == "hp.label") {
            return lang == "zh-TW" ? std::string_view("Life") : std::string_view("Health");
        }
        return {};
    }
};

}  // namespace

TEST_CASE("bindings: localize resolves through the provider and follows the language") {
    Ui ui = open(page("", R"X(<div id="P" port:key="string"></div>
        <span id="L" bind:text="key | localize | upper"></span>)X"));
    Strings strings;
    ui.setText("key", "@hp.label");
    CHECK(ui.text("L") == "HP.LABEL");
    ui.doc->setLocalization(&strings, "en");
    CHECK(ui.text("L") == "HEALTH");
    ui.doc->setLocalization(&strings, "zh-TW");
    CHECK(ui.text("L") == "LIFE");
    ui.doc->setLocalization(nullptr, "zh-TW");
}

TEST_CASE("bindings: select and step") {
    Ui ui = open(page("", R"X(
        <div id="P" port:mode="enum(Normal|Warn|Danger)" port:hp="number"></div>
        <span id="S" bind:text="mode | select(Normal: ok, Warn: 'look out', default: '?')"></span>
        <span id="F" bind:text="select(mode, Danger: RUN)"></span>
        <span id="T" bind:text="hp | step(0: empty, 30: low, 70: fine)"></span>)X"));
    CHECK(ui.text("S") == "ok");
    CHECK(ui.text("F") == "");  // No match, no default: nothing applied.
    CHECK(ui.text("T") == "empty");
    ui.setText("mode", "Warn");
    CHECK(ui.text("S") == "look out");
    ui.setText("mode", "Danger");
    CHECK(ui.text("S") == "?");
    CHECK(ui.text("F") == "RUN");
    ui.set("hp", 45);
    CHECK(ui.text("T") == "low");
    ui.set("hp", 70);
    CHECK(ui.text("T") == "fine");
}

TEST_CASE("bindings: lerpColor, count, item and host converters") {
    Ui ui = open(page("#B { width: 10px; height: 10px; }", R"X(
        <div id="P" port:t="number" port:stats="struct{atk: number, name: string}"
             port:items="list<struct{name: string, count: number}>"></div>
        <div id="B" bind:background-color="t | lerpColor(#000000, #ff0000)"></div>
        <span id="N" bind:text="items | count"></span>
        <span id="A" bind:text="stats | item(name)"></span>
        <span id="H" bind:text="t | host.twice(10)"></span>)X"));
    ui.set("t", 0.5f);
    ui.step(0.016f);
    Color c = ui.el("B").property(prop::BackgroundColor).c;
    CHECK(c.r == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(c.g == doctest::Approx(0.0f));
    CHECK(ui.text("H") == "0.5");  // Unregistered: passes through (with a warning).

    Result<uint32_t> op =
        ui.doc->registerConverter("twice", [](const Value& in, std::span<const Value> args) {
            return Value::ofFloat(in.f * 2 + (args.empty() ? 0.0f : args[0].f));
        });
    REQUIRE(op.ok());
    CHECK(op.value() == 0x80u);
    CHECK(ui.text("H") == "11");  // Re-evaluated on registration.
    CHECK(ui.doc->registerConverter("", {}).status() == Status::kInvalidArgument);

    StringId names[2] = {ui.doc->intern("atk"), ui.doc->intern("name")};
    Value values[2] = {Value::ofFloat(7), Value::ofStringId(ui.doc->intern("Rex"))};
    CHECK(ui.doc->setPort(ui.port("stats"), Value::ofStruct(names, values, 2)) == Status::kOk);
    CHECK(ui.text("A") == "Rex");
    Value got = ui.doc->getPort(ui.port("stats"));
    REQUIRE(got.type == ValueType::Struct);
    CHECK(got.itemCount == 2);
    CHECK(got.items[0].f == 7.0f);

    PortId items = ui.port("items");
    StringId entryNames[2] = {ui.doc->intern("name"), ui.doc->intern("count")};
    Value e0[2] = {Value::ofStringId(ui.doc->intern("a")), Value::ofFloat(1)};
    Value e1[2] = {Value::ofStringId(ui.doc->intern("b")), Value::ofFloat(2)};
    Value entries[2] = {Value::ofStruct(entryNames, e0, 2), Value::ofStruct(entryNames, e1, 2)};
    std::vector<PortEvent> events;
    uint32_t listener = ui.doc->addPortListener([&](const PortEvent& ev) { events.push_back(ev); });
    CHECK(ui.doc->listSet(items, entries) == Status::kOk);
    CHECK(ui.text("N") == "2");
    CHECK(ui.doc->listInsert(items, 1, entries[0]) == Status::kOk);
    CHECK(ui.text("N") == "3");
    CHECK(ui.doc->listUpdate(items, 0, entries[1]) == Status::kOk);
    CHECK(ui.doc->listRemove(items, 2) == Status::kOk);
    CHECK(ui.text("N") == "2");
    CHECK(ui.doc->listRemove(items, 5) == Status::kInvalidArgument);
    CHECK(ui.doc->listInsert(items, 0, Value::ofFloat(1)) == Status::kInvalidArgument);
    CHECK(ui.doc->listSet(ui.port("stats"), entries) == Status::kInvalidArgument);
    Value list = ui.doc->getPort(items);
    REQUIRE(list.type == ValueType::List);
    REQUIRE(list.itemCount == 2);
    CHECK(ui.doc->string(list.items[0].items[0].sid) == "b");
    CHECK(list.items[1].items[1].f == 1.0f);
    REQUIRE(events.size() == 4);
    CHECK(events[0].kind == PortEventKind::ListChanged);
    CHECK(events[1].index == 1);
    ui.doc->removePortListener(listener);
    ui.doc->listRemove(items, 0);
    CHECK(events.size() == 4);
}

TEST_CASE("T-478: port listeners added and removed while a notification runs") {
    Ui ui = open(page("", R"X(<div id="P" port:a="number" port:b="number"></div>)X"));
    std::vector<std::string> calls;
    uint32_t idSecond = 0;
    uint32_t idThird = 0;
    uint32_t idAdded = 0;
    // The first listener removes the second and itself, and adds one, on the first notification.
    uint32_t idFirst = 0;
    idFirst = ui.doc->addPortListener([&](const PortEvent&) {
        calls.push_back("first");
        ui.doc->removePortListener(idSecond);
        ui.doc->removePortListener(idFirst);  // Itself, while it runs.
        idAdded = ui.doc->addPortListener([&](const PortEvent&) { calls.push_back("added"); });
    });
    idSecond = ui.doc->addPortListener([&](const PortEvent&) { calls.push_back("second"); });
    // The third sets another port from inside the callback: the nested notification sees the
    // list as edited (no first, no second) and the listener added meanwhile.
    bool nested = false;
    idThird = ui.doc->addPortListener([&](const PortEvent& ev) {
        calls.push_back(ev.port == ui.port("b") ? "third:b" : "third:a");
        if (!nested) {
            nested = true;
            ui.set("b", 2);
        }
    });
    ui.set("a", 1);
    // first; second skipped (removed); third, whose nested `b` notification calls third and added;
    // added itself is not called by the outer notification (it was not there when it started).
    const std::vector<std::string> expected{"first", "third:a", "third:b", "added"};
    CHECK(calls == expected);
    calls.clear();
    ui.set("a", 3);
    const std::vector<std::string> after{"third:a", "added"};
    CHECK(calls == after);
    // Removed outside a notification; ids stay unique.
    CHECK(idAdded != idFirst);
    CHECK(idAdded != idSecond);
    ui.doc->removePortListener(idThird);
    ui.doc->removePortListener(idAdded);
    ui.doc->removePortListener(idSecond);  // Already gone: no-op.
    calls.clear();
    ui.set("a", 4);
    CHECK(calls.empty());
}

// -- Targets ------------------------------------------------------------------------------------

TEST_CASE("bindings: property, visible, state, class and text targets mark only what changed") {
    Ui ui = open(page(R"(#Bar { position: absolute; left: 0px; top: 0px; height: 10px; }
                         #Box { width: 200px; height: 20px; position: relative; }
                         #Tag:state(Low) { opacity: 0.5; })",
                      R"X(<div id="P" port:hp="number" port:hp_max="number"></div>
        <div id="Box"><div id="Bar" bind:width="hp | range(0, hp_max, 0%, 100%)"></div></div>
        <div id="Tag" bind:state.Low="hp | lt(20%)" bind:class.full="hp | ge(hp_max)"
             bind:visible="hp | gt(0)" bind:opacity="hp | range(0, hp_max, 0.2, 1)"></div>)X"));
    ui.set("hp_max", 100);
    ui.set("hp", 72);
    ui.step(0.016f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(144.0f));
    CHECK(!ui.doc->hasCustomState(ui.el("Tag"), "Low"));
    CHECK(!ui.el("Tag").hasClass("full"));
    CHECK(ui.el("Tag").computedStyleValue(170).e == 0u);  // visibility: visible.
    CHECK(ui.el("Tag").property(prop::Opacity).f == doctest::Approx(0.776f));

    ui.set("hp", 10);
    ui.step(0.016f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(20.0f));
    CHECK(ui.doc->hasCustomState(ui.el("Tag"), "Low"));

    ui.set("hp", 100);
    ui.step(0.016f);
    CHECK(ui.el("Tag").hasClass("full"));
    ui.set("hp", 0);
    ui.step(0.016f);
    CHECK(ui.el("Tag").computedStyleValue(170).e == 1u);  // visibility: hidden.

    // An unchanged value applies nothing: the element stays clean.
    ui.step(0.016f);
    ui.set("hp", 0);
    CHECK(!ui.doc->arena().anyDirty(static_cast<uint8_t>(kDirtyStyle)));
}

TEST_CASE("bindings: variant on a symbol instance swaps its v-<Prop>-* class") {
    Ui ui = open(R"X(<xt><head>
      <symbol name="Btn" variants="Kind=Primary|Secondary"><div id="Bg"></div></symbol>
      <style>#Root { width: 100px; }</style></head>
      <body id="Root"><div id="P" port:kind="enum(Primary|Secondary)"></div>
      <use symbol="Btn" id="B1" variant:Kind="Primary" bind:variant.Kind="kind"></use></body></xt>)X");
    CHECK(ui.el("B1").hasClass("v-Kind-Primary"));
    ui.setText("kind", "Secondary");
    ui.step(0.0f);  // T-389: evaluated by the next update().
    CHECK(ui.el("B1").hasClass("v-Kind-Secondary"));
    CHECK(!ui.el("B1").hasClass("v-Kind-Primary"));
}

TEST_CASE("bindings: #Other.<prop> is read after layout") {
    Ui ui = open(page(R"(#Src { width: 120px; height: 10px; } #Dst { height: 10px; })",
                      R"X(<div id="Src"></div><div id="Dst" bind:width="#Src.width | scale(0.5)"></div>
                          <span id="W" bind:text="#Src.width"></span>)X"));
    CHECK(ui.el("Dst").layoutBox().width == doctest::Approx(60.0f));  // Same frame.
    CHECK(ui.text("W") == "120");
    ui.el("Src").setStyle("width: 80px");
    ui.step(0.016f);
    CHECK(ui.el("Dst").layoutBox().width == doctest::Approx(40.0f));
    CHECK(ui.text("W") == "80");
}

TEST_CASE("bindings: tween runs a transition on a property and interpolates inside a chain") {
    Ui ui = open(page(R"(#Bar { height: 10px; })",
                      R"X(<div id="P" port:hp="number" port:gold="number"></div>
        <div id="Bar" bind:width="hp | tween(1s)"></div>
        <span id="G" bind:text="gold | tween(1s) | round"></span>
        <span id="D" bind:text="gold | delay(0.5s)"></span>)X"));
    ui.set("hp", 100);
    ui.set("gold", 100);
    CHECK(ui.text("G") == "0");  // Just started.
    CHECK(ui.text("D") == "0");  // Delayed.
    ui.step(0.5f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(100.0f));  // T-546: the first write is direct.
    CHECK(ui.text("G") == "50");
    CHECK(ui.text("D") == "100");
    ui.step(0.6f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(100.0f));
    CHECK(ui.text("G") == "100");
    // A change mid-way restarts from the current value.
    ui.set("gold", 0);
    ui.set("hp", 0);
    ui.step(0.5f);
    CHECK(ui.text("G") == "50");
    CHECK(ui.el("Bar").layoutBox().width ==
          doctest::Approx(50.0f).epsilon(0.05));  // The second write tweens.
}

TEST_CASE("T-546: tween applies the initial value and the first port write directly; the second tweens") {
    Ui ui = open(page("#Bar { height: 10px; }",
                      R"X(<div id="P" port:hp="number = 80"></div>
        <div id="Bar" bind:width="hp | range(0, 100, 0%, 100%) | tween(1s)"></div>)X"));
    ui.step(0.016f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(320.0f));  // Initial: 80% of 400.
    ui.set("hp", 20);
    ui.step(0.0f);
    ui.step(0.016f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(80.0f));  // First port write: direct.
    ui.set("hp", 60);
    ui.step(0.0f);
    ui.step(0.5f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(160.0f).epsilon(0.05));  // 80 -> 240 midpoint.
    ui.step(1.0f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(240.0f));
}

TEST_CASE("T-546: a tween binding with no declared default: first set is direct, second tweens") {
    Ui ui = open(page("#Bar { height: 10px; }",
                      R"X(<div id="P" port:hp="number"></div>
        <div id="Bar" bind:width="hp | tween(1s)"></div>)X"));
    ui.set("hp", 100);
    ui.step(0.016f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(100.0f));
    ui.set("hp", 0);
    ui.step(0.0f);
    ui.step(0.5f);
    CHECK(ui.el("Bar").layoutBox().width == doctest::Approx(50.0f).epsilon(0.05));
}

// -- Triggers, emit, call -------------------------------------------------------------------------

namespace {

struct HostLog : ContextListener {
    std::vector<HostEvent> events;
    std::vector<std::string> names;
    void onHostEvent(const HostEvent& e) override {
        events.push_back(e);
        names.emplace_back(e.name);
    }
};

}  // namespace

TEST_CASE("bindings: port() events fire with the value and `if` compares it") {
    Ui ui = open(page("", R"X(<div id="Hp" port:hp="number" port:hp_max="number"
        on:port(hp)="if lt(20%) state(Low) else state(Low, off)" state-groups="Level: Low|Ok"></div>
        <div id="Q" port:q="number" on:port(q)="if between(2, 4) class(mid) else class(mid, off)"></div>)X"));
    ui.set("hp_max", 100);
    ui.set("hp", 50);
    CHECK(!ui.doc->hasCustomState(ui.el("Hp"), "Low"));
    ui.set("hp", 10);
    CHECK(ui.doc->hasCustomState(ui.el("Hp"), "Low"));
    ui.set("hp", 30);
    CHECK(!ui.doc->hasCustomState(ui.el("Hp"), "Low"));
    ui.set("q", 3);
    CHECK(ui.el("Q").hasClass("mid"));
    ui.set("q", 5);
    CHECK(!ui.el("Q").hasClass("mid"));
}

TEST_CASE("bindings: emit carries the port value and the instance id; call runs port triggers") {
    CompileResult r = compileSrc(page("#A { width: 40px; height: 40px; }",
                                      R"X(<div id="A" port:score="number out" port:ask="number call"
            port:reply="number out" on:click="emit(score)"
            on:port(ask)="emit(reply)" bind:text="ask | scale(2)"></div>)X"));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    HostLog log;
    ctx.addListener(&log);
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
    REQUIRE(h.ok());
    REQUIRE(ctx.setInstanceId(h.value(), 7) == Status::kOk);
    CHECK(ctx.instanceId(h.value()) == 7u);
    Document& doc = *ctx.document(h.value());
    ctx.update(0.0f);
    doc.setPort(doc.lookupPort("score"), Value::ofFloat(42));
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{20, 20};
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ctx.pushInput(down);
    ctx.pushInput(up);
    ctx.update(0.016f);
    REQUIRE(log.events.size() == 1);
    CHECK(log.names[0] == "score");
    CHECK(log.events[0].kind == HostEventKind::Emit);
    CHECK(log.events[0].value.f == 42.0f);
    CHECK(log.events[0].port == doc.lookupPort("score"));
    CHECK(log.events[0].instanceId == 7u);

    // Host -> UI call: the argument becomes the value, port(ask) runs, the emit is returned.
    doc.setPort(doc.lookupPort("reply"), Value::ofFloat(9));
    Value arg = Value::ofFloat(5);
    Result<Value> result = doc.callPort(doc.lookupPort("ask"), std::span<const Value>(&arg, 1));
    REQUIRE(result.ok());
    CHECK(result.value().f == 9.0f);
    CHECK(doc.byId("A").text() == "10");  // Bindings follow the argument.
    CHECK(doc.callPort(doc.lookupPort("score"), {}).status() == Status::kInvalidArgument);

    // UI -> host call: emit(<call port>) runs the host handler and stores its result.
    REQUIRE(doc.setPortCallHandler(doc.lookupPort("ask"), [](std::span<const Value> args) {
        return Value::ofFloat(args[0].f + 1);
    }) == Status::kOk);
    CHECK(doc.setPortCallHandler(doc.lookupPort("score"), {}) == Status::kInvalidArgument);
    ctx.removeListener(&log);
}

TEST_CASE("bindings: UI-side emit on a call port invokes the host handler") {
    Ui ui =
        open(page("#A { width: 40px; height: 40px; }",
                  R"X(<div id="A" port:roll="number call" on:click="emit(roll)" bind:text="roll"></div>)X"));
    PortId roll = ui.port("roll");
    int calls = 0;
    REQUIRE(ui.doc->setPortCallHandler(roll, [&](std::span<const Value> args) {
        ++calls;
        return Value::ofFloat(args[0].f + 6);
    }) == Status::kOk);
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{20, 20};
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ui.doc->dispatchInput(down);
    ui.doc->dispatchInput(up);
    CHECK(calls == 1);
    CHECK(ui.text("A") == "6");
}

// -- Symbols ------------------------------------------------------------------------------------

TEST_CASE("bindings: ports inside symbol instances are per instance, exposed as <use id>.<port>") {
    Ui ui = open(R"X(<xt><head>
      <symbol name="Bar"><div id="Fill" port:value="number" bind:text="value | round"></div>
        <div id="Hidden" port:secret="number" expose="false"></div></symbol></head>
      <body id="Root"><use symbol="Bar" id="P1"></use><use symbol="Bar" id="P2" expose:false></use>
      <div id="Doc" port:value="number" bind:text="value"></div></body></xt>)X");
    PortId p1 = ui.doc->lookupPort("P1.value");
    PortId p2path = ui.doc->lookupPort("P2/Fill.value");
    REQUIRE(p1 != kInvalidPort);
    REQUIRE(p2path != kInvalidPort);
    CHECK(ui.doc->lookupPort("P1/Fill.value") == p1);
    CHECK(ui.doc->lookupPort("P2.value") == kInvalidPort);   // expose:false on the <use>.
    CHECK(ui.doc->lookupPort("P1.secret") == kInvalidPort);  // expose="false" on the element.
    CHECK(ui.doc->lookupPort("P1/Hidden.secret") != kInvalidPort);
    CHECK(ui.doc->lookupPort("value") == ui.doc->lookupPort("Doc.value"));  // Document level.
    ui.doc->setPort(p1, Value::ofFloat(3.4f));
    ui.doc->setPort(p2path, Value::ofFloat(8));
    CHECK(ui.text("P1/Fill") == "3");
    CHECK(ui.text("P2/Fill") == "8");
    CHECK(ui.text("Doc") == "0");
    PortRecord info = ui.doc->portInfo(p1);
    CHECK(!info.documentLevel);
    CHECK(info.exposedName == "P1.value");
    CHECK(info.qualifiedName == "P1/Fill.value");
}

TEST_CASE("bindings: BIND layout, converter numbering and IR validation") {
    CompileResult r = compileSrc(page("", R"X(<div id="P" port:hp="number" port:hp_max="number"></div>
        <span id="T" bind:text="format('{0}/{1}', hp, hp_max)"></span>
        <div id="B" bind:width="hp | range(0, hp_max, 0%, 100%) | tween(0.2s, ease-in)"></div>)X"));
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const BindingChunk& bc = view.value().bindings();
    REQUIRE(bc.bindings().size() == 2);
    const IrBinding& text = bc.bindings()[0];
    CHECK(static_cast<BindTarget>(text.target) == BindTarget::Text);
    CHECK(text.sourceCount == 2);
    CHECK(bc.sources()[text.sourceOffset] == 0u);  // hp first (the input), then hp_max.
    CHECK(bc.sources()[text.sourceOffset + 1] == 1u);
    CHECK(bc.steps()[text.stepOffset].op == static_cast<uint8_t>(ConverterOp::Format));
    CHECK(converterArgKind(bc.steps()[text.stepOffset], 0) == ConverterArg::String);
    CHECK(converterArgKind(bc.steps()[text.stepOffset], 1) == ConverterArg::Port);
    const IrBinding& width = bc.bindings()[1];
    CHECK(static_cast<BindTarget>(width.target) == BindTarget::Prop);
    CHECK(width.targetId == prop::Width);
    const IrConverterStep& range = bc.steps()[width.stepOffset];
    CHECK(range.op == 1);
    CHECK(range.argCount == 4);
    CHECK(converterArgKind(range, 1) == ConverterArg::Port);
    CHECK(converterArgKind(range, 3) == ConverterArg::Percent);
    CHECK(bc.steps()[width.stepOffset + 1].op == 31);       // tween
    CHECK(bc.steps()[width.stepOffset + 1].argCount == 6);  // seconds, easing x5
    CHECK(bc.steps()[width.stepOffset + 2].op == static_cast<uint8_t>(ConverterOp::Operands));
    CHECK(static_cast<uint8_t>(ConverterOp::ElemProp) == 36);
    CHECK(static_cast<uint8_t>(ConverterOp::Not) == 14);
    CHECK(static_cast<uint8_t>(ConverterOp::Lt) == 7);  // T-174's numbering, unchanged.
    IrWriter again = IrWriter::fromView(view.value());
    CHECK(again.finish() == r.ir);

    // A source past the PORT table is rejected by IrView::open.
    IrWriter bad = IrWriter::fromView(view.value());
    BindingBuilder bb;
    bb.bindings.assign(bc.bindings().begin(), bc.bindings().end());
    bb.sources.assign(bc.sources().begin(), bc.sources().end());
    bb.sources[0] = 99;
    bb.steps.assign(bc.steps().begin(), bc.steps().end());
    bad.setBindings(bb);
    std::vector<uint8_t> blob = bad.finish();
    CHECK(!IrView::open(IrBlob{blob.data(), blob.size()}).ok());
}

TEST_CASE("bindings: bind:checked follows a bool port without firing check triggers") {
    // T-204 (05 §5.9.1, 30 §30.11 gap 1): the host's value moves the checkbox; the user's click
    // still goes through on:check -> set(value, true).
    CompileResult r =
        compileSrc(page("#C { width: 20px; height: 20px; }", R"X(<div id="Box" port:value="bool in">
        <input type="checkbox" id="C" bind:checked="value"
               on:check="class(fired); set(value, true)" on:uncheck="set(value, false)"/></div>)X"));
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
    REQUIRE(h.ok());
    Document& doc = *ctx.document(h.value());
    ctx.update(0.0f);
    ElementRef c = doc.byId("C");
    REQUIRE(c.valid());
    const PortId value = doc.lookupPort("value");
    REQUIRE(value != kInvalidPort);
    CHECK_FALSE(c.hasState(kStateChecked));

    REQUIRE(doc.setPort(value, Value::ofFloat(1)) == Status::kOk);
    ctx.update(0.016f);
    CHECK(c.hasState(kStateChecked));
    CHECK_FALSE(c.hasClass("fired"));  // No on:check.
    REQUIRE(doc.setPort(value, Value::ofFloat(0)) == Status::kOk);
    ctx.update(0.032f);
    CHECK_FALSE(c.hasState(kStateChecked));

    // A click checks it, fires on:check and writes the port back; the binding then agrees.
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{10, 10};
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ctx.pushInput(down);
    ctx.pushInput(up);
    ctx.update(0.048f);
    CHECK(c.hasState(kStateChecked));
    CHECK(c.hasClass("fired"));
    CHECK(doc.getPort(value).f == 1.0f);
    ctx.update(0.064f);
    CHECK(c.hasState(kStateChecked));
}

TEST_CASE("bindings: bind:checked on anything but a checkbox input is a compile error") {
    CHECK(compileError(compileSrc(page("", R"X(<div port:on="bool" bind:checked="on"></div>)X")),
                       "binds only on an <input type=\"checkbox\">"));
    CHECK(compileError(
        compileSrc(page("", R"X(<div port:on="bool"><input type="text" bind:checked="on"/></div>)X")),
        "binds only on an <input type=\"checkbox\">"));
}

// -- T-358: port default values (27 §27.3.1, 02 §2.s.2, IR minor 7) ------------------------------

namespace {

const IrPortDefault* defaultOf(const PortChunk& pc, uint32_t port) {
    for (const IrPortDefault& d : pc.defaults()) {
        if (d.port == port) {
            return &d;
        }
    }
    return nullptr;
}

float defaultFloat(const IrPortDefault& d, size_t i) {
    float f[3] = {0, 0, 0};
    std::memcpy(f, d.value, sizeof(f));
    return f[i];
}

uint32_t defaultU32(const IrPortDefault& d) {
    uint32_t u = 0;
    std::memcpy(&u, d.value, sizeof(u));
    return u;
}

}  // namespace

TEST_CASE("T-358: port defaults of every allowed type compile into the PORT defaults[] tail") {
    CompileResult r = compileSrc(page("", R"X(
        <div id="Hud" port:hp="number in = 80" port:alive="bool = true" port:tint="color in = #ff800040"
             port:tint2="color = rgb(0, 128, 255)" port:who="string in = &quot;Guest&quot;"
             port:pos="vec2 = 1 2" port:dir="vec3 out = 1 2 3" port:mode="enum(low|mid|high) in = mid"
             port:plain="number in" port:hit="event out"></div>)X"));
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    IrHeader header{};
    std::memcpy(&header, r.ir.data(), sizeof(header));
    CHECK(header.minor >= 7);  // T-405: 8.
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const PortChunk& pc = view.value().ports();
    REQUIRE(pc.ports().size() == 10);
    CHECK(pc.defaults().size() == 8);
    CHECK(static_cast<PortDir>(pc.ports()[0].dir) == PortDir::In);
    CHECK(static_cast<PortDir>(pc.ports()[6].dir) == PortDir::Out);
    const IrPortDefault* hp = defaultOf(pc, 0);
    REQUIRE(hp != nullptr);
    CHECK(static_cast<PortValueType>(hp->type) == PortValueType::Number);
    CHECK(defaultFloat(*hp, 0) == 80.0f);
    REQUIRE(defaultOf(pc, 1) != nullptr);
    CHECK(defaultOf(pc, 1)->value[0] == 1);
    const IrPortDefault* tint = defaultOf(pc, 2);
    REQUIRE(tint != nullptr);
    CHECK(tint->value[0] == 0xff);
    CHECK(tint->value[1] == 0x80);
    CHECK(tint->value[2] == 0x00);
    CHECK(tint->value[3] == 0x40);
    const IrPortDefault* tint2 = defaultOf(pc, 3);
    REQUIRE(tint2 != nullptr);
    CHECK(tint2->value[1] == 128);
    CHECK(tint2->value[2] == 255);
    CHECK(tint2->value[3] == 255);
    REQUIRE(defaultOf(pc, 4) != nullptr);
    CHECK(view.value().string(defaultU32(*defaultOf(pc, 4))) == "Guest");
    REQUIRE(defaultOf(pc, 5) != nullptr);
    CHECK(defaultFloat(*defaultOf(pc, 5), 1) == 2.0f);
    REQUIRE(defaultOf(pc, 6) != nullptr);
    CHECK(defaultFloat(*defaultOf(pc, 6), 2) == 3.0f);
    REQUIRE(defaultOf(pc, 7) != nullptr);
    CHECK(view.value().string(defaultU32(*defaultOf(pc, 7))) == "mid");
    CHECK(defaultOf(pc, 8) == nullptr);  // No `=`: no default.
    CHECK(defaultOf(pc, 9) == nullptr);

    // Round trip: the writer rebuilt from the view writes the same bytes.
    CHECK(IrWriter::fromView(view.value()).finish() == r.ir);
}

TEST_CASE("T-358: a bad port default is a diagnostic") {
    CHECK(
        compileError(compileSrc(page("", R"X(<div port:hp="number = abc"></div>)X")), "port 'hp': default"));
    CHECK(
        compileError(compileSrc(page("", R"X(<div port:on="bool in = yes"></div>)X")), "port 'on': default"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:m="enum(a|b) = c"></div>)X")), "port 'm': default"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:p="vec2 = 1"></div>)X")), "port 'p': default"));
    CHECK(
        compileError(compileSrc(page("", R"X(<div port:s="string = Guest"></div>)X")), "port 's': default"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:c="color = red"></div>)X")), "port 'c': default"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:e="event out = 1"></div>)X")), "port 'e': default"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:t="texture = 1"></div>)X")), "port 't': default"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:n="number ="></div>)X")), "port 'n': default"));
    std::string error;
    CHECK(validatePortDeclaration("number in = 80", error));
    CHECK(validatePortDeclaration("enum(a|b) out = b", error));
    CHECK_FALSE(validatePortDeclaration("enum(a|b) out = c", error));
    CHECK_FALSE(validatePortDeclaration("bool = 2", error));
}

TEST_CASE("T-358: ports start at their default; a host set overrides it; bound text shows it") {
    Ui ui = open(page("", R"X(
        <div id="P" port:x="number in = 80" port:who="string = &quot;Guest&quot;" port:on="bool = true"
             port:mode="enum(low|mid|high) = high" port:tint="color = #ff0000" port:pos="vec2 = 3 4"></div>
        <span id="T" bind:text="x"></span><span id="W" bind:text="who"></span>)X"));
    CHECK(ui.text("T") == "80");
    CHECK(ui.text("W") == "Guest");
    CHECK(ui.doc->getPort(ui.port("x")).f == 80.0f);
    CHECK(ui.doc->portText(ui.port("who")) == "Guest");
    CHECK(ui.doc->portText(ui.port("mode")) == "high");
    CHECK(ui.doc->getPort(ui.port("on")).f == 1.0f);
    const Value tint = ui.doc->getPort(ui.port("tint"));
    REQUIRE(tint.type == ValueType::Color);
    CHECK(tint.c.r == doctest::Approx(1.0f));
    CHECK(tint.c.a == doctest::Approx(1.0f));
    const Value pos = ui.doc->getPort(ui.port("pos"));
    REQUIRE(pos.type == ValueType::Vec2);
    CHECK(pos.v2.y == 4.0f);
    ui.set("x", 12);
    ui.step(0.016f);
    CHECK(ui.text("T") == "12");
    CHECK(ui.doc->getPort(ui.port("x")).f == 12.0f);
}

TEST_CASE("T-358: an IR minor 6 PORT chunk without the defaults tail opens with no defaults") {
    CompileResult r =
        compileSrc(page("", R"X(<div port:hp="number in"></div><span id="T" bind:text="hp"></span>)X"));
    REQUIRE(r.ok());
    std::vector<uint8_t> bytes = r.ir;
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.minor = 6;
    std::memcpy(bytes.data(), &header, sizeof(header));
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(view.ok());
    CHECK(view.value().ports().ports().size() == 1);
    CHECK(view.value().ports().defaults().empty());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{bytes.data(), bytes.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->update(0.0f);
    CHECK(doc->getPort(doc->lookupPort("hp")).f == 0.0f);
}

TEST_CASE("T-358: IrView rejects a default with a bad port index, type or enum value") {
    CompileResult r = compileSrc(page("", R"X(<div port:m="enum(a|b) = b" port:hp="number = 1"></div>)X"));
    REQUIRE(r.ok());
    Result<IrView> good = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(good.ok());
    REQUIRE(good.value().ports().defaults().size() == 2);
    const size_t at = static_cast<size_t>(
        reinterpret_cast<const uint8_t*>(good.value().ports().defaults().data()) - r.ir.data());
    auto opens = [&](auto mutate) {
        std::vector<uint8_t> bytes = r.ir;
        IrPortDefault d{};
        std::memcpy(&d, bytes.data() + at, sizeof(d));
        mutate(d);
        std::memcpy(bytes.data() + at, &d, sizeof(d));
        return IrView::open(IrBlob{bytes.data(), bytes.size()}).ok();
    };
    CHECK(opens([](IrPortDefault&) {}));
    CHECK_FALSE(opens([](IrPortDefault& d) { d.port = 7; }));
    CHECK_FALSE(opens([](IrPortDefault& d) { d.type = static_cast<uint8_t>(PortValueType::Number); }));
    CHECK_FALSE(opens([&](IrPortDefault& d) {
        const uint32_t notAValue = good.value().ports().ports()[1].name;  // "hp"
        std::memcpy(d.value, &notAValue, sizeof(notAValue));
    }));
}

// -- T-385: literal port values and state= on <use>, bind:disabled / bind:display (27 §27.3.2) ----

namespace {

const char* kLiteralSymbol = R"X(<symbol name="X" state-groups="Drag: Idle|Dragging">
      <span id="T" port:icon="string in" bind:text="icon" port:n="number in" port:on="bool in"
            port:mode="enum(a|b|c) in" port:tags="list<string> in" port:pos="struct{x: number} in"
            port:done="number out" port:ping="event"></span>
      <span id="Second" bind:text="tags | item(1)"></span></symbol>)X";

std::string literalPage(std::string_view use) {
    return std::string("<xt><head>") + kLiteralSymbol +
           "<style>symbol(X) { :state(Dragging) #T { width: 33px; } }</style></head><body id=\"Root\">" +
           std::string(use) + "</body></xt>";
}

CompileResult compileStrict(const std::string& xt, bool strict) {
    CompileOptions options;
    options.emitMeta = false;
    options.strict = strict;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasWarning(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("T-385: a literal attribute on <use> is the instance port's initial value, by port type") {
    Ui ui = open(literalPage(R"X(<use symbol="X" id="U" icon="prev" n="3" on="true" mode="c"
        tags='["a", "b"]' pos='{"x": 4}'></use><use symbol="X" id="V"></use>)X"));
    CHECK(ui.doc->portText(ui.port("U.icon")) == "prev");
    CHECK(ui.text("U/T") == "prev");
    CHECK(ui.doc->getPort(ui.port("U.n")).f == doctest::Approx(3));
    CHECK(ui.doc->portText(ui.port("U.on")) == "true");
    CHECK(ui.doc->portText(ui.port("U.mode")) == "c");
    CHECK(ui.text("U/Second") == "b");
    const Value pos = ui.doc->getPort(ui.port("U.pos"));
    REQUIRE(pos.type == ValueType::Struct);
    CHECK(pos.items[0].f == doctest::Approx(4));
    // Another instance keeps the declared (empty) values.
    CHECK(ui.doc->portText(ui.port("V.icon")) == "");
    CHECK(ui.doc->getPort(ui.port("V.n")).f == 0.0f);
    // The literal is a port value, not an element attribute of the instance root.
    CHECK(ui.el("U").attribute("icon").empty());
}

TEST_CASE("T-385: literal port value errors and the strict unknown-attribute warning") {
    CHECK(compileError(compileSrc(literalPage(R"X(<div port:s="string"></div>
        <use symbol="X" id="U" icon="prev" bind:icon="s"></use>)X")),
                       "a literal value and bind:icon cannot both be given"));
    CHECK(compileError(compileSrc(literalPage(R"X(<use symbol="X" id="U" done="1"></use>)X")),
                       "a literal value cannot set an out port or an event port"));
    CHECK(compileError(compileSrc(literalPage(R"X(<use symbol="X" id="U" ping="1"></use>)X")),
                       "a literal value cannot set an out port or an event port"));
    CHECK(compileError(compileSrc(literalPage(R"X(<use symbol="X" id="U" n="many"></use>)X")),
                       "attribute \"n\" on <use symbol=\"X\">"));
    CHECK(compileError(compileSrc(literalPage(R"X(<use symbol="X" id="U" mode="z"></use>)X")),
                       "attribute \"mode\" on <use symbol=\"X\">"));
    const std::string unknown = literalPage(R"X(<use symbol="X" id="U" iconn="prev" title="t"></use>)X");
    const CompileResult loose = compileStrict(unknown, false);
    CHECK(loose.ok());
    CHECK_FALSE(hasWarning(loose, "unknown attribute"));
    const CompileResult strict = compileStrict(unknown, true);
    CHECK_FALSE(strict.ok());
    CHECK(hasWarning(strict, "unknown attribute \"iconn\" on <use symbol=\"X\">: not a port of X"));
    CHECK_FALSE(hasWarning(strict, "unknown attribute \"title\""));  // A standard attribute.
}

TEST_CASE("T-385: state= on <use> seeds the instance's custom states") {
    Ui ui =
        open(literalPage(R"X(<use symbol="X" id="U" state="Dragging"></use><use symbol="X" id="V"></use>)X"));
    CHECK(ui.doc->hasCustomState(ui.el("U"), "Dragging"));
    CHECK_FALSE(ui.doc->hasCustomState(ui.el("V"), "Dragging"));
    CHECK(ui.el("U/T").boundingBox().w == doctest::Approx(33));
    CHECK(ui.el("V/T").boundingBox().w != doctest::Approx(33));
    CHECK(compileError(compileSrc(literalPage(R"X(<use symbol="X" id="U" state="Flying"></use>)X")),
                       "unknown state \"Flying\" in state="));
    CHECK(compileError(compileSrc(literalPage(R"X(<use symbol="X" id="U" state="Hover"></use>)X")),
                       "is a built-in state"));
}

TEST_CASE("T-385: bind:disabled sets and clears the Disabled state; bind:state.Disabled is the same") {
    for (const char* attr : {"bind:disabled", "bind:state.Disabled"}) {
        CAPTURE(attr);
        Ui ui = open(page("#B { width: 50px; height: 20px; } #B:disabled { opacity: 0.5; }",
                          std::string(R"X(<div id="P" port:flag="bool in"></div><button id="B" )X") + attr +
                              R"X(="flag" on:click="class(clicked)"></button>)X"));
        CHECK(ui.doc->ir().customStates().names().empty());  // No custom state named Disabled.
        ElementRef b = ui.el("B");
        CHECK_FALSE(b.hasState(kStateDisabled));
        auto click = [&] {
            InputEvent down;
            down.kind = InputEvent::PointerDown;
            down.pos = Vec2{10, 10};
            ui.doc->dispatchInput(down);
            InputEvent up = down;
            up.kind = InputEvent::PointerUp;
            ui.doc->dispatchInput(up);
        };
        ui.set("flag", 1);
        ui.step(0.016f);
        CHECK(b.hasState(kStateDisabled));
        CHECK(b.property(prop::Opacity).f == doctest::Approx(0.5f));
        click();
        CHECK_FALSE(b.hasClass("clicked"));
        ui.set("flag", 0);
        ui.step(0.016f);
        CHECK_FALSE(b.hasState(kStateDisabled));
        CHECK(b.property(prop::Opacity).f == doctest::Approx(1.0f));
        click();
        CHECK(b.hasClass("clicked"));
    }
}

TEST_CASE("T-385: bind:display removes the element from layout and restores the style sheet's value") {
    Ui ui = open(page(".row { display: flex; height: 20px; } #B { height: 20px; }",
                      R"X(<div id="P" port:flag="bool in"></div>
        <div id="A" class="row" bind:display="flag"></div><div id="B"></div>)X"));
    CHECK(ui.el("B").boundingBox().y == doctest::Approx(0));  // flag starts false: A takes no room.
    ui.set("flag", 1);
    ui.step(0.016f);
    CHECK(ui.el("B").boundingBox().y == doctest::Approx(20));
    ui.set("flag", 0);
    ui.step(0.016f);
    CHECK(ui.el("B").boundingBox().y == doctest::Approx(0));
    // The older select() form still writes the display value itself.
    Ui old = open(page("#A, #B { height: 20px; }", R"X(<div id="P" port:flag="bool in"></div>
        <div id="A" bind:display="flag | select(true: block, false: none)"></div><div id="B"></div>)X"));
    CHECK(old.el("B").boundingBox().y == doctest::Approx(0));
    old.set("flag", 1);
    old.step(0.016f);
    CHECK(old.el("B").boundingBox().y == doctest::Approx(20));
}

// -- T-390 (27.3.2): Disabled is the built-in state everywhere; <use> port names ignore case -----

TEST_CASE("T-390: bind:state.Disabled with a :state(Disabled) rule is the built-in state, no warning") {
    const std::string xt =
        std::string("<xt><head>") +
        R"X(<symbol name="S" requires="states: Disabled" state-groups="Mode: Idle|Disabled">
          <div id="R" port:off="bool in" bind:state.Disabled="off"><span id="T"></span></div></symbol>)X" +
        "<style>symbol(S) { :state(Disabled) #T { width: 33px; } }"
        " #B:state(Disabled) { opacity: 0.5; } #B { width: 50px; height: 20px; }</style>"
        "</head><body id=\"Root\">" +
        R"X(<use symbol="S" id="U"></use><button id="B"></button>
          <button id="C" on:click="state(Disabled, toggle, target=#B)"></button>)X" +
        "</body></xt>";
    Ui ui = open(xt);
    for (const Diagnostic& d : ui.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Warning);
    }
    // Only Idle is a custom state: Disabled in state-groups makes no slot.
    std::span<const uint32_t> names = ui.doc->ir().customStates().names();
    REQUIRE(names.size() == 1);
    CHECK(ui.doc->ir().string(names[0]) == "Idle");

    CHECK(ui.el("U/T").boundingBox().w != doctest::Approx(33));
    ui.set("U.off", 1);
    ui.step(0.016f);
    CHECK(ui.el("U/R").hasState(kStateDisabled));
    CHECK(ui.doc->hasCustomState(ui.el("U/R"), "Disabled"));
    CHECK(ui.el("U/T").boundingBox().w == doctest::Approx(33));

    // setCustomState("Disabled") sets the built-in bit, so :disabled / :state(Disabled) rules match.
    ElementRef b = ui.el("B");
    CHECK(ui.doc->setCustomState(b, "Disabled", true) == Status::kOk);
    ui.step(0.016f);
    CHECK(b.hasState(kStateDisabled));
    CHECK(ui.doc->hasCustomState(b, "Disabled"));
    CHECK(b.property(prop::Opacity).f == doctest::Approx(0.5f));
    CHECK(ui.doc->setCustomState(b, "Disabled", false) == Status::kOk);
    ui.step(0.016f);
    CHECK_FALSE(b.hasState(kStateDisabled));
    CHECK_FALSE(ui.doc->hasCustomState(b, "Disabled"));

    // The state(Disabled, ...) action.
    ui.doc->fireTrigger(ui.el("C"), TriggerEvent::Click);
    ui.step(0.016f);
    CHECK(b.hasState(kStateDisabled));
    ui.doc->fireTrigger(ui.el("C"), TriggerEvent::Click);
    ui.step(0.016f);
    CHECK_FALSE(b.hasState(kStateDisabled));
}

TEST_CASE("T-390: <use> attributes match port names without regard to case") {
    const char* symbol = R"X(<symbol name="Y"><span id="T" port:hasAdv="bool in" port:Label="string in"
        bind:text="Label"></span></symbol>)X";
    const std::string xt = std::string("<xt><head>") + symbol + "</head><body id=\"Root\">" +
                           R"X(<use symbol="Y" id="U" hasAdv="true" label="go"></use>)X" + "</body></xt>";
    const CompileResult strict = compileStrict(xt, true);
    CHECK(strict.ok());
    CHECK_FALSE(hasWarning(strict, "unknown attribute"));
    Ui ui = open(xt);
    CHECK(ui.doc->portText(ui.port("U.hasAdv")) == "true");
    CHECK(ui.text("U/T") == "go");
    CHECK(ui.el("U").attribute("hasadv").empty());

    const std::string unknown = std::string("<xt><head>") + symbol + "</head><body id=\"Root\">" +
                                R"X(<use symbol="Y" id="U" hasAdvx="true"></use>)X" + "</body></xt>";
    CHECK(hasWarning(compileStrict(unknown, true), "(attribute names are case-insensitive)"));

    const std::string twin =
        std::string("<xt><head>") +
        R"X(<symbol name="Z"><span id="T" port:a="number in" port:A="number in"></span></symbol>)X" +
        "</head><body id=\"Root\">" + R"X(<use symbol="Z" id="U" a="1"></use>)X" + "</body></xt>";
    CHECK(compileError(compileSrc(twin), "ambiguous"));
}

// -- T-392 (27 §27.3.5): <input> bind:value / on:change / on:input / value, built-in aliases ----

namespace {

InputEvent typed(const char* utf8) {
    InputEvent ev;
    ev.kind = InputEvent::TextInput;
    const std::string s(utf8);
    REQUIRE(s.size() < sizeof(ev.utf8));
    std::memcpy(ev.utf8, s.data(), s.size());
    ev.utf8Len = static_cast<uint8_t>(s.size());
    return ev;
}

}  // namespace

TEST_CASE("T-392: <input bind:value> shows the port; on:change / on:input set ports from value") {
    Ui ui = open(page("", R"X(<div id="P" port:q="string" port:last="string" port:search="event out">
        <input id="In" bind:value="q" on:change="set(q, value); emit(search)"
               on:input="set(last, value)"/></div>)X"));
    std::vector<std::string> emits;
    ui.doc->setHostEventListener([&emits](const HostEvent& e) {
        if (e.kind == HostEventKind::Emit) {
            emits.emplace_back(e.name);
        }
    });
    // Port -> text, no Change (so no on:change, no emit).
    ui.setText("q", "hello");
    ui.step(0.016f);
    CHECK(ui.el("In").value() == "hello");
    CHECK(emits.empty());
    ui.setText("q", "");
    ui.step(0.016f);
    CHECK(ui.el("In").value().empty());
    CHECK(emits.empty());
    // Every edit: Change -> on:change (port + emit) and on:input.
    ui.doc->setFocus(ui.el("In"));
    ui.doc->dispatchInput(typed("a"));
    ui.step(0.016f);
    CHECK(ui.doc->portText(ui.port("q")) == "a");
    CHECK(ui.doc->portText(ui.port("last")) == "a");
    REQUIRE(emits.size() == 1);
    CHECK(emits[0] == "search");
    ui.doc->dispatchInput(typed("b"));
    ui.step(0.016f);
    CHECK(ui.doc->portText(ui.port("q")) == "ab");
    CHECK(ui.doc->portText(ui.port("last")) == "ab");
    CHECK(emits.size() == 2);
    CHECK(ui.el("In").value() == "ab");
}

TEST_CASE("T-392: value outside change / input and bind:value off an editable input are errors") {
    CHECK(compileError(
        compileSrc(page("", R"X(<div port:q="string"><input on:click="set(q, value)"/></div>)X")),
        "value is only valid in change and input triggers"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:q="string"><span bind:value="q"></span></div>)X")),
                       "editable <input>"));
    CHECK(compileError(
        compileSrc(page("", R"X(<div port:q="string"><input type="checkbox" bind:value="q"/></div>)X")),
        "editable <input>"));
}

TEST_CASE("T-392: focus() without an argument focuses the element; on:state-enter(Open)") {
    Ui ui = open(page("", R"X(<input id="A"/>
        <input id="In" state-groups="Mode: Open|Closed" on:state-enter(Open)="focus()"
               on:state-exit(Open)="focus(#A)"/>)X"));
    CHECK(ui.doc->focus().node != ui.el("In").node);
    REQUIRE(ui.doc->setCustomState(ui.el("In"), "Open", true) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.doc->focus().node == ui.el("In").node);
    REQUIRE(ui.doc->setCustomState(ui.el("In"), "Closed", true) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.doc->focus().node == ui.el("A").node);
}

TEST_CASE("T-392: Focused / Hover / Active / Checked are the built-in states in every position") {
    const std::string xt = page(
        "#A, #H, #K, #X { width: 10px; height: 10px; } #A:focus { width: 31px; }"
        " #H:state(Hover) { width: 32px; } #K:active { width: 33px; } #X:state(Checked) { width: 34px; }",
        R"X(<div id="P" port:f="bool" state-groups="Mode: Idle|Focused">
          <div id="A" bind:state.Focused="f"></div><div id="H"></div><div id="K"></div><div id="X"></div>
          <button id="C" on:click="state(Active, toggle, target=#K)"></button></div>)X");
    Ui ui = open(xt);
    for (const Diagnostic& d : ui.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Warning);
    }
    // Only Idle is a custom state.
    std::span<const uint32_t> names = ui.doc->ir().customStates().names();
    REQUIRE(names.size() == 1);
    CHECK(ui.doc->ir().string(names[0]) == "Idle");

    // bind:state.Focused sets :focus.
    CHECK(ui.el("A").boundingBox().w == doctest::Approx(10));
    ui.set("f", 1);
    ui.step(0.016f);
    CHECK(ui.el("A").hasState(kStateFocus));
    CHECK(ui.doc->hasCustomState(ui.el("A"), "Focused"));
    CHECK(ui.el("A").boundingBox().w == doctest::Approx(31));
    ui.set("f", 0);
    ui.step(0.016f);
    CHECK_FALSE(ui.el("A").hasState(kStateFocus));
    CHECK(ui.el("A").boundingBox().w == doctest::Approx(10));

    // setCustomState("Hover") and a :state(Hover) rule.
    REQUIRE(ui.doc->setCustomState(ui.el("H"), "Hover", true) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.el("H").hasState(kStateHover));
    CHECK(ui.el("H").boundingBox().w == doctest::Approx(32));

    // state(Active, toggle, target=#K).
    ui.doc->fireTrigger(ui.el("C"), TriggerEvent::Click);
    ui.step(0.016f);
    CHECK(ui.el("K").hasState(kStateActive));
    CHECK(ui.el("K").boundingBox().w == doctest::Approx(33));

    // setCustomState("Checked") on any element and :state(Checked).
    REQUIRE(ui.doc->setCustomState(ui.el("X"), "Checked", true) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.el("X").hasState(kStateChecked));
    CHECK(ui.el("X").boundingBox().w == doctest::Approx(34));
}

// T-389 (27 §27.4.1, 2026-10-21 ruling (b)): a port set 100 times in one frame evaluates the
// binding that reads it once, from the next update(); getPort() sees each value at once.
TEST_CASE("T-389: 100 sets of a port in one frame evaluate its binding once") {
    Ui ui = open(page("", R"X(<div id="P" port:hp="number"></div>
        <span id="T" bind:text="hp | host.count"></span>)X"),
                 kBehaviorFlowSignalCoalescing);
    int calls = 0;
    REQUIRE(ui.doc
                ->registerConverter("count",
                                    [&calls](const Value& in, std::span<const Value>) {
                                        ++calls;
                                        return in;
                                    })
                .ok());
    ui.step(0.016f);
    calls = 0;
    const PortId hp = ui.port("hp");
    for (int i = 1; i <= 100; ++i) {
        REQUIRE(ui.doc->setPort(hp, Value::ofFloat(static_cast<float>(i))) == Status::kOk);
        CHECK(ui.doc->getPort(hp).f == doctest::Approx(static_cast<float>(i)));
    }
    CHECK(calls == 0);
    ui.step(0.016f);
    CHECK(calls == 1);
    CHECK(ui.text("T") == "100");
    ui.step(0.016f);  // Nothing changed: not evaluated again.
    CHECK(calls == 1);
}

// -- T-399 (08 §8.18): focus validation after layout (XF-101), bind:value re-apply (XF-100) ------

TEST_CASE("T-399: an input a port change shows takes focus() in the same frame; a hidden one loses it") {
    Ui ui =
        open(page("#Box { display: none; } #Box.on { display: block; } input { width: 80px; height: 20px; }",
                  R"X(<div id="P" port:show="bool" port:other="string">
        <div id="Box" bind:class.on="show"><input id="In"/></div><span bind:text="other"></span></div>)X"),
             kBehaviorFlowSignalCoalescing);  // T-407: "a port change is waiting" needs behavior 1.
    std::vector<std::string> events;
    const ElementRef in = ui.el("In");
    ui.doc->addListener(in, EventType::Focus, [&](Event&) { events.emplace_back("focus"); });
    ui.doc->addListener(in, EventType::Blur, [&](Event&) { events.emplace_back("blur"); });
    // No pending change can show it: refused at once (T-397).
    CHECK(ui.el("In").focus() == Status::kUnsupported);
    // The port change shows it this frame: focus() is accepted and lands after layout.
    ui.setText("show", "true");
    CHECK(ui.el("In").focus() == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.doc->focus().node == in.node);
    CHECK(ui.el("In").hasState(kStateFocus));
    ui.step(0.016f);
    CHECK(ui.doc->focus().node == in.node);
    CHECK(events == std::vector<std::string>{"focus"});
    // Really display: none -> the focus is cleared (Blur).
    ui.setText("show", "false");
    ui.step(0.016f);
    CHECK_FALSE(ui.doc->focus().valid());
    CHECK(events == std::vector<std::string>{"focus", "blur"});
    // Document::setFocus on an element the same frame's port change shows keeps it too.
    ui.setText("show", "true");
    ui.doc->setFocus(ui.el("In"));
    ui.step(0.016f);
    ui.step(0.016f);
    CHECK(ui.doc->focus().node == in.node);
    CHECK(events == std::vector<std::string>{"focus", "blur", "focus"});
    // A pending focus (a port change is waiting) whose element stays hidden: the focus does not
    // move, reported once.
    ui.setText("show", "false");
    ui.step(0.016f);
    ui.setText("other", "x");
    CHECK(ui.el("In").focus() == Status::kOk);
    ui.step(0.016f);
    CHECK_FALSE(ui.doc->focus().valid());
    bool warned = false;
    for (const std::string& w : ui.doc->warnings()) {
        warned = warned || w.find("still display:none after layout") != std::string::npos;
    }
    CHECK(warned);
}

TEST_CASE("T-399: bind:value is applied again when shown, by reset(value) and after restoreState") {
    Ui ui = open(page("#Box.off { display: none; } input { width: 80px; height: 20px; }",
                      R"X(<div id="P" port:q="string" port:hide="bool">
        <div id="Box" bind:class.off="hide"><input id="In" bind:value="q"/></div>
        <input id="R" bind:value="q" on:blur="reset(value)"/></div>)X"));
    ui.setText("q", "port");
    ui.step(0.016f);
    CHECK(ui.el("In").value() == "port");
    // The user's edit stays while the port does not change ...
    ui.doc->setFocus(ui.el("In"));
    ui.doc->dispatchInput(typed("x"));
    ui.step(0.016f);
    CHECK(ui.el("In").value() == "portx");
    CHECK(ui.doc->portText(ui.port("q")) == "port");
    // ... until the input is hidden (an ancestor display: none) and shown again.
    ui.setText("hide", "true");
    ui.step(0.016f);
    CHECK(ui.el("In").value() == "portx");  // Hidden: untouched.
    ui.setText("hide", "false");
    ui.step(0.016f);
    CHECK(ui.el("In").value() == "port");

    // reset(value): here on blur.
    ui.doc->setFocus(ui.el("R"));
    ui.doc->dispatchInput(typed("y"));
    ui.step(0.016f);
    CHECK(ui.el("R").value() == "porty");
    ui.doc->setFocus(ElementRef{});
    ui.step(0.016f);
    CHECK(ui.el("R").value() == "port");

    // restoreState: the snapshot's leftover edit gives way to the port value.
    ui.doc->setFocus(ui.el("In"));
    ui.doc->dispatchInput(typed("z"));
    ui.step(0.016f);
    CHECK(ui.el("In").value() == "portz");
    const std::string snap = ui.doc->snapshotState();
    REQUIRE(ui.doc->restoreState(snap) == Status::kOk);
    CHECK(ui.el("In").value() == "port");
}

TEST_CASE("T-399: reset() takes value and needs bind:value on the element") {
    CHECK(
        compileError(compileSrc(page("", R"X(<div port:q="string"><input on:blur="reset(value)"/></div>)X")),
                     "reset(value) needs bind:value on the element"));
    CHECK(compileError(
        compileSrc(page("", R"X(<div port:q="string"><input bind:value="q" on:blur="reset(text)"/></div>)X")),
        "reset() takes value"));
    CHECK(compileError(
        compileSrc(page("", R"X(<div port:q="string"><input bind:value="q" on:blur="reset()"/></div>)X")),
        "reset() takes value"));
}

// T-407 (43 §43.3): behavior 0 keeps the pre-T-389 binding evaluation -- at once, on every set.
TEST_CASE("T-407: behavior 0 evaluates a binding on each of 100 sets, at once") {
    Ui ui = open(page("", R"X(<div id="P" port:hp="number"></div>
        <span id="T" bind:text="hp | host.count"></span>)X"),
                 0);
    REQUIRE(ui.doc->behavior() == 0);
    int calls = 0;
    REQUIRE(ui.doc
                ->registerConverter("count",
                                    [&calls](const Value& in, std::span<const Value>) {
                                        ++calls;
                                        return in;
                                    })
                .ok());
    ui.step(0.016f);
    calls = 0;
    const PortId hp = ui.port("hp");
    for (int i = 1; i <= 100; ++i) {
        REQUIRE(ui.doc->setPort(hp, Value::ofFloat(static_cast<float>(i))) == Status::kOk);
        CHECK(calls == i);
        CHECK(std::string(ui.el("T").text()) == std::to_string(i));  // No update() in between.
    }
    ui.step(0.016f);
    CHECK(calls == 100);
}

// The pre-T-389 expectation of "bindings: variant on a symbol instance swaps its v-<Prop>-* class":
// the class follows the port at once, no update() in between.
TEST_CASE("T-407: behavior 0 swaps a bound variant class within the set") {
    Ui ui = open(R"X(<xt><head>
      <symbol name="Btn" variants="Kind=Primary|Secondary"><div id="Bg"></div></symbol>
      <style>#Root { width: 100px; }</style></head>
      <body id="Root"><div id="P" port:kind="enum(Primary|Secondary)"></div>
      <use symbol="Btn" id="B1" variant:Kind="Primary" bind:variant.Kind="kind"></use></body></xt>)X");
    CHECK(ui.el("B1").hasClass("v-Kind-Primary"));
    ui.setText("kind", "Secondary");
    CHECK(ui.el("B1").hasClass("v-Kind-Secondary"));
    CHECK(!ui.el("B1").hasClass("v-Kind-Primary"));
}

TEST_CASE("T-407: Document::behavior() is META's behavior; no META or no tail reads 0") {
    const std::string xt = page("", R"X(<div id="P" port:hp="number"></div>)X");
    CHECK(open(xt, 0).doc->behavior() == 0);  // emitMeta off: no META at all.
    CHECK(open(xt, 1).doc->behavior() == 1);
    CHECK(open(xt, 7).doc->behavior() == 7);  // A later version: kept as written.
    // META without the tail (behavior 0).
    CompileOptions options;
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileResult r = compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(r.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    CHECK(doc->ir().meta().present());
    CHECK(doc->behavior() == 0);
}

namespace {

/// T-408: compiles `body` (inside <body>) with `strict` / `reportDeprecated`.
CompileResult compileAliases(const std::string& body, bool strict, bool reportDeprecated,
                             const std::string& style = "") {
    const std::string text = "<xt><head><style>" + style + "</style></head><body>" + body + "</body></xt>";
    const SourceFile file{"alias.xt", text};
    CompileOptions options;
    options.strict = strict;
    options.reportDeprecated = reportDeprecated;
    return compileDocumentFromStrings({&file, 1}, "alias.xt", options);
}

std::vector<std::string> deprecations(const CompileResult& result) {
    std::vector<std::string> out;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning && d.message.rfind("deprecated: use ", 0) == 0) {
            out.push_back(d.message);
        }
    }
    return out;
}

constexpr const char* kAliasBody =
    R"X(<div id="P" port:off="bool in" bind:state.Disabled="off" state-groups="Mode: Open|Closed"
      on:pointerdown="state(Open)" on:pointerup(left)="state(Closed)"
      on:state-enter(Open)="state(Closed)" on:state-exit(Open)="state(Open)"></div>)X";

}  // namespace

TEST_CASE("T-408: the alias registry lists the existing aliases") {
    CHECK(std::string(findNameAlias(AliasKind::Trigger, "pointerdown")->replacement) == "press");
    CHECK(std::string(findNameAlias(AliasKind::Trigger, "pointerup")->replacement) == "release");
    CHECK(std::string(findNameAlias(AliasKind::Trigger, "state-enter(<Name>)")->replacement) ==
          "state(<Name>).enter");
    CHECK(std::string(findNameAlias(AliasKind::Binding, "bind:state.Disabled")->replacement) ==
          "bind:disabled");
    CHECK(findNameAlias(AliasKind::Trigger, "press") == nullptr);
    CHECK(findNameAlias(AliasKind::Action, "pointerdown") == nullptr);
    CHECK(nameAliases().size() == 5);
    CHECK(deprecatedMessage(*findNameAlias(AliasKind::Trigger, "pointerdown")) ==
          "deprecated: use press instead of pointerdown");
}

TEST_CASE("T-408: an alias compiles; strict and reportDeprecated warn, strict does not fail on it") {
    // Default: compiles, no warning (unchanged output).
    const CompileResult plain = compileAliases(kAliasBody, false, false);
    CHECK(plain.ok());
    CHECK(deprecations(plain).empty());

    // reportDeprecated (editor preview / lint): one warning per use.
    const CompileResult reported = compileAliases(kAliasBody, false, true);
    CHECK(reported.ok());
    const std::vector<std::string> messages = deprecations(reported);
    REQUIRE(messages.size() == 5);
    CHECK(std::count(messages.begin(), messages.end(),
                     "deprecated: use bind:disabled instead of bind:state.Disabled") == 1);
    CHECK(std::count(messages.begin(), messages.end(), "deprecated: use press instead of pointerdown") == 1);
    CHECK(std::count(messages.begin(), messages.end(), "deprecated: use release instead of pointerup") == 1);
    CHECK(std::count(messages.begin(), messages.end(),
                     "deprecated: use state(<Name>).enter instead of state-enter(<Name>)") == 1);
    CHECK(std::count(messages.begin(), messages.end(),
                     "deprecated: use state(<Name>).exit instead of state-exit(<Name>)") == 1);
    // The same IR as the new spellings.
    const CompileResult renamed = compileAliases(
        R"X(<div id="P" port:off="bool in" bind:disabled="off" state-groups="Mode: Open|Closed"
          on:press="state(Open)" on:release(left)="state(Closed)"
          on:state(Open).enter="state(Closed)" on:state(Open).exit="state(Open)"></div>)X",
        false, false);
    REQUIRE(renamed.ok());
    CHECK(deprecations(renamed).empty());

    // strict: the warnings are reported, the compile still succeeds.
    const CompileResult strict = compileAliases(kAliasBody, true, false);
    CHECK(strict.ok());
    CHECK(deprecations(strict).size() == 5);
    // ...but any other warning still fails it.
    const CompileResult other = compileAliases(R"X(<div id="P" on:pointerdown="state(Open)"
      state-groups="Mode: Open|Closed"></div>)X",
                                               true, false, "#P { bogus-prop: 1; }");
    CHECK(!other.ok());
}

// -- T-544: bind:effect.<effect>.<param> (18 §18.12, 27 §27.3.2) ----------------------------------

namespace {

constexpr const char* kMaskStyle = "effect: gradient-mask(radial, 0px 0px, 0px 0px, #000f 0%, #0000 100%)";

std::string effectBody(std::string_view binds) {
    return "<div id=\"P\" port:px=\"number\" port:py=\"number\" port:tint=\"color\"></div>"
           "<div id=\"M\" style=\"width: 100px; height: 100px; " +
           std::string(kMaskStyle) + "\" " + std::string(binds) + "></div>";
}

}  // namespace

TEST_CASE("T-544: bind:effect.<name>.<param> and bind:effect.<index>.<param> compile to the effect PropId") {
    CompileResult byName = compileSrc(page("", effectBody("bind:effect.gradient-mask.p0x=\"px\"")));
    REQUIRE(byName.ok());
    Result<IrView> v1 = IrView::open(IrBlob{byName.ir.data(), byName.ir.size()});
    REQUIRE(v1.ok());
    REQUIRE(v1.value().bindings().bindings().size() == 1);
    const IrBinding& b1 = v1.value().bindings().bindings()[0];
    CHECK(b1.target == static_cast<uint8_t>(BindTarget::Prop));
    CHECK(b1.targetId == xt::effectParam(0, 1));
    CHECK(v1.value().string(b1.targetName) == "effect.gradient-mask.p0x");

    CompileResult byIndex = compileSrc(page("", effectBody("bind:effect.0.p0y=\"py\"")));
    REQUIRE(byIndex.ok());
    Result<IrView> v2 = IrView::open(IrBlob{byIndex.ir.data(), byIndex.ir.size()});
    REQUIRE(v2.ok());
    REQUIRE(v2.value().bindings().bindings().size() == 1);
    const IrBinding& b2 = v2.value().bindings().bindings()[0];
    CHECK(b2.target == static_cast<uint8_t>(BindTarget::Prop));
    CHECK(b2.targetId == xt::effectParam(0, 2));
}

TEST_CASE("T-544: bind:effect.* reports unresolvable addresses") {
    // No blur on the element.
    CHECK(compileError(compileSrc(page("", effectBody("bind:effect.blur.radiusX=\"px\""))),
                       "bind:effect.blur.radiusX: the element has no such effect"));
    // No parameter of that name.
    CHECK(compileError(compileSrc(page("", effectBody("bind:effect.gradient-mask.nope=\"px\""))),
                       "bind:effect.gradient-mask.nope: unknown effect parameter"));
    // One segment only.
    CHECK(compileError(compileSrc(page("", effectBody("bind:effect.gradient-mask=\"px\""))),
                       "bind:effect.<effect>.<param> needs an effect and a parameter"));
    // Index out of range.
    CHECK(compileError(compileSrc(page("", effectBody("bind:effect.3.p0x=\"px\""))),
                       "effect index out of range"));
    // An element with no effect at all.
    CHECK(compileError(compileSrc(page("",
                                       "<div id=\"P\" port:px=\"number\"></div>"
                                       "<div id=\"N\" bind:effect.glow.radius=\"px\"></div>")),
                       "the element has no effect"));
    // <svg> content does not take effect.*.
    CHECK(compileError(compileSrc(page("",
                                       "<div id=\"P\" port:px=\"number\"></div>"
                                       "<svg id=\"S\" viewBox=\"0 0 10 10\"><rect id=\"R\" width=\"4\" "
                                       "height=\"4\" bind:effect.glow.radius=\"px\"/></svg>")),
                       "effect.* does not bind on <svg> content"));
}

TEST_CASE("T-544: a color effect parameter takes only a color source and a numeric one only a number") {
    const std::string glowStyle = "effect: glow(8px #2f81f7 1)";
    auto glowPage = [&](std::string_view binds) {
        return page("", "<div id=\"P\" port:px=\"number\" port:tint=\"color\"></div><div id=\"M\" style=\"" +
                            glowStyle + "\" " + std::string(binds) + "></div>");
    };
    CHECK(compileError(compileSrc(glowPage("bind:effect.glow.color=\"px\"")),
                       "a color effect parameter needs a color source"));
    CHECK(compileSrc(glowPage("bind:effect.glow.color=\"tint\"")).ok());
    CHECK(compileError(compileSrc(glowPage("bind:effect.glow.radius=\"tint\"")),
                       "a numeric effect parameter needs a number source"));
    CHECK(compileSrc(glowPage("bind:effect.glow.radius=\"px\"")).ok());
}

TEST_CASE("T-544: a port drives the effect parameter, painting only") {
    Ui ui = open(
        page("", effectBody("bind:effect.gradient-mask.p0x=\"px\" bind:effect.gradient-mask.p0y=\"py\"")));
    const NodeId node = ui.el("M").node;
    auto param = [&](uint32_t i) {
        const NodeCommon& c = ui.doc->arena().common(node);
        REQUIRE(c.effectCount == 1);
        return ui.doc->arena().effects()[c.effectOffset].params[i];
    };
    ui.step(0.016f);
    CHECK(param(0) == doctest::Approx(0.0f));
    const Rect before = ui.el("M").boundingBox();

    ui.set("px", 120);
    ui.set("py", 45);
    ui.step(0.016f);
    CHECK(param(0) == doctest::Approx(120.0f));
    CHECK(param(1) == doctest::Approx(45.0f));
    // No relayout: the box is where it was; a later frame keeps the value and nothing stays dirty.
    CHECK(ui.el("M").boundingBox().x == doctest::Approx(before.x));
    CHECK(ui.el("M").boundingBox().w == doctest::Approx(before.w));
    ui.step(0.016f);
    CHECK(param(0) == doctest::Approx(120.0f));
    CHECK(!ui.doc->arena().anyDirty(static_cast<uint8_t>(kDirtyStyle)));
    CHECK(!ui.doc->arena().anyDirty(static_cast<uint8_t>(kDirtyLayout)));
}

TEST_CASE("T-544: bind:effect.* on an element whose effect is gone is a no-op with one warning") {
    Ui ui = open(page(".fx { " + std::string(kMaskStyle) + "; }",
                      "<div id=\"P\" port:px=\"number\"></div>"
                      "<div id=\"M\" class=\"fx\" bind:effect.gradient-mask.p0x=\"px\"></div>"));
    ui.step(0.016f);
    REQUIRE(ui.doc->arena().common(ui.el("M").node).effectCount == 1);
    REQUIRE(ui.el("M").removeClass("fx"));
    ui.step(0.016f);
    CHECK(ui.doc->arena().common(ui.el("M").node).effectCount == 0);
    ui.set("px", 10);
    ui.step(0.016f);
    ui.set("px", 20);
    ui.step(0.016f);
    size_t missing = 0;
    for (const std::string& w : ui.doc->warnings()) {
        missing += w == kWarnEffectMissing ? 1u : 0u;
    }
    CHECK(missing == 1);
}

TEST_CASE("T-544: the port default reaches the effect parameter once the style pass assigned the stack") {
    Ui ui = open(page("", "<div id=\"P\" port:px=\"number in = 77\"></div><div id=\"M\" style=\"" +
                              std::string(kMaskStyle) + "\" bind:effect.gradient-mask.p0x=\"px\"></div>"));
    const NodeCommon& c = ui.doc->arena().common(ui.el("M").node);
    REQUIRE(c.effectCount == 1);
    CHECK(ui.doc->arena().effects()[c.effectOffset].params[0] == doctest::Approx(77.0f));
    for (const std::string& w : ui.doc->warnings()) {
        CHECK(w != kWarnEffectMissing);
    }
}

TEST_CASE("T-544: | tween(d) on an effect parameter is a transition from the live value") {
    Ui ui = open(page("", "<div id=\"P\" port:px=\"number\"></div><div id=\"M\" style=\"" +
                              std::string(kMaskStyle) +
                              "\" bind:effect.gradient-mask.p0x=\"px | tween(1s)\"></div>"));
    const NodeCommon& c = ui.doc->arena().common(ui.el("M").node);
    REQUIRE(c.effectCount == 1);
    auto p0x = [&] { return ui.doc->arena().effects()[c.effectOffset].params[0]; };
    ui.set("px", 100);
    ui.step(0.016f);
    ui.step(0.5f);
    CHECK(p0x() > 5.0f);
    CHECK(p0x() < 95.0f);
    ui.step(1.0f);
    CHECK(p0x() == doctest::Approx(100.0f));
}

TEST_CASE("T-544: validateBindingTarget accepts effect.<effect>.<param> and rejects other shapes") {
    std::string error;
    CHECK(validateBindingTarget("effect.gradient-mask.p0x", error));
    CHECK(validateBindingTarget("effect.0.radius", error));
    CHECK(!validateBindingTarget("effect.glow", error));
    CHECK(!validateBindingTarget("effect.glow.radius.x", error));
    CHECK(!validateBindingTarget("effect..radius", error));
}

// -- T-545: bind:effect.* lives in the Animator's override layer (04 §4.7, 18 §18.16) ---------------

namespace {

float effectParam0(Ui& ui, std::string_view id) {
    const NodeCommon& c = ui.doc->arena().common(ui.el(id).node);
    REQUIRE(c.effectCount == 1);
    return ui.doc->arena().effects()[c.effectOffset].params[0];
}

size_t effectMissingWarnings(const Ui& ui) {
    size_t n = 0;
    for (const std::string& w : ui.doc->warnings()) {
        n += w == kWarnEffectMissing ? 1u : 0u;
    }
    return n;
}

}  // namespace

TEST_CASE("T-545: an idle bound effect parameter does not dirty the node") {
    Ui ui = open(page("", effectBody("bind:effect.gradient-mask.p0x=\"px\"")));
    ui.step(0.016f);
    ui.set("px", 55);
    ui.step(0.016f);
    ui.step(0.016f);
    const NodeId n = ui.el("M").node;
    // update() clears the dirty bits at its end, so drive the Animator's re-apply directly.
    NodeArena& arena = ui.doc->arena();
    arena.clearAllDirty();
    ui.doc->animator().evaluate(ui.clock + 0.016f, arena);  // same override, same value
    CHECK((arena.dirty(n) & kDirtyPaint) == 0);
    CHECK(effectParam0(ui, "M") == doctest::Approx(55.0f));
    const PropId p0 = effectParam(0, 1);
    ui.doc->animator().setOverride(n, p0, Value::ofFloat(60.0f));  // a real change still dirties
    ui.doc->animator().evaluate(ui.clock + 0.032f, arena);
    CHECK((arena.dirty(n) & kDirtyPaint) != 0);
    CHECK(effectParam0(ui, "M") == doctest::Approx(60.0f));
}

TEST_CASE("T-545: a bound effect parameter survives a restyle") {
    Ui ui = open(page(".x { opacity: 0.5; }", effectBody("bind:effect.gradient-mask.p0x=\"px\"")));
    ui.step(0.016f);
    ui.set("px", 120);
    ui.step(0.016f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(120.0f));  // before the restyle
    REQUIRE(ui.el("M").addClass("x"));
    ui.step(0.016f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(120.0f));  // after the restyle (T-544: the style value)
    ui.set("px", 30);
    ui.step(0.016f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(30.0f));
    CHECK(effectMissingWarnings(ui) == 0);
}

TEST_CASE("T-545: a restyle that rebuilds the effect stack keeps the bound parameter") {
    Ui ui = open(page(".fx { " + std::string(kMaskStyle) +
                          "; } .y { effect: gradient-mask(radial, 5px 0px, 0px 0px, #000f 0%, #0000 100%); }",
                      "<div id=\"P\" port:px=\"number\"></div>"
                      "<div id=\"M\" class=\"fx\" bind:effect.gradient-mask.p0x=\"px\"></div>"));
    ui.step(0.016f);
    ui.set("px", 55);
    ui.step(0.016f);
    REQUIRE(ui.el("M").addClass("y"));
    ui.step(0.016f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(55.0f));
}

TEST_CASE("T-545: the port default reaches the effect parameter at the first update, without a warning") {
    Ui ui = open(page("", "<div id=\"P\" port:px=\"number in = 77\"></div><div id=\"M\" style=\"" +
                              std::string(kMaskStyle) + "\" bind:effect.gradient-mask.p0x=\"px\"></div>"));
    CHECK(effectParam0(ui, "M") == doctest::Approx(77.0f));
    CHECK(effectMissingWarnings(ui) == 0);
}

TEST_CASE("T-545: | tween(d) tweens the override and a restyle mid-tween does not interrupt it") {
    Ui ui =
        open(page(".x { opacity: 0.5; }", "<div id=\"P\" port:px=\"number\"></div><div id=\"M\" style=\"" +
                                              std::string(kMaskStyle) +
                                              "\" bind:effect.gradient-mask.p0x=\"px | tween(1s)\"></div>"));
    ui.step(0.016f);
    ui.set("px", 100);
    ui.step(0.0f);  // The tween starts at this clock.
    ui.step(0.5f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(50.0f).epsilon(0.02));
    REQUIRE(ui.el("M").addClass("x"));
    ui.step(0.25f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(75.0f).epsilon(0.02));
    ui.step(1.0f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(100.0f));
}

TEST_CASE("T-545: an effect removed by a class warns once; its return applies the override at once") {
    Ui ui = open(page(".fx { " + std::string(kMaskStyle) + "; }",
                      "<div id=\"P\" port:px=\"number\"></div>"
                      "<div id=\"M\" class=\"fx\" bind:effect.gradient-mask.p0x=\"px\"></div>"));
    ui.step(0.016f);
    ui.set("px", 42);
    ui.step(0.016f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(42.0f));
    REQUIRE(ui.el("M").removeClass("fx"));
    ui.step(0.016f);
    ui.step(0.016f);
    CHECK(ui.doc->arena().common(ui.el("M").node).effectCount == 0);
    CHECK(effectMissingWarnings(ui) == 1);
    REQUIRE(ui.el("M").addClass("fx"));
    ui.step(0.016f);
    CHECK(effectParam0(ui, "M") == doctest::Approx(42.0f));
    CHECK(effectMissingWarnings(ui) == 1);
}
