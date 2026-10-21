#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "binding_runtime.h"  // core/src (on the test include path): DocumentTestAccess below.
#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/rig.h"
#include "xt/types.h"

/// T-260b: Document::remove() tells the repeat controller and the rig state about the removed
/// subtree, `<repeat>` list changes made from inside an instance binding, the listener pool
/// compaction during a dispatch, and the Context state a freed handle leaves behind
/// (docs/reviews/2026-10-02-full-audit-raw/C.md C3, C10, C14; B.md B9; docs/tasks/T-260b.md).

using namespace xt;
using namespace xt::compile;

namespace xt {

/// Document's friend for tests: reads the private runtimes' counters.
struct DocumentTestAccess {
    static size_t liveInstanceBindings(const Document& d) { return d.m_bindings->liveInstanceBindings(); }
};

}  // namespace xt

namespace {

std::string page(std::string_view style, std::string_view body, std::string_view bodyAttrs = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\" " +
           std::string(bodyAttrs) + ">" + std::string(body) + "</body></xt>";
}

CompileResult compileXt(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileResult r = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    return r;
}

IrBlob blobOf(const CompileResult& r) { return IrBlob{r.ir.data(), r.ir.size()}; }

/// A standalone Document (no Context) and its clock.
struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;
    float clock = 0;
    StringId names[1] = {};
    std::vector<std::vector<Value>> entries;

    void step(float dt) {
        clock += dt;
        doc->update(clock);
    }
    PortId port(std::string_view name) const {
        const PortId p = doc->lookupPort(name);
        REQUIRE(p != kInvalidPort);
        return p;
    }
    /// A `{name}` entry (its storage lives as long as this Ui).
    Value entry(std::string_view name) {
        names[0] = doc->intern("name");
        entries.push_back({Value::ofStringId(doc->intern(name))});
        return Value::ofStruct(names, entries.back().data(), 1);
    }
    std::vector<std::string> texts(std::string_view repeat, std::string_view inner) {
        std::vector<std::string> out;
        for (const ElementRef& inst : doc->repeatInstances(repeat)) {
            const std::string id(inst.id());
            const std::string prefix = id.substr(0, id.find('/', id.find('/') + 1) + 1);
            const ElementRef e = doc->byId(prefix + std::string(inner));
            out.emplace_back(e.valid() ? std::string(e.text()) : std::string("?"));
        }
        return out;
    }
};

Ui open(const std::string& xt, float w = 400, float h = 200) {
    Ui out;
    out.result = compileXt(xt);
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(blobOf(out.result), false, nullptr, nullptr, out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, w, h});
    out.doc->update(0.0f);
    return out;
}

constexpr const char* kList = R"X(
    <div id="P" port:items="list<struct{name: string}>"></div>
    <div id="List">
      <repeat port="items" id="Item">
        <div id="Cell"><span id="Name" bind:text="item(name)"></span></div>
      </repeat>
    </div>)X";

}  // namespace

// -- C3: a removed instance / repeat element ------------------------------------------------------

TEST_CASE("removal: the host removes a repeat instance; the next listInsert anchors on live nodes only") {
    Ui ui = open(page(".Cell { height: 20px; }", kList));
    const PortId items = ui.port("items");
    REQUIRE(ui.doc->setPortText(items, R"([{"name":"A"},{"name":"B"}])") == Status::kOk);
    ui.step(0.016f);
    std::span<const ElementRef> inst = ui.doc->repeatInstances("Item");
    REQUIRE(inst.size() == 2);
    const ElementRef first = inst[0];
    ui.doc->remove(first);
    CHECK(ui.doc->repeatInstances("Item").size() == 1);
    // Re-points the removed instance to entry 1 and anchors the new one before it (C3).
    REQUIRE(ui.doc->listInsert(items, 0, ui.entry("X")) == Status::kOk);
    ui.step(0.016f);
    // The list is the truth: entry 1 ("A") gets a fresh instance.
    CHECK(ui.texts("Item", "Name") == std::vector<std::string>{"X", "A", "B"});
    REQUIRE(ui.doc->listRemove(items, 1) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.texts("Item", "Name") == std::vector<std::string>{"X", "B"});
}

TEST_CASE("removal: the host removes an ancestor of a <repeat>; later list changes are ignored") {
    Ui ui = open(page(".Cell { height: 20px; }", kList));
    const PortId items = ui.port("items");
    REQUIRE(ui.doc->setPortText(items, R"([{"name":"A"},{"name":"B"}])") == Status::kOk);
    ui.step(0.016f);
    const size_t before = DocumentTestAccess::liveInstanceBindings(*ui.doc);
    CHECK(before == 2);
    ui.doc->remove(ui.doc->byId("List"));
    CHECK(DocumentTestAccess::liveInstanceBindings(*ui.doc) == 0);
    // Would attach new instances to the freed `<repeat>` slot (C3).
    REQUIRE(ui.doc->setPortText(items, R"([{"name":"A"},{"name":"B"},{"name":"C"}])") == Status::kOk);
    REQUIRE(ui.doc->listInsert(items, 0, ui.entry("X")) == Status::kOk);
    REQUIRE(ui.doc->listRemove(items, 2) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.doc->repeatInstances("Item").empty());
    CHECK(DocumentTestAccess::liveInstanceBindings(*ui.doc) == 0);
    // New elements reuse the freed slots; none of them is mistaken for the repeat.
    for (int i = 0; i < 16; ++i) {
        ElementRef e = ui.doc->createElement("div");
        ui.doc->appendChild(ui.doc->byId("Root"), e);
    }
    REQUIRE(ui.doc->listInsert(items, 0, ui.entry("Y")) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.doc->repeatInstances("Item").empty());
}

// -- C14: a removed spawn instance gives its bindings back ----------------------------------------

TEST_CASE("removal: a spawned element removed before it expires leaves no instance binding behind") {
    Ui ui = open(R"X(<xt><head>
      <symbol name="Pop"><div id="Box"><span id="Amount" bind:text="amount"></span></div></symbol>
      <style>
        #Field { position: absolute; left: 20px; top: 20px; width: 100px; height: 50px; }
        symbol(Pop) { #Box { width: 10px; height: 10px; } }
      </style>
    </head><body id="Root" port:amount="number in" port:hit="event in">
      <div id="Field" on:port(hit)="spawn(Pop, at=self, lifetime=0.5s)"></div>
    </body></xt>)X");
    const size_t base = DocumentTestAccess::liveInstanceBindings(*ui.doc);
    REQUIRE(ui.doc->setPort(ui.port("amount"), Value::ofFloat(7)) == Status::kOk);
    REQUIRE(ui.doc->setPort(ui.port("hit"), Value::ofFloat(0)) == Status::kOk);
    ui.step(0.01f);
    std::span<const ElementRef> live = ui.doc->repeatInstances("spawn-0");
    REQUIRE(live.size() == 1);
    CHECK(DocumentTestAccess::liveInstanceBindings(*ui.doc) == base + 1);
    ui.doc->remove(live[0]);
    ui.step(0.6f);  // Past its lifetime: expireSpawned() finds it gone.
    CHECK(ui.doc->repeatInstances("spawn-0").empty());
    CHECK(DocumentTestAccess::liveInstanceBindings(*ui.doc) == base);
    // The pool still works.
    REQUIRE(ui.doc->setPort(ui.port("hit"), Value::ofFloat(0)) == Status::kOk);
    ui.step(0.01f);
    CHECK(ui.doc->repeatInstances("spawn-0").size() == 1);
    CHECK(DocumentTestAccess::liveInstanceBindings(*ui.doc) == base + 1);
}

// -- C10: a binding's callback changes the same list ----------------------------------------------

TEST_CASE("reentrancy: an instance binding's Change listener inserts into the same list") {
    Ui ui = open(page("", R"X(
        <div id="P" port:items="list<struct{name: string}>"></div>
        <repeat port="items" id="Item">
          <div id="Cell"><input id="Field" type="text" bind:text="format('{0}/{1}', index, count)"/></div>
        </repeat>)X"));
    const PortId items = ui.port("items");
    REQUIRE(ui.doc->setPortText(items, R"([{"name":"A"},{"name":"B"}])") == Status::kOk);
    ui.step(0.016f);
    REQUIRE(ui.texts("Item", "Field") == std::vector<std::string>{"0/2", "1/2"});
    const NodeId fieldB = ui.doc->byId("Item/1/Field").node;
    REQUIRE(fieldB != kInvalidNode);
    int nested = 0;
    // B's input changes ("1/2" -> "1/3") while the outer insert re-evaluates every live instance;
    // its listener inserts at the front, which moves every Instance of that repeat.
    ui.doc->addListener(ui.doc->byId("Root"), EventType::Change, [&](Event& e) {
        if (e.target != fieldB || nested > 0) {
            return;
        }
        ++nested;
        CHECK(ui.doc->listInsert(items, 0, ui.entry("N")) == Status::kOk);
    });
    REQUIRE(ui.doc->listInsert(items, 2, ui.entry("X")) == Status::kOk);
    CHECK(nested == 1);
    ui.step(0.016f);
    CHECK(ui.texts("Item", "Field") == std::vector<std::string>{"0/4", "1/4", "2/4", "3/4"});
    CHECK(DocumentTestAccess::liveInstanceBindings(*ui.doc) == 4);
}

// -- B9: a removed skeleton ------------------------------------------------------------------------

TEST_CASE("removal: removing the <svg> holding a skeleton drops its rig; new elements do not revive it") {
    Ui ui = open(page("svg { position: absolute; left: 20px; top: 10px; width: 200px; height: 200px; }",
                      R"X(<div port:rb="number"></div>
<svg id="S" viewBox="0 0 200 200" width="200" height="200">
  <path id="Box" d="M50 90 L150 90 L150 110 L50 110 Z" fill="#ff0000" xt-skin="Rig">
    <xt-weights bones="A B">0 0:1
1 1:1
2 1:1
3 0:1</xt-weights></path>
  <xt-skeleton id="Rig">
    <xt-bone id="A" length="50" translate="50 100"><xt-bone id="B" length="50" bind:rotate="rb"/></xt-bone>
  </xt-skeleton>
</svg>)X"));
    ui.step(0.016f);
    REQUIRE(ui.doc->rigCount() == 1);
    const NodeId skeleton = ui.doc->rigAt(0).skeleton;
    REQUIRE(skeleton != kInvalidNode);
    ui.doc->remove(ui.doc->byId("S"));
    CHECK(ui.doc->rigCount() == 0);
    // Grow the tree until the skeleton's slot is reused (B9: alive(rs.skeleton) is true again).
    for (int i = 0; i < 64 && !ui.doc->arena().alive(skeleton); ++i) {
        ElementRef e = ui.doc->createElement("div");
        ui.doc->appendChild(ui.doc->byId("Root"), e);
    }
    ui.step(0.016f);
    REQUIRE(ui.doc->setPort(ui.port("rb"), Value::ofFloat(30.0f)) == Status::kOk);
    ui.step(0.016f);
    CHECK(ui.doc->rigCount() == 0);
}

// -- Listener pool compaction during a dispatch ---------------------------------------------------

TEST_CASE("reentrancy: a click listener removes 1100 listeners; the rest of that dispatch is unaffected") {
    Ui ui =
        open(page("#P { position: absolute; left: 0px; top: 0px; width: 100px; height: 60px; }"
                  "#A { width: 40px; height: 40px; } #Q { width: 10px; height: 10px; }",
                  R"X(<div id="P"><div id="A"></div></div><div id="Q"></div>)X"));
    ElementRef a = ui.doc->byId("A");
    ElementRef q = ui.doc->byId("Q");
    std::vector<uint32_t> spare;
    for (int i = 0; i < 1100; ++i) {
        spare.push_back(ui.doc->addListener(q, EventType::Click, [](Event&) {}));
    }
    std::vector<std::string> heard;
    ui.doc->addListener(a, EventType::Click, [&](Event&) {
        heard.emplace_back("A1");
        for (uint32_t id : spare) {
            ui.doc->removeListener(id);  // Crosses the 1024-removal compaction threshold.
        }
    });
    ui.doc->addListener(a, EventType::Click, [&](Event&) { heard.emplace_back("A2"); });
    ui.doc->addListener(a, EventType::Click, [&](Event&) { heard.emplace_back("A3"); });
    ui.doc->addListener(ui.doc->byId("P"), EventType::Click, [&](Event&) { heard.emplace_back("P"); });
    ui.doc->addListener(ui.doc->byId("Root"), EventType::Click, [&](Event&) { heard.emplace_back("Root"); });
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{10, 10};
    down.button = PointerButton::Left;
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ui.doc->dispatchInput(down);
    ui.doc->dispatchInput(up);
    CHECK(heard == std::vector<std::string>{"A1", "A2", "A3", "P", "Root"});
    heard.clear();
    ui.doc->dispatchInput(down);
    ui.doc->dispatchInput(up);
    CHECK(heard == std::vector<std::string>{"A1", "A2", "A3", "P", "Root"});
}

// -- Context: a freed handle leaves no state behind -----------------------------------------------

namespace {

constexpr const char* kBoxStyle =
    "#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }";

/// Runs the test's callbacks from inside the Context's dispatch.
struct HostListener : ContextListener {
    std::function<void(const HostEvent&)> host;
    std::function<void(DocumentHandle)> shown;
    void onHostEvent(const HostEvent& e) override {
        if (host) {
            host(e);
        }
    }
    void onShown(DocumentHandle d) override {
        if (shown) {
            shown(d);
        }
    }
};

void pushPointer(Context& ctx, InputEvent::Kind kind, float x, float y) {
    InputEvent ev;
    ev.kind = kind;
    ev.pos = Vec2{x, y};
    ev.button = PointerButton::Left;
    ctx.pushInput(ev);
}

/// Records (into `*seen`) whether `ctx` still hands out `handle` while a Document is destroyed.
struct DestroyProbe {
    Context* ctx = nullptr;
    DocumentHandle handle = 0;
    bool* seen = nullptr;
    ~DestroyProbe() {
        if (ctx != nullptr) {
            *seen = *seen || ctx->document(handle) != nullptr;
        }
    }
};

}  // namespace

TEST_CASE("reentrancy: a surface unloaded from its press listener leaves no capture, hover or z") {
    CompileResult r = compileXt(page(kBoxStyle, "<div id=\"A\"></div>"));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> under = ctx.loadDocument(blobOf(r), false);
    Result<DocumentHandle> top = ctx.loadDocument(blobOf(r), false);
    REQUIRE(under.ok());
    REQUIRE(top.ok());
    REQUIRE(ctx.setSurfaceZ(top.value(), 5) == Status::kOk);
    REQUIRE(ctx.setSurfaceInput(top.value(), SurfaceInput::Pass) == Status::kOk);
    Document* topDoc = ctx.document(top.value());
    Document* underDoc = ctx.document(under.value());
    topDoc->addListener(topDoc->byId("A"), EventType::PointerDown,
                        [&](Event&) { ctx.unloadDocument(top.value()); });
    int underMoves = 0;
    underDoc->addListener(underDoc->byId("A"), EventType::PointerMove, [&](Event&) { ++underMoves; });
    ctx.update(0.0f);
    pushPointer(ctx, InputEvent::PointerMove, 20, 20);  // Hover: the top surface.
    pushPointer(ctx, InputEvent::PointerDown, 20, 20);  // Capture and keyboard: the top surface.
    ctx.update(0.016f);
    CHECK(ctx.document(top.value()) == nullptr);
    CHECK(ctx.surfaceZ(top.value()) == 0);
    CHECK(ctx.surfaceInput(top.value()) == SurfaceInput::Block);
    CHECK(underMoves == 0);
    pushPointer(ctx, InputEvent::PointerMove, 21, 21);
    pushPointer(ctx, InputEvent::PointerUp, 21, 21);
    ctx.update(0.016f);
    CHECK(underMoves == 1);  // The document underneath takes the pointer.
    CHECK(ctx.cursor() == Cursor::Default);
}

TEST_CASE("reentrancy: reloadDocument then unloadDocument of one handle in a callback drops the reload") {
    CompileResult r = compileXt(page(kBoxStyle, R"X(<div id="A" on:click="emit(go)"></div>)X"));
    CompileResult next = compileXt(page(kBoxStyle, "<div id=\"A\"></div>", "on:show=\"emit(shown)\""));
    REQUIRE(r.ok());
    REQUIRE(next.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
    REQUIRE(h.ok());
    const DocumentHandle handle = h.value();
    bool handedOutWhileDestroyed = false;
    {
        // Lives in the old Document's listener table: its destructor runs when that Document goes.
        auto probe = std::make_shared<DestroyProbe>();
        probe->ctx = &ctx;
        probe->handle = handle;
        probe->seen = &handedOutWhileDestroyed;
        Document* old = ctx.document(handle);
        old->addListener(old->byId("A"), EventType::Focus, [probe](Event&) {});
    }
    HostListener listener;
    std::vector<std::string> events;
    std::vector<DocumentHandle> shownHandles;
    Status reloadStatus = Status::kInternal;
    listener.host = [&](const HostEvent& e) {
        events.emplace_back(e.name);
        if (e.name == "go") {
            reloadStatus = ctx.reloadDocument(handle, blobOf(next), false).status();
            ctx.unloadDocument(handle);
        }
    };
    listener.shown = [&](DocumentHandle d) { shownHandles.push_back(d); };
    ctx.addListener(&listener);
    ctx.update(0.0f);
    shownHandles.clear();
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{20, 20};
    down.button = PointerButton::Left;
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ctx.pushInput(down);
    ctx.pushInput(up);
    ctx.update(0.016f);
    CHECK(reloadStatus == Status::kOk);
    CHECK(ctx.document(handle) == nullptr);
    CHECK_FALSE(handedOutWhileDestroyed);  // The replacement was never installed.
    ctx.update(0.016f);
    CHECK(events == std::vector<std::string>{"go"});
    CHECK(shownHandles.empty());
    ctx.removeListener(&listener);
}

// -- Caller audit: the shell's "current document" switched from a callback ------------------------

TEST_CASE("caller audit: an App-style current handle switched (unload, check, load) from a callback") {
    // shell/src/app.cpp keeps one DocumentHandle and resolves it with Context::document() every
    // frame (presentSw / presentGl); the host switches it with App::setCurrentDocument().
    CompileResult first = compileXt(page(kBoxStyle, R"X(<div id="A" on:click="emit(next)"></div>)X"));
    CompileResult second = compileXt(page(kBoxStyle, "<div id=\"A\"></div>", "on:show=\"class(shown)\""));
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(blobOf(first), false);
    REQUIRE(h.ok());
    DocumentHandle current = h.value();
    HostListener listener;
    std::vector<bool> goneAtOnce;
    listener.host = [&](const HostEvent& e) {
        if (e.name != "next") {
            return;
        }
        const DocumentHandle old = current;
        ctx.unloadDocument(old);
        goneAtOnce.push_back(ctx.document(old) == nullptr);
        Result<DocumentHandle> n = ctx.loadDocument(blobOf(second), false);
        REQUIRE(n.ok());
        current = n.value();  // App::setCurrentDocument(n.value()).
    };
    ctx.addListener(&listener);
    ctx.update(0.0f);
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{20, 20};
    down.button = PointerButton::Left;
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ctx.pushInput(down);
    ctx.pushInput(up);
    ctx.update(0.016f);
    REQUIRE(goneAtOnce == std::vector<bool>{true});
    CHECK(ctx.document(h.value()) == nullptr);
    Document* doc = ctx.document(current);  // What the next present reads.
    REQUIRE(doc != nullptr);
    CHECK(doc->ir().doc().elements().size() > 0);
    ctx.update(0.016f);
    CHECK(ctx.document(current)->byId("Root").hasClass("shown"));
    ctx.removeListener(&listener);
}
