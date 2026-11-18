#include "frame_cache_pair.h"

/// T-730: see frame_cache_pair.h.

namespace xt::gpu::detail {

uint64_t BackdropPairHead::hash(const CommandList& list, size_t index, const CommandHashOrigin& origin) {
    const Command& cmd = list.commands[index];
    const EffectCmd& c = list.effectCmds[cmd.index];
    m_scratch.commands.clear();
    m_scratch.effectCmds.clear();
    m_scratch.effects.clear();
    Command head = cmd;
    head.index = 0;
    m_scratch.commands.push_back(head);
    EffectCmd copy = c;
    copy.effectOffset = 0;
    m_scratch.effectCmds.push_back(copy);
    for (uint32_t i = 0; i < c.effectCount; ++i) {
        m_scratch.effects.push_back(list.effects[c.effectOffset + i]);
    }
    if (!m_scratch.effects.empty()) {
        EffectInstance& backdrop = m_scratch.effects[0];
        for (float& p : backdrop.params) {
            p = 0.0f;
        }
        for (Color& color : backdrop.colors) {
            color = Color{};
        }
    }
    return hashCommandRelative(m_scratch, 0, origin);
}

}  // namespace xt::gpu::detail
