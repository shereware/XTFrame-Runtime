#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/input.h"
#include "xt/input_channels.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-873 (Docs/design/60-input-channels.md §60.2, §60.3.1, §60.3.2, §60.5; phase I2): the channel
/// table (names, the three kinds, a frame's pushes summed), the standard channels the existing input
/// events move (the right stick and triggers are no longer dropped), the `input(<channel>)` binding
/// source, the `input(...)` / `.press` / `.release` triggers, player prefixes, needsFrame() and the
/// Context's own table.

using namespace xt;
using namespace xt::compile;

namespace {

std::string doc(std::string_view style, std::string_view body, std::string_view bodyAttrs = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\" " +
           std::string(bodyAttrs) + ">" + std::string(body) + "</body></xt>";
}

CompileResult compileXt(const std::string& xt) {
    CompileOptions options;
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

    void step(float dt = 0.016f) {
        clock += dt;
        doc->update(clock);
    }
    void push(const InputEvent& ev) { doc->dispatchInput(ev); }
    void channel(std::string_view name, float value, float strength = 1.0f, uint8_t flags = 0,
                 uint8_t player = 0) {
        InputEvent ev;
        ev.kind = InputEvent::Channel;
        ev.channel = doc->inputChannel(name);
        REQUIRE(ev.channel != kInvalidChannel);
        ev.channelValue = value;
        ev.channelStrength = strength;
        ev.channelFlags = flags;
        ev.player = player;
        push(ev);
    }
    void axis(GamepadAxis a, float v, uint8_t player = 0) {
        InputEvent ev;
        ev.kind = InputEvent::GamepadAxisMotion;
        ev.gamepadAxis = a;
        ev.axisValue = v;
        ev.player = player;
        push(ev);
    }
    float port(std::string_view name) const {
        const PortId id = doc->lookupPort(name);
        REQUIRE(id != kInvalidPort);
        return doc->getPort(id).f;
    }
    float opacity(std::string_view id) {
        const ElementRef e = doc->byId(id);
        REQUIRE(e.valid());
        return e.property(prop::Opacity).f;
    }
};

Loaded load(const std::string& xt) {
    Loaded out;
    out.result = compileXt(xt);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    const Status status =
        Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr, out.doc);
    REQUIRE(status == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 300, 300});
    out.doc->update(0.0f);
    return out;
}

}  // namespace

// -- Names ----------------------------------------------------------------------------------------

TEST_CASE("T-873: channel names: grammar, canonical spelling, player prefix, fixed kinds") {
    std::string c;
    uint8_t p = 9;
    CHECK(parseChannelRef("pad.rx", c, p));
    CHECK(c == "pad.rx");
    CHECK(p == 0);
    CHECK(parseChannelRef("player(3).pad.a", c, p));
    CHECK(c == "pad.a");
    CHECK(p == 3);
    CHECK(parseChannelRef("midi.cc( 07 )", c, p));
    CHECK(c == "midi.cc(7)");
    CHECK(parseChannelRef("midi.ch(16).note(127)", c, p));
    CHECK(c == "midi.ch(16).note(127)");
    CHECK(parseChannelRef("key.Left", c, p));
    CHECK(c == "key.ArrowLeft");
    CHECK(parseChannelRef("key.A", c, p));
    CHECK(c == "key.KeyA");
    CHECK(parseChannelRef("key.Shift", c, p));
    CHECK(parseChannelRef("key.NumpadEnter", c, p));
    CHECK(parseChannelRef("host.steering wheel", c, p));  // Blanks are dropped.
    CHECK(c == "host.steeringwheel");
    CHECK(parseChannelRef("action:confirm", c, p));
    std::string err;
    CHECK_FALSE(parseChannelRef("pad.z", c, p, &err));
    CHECK(err.find("pad.z") != std::string::npos);
    CHECK_FALSE(parseChannelRef("midi.cc(128)", c, p));
    CHECK_FALSE(parseChannelRef("midi.ch(0).cc(1)", c, p));
    CHECK_FALSE(parseChannelRef("player(8).pad.a", c, p));
    CHECK_FALSE(parseChannelRef("joy.x", c, p));
    CHECK_FALSE(parseChannelRef("host.", c, p));
    CHECK_FALSE(parseChannelRef("key.Nope", c, p));

    ChannelKind k = ChannelKind::Axis;
    CHECK((fixedChannelKind("pad.a", k) && k == ChannelKind::Button));
    CHECK((fixedChannelKind("pad.lt", k) && k == ChannelKind::Axis));
    CHECK((fixedChannelKind("wheel.y", k) && k == ChannelKind::Relative));
    CHECK((fixedChannelKind("pointer.dx", k) && k == ChannelKind::Relative));
    CHECK((fixedChannelKind("midi.note(36)", k) && k == ChannelKind::Button));
    CHECK((fixedChannelKind("midi.pitch", k) && k == ChannelKind::Axis));
    CHECK_FALSE(fixedChannelKind("midi.cc(1)", k));
    CHECK_FALSE(fixedChannelKind("host.x", k));
    CHECK_FALSE(fixedChannelKind("action:tune", k));

    KeyCode code = KeyCode::Unknown;
    CHECK((keyCodeFromName("ShiftLeft", code) && code == KeyCode::ShiftLeft));
    CHECK((keyCodeFromName("Slash", code) && code == KeyCode::Slash));
    CHECK((keyCodeFromName("Numpad7", code) && code == KeyCode::Numpad7));
    CHECK(keyCodeName(KeyCode::Key0) == "Digit0");
    CHECK(keyCodeName(KeyCode::Up) == "ArrowUp");
}

// -- The three kinds --------------------------------------------------------------------------------

TEST_CASE("T-873: an axis takes the last push, a relative channel sums a frame, a button keeps both edges") {
    InputChannels t;
    const ChannelId ax = t.intern("host.lever");
    const ChannelId rel = t.intern("host.knob");
    const ChannelId btn = t.intern("host.trigger");
    t.push(ax, 0.25f, 1, 0, 0);
    t.push(ax, 0.75f, 1, 0, 0);
    t.push(rel, 2, 1, kChannelRelative, 0);
    t.push(rel, 3, 1, kChannelRelative, 0);
    t.push(rel, -1, 1, kChannelRelative, 0);
    t.push(btn, 1, 0.5f, kChannelButton, 0);
    t.push(btn, 0, 0.5f, kChannelButton, 0);
    CHECK(t.pending());
    t.commit();
    CHECK(t.kind(ax) == ChannelKind::Axis);
    CHECK(t.kind(rel) == ChannelKind::Relative);
    CHECK(t.kind(btn) == ChannelKind::Button);
    CHECK(t.value(ax) == doctest::Approx(0.75f));
    CHECK(t.changed(ax));
    CHECK(t.value(rel) == doctest::Approx(4.0f));
    CHECK(t.changed(rel));
    CHECK(t.value(btn) == 0.0f);  // Pressed and released within the frame.
    CHECK(t.pressed(btn));
    CHECK(t.released(btn));
    CHECK(t.strength(btn) == doctest::Approx(0.5f));
    CHECK(t.changes().size() == 3);
    // A quiet frame: the relative sum goes back to 0 (moved, but no change event), the edges go.
    CHECK(t.settling());
    t.commit();
    CHECK(t.value(rel) == 0.0f);
    CHECK(t.moved(rel));
    CHECK_FALSE(t.changed(rel));
    CHECK_FALSE(t.pressed(btn));
    CHECK_FALSE(t.changed(ax));
    CHECK(t.delta(ax) == 0.0f);
    t.commit();
    CHECK_FALSE(t.settling());
    // The kind of the first push stays (a later flag does not change it).
    t.push(rel, 1, 1, 0, 0);
    t.commit();
    CHECK(t.kind(rel) == ChannelKind::Relative);
    // Players are separate.
    t.push(ax, -1, 1, 0, 2);
    t.commit();
    CHECK(t.value(ax, 2) == -1.0f);
    CHECK(t.value(ax, 0) == doctest::Approx(0.75f));
    CHECK(t.held(ax));
}

TEST_CASE("T-873: the existing events move the standard channels") {
    InputChannels t;
    InputEvent ev;
    ev.kind = InputEvent::GamepadAxisMotion;
    ev.gamepadAxis = GamepadAxis::RightX;
    ev.axisValue = 0.5f;
    t.applyEvent(ev);
    ev.gamepadAxis = GamepadAxis::TriggerRight;
    ev.axisValue = 0.8f;
    ev.player = 1;
    t.applyEvent(ev);
    InputEvent b;
    b.kind = InputEvent::GamepadButtonDown;
    b.gamepadButton = GamepadButton::X;
    t.applyEvent(b);
    InputEvent k;
    k.kind = InputEvent::KeyDown;
    k.key = KeyCode::ShiftRight;
    t.applyEvent(k);
    InputEvent w;
    w.kind = InputEvent::Wheel;
    w.wheelDelta = Vec2{0, 1.5f};
    t.applyEvent(w);
    t.applyEvent(w);
    InputEvent m;
    m.kind = InputEvent::PointerMove;
    m.pos = Vec2{10, 10};
    t.applyEvent(m);
    m.pos = Vec2{14, 7};
    t.applyEvent(m);
    m.kind = InputEvent::PointerDown;
    m.button = PointerButton::Right;
    m.pos = Vec2{15, 7};
    t.applyEvent(m);
    t.commit();
    CHECK(t.value(t.find("pad.rx")) == doctest::Approx(0.5f));
    CHECK(t.value(t.find("pad.rt"), 1) == doctest::Approx(0.8f));
    CHECK(t.value(t.find("pad.x")) == 1.0f);
    CHECK(t.pressed(t.find("pad.x")));
    CHECK(t.value(t.find("key.ShiftRight")) == 1.0f);
    CHECK(t.value(t.find("key.Shift")) == 1.0f);
    CHECK(t.value(t.find("wheel.y")) == doctest::Approx(3.0f));
    CHECK(t.value(t.find("pointer.dx")) == doctest::Approx(5.0f));
    CHECK(t.value(t.find("pointer.dy")) == doctest::Approx(-3.0f));
    CHECK(t.value(t.find("pointer.right")) == 1.0f);
}

// -- input() binding source ---------------------------------------------------------------------------

TEST_CASE("T-873: input(pad.rx) follows the right stick through GamepadAxisMotion; converters apply") {
    Loaded m = load(doc("#K { width: 10px; height: 10px; }",
                        "<div id=\"K\" bind:opacity=\"input(pad.rt)\"></div>"
                        "<div id=\"R\" bind:opacity=\"input(pad.rx) | range(-1, 1, 0, 1)\"></div>"
                        "<div id=\"W\" bind:opacity=\"input(wheel.y) | scale(0.25)\"></div>"));
    CHECK(m.opacity("R") == doctest::Approx(0.5f));
    CHECK(m.opacity("K") == doctest::Approx(0.0f));
    m.axis(GamepadAxis::RightX, 0.5f);
    m.axis(GamepadAxis::TriggerRight, 0.25f);
    m.step();
    CHECK(m.opacity("R") == doctest::Approx(0.75f));
    CHECK(m.opacity("K") == doctest::Approx(0.25f));
    // A relative channel reads the frame's sum, and 0 on the next frame.
    InputEvent w;
    w.kind = InputEvent::Wheel;
    w.wheelDelta = Vec2{0, 1};
    m.push(w);
    m.push(w);
    m.step();
    CHECK(m.opacity("W") == doctest::Approx(0.5f));
    m.step();
    CHECK(m.opacity("W") == doctest::Approx(0.0f));
}

TEST_CASE("T-873: bind:<prop>=input(<channel>) compiles; a malformed channel is an error") {
    CHECK(
        compileXt(doc("", "<div bind:translate-y=\"input(midi.cc(1)) | range(0, 1, 0, -120)\"></div>")).ok());
    CHECK(compileXt(doc("", "<div bind:opacity=\"input(player(1).host.wheel)\"></div>")).ok());
    const CompileResult bad = compileXt(doc("", "<div bind:opacity=\"input(pad.q)\"></div>"));
    CHECK_FALSE(bad.ok());
    CHECK(hasError(bad, "pad.q"));
}

// -- on:input triggers --------------------------------------------------------------------------------

TEST_CASE("T-873: on:input fires once a frame with the frame's value; .press / .release give the strength") {
    Loaded m =
        load(doc("",
                 "<div id=\"A\" port:v=\"number\" port:n=\"number\" port:s=\"number\" port:r=\"number\""
                 " on:input(host.knob)=\"set(v, event.value); add(n, 1)\""
                 " on:input(midi.note(36)).press=\"set(s, event.value)\""
                 " on:input(midi.note(36)).release=\"add(r, 1)\"></div>"));
    m.channel("host.knob", 2, 1, kChannelRelative);
    m.channel("host.knob", 3, 1, kChannelRelative);
    m.step();
    CHECK(m.port("v") == doctest::Approx(5.0f));
    CHECK(m.port("n") == doctest::Approx(1.0f));
    m.step();  // A relative channel at 0 does not fire.
    CHECK(m.port("n") == doctest::Approx(1.0f));
    m.channel("midi.note(36)", 1, 0.75f);
    m.step();
    CHECK(m.port("s") == doctest::Approx(0.75f));
    CHECK(m.port("r") == doctest::Approx(0.0f));
    m.channel("midi.note(36)", 0, 0.75f);
    m.step();
    CHECK(m.port("r") == doctest::Approx(1.0f));
}

TEST_CASE("T-873: player prefixes keep players apart") {
    Loaded m = load(doc("",
                        "<div id=\"A\" port:b=\"number\" bind:opacity=\"input(pad.lx) | range(-1, 1, 0, 1)\""
                        " on:input(player(1).pad.lx)=\"set(b, event.value)\"></div>"));
    m.axis(GamepadAxis::LeftX, -0.5f, 1);
    m.step();
    CHECK(m.opacity("A") == doctest::Approx(0.5f));
    CHECK(m.port("b") == doctest::Approx(-0.5f));
    m.axis(GamepadAxis::LeftX, 0.25f, 0);
    m.step();
    CHECK(m.opacity("A") == doctest::Approx(0.625f));
    CHECK(m.port("b") == doctest::Approx(-0.5f));
}

TEST_CASE("T-873: on:input(...) compile errors: unknown channel, .press on an axis") {
    CHECK(hasError(compileXt(doc("", "<div port:a=\"number\" on:input(pad.zz)=\"set(a, 1)\"></div>")),
                   "pad.zz"));
    CHECK(hasError(compileXt(doc("", "<div port:a=\"number\" on:input(pad.lx).press=\"set(a, 1)\"></div>")),
                   "not a button channel"));
    CHECK(compileXt(doc("", "<div port:a=\"number\" on:input(host.x).press=\"set(a, 1)\"></div>")).ok());
    CHECK(compileXt(doc("",
                        "<div port:a=\"number\" on:key(ShiftLeft)=\"set(a, 1)\""
                        " on:key(Slash).release=\"set(a, 2)\"></div>"))
              .ok());
}

// -- Context ----------------------------------------------------------------------------------------

TEST_CASE("T-873: a Context's channels reach every Document; needsFrame() follows held listened channels") {
    const CompileResult a = compileXt(doc("", "<div id=\"A\" bind:opacity=\"input(host.lever)\"></div>"));
    const CompileResult b =
        compileXt(doc("", "<div id=\"B\" bind:opacity=\"input(pad.ry) | range(-1, 1, 0, 1)\"></div>"));
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
    const DocumentHandle ha = ctx.loadDocument(IrBlob{a.ir.data(), a.ir.size()}, false).value();
    const DocumentHandle hb = ctx.loadDocument(IrBlob{b.ir.data(), b.ir.size()}, false).value();
    ctx.update(0.016f);
    const ChannelId lever = ctx.inputChannel("host.lever");
    REQUIRE(lever != kInvalidChannel);
    CHECK(ctx.inputChannel("pad.nope") == kInvalidChannel);
    InputEvent ev;
    ev.kind = InputEvent::Channel;
    ev.channel = lever;
    ev.channelValue = 0.6f;
    ctx.pushInput(ev);
    InputEvent stick;
    stick.kind = InputEvent::GamepadAxisMotion;
    stick.gamepadAxis = GamepadAxis::RightY;
    stick.axisValue = -0.4f;
    ctx.pushInput(stick);
    CHECK(ctx.needsFrame());
    ctx.update(0.016f);
    CHECK(ctx.channelValue(lever) == doctest::Approx(0.6f));
    CHECK(ctx.channelValue(ctx.inputChannel("pad.ry")) == doctest::Approx(-0.4f));
    Document& da = *ctx.document(ha);
    Document& db = *ctx.document(hb);
    CHECK(da.byId("A").property(prop::Opacity).f == doctest::Approx(0.6f));
    CHECK(db.byId("B").property(prop::Opacity).f == doctest::Approx(0.3f));
    // Held and listened: frames keep coming; back at rest: they stop.
    ctx.update(0.016f);
    CHECK(ctx.needsFrame());
    ev.channelValue = 0;
    ctx.pushInput(ev);
    stick.axisValue = 0;
    ctx.pushInput(stick);
    for (int i = 0; i < 4; ++i) {
        ctx.update(0.016f);
        CommandList list;
        ctx.render(list, nullptr);
    }
    CHECK_FALSE(ctx.needsFrame());
    // A Document loaded while a channel is held starts from it.
    ev.channelValue = 0.3f;
    ctx.pushInput(ev);
    ctx.update(0.016f);
    const DocumentHandle hc = ctx.loadDocument(IrBlob{a.ir.data(), a.ir.size()}, false).value();
    ctx.update(0.016f);
    Document& dc = *ctx.document(hc);
    CHECK(dc.byId("A").property(prop::Opacity).f == doctest::Approx(0.3f));
}

TEST_CASE("T-873: a Document loaded while a button is held gets no press; a held axis fires on:input once") {
    // 60 §60.11 (ruling on point 6): the press happened before the Document existed, so
    // `input(<button>).press` does not fire on its first frame (the binding still reads 1); an axis away
    // from rest fires `on:input(<axis>)` once.
    const CompileResult late =
        compileXt(doc("",
                      "<div id=\"A\" port:presses=\"number\" port:moves=\"number\" port:last=\"number\""
                      " bind:opacity=\"input(host.btn)\" on:input(host.btn).press=\"add(presses, 1)\""
                      " on:input(host.lever)=\"add(moves, 1); set(last, event.value)\"></div>"));
    REQUIRE(late.ok());
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
    InputEvent btn;
    btn.kind = InputEvent::Channel;
    btn.channel = ctx.inputChannel("host.btn");
    btn.channelValue = 1;
    btn.channelFlags = kChannelButton;
    ctx.pushInput(btn);
    InputEvent lever;
    lever.kind = InputEvent::Channel;
    lever.channel = ctx.inputChannel("host.lever");
    lever.channelValue = 0.4f;
    ctx.pushInput(lever);
    ctx.update(0.016f);
    const DocumentHandle h = ctx.loadDocument(IrBlob{late.ir.data(), late.ir.size()}, false).value();
    ctx.update(0.016f);
    ctx.update(0.016f);
    Document& d = *ctx.document(h);
    CHECK(d.byId("A").property(prop::Opacity).f == doctest::Approx(1.0f));
    CHECK(d.getPort(d.lookupPort("presses")).f == doctest::Approx(0.0f));
    CHECK(d.getPort(d.lookupPort("moves")).f == doctest::Approx(1.0f));
    CHECK(d.getPort(d.lookupPort("last")).f == doctest::Approx(0.4f));
    // A real press after the release still fires.
    btn.channelValue = 0;
    ctx.pushInput(btn);
    ctx.update(0.016f);
    btn.channelValue = 1;
    ctx.pushInput(btn);
    ctx.update(0.016f);
    CHECK(d.getPort(d.lookupPort("presses")).f == doctest::Approx(1.0f));
}
