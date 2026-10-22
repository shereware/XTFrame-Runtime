#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/types.h"

/// Cross-module event integration tests (Docs/tasks/T-36.md "test_event_integration.cpp"): the 10
/// scenarios that exercise layout + hit testing + style + animation + focus together, none of
/// which is covered by a single-module suite (test_events.cpp, test_focus_keyboard.cpp,
/// test_style_animation_integration.cpp). Documents are built from inline `.xt` source strings
/// (Docs/tasks/T-36.md: "文件以 compileDocument 從字串建立"); compileDocument() itself only takes
/// a path, so, like test_layout_text_node.cpp's writeTempXt(), each source string is written to a
/// uniquely named temp file first.
///
/// See Docs/design/08-events.md §8.4-8.6, Docs/design/06-layout.md §6.11,
/// Docs/design/05-style-and-css.md §5.6-5.7.

using namespace xt;
using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

/// Writes `source` to a uniquely named `.xt` file in the system temp directory and returns its
/// path (kept around deliberately, like test_layout_text_node.cpp's writeTempXt()).
std::string writeTempXt(const std::string& name, const std::string& source) {
    fs::path dir = xt::test::tempDir() / "xt_t36_event_integration";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path path = dir / (name + ".xt");
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    REQUIRE(file != nullptr);
    std::fwrite(source.data(), 1, source.size(), file);
    std::fclose(file);
    return path.string();
}

/// Compiles `source` (a full `<xt>...</xt>` document), loads it with `shaper`, sets a 400x300
/// viewport and runs one update(0).
std::unique_ptr<Document> loadDocFromString(const std::string& name, const std::string& source,
                                            TextShaper& shaper) {
    std::string path = writeTempXt(name, source);
    CompileOptions options;
    CompileResult result = compileDocument(path, options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

std::string document(const std::string& css, const std::string& body) {
    return "<xt>\n<head>\n<style>\n" + css + "\n</style>\n</head>\n<body>\n" + body + "\n</body>\n</xt>\n";
}

InputEvent pointerMove(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = pos;
    return ev;
}

InputEvent pointerDown(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = pos;
    return ev;
}

InputEvent pointerUp(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerUp;
    ev.pos = pos;
    return ev;
}

InputEvent wheel(Vec2 pos, Vec2 delta) {
    InputEvent ev;
    ev.kind = InputEvent::Wheel;
    ev.pos = pos;
    ev.wheelDelta = delta;
    return ev;
}

InputEvent keyDown(KeyCode key, uint8_t modifiers = 0) {
    InputEvent ev;
    ev.kind = InputEvent::KeyDown;
    ev.key = key;
    ev.modifiers = modifiers;
    return ev;
}

InputEvent textInput(const char* utf8) {
    InputEvent ev;
    ev.kind = InputEvent::TextInput;
    std::string s(utf8);
    REQUIRE(s.size() < sizeof(ev.utf8));
    std::memcpy(ev.utf8, s.data(), s.size());
    ev.utf8Len = static_cast<uint8_t>(s.size());
    return ev;
}

void click(Document& doc, Vec2 pos) {
    doc.dispatchInput(pointerDown(pos));
    doc.dispatchInput(pointerUp(pos));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// 1. Scroll container hit testing.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: clicking a scrolled-into-view element hits it; the pre-scroll spot misses") {
    NullTextShaper shaper;
    // #box (100x100, overflow: scroll) holds #content (80x300): a 100px filler that exactly covers
    // the unscrolled viewport window, then #btn (30px), then a 170px filler. Before scrolling,
    // #btn (content y 100-130) is entirely outside the 100px-tall viewport window; scrolling #box
    // down by 80px (wheel wheelDelta.y=2 * 40 = 80, Docs/design/08-events.md §8.4) brings it into
    // view at viewport y 20-50.
    std::string src = document(
        "#box { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; overflow: scroll; }"
        "#content { width: 80px; height: 300px; }"
        "#filler1 { width: 80px; height: 100px; }"
        "#btn { width: 80px; height: 30px; }"
        "#filler2 { width: 80px; height: 170px; }",
        "<div id=\"box\"><div id=\"content\">"
        "<div id=\"filler1\"></div><div id=\"btn\"></div><div id=\"filler2\"></div>"
        "</div></div>");
    std::unique_ptr<Document> doc = loadDocFromString("scroll_hit", src, shaper);

    ElementRef box = doc->byId("box");
    ElementRef btn = doc->byId("btn");
    REQUIRE(box.valid());
    REQUIRE(btn.valid());

    int btnClicks = 0;
    doc->addListener(btn, EventType::Click, [&](Event&) { ++btnClicks; });

    // Viewport y=35: at scrollTop=0 this is content y=35 (#filler1), so it misses #btn.
    click(*doc, Vec2{40, 35});
    CHECK(btnClicks == 0);
    // Resolve the hover/active state churn from that click (08.4 sets kStateHover on #box too,
    // as an ancestor of the hit element) before writing ScrollY below: see this test's own
    // "suspected defect" sibling case just below -- an unresolved kDirtyStyle on #box would make
    // the *next* style resolve reset ScrollY back to its CSS default (0).
    doc->update(0.0f);

    doc->dispatchInput(wheel(Vec2{40, 50}, Vec2{0, 2}));  // ScrollY += 2*40 = 80.
    CHECK(box.property(prop::ScrollY).f == doctest::Approx(80.0f));
    // ScrollY is now kDirtyLayout (see test_events.cpp's wheel test): relayout to recompute the
    // scrolled children's world transform before hit-testing against it.
    doc->update(0.0f);

    // The same viewport coordinate (40,35) now maps to content y=35+80=115, inside #btn's 100-130,
    // so the identical click now hits it: scrolling moved what's under that pixel.
    click(*doc, Vec2{40, 35});
    CHECK(btnClicks == 1);
}

// Regression (found by T-36): a scroll container's ScrollX/ScrollY are runtime state written
// by the wheel handler, not cascaded values; re-resolving the container's style for an unrelated
// reason (a hover passing through it) must leave them untouched (style_engine.cpp).
TEST_CASE(
    "integration: re-resolving a scroll container's style for an unrelated reason keeps its "
    "ScrollY") {
    NullTextShaper shaper;
    std::string src = document(
        "#box { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; overflow: scroll; }"
        "#content { width: 80px; height: 300px; }",
        "<div id=\"box\"><div id=\"content\"></div></div>");
    std::unique_ptr<Document> doc = loadDocFromString("scroll_hover_reset_defect", src, shaper);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());

    doc->dispatchInput(wheel(Vec2{50, 50}, Vec2{0, 2}));  // ScrollY += 2*40 = 80.
    doc->update(0.0f);
    CHECK(box.property(prop::ScrollY).f == doctest::Approx(80.0f));  // Scroll took effect.

    // A hover that merely passes over #box (as an ancestor of the hit point) marks #box itself
    // kDirtyStyle for :hover-selector re-evaluation, even though #box has no :hover rule at all.
    doc->dispatchInput(pointerMove(Vec2{50, 50}));
    doc->update(0.0f);

    // Scrolling is orthogonal to hover: ScrollY must be untouched.
    CHECK(box.property(prop::ScrollY).f == doctest::Approx(80.0f));
}

// Regression (found by T-36): @keyframes that set `transform: translateX(...)` must produce a
// Float track (transform components are Float scene properties); the emitter used to keep the
// Length value and Animator's setProperty asserted (style_emit.cpp).
TEST_CASE("integration: CSS animation keyframing transform translateX interpolates TranslateX") {
    NullTextShaper shaper;
    std::string src = document(
        "@keyframes slide { from { transform: translateX(0px) } to { transform: translateX(150px) } }"
        "#box { width: 40px; height: 40px; animation: slide 1s linear; }",
        "<div id=\"box\"></div>");
    std::unique_ptr<Document> doc = loadDocFromString("transform_keyframe_crash", src, shaper);
    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    doc->update(0.5f);
    CHECK(box.property(prop::TranslateX).f > 0.0f);
}

// ---------------------------------------------------------------------------------------------
// 2. Rotated element hover + transition.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: hover on a rotated element starts its opacity transition") {
    NullTextShaper shaper;
    std::string src = document(
        "#el { position: absolute; left: 80px; top: 80px; width: 80px; height: 80px;"
        "      background-color: #ff0000; opacity: 1; transform: rotate(45deg);"
        "      transform-origin: 40px 40px; transition: opacity 0.2s linear; }"
        "#el:hover { opacity: 0; }",
        "<div id=\"el\"></div>");
    std::unique_ptr<Document> doc = loadDocFromString("rotate_hover", src, shaper);

    ElementRef el = doc->byId("el");
    REQUIRE(el.valid());
    CHECK(el.property(prop::Opacity).f == doctest::Approx(1.0f));

    // The element's untransformed center (120,120) stays inside the rotated square regardless of
    // rotation, so this pointer position exercises hit-testing through the world transform.
    doc->dispatchInput(pointerMove(Vec2{120, 120}));
    REQUIRE(el.hasState(kStateHover));
    doc->update(0.0f);  // Resolves opacity: 0 and registers the 0.2s linear transition.
    CHECK(doc->animator().transitionCount() == 1);

    doc->update(0.1f);  // Half way through the 0.2s transition: 1 -> 0.5.
    CHECK(el.property(prop::Opacity).f == doctest::Approx(0.5f).epsilon(0.02));
}

// ---------------------------------------------------------------------------------------------
// 3. Dynamically inserted/removed element in the Tab ring.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: an inserted button joins the Tab ring in document order; remove() drops it") {
    NullTextShaper shaper;
    std::string src = document("button { width: 40px; height: 20px; }",
                               "<button id=\"a\">A</button><button id=\"b\">B</button>");
    std::unique_ptr<Document> doc = loadDocFromString("tab_dynamic", src, shaper);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    ElementRef body = doc->root().firstChild();  // <body>, the only child of <xt>.
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(body.valid());
    REQUIRE(body.tag() == "body");

    ElementRef inserted = doc->createElement("button");
    doc->insertBefore(body, inserted, b);  // Document order becomes a, inserted, b.
    doc->update(0.0f);

    doc->dispatchInput(keyDown(KeyCode::Tab));
    CHECK(doc->focus().node == a.node);
    doc->dispatchInput(keyDown(KeyCode::Tab));
    CHECK(doc->focus().node == inserted.node);
    doc->dispatchInput(keyDown(KeyCode::Tab));
    CHECK(doc->focus().node == b.node);

    doc->remove(inserted);
    doc->setFocus(a);
    doc->dispatchInput(keyDown(KeyCode::Tab));  // The removed element is skipped entirely now.
    CHECK(doc->focus().node == b.node);
}

// ---------------------------------------------------------------------------------------------
// 4. Drag semantics: PointerMove keeps targeting the pointer-down element.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: PointerMove after PointerDown keeps dispatching to the active element") {
    NullTextShaper shaper;
    std::string src = document(
        "#a { position: absolute; left: 0px; top: 0px; width: 50px; height: 50px; }"
        "#b { position: absolute; left: 60px; top: 0px; width: 50px; height: 50px; }",
        "<div id=\"a\"></div><div id=\"b\"></div>");
    std::unique_ptr<Document> doc = loadDocFromString("drag", src, shaper);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    REQUIRE(a.valid());
    REQUIRE(b.valid());

    int aMoveCount = 0;
    int bMoveCount = 0;
    int aClickCount = 0;
    int bClickCount = 0;
    doc->addListener(a, EventType::PointerMove, [&](Event&) { ++aMoveCount; });
    doc->addListener(b, EventType::PointerMove, [&](Event&) { ++bMoveCount; });
    doc->addListener(a, EventType::Click, [&](Event&) { ++aClickCount; });
    doc->addListener(b, EventType::Click, [&](Event&) { ++bClickCount; });

    doc->dispatchInput(pointerDown(Vec2{25, 25}));  // Inside #a.
    doc->dispatchInput(pointerMove(Vec2{85, 25}));  // Now over #b, but #a is the active node.
    // 08.4: hover tracking (hoverNode/kStateHover, PointerEnter/Leave) always follows hitTest(pos)
    // regardless of activeNode -- only the PointerMove *event* stays pinned to activeNode (drag
    // semantics). So #b does become hovered here; it just never gets a PointerMove listener call.
    CHECK(aMoveCount == 1);
    CHECK(bMoveCount == 0);
    CHECK(b.hasState(kStateHover));
    CHECK_FALSE(a.hasState(kStateHover));

    doc->dispatchInput(pointerUp(Vec2{85, 25}));  // Released over #b: neither gets a Click.
    CHECK(aClickCount == 0);
    CHECK(bClickCount == 0);
}

// ---------------------------------------------------------------------------------------------
// 5. z-index overrides which of two overlapping absolute elements receives a click.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: the higher z-index element wins overlapping clicks, and setStyle can invert it") {
    NullTextShaper shaper;
    std::string src = document(
        "#hi { position: absolute; left: 0px; top: 0px; width: 80px; height: 80px; z-index: 2; }"
        "#lo { position: absolute; left: 0px; top: 0px; width: 80px; height: 80px; z-index: 1; }",
        "<div id=\"hi\"></div><div id=\"lo\"></div>");
    std::unique_ptr<Document> doc = loadDocFromString("zindex_click", src, shaper);

    ElementRef hi = doc->byId("hi");
    ElementRef lo = doc->byId("lo");
    REQUIRE(hi.valid());
    REQUIRE(lo.valid());

    int hiClicks = 0;
    int loClicks = 0;
    doc->addListener(hi, EventType::Click, [&](Event&) { ++hiClicks; });
    doc->addListener(lo, EventType::Click, [&](Event&) { ++loClicks; });

    click(*doc, Vec2{40, 40});
    CHECK(hiClicks == 1);
    CHECK(loClicks == 0);

    hi.setStyle("z-index: -1");
    doc->update(0.0f);

    click(*doc, Vec2{40, 40});
    CHECK(hiClicks == 1);  // Unchanged.
    CHECK(loClicks == 1);  // Now on top.
}

// ---------------------------------------------------------------------------------------------
// 6. display:none on the focused element clears focus (T-33 rule).
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: hiding the focused element via setStyle blurs and clears focus next frame") {
    NullTextShaper shaper;
    std::string src = document("#btn { width: 40px; height: 20px; }", "<button id=\"btn\">OK</button>");
    std::unique_ptr<Document> doc = loadDocFromString("focus_display_none", src, shaper);

    ElementRef btn = doc->byId("btn");
    REQUIRE(btn.valid());
    doc->setFocus(btn);
    CHECK(doc->focus().node == btn.node);

    int blurCount = 0;
    doc->addListener(btn, EventType::Blur, [&](Event&) { ++blurCount; });

    btn.setStyle("display: none");
    CHECK(blurCount == 0);  // Not yet: takes effect on the next update().
    // T-399 (08 §8.18): Document::update() validates the focus *after* the style / layout pass,
    // so the update() right after setStyle() clears the focus (and sends the Blur).
    doc->update(0.0f);
    CHECK(blurCount == 1);
    CHECK_FALSE(doc->focus().valid());
    doc->update(0.0f);
    CHECK(blurCount == 1);
}

// ---------------------------------------------------------------------------------------------
// 7. remove() from inside an AnimationEnd callback is deferred to the next frame.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: AnimationEnd is safe to observe and act on; remove() happens next frame") {
    // Not covered by Docs/tasks/T-36.md: whether remove() is safe to call directly from inside an
    // AnimationEnd callback (dispatched mid-Document::update(), see document.cpp's
    // handleAnimationFinished -> dispatchAnimationEvent) is unspecified by T-35's design. Per
    // T-36's own fallback instruction ("若 T-35 的實作要求延後，於回呼中記錄後在下一幀 remove"),
    // this test only *records* the element in the callback and calls remove() from ordinary test
    // code on the following frame, which is unambiguously safe.
    NullTextShaper shaper;
    std::string src = document(
        "@keyframes fade { from { opacity: 1 } to { opacity: 0 } }"
        "#el { width: 10px; height: 10px; animation: fade 0.1s linear; }",
        "<div id=\"el\"></div>");
    std::unique_ptr<Document> doc = loadDocFromString("anim_end_remove", src, shaper);

    ElementRef el = doc->byId("el");
    REQUIRE(el.valid());
    uint32_t aliveBefore = doc->arena().aliveCount();

    bool ended = false;
    doc->addListener(el, EventType::AnimationEnd, [&](Event&) { ended = true; });

    doc->update(0.05f);  // Document::update(clock) takes an absolute clock, not a delta.
    CHECK_FALSE(ended);
    doc->update(0.11f);  // Crosses the 0.1s duration: AnimationEnd fires here.
    CHECK(ended);

    doc->remove(el);  // Deferred to "next frame" (i.e. after the update() that fired the event).
    CHECK(doc->arena().aliveCount() == aliveBefore - 1);
    CHECK_FALSE(doc->byId("el").valid());

    doc->update(0.0f);  // No crash with the element gone.
}

// ---------------------------------------------------------------------------------------------
// 8. Wheel scrolling clamps at the bottom and stays clamped.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: wheel scrolling clamps to scrollHeight - clientHeight and stays put") {
    NullTextShaper shaper;
    std::string src = document(
        "#box { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; overflow: scroll; }"
        "#content { width: 80px; height: 300px; }",
        "<div id=\"box\"><div id=\"content\"></div></div>");
    std::unique_ptr<Document> doc = loadDocFromString("wheel_clamp", src, shaper);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());

    // scrollHeight(300) - clientHeight(100) = 200: any delta at or beyond that clamps ScrollY.
    doc->dispatchInput(wheel(Vec2{50, 50}, Vec2{0, 100}));  // 100*40 = 4000, clamped to 200.
    CHECK(box.property(prop::ScrollY).f == doctest::Approx(200.0f));

    doc->dispatchInput(wheel(Vec2{50, 50}, Vec2{0, 5}));  // Already at the bottom.
    CHECK(box.property(prop::ScrollY).f == doctest::Approx(200.0f));
}

// ---------------------------------------------------------------------------------------------
// 9. checkbox click: one Change event, and the :checked style applies.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: clicking a checkbox dispatches one Change and applies :checked's style") {
    NullTextShaper shaper;
    std::string src = document(
        "#chk { position: absolute; left: 0px; top: 0px; width: 20px; height: 20px;"
        "       background-color: #cccccc; }"
        "#chk:checked { background-color: #00aa00; }",
        "<input id=\"chk\" type=\"checkbox\" />");
    std::unique_ptr<Document> doc = loadDocFromString("checkbox_change", src, shaper);

    ElementRef chk = doc->byId("chk");
    REQUIRE(chk.valid());
    float cc = static_cast<float>(0xCC) / 255.0f;
    CHECK(chk.property(prop::BackgroundColor).c == Color{cc, cc, cc, 1.0f});

    int changeCount = 0;
    doc->addListener(chk, EventType::Change, [&](Event&) { ++changeCount; });

    click(*doc, Vec2{10, 10});
    CHECK(changeCount == 1);
    CHECK(chk.hasState(kStateChecked));
    doc->update(0.0f);
    CHECK(chk.property(prop::BackgroundColor).c ==
          Color{0.0f, static_cast<float>(0xAA) / 255.0f, 0.0f, 1.0f});
}

// ---------------------------------------------------------------------------------------------
// 10. Typing into a fixed-width input keeps its layout box size; the caret x is monotonic.
// ---------------------------------------------------------------------------------------------
TEST_CASE("integration: typing into a fixed-width input doesn't change its layout box; caret x grows") {
    FakeTextShaper shaper;
    std::string src = document("#inp { width: 200px; height: 20px; font-size: 10px; }",
                               "<input id=\"inp\" type=\"text\" />");
    std::unique_ptr<Document> doc = loadDocFromString("input_caret_grows", src, shaper);

    ElementRef inp = doc->byId("inp");
    REQUIRE(inp.valid());
    inp.focus();
    doc->update(0.0f);

    LayoutBox before = inp.layoutBox();
    float lastCaretX = -1.0f;
    const char* chars[10] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"};
    for (const char* ch : chars) {
        doc->dispatchInput(textInput(ch));
        doc->update(0.0f);  // Relayout, so textInputRect() sees the new shaping result.
        Rect caret = doc->textInputRect();
        CHECK(caret.x > lastCaretX);  // Monotonically increasing (FakeTextShaper: 0.6em/glyph).
        lastCaretX = caret.x;
    }

    LayoutBox after = inp.layoutBox();
    CHECK(after.width == doctest::Approx(before.width));
}
