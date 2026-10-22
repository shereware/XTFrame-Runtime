#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/types.h"

/// Focus/keyboard/editing tests (Docs/tasks/T-33.md "測試", `test_focus_keyboard.cpp`): the focus
/// model and its events, the Tab ring, keyboard dispatch, button activation, checkbox toggling,
/// `input[type=text]` editing and the caret/selection paint commands. Fixtures are
/// Tests/layout/focus_basic.xt and Tests/layout/focus_input.xt, compiled through the real
/// pipeline (see test_events.cpp's loadDoc pattern) with FakeTextShaper's monospace metrics
/// (at font-size 10: 6px per ASCII glyph, 12px line height).
///
/// See Docs/design/08-events.md §8.4-8.5, §8.8.

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

InputEvent pointerDown(Vec2 pos, uint8_t modifiers = 0) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = pos;
    ev.modifiers = modifiers;
    return ev;
}

InputEvent pointerUp(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerUp;
    ev.pos = pos;
    return ev;
}

InputEvent keyDown(KeyCode key, uint8_t modifiers = 0, uint8_t repeat = 0) {
    InputEvent ev;
    ev.kind = InputEvent::KeyDown;
    ev.key = key;
    ev.modifiers = modifiers;
    ev.repeat = repeat;
    return ev;
}

InputEvent keyUp(KeyCode key) {
    InputEvent ev;
    ev.kind = InputEvent::KeyUp;
    ev.key = key;
    return ev;
}

InputEvent textInput(const char* utf8) {
    InputEvent ev;
    ev.kind = InputEvent::TextInput;
    size_t length = std::strlen(utf8);
    REQUIRE(length < sizeof(ev.utf8));
    std::memcpy(ev.utf8, utf8, length);
    ev.utf8Len = static_cast<uint8_t>(length);
    return ev;
}

/// A full click (down + up) at `pos`.
void click(Document& doc, Vec2 pos) {
    doc.dispatchInput(pointerDown(pos));
    doc.dispatchInput(pointerUp(pos));
}

/// Number of FillPath commands in `list` painted with exactly `color`.
int countFills(const CommandList& list, Color color) {
    int count = 0;
    for (const Command& cmd : list.commands) {
        if (cmd.kind == CmdKind::FillPath && list.fills[cmd.index].paint.color == color) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("Focus: nothing focused after load; PointerDown focuses, :focus applies, a plain div blurs") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_basic.xt", &shaper, &assets);

    ElementRef btn = doc->byId("btn");
    REQUIRE(btn.valid());
    CHECK_FALSE(doc->focus().valid());
    CHECK(doc->arena().element(btn.node).style.base[prop::BackgroundColor].c == Color{0, 0, 0, 1});

    int focusCount = 0;
    int blurCount = 0;
    doc->addListener(btn, EventType::Focus, [&](Event& ev) {
        ++focusCount;
        CHECK(ev.target == btn.node);
    });
    doc->addListener(btn, EventType::Blur, [&](Event&) { ++blurCount; });

    doc->dispatchInput(pointerDown(Vec2{10, 10}));
    CHECK(focusCount == 1);
    CHECK(btn.hasState(kStateFocus));
    CHECK(doc->focus().node == btn.node);
    doc->update(0.0f);
    CHECK(doc->arena().element(btn.node).style.base[prop::BackgroundColor].c == Color{1, 1, 1, 1});
    doc->dispatchInput(pointerUp(Vec2{10, 10}));

    // 08.4: a PointerDown with no focusable element at or above the hit clears the focus.
    doc->dispatchInput(pointerDown(Vec2{220, 220}));  // #plain, a div.
    CHECK(blurCount == 1);
    CHECK_FALSE(btn.hasState(kStateFocus));
    CHECK_FALSE(doc->focus().valid());
}

TEST_CASE("Focus: setFocus API, and preventDefault on PointerDown keeps the focus put") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_basic.xt", &shaper, &assets);

    ElementRef btn = doc->byId("btn");
    ElementRef field = doc->byId("field");
    REQUIRE(btn.valid());
    REQUIRE(field.valid());

    doc->setFocus(btn);
    CHECK(doc->focus().node == btn.node);
    doc->setFocus(ElementRef{});
    CHECK_FALSE(doc->focus().valid());

    doc->setFocus(field);
    doc->addListener(btn, EventType::PointerDown, [](Event& ev) { ev.preventDefault = true; });
    doc->dispatchInput(pointerDown(Vec2{10, 10}));
    CHECK(doc->focus().node == field.node);  // Unchanged: the default focus action was prevented.
}

TEST_CASE("Tab: document-order ring, skipping disabled and display:none elements; Shift+Tab reverses") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_basic.xt", &shaper, &assets);

    ElementRef btn = doc->byId("btn");
    ElementRef field = doc->byId("field");
    ElementRef check = doc->byId("check");
    ElementRef offBtn = doc->byId("offBtn");
    REQUIRE(offBtn.valid());
    CHECK(offBtn.setState(kStateDisabled, true));

    doc->dispatchInput(keyDown(KeyCode::Tab));  // No focus yet -> the first focusable element.
    CHECK(doc->focus().node == btn.node);
    doc->dispatchInput(keyDown(KeyCode::Tab));
    CHECK(doc->focus().node == field.node);
    doc->dispatchInput(keyDown(KeyCode::Tab));
    CHECK(doc->focus().node == check.node);
    // #offBtn is disabled and #hiddenField is display:none, so the ring wraps straight back.
    doc->dispatchInput(keyDown(KeyCode::Tab));
    CHECK(doc->focus().node == btn.node);

    doc->dispatchInput(keyDown(KeyCode::Tab, kShift));
    CHECK(doc->focus().node == check.node);
    doc->dispatchInput(keyDown(KeyCode::Tab, kShift));
    CHECK(doc->focus().node == field.node);
}

TEST_CASE("KeyDown/KeyUp: dispatched to the focus and bubbling; to the root when nothing is focused") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_basic.xt", &shaper, &assets);

    ElementRef root = doc->root();
    ElementRef body = doc->byId("body");
    ElementRef btn = doc->byId("btn");

    std::vector<std::string> order;
    int rootKeys = 0;
    int keyUps = 0;
    doc->addListener(btn, EventType::KeyDown, [&](Event& ev) {
        order.push_back("btn");
        CHECK(ev.key == KeyCode::KeyB);
        CHECK(ev.target == btn.node);
    });
    doc->addListener(body, EventType::KeyDown, [&](Event&) { order.push_back("body"); });
    doc->addListener(root, EventType::KeyDown, [&](Event& ev) {
        ++rootKeys;
        if (order.empty()) {
            CHECK(ev.target == root.node);  // No focus: the root is the target.
        }
    });
    doc->addListener(btn, EventType::KeyUp, [&](Event&) { ++keyUps; });

    doc->dispatchInput(keyDown(KeyCode::KeyB));  // Nothing focused yet.
    CHECK(order.empty());
    CHECK(rootKeys == 1);

    doc->setFocus(btn);
    doc->dispatchInput(keyDown(KeyCode::KeyB));
    CHECK(order == std::vector<std::string>{"btn", "body"});
    CHECK(rootKeys == 2);  // Bubbled all the way up.
    doc->dispatchInput(keyUp(KeyCode::KeyB));
    CHECK(keyUps == 1);
}

TEST_CASE("button: Enter and Space fire Click; a repeat does not") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_basic.xt", &shaper, &assets);

    ElementRef btn = doc->byId("btn");
    int clicks = 0;
    doc->addListener(btn, EventType::Click, [&](Event&) { ++clicks; });
    doc->setFocus(btn);

    doc->dispatchInput(keyDown(KeyCode::Enter));
    CHECK(clicks == 1);
    doc->dispatchInput(keyDown(KeyCode::Space));
    CHECK(clicks == 2);
    doc->dispatchInput(keyDown(KeyCode::Enter, 0, /*repeat=*/1));
    CHECK(clicks == 2);
    // preventDefault on the KeyDown suppresses the activation.
    doc->addListener(btn, EventType::KeyDown, [](Event& ev) { ev.preventDefault = true; });
    doc->dispatchInput(keyDown(KeyCode::Enter));
    CHECK(clicks == 2);
}

TEST_CASE("checkbox: click and Space toggle kStateChecked and fire Change; disabled does nothing") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_basic.xt", &shaper, &assets);

    ElementRef check = doc->byId("check");
    REQUIRE(check.valid());
    int changes = 0;
    doc->addListener(check, EventType::Change, [&](Event& ev) {
        ++changes;
        CHECK(ev.target == check.node);
    });

    click(*doc, Vec2{10, 85});  // Inside #check (0,80)-(20,100).
    CHECK(check.hasState(kStateChecked));
    CHECK(changes == 1);
    CHECK(doc->focus().node == check.node);

    doc->dispatchInput(keyDown(KeyCode::Space));
    CHECK_FALSE(check.hasState(kStateChecked));
    CHECK(changes == 2);

    CHECK(check.setState(kStateDisabled, true));
    doc->dispatchInput(keyDown(KeyCode::Space));
    CHECK_FALSE(check.hasState(kStateChecked));
    CHECK(changes == 2);
    click(*doc, Vec2{10, 85});
    CHECK(changes == 2);
}

TEST_CASE("input[type=text]: TextInput, Backspace, Left, mouse caret placement, setValue") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_input.xt", &shaper, &assets);

    ElementRef field = doc->byId("field");
    ElementRef preset = doc->byId("preset");
    REQUIRE(field.valid());
    CHECK(field.value().empty());
    CHECK(preset.value() == "xy");  // Initialized from the `value` attribute.
    CHECK_FALSE(doc->wantsTextInput());

    int changes = 0;
    doc->addListener(field, EventType::Change, [&](Event&) { ++changes; });

    doc->dispatchInput(pointerDown(Vec2{5, 5}));  // Inside #field (0,0)-(200,30).
    doc->dispatchInput(pointerUp(Vec2{5, 5}));
    CHECK(doc->focus().node == field.node);
    CHECK(doc->wantsTextInput());

    doc->dispatchInput(textInput("a"));
    doc->dispatchInput(textInput("b"));
    CHECK(field.value() == "ab");
    CHECK(changes == 2);

    doc->dispatchInput(keyDown(KeyCode::Backspace));
    CHECK(field.value() == "a");
    CHECK(changes == 3);
    doc->update(0.0f);  // Relayout, so the caret moves over the new shaping result.

    doc->dispatchInput(keyDown(KeyCode::Left));
    doc->dispatchInput(textInput("z"));
    CHECK(field.value() == "za");  // Inserted before the caret's grapheme.
    doc->update(0.0f);

    // A click past the end of the text puts the caret at the end (07.7 + 08.4).
    click(*doc, Vec2{150, 10});
    doc->dispatchInput(textInput("!"));
    CHECK(field.value() == "za!");
    doc->update(0.0f);

    // A click at the very start puts it at offset 0.
    click(*doc, Vec2{0, 10});
    doc->dispatchInput(textInput("^"));
    CHECK(field.value() == "^za!");

    int before = changes;
    field.setValue("hello");
    CHECK(field.value() == "hello");
    CHECK(changes == before + 1);
    field.setValue("hello");  // Unchanged -> no Change.
    CHECK(changes == before + 1);

    // Home/End/Ctrl+A stay inside the value; Enter inserts nothing (single line).
    doc->update(0.0f);
    doc->dispatchInput(keyDown(KeyCode::Enter));
    CHECK(field.value() == "hello");
    doc->dispatchInput(keyDown(KeyCode::Home));
    doc->dispatchInput(textInput("-"));
    CHECK(field.value() == "-hello");
    doc->update(0.0f);
    doc->dispatchInput(keyDown(KeyCode::KeyA, kCtrl));
    doc->dispatchInput(textInput("+"));
    CHECK(field.value() == "+");  // Ctrl+A selected everything, the insert replaced it.

    // Focus elsewhere: the host should stop platform text input.
    doc->dispatchInput(pointerDown(Vec2{10, 110}));  // #plain.
    CHECK_FALSE(doc->wantsTextInput());
}

TEST_CASE("input[type=text]: hover reports Cursor::Text, and TextInput without focus is dropped") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_input.xt", &shaper, &assets);

    ElementRef field = doc->byId("field");
    InputEvent move;
    move.kind = InputEvent::PointerMove;
    move.pos = Vec2{5, 5};
    doc->dispatchInput(move);
    CHECK(doc->cursor() == Cursor::Text);

    doc->dispatchInput(textInput("a"));
    CHECK(field.value().empty());
}

TEST_CASE("Painter: caret blinks on a 1s period, selection paints behind the text") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_input.xt", &shaper, &assets);

    ElementRef field = doc->byId("field");
    doc->setFocus(field);
    field.setValue("ab");
    doc->update(0.2f);

    const Color caretColor{1, 0, 0, 1};  // #field's `color: #ff0000`.
    const Color selectionColor{51.0f / 255.0f, 153.0f / 255.0f, 255.0f / 255.0f, 0.35f};

    CommandList visible;
    doc->paint(visible);
    CHECK(countFills(visible, caretColor) == 1);
    CHECK(countFills(visible, selectionColor) == 0);

    doc->update(0.7f);  // Second half of the blink period.
    CommandList blinkedOff;
    doc->paint(blinkedOff);
    CHECK(countFills(blinkedOff, caretColor) == 0);

    doc->dispatchInput(keyDown(KeyCode::KeyA, kCtrl));  // Select all.
    doc->update(0.2f);
    CommandList selected;
    doc->paint(selected);
    CHECK(countFills(selected, caretColor) == 1);
    CHECK(countFills(selected, selectionColor) == 1);

    // An unfocused input paints neither.
    doc->setFocus(ElementRef{});
    doc->update(0.2f);
    CommandList unfocused;
    doc->paint(unfocused);
    CHECK(countFills(unfocused, caretColor) == 0);
    CHECK(countFills(unfocused, selectionColor) == 0);
}

TEST_CASE("Focus is dropped on the next update when its element stops being focusable or dies") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("focus_basic.xt", &shaper, &assets);

    ElementRef btn = doc->byId("btn");
    int blurs = 0;
    doc->addListener(btn, EventType::Blur, [&](Event&) { ++blurs; });
    doc->setFocus(btn);
    REQUIRE(doc->focus().node == btn.node);

    // Still alive but no longer focusable: it gets its Blur.
    CHECK(btn.setState(kStateDisabled, true));
    doc->update(0.0f);
    CHECK(blurs == 1);
    CHECK_FALSE(doc->focus().valid());
    CHECK_FALSE(btn.hasState(kStateFocus));

    // Removed outright: the focus is simply dropped (the element's listeners went with it).
    ElementRef field = doc->byId("field");
    doc->setFocus(field);
    REQUIRE(doc->focus().node == field.node);
    doc->arena().destroy(field.node);
    doc->update(0.0f);
    CHECK_FALSE(doc->focus().valid());
}
