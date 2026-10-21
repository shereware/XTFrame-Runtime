#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/types.h"

/// Custom states, state groups, triggers and actions (docs/design/27-interaction.md §27.1-27.2,
/// 03 §3.12.2-3.12.3, 08 §8.9, 12 §12.7, 13 §13.8.2-13.8.3, docs/tasks/T-174.md).

using namespace xt;
using namespace xt::compile;

namespace {

std::string doc(std::string_view style, std::string_view body, std::string_view bodyAttrs = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\" " +
           std::string(bodyAttrs) + ">" + std::string(body) + "</body></xt>";
}

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
};

Loaded load(const std::string& xt) {
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
    out.doc->setViewport(Rect{0, 0, 200, 100});
    out.doc->update(0.0f);
    return out;
}

Result<IrView> viewOf(const CompileResult& r) { return IrView::open(IrBlob{r.ir.data(), r.ir.size()}); }

constexpr const char* kBoxes = R"X(
  #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
  #B { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px; }
  #C { position: absolute; left: 100px; top: 0px; width: 40px; height: 40px; }
  @timeline fade { duration: 1s; marker: half 0.5s; #A { 0s { opacity: 0 } 1s { opacity: 1 } } }
  @timeline pulse { duration: 0.2s; #A { 0s { transform: translateX(0px) } 0.2s { transform: translateX(2px) } } }
)X";

Color bg(const Loaded& l, std::string_view id) {
    return l.el(id).computedStyleValue(prop::BackgroundColor).c;
}

}  // namespace

// -- Parsing ------------------------------------------------------------------------------------

TEST_CASE("triggers: :state() selectors and .xts trigger declarations parse") {
    DiagnosticSink sink;
    RuleSet rs;
    REQUIRE(parseXts(R"X(
      #Hp:state(Low):hover > .bar:state(Full) { color: red; }
      #Card {
        on-hover: play(card-hover);
        on-key(Enter): play(a) then hide;
        on-timeline(intro).marker(ready): "sound(ui_click); state(Selected, toggle)";
        on-press: play(x, from=m), sound(y, 0.5);
        state-enter: Low play(hp-low-in);
        state-exit: Low play(hp-low-out);
        pointer-watch: true;
      }
    )X",
                     "t.xts", 1, sink, rs));
    REQUIRE(rs.rules.size() == 2);
    const Selector& sel = rs.rules[0].selectors[0];
    REQUIRE(sel.parts.size() == 2);
    REQUIRE(sel.parts[0].conds.size() == 1);
    CHECK(sel.parts[0].conds[0].kind == SelectorCondKind::CustomState);
    CHECK(sel.parts[0].conds[0].name == "Full");
    REQUIRE(sel.parts[1].conds.size() == 1);
    CHECK(sel.parts[1].conds[0].name == "Low");
    CHECK(sel.parts[1].stateMask == kStateHover);
    // #Hp = 1 id; .bar + :state(Full) + :state(Low) + :hover = 4 classes.
    CHECK(sel.specificity == ((1u << 16) | (4u << 8)));
    const StyleRule& card = rs.rules[1];
    REQUIRE(card.triggers.size() == 6);
    CHECK(card.triggers[0].event == "hover");
    CHECK(card.triggers[0].actions == "play ( card-hover )");
    CHECK(card.triggers[1].event == "key(Enter)");
    CHECK(card.triggers[2].event == "timeline(intro).marker(ready)");
    CHECK(card.triggers[2].actions == "sound(ui_click); state(Selected, toggle)");
    CHECK(card.triggers[3].actions.find('=') != std::string::npos);
    CHECK(card.triggers[4].event == "state(Low).enter");
    CHECK(card.triggers[5].event == "state(Low).exit");
    REQUIRE(card.decls.size() == 1);  // pointer-watch is an ordinary (StyleProp 191) declaration.
    CHECK(card.decls[0].prop == 191);
    CHECK(card.decls[0].value.e == 1);
}

TEST_CASE("triggers: a trigger declaration outside a stylesheet rule is ignored with a warning") {
    DiagnosticSink sink;
    std::vector<StyleDecl> decls;
    CHECK(parseDeclarations("on-hover: hide; color: red", "inline", 1, sink, decls));
    CHECK(decls.size() == 1);
    REQUIRE(sink.all().size() == 1);
    CHECK(sink.all()[0].level == Diagnostic::Level::Warning);
    CHECK(sink.all()[0].message.find("on-hover") != std::string::npos);

    CompileResult r = compileXt(doc("", "<div id=\"A\" style=\"on-hover: hide\"></div>"));
    REQUIRE(r.ok());
    Result<IrView> view = viewOf(r);
    REQUIRE(view.ok());
    CHECK_FALSE(view.value().triggers().present());
}

// -- Compilation --------------------------------------------------------------------------------

TEST_CASE("triggers: TRIG and STAT layout") {
    CompileResult r = compileXt(doc(std::string(kBoxes) + R"X(
        #A:state(Low) { background-color: #ff0000; }
      )X",
                                    R"X(<div id="A" state-groups="Level: Full|Low"
                                            on:click="play(fade) then state(Full); sound(ui_click, 0.5)"
                                            on:key(Enter)="if between(-1, 1) class(on) else class(off, toggle)"></div>
                                        <div id="B" on:state(Low).enter="stop(fade)"></div>)X"));
    REQUIRE(r.ok());
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    const IrView& ir = opened.value();

    const CustomStateChunk& states = ir.customStates();
    REQUIRE(states.present());
    REQUIRE(states.names().size() == 2);  // state-groups first: Full = bit 0, Low = bit 1.
    CHECK(ir.string(states.names()[0]) == "Full");
    CHECK(ir.string(states.names()[1]) == "Low");
    REQUIRE(states.groups().size() == 1);
    CHECK(ir.string(states.groups()[0].name) == "Level");
    CHECK(states.groups()[0].mask == 3u);
    // `:state(Low)` is a CustomState condition part naming the state's StringId.
    bool sawCondition = false;
    for (const IrSelPart& part : ir.style().selectorParts()) {
        if (part.combinator == kIrSelCondition &&
            part.classCount == static_cast<uint8_t>(IrSelCond::CustomState)) {
            sawCondition = part.tag == states.names()[1];
        }
    }
    CHECK(sawCondition);

    const TriggerChunk& trig = ir.triggers();
    REQUIRE(trig.present());
    REQUIRE(trig.tables().size() == 2);
    const IrTriggerTable& a = trig.tables()[0];
    CHECK(ir.string(ir.doc().elements()[a.element].id) == "A");
    REQUIRE(a.triggerCount == 2);
    const IrTrigger& click = trig.triggers()[a.triggerOffset];
    CHECK(click.event == static_cast<uint8_t>(TriggerEvent::Click));
    CHECK(click.headCount == 2);
    REQUIRE(click.actionCount == 3);
    const IrTriggerAction* acts = &trig.actions()[click.actionOffset];
    CHECK(acts[0].kind == static_cast<uint8_t>(TriggerAction::Play));
    CHECK(ir.string(acts[0].arg0) == "fade");
    CHECK(acts[0].arg1 == kInvalidString);
    CHECK(acts[0].next == 3);  // then -> the continuation stored after the two heads.
    CHECK(acts[1].kind == static_cast<uint8_t>(TriggerAction::Sound));
    CHECK(acts[1].arg2 == doctest::Approx(0.5f));
    CHECK(acts[2].kind == static_cast<uint8_t>(TriggerAction::State));
    CHECK(acts[2].arg0 == 0u);
    CHECK(acts[2].flags == static_cast<uint8_t>(TriggerMode::On));

    const IrTrigger& key = trig.triggers()[a.triggerOffset + 1];
    CHECK(key.event == static_cast<uint8_t>(TriggerEvent::Key));
    CHECK(key.arg0 == static_cast<uint32_t>(KeyCode::Enter));
    REQUIRE(key.actionCount == 3);
    const IrTriggerAction* k = &trig.actions()[key.actionOffset];
    CHECK(k[0].kind == static_cast<uint8_t>(TriggerAction::If));
    CHECK(k[0].next == 2);
    CHECK(k[0].alt == 3);
    REQUIRE(k[0].arg1 == 1u);
    const IrConverterStep& cond = trig.conditions()[k[0].arg0];
    CHECK(cond.op == static_cast<uint8_t>(ConverterOp::Between));
    CHECK(cond.args[0] == doctest::Approx(-1.0f));
    CHECK(k[2].flags == static_cast<uint8_t>(TriggerMode::Toggle));

    const IrTrigger& enter = trig.triggers()[trig.tables()[1].triggerOffset];
    CHECK(enter.event == static_cast<uint8_t>(TriggerEvent::StateEnter));
    CHECK(enter.arg0 == 1u);

    // Round trip (IrWriter::fromView) is byte-identical.
    IrWriter back = IrWriter::fromView(ir);
    CHECK(back.finish() == r.ir);
}

TEST_CASE("triggers: .xt attributes win over .xts; .xts follows specificity then order") {
    CompileResult r = compileXt(doc(std::string(kBoxes) + R"X(
        #A { on-click: play(pulse); on-hover: play(fade); }
        div { on-hover: stop(fade); on-leave: stop(fade); }
        .late { on-leave: play(pulse); }
      )X",
                                    R"X(<div id="A" class="late" on:click="hide"></div>)X"));
    REQUIRE(r.ok());
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    const IrView& ir = opened.value();
    const TriggerChunk& trig = ir.triggers();
    REQUIRE(trig.tables().size() == 1);
    const IrTriggerTable& t = trig.tables()[0];
    REQUIRE(t.triggerCount == 3);
    auto actionOf = [&](TriggerEvent ev) -> const IrTriggerAction* {
        for (uint32_t i = 0; i < t.triggerCount; ++i) {
            const IrTrigger& tr = trig.triggers()[t.triggerOffset + i];
            if (tr.event == static_cast<uint8_t>(ev)) {
                return &trig.actions()[tr.actionOffset];
            }
        }
        return nullptr;
    };
    REQUIRE(actionOf(TriggerEvent::Click) != nullptr);
    CHECK(actionOf(TriggerEvent::Click)->kind == static_cast<uint8_t>(TriggerAction::Hide));  // .xt wins.
    CHECK(actionOf(TriggerEvent::Hover)->kind == static_cast<uint8_t>(TriggerAction::Play));  // #A > div.
    CHECK(actionOf(TriggerEvent::Leave)->kind == static_cast<uint8_t>(TriggerAction::Play));  // .late > div.
}

TEST_CASE("triggers: compile errors for unknown events, actions and references") {
    auto errs = [](std::string_view attrs, std::string_view needle) {
        CompileResult r = compileXt(doc(kBoxes, "<div id=\"A\" " + std::string(attrs) + "></div>"));
        CAPTURE(attrs);
        CHECK_FALSE(r.ok());
        CHECK(hasError(r, needle));
    };
    errs("on:wiggle=\"hide\"", "unknown event");
    errs("on:click=\"explode()\"", "unknown action");
    errs("on:click=\"play(nope)\"", "unknown timeline");
    errs("on:click=\"play(fade, from=nope)\"", "has no marker");
    errs("on:click=\"play(fade, sideways)\"", "unknown play() option");
    errs("on:click=\"focus(Nobody)\"", "is not an element id");
    errs("on:click=\"set(hp, 3)\"", "no port \"hp\"");  // T-201: set() exists; hp is undeclared.
    errs("on:click=\"spawn(x, lifetime=1s)\"", "unknown symbol \"x\"");  // T-201: a spawn pool of x.
    errs("on:key(Hyper)=\"hide\"", "unknown key");
    errs("on:timeline(fade).marker(nope)=\"hide\"", "has no marker");
    errs("on:click=\"if range(0, 1) hide\"", "comparison converter");
    errs("on:click=\"state(A, sideways)\"", "on, off or toggle");
    errs("on:click=\"play(fade) then if eq(1) hide\"", "must start a statement");
    errs("state-groups=\"G: a|b; H: b|c\"", "two state groups");
}

TEST_CASE("triggers: more than 32 custom state names is an error") {
    std::string groups = "G: ";
    for (int i = 0; i < 33; ++i) {
        groups += (i == 0 ? "" : "|") + std::string("S") + std::to_string(i);
    }
    CompileResult r = compileXt(doc("", "<div id=\"A\" state-groups=\"" + groups + "\"></div>"));
    CHECK_FALSE(r.ok());
    CHECK(hasError(r, "more than 32 custom state names"));
}

TEST_CASE("triggers: symbol requires accepts a declared custom state") {
    const char* body = R"X(<symbol name="Bar" requires="states: Low" state-groups="Level: Full|Low">
                            <div id="Fill"></div></symbol><use symbol="Bar" id="Hp"></use>)X";
    CompileResult ok = compileXt(doc("", body));
    for (const Diagnostic& d : ok.diagnostics) {
        CAPTURE(d.message);
    }
    CHECK(ok.ok());
    CompileResult bad =
        compileXt(doc("", R"X(<symbol name="Bar" requires="states: Low"><div id="Fill"></div></symbol>
                                             <use symbol="Bar" id="Hp"></use>)X"));
    CHECK_FALSE(bad.ok());
    CHECK(hasError(bad, "requires state \"Low\""));
}

// -- Runtime: custom states ---------------------------------------------------------------------

TEST_CASE("triggers: setCustomState drives :state() styles, groups and enter/exit") {
    Loaded l = load(doc(std::string(kBoxes) + R"X(
        #A { background-color: #0000ff; }
        #A:state(Low) { background-color: #ff0000; }
        #A:state(Full) > #Inner { background-color: #00ff00; }
        #Inner:not(:state(Low)) { color: #ffffff; }
      )X",
                        R"X(<div id="A" state-groups="Level: Full|Low"
                                on:state(Low).enter="class(low)" on:state(Low).exit="class(low, off)">
                             <div id="Inner"></div></div>)X"));
    CHECK(bg(l, "A").b == doctest::Approx(1.0f));
    CHECK(l.doc->setCustomState(l.el("A"), "Nope", true) == Status::kNotFound);
    CHECK(l.doc->setCustomState(ElementRef{}, "Low", true) == Status::kInvalidArgument);

    REQUIRE(l.doc->setCustomState(l.el("A"), "Low", true) == Status::kOk);
    CHECK(l.has("A", "Low"));
    CHECK(l.el("A").hasClass("low"));
    l.step(0.016f);
    CHECK(bg(l, "A").r == doctest::Approx(1.0f));

    // Same group: Full clears Low (Low's exit trigger runs).
    REQUIRE(l.doc->setCustomState(l.el("A"), "Full", true) == Status::kOk);
    CHECK(l.has("A", "Full"));
    CHECK_FALSE(l.has("A", "Low"));
    CHECK_FALSE(l.el("A").hasClass("low"));
    l.step(0.016f);
    CHECK(bg(l, "A").b == doctest::Approx(1.0f));
    CHECK(bg(l, "Inner").g == doctest::Approx(1.0f));  // :state() on an ancestor part.
    CHECK(l.el("Inner").computedStyleValue(prop::Color).c.b == doctest::Approx(1.0f));  // :not(:state()).
    REQUIRE(l.doc->setCustomState(l.el("Inner"), "Low", true) == Status::kOk);
    l.step(0.016f);
    CHECK(l.el("Inner").computedStyleValue(prop::Color).c.b == doctest::Approx(0.0f));
}

TEST_CASE("triggers: state actions toggle, group exclusivity through a trigger, .xts state-enter") {
    Loaded l = load(doc(std::string(kBoxes) + R"X(
        #B { state-enter: Selected class(sel); state-exit: Selected class(sel, off); }
      )X",
                        R"X(<div id="A" state-groups="Tone: Warn|Danger" on:click="state(Danger)"></div>
                           <div id="B" on:click="state(Selected, toggle)"></div>)X"));
    REQUIRE(l.doc->setCustomState(l.el("A"), "Warn", true) == Status::kOk);
    l.click(20, 20);
    CHECK(l.has("A", "Danger"));
    CHECK_FALSE(l.has("A", "Warn"));
    l.click(70, 20);
    CHECK(l.has("B", "Selected"));
    CHECK(l.el("B").hasClass("sel"));
    l.click(70, 20);
    CHECK_FALSE(l.has("B", "Selected"));
    CHECK_FALSE(l.el("B").hasClass("sel"));
}

// -- Runtime: events ------------------------------------------------------------------------------

TEST_CASE("triggers: pointer, focus, key and check events run their actions") {
    Loaded l = load(doc(kBoxes, R"X(<div id="A" tabindex="0" on:hover="class(h)" on:leave="class(h, off)"
                                        on:press="class(p)" on:release="class(r)" on:click="class(c)"
                                        on:focus="class(f)" on:blur="class(f, off)" on:key(Enter)="class(k)"></div>
                                   <input id="Box" type="checkbox" on:check="class(yes)" on:uncheck="class(yes, off)"/>)X"));
    ElementRef a = l.el("A");
    l.pointer(InputEvent::PointerMove, 20, 20);
    CHECK(a.hasClass("h"));
    l.pointer(InputEvent::PointerDown, 20, 20);
    CHECK(a.hasClass("p"));
    CHECK(a.hasClass("f"));  // The press focused A (tabindex).
    l.pointer(InputEvent::PointerUp, 20, 20);
    CHECK(a.hasClass("r"));
    CHECK(a.hasClass("c"));
    InputEvent key;
    key.kind = InputEvent::KeyDown;
    key.key = KeyCode::Space;
    l.doc->dispatchInput(key);
    CHECK_FALSE(a.hasClass("k"));
    key.key = KeyCode::Enter;
    l.doc->dispatchInput(key);
    CHECK(a.hasClass("k"));
    l.pointer(InputEvent::PointerMove, 190, 90);
    CHECK_FALSE(a.hasClass("h"));
    l.doc->setFocus(ElementRef{});
    CHECK_FALSE(a.hasClass("f"));

    ElementRef box = l.el("Box");
    box.setState(kStateChecked, true);
    CHECK(box.hasClass("yes"));
    l.doc->setElementState(box, kStateChecked, false);
    CHECK_FALSE(box.hasClass("yes"));
}

TEST_CASE("triggers: play / then / stop / seek / timeline end and marker events") {
    Loaded l = load(doc(kBoxes, R"X(<div id="A" on:click="play(fade) then class(done)"
                                        on:timeline(fade).marker(half)="class(half)"
                                        on:timeline(fade).end="class(ended)"></div>
                                   <div id="B" on:click="stop(fade)" on:hover="seek(fade, half)"></div>
                                   <div id="C" on:click="play(pulse, loop) then class(never)"></div>)X"));
    l.click(20, 20);
    CHECK(l.doc->isTimelinePlaying("", "fade"));
    l.step(0.6f);
    CHECK(l.el("A").hasClass("half"));
    CHECK_FALSE(l.el("A").hasClass("done"));
    l.step(0.6f);
    CHECK(l.el("A").hasClass("done"));
    CHECK(l.el("A").hasClass("ended"));
    // Default play holds the last frame.
    CHECK(l.el("A").property(prop::Opacity).f == doctest::Approx(1.0f));

    // A stopped timeline never ends: the waiting `then` is dropped.
    l.el("A").removeClass("done");
    l.click(20, 20);
    l.step(0.1f);
    l.click(70, 20);
    CHECK_FALSE(l.doc->isTimelinePlaying("", "fade"));
    l.step(2.0f);
    CHECK_FALSE(l.el("A").hasClass("done"));

    // seek to a marker.
    l.click(20, 20);
    l.pointer(InputEvent::PointerMove, 70, 20);
    CHECK(l.doc->timelineTime("", "fade") == doctest::Approx(0.5f));

    // A looping play never runs its `then`.
    l.click(120, 20);
    l.step(1.0f);
    CHECK(l.doc->isTimelinePlaying("", "pulse"));
    CHECK_FALSE(l.el("C").hasClass("never"));
}

TEST_CASE("triggers: play modes once, reverse and an explicit hold") {
    Loaded l =
        load(doc(kBoxes, R"X(<div id="A" on:click="play(fade, once)" on:hover="play(fade, reverse)"></div>
                                   <div id="B" on:click="play(fade, hold)"></div>)X"));
    l.click(20, 20);
    l.step(1.2f);
    CHECK_FALSE(l.doc->isTimelinePlaying("", "fade"));

    l.pointer(InputEvent::PointerMove, 190, 90);
    l.pointer(InputEvent::PointerMove, 20, 20);  // hover -> reverse, starts at the end.
    CHECK(l.doc->timelineTime("", "fade") == doctest::Approx(1.0f));
    l.step(0.3f);
    CHECK(l.doc->timelineTime("", "fade") == doctest::Approx(0.7f));

    // Explicit hold: a re-trigger while playing keeps the running playback ...
    l.click(70, 20);
    CHECK(l.doc->timelineTime("", "fade") == doctest::Approx(0.7f));
    CHECK(l.doc->isTimelinePlaying("", "fade"));
    // ... and starts it (forwards, from 0) once it is not playing.
    l.step(1.0f);
    CHECK_FALSE(l.doc->isTimelinePlaying("", "fade"));
    l.click(70, 20);
    CHECK(l.doc->isTimelinePlaying("", "fade"));
    CHECK(l.doc->timelineTime("", "fade") == doctest::Approx(0.0f));
}

TEST_CASE("triggers: if/else compares the event value against constants") {
    Loaded l = load(doc(
        kBoxes,
        R"X(<div id="A" on:click="if eq(0) class(zero) else class(other); if gt(1) class(big) else class(small)"></div>)X"));
    l.click(20, 20);
    CHECK(l.el("A").hasClass("zero"));
    CHECK_FALSE(l.el("A").hasClass("other"));
    CHECK(l.el("A").hasClass("small"));
    CHECK_FALSE(l.el("A").hasClass("big"));
}

TEST_CASE("triggers: focus(), show()/hide() on elements") {
    Loaded l = load(doc(kBoxes, R"X(<div id="A" on:click="focus(#B); hide()"></div>
                                   <div id="B" tabindex="0"></div>
                                   <div id="C" on:press="hide" on:click="show()" on:show="class(shown)"></div>)X"));
    l.click(20, 20);
    CHECK(l.doc->focus().node == l.el("B").node);
    l.step(0.016f);
    CHECK(l.el("A").computedStyleValue(170).e == 1u);  // visibility: hidden (no hide trigger: at once).

    // C: the press hides it, the click (same gesture) shows it again and fires `show`.
    l.click(120, 20);
    l.step(0.016f);
    CHECK(l.el("C").computedStyleValue(170).e == 0u);
    CHECK(l.el("C").hasClass("shown"));
}

TEST_CASE("triggers: element hide handshake waits for its `then hide` outro") {
    // T-374: B keeps a shown direct child of `<body>`, so A's hide stays an element hide.
    Loaded l = load(doc(kBoxes, R"X(<div id="A" on:click="hide()" on:hide="play(pulse) then hide"></div>
                                   <div id="B"></div>)X"));
    l.click(20, 20);
    l.step(0.016f);
    CHECK(l.el("A").computedStyleValue(170).e == 0u);  // Still visible during the outro.
    l.step(0.3f);
    l.step(0.016f);
    CHECK(l.el("A").computedStyleValue(170).e == 1u);  // Hidden once pulse ended.
}

// -- Runtime: surface lifecycle and host events --------------------------------------------------

namespace {

struct Recorder : ContextListener {
    std::vector<std::string> log;
    void onHostEvent(const HostEvent& e) override {
        const char* kind = e.kind == HostEventKind::Sound    ? "sound"
                           : e.kind == HostEventKind::Haptic ? "haptic"
                                                             : "emit";
        log.push_back(std::string(kind) + ":" + std::string(e.name) + "@" + std::string(e.elementId) + "#" +
                      std::to_string(e.document) +
                      (e.kind == HostEventKind::Sound ? "v" + std::to_string(e.volume).substr(0, 3) : ""));
    }
    void onShown(DocumentHandle d) override { log.push_back("shown#" + std::to_string(d)); }
    void onHidden(DocumentHandle d) override { log.push_back("hidden#" + std::to_string(d)); }
};

}  // namespace

TEST_CASE("triggers: sound / haptic / emit reach the Context listener") {
    CompileResult r = compileXt(
        doc(kBoxes, R"X(<div id="A" on:click="sound(ui_click, 0.5); haptic(tap); emit(pressed)"></div>)X"));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    Recorder rec;
    ctx.addListener(&rec);
    ctx.addListener(&rec);  // No duplicate.
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
    REQUIRE(h.ok());
    ctx.update(0.0f);
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{20, 20};
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ctx.pushInput(down);
    ctx.pushInput(up);
    ctx.update(0.016f);
    REQUIRE(rec.log.size() == 3);
    CHECK(rec.log[0] == "sound:ui_click@A#1v0.5");
    CHECK(rec.log[1] == "haptic:tap@A#1");
    CHECK(rec.log[2] == "emit:pressed@A#1");
    ctx.removeListener(&rec);
}

TEST_CASE("triggers: requestHide waits for `then hide`, reports OnHidden, requestShow reports OnShown") {
    CompileResult r = compileXt(
        doc(kBoxes, "<div id=\"A\"></div>", "on:show=\"class(shown)\" on:hide=\"play(fade) then hide\""));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    Recorder rec;
    ctx.addListener(&rec);
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
    REQUIRE(h.ok());
    Document& d = *ctx.document(h.value());
    ctx.update(0.0f);
    CHECK(d.byId("Root").hasClass("shown"));  // The first update fires the surface's `show`.
    CHECK(ctx.isVisible(h.value()));

    REQUIRE(ctx.requestHide(h.value()) == Status::kOk);
    CHECK(ctx.isHiding(h.value()));
    CHECK(ctx.isVisible(h.value()));
    ctx.update(0.5f);
    CHECK(ctx.isHiding(h.value()));
    ctx.update(0.6f);
    CHECK_FALSE(ctx.isHiding(h.value()));
    CHECK_FALSE(ctx.isVisible(h.value()));
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0] == "hidden#1");
    CommandList cmds;
    ctx.render(cmds);
    CHECK(cmds.commands.empty());  // A hidden surface paints nothing.

    d.byId("Root").removeClass("shown");
    REQUIRE(ctx.requestShow(h.value()) == Status::kOk);
    CHECK(ctx.isVisible(h.value()));
    CHECK(d.byId("Root").hasClass("shown"));
    REQUIRE(rec.log.size() == 2);
    CHECK(rec.log[1] == "shown#1");
    CHECK(ctx.requestShow(99) == Status::kInvalidArgument);
}

TEST_CASE("triggers: requestHide without a delaying action hides at once; show cancels a pending hide") {
    Loaded plain = load(doc(kBoxes, "<div id=\"A\"></div>", "on:hide=\"play(fade)\""));
    REQUIRE(plain.doc->requestHide() == Status::kOk);
    CHECK_FALSE(plain.doc->isVisible());
    CHECK_FALSE(plain.doc->isHiding());

    Loaded l = load(doc(kBoxes, "<div id=\"A\"></div>", "on:hide=\"play(fade) then hide\""));
    REQUIRE(l.doc->requestHide() == Status::kOk);
    CHECK(l.doc->isHiding());
    REQUIRE(l.doc->requestShow() == Status::kOk);
    CHECK_FALSE(l.doc->isHiding());
    l.step(1.5f);
    CHECK(l.doc->isVisible());  // The cancelled chain's `hide` never runs.
}

// T-349 (27.2.4): the surface's show / hide reach `<body>`'s direct children too.

TEST_CASE("T-349: the first frame and requestShow fire `show` on body's direct children only") {
    Loaded l = load(R"X(<xt><head><style>
      #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
    </style></head><body id="Root" port:n="number in" port:m="number in">
      <div id="A" on:show="set(n, 5)"><div id="Deep" on:show="set(m, 9)"></div></div>
    </body></xt>)X");
    const PortId n = l.doc->lookupPort("n");
    const PortId m = l.doc->lookupPort("m");
    CHECK(l.doc->getPort(n).f == doctest::Approx(5));
    CHECK(l.doc->getPort(m).f == doctest::Approx(0));  // One level deeper: not part of the surface.

    REQUIRE(l.doc->requestHide() == Status::kOk);
    CHECK_FALSE(l.doc->isVisible());
    REQUIRE(l.doc->setPort(n, Value{}) == Status::kOk);
    CHECK(l.doc->getPort(n).f == doctest::Approx(0));
    REQUIRE(l.doc->requestShow() == Status::kOk);
    CHECK(l.doc->getPort(n).f == doctest::Approx(5));  // requestShow fires the root div's `show` again.
    CHECK(l.doc->getPort(m).f == doctest::Approx(0));
}

TEST_CASE("T-349: a root div's `then hide` belongs to the surface") {
    CompileResult r = compileXt(doc(kBoxes, R"X(<div id="A" on:hide="play(fade) then hide"></div>)X"));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    Recorder rec;
    ctx.addListener(&rec);
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
    REQUIRE(h.ok());
    Document& d = *ctx.document(h.value());
    ctx.update(0.0f);

    REQUIRE(ctx.requestHide(h.value()) == Status::kOk);
    CHECK(ctx.isHiding(h.value()));
    ctx.update(0.5f);
    CHECK(ctx.isHiding(h.value()));
    ctx.update(1.1f);
    CHECK_FALSE(ctx.isHiding(h.value()));
    CHECK_FALSE(ctx.isVisible(h.value()));
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0] == "hidden#1");
    CHECK(d.byId("A").computedStyleValue(170).e == 0u);  // The div itself is not `visibility: hidden`.

    // requestShow cancels a direct child's pending `then hide` as well.
    REQUIRE(ctx.requestShow(h.value()) == Status::kOk);
    REQUIRE(ctx.requestHide(h.value()) == Status::kOk);
    CHECK(ctx.isHiding(h.value()));
    REQUIRE(ctx.requestShow(h.value()) == Status::kOk);
    CHECK_FALSE(ctx.isHiding(h.value()));
    ctx.update(3.0f);
    CHECK(ctx.isVisible(h.value()));
    CHECK(d.byId("A").computedStyleValue(170).e == 0u);
    ctx.removeListener(&rec);
}

TEST_CASE("T-349: the surface waits for every member's `then hide`") {
    Loaded l = load(doc(kBoxes, R"X(<div id="A" on:hide="play(fade) then hide"></div>)X",
                        "on:hide=\"play(pulse) then hide\""));
    REQUIRE(l.doc->requestHide() == Status::kOk);
    l.step(0.3f);
    CHECK(l.doc->isHiding());  // pulse (body) ended; fade (A) still runs.
    l.step(0.8f);
    CHECK_FALSE(l.doc->isHiding());
    CHECK_FALSE(l.doc->isVisible());
}

// T-374 (27.2.4, 2026-10-18): `show()` / `hide()` on a member (`<body>` or a direct child) act on
// the surface.

namespace {

/// A Context-loaded document for the T-374 cases, with a Recorder for OnShown / OnHidden.
struct Surface {
    CompileResult result;
    Context ctx{ContextConfig{}};
    Recorder rec;
    DocumentHandle handle = 0;
    Document* d = nullptr;

    explicit Surface(const std::string& xt, bool startHidden = false) {
        result = compileXt(xt);
        REQUIRE(result.ok());
        ctx.addListener(&rec);
        ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
        Result<DocumentHandle> h = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false);
        REQUIRE(h.ok());
        handle = h.value();
        d = ctx.document(handle);
        if (startHidden) {
            REQUIRE(ctx.setVisible(handle, false) == Status::kOk);  // T-373: before the first frame.
        }
        ctx.update(0.0f);
    }
    ~Surface() { ctx.removeListener(&rec); }
    size_t count(const std::string& entry) const {
        return static_cast<size_t>(std::count(rec.log.begin(), rec.log.end(), entry));
    }
};

}  // namespace

TEST_CASE("T-374: a root div's port-driven hide() / show() hide and show the surface") {
    Surface s(R"X(<xt><head><style>
      #Box { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
    </style></head><body id="Root">
      <div id="Box" port:active="bool in = true" port:n="number in"
           on:port(active)="if eq(1) show() else hide()" on:show="set(n, 5)"></div>
    </body></xt>)X");
    const PortId active = s.d->lookupPort("active");
    const PortId n = s.d->lookupPort("n");
    CHECK(s.d->getPort(n).f == doctest::Approx(5));  // The first frame's `show`.
    CHECK(s.d->lastHideSource().empty());

    REQUIRE(s.d->setPort(active, Value::ofFloat(0)) == Status::kOk);
    CHECK_FALSE(s.ctx.isVisible(s.handle));
    CHECK(s.count("hidden#1") == 1);
    CHECK(s.d->lastHideSource() == "port active");
    s.ctx.update(0.1f);
    CHECK(s.d->byId("Box").computedStyleValue(170).e == 0u);  // The div keeps its own visibility.

    REQUIRE(s.d->setPort(n, Value::ofFloat(0)) == Status::kOk);
    REQUIRE(s.d->setPort(active, Value::ofFloat(1)) == Status::kOk);
    CHECK(s.ctx.isVisible(s.handle));
    CHECK(s.count("shown#1") == 1);
    CHECK(s.d->getPort(n).f == doctest::Approx(5));  // `on:show` fired again.
    CHECK(s.count("hidden#1") == 1);

    // A host hide records `host`.
    REQUIRE(s.ctx.requestHide(s.handle) == Status::kOk);
    CHECK(s.d->lastHideSource() == "host");
}

TEST_CASE("T-374: with two top-level children, hiding one leaves the surface; hiding both hides it") {
    Surface s(R"X(<xt><head><style>
      #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
      #B { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px; }
      #T { display: none; }
    </style></head><body id="Root">
      <div id="A" on:click="hide()"></div>
      <div id="B" on:click="hide()" on:show="class(shown)"></div>
      <div id="T"></div>
    </body></xt>)X");
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.button = PointerButton::Left;
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    auto click = [&](float x) {
        down.pos = up.pos = Vec2{x, 20};
        s.d->dispatchInput(down);
        s.d->dispatchInput(up);
    };
    click(20);  // A.
    CHECK(s.ctx.isVisible(s.handle));
    CHECK(s.rec.log.empty());
    s.ctx.update(0.1f);
    CHECK(s.d->byId("A").computedStyleValue(170).e == 1u);

    click(70);  // B: no shown direct child is left (T is `display: none`).
    CHECK_FALSE(s.ctx.isVisible(s.handle));
    CHECK(s.count("hidden#1") == 1);
    CHECK(s.d->lastHideSource() == "trigger on:click");

    // A host show brings back what the last hide left: B, not the element-hidden A.
    REQUIRE(s.ctx.requestShow(s.handle) == Status::kOk);
    CHECK(s.d->byId("B").hasClass("shown"));
    s.ctx.update(0.2f);
    CHECK(s.d->byId("A").computedStyleValue(170).e == 1u);
    CHECK(s.d->byId("B").computedStyleValue(170).e == 0u);
}

TEST_CASE("T-374: a member's `then hide` still runs the surface's waiting chain") {
    Surface s(doc(kBoxes,
                  R"X(<div id="A" port:active="bool in = true" on:port(active)="if eq(1) show() else hide()"
                           on:hide="play(fade) then hide"></div>)X"));
    REQUIRE(s.d->setPort(s.d->lookupPort("active"), Value::ofFloat(0)) == Status::kOk);
    CHECK(s.ctx.isHiding(s.handle));
    CHECK(s.ctx.isVisible(s.handle));
    s.ctx.update(0.5f);
    CHECK(s.ctx.isHiding(s.handle));
    CHECK(s.rec.log.empty());
    s.ctx.update(1.1f);
    CHECK_FALSE(s.ctx.isHiding(s.handle));
    CHECK_FALSE(s.ctx.isVisible(s.handle));
    CHECK(s.count("hidden#1") == 1);
    CHECK(s.d->lastHideSource() == "port active");
    CHECK(s.d->byId("A").computedStyleValue(170).e == 0u);

    // show() while the outro runs cancels the hide.
    REQUIRE(s.d->setPort(s.d->lookupPort("active"), Value::ofFloat(1)) == Status::kOk);
    REQUIRE(s.d->setPort(s.d->lookupPort("active"), Value::ofFloat(0)) == Status::kOk);
    CHECK(s.ctx.isHiding(s.handle));
    REQUIRE(s.d->setPort(s.d->lookupPort("active"), Value::ofFloat(1)) == Status::kOk);
    CHECK_FALSE(s.ctx.isHiding(s.handle));
    s.ctx.update(3.0f);
    CHECK(s.ctx.isVisible(s.handle));
    CHECK(s.count("hidden#1") == 1);
}

TEST_CASE("T-374: a surface loaded hidden (T-373) shows on a member show() and fires `show`") {
    Surface s(R"X(<xt><head><style>
      #Title { width: 40px; height: 20px; }
      @timeline intro { duration: 1s; #Title { 0s { opacity: 0 } 1s { opacity: 1 } } }
    </style></head><body id="Root">
      <div id="Box" port:active="bool in = true" on:port(active)="if eq(1) show() else hide()"
           on:show="play(intro)"><span id="Title">t</span></div>
    </body></xt>)X",
              /*startHidden=*/true);
    const PortId active = s.d->lookupPort("active");
    CHECK_FALSE(s.ctx.isVisible(s.handle));
    CHECK_FALSE(s.d->isTimelinePlaying("", "intro"));
    CHECK(s.d->lastHideSource() == "host");
    REQUIRE(s.d->setPort(active, Value::ofFloat(0)) == Status::kOk);  // Already hidden: nothing.
    CHECK(s.rec.log.empty());

    REQUIRE(s.d->setPort(active, Value::ofFloat(1)) == Status::kOk);
    CHECK(s.ctx.isVisible(s.handle));
    CHECK(s.count("shown#1") == 1);
    CHECK(s.d->isTimelinePlaying("", "intro"));
    s.ctx.update(0.1f);
    CHECK(s.d->timelineTime("", "intro") < 0.2f);
}

// -- Runtime: pointer-watch ------------------------------------------------------------------------

TEST_CASE("triggers: pointer-watch elements get hover but let presses through") {
    Loaded l = load(doc(std::string(R"X(
        #Under { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }
        #Watch { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; pointer-watch: true; }
      )X"),
                        R"X(<div id="Under" on:click="class(clicked)"></div>
                           <div id="Watch" on:hover="class(hovered)" on:click="class(clicked)"><span id="Kid"></span></div>)X"));
    l.pointer(InputEvent::PointerMove, 20, 20);
    CHECK(l.el("Watch").hasClass("hovered"));
    CHECK(l.doc->hitTest(Vec2{20, 20}) == l.el("Watch").node);
    CHECK(l.doc->hitTestPress(Vec2{20, 20}) == l.el("Under").node);
    l.click(20, 20);
    CHECK(l.el("Under").hasClass("clicked"));
    CHECK_FALSE(l.el("Watch").hasClass("clicked"));
    CHECK(l.el("Watch").computedStyleValue(191).e == 1u);
    CHECK(l.el("Kid").computedStyleValue(191).e == 1u);  // Inherited.
}

// -- IR validation ---------------------------------------------------------------------------------

TEST_CASE("triggers: IrView rejects out-of-range TRIG / STAT references") {
    CompileResult r =
        compileXt(doc(kBoxes, R"X(<div id="A" state-groups="G: X|Y" on:click="state(X)"></div>)X"));
    REQUIRE(r.ok());
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    IrWriter writer = IrWriter::fromView(opened.value());

    {
        IrWriter bad = IrWriter::fromView(opened.value());
        TriggerBuilder tb;
        tb.tables = std::vector<IrTriggerTable>(opened.value().triggers().tables().begin(),
                                                opened.value().triggers().tables().end());
        tb.triggers = std::vector<IrTrigger>(opened.value().triggers().triggers().begin(),
                                             opened.value().triggers().triggers().end());
        tb.actions = std::vector<IrTriggerAction>(opened.value().triggers().actions().begin(),
                                                  opened.value().triggers().actions().end());
        tb.actions[0].arg0 = 7;  // State bit past the two names.
        bad.setTriggers(std::move(tb));
        std::vector<uint8_t> blob = bad.finish();
        CHECK(IrView::open(IrBlob{blob.data(), blob.size()}).status() == Status::kInvalidIr);
    }
    {
        IrWriter bad = IrWriter::fromView(opened.value());
        CustomStateBuilder sb;
        sb.names = std::vector<uint32_t>(opened.value().customStates().names().begin(),
                                         opened.value().customStates().names().end());
        sb.groups.push_back(IrStateGroup{sb.names[0], 1u << 5});  // Mask past the names.
        bad.setCustomStates(std::move(sb));
        std::vector<uint8_t> blob = bad.finish();
        CHECK(IrView::open(IrBlob{blob.data(), blob.size()}).status() == Status::kInvalidIr);
    }
    std::vector<uint8_t> good = writer.finish();
    CHECK(IrView::open(IrBlob{good.data(), good.size()}).ok());
}
