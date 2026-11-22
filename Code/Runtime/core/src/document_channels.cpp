#include "binding_runtime.h"
#include "trigger_runtime.h"
#include "xt/document.h"

/// Document's input channels (Docs/design/60-input-channels.md §60.3.1, §60.3.2, §60.5;
/// Docs/tasks/T-873.md): the per-frame commit that fires the `input(...)` triggers and re-evaluates
/// the `input()` bindings, and the needsFrame() part.

namespace xt {

ChannelId Document::inputChannel(std::string_view name) { return m_channels.intern(name); }

void Document::commitChannels() {
    if (!m_channels.settling()) {
        return;
    }
    m_channels.commit();
    if (m_channels.changes().empty()) {
        return;
    }
    m_bindings->channelsChanged();
    // 60 §60.3.2: at most once a frame per channel; a hidden surface takes no input.
    if (m_root != kInvalidNode && m_surfaceVisible) {
        m_triggers->beginDefer();
        m_triggers->fireChannels();
        m_triggers->endDefer();
    }
}

}  // namespace xt
