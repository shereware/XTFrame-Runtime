#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/animator.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// T-260a: host callbacks that call back into the core while it is dispatching -- unloading,
/// reloading or loading a Document from a ContextListener, removing an element from its
/// `transitionend` listener, playing or stopping instances from an iteration / marker callback
/// (docs/reviews/2026-10-02-full-audit-raw/C.md C1, C2; docs/tasks/T-260a.md).

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
    CompileResult r = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    return r;
}

IrBlob blobOf(const CompileResult& r) { return IrBlob{r.ir.data(), r.ir.size()}; }

constexpr const char* kStyle = R"X(
  #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
  @timeline fade { duration: 1s; #A { 0s { opacity: 0 } 1s { opacity: 1 } } }
)X";

/// Runs the test's callbacks from inside the Context's dispatch.
struct CallbackListener : ContextListener {
    std::function<void(const HostEvent&)> host;
    std::function<void(DocumentHandle)> hidden;
    void onHostEvent(const HostEvent& e) override {
        if (host) {
            host(e);
        }
    }
    void onHidden(DocumentHandle d) override {
        if (hidden) {
            hidden(d);
        }
    }
};

void queueClick(Context& ctx, float x, float y) {
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{x, y};
    down.button = PointerButton::Left;
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ctx.pushInput(down);
    ctx.pushInput(up);
}

std::unique_ptr<Document> loadStandalone(const CompileResult& r) {
    REQUIRE(r.ok());
    std::unique_ptr<Document> d;
    REQUIRE(Document::load(blobOf(r), false, nullptr, nullptr, d) == Status::kOk);
    d->setViewport(Rect{0, 0, 200, 100});
    d->update(0.0f);
    return d;
}

}  // namespace

// -- C1: the Context's document table ------------------------------------------------------------

TEST_CASE("reentrancy: unloadDocument from onHidden while a `then hide` outro settles in update()") {
    CompileResult r = compileXt(doc(kStyle, "<div id=\"A\"></div>", "on:hide=\"play(fade) then hide\""));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
    REQUIRE(h.ok());
    const DocumentHandle handle = h.value();

    CallbackListener listener;
    int hiddenCalls = 0;
    bool goneAtOnce = false;
    listener.hidden = [&](DocumentHandle d) {
        ++hiddenCalls;
        ctx.unloadDocument(d);
        goneAtOnce = ctx.document(d) == nullptr;  // Dead to the host now; freed after update().
    };
    ctx.addListener(&listener);
    ctx.update(0.0f);
    REQUIRE(ctx.requestHide(handle) == Status::kOk);
    ctx.update(0.5f);
    CHECK(hiddenCalls == 0);
    ctx.update(0.6f);  // `fade` ends -> `then hide` -> OnHidden -> unloadDocument.
    CHECK(hiddenCalls == 1);
    CHECK(goneAtOnce);
    CHECK(ctx.document(handle) == nullptr);  // Freed once update() returned.
    CHECK_FALSE(ctx.isVisible(handle));
    ctx.update(0.1f);
    CommandList cmds;
    ctx.render(cmds);
    CHECK(cmds.commands.empty());
    ctx.removeListener(&listener);
}

TEST_CASE("reentrancy: unloadDocument / reloadDocument from onHostEvent (an emit fired by a click)") {
    CompileResult r =
        compileXt(doc(kStyle, R"X(<div id="A" on:click="emit(pressed); class(after)"></div>)X"));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
    REQUIRE(h.ok());
    const DocumentHandle handle = h.value();
    Document* before = ctx.document(handle);
    REQUIRE(before != nullptr);
    CallbackListener listener;
    ctx.addListener(&listener);
    ctx.update(0.0f);

    SUBCASE("unload: the handle is dead at once, the memory goes later; a second unload is a no-op") {
        bool goneAfterFirst = false;
        bool goneAfterSecond = false;
        bool visibleDuring = true;
        listener.host = [&](const HostEvent& e) {
            if (e.name != "pressed") {
                return;
            }
            ctx.unloadDocument(e.document);
            goneAfterFirst = ctx.document(e.document) == nullptr;
            ctx.unloadDocument(e.document);
            goneAfterSecond = ctx.document(e.document) == nullptr;
            visibleDuring = ctx.isVisible(e.document);
        };
        queueClick(ctx, 20, 20);
        ctx.update(0.016f);
        CHECK(goneAfterFirst);
        CHECK(goneAfterSecond);
        CHECK_FALSE(visibleDuring);
        CHECK(ctx.document(handle) == nullptr);
        CHECK_FALSE(ctx.isVisible(handle));
        ctx.update(0.016f);
    }

    SUBCASE("reload: parsed at once, installed when the outermost dispatch returns") {
        Status reloadStatus = Status::kInternal;
        bool oldDuring = false;
        listener.host = [&](const HostEvent& e) {
            if (e.name != "pressed") {
                return;
            }
            Result<DocumentHandle> re = ctx.reloadDocument(e.document, blobOf(r), false);
            reloadStatus = re.status();
            oldDuring = ctx.document(e.document) == before;
        };
        queueClick(ctx, 20, 20);
        ctx.update(0.016f);
        CHECK(reloadStatus == Status::kOk);
        CHECK(oldDuring);
        Document* after = ctx.document(handle);
        REQUIRE(after != nullptr);
        CHECK(after != before);
        CHECK(after->byId("A").valid());
        CHECK_FALSE(after->byId("A").hasClass("after"));  // The old Document ran `class(after)`.
        ctx.update(0.016f);
    }
    ctx.removeListener(&listener);
}

TEST_CASE("reentrancy: loadDocument from onHostEvent while Context::update() walks the documents") {
    CompileResult intro =
        compileXt(doc(kStyle, R"X(<div id="A" on:timeline(fade).end="emit(intro_done)"></div>)X",
                      "on:show=\"play(fade)\""));
    CompileResult other = compileXt(doc(kStyle, "<div id=\"A\"></div>"));
    CompileResult menu = compileXt(doc(kStyle, "<div id=\"A\"></div>", "on:show=\"class(shown)\""));
    REQUIRE(intro.ok());
    REQUIRE(other.ok());
    REQUIRE(menu.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h1 = ctx.loadDocument(blobOf(intro), false);
    Result<DocumentHandle> h2 = ctx.loadDocument(blobOf(other), false);
    REQUIRE(h1.ok());
    REQUIRE(h2.ok());

    CallbackListener listener;
    DocumentHandle loaded = 0;
    listener.host = [&](const HostEvent& e) {
        if (e.name == "intro_done") {
            Result<DocumentHandle> h3 = ctx.loadDocument(blobOf(menu), false);
            loaded = h3.ok() ? h3.value() : 0;
        }
    };
    ctx.addListener(&listener);
    ctx.update(0.0f);  // The intro's `show` plays `fade`.
    ctx.update(1.1f);  // `fade` ends inside the intro's update -> emit -> loadDocument.
    REQUIRE(loaded == 3);
    REQUIRE(ctx.document(loaded) != nullptr);
    // Loaded during the walk: updated from the next frame on, so its first `show` has not run yet.
    CHECK_FALSE(ctx.document(loaded)->byId("Root").hasClass("shown"));
    ctx.update(0.016f);
    CHECK(ctx.document(loaded)->byId("Root").hasClass("shown"));
    CHECK(ctx.document(h2.value()) != nullptr);
    ctx.removeListener(&listener);
}

TEST_CASE("reentrancy: a Document unloaded by another one's callback is skipped for the rest of the frame") {
    CompileResult first = compileXt(doc(
        kStyle, R"X(<div id="A" on:timeline(fade).end="emit(a_done)"></div>)X", "on:show=\"play(fade)\""));
    CompileResult second = compileXt(doc(
        kStyle, R"X(<div id="A" on:timeline(fade).end="emit(b_done)"></div>)X", "on:show=\"play(fade)\""));
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> ha = ctx.loadDocument(blobOf(first), false);
    Result<DocumentHandle> hb = ctx.loadDocument(blobOf(second), false);
    REQUIRE(ha.ok());
    REQUIRE(hb.ok());
    CallbackListener listener;
    std::vector<std::string> events;
    bool bGoneInCallback = false;
    listener.host = [&](const HostEvent& e) {
        events.emplace_back(e.name);
        if (e.name == "a_done") {
            ctx.unloadDocument(hb.value());
            bGoneInCallback = ctx.document(hb.value()) == nullptr;
        }
    };
    ctx.addListener(&listener);
    ctx.update(0.0f);  // Both play `fade`.
    ctx.update(1.1f);  // A's ends first and unloads B; B's end (same frame) never reaches the host.
    REQUIRE(events.size() == 1);
    CHECK(events[0] == "a_done");
    CHECK(bGoneInCallback);
    CHECK(ctx.document(hb.value()) == nullptr);
    CHECK(ctx.document(ha.value()) != nullptr);
    ctx.update(0.016f);
    CHECK(events.size() == 1);
    ctx.removeListener(&listener);
}

TEST_CASE("reentrancy: a Document that unloads itself makes no further host callback in that dispatch") {
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    CallbackListener listener;
    std::vector<std::string> events;
    ctx.addListener(&listener);

    SUBCASE("a second element listener and the ancestors' listeners") {
        CompileResult r = compileXt(doc(kStyle, "<div id=\"A\"></div>"));
        REQUIRE(r.ok());
        Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
        REQUIRE(h.ok());
        Document* d = ctx.document(h.value());
        ElementRef a = d->byId("A");
        d->addListener(a, EventType::Click, [&](Event&) {
            events.emplace_back("first");
            ctx.unloadDocument(h.value());
        });
        d->addListener(a, EventType::Click, [&](Event&) { events.emplace_back("second"); });
        d->addListener(d->byId("Root"), EventType::Click, [&](Event&) { events.emplace_back("root"); });
        ctx.update(0.0f);
        queueClick(ctx, 20, 20);
        ctx.update(0.016f);
        REQUIRE(events.size() == 1);
        CHECK(events[0] == "first");
        CHECK(ctx.document(h.value()) == nullptr);
    }

    SUBCASE("the rest of the trigger chain emits nothing") {
        CompileResult r =
            compileXt(doc(kStyle, R"X(<div id="A" on:click="emit(first); emit(second)"></div>)X"));
        REQUIRE(r.ok());
        Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
        REQUIRE(h.ok());
        listener.host = [&](const HostEvent& e) {
            events.emplace_back(e.name);
            ctx.unloadDocument(e.document);
        };
        ctx.update(0.0f);
        queueClick(ctx, 20, 20);
        ctx.update(0.016f);
        REQUIRE(events.size() == 1);
        CHECK(events[0] == "first");
        CHECK(ctx.document(h.value()) == nullptr);
    }

    SUBCASE("a transitionend queued in the same evaluate() is not delivered") {
        constexpr const char* kTwoTransitions = R"X(
          #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px;
               background-color: #000000; transition: background-color 0.1s linear; }
          #B { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px;
               background-color: #000000; transition: background-color 0.1s linear; }
          #A.on, #B.on { background-color: #ffffff; }
        )X";
        CompileResult r = compileXt(doc(kTwoTransitions, "<div id=\"A\"></div><div id=\"B\"></div>"));
        REQUIRE(r.ok());
        Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
        REQUIRE(h.ok());
        Document* d = ctx.document(h.value());
        ElementRef a = d->byId("A");
        ElementRef b = d->byId("B");
        // Whichever end is delivered first unloads the document; the other must not arrive.
        d->addListener(a, EventType::TransitionEnd, [&](Event&) {
            events.emplace_back("a");
            ctx.unloadDocument(h.value());
        });
        d->addListener(b, EventType::TransitionEnd, [&](Event&) {
            events.emplace_back("b");
            ctx.unloadDocument(h.value());
        });
        ctx.update(0.0f);
        a.addClass("on");
        b.addClass("on");
        ctx.update(0.01f);  // Both transitions start.
        ctx.update(0.5f);   // ... and both end in one evaluate().
        CHECK(events.size() == 1);
        CHECK(ctx.document(h.value()) == nullptr);
    }
    ctx.removeListener(&listener);
}

TEST_CASE("reentrancy: unload, see the handle gone, then load a replacement inside a callback") {
    CompileResult r = compileXt(doc(kStyle, R"X(<div id="A" on:click="emit(next)"></div>)X"));
    CompileResult menu = compileXt(doc(kStyle, "<div id=\"A\"></div>", "on:show=\"class(shown)\""));
    REQUIRE(r.ok());
    REQUIRE(menu.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
    REQUIRE(h.ok());
    CallbackListener listener;
    bool goneBeforeLoad = false;
    DocumentHandle replacement = 0;
    listener.host = [&](const HostEvent& e) {
        if (e.name != "next") {
            return;
        }
        ctx.unloadDocument(e.document);
        goneBeforeLoad = ctx.document(e.document) == nullptr;
        Result<DocumentHandle> n = ctx.loadDocument(blobOf(menu), false);
        replacement = n.ok() ? n.value() : 0;
    };
    ctx.addListener(&listener);
    ctx.update(0.0f);
    queueClick(ctx, 20, 20);
    ctx.update(0.016f);
    CHECK(goneBeforeLoad);
    REQUIRE(replacement == 2);  // A new handle; the old one is never reused.
    CHECK(ctx.document(h.value()) == nullptr);
    REQUIRE(ctx.document(replacement) != nullptr);
    CHECK_FALSE(ctx.document(replacement)->byId("Root").hasClass("shown"));  // From the next frame.
    ctx.update(0.016f);
    CHECK(ctx.document(replacement)->byId("Root").hasClass("shown"));
    ctx.removeListener(&listener);
}

TEST_CASE(
    "reentrancy: Context::update() called again from a listener (unsupported) does not corrupt memory") {
    CompileResult r = compileXt(doc(kStyle, R"X(<div id="A" on:click="emit(pressed)"></div>)X"));
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> h = ctx.loadDocument(blobOf(r), false);
    REQUIRE(h.ok());
    CallbackListener listener;
    int nested = 0;
    listener.host = [&](const HostEvent& e) {
        if (e.name == "pressed" && nested == 0) {
            ++nested;
            ctx.update(0.016f);  // Clears and refills the outer update()'s input scratch.
        }
    };
    ctx.addListener(&listener);
    ctx.update(0.0f);
    queueClick(ctx, 20, 20);
    InputEvent move;
    move.kind = InputEvent::PointerMove;
    move.pos = Vec2{30, 30};
    ctx.pushInput(move);
    ctx.pushInput(move);
    ctx.update(0.016f);
    CHECK(nested == 1);
    REQUIRE(ctx.document(h.value()) != nullptr);
    CHECK(ctx.document(h.value())->byId("A").valid());
    ctx.update(0.016f);
    CHECK(ctx.clock() == doctest::Approx(0.048f));  // Outer, nested and one more frame.
    CommandList cmds;
    ctx.render(cmds);
    ctx.removeListener(&listener);
}

// -- Event propagation: the path is fixed before the listeners run ---------------------------------

namespace {

constexpr const char* kNestedStyle = R"X(
  #P { position: absolute; left: 0px; top: 0px; width: 100px; height: 60px; }
  #A { width: 40px; height: 40px; }
)X";

/// A PointerDown + PointerUp at (x, y), straight into `d`.
void clickDoc(Document& d, float x, float y) {
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{x, y};
    down.button = PointerButton::Left;
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    d.dispatchInput(down);
    d.dispatchInput(up);
}

}  // namespace

TEST_CASE("reentrancy: a click listener removes its own target; the ancestors still hear the click") {
    CompileResult r = compileXt(doc(kNestedStyle, R"X(<div id="P"><div id="A"></div></div>)X"));
    std::unique_ptr<Document> d = loadStandalone(r);
    ElementRef a = d->byId("A");
    REQUIRE(a.valid());
    std::vector<std::string> heard;
    d->addListener(a, EventType::Click, [&](Event&) {
        heard.emplace_back("A");
        d->remove(a);
    });
    d->addListener(a, EventType::Click, [&](Event&) { heard.emplace_back("A2"); });  // Removed with #A.
    d->addListener(d->byId("P"), EventType::Click, [&](Event&) { heard.emplace_back("P"); });
    d->addListener(d->byId("Root"), EventType::Click, [&](Event&) { heard.emplace_back("Root"); });
    clickDoc(*d, 10, 10);
    REQUIRE(heard.size() == 3);
    CHECK(heard[0] == "A");
    CHECK(heard[1] == "P");
    CHECK(heard[2] == "Root");
    CHECK_FALSE(a.valid());
    d->update(0.016f);
}

TEST_CASE(
    "reentrancy: a target listener removes its parent; the parent is skipped, the grandparent hears it") {
    CompileResult r = compileXt(doc(kNestedStyle, R"X(<div id="P"><div id="A"></div></div>)X"));
    std::unique_ptr<Document> d = loadStandalone(r);
    ElementRef a = d->byId("A");
    ElementRef p = d->byId("P");
    REQUIRE(a.valid());
    REQUIRE(p.valid());
    std::vector<std::string> heard;
    d->addListener(a, EventType::Click, [&](Event&) {
        heard.emplace_back("A");
        d->remove(p);
    });
    d->addListener(p, EventType::Click, [&](Event&) { heard.emplace_back("P"); });
    d->addListener(d->byId("Root"), EventType::Click, [&](Event&) { heard.emplace_back("Root"); });
    clickDoc(*d, 10, 10);
    REQUIRE(heard.size() == 2);
    CHECK(heard[0] == "A");
    CHECK(heard[1] == "Root");
    CHECK_FALSE(p.valid());
    CHECK_FALSE(a.valid());
    d->update(0.016f);
}

// -- C2: Animator notifications --------------------------------------------------------------------

namespace {

constexpr const char* kTransitionStyle = R"X(
  #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px;
       background-color: #000000; transition: background-color 0.1s linear; }
  #B { position: absolute; left: 50px; top: 0px; width: 40px; height: 40px;
       background-color: #000000; transition: background-color 0.3s linear; }
  #A.on, #B.on { background-color: #ffffff; }
)X";

}  // namespace

TEST_CASE("reentrancy: a transitionend listener removes its element (the only transition)") {
    CompileResult r = compileXt(doc(kTransitionStyle, "<div id=\"A\"></div>"));
    std::unique_ptr<Document> d = loadStandalone(r);
    ElementRef a = d->byId("A");
    REQUIRE(a.valid());
    int ends = 0;
    int rootEnds = 0;
    d->addListener(a, EventType::TransitionEnd, [&](Event&) {
        ++ends;
        d->remove(a);
    });
    d->addListener(d->byId("Root"), EventType::TransitionEnd, [&](Event&) { ++rootEnds; });
    a.addClass("on");
    d->update(0.01f);  // The transition starts.
    d->update(0.2f);   // ... and ends: the listener removes #A.
    CHECK(ends == 1);
    CHECK(rootEnds == 1);  // The recorded path still bubbles past the destroyed target.
    CHECK_FALSE(a.valid());
    CHECK(d->animator().transitionCount() == 0);
    d->update(0.3f);
    CHECK(ends == 1);
}

TEST_CASE("reentrancy: removing one element in its transitionend keeps another element's transition") {
    CompileResult r = compileXt(doc(kTransitionStyle, "<div id=\"A\"></div><div id=\"B\"></div>"));
    std::unique_ptr<Document> d = loadStandalone(r);
    ElementRef a = d->byId("A");
    ElementRef b = d->byId("B");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    int aEnds = 0;
    int bEnds = 0;
    d->addListener(a, EventType::TransitionEnd, [&](Event&) {
        ++aEnds;
        d->remove(a);
    });
    d->addListener(b, EventType::TransitionEnd, [&](Event&) { ++bEnds; });
    a.addClass("on");
    b.addClass("on");
    d->update(0.01f);
    d->update(0.15f);  // #A ends (and is removed); #B is half way.
    CHECK(aEnds == 1);
    CHECK_FALSE(a.valid());
    CHECK(d->animator().transitionCount() == 1);
    d->update(0.5f);  // #B ends.
    CHECK(bEnds == 1);
    CHECK(b.property(prop::BackgroundColor).c == Color{1, 1, 1, 1});
}

namespace {

/// Opacity of scene-local node 1, 0 -> 1 over one second, with a marker at 0.5 s.
Timeline makeMarkedTimeline() {
    Timeline tl;
    tl.duration = 1.0f;
    tl.keyframes.push_back(Keyframe{0.0f, Value::ofFloat(0.0f), Easing::linear(), {}, {}, 0});
    tl.keyframes.push_back(Keyframe{1.0f, Value::ofFloat(1.0f), Easing::linear(), {}, {}, 0});
    tl.tracks.push_back(Track{1, prop::Opacity, ValueType::Float, 0, 2, 0});
    tl.markers.push_back(Marker{7, 0.5f, 0.0f});
    return tl;
}

}  // namespace

TEST_CASE("reentrancy: Animator iteration / marker callbacks play and stop instances") {
    NodeArena arena;
    const NodeId n0 = arena.create(NodeKind::Group);
    const NodeId n1 = arena.create(NodeKind::Group);
    const NodeId map[] = {n0, n1};
    Timeline tl = makeMarkedTimeline();
    Animator anim;
    PlayParams pp;
    pp.timeline = anim.registerTimeline(&tl);
    pp.localToArena = map;
    pp.localCount = 2;
    pp.loop = LoopMode::Loop;
    pp.iterations = 0;

    // Document::remove() reaches the Animator as clearNode(): a transition-end callback that clears
    // its node must neither pop an empty list nor drop another node's transition.
    auto transition = [](NodeId node, float duration) {
        Transition tr;
        tr.node = node;
        tr.prop = prop::Opacity;
        tr.from = Value::ofFloat(0.0f);
        tr.to = Value::ofFloat(1.0f);
        tr.startClock = 0.0f;
        tr.delay = 0.0f;
        tr.duration = duration;
        tr.easing = Easing::linear();
        return tr;
    };

    SUBCASE("a transition-end callback clears its node (the only transition)") {
        anim.addTransition(transition(n0, 0.5f));
        std::vector<NodeId> ended;
        anim.onTransitionEnd = [&](NodeId node, PropId) {
            ended.push_back(node);
            anim.clearNode(node);
        };
        anim.evaluate(0.6f, arena);
        REQUIRE(ended.size() == 1);
        CHECK(ended[0] == n0);
        CHECK(anim.transitionCount() == 0);
        CHECK(getProperty(arena, n0, prop::Opacity).f == doctest::Approx(1.0f));
    }

    SUBCASE("a transition-end callback clearing its node keeps another node's transition") {
        anim.addTransition(transition(n0, 0.5f));
        anim.addTransition(transition(n1, 1.0f));
        std::vector<NodeId> ended;
        anim.onTransitionEnd = [&](NodeId node, PropId) {
            ended.push_back(node);
            anim.clearNode(node);
        };
        anim.evaluate(0.6f, arena);
        REQUIRE(ended.size() == 1);
        CHECK(anim.transitionCount() == 1);  // n1's is still running.
        anim.evaluate(1.1f, arena);
        REQUIRE(ended.size() == 2);
        CHECK(ended[1] == n1);
        CHECK(getProperty(arena, n1, prop::Opacity).f == doctest::Approx(1.0f));
    }

    SUBCASE("an iteration callback stops both instances") {
        const uint32_t a = anim.play(pp, 0.0f);
        const uint32_t b = anim.play(pp, 0.0f);
        std::vector<uint32_t> seen;
        anim.onIteration = [&](uint32_t id, uint32_t) {
            seen.push_back(id);
            anim.stop(a);
            anim.stop(b);
        };
        anim.evaluate(1.5f, arena);
        REQUIRE(seen.size() == 1);  // Both wrapped; b's notification is stale once a's stopped it.
        CHECK(seen[0] == a);
        CHECK(anim.activeInstanceCount() == 0);
        anim.evaluate(2.5f, arena);
        CHECK(seen.size() == 1);
    }

    SUBCASE("the first iteration callback stops the second instance: its notification is dropped") {
        const uint32_t a = anim.play(pp, 0.0f);
        const uint32_t b = anim.play(pp, 0.0f);
        std::vector<uint32_t> seen;
        anim.onIteration = [&](uint32_t id, uint32_t) {
            seen.push_back(id);
            if (id == a) {
                anim.stop(b);
            }
        };
        anim.evaluate(1.5f, arena);
        REQUIRE(seen.size() == 1);
        CHECK(seen[0] == a);
        CHECK(anim.activeInstanceCount() == 1);
    }

    SUBCASE("a marker callback plays new instances") {
        anim.play(pp, 0.0f);
        anim.evaluate(0.4f, arena);
        int markers = 0;
        anim.onMarker = [&](uint32_t, StringId) {
            ++markers;
            for (int i = 0; i < 8; ++i) {
                anim.play(pp, 0.6f);  // Grows m_instances while evaluate() would be walking it.
            }
        };
        anim.evaluate(0.6f, arena);
        CHECK(markers == 1);
        CHECK(anim.activeInstanceCount() == 9);
        CHECK(getProperty(arena, n1, prop::Opacity).f == doctest::Approx(0.6f));
        anim.evaluate(0.7f, arena);
        CHECK(markers == 1);
    }
}
