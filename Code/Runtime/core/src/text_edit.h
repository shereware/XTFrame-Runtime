#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/text_layout.h"
#include "xt/text_shaper.h"

/// Single-line text editing state and the pure functions that mutate it: caret/selection
/// movement (grapheme-aware, visual direction across bidi runs), insertion, deletion, and the
/// caret/selection geometry the Painter needs. See Docs/design/08-events.md §8.5/§8.8,
/// Docs/design/07-text.md §7.6-7.7, Docs/tasks/T-33.md.
///
/// Internal to xt_core (not installed under Code/Runtime/core/include): Document (document.cpp), the event
/// dispatcher, layout_text.cpp, painter_caret.cpp and the unit tests use it directly.
///
/// Every function here is pure with respect to everything but its `EditState&`: it reads the
/// caller-supplied ShapeResult (the shaping cache of the element's Text node, see TextNodeData)
/// and never touches the arena, the Document or the shaper.

namespace xt {

/// One editable element's runtime value plus its caret. `cursor` is the moving end of the
/// selection and `anchor` the fixed one; `cursor == anchor` means "no selection, caret only".
/// Both are UTF-8 byte offsets into `value` and always sit on a grapheme boundary.
struct EditState {
    std::string value;
    uint32_t cursor = 0;
    uint32_t anchor = 0;
};

/// Sentinel for TextNodeData::editIndex / "this element has no EditState".
inline constexpr uint32_t kInvalidEditIndex = 0xFFFFFFFFu;

inline uint32_t selectionBegin(const EditState& s) { return s.cursor < s.anchor ? s.cursor : s.anchor; }
inline uint32_t selectionEnd(const EditState& s) { return s.cursor < s.anchor ? s.anchor : s.cursor; }
inline bool hasSelection(const EditState& s) { return s.cursor != s.anchor; }

/// Next/previous grapheme boundary of `s.value` at/before `off`. `shaped` supplies
/// ShapeResult::graphemeStart when it still matches `s.value` (same byte length); otherwise --
/// e.g. two edits in one frame, before the next layout reshapes -- these fall back to UTF-8
/// codepoint boundaries, which is the same thing for everything but combining sequences.
uint32_t nextGrapheme(const EditState& s, const ShapeResult& shaped, uint32_t off);
uint32_t prevGrapheme(const EditState& s, const ShapeResult& shaped, uint32_t off);

/// Replaces the selection (or inserts at the caret) with `bytes`, leaving the caret collapsed
/// after the inserted text. Returns true if `value` changed.
bool insertUtf8(EditState& s, std::string_view bytes);

/// Deletes the selection, or -- with no selection -- the one grapheme before/after the caret.
/// Returns true if `value` changed.
bool deleteBackward(EditState& s, const ShapeResult& shaped);
bool deleteForward(EditState& s, const ShapeResult& shaped);

/// Moves the caret one grapheme in the *visual* direction (Docs/design/08-events.md §8.8): inside
/// an LTR run Right is the next grapheme, inside an RTL run it is the previous one, and a run
/// boundary is crossed following the UAX#9 (bidiReorder) visual run order. Implemented by
/// evaluating every grapheme boundary's visual x within the shaped paragraph and stepping to the
/// nearest boundary strictly to the right/left, which is monotonic in x by construction and needs
/// no special cases for run boundaries. With no selection and `extendSelection == false` the
/// caret just moves; with a selection it first collapses to its logical begin/end (Left/Right).
/// `extendSelection` keeps `anchor` fixed instead.
void moveLeft(EditState& s, const ShapeResult& shaped, bool extendSelection);
void moveRight(EditState& s, const ShapeResult& shaped, bool extendSelection);

/// Logical string start/end (§8.8: "Home/End 到邏輯字串首尾").
void moveHome(EditState& s, bool extendSelection);
void moveEnd(EditState& s, bool extendSelection);
/// anchor = 0, cursor = value.size().
void selectAll(EditState& s);
/// Places the caret at `byteOffset` (clamped to the value, snapped to a grapheme boundary),
/// optionally extending the selection (mouse drag / Shift+click).
void setCaret(EditState& s, const ShapeResult& shaped, uint32_t byteOffset, bool extendSelection);

/// The caret's x within the laid-out text node, for the logical byte offset `byteOffset`
/// (Docs/tasks/T-33.md "游標視覺 x"): the advances of every glyph logically before it inside the
/// run holding it, accumulated from that run's left edge (LTR) or right edge (RTL). A boundary
/// shared by two runs resolves to the run that *ends* there (trailing affinity); an empty `runs`
/// (no text, or no font) gives 0.
float caretX(const ShapeResult& shaped, const std::vector<GlyphRun>& runs, uint32_t byteOffset);

/// The x range [outX0, outX1] covered by the logical byte range [from, to) inside `run`. Returns
/// false when the range does not intersect the run at all.
bool runSelectionSpan(const ShapeResult& shaped, const GlyphRun& run, uint32_t from, uint32_t to,
                      float& outX0, float& outX1);

}  // namespace xt
