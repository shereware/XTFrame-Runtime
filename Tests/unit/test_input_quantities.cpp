#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/validate.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/types.h"

/// T-871 (Docs/design/60-input-channels.md §60.3.2-§60.3.4, phase I1): `on:wheel`, the pointer
/// quantities of set() / add() (`pointer.delta`, `.fromPress`, `.angle`, `.turn`, `.distance`,
/// `.grab`), the `add()` action, `key(<k>).release`, and the event values of `gesture(pinch)` and
/// `scroll`. The last cases are the four acceptance UIs of the conversion report (relative drag
/// tuning, map panning, a sticker keeping its grab point, a turntable following the hand).

using namespace xt;
using namespace xt::compile;

namespace {

std::string doc(std::string_view style, std::string_view body, std::string_view bodyAttrs = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\" " +
           std::string(bodyAttrs) + ">" + std::string(body) + "</body></xt>";
}

/// `behavior` > 0 emits META with that behavior (pointermove coalescing from 1 on, T-407).
CompileResult compileXt(const std::string& xt, uint32_t behavior = 0) {
    CompileOptions options;
    options.emitMeta = behavior != 0;
    options.behavior = behavior;
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

    void update() { doc->update(clock); }
    void step(float dt) {
        clock += dt;
        doc->update(clock);
    }
    /// Dispatches without an update (the events of one frame).
    void push(const InputEvent& ev) { doc->dispatchInput(ev); }
    void send(const InputEvent& ev) {
        push(ev);
        step(0.016f);
    }
    void pointer(InputEvent::Kind kind, float x, float y, bool frame = true) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = Vec2{x, y};
        ev.button = PointerButton::Left;
        if (frame) {
            send(ev);
        } else {
            push(ev);
        }
    }
    void wheel(float x, float y, float dx, float dy, bool frame = true) {
        InputEvent ev;
        ev.kind = InputEvent::Wheel;
        ev.pos = Vec2{x, y};
        ev.wheelDelta = Vec2{dx, dy};
        if (frame) {
            send(ev);
        } else {
            push(ev);
        }
    }
    float port(std::string_view name) const {
        const PortId id = doc->lookupPort(name);
        REQUIRE(id != kInvalidPort);
        return doc->getPort(id).f;
    }
    void setPort(std::string_view name, float v) {
        const PortId id = doc->lookupPort(name);
        REQUIRE(id != kInvalidPort);
        REQUIRE(doc->setPort(id, Value::ofFloat(v)) == Status::kOk);
    }
};

Loaded load(const std::string& xt, uint32_t behavior = 0, float w = 300, float h = 300) {
    Loaded out;
    out.result = compileXt(xt, behavior);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    const Status status =
        Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr, out.doc);
    REQUIRE(status == Status::kOk);
    out.doc->setViewport(Rect{0, 0, w, h});
    out.doc->update(0.0f);
    return out;
}

}  // namespace

// -- on:wheel ------------------------------------------------------------------------------------

TEST_CASE("T-871: on:wheel sums a frame's notches, runs once a frame, gives event.dx / event.dy") {
    for (const uint32_t behavior : {0u, 1u}) {
        CAPTURE(behavior);
        Loaded m = load(doc("#K { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }",
                            "<div id=\"K\" port:w=\"number\" port:dx=\"number\" port:dy=\"number\""
                            " port:n=\"number\" on:wheel=\"set(w, event.value); set(dx, event.dx);"
                            " set(dy, event.dy); add(n, 1)\"></div>"),
                        behavior);
        m.wheel(50, 50, 0.0f, 1.0f, false);
        m.wheel(50, 50, 0.5f, 2.0f, false);
        m.wheel(50, 50, 0.0f, -0.5f, false);
        m.step(0.016f);
        CHECK(m.port("w") == doctest::Approx(2.5f));
        CHECK(m.port("dx") == doctest::Approx(0.5f));
        CHECK(m.port("dy") == doctest::Approx(2.5f));
        CHECK(m.port("n") == doctest::Approx(1.0f));
        // A wheel outside the element does not reach it.
        m.wheel(250, 250, 0.0f, 3.0f);
        CHECK(m.port("n") == doctest::Approx(1.0f));
        // The next frame is a run of its own.
        m.wheel(10, 10, 0.0f, -1.0f);
        CHECK(m.port("w") == doctest::Approx(-1.0f));
        CHECK(m.port("n") == doctest::Approx(2.0f));
    }
}

TEST_CASE("T-871: on:wheel bubbles; the scroll container still scrolls; on:scroll event.value") {
    Loaded m =
        load(doc("#S { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px;"
                 " overflow: auto; } #C { width: 100px; height: 400px; }",
                 "<div id=\"S\" port:sv=\"number\" port:sw=\"number\" on:scroll=\"set(sv, event.value)\""
                 " on:wheel=\"add(sw, event.value)\">"
                 "<div id=\"C\" port:cw=\"number\" on:wheel=\"add(cw, event.value)\"></div></div>"));
    m.wheel(50, 50, 0.0f, 1.0f);
    CHECK(m.port("cw") == doctest::Approx(1.0f));
    CHECK(m.port("sw") == doctest::Approx(1.0f));  // Bubbled to the container.
    // The default action is unchanged: 40 px a notch, of 300 px scrollable.
    CHECK(m.doc->byId("S").scrollPos().y == doctest::Approx(40.0f));
    CHECK(m.port("sv") == doctest::Approx(40.0f / 300.0f));
    m.wheel(50, 50, 0.0f, 20.0f);
    CHECK(m.port("sv") == doctest::Approx(1.0f));
}

TEST_CASE("T-871: an on:wheel starting with consume() keeps the wheel from the scroll container") {
    const char* style =
        "#S { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; overflow: auto; }"
        " #C { width: 100px; height: 400px; } #K { width: 50px; height: 50px; }";
    for (const bool consume : {true, false}) {
        CAPTURE(consume);
        const std::string actions = consume ? "consume(); add(kw, event.value)" : "add(kw, event.value)";
        Loaded m = load(doc(style,
                            "<div id=\"S\" port:sw=\"number\" on:wheel=\"add(sw, 1)\"><div id=\"C\">"
                            "<div id=\"K\" port:kw=\"number\" on:wheel=\"" +
                                actions + "\"></div></div></div>"));
        m.wheel(20, 20, 0.0f, 1.0f);                   // Over #K.
        CHECK(m.port("kw") == doctest::Approx(1.0f));  // The trigger runs either way.
        CHECK(m.port("sw") == doctest::Approx(1.0f));  // The event still bubbles.
        CHECK(m.doc->byId("S").scrollPos().y == doctest::Approx(consume ? 0.0f : 40.0f));
        m.wheel(20, 80, 0.0f, 1.0f);  // Outside #K the container scrolls.
        CHECK(m.doc->byId("S").scrollPos().y == doctest::Approx(consume ? 40.0f : 80.0f));
    }
}

// -- pointer quantities --------------------------------------------------------------------------

TEST_CASE("T-871: pointer.delta sums the moves a frame coalesces; none is lost") {
    for (const uint32_t behavior : {0u, 1u}) {
        CAPTURE(behavior);
        Loaded m =
            load(doc("#P { position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; }",
                     "<div id=\"P\" port:px=\"number\" port:py=\"number\" port:runs=\"number\""
                     " on:pointermove=\"add(px, pointer.delta.x); add(py, pointer.delta.y); add(runs, 1)\">"
                     "</div>"),
                 behavior);
        m.pointer(InputEvent::PointerMove, 10, 10);  // The first pointer input: no movement yet.
        CHECK(m.port("px") == doctest::Approx(0.0f));
        CHECK(m.port("runs") == doctest::Approx(1.0f));
        for (int i = 1; i <= 5; ++i) {
            m.pointer(InputEvent::PointerMove, 10.0f + 3.0f * static_cast<float>(i),
                      10.0f + static_cast<float>(i), false);
        }
        m.step(0.016f);
        CHECK(m.port("px") == doctest::Approx(15.0f));
        CHECK(m.port("py") == doctest::Approx(5.0f));
        CHECK(m.port("runs") == doctest::Approx(behavior >= 1 ? 2.0f : 6.0f));
    }
}

TEST_CASE("T-871: pointer.delta(#id) follows a rotated element's axes") {
    Loaded m =
        load(doc("#R { position: absolute; left: 50px; top: 50px; width: 100px; height: 40px;"
                 " transform: rotate(90deg); }",
                 "<div id=\"R\" port:lx=\"number\" port:ly=\"number\""
                 " on:pointermove=\"add(lx, pointer.delta(#R).x); add(ly, pointer.delta(#R).y)\"></div>"));
    const NodeId r = m.doc->byId("R").node;
    m.pointer(InputEvent::PointerMove, 100, 50);
    const Vec2 a = m.doc->localPosIn(r, Vec2{100, 50});
    const Vec2 b = m.doc->localPosIn(r, Vec2{100, 60});
    m.pointer(InputEvent::PointerMove, 100, 60);  // Screen down = the element's +x.
    CHECK(m.port("lx") == doctest::Approx(b.x - a.x));
    CHECK(m.port("ly") == doctest::Approx(b.y - a.y));
    CHECK(m.port("lx") == doctest::Approx(10.0f));
    CHECK(m.port("ly") == doctest::Approx(0.0f).epsilon(0.001));
    m.pointer(InputEvent::PointerMove, 110, 60);  // Screen right = the element's -y.
    CHECK(m.port("lx") == doctest::Approx(10.0f));
    CHECK(m.port("ly") == doctest::Approx(-10.0f));
}

TEST_CASE("T-871: pointer.turn accumulates across +-180 (two turns = 720); angle and distance") {
    Loaded m = load(doc("#K { position: absolute; left: 50px; top: 50px; width: 100px; height: 100px; }",
                        "<div id=\"K\" port:t=\"number\" port:a=\"number\" port:d=\"number\""
                        " on:pointermove=\"set(t, pointer.turn(#K)); set(a, pointer.angle(#K));"
                        " set(d, pointer.distance(#K))\"></div>"));
    const float kPi = 3.14159265358979f;
    m.pointer(InputEvent::PointerDown, 140, 100);
    for (int i = 1; i <= 64; ++i) {  // Two clockwise turns, radius 40, around (100, 100).
        const float th = 2.0f * kPi * static_cast<float>(i) / 32.0f;
        m.pointer(InputEvent::PointerMove, 100.0f + 40.0f * std::cos(th), 100.0f + 40.0f * std::sin(th));
    }
    CHECK(m.port("t") == doctest::Approx(720.0f).epsilon(0.001));
    CHECK(std::fabs(m.port("a")) < 0.01f);
    CHECK(m.port("d") == doctest::Approx(40.0f));
    m.pointer(InputEvent::PointerMove, 100, 140);  // A quarter more: straight down = +90.
    CHECK(m.port("t") == doctest::Approx(810.0f).epsilon(0.001));
    CHECK(m.port("a") == doctest::Approx(90.0f));
    m.pointer(InputEvent::PointerMove, 60, 100);  // Past 180 the angle reads -180..180 ...
    CHECK(m.port("a") == doctest::Approx(180.0f));
    m.pointer(InputEvent::PointerMove, 100, 60);
    CHECK(m.port("a") == doctest::Approx(-90.0f));
    CHECK(m.port("t") == doctest::Approx(990.0f).epsilon(0.001));  // ... the turn does not jump.
    m.pointer(InputEvent::PointerMove, 60, 100);                   // Back counter-clockwise.
    CHECK(m.port("t") == doctest::Approx(900.0f).epsilon(0.001));
    // A new press starts from 0.
    m.pointer(InputEvent::PointerUp, 60, 100);
    m.pointer(InputEvent::PointerDown, 140, 100);
    m.pointer(InputEvent::PointerMove, 100, 140);
    CHECK(m.port("t") == doctest::Approx(90.0f).epsilon(0.001));
}

TEST_CASE("T-871: pointer.fromPress and pointer.grab(#id), taken at the press") {
    Loaded m =
        load(doc("#G { position: absolute; left: 20px; top: 20px; width: 100px; height: 50px; }",
                 "<div id=\"G\" port:fx=\"number\" port:fy=\"number\" port:gx=\"number\" port:gy=\"number\""
                 " port:lx=\"number\" on:pointermove=\"set(fx, pointer.fromPress.x);"
                 " set(fy, pointer.fromPress.y); set(gx, pointer.grab(#G).x); set(gy, pointer.grab(#G).y);"
                 " set(lx, pointer.fromPress(#G).x)\"></div>"));
    m.pointer(InputEvent::PointerDown, 45, 30);
    m.pointer(InputEvent::PointerMove, 60, 40);
    CHECK(m.port("fx") == doctest::Approx(15.0f));
    CHECK(m.port("fy") == doctest::Approx(10.0f));
    CHECK(m.port("lx") == doctest::Approx(15.0f));
    CHECK(m.port("gx") == doctest::Approx(0.25f));
    CHECK(m.port("gy") == doctest::Approx(0.2f));
    m.pointer(InputEvent::PointerMove, 100, 60);
    CHECK(m.port("fx") == doctest::Approx(55.0f));
    CHECK(m.port("gx") == doctest::Approx(0.25f));  // Unchanged while the pointer moves.
}

TEST_CASE("T-871: at uiscale 1 and 1.5 the lengths are pointer.local()'s units") {
    for (const float scale : {1.0f, 1.5f}) {
        CAPTURE(scale);
        Loaded m = load(doc("#U { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }",
                            "<div id=\"U\" port:dx=\"number\" port:lx=\"number\" port:l=\"number\""
                            " port:fx=\"number\" on:pointermove=\"add(dx, pointer.delta.x);"
                            " add(lx, pointer.delta(#U).x); set(l, pointer.local(#U).x);"
                            " set(fx, pointer.fromPress(#U).x)\"></div>"));
        m.doc->setUiScale(scale);
        m.step(0.016f);
        m.pointer(InputEvent::PointerMove, 10, 10);
        m.pointer(InputEvent::PointerDown, 10, 10);
        const float before = m.port("l");
        m.setPort("dx", 0.0f);
        m.setPort("lx", 0.0f);
        m.pointer(InputEvent::PointerMove, 40, 10);
        CHECK(m.port("dx") == doctest::Approx(30.0f));
        CHECK(m.port("lx") == doctest::Approx(m.port("l") - before));
        CHECK(m.port("lx") == doctest::Approx(30.0f));
        CHECK(m.port("fx") == doctest::Approx(30.0f));
    }
}

TEST_CASE("T-871: a snapshot keeps the press point, the previous point and the turn") {
    const std::string xt =
        doc("#K { position: absolute; left: 50px; top: 50px; width: 100px; height: 100px; }",
            "<div id=\"K\" port:t=\"number\" port:fx=\"number\" port:dx=\"number\""
            " on:pointermove=\"set(t, pointer.turn(#K)); set(fx, pointer.fromPress.x);"
            " add(dx, pointer.delta.x)\"></div>");
    Loaded a = load(xt);
    a.pointer(InputEvent::PointerDown, 140, 100);
    a.pointer(InputEvent::PointerMove, 128, 128);
    a.pointer(InputEvent::PointerMove, 100, 140);
    CHECK(a.port("t") == doctest::Approx(90.0f).epsilon(0.001));
    CHECK(a.port("dx") == doctest::Approx(-40.0f));
    const std::string snap = a.doc->snapshotState();

    Loaded b = load(xt);
    REQUIRE(b.doc->restoreState(snap) == Status::kOk);
    b.pointer(InputEvent::PointerMove, 72, 128);
    b.pointer(InputEvent::PointerMove, 60, 100);
    CHECK(b.port("t") == doctest::Approx(180.0f).epsilon(0.001));
    CHECK(b.port("fx") == doctest::Approx(-80.0f));  // From the press at x 140, not the restored 100.
    CHECK(b.port("dx") == doctest::Approx(-80.0f));  // -40 restored, then -40 from (100, 140).
}

// -- add() ---------------------------------------------------------------------------------------

TEST_CASE("T-871: add() clamps, wraps, takes converters; a non-number port is an error") {
    Loaded m =
        load(doc("#B { position: absolute; left: 0px; top: 0px; width: 50px; height: 50px; }",
                 "<div id=\"B\" port:c=\"number\" port:w=\"number\" port:n=\"number\" port:k=\"number\""
                 " port:o=\"number out\" on:click=\"add(c, 30, 0, 100); add(w, 30, 0, 100, wrap);"
                 " add(n, -30, 0, 100, wrap); add(k, 2 | scale(5)); add(o, 1)\"></div>"));
    for (int i = 0; i < 4; ++i) {
        m.pointer(InputEvent::PointerDown, 10, 10);
        m.pointer(InputEvent::PointerUp, 10, 10);
    }
    CHECK(m.port("c") == doctest::Approx(100.0f));
    CHECK(m.port("w") == doctest::Approx(20.0f));
    CHECK(m.port("n") == doctest::Approx(80.0f));  // 70, 40, 10, 80.
    CHECK(m.port("k") == doctest::Approx(40.0f));
    CHECK(m.port("o") == doctest::Approx(4.0f));
    // Clamped below too.
    m.setPort("c", -5.0f);
    m.pointer(InputEvent::PointerDown, 10, 10);
    m.pointer(InputEvent::PointerUp, 10, 10);
    CHECK(m.port("c") == doctest::Approx(25.0f));

    CompileResult text = compileXt(doc("", "<div id=\"B\" port:s=\"string\" on:click=\"add(s, 1)\"></div>"));
    CHECK_FALSE(text.ok());
    CHECK(hasError(text, "add() needs a number port"));
    CompileResult flag = compileXt(doc("", "<div id=\"B\" port:s=\"bool\" on:click=\"add(s, 1)\"></div>"));
    CHECK_FALSE(flag.ok());
    CHECK(hasError(flag, "add() needs a number port"));
    CompileResult str = compileXt(doc("", "<div id=\"B\" port:s=\"number\" on:click=\"add(s, 'x')\"></div>"));
    CHECK_FALSE(str.ok());
}

TEST_CASE("T-871: the wheel tunes a value with add(), the design's example") {
    Loaded m = load(doc("#K { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }",
                        "<div id=\"K\" port:freq=\"number\""
                        " on:wheel=\"add(freq, event.value | scale(-0.1), 87.5, 108)\"></div>"));
    m.setPort("freq", 100.0f);
    m.wheel(50, 50, 0.0f, -3.0f);
    CHECK(m.port("freq") == doctest::Approx(100.3f));
    m.wheel(50, 50, 0.0f, 500.0f);
    CHECK(m.port("freq") == doctest::Approx(87.5f));
}

// -- key().release, gesture(pinch), errors -------------------------------------------------------

TEST_CASE("T-871: key(<k>).release fires on the key-up; key(any).release") {
    Loaded m = load(doc("#B { position: absolute; left: 0px; top: 0px; width: 50px; height: 50px; }",
                        "<button id=\"B\" port:p=\"number\" port:r=\"number\" port:any=\"number\""
                        " on:key(Enter)=\"add(p, 1)\" on:key(Enter).release=\"add(r, 1)\""
                        " on:key(any).release=\"add(any, 1)\"></button>"));
    m.doc->setFocus(m.doc->byId("B"));
    InputEvent ev;
    ev.kind = InputEvent::KeyDown;
    ev.key = KeyCode::Enter;
    m.send(ev);
    CHECK(m.port("p") == doctest::Approx(1.0f));
    CHECK(m.port("r") == doctest::Approx(0.0f));
    ev.kind = InputEvent::KeyUp;
    m.send(ev);
    CHECK(m.port("r") == doctest::Approx(1.0f));
    CHECK(m.port("any") == doctest::Approx(1.0f));
    ev.key = KeyCode::Escape;
    m.send(ev);
    CHECK(m.port("r") == doctest::Approx(1.0f));
    CHECK(m.port("any") == doctest::Approx(2.0f));
}

TEST_CASE("T-871: gesture(pinch) carries the scale as event.value") {
    Loaded m = load(doc("#K { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }",
                        "<div id=\"K\" port:s=\"number\" on:gesture(pinch)=\"set(s, event.value)\"></div>"));
    InputEvent ev;
    ev.kind = InputEvent::Gesture;
    ev.gesture = GestureKind::Pinch;
    ev.gestureScale = 1.5f;
    ev.pos = Vec2{50, 50};
    m.send(ev);
    CHECK(m.port("s") == doctest::Approx(1.5f));
}

TEST_CASE("T-871: compile errors; the validator and the event table know the new items") {
    CompileResult show =
        compileXt(doc("", "<div id=\"T\" port:v=\"number\" on:show=\"set(v, pointer.delta.x)\"></div>"));
    CHECK_FALSE(show.ok());
    CHECK(hasError(show, "only valid in pointer triggers"));
    CompileResult wheelPointer =
        compileXt(doc("", "<div id=\"T\" port:v=\"number\" on:wheel=\"add(v, pointer.turn(#T))\"></div>"));
    CHECK_FALSE(wheelPointer.ok());
    CompileResult dx =
        compileXt(doc("", "<div id=\"T\" port:v=\"number\" on:click=\"set(v, event.dx)\"></div>"));
    CHECK_FALSE(dx.ok());
    CHECK(hasError(dx, "only valid in wheel triggers"));
    CompileResult noId =
        compileXt(doc("", "<div id=\"T\" port:v=\"number\" on:click=\"set(v, pointer.turn)\"></div>"));
    CHECK_FALSE(noId.ok());
    CompileResult badId = compileXt(
        doc("", "<div id=\"T\" port:v=\"number\" on:click=\"set(v, pointer.grab(#Nope).x)\"></div>"));
    CHECK_FALSE(badId.ok());
    CHECK(hasError(badId, "#Nope"));
    // consume() in wheel only as the unconditional head of the first statement (60 §60.3.2).
    CHECK(
        compileXt(doc("", "<div id=\"T\" port:v=\"number\" on:wheel=\"consume(); add(v, 1)\"></div>")).ok());
    for (const char* bad : {"add(v, 1); consume()", "if gt(0) consume()", "add(v, 1) then consume()"}) {
        CAPTURE(bad);
        CompileResult r = compileXt(
            doc("", "<div id=\"T\" port:v=\"number\" on:wheel=\"" + std::string(bad) + "\"></div>"));
        CHECK_FALSE(r.ok());
        CHECK(hasError(r, "consume() must come first"));
    }
    CompileResult press = compileXt(doc("", "<div id=\"T\" on:press=\"consume()\"></div>"));
    CHECK_FALSE(press.ok());
    CHECK(hasError(press, "consume() is only valid in outsidepress"));

    std::string err;
    CHECK(validateTriggerEvent("wheel", err));
    CHECK(validateTriggerEvent("key(Enter).release", err));
    CHECK(validateTriggerEvent("key(any).release", err));
    CHECK_FALSE(validateTriggerEvent("key(Enter).hold", err));
    CHECK(validateTriggerActions("add(v, pointer.delta.x, 0, 10, wrap)", err));
    CHECK(validateTriggerActions("add(v, event.value | scale(-0.1), 87.5, 108)", err));
    CHECK(validateTriggerActions("set(v, pointer.turn(#K)); set(w, pointer.grab(#K).y)", err));
    CHECK(
        validateTriggerActions("set(v, pointer.fromPress(#K).x); set(v, pointer.angle(#K) | scale(2))", err));
    CHECK(validateTriggerActions("set(v, pointer.distance(#K)); set(v, event.dy)", err));
    CHECK_FALSE(validateTriggerActions("add(v)", err));
    CHECK_FALSE(validateTriggerActions("add(v, 1, 0)", err));
    CHECK_FALSE(validateTriggerActions("add(v, 1, 0, 10, loop)", err));
    CHECK_FALSE(validateTriggerActions("add(v, 1, a, 10)", err));
    CHECK_FALSE(validateTriggerActions("set(v, pointer.turn(#K).x)", err));
    CHECK_FALSE(validateTriggerActions("set(v, pointer.delta.z)", err));
    bool wheel = false;
    bool release = false;
    for (const TriggerEventInfo& e : triggerEventTable()) {
        wheel = wheel || std::string_view(e.pattern) == "wheel";
        release = release || std::string_view(e.pattern) == "key({0}).release";
    }
    CHECK(wheel);
    CHECK(release);
    bool add = false;
    for (const TriggerActionInfo& a : triggerActionTable()) {
        add = add || std::string_view(a.name) == "add";
    }
    CHECK(add);
}

// -- The conversion report's four scenes (REPORT part 2 §3) --------------------------------------

TEST_CASE("T-871 acceptance: relative drag tuning (pointer.delta + add)") {
    // A tuning strip: dragging right by 10 px raises the frequency by 1 MHz, wherever the drag
    // starts; the value stays in 87.5..108.
    Loaded m = load(doc("#Dial { position: absolute; left: 0px; top: 100px; width: 300px; height: 40px; }",
                        "<div id=\"Dial\" port:freq=\"number\""
                        " on:pointerdrag=\"add(freq, pointer.delta.x | scale(0.1), 87.5, 108)\"></div>"),
                    1);
    m.setPort("freq", 100.0f);
    m.pointer(InputEvent::PointerDown, 200, 120);
    m.pointer(InputEvent::PointerMove, 210, 120);  // Past the 4 px threshold: the drag starts.
    const float started = m.port("freq");
    m.pointer(InputEvent::PointerMove, 230, 121, false);
    m.pointer(InputEvent::PointerMove, 250, 119, false);
    m.step(0.016f);
    CHECK(m.port("freq") == doctest::Approx(started + 4.0f));
    m.pointer(InputEvent::PointerUp, 250, 119);
    // A second drag starting elsewhere continues from the value (relative, no jump).
    const float before = m.port("freq");
    m.pointer(InputEvent::PointerDown, 20, 120);
    m.pointer(InputEvent::PointerMove, 10, 120);
    m.pointer(InputEvent::PointerMove, 0, 120);
    CHECK(m.port("freq") < before);
    CHECK(m.port("freq") > before - 2.1f);
    m.pointer(InputEvent::PointerUp, 0, 120);
}

TEST_CASE("T-871 acceptance: map panning (pointer.delta on both axes)") {
    Loaded m = load(
        doc("#View { position: absolute; left: 0px; top: 0px; width: 200px; height: 200px;"
            " overflow: hidden; } #Map { position: absolute; width: 600px; height: 600px; }",
            "<div id=\"View\" port:panx=\"number\" port:pany=\"number\""
            " on:pointerdrag=\"add(panx, pointer.delta.x, -400, 0); add(pany, pointer.delta.y, -400, 0)\">"
            "<div id=\"Map\" bind:left=\"panx\" bind:top=\"pany\"></div></div>"),
        1);
    m.pointer(InputEvent::PointerDown, 150, 150);
    m.pointer(InputEvent::PointerMove, 140, 150);
    m.pointer(InputEvent::PointerMove, 100, 120);
    m.pointer(InputEvent::PointerUp, 100, 120);
    // The map follows the hand from the threshold-crossing move on (that move's own -10 included).
    CHECK(m.port("panx") == doctest::Approx(-50.0f));
    CHECK(m.port("pany") == doctest::Approx(-30.0f));
    m.step(0.016f);
    const Rect box = m.doc->byId("Map").boundingBox();
    CHECK(box.x == doctest::Approx(-50.0f));
    CHECK(box.y == doctest::Approx(-30.0f));
    // Clamped at the map's edge.
    m.pointer(InputEvent::PointerDown, 150, 150);
    m.pointer(InputEvent::PointerMove, 160, 150);
    m.pointer(InputEvent::PointerMove, 299, 299);
    CHECK(m.port("panx") == doctest::Approx(0.0f));
    CHECK(m.port("pany") == doctest::Approx(0.0f));
}

TEST_CASE("T-871 acceptance: a sticker keeps its grab point (pointer.grab)") {
    // The sticker's top-left goes to the pointer minus the grab offset, so it does not jump to put
    // its corner under the hand.
    Loaded m = load(
        doc("#Board { position: absolute; left: 0px; top: 0px; width: 300px; height: 300px; }"
            " #St { position: absolute; width: 80px; height: 40px; }",
            "<div id=\"Board\" port:x=\"number\" port:y=\"number\" port:gx=\"number\" port:gy=\"number\">"
            "<div id=\"St\" bind:left=\"x\" bind:top=\"y\""
            " on:press=\"set(gx, pointer.grab(#St).x); set(gy, pointer.grab(#St).y)\""
            " on:pointermove=\"if active add(x, pointer.delta.x); if active add(y, pointer.delta.y)\">"
            "</div></div>"));
    m.setPort("x", 50.0f);
    m.setPort("y", 50.0f);
    m.step(0.016f);
    m.pointer(InputEvent::PointerDown, 70, 60);  // 20 px in, 10 px down.
    CHECK(m.port("gx") == doctest::Approx(0.25f));
    CHECK(m.port("gy") == doctest::Approx(0.25f));
    m.pointer(InputEvent::PointerMove, 150, 120);
    m.pointer(InputEvent::PointerMove, 170, 200);
    m.pointer(InputEvent::PointerUp, 170, 200);
    m.step(0.016f);
    const Rect box = m.doc->byId("St").boundingBox();
    // The grab point is still under the pointer.
    CHECK(box.x + 0.25f * box.w == doctest::Approx(170.0f));
    CHECK(box.y + 0.25f * box.h == doctest::Approx(200.0f));
}

TEST_CASE("T-871 acceptance: a turntable follows the hand (pointer.turn)") {
    Loaded m = load(doc("#Deck { position: absolute; left: 50px; top: 50px; width: 200px; height: 200px; }"
                        " #Platter { position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; }",
                        "<div id=\"Deck\" port:angle=\"number\" port:base=\"number\""
                        " on:press=\"set(base, port(angle))\""
                        " on:pointerdrag=\"set(angle, pointer.turn(#Platter) | offset(base))\">"
                        "<div id=\"Platter\" bind:rotate=\"angle\"></div></div>"),
                    1);
    const float kPi = 3.14159265358979f;
    m.pointer(InputEvent::PointerDown, 230, 150);  // Right of the centre (150, 150).
    for (int i = 1; i <= 24; ++i) {                // One and a half clockwise turns.
        const float th = 2.0f * kPi * static_cast<float>(i) / 16.0f;
        m.pointer(InputEvent::PointerMove, 150.0f + 80.0f * std::cos(th), 150.0f + 80.0f * std::sin(th));
    }
    m.pointer(InputEvent::PointerUp, 70, 150);
    CHECK(m.port("angle") == doctest::Approx(540.0f).epsilon(0.001));
    // The second grab continues from where the platter stopped.
    m.pointer(InputEvent::PointerDown, 70, 150);
    m.pointer(InputEvent::PointerMove, 150, 70);  // A quarter turn clockwise (left -> top).
    CHECK(m.port("angle") == doctest::Approx(630.0f).epsilon(0.001));
}
