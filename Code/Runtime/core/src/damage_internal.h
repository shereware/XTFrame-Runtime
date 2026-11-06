#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/types.h"

/// Internals of Docs/design/23-host-integration.md §23.2's damage computation, exposed for
/// Tests/unit/test_damage.cpp only (Docs/tasks/T-98.md "可在 damage_internal.h 宣告供測試").

namespace xt {

/// Per-command FNV-1a 64 hash of `list`: command kind, payload bytes, and the pool ranges the
/// payload refers to (path verbs/points, glyphs, effects) -- by content: since T-644 the pool offsets
/// themselves are not hashed, so a command shifted by an insertion keeps its hash. `out` is resized to
/// `list.commands.size()` (capacity reused). T-263d: an asset command's `source` index is part of
/// its payload, so two lists that differ only in it differ; `list.sources` itself is never read
/// (neither here nor by computeDamage / commandBounds) -- the previous frame's table may name IRs
/// that are gone.
void hashCommandList(const CommandList& list, std::vector<uint64_t>& out);

// commandBounds() now lives in the public xt/damage.h (Docs/tasks/T-99.md): the SW backend's
// partial redraw needs it, and xt/damage.h is already part of the host contract (§23.6).

/// T-644 (§23.2 revision "步驟 2 改為多區間"): lists longer than this many commands skip the sequence
/// diff and keep the single first-to-last-differing range.
inline constexpr size_t kDamageDiffMaxCommands = 4096;
/// T-644: most edits (inserted plus deleted commands) the sequence diff searches before it gives up
/// and falls back to the single range; bounds its O((n + m) d) time and O(d^2) trace.
inline constexpr size_t kDamageDiffMaxEdits = 256;
/// matchCommandHashes()'s "this command has no partner in the other list".
inline constexpr uint32_t kNoCommandMatch = 0xffffffffu;

/// T-644: aligns two per-command hash sequences (hashCommandList()). `prevToCur[i]` is the index of
/// the `cur` command matched with `prev[i]` (kNoCommandMatch when it was deleted or changed), and
/// `curToPrev` the inverse; matches are monotone and only pair equal hashes. The common head and tail
/// are always matched; the middle is aligned by a Myers O((n + m) d) diff (a longest common
/// subsequence), so several separated changed ranges leave the commands between them matched.
/// Returns false when the diff did not run -- either list has more than kDamageDiffMaxCommands
/// commands, or more than kDamageDiffMaxEdits edits are needed -- and only head and tail are matched
/// (the single range of §23.2 step 2).
bool matchCommandHashes(const std::vector<uint64_t>& prev, const std::vector<uint64_t>& cur,
                        std::vector<uint32_t>& prevToCur, std::vector<uint32_t>& curToPrev);

/// §23.2 steps 2-4: hashes both lists, aligns them (matchCommandHashes()), unions the unmatched
/// commands' bounds (plus the propagation rules), scales them to device pixels, clips them to
/// `viewportDevice` and merges them down to kMaxDamageRects. `out` is cleared first; `out.viewport`
/// is set to `viewportDevice`.
void computeDamage(const CommandList& prev, const CommandList& cur, Rect viewportDevice, DamageList& out);

}  // namespace xt
