#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/types.h"

/// `<repeat>` list templates, RepeatController and virtualization (Docs/design/27-interaction.md
/// §27.3.4, 03 §3.12.4, 12 §12.7, 13 §13.8.9, Docs/tasks/T-177.md).

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
    void setText(std::string_view name, std::string_view v) {
        REQUIRE(doc->setPortText(port(name), v) == Status::kOk);
    }
    /// T-389 (27 §27.4.1): a port change is evaluated by the next update(); the text is read after
    /// one (the clock unmoved).
    std::string text(std::string_view id) const {
        doc->update(clock);
        return std::string(el(id).text());
    }
    std::vector<std::string> instanceTexts(std::string_view repeat, std::string_view inner) {
        std::vector<std::string> out;
        for (const ElementRef& inst : doc->repeatInstances(repeat)) {
            // `<repeat id>/<index>/<template id>`: the inner element shares the first two segments.
            std::string id(inst.id());
            std::string prefix = id.substr(0, id.find('/', id.find('/') + 1) + 1);
            ElementRef e = inner.empty() ? inst : doc->byId(prefix + std::string(inner));
            out.emplace_back(e.valid() ? std::string(e.text()) : std::string("?"));
        }
        return out;
    }
    /// A `{name, count}` entry.
    Value entry(std::string_view name, float count) {
        names[0] = doc->intern("name");
        names[1] = doc->intern("count");
        scratch.push_back({Value::ofStringId(doc->intern(name)), Value::ofFloat(count)});
        return Value::ofStruct(names, scratch.back().data(), 2);
    }
    StringId names[2] = {};
    std::vector<std::vector<Value>> scratch;
};

Ui open(const std::string& xt, float w = 400, float h = 200) {
    Ui out;
    out.result = compileSrc(xt);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    Status status =
        Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr, out.doc);
    REQUIRE(status == Status::kOk);
    out.doc->setViewport(Rect{0, 0, w, h});
    out.doc->update(0.0f);
    return out;
}

const char* kListPage = R"X(
    <div id="P" port:items="list<struct{name: string, count: number}>" port:sel="number out"></div>
    <div id="List">
      <repeat port="items" id="Item">
        <div id="Cell" class="cell" on:click="emit(sel)">
          <span id="Name" bind:text="item(name)"></span>
          <span id="Pos" bind:text="format('{0}/{1}', index, count)"></span>
          <span id="Big" bind:text="item(count) | gt(5)"></span>
        </div>
      </repeat>
    </div>)X";

}  // namespace

// -- Compile ------------------------------------------------------------------------------------

TEST_CASE("repeat: RPT row, template-relative ids, repeat-item bindings and a round trip") {
    CompileResult r = compileSrc(
        page("@timeline item-in { duration: 0.2s; #Cell { 0s { opacity: 0 } 0.2s { opacity: 1 } } }",
             R"X(
        <div id="P" port:items="list<struct{name: string}>"></div>
        <div id="Scroll" style="overflow: scroll; height: 100px">
          <repeat port="items" id="Item" item-enter="play(item-in)" item-exit="play(item-in, reverse) then remove"
                  virtualize viewport="#Scroll">
            <div id="Cell"><span id="Name" bind:text="item(name)"></span></div>
          </repeat>
        </div>)X"));
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    Result<IrView> opened = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(opened.ok());
    const IrView& view = opened.value();
    REQUIRE(view.repeats().present());
    REQUIRE(view.repeats().repeats().size() == 1);
    const IrRepeat& rep = view.repeats().repeats()[0];
    std::span<const IrDocElement> elements = view.doc().elements();
    CHECK(view.string(elements[rep.element].tag) == "repeat");
    CHECK(view.string(elements[rep.element].id) == "Item");
    CHECK(elements[rep.templateRoot].parent == rep.element);
    CHECK(view.string(elements[rep.templateRoot].id) == "Cell");  // Template-relative.
    CHECK(view.string(view.ports().ports()[rep.listPort].name) == "items");
    CHECK(rep.flags == kRepeatVirtualize);
    CHECK(view.string(elements[rep.viewportElement].id) == "Scroll");
    REQUIRE(rep.enterTriggerActions != 0);
    REQUIRE(rep.exitTriggerActions != 0);
    const TriggerChunk& tc = view.triggers();
    CHECK(static_cast<TriggerEvent>(tc.triggers()[rep.enterTriggerActions - 1].event) ==
          TriggerEvent::ItemEnter);
    const IrTrigger& exit = tc.triggers()[rep.exitTriggerActions - 1];
    CHECK(static_cast<TriggerEvent>(exit.event) == TriggerEvent::ItemExit);
    bool remove = false;
    for (uint32_t a = 0; a < exit.actionCount; ++a) {
        remove =
            remove || tc.actions()[exit.actionOffset + a].kind == static_cast<uint8_t>(TriggerAction::Remove);
    }
    CHECK(remove);
    // The item(name) binding starts at the entry.
    REQUIRE(view.bindings().bindings().size() == 1);
    const IrBinding& b = view.bindings().bindings()[0];
    CHECK((b.flags & kBindRepeatItem) != 0);
    CHECK((b.flags & kBindNoPrimary) != 0);
    CHECK(b.sourceCount == 0);
    CHECK(view.bindings().steps()[b.stepOffset].op == static_cast<uint8_t>(ConverterOp::Item));

    IrWriter rebuilt = IrWriter::fromView(view);
    CHECK(rebuilt.finish() == r.ir);
}

TEST_CASE("repeat: IR validation rejects a broken RPT row") {
    CompileResult r =
        compileSrc(page("", R"X(<div id="P" port:items="list<struct{name: string}>" port:n="number"></div>
        <repeat port="items" id="Item"><div id="Cell"></div></repeat>)X"));
    REQUIRE(r.ok());
    Result<IrView> opened = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(opened.ok());
    const IrRepeat& rep = opened.value().repeats().repeats()[0];
    auto corrupt = [&](auto mutate) {
        std::vector<uint8_t> bytes = r.ir;
        const size_t offset = static_cast<size_t>(reinterpret_cast<const uint8_t*>(&rep) - r.ir.data());
        IrRepeat copy = rep;
        mutate(copy);
        std::memcpy(bytes.data() + offset, &copy, sizeof(copy));
        return IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{/*verifyCrc=*/false}).ok();
    };
    CHECK(corrupt([](IrRepeat&) {}));
    CHECK(!corrupt([](IrRepeat& x) { x.templateRoot = x.element; }));  // Not its child.
    CHECK(!corrupt([](IrRepeat& x) { x.listPort = 1; }));              // `n` is no list.
    CHECK(!corrupt([](IrRepeat& x) { x.exitTriggerActions = 99; }));
    CHECK(!corrupt([](IrRepeat& x) { x.viewportElement = 999; }));
}

TEST_CASE("repeat: compile errors") {
    const std::string port =
        R"X(<div id="P" port:items="list<struct{name: string}>" port:n="number"></div>)X";
    CHECK(compileError(compileSrc(page("", port + R"X(<repeat id="Item"><div></div></repeat>)X")),
                       "missing port"));
    CHECK(compileError(compileSrc(page("", port + R"X(<repeat port="items"><div></div></repeat>)X")),
                       "missing id"));
    CHECK(compileError(
        compileSrc(page("", port + R"X(<repeat port="items" id="I"><div></div><div></div></repeat>)X")),
        "exactly one child"));
    CHECK(compileError(compileSrc(page("", port + R"X(<repeat port="n" id="I"><div></div></repeat>)X")),
                       "not a list"));
    CHECK(compileError(compileSrc(page("", port + R"X(<repeat port="nope" id="I"><div></div></repeat>)X")),
                       "no such port"));
    CHECK(compileError(
        compileSrc(page(
            "", port + R"X(<repeat port="items" id="I"><span bind:text="item(nope)"></span></repeat>)X")),
        "no field"));
    CHECK(compileError(compileSrc(page("", port + R"X(<span bind:text="item(name)"></span>)X")),
                       "an expression starts with"));
    CHECK(compileError(compileSrc(page("", port + R"X(<div on:click="remove"></div>)X")),
                       "only valid in item-exit"));
    CHECK(compileError(compileSrc(page("", port + R"X(<div on:item-enter="show()"></div>)X")),
                       "only valid on the template"));
    // T-391 (27 §27.3.4): a nested repeat is allowed, but only the outermost one virtualizes.
    CHECK(compileError(
        compileSrc(page(
            "",
            port +
                R"X(<repeat port="items" id="I"><div><repeat port="items" id="J" virtualize><div></div></repeat></div></repeat>)X")),
        "only supported on the outermost"));
    CHECK(compileError(
        compileSrc(page("", port + R"X(<repeat port="items" id="I"><div port:x="number"></div></repeat>)X")),
        "cannot be declared inside a <repeat> template"));
    CHECK(compileError(
        compileSrc(page("", port + R"X(<repeat port="items" id="I" viewport="#Nope"><div></div></repeat>)X")),
        "no element with that id"));
}

// -- Runtime ------------------------------------------------------------------------------------

TEST_CASE("repeat: listSet instantiates the template per entry with item(), index and count") {
    Ui ui = open(page(".cell { height: 20px; } #Cell { width: 50px; }", kListPage));
    CHECK(ui.doc->repeatInstances("Item").empty());
    CHECK(!ui.el("Cell").valid());  // The template is not part of the document.
    ui.setText("items", R"([{"name":"A","count":1},{"name":"B","count":9},{"name":"C"}])");
    ui.step(0.016f);
    std::span<const ElementRef> inst = ui.doc->repeatInstances("Item");
    REQUIRE(inst.size() == 3);
    CHECK(inst[0].id() == "Item/0/Cell");
    CHECK(inst[2].id() == "Item/2/Cell");
    CHECK(ui.text("Item/1/Name") == "B");
    CHECK(ui.text("Item/1/Pos") == "1/3");
    CHECK(ui.text("Item/1/Big") == "true");
    CHECK(ui.text("Item/0/Big") == "false");
    CHECK(ui.text("Item/2/Name") == "C");
    // `#Cell` (template-relative) still styles every instance; children follow list order.
    CHECK(inst[1].layoutBox().width == doctest::Approx(50.0f));
    CHECK(inst[1].layoutBox().y == doctest::Approx(20.0f));
    CHECK(inst[0].parent().id() == "Item");
    CHECK(ui.instanceTexts("Item", "Name") == std::vector<std::string>{"A", "B", "C"});
}

TEST_CASE("repeat: insert, remove, update and reorder") {
    Ui ui = open(page(".cell { height: 20px; }", kListPage));
    ui.setText("items", R"([{"name":"A"},{"name":"B"},{"name":"C"}])");
    ui.step(0.016f);
    const NodeId nodeB = ui.el("Item/1/Cell").node;

    PortId items = ui.port("items");
    REQUIRE(ui.doc->listInsert(items, 1, ui.entry("X", 0)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Name") == std::vector<std::string>{"A", "X", "B", "C"});
    CHECK(ui.instanceTexts("Item", "Pos") == std::vector<std::string>{"0/4", "1/4", "2/4", "3/4"});
    CHECK(ui.el("Item/2/Cell").node == nodeB);  // Renamed, not rebuilt.
    CHECK(ui.el("Item/1/Cell").layoutBox().y == doctest::Approx(20.0f));

    // listUpdate re-evaluates that instance only.
    REQUIRE(ui.doc->listUpdate(items, 3, ui.entry("Z", 7)) == Status::kOk);
    CHECK(ui.text("Item/3/Name") == "Z");
    CHECK(ui.text("Item/3/Big") == "true");

    // Without item-exit a removed entry's instance goes at once.
    REQUIRE(ui.doc->listRemove(items, 0) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Name") == std::vector<std::string>{"X", "B", "Z"});
    CHECK(ui.instanceTexts("Item", "Pos") == std::vector<std::string>{"0/3", "1/3", "2/3"});
    CHECK(!ui.el("Item/3/Cell").valid());

    // A reordered list re-binds in place; the child order follows the list.
    ui.setText("items", R"([{"name":"Z"},{"name":"B"},{"name":"X"}])");
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Name") == std::vector<std::string>{"Z", "B", "X"});
    std::vector<std::string> order;
    ElementRef repeat = ui.el("Item");
    for (ElementRef c = repeat.firstChild(); c.valid(); c = c.nextSibling()) {
        order.emplace_back(c.id());
    }
    CHECK(order == std::vector<std::string>{"Item/0/Cell", "Item/1/Cell", "Item/2/Cell"});

    // A shorter list drops the surplus.
    ui.setText("items", R"([{"name":"Q"}])");
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Name") == std::vector<std::string>{"Q"});
    ui.setText("items", "[]");
    ui.step(0.016f);
    CHECK(ui.doc->repeatInstances("Item").empty());
}

TEST_CASE("repeat: events from an instance carry the entry index and entry") {
    Ui ui = open(page(".cell { height: 20px; width: 100px; }", kListPage));
    ui.setText("items", R"([{"name":"A"},{"name":"B"}])");
    ui.step(0.016f);
    std::vector<PortEvent> portEvents;
    std::vector<uint32_t> hostItems;
    ui.doc->addPortListener([&](const PortEvent& ev) {
        if (ev.kind == PortEventKind::Emit) {
            portEvents.push_back(ev);
            REQUIRE(ev.item.type == ValueType::Struct);
            CHECK(ui.doc->string(ev.item.items[0].sid) == "B");
        }
    });
    ui.doc->setHostEventListener([&](const HostEvent& ev) { hostItems.push_back(ev.itemIndex); });
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{10, 30};
    ui.doc->dispatchInput(down);
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ui.doc->dispatchInput(up);
    REQUIRE(portEvents.size() == 1);
    CHECK(portEvents[0].itemIndex == 1);
    REQUIRE(hostItems.size() == 1);
    CHECK(hostItems[0] == 1);
}

TEST_CASE("repeat: item-enter and item-exit play per-instance timelines; remove waits for the exit") {
    Ui ui = open(page(R"(.cell { height: 20px; }
        @timeline item-in { duration: 0.2s; #Cell { 0s { opacity: 0 } 0.2s { opacity: 1 } } }
        @timeline item-out { duration: 0.2s; #Cell { 0s { opacity: 1 } 0.2s { opacity: 0 } } })",
                      R"X(<div id="P" port:items="list<struct{name: string}>"></div>
        <repeat port="items" id="Item" item-enter="play(item-in)" item-exit="play(item-out) then remove">
          <div id="Cell" class="cell"><span id="Name" bind:text="item(name)"></span></div>
        </repeat>)X"));
    ui.setText("items", R"([{"name":"A"},{"name":"B"}])");  // listSet: no item-enter.
    ui.step(0.016f);
    CHECK(ui.el("Item/0/Cell").property(prop::Opacity).f == doctest::Approx(1.0f));

    REQUIRE(ui.doc->listInsert(ui.port("items"), 1, ui.entry("X", 0)) == Status::kOk);
    ui.step(0.0f);
    ui.step(0.1f);
    CHECK(ui.el("Item/1/Cell").property(prop::Opacity).f == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(ui.el("Item/0/Cell").property(prop::Opacity).f == doctest::Approx(1.0f));  // Only the new one.
    ui.step(0.2f);
    CHECK(ui.el("Item/1/Cell").property(prop::Opacity).f == doctest::Approx(1.0f));

    const NodeId exiting = ui.el("Item/0/Cell").node;
    REQUIRE(ui.doc->listRemove(ui.port("items"), 0) == Status::kOk);
    CHECK(ui.doc->repeatInstances("Item").size() == 2);  // The exiting one is not listed ...
    CHECK(ui.doc->arena().alive(exiting));               // ... but still there.
    CHECK(ui.text("Item/0/Name") == "X");
    ui.step(0.1f);
    CHECK(ui.doc->arena().alive(exiting));
    CHECK(ui.doc->arena().element(exiting).style.base[prop::Opacity].f <= 1.0f);
    ui.step(0.15f);
    ui.step(0.016f);
    CHECK(!ui.doc->arena().alive(exiting));  // `then remove` after item-out's end.
    CHECK(ui.instanceTexts("Item", "Name") == std::vector<std::string>{"X", "B"});
}

TEST_CASE("repeat: item-exit without remove removes at once") {
    CompileResult r =
        compileSrc(page("@timeline out { duration: 0.2s; #Cell { 0s { opacity: 1 } 0.2s { opacity: 0 } } }",
                        R"X(<div id="P" port:items="list<struct{name: string}>"></div>
        <repeat port="items" id="Item" item-exit="play(out)"><div id="Cell"></div></repeat>)X"));
    bool warned = false;
    for (const Diagnostic& d : r.diagnostics) {
        warned = warned || (d.level == Diagnostic::Level::Warning &&
                            d.message.find("then remove") != std::string::npos);
    }
    CHECK(warned);
    Ui ui = open(page("@timeline out { duration: 0.2s; #Cell { 0s { opacity: 1 } 0.2s { opacity: 0 } } }",
                      R"X(<div id="P" port:items="list<struct{name: string}>"></div>
        <repeat port="items" id="Item" item-exit="play(out)"><div id="Cell"></div></repeat>)X"));
    ui.setText("items", R"([{"name":"A"}])");
    ui.step(0.016f);
    const NodeId node = ui.el("Item/0/Cell").node;
    REQUIRE(ui.doc->listRemove(ui.port("items"), 0) == Status::kOk);
    CHECK(!ui.doc->arena().alive(node));
}

TEST_CASE("repeat: struct and list ports accept JSON text") {
    Ui ui = open(page(
        "", R"X(<div id="P" port:items="list<struct{name: string, n: number, on: bool, c: color, at: vec2}>"
        port:one="struct{name: string}"></div>
        <span id="One" bind:text="one | item(name)"></span>
        <span id="N" bind:text="items | count"></span>)X"));
    ui.setText("one", R"({"name":"Rex é"})");
    CHECK(ui.text("One") == "Rex \xC3\xA9");
    ui.setText("items", R"([{"name":"a","n":2,"on":true,"c":"#ff0000","at":[1,2]}, {"extra":1}])");
    CHECK(ui.text("N") == "2");
    Value list = ui.doc->getPort(ui.port("items"));
    REQUIRE(list.itemCount == 2);
    CHECK(list.items[0].items[1].f == 2.0f);
    CHECK(list.items[0].items[2].f == 1.0f);
    CHECK(list.items[0].items[3].c.r == doctest::Approx(1.0f));
    CHECK(list.items[0].items[4].v2.y == 2.0f);
    CHECK(ui.doc->setPortText(ui.port("items"), "[1,2]") == Status::kInvalidArgument);
    CHECK(ui.doc->setPortText(ui.port("items"), "{") == Status::kInvalidArgument);
    CHECK(ui.doc->setPortText(ui.port("one"), "[]") == Status::kInvalidArgument);
    CHECK(ui.doc->setPortText(ui.port("items"), R"([{"c":"nope"}])") == Status::kInvalidArgument);
}

// -- Virtualization -------------------------------------------------------------------------------

namespace {

const char* kVirtualPage = R"X(
    <div id="P" port:items="list<struct{name: string}>"></div>
    <div id="Scroll" style="position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: scroll">
      <repeat port="items" id="Item" virtualize>
        <div id="Row" style="height: 20px"><span id="Name" bind:text="item(name)"></span></div>
      </repeat>
    </div>)X";

std::string manyEntries(int n) {
    std::string json = "[";
    for (int i = 0; i < n; ++i) {
        json += (i > 0 ? "," : "") + std::string("{\"name\":\"R") + std::to_string(i) + "\"}";
    }
    return json + "]";
}

void scrollTo(Ui& ui, float y) {
    ElementRef scroll = ui.el("Scroll");
    setProperty(ui.doc->arena(), scroll.node, prop::ScrollY, Value::ofFloat(y));
    ui.step(0.016f);
}

}  // namespace

TEST_CASE("repeat: a virtualized list only instantiates the visible window") {
    Ui ui = open(page("", kVirtualPage));
    ui.setText("items", manyEntries(200));
    ui.step(0.016f);
    std::span<const ElementRef> inst = ui.doc->repeatInstances("Item");
    // 200 px viewport / 20 px rows = 10 visible, plus one page after (none before at the top).
    CHECK(inst.size() == 20);
    CHECK(inst.front().id() == "Item/0/Row");
    CHECK(inst.back().id() == "Item/19/Row");
    CHECK(ui.text("Item/5/Name") == "R5");
    // The spacers keep the scroll length of all 200 rows.
    CHECK(ui.el("Scroll").layoutBox().scrollHeight == doctest::Approx(4000.0f));
    uint32_t rows = 0;
    ui.doc->arena().traversePreorder(ui.el("Item").node, [&](NodeId n) {
        rows += ui.doc->arena().kind(n) == NodeKind::Element &&
                ui.doc->string(ui.doc->arena().element(n).tag) == "div";
        return true;
    });
    CHECK(rows == 20 + 2);  // 20 rows and the two spacers.

    scrollTo(ui, 2000);  // Rows 100..110 visible.
    inst = ui.doc->repeatInstances("Item");
    CHECK(inst.size() == 30);
    CHECK(inst.front().id() == "Item/90/Row");
    CHECK(inst.back().id() == "Item/119/Row");
    CHECK(ui.text("Item/100/Name") == "R100");
    CHECK(ui.el("Item/100/Row").layoutBox().y == doctest::Approx(2000.0f));  // Under the spacer.
    CHECK(!ui.el("Item/5/Row").valid());
    CHECK(ui.el("Scroll").layoutBox().scrollHeight == doctest::Approx(4000.0f));
}

TEST_CASE("repeat: virtualization recycles instances through a bounded pool") {
    Ui ui = open(page("", kVirtualPage));
    ui.setText("items", manyEntries(200));
    ui.step(0.016f);
    scrollTo(ui, 1000);
    const uint32_t aliveAfterFirstScroll = ui.doc->arena().aliveCount();
    for (int i = 0; i < 6; ++i) {
        scrollTo(ui, i % 2 == 0 ? 3000.0f : 400.0f);
    }
    scrollTo(ui, 1000);
    // Scrolling back and forth reuses pooled instances: the arena does not keep growing.
    CHECK(ui.doc->arena().aliveCount() <= aliveAfterFirstScroll + 30u * 4u);
    CHECK(ui.doc->repeatInstances("Item").size() == 30);
    CHECK(ui.text("Item/50/Name") == "R50");
    // A recycled row shows its new entry and path id.
    scrollTo(ui, 3500);
    CHECK(ui.text("Item/180/Name") == "R180");
    const uint32_t a = ui.doc->arena().aliveCount();
    scrollTo(ui, 3000);
    scrollTo(ui, 3500);
    CHECK(ui.doc->arena().aliveCount() == a);
    // Inserting in the window shifts the rows; removing keeps the scroll length consistent.
    REQUIRE(ui.doc->listInsert(ui.port("items"), 180, ui.entry("NEW", 0)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("Item/180/Name") == "NEW");
    CHECK(ui.text("Item/181/Name") == "R180");
    CHECK(ui.el("Scroll").layoutBox().scrollHeight == doctest::Approx(4020.0f));
    REQUIRE(ui.doc->listRemove(ui.port("items"), 0) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("Item/180/Name") == "R180");
    CHECK(ui.el("Scroll").layoutBox().scrollHeight == doctest::Approx(4000.0f));
}

// -- Repeats inside symbol instances (T-378, 27 §27.11) -----------------------------------------

namespace {

const char* kSymbolListPage = R"X(<xt><head><style>
      symbol(CListSym) { #Track { width: 50px; height: 20px; } }
    </style>
    <symbol name="TrackSym"><span id="Add" bind:text="item(name)"></span></symbol>
    <symbol name="CListSym">
      <div id="Box" port:items="list<struct{name: string}>">
        <repeat port="items" id="CRow"><use symbol="TrackSym" id="Track"></use></repeat>
      </div>
    </symbol>
    <symbol name="InnerSym">
      <div id="Box" port:rows="list<struct{name: string}>">
        <repeat port="rows" id="Row"><div id="Cell" bind:text="item(name)"></div></repeat>
      </div>
    </symbol>
    <symbol name="OuterSym"><use symbol="InnerSym" id="Inner"></use></symbol>
    </head><body id="Root">
      <use symbol="CListSym" id="CList"></use>
      <use symbol="OuterSym" id="Outer"></use>
    </body></xt>)X";

}  // namespace

TEST_CASE("repeat: path ids inside a symbol instance carry the instance prefix once") {
    Ui ui = open(kSymbolListPage);
    ui.setText("CList/Box.items", R"([{"name":"A"},{"name":"B"},{"name":"C"}])");
    ui.step(0.016f);
    std::span<const ElementRef> inst = ui.doc->repeatInstances("CList/CRow");
    REQUIRE(inst.size() == 3);
    CHECK(inst[0].id() == "CList/CRow/0/Track");
    CHECK(inst[2].id() == "CList/CRow/2/Track");
    CHECK(ui.text("CList/CRow/0/Track/Add") == "A");
    CHECK(ui.text("CList/CRow/1/Track/Add") == "B");
    CHECK(ui.text("CList/CRow/2/Track/Add") == "C");
    CHECK(ui.el("CList/CRow/1/Track/Add").valid());
    CHECK(!ui.el("CList/CRow/0/CList/Track/Add").valid());  // The doubled prefix of XF-024.

    // The symbol's `#Track` rule (rewritten to the template id) still matches every instance.
    for (const ElementRef& e : inst) {
        CHECK(e.layoutBox().width == doctest::Approx(50.0f));
    }

    // An index shift renames with the same rule.
    PortId items = ui.port("CList/Box.items");
    ui.setText("CList/Box.items", R"([{"name":"X"},{"name":"A"},{"name":"B"},{"name":"C"}])");
    ui.step(0.016f);
    CHECK(ui.text("CList/CRow/0/Track/Add") == "X");
    CHECK(ui.text("CList/CRow/3/Track/Add") == "C");
    REQUIRE(ui.doc->listRemove(items, 0) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("CList/CRow/0/Track/Add") == "A");
    CHECK(ui.text("CList/CRow/2/Track/Add") == "C");
    CHECK(!ui.el("CList/CRow/3/Track/Add").valid());
}

TEST_CASE("repeat: nested symbol instances strip only the shared instance prefix") {
    Ui ui = open(kSymbolListPage);
    ui.setText("Outer/Inner/Box.rows", R"([{"name":"P"},{"name":"Q"}])");
    ui.step(0.016f);
    std::span<const ElementRef> inst = ui.doc->repeatInstances("Outer/Inner/Row");
    REQUIRE(inst.size() == 2);
    CHECK(inst[0].id() == "Outer/Inner/Row/0/Cell");
    CHECK(ui.text("Outer/Inner/Row/1/Cell") == "Q");
}

TEST_CASE("repeat: top-level path ids keep their format") {
    Ui ui = open(R"X(<xt><head>
      <symbol name="CellSym"><span id="Icon" bind:text="item(name)"></span></symbol></head>
      <body id="Root"><div id="P" port:items="list<struct{name: string}>"></div>
      <repeat port="items" id="Item"><use symbol="CellSym" id="Cell"></use></repeat></body></xt>)X");
    ui.setText("items", R"([{"name":"a"},{"name":"b"},{"name":"c"},{"name":"d"}])");
    ui.step(0.016f);
    CHECK(ui.text("Item/3/Cell/Icon") == "d");
}

// -- bind:<port> on symbol instance in-ports (T-380, 27 §27.3.4) --------------------------------

namespace {

const char* kRowPage = R"X(<xt><head>
    <symbol name="Row"><span id="T" port:title="string in" bind:text="title"
      port:changed="number out" on:port(title)="emit(changed)"></span></symbol></head>
  <body id="Root">
    <div id="P" port:headline="string" port:items="list<struct{title: string}>"></div>
    <use symbol="Row" id="R" bind:title="headline"></use>
    <repeat port="items" id="Item"><use symbol="Row" id="Cell" bind:title="item(title)"></use></repeat>
  </body></xt>)X";

Value titleEntry(Ui& ui, std::string_view title) {
    ui.names[0] = ui.doc->intern("title");
    ui.scratch.push_back({Value::ofStringId(ui.doc->intern(title))});
    return Value::ofStruct(ui.names, ui.scratch.back().data(), 1);
}

std::string portText(Ui& ui, std::string_view name) { return ui.doc->portText(ui.port(name)); }

}  // namespace

TEST_CASE("repeat: bind:<port> on a top-level <use> writes the instance's in-port") {
    Ui ui = open(kRowPage);
    CHECK(ui.text("R/T") == "");
    ui.setText("headline", "Hello");
    ui.step(0.016f);
    CHECK(ui.text("R/T") == "Hello");
    CHECK(portText(ui, "R.title") == "Hello");
    CHECK(ui.doc->lookupPort("R/T.title") == ui.port("R.title"));
}

TEST_CASE("repeat: bind:<port>=item(...) gives every repeat instance its own port value") {
    Ui ui = open(kRowPage);
    std::vector<PortEvent> emits;
    ui.doc->addPortListener([&emits](const PortEvent& e) {
        if (e.kind == PortEventKind::Emit) {
            emits.push_back(e);
        }
    });
    ui.setText("items", R"([{"title":"a"},{"title":"b"},{"title":"c"}])");
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Cell/T") == std::vector<std::string>{"a", "b", "c"});
    CHECK(portText(ui, "Item/2/Cell.title") == "c");
    CHECK(ui.doc->lookupPort("Item/2/Cell/T.title") == ui.port("Item/2/Cell.title"));
    CHECK(ui.port("Item/0/Cell.title") != ui.port("Item/1/Cell.title"));
    CHECK(emits.empty());  // The first value of a new instance notifies no trigger.
    // The template's own port copy is not reachable.
    CHECK(ui.doc->lookupPort("Cell.title") == kInvalidPort);

    // (c) The host writes one instance's port by its path id.
    REQUIRE(ui.doc->setPortText(ui.port("Item/2/Cell.title"), "Z") == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Cell/T") == std::vector<std::string>{"a", "b", "Z"});
    REQUIRE(emits.size() == 1);
    CHECK(emits[0].itemIndex == 2);
    emits.clear();

    // listUpdate of the first entry re-evaluates only the first instance.
    REQUIRE(ui.doc->listUpdate(ui.port("items"), 0, titleEntry(ui, "A")) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Cell/T") == std::vector<std::string>{"A", "b", "Z"});
    REQUIRE(emits.size() == 1);  // `on:port(title)` inside the symbol, on that instance only.
    CHECK(emits[0].itemIndex == 0);
    CHECK(emits[0].port == ui.port("Item/0/Cell/T.changed"));

    // listRemove shifts the indices; every remaining instance shows its entry again.
    REQUIRE(ui.doc->listRemove(ui.port("items"), 0) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Cell/T") == std::vector<std::string>{"b", "c"});
    CHECK(portText(ui, "Item/0/Cell.title") == "b");
    CHECK(portText(ui, "Item/1/Cell.title") == "c");
    CHECK(ui.doc->lookupPort("Item/2/Cell.title") == kInvalidPort);

    // A new instance gets a fresh copy.
    REQUIRE(ui.doc->listInsert(ui.port("items"), 2, titleEntry(ui, "d")) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "Cell/T") == std::vector<std::string>{"b", "c", "d"});
    CHECK(portText(ui, "Item/2/Cell.title") == "d");
    // Instance copies are not part of the port table.
    for (const PortRecord& rec : ui.doc->ports()) {
        CHECK(rec.qualifiedName.rfind("Item/", 0) != 0);
        CHECK(rec.qualifiedName != "Cell/T.title");
    }
}

TEST_CASE("repeat: bind:<port> compile rules") {
    // (d) An out or event port is no target.
    const std::string sym =
        R"X(<xt><head><symbol name="O"><span id="T" port:clicked="number out" port:ping="event"
          port:opacity="number in" bind:text="opacity"></span></symbol></head><body id="Root">
          <div id="P" port:n="number"></div>)X";
    CHECK(compileError(compileSrc(sym + R"X(<use symbol="O" id="X" bind:clicked="n"></use></body></xt>)X"),
                       "bind: cannot target an out port"));
    CHECK(compileError(compileSrc(sym + R"X(<use symbol="O" id="X" bind:ping="n"></use></body></xt>)X"),
                       "bind: cannot target an out port"));
    // (f) A port wins over the property of the same name.
    CompileResult r = compileSrc(sym + R"X(<use symbol="O" id="X" bind:opacity="n"></use></body></xt>)X");
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    bool portTarget = false;
    for (const IrBinding& b : view.value().bindings().bindings()) {
        portTarget = portTarget || static_cast<BindTarget>(b.target) == BindTarget::Port;
    }
    CHECK(portTarget);
    Ui ui = open(sym + R"X(<use symbol="O" id="X" bind:opacity="n"></use></body></xt>)X");
    REQUIRE(ui.doc->setPort(ui.port("n"), Value::ofFloat(3)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("X/T") == "3");
    CHECK(ui.doc->getPort(ui.port("X.opacity")).f == doctest::Approx(3));
    // A name the symbol does not declare is still a property.
    CHECK(compileSrc(sym + R"X(<use symbol="O" id="X" bind:width="n"></use></body></xt>)X").ok());

    // (e) A port written directly inside a template names the way to do it.
    const std::string list = R"X(<div id="P" port:items="list<struct{title: string}>"></div>)X";
    const char* hint = "declare the port on the symbol and bind it on <use> with bind:<port>=item(...)";
    CHECK(compileError(
        compileSrc(page("", list + R"X(<repeat port="items" id="I"><div port:x="number"></div></repeat>)X")),
        hint));
    CHECK(compileError(compileSrc(R"X(<xt><head><symbol name="S"><span id="A"></span></symbol></head>
        <body id="Root"><div id="P" port:items="list<struct{title: string}>"></div>
        <repeat port="items" id="I"><use symbol="S" id="C" port:x="number"></use></repeat></body></xt>)X"),
                       hint));
}

// -- T-381: list<primitive> ports, item() and item(<n>) -------------------------------------------

TEST_CASE("repeat: list<primitive> port types compile") {
    CompileResult r = compileSrc(page("", R"X(
        <div id="P" port:labels="list<string>" port:items="list<struct{name: string, count: number}>"
             port:modes="list<enum(a|b|c)>" port:pts="list<vec2>"></div>)X"));
    REQUIRE(r.ok());
    Result<IrView> opened = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(opened.ok());
    const IrView& view = opened.value();
    auto find = [&](std::string_view name) -> IrPort {
        for (const IrPort& p : view.ports().ports()) {
            if (view.string(p.name) == name) {
                return p;
            }
        }
        FAIL("no port " << name);
        return IrPort{};
    };
    const IrPort labels = find("labels");
    CHECK(static_cast<PortValueType>(labels.type) == PortValueType::List);
    CHECK(static_cast<PortValueType>(labels.entryType) == PortValueType::String);
    CHECK(labels.fieldCount == 0);
    const IrPort items = find("items");
    CHECK(items.entryType == 0);  // list<struct> is unchanged.
    CHECK(items.fieldCount == 2);
    const IrPort modes = find("modes");
    CHECK(static_cast<PortValueType>(modes.entryType) == PortValueType::Enum);
    CHECK(modes.fieldCount == 3);
    CHECK(static_cast<PortValueType>(find("pts").entryType) == PortValueType::Vec2);

    CHECK(compileError(compileSrc(page("", R"X(<div port:t="list<texture>"></div>)X")),
                       "a list's entries must be a struct{...} or one of"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:t="list<event>"></div>)X")),
                       "a list's entries must be"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:t="list<list<string>>"></div>)X")),
                       "a list's entries must be"));
}

TEST_CASE("repeat: item() and item(<n>) compile rules") {
    const std::string ports =
        R"X(<div id="P" port:labels="list<string>" port:items="list<struct{name: string}>"
             port:n="number"></div>)X";
    CHECK(compileError(
        compileSrc(
            page("", ports + R"X(<repeat port="items" id="I"><span bind:text="item()"></span></repeat>)X")),
        "item() needs a field name"));
    CHECK(compileError(
        compileSrc(page(
            "", ports + R"X(<repeat port="labels" id="I"><span bind:text="item(name)"></span></repeat>)X")),
        "the list entries are not structs"));
    CHECK(compileError(compileSrc(page("", ports + R"X(<span bind:text="items | item(1)"></span>)X")),
                       "follow it with item(<field>)"));
    CHECK(compileError(compileSrc(page("", ports + R"X(<span bind:text="n | item(1)"></span>)X")),
                       "reads an entry of a list port"));
    CHECK(compileError(compileSrc(page("", ports + R"X(<span bind:text="labels | item(-1)"></span>)X")),
                       "non-negative integer index"));
    CHECK(compileError(compileSrc(page("", ports + R"X(<span bind:text="labels | item(1.5)"></span>)X")),
                       "non-negative integer index"));
    CHECK(compileError(compileSrc(page("", ports + R"X(<span bind:text="labels | item(name)"></span>)X")),
                       "read a list entry first with item(<n>)"));
    CHECK(compileSrc(page("", ports + R"X(<span bind:text="items | item(1) | item(name)"></span>)X")).ok());
    CHECK(compileSrc(page("", ports + R"X(<span bind:text="labels | item(0) | upper"></span>)X")).ok());
}

TEST_CASE("repeat: <repeat> over a list<string> shows item(); item(<n>) reads one entry") {
    Ui ui = open(page("", R"X(
        <div id="P" port:labels="list<string>"></div>
        <span id="Second" bind:text="labels | item(1)"></span>
        <span id="Upper" bind:text="labels | item(0) | upper"></span>
        <div id="List">
          <repeat port="labels" id="Item">
            <span id="Cell" bind:text="item()"></span>
          </repeat>
        </div>)X"));
    ui.setText("labels", R"(["a","b","c"])");
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "") == std::vector<std::string>{"a", "b", "c"});
    CHECK(ui.text("Second") == "b");
    CHECK(ui.text("Upper") == "A");
    CHECK(ui.doc->portInfo(ui.port("labels")).entryType == PortValueType::String);

    // listUpdate re-evaluates item(<n>) and the instance.
    const PortId labels = ui.port("labels");
    REQUIRE(ui.doc->listUpdate(labels, 1, Value::ofStringId(ui.doc->intern("B"))) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("Second") == "B");
    CHECK(ui.instanceTexts("Item", "") == std::vector<std::string>{"a", "B", "c"});

    // Out of range: the entry type's empty value (T-383, was "keeps its previous value").
    Value one = Value::ofStringId(ui.doc->intern("z"));
    REQUIRE(ui.doc->listSet(labels, std::span<const Value>(&one, 1)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("Second") == "");
    CHECK(ui.text("Upper") == "Z");
    CHECK(ui.instanceTexts("Item", "") == std::vector<std::string>{"z"});
    REQUIRE(ui.doc->listInsert(labels, 1, Value::ofStringId(ui.doc->intern("y"))) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("Second") == "y");

    // A wrong entry type is refused.
    CHECK(ui.doc->listInsert(labels, 0, Value::ofFloat(1)) == Status::kInvalidArgument);
    CHECK(ui.doc->setPortText(labels, "[1, \"w\"]") == Status::kOk);  // JSON numbers become text.
    ui.step(0.016f);
    CHECK(ui.text("Second") == "w");
    CHECK(ui.text("Upper") == "1");
}

TEST_CASE("repeat: list<primitive> JSON setPortText and values by entry type") {
    Ui ui = open(page("", R"X(
        <div id="P" port:nums="list<number>" port:flags="list<bool>" port:modes="list<enum(a|b|c)>"
             port:items="list<struct{name: string}>"></div>
        <span id="N" bind:text="nums | item(1) | scale(2)"></span>
        <span id="F" bind:text="flags | item(0)"></span>
        <span id="M" bind:text="modes | item(1)"></span>
        <span id="S" bind:text="items | item(1) | item(name)"></span>)X"));
    ui.setText("nums", "[1, 2.5]");
    ui.setText("flags", "[true, false]");
    ui.setText("modes", R"(["c", 1])");
    ui.setText("items", R"([{"name":"x"},{"name":"y"}])");
    ui.step(0.016f);
    CHECK(ui.text("N") == "5");
    CHECK(ui.text("F") == "true");
    CHECK(ui.text("M") == "b");
    CHECK(ui.text("S") == "y");

    Value nums = ui.doc->getPort(ui.port("nums"));
    REQUIRE(nums.type == ValueType::List);
    REQUIRE(nums.itemCount == 2);
    CHECK(nums.items[1].f == doctest::Approx(2.5f));
    Value modes = ui.doc->getPort(ui.port("modes"));
    REQUIRE(modes.itemCount == 2);
    CHECK(ui.doc->string(modes.items[0].sid) == "c");

    // Enum entries by ordinal or name; an unknown name or a bad value is refused.
    CHECK(ui.doc->listUpdate(ui.port("modes"), 1, Value::ofEnum(0)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("M") == "a");
    CHECK(ui.doc->listUpdate(ui.port("modes"), 1, Value::ofStringId(ui.doc->intern("d"))) ==
          Status::kInvalidArgument);
    CHECK(ui.doc->setPortText(ui.port("modes"), R"(["d"])") == Status::kInvalidArgument);
    CHECK(ui.doc->setPortText(ui.port("nums"), R"(["x"])") == Status::kInvalidArgument);
    CHECK(ui.doc->setPortText(ui.port("nums"), R"({"a": 1})") == Status::kInvalidArgument);
    CHECK(ui.text("N") == "5");
}

// -- T-383 (27 §27.3.3 / §27.3.4; Aux Duty XF-041 / XF-043) ---------------------------------------

namespace {

bool hasWarning(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const char* kIndexPortDoc = R"X(<xt><head>
    <symbol name="Row"><div id="Box" port:index="number in">
      <span id="T" bind:text="index"></span><span id="R" bind:text="repeat.index"></span>
      <span id="N" bind:text="repeat.count"></span>
    </div></symbol>
  </head><body id="Root">
    <div id="P" port:items="list<struct{name: string}>"></div>
    <div id="List"><repeat port="items" id="Item">
      <use symbol="Row" id="C" bind:index="index | scale(10)"></use>
    </repeat></div>
  </body></xt>)X";

}  // namespace

TEST_CASE("repeat: T-383 a declared in port named index wins over the repeat index, with a warning") {
    const CompileResult r = compileSrc(kIndexPortDoc);
    REQUIRE(r.ok());
    CHECK(hasWarning(r,
                     "port name \"index\" shadows a reserved name; bindings read the port; write "
                     "repeat.index for the repeat counter"));
    Ui ui = open(kIndexPortDoc);
    ui.setText("items", R"([{"name":"a"},{"name":"b"},{"name":"c"}])");
    ui.step(0.016f);
    // `index` inside the symbol reads its port (the <use> fed it repeat index x 10);
    // `repeat.index` / `repeat.count` read the repeat.
    CHECK(ui.instanceTexts("Item", "C/T") == std::vector<std::string>{"0", "10", "20"});
    CHECK(ui.instanceTexts("Item", "C/R") == std::vector<std::string>{"0", "1", "2"});
    CHECK(ui.instanceTexts("Item", "C/N") == std::vector<std::string>{"3", "3", "3"});
}

TEST_CASE("repeat: T-383 repeat.index / repeat.item() outside a template are errors") {
    CHECK(compileError(compileSrc(page("", R"X(<span bind:text="repeat.index"></span>)X")),
                       "repeat.index is only valid inside a <repeat> template"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:n="number"></div>
        <span bind:text="format('{0}{1}', n, repeat.item(name))"></span>)X")),
                       "repeat.item(name) is only valid inside a <repeat> template"));
    // Inside a template repeat.item(<field>) reads the entry.
    Ui ui = open(page("", R"X(<div id="P" port:items="list<struct{name: string}>"></div>
        <div id="List"><repeat port="items" id="Item"><span id="S" bind:text="repeat.item(name) | upper">
        </span></repeat></div>)X"));
    ui.setText("items", R"([{"name":"a"},{"name":"b"}])");
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "") == std::vector<std::string>{"A", "B"});
}

TEST_CASE(
    "repeat: T-383 a port named format is the port as a source (no warning); | format(...) stays the "
    "converter") {
    const std::string xt = page("", R"X(<div id="P" port:format="string in" port:n="number in"></div>
        <span id="A" bind:text="format"></span>
        <span id="B" bind:text="n | format('{0}!')"></span>)X");
    const CompileResult r = compileSrc(xt);
    REQUIRE(r.ok());
    CHECK_FALSE(hasWarning(r, "shadows"));  // T-385 ruling: a converter name is never ambiguous.
    CHECK(hasWarning(compileSrc(page("", R"X(<div port:time="number in"></div>)X")),
                     "port name \"time\" shadows a reserved name"));
    Ui ui = open(xt);
    ui.setText("format", "hello");
    ui.setText("n", "5");
    ui.step(0.016f);
    CHECK(ui.text("A") == "hello");
    CHECK(ui.text("B") == "5!");
    // An out port with a reserved name does not warn.
    CHECK_FALSE(hasWarning(compileSrc(page("", R"X(<div port:select="event out"></div>)X")), "shadows"));
}

TEST_CASE("repeat: T-383 item(<n>) out of range yields the entry type's empty value") {
    Ui ui = open(page("", R"X(
        <div id="P" port:labels="list<string>" port:nums="list<number>" port:flags="list<bool>"
             port:modes="list<enum(a|b|c)>" port:items="list<struct{name: string, n: number}>"></div>
        <span id="L" bind:text="labels | item(4)"></span>
        <span id="N" bind:text="nums | item(2)"></span>
        <span id="F" bind:text="flags | item(2)"></span>
        <span id="M" bind:text="modes | item(2)"></span>
        <span id="S" bind:text="items | item(2) | item(name)"></span>
        <span id="SN" bind:text="items | item(2) | item(n) | offset(1)"></span>)X"));
    ui.setText("labels", R"(["a","b","c"])");
    ui.setText("nums", "[4]");
    ui.setText("flags", "[true]");
    ui.setText("modes", R"(["c"])");
    ui.setText("items", R"([{"name":"x","n":3}])");
    ui.step(0.016f);
    CHECK(ui.text("L") == "");
    CHECK(ui.text("N") == "0");
    CHECK(ui.text("F") == "false");
    CHECK(ui.text("M") == "a");
    CHECK(ui.text("S") == "");
    CHECK(ui.text("SN") == "1");
    // listSet with enough entries brings the value back.
    ui.setText("labels", R"(["a","b","c","d","e"])");
    ui.setText("items", R"([{"name":"x","n":3},{"name":"y","n":4},{"name":"z","n":5}])");
    ui.step(0.016f);
    CHECK(ui.text("L") == "e");
    CHECK(ui.text("S") == "z");
    CHECK(ui.text("SN") == "6");
}

TEST_CASE("repeat: T-383 a list port's JSON-array default is loaded with the document") {
    Ui ui = open(page("", R"X(
        <div id="P" port:labels='list<string> in = ["a","b","c"]'
             port:items='list<struct{name: string}> = [{"name":"x"}, {"name":"y"}]'
             port:pts="list<vec2> = [[1, 2]]"></div>
        <span id="Second" bind:text="labels | item(1)"></span>
        <span id="Fifth" bind:text="labels | item(4)"></span>
        <span id="Y" bind:text="items | item(1) | item(name)"></span>
        <div id="List"><repeat port="labels" id="Item"><span id="Cell" bind:text="item()"></span></repeat></div>)X"));
    ui.step(0.016f);
    CHECK(ui.instanceTexts("Item", "") == std::vector<std::string>{"a", "b", "c"});
    CHECK(ui.text("Second") == "b");
    CHECK(ui.text("Fifth") == "");
    CHECK(ui.text("Y") == "y");
    CHECK(ui.doc->getPort(ui.port("pts")).itemCount == 1);
    // listSet afterwards replaces the default.
    const PortId labels = ui.port("labels");
    Value e[5];
    for (int i = 0; i < 5; ++i) {
        e[i] = Value::ofStringId(ui.doc->intern(std::string(1, static_cast<char>('p' + i))));
    }
    REQUIRE(ui.doc->listSet(labels, std::span<const Value>(e, 5)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.text("Fifth") == "t");
    CHECK(ui.instanceTexts("Item", "").size() == 5);

    CHECK(compileError(compileSrc(page("", R"X(<div port:l='list<number> = ["x"]'></div>)X")),
                       "is not a value of the list's entry type"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:l="list<number> = 1"></div>)X")),
                       "is not a JSON array"));
    CHECK(compileError(compileSrc(page("", R"X(<div port:l='list<struct{a: number}> = [1]'></div>)X")),
                       "is not a JSON object"));
}

// -- T-387 (13 §13.9): Document::repeatIndexAt ------------------------------------------------------

TEST_CASE("T-387: repeatIndexAt hits the instance boxes of a plain repeat") {
    Ui ui = open(page(".cell { height: 30px; }", kListPage));
    ui.setText("items", R"([{"name":"a","count":1},{"name":"b","count":2},{"name":"c","count":3}])");
    ui.step(0.016f);
    std::span<const ElementRef> inst = ui.doc->repeatInstances("Item");
    REQUIRE(inst.size() == 3);
    for (uint32_t i = 0; i < 3; ++i) {
        const Rect box = inst[i].boundingBox();
        CAPTURE(i);
        CHECK(ui.doc->repeatIndexAt("Item", Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f}) == i);
    }
    const Rect last = inst[2].boundingBox();
    CHECK(ui.doc->repeatIndexAt("Item", Vec2{10, last.y + last.h + 20}) == 0xFFFFFFFFu);  // Below.
    CHECK(ui.doc->repeatIndexAt("Nope", Vec2{10, 10}) == 0xFFFFFFFFu);
}

TEST_CASE("T-387: repeatIndexAt of a virtualized repeat follows the scroll position") {
    Ui ui = open(page("", kVirtualPage));
    ui.setText("items", manyEntries(200));
    ui.step(0.016f);
    CHECK(ui.doc->repeatIndexAt("Item", Vec2{50, 5}) == 0);
    CHECK(ui.doc->repeatIndexAt("Item", Vec2{50, 45}) == 2);
    scrollTo(ui, 2000);  // Rows 100..110 visible.
    CHECK(ui.doc->repeatIndexAt("Item", Vec2{50, 5}) == 100);
    CHECK(ui.doc->repeatIndexAt("Item", Vec2{50, 50}) == 102);
    CHECK(ui.doc->repeatIndexAt("Item", Vec2{50, 199}) == 109);
    CHECK(ui.doc->repeatIndexAt("Item", Vec2{250, 50}) == 0xFFFFFFFFu);  // Right of the list.

    // A short list: the empty area below its last row is no entry.
    Ui small = open(page("", kVirtualPage));
    small.setText("items", manyEntries(5));
    small.step(0.016f);
    CHECK(small.doc->repeatIndexAt("Item", Vec2{50, 95}) == 4);
    CHECK(small.doc->repeatIndexAt("Item", Vec2{50, 150}) == 0xFFFFFFFFu);
}

// -- T-392 (27 §27.3.2, XF-073): a virtualized repeat with a zero-height viewport -----------------

TEST_CASE("T-392: a virtualized repeat whose viewport is 0 px high reports it once") {
    auto zeroViewportWarnings = [](const Ui& ui) {
        int n = 0;
        for (const std::string& w : ui.doc->warnings()) {
            n += w ==
                 "virtualized repeat \"Item\" has a zero-height viewport; give the scroll container a height";
        }
        return n;
    };
    std::string flat = kVirtualPage;
    flat.replace(flat.find("height: 200px"), 13, "height: 0px");
    Ui zero = open(page("", flat));
    zero.setText("items", manyEntries(50));
    zero.step(0.016f);
    zero.step(0.016f);
    zero.setText("items", manyEntries(60));
    zero.step(0.016f);
    CHECK(zeroViewportWarnings(zero) == 1);

    Ui sized = open(page("", kVirtualPage));
    sized.setText("items", manyEntries(50));
    sized.step(0.016f);
    sized.step(0.016f);
    CHECK(zeroViewportWarnings(sized) == 0);
}

// -- T-391: nested lists ---------------------------------------------------------------------------

namespace {

const char* kNestPage = R"X(
    <div id="P" port:groups="list<struct{name: string, rows: list<struct{t: string}>}>" port:sel="number out"></div>
    <repeat port="groups" id="Rows" style="display: flex; flex-direction: column">
      <div id="Row" style="height: 40px; display: flex">
        <span id="Name" bind:text="item(name)"></span>
        <span id="Len" bind:text="item(rows) | count"></span>
        <repeat port="item(rows)" id="Sub" style="display: flex">
          <div id="It" style="display: flex">
            <span id="Cell" style="width: 40px; height: 20px"
                  bind:text="format('{0}:{1}:{2}', outer.index, index, item(t))" on:click="emit(sel)"></span>
            <span id="Up" bind:text="outer.item(name)"></span>
          </div>
        </repeat>
      </div>
    </repeat>)X";

const char* kNestData =
    R"([{"name":"A","rows":[{"t":"a0"},{"t":"a1"},{"t":"a2"}]},{"name":"B","rows":[{"t":"b0"},{"t":"b1"}]}])";

/// Builds `{name, rows: [{t}...]}` Values for one test (the storage lives as long as the builder).
struct NestBuilder {
    // Not an aggregate: GCC -Wextra flags `NestBuilder{doc}` (missing-field-initializers).
    explicit NestBuilder(Document* d) : doc(d) {}

    Document* doc = nullptr;
    std::vector<std::unique_ptr<std::vector<Value>>> store;
    StringId nameKey = kInvalidString, rowsKey = kInvalidString, tKey = kInvalidString;
    StringId groupKeys[2] = {};

    Value group(std::string_view name, const std::vector<std::string>& ts) {
        nameKey = doc->intern("name");
        rowsKey = doc->intern("rows");
        tKey = doc->intern("t");
        groupKeys[0] = nameKey;
        groupKeys[1] = rowsKey;
        auto rows = std::make_unique<std::vector<Value>>();
        for (const std::string& t : ts) {
            store.push_back(std::make_unique<std::vector<Value>>(1, Value::ofStringId(doc->intern(t))));
            rows->push_back(Value::ofStruct(&tKey, store.back()->data(), 1));
        }
        const Value list = Value::ofList(rows->data(), static_cast<uint32_t>(rows->size()));
        store.push_back(std::move(rows));
        store.push_back(std::make_unique<std::vector<Value>>(
            std::vector<Value>{Value::ofStringId(doc->intern(name)), list}));
        return Value::ofStruct(groupKeys, store.back()->data(), 2);
    }
    Value row(std::string_view t) {
        tKey = doc->intern("t");
        store.push_back(std::make_unique<std::vector<Value>>(1, Value::ofStringId(doc->intern(t))));
        return Value::ofStruct(&tKey, store.back()->data(), 1);
    }
};

/// The text of element `inner` (a template-relative id) of every instance of repeat `sub`.
std::vector<std::string> cellTexts(Ui& ui, std::string_view sub, std::string_view inner = "Cell") {
    std::vector<std::string> out;
    ui.doc->update(ui.clock);
    for (const ElementRef& inst : ui.doc->repeatInstances(sub)) {
        const std::string id(inst.id());
        const ElementRef e = ui.doc->byId(id.substr(0, id.rfind('/') + 1) + std::string(inner));
        out.emplace_back(e.valid() ? std::string(e.text()) : std::string("?"));
    }
    return out;
}

}  // namespace

TEST_CASE("T-391: list fields in structs, <repeat> inside a template, IR nesting and errors") {
    CompileResult r = compileSrc(page("", kNestPage));
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    Result<IrView> opened = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(opened.ok());
    const IrView& view = opened.value();
    REQUIRE(view.ports().nested().size() == 1);
    const IrPortNested& n = view.ports().nested()[0];
    CHECK(view.string(view.ports().fields()[n.field].name) == "rows");
    CHECK(static_cast<PortValueType>(view.ports().fields()[n.field].type) == PortValueType::List);
    CHECK(n.entryType == 0);
    REQUIRE(n.fieldCount == 1);
    CHECK(view.string(view.ports().fields()[n.fieldOffset].name) == "t");
    REQUIRE(view.repeats().repeats().size() == 2);
    const IrRepeat& inner = view.repeats().repeats()[1];
    CHECK(inner.flags == kRepeatItemField);
    CHECK(inner.listPort == 1);  // `rows` is the second field of the enclosing entry.
    IrWriter rebuilt = IrWriter::fromView(view);
    CHECK(rebuilt.finish() == r.ir);
    // IR validation: the nested record and the item-field row are checked.
    auto reopen = [&](auto mutate) {
        std::vector<uint8_t> bytes = r.ir;
        mutate(bytes);
        return IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{/*verifyCrc=*/false}).ok();
    };
    auto at = [&](const void* p) {
        return static_cast<size_t>(static_cast<const uint8_t*>(p) - r.ir.data());
    };
    CHECK(reopen([](std::vector<uint8_t>&) {}));
    CHECK(!reopen([&](std::vector<uint8_t>& b) {
        IrPortNested copy = n;
        copy.fieldOffset = copy.field;  // Not after the field: a cycle.
        std::memcpy(b.data() + at(&n), &copy, sizeof(copy));
    }));
    CHECK(!reopen([&](std::vector<uint8_t>& b) {
        IrRepeat copy = view.repeats().repeats()[0];
        copy.flags = kRepeatItemField;  // The outermost repeat has no enclosing entry.
        std::memcpy(b.data() + at(&view.repeats().repeats()[0]), &copy, sizeof(copy));
    }));
    CHECK(!reopen([&](std::vector<uint8_t>& b) {
        IrRepeat copy = inner;
        copy.listPort = 0;  // `name` is no list field.
        std::memcpy(b.data() + at(&inner), &copy, sizeof(copy));
    }));

    // Three levels, a primitive innermost list.
    CHECK(compileSrc(page("", R"X(
        <div id="P" port:l="list<struct{n: string, a: list<struct{m: string, b: list<string>}>}>"></div>
        <repeat port="l" id="L"><div id="X">
          <repeat port="item(a)" id="A"><div id="Y">
            <repeat port="item(b)" id="B"><span id="C" bind:text="format('{0}{1}', outer.item(m), item())"></span></repeat>
          </div></repeat>
        </div></repeat>)X"))
              .ok());

    const std::string decl =
        R"X(<div id="P" port:groups="list<struct{name: string, rows: list<struct{t: string}>}>"></div>)X";
    CHECK(compileError(compileSrc(page("", decl + R"X(<repeat port="groups" id="R"><div>
        <repeat port="item(rows)" id="S" virtualize><span></span></repeat></div></repeat>)X")),
                       "only supported on the outermost"));
    CHECK(compileError(compileSrc(page("", decl + R"X(<span bind:text="outer.index"></span>)X")),
                       "nested in another <repeat>"));
    CHECK(compileError(compileSrc(page("", decl + R"X(<repeat port="groups" id="R">
        <span bind:text="outer.index"></span></repeat>)X")),
                       "nested in another <repeat>"));
    CHECK(compileError(
        compileSrc(page("", decl + R"X(<repeat port="item(rows)" id="R"><span></span></repeat>)X")),
        "only valid inside another <repeat> template"));
    CHECK(compileError(compileSrc(page("", decl + R"X(<repeat port="groups" id="R"><div>
        <repeat port="item(name)" id="S"><span></span></repeat></div></repeat>)X")),
                       "is not a list"));
    CHECK(compileError(compileSrc(page("", decl + R"X(<repeat port="groups" id="R"><div>
        <repeat port="item(rows)" id="S"><span bind:text="outer.item(nope)"></span></repeat></div></repeat>)X")),
                       "no field \"nope\""));
    CHECK(compileError(compileSrc(page("", R"X(<div port:p="struct{a: list<struct{b: event}>}"></div>)X")),
                       "invalid struct field"));
}

TEST_CASE("T-391: nested instances, path ids, outer.index / outer.item and the port record") {
    Ui ui = open(page("", kNestPage), 400, 400);
    ui.setText("groups", kNestData);
    ui.step(0.016f);
    REQUIRE(ui.doc->repeatInstances("Rows").size() == 2);
    CHECK(cellTexts(ui, "Rows/0/Sub") == std::vector<std::string>{"0:0:a0", "0:1:a1", "0:2:a2"});
    CHECK(cellTexts(ui, "Rows/1/Sub") == std::vector<std::string>{"1:0:b0", "1:1:b1"});
    CHECK(ui.text("Rows/1/Name") == "B");
    CHECK(ui.text("Rows/0/Len") == "3");
    CHECK(ui.text("Rows/0/Sub/2/Cell") == "0:2:a2");
    CHECK(ui.text("Rows/1/Sub/0/Up") == "B");
    CHECK(ui.el("Rows/0/Sub").valid());  // The nested <repeat> element itself.
    // The port table describes the nested entry type.
    PortRecord rec = ui.doc->portInfo(ui.port("groups"));
    REQUIRE(rec.fieldLists.size() == 1);
    CHECK(rec.fieldLists[0].name == "rows");
    REQUIRE(rec.fieldLists[0].fields.size() == 1);
    CHECK(rec.fieldLists[0].fields[0].first == "t");
    // getPort returns the nested lists.
    Value v = ui.doc->getPort(ui.port("groups"));
    REQUIRE(v.type == ValueType::List);
    REQUIRE(v.itemCount == 2);
    REQUIRE(v.items[0].items[1].type == ValueType::List);
    CHECK(v.items[0].items[1].itemCount == 3);
    CHECK(ui.doc->string(v.items[1].items[1].items[1].items[0].sid) == "b1");
}

TEST_CASE("T-391: listUpdate rebinds the nested list in place; listRemove releases and shifts") {
    Ui ui = open(page("", kNestPage), 400, 400);
    ui.setText("groups", kNestData);
    ui.step(0.016f);
    NestBuilder nb{ui.doc.get()};
    const NodeId cell0 = ui.el("Rows/0/Sub/0/Cell").node;
    const NodeId row1Cell = ui.el("Rows/1/Sub/0/Cell").node;
    REQUIRE(ui.doc->listUpdate(ui.port("groups"), 0, nb.group("A2", {"x0", "x1", "x2", "x3"})) ==
            Status::kOk);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Rows/0/Sub") == std::vector<std::string>{"0:0:x0", "0:1:x1", "0:2:x2", "0:3:x3"});
    CHECK(ui.el("Rows/0/Sub/0/Cell").node == cell0);     // Rebound in place (index keyed).
    CHECK(ui.el("Rows/1/Sub/0/Cell").node == row1Cell);  // The other entry untouched.
    CHECK(ui.text("Rows/0/Sub/3/Up") == "A2");
    CHECK(ui.text("Rows/0/Sub/0/Up") == "A2");  // outer.item follows the update with an equal-length start.
    CHECK(ui.doc->lookupPort("Rows/1.rows") != kInvalidPort);

    REQUIRE(ui.doc->listRemove(ui.port("groups"), 0) == Status::kOk);
    ui.step(0.016f);
    REQUIRE(ui.doc->repeatInstances("Rows").size() == 1);
    CHECK(cellTexts(ui, "Rows/0/Sub") == std::vector<std::string>{"0:0:b0", "0:1:b1"});
    CHECK(ui.el("Rows/0/Sub/0/Cell").node == row1Cell);  // Renamed, not rebuilt.
    CHECK(ui.doc->repeatInstances("Rows/1/Sub").empty());
    CHECK(!ui.el("Rows/1/Sub/0/Cell").valid());
    CHECK(ui.doc->lookupPort("Rows/1.rows") == kInvalidPort);
    CHECK(ui.doc->lookupPort("Rows/0.rows") != kInvalidPort);
    CHECK(ui.text("Rows/0/Sub/1/Up") == "B");
}

TEST_CASE("T-391: the path list port Rows/1.rows takes the list API and writes back") {
    Ui ui = open(page("", kNestPage), 400, 400);
    ui.setText("groups", kNestData);
    ui.step(0.016f);
    NestBuilder nb{ui.doc.get()};
    std::vector<PortEvent> events;
    ui.doc->addPortListener([&](const PortEvent& ev) { events.push_back(ev); });
    const PortId rows1 = ui.port("Rows/1.rows");
    CHECK(ui.doc->portInfo(rows1).qualifiedName == "Rows/1.rows");
    REQUIRE(ui.doc->listInsert(rows1, 0, nb.row("bx")) == Status::kOk);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Rows/1/Sub") == std::vector<std::string>{"1:0:bx", "1:1:b0", "1:2:b1"});
    CHECK(ui.text("Rows/1/Len") == "3");  // The enclosing entry took the list back.
    Value v = ui.doc->getPort(ui.port("groups"));
    REQUIRE(v.itemCount == 2);
    CHECK(v.items[1].items[1].itemCount == 3);
    bool outerEvent = false;
    for (const PortEvent& ev : events) {
        outerEvent = outerEvent ||
                     (ev.port == ui.port("groups") && ev.kind == PortEventKind::ListChanged && ev.index == 1);
    }
    CHECK(outerEvent);
    REQUIRE(ui.doc->listRemove(rows1, 2) == Status::kOk);
    REQUIRE(ui.doc->setPortText(rows1, R"([{"t":"q"}])") == Status::kOk);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Rows/1/Sub") == std::vector<std::string>{"1:0:q"});
    CHECK(ui.doc->getPort(ui.port("groups")).items[1].items[1].itemCount == 1);
    // An outer insert shifts the path: the same nested list answers to Rows/2.rows.
    REQUIRE(ui.doc->listInsert(ui.port("groups"), 0, nb.group("Z", {})) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.doc->lookupPort("Rows/2.rows") == rows1);
    CHECK(cellTexts(ui, "Rows/2/Sub") == std::vector<std::string>{"2:0:q"});
    CHECK(ui.doc->repeatInstances("Rows/0/Sub").empty());
}

TEST_CASE("T-391: an emit from a nested instance carries the outermost entry; three levels") {
    Ui ui = open(page("", kNestPage), 400, 400);
    ui.setText("groups", kNestData);
    ui.step(0.016f);
    std::vector<PortEvent> emits;
    std::vector<std::string> hostIds;
    std::vector<uint32_t> hostItems;
    ui.doc->addPortListener([&](const PortEvent& ev) {
        if (ev.kind == PortEventKind::Emit) {
            emits.push_back(ev);
            REQUIRE(ev.item.type == ValueType::Struct);
            CHECK(ui.doc->string(ev.item.items[0].sid) == "B");
        }
    });
    ui.doc->setHostEventListener([&](const HostEvent& ev) {
        hostIds.emplace_back(ev.elementId);
        hostItems.push_back(ev.itemIndex);
    });
    const Rect box = ui.el("Rows/1/Sub/1/Cell").boundingBox();
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    ui.doc->dispatchInput(down);
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ui.doc->dispatchInput(up);
    REQUIRE(emits.size() == 1);
    CHECK(emits[0].itemIndex == 1);
    REQUIRE(hostItems.size() == 1);
    CHECK(hostItems[0] == 1);
    CHECK(hostIds[0] == "Rows/1/Sub/1/Cell");  // The inner index travels in the path id.

    Ui deep = open(page("", R"X(
        <div id="P" port:l="list<struct{n: string, a: list<struct{m: string, b: list<string>}>}>"></div>
        <repeat port="l" id="L"><div id="X">
          <repeat port="item(a)" id="A"><div id="Y">
            <repeat port="item(b)" id="B"><span id="C" bind:text="format('{0}{1}{2}', outer.item(m), outer.index, item())"></span></repeat>
          </div></repeat>
        </div></repeat>)X"));
    deep.setText("l", R"([{"n":"x","a":[{"m":"p","b":["1","2"]},{"m":"q","b":["3"]}]},{"n":"y","a":[]}])");
    deep.step(0.016f);
    CHECK(cellTexts(deep, "L/0/A/0/B", "C") == std::vector<std::string>{"p01", "p02"});
    CHECK(cellTexts(deep, "L/0/A/1/B", "C") == std::vector<std::string>{"q13"});
    CHECK(deep.text("L/0/A/1/B/0/C") == "q13");
    CHECK(deep.doc->repeatInstances("L/1/A").empty());
    // The innermost list by its path: L/0/A/1.b, written back two levels up.
    const PortId b = deep.port("L/0/A/1.b");
    REQUIRE(deep.doc->setPortText(b, R"(["7","8"])") == Status::kOk);
    deep.step(0.016f);
    CHECK(cellTexts(deep, "L/0/A/1/B", "C") == std::vector<std::string>{"q17", "q18"});
    Value v = deep.doc->getPort(deep.port("l"));
    REQUIRE(v.itemCount == 2);
    CHECK(v.items[0].items[1].items[1].items[1].itemCount == 2);
}

TEST_CASE("T-391: symbol instance ports inside a nested template; a shared document list") {
    Ui ui = open(
        R"X(<xt><head><symbol name="Tag"><span id="T" port:label="string in" bind:text="label"></span></symbol>
        </head><body id="Root">
        <div id="P" port:groups="list<struct{name: string, rows: list<struct{t: string}>}>" port:shared="list<string>"></div>
        <repeat port="groups" id="Rows"><div id="Row">
          <repeat port="item(rows)" id="Sub"><use symbol="Tag" id="Cell" bind:label="item(t)"></use></repeat>
          <repeat port="shared" id="Sh"><span id="S" bind:text="format('{0}{1}', outer.item(name), item())"></span></repeat>
        </div></repeat></body></xt>)X");
    ui.setText("groups", kNestData);
    ui.setText("shared", R"(["!","?"])");
    ui.step(0.016f);
    CHECK(ui.text("Rows/1/Sub/1/Cell/T") == "b1");
    const PortId label = ui.port("Rows/1/Sub/1/Cell.label");
    REQUIRE(ui.doc->setPortText(label, "host") == Status::kOk);
    CHECK(ui.text("Rows/1/Sub/1/Cell/T") == "host");
    CHECK(cellTexts(ui, "Rows/0/Sh", "S") == std::vector<std::string>{"A!", "A?"});
    CHECK(cellTexts(ui, "Rows/1/Sh", "S") == std::vector<std::string>{"B!", "B?"});
    ui.setText("shared", R"(["#"])");
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Rows/0/Sh", "S") == std::vector<std::string>{"A#"});
    CHECK(cellTexts(ui, "Rows/1/Sh", "S") == std::vector<std::string>{"B#"});
}

TEST_CASE("T-391: a virtualized outer list recycles its rows with their nested lists; item-exit") {
    Ui ui = open(page("", R"X(
        <div id="P" port:items="list<struct{name: string, tags: list<string>}>"></div>
        <div id="Scroll" style="position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: scroll">
          <repeat port="items" id="Item" virtualize>
            <div id="Row" style="height: 20px; display: flex">
              <repeat port="item(tags)" id="Tags"><span id="Tag" bind:text="format('{0}-{1}', outer.item(name), item())"></span></repeat>
            </div>
          </repeat>
        </div>)X"));
    std::string json = "[";
    for (int i = 0; i < 200; ++i) {
        json += (i > 0 ? "," : "") + std::string("{\"name\":\"R") + std::to_string(i) +
                "\",\"tags\":[\"a\",\"b" + std::to_string(i) + "\"]}";
    }
    ui.setText("items", json + "]");
    ui.step(0.016f);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Item/0/Tags", "Tag") == std::vector<std::string>{"R0-a", "R0-b0"});
    scrollTo(ui, 1000);
    const uint32_t alive = ui.doc->arena().aliveCount();
    for (int i = 0; i < 6; ++i) {
        scrollTo(ui, i % 2 == 0 ? 3000.0f : 400.0f);
    }
    scrollTo(ui, 1000);
    CHECK(ui.doc->arena().aliveCount() <= alive + 30u * 8u);  // Pooled rows drop their nested instances.
    CHECK(cellTexts(ui, "Item/50/Tags", "Tag") == std::vector<std::string>{"R50-a", "R50-b50"});
    scrollTo(ui, 3500);
    CHECK(cellTexts(ui, "Item/180/Tags", "Tag") == std::vector<std::string>{"R180-a", "R180-b180"});
    CHECK(ui.doc->lookupPort("Item/180.tags") != kInvalidPort);
    CHECK(ui.doc->lookupPort("Item/50.tags") == kInvalidPort);  // Not instantiated (or pooled).

    // item-exit on the outer repeat: the exiting row keeps its nested list until it goes.
    Ui ex = open(page("@timeline out { duration: 0.2s; #Row { 0s { opacity: 1 } 0.2s { opacity: 0 } } }", R"X(
        <div id="P" port:items="list<struct{name: string, tags: list<string>}>"></div>
        <repeat port="items" id="Item" item-exit="play(out) then remove">
          <div id="Row"><repeat port="item(tags)" id="Tags"><span id="Tag" bind:text="item()"></span></repeat></div>
        </repeat>)X"));
    ex.setText("items", R"([{"name":"A","tags":["x","y"]},{"name":"B","tags":["z"]}])");
    ex.step(0.016f);
    REQUIRE(ex.doc->listRemove(ex.port("items"), 0) == Status::kOk);
    ex.step(0.05f);
    CHECK(cellTexts(ex, "Item/0/Tags", "Tag") == std::vector<std::string>{"z"});
    CHECK(ex.doc->lookupPort("Item/0.tags") != kInvalidPort);
    ex.step(0.3f);
    ex.step(0.016f);
    REQUIRE(ex.doc->repeatInstances("Item").size() == 1);
    CHECK(cellTexts(ex, "Item/0/Tags", "Tag") == std::vector<std::string>{"z"});
}

// -- T-399 (08 §8.18, XF-098): a symbol's list in-port <repeat> inside an outer template --------

namespace {

const char* kSymListPage = R"X(<xt><head>
    <symbol name="Row"><div id="Box" port:rows="list<struct{t: string}> in">
      <repeat port="rows" id="Sub"><span id="X" bind:text="item(t)"></span></repeat>
    </div></symbol></head>
  <body id="Root">
    <div id="P" port:items="list<struct{name: string, rows: list<struct{t: string}>}>"></div>
    <repeat port="items" id="Rows"><use symbol="Row" id="Cell" bind:rows="item(rows)"></use></repeat>
  </body></xt>)X";

}  // namespace

TEST_CASE("T-399: a symbol's list in-port repeat inside an outer template reads its instance copy") {
    Ui ui = open(kSymListPage, 400, 400);
    ui.setText("items", kNestData);
    ui.step(0.016f);
    REQUIRE(ui.doc->repeatInstances("Rows").size() == 2);
    CHECK(cellTexts(ui, "Rows/0/Cell/Sub", "X") == std::vector<std::string>{"a0", "a1", "a2"});
    CHECK(cellTexts(ui, "Rows/1/Cell/Sub", "X") == std::vector<std::string>{"b0", "b1"});
    CHECK(ui.text("Rows/1/Cell/Sub/1/X") == "b1");
    CHECK(ui.el("Rows/1/Cell/Sub/1/X").valid());
    CHECK(!ui.el("Cell/Sub/0/X").valid());  // The template's own copy is no instance.
    CHECK(ui.doc->repeatInstances("Cell/Sub").empty());

    NestBuilder nb{ui.doc.get()};
    const NodeId first = ui.el("Rows/0/Cell/Sub/0/X").node;
    const NodeId other = ui.el("Rows/1/Cell/Sub/0/X").node;
    REQUIRE(ui.doc->listUpdate(ui.port("items"), 0, nb.group("A2", {"x0", "x1", "x2", "x3"})) == Status::kOk);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Rows/0/Cell/Sub", "X") == std::vector<std::string>{"x0", "x1", "x2", "x3"});
    CHECK(ui.el("Rows/0/Cell/Sub/0/X").node == first);  // Rebound in place.
    CHECK(ui.el("Rows/1/Cell/Sub/0/X").node == other);

    REQUIRE(ui.doc->listRemove(ui.port("items"), 0) == Status::kOk);
    ui.step(0.016f);
    REQUIRE(ui.doc->repeatInstances("Rows").size() == 1);
    CHECK(cellTexts(ui, "Rows/0/Cell/Sub", "X") == std::vector<std::string>{"b0", "b1"});
    CHECK(ui.el("Rows/0/Cell/Sub/0/X").node == other);  // Renamed, not rebuilt.
    CHECK(!ui.el("Rows/1/Cell/Sub/0/X").valid());
    CHECK(ui.doc->repeatInstances("Rows/1/Cell/Sub").empty());
}

TEST_CASE("T-399: the instance's list in-port by its path id; symbol in a nested repeat; nested inside") {
    Ui ui = open(kSymListPage, 400, 400);
    ui.setText("items", kNestData);
    ui.step(0.016f);
    // The host writes one outer instance's copy directly.
    const PortId rows1 = ui.port("Rows/1/Cell.rows");
    CHECK(rows1 != ui.port("Rows/0/Cell.rows"));
    REQUIRE(ui.doc->setPortText(rows1, R"([{"t":"h0"}])") == Status::kOk);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Rows/1/Cell/Sub", "X") == std::vector<std::string>{"h0"});
    CHECK(cellTexts(ui, "Rows/0/Cell/Sub", "X") == std::vector<std::string>{"a0", "a1", "a2"});
    // An outer insert shifts the later instances' ids; each keeps its own list.
    NestBuilder nb{ui.doc.get()};
    REQUIRE(ui.doc->listInsert(ui.port("items"), 0, nb.group("N", {"n0"})) == Status::kOk);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Rows/0/Cell/Sub", "X") == std::vector<std::string>{"n0"});
    CHECK(cellTexts(ui, "Rows/1/Cell/Sub", "X") == std::vector<std::string>{"a0", "a1", "a2"});
    // The moved instance's binding follows its (re-pointed) entry again: the host's write gives way.
    CHECK(cellTexts(ui, "Rows/2/Cell/Sub", "X") == std::vector<std::string>{"b0", "b1"});
    CHECK(ui.text("Rows/2/Cell/Sub/1/X") == "b1");

    // The symbol inside a nested repeat (the list two levels down), and a nested item field
    // repeat inside the symbol's own repeat.
    Ui deep = open(R"X(<xt><head>
        <symbol name="Row"><div id="Box" port:rows="list<struct{t: string, k: list<string>}> in">
          <repeat port="rows" id="Sub"><div id="Y"><span id="X" bind:text="item(t)"></span>
            <repeat port="item(k)" id="K"><span id="Z" bind:text="format('{0}{1}', outer.item(t), item())"></span></repeat>
          </div></repeat>
        </div></symbol></head>
      <body id="Root">
        <div id="P" port:l="list<struct{a: list<struct{rows: list<struct{t: string, k: list<string>}>}>}>"></div>
        <repeat port="l" id="L"><div id="W">
          <repeat port="item(a)" id="A"><use symbol="Row" id="Cell" bind:rows="item(rows)"></use></repeat>
        </div></repeat>
      </body></xt>)X",
                   400, 400);
    deep.setText(
        "l",
        R"([{"a":[{"rows":[{"t":"p","k":["1","2"]}]},{"rows":[{"t":"q","k":[]},{"t":"r","k":["3"]}]}]}])");
    deep.step(0.016f);
    CHECK(cellTexts(deep, "L/0/A/0/Cell/Sub", "X") == std::vector<std::string>{"p"});
    CHECK(cellTexts(deep, "L/0/A/1/Cell/Sub", "X") == std::vector<std::string>{"q", "r"});
    CHECK(cellTexts(deep, "L/0/A/0/Cell/Sub/0/K", "Z") == std::vector<std::string>{"p1", "p2"});
    CHECK(cellTexts(deep, "L/0/A/1/Cell/Sub/1/K", "Z") == std::vector<std::string>{"r3"});
    CHECK(deep.doc->repeatInstances("L/0/A/1/Cell/Sub/0/K").empty());
    deep.setText("l", R"([{"a":[{"rows":[{"t":"s","k":["9"]}]}]}])");
    deep.step(0.016f);
    CHECK(cellTexts(deep, "L/0/A/0/Cell/Sub/0/K", "Z") == std::vector<std::string>{"s9"});
    CHECK(deep.doc->repeatInstances("L/0/A/1/Cell/Sub").empty());
}

TEST_CASE("T-399: a virtualized outer list recycles symbol rows with their list in-port repeats") {
    Ui ui = open(R"X(<xt><head>
        <symbol name="Row"><div id="Box" style="height: 20px; display: flex" port:tags="list<string> in">
          <repeat port="tags" id="Tags"><span id="Tag" bind:text="item()"></span></repeat>
        </div></symbol></head>
      <body id="Root">
        <div id="P" port:items="list<struct{name: string, tags: list<string>}>"></div>
        <div id="Scroll" style="position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: scroll">
          <repeat port="items" id="Item" virtualize><use symbol="Row" id="Cell" bind:tags="item(tags)"></use></repeat>
        </div></body></xt>)X");
    std::string json = "[";
    for (int i = 0; i < 200; ++i) {
        json += (i > 0 ? "," : "") + std::string("{\"name\":\"R") + std::to_string(i) +
                "\",\"tags\":[\"a\",\"b" + std::to_string(i) + "\"]}";
    }
    ui.setText("items", json + "]");
    ui.step(0.016f);
    ui.step(0.016f);
    CHECK(cellTexts(ui, "Item/0/Cell/Tags", "Tag") == std::vector<std::string>{"a", "b0"});
    for (int i = 0; i < 4; ++i) {
        scrollTo(ui, i % 2 == 0 ? 3000.0f : 400.0f);
    }
    scrollTo(ui, 1000);
    CHECK(cellTexts(ui, "Item/50/Cell/Tags", "Tag") == std::vector<std::string>{"a", "b50"});
    scrollTo(ui, 3500);
    CHECK(cellTexts(ui, "Item/180/Cell/Tags", "Tag") == std::vector<std::string>{"a", "b180"});
}

TEST_CASE("T-546: a repeat instance's tween binding applies its first value directly; later updates tween") {
    Ui ui = open(page(".cell { height: 20px; } #Cell { width: 10px; }",
                      R"X(<div id="P" port:items="list<struct{name: string, count: number}>"></div>
    <repeat port="items" id="Item">
      <div id="Cell" class="cell" bind:width="item(count) | tween(1s)"></div>
    </repeat>)X"));
    ui.setText("items", R"([{"name":"A","count":100},{"name":"B","count":40}])");
    ui.step(0.016f);
    CHECK(ui.el("Item/0/Cell").layoutBox().width == doctest::Approx(100.0f));  // Not 10 -> 100.
    CHECK(ui.el("Item/1/Cell").layoutBox().width == doctest::Approx(40.0f));
    // The first update after creation is direct, the second one tweens.
    REQUIRE(ui.doc->listUpdate(ui.port("items"), 0, ui.entry("A", 0)) == Status::kOk);
    ui.step(0.0f);
    ui.step(0.016f);
    CHECK(ui.el("Item/0/Cell").layoutBox().width == doctest::Approx(0.0f));
    REQUIRE(ui.doc->listUpdate(ui.port("items"), 0, ui.entry("A", 100)) == Status::kOk);
    ui.step(0.0f);
    ui.step(0.5f);
    CHECK(ui.el("Item/0/Cell").layoutBox().width == doctest::Approx(50.0f).epsilon(0.05));
    ui.step(1.0f);
    CHECK(ui.el("Item/0/Cell").layoutBox().width == doctest::Approx(100.0f));
    // A new instance appended later is direct too.
    REQUIRE(ui.doc->listInsert(ui.port("items"), 2, ui.entry("C", 70)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.el("Item/2/Cell").layoutBox().width == doctest::Approx(70.0f));
}
