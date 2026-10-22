#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "composition.h"
#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/types.h"

/// IME composition tests (Docs/tasks/T-33b.md "測試", `test_composition.cpp`): the display string
/// while composing, the in-composition caret, the underline paint command, the state after
/// CompositionEnd, the committing TextInput, KeyDown suppression, preventDefault and
/// textInputRect(). Uses Tests/layout/focus_input.xt with FakeTextShaper's monospace metrics (at
/// font-size 10: 6px per ASCII glyph, 12px line height).
///
/// See Docs/design/08-events.md §8.8, Docs/design/07-text.md §7.6.

using namespace xt;
using namespace xt::compile;

namespace {

std::unique_ptr<Document> loadDoc(TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(std::string(XT_TEST_LAYOUT_DIR) + "/focus_input.xt", options);
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

InputEvent compStart() {
    InputEvent ev;
    ev.kind = InputEvent::CompositionStart;
    return ev;
}

InputEvent compUpdate(std::string_view text, uint32_t cursor, uint32_t selLen = 0) {
    InputEvent ev;
    ev.kind = InputEvent::CompositionUpdate;
    ev.composition = text;
    ev.compositionCursor = cursor;
    ev.compositionSelLen = selLen;
    return ev;
}

InputEvent compEnd() {
    InputEvent ev;
    ev.kind = InputEvent::CompositionEnd;
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

InputEvent keyDown(KeyCode key) {
    InputEvent ev;
    ev.kind = InputEvent::KeyDown;
    ev.key = key;
    return ev;
}

/// The string the element's Text node was actually laid out from, reconstructed from its shaping
/// cache's byte length and the runs' clusters is fragile; instead this reads the display string
/// the layout used through TextNodeData::shaped's graphemeStart size plus CompositionState, which
/// is what the tests below actually care about: the *displayed* text.
std::string displayString(const Document& doc, ElementRef element) {
    const CompositionState& comp = doc.compositionState();
    if (comp.active && comp.element == element.node) {
        return comp.display;
    }
    return std::string(element.value());
}

/// The byte length the Text node was last shaped with, i.e. the length of the string layout saw.
size_t shapedLength(const Document& doc, ElementRef element) {
    NodeId textNode = kInvalidNode;
    doc.arena().forEachChild(element.node, [&](NodeId child) {
        if (textNode == kInvalidNode && doc.arena().kind(child) == NodeKind::Text) {
            textNode = child;
        }
    });
    REQUIRE(textNode != kInvalidNode);
    return doc.arena().text(textNode).shaped.graphemeStart.size();
}

/// Sets `field`'s value and places the caret `back` graphemes from the end (Left presses).
void setValueWithCaret(Document& doc, ElementRef field, std::string_view value, int back) {
    field.setValue(value);
    doc.update(0.0f);  // Left moves visually, so the value has to be shaped/laid out first.
    for (int i = 0; i < back; ++i) {
        doc.dispatchInput(keyDown(KeyCode::Left));
    }
    doc.update(0.0f);
}

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

TEST_CASE("Composition: display string, no Change, End restores the value, TextInput commits") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    ElementRef field = doc->byId("field");
    REQUIRE(field.valid());
    doc->setFocus(field);
    setValueWithCaret(*doc, field, "abcd", 2);
    doc->update(0.0f);

    int changes = 0;
    doc->addListener(field, EventType::Change, [&changes](Event&) { ++changes; });

    doc->dispatchInput(compStart());
    CHECK(doc->compositionState().active);
    CHECK(doc->compositionState().anchorByte == 2);
    CHECK(changes == 0);

    doc->dispatchInput(compUpdate("ni", 2));
    doc->update(0.0f);
    CHECK(displayString(*doc, field) == "abnicd");
    CHECK(shapedLength(*doc, field) == 6);
    CHECK(field.value() == "abcd");  // §8.8: the value itself never changes while composing.
    CHECK(changes == 0);

    // A second update replaces the composition (here with a 3-byte CJK codepoint).
    doc->dispatchInput(compUpdate("\xE4\xBD\xA0", 3));
    doc->update(0.0f);
    CHECK(displayString(*doc, field) ==
          "ab\xE4\xBD\xA0"
          "cd");
    CHECK(shapedLength(*doc, field) == 7);
    CHECK(changes == 0);

    doc->dispatchInput(compEnd());
    doc->update(0.0f);
    CHECK_FALSE(doc->compositionState().active);
    CHECK(doc->compositionState().text.empty());
    CHECK(displayString(*doc, field) == "abcd");
    CHECK(shapedLength(*doc, field) == 4);
    CHECK(changes == 0);

    // The platform's committing TextInput is what actually inserts, at the composition's anchor.
    doc->dispatchInput(textInput("\xE4\xBD\xA0"));
    doc->update(0.0f);
    CHECK(field.value() ==
          "ab\xE4\xBD\xA0"
          "cd");
    CHECK(changes == 1);
}

TEST_CASE("Composition: the three events bubble") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    ElementRef field = doc->byId("field");
    ElementRef body = doc->byId("body");
    REQUIRE(field.valid());
    REQUIRE(body.valid());
    doc->setFocus(field);

    int start = 0;
    int update = 0;
    int end = 0;
    std::string seen;
    doc->addListener(body, EventType::CompositionStart, [&](Event& ev) {
        ++start;
        CHECK(ev.target == field.node);
    });
    doc->addListener(body, EventType::CompositionUpdate, [&](Event& ev) {
        ++update;
        seen = std::string(ev.text);
    });
    doc->addListener(body, EventType::CompositionEnd, [&end](Event&) { ++end; });

    doc->dispatchInput(compStart());
    doc->dispatchInput(compUpdate("ni", 1));
    doc->dispatchInput(compEnd());
    CHECK(start == 1);
    CHECK(update == 1);
    CHECK(end == 1);
    CHECK(seen == "ni");
}

TEST_CASE("Composition: CompositionStart deletes the selection and fires Change") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    ElementRef field = doc->byId("field");
    doc->setFocus(field);
    field.setValue("abcd");
    doc->update(0.0f);

    int changes = 0;
    doc->addListener(field, EventType::Change, [&changes](Event&) { ++changes; });

    InputEvent selectAllKey = keyDown(KeyCode::KeyA);
    selectAllKey.modifiers = kCtrl;
    doc->dispatchInput(selectAllKey);
    doc->dispatchInput(compStart());
    CHECK(field.value().empty());
    CHECK(changes == 1);
    CHECK(doc->compositionState().anchorByte == 0);
}

TEST_CASE("Composition: a KeyDown mid-composition performs no default editing action") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    ElementRef field = doc->byId("field");
    doc->setFocus(field);
    field.setValue("abcd");
    doc->update(0.0f);

    int keyDowns = 0;
    doc->addListener(field, EventType::KeyDown, [&keyDowns](Event&) { ++keyDowns; });

    doc->dispatchInput(compStart());
    doc->dispatchInput(compUpdate("ni", 2));
    doc->dispatchInput(keyDown(KeyCode::Backspace));
    CHECK(field.value() == "abcd");  // Unchanged: the IME owns the key.
    CHECK(keyDowns == 1);            // But the event still bubbled.
    CHECK(doc->compositionState().active);
}

TEST_CASE("Composition: the Painter underlines the composition below the baseline") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    ElementRef field = doc->byId("field");  // color: #ff0000
    doc->setFocus(field);
    setValueWithCaret(*doc, field, "abcd", 2);
    doc->dispatchInput(compStart());
    doc->dispatchInput(compUpdate("ni", 2));
    doc->update(0.0f);

    CommandList list;
    doc->paint(list);
    const Color red{1, 0, 0, 1};
    // Two red fills: the 1px underline under "ni" and the caret at its end.
    CHECK(countFills(list, red) == 2);

    // FakeTextShaper's metrics at font-size 10: ascent 8, descent 2, line height 12, so the
    // half-leading is 1 and TextLine::baseline is 9.
    bool foundUnderline = false;
    for (const Command& cmd : list.commands) {
        if (cmd.kind != CmdKind::FillPath || !(list.fills[cmd.index].paint.color == red)) {
            continue;
        }
        const FillCmd& fill = list.fills[cmd.index];
        float minX = list.points[fill.path.pointOffset];
        float minY = list.points[fill.path.pointOffset + 1];
        float maxY = minY;
        for (uint32_t i = 0; i < fill.path.pointCount; i += 2) {
            minX = std::min(minX, list.points[fill.path.pointOffset + i]);
            minY = std::min(minY, list.points[fill.path.pointOffset + i + 1]);
            maxY = std::max(maxY, list.points[fill.path.pointOffset + i + 1]);
        }
        if (maxY - minY > 1.5f) {
            continue;  // The caret (full line height), not the underline.
        }
        foundUnderline = true;
        CHECK(minY == doctest::Approx(11.0f));  // baseline (9) + 2
        CHECK(minX == doctest::Approx(12.0f));  // after "ab": 2 glyphs x 6px
    }
    CHECK(foundUnderline);
}

TEST_CASE("Composition: preventDefault keeps the state but suppresses the underline") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    ElementRef field = doc->byId("field");
    doc->setFocus(field);
    setValueWithCaret(*doc, field, "abcd", 2);
    doc->addListener(field, EventType::CompositionUpdate, [](Event& ev) { ev.preventDefault = true; });

    doc->dispatchInput(compStart());
    doc->dispatchInput(compUpdate("ni", 2));
    doc->update(0.0f);
    CHECK(doc->compositionState().active);
    CHECK(doc->compositionState().suppressDefault);
    CHECK(displayString(*doc, field) == "abnicd");

    CommandList list;
    doc->paint(list);
    // Only the caret remains red; the underline is the host's job now.
    CHECK(countFills(list, Color{1, 0, 0, 1}) == 1);
}

TEST_CASE("Composition: textInputRect follows the composition caret") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    ElementRef field = doc->byId("field");  // absolutely positioned at (0, 0)
    doc->setFocus(field);
    setValueWithCaret(*doc, field, "abcd", 2);
    doc->update(0.0f);
    CHECK(doc->wantsTextInput());

    Rect before = doc->textInputRect();
    CHECK(before.w == doctest::Approx(1.0f));
    CHECK(before.h == doctest::Approx(12.0f));
    // Absolute x depends on the element's position; what matters is that it tracks the caret.

    doc->dispatchInput(compStart());
    doc->dispatchInput(compUpdate("ni", 2));
    doc->update(0.0f);
    CHECK(doc->wantsTextInput());  // Still true throughout the composition.
    Rect during = doc->textInputRect();
    CHECK(during.x == doctest::Approx(before.x + 12.0f));  // two more ASCII glyphs at 6px

    doc->dispatchInput(compEnd());
    doc->update(0.0f);
    CHECK(doc->textInputRect().x == doctest::Approx(before.x));
}

TEST_CASE("Composition: no focused editable element, and blurring mid-composition") {
    FakeTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(&shaper, &assets);

    // No focus at all: the event still bubbles to the root, but nothing starts.
    doc->dispatchInput(compStart());
    doc->dispatchInput(compUpdate("ni", 2));
    CHECK_FALSE(doc->compositionState().active);
    CHECK(doc->textInputRect().w == doctest::Approx(0.0f));

    ElementRef field = doc->byId("field");
    doc->setFocus(field);
    setValueWithCaret(*doc, field, "abcd", 2);
    doc->dispatchInput(compStart());
    doc->dispatchInput(compUpdate("ni", 2));
    doc->update(0.0f);
    REQUIRE(doc->compositionState().active);

    // Moving the focus away cancels the composition; the value is untouched.
    doc->setFocus(doc->byId("plain"));
    doc->update(0.0f);
    CHECK_FALSE(doc->compositionState().active);
    CHECK(field.value() == "abcd");
    CHECK(shapedLength(*doc, field) == 4);
}

TEST_CASE("composeDisplay: prefix + composition + suffix, with a clamped anchor") {
    CompositionState state;
    state.text = "ni";
    state.anchorByte = 2;
    composeDisplay(state, "abcd");
    CHECK(state.display == "abnicd");
    CHECK(compositionBegin(state) == 2);
    CHECK(compositionEnd(state) == 4);
    state.cursor = 1;
    CHECK(compositionCaret(state) == 3);

    state.anchorByte = 99;
    composeDisplay(state, "abcd");
    CHECK(state.anchorByte == 4);
    CHECK(state.display == "abcdni");
}
