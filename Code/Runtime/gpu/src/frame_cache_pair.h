#pragma once

#include <cstddef>
#include <cstdint>

#include "damage_internal.h"  // Code/Runtime/core/src: CommandHashOrigin, hashCommandRelative
#include "xt/command_list.h"

/// T-730 (Docs/design/10-backends.md §10.18 "B 定案" second step, Docs/tasks/T-730.md): the part of a
/// backdrop pair's cache key that differs from an ordinary effect segment's. A segment whose stack is a
/// BackdropBlur followed only by shadow-type effects (effect_math::backdropThenShadowsOnly) keeps two
/// results, the source layer S and the shadow result R' -- neither reads the surface, so neither
/// depends on the BackdropBlur's parameters. Its key is the segment's key (frame_builder.cpp
/// segmentHash / relativeSegmentHash) with the PushEffect command's own hash replaced by this one.

namespace xt::gpu::detail {

/// Tags the two key forms of a pair, so that no pair key equals an ordinary segment's.
inline constexpr uint32_t kBackdropPairTag = 0x54373330u;  // "T730"

class BackdropPairHead {
public:
    /// The hash core gives PushEffect command `index` of `list` (hashCommandRelative with `origin`; a
    /// default `origin` is hashCommandList's value), computed as if the leading BackdropBlur had no
    /// parameters and no colours: its kind, its enabled flag and the rest of the stack are hashed as
    /// they are. `index` must name a PushEffect whose effects lie inside `list`.
    uint64_t hash(const CommandList& list, size_t index, const CommandHashOrigin& origin);

private:
    /// One PushEffect and its effects, copied (capacity retained across calls).
    CommandList m_scratch;
};

}  // namespace xt::gpu::detail
