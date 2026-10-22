#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "event_dispatcher.h"  // navScore (Code/Runtime/core/src, on the test include path).
#include "mini_css.h"          // T-389: parseDeclarationsCount() (Code/Runtime/core/src).
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/validate.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/gesture.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/types.h"
#include "xt/version.h"

/// Game UI interaction (Docs/design/08-events.md §8.9, 27-interaction.md §27.7, Docs/tasks/
/// T-194.md): focus navigation (geometric and explicit), local players, gamepad mapping, drag and
/// drop, the overlay layer, surface stacking, focus traps, gestures, haptics and announcements.

using namespace xt;
using namespace xt::compile;

namespace {

std::string doc(std::string_view style, std::string_view body, std::string_view bodyAttrs = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\" " +
           std::string(bodyAttrs) + ">" + std::string(body) + "</body></xt>";
}

/// T-407: `behavior` > 0 emits META with that `behavior` (0 = no META, the pre-beta behavior).
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

    void step(float dt) {
        clock += dt;
        doc->update(clock);
    }
    void send(const InputEvent& ev) {
        doc->dispatchInput(ev);
        doc->update(clock);
    }
    void pointer(InputEvent::Kind kind, float x, float y) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = Vec2{x, y};
        send(ev);
    }
    void key(KeyCode code, uint8_t player = 0) {
        InputEvent ev;
        ev.kind = InputEvent::KeyDown;
        ev.key = code;
        ev.player = player;
        send(ev);
        ev.kind = InputEvent::KeyUp;
        send(ev);
    }
    void padDown(GamepadButton b, uint8_t player = 0) {
        InputEvent ev;
        ev.kind = InputEvent::GamepadButtonDown;
        ev.gamepadButton = b;
        ev.player = player;
        send(ev);
    }
    void padUp(GamepadButton b, uint8_t player = 0) {
        InputEvent ev;
        ev.kind = InputEvent::GamepadButtonUp;
        ev.gamepadButton = b;
        ev.player = player;
        send(ev);
    }
    void pad(GamepadButton b, uint8_t player = 0) {
        padDown(b, player);
        padUp(b, player);
    }
    ElementRef el(std::string_view id) const { return doc->byId(id); }
    bool has(std::string_view id, std::string_view state) const { return doc->hasCustomState(el(id), state); }
    std::string focusId(uint8_t player = 0) const {
        ElementRef f = doc->focus(player);
        if (!f.valid()) {
            return "";
        }
        const StringId id = doc->arena().element(f.node).id;
        return id != kInvalidString ? std::string(doc->string(id)) : std::string();
    }
};

Loaded load(const std::string& xt, float w = 300, float h = 200, uint32_t behavior = 0) {
    Loaded out;
    out.result = compileXt(xt, behavior);
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

/// A 3 x 3 grid of 40 px buttons, 50 px apart (A1 top left ... C3 bottom right).
constexpr const char* kGridStyle = R"X(
  button { position: absolute; width: 40px; height: 40px; }
  #A1 { left: 0px; top: 0px; } #A2 { left: 50px; top: 0px; } #A3 { left: 100px; top: 0px; }
  #B1 { left: 0px; top: 50px; } #B2 { left: 50px; top: 50px; } #B3 { left: 100px; top: 50px; }
  #C1 { left: 0px; top: 100px; } #C2 { left: 50px; top: 100px; } #C3 { left: 100px; top: 100px; }
)X";

std::string gridBody(std::string_view a1Attrs = "") {
    return "<button id=\"A1\" " + std::string(a1Attrs) +
           "></button><button id=\"A2\"></button><button id=\"A3\"></button>"
           "<button id=\"B1\"></button><button id=\"B2\"></button><button id=\"B3\"></button>"
           "<button id=\"C1\"></button><button id=\"C2\"></button><button id=\"C3\"></button>";
}

Color bg(const Loaded& l, std::string_view id) {
    return l.el(id).computedStyleValue(prop::BackgroundColor).c;
}

}  // namespace

// -- Geometric scoring (focus_nav.cpp) ------------------------------------------------------------

TEST_CASE("interaction: navScore accepts the cone ahead in each direction and prefers alignment") {
    const Rect from{100, 100, 40, 40};
    bool ok = false;
    // Straight ahead, 10 px gap, in each direction.
    CHECK(navScore(from, Rect{150, 100, 40, 40}, NavDirection::Right, ok) == doctest::Approx(10));
    CHECK(ok);
    CHECK(navScore(from, Rect{50, 100, 40, 40}, NavDirection::Left, ok) == doctest::Approx(10));
    CHECK(ok);
    CHECK(navScore(from, Rect{100, 150, 40, 40}, NavDirection::Down, ok) == doctest::Approx(10));
    CHECK(ok);
    CHECK(navScore(from, Rect{100, 50, 40, 40}, NavDirection::Up, ok) == doctest::Approx(10));
    CHECK(ok);
    // Behind is never eligible.
    navScore(from, Rect{50, 100, 40, 40}, NavDirection::Right, ok);
    CHECK_FALSE(ok);
    navScore(from, Rect{100, 150, 40, 40}, NavDirection::Up, ok);
    CHECK_FALSE(ok);
    // Far outside the cone (perpendicular offset > 2 x the forward offset, no overlap).
    navScore(from, Rect{150, 300, 40, 40}, NavDirection::Right, ok);
    CHECK_FALSE(ok);
    // An aligned candidate beats a nearer diagonal one.
    const float aligned = navScore(from, Rect{200, 100, 40, 40}, NavDirection::Right, ok);
    CHECK(ok);
    const float diagonal = navScore(from, Rect{150, 160, 40, 40}, NavDirection::Right, ok);
    CHECK(ok);
    CHECK(aligned < diagonal);
}

// -- Keyboard / gamepad navigation ----------------------------------------------------------------

TEST_CASE("interaction: arrow keys navigate a grid geometrically and report nav.blocked") {
    Loaded l = load(doc(kGridStyle, gridBody(), "on:nav.blocked(up)=\"state(BlockedUp)\""));
    l.key(KeyCode::Right);  // Nothing focused: the first focusable element.
    CHECK(l.focusId() == "A1");
    l.key(KeyCode::Right);
    CHECK(l.focusId() == "A2");
    l.key(KeyCode::Down);
    CHECK(l.focusId() == "B2");
    l.key(KeyCode::Down);
    CHECK(l.focusId() == "C2");
    l.key(KeyCode::Left);
    CHECK(l.focusId() == "C1");
    l.key(KeyCode::Up);
    CHECK(l.focusId() == "B1");
    l.key(KeyCode::Up);
    CHECK(l.focusId() == "A1");
    CHECK_FALSE(l.has("Root", "BlockedUp"));
    l.key(KeyCode::Up);  // Nothing above: blocked, bubbles to the body.
    CHECK(l.focusId() == "A1");
    CHECK(l.has("Root", "BlockedUp"));
}

TEST_CASE("interaction: arrow keys inside a text input move its caret, not the focus") {
    Loaded l = load(doc(kGridStyle, gridBody() + "<input id=\"T\" style=\"position:absolute; left:0px; "
                                                 "top:150px; width:80px; height:20px\"/>"));
    l.doc->setFocus(l.el("T"));
    l.key(KeyCode::Up);
    CHECK(l.focusId() == "T");
}

TEST_CASE("interaction: nav:<dir> targets, focus-nav: explicit and focus-nav: none") {
    Loaded l = load(doc(std::string(kGridStyle) + "#B2 { focus-nav: explicit; } #A2 { focus-nav: none; }",
                        gridBody("nav:right=\"#C3\"")));
    l.doc->setFocus(l.el("A1"));
    l.key(KeyCode::Right);  // nav:right wins over geometry.
    CHECK(l.focusId() == "C3");
    l.doc->setFocus(l.el("B1"));
    l.key(KeyCode::Up);  // A1 (A2 is `none` but not in that direction anyway).
    CHECK(l.focusId() == "A1");
    l.doc->setFocus(l.el("B2"));
    l.key(KeyCode::Up);  // `explicit` without nav:up: blocked.
    CHECK(l.focusId() == "B2");
    l.doc->setFocus(l.el("C2"));
    l.key(KeyCode::Up);  // B2 is a candidate...
    CHECK(l.focusId() == "B2");
    l.doc->setFocus(l.el("A3"));
    l.key(KeyCode::Left);  // ...but A2 (`none`) is skipped: A1.
    CHECK(l.focusId() == "A1");
}

TEST_CASE("interaction: autofocus is the initial focus and the first navigation target") {
    Loaded l = load(doc(kGridStyle, "<button id=\"A1\"></button><button id=\"B2\" autofocus></button>"));
    CHECK(l.focusId() == "B2");  // The surface's first show.
    l.doc->setFocus(ElementRef{});
    l.pad(GamepadButton::DpadLeft);  // Nothing focused: the autofocus element first.
    CHECK(l.focusId() == "B2");
}

TEST_CASE("interaction: two players keep independent focus; :focus-player(n) styles per player") {
    Loaded l = load(doc(std::string(kGridStyle) + "button:focus-player(1) { background-color: #0000ff; } "
                                                  "button:focus-player(0) { background-color: #ff0000; }",
                        gridBody()));
    l.pad(GamepadButton::DpadRight, 0);  // Player 0: A1.
    l.pad(GamepadButton::DpadRight, 0);  // A2.
    l.pad(GamepadButton::DpadDown, 1);   // Player 1: A1.
    l.pad(GamepadButton::DpadDown, 1);   // B1.
    CHECK(l.focusId(0) == "A2");
    CHECK(l.focusId(1) == "B1");
    CHECK(l.focusId(2) == "");
    CHECK(l.focusId() == "A2");  // Document::focus() is player 0's.
    CHECK(bg(l, "A2").r == doctest::Approx(1.0f));
    CHECK(bg(l, "B1").b == doctest::Approx(1.0f));
    CHECK(bg(l, "B1").r == doctest::Approx(0.0f));
    // Both players on one element: :focus stays until the last one leaves.
    l.doc->setFocus(l.el("A2"), 1);
    CHECK((l.doc->arena().element(l.el("A2").node).stateBits & kStateFocus) != 0);
    l.doc->setFocus(l.el("C3"), 0);
    CHECK((l.doc->arena().element(l.el("A2").node).stateBits & kStateFocus) != 0);
    l.doc->setFocus(l.el("C3"), 1);
    CHECK((l.doc->arena().element(l.el("A2").node).stateBits & kStateFocus) == 0);
}

TEST_CASE("interaction: gamepad A presses/clicks, B sends back up to the surface, shoulders tab") {
    Loaded l = load(doc(kGridStyle,
                        gridBody("on:press=\"state(Pressed)\" on:click=\"state(Clicked)\" "
                                 "on:player(1).click=\"state(P1)\" on:player(0).click=\"state(P0)\""),
                        "on:back=\"state(Back)\""));
    l.doc->setFocus(l.el("A1"), 1);
    l.padDown(GamepadButton::A, 1);
    CHECK(l.has("A1", "Pressed"));
    CHECK((l.doc->arena().element(l.el("A1").node).stateBits & kStateActive) != 0);
    CHECK_FALSE(l.has("A1", "Clicked"));
    l.padUp(GamepadButton::A, 1);
    CHECK(l.has("A1", "Clicked"));
    CHECK(l.has("A1", "P1"));
    CHECK_FALSE(l.has("A1", "P0"));  // player(0).click only for player 0.
    CHECK((l.doc->arena().element(l.el("A1").node).stateBits & kStateActive) == 0);

    CHECK_FALSE(l.has("Root", "Back"));
    l.pad(GamepadButton::B, 1);
    CHECK(l.has("Root", "Back"));  // From the focused A1, bubbling to the body.

    l.pad(GamepadButton::RightShoulder, 1);
    CHECK(l.focusId(1) == "A2");
    l.pad(GamepadButton::LeftShoulder, 1);
    CHECK(l.focusId(1) == "A1");
}

TEST_CASE("interaction: the left stick navigates with a threshold, hysteresis and repeat") {
    Loaded l = load(doc(kGridStyle, gridBody()));
    l.doc->setFocus(l.el("A1"));
    auto stick = [&](GamepadAxis axis, float v) {
        InputEvent ev;
        ev.kind = InputEvent::GamepadAxisMotion;
        ev.gamepadAxis = axis;
        ev.axisValue = v;
        l.send(ev);
    };
    stick(GamepadAxis::LeftX, 0.3f);  // Below the threshold.
    CHECK(l.focusId() == "A1");
    stick(GamepadAxis::LeftX, 0.9f);
    CHECK(l.focusId() == "A2");
    stick(GamepadAxis::LeftX, 0.4f);  // Still held (above the release threshold): no new move.
    CHECK(l.focusId() == "A2");
    l.step(0.45f);  // Past the repeat delay.
    CHECK(l.focusId() == "A3");
    stick(GamepadAxis::LeftX, 0.0f);
    l.step(1.0f);
    CHECK(l.focusId() == "A3");
    stick(GamepadAxis::LeftY, 1.0f);
    CHECK(l.focusId() == "B3");
}

TEST_CASE("interaction: a focus trap keeps Tab and navigation inside the modal") {
    Loaded l = load(doc(R"X(
      button { position: absolute; width: 40px; height: 40px; }
      #Out { left: 0px; top: 0px; }
      #Modal { position: absolute; left: 100px; top: 0px; width: 200px; height: 100px; }
      #M1 { left: 0px; top: 0px; } #M2 { left: 50px; top: 0px; }
    )X",
                        "<button id=\"Out\"></button><div id=\"Modal\"><button id=\"M1\"></button>"
                        "<button id=\"M2\"></button></div>"));
    REQUIRE(l.doc->setFocusTrap(l.el("Modal"), true) == Status::kOk);
    l.doc->setFocus(l.el("M1"));
    l.key(KeyCode::Tab);
    CHECK(l.focusId() == "M2");
    l.key(KeyCode::Tab);
    CHECK(l.focusId() == "M1");  // Wraps inside the modal.
    l.key(KeyCode::Left);        // #Out is left of the modal: blocked.
    CHECK(l.focusId() == "M1");
    l.doc->setFocusTrap(l.el("Modal"), false);
    l.key(KeyCode::Left);
    CHECK(l.focusId() == "Out");
}

// -- Drag and drop --------------------------------------------------------------------------------

namespace {

constexpr const char* kDragStyle = R"X(
  #Src { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; background-color: #ff0000; }
  #Dst { position: absolute; left: 100px; top: 0px; width: 60px; height: 60px; }
  #Src:state(Dragging) { opacity: 0.5; }
  #Dst:state(DropHover) { background-color: #00ff00; }
)X";

constexpr const char* kDragBody =
    "<div id=\"Src\" draggable on:drag.start=\"state(Started)\" on:click=\"state(Clicked)\"></div>"
    "<div id=\"Dst\" drop-target on:drag.over=\"state(Over)\" on:drag.leave=\"state(Left)\" "
    "on:drop=\"state(Dropped)\"></div>";

}  // namespace

TEST_CASE("interaction: pointer drag and drop: events, states, ghost and host drop event") {
    Loaded l = load(doc(kDragStyle, kDragBody));
    std::vector<std::string> log;
    auto record = [&](const char* name) {
        return [&log, name](Event& e) {
            log.push_back(std::string(name) + (e.dragSource != kInvalidNode ? "+src" : "") +
                          (e.dropTarget != kInvalidNode ? "+dst" : ""));
        };
    };
    l.doc->addListener(l.el("Src"), EventType::DragStart, record("start"));
    l.doc->addListener(l.el("Dst"), EventType::DragOver, record("over"));
    l.doc->addListener(l.el("Dst"), EventType::DragLeave, record("leave"));
    l.doc->addListener(l.el("Dst"), EventType::Drop, record("drop"));
    std::vector<HostEvent> host;
    std::vector<std::string> hostIds;
    l.doc->setHostEventListener([&](const HostEvent& e) {
        host.push_back(e);
        hostIds.push_back(std::string(e.elementId) + ">" + std::string(e.targetId));
    });

    l.pointer(InputEvent::PointerMove, 10, 10);
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 12, 11);  // Within 4 px: still a press.
    CHECK_FALSE(l.doc->dragSource().valid());
    l.pointer(InputEvent::PointerMove, 60, 20);  // A drag, over nothing yet.
    REQUIRE(l.doc->dragSource().valid());
    CHECK(l.has("Src", "Started"));
    CHECK(l.has("Src", "Dragging"));
    CHECK(l.el("Src").computedStyleValue(prop::Opacity).f == doctest::Approx(0.5f));
    CHECK(l.doc->dragGhostOffset().x == doctest::Approx(50));
    CHECK(l.doc->dragGhostOffset().y == doctest::Approx(10));
    l.pointer(InputEvent::PointerMove, 120, 20);  // Into the drop target.
    CHECK(l.has("Dst", "Over"));
    CHECK(l.has("Dst", "DropHover"));
    CHECK(bg(l, "Dst").g == doctest::Approx(1.0f));
    CHECK(l.doc->dragOver().node == l.el("Dst").node);

    // The ghost: a 60 % layer after everything else, the source's fill shifted by the offset.
    CommandList list;
    list.viewport = Rect{0, 0, 300, 200};
    l.doc->paint(list);
    REQUIRE(list.commands.size() >= 3);
    CHECK(list.commands.back().kind == CmdKind::PopLayer);
    bool ghostLayer = false;
    for (size_t i = 0; i < list.commands.size(); ++i) {
        if (list.commands[i].kind == CmdKind::PushLayer &&
            list.layers[list.commands[i].index].opacity == doctest::Approx(0.6f)) {
            ghostLayer = true;
            REQUIRE(i + 1 < list.commands.size());
            const Command& fill = list.commands[i + 1];
            REQUIRE(fill.kind == CmdKind::FillPath);
            CHECK(list.fills[fill.index].xf.e == doctest::Approx(110));  // 0 + 110 px.
        }
    }
    CHECK(ghostLayer);

    l.pointer(InputEvent::PointerUp, 120, 20);
    CHECK(l.has("Dst", "Dropped"));
    CHECK_FALSE(l.has("Dst", "DropHover"));
    CHECK_FALSE(l.has("Src", "Dragging"));
    CHECK_FALSE(l.has("Src", "Clicked"));  // A drag never clicks.
    CHECK_FALSE(l.doc->dragSource().valid());
    REQUIRE(log.size() == 3);
    CHECK(log[0] == "start+src");
    CHECK(log[1] == "over+src+dst");
    CHECK(log[2] == "drop+src+dst");
    REQUIRE(host.size() == 1);
    CHECK(host[0].kind == HostEventKind::Drop);
    CHECK(hostIds[0] == "Src>Dst");
}

TEST_CASE("interaction: a drag released off every target leaves without a drop") {
    Loaded l = load(doc(kDragStyle, kDragBody));
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 120, 20);
    CHECK(l.has("Dst", "Over"));
    l.pointer(InputEvent::PointerMove, 250, 150);
    CHECK(l.has("Dst", "Left"));
    l.pointer(InputEvent::PointerUp, 250, 150);
    CHECK_FALSE(l.has("Dst", "Dropped"));
    CHECK_FALSE(l.has("Src", "Dragging"));
}

TEST_CASE("interaction: gamepad drag: A long press starts, the d-pad targets, A drops, B cancels") {
    Loaded l = load(doc(kDragStyle, kDragBody));
    l.doc->setFocus(l.el("Src"));
    l.padDown(GamepadButton::A);
    CHECK_FALSE(l.doc->dragSource().valid());
    l.step(0.6f);  // Held past the long press.
    REQUIRE(l.doc->dragSource().valid());
    CHECK(l.has("Src", "Dragging"));
    l.padUp(GamepadButton::A);
    CHECK(l.doc->dragSource().valid());  // The release of the long press does not drop.
    CHECK_FALSE(l.has("Src", "Clicked"));
    l.pad(GamepadButton::DpadRight);
    CHECK(l.focusId() == "Dst");
    CHECK(l.has("Dst", "Over"));
    l.pad(GamepadButton::A);
    CHECK(l.has("Dst", "Dropped"));
    CHECK_FALSE(l.doc->dragSource().valid());

    // B cancels.
    Loaded m = load(doc(kDragStyle, kDragBody));
    m.doc->setFocus(m.el("Src"));
    m.padDown(GamepadButton::A);
    m.step(0.6f);
    m.padUp(GamepadButton::A);
    m.pad(GamepadButton::DpadRight);
    m.pad(GamepadButton::B);
    CHECK_FALSE(m.doc->dragSource().valid());
    CHECK(m.has("Dst", "Left"));
    CHECK_FALSE(m.has("Dst", "Dropped"));
}

// -- Drag reorder (T-388, 08 §8.12) ---------------------------------------------------------------

namespace {

constexpr const char* kReorderBody =
    "<div id=\"Src\" draggable on:drag.cancel=\"state(Cancelled)\"></div>"
    "<div id=\"Dst\" drop-target></div>";

/// Records the drag events of `l`'s Src / Dst (and every Click on Src).
struct DragLog {
    std::vector<std::string> events;
    Vec2 overFraction{-1, -1};
    Vec2 dropFraction{-1, -1};
    int clicks = 0;
    int cancels = 0;
    int ends = 0;
    bool dropped = false;

    void attach(Loaded& l) {
        l.doc->addListener(l.el("Dst"), EventType::DragOver, [this](Event& e) {
            events.push_back("over");
            overFraction = e.dropFraction;
        });
        l.doc->addListener(l.el("Dst"), EventType::DragLeave, [this](Event&) { events.push_back("leave"); });
        l.doc->addListener(l.el("Dst"), EventType::Drop, [this](Event& e) {
            events.push_back("drop");
            dropFraction = e.dropFraction;
        });
        l.doc->addListener(l.el("Src"), EventType::DragCancel, [this](Event& e) {
            events.push_back("cancel");
            CHECK(e.dragSource == e.target);
            ++cancels;
        });
        l.doc->addListener(l.el("Src"), EventType::DragEnd, [this](Event& e) {
            events.push_back("end");
            dropped = e.dropped;
            ++ends;
        });
        l.doc->addListener(l.el("Src"), EventType::Click, [this](Event&) { ++clicks; });
    }
};

/// Presses Src, drags it past the threshold and into Dst at (120, `y`).
void dragIntoDst(Loaded& l, float y) {
    l.pointer(InputEvent::PointerMove, 10, 10);
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 60, y);
    REQUIRE(l.doc->dragSource().valid());
    l.pointer(InputEvent::PointerMove, 120, y);
    REQUIRE(l.doc->dragOver().node == l.el("Dst").node);
}

}  // namespace

TEST_CASE("interaction: T-388 dropFraction on DragOver and Drop; DragEnd dropped=true") {
    {
        Loaded l = load(doc(kDragStyle, kReorderBody));
        DragLog log;
        log.attach(l);
        dragIntoDst(l, 10);  // Dst is 60 px tall from y = 0: the upper half.
        CHECK(log.overFraction.y < 0.5f);
        CHECK(log.overFraction.y == doctest::Approx(10.0f / 60.0f));
        CHECK(log.overFraction.x == doctest::Approx(20.0f / 60.0f));
        l.pointer(InputEvent::PointerUp, 120, 10);
        CHECK(log.dropFraction.y < 0.5f);
        REQUIRE(log.events.size() == 3);
        CHECK(log.events[0] == "over");
        CHECK(log.events[1] == "drop");
        CHECK(log.events[2] == "end");
        CHECK(log.dropped);
        CHECK(log.cancels == 0);
        CHECK(log.clicks == 0);
    }
    {
        Loaded l = load(doc(kDragStyle, kReorderBody));
        DragLog log;
        log.attach(l);
        dragIntoDst(l, 50);  // The lower half.
        CHECK(log.overFraction.y > 0.5f);
        l.pointer(InputEvent::PointerUp, 120, 50);
        CHECK(log.dropFraction.y > 0.5f);
        CHECK(log.dropFraction.y == doctest::Approx(50.0f / 60.0f));
        CHECK(log.dropped);
    }
    {
        // Released off every target: DragEnd with dropped = false, no DragCancel.
        Loaded l = load(doc(kDragStyle, kReorderBody));
        DragLog log;
        log.attach(l);
        l.pointer(InputEvent::PointerDown, 10, 10);
        l.pointer(InputEvent::PointerMove, 250, 150);
        l.pointer(InputEvent::PointerUp, 250, 150);
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0] == "end");
        CHECK_FALSE(log.dropped);
        CHECK(log.cancels == 0);
    }
}

TEST_CASE("interaction: T-388 DragOver repeats on every move over the target; HostEvent::Drop fraction") {
    Loaded l = load(doc(kDragStyle, kReorderBody));
    std::vector<float> overY;
    int hovers = 0;
    l.doc->addListener(l.el("Dst"), EventType::DragOver, [&](Event& e) {
        overY.push_back(e.dropFraction.y);
        CHECK(e.dropTarget == l.el("Dst").node);
    });
    l.doc->addListener(l.el("Dst"), EventType::DragLeave, [&](Event&) { ++hovers; });
    std::vector<HostEvent> host;
    l.doc->setHostEventListener([&](const HostEvent& e) { host.push_back(e); });
    dragIntoDst(l, 10);                           // Enters the top half: DragOver 1.
    l.pointer(InputEvent::PointerMove, 125, 50);  // Same target, bottom half: DragOver 2.
    REQUIRE(overY.size() == 2);
    CHECK(overY[0] < 0.5f);
    CHECK(overY[1] > 0.5f);
    CHECK(hovers == 0);  // No DragLeave while staying on the target.
    CHECK(l.has("Dst", "DropHover"));
    l.pointer(InputEvent::PointerUp, 125, 50);
    REQUIRE(host.size() == 1);
    CHECK(host[0].kind == HostEventKind::Drop);
    CHECK(host[0].dropFraction.y == doctest::Approx(50.0f / 60.0f));
}

TEST_CASE("interaction: T-388 drag cancel: Escape, second button, PointerCancel, cancelDrag()") {
    enum class How { Escape, RightButton, PointerCancel, DocumentApi };
    for (How how : {How::Escape, How::RightButton, How::PointerCancel, How::DocumentApi}) {
        CAPTURE(static_cast<int>(how));
        Loaded l = load(doc(kDragStyle, kReorderBody));
        DragLog log;
        log.attach(l);
        dragIntoDst(l, 10);
        CHECK(l.has("Src", "Dragging"));
        CHECK(l.has("Dst", "DropHover"));
        switch (how) {
            case How::Escape:
                l.key(KeyCode::Escape);
                break;
            case How::RightButton: {
                InputEvent ev;
                ev.kind = InputEvent::PointerDown;
                ev.pos = Vec2{120, 10};
                ev.button = PointerButton::Right;
                l.send(ev);
                ev.kind = InputEvent::PointerUp;
                l.send(ev);
                break;
            }
            case How::PointerCancel: {
                InputEvent ev;
                ev.kind = InputEvent::PointerCancel;
                l.send(ev);
                break;
            }
            case How::DocumentApi:
                l.doc->cancelDrag();
                l.step(0.016f);
                break;
        }
        CHECK_FALSE(l.doc->dragSource().valid());
        CHECK_FALSE(l.has("Src", "Dragging"));
        CHECK_FALSE(l.has("Dst", "DropHover"));
        CHECK(l.has("Src", "Cancelled"));  // on:drag.cancel.
        CHECK(l.doc->dragGhostOffset().x == doctest::Approx(0));
        CHECK(log.cancels == 1);
        CHECK(log.ends == 1);
        CHECK_FALSE(log.dropped);
        REQUIRE(log.events.size() == 4);
        CHECK(log.events[0] == "over");
        CHECK(log.events[1] == "leave");
        CHECK(log.events[2] == "cancel");
        CHECK(log.events[3] == "end");
        // Releasing the (still held) button afterwards drops nothing and clicks nothing, even
        // back on the source.
        l.pointer(InputEvent::PointerMove, 10, 10);
        l.pointer(InputEvent::PointerUp, 10, 10);
        CHECK(log.events.size() == 4);
        CHECK(log.clicks == 0);
        // The next press is a fresh one: a plain click on Src clicks.
        l.pointer(InputEvent::PointerDown, 10, 10);
        l.pointer(InputEvent::PointerUp, 10, 10);
        CHECK(log.clicks == 1);
    }
}

TEST_CASE("interaction: T-388 Context::cancelDrag() cancels the drag of a surface") {
    CompileResult ir = compileXt(doc(kDragStyle, kReorderBody));
    REQUIRE(ir.ok());
    Context ctx{ContextConfig{}};
    const DocumentHandle h = ctx.loadDocument(IrBlob{ir.ir.data(), ir.ir.size()}, false).value();
    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    ctx.update(0);
    Document& d = *ctx.document(h);
    int cancels = 0;
    bool ended = false;
    d.addListener(d.byId("Src"), EventType::DragCancel, [&](Event&) { ++cancels; });
    d.addListener(d.byId("Src"), EventType::DragEnd, [&](Event& e) { ended = !e.dropped; });
    auto push = [&](InputEvent::Kind kind, float x, float y) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = Vec2{x, y};
        ctx.pushInput(ev);
        ctx.update(0);
    };
    push(InputEvent::PointerDown, 10, 10);
    push(InputEvent::PointerMove, 120, 10);
    REQUIRE(d.dragSource().valid());
    ctx.cancelDrag();
    CHECK_FALSE(d.dragSource().valid());
    CHECK(cancels == 1);
    CHECK(ended);
    ctx.cancelDrag();  // No drag: nothing.
    CHECK(cancels == 1);
}

TEST_CASE("interaction: T-388 gamepad B cancel sends DragCancel and DragEnd") {
    Loaded l = load(doc(kDragStyle, kReorderBody));
    DragLog log;
    log.attach(l);
    l.doc->setFocus(l.el("Src"));
    l.padDown(GamepadButton::A);
    l.step(0.6f);
    l.padUp(GamepadButton::A);
    REQUIRE(l.doc->dragSource().valid());
    l.pad(GamepadButton::B);
    CHECK_FALSE(l.doc->dragSource().valid());
    CHECK(l.has("Src", "Cancelled"));
    CHECK(log.cancels == 1);
    CHECK(log.ends == 1);
    CHECK_FALSE(log.dropped);
}

TEST_CASE("interaction: T-388 drag-threshold and Click suppression") {
    {
        Loaded l = load(doc(kDragStyle,
                            "<div id=\"Src\" draggable drag-threshold=\"20\"></div>"
                            "<div id=\"Dst\" drop-target></div>"));
        DragLog log;
        log.attach(l);
        l.pointer(InputEvent::PointerDown, 10, 10);
        l.pointer(InputEvent::PointerMove, 20, 10);  // 10 px < 20: still a press.
        CHECK_FALSE(l.doc->dragSource().valid());
        l.pointer(InputEvent::PointerUp, 20, 10);
        CHECK(log.clicks == 1);
        CHECK(log.ends == 0);

        l.pointer(InputEvent::PointerDown, 10, 10);
        l.pointer(InputEvent::PointerMove, 35, 10);  // 25 px > 20: a drag.
        CHECK(l.doc->dragSource().valid());
        l.pointer(InputEvent::PointerUp, 35, 10);  // Released on the source itself.
        CHECK(log.clicks == 1);                    // No Click after a drag.
        CHECK(log.ends == 1);
    }
    {
        // The default threshold is 4 px.
        Loaded l = load(doc(kDragStyle, kReorderBody));
        l.pointer(InputEvent::PointerDown, 10, 10);
        l.pointer(InputEvent::PointerMove, 13, 10);
        CHECK_FALSE(l.doc->dragSource().valid());
        l.pointer(InputEvent::PointerMove, 15, 10);
        CHECK(l.doc->dragSource().valid());
    }
    {
        CompileResult r = compileXt(doc("", "<div id=\"A\" draggable drag-threshold=\"wide\"></div>"));
        CHECK_FALSE(r.ok());
        CHECK(hasError(r, "drag-threshold must be"));
        CompileResult s = compileXt(doc("", "<div id=\"A\" drag-autoscroll=\"maybe\"></div>"));
        CHECK_FALSE(s.ok());
        CHECK(hasError(s, "drag-autoscroll must be"));
    }
}

namespace {

std::string reorderList(std::string_view scrollAttrs) {
    return "<div id=\"P\" port:items=\"list&lt;struct{name: string}&gt;\"></div>"
           "<div id=\"Scroll\" " +
           std::string(scrollAttrs) +
           " style=\"position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: "
           "scroll\">"
           "<repeat port=\"items\" id=\"Item\" virtualize>"
           "<div id=\"Row\" draggable drop-target style=\"height: 20px\"></div>"
           "</repeat></div>";
}

std::string manyRows(int n) {
    std::string json = "[";
    for (int i = 0; i < n; ++i) {
        json += (i > 0 ? "," : "") + std::string("{\"name\":\"R") + std::to_string(i) + "\"}";
    }
    return json + "]";
}

float scrollYOf(const Loaded& l, std::string_view id) {
    return l.doc->arena().element(l.el(id).node).style.base[prop::ScrollY].f;
}

}  // namespace

TEST_CASE("interaction: T-388 a drag at a scroll container's edge auto-scrolls it") {
    for (bool enabled : {true, false}) {
        CAPTURE(enabled);
        Loaded l = load(doc("", reorderList(enabled ? "" : "drag-autoscroll=\"false\"")));
        REQUIRE(l.doc->setPortText(l.doc->lookupPort("items"), manyRows(200)) == Status::kOk);
        l.step(0.016f);
        REQUIRE(l.el("Scroll").layoutBox().scrollHeight == doctest::Approx(4000.0f));
        l.pointer(InputEvent::PointerMove, 100, 10);
        l.pointer(InputEvent::PointerDown, 100, 10);
        l.pointer(InputEvent::PointerMove, 100, 196);  // 4 px above the bottom edge.
        REQUIRE(l.doc->dragSource().valid());
        CHECK(scrollYOf(l, "Scroll") == doctest::Approx(0));
        int scrolls = 0;  // T-387: the auto-scroll goes through setScroll() and dispatches Scroll.
        l.doc->addListener(l.el("Scroll"), EventType::Scroll, [&](Event&) { ++scrolls; });
        l.step(0.5f);
        const float maxY = 4000.0f - 200.0f;
        CHECK(scrolls == (enabled ? 1 : 0));
        if (enabled) {
            // (24 - 4) / 24 x 600 px/s x 0.5 s.
            CHECK(scrollYOf(l, "Scroll") == doctest::Approx(250.0f));
            CHECK(scrollYOf(l, "Scroll") <= maxY);
        } else {
            CHECK(scrollYOf(l, "Scroll") == doctest::Approx(0));
        }
    }
}

// -- Overlay layer --------------------------------------------------------------------------------

namespace {

constexpr const char* kOverlayStyle = R"X(
  #Clip { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; overflow: hidden; }
  #Pop { position: absolute; left: 20px; top: 20px; width: 60px; height: 60px; background-color: #ff0000; }
  #Cover { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; background-color: #0000ff; }
  .over { layer: overlay; }
)X";

/// Index in `list.commands` of the FillPath whose solid color is `color`, or -1.
int fillIndex(const CommandList& list, float r, float b) {
    for (size_t i = 0; i < list.commands.size(); ++i) {
        const Command& c = list.commands[i];
        if (c.kind == CmdKind::FillPath && list.fills[c.index].paint.color.r == doctest::Approx(r) &&
            list.fills[c.index].paint.color.b == doctest::Approx(b)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Clip depth (open PushClipRect/Path minus PopClip) right before command `index`.
int clipDepthAt(const CommandList& list, int index) {
    int depth = 0;
    for (int i = 0; i < index; ++i) {
        const CmdKind k = list.commands[static_cast<size_t>(i)].kind;
        depth +=
            (k == CmdKind::PushClipRect || k == CmdKind::PushClipPath) ? 1 : (k == CmdKind::PopClip ? -1 : 0);
    }
    return depth;
}

}  // namespace

TEST_CASE("interaction: layer: overlay paints after the root, unclipped, and hit tests first") {
    Loaded plain =
        load(doc(kOverlayStyle, "<div id=\"Clip\"><div id=\"Pop\"></div></div><div id=\"Cover\"></div>"));
    CommandList a;
    a.viewport = Rect{0, 0, 300, 200};
    plain.doc->paint(a);
    const int popA = fillIndex(a, 1, 0);
    const int coverA = fillIndex(a, 0, 1);
    REQUIRE(popA >= 0);
    REQUIRE(coverA >= 0);
    CHECK(popA < coverA);
    CHECK(clipDepthAt(a, popA) == 1);
    CHECK(plain.doc->hitTest(Vec2{70, 70}) == plain.el("Cover").node);

    Loaded over =
        load(doc(kOverlayStyle,
                 "<div id=\"Clip\"><div id=\"Pop\" class=\"over\"></div></div><div id=\"Cover\"></div>"));
    CommandList b;
    b.viewport = Rect{0, 0, 300, 200};
    over.doc->paint(b);
    const int popB = fillIndex(b, 1, 0);
    const int coverB = fillIndex(b, 0, 1);
    REQUIRE(popB >= 0);
    REQUIRE(coverB >= 0);
    CHECK(popB > coverB);
    CHECK(clipDepthAt(b, popB) == 0);
    CHECK(over.doc->hitTest(Vec2{70, 70}) == over.el("Pop").node);  // Outside #Clip's clip.
    CHECK(over.doc->hitTest(Vec2{90, 90}) == over.el("Cover").node);
}

TEST_CASE("interaction: layer(overlay) / layer(normal) actions") {
    // The action applies to the trigger's own element.
    Loaded m = load(doc(kOverlayStyle,
                        "<div id=\"Clip\"><div id=\"Pop\" on:focus=\"layer(overlay)\" "
                        "on:blur=\"layer(normal)\"></div></div><div id=\"Cover\"></div>"));
    CHECK(m.doc->hitTest(Vec2{70, 70}) == m.el("Cover").node);
    m.doc->setFocus(m.el("Pop"));
    m.step(0);
    CHECK(m.el("Pop").computedStyleValue(196).e == 1);
    CHECK(m.doc->hitTest(Vec2{70, 70}) == m.el("Pop").node);
    m.doc->setFocus(ElementRef{});
    m.step(0);
    CHECK(m.doc->hitTest(Vec2{70, 70}) == m.el("Cover").node);
}

// -- Surface stacking -----------------------------------------------------------------------------

namespace {

struct TwoSurfaces {
    CompileResult bottomIr;
    CompileResult topIr;
    Context ctx{ContextConfig{}};
    DocumentHandle bottom = 0;
    DocumentHandle top = 0;
    float clock = 0;

    TwoSurfaces() {
        bottomIr = compileXt(
            doc("#B { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }",
                "<button id=\"B\" on:press=\"state(Pressed)\" on:hover=\"state(Hovered)\"></button>"));
        topIr = compileXt(
            doc("#T { position: absolute; left: 100px; top: 0px; width: 40px; height: 40px; }",
                "<button id=\"T\" on:press=\"state(Pressed)\" on:hover=\"state(Hovered)\"></button>"));
        REQUIRE(bottomIr.ok());
        REQUIRE(topIr.ok());
        bottom = ctx.loadDocument(IrBlob{bottomIr.ir.data(), bottomIr.ir.size()}, false).value();
        top = ctx.loadDocument(IrBlob{topIr.ir.data(), topIr.ir.size()}, false).value();
        ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
        ctx.update(0);
    }
    void pointer(InputEvent::Kind kind, float x, float y) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = Vec2{x, y};
        ctx.pushInput(ev);
        ctx.update(clock);
    }
    bool has(DocumentHandle h, std::string_view id, std::string_view state) {
        Document* d = ctx.document(h);
        return d->hasCustomState(d->byId(id), state);
    }
};

}  // namespace

TEST_CASE("interaction: surfaces: Block takes everything, Pass lets empty areas through") {
    TwoSurfaces s;
    CHECK(s.ctx.surfaceInput(s.top) == SurfaceInput::Block);
    s.pointer(InputEvent::PointerDown, 10, 10);
    s.pointer(InputEvent::PointerUp, 10, 10);
    CHECK_FALSE(s.has(s.bottom, "B", "Pressed"));  // The top surface's body swallowed it.

    REQUIRE(s.ctx.setSurfaceInput(s.top, SurfaceInput::Pass) == Status::kOk);
    s.pointer(InputEvent::PointerDown, 10, 10);
    s.pointer(InputEvent::PointerUp, 10, 10);
    CHECK(s.has(s.bottom, "B", "Pressed"));
    s.pointer(InputEvent::PointerDown, 110, 10);
    s.pointer(InputEvent::PointerUp, 110, 10);
    CHECK(s.has(s.top, "T", "Pressed"));
}

TEST_CASE("interaction: surfaces: Watch sees hover but never presses; z reorders") {
    TwoSurfaces s;
    REQUIRE(s.ctx.setSurfaceInput(s.top, SurfaceInput::Watch) == Status::kOk);
    s.pointer(InputEvent::PointerMove, 110, 10);
    CHECK(s.has(s.top, "T", "Hovered"));
    s.pointer(InputEvent::PointerDown, 110, 10);
    s.pointer(InputEvent::PointerUp, 110, 10);
    CHECK_FALSE(s.has(s.top, "T", "Pressed"));
    s.pointer(InputEvent::PointerMove, 10, 10);
    CHECK(s.has(s.bottom, "B", "Hovered"));  // The move also reached the surface below.

    TwoSurfaces z;
    REQUIRE(z.ctx.setSurfaceZ(z.bottom, 5) == Status::kOk);
    CHECK(z.ctx.surfaceZ(z.bottom) == 5);
    z.pointer(InputEvent::PointerDown, 10, 10);
    z.pointer(InputEvent::PointerUp, 10, 10);
    CHECK(z.has(z.bottom, "B", "Pressed"));  // `bottom` is now the top Block surface.
    // Keyboard / gamepad go to the topmost Block surface too.
    InputEvent pad;
    pad.kind = InputEvent::GamepadButtonDown;
    pad.gamepadButton = GamepadButton::DpadRight;
    z.ctx.pushInput(pad);
    z.ctx.update(0);
    Document* bottom = z.ctx.document(z.bottom);
    CHECK(bottom->focus().valid());
    CHECK_FALSE(z.ctx.document(z.top)->focus().valid());

    CHECK(z.ctx.setSurfaceInput(99, SurfaceInput::Pass) == Status::kInvalidArgument);
    CHECK(z.ctx.setPlayerDevice(8, InputKind::Gamepad) == Status::kInvalidArgument);
    CHECK(z.ctx.setPlayerDevice(1, InputKind::Gamepad) == Status::kOk);
    CHECK(z.ctx.playerDevice(1) == InputKind::Gamepad);
}

TEST_CASE("interaction: a drag released over another surface reaches ContextListener::onDragCross") {
    CompileResult srcIr = compileXt(doc(kDragStyle, "<div id=\"Src\" draggable></div>"));
    CompileResult dstIr =
        compileXt(doc("#Bin { position: absolute; left: 150px; top: 0px; width: 40px; height: 40px; }",
                      "<div id=\"Bin\" drop-target></div>"));
    REQUIRE(srcIr.ok());
    REQUIRE(dstIr.ok());
    Context ctx{ContextConfig{}};
    const DocumentHandle dst = ctx.loadDocument(IrBlob{dstIr.ir.data(), dstIr.ir.size()}, false).value();
    const DocumentHandle src = ctx.loadDocument(IrBlob{srcIr.ir.data(), srcIr.ir.size()}, false).value();
    ctx.setSurfaceInput(src, SurfaceInput::Pass);
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    ctx.update(0);
    struct Listener : ContextListener {
        std::vector<std::string> seen;
        void onDragCross(const DragCross& c) override {
            seen.push_back(std::string(c.sourceId) + ">" + std::string(c.targetId));
        }
    } listener;
    ctx.addListener(&listener);
    auto push = [&](InputEvent::Kind kind, float x, float y) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = Vec2{x, y};
        ctx.pushInput(ev);
        ctx.update(0);
    };
    push(InputEvent::PointerDown, 10, 10);
    push(InputEvent::PointerMove, 160, 10);
    push(InputEvent::PointerUp, 160, 10);
    REQUIRE(listener.seen.size() == 1);
    CHECK(listener.seen[0] == "Src>Bin");
    CHECK(ctx.surfaceInput(dst) == SurfaceInput::Block);
    ctx.removeListener(&listener);
}

// -- Gestures -------------------------------------------------------------------------------------

TEST_CASE("interaction: GestureRecognizer: tap, double tap, swipe, hold, pinch") {
    GestureRecognizer g;
    InputEvent ev;
    g.touchDown(1, Vec2{10, 10}, 0.0f);
    g.touchUp(1, Vec2{11, 10}, 0.1f);
    REQUIRE(g.poll(ev));
    CHECK(ev.kind == InputEvent::Gesture);
    CHECK(ev.gesture == GestureKind::Tap);
    CHECK_FALSE(g.poll(ev));
    g.touchDown(1, Vec2{12, 10}, 0.25f);
    g.touchUp(1, Vec2{12, 10}, 0.3f);
    REQUIRE(g.poll(ev));
    CHECK(ev.gesture == GestureKind::Tap);
    REQUIRE(g.poll(ev));
    CHECK(ev.gesture == GestureKind::DoubleTap);

    g.touchDown(2, Vec2{100, 100}, 1.0f);
    g.touchMove(2, Vec2{60, 105}, 1.1f);
    g.touchUp(2, Vec2{20, 110}, 1.2f);
    REQUIRE(g.poll(ev));
    CHECK(ev.gesture == GestureKind::Swipe);
    CHECK(ev.direction == NavDirection::Left);

    g.touchDown(3, Vec2{50, 50}, 2.0f);
    g.update(2.3f);
    CHECK_FALSE(g.poll(ev));
    g.update(2.6f);
    REQUIRE(g.poll(ev));
    CHECK(ev.gesture == GestureKind::Hold);
    g.touchUp(3, Vec2{50, 50}, 2.7f);
    CHECK_FALSE(g.poll(ev));  // A hold is not also a tap.

    GestureConfig config;
    config.pinchMinChange = 0.2f;
    g.setConfig(config);
    g.touchDown(4, Vec2{100, 100}, 3.0f);
    g.touchDown(5, Vec2{200, 100}, 3.0f);
    g.touchMove(5, Vec2{210, 100}, 3.1f);  // 1.1x: below the threshold.
    CHECK_FALSE(g.poll(ev));
    g.touchMove(5, Vec2{250, 100}, 3.2f);  // 1.5x.
    REQUIRE(g.poll(ev));
    CHECK(ev.gesture == GestureKind::Pinch);
    CHECK(ev.gestureScale == doctest::Approx(1.5f));
    CHECK(ev.pos.x == doctest::Approx(175));
    g.touchUp(4, Vec2{100, 100}, 3.3f);
    g.touchUp(5, Vec2{250, 100}, 3.3f);
    CHECK_FALSE(g.poll(ev));
}

TEST_CASE("interaction: gesture events reach gesture() triggers of the hit element") {
    Loaded l = load(doc(kGridStyle, gridBody("on:gesture(tap)=\"state(Tapped)\" "
                                             "on:gesture(swipe(left))=\"state(SwipedLeft)\" "
                                             "on:gesture(swipe)=\"state(Swiped)\"")));
    InputEvent ev;
    ev.kind = InputEvent::Gesture;
    ev.pos = Vec2{10, 10};
    ev.gesture = GestureKind::Swipe;
    ev.direction = NavDirection::Right;
    l.send(ev);
    CHECK(l.has("A1", "Swiped"));
    CHECK_FALSE(l.has("A1", "SwipedLeft"));
    ev.direction = NavDirection::Left;
    l.send(ev);
    CHECK(l.has("A1", "SwipedLeft"));
    ev.gesture = GestureKind::Tap;
    l.send(ev);
    CHECK(l.has("A1", "Tapped"));
}

// -- Haptics, announcements -----------------------------------------------------------------------

TEST_CASE("interaction: haptic(cue, strength) and aria-label announcements reach the host") {
    Loaded l = load(doc(kGridStyle, gridBody("aria-label=\"Start game\" on:click=\"haptic(rumble, 0.25)\"")));
    std::vector<HostEvent> events;
    std::vector<std::string> texts;
    l.doc->setHostEventListener([&](const HostEvent& e) {
        events.push_back(e);
        texts.push_back(std::string(e.kind == HostEventKind::Announce ? e.text : e.name));
    });
    l.pad(GamepadButton::DpadRight, 2);  // Player 2 lands on A1 (first target).
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == HostEventKind::Announce);
    CHECK(texts[0] == "Start game");
    CHECK(events[0].player == 2);
    l.pad(GamepadButton::A, 2);
    REQUIRE(events.size() == 2);
    CHECK(events[1].kind == HostEventKind::Haptic);
    CHECK(texts[1] == "rumble");
    CHECK(events[1].strength == doctest::Approx(0.25f));
    CHECK(events[1].player == 2);
    l.pad(GamepadButton::DpadRight, 2);  // A2 has no aria-label: nothing announced.
    CHECK(events.size() == 2);
}

// -- Compiler -------------------------------------------------------------------------------------

TEST_CASE("interaction: compiler accepts the T-194 syntax and encodes the new trigger codes") {
    CompileResult r = compileXt(
        doc(std::string(kGridStyle) +
                "#A2 { focus-nav: explicit; layer: overlay; } button:focus-player(3) { color: red; }",
            "<button id=\"A1\" draggable drop-target=\"true\" autofocus nav:up=\"#A2\" aria-label=\"x\" "
            "on:drag.start=\"hide\" on:drag.over=\"hide\" on:drag.leave=\"hide\" on:drag.drop=\"hide\" "
            "on:back=\"hide\" on:nav.blocked(left)=\"hide\" on:gesture(pinch)=\"hide\" "
            "on:player(7).back=\"layer(overlay)\" on:click=\"haptic(x, 0.5); layer(normal)\"></button>"
            "<button id=\"A2\"></button>"));
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const TriggerChunk& trig = view.value().triggers();
    bool player = false;
    bool gesture = false;
    bool nav = false;
    for (const IrTrigger& t : trig.triggers()) {
        if (static_cast<TriggerEvent>(t.event) == TriggerEvent::Player) {
            player = t.arg0 == 7 && t.arg1 == static_cast<uint32_t>(TriggerEvent::Back);
        }
        if (static_cast<TriggerEvent>(t.event) == TriggerEvent::Gesture) {
            gesture = t.arg0 == static_cast<uint32_t>(GestureKind::Pinch) && t.arg1 == kInvalidString;
        }
        if (static_cast<TriggerEvent>(t.event) == TriggerEvent::NavBlocked) {
            nav = t.arg0 == static_cast<uint32_t>(NavDirection::Left);
        }
    }
    CHECK(player);
    CHECK(gesture);
    CHECK(nav);
    bool haptic = false;
    bool layer = false;
    for (const IrTriggerAction& a : trig.actions()) {
        haptic = haptic ||
                 (a.kind == static_cast<uint8_t>(TriggerAction::Haptic) && a.arg2 == doctest::Approx(0.5f));
        layer = layer || a.kind == static_cast<uint8_t>(TriggerAction::Layer);
    }
    CHECK(haptic);
    CHECK(layer);
}

TEST_CASE("interaction: compiler errors for malformed T-194 syntax") {
    auto errs = [](std::string_view attrs, std::string_view needle, std::string_view style = "") {
        CompileResult r =
            compileXt(doc(style, "<div id=\"A\" " + std::string(attrs) + "></div><div id=\"B\"></div>"));
        CAPTURE(attrs);
        CHECK_FALSE(r.ok());
        CHECK(hasError(r, needle));
    };
    errs("nav:diagonal=\"#B\"", "unknown navigation direction");
    errs("nav:up=\"#Nobody\"", "is not an element id");
    errs("draggable=\"maybe\"", "must be true or false");
    errs("on:player(8).press=\"hide\"", "player(<0..7>)");
    errs("on:player(1).check=\"hide\"", "does not support");
    errs("on:gesture(wiggle)=\"hide\"", "expected gesture(");
    errs("on:nav.blocked(sideways)=\"hide\"", "nav.blocked");
    errs("on:click=\"layer(top)\"", "overlay or normal");
    errs("on:click=\"haptic(x, strong)\"", "strength must be a number");
    errs("", "player index 0-7", "#A:focus-player(8) { color: red; }");
}

// -- T-379 (08.10): key-activation switch and DoubleClick ------------------------------------------

namespace {

constexpr const char* kActivationStyle = R"X(
  #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
  #B { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px; }
  #C { position: absolute; left: 100px; top: 0px; width: 20px; height: 20px; }
  #T { position: absolute; left: 150px; top: 0px; width: 40px; height: 40px; }
)X";
constexpr const char* kActivationBody =
    "<button id=\"A\"></button><button id=\"B\"></button>"
    "<input id=\"C\" type=\"checkbox\" /><div id=\"T\" tabindex=\"0\"></div>";

/// One press + release at (x, y) carrying the host click count `clicks` on both.
void clickAt(Loaded& m, float x, float y, uint8_t clicks = 0) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{x, y};
    ev.clicks = clicks;
    m.send(ev);
    ev.kind = InputEvent::PointerUp;
    m.send(ev);
}

}  // namespace

TEST_CASE("T-379: key-activation auto (default): Enter / Space activate; none skips them, Tab still moves") {
    {
        Loaded m = load(doc(kActivationStyle, kActivationBody));
        CHECK(m.doc->keyboardActivation());
        int clicks = 0;
        m.doc->addListener(m.el("A"), EventType::Click, [&](Event&) { ++clicks; });
        m.doc->setFocus(m.el("A"), 0);
        m.key(KeyCode::Enter);
        m.key(KeyCode::Space);
        CHECK(clicks == 2);
    }
    Loaded m = load(doc(kActivationStyle, kActivationBody, "key-activation=\"none\""));
    CHECK_FALSE(m.doc->keyboardActivation());
    int clicks = 0;
    int tabClicks = 0;
    m.doc->addListener(m.el("A"), EventType::Click, [&](Event&) { ++clicks; });
    m.doc->addListener(m.el("T"), EventType::Click, [&](Event&) { ++tabClicks; });
    m.doc->setFocus(m.el("A"), 0);
    m.key(KeyCode::Enter);
    m.key(KeyCode::Space);
    CHECK(clicks == 0);
    m.doc->setFocus(m.el("C"), 0);
    m.key(KeyCode::Space);
    CHECK_FALSE(m.el("C").hasState(kStateChecked));
    m.doc->setFocus(m.el("T"), 0);
    m.key(KeyCode::Enter);
    CHECK(tabClicks == 0);
    // Navigation is unaffected.
    m.doc->setFocus(m.el("A"), 0);
    m.key(KeyCode::Tab);
    CHECK(m.focusId() == "B");
    // The pointer still clicks.
    clickAt(m, 10, 10);
    CHECK(clicks == 1);
}

TEST_CASE("T-379: Context::setKeyboardActivation overrides every document; true returns to the declaration") {
    CompileResult autoIr = compileXt(doc(kActivationStyle, kActivationBody));
    CompileResult noneIr = compileXt(doc(kActivationStyle, kActivationBody, "key-activation=\"none\""));
    REQUIRE(autoIr.ok());
    REQUIRE(noneIr.ok());
    Context ctx{ContextConfig{}};
    CHECK(ctx.keyboardActivation());
    const DocumentHandle ha = ctx.loadDocument(IrBlob{autoIr.ir.data(), autoIr.ir.size()}, false).value();
    const DocumentHandle hn = ctx.loadDocument(IrBlob{noneIr.ir.data(), noneIr.ir.size()}, false).value();
    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    ctx.update(0);
    Document* a = ctx.document(ha);
    Document* n = ctx.document(hn);
    int clicksA = 0;
    int clicksN = 0;
    a->addListener(a->byId("A"), EventType::Click, [&](Event&) { ++clicksA; });
    n->addListener(n->byId("A"), EventType::Click, [&](Event&) { ++clicksN; });
    a->setFocus(a->byId("A"), 0);
    n->setFocus(n->byId("A"), 0);
    InputEvent enter;
    enter.kind = InputEvent::KeyDown;
    enter.key = KeyCode::Enter;

    ctx.setKeyboardActivation(false);
    CHECK_FALSE(ctx.keyboardActivation());
    a->dispatchInput(enter);
    n->dispatchInput(enter);
    CHECK(clicksA == 0);
    CHECK(clicksN == 0);

    ctx.setKeyboardActivation(true);
    a->dispatchInput(enter);
    n->dispatchInput(enter);
    CHECK(clicksA == 1);
    CHECK(clicksN == 0);  // Back to its own `key-activation="none"`.
}

TEST_CASE("T-379: DoubleClick after the second Click within 400 ms on the same target, bubbling") {
    Loaded m = load(doc(kActivationStyle, kActivationBody));
    std::vector<std::string> order;
    int rootDouble = 0;
    m.doc->addListener(m.el("A"), EventType::Click, [&](Event&) { order.push_back("click"); });
    m.doc->addListener(m.el("A"), EventType::DoubleClick, [&](Event& ev) {
        order.push_back("double");
        CHECK(ev.target == m.el("A").node);
    });
    m.doc->addListener(m.el("Root"), EventType::DoubleClick, [&](Event& ev) {
        ++rootDouble;
        CHECK(ev.target == m.el("A").node);
    });

    clickAt(m, 10, 10);
    m.step(0.3f);
    clickAt(m, 12, 11);  // 300 ms and ~2 px later.
    CHECK(order == std::vector<std::string>{"click", "click", "double"});
    CHECK(rootDouble == 1);

    // A third quick click starts a new pair: no second DoubleClick.
    m.step(0.1f);
    clickAt(m, 10, 10);
    CHECK(rootDouble == 1);

    // 500 ms apart: no DoubleClick.
    m.step(0.5f);
    clickAt(m, 10, 10);
    CHECK(rootDouble == 1);

    // Too far apart (> 4 px): none either.
    m.step(0.1f);
    clickAt(m, 20, 10);
    CHECK(rootDouble == 1);

    // A different target: none.
    m.step(1.0f);
    clickAt(m, 10, 10);
    m.step(0.1f);
    clickAt(m, 60, 10);
    CHECK(rootDouble == 1);
    CHECK(std::count(order.begin(), order.end(), std::string("click")) == 6);
}

TEST_CASE("T-379: the host's click count decides when given; on:dblclick fires the trigger") {
    Loaded m = load(doc(kActivationStyle, "<button id=\"A\" on:dblclick=\"state(Twice)\"></button>"));
    int doubles = 0;
    m.doc->addListener(m.el("A"), EventType::DoubleClick, [&](Event&) { ++doubles; });

    // The host says these are single clicks: no DoubleClick although they are 100 ms apart.
    clickAt(m, 10, 10, 1);
    m.step(0.1f);
    clickAt(m, 10, 10, 1);
    CHECK(doubles == 0);
    CHECK_FALSE(m.has("A", "Twice"));

    // clicks = 2 dispatches it directly, whatever the timing.
    m.step(2.0f);
    clickAt(m, 10, 10, 2);
    CHECK(doubles == 1);
    CHECK(m.has("A", "Twice"));
}

TEST_CASE("T-379: compiler rejects an unknown key-activation value") {
    CompileResult r = compileXt(doc("", "<div id=\"A\"></div>", "key-activation=\"sometimes\""));
    CHECK_FALSE(r.ok());
    CHECK(hasError(r, "key-activation"));
    CHECK(compileXt(doc("", "<div id=\"A\"></div>", "key-activation=\"auto\"")).ok());
}

// -- T-383 (08.10, Aux Duty XF-044): Event::clicks -------------------------------------------------

TEST_CASE("T-383: Click carries its click number (1, 2), DoubleClick 2; the host's count wins") {
    Loaded m = load(doc(kActivationStyle, kActivationBody));
    std::vector<int> clicks;
    std::vector<int> doubles;
    m.doc->addListener(m.el("A"), EventType::Click, [&](Event& ev) { clicks.push_back(ev.clicks); });
    m.doc->addListener(m.el("A"), EventType::DoubleClick, [&](Event& ev) { doubles.push_back(ev.clicks); });

    clickAt(m, 10, 10);
    m.step(0.2f);
    clickAt(m, 10, 10);
    CHECK(clicks == std::vector<int>{1, 2});
    CHECK(doubles == std::vector<int>{2});

    // A third quick click starts a new run; a slow one too.
    m.step(0.1f);
    clickAt(m, 10, 10);
    m.step(1.0f);
    clickAt(m, 10, 10);
    CHECK(clicks == std::vector<int>{1, 2, 1, 1});

    // The host's InputEvent::clicks is used as is.
    m.step(2.0f);
    clickAt(m, 10, 10, 2);
    CHECK(clicks.back() == 2);
    CHECK(doubles == std::vector<int>{2, 2});
    clickAt(m, 10, 10, 1);
    CHECK(clicks.back() == 1);

    // Keyboard activation: 1.
    m.doc->setFocus(m.el("A"), 0);
    m.key(KeyCode::Enter);
    CHECK(clicks.back() == 1);
    CHECK(clicks.size() == 7);
}

// -- T-386 (08.11, 05 §5.9.11; Aux Duty XF-052/053/054/056) ------------------------------------------

namespace {

bool isActive(const Loaded& m, std::string_view id) {
    return (m.doc->arena().element(m.el(id).node).stateBits & kStateActive) != 0;
}

}  // namespace

TEST_CASE("T-386: PointerCancel goes to the active element, clears :active, no Click after; else no-op") {
    Loaded m = load(doc("#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }",
                        "<button id=\"A\" on:pointercancel=\"state(Cancelled)\"></button>"));
    int cancels = 0;
    int rootCancels = 0;
    int clicks = 0;
    m.doc->addListener(m.el("A"), EventType::PointerCancel, [&](Event& ev) {
        ++cancels;
        CHECK(ev.target == m.el("A").node);
    });
    m.doc->addListener(m.el("Root"), EventType::PointerCancel, [&](Event&) { ++rootCancels; });
    m.doc->addListener(m.el("A"), EventType::Click, [&](Event&) { ++clicks; });

    // Nothing pressed: nothing happens.
    InputEvent cancel;
    cancel.kind = InputEvent::PointerCancel;
    m.send(cancel);
    CHECK(cancels == 0);

    m.pointer(InputEvent::PointerDown, 10, 10);
    CHECK(isActive(m, "A"));
    CHECK(isActive(m, "Root"));
    m.send(cancel);
    CHECK(cancels == 1);
    CHECK(rootCancels == 1);  // Bubbles.
    CHECK(m.has("A", "Cancelled"));
    CHECK_FALSE(isActive(m, "A"));
    CHECK_FALSE(isActive(m, "Root"));
    m.pointer(InputEvent::PointerUp, 10, 10);
    CHECK(clicks == 0);

    // A second cancel without a press is a no-op; a normal press + release still clicks.
    m.send(cancel);
    CHECK(cancels == 1);
    m.pointer(InputEvent::PointerDown, 10, 10);
    m.pointer(InputEvent::PointerUp, 10, 10);
    CHECK(clicks == 1);
}

TEST_CASE("T-386: Context::cancelPointer cancels the press of the surface holding it") {
    CompileResult r =
        compileXt(doc("#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }",
                      "<button id=\"A\" on:pointercancel=\"state(Cancelled)\"></button>"));
    REQUIRE(r.ok());
    Context ctx{ContextConfig{}};
    const DocumentHandle h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false).value();
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    ctx.update(0);
    Document* d = ctx.document(h);
    ctx.cancelPointer();  // Nothing pressed.
    ctx.update(0);
    CHECK_FALSE(d->hasCustomState(d->byId("A"), "Cancelled"));
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{10, 10};
    ctx.pushInput(down);
    ctx.update(0);
    CHECK((d->arena().element(d->byId("A").node).stateBits & kStateActive) != 0);
    ctx.cancelPointer();
    ctx.update(0);
    CHECK(d->hasCustomState(d->byId("A"), "Cancelled"));
    CHECK((d->arena().element(d->byId("A").node).stateBits & kStateActive) == 0);
}

TEST_CASE("T-386: Document::localPosIn / fractionIn invert the world transform; fractions clamp") {
    Loaded m = load(doc("#E { position: absolute; left: 50px; top: 20px; width: 100px; height: 40px;"
                        " transform: translate(10px, 0px) scale(2); transform-origin: 0px 0px; }"
                        " #N { display: none; }",
                        "<div id=\"E\"></div><div id=\"N\"></div>"),
                    400, 300);
    const NodeId e = m.el("E").node;
    const Vec2 screen = m.doc->nodeToScreen(e, Vec2{25, 10});
    CHECK(screen.x == doctest::Approx(110.0f));  // 50 + 10 + 2 * 25.
    CHECK(screen.y == doctest::Approx(40.0f));   // 20 + 2 * 10.
    const Vec2 local = m.doc->localPosIn(e, Vec2{110, 40});
    CHECK(local.x == doctest::Approx(25.0f));
    CHECK(local.y == doctest::Approx(10.0f));
    const Vec2 f = m.doc->fractionIn(e, Vec2{110, 40});
    CHECK(f.x == doctest::Approx(0.25f));
    CHECK(f.y == doctest::Approx(0.25f));
    const Vec2 outside = m.doc->localPosIn(e, Vec2{0, 0});
    CHECK(outside.x == doctest::Approx(-30.0f));
    CHECK(outside.y == doctest::Approx(-10.0f));
    const Vec2 lo = m.doc->fractionIn(e, Vec2{0, 0});
    CHECK(lo.x == 0.0f);
    CHECK(lo.y == 0.0f);
    const Vec2 hi = m.doc->fractionIn(e, Vec2{1000, 1000});
    CHECK(hi.x == 1.0f);
    CHECK(hi.y == 1.0f);
    // Not laid out (display: none) and an invalid node: (0, 0).
    const Vec2 none = m.doc->fractionIn(m.el("N").node, Vec2{10, 10});
    CHECK(none.x == 0.0f);
    CHECK(none.y == 0.0f);
    CHECK(m.doc->localPosIn(kInvalidNode, Vec2{5, 5}).x == 0.0f);
}

namespace {

constexpr const char* kSliderXt = R"X(<xt><head>
  <symbol name="Slider"><div id="Body" port:value="number in"
      on:pointerdown="set(value, pointer.fraction(#Track).x)"
      on:pointermove="if active set(value, pointer.fraction(#Track).x)"
      on:pointercancel="set(value, 0.5)">
    <div id="Track"></div><div id="Thumb"></div></div></symbol>
  <style>
    #S1 { position: absolute; left: 0px; top: 0px; width: 200px; height: 20px; }
    #S1/Track { position: absolute; left: 20px; top: 7px; width: 100px; height: 6px; }
    #S1/Thumb { position: absolute; left: 0px; top: 0px; width: 10px; height: 20px; }
  </style></head>
  <body id="Root"><use symbol="Slider" id="S1"></use></body></xt>)X";

}  // namespace

TEST_CASE("T-386: a symbol slider sets its port from pointer.fraction(#Track).x while pressed") {
    Loaded m = load(kSliderXt);
    const PortId port = m.doc->lookupPort("S1.value");
    REQUIRE(port != kInvalidPort);
    // Hovering without a press: `if active` is false.
    m.pointer(InputEvent::PointerMove, 95, 10);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.0f));
    // Press at 10 % of the track, drag to 75 %.
    m.pointer(InputEvent::PointerDown, 30, 10);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.1f));
    m.pointer(InputEvent::PointerMove, 95, 10);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.75f));
    // Past the end: clamped.
    m.pointer(InputEvent::PointerMove, 180, 15);
    CHECK(m.doc->getPort(port).f == doctest::Approx(1.0f));
    // Released: moves no longer change it.
    m.pointer(InputEvent::PointerUp, 180, 15);
    m.pointer(InputEvent::PointerMove, 40, 10);
    CHECK(m.doc->getPort(port).f == doctest::Approx(1.0f));
    // on:pointercancel fires for a cancelled press.
    m.pointer(InputEvent::PointerDown, 70, 10);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.5f));
    m.pointer(InputEvent::PointerMove, 120, 10);
    CHECK(m.doc->getPort(port).f == doctest::Approx(1.0f));
    InputEvent cancel;
    cancel.kind = InputEvent::PointerCancel;
    m.send(cancel);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.5f));
}

TEST_CASE("T-386: pointer.local(); pointer.* outside pointer triggers and unknown ids are errors") {
    Loaded m = load(
        doc("#T { position: absolute; left: 10px; top: 10px; width: 50px; height: 20px; }",
            "<div id=\"T\" port:x=\"number\" port:y=\"number\""
            " on:press=\"set(x, pointer.local(#T).x); set(y, pointer.local( #T ).y | scale(2))\"></div>"));
    m.pointer(InputEvent::PointerDown, 30, 15);
    CHECK(m.doc->getPort(m.doc->lookupPort("x")).f == doctest::Approx(20.0f));
    CHECK(m.doc->getPort(m.doc->lookupPort("y")).f == doctest::Approx(10.0f));

    CompileResult bad = compileXt(
        doc("", "<div id=\"T\" port:v=\"number\" on:show=\"set(v, pointer.fraction(#T).x)\"></div>"));
    CHECK_FALSE(bad.ok());
    CHECK(hasError(bad, "only valid in pointer triggers"));
    CompileResult unknown = compileXt(
        doc("", "<div id=\"T\" port:v=\"number\" on:click=\"set(v, pointer.fraction(#Nope).x)\"></div>"));
    CHECK_FALSE(unknown.ok());
    CHECK(hasError(unknown, "#Nope"));
    CompileResult malformed = compileXt(
        doc("", "<div id=\"T\" port:v=\"number\" on:click=\"set(v, pointer.fraction(#T).z)\"></div>"));
    CHECK_FALSE(malformed.ok());
    CHECK(hasError(malformed, "pointer.fraction(#<id>)"));
    std::string err;
    CHECK(validateTriggerActions("if active set(v, pointer.fraction(#T).y) else set(v, 0)", err));
    CHECK(validateTriggerEvent("pointermove", err));
    CHECK(validateTriggerEvent("pointercancel", err));
    CHECK(validateTriggerEvent("pointerdown", err));
}

TEST_CASE("T-386: hit-slop grows the hit rectangle only; clip-path elements do not grow") {
    const char* style =
        "#Track { position: absolute; left: 20px; top: 50px; width: 100px; height: 6px; hit-slop: 10px; }"
        " #Plain { position: absolute; left: 20px; top: 100px; width: 100px; height: 6px; }"
        " #Clip { position: absolute; left: 20px; top: 150px; width: 100px; height: 6px; hit-slop: 10px;"
        "   clip-path: inset(0px); }"
        " #Pct { position: absolute; left: 200px; top: 50px; width: 40px; height: 6px;"
        "   hit-slop: 50% 0px 0px; }"
        " #Em { position: absolute; left: 200px; top: 150px; width: 40px; height: 6px; font-size: 10px;"
        "   hit-slop: 0px 1em; }";
    Loaded m = load(doc(style,
                        "<div id=\"Track\"></div><div id=\"Plain\"></div><div id=\"Clip\"></div>"
                        "<div id=\"Pct\"></div><div id=\"Em\"></div>"),
                    300, 200);
    const NodeId track = m.el("Track").node;
    CHECK(m.doc->arena().element(track).box.height == doctest::Approx(6.0f));  // Layout unchanged.
    CHECK(m.doc->hitTest(Vec2{60, 64}) == track);                              // 8 px below.
    CHECK(m.doc->hitTest(Vec2{60, 42}) == track);                              // 8 px above.
    CHECK(m.doc->hitTest(Vec2{12, 52}) == track);                              // 8 px left.
    CHECK(m.doc->hitTest(Vec2{60, 67}) != track);                              // 11 px below.
    CHECK(m.doc->hitTest(Vec2{60, 114}) != m.el("Plain").node);
    CHECK(m.doc->hitTest(Vec2{60, 103}) == m.el("Plain").node);
    CHECK(m.doc->hitTest(Vec2{60, 164}) != m.el("Clip").node);
    CHECK(m.doc->hitTest(Vec2{60, 153}) == m.el("Clip").node);
    // 50 % of the 6 px height, on top only.
    CHECK(m.doc->hitTest(Vec2{210, 48}) == m.el("Pct").node);
    CHECK(m.doc->hitTest(Vec2{210, 46}) != m.el("Pct").node);
    CHECK(m.doc->hitTest(Vec2{210, 58}) != m.el("Pct").node);
    // 1em = 10 px, left and right.
    CHECK(m.doc->hitTest(Vec2{192, 152}) == m.el("Em").node);
    CHECK(m.doc->hitTest(Vec2{248, 152}) == m.el("Em").node);
    CHECK(m.doc->hitTest(Vec2{220, 145}) != m.el("Em").node);
    // A press in the slop presses the element.
    m.pointer(InputEvent::PointerDown, 60, 64);
    CHECK(isActive(m, "Track"));
    m.pointer(InputEvent::PointerUp, 60, 64);

    // Computed value (top, right, bottom, left) and the runtime setStyle path.
    Value v = m.el("Track").computedStyleValue(276);
    REQUIRE(v.type == ValueType::Color);
    CHECK(v.c.r == doctest::Approx(10.0f));
    CHECK(v.c.a == doctest::Approx(10.0f));
    CHECK(m.el("Plain").setStyle("hit-slop: 4px 2px") == Status::kOk);
    m.step(0.0f);
    v = m.el("Plain").computedStyleValue(276);
    CHECK(v.c.r == doctest::Approx(4.0f));
    CHECK(v.c.g == doctest::Approx(2.0f));
    CHECK(v.c.b == doctest::Approx(4.0f));
    CHECK(v.c.a == doctest::Approx(2.0f));
    CHECK(m.doc->hitTest(Vec2{60, 109}) == m.el("Plain").node);
    CHECK(m.doc->hitTest(Vec2{60, 111}) != m.el("Plain").node);
}

// -- T-387 (08.12, 13 §13.9; Aux Duty XF-057/059/061) ------------------------------------------------

TEST_CASE("T-387: pointer, click and wheel events carry InputEvent::modifiers") {
    Loaded m = load(doc("#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }",
                        "<button id=\"A\"></button>"));
    uint8_t enter = 0;
    uint8_t move = 0;
    uint8_t down = 0;
    uint8_t up = 0;
    uint8_t click = 0;
    uint8_t dbl = 0;
    uint8_t wheel = 0;
    uint8_t leave = 0;
    m.doc->addListener(m.el("A"), EventType::PointerEnter, [&](Event& ev) { enter = ev.modifiers; });
    m.doc->addListener(m.el("A"), EventType::PointerMove, [&](Event& ev) { move = ev.modifiers; });
    m.doc->addListener(m.el("A"), EventType::PointerDown, [&](Event& ev) { down = ev.modifiers; });
    m.doc->addListener(m.el("A"), EventType::PointerUp, [&](Event& ev) { up = ev.modifiers; });
    m.doc->addListener(m.el("A"), EventType::Click, [&](Event& ev) { click = ev.modifiers; });
    m.doc->addListener(m.el("Root"), EventType::DoubleClick, [&](Event& ev) { dbl = ev.modifiers; });
    m.doc->addListener(m.el("Root"), EventType::Wheel, [&](Event& ev) { wheel = ev.modifiers; });
    m.doc->addListener(m.el("A"), EventType::PointerLeave, [&](Event& ev) { leave = ev.modifiers; });

    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{10, 10};
    ev.modifiers = kCtrl;
    m.send(ev);
    CHECK(enter == kCtrl);
    CHECK(move == kCtrl);
    ev.kind = InputEvent::PointerDown;
    ev.modifiers = kShift;
    m.send(ev);
    ev.kind = InputEvent::PointerUp;
    m.send(ev);
    CHECK((down & kShift) != 0);
    CHECK((up & kShift) != 0);
    CHECK((click & kShift) != 0);
    // The second press of a double click, with Shift + Super.
    ev.modifiers = static_cast<uint8_t>(kShift | kSuper);
    ev.kind = InputEvent::PointerDown;
    m.send(ev);
    ev.kind = InputEvent::PointerUp;
    m.send(ev);
    CHECK(click == (kShift | kSuper));
    CHECK(dbl == (kShift | kSuper));
    ev.kind = InputEvent::Wheel;
    ev.wheelDelta = Vec2{0, 1};
    ev.modifiers = kAlt;
    m.send(ev);
    CHECK(wheel == kAlt);
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{200, 150};
    ev.modifiers = kShift;
    m.send(ev);
    CHECK(leave == kShift);
    // Without modifiers the events carry none.
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{10, 10};
    ev.modifiers = 0;
    m.send(ev);
    CHECK(down == 0);
    ev.kind = InputEvent::PointerUp;
    m.send(ev);
}

namespace {

constexpr const char* kScrollStyle = R"X(
  #S { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; overflow: scroll; }
  .row { height: 50px; }
  #N { position: absolute; left: 150px; top: 0px; width: 40px; height: 40px; }
)X";

constexpr const char* kScrollBody =
    "<div id=\"S\" on:scroll=\"state(Scrolled)\"><div id=\"R0\" class=\"row\"></div>"
    "<div id=\"R1\" class=\"row\"></div><div id=\"R2\" class=\"row\"></div><div id=\"R3\" "
    "class=\"row\"></div>"
    "<div id=\"R4\" class=\"row\"></div><div id=\"R5\" class=\"row\"></div><div id=\"R6\" "
    "class=\"row\"></div>"
    "<div id=\"R7\" class=\"row\"></div></div><div id=\"N\"></div>";

}  // namespace

TEST_CASE("T-387: ElementRef scrollTo / scrollBy / maxScroll clamp and dispatch one Scroll") {
    Loaded m = load(doc(kScrollStyle, kScrollBody));
    ElementRef s = m.el("S");
    int scrolls = 0;
    int rootScrolls = 0;
    Vec2 last{-1, -1};
    m.doc->addListener(s, EventType::Scroll, [&](Event& ev) {
        ++scrolls;
        CHECK(ev.target == s.node);
        last = ev.scrollPos;
    });
    m.doc->addListener(m.el("Root"), EventType::Scroll, [&](Event&) { ++rootScrolls; });

    CHECK(s.maxScroll().x == doctest::Approx(0.0f));
    CHECK(s.maxScroll().y == doctest::Approx(300.0f));
    CHECK(s.scrollPos().y == doctest::Approx(0.0f));
    CHECK_FALSE(m.has("S", "Scrolled"));

    CHECK(s.scrollTo(30, 1000) == Status::kOk);
    CHECK(scrolls == 1);
    CHECK(rootScrolls == 1);                          // Bubbles.
    CHECK(s.scrollPos().x == doctest::Approx(0.0f));  // No horizontal overflow: clamped to 0.
    CHECK(s.scrollPos().y == doctest::Approx(300.0f));
    CHECK(last.y == doctest::Approx(300.0f));
    m.step(0.0f);
    CHECK(m.has("S", "Scrolled"));  // on:scroll.

    // An unchanged offset dispatches nothing.
    CHECK(s.scrollTo(0, 400) == Status::kOk);
    CHECK(scrolls == 1);
    CHECK(s.scrollBy(0, -100) == Status::kOk);
    CHECK(scrolls == 2);
    CHECK(s.scrollPos().y == doctest::Approx(200.0f));
    CHECK(last.y == doctest::Approx(200.0f));
    CHECK(s.scrollTo(0, -5) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(0.0f));
    CHECK(scrolls == 3);

    // Not a scroll container / invalid refs.
    CHECK(m.el("N").scrollTo(0, 10) == Status::kUnsupported);
    CHECK(m.el("N").scrollBy(0, 10) == Status::kUnsupported);
    CHECK(m.el("N").scrollIntoView(m.el("R0")) == Status::kUnsupported);
    CHECK(m.el("N").maxScroll().y == doctest::Approx(0.0f));
    CHECK(ElementRef{}.scrollTo(0, 0) == Status::kInvalidArgument);
    CHECK(s.scrollIntoView(m.el("N")) == Status::kInvalidArgument);  // Not a descendant.
    CHECK(scrolls == 3);
}

TEST_CASE("T-387: the wheel scrolls through the same path and dispatches Scroll") {
    Loaded m = load(doc(kScrollStyle, kScrollBody));
    ElementRef s = m.el("S");
    int scrolls = 0;
    Vec2 last{};
    uint8_t mods = 0;
    m.doc->addListener(s, EventType::Scroll, [&](Event& ev) {
        ++scrolls;
        last = ev.scrollPos;
        mods = ev.modifiers;
    });
    InputEvent wheel;
    wheel.kind = InputEvent::Wheel;
    wheel.pos = Vec2{50, 50};
    wheel.wheelDelta = Vec2{0, 1};
    wheel.modifiers = kShift;
    m.send(wheel);
    CHECK(scrolls == 1);
    CHECK(last.y == doctest::Approx(40.0f));
    CHECK(mods == kShift);
    CHECK(s.scrollPos().y == doctest::Approx(40.0f));
    CHECK(m.has("S", "Scrolled"));
    // A wheel that cannot move the offset (back at the top) dispatches nothing.
    wheel.wheelDelta = Vec2{0, -1};
    m.send(wheel);
    CHECK(scrolls == 2);
    CHECK(s.scrollPos().y == doctest::Approx(0.0f));
    m.send(wheel);
    CHECK(scrolls == 2);
}

TEST_CASE("T-387: scrollIntoView Start / Center / End / Nearest") {
    Loaded m = load(doc(kScrollStyle, kScrollBody));
    ElementRef s = m.el("S");
    // R5 spans 250..300 of the 400 px content; the visible area is 100 px tall.
    CHECK(s.scrollIntoView(m.el("R5"), ScrollAlign::Center) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(225.0f));
    CHECK(s.scrollIntoView(m.el("R5"), ScrollAlign::Start) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(250.0f));
    CHECK(s.scrollIntoView(m.el("R5"), ScrollAlign::End) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(200.0f));
    // Nearest: above the window -> its top at the top; below -> its bottom at the bottom; already
    // visible -> unchanged.
    CHECK(s.scrollIntoView(m.el("R1")) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(50.0f));
    CHECK(s.scrollIntoView(m.el("R2")) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(50.0f));
    CHECK(s.scrollIntoView(m.el("R7")) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(300.0f));
    // Clamped like scrollTo.
    CHECK(s.scrollIntoView(m.el("R7"), ScrollAlign::Start) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(300.0f));
}

namespace {

constexpr const char* kPopStyle = R"X(
  #Btn { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
  #Clip { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px; overflow: hidden; }
  #Pop { position: absolute; left: 20px; top: 20px; width: 60px; height: 60px; layer: overlay; }
  #PopInner { position: absolute; left: 10px; top: 10px; width: 20px; height: 20px; }
  #Other { position: absolute; left: 200px; top: 0px; width: 40px; height: 40px; }
)X";

void press(Loaded& m, float x, float y) {
    m.pointer(InputEvent::PointerDown, x, y);
    m.pointer(InputEvent::PointerUp, x, y);
}

}  // namespace

TEST_CASE("T-387: OutsidePress / on:outsidepress dismiss a visible popover, not a press inside it") {
    Loaded m = load(doc(kPopStyle,
                        "<button id=\"Btn\"></button><div id=\"Clip\"><div id=\"Pop\" "
                        "on:outsidepress=\"state(Out); hide()\"><div id=\"PopInner\"></div></div></div>"
                        "<div id=\"Other\"></div>"));
    int pops = 0;
    Vec2 popPos{};
    m.doc->addListener(m.el("Pop"), EventType::OutsidePress, [&](Event& ev) {
        ++pops;
        CHECK(ev.target == m.el("Pop").node);
        popPos = ev.pos;
    });
    // A listener alone registers an element too.
    int others = 0;
    m.doc->addListener(m.el("Other"), EventType::OutsidePress, [&](Event&) { ++others; });

    // Inside Pop's subtree: its child, and Pop itself outside #Clip's clip (layer: overlay).
    press(m, 90, 40);
    CHECK(m.doc->hitTest(Vec2{90, 40}) == m.el("PopInner").node);
    press(m, 120, 70);
    CHECK(m.doc->hitTest(Vec2{120, 70}) == m.el("Pop").node);
    CHECK(pops == 0);
    CHECK(others == 2);
    CHECK_FALSE(m.has("Pop", "Out"));

    // Outside: on the button.
    press(m, 10, 10);
    CHECK(pops == 1);
    CHECK(popPos.x == doctest::Approx(10.0f));
    CHECK(m.has("Pop", "Out"));
    CHECK(others == 3);
    // Inside Other: Other does not get it.
    press(m, 210, 10);
    CHECK(others == 3);
    // hide() made Pop invisible: no more OutsidePress for it.
    m.step(0.0f);
    press(m, 10, 10);
    press(m, 250, 150);
    CHECK(pops == 1);
    CHECK(others == 5);
}

TEST_CASE("T-387: on:scroll and on:outsidepress are trigger names") {
    std::string err;
    CHECK(validateTriggerEvent("scroll", err));
    CHECK(validateTriggerEvent("outsidepress", err));
}

// -- T-390 (08.13, 27.3.2): trigger button filters, if conditions, Document::cancelPointer, the
// disabled subtree --------------------------------------------------------------------------------

namespace {

void click390(Loaded& m, float x, float y, PointerButton button) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{x, y};
    ev.button = button;
    m.send(ev);
    ev.kind = InputEvent::PointerUp;
    m.send(ev);
}

}  // namespace

TEST_CASE("T-390: press(right) / click(left) filter on the button; if button() reads it") {
    Loaded m = load(doc("#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }",
                        "<button id=\"A\" on:press(right)=\"state(Right)\" on:press=\"state(Any, toggle)\" "
                        "on:click(left)=\"state(LeftClick)\" "
                        "on:release=\"if button(left) state(LeftUp) else state(OtherUp)\"></button>"));
    click390(m, 10, 10, PointerButton::Left);
    CHECK_FALSE(m.has("A", "Right"));
    CHECK(m.has("A", "Any"));
    CHECK(m.has("A", "LeftClick"));
    CHECK(m.has("A", "LeftUp"));
    CHECK_FALSE(m.has("A", "OtherUp"));
    REQUIRE(m.doc->setCustomState(m.el("A"), "LeftClick", false) == Status::kOk);
    REQUIRE(m.doc->setCustomState(m.el("A"), "LeftUp", false) == Status::kOk);
    click390(m, 10, 10, PointerButton::Right);
    CHECK(m.has("A", "Right"));
    CHECK_FALSE(m.has("A", "Any"));  // Toggled back: the unfiltered trigger fires for any button.
    CHECK_FALSE(m.has("A", "LeftClick"));
    CHECK_FALSE(m.has("A", "LeftUp"));
    CHECK(m.has("A", "OtherUp"));
}

TEST_CASE("T-390: pointermove(left) needs the left button held") {
    Loaded m = load(doc("#A { position: absolute; left: 0px; top: 0px; width: 100px; height: 40px; }",
                        "<div id=\"A\" on:pointermove(left)=\"state(Dragged)\"></div>"));
    m.pointer(InputEvent::PointerMove, 10, 10);
    m.pointer(InputEvent::PointerMove, 20, 10);
    CHECK_FALSE(m.has("A", "Dragged"));
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{20, 10};
    down.button = PointerButton::Right;
    m.send(down);
    m.pointer(InputEvent::PointerMove, 30, 10);
    CHECK_FALSE(m.has("A", "Dragged"));  // The right button is held.
    m.pointer(InputEvent::PointerUp, 30, 10);
    m.pointer(InputEvent::PointerDown, 30, 10);
    m.pointer(InputEvent::PointerMove, 40, 10);
    CHECK(m.has("A", "Dragged"));
}

TEST_CASE("T-390: if state(), if port() <op> <value>, if not ... and ...") {
    Loaded m = load(doc("#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }",
                        "<div id=\"P\" port:value=\"number in\" port:x=\"number in\"></div>"
                        "<button id=\"A\" on:click=\"if state(Open) state(SawOpen) else state(SawClosed); "
                        "if port(value) gt 0.5 state(Big); "
                        "if not state(Open) and port(x) eq 1 state(Hit); "
                        "if state(Hover) and not button(right) state(Hovered)\"></button>"));
    m.pointer(InputEvent::PointerMove, 10, 10);  // Hover comes from a move.
    click390(m, 10, 10, PointerButton::Left);
    CHECK(m.has("A", "SawClosed"));
    CHECK_FALSE(m.has("A", "SawOpen"));
    CHECK_FALSE(m.has("A", "Big"));
    CHECK_FALSE(m.has("A", "Hit"));
    CHECK(m.has("A", "Hovered"));  // A built-in state.
    REQUIRE(m.doc->setPort(m.doc->lookupPort("value"), Value::ofFloat(0.75f)) == Status::kOk);
    REQUIRE(m.doc->setPort(m.doc->lookupPort("x"), Value::ofFloat(1.0f)) == Status::kOk);
    click390(m, 10, 10, PointerButton::Left);
    CHECK(m.has("A", "Big"));
    CHECK(m.has("A", "Hit"));
    REQUIRE(m.doc->setCustomState(m.el("A"), "Hit", false) == Status::kOk);
    REQUIRE(m.doc->setCustomState(m.el("A"), "Open", true) == Status::kOk);
    click390(m, 10, 10, PointerButton::Left);
    CHECK(m.has("A", "SawOpen"));
    CHECK_FALSE(m.has("A", "Hit"));  // `not state(Open)` fails now.
}

TEST_CASE("T-390: if button() outside a pointer trigger, bad button names and port terms are errors") {
    CHECK(hasError(compileXt(doc("", "<button id=\"A\" on:focus=\"if button(left) state(X)\"></button>")),
                   "if button() is only valid in pointer triggers"));
    CHECK(hasError(compileXt(doc("", "<button id=\"A\" on:press(top)=\"state(X)\"></button>")),
                   "expected press(left|middle|right)"));
    CHECK(hasError(compileXt(doc("", "<button id=\"A\" on:click=\"if button(up) state(X)\"></button>")),
                   "expected button(left|middle|right)"));
    CHECK(hasError(compileXt(doc("", "<button id=\"A\" on:click=\"if port(nope) eq 1 state(X)\"></button>")),
                   "if port(nope)"));
}

TEST_CASE("T-390: Document::cancelPointer cancels the press like the queued Context cancel") {
    Loaded m = load(doc("#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }",
                        "<button id=\"A\" on:pointercancel=\"state(Cancelled)\"></button>"));
    int clicks = 0;
    m.doc->addListener(m.el("A"), EventType::Click, [&](Event&) { ++clicks; });
    m.doc->cancelPointer();  // Nothing pressed: no-op.
    CHECK_FALSE(m.has("A", "Cancelled"));
    m.pointer(InputEvent::PointerDown, 10, 10);
    CHECK(isActive(m, "A"));
    m.doc->cancelPointer();
    CHECK(m.has("A", "Cancelled"));
    CHECK_FALSE(isActive(m, "A"));
    m.pointer(InputEvent::PointerUp, 10, 10);
    CHECK(clicks == 0);
}

TEST_CASE("T-390: a disabled ancestor disables the subtree for pointer, click and focus") {
    Loaded m =
        load(doc("#Box { position: absolute; left: 0px; top: 0px; width: 100px; height: 40px; }"
                 " #Track { width: 40px; height: 40px; } #Track:disabled { opacity: 0.5; }",
                 "<div id=\"P\" port:off=\"bool in\"></div><div id=\"Box\" bind:disabled=\"off\">"
                 "<button id=\"Track\" on:press=\"state(Pressed, toggle)\"></button></div>"));
    int downs = 0;
    int clicks = 0;
    m.doc->addListener(m.el("Track"), EventType::PointerDown, [&](Event&) { ++downs; });
    m.doc->addListener(m.el("Track"), EventType::Click, [&](Event&) { ++clicks; });
    REQUIRE(m.doc->setPort(m.doc->lookupPort("off"), Value::ofFloat(1.0f)) == Status::kOk);
    m.step(0.016f);
    CHECK(m.el("Box").hasState(kStateDisabled));
    CHECK_FALSE(m.el("Track").hasState(kStateDisabled));  // `:disabled` only on Box.
    CHECK(m.el("Track").property(prop::Opacity).f == doctest::Approx(1.0f));
    click390(m, 10, 10, PointerButton::Left);
    CHECK(downs == 0);
    CHECK(clicks == 0);
    CHECK_FALSE(m.has("Track", "Pressed"));
    CHECK(m.focusId() != "Track");
    REQUIRE(m.doc->setPort(m.doc->lookupPort("off"), Value::ofFloat(0.0f)) == Status::kOk);
    m.step(0.016f);
    click390(m, 10, 10, PointerButton::Left);
    CHECK(downs == 1);
    CHECK(clicks == 1);
    CHECK(m.has("Track", "Pressed"));
    CHECK(m.focusId() == "Track");
}

// -- T-389 (27 §27.4.1, 2026-10-21 ruling (c)/(d)) ----------------------------------------------------

TEST_CASE("T-389: 100 pointer moves in one frame run on:pointermove once, with the last event") {
    Loaded m = load(doc("#A { position: absolute; left: 0px; top: 0px; width: 200px; height: 100px; }",
                        "<div id=\"A\" port:x=\"number\" port:hit=\"event out\""
                        " on:pointermove=\"set(x, pointer.local(#A).x)\" on:click=\"emit(hit)\"></div>"),
                    300, 200, kBehaviorFlowSignalCoalescing);
    const PortId x = m.doc->lookupPort("x");
    const PortId hit = m.doc->lookupPort("hit");
    REQUIRE(x != kInvalidPort);
    REQUIRE(hit != kInvalidPort);
    int sets = 0;
    int hits = 0;
    m.doc->addPortListener([&](const PortEvent& ev) {
        sets += ev.port == x ? 1 : 0;
        hits += ev.port == hit ? 1 : 0;
    });
    for (int i = 0; i < 100; ++i) {
        InputEvent ev;
        ev.kind = InputEvent::PointerMove;
        ev.pos = Vec2{static_cast<float>(i), 50.0f};
        m.doc->dispatchInput(ev);
    }
    CHECK(sets == 0);  // Held until the frame's update().
    m.step(0.016f);
    CHECK(sets == 1);
    CHECK(m.doc->getPort(x).f == doctest::Approx(99.0f));

    // Discrete events stay one run per event: three clicks, three emits.
    for (int i = 0; i < 3; ++i) {
        InputEvent ev;
        ev.kind = InputEvent::PointerDown;
        ev.pos = Vec2{150.0f, 50.0f};
        m.doc->dispatchInput(ev);
        ev.kind = InputEvent::PointerUp;
        m.doc->dispatchInput(ev);
    }
    CHECK(hits == 3);
}

TEST_CASE("T-389: a press runs the pointer moves held before it first") {
    Loaded m = load(kSliderXt, 300, 200, kBehaviorFlowSignalCoalescing);
    const PortId port = m.doc->lookupPort("S1.value");
    REQUIRE(port != kInvalidPort);
    // Press, drag to 75 % and release, all inside one frame: the move still ran while pressed.
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{30, 10};
    m.doc->dispatchInput(ev);
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{60, 10};
    m.doc->dispatchInput(ev);
    ev.pos = Vec2{95, 10};
    m.doc->dispatchInput(ev);
    ev.kind = InputEvent::PointerUp;
    m.doc->dispatchInput(ev);
    m.step(0.016f);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.75f));
}

TEST_CASE("T-389: the same setStyle text again is a no-op; a text another element used is not re-parsed") {
    Loaded m = load(doc("#A, #B { width: 10px; height: 10px; }", "<div id=\"A\"></div><div id=\"B\"></div>"));
    ElementRef a = m.doc->byId("A");
    ElementRef b = m.doc->byId("B");
    const std::string text =
        "opacity: 0.25; effect: gradient-mask(radial, 1px 2px, 3px 2px, #000000ff 0%, #00000000 100%)";
    const uint64_t before = mini_css::parseDeclarationsCount();
    CHECK(a.setStyle(text) == Status::kOk);
    CHECK(mini_css::parseDeclarationsCount() == before + 1);
    CHECK(a.setStyle(text) == Status::kOk);  // Same element, same text: nothing to do.
    CHECK(b.setStyle(text) == Status::kOk);  // Another element: the cached parse.
    CHECK(mini_css::parseDeclarationsCount() == before + 1);
    m.step(0.016f);
    CHECK(a.property(prop::Opacity).f == doctest::Approx(0.25f));
    CHECK(b.property(prop::Opacity).f == doctest::Approx(0.25f));
    // A different text, then the first one again: applied again (no stale no-op).
    CHECK(a.setStyle("opacity: 0.5") == Status::kOk);
    m.step(0.016f);
    CHECK(a.property(prop::Opacity).f == doctest::Approx(0.5f));
    CHECK(a.setStyle(text) == Status::kOk);
    m.step(0.016f);
    CHECK(a.property(prop::Opacity).f == doctest::Approx(0.25f));
    // The status of a text with an invalid declaration comes back with the no-op and the cache.
    CHECK(a.setStyle("opacity: 0.5; bogus-prop: 1") == Status::kInvalidArgument);
    CHECK(a.setStyle("opacity: 0.5; bogus-prop: 1") == Status::kInvalidArgument);
    CHECK(b.setStyle("opacity: 0.5; bogus-prop: 1") == Status::kInvalidArgument);
}

// -- T-393 (08.14): external file drop ------------------------------------------------------------

namespace {

constexpr const char* kExternalBody =
    "<div id=\"Src\" draggable></div>"
    "<div id=\"Dst\" drop-target on:drag.over=\"if external state(ExtOver)\" "
    "on:drop=\"if external state(ExtDrop) else state(IntDrop)\" "
    "on:drag.leave=\"state(Left)\"></div>";

void external(Loaded& l, InputEvent::Kind kind, float x, float y,
              std::span<const std::string_view> paths = {}) {
    InputEvent ev;
    ev.kind = kind;
    ev.pos = Vec2{x, y};
    ev.paths = paths;
    l.send(ev);
}

}  // namespace

TEST_CASE("T-393: an external drag over a drop target: DragOver with dropFraction, DropHover, DragLeave") {
    Loaded l = load(doc(kDragStyle, kExternalBody));
    std::vector<std::string> events;
    std::vector<Vec2> fractions;
    l.doc->addListener(l.el("Dst"), EventType::DragOver, [&](Event& e) {
        events.push_back("over");
        fractions.push_back(e.dropFraction);
        CHECK(e.dragSource == kInvalidNode);
        CHECK(e.dropTarget == l.el("Dst").node);
        CHECK(e.externalPaths.empty());
    });
    l.doc->addListener(l.el("Dst"), EventType::DragLeave, [&](Event& e) {
        events.push_back("leave");
        CHECK(e.dragSource == kInvalidNode);
    });
    l.doc->addListener(l.el("Src"), EventType::DragCancel, [&](Event&) { events.push_back("cancel"); });
    l.doc->addListener(l.el("Src"), EventType::DragEnd, [&](Event&) { events.push_back("end"); });

    external(l, InputEvent::ExternalDragEnter, 250, 150);  // Over nothing.
    CHECK(events.empty());
    CHECK_FALSE(l.doc->dragSource().valid());
    CHECK(l.doc->hasDropTargetAt(Vec2{120, 10}));
    CHECK_FALSE(l.doc->hasDropTargetAt(Vec2{250, 150}));
    external(l, InputEvent::ExternalDragMove, 120, 10);
    external(l, InputEvent::ExternalDragMove, 130, 50);
    REQUIRE(events.size() == 2);
    CHECK(fractions[0].y == doctest::Approx(10.0f / 60.0f));
    CHECK(fractions[1].y == doctest::Approx(50.0f / 60.0f));
    CHECK(l.doc->dragOver().node == l.el("Dst").node);
    CHECK(l.has("Dst", "DropHover"));
    CHECK(l.has("Dst", "ExtOver"));  // `if external` holds.
    CHECK(bg(l, "Dst").g == doctest::Approx(1.0f));

    external(l, InputEvent::ExternalDragMove, 250, 150);  // Moved off: DragLeave.
    REQUIRE(events.size() == 3);
    CHECK(events[2] == "leave");
    CHECK_FALSE(l.has("Dst", "DropHover"));
    external(l, InputEvent::ExternalDragMove, 120, 10);
    external(l, InputEvent::ExternalDragLeave, 0, 0);  // Left the window: DragLeave, nothing else.
    REQUIRE(events.size() == 5);
    CHECK(events[3] == "over");
    CHECK(events[4] == "leave");
    CHECK_FALSE(l.has("Dst", "DropHover"));
    CHECK_FALSE(l.doc->dragOver().valid());
}

TEST_CASE("T-399: an external drag's DragLeave: leftWindow on ExternalDragLeave, not on a target change") {
    Loaded l = load(doc(kDragStyle, kExternalBody));
    std::vector<bool> left;
    l.doc->addListener(l.el("Dst"), EventType::DragLeave, [&](Event& e) { left.push_back(e.leftWindow); });
    l.doc->addListener(l.el("Dst"), EventType::DragOver, [&](Event& e) { CHECK_FALSE(e.leftWindow); });
    external(l, InputEvent::ExternalDragEnter, 120, 10);
    external(l, InputEvent::ExternalDragMove, 250, 150);  // Off the target, inside the window.
    REQUIRE(left.size() == 1);
    CHECK_FALSE(left[0]);
    external(l, InputEvent::ExternalDragMove, 120, 10);
    external(l, InputEvent::ExternalDragLeave, 0, 0);  // Left the window.
    REQUIRE(left.size() == 2);
    CHECK(left[1]);
}

TEST_CASE("T-393: ExternalDrop with two paths: Drop externalPaths, HostEvent::Drop paths, if external") {
    Loaded l = load(doc(kDragStyle, kExternalBody));
    std::vector<std::string> dropped;
    Vec2 fraction{-1, -1};
    l.doc->addListener(l.el("Dst"), EventType::Drop, [&](Event& e) {
        for (const std::string& p : e.externalPaths) {
            dropped.push_back(p);
        }
        fraction = e.dropFraction;
        CHECK(e.dragSource == kInvalidNode);
    });
    std::vector<std::string> hostPaths;
    int hostDrops = 0;
    l.doc->setHostEventListener([&](const HostEvent& e) {
        if (e.kind != HostEventKind::Drop) {
            return;
        }
        ++hostDrops;
        CHECK(e.element == kInvalidNode);
        CHECK(e.targetId == "Dst");
        for (const std::string& p : e.paths) {
            hostPaths.push_back(p);
        }
    });
    {
        // The host's strings die right after the dispatch: the core copies what it keeps.
        const std::string a = "C:/data/first file.png";
        const std::string b = "C:/data/second.txt";
        const std::string_view views[] = {a, b};
        external(l, InputEvent::ExternalDragEnter, 120, 10);
        external(l, InputEvent::ExternalDrop, 120, 30, views);
    }
    REQUIRE(dropped.size() == 2);
    CHECK(dropped[0] == "C:/data/first file.png");
    CHECK(dropped[1] == "C:/data/second.txt");
    CHECK(fraction.y == doctest::Approx(0.5f));
    REQUIRE(hostDrops == 1);
    REQUIRE(hostPaths.size() == 2);
    CHECK(hostPaths[1] == "C:/data/second.txt");
    CHECK(l.has("Dst", "ExtDrop"));
    CHECK_FALSE(l.has("Dst", "IntDrop"));
    CHECK_FALSE(l.has("Dst", "DropHover"));
    CHECK_FALSE(l.doc->dragOver().valid());

    // An internal drag's drop: `if external` is false, no paths.
    Loaded m = load(doc(kDragStyle, kExternalBody));
    size_t internalPaths = 99;
    m.doc->addListener(m.el("Dst"), EventType::Drop,
                       [&](Event& e) { internalPaths = e.externalPaths.size(); });
    std::vector<size_t> hostInternal;
    m.doc->setHostEventListener([&](const HostEvent& e) { hostInternal.push_back(e.paths.size()); });
    dragIntoDst(m, 10);
    m.pointer(InputEvent::PointerUp, 120, 10);
    CHECK(internalPaths == 0);
    REQUIRE(hostInternal.size() == 1);
    CHECK(hostInternal[0] == 0);
    CHECK(m.has("Dst", "IntDrop"));
    CHECK_FALSE(m.has("Dst", "ExtDrop"));
    CHECK_FALSE(m.has("Dst", "ExtOver"));
}

TEST_CASE("T-393: a drop with no hover before it (the SDL fallback) sends Drop alone; off-target nothing") {
    Loaded l = load(doc(kDragStyle, kExternalBody));
    std::vector<std::string> events;
    l.doc->addListener(l.el("Dst"), EventType::DragOver, [&](Event&) { events.push_back("over"); });
    l.doc->addListener(l.el("Dst"), EventType::DragLeave, [&](Event&) { events.push_back("leave"); });
    l.doc->addListener(l.el("Dst"), EventType::Drop,
                       [&](Event& e) { events.push_back("drop" + std::to_string(e.externalPaths.size())); });
    const std::string_view one[] = {"/tmp/a.png"};
    external(l, InputEvent::ExternalDrop, 250, 150, one);  // Off every target.
    CHECK(events.empty());
    CHECK_FALSE(l.doc->dragOver().valid());
    external(l, InputEvent::ExternalDrop, 120, 10, one);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == "drop1");
    CHECK(l.has("Dst", "ExtDrop"));
    CHECK_FALSE(l.has("Dst", "ExtOver"));
}

TEST_CASE("T-393: Context copies the ExternalDrop paths and routes the external drag to the surface") {
    CompileResult ir = compileXt(doc(kDragStyle, kExternalBody));
    REQUIRE(ir.ok());
    Context ctx{ContextConfig{}};
    const DocumentHandle h = ctx.loadDocument(IrBlob{ir.ir.data(), ir.ir.size()}, false).value();
    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    ctx.update(0);
    Document* d = ctx.document(h);
    std::vector<std::string> paths;
    d->addListener(d->byId("Dst"), EventType::Drop,
                   [&](Event& e) { paths.assign(e.externalPaths.begin(), e.externalPaths.end()); });
    struct Listener : ContextListener {
        size_t hostPaths = 0;
        void onHostEvent(const HostEvent& e) override {
            if (e.kind == HostEventKind::Drop) {
                hostPaths = e.paths.size();
            }
        }
    } listener;
    ctx.addListener(&listener);
    InputEvent enter;
    enter.kind = InputEvent::ExternalDragEnter;
    enter.pos = Vec2{120, 10};
    ctx.pushInput(enter);
    ctx.update(0);
    CHECK(d->hasCustomState(d->byId("Dst"), "DropHover"));
    {
        std::string a = "D:/one.xt";
        std::string b = "D:/two.xt";
        const std::string_view views[] = {a, b};
        InputEvent drop;
        drop.kind = InputEvent::ExternalDrop;
        drop.pos = Vec2{120, 10};
        drop.paths = views;
        ctx.pushInput(drop);
        a.assign("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");  // The host reuses its buffers.
        b.clear();
    }
    ctx.update(0);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "D:/one.xt");
    CHECK(paths[1] == "D:/two.xt");
    CHECK(listener.hostPaths == 2);
    CHECK_FALSE(d->hasCustomState(d->byId("Dst"), "DropHover"));
    // Enter, then leave the window: DragLeave clears the state.
    ctx.pushInput(enter);
    ctx.update(0);
    CHECK(d->hasCustomState(d->byId("Dst"), "DropHover"));
    InputEvent leave;
    leave.kind = InputEvent::ExternalDragLeave;
    ctx.pushInput(leave);
    ctx.update(0);
    CHECK_FALSE(d->hasCustomState(d->byId("Dst"), "DropHover"));
    CHECK(d->hasCustomState(d->byId("Dst"), "Left"));
    ctx.removeListener(&listener);
}

// -- T-394 (08.15): drag round 2, OutsidePress consume, on:pointerdrag, scrollIntoView(repeat) --

namespace {

/// A plain (not virtualized) list of 20 px rows from y = 0 and a separate drop target.
std::string rowList(std::string_view rowAttrs = "draggable drop-target") {
    return "<div id=\"P\" port:items=\"list&lt;struct{name: string}&gt;\"></div>"
           "<div id=\"List\" style=\"position: absolute; left: 0px; top: 0px; width: 200px\">"
           "<repeat port=\"items\" id=\"Rows\">"
           "<div id=\"Row\" " +
           std::string(rowAttrs) +
           " style=\"height: 20px\"></div>"
           "</repeat></div>"
           "<div id=\"Other\" drop-target style=\"position: absolute; left: 250px; top: 0px; width: 40px; "
           "height: 40px\"></div>";
}

struct DropLog {
    std::vector<uint32_t> overIndex;
    std::vector<uint32_t> overItem;
    uint32_t dropIndex = 0;
    int drops = 0;
    void attach(Loaded& l) {
        l.doc->addListener(l.el("Root"), EventType::DragOver, [this](Event& e) {
            overIndex.push_back(e.dropIndex);
            overItem.push_back(e.targetItem);
        });
        l.doc->addListener(l.el("Root"), EventType::Drop, [this](Event& e) {
            dropIndex = e.dropIndex;
            ++drops;
        });
    }
};

void pointerButton(Loaded& l, InputEvent::Kind kind, float x, float y, PointerButton button) {
    InputEvent ev;
    ev.kind = kind;
    ev.pos = Vec2{x, y};
    ev.button = button;
    l.send(ev);
}

}  // namespace

TEST_CASE("T-394: dropIndex -- upper / lower half, the source row, a non-list target") {
    Loaded l = load(doc("", rowList()));
    REQUIRE(l.doc->setPortText(l.doc->lookupPort("items"), manyRows(5)) == Status::kOk);
    l.step(0.016f);
    DropLog log;
    log.attach(l);
    l.pointer(InputEvent::PointerMove, 100, 10);
    l.pointer(InputEvent::PointerDown, 100, 10);  // Row 0.
    l.pointer(InputEvent::PointerMove, 100, 25);  // Row 1, upper half: insert before it.
    REQUIRE(l.doc->dragSource().valid());
    REQUIRE(!log.overIndex.empty());
    CHECK(log.overItem.back() == 1u);
    CHECK(log.overIndex.back() == 1u);
    l.pointer(InputEvent::PointerMove, 100, 55);  // Row 2, lower half: insert after it.
    CHECK(log.overItem.back() == 2u);
    CHECK(log.overIndex.back() == 3u);
    l.pointer(InputEvent::PointerMove, 100, 15);  // The source row itself, lower half.
    CHECK(l.doc->dragOver().node == l.doc->dragSource().node);
    CHECK(log.overItem.back() == 0u);
    CHECK(log.overIndex.back() == 1u);
    l.pointer(InputEvent::PointerMove, 100, 3);  // Its upper half.
    CHECK(log.overIndex.back() == 0u);
    l.pointer(InputEvent::PointerMove, 260, 10);  // Not a list.
    CHECK(log.overItem.back() == 0xFFFFFFFFu);
    CHECK(log.overIndex.back() == 0xFFFFFFFFu);
    l.pointer(InputEvent::PointerMove, 100, 75);  // Row 3, lower half.
    l.pointer(InputEvent::PointerUp, 100, 75);
    CHECK(log.drops == 1);
    CHECK(log.dropIndex == 4u);
}

TEST_CASE("T-394: Escape before the threshold cancels the press; other buttons are swallowed") {
    Loaded l = load(doc(kDragStyle, kReorderBody));
    std::vector<std::string> events;
    auto record = [&events](const char* name) { return [&events, name](Event&) { events.push_back(name); }; };
    l.doc->addListener(l.el("Root"), EventType::PointerDown, record("down"));
    l.doc->addListener(l.el("Root"), EventType::PointerUp, record("up"));
    l.doc->addListener(l.el("Root"), EventType::Click, record("click"));
    l.doc->addListener(l.el("Root"), EventType::KeyDown, record("key"));
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 12, 10);  // Under the 4 px threshold.
    l.key(KeyCode::Escape);
    pointerButton(l, InputEvent::PointerDown, 12, 10, PointerButton::Right);
    pointerButton(l, InputEvent::PointerUp, 12, 10, PointerButton::Right);
    l.pointer(InputEvent::PointerMove, 30, 10);  // Would be past the threshold: no drag now.
    CHECK_FALSE(l.doc->dragSource().valid());
    l.pointer(InputEvent::PointerUp, 10, 10);
    REQUIRE(events.size() == 2);
    CHECK(events[0] == "down");
    CHECK(events[1] == "up");  // The left release: no Click; Escape and the right button: nothing.
    // Everything is up: the next press is a normal one.
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerUp, 10, 10);
    CHECK(events.back() == "click");

    // After a drag cancel (Escape) the other buttons are swallowed too.
    events.clear();
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 120, 10);
    REQUIRE(l.doc->dragSource().valid());
    l.key(KeyCode::Escape);
    CHECK_FALSE(l.doc->dragSource().valid());
    pointerButton(l, InputEvent::PointerDown, 120, 10, PointerButton::Right);
    pointerButton(l, InputEvent::PointerUp, 120, 10, PointerButton::Right);
    l.pointer(InputEvent::PointerUp, 10, 10);
    REQUIRE(events.size() == 2);
    CHECK(events[1] == "up");
}

TEST_CASE("T-394: only the primary button drags, unless draggable=\"any-button\"") {
    {
        Loaded l = load(doc(kDragStyle, kReorderBody));
        pointerButton(l, InputEvent::PointerDown, 10, 10, PointerButton::Right);
        l.pointer(InputEvent::PointerMove, 60, 10);
        CHECK_FALSE(l.doc->dragSource().valid());
        pointerButton(l, InputEvent::PointerUp, 60, 10, PointerButton::Right);
    }
    {
        Loaded l = load(doc(
            kDragStyle, "<div id=\"Src\" draggable=\"any-button\"></div><div id=\"Dst\" drop-target></div>"));
        pointerButton(l, InputEvent::PointerDown, 10, 10, PointerButton::Right);
        l.pointer(InputEvent::PointerMove, 60, 10);
        CHECK(l.doc->dragSource().valid());
        pointerButton(l, InputEvent::PointerUp, 60, 10, PointerButton::Right);
        CHECK_FALSE(l.doc->dragSource().valid());
    }
    CompileResult bad = compileXt(doc("", "<div id=\"A\" draggable=\"sometimes\"></div>"));
    CHECK(hasError(bad, "draggable must be true or false"));
}

TEST_CASE("T-394: the edge auto-scroll dispatches DragOver again every frame") {
    Loaded l = load(doc("", reorderList("")));
    REQUIRE(l.doc->setPortText(l.doc->lookupPort("items"), manyRows(200)) == Status::kOk);
    l.step(0.016f);
    std::vector<uint32_t> index;
    l.doc->addListener(l.el("Root"), EventType::DragOver, [&](Event& e) { index.push_back(e.dropIndex); });
    l.pointer(InputEvent::PointerMove, 100, 10);
    l.pointer(InputEvent::PointerDown, 100, 10);
    l.pointer(InputEvent::PointerMove, 100, 196);  // 4 px above the bottom edge: row 9.
    REQUIRE(l.doc->dragSource().valid());
    const size_t before = index.size();
    REQUIRE(before >= 1);
    CHECK(index.back() == 10u);
    l.step(0.016f);  // Scrolls.
    l.step(0.016f);  // DragOver for the new geometry, scrolls again.
    l.step(0.016f);  // And again.
    CHECK(index.size() == before + 2);
    CHECK(index.back() > 10u);
    CHECK(scrollYOf(l, "Scroll") > 0.0f);
}

TEST_CASE("T-394: drag-ghost=\"none\" draws no ghost; bad values are errors") {
    Loaded l =
        load(doc(kDragStyle,
                 "<div id=\"Src\" draggable drag-ghost=\"none\"></div><div id=\"Dst\" drop-target></div>"));
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 60, 20);
    REQUIRE(l.doc->dragSource().valid());
    CommandList list;
    list.viewport = Rect{0, 0, 300, 200};
    l.doc->paint(list);
    for (size_t i = 0; i < list.commands.size(); ++i) {
        if (list.commands[i].kind == CmdKind::PushLayer) {
            CHECK(list.layers[list.commands[i].index].opacity != doctest::Approx(0.6f));
        }
    }
    CompileResult bad = compileXt(doc("", "<div id=\"A\" draggable drag-ghost=\"faded\"></div>"));
    CHECK(hasError(bad, "drag-ghost must be none"));
}

TEST_CASE("T-394: drag attributes on <use> are element attributes, not ports (strict)") {
    const std::string xt =
        "<xt><head><symbol name=\"Card\"><div id=\"C\" port:n=\"number in\"></div></symbol></head>"
        "<body id=\"Root\"><use symbol=\"Card\" id=\"U\" draggable drop-target drag-threshold=\"8\" "
        "drag-autoscroll=\"false\" drag-ghost=\"none\"></use></body></xt>";
    CompileOptions options;
    options.emitMeta = false;
    options.strict = true;
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileResult r = compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(r.ok());
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.message.find("unknown attribute") == std::string::npos);
    }
}

TEST_CASE("T-394: consume() in on:outsidepress takes the press; without it the click goes through") {
    for (bool consume : {true, false}) {
        CAPTURE(consume);
        const std::string pop = consume ? "consume(); hide()" : "hide()";
        Loaded m = load(doc(kPopStyle,
                            "<button id=\"Btn\" on:click=\"state(Clicked)\"></button>"
                            "<div id=\"Pop\" on:outsidepress=\"" +
                                pop + "\"></div>"));
        int downs = 0;
        m.doc->addListener(m.el("Btn"), EventType::PointerDown, [&](Event&) { ++downs; });
        press(m, 10, 10);
        CHECK(m.has("Btn", "Clicked") == !consume);
        CHECK(downs == (consume ? 0 : 1));
        // Pop is hidden now: the next press on the button clicks either way.
        m.step(0.0f);
        press(m, 10, 10);
        CHECK(m.has("Btn", "Clicked"));
    }
    // A listener can take it too.
    Loaded m = load(
        doc(kPopStyle, "<button id=\"Btn\" on:click=\"state(Clicked)\"></button><div id=\"Pop\"></div>"));
    m.doc->addListener(m.el("Pop"), EventType::OutsidePress, [](Event& e) { e.consume = true; });
    press(m, 10, 10);
    CHECK_FALSE(m.has("Btn", "Clicked"));
    // consume() anywhere else is an error.
    CompileResult bad = compileXt(doc("", "<div id=\"A\" on:click=\"consume()\"></div>"));
    CHECK(hasError(bad, "consume() is only valid in outsidepress"));
}

namespace {

constexpr const char* kTrackStyle =
    "#Track { position: absolute; left: 0px; top: 0px; width: 100px; height: 20px; }";

void moveNoFrame(Loaded& l, float x, float y) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{x, y};
    l.doc->dispatchInput(ev);
}

}  // namespace

TEST_CASE("T-394: on:pointerdrag after the threshold, once per frame, if dragging, stops on release") {
    Loaded l = load(doc(kTrackStyle,
                        "<div id=\"Track\" port:v=\"number in\" "
                        "on:pointerdrag=\"set(v, pointer.fraction(#Track).x)\" "
                        "on:pointermove=\"if dragging state(InDrag)\" on:release=\"state(Released)\"></div>"),
                    300, 200, kBehaviorFlowSignalCoalescing);  // T-407: once per frame needs behavior 1.
    const PortId v = l.doc->lookupPort("v");
    int sets = 0;
    l.doc->addPortListener([&](const PortEvent& e) {
        if (e.port == v && e.kind == PortEventKind::Changed) {
            ++sets;
        }
    });
    int drags = 0;
    l.doc->addListener(l.el("Track"), EventType::PointerDrag, [&](Event& e) {
        ++drags;
        CHECK(e.target == l.el("Track").node);
    });
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 13, 10);  // 3 px: not yet.
    CHECK(drags == 0);
    CHECK(sets == 0);
    CHECK_FALSE(l.has("Track", "InDrag"));
    l.pointer(InputEvent::PointerMove, 18, 10);  // 8 px: a pointerdrag.
    CHECK(drags == 1);
    CHECK(sets == 1);
    CHECK(l.doc->getPort(v).f == doctest::Approx(0.18f));
    CHECK(l.has("Track", "InDrag"));
    for (float x : {20.0f, 30.0f, 40.0f, 50.0f, 60.0f}) {
        moveNoFrame(l, x, 10);  // Five moves in one frame.
    }
    l.step(0.016f);
    CHECK(drags == 6);
    CHECK(sets == 2);  // The trigger ran once, with the last move.
    CHECK(l.doc->getPort(v).f == doctest::Approx(0.6f));
    l.pointer(InputEvent::PointerUp, 60, 10);
    CHECK(l.has("Track", "Released"));
    l.pointer(InputEvent::PointerMove, 80, 10);
    CHECK(drags == 6);
    CHECK(sets == 2);

    // PointerCancel ends it as well.
    l.pointer(InputEvent::PointerDown, 10, 10);
    l.pointer(InputEvent::PointerMove, 30, 10);
    CHECK(drags == 7);
    InputEvent cancel;
    cancel.kind = InputEvent::PointerCancel;
    l.send(cancel);
    l.pointer(InputEvent::PointerMove, 50, 10);
    CHECK(drags == 7);

    // A draggable element drags instead.
    Loaded d = load(doc(kTrackStyle, "<div id=\"Track\" draggable on:pointerdrag=\"state(PD)\"></div>"));
    d.pointer(InputEvent::PointerDown, 10, 10);
    d.pointer(InputEvent::PointerMove, 40, 10);
    CHECK(d.doc->dragSource().valid());
    CHECK_FALSE(d.has("Track", "PD"));
}

TEST_CASE("T-394: scrollIntoView(repeatId, index) on a virtualized repeat, also right after listInsert") {
    Loaded l = load(doc("",
                        "<div id=\"P\" port:items=\"list&lt;struct{name: string}&gt;\"></div>"
                        "<div id=\"Scroll\" style=\"position: absolute; left: 0px; top: 0px; width: 200px; "
                        "height: 200px; overflow: scroll\">"
                        "<repeat port=\"items\" id=\"Rows\" virtualize>"
                        "<div id=\"Row\" style=\"height: 20px\"></div></repeat></div>"));
    const PortId items = l.doc->lookupPort("items");
    REQUIRE(l.doc->setPortText(items, manyRows(200)) == Status::kOk);
    l.step(0.016f);
    ElementRef s = l.el("Scroll");
    CHECK(s.scrollIntoView("Rows", 150, ScrollAlign::Center) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(150 * 20 + 10 - 100));
    CHECK(s.scrollIntoView("Rows", 3, ScrollAlign::Start) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(60.0f));
    CHECK(s.scrollIntoView("Rows", 200) == Status::kInvalidArgument);
    CHECK(s.scrollIntoView("Nope", 0) == Status::kInvalidArgument);

    // Inserted this frame (no update in between): reachable at once.
    StringId names[1] = {l.doc->intern("name")};
    Value fields[1] = {Value::ofStringId(l.doc->intern("New"))};
    REQUIRE(l.doc->listInsert(items, 200, Value::ofStruct(names, fields, 1)) == Status::kOk);
    CHECK(s.scrollIntoView("Rows", 200, ScrollAlign::End) == Status::kOk);
    CHECK(s.scrollPos().y == doctest::Approx(201 * 20 - 200));

    // A plain repeat uses the instance boxes.
    Loaded p = load(doc(
        "",
        "<div id=\"P\" port:items=\"list&lt;struct{name: string}&gt;\"></div>"
        "<div id=\"Scroll\" style=\"position: absolute; left: 0px; top: 0px; width: 200px; "
        "height: 100px; overflow: scroll\">"
        "<repeat port=\"items\" id=\"Rows\"><div id=\"Row\" style=\"height: 20px\"></div></repeat></div>"));
    REQUIRE(p.doc->setPortText(p.doc->lookupPort("items"), manyRows(20)) == Status::kOk);
    p.step(0.016f);
    CHECK(p.el("Scroll").scrollIntoView("Rows", 10, ScrollAlign::Start) == Status::kOk);
    CHECK(p.el("Scroll").scrollPos().y == doctest::Approx(200.0f));
}

// -- T-397 (08.17): FocusIn / FocusOut, the text selection API, hidden documents, repeatChain ----

TEST_CASE("T-397: FocusIn / FocusOut bubble right after Focus / Blur; on:focusin / on:focusout") {
    Loaded l = load(doc("button { position: absolute; width: 40px; height: 40px; } #B { left: 50px; }",
                        "<div id=\"Box\" on:focusin=\"class(in)\" on:focusout=\"class(out)\">"
                        "<button id=\"A\"></button><button id=\"B\"></button></div>"));
    std::vector<std::string> log;
    auto record = [&log](std::string what) { return [&log, what](Event&) { log.push_back(what); }; };
    for (const char* id : {"A", "B"}) {
        const std::string s(id);
        l.doc->addListener(l.el(id), EventType::Focus, record(s + ":focus"));
        l.doc->addListener(l.el(id), EventType::FocusIn, record(s + ":focusin"));
        l.doc->addListener(l.el(id), EventType::Blur, record(s + ":blur"));
        l.doc->addListener(l.el(id), EventType::FocusOut, record(s + ":focusout"));
    }
    // Focus and Blur do not bubble: the root never sees them.
    l.doc->addListener(l.el("Root"), EventType::Focus, record("Root:focus"));
    l.doc->addListener(l.el("Root"), EventType::Blur, record("Root:blur"));
    NodeId lastTarget = kInvalidNode;
    l.doc->addListener(l.el("Root"), EventType::FocusIn, [&](Event& e) {
        lastTarget = e.target;
        log.push_back("Root:focusin");
    });
    l.doc->addListener(l.el("Root"), EventType::FocusOut, [&](Event& e) {
        CHECK(e.target == l.el("A").node);
        log.push_back("Root:focusout");
    });

    CHECK(l.el("A").focus() == Status::kOk);
    CHECK(log == std::vector<std::string>{"A:focus", "A:focusin", "Root:focusin"});
    CHECK(lastTarget == l.el("A").node);
    CHECK(l.el("Box").hasClass("in"));
    CHECK_FALSE(l.el("Box").hasClass("out"));

    log.clear();
    CHECK(l.el("B").focus() == Status::kOk);
    CHECK(log == std::vector<std::string>{"A:blur", "A:focusout", "Root:focusout", "B:focus", "B:focusin",
                                          "Root:focusin"});
    CHECK(lastTarget == l.el("B").node);
    CHECK(l.el("Box").hasClass("out"));

    std::string err;
    CHECK(validateTriggerEvent("focusin", err));
    CHECK(validateTriggerEvent("focusout", err));
}

TEST_CASE("T-397: selectAll / setSelection / selection; setValue while display:none; focus() unlaid out") {
    Loaded l = load(doc("input { position: absolute; width: 100px; height: 20px; } #H { display: none; }",
                        "<input id=\"I\" type=\"text\" value=\"hello\"/>"
                        "<div id=\"H\"><input id=\"J\" type=\"text\"/></div><div id=\"D\"></div>"));
    ElementRef i = l.el("I");
    CHECK(i.selectAll() == Status::kOk);
    CHECK(i.selection().start == 0);
    CHECK(i.selection().end == 5);
    CHECK(i.setSelection(1, 99) == Status::kOk);  // Clamped to the length.
    CHECK(i.selection().start == 1);
    CHECK(i.selection().end == 5);
    CHECK(i.setSelection(4, 2) == Status::kOk);  // start > end: a caret at end.
    CHECK(i.selection().start == 2);
    CHECK(i.selection().end == 2);
    i.setValue("a\xC3\xA9");                     // "a" + e-acute: bytes 1..2.
    CHECK(i.setSelection(0, 2) == Status::kOk);  // Never inside a UTF-8 sequence.
    CHECK(i.selection().end == 1);
    // Typing replaces the selection.
    CHECK(i.focus() == Status::kOk);
    CHECK(i.selectAll() == Status::kOk);
    InputEvent text;
    text.kind = InputEvent::TextInput;
    text.utf8[0] = 'z';
    text.utf8Len = 1;
    l.send(text);
    CHECK(i.value() == "z");

    ElementRef d = l.el("D");
    CHECK(d.selectAll() == Status::kUnsupported);
    CHECK(d.setSelection(0, 1) == Status::kUnsupported);
    CHECK(d.selection().end == 0);
    CHECK(ElementRef{}.selectAll() == Status::kInvalidArgument);

    // Not laid out: the value is writable, the focus is refused (the old focus stays).
    ElementRef j = l.el("J");
    j.setValue("kept");
    CHECK(j.value() == "kept");
    CHECK(j.focus() == Status::kUnsupported);
    CHECK(l.focusId() == "I");
    REQUIRE(l.el("H").setStyle("display: block") == Status::kOk);
    l.step(0.016f);
    CHECK(j.value() == "kept");
    CHECK(j.focus() == Status::kOk);
    CHECK(l.focusId() == "J");
}

TEST_CASE("T-397: a hidden document skips layout and paint, its timelines go on; shown it lays out") {
    Loaded l =
        load(doc("@timeline fade { duration: 1s; #A { 0s { opacity: 0 } 1s { opacity: 1 } } } "
                 "#A { position: absolute; left: 0px; top: 0px; width: 10px; height: 10px; "
                 "background: #ff0000; }",
                 "<div id=\"A\" on:timeline(fade).end=\"class(ended)\"></div>", "on:show=\"play(fade)\""));
    ElementRef a = l.el("A");
    CHECK(a.layoutBox().width == doctest::Approx(10.0f));
    REQUIRE(l.doc->requestHide() == Status::kOk);  // No `on:hide`: hidden at once.
    REQUIRE_FALSE(l.doc->isVisible());
    REQUIRE(a.setStyle("width: 50px") == Status::kOk);
    l.step(0.5f);
    l.step(1.0f);
    CHECK(a.layoutBox().width == doctest::Approx(10.0f));  // Not laid out.
    CHECK(a.hasClass("ended"));                            // The timeline ran to its end.
    CommandList hidden;
    l.doc->paint(hidden);
    CHECK(hidden.commands.empty());

    // Shown between two updates: the paint lays out first (no stale boxes)...
    REQUIRE(l.doc->requestShow() == Status::kOk);
    CommandList shown;
    l.doc->paint(shown);
    CHECK(a.layoutBox().width == doctest::Approx(50.0f));
    CHECK(a.boundingBox().w == doctest::Approx(50.0f));  // World transforms too.
    // ... and the next update lays out as usual.
    REQUIRE(a.setStyle("width: 70px") == Status::kOk);
    l.step(0.016f);
    CHECK(a.layoutBox().width == doctest::Approx(70.0f));

    // Hidden again, then shown, then one update: the whole tree is laid out again.
    REQUIRE(l.doc->requestHide() == Status::kOk);
    REQUIRE(a.setStyle("width: 30px") == Status::kOk);
    l.step(0.016f);
    CHECK(a.layoutBox().width == doctest::Approx(70.0f));
    REQUIRE(l.doc->requestShow() == Status::kOk);
    l.step(0.016f);
    CHECK(a.layoutBox().width == doctest::Approx(30.0f));
}

TEST_CASE("T-397: requestShow / requestHide called while dispatching apply after the dispatch") {
    CompileResult ir =
        compileXt(doc("#Btn { position: absolute; left: 0px; top: 0px; width: 50px; height: 50px; }",
                      "<div id=\"Btn\"></div>"));
    REQUIRE(ir.ok());
    Context ctx{ContextConfig{}};
    const DocumentHandle hb = ctx.loadDocument(IrBlob{ir.ir.data(), ir.ir.size()}, false).value();
    const DocumentHandle ha = ctx.loadDocument(IrBlob{ir.ir.data(), ir.ir.size()}, false).value();
    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    ctx.update(0);
    REQUIRE(ctx.requestHide(hb) == Status::kOk);  // Not dispatching: at once.
    REQUIRE_FALSE(ctx.isVisible(hb));
    Document* a = ctx.document(ha);
    bool visibleInside = true;
    bool hidAInside = true;
    int clicks = 0;
    a->addListener(a->byId("Btn"), EventType::Click, [&](Event&) {
        ++clicks;
        CHECK(ctx.requestShow(hb) == Status::kOk);
        visibleInside = ctx.isVisible(hb);
        CHECK(a->requestHide() == Status::kOk);
        hidAInside = !a->isVisible();
    });
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{10, 10};
    ctx.pushInput(ev);
    ev.kind = InputEvent::PointerUp;
    ctx.pushInput(ev);
    ctx.update(0.016f);
    REQUIRE(clicks == 1);
    CHECK_FALSE(visibleInside);
    CHECK_FALSE(hidAInside);
    CHECK(ctx.isVisible(hb));  // Applied once the dispatch (the whole update) ended.
    CHECK_FALSE(ctx.isVisible(ha));
    CommandList cmds;
    ctx.render(cmds);
    CHECK(ctx.document(hb)->byId("Btn").layoutBox().width == doctest::Approx(50.0f));
}

TEST_CASE("T-397: repeatChain lists the repeats around an element, nearest first") {
    Loaded l =
        load(doc("",
                 "<div id=\"P\" port:groups=\"list&lt;struct{name: string, rows: list&lt;struct{t: "
                 "string}&gt;}&gt;\"></div>"
                 "<repeat port=\"groups\" id=\"Rows\"><div id=\"Row\">"
                 "<repeat port=\"item(rows)\" id=\"Sub\"><div id=\"It\"><span id=\"Cell\"></span></div>"
                 "</repeat></div></repeat><div id=\"Out\"></div>"));
    REQUIRE(l.doc->setPortText(
                l.doc->lookupPort("groups"),
                R"([{"name":"A","rows":[{"t":"a0"}]},{"name":"B","rows":[{"t":"b0"},{"t":"b1"}]}])") ==
            Status::kOk);
    l.step(0.016f);
    std::span<const ElementRef> inner = l.doc->repeatInstances("Rows/1/Sub");
    REQUIRE(inner.size() == 2);
    const NodeId cell = l.doc->arena().firstChild(inner[1].node);
    REQUIRE(cell != kInvalidNode);
    const std::vector<RepeatRef> chain = l.doc->repeatChain(cell);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].repeatId == "Rows/1/Sub");
    CHECK(chain[0].index == 1);
    CHECK(chain[1].repeatId == "Rows");
    CHECK(chain[1].index == 1);
    CHECK(l.doc->repeatChain(l.el("Out").node).empty());
    CHECK(l.doc->repeatChain(kInvalidNode).empty());
}

// -- T-407 (43 §43.3): behavior 0 keeps the pre-T-389 per-event triggers -----------------------------

TEST_CASE("T-407: behavior 0 runs on:pointermove for each of 100 moves, at once") {
    Loaded m =
        load(doc("#A { position: absolute; left: 0px; top: 0px; width: 200px; height: 100px; }",
                 "<div id=\"A\" port:x=\"number\" on:pointermove=\"set(x, pointer.local(#A).x)\"></div>"));
    REQUIRE(m.doc->behavior() == 0);
    const PortId x = m.doc->lookupPort("x");
    REQUIRE(x != kInvalidPort);
    int sets = 0;
    m.doc->addPortListener([&](const PortEvent& ev) { sets += ev.port == x ? 1 : 0; });
    for (int i = 1; i <= 100; ++i) {
        InputEvent ev;
        ev.kind = InputEvent::PointerMove;
        ev.pos = Vec2{static_cast<float>(i), 50.0f};
        m.doc->dispatchInput(ev);
        CHECK(m.doc->getPort(x).f == doctest::Approx(static_cast<float>(i)));  // No update() needed.
    }
    CHECK(sets == 100);
    m.step(0.016f);
    CHECK(sets == 100);  // Nothing was held back for the frame.
}

TEST_CASE("T-407: behavior 0 -- a press, drag and release in one frame still drags the slider") {
    Loaded m = load(kSliderXt);
    const PortId port = m.doc->lookupPort("S1.value");
    REQUIRE(port != kInvalidPort);
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{30, 10};
    m.doc->dispatchInput(ev);
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{95, 10};
    m.doc->dispatchInput(ev);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.75f));  // Before the release and the frame.
    ev.kind = InputEvent::PointerUp;
    m.doc->dispatchInput(ev);
    m.step(0.016f);
    CHECK(m.doc->getPort(port).f == doctest::Approx(0.75f));
}

// -- T-411 (27 §27.2.3): set() values that are bind-source expressions ---------------------------

TEST_CASE("T-411: set() reads port(), constants and converters; port values when the action runs") {
    Loaded m = load(doc(
        "#A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }"
        "#B { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px; }"
        "@timeline wait { duration: 0.5s; #B { 0s { opacity: 0 } 0.5s { opacity: 1 } } }",
        "<div id=\"P\" port:a=\"number = 2\" port:b=\"number = 3\" port:total=\"number out\""
        " port:twice=\"number\" port:c=\"number\" port:s=\"string\" port:flag=\"bool\" port:late=\"number\">"
        "<div id=\"A\" on:click=\"set(total, port(a) | offset(port(b))); set(twice, port(a) | scale(2) | "
        "offset(b)); set(c, 5 | scale(2)); set(s, 'ab' | upper); set(flag, port(a) | gt(port(b)))\"></div>"
        "<div id=\"B\" on:click=\"play(wait) then set(late, port(a))\"></div></div>"));
    m.pointer(InputEvent::PointerDown, 20, 20);
    m.pointer(InputEvent::PointerUp, 20, 20);
    CHECK(m.doc->getPort(m.doc->lookupPort("total")).f == doctest::Approx(5.0f));
    CHECK(m.doc->getPort(m.doc->lookupPort("twice")).f == doctest::Approx(7.0f));
    CHECK(m.doc->getPort(m.doc->lookupPort("c")).f == doctest::Approx(10.0f));
    CHECK(m.doc->portText(m.doc->lookupPort("s")) == "AB");
    CHECK(m.doc->getPort(m.doc->lookupPort("flag")).f == doctest::Approx(0.0f));
    // A `then` wait: the port is read when the set() runs, not when the click happened.
    m.pointer(InputEvent::PointerDown, 70, 20);
    m.pointer(InputEvent::PointerUp, 70, 20);
    CHECK(m.doc->getPort(m.doc->lookupPort("late")).f == doctest::Approx(0.0f));
    REQUIRE(m.doc->setPort(m.doc->lookupPort("a"), Value::ofFloat(9.0f)) == Status::kOk);
    m.step(0.3f);
    m.step(0.3f);
    CHECK(m.doc->getPort(m.doc->lookupPort("late")).f == doctest::Approx(9.0f));
}

TEST_CASE("T-411: set() inside a <repeat> template reads item(), item(<field>) and repeat.index") {
    Loaded m = load(doc(
        "#Rows > div { height: 20px; } #Names > div { height: 20px; }",
        "<div id=\"P\" port:rows=\"list&lt;struct{id: string, n: number}&gt;\" port:selected=\"string\""
        " port:at=\"number\" port:big=\"number\" port:names=\"list&lt;string&gt;\" port:pick=\"string\">"
        "<repeat port=\"rows\" id=\"Rows\"><div id=\"Row\" on:click=\"set(selected, item(id));"
        " set(at, repeat.index); set(big, item(n) | scale(10))\"></div></repeat>"
        "<repeat port=\"names\" id=\"Names\"><div id=\"Name\" on:click=\"set(pick, item() | upper)\"></div>"
        "</repeat></div>"));
    REQUIRE(m.doc->setPortText(m.doc->lookupPort("rows"), R"([{"id":"a","n":1},{"id":"b","n":2}])") ==
            Status::kOk);
    REQUIRE(m.doc->setPortText(m.doc->lookupPort("names"), R"(["x","y"])") == Status::kOk);
    m.step(0.016f);
    auto click = [&m](const ElementRef& e) {
        const Rect box = e.boundingBox();
        m.pointer(InputEvent::PointerDown, box.x + 5, box.y + box.h * 0.5f);
        m.pointer(InputEvent::PointerUp, box.x + 5, box.y + box.h * 0.5f);
    };
    std::span<const ElementRef> rows = m.doc->repeatInstances("Rows");
    REQUIRE(rows.size() == 2);
    click(rows[1]);
    CHECK(m.doc->portText(m.doc->lookupPort("selected")) == "b");
    CHECK(m.doc->getPort(m.doc->lookupPort("at")).f == doctest::Approx(1.0f));
    CHECK(m.doc->getPort(m.doc->lookupPort("big")).f == doctest::Approx(20.0f));
    // The entry is read when the action runs: an update of entry 0 shows on the next click.
    REQUIRE(m.doc->setPortText(m.doc->lookupPort("rows"), R"([{"id":"z","n":5},{"id":"b","n":2}])") ==
            Status::kOk);
    m.step(0.016f);
    click(m.doc->repeatInstances("Rows")[0]);
    CHECK(m.doc->portText(m.doc->lookupPort("selected")) == "z");
    CHECK(m.doc->getPort(m.doc->lookupPort("at")).f == doctest::Approx(0.0f));
    CHECK(m.doc->getPort(m.doc->lookupPort("big")).f == doctest::Approx(50.0f));
    std::span<const ElementRef> names = m.doc->repeatInstances("Names");
    REQUIRE(names.size() == 2);
    click(names[1]);
    CHECK(m.doc->portText(m.doc->lookupPort("pick")) == "Y");
}

TEST_CASE("T-411: set() source errors -- unknown port, item() outside a template, call ports, outer") {
    auto bad = [](const std::string& body, std::string_view needle) {
        CompileResult r = compileXt(doc("", body));
        CAPTURE(body);
        CHECK_FALSE(r.ok());
        CHECK(hasError(r, needle));
    };
    bad("<div port:t=\"number\" on:click=\"set(t, port(nope))\"></div>", "no port \"nope\" is declared here");
    bad("<div port:t=\"number\" port:a=\"number\" on:click=\"set(t, port(a) | offset(port(nope)))\"></div>",
        "no port \"nope\"");
    bad("<div port:t=\"string\" on:click=\"set(t, item(id))\"></div>",
        "item(id) is only valid inside a <repeat>");
    bad("<div port:t=\"number\" on:click=\"set(t, repeat.index)\"></div>",
        "repeat.index is only valid inside");
    bad("<div port:t=\"number\" port:go=\"number call\" on:click=\"set(t, port(go))\"></div>",
        "a call or event port has no value to read");
    bad("<div port:t=\"number\" port:a=\"number\" on:click=\"set(t, port(a) | tween(1))\"></div>",
        "cannot be used in set()");
    bad("<div port:t=\"number\" port:go=\"number call\" on:click=\"set(go, port(t))\"></div>",
        "cannot set the call, list or struct port");
    bad("<div port:rows=\"list&lt;struct{id: string}&gt;\" port:t=\"string\"><repeat port=\"rows\" id=\"R\">"
        "<div on:click=\"set(t, item(nope))\"></div></repeat></div>",
        "no field \"nope\"");
    std::string err;
    CHECK(validateTriggerActions("set(t, port(a) | offset(port(b))); set(u, item(id)); set(v, 5 | scale(2))",
                                 err));
    CHECK_FALSE(validateTriggerActions("set(t, nope | scale(2))", err));
}
