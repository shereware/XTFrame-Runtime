#include "composition.h"

#include "text_edit.h"
#include "xt/node.h"

/// CompositionState helpers. See composition.h, docs/design/08-events.md §8.8,
/// docs/tasks/T-33b.md.

namespace xt {

void composeDisplay(CompositionState& state, std::string_view value) {
    if (state.anchorByte > value.size()) {
        state.anchorByte = static_cast<uint32_t>(value.size());
    }
    state.display.assign(value.substr(0, state.anchorByte));
    state.display.append(state.text);
    state.display.append(value.substr(state.anchorByte));
}

const TextLine* lineForOffset(const TextNodeData& text, uint32_t off) {
    if (text.lines.empty()) {
        return nullptr;
    }
    for (const TextLine& line : text.lines) {
        if (off >= line.byteStart && off <= line.byteEnd) {
            return &line;
        }
    }
    return &text.lines.back();
}

bool caretLocalRect(const TextNodeData& text, uint32_t off, Rect& out) {
    const TextLine* line = lineForOffset(text, off);
    if (line == nullptr) {
        return false;
    }
    out = Rect{caretX(text.shaped, text.runs, off), line->rect.y, 1.0f, line->rect.h};
    return true;
}

}  // namespace xt
