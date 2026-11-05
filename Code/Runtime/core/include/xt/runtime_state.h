#pragma once

#include <memory>

/// T-616 (Docs/design/04-timeline.md §4.13, 13-api.md §13.8.3): the runtime state a Document
/// hands to the Document loaded in its place -- the editor's recompile, the same carry-over
/// Context::hotReload makes. Document::captureRuntimeState() takes it from the outgoing Document;
/// Document::restoreRuntimeState() applies it to the new one, after load and before its first
/// update(). Carried, keyed by name so it fits a recompiled IR: the new Document's clock (the old
/// one's), each id'd element's custom states and CSS `animation` items (position + `@keyframes`
/// name -> elapsed seconds, paused, ended), every active `@timeline` group (scope id + name ->
/// position, playing, loop, reverse, hold, rate), editable `input` values, focus and scroll
/// offsets. Whatever the new Document lacks is skipped (it keeps its own initial state); port
/// values are not part of it.

namespace xt {

struct DocumentState;  // Code/Runtime/core/src/document_state.h (private).

/// An opaque captured runtime state; empty when default-constructed. Move-only.
class RuntimeState {
public:
    RuntimeState();
    ~RuntimeState();
    RuntimeState(RuntimeState&& other) noexcept;
    RuntimeState& operator=(RuntimeState&& other) noexcept;
    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;

    /// True when nothing was captured (restoreRuntimeState() then does nothing).
    bool empty() const { return m_state == nullptr; }
    /// The captured Document's clock.
    float clock() const { return m_clock; }

private:
    friend class Document;
    std::unique_ptr<DocumentState> m_state;
    float m_clock = 0;
};

}  // namespace xt
