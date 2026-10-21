#include <string>
#include <string_view>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "text_edit.h"
#include "xt/text_layout.h"
#include "xt/text_shaper.h"

/// Pure text-editing tests (docs/tasks/T-33.md "測試", `test_text_edit.cpp`): insertion over a
/// selection, grapheme-aware Backspace, visual Left/Right across an LTR and an RTL run, Home/End,
/// select-all and caretX. Driven by FakeTextShaper's monospace metrics: at size 10 an ASCII glyph
/// advances 6px and a space 3px.

using namespace xt;

namespace {

/// Shapes `text` (LTR paragraph, `rtlRanges` given as codepoint index ranges) into `out`.
void shapeText(std::string_view text, ShapeResult& out,
               std::vector<std::pair<uint32_t, uint32_t>> rtlRanges = {}) {
    FakeTextShaper shaper(std::move(rtlRanges));
    ShapeParams params;
    params.size = 10;
    params.paragraphDirection = TextDirection::Ltr;
    shaper.shape(text, params, out);
}

/// Lays `shaped` out on one unbounded line, so caretX() sees the same GlyphRuns the Painter does.
void layoutOneLine(const ShapeResult& shaped, TextLayoutResult& out) {
    TextLayoutParams params;
    params.availMain = 1000;
    params.fontSize = 10;
    params.lineHeight = 12;
    params.ascent = 8;
    params.descent = 2;
    layoutText(shaped, params, out);
}

EditState makeState(std::string value, uint32_t cursor, uint32_t anchor) {
    EditState state;
    state.value = std::move(value);
    state.cursor = cursor;
    state.anchor = anchor;
    return state;
}

}  // namespace

TEST_CASE("insertUtf8: at the caret and over a selection") {
    EditState state = makeState("abc", 1, 1);
    CHECK(insertUtf8(state, "XY"));
    CHECK(state.value == "aXYbc");
    CHECK(state.cursor == 3);
    CHECK(state.anchor == 3);

    // A selection is replaced, and the caret collapses after the inserted text.
    state = makeState("abcd", 3, 1);  // selects "bc"
    CHECK(insertUtf8(state, "Z"));
    CHECK(state.value == "aZd");
    CHECK(state.cursor == 2);
    CHECK(state.anchor == 2);

    // Inserting nothing with no selection changes nothing; with a selection it deletes it.
    state = makeState("ab", 1, 1);
    CHECK_FALSE(insertUtf8(state, std::string_view{}));
    state = makeState("ab", 0, 2);
    CHECK(insertUtf8(state, std::string_view{}));
    CHECK(state.value.empty());
}

TEST_CASE("deleteBackward/Forward: a combining sequence is one grapheme") {
    // "e" + U+0301 COMBINING ACUTE ACCENT: 3 bytes, one grapheme. FakeTextShaper has no grapheme
    // clustering of its own (it marks every codepoint start), so the combining mark's
    // graphemeStart flag is cleared here -- that flag is exactly the contract text_edit relies on.
    std::string value = "e\xCC\x81";
    ShapeResult shaped;
    shapeText(value, shaped);
    REQUIRE(shaped.graphemeStart.size() == 3);
    shaped.graphemeStart[1] = 0;

    EditState state = makeState(value, 3, 3);
    CHECK(deleteBackward(state, shaped));
    CHECK(state.value.empty());
    CHECK(state.cursor == 0);
    CHECK_FALSE(deleteBackward(state, shaped));

    state = makeState(value, 0, 0);
    CHECK(deleteForward(state, shaped));
    CHECK(state.value.empty());

    // With a selection, both delete exactly the selection.
    state = makeState("abc", 2, 0);
    CHECK(deleteBackward(state, shaped));
    CHECK(state.value == "c");
}

TEST_CASE("moveLeft/Right: LTR run") {
    ShapeResult shaped;
    shapeText("abc", shaped);
    EditState state = makeState("abc", 0, 0);

    moveRight(state, shaped, /*extendSelection=*/false);
    CHECK(state.cursor == 1);
    moveRight(state, shaped, false);
    CHECK(state.cursor == 2);
    moveLeft(state, shaped, false);
    CHECK(state.cursor == 1);
    CHECK(state.anchor == 1);

    // Shift keeps the anchor, so a selection grows.
    moveRight(state, shaped, /*extendSelection=*/true);
    CHECK(state.cursor == 2);
    CHECK(state.anchor == 1);
    CHECK(hasSelection(state));
    // Without Shift, a selection collapses to its logical end (Right) / begin (Left).
    moveRight(state, shaped, false);
    CHECK(state.cursor == 2);
    CHECK_FALSE(hasSelection(state));

    // The ends clamp.
    moveHome(state, false);
    moveLeft(state, shaped, false);
    CHECK(state.cursor == 0);
    moveEnd(state, false);
    moveRight(state, shaped, false);
    CHECK(state.cursor == 3);
}

TEST_CASE("moveLeft/Right: visual direction across an RTL run") {
    // "abc XYZ" with XYZ (codepoints 4..6) RTL: it renders as "abc ZYX", i.e. run0 "abc "
    // (bytes 0..4, LTR) then run1 "XYZ" (bytes 4..7, RTL, drawn Z Y X left to right).
    std::string value = "abc XYZ";
    ShapeResult shaped;
    shapeText(value, shaped, {{4, 7}});
    REQUIRE(shaped.runs.size() == 2);
    CHECK(shaped.runs[0].bidiLevel == 0);
    CHECK(shaped.runs[1].bidiLevel == 1);

    // From the caret right after the space (the LTR run's trailing edge), Right enters the RTL
    // run at its *visual left* end and crosses one grapheme: the visually first glyph there is Z,
    // logical byte 6 (docs/design/08-events.md §8.8).
    EditState state = makeState(value, 4, 4);
    moveRight(state, shaped, false);
    CHECK(state.cursor == 6);
    // Further Right keeps walking the RTL run leftward in logical terms (Z -> Y -> X).
    moveRight(state, shaped, false);
    CHECK(state.cursor == 5);
    // Left retraces the same visual positions.
    moveLeft(state, shaped, false);
    CHECK(state.cursor == 6);
    moveLeft(state, shaped, false);
    CHECK(state.cursor == 4);
    moveLeft(state, shaped, false);
    CHECK(state.cursor == 3);
}

TEST_CASE("moveHome/moveEnd/selectAll") {
    ShapeResult shaped;
    shapeText("abc", shaped);
    EditState state = makeState("abc", 1, 1);

    moveEnd(state, false);
    CHECK(state.cursor == 3);
    CHECK(state.anchor == 3);
    moveHome(state, /*extendSelection=*/true);
    CHECK(state.cursor == 0);
    CHECK(state.anchor == 3);  // Shift+Home selects back to the start.

    selectAll(state);
    CHECK(state.anchor == 0);
    CHECK(state.cursor == 3);

    setCaret(state, shaped, 99, false);  // Clamped to the value's end.
    CHECK(state.cursor == 3);
    CHECK_FALSE(hasSelection(state));
}

TEST_CASE("caretX: accumulated advances, LTR and RTL runs") {
    ShapeResult shaped;
    shapeText("abc", shaped);
    TextLayoutResult laid;
    layoutOneLine(shaped, laid);

    CHECK(caretX(shaped, laid.runs, 0) == doctest::Approx(0.0f));
    CHECK(caretX(shaped, laid.runs, 1) == doctest::Approx(6.0f));
    CHECK(caretX(shaped, laid.runs, 2) == doctest::Approx(12.0f));
    CHECK(caretX(shaped, laid.runs, 3) == doctest::Approx(18.0f));

    ShapeResult bidi;
    shapeText("abc XYZ", bidi, {{4, 7}});
    TextLayoutResult bidiLaid;
    layoutOneLine(bidi, bidiLaid);
    REQUIRE(bidiLaid.runs.size() == 2);
    // run0 "abc " is 3*6 + 3 = 21 wide, run1 "XYZ" is 18 wide and starts at x 21.
    CHECK(caretX(bidi, bidiLaid.runs, 4) == doctest::Approx(21.0f));
    // Inside the RTL run the caret counts advances from the run's right edge.
    CHECK(caretX(bidi, bidiLaid.runs, 5) == doctest::Approx(33.0f));
    CHECK(caretX(bidi, bidiLaid.runs, 6) == doctest::Approx(27.0f));
    CHECK(caretX(bidi, bidiLaid.runs, 7) == doctest::Approx(21.0f));

    // Empty text: no runs, caret at the origin.
    ShapeResult empty;
    TextLayoutResult emptyLaid;
    CHECK(caretX(empty, emptyLaid.runs, 0) == doctest::Approx(0.0f));
}

TEST_CASE("runSelectionSpan: per-run x range of a byte range") {
    ShapeResult shaped;
    shapeText("abc", shaped);
    TextLayoutResult laid;
    layoutOneLine(shaped, laid);
    REQUIRE(laid.runs.size() == 1);

    float x0 = 0;
    float x1 = 0;
    CHECK(runSelectionSpan(shaped, laid.runs[0], 1, 3, x0, x1));
    CHECK(x0 == doctest::Approx(6.0f));
    CHECK(x1 == doctest::Approx(18.0f));
    CHECK_FALSE(runSelectionSpan(shaped, laid.runs[0], 2, 2, x0, x1));
}
