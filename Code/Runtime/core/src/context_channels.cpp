#include "xt/context.h"

/// Context's input channels (Docs/design/60-input-channels.md §60.5; Docs/tasks/T-873.md): the
/// Context keeps its own table (Context::inputChannel() ids, channelValue()) and sends every channel
/// value to every live Document by name, translated into that Document's ids.

namespace xt {

ChannelId Context::inputChannel(std::string_view name) { return m_channels.intern(name); }

float Context::channelValue(ChannelId id, uint8_t player) const { return m_channels.value(id, player); }

bool Context::routeChannels(const InputEvent& event) {
    m_channelPushes.clear();
    const bool channel = event.kind == InputEvent::Channel;
    if (channel) {
        if (event.channel == kInvalidChannel || event.channel > m_channels.count()) {
            return true;  // An id this Context never handed out.
        }
        m_channels.push(event.channel, event.channelValue, event.channelStrength, event.channelFlags,
                        event.player);
        m_channelPushes.push_back(InputChannels::Push{
            event.channel, event.channelValue, event.channelStrength, event.channelFlags, event.player});
    } else {
        m_channels.applyEvent(event, &m_channelPushes);
    }
    for (size_t d = 0; d < m_documents.size(); ++d) {
        Document* doc = liveAt(d);
        if (doc == nullptr) {
            continue;
        }
        for (const InputChannels::Push& p : m_channelPushes) {
            InputEvent ev;
            ev.kind = InputEvent::Channel;
            ev.channel = doc->inputChannel(m_channels.name(p.id));
            ev.channelValue = p.value;
            ev.channelStrength = p.strength;
            // The Context's kind travels with it (a host channel's first push decided it here).
            const ChannelKind kind = m_channels.kind(p.id);
            ev.channelFlags = kind == ChannelKind::Relative ? kChannelRelative
                              : kind == ChannelKind::Button ? kChannelButton
                                                            : static_cast<uint8_t>(0);
            ev.player = p.player;
            doc->dispatchInput(ev);
        }
    }
    return channel;
}

}  // namespace xt
