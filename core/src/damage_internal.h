#pragma once

#include <cstdint>
#include <vector>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/types.h"

/// Internals of docs/design/23-host-integration.md §23.2's damage computation, exposed for
/// tests/unit/test_damage.cpp only (docs/tasks/T-98.md "可在 damage_internal.h 宣告供測試").

namespace xt {

/// Per-command FNV-1a 64 hash of `list`: command kind, payload bytes, and the pool ranges the
/// payload refers to (path verbs/points, glyphs, effects). `out` is resized to
/// `list.commands.size()` (capacity reused). T-263d: an asset command's `source` index is part of
/// its payload, so two lists that differ only in it differ; `list.sources` itself is never read
/// (neither here nor by computeDamage / commandBounds) -- the previous frame's table may name IRs
/// that are gone.
void hashCommandList(const CommandList& list, std::vector<uint64_t>& out);

// commandBounds() now lives in the public xt/damage.h (docs/tasks/T-99.md): the SW backend's
// partial redraw needs it, and xt/damage.h is already part of the host contract (§23.6).

/// §23.2 steps 2-4: hashes both lists, diffs them from both ends, unions the changed commands'
/// bounds, scales them to device pixels, clips them to `viewportDevice` and merges them down to
/// kMaxDamageRects. `out` is cleared first; `out.viewport` is set to `viewportDevice`.
void computeDamage(const CommandList& prev, const CommandList& cur, Rect viewportDevice, DamageList& out);

}  // namespace xt
