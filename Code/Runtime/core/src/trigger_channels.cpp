#include <algorithm>

#include "trigger_runtime.h"
#include "xt/document.h"
#include "xt/ir.h"

/// The input-channel triggers `input(<channel>)`, `input(<channel>).press` and `.release`
/// (Docs/design/60-input-channels.md §60.3.2; Docs/tasks/T-873.md). They are not DOM events: every
/// element holding one is listed (load(), bindNode()) and fireChannels() runs the ones whose channel
/// the Document's frame commit changed, at most once a frame each, regardless of focus and hit
/// testing.

namespace xt {

void TriggerRuntime::indexChannelTriggers(NodeId node) {
    const NodeArena& arena = m_doc.m_arena;
    if (node == kInvalidNode || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return;
    }
    const uint32_t table = arena.common(node).triggerTable;
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (table == 0 || table > tc.tables().size()) {
        return;
    }
    const uint32_t generation = arena.generation(node);
    // A recycled node id: its old entries go.
    m_channelTriggers.erase(
        std::remove_if(m_channelTriggers.begin(), m_channelTriggers.end(),
                       [&](const ChannelTrigger& c) { return c.node == node && c.generation != generation; }),
        m_channelTriggers.end());
    const IrTriggerTable& t = tc.tables()[table - 1];
    for (uint32_t k = 0; k < t.triggerCount; ++k) {
        const IrTrigger& trig = tc.triggers()[t.triggerOffset + k];
        const auto ev = static_cast<TriggerEvent>(trig.event);
        if (ev != TriggerEvent::InputChange && ev != TriggerEvent::InputPress &&
            ev != TriggerEvent::InputRelease) {
            continue;
        }
        if (trig.arg1 >= kMaxPlayers) {
            continue;
        }
        const auto player = static_cast<uint8_t>(trig.arg1);
        const bool listed =
            std::any_of(m_channelTriggers.begin(), m_channelTriggers.end(), [&](const ChannelTrigger& c) {
                return c.node == node && c.event == ev && c.name == trig.arg0 && c.player == player;
            });
        if (listed) {
            continue;  // fire() runs every matching trigger of the element.
        }
        const ChannelId channel = m_doc.m_channels.intern(m_doc.string(trig.arg0));
        if (channel == kInvalidChannel) {
            continue;
        }
        m_doc.m_channels.listen(channel);
        m_channelTriggers.push_back(ChannelTrigger{node, generation, ev, trig.arg0, player, channel});
    }
}

void TriggerRuntime::fireChannels() {
    const InputChannels& channels = m_doc.m_channels;
    const NodeArena& arena = m_doc.m_arena;
    // By index: a trigger's actions may add a `<repeat>` instance (bindNode() appends).
    for (size_t i = 0; i < m_channelTriggers.size(); ++i) {
        const ChannelTrigger c = m_channelTriggers[i];
        if (!arena.alive(c.node) || arena.generation(c.node) != c.generation) {
            continue;
        }
        float value = 0.0f;
        switch (c.event) {
            case TriggerEvent::InputChange:
                if (!channels.changed(c.channel, c.player)) {
                    continue;
                }
                value = channels.value(c.channel, c.player);
                break;
            case TriggerEvent::InputPress:
                if (!channels.pressed(c.channel, c.player)) {
                    continue;
                }
                value = channels.strength(c.channel, c.player);
                break;
            case TriggerEvent::InputRelease:
                if (!channels.released(c.channel, c.player)) {
                    continue;
                }
                value = channels.strength(c.channel, c.player);
                break;
            default:
                continue;
        }
        fire(c.node, c.event, c.name, c.player, value, 0xFFFFFFFFu, c.player);
    }
    // Entries of destroyed elements go.
    m_channelTriggers.erase(std::remove_if(m_channelTriggers.begin(), m_channelTriggers.end(),
                                           [&](const ChannelTrigger& c) {
                                               return !arena.alive(c.node) ||
                                                      arena.generation(c.node) != c.generation;
                                           }),
                            m_channelTriggers.end());
}

}  // namespace xt
