#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/types.h"

/// Core gaps found by the standard symbol library (Docs/tasks/T-201.md, Assets/Symbols/README.md):
/// symbol-scope timeline triggers, `set()` / `spawn()`, path-id selectors, `<link>` resolution,
/// layout fixes, transform bindings, focus-trap markup, targeted `state()`, slider drag.

using namespace xt;
using namespace xt::compile;

namespace {

CompileResult compileXt(const std::string& xt) {
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

struct Loaded {
    CompileResult result;
    std::unique_ptr<Document> doc;
    float clock = 0;

    void step(float dt) {
        clock += dt;
        doc->update(clock);
    }
    void pointer(InputEvent::Kind kind, float x, float y) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = Vec2{x, y};
        ev.button = PointerButton::Left;
        doc->dispatchInput(ev);
    }
    void click(float x, float y) {
        pointer(InputEvent::PointerDown, x, y);
        pointer(InputEvent::PointerUp, x, y);
    }
    ElementRef el(std::string_view id) const { return doc->byId(id); }
    bool has(std::string_view id, std::string_view state) const { return doc->hasCustomState(el(id), state); }
    LayoutBox box(std::string_view id) const { return el(id).layoutBox(); }
    Rect bounds(std::string_view id) const { return el(id).boundingBox(); }
};

Loaded load(const std::string& xt, float w = 200, float h = 100) {
    Loaded out;
    out.result = compileXt(xt);
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

}  // namespace

// -- 1. Symbol-scope timeline triggers -----------------------------------------------------------

namespace {

constexpr const char* kPulseSymbol = R"X(<xt><head>
  <symbol name="Pulse">
    <div id="Box" on:press="play(pop) then state(Done)" on:timeline(pop).end="state(Ended)"></div>
  </symbol>
  <style>
    #A { position: absolute; left: 0px; top: 0px; }
    #B { position: absolute; left: 50px; top: 0px; }
    symbol(Pulse) {
      #Box { width: 40px; height: 40px; }
      @timeline pop { duration: 0.2s; #Box { 0s { opacity: 0.5 } 0.2s { opacity: 1 } } }
    }
    :state(Done) { color: red; }
    :state(Ended) { color: blue; }
  </style>
</head><body id="Root">
  <use symbol="Pulse" id="A"/>
  <use symbol="Pulse" id="B"/>
</body></xt>)X";

}  // namespace

TEST_CASE("T-201: play() in a symbol plays the instance's own timeline; then and end stay in the instance") {
    Loaded l = load(kPulseSymbol);
    l.click(10, 10);
    CHECK(l.doc->isTimelinePlaying("A", "pop"));
    CHECK_FALSE(l.doc->isTimelinePlaying("B", "pop"));
    CHECK_FALSE(l.has("A/Box", "Done"));
    l.step(0.1f);
    l.step(0.2f);
    CHECK(l.has("A/Box", "Done"));
    CHECK(l.has("A/Box", "Ended"));
    CHECK_FALSE(l.has("B/Box", "Done"));
    CHECK_FALSE(l.has("B/Box", "Ended"));
    // The other instance plays its own copy; A's chain does not run again.
    l.click(60, 10);
    CHECK(l.doc->isTimelinePlaying("B", "pop"));
    l.step(0.3f);
    CHECK(l.has("B/Box", "Done"));
    CHECK(l.has("B/Box", "Ended"));
}

// -- 2. set() ------------------------------------------------------------------------------------

TEST_CASE("T-201: set() writes in ports (bindings react), toggles bools and emits out ports") {
    Loaded l = load(R"X(<xt><head><style>
      #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
      #B { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px; }
      #C { position: absolute; left: 100px; top: 0px; width: 40px; height: 40px; }
      #D { position: absolute; left: 150px; top: 0px; width: 40px; height: 40px; }
    </style></head><body id="Root" port:n="number in" port:on="bool in" port:label="string in"
        port:out="number out">
      <div id="A" on:click="set(n, 5); set(label, 'hi')" bind:text="n"></div>
      <div id="B" on:click="set(on, toggle)"></div>
      <div id="C" on:click="set(out, 7)"></div>
      <div id="D" on:port(n)="set(out, event.value | scale(2) | offset(1))"></div>
    </body></xt>)X");
    std::vector<std::pair<std::string, float>> emitted;
    l.doc->setHostEventListener([&](const HostEvent& e) {
        if (e.kind == HostEventKind::Emit) {
            emitted.emplace_back(std::string(e.name), e.value.f);
        }
    });
    const PortId n = l.doc->lookupPort("n");
    const PortId on = l.doc->lookupPort("on");
    const PortId out = l.doc->lookupPort("out");
    l.click(10, 10);
    CHECK(l.doc->getPort(n).f == doctest::Approx(5));
    CHECK(l.doc->portText(l.doc->lookupPort("label")) == "hi");
    // set(n) fired port(n) on D: out = 5 * 2 + 1, emitted.
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0].first == "out");
    CHECK(emitted[0].second == doctest::Approx(11));
    CHECK(l.doc->getPort(out).f == doctest::Approx(11));
    l.click(60, 10);
    CHECK(l.doc->getPort(on).f == doctest::Approx(1));
    l.click(60, 10);
    CHECK(l.doc->getPort(on).f == doctest::Approx(0));
    l.click(110, 10);
    REQUIRE(emitted.size() == 2);
    CHECK(emitted[1].second == doctest::Approx(7));
}

TEST_CASE("T-201: set() compile errors") {
    auto body = [](std::string_view attrs) {
        return "<xt><body id=\"Root\" port:n=\"number in\" port:c=\"number call\"><div id=\"A\" " +
               std::string(attrs) + "></div></body></xt>";
    };
    CHECK(hasError(compileXt(body("on:click=\"set(missing, 1)\"")), "no port \"missing\""));
    CHECK(hasError(compileXt(body("on:click=\"set(n, toggle)\"")), "needs a bool port"));
    CHECK(hasError(compileXt(body("on:click=\"set(c, 1)\"")), "cannot set the call"));
    CHECK(hasError(compileXt(body("on:click=\"set(n, nope)\"")), "set() value must be"));
    CHECK(
        hasError(compileXt(body("on:click=\"set(n, event.value | tween(1s))\"")), "cannot be used in set()"));
    CHECK(hasError(compileXt(body("on:click=\"set(n)\"")), "set() takes a port and a value"));
    CHECK(compileXt(body("on:click=\"set(n, event.value | range(0, 1, 0, 100) | round)\"")).ok());
}

// -- 3. spawn() ----------------------------------------------------------------------------------

namespace {

constexpr const char* kSpawnDoc = R"X(<xt><head>
  <symbol name="Pop">
    <div id="Box"><span id="Amount" bind:text="amount"></span></div>
  </symbol>
  <style>
    #Field { position: absolute; left: 20px; top: 20px; width: 100px; height: 50px; }
    symbol(Pop) {
      #Box { width: 10px; height: 10px; }
      @timeline pop { duration: 0.2s; #Box { 0s { opacity: 0 } 0.2s { opacity: 1 } } }
    }
  </style>
</head><body id="Root" port:amount="number in" port:hit="event in">
  <div id="Field" on:port(hit)="spawn(Pop, at=self, lifetime=0.5s, timeline=pop)"
       on:click="spawn(Pop, at=pointer, lifetime=0.25s)"></div>
</body></xt>)X";

}  // namespace

TEST_CASE("T-201: spawn() creates frozen pooled instances at the anchor and recycles them") {
    Loaded l = load(kSpawnDoc);
    const PortId amount = l.doc->lookupPort("amount");
    const PortId hit = l.doc->lookupPort("hit");
    REQUIRE(amount != kInvalidPort);
    REQUIRE(l.doc->repeatInstances("spawn-0").empty());
    l.doc->setPort(amount, Value::ofFloat(7));
    l.doc->setPort(hit, Value::ofFloat(0));
    l.step(0.01f);
    std::span<const ElementRef> live = l.doc->repeatInstances("spawn-0");
    REQUIRE(live.size() == 1);
    const std::string first(live[0].id());
    // Top-left at the centre of #Field (70, 45); not hit-testable.
    Rect box = live[0].boundingBox();
    CHECK(box.x == doctest::Approx(70));
    CHECK(box.y == doctest::Approx(45));
    CHECK(l.doc->isTimelinePlaying(std::string_view{}, "pop") == false);  // The per-instance copy plays.
    l.doc->setPort(amount, Value::ofFloat(9));
    l.doc->setPort(hit, Value::ofFloat(0));
    l.step(0.01f);
    live = l.doc->repeatInstances("spawn-0");
    REQUIRE(live.size() == 2);
    // The first instance kept the value it was spawned with.
    CHECK(l.doc->byId(first + "/Amount").text() == "7");
    CHECK(l.doc->byId(std::string(live[1].id()) + "/Amount").text() == "9");
    l.step(0.6f);
    CHECK(l.doc->repeatInstances("spawn-0").empty());
    // A pooled instance is reused (new path id, fresh value).
    l.doc->setPort(amount, Value::ofFloat(3));
    l.doc->setPort(hit, Value::ofFloat(0));
    l.step(0.01f);
    live = l.doc->repeatInstances("spawn-0");
    REQUIRE(live.size() == 1);
    CHECK(l.doc->byId(std::string(live[0].id()) + "/Amount").text() == "3");
    // at=pointer.
    l.click(30, 30);
    l.step(0.01f);
    live = l.doc->repeatInstances("spawn-0");
    REQUIRE(live.size() == 2);
    CHECK(live[1].boundingBox().x == doctest::Approx(30));
}

TEST_CASE("T-201: spawn() compile errors") {
    auto body = [](std::string_view actions) {
        return std::string(R"X(<xt><head><symbol name="Pop"><div id="Box"></div></symbol></head>
          <body id="Root"><div id="A" on:click=")X") +
               std::string(actions) + "\"></div></body></xt>";
    };
    CHECK(hasError(compileXt(body("spawn(Pop, at=self)")), "spawn() needs lifetime"));
    CHECK(hasError(compileXt(body("spawn(Pop, lifetime=1s, timeline=nope)")), "unknown timeline"));
    CHECK(hasError(compileXt(body("spawn(Pop, lifetime=1s, at=Nobody)")), "is not an element of this scope"));
    CHECK(hasError(compileXt(body("spawn(Pop, lifetime=1s, speed=2)")), "unknown spawn() option"));
    CHECK(compileXt(body("spawn(Pop, lifetime=1s, at=A)")).ok());
}

// -- 4. Path id selectors ------------------------------------------------------------------------

TEST_CASE("T-201: a UI stylesheet selects an instance's internal element by its path id") {
    Loaded l = load(R"X(<xt><head>
      <symbol name="Card"><div id="Box"><div id="Inner"></div></div></symbol>
      <style>
        symbol(Card) { #Box { width: 40px; height: 10px; background-color: #102030; } }
        #B/Box { width: 70px; }
        #B/Box > #B/Inner, #A/Inner { height: 3px; }
      </style>
    </head><body id="Root"><use symbol="Card" id="A"/><use symbol="Card" id="B"/></body></xt>)X");
    CHECK(l.box("A/Box").width == doctest::Approx(40));
    CHECK(l.box("B/Box").width == doctest::Approx(70));
    CHECK(l.box("A/Inner").height == doctest::Approx(3));
    CHECK(l.box("B/Inner").height == doctest::Approx(3));
    // A color is still a color.
    CHECK(l.el("A/Box").computedStyleValue(prop::BackgroundColor).c.b == doctest::Approx(0x30 / 255.0f));
}

// -- 5. <link> resolves by path ------------------------------------------------------------------

TEST_CASE("T-201: <link> resolves by path only; a document cannot link itself") {
    auto compileFiles = [](std::vector<SourceFile> files, std::string_view main) {
        CompileOptions options;
        options.emitMeta = false;
        options.rootDir = "t201-no-such-dir";
        return compileDocumentFromStrings(files, main, options);
    };
    const std::string lib = R"(<xt><head><symbol name="Button"><div id="Box"></div></symbol></head></xt>)";
    const std::string sheet = "#Root { width: 10px; }";
    // A library named like the linking document resolves by its path, not to the document.
    CompileResult ok = compileFiles(
        {{"Button.xt",
          R"(<xt><head><link rel="symbols" href="lib/Button.xt"/></head><body id="Root"><use symbol="Button" id="B"/></body></xt>)"},
         {"lib/Button.xt", lib}},
        "Button.xt");
    CHECK(ok.ok());
    // A normalized path matches too.
    CompileResult normal = compileFiles(
        {{"main.xt", R"(<xt><head><link href="./a/../a/theme.xts"/></head><body id="Root"></body></xt>)"},
         {"a/theme.xts", sheet}},
        "main.xt");
    CHECK(normal.ok());
    // No file-name fallback: b/theme.xts is not a/theme.xts.
    CompileResult other = compileFiles(
        {{"main.xt", R"(<xt><head><link href="a/theme.xts"/></head><body id="Root"></body></xt>)"},
         {"b/theme.xts", sheet}},
        "main.xt");
    CHECK(hasError(other, "failed to read file"));
    // Self-reference.
    CompileResult self = compileFiles(
        {{"Button.xt",
          R"(<xt><head><link rel="symbols" href="Button.xt"/></head><body id="Root"></body></xt>)"}},
        "Button.xt");
    CHECK(hasError(self, "cannot link itself"));
}

// -- 6. Layout and input fixes -------------------------------------------------------------------

TEST_CASE("T-201: a block shrinks to its max-content width in a flex row and when absolute") {
    Loaded l = load(R"X(<xt><head><style>
      #Row { display: flex; }
      #Blk { padding: 0px 5px; }
      #Inner { width: 30px; height: 10px; }
      #Pair { display: flex; column-gap: 4px; }
      #P1 { width: 10px; height: 5px; }
      #P2 { width: 20px; height: 5px; }
      #Holder { position: relative; width: 20px; height: 20px; }
      #Abs { position: absolute; left: 0px; top: 0px; }
      #AbsNowrap { position: absolute; left: 0px; top: 0px; white-space: nowrap; }
    </style></head><body id="Root">
      <div id="Row"><div id="Blk"><div id="Inner"></div><div id="Pair"><div id="P1"></div><div id="P2"></div></div></div></div>
      <div id="Holder"><div id="Abs"><div id="AbsInner" style="width: 12px; height: 4px;"></div></div>
        <div id="AbsNowrap"><div id="Wide" style="width: 50px; height: 4px;"></div></div></div>
    </body></xt>)X");
    // max(30, 10 + 4 + 20) + padding 10.
    CHECK(l.box("Blk").width == doctest::Approx(44));
    CHECK(l.box("Abs").width == doctest::Approx(12));
    // Wider than its containing block (20 px): clamped, unless it cannot wrap.
    CHECK(l.box("AbsNowrap").width == doctest::Approx(50));
}

TEST_CASE("T-201: a click on an element inside a checkbox input toggles it") {
    Loaded l = load(R"X(<xt><head><style>
      #Sw { display: flex; width: 44px; height: 24px; }
      #Knob { width: 20px; height: 20px; }
    </style></head><body id="Root"><input type="checkbox" id="Sw"><div id="Knob"></div></input></body></xt>)X");
    CHECK_FALSE(l.el("Sw").hasState(kStateChecked));
    l.click(5, 5);  // On the knob.
    CHECK(l.el("Sw").hasState(kStateChecked));
    l.click(40, 5);  // On the input itself.
    CHECK_FALSE(l.el("Sw").hasState(kStateChecked));
}

// -- 7. Bindings to transform components ---------------------------------------------------------

TEST_CASE("T-201: bind:translate-x / scale-x / rotate drive the transform without relayout") {
    Loaded l = load(R"X(<xt><head><style>
      #A { position: absolute; left: 0px; top: 0px; width: 10px; height: 10px; transform-origin: 0% 0%; }
      #B { position: absolute; left: 50px; top: 0px; width: 20px; height: 10px; }
    </style></head><body id="Root" port:x="number in" port:s="number in" port:r="number in">
      <div id="A" bind:translate-x="x" bind:translate-y="x | scale(2)" bind:scale-x="s"></div>
      <div id="B" bind:rotate="r"></div>
    </body></xt>)X");
    l.doc->setPort(l.doc->lookupPort("x"), Value::ofFloat(15));
    l.doc->setPort(l.doc->lookupPort("s"), Value::ofFloat(2));
    l.doc->setPort(l.doc->lookupPort("r"), Value::ofFloat(90));
    l.step(0.01f);
    const Rect a = l.bounds("A");
    CHECK(a.x == doctest::Approx(15));
    CHECK(a.y == doctest::Approx(30));
    CHECK(a.w == doctest::Approx(20));
    CHECK(l.box("A").width == doctest::Approx(10));  // Layout untouched.
    const Rect b = l.bounds("B");
    CHECK(b.w == doctest::Approx(10).epsilon(0.01));
    CHECK(b.h == doctest::Approx(20).epsilon(0.01));
}

// -- 8. conic-gradient ---------------------------------------------------------------------------

TEST_CASE("T-201: conic-gradient compiles, resolves and paints a Conic paint") {
    Loaded l = load(R"X(<xt><head><style>
      #A { width: 40px; height: 20px; background-gradient: conic-gradient(from 90deg at 25% 50%, #ff0000 0%, #0000ff 100%); }
      #B { width: 40px; height: 20px; background-gradient: conic-gradient(0.5turn, #ff0000 0%, #0000ff 100%); }
    </style></head><body id="Root"><div id="A"></div><div id="B"></div></body></xt>)X");
    CommandList cmds;
    l.doc->paint(cmds);
    std::vector<PaintRef> conics;
    for (const FillCmd& fc : cmds.fills) {
        if (fc.paint.kind == PaintKind::Conic) {
            conics.push_back(fc.paint);
        }
    }
    REQUIRE(conics.size() == 2);
    // A: centre (10, 10), start ray towards +x (90deg).
    CHECK(conics[0].p0.x == doctest::Approx(10));
    CHECK(conics[0].p0.y == doctest::Approx(10));
    CHECK(conics[0].p1.x - conics[0].p0.x == doctest::Approx(1));
    CHECK(conics[0].p1.y - conics[0].p0.y == doctest::Approx(0).epsilon(1e-4));
    // B: the default centre, start ray downwards (half a turn).
    CHECK(conics[1].p0.x == doctest::Approx(20));
    CHECK(conics[1].p1.y - conics[1].p0.y == doctest::Approx(1));
    // A malformed head is dropped with a warning, like the other gradients.
    CompileResult bad = compileXt(
        "<xt><head><style>#X { background-gradient: conic-gradient(at, #000000 0%); }"
        "</style></head><body id=\"R\"><div id=\"X\"></div></body></xt>");
    CHECK(bad.ok());
    // Runtime setStyle parses it too.
    CHECK(l.el("A").setStyle("background-gradient: conic-gradient(45deg, #000000 0%, #ffffff 100%)") ==
          Status::kOk);
}

// -- 9. focus-trap markup ------------------------------------------------------------------------

TEST_CASE("T-201: the focus-trap attribute keeps Tab and navigation inside the element") {
    Loaded l = load(R"X(<xt><head><style>
      button { position: absolute; width: 40px; height: 40px; }
      #Out { left: 0px; top: 0px; }
      #Modal { position: absolute; left: 100px; top: 0px; width: 200px; height: 100px; }
      #M1 { left: 0px; top: 0px; } #M2 { left: 50px; top: 0px; }
    </style></head><body id="Root"><button id="Out"></button>
      <div id="Modal" focus-trap><button id="M1"></button><button id="M2"></button></div>
      <div id="Open" focus-trap="false"></div>
    </body></xt>)X",
                    400, 200);
    auto key = [&](KeyCode code) {
        InputEvent ev;
        ev.kind = InputEvent::KeyDown;
        ev.key = code;
        l.doc->dispatchInput(ev);
        ev.kind = InputEvent::KeyUp;
        l.doc->dispatchInput(ev);
    };
    auto focusId = [&]() { return std::string(l.doc->focus().id()); };
    l.doc->setFocus(l.el("M1"));
    key(KeyCode::Tab);
    CHECK(focusId() == "M2");
    key(KeyCode::Tab);
    CHECK(focusId() == "M1");  // Wraps inside the trap.
    key(KeyCode::Left);        // #Out is left of the trap: blocked.
    CHECK(focusId() == "M1");
    CHECK(hasError(compileXt("<xt><body id=\"R\"><div id=\"A\" focus-trap=\"maybe\"></div></body></xt>"),
                   "focus-trap must be true or false"));
}

// -- 10. state() with a target -------------------------------------------------------------------

TEST_CASE("T-201: state(<Name>, <mode>, target=#id) sets the state on an element of the same scope") {
    Loaded l = load(R"X(<xt><head>
      <symbol name="Tabs" state-groups="Tab: One|Two">
        <div id="Box">
          <div id="T1" on:click="state(One, on, target=#Box)"></div>
          <div id="T2" on:click="state(Two, on, target=#Box)"></div>
        </div>
      </symbol>
      <style>
        #A { position: absolute; left: 0px; top: 0px; }
        #B { position: absolute; left: 0px; top: 50px; }
        symbol(Tabs) { #Box { display: flex; } #T1, #T2 { width: 20px; height: 20px; } }
        :state(One) { color: red; } :state(Two) { color: blue; }
      </style>
    </head><body id="Root"><use symbol="Tabs" id="A"/><use symbol="Tabs" id="B"/></body></xt>)X");
    l.click(25, 10);  // A/T2.
    CHECK(l.has("A/Box", "Two"));
    CHECK_FALSE(l.has("A/T2", "Two"));
    CHECK_FALSE(l.has("B/Box", "Two"));  // The other instance's Box is out of scope.
    l.click(5, 10);                      // A/T1: the group makes it exclusive.
    CHECK(l.has("A/Box", "One"));
    CHECK_FALSE(l.has("A/Box", "Two"));
    const std::string bad =
        R"X(<xt><head><symbol name="S"><div id="Box" on:click="state(On, on, target=#Nope)"></div></symbol>
      <style>:state(On) { color: red; }</style></head><body id="Root"><use symbol="S" id="A"/></body></xt>)X";
    CHECK(hasError(compileXt(bad), "is not an element of this symbol scope"));
}

// -- 11. Slider drag to value --------------------------------------------------------------------

TEST_CASE("T-201: draggable=axis-x fires drag.value along the element and nav.value for left/right") {
    Loaded l = load(R"X(<xt><head><style>
      #Track { position: absolute; left: 20px; top: 10px; width: 100px; height: 10px; }
      #Other { position: absolute; left: 150px; top: 10px; width: 10px; height: 10px; }
    </style></head><body id="Root" port:value="number in" port:min="number in" port:max="number in"
        port:steps="number in">
      <div id="Track" draggable="axis-x"
           on:drag.value="set(value, event.value | range(0, 1, min, max))"
           on:nav.value="set(steps, event.value)"></div>
      <button id="Other"></button>
    </body></xt>)X");
    const PortId value = l.doc->lookupPort("value");
    const PortId steps = l.doc->lookupPort("steps");
    l.doc->setPort(l.doc->lookupPort("min"), Value::ofFloat(0));
    l.doc->setPort(l.doc->lookupPort("max"), Value::ofFloat(200));
    l.pointer(InputEvent::PointerDown, 45, 15);  // A quarter of the way.
    CHECK(l.doc->getPort(value).f == doctest::Approx(50));
    l.pointer(InputEvent::PointerMove, 95, 40);  // Dragging (the pointer may leave the track).
    CHECK(l.doc->getPort(value).f == doctest::Approx(150));
    l.pointer(InputEvent::PointerMove, 500, 40);  // Clamped.
    CHECK(l.doc->getPort(value).f == doctest::Approx(200));
    l.pointer(InputEvent::PointerUp, 500, 40);
    l.pointer(InputEvent::PointerMove, 20, 15);  // Released: no longer dragging.
    CHECK(l.doc->getPort(value).f == doctest::Approx(200));
    // The track took the focus; Left / Right adjust it instead of moving the focus.
    CHECK(l.doc->focus().id() == "Track");
    InputEvent key;
    key.kind = InputEvent::KeyDown;
    key.key = KeyCode::Left;
    l.doc->dispatchInput(key);
    CHECK(l.doc->getPort(steps).f == doctest::Approx(-1));
    key.key = KeyCode::Right;
    l.doc->dispatchInput(key);
    CHECK(l.doc->getPort(steps).f == doctest::Approx(1));
    CHECK(l.doc->focus().id() == "Track");
}
