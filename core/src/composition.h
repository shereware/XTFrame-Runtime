#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "xt/types.h"

/// IME composition state (docs/design/08-events.md §8.8, docs/design/07-text.md §7.6,
/// docs/tasks/T-33b.md) and the two small geometry helpers layout, the Painter and
/// Context::textInputRect() share.
///
/// Internal to xt_core (not installed under core/include): Document owns exactly one
/// CompositionState and lends it to the EventDispatcher (which mutates it), to the LayoutEngine
/// and to the Painter (which only read it).
///
/// While a composition is active the element's *value* never changes; only the display string
/// -- value prefix + composition + value suffix -- does. `CompositionEnd` clears the state and
/// the platform's following `TextInput` is what actually inserts the committed text.

namespace xt {

struct TextNodeData;
struct TextLine;

/// One Document's single in-flight composition. `element` identifies the composing element, so
/// layout/paint can tell "this Text node is the one being composed into" apart from every other
/// editable input in the document.
struct CompositionState {
    /// The IME's current composition string (UTF-8). Empty when !active.
    std::string text;
    /// Caret byte offset within `text`.
    uint32_t cursor = 0;
    /// Byte length of the IME's selected clause, starting at `cursor`, within `text`.
    uint32_t selLen = 0;
    /// Byte offset within the element's EditState value where `text` is inserted: the caret
    /// position captured at CompositionStart (after any selection was deleted).
    uint32_t anchorByte = 0;
    bool active = false;
    /// Set when the latest Composition event was preventDefault()ed: the host draws the
    /// composition itself, so the Painter emits no underline and no clause highlight (the value
    /// semantics are unaffected, §8.8 "阻止預設顯示（不影響 value 語意）").
    bool suppressDefault = false;
    /// The composing Element, or kInvalidNode when !active.
    NodeId element = kInvalidNode;
    /// value prefix + `text` + value suffix; rebuilt by composeDisplay() on every state change and
    /// reused (never freed) so layout reads it without allocating.
    std::string display;
};

/// Rebuilds `state.display` from `value` and the current composition, clamping `anchorByte` to
/// `value`'s length first.
void composeDisplay(CompositionState& state, std::string_view value);

/// Byte range of the composition, and the caret's byte offset, within `state.display`.
inline uint32_t compositionBegin(const CompositionState& state) { return state.anchorByte; }
inline uint32_t compositionEnd(const CompositionState& state) {
    return state.anchorByte + static_cast<uint32_t>(state.text.size());
}
inline uint32_t compositionCaret(const CompositionState& state) { return state.anchorByte + state.cursor; }

/// The laid-out line holding byte offset `off`: the first whose byte range contains it, else the
/// last. nullptr when `text` has no lines.
const TextLine* lineForOffset(const TextNodeData& text, uint32_t off);

/// The caret rectangle for byte offset `off` in `text`'s own coordinate system: 1 logical pixel
/// wide, the holding line's rect height (docs/tasks/T-33b.md "textInputRect()"). False when the
/// text has not been laid out.
bool caretLocalRect(const TextNodeData& text, uint32_t off, Rect& out);

}  // namespace xt
