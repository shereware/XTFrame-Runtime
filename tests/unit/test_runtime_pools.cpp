#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/animator.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-270a: the runtime pools stay bounded however often a value changes -- node-owned text for
/// setText / `bind:text`, query() without interning, one runtime effect segment per element, the
/// node effect pool's in-place reuse and automatic compaction, the IME composition pool -- and
/// host-created tags / attributes resolve through the runtime string pool
/// (docs/reviews/2026-10-02-full-audit-raw/C.md C6, C12, C13; B.md B14; docs/tasks/T-270a.md).
/// T-270b: what consumes those ids and offsets follows their in-place changes -- glyphs and damage
/// after setText, `<repeat>` copies and recycled instances, effect transitions across an in-place
/// declaration and across the automatic compaction -- and the incremental live effect counts.

using namespace xt;
using namespace xt::compile;

namespace xt {

/// Document's / Context's friend for tests: reads the runtime pools' sizes.
struct RuntimePoolTestAccess {
    static size_t runtimeStrings(const Document& d) { return d.m_runtimeStrings.size(); }
    static size_t compositionPool(const Context& c) { return c.m_compositionPool.size(); }
    static size_t runtimeTexts(const Document& d) { return d.m_runtimeTexts.size(); }
    /// T-270b: the Text node holding runtime text slot `id` (kInvalidNode when free or not a slot).
    static NodeId runtimeTextOwner(const Document& d, StringId id) {
        if (id < kRuntimeTextBase || id == kInvalidString ||
            id - kRuntimeTextBase >= d.m_runtimeTextOwners.size()) {
            return kInvalidNode;
        }
        return d.m_runtimeTextOwners[id - kRuntimeTextBase];
    }
    /// T-270e: how many elements own a runtime effect segment.
    static size_t runtimeEffectSegments(const Document& d) { return d.m_runtimeEffectSegments.size(); }
};

}  // namespace xt

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\">" +
           std::string(body) + "</body></xt>";
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

/// A standalone Document (no Context) and its clock.
struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;
    float clock = 0;

    void step(float dt = 0.016f) {
        clock += dt;
        doc->update(clock);
    }
    PortId port(std::string_view name) const {
        const PortId p = doc->lookupPort(name);
        REQUIRE(p != kInvalidPort);
        return p;
    }
};

Ui open(const std::string& xt) {
    Ui out;
    out.result = compileXt(xt);
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 200});
    out.doc->update(0.0f);
    return out;
}

}  // namespace

// -- C6 (1): a text node's runtime text is its own, not an interned string ------------------------

TEST_CASE("runtime pools: setText with 10000 different values does not grow the runtime string pool") {
    Ui ui = open(page("", R"X(<div id="A">x</div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    a.setText("value 0");
    const size_t before = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    for (int i = 1; i < 10000; ++i) {
        a.setText("value " + std::to_string(i));
    }
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) == before);
    CHECK(a.text() == "value 9999");
    ui.step();
    CHECK(a.text() == "value 9999");
}

TEST_CASE("runtime pools: bind:text on a port changing every frame does not grow the runtime string pool") {
    Ui ui = open(page("", R"X(<div id="A" port:score="number" bind:text="score"></div>)X"));
    const PortId score = ui.port("score");
    REQUIRE(ui.doc->setPort(score, Value::ofFloat(0.5f)) == Status::kOk);
    ui.step();
    const size_t before = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    for (int i = 1; i <= 1000; ++i) {
        REQUIRE(ui.doc->setPort(score, Value::ofFloat(static_cast<float>(i) + 0.5f)) == Status::kOk);
        ui.step();
    }
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) == before);
    CHECK(ui.doc->byId("A").text() == "1000.5");
}

TEST_CASE("runtime pools: a removed element's text slot is reused by the next one") {
    Ui ui = open(page("", R"X(<div id="A">x</div>)X"));
    ElementRef a = ui.doc->byId("A");
    for (int i = 0; i < 1000; ++i) {
        ElementRef e = ui.doc->createElement("div");
        ui.doc->appendChild(a, e);
        e.setText("item " + std::to_string(i));
        CHECK(e.text() == "item " + std::to_string(i));
        ui.step();
        ui.doc->remove(e);
    }
    CHECK(RuntimePoolTestAccess::runtimeTexts(*ui.doc) <= 1);
    CHECK(a.text() == "x");
}

// -- C6 (2): query() does not intern the names it is given ----------------------------------------

TEST_CASE("runtime pools: query with 1000 names nothing carries does not grow the runtime string pool") {
    Ui ui = open(page("", R"X(<div id="A" class="k">x</div>)X"));
    const size_t before = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    std::vector<ElementRef> found;
    for (int i = 0; i < 1000; ++i) {
        const std::string n = std::to_string(i);
        CHECK(ui.doc->query("#missing" + n, found) == Status::kOk);
        CHECK(found.empty());
        CHECK(ui.doc->query("div.gone" + n, found) == Status::kOk);
        CHECK(found.empty());
        CHECK(ui.doc->query("tag" + n + " .k", found) == Status::kOk);
        CHECK(found.empty());
    }
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) == before);
    // Names that exist still match, a runtime-interned class included; bad syntax still fails.
    CHECK(ui.doc->query("div.k", found) == Status::kOk);
    CHECK(found.size() == 1);
    ui.doc->byId("A").addClass("late");
    CHECK(ui.doc->query("#A.late", found) == Status::kOk);
    CHECK(found.size() == 1);
    CHECK(ui.doc->query("#nothing >", found) == Status::kInvalidArgument);
    CHECK(ui.doc->query("#nothing ::before", found) == Status::kInvalidArgument);
}

// -- C6 (3): one runtime effect segment per element -----------------------------------------------

TEST_CASE("runtime pools: setStyle effect alternating 1000 times keeps the runtime effect pool bounded") {
    Ui ui = open(page("", R"X(<div id="A">x</div><div id="B">y</div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    const char* kEffects[2] = {"effect: shadow(1px 2px 3px #f00) outline(2px #00f inner)",
                               "effect: blur(4px)"};
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(a.setStyle(kEffects[i % 2]) == Status::kOk);
        ui.step();
    }
    CAPTURE(ui.doc->runtimeEffects().size());
    CHECK(ui.doc->runtimeEffects().size() <= 2);
    // The cascade still sees the last declaration (i = 999: blur).
    CHECK(a.effectCount() == 1);
    REQUIRE(a.setStyle(kEffects[0]) == Status::kOk);
    ui.step();
    CHECK(a.effectCount() == 2);
    CHECK(ui.doc->runtimeEffects().size() <= 2);
    // A removed element gives its segment back: another element's declaration reuses it.
    ui.doc->remove(a);
    ElementRef b = ui.doc->byId("B");
    REQUIRE(b.setStyle(kEffects[1]) == Status::kOk);
    ui.step();
    CHECK(ui.doc->runtimeEffects().size() <= 2);
    CHECK(b.effectCount() == 1);
}

TEST_CASE("runtime pools: a reused effect segment still delivers changed parameters") {
    // Same kind and count: the declaration lands in the element's own segment at the same offset,
    // so the cascaded value is identical; the new parameters must reach the element all the same.
    Ui ui = open(page("", R"X(<div id="A" style="width: 50px; height: 20px">x</div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    REQUIRE(a.setStyle("effect: blur(4px)") == Status::kOk);
    ui.step();
    REQUIRE(a.effectCount() == 1);
    const EffectInstance first = a.effect(0);
    REQUIRE(a.setStyle("effect: blur(9px)") == Status::kOk);
    ui.step();
    REQUIRE(a.effectCount() == 1);
    const EffectInstance second = a.effect(0);
    CHECK(ui.doc->runtimeEffects().size() <= 1);
    CHECK((second.params[0] != first.params[0] || second.params[1] != first.params[1]));
}

// -- B14: the node effect pool -------------------------------------------------------------------

TEST_CASE("runtime pools: a :hover effect switched 1000 times keeps the node effect pool bounded") {
    // A switches between two one-effect sequences, B between one and two effects; every switch
    // appends a segment, and update()'s automatic compaction keeps the pool bounded.
    Ui ui = open(page(R"X(
        #A { position: absolute; left: 0px; top: 0px; width: 100px; height: 50px;
             effect: shadow(1px 2px 3px #f00); }
        #A:hover { effect: blur(4px); }
        #B { position: absolute; left: 200px; top: 0px; width: 100px; height: 50px;
             effect: blur(2px); }
        #B:hover { effect: shadow(1px 2px 3px #f00) outline(2px #00f inner); })X",
                      R"X(<div id="A">a</div><div id="B">b</div>)X"));
    ElementRef a = ui.doc->byId("A");
    ElementRef b = ui.doc->byId("B");
    auto moveTo = [&](Vec2 pos) {
        InputEvent ev;
        ev.kind = InputEvent::PointerMove;
        ev.pos = pos;
        ui.doc->dispatchInput(ev);
        ui.step();
    };
    moveTo(Vec2{50, 25});
    REQUIRE(a.hasState(kStateHover));
    moveTo(Vec2{250, 25});
    REQUIRE(b.hasState(kStateHover));
    const size_t base = ui.doc->arena().effects().size();
    for (int i = 0; i < 1000; ++i) {
        moveTo(i % 2 == 0 ? Vec2{50, 25} : Vec2{250, 25});
    }
    CAPTURE(base);
    CAPTURE(ui.doc->arena().effects().size());
    CHECK(ui.doc->arena().effects().size() <= base + 512);
    // The last move (i = 999) hovers B.
    CHECK(a.effectCount() == 1);
    CHECK(a.effect(0).kind == EffectKind::DropShadow);
    CHECK(b.effectCount() == 2);
    CHECK(b.effect(1).kind == EffectKind::Outline);
}

// -- C12: the IME composition string pool ---------------------------------------------------------

TEST_CASE("runtime pools: update() empties the composition pool after dispatching 1000 CompositionUpdates") {
    ContextConfig config;
    Context ctx(config);
    for (int i = 0; i < 1000; ++i) {
        const std::string text = "ni" + std::to_string(i);
        InputEvent ev;
        ev.kind = InputEvent::CompositionUpdate;
        ev.composition = text;
        ev.compositionCursor = static_cast<uint32_t>(text.size());
        ctx.pushInput(ev);
    }
    CHECK(RuntimePoolTestAccess::compositionPool(ctx) == 1000);
    ctx.update(0.016f);
    CHECK(ctx.pendingInputCount() == 0);
    CHECK(RuntimePoolTestAccess::compositionPool(ctx) == 0);
    // drainInput() itself (no update) still leaves the pooled strings readable.
    InputEvent ev;
    ev.kind = InputEvent::CompositionUpdate;
    std::string text = "ka";
    ev.composition = text;
    ctx.pushInput(ev);
    text.clear();
    std::vector<InputEvent> drained;
    ctx.drainInput(drained);
    REQUIRE(drained.size() == 1);
    CHECK(drained[0].composition == "ka");
}

// -- C13: host-created tags and attributes resolve through the runtime string pool ----------------

namespace {

/// A document whose IR string table holds none of `button`, `input`, `type`, `checkbox`, `lang`
/// or `ja`, so every one of them is interned at runtime.
Ui openPlain() {
    Ui ui = open(page("div { width: 40px; height: 20px; }", R"X(<div id="A">x</div>)X"));
    REQUIRE(ui.doc->ir().strings().count() > 0);
    for (uint32_t i = 0; i < ui.doc->ir().strings().count(); ++i) {
        const std::string_view s = ui.doc->ir().string(i);
        REQUIRE(s != "button");
        REQUIRE(s != "input");
        REQUIRE(s != "type");
        REQUIRE(s != "lang");
    }
    return ui;
}

InputEvent keyDown(KeyCode key) {
    InputEvent ev;
    ev.kind = InputEvent::KeyDown;
    ev.key = key;
    return ev;
}

}  // namespace

TEST_CASE("runtime strings: createElement(\"button\") is a button (Space fires Click)") {
    Ui ui = openPlain();
    ElementRef button = ui.doc->createElement("button");
    ui.doc->appendChild(ui.doc->byId("A"), button);
    ui.step();
    int clicks = 0;
    ui.doc->addListener(button, EventType::Click, [&](Event&) { ++clicks; });
    ui.doc->setFocus(button);
    REQUIRE(ui.doc->focus().node == button.node);
    ui.doc->dispatchInput(keyDown(KeyCode::Space));
    CHECK(clicks == 1);
}

TEST_CASE("runtime strings: setAttribute(\"lang\", \"ja\") sets the element's language") {
    Ui ui = openPlain();
    ElementRef a = ui.doc->byId("A");
    CHECK(a.setAttribute("lang", "ja") == Status::kOk);
    ui.step();
    const StringId lang = ui.doc->arena().element(a.node).style.lang;
    CHECK(ui.doc->string(lang) == "ja");
}

TEST_CASE("runtime strings: setAttribute(\"type\", \"checkbox\") makes a created input a checkbox") {
    Ui ui = openPlain();
    ElementRef box = ui.doc->createElement("input");
    CHECK(box.setAttribute("type", "checkbox") == Status::kOk);
    ui.doc->appendChild(ui.doc->byId("A"), box);
    ui.step();
    ui.doc->setFocus(box);
    REQUIRE(ui.doc->focus().node == box.node);
    ui.doc->dispatchInput(keyDown(KeyCode::Space));
    CHECK(box.hasState(kStateChecked));
}

// -- T-270b (1): consumers of a Text node's runtime text id ---------------------------------------

namespace {

/// FakeTextShaper whose glyph outlines differ per glyph: glyph g fills the em column
/// [(g % 4) / 4, (g % 4 + 1) / 4), so 'A' (65) and 'B' (66) cover different pixels.
class ColumnGlyphShaper : public FakeTextShaper {
public:
    bool glyphOutline(FontId, uint32_t glyph, std::vector<PathVerb>& verbs,
                      std::vector<float>& points) override {
        const float x0 = static_cast<float>(glyph % 4) * 0.25f;
        verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
        points = {x0, 0.0f, x0 + 0.25f, 0.0f, x0 + 0.25f, 0.7f, x0, 0.7f};
        return true;
    }
};

/// Every DrawGlyphs glyph id of `list`, in order; `bounds` gets the union of those commands' bounds.
std::vector<uint32_t> glyphIds(const CommandList& list, Rect* bounds = nullptr) {
    std::vector<uint32_t> out;
    Rect all{};
    bool any = false;
    for (const Command& cmd : list.commands) {
        if (cmd.kind != CmdKind::DrawGlyphs) {
            continue;
        }
        const GlyphsCmd& run = list.glyphRuns[cmd.index];
        for (uint32_t i = 0; i < run.glyphCount; ++i) {
            out.push_back(list.glyphs[run.glyphOffset + i]);
        }
        const Rect r = commandBounds(list, cmd);
        all = any ? unite(all, r) : r;
        any = true;
    }
    if (bounds != nullptr) {
        *bounds = all;
    }
    return out;
}

bool rectContains(const Rect& outer, const Rect& inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

}  // namespace

TEST_CASE("T-270b: setText twice with equal-length values draws the new glyphs and damages the text (SW)") {
    constexpr int kW = 240;
    constexpr int kH = 80;
    ColumnGlyphShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    const CompileResult compiled =
        compileXt(page("#A { position: absolute; left: 10px; top: 10px; width: 200px; height: 50px;"
                       "     font-size: 32px; color: #000000; }",
                       R"X(<div id="A">x</div>)X"));
    REQUIRE(compiled.ok());
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{compiled.ir.data(), compiled.ir.size()}, false);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)}, 1.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    ElementRef a = doc->byId("A");
    REQUIRE(a.valid());

    CommandList list;
    DamageList damage;
    a.setText("AAAA");
    ctx.update(0.016f);
    ctx.render(list, &damage);
    CHECK(glyphIds(list) == std::vector<uint32_t>{65, 65, 65, 65});
    std::vector<uint8_t> first(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{first.data(), kW, kH, kW * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, &shaper, nullptr);

    // Same length, same node, same slot (overwritten in place): the new text is shaped and drawn.
    const StringId slot = doc->arena().text(doc->arena().firstChild(a.node)).text;
    CHECK(slot >= kRuntimeTextBase);
    a.setText("BBBB");
    CHECK(doc->arena().text(doc->arena().firstChild(a.node)).text == slot);
    ctx.update(0.016f);
    ctx.render(list, &damage);
    Rect textBounds{};
    CHECK(glyphIds(list, &textBounds) == std::vector<uint32_t>{66, 66, 66, 66});
    CHECK(!damage.full);
    REQUIRE(!damage.rects.empty());
    Rect damaged = damage.rects[0];
    for (const Rect& r : damage.rects) {
        damaged = unite(damaged, r);
    }
    CAPTURE(textBounds.x);
    CAPTURE(textBounds.w);
    CAPTURE(damaged.x);
    CAPTURE(damaged.w);
    CHECK(textBounds.w > 0);
    // commandBounds() is conservative (outset by the glyph size), damage is clipped to the viewport.
    const float x0 = std::max(textBounds.x, 0.0f);
    const float y0 = std::max(textBounds.y, 0.0f);
    const float x1 = std::min(textBounds.x + textBounds.w, static_cast<float>(kW));
    const float y1 = std::min(textBounds.y + textBounds.h, static_cast<float>(kH));
    CHECK(rectContains(damaged, Rect{x0, y0, x1 - x0, y1 - y0}));

    // The partial redraw over the first frame equals a full render of the second, which differs.
    std::vector<uint8_t> partial = first;
    renderer.setTarget(sw::SwTarget{partial.data(), kW, kH, kW * 4});
    renderer.clearRects(Color{1, 1, 1, 1}, damage.rects);
    renderer.render(list, &shaper, nullptr, nullptr, nullptr, &damage);
    std::vector<uint8_t> full(static_cast<size_t>(kW) * kH * 4, 0);
    renderer.setTarget(sw::SwTarget{full.data(), kW, kH, kW * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, &shaper, nullptr);
    CHECK(full != first);
    CHECK(partial == full);
}

// -- T-270b (2): `<repeat>` copies never share a runtime text slot ---------------------------------

TEST_CASE("T-270b: <repeat> instances, recycled ones included, each own their runtime text slot") {
    Ui ui = open(page("", R"X(
        <div id="P" port:items="list<struct{name: string}>"></div>
        <div id="Scroll" style="position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: scroll">
          <repeat port="items" id="Item" virtualize>
            <div id="Row" style="height: 20px"><span id="Name">tpl</span></div>
          </repeat>
        </div>)X"));
    // The template's own span: a detached template element, which byId() does not return.
    ElementRef tpl;
    for (uint32_t i = 0; i < ui.doc->ir().doc().elements().size(); ++i) {
        const ElementRef e = ui.doc->elementAt(i);
        if (e.valid() && e.id() == "Name") {
            tpl = e;
        }
    }
    REQUIRE(tpl.valid());
    tpl.setText("template text");

    REQUIRE(ui.doc->setPortText(ui.port("items"), R"([{"name":"a"},{"name":"b"},{"name":"c"}])") ==
            Status::kOk);
    for (int i = 0; i < 3; ++i) {
        ui.step();
    }
    REQUIRE(ui.doc->repeatInstances("Item").size() == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(ui.doc->byId("Item/" + std::to_string(i) + "/Name").text() == "template text");
    }
    ui.doc->byId("Item/0/Name").setText("first");
    ui.doc->byId("Item/1/Name").setText("second");
    ui.doc->byId("Item/2/Name").setText("third");
    ui.step();

    // Remove the middle one (recycled into the pool), then add an entry: the pooled one comes back.
    const NodeId recycledRow = ui.doc->byId("Item/1/Row").node;
    REQUIRE(ui.doc->listRemove(ui.port("items"), 1) == Status::kOk);
    ui.step();
    StringId names[1] = {ui.doc->intern("name")};
    Value fields[1] = {Value::ofStringId(ui.doc->intern("d"))};
    REQUIRE(ui.doc->listInsert(ui.port("items"), 2, Value::ofStruct(names, fields, 1)) == Status::kOk);
    ui.step();
    REQUIRE(ui.doc->repeatInstances("Item").size() == 3);
    REQUIRE(ui.doc->byId("Item/2/Row").node == recycledRow);

    CHECK(tpl.text() == "template text");
    CHECK(ui.doc->byId("Item/0/Name").text() == "first");
    CHECK(ui.doc->byId("Item/1/Name").text() == "third");
    CHECK(ui.doc->byId("Item/2/Name").text() == "template text");  // Reset from the template.
    ui.doc->byId("Item/2/Name").setText("fourth");
    ui.step();
    CHECK(ui.doc->byId("Item/2/Name").text() == "fourth");
    CHECK(ui.doc->byId("Item/1/Name").text() == "third");
    CHECK(tpl.text() == "template text");

    // One slot per Text node, each owned by the node that holds it.
    const NodeArena& arena = ui.doc->arena();
    const ElementRef holders[4] = {tpl, ui.doc->byId("Item/0/Name"), ui.doc->byId("Item/1/Name"),
                                   ui.doc->byId("Item/2/Name")};
    std::vector<StringId> slots;
    for (const ElementRef& e : holders) {
        REQUIRE(e.valid());
        const NodeId textNode = arena.firstChild(e.node);
        REQUIRE(textNode != kInvalidNode);
        REQUIRE(arena.kind(textNode) == NodeKind::Text);
        const StringId id = arena.text(textNode).text;
        CHECK(id >= kRuntimeTextBase);
        CHECK(RuntimePoolTestAccess::runtimeTextOwner(*ui.doc, id) == textNode);
        for (StringId other : slots) {
            CHECK(other != id);
        }
        slots.push_back(id);
    }
}

// -- T-270b (3): an effect transition across an in-place runtime effect declaration ---------------

TEST_CASE("T-270b: an effect transition between two in-place setStyle effect values interpolates") {
    Ui ui = open(
        page("#A { width: 50px; height: 20px; transition: effect 1s linear; }", R"X(<div id="A">x</div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    REQUIRE(a.setStyle("effect: blur(2px)") == Status::kOk);
    ui.step();
    REQUIRE(a.effectCount() == 1);
    CHECK(a.effect(0).params[0] == doctest::Approx(2.0f));
    // Same kind and count: the declaration overwrites the element's runtime segment in place.
    REQUIRE(a.setStyle("effect: blur(10px)") == Status::kOk);
    ui.step();
    ui.step(0.5f);
    REQUIRE(a.effectCount() == 1);
    const float mid = a.effect(0).params[0];
    CAPTURE(mid);
    CHECK(mid > 2.0f);
    CHECK(mid < 10.0f);
    ui.step(1.0f);
    CHECK(a.effect(0).params[0] == doctest::Approx(10.0f));
}

// -- T-270b (4): an effect transition across update()'s automatic compaction ----------------------

TEST_CASE("T-270b: a :hover effect transition stays continuous across the automatic effect compaction") {
    Ui ui = open(page(R"X(
        #F { position: absolute; left: 200px; top: 0px; width: 50px; height: 50px;
             effect: shadow(1px 1px 1px #000000); }
        #A { position: absolute; left: 0px; top: 0px; width: 100px; height: 50px;
             effect: shadow(1px 1px 1px #000000); transition: effect 1s linear; }
        #A.b { effect: blur(2px); }
        #A.b:hover { effect: blur(10px); })X",
                      R"X(<div id="F">f</div><div id="A">a</div>)X"));
    ElementRef f = ui.doc->byId("F");
    ElementRef a = ui.doc->byId("A");
    REQUIRE(f.valid());
    REQUIRE(a.valid());
    // F switches effect kind every frame: each switch appends a segment and leaves a hole.
    int flip = 0;
    auto churn = [&] {
        ++flip;
        REQUIRE(f.setStyle(flip % 2 == 0 ? "effect: blur(1px)" : "effect: shadow(1px 1px 1px #000000)") ==
                Status::kOk);
    };
    for (int i = 0; i < 200; ++i) {
        churn();
        ui.step();
    }
    // A's blur segment is appended after F's holes, so the compaction moves it.
    a.addClass("b");
    ui.step();
    REQUIRE(a.effectCount() == 1);
    REQUIRE(a.effect(0).kind == EffectKind::Blur);
    CHECK(a.effect(0).params[0] == doctest::Approx(2.0f));
    const uint32_t offsetBefore = ui.doc->arena().common(a.node).effectOffset;
    REQUIRE(ui.doc->arena().effects().size() <= 256);

    InputEvent move;
    move.kind = InputEvent::PointerMove;
    move.pos = Vec2{50, 25};
    ui.doc->dispatchInput(move);
    ui.step(0.004f);
    REQUIRE(a.hasState(kStateHover));
    float last = a.effect(0).params[0];
    CHECK(last >= 2.0f);
    bool compacted = false;
    size_t poolSize = ui.doc->arena().effects().size();
    for (int i = 0; i < 150; ++i) {
        churn();
        ui.step(0.004f);
        const size_t size = ui.doc->arena().effects().size();
        compacted = compacted || size < poolSize;
        poolSize = size;
        REQUIRE(a.effectCount() == 1);
        CHECK(a.effect(0).kind == EffectKind::Blur);  // Never another element's effect.
        const float now = a.effect(0).params[0];
        CAPTURE(i);
        CAPTURE(last);
        CAPTURE(now);
        CHECK(now >= last);        // Continuous: no step back ...
        CHECK(now - last < 0.1f);  // ... and no jump (8 px/s x 4 ms per frame).
        last = now;
    }
    CHECK(compacted);
    CHECK(ui.doc->arena().common(a.node).effectOffset != offsetBefore);
    CHECK(last < 10.0f);  // Still mid-transition after the compaction.
    ui.step(2.0f);
    REQUIRE(a.effectCount() == 1);
    CHECK(a.effect(0).kind == EffectKind::Blur);
    CHECK(a.effect(0).params[0] == doctest::Approx(10.0f));
}

// -- T-270b (5): query() and negation -------------------------------------------------------------

TEST_CASE("T-270b: query() rejects :not(), so an unknown name inside one is never read as unmatchable") {
    Ui ui = open(page("", R"X(<div id="A" class="k">x</div>)X"));
    std::vector<ElementRef> found;
    CHECK(ui.doc->query(":not(.nope)", found) == Status::kInvalidArgument);
    CHECK(ui.doc->query("div:not(.k)", found) == Status::kInvalidArgument);
    CHECK(ui.doc->query("div:not(#A)", found) == Status::kInvalidArgument);
    CHECK(found.empty());
}

// -- T-270b (7): incremental live effect counts ----------------------------------------------------

TEST_CASE("T-270b: NodeArena's live effect counts follow assignment, overwrite, removal and compaction") {
    NodeArena arena;
    const NodeId root = arena.create(NodeKind::Element);
    const NodeId n1 = arena.create(NodeKind::Element);
    const NodeId n2 = arena.create(NodeKind::Element);
    arena.appendChild(root, n1);
    arena.appendChild(root, n2);
    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    blur.enabled = 1;
    EffectInstance mask{};
    mask.kind = EffectKind::GradientMask;
    mask.enabled = 1;
    mask.stopOffset = 0;
    mask.stopCount = 2;
    const GradientStop stops[3] = {GradientStop{0, Color{0, 0, 0, 1}},
                                   GradientStop{0.5f, Color{0, 0, 0, 0.5f}},
                                   GradientStop{1, Color{0, 0, 0, 0}}};
    size_t effects = 0;
    size_t liveStops = 0;
    auto counts = [&] { arena.liveEffectCounts(effects, liveStops); };

    const EffectInstance pair[2] = {blur, mask};
    REQUIRE(arena.assignEffects(n1, pair, std::span<const GradientStop>(stops, 2)) == Status::kOk);
    REQUIRE(arena.assignEffects(n2, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);
    counts();
    CHECK(effects == 3);
    CHECK(liveStops == 2);
    // In place with a longer stop list: the old stops become a hole, the live count follows.
    EffectInstance mask3 = mask;
    mask3.stopCount = 3;
    const EffectInstance pair3[2] = {blur, mask3};
    REQUIRE(arena.assignEffects(n1, pair3, stops) == Status::kOk);
    counts();
    CHECK(effects == 3);
    CHECK(liveStops == 3);
    CHECK(arena.effectStops().size() == 5);
    // A kind change appends; the old segment becomes a hole.
    REQUIRE(arena.assignEffects(n2, std::span<const EffectInstance>(&mask, 1),
                                std::span<const GradientStop>(stops, 2)) == Status::kOk);
    counts();
    CHECK(effects == 3);
    CHECK(liveStops == 5);
    CHECK(arena.effects().size() == 4);
    // Removal, then an empty sequence.
    arena.destroy(n2);
    counts();
    CHECK(effects == 2);
    CHECK(liveStops == 3);
    REQUIRE(arena.assignEffects(n1, {}, {}) == Status::kOk);
    counts();
    CHECK(effects == 0);
    CHECK(liveStops == 0);
    REQUIRE(arena.assignEffects(n1, pair, std::span<const GradientStop>(stops, 2)) == Status::kOk);
    arena.compactEffects();
    counts();
    CHECK(effects == 2);
    CHECK(liveStops == 2);
    CHECK(arena.effects().size() == effects);
    CHECK(arena.effectStops().size() == liveStops);
}

// -- T-270c (confirmation): every writer of a node's effect range keeps the live counts -----------

namespace {

/// The full scan NodeArena::liveEffectCounts() replaced (T-270b), done here independently.
void scanEffects(const NodeArena& arena, size_t& effects, size_t& stops) {
    effects = 0;
    stops = 0;
    for (uint32_t id = 0; id < arena.capacity(); ++id) {
        if (!arena.alive(id)) {
            continue;
        }
        const NodeCommon& c = arena.common(id);
        effects += c.effectCount;
        for (uint32_t i = 0; i < c.effectCount; ++i) {
            stops += arena.effects()[c.effectOffset + i].stopCount;
        }
    }
}

/// Kind, parameters, colours and stops of an element's effects (its offsets left out).
struct EffectSnapshot {
    std::vector<EffectInstance> effects;
    std::vector<GradientStop> stops;
};

EffectSnapshot snapshotEffects(const Document& doc, NodeId node) {
    const NodeArena& arena = doc.arena();
    const NodeCommon& c = arena.common(node);
    EffectSnapshot out;
    for (uint32_t i = 0; i < c.effectCount; ++i) {
        EffectInstance e = arena.effects()[c.effectOffset + i];
        for (uint32_t s = 0; s < e.stopCount; ++s) {
            out.stops.push_back(arena.effectStops()[e.stopOffset + s]);
        }
        e.stopOffset = 0;
        out.effects.push_back(e);
    }
    return out;
}

bool sameColor(const Color& a, const Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool sameEffects(const EffectSnapshot& a, const EffectSnapshot& b) {
    if (a.effects.size() != b.effects.size() || a.stops.size() != b.stops.size()) {
        return false;
    }
    for (size_t i = 0; i < a.effects.size(); ++i) {
        const EffectInstance& x = a.effects[i];
        const EffectInstance& y = b.effects[i];
        if (x.kind != y.kind || x.enabled != y.enabled || x.stopCount != y.stopCount ||
            !sameColor(x.colors[0], y.colors[0]) || !sameColor(x.colors[1], y.colors[1])) {
            return false;
        }
        for (int p = 0; p < 8; ++p) {
            if (x.params[p] != y.params[p]) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < a.stops.size(); ++i) {
        if (a.stops[i].offset != b.stops[i].offset || !sameColor(a.stops[i].color, b.stops[i].color)) {
            return false;
        }
    }
    return true;
}

std::string itemsJson(int n) {
    std::string json = "[";
    for (int i = 0; i < n; ++i) {
        json += (i == 0 ? "{\"name\":\"" : ",{\"name\":\"") + std::to_string(i) + "\"}";
    }
    return json + "]";
}

}  // namespace

TEST_CASE("T-270c: <repeat> instances keep their effects across the automatic compaction") {
    // 150 instances of a template with two effects (one with gradient stops): past 256 node effect
    // entries. Even instances get their own `effect` through setStyle (an append, so holes), half
    // the list goes to the recycle pool, and the list grows back (the pooled instances come back
    // reset from the template, appending again): update() compacts along the way.
    Ui ui = open(page(R"X(
        .row { height: 1px;
               effect: gradient-mask(linear, 0px 0px, 10px 10px, #fff 0%, #000 100%, invert)
                       outline(2px #00f inner); })X",
                      R"X(
        <div id="P" port:items="list<struct{name: string}>"></div>
        <div id="Scroll" style="position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: scroll">
          <repeat port="items" id="Item" virtualize>
            <div id="Row" class="row"><span id="Name">x</span></div>
          </repeat>
        </div>)X"));
    constexpr int kCount = 150;
    auto row = [&](int i) { return ui.doc->byId("Item/" + std::to_string(i) + "/Row"); };
    auto checkCounts = [&] {
        size_t liveEffects = 0;
        size_t liveStops = 0;
        ui.doc->arena().liveEffectCounts(liveEffects, liveStops);  // Debug: asserts against a scan.
        size_t scanned = 0;
        size_t scannedStops = 0;
        scanEffects(ui.doc->arena(), scanned, scannedStops);
        CHECK(liveEffects == scanned);
        CHECK(liveStops == scannedStops);
    };
    REQUIRE(ui.doc->setPortText(ui.port("items"), itemsJson(kCount)) == Status::kOk);
    for (int i = 0; i < 3; ++i) {
        ui.step();
    }
    REQUIRE(ui.doc->repeatInstances("Item").size() == kCount);
    REQUIRE(ui.doc->arena().effects().size() > 256);
    const EffectSnapshot tplEffects = snapshotEffects(*ui.doc, row(1).node);
    REQUIRE(tplEffects.effects.size() == 2);
    REQUIRE(tplEffects.stops.size() == 2);
    checkCounts();

    for (int i = 0; i < kCount; i += 2) {
        REQUIRE(row(i).setStyle("effect: blur(" + std::to_string(i + 1) + "px)") == Status::kOk);
    }
    ui.step();
    std::vector<EffectSnapshot> expected(kCount);
    for (int i = 0; i < kCount; ++i) {
        expected[static_cast<size_t>(i)] = snapshotEffects(*ui.doc, row(i).node);
        if (i % 2 == 0) {
            REQUIRE(expected[static_cast<size_t>(i)].effects.size() == 1);
            CHECK(expected[static_cast<size_t>(i)].effects[0].kind == EffectKind::Blur);
        } else {
            CHECK(sameEffects(expected[static_cast<size_t>(i)], tplEffects));
        }
    }
    checkCounts();

    // Half the list goes away (recycled, alive in the pool), then comes back.
    REQUIRE(ui.doc->setPortText(ui.port("items"), itemsJson(kCount / 2)) == Status::kOk);
    ui.step();
    ui.step();
    REQUIRE(ui.doc->repeatInstances("Item").size() == kCount / 2);
    checkCounts();
    REQUIRE(ui.doc->setPortText(ui.port("items"), itemsJson(kCount)) == Status::kOk);
    for (int i = 0; i < 3; ++i) {
        ui.step();
    }
    REQUIRE(ui.doc->repeatInstances("Item").size() == kCount);
    checkCounts();
    size_t liveEffects = 0;
    size_t liveStops = 0;
    ui.doc->arena().liveEffectCounts(liveEffects, liveStops);
    CAPTURE(ui.doc->arena().effects().size());
    CAPTURE(liveEffects);
    // The automatic compaction ran: no more than half the pool is holes.
    CHECK(ui.doc->arena().effects().size() <= std::max<size_t>(256, liveEffects * 2));
    for (int i = 0; i < kCount; ++i) {
        CAPTURE(i);
        const EffectSnapshot now = snapshotEffects(*ui.doc, row(i).node);
        // The instances that came back from the pool were reset from the template.
        CHECK(sameEffects(now, i < kCount / 2 ? expected[static_cast<size_t>(i)] : tplEffects));
    }
}

// -- T-270c: `background-gradient` / `animation` declarations, two alternating slots per element --

TEST_CASE(
    "T-270c: setStyle background-gradient alternating 1000 times uses two slots and repaints each time") {
    Ui ui = open(page("", R"X(<div id="A" style="width: 50px; height: 20px">x</div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    const char* kGradients[2] = {"background-gradient: linear-gradient(90deg, #f00, #00f)",
                                 "background-gradient: linear-gradient(0deg, #0f0, #ff0, #fff)"};
    const ElementData& data = ui.doc->arena().element(a.node);
    uint32_t lastOffset = data.style.gradientOffset;
    for (int i = 0; i < 1000; ++i) {
        // The same value twice in a row too: T-389 (27 §27.4.1 (d)) made the repeat a no-op.
        REQUIRE(a.setStyle(kGradients[(i / 2) % 2]) == Status::kOk);
        ui.step();
        CAPTURE(i);
        // The cascaded offset changes with every new value (what markNonAnimatableDirty repaints
        // on), and stays for the repeat.
        if (i % 2 == 0) {
            CHECK(data.style.gradientOffset != lastOffset);
        } else {
            CHECK(data.style.gradientOffset == lastOffset);
        }
        lastOffset = data.style.gradientOffset;
        REQUIRE(data.backgroundGradient.kind == GradientKind::Linear);
        CHECK(data.backgroundGradient.stops.size() == ((i / 2) % 2 == 0 ? 2u : 3u));
    }
    CAPTURE(ui.doc->runtimeGradients().size());
    CHECK(ui.doc->runtimeGradients().size() <= 2);
    // A removed element gives its slots back: another element's declarations reuse them.
    ui.doc->remove(a);
    ui.step();
    ElementRef root = ui.doc->byId("Root");
    REQUIRE(root.setStyle(kGradients[0]) == Status::kOk);
    ui.step();
    REQUIRE(root.setStyle(kGradients[1]) == Status::kOk);
    ui.step();
    CHECK(ui.doc->runtimeGradients().size() <= 2);
    CHECK(ui.doc->arena().element(root.node).backgroundGradient.stops.size() == 3);
}

TEST_CASE("T-270c: an effect switching between two stop counts 1000 times keeps the node stop pool bounded") {
    // Same kind sequence, different stop count: NodeArena::assignEffects() overwrites the effect in
    // place but appends its stops (18.2); update()'s automatic compaction bounds the stop pool.
    Ui ui = open(page("", R"X(<div id="A" style="width: 50px; height: 20px">x</div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    const char* kMasks[2] = {
        "effect: gradient-mask(linear, 0px 0px, 10px 10px, #fff 0%, #000 100%)",
        "effect: gradient-mask(linear, 0px 0px, 10px 10px, #fff 0%, #888 50%, #000 100%)"};
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(a.setStyle(kMasks[i % 2]) == Status::kOk);
        ui.step();
        CAPTURE(i);
        REQUIRE(a.effectCount() == 1);
        CHECK(a.effect(0).stopCount == (i % 2 == 0 ? 2u : 3u));
    }
    CAPTURE(ui.doc->arena().effectStops().size());
    CHECK(ui.doc->arena().effectStops().size() <= 2 * 256 + 3);
    CHECK(ui.doc->arena().effects().size() <= 256);
}

TEST_CASE("T-270c: setStyle animation alternating 1000 times stays bounded and restarts each time") {
    Ui ui = open(page("@keyframes spin { from { opacity: 1 } to { opacity: 0.5 } }",
                      R"X(<div id="A" style="width: 50px; height: 20px">x</div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    const char* kAnimations[3] = {"animation: spin 1s linear", "animation: spin 2s linear",
                                  "animation: spin 1s linear, spin 3s linear"};
    std::vector<uint32_t> last;
    for (int i = 0; i < 1000; ++i) {
        // The same declaration twice in a row as well: declared again, it restarts (17.6).
        const int which = (i / 2) % 3;
        REQUIRE(a.setStyle(kAnimations[which]) == Status::kOk);
        ui.step();
        CAPTURE(i);
        const std::vector<uint32_t> playing = ui.doc->animationsOf(a);
        REQUIRE(playing.size() == (which == 2 ? 2u : 1u));
        CHECK(ui.doc->animationTimelineDuration(playing[0]) == doctest::Approx(which == 1 ? 2.0f : 1.0f));
        for (uint32_t id : playing) {
            CHECK(std::find(last.begin(), last.end(), id) == last.end());  // A new instance.
        }
        last = playing;
    }
    CAPTURE(ui.doc->runtimeAnimations().size());
    // Two slots, each grown once from one item to two (the outgrown one-item spans stay free).
    CHECK(ui.doc->runtimeAnimations().size() <= 6);
}

// -- T-270c: string-valued bindings ---------------------------------------------------------------

namespace {

size_t unboundedWarnings(const Document& doc) {
    size_t n = 0;
    for (const std::string& w : doc.warnings()) {
        n += w.find("produces unbounded distinct strings") != std::string::npos ? size_t{1} : size_t{0};
    }
    return n;
}

}  // namespace

TEST_CASE("T-270c: a string binding into text with a new value every frame does not grow the string pool") {
    // Text targets through a pipeline (format), from a string port set every frame, and a trigger
    // `set` of a string port from the event's value: no runtime string is interned.
    Ui ui = open(page("", R"X(
        <div id="A" port:name="string" bind:text="format('<{0}>', name)"></div>
        <div id="B" bind:text="name | upper"></div>
        <div id="C" port:copy="string" bind:text="copy" on:port(name)="set(copy, event.value)"></div>)X"));
    const PortId name = ui.port("name");
    REQUIRE(ui.doc->setPortText(name, "start") == Status::kOk);
    ui.step();
    const size_t before = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(ui.doc->setPortText(name, "value " + std::to_string(i)) == Status::kOk);
        ui.step();
    }
    CAPTURE(RuntimePoolTestAccess::runtimeStrings(*ui.doc) - before);
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) == before);
    CHECK(ui.doc->byId("A").text() == "<value 999>");
    CHECK(ui.doc->byId("B").text() == "VALUE 999");
    CHECK(ui.doc->byId("C").text() == "value 999");
    CHECK(unboundedWarnings(*ui.doc) == 0);
}

TEST_CASE("T-270c: a string binding into a class grows only by its distinct values and warns past 1024") {
    Ui ui = open(R"X(<xt><head>
      <symbol name="Btn" variants="Kind=Primary|Secondary"><div id="Bg"></div></symbol>
      <style>#Root { width: 100px; }</style></head>
      <body id="Root"><div id="P" port:kind="string"></div>
      <use symbol="Btn" id="B1" variant:Kind="Primary" bind:variant.Kind="kind"></use></body></xt>)X");
    const PortId kind = ui.port("kind");
    const char* kValues[3] = {"Primary", "Secondary", "Tertiary"};
    REQUIRE(ui.doc->setPortText(kind, kValues[0]) == Status::kOk);
    ui.step();
    const size_t before = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(ui.doc->setPortText(kind, kValues[i % 3]) == Status::kOk);
        ui.step();
    }
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) <= before + 3);
    CHECK(ui.doc->byId("B1").hasClass("v-Kind-Primary"));  // i = 999.
    CHECK(unboundedWarnings(*ui.doc) == 0);
    // 1100 distinct values: the pool grows by each (the class needs a StringId), one warning.
    for (int i = 0; i < 1100; ++i) {
        REQUIRE(ui.doc->setPortText(kind, "K" + std::to_string(i)) == Status::kOk);
        ui.step();
    }
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) <= before + 3 + 1100);
    CHECK(ui.doc->byId("B1").hasClass("v-Kind-K1099"));
    CHECK(unboundedWarnings(*ui.doc) == 1);
}

// -- T-270c: setStyle's other text-valued declarations ----------------------------------------------

TEST_CASE("T-270c: setStyle text values alternating 1000 times do not grow the string pools") {
    // `font-family` alternates between a name the stylesheet compiled (it resolves) and a new one
    // each time (no compiled font-family has it, so it is ignored, as before); a style binding
    // writes a new `background-image` name every frame through the same setStyle path.
    Ui ui = open(page("#K { font-family: Known; }", R"X(
        <div id="K">k</div>
        <div id="A" style="width: 50px; height: 20px">x</div>
        <div id="B" port:img="string" bind:background-image="img"></div>)X"));
    ElementRef a = ui.doc->byId("A");
    REQUIRE(a.valid());
    const StringId known = ui.doc->intern("Known");
    REQUIRE(known < kRuntimeStringBase);  // Compiled.
    const PortId img = ui.port("img");
    ui.step();
    const size_t strings = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    const size_t texts = RuntimePoolTestAccess::runtimeTexts(*ui.doc);
    const ComputedStyle& style = ui.doc->arena().element(a.node).style;
    std::span<const uint32_t> families = ui.doc->ir().style().fontFamilies();
    for (int i = 0; i < 1000; ++i) {
        const bool compiled = i % 2 == 0;
        REQUIRE(a.setStyle(compiled ? std::string("font-family: Known")
                                    : "font-family: Runtime" + std::to_string(i)) == Status::kOk);
        REQUIRE(ui.doc->setPortText(img, "image" + std::to_string(i)) == Status::kOk);
        ui.step();
        if (compiled) {
            CAPTURE(i);
            REQUIRE(style.fontFamilyCount == 1);
            REQUIRE(style.fontFamilyOffset < families.size());
            CHECK(families[style.fontFamilyOffset] == known);
        }
    }
    CAPTURE(RuntimePoolTestAccess::runtimeStrings(*ui.doc) - strings);
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) == strings);
    // One text slot per (element, property) at most.
    CHECK(RuntimePoolTestAccess::runtimeTexts(*ui.doc) <= texts + 2);
    // A removed element gives its slot back.
    ui.doc->remove(a);
    ui.step();
    ElementRef k = ui.doc->byId("K");
    REQUIRE(k.setStyle("font-family: Another") == Status::kOk);
    ui.step();
    CHECK(RuntimePoolTestAccess::runtimeTexts(*ui.doc) <= texts + 2);
}

// -- T-270c: `<repeat>` instance ids -----------------------------------------------------------------

namespace {

const char* kSpawnDoc = R"X(<xt><head>
      <symbol name="Pop"><div id="Box"><span id="Amount" bind:text="amount"></span></div></symbol>
      <style>
        #Field { position: absolute; left: 20px; top: 20px; width: 100px; height: 50px; }
        symbol(Pop) { #Box { width: 10px; height: 10px; } }
      </style>
    </head><body id="Root" port:amount="number in" port:hit="event in">
      <div id="Field" on:port(hit)="spawn(Pop, at=self, lifetime=0.5s)"></div>
    </body></xt>)X";

}  // namespace

TEST_CASE("T-270c: <repeat> spawn / expire 1000 rounds keeps the string pools bounded") {
    Ui ui = open(kSpawnDoc);
    const PortId hit = ui.port("hit");
    auto round = [&] {
        REQUIRE(ui.doc->setPort(hit, Value::ofFloat(0)) == Status::kOk);
        ui.step(0.01f);
        REQUIRE(ui.doc->repeatInstances("spawn-0").size() == 1);
        ui.step(0.6f);  // Past its lifetime: recycled into the pool.
        REQUIRE(ui.doc->repeatInstances("spawn-0").empty());
    };
    for (int i = 0; i < 10; ++i) {
        round();
    }
    const size_t strings = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    const size_t texts = RuntimePoolTestAccess::runtimeTexts(*ui.doc);
    for (int i = 0; i < 1000; ++i) {
        round();
    }
    CAPTURE(RuntimePoolTestAccess::runtimeStrings(*ui.doc) - strings);
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) == strings);
    CHECK(RuntimePoolTestAccess::runtimeTexts(*ui.doc) == texts);
}

TEST_CASE("T-270c: a recycled <repeat> instance is found by its new id, not by its old one") {
    Ui ui = open(kSpawnDoc);
    const PortId hit = ui.port("hit");
    REQUIRE(ui.doc->setPort(hit, Value::ofFloat(0)) == Status::kOk);
    ui.step(0.01f);
    REQUIRE(ui.doc->repeatInstances("spawn-0").size() == 1);
    // A spawn instance's root is the `<use>` copy, `spawn-0/<serial>/spawn-0-Pop`.
    const ElementRef first = ui.doc->byId("spawn-0/0/spawn-0-Pop");
    REQUIRE(first.valid());
    CHECK(ui.doc->repeatInstances("spawn-0")[0].node == first.node);
    ui.step(0.6f);  // Expired: recycled.
    CHECK_FALSE(ui.doc->byId("spawn-0/0/spawn-0-Pop").valid());

    REQUIRE(ui.doc->setPort(hit, Value::ofFloat(0)) == Status::kOk);
    ui.step(0.01f);
    const ElementRef second = ui.doc->byId("spawn-0/1/spawn-0-Pop");
    REQUIRE(second.valid());
    CHECK(second.node == first.node);  // The pooled instance came back...
    CHECK(second.id() == "spawn-0/1/spawn-0-Pop");
    CHECK_FALSE(ui.doc->byId("spawn-0/0/spawn-0-Pop").valid());  // ...under its new id only.
    // Every node of the instance was renamed: none answers to the old prefix.
    const NodeArena& arena = ui.doc->arena();
    arena.traversePreorder(second.node, [&](NodeId n) {
        if (arena.kind(n) == NodeKind::Element && arena.element(n).id != kInvalidString) {
            const std::string_view id = ui.doc->string(arena.element(n).id);
            CHECK(id.substr(0, 10) == "spawn-0/1/");
            CHECK(ui.doc->byId(id).node == n);
        }
        return true;
    });
}

// -- T-270d (1): a recycled <repeat> instance drops its committed slot references ------------------

namespace {

const char* kRecycleDoc = R"X(
        <div id="P" port:items="list<struct{name: string}>"></div>
        <div id="Other" style="width: 50px; height: 20px">o</div>
        <div id="Scroll" style="position: absolute; left: 0px; top: 40px; width: 200px; height: 100px; overflow: scroll">
          <repeat port="items" id="Item" virtualize>
            <div id="Row" class="row"><span id="Name">x</span></div>
          </repeat>
        </div>)X";

}  // namespace

TEST_CASE(
    "T-270d: a recycled <repeat> instance drops its committed effect / gradient / animation references") {
    Ui ui = open(page(R"X(
        .row { width: 50px; height: 10px; transition: effect 1s linear; }
        @keyframes fade { from { opacity: 1 } to { opacity: 0.5 } })X",
                      kRecycleDoc));
    const PortId items = ui.port("items");
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);
    ui.step();
    ui.step();
    ElementRef row = ui.doc->byId("Item/0/Row");
    REQUIRE(row.valid());
    const NodeId node = row.node;
    REQUIRE(row.setStyle("effect: blur(3px)") == Status::kOk);
    REQUIRE(row.setStyle("background-gradient: linear-gradient(90deg, #f00, #00f)") == Status::kOk);
    REQUIRE(row.setStyle("animation: fade 1s linear") == Status::kOk);
    ui.step();
    const NodeArena& arena = ui.doc->arena();
    const ComputedStyle& committed = arena.element(node).style;
    REQUIRE(row.effectCount() == 1);
    REQUIRE((committed.effectOffset & kRuntimeEffectOffsetBit) != 0);
    REQUIRE((committed.gradientOffset & kRuntimeEffectOffsetBit) != 0);
    REQUIRE(committed.gradientCount == 1);
    REQUIRE((committed.lastAnimationOffset & kRuntimeEffectOffsetBit) != 0);
    const uint32_t effectSlot = committed.effectOffset;
    const uint32_t gradientSlot = committed.gradientOffset;

    // Recycled: the slots go back to the pools, and the committed style no longer points at them.
    REQUIRE(ui.doc->setPortText(items, itemsJson(0)) == Status::kOk);
    ui.step();
    ui.step();
    REQUIRE(ui.doc->repeatInstances("Item").empty());
    REQUIRE(arena.alive(node));
    CHECK(committed.effectCount == 0);
    CHECK((committed.effectOffset & kRuntimeEffectOffsetBit) == 0);
    CHECK(committed.gradientCount == 0);
    CHECK((committed.gradientOffset & kRuntimeEffectOffsetBit) == 0);
    CHECK(committed.animationCount == 0);
    CHECK((committed.animationOffset & kRuntimeEffectOffsetBit) == 0);
    CHECK(committed.lastAnimationCount == 0);
    CHECK((committed.lastAnimationOffset & kRuntimeEffectOffsetBit) == 0);

    // Another element takes the freed effect segment and gradient slot with its own values.
    ElementRef other = ui.doc->byId("Other");
    REQUIRE(other.setStyle("effect: blur(20px)") == Status::kOk);
    REQUIRE(other.setStyle("background-gradient: linear-gradient(0deg, #0f0, #ff0)") == Status::kOk);
    ui.step();
    const ComputedStyle& otherStyle = arena.element(other.node).style;
    CHECK(otherStyle.effectOffset == effectSlot);
    CHECK(otherStyle.gradientOffset == gradientSlot);

    // The instance comes back: its first frame starts from no effect and no gradient, not from the
    // other element's values; an effect declared on it then applies without a transition from them.
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);
    ui.step();
    ElementRef again = ui.doc->byId("Item/0/Row");
    REQUIRE(again.valid());
    REQUIRE(again.node == node);
    CHECK(again.effectCount() == 0);
    CHECK(arena.element(node).backgroundGradient.kind == GradientKind::None);
    CHECK(ui.doc->animationsOf(again).empty());
    REQUIRE(again.setStyle("effect: blur(10px)") == Status::kOk);
    ui.step();
    REQUIRE(again.effectCount() == 1);
    CHECK(again.effect(0).params[0] == doctest::Approx(10.0f));
    ui.step(0.5f);
    CHECK(again.effect(0).params[0] == doctest::Approx(10.0f));
    // The other element kept its own values.
    REQUIRE(other.effectCount() == 1);
    CHECK(other.effect(0).params[0] == doctest::Approx(20.0f));
    CHECK(arena.element(other.node).backgroundGradient.stops.size() == 2);
}

// -- T-270d (3): the unbounded-string guard's warning and the FrameStats string counts --------------

TEST_CASE("T-270d: the unbounded-string warning names the fix and FrameStats carries the string counts") {
    Ui ui = open(R"X(<xt><head>
      <symbol name="Btn" variants="Kind=Primary|Secondary"><div id="Bg"></div></symbol>
      <style>#Root { width: 100px; }</style></head>
      <body id="Root"><div id="P" port:kind="string"></div>
      <use symbol="Btn" id="B1" variant:Kind="Primary" bind:variant.Kind="kind"></use></body></xt>)X");
    PerfTracker perf;
    perf.setEnabled(true);
    ui.doc->setPerfTracker(&perf);
    const PortId kind = ui.port("kind");
    for (int i = 0; i < 1100; ++i) {
        REQUIRE(ui.doc->setPortText(kind, "K" + std::to_string(i)) == Status::kOk);
        ui.step();
    }
    REQUIRE(ui.doc->byId("B1").hasClass("v-Kind-K1099"));
    size_t warnings = 0;
    for (const std::string& w : ui.doc->warnings()) {
        if (w.find("produces unbounded distinct strings") == std::string::npos) {
            continue;
        }
        ++warnings;
        CAPTURE(w);
        CHECK(w.rfind("binding \"", 0) == 0);
        CHECK(w.find("\" produces unbounded distinct strings; bind it to text or a String port instead of a "
                     "class / converter argument") != std::string::npos);
    }
    CHECK(warnings == 1);
    perf.beginFrame(ui.clock);
    ui.step();
    const FrameStats& stats = perf.current();
    CHECK(stats.runtimeStrings == RuntimePoolTestAccess::runtimeStrings(*ui.doc));
    CHECK(stats.maxDistinctBindingStrings == 1100);
    ui.doc->setPerfTracker(nullptr);
}

// -- T-270e: setStyle names a font family or an image the compiled document does not have ----------

namespace {

/// How many of `doc`'s warnings are exactly `text`.
size_t countWarnings(const Document& doc, std::string_view text) {
    return static_cast<size_t>(std::count(doc.warnings().begin(), doc.warnings().end(), text));
}

/// How many of `doc`'s warnings start with `prefix`.
size_t countWarningsStartingWith(const Document& doc, std::string_view prefix) {
    return static_cast<size_t>(
        std::count_if(doc.warnings().begin(), doc.warnings().end(),
                      [prefix](const std::string& w) { return w.rfind(prefix, 0) == 0; }));
}

std::string imageWarning(std::string_view name) {
    return "image \"" + std::string(name) + "\" is not an asset of the compiled document";
}

/// T-270f: the text for every font-family warning, whatever the list's length.
std::string familyWarning(std::string_view first) {
    return "font-family \"" + std::string(first) +
           "\" is not declared by the compiled document; setStyle uses only the first family of a list "
           "and can only select families the document names (declare it in a stylesheet, or use "
           "@font-fallback)";
}

}  // namespace

TEST_CASE("T-270e: setStyle background-image naming no asset warns once per element; known names do not") {
    Ui ui = open(page(R"X(#Known { background-image: url(known.png); })X",
                      R"X(<div id="A"></div><div id="B"></div><div id="C"></div><div id="Known"></div>)X"));
    ElementRef a = ui.doc->byId("A");
    ElementRef b = ui.doc->byId("B");
    ElementRef c = ui.doc->byId("C");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(c.valid());
    const NodeArena& arena = ui.doc->arena();
    REQUIRE(arena.element(ui.doc->byId("Known").node).style.backgroundImage != kInvalidAsset);

    REQUIRE(a.setStyle("background-image: \"missing.png\"") == Status::kOk);
    CHECK(countWarnings(*ui.doc, imageWarning("missing.png")) == 1);
    ui.step();
    CHECK(arena.element(a.node).style.backgroundImage == kInvalidAsset);  // Still nothing.
    // The same element and property again, with the same or another missing name: no second one.
    REQUIRE(a.setStyle("background-image: \"missing.png\"") == Status::kOk);
    REQUIRE(a.setStyle("background-image: other") == Status::kOk);
    CHECK(countWarnings(*ui.doc, imageWarning("missing.png")) == 1);
    CHECK(countWarningsStartingWith(*ui.doc, "image \"") == 1);
    // Another element warns on its own.
    REQUIRE(b.setStyle("background-image: \"missing.png\"") == Status::kOk);
    CHECK(countWarnings(*ui.doc, imageWarning("missing.png")) == 2);

    // A name the asset table has, `none` and an empty name: no warning.
    const size_t before = ui.doc->warnings().size();
    REQUIRE(c.setStyle("background-image: \"known.png\"") == Status::kOk);
    REQUIRE(c.setStyle("background-image: none") == Status::kOk);
    REQUIRE(c.setStyle("background-image: \"\"") == Status::kOk);
    REQUIRE(c.setStyle("background-image: \"known.png\"") == Status::kOk);
    CHECK(ui.doc->warnings().size() == before);
    ui.step();
    CHECK(arena.element(c.node).style.backgroundImage != kInvalidAsset);

    // What setStyle's parser accepts today: a bare or quoted name. `url(...)` and
    // `border-image-source` are rejected before any value is written, so nothing warns (T-270e
    // report: the sheet's url() / border-image-source cases do not reach a written value).
    REQUIRE(c.setStyle("background-image: url(missing.png)") == Status::kInvalidArgument);
    REQUIRE(c.setStyle("border-image-source: \"missing.png\"") == Status::kInvalidArgument);
    CHECK(ui.doc->warnings().size() == before);
}

// T-270f: the condition is "this declaration has no effect": setStyle uses only the first family
// of a list, so the warning keys on that one, whatever follows it.
TEST_CASE("T-270f: setStyle font-family warns when the list's first family is not declared") {
    Ui ui =
        open(page(R"X(#Decl { font-family: "Declared Face", sans-serif; })X",
                  R"X(<div id="A">a</div><div id="B">b</div><div id="C">c</div><div id="Decl">d</div>)X"));
    ElementRef a = ui.doc->byId("A");
    ElementRef b = ui.doc->byId("B");
    ElementRef c = ui.doc->byId("C");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(c.valid());

    // An undeclared first family warns, even when a later one is declared.
    REQUIRE(a.setStyle("font-family: Missing, \"Declared Face\"") == Status::kOk);
    CHECK(countWarnings(*ui.doc, familyWarning("Missing")) == 1);
    REQUIRE(a.setStyle("font-family: Another") == Status::kOk);  // Same element and property.
    CHECK(countWarningsStartingWith(*ui.doc, "font-family \"") == 1);
    // A list of undeclared families, and an undeclared family alone: the same text.
    REQUIRE(b.setStyle("font-family: \"Missing Face\", Other Face") == Status::kOk);
    CHECK(countWarnings(*ui.doc, familyWarning("Missing Face")) == 1);
    CHECK(countWarningsStartingWith(*ui.doc, "font-family \"") == 2);

    // A declared first family (whatever follows), a declared family alone, a CSS-wide keyword: no
    // warning.
    const size_t before = ui.doc->warnings().size();
    REQUIRE(c.setStyle("font-family: \"Declared Face\", Missing") == Status::kOk);
    REQUIRE(c.setStyle("font-family: \"Declared Face\"") == Status::kOk);
    REQUIRE(c.setStyle("font-family: inherit") == Status::kOk);
    CHECK(ui.doc->warnings().size() == before);
}

TEST_CASE("T-270e: a recycled and reused <repeat> instance warns again for the same setStyle") {
    Ui ui = open(page(".row { width: 50px; height: 10px; }", kRecycleDoc));
    const PortId items = ui.port("items");
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);
    ui.step();
    ui.step();
    ElementRef row = ui.doc->byId("Item/0/Row");
    REQUIRE(row.valid());
    const NodeId node = row.node;
    REQUIRE(row.setStyle("background-image: \"missing.png\"") == Status::kOk);
    REQUIRE(row.setStyle("background-image: \"missing.png\"") == Status::kOk);
    CHECK(countWarnings(*ui.doc, imageWarning("missing.png")) == 1);

    REQUIRE(ui.doc->setPortText(items, itemsJson(0)) == Status::kOk);  // Recycled into the pool.
    ui.step();
    ui.step();
    REQUIRE(ui.doc->repeatInstances("Item").empty());
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);  // Reused.
    ui.step();
    ElementRef again = ui.doc->byId("Item/0/Row");
    REQUIRE(again.valid());
    REQUIRE(again.node == node);
    REQUIRE(again.setStyle("background-image: \"missing.png\"") == Status::kOk);
    CHECK(countWarnings(*ui.doc, imageWarning("missing.png")) == 2);
}

TEST_CASE("T-270e: setStyle / setText through a kept ref to a pooled <repeat> instance are refused") {
    Ui ui = open(page(".row { width: 50px; height: 10px; }", kRecycleDoc));
    const PortId items = ui.port("items");
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);
    ui.step();
    ui.step();
    ElementRef row = ui.doc->byId("Item/0/Row");
    ElementRef name = ui.doc->byId("Item/0/Name");
    REQUIRE(row.valid());
    REQUIRE(name.valid());
    const NodeArena& arena = ui.doc->arena();
    REQUIRE(row.setStyle("width: 60px; effect: blur(2px)") == Status::kOk);
    name.setText("live");
    ui.step();
    REQUIRE(!arena.element(row.node).runtimeDecls.empty());

    // Recycled: the refs stay valid (the nodes are alive, detached, waiting in the pool), and the
    // released slots took the element's runtime declarations with them.
    REQUIRE(ui.doc->setPortText(items, itemsJson(0)) == Status::kOk);
    ui.step();
    ui.step();
    REQUIRE(ui.doc->repeatInstances("Item").empty());
    REQUIRE(row.valid());
    REQUIRE(name.valid());
    CHECK(arena.element(row.node).runtimeDecls.empty());

    const size_t strings = RuntimePoolTestAccess::runtimeStrings(*ui.doc);
    const size_t texts = RuntimePoolTestAccess::runtimeTexts(*ui.doc);
    const size_t segments = RuntimePoolTestAccess::runtimeEffectSegments(*ui.doc);
    const size_t warnings = ui.doc->warnings().size();
    CHECK(row.setStyle("width: 70px; effect: blur(9px); background-image: \"refused.png\"") ==
          Status::kInvalidArgument);
    name.setText("refused");
    CHECK(arena.element(row.node).runtimeDecls.empty());
    CHECK(name.text() != "refused");
    CHECK(RuntimePoolTestAccess::runtimeStrings(*ui.doc) == strings);
    CHECK(RuntimePoolTestAccess::runtimeTexts(*ui.doc) == texts);
    CHECK(RuntimePoolTestAccess::runtimeEffectSegments(*ui.doc) == segments);
    CHECK(ui.doc->warnings().size() == warnings);

    // Reused: the instance shows the template's values, not the refused ones.
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);
    ui.step();
    ui.step();
    ElementRef again = ui.doc->byId("Item/0/Row");
    REQUIRE(again.valid());
    REQUIRE(again.node == row.node);
    CHECK(again.layoutBox().width == doctest::Approx(50.0f));
    CHECK(again.effectCount() == 0);
    CHECK(ui.doc->byId("Item/0/Name").text() == "x");
    // And a live instance takes setStyle / setText again.
    CHECK(again.setStyle("width: 70px") == Status::kOk);
    ui.doc->byId("Item/0/Name").setText("back");
    ui.step();
    CHECK(again.layoutBox().width == doctest::Approx(70.0f));
    CHECK(ui.doc->byId("Item/0/Name").text() == "back");
}

// -- T-270f: every mutating ElementRef call on a pooled <repeat> instance node is refused ----------

namespace {

const char* kPooledCallsDoc = R"X(
        <div id="P" port:items="list<struct{name: string}>"></div>
        <div id="Other" tabindex="0" style="width: 50px; height: 20px">o</div>
        <div id="Gone" style="width: 50px; height: 20px">g</div>
        <div id="Scroll" style="position: absolute; left: 0px; top: 40px; width: 200px; height: 100px; overflow: scroll">
          <repeat port="items" id="Item" virtualize>
            <div id="Row" class="row" tabindex="0" data-k="t"><span id="Name">x</span></div>
          </repeat>
        </div>)X";

/// What a node and the Document's runtime slots look like, for "the call changed nothing".
struct PooledSnap {
    std::vector<StringId> classes;
    uint16_t stateBits = 0;
    std::vector<std::pair<StringId, StringId>> attributes;
    std::string id;
    StringId inlineStyle = kInvalidString;
    size_t runtimeDecls = 0;
    std::string text;
    size_t children = 0;
    float opacity = 0;
    uint32_t effectCount = 0;
    float effectParam = 0;
    bool override = false;
    uint8_t dirty = 0;
    NodeId focus = kInvalidNode;
    size_t runtimeStrings = 0;
    size_t runtimeTexts = 0;
    size_t effectSegments = 0;
    size_t warnings = 0;
    bool operator==(const PooledSnap&) const = default;
};

PooledSnap snapPooled(Document& doc, ElementRef ref) {
    const NodeArena& arena = doc.arena();
    const ElementData& data = arena.element(ref.node);
    PooledSnap s;
    s.classes = data.classes;
    s.stateBits = data.stateBits;
    s.attributes = data.attributes;
    s.id = std::string(ref.id());
    s.inlineStyle = data.inlineStyle;
    s.runtimeDecls = data.runtimeDecls.size();
    s.text = std::string(ref.text());
    for (NodeId c = arena.firstChild(ref.node); c != kInvalidNode; c = arena.nextSibling(c)) {
        ++s.children;
    }
    s.opacity = arena.common(ref.node).opacity;
    s.effectCount = ref.effectCount();
    s.effectParam = s.effectCount > 0 ? ref.effect(0).params[0] : 0.0f;
    s.override = doc.animator().hasOverride(ref.node, prop::Opacity);
    s.dirty = arena.dirty(ref.node);
    s.focus = doc.focus().node;
    s.runtimeStrings = RuntimePoolTestAccess::runtimeStrings(doc);
    s.runtimeTexts = RuntimePoolTestAccess::runtimeTexts(doc);
    s.effectSegments = RuntimePoolTestAccess::runtimeEffectSegments(doc);
    s.warnings = doc.warnings().size();
    return s;
}

std::string outcome(Status s) { return "status " + std::to_string(static_cast<int>(s)); }
std::string outcome(bool b) { return b ? "true" : "false"; }
const std::string kNoChange = "void";

/// One mutating ElementRef call (every one the header declares) and what it does on a removed node.
struct MutatingCall {
    const char* name;
    std::string (*call)(ElementRef& ref);
    std::string onRemoved;
};

const EffectInstance kBlur = [] {
    EffectInstance e;
    e.kind = EffectKind::Blur;
    e.params[0] = 6.0f;
    return e;
}();

const MutatingCall kMutatingCalls[] = {
    {"setAttribute(data-k)", [](ElementRef& r) { return outcome(r.setAttribute("data-k", "refused")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(data-new)", [](ElementRef& r) { return outcome(r.setAttribute("data-new", "refused")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(class)", [](ElementRef& r) { return outcome(r.setAttribute("class", "refused")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(id)", [](ElementRef& r) { return outcome(r.setAttribute("id", "Refused")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(style)", [](ElementRef& r) { return outcome(r.setAttribute("style", "width: 90px")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(src)", [](ElementRef& r) { return outcome(r.setAttribute("src", "a.png")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(value)", [](ElementRef& r) { return outcome(r.setAttribute("value", "refused")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(checked)", [](ElementRef& r) { return outcome(r.setAttribute("checked", "true")); },
     outcome(Status::kInvalidArgument)},
    {"setAttribute(disabled)", [](ElementRef& r) { return outcome(r.setAttribute("disabled", "true")); },
     outcome(Status::kInvalidArgument)},
    {"setText",
     [](ElementRef& r) {
         r.setText("refused");
         return kNoChange;
     },
     kNoChange},
    {"setStyle", [](ElementRef& r) { return outcome(r.setStyle("width: 90px; effect: blur(9px)")); },
     outcome(Status::kInvalidArgument)},
    {"setProperty",
     [](ElementRef& r) {
         r.setProperty(prop::Opacity, Value::ofFloat(0.25f));
         return kNoChange;
     },
     kNoChange},
    {"setProperty(sticky)",
     [](ElementRef& r) {
         r.setProperty(prop::Opacity, Value::ofFloat(0.25f), /*sticky=*/true);
         return kNoChange;
     },
     kNoChange},
    {"clearOverride",
     [](ElementRef& r) {
         r.clearOverride(prop::Opacity);
         return kNoChange;
     },
     kNoChange},
    {"setEffectParam",
     [](ElementRef& r) {
         r.setEffectParam(0, 1, Value::ofFloat(7.0f));
         return kNoChange;
     },
     kNoChange},
    {"setEffects",
     [](ElementRef& r) {
         return outcome(
             r.setEffects(std::span<const EffectInstance>(&kBlur, 1), std::span<const GradientStop>{}));
     },
     outcome(Status::kInvalidArgument)},
    {"focus",
     [](ElementRef& r) {
         r.focus();
         return kNoChange;
     },
     kNoChange},
    {"addClass", [](ElementRef& r) { return outcome(r.addClass("extra")); }, outcome(false)},
    {"removeClass", [](ElementRef& r) { return outcome(r.removeClass("row")); }, outcome(false)},
    {"toggleClass(present)", [](ElementRef& r) { return outcome(r.toggleClass("row")); }, outcome(false)},
    {"toggleClass(absent)", [](ElementRef& r) { return outcome(r.toggleClass("extra")); }, outcome(false)},
    {"setState(checked)", [](ElementRef& r) { return outcome(r.setState(kStateChecked, true)); },
     outcome(false)},
    {"setState(disabled)", [](ElementRef& r) { return outcome(r.setState(kStateDisabled, true)); },
     outcome(false)},
    {"setValue",
     [](ElementRef& r) {
         r.setValue("refused");
         return kNoChange;
     },
     kNoChange},
};

}  // namespace

TEST_CASE(
    "T-270f: every mutating ElementRef call on a pooled <repeat> instance node behaves as on a removed "
    "node") {
    Ui ui = open(page(".row { width: 50px; height: 10px; effect: blur(1px); }", kPooledCallsDoc));
    const PortId items = ui.port("items");
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);
    ui.step();
    ui.step();
    ElementRef row = ui.doc->byId("Item/0/Row");
    REQUIRE(row.valid());
    REQUIRE(row.effectCount() == 1);
    const float templateBlur = row.effect(0).params[0];
    ElementRef other = ui.doc->byId("Other");
    REQUIRE(other.valid());
    other.focus();
    REQUIRE(ui.doc->focus().node == other.node);
    ElementRef gone = ui.doc->byId("Gone");
    REQUIRE(gone.valid());
    ui.doc->remove(gone);
    REQUIRE(!gone.valid());  // A removed node: its generation no longer matches.

    // Recycled: the ref stays valid (alive, detached, waiting in the pool).
    REQUIRE(ui.doc->setPortText(items, itemsJson(0)) == Status::kOk);
    ui.step();
    ui.step();
    REQUIRE(ui.doc->repeatInstances("Item").empty());
    REQUIRE(row.valid());

    for (const MutatingCall& c : kMutatingCalls) {
        CAPTURE(c.name);
        ElementRef removed = gone;
        CHECK(c.call(removed) == c.onRemoved);
        const PooledSnap before = snapPooled(*ui.doc, row);
        ElementRef pooled = row;
        CHECK(c.call(pooled) == c.onRemoved);
        CHECK(snapPooled(*ui.doc, row) == before);  // No change, and no runtime slot taken.
    }

    // Reused: the instance shows the template's values.
    REQUIRE(ui.doc->setPortText(items, itemsJson(1)) == Status::kOk);
    ui.step();
    ui.step();
    ElementRef again = ui.doc->byId("Item/0/Row");
    REQUIRE(again.valid());
    REQUIRE(again.node == row.node);
    CHECK(again.hasClass("row"));
    CHECK(!again.hasClass("extra"));
    CHECK(!again.hasClass("refused"));
    CHECK(again.attribute("data-k") == "t");
    CHECK(again.attribute("data-new").empty());
    CHECK(!again.hasState(kStateChecked));
    CHECK(!again.hasState(kStateDisabled));
    CHECK(again.layoutBox().width == doctest::Approx(50.0f));
    CHECK(again.property(prop::Opacity).f == doctest::Approx(1.0f));
    CHECK(!ui.doc->animator().hasOverride(again.node, prop::Opacity));
    REQUIRE(again.effectCount() == 1);
    CHECK(again.effect(0).params[0] == doctest::Approx(templateBlur));
    CHECK(ui.doc->byId("Item/0/Name").text() == "x");
    CHECK(ui.doc->focus().node == other.node);

    // A live instance takes every call again.
    CHECK(again.addClass("extra"));
    CHECK(again.removeClass("extra"));
    CHECK(again.setAttribute("data-k", "u") == Status::kOk);
    CHECK(again.setState(kStateChecked, true));
    again.setProperty(prop::Opacity, Value::ofFloat(0.5f));
    CHECK(again.property(prop::Opacity).f == doctest::Approx(0.5f));
    again.setEffectParam(0, 1, Value::ofFloat(7.0f));
    CHECK(again.effect(0).params[0] == doctest::Approx(7.0f));
    CHECK(again.setEffects(std::span<const EffectInstance>(&kBlur, 1), std::span<const GradientStop>{}) ==
          Status::kOk);
    again.focus();
    CHECK(ui.doc->focus().node == again.node);
    CHECK(again.attribute("data-k") == "u");
    CHECK(again.hasState(kStateChecked));
}
