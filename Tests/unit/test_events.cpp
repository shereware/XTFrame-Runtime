#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/animator.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// EventDispatcher/Document event tests: hover/active state machine, bubbling + stopPropagation,
/// disabled elements, wheel scroll + preventDefault, listener pool compaction, cursor(), animation
/// event bridging (AnimationEnd/TransitionEnd/Marker), Enter/Leave divergence. Fixtures compiled
/// from Tests/layout/events_*.xt (see test_layout_block.cpp's loadLayoutDoc pattern) plus, for
/// Marker, a hand-built xt::Timeline played directly via Document::play() (no .xt syntax exists
/// for a Lottie-style marker outside a real Lottie scene; see Docs/tasks/T-32.md report).
///
/// See Docs/design/08-events.md §8.3-8.4, §8.6-8.7, Docs/tasks/T-32.md "測試".

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

InputEvent pointerMove(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = pos;
    return ev;
}

InputEvent pointerDown(Vec2 pos, PointerButton button = PointerButton::Left) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = pos;
    ev.button = button;
    return ev;
}

InputEvent pointerUp(Vec2 pos, PointerButton button = PointerButton::Left) {
    InputEvent ev;
    ev.kind = InputEvent::PointerUp;
    ev.pos = pos;
    ev.button = button;
    return ev;
}

InputEvent wheel(Vec2 pos, Vec2 delta) {
    InputEvent ev;
    ev.kind = InputEvent::Wheel;
    ev.pos = pos;
    ev.wheelDelta = delta;
    return ev;
}

}  // namespace

TEST_CASE("PointerMove: hover state + Enter (non-bubbling) + :hover style recompute") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_basic.xt", &shaper, &assets);

    ElementRef hoverable = doc->byId("hoverable");
    ElementRef body = doc->byId("body");
    REQUIRE(hoverable.valid());
    REQUIRE(body.valid());
    CHECK(doc->arena().element(hoverable.node).style.base[prop::BackgroundColor].c == Color{0, 0, 0, 1});

    int hoverEnterCount = 0;
    int bodyEnterCount = 0;
    doc->addListener(hoverable, EventType::PointerEnter, [&](Event&) { ++hoverEnterCount; });
    doc->addListener(body, EventType::PointerEnter, [&](Event&) { ++bodyEnterCount; });

    doc->dispatchInput(pointerMove(Vec2{10, 130}));  // inside #hoverable (0,120)-(60,150)
    CHECK(hoverEnterCount == 1);
    // 08.4 sets kStateHover (and so dispatches Enter) on the hit element AND every ancestor, since
    // this is the very first hover (there was no previous hover path to diverge from) -- body is
    // one of those ancestors, so it gets its own (non-bubbling) Enter too. The "doesn't bubble"
    // property proper is exercised by the divergence test below (A's Enter/Leave count stays 0
    // when hover moves between two of its children).
    CHECK(bodyEnterCount == 1);
    CHECK(hoverable.hasState(kStateHover));

    doc->update(0.0f);  // re-resolves style now that kDirtyStyle is set.
    CHECK(doc->arena().element(hoverable.node).style.base[prop::BackgroundColor].c == Color{1, 1, 1, 1});

    int hoverLeaveCount = 0;
    doc->addListener(hoverable, EventType::PointerLeave, [&](Event&) { ++hoverLeaveCount; });
    doc->dispatchInput(pointerMove(Vec2{300, 250}));  // outside every element -> `xt` root.
    CHECK(hoverLeaveCount == 1);
    CHECK_FALSE(hoverable.hasState(kStateHover));
}

TEST_CASE("PointerEnter/Leave divergence: A's a1 -> a2 leaves only a1, enters only a2") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_basic.xt", &shaper, &assets);

    ElementRef a = doc->byId("A");
    ElementRef a1 = doc->byId("a1");
    ElementRef a2 = doc->byId("a2");
    REQUIRE(a.valid());
    REQUIRE(a1.valid());
    REQUIRE(a2.valid());

    doc->dispatchInput(pointerMove(Vec2{10, 10}));  // inside a1: establishes the initial hover path.

    int aEnter = 0, aLeave = 0, a1Leave = 0, a2Enter = 0;
    doc->addListener(a, EventType::PointerEnter, [&](Event&) { ++aEnter; });
    doc->addListener(a, EventType::PointerLeave, [&](Event&) { ++aLeave; });
    doc->addListener(a1, EventType::PointerLeave, [&](Event&) { ++a1Leave; });
    doc->addListener(a2, EventType::PointerEnter, [&](Event&) { ++a2Enter; });

    doc->dispatchInput(pointerMove(Vec2{70, 10}));  // inside a2 now.

    CHECK(a1Leave == 1);
    CHECK(a2Enter == 1);
    CHECK(aEnter == 0);
    CHECK(aLeave == 0);
}

TEST_CASE("PointerDown/Up: bubbling order, current, stopPropagation, Click, disabled blocks Click") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_basic.xt", &shaper, &assets);

    ElementRef a = doc->byId("A");
    ElementRef a1 = doc->byId("a1");
    REQUIRE(a.valid());
    REQUIRE(a1.valid());

    std::vector<std::string> order;
    doc->addListener(a1, EventType::PointerDown, [&](Event& ev) {
        order.push_back("a1");
        CHECK(ev.current == a1.node);
        CHECK(ev.target == a1.node);
    });
    doc->addListener(a, EventType::PointerDown, [&](Event& ev) {
        order.push_back("A");
        CHECK(ev.current == a.node);
    });

    doc->dispatchInput(pointerDown(Vec2{10, 10}));  // inside a1.
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "a1");  // target first, then bubbles to the parent.
    CHECK(order[1] == "A");
    CHECK(a1.hasState(kStateActive));
    CHECK(a.hasState(kStateActive));  // 08.4: kStateActive is set on the whole ancestor chain.

    int clickCount = 0;
    doc->addListener(a1, EventType::Click, [&](Event&) { ++clickCount; });
    doc->dispatchInput(pointerUp(Vec2{10, 10}));  // up on the same element -> Click.
    CHECK(clickCount == 1);
    CHECK_FALSE(a1.hasState(kStateActive));

    // Down on a1, up elsewhere (inside a2's area, not a1/A) -> no Click.
    doc->dispatchInput(pointerDown(Vec2{10, 10}));
    doc->dispatchInput(pointerUp(Vec2{70, 10}));
    CHECK(clickCount == 1);  // unchanged.

    // Down on a1, up outside the document entirely (no node under the pointer) -> no Click, active
    // state cleared, and no assert on kInvalidNode (Docs/tasks/T-87.md).
    doc->dispatchInput(pointerDown(Vec2{10, 10}));
    CHECK(a1.hasState(kStateActive));
    doc->dispatchInput(pointerUp(Vec2{2000, 2000}));
    CHECK(clickCount == 1);
    CHECK_FALSE(a1.hasState(kStateActive));

    // stopPropagation on a1 blocks A from seeing PointerDown.
    order.clear();
    doc->addListener(a1, EventType::PointerDown, [](Event& ev) { ev.stopPropagation = true; });
    doc->dispatchInput(pointerDown(Vec2{10, 10}));
    CHECK(order == std::vector<std::string>{"a1"});  // a1's own listener ran; A did not (stopped).
    doc->dispatchInput(pointerUp(Vec2{10, 10}));     // clean up active state for later cases.

    ElementRef btn = doc->byId("btn");
    REQUIRE(btn.valid());
    CHECK(btn.setState(kStateDisabled, true));
    int btnClicks = 0;
    doc->addListener(btn, EventType::Click, [&](Event&) { ++btnClicks; });
    doc->dispatchInput(pointerDown(Vec2{10, 210}));
    CHECK_FALSE(btn.hasState(kStateActive));  // 08.4: disabled elements never get kStateActive.
    doc->dispatchInput(pointerUp(Vec2{10, 210}));
    CHECK(btnClicks == 0);
}

TEST_CASE(
    "Wheel: scrolls an overflow:scroll element (clamped), preventDefault blocks it, "
    "child world transform follows scrollY") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_basic.xt", &shaper, &assets);

    ElementRef scrollBoxA = doc->byId("scrollBoxA");
    ElementRef scrollChildA = doc->byId("scrollChildA");
    REQUIRE(scrollBoxA.valid());
    REQUIRE(scrollChildA.valid());

    float beforeY = doc->arena().common(scrollChildA.node).world.f;

    doc->dispatchInput(wheel(Vec2{260, 10}, Vec2{0, 1}));  // inside scrollBoxA (250,0)-(300,50).
    doc->update(0.0f);  // ScrollY is now kDirtyLayout: relayout to recompute the world transform.
    CHECK(doc->arena().element(scrollBoxA.node).style.base[prop::ScrollY].f == doctest::Approx(40.0f));
    float afterY = doc->arena().common(scrollChildA.node).world.f;
    CHECK(afterY == doctest::Approx(beforeY - 40.0f));

    // Clamp: scrollHeight (200) - clientHeight (50) = 150 max; 5 more wheel ticks would overshoot.
    for (int i = 0; i < 5; ++i) {
        doc->dispatchInput(wheel(Vec2{260, 10}, Vec2{0, 1}));
    }
    CHECK(doc->arena().element(scrollBoxA.node).style.base[prop::ScrollY].f == doctest::Approx(150.0f));

    ElementRef scrollBoxB = doc->byId("scrollBoxB");
    REQUIRE(scrollBoxB.valid());
    doc->addListener(scrollBoxB, EventType::Wheel, [](Event& ev) { ev.preventDefault = true; });
    doc->dispatchInput(wheel(Vec2{260, 70}, Vec2{0, 1}));  // inside scrollBoxB (250,60)-(300,110).
    CHECK(doc->arena().element(scrollBoxB.node).style.base[prop::ScrollY].f == doctest::Approx(0.0f));
}

TEST_CASE("Event::localPos: translate(30,40) and rotate(90deg) both report the hit node's own local coords") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_localpos.xt", &shaper, &assets);

    ElementRef translateBox = doc->byId("translateBox");
    ElementRef rotateBox = doc->byId("rotateBox");
    REQUIRE(translateBox.valid());
    REQUIRE(rotateBox.valid());

    Vec2 translateLocalPos{};
    doc->addListener(translateBox, EventType::PointerDown,
                     [&](Event& ev) { translateLocalPos = ev.localPos; });
    // #translateBox: 100x100 at (0,0), transform: translate(30px, 40px) -> world = local + (30, 40).
    doc->dispatchInput(pointerDown(Vec2{50, 60}));
    CHECK(std::abs(translateLocalPos.x - 20.0f) < 0.01f);
    CHECK(std::abs(translateLocalPos.y - 20.0f) < 0.01f);
    doc->dispatchInput(pointerUp(Vec2{50, 60}));  // clean up active state.

    Vec2 rotateLocalPos{};
    doc->addListener(rotateBox, EventType::PointerDown, [&](Event& ev) { rotateLocalPos = ev.localPos; });
    // #rotateBox: 100x100 at (200,0), transform: rotate(90deg) around its default center (250, 50).
    // The screen point 30px to the right of center (280, 50) is the local point 30px above center,
    // i.e. local (50, 20) (a clockwise rotation, y down, per Mat2x3::rotationDeg's convention).
    doc->dispatchInput(pointerDown(Vec2{280, 50}));
    CHECK(std::abs(rotateLocalPos.x - 50.0f) < 0.01f);
    CHECK(std::abs(rotateLocalPos.y - 20.0f) < 0.01f);
    doc->dispatchInput(pointerUp(Vec2{280, 50}));
}

TEST_CASE("addListener/removeListener: pool compaction keeps ids valid; safe from a callback") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_basic.xt", &shaper, &assets);

    ElementRef a = doc->byId("A");
    REQUIRE(a.valid());

    int survivorCalls = 0;
    uint32_t survivorId = doc->addListener(a, EventType::Wheel, [&](Event&) { ++survivorCalls; });

    // 1100 add+remove cycles: pushes the dispatcher's dead-listener count past the 1024
    // compaction threshold (Docs/tasks/T-32.md "每 1024 次移除做一次壓實").
    for (int i = 0; i < 1100; ++i) {
        uint32_t id = doc->addListener(a, EventType::Wheel, [](Event&) {});
        doc->removeListener(id);
    }

    InputEvent w = wheel(Vec2{10, 10}, Vec2{0, 1});
    doc->dispatchInput(w);
    CHECK(survivorCalls == 1);  // survivor was never removed: still fires after compaction.

    doc->removeListener(survivorId);  // id issued before the compaction; must still be valid.
    doc->dispatchInput(w);
    CHECK(survivorCalls == 1);  // no longer fires.

    // Listeners may add/remove other listeners from inside a callback; the newly-added one must
    // not fire in the same dispatch, and one removed mid-dispatch must not fire either.
    // Listeners fire in registration order (oldest first, 08.3), so the "remover" must be
    // registered before l2 for this to exercise "removed mid-dispatch never runs".
    int l1 = 0, l2 = 0, l3 = 0;
    uint32_t l2Id = 0;
    uint32_t l3Id = 0;
    doc->addListener(a, EventType::Wheel, [&](Event&) {
        ++l1;
        doc->removeListener(l2Id);
        l3Id = doc->addListener(a, EventType::Wheel, [&](Event&) { ++l3; });
    });
    l2Id = doc->addListener(a, EventType::Wheel, [&](Event&) { ++l2; });
    (void)l3Id;

    doc->dispatchInput(w);
    CHECK(l1 == 1);
    CHECK(l2 == 0);  // removed by l1 before its own turn.
    CHECK(l3 == 0);  // added during this dispatch: does not fire this round.

    doc->dispatchInput(w);
    CHECK(l1 == 2);
    CHECK(l2 == 0);
    CHECK(l3 == 1);  // fires on the next dispatch.
}

TEST_CASE("Context::cursor(): hovering a cursor:pointer element reports Pointer") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_basic.xt", &shaper, &assets);

    CHECK(doc->cursor() == Cursor::Default);
    doc->dispatchInput(pointerMove(Vec2{10, 170}));  // inside #cursorEl (0,160)-(30,190).
    CHECK(doc->cursor() == Cursor::Pointer);
}

TEST_CASE("Animation events: CSS animation -> AnimationEnd bubbles; CSS transition -> TransitionEnd") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_anim.xt", &shaper, &assets);

    ElementRef animEl = doc->byId("animEl");
    ElementRef body = doc->byId("body");
    ElementRef transEl = doc->byId("transEl");
    REQUIRE(animEl.valid());
    REQUIRE(body.valid());
    REQUIRE(transEl.valid());

    int animEndOnEl = 0, animEndOnBody = 0;
    doc->addListener(animEl, EventType::AnimationEnd, [&](Event&) { ++animEndOnEl; });
    doc->addListener(body, EventType::AnimationEnd, [&](Event&) { ++animEndOnBody; });
    // events_anim.xt's #animEl already has `animation: fadeIn 0.1s` (started by loadDoc's own
    // update(0.0f)); advance past its 0.1s duration.
    doc->update(0.2f);
    CHECK(animEndOnEl == 1);
    CHECK(animEndOnBody == 1);  // 08.6: animation events bubble.

    int transEnd = 0;
    doc->addListener(transEl, EventType::TransitionEnd, [&](Event&) { ++transEnd; });
    CHECK(transEl.addClass("on"));  // background-color change -> registers a 0.1s transition.
    doc->update(0.21f);             // one more resolve so the transition actually starts.
    doc->update(0.4f);              // past the transition's own 0.1s duration.
    CHECK(transEnd == 1);
}

TEST_CASE("Animation events: a hand-built Timeline marker dispatches Marker to the played element") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("events_anim.xt", &shaper, &assets);
    ElementRef animEl = doc->byId("animEl");
    REQUIRE(animEl.valid());

    Timeline tl;
    tl.duration = 1.0f;
    constexpr StringId kMarkerId = 4242;
    tl.markers.push_back(Marker{kMarkerId, 0.1f, 0.0f});
    TimelineId tid = doc->animator().registerTimeline(&tl);

    int markerCount = 0;
    StringId seenMarker = kInvalidString;
    doc->addListener(animEl, EventType::Marker, [&](Event& ev) {
        ++markerCount;
        seenMarker = ev.markerName;
    });

    PlayParams params;
    params.timeline = tid;
    params.selfNode = animEl.node;
    params.loop = LoopMode::Once;
    params.iterations = 1;
    doc->play(params);

    doc->update(0.0f);
    doc->update(0.05f);  // before the marker.
    CHECK(markerCount == 0);
    doc->update(0.2f);  // crosses the marker at 0.1.
    CHECK(markerCount == 1);
    CHECK(seenMarker == kMarkerId);
}
