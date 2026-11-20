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

/// T-721 (Docs/design/10-backends.md §10.16 W2 item 1): what hashCommandRelative() takes positions
/// relative to. `plane` is in the coordinates of the commands' transforms (the plane the commands are
/// drawn in), `screen` in those of the screen-space boxes (LayerCmd / MaskCmd / EffectCmd bounds,
/// EffectCmd::xf's translation); outside a 3D space the two are one point. The `*Steps` are 0 for
/// the exact form (the float `value - origin` is hashed) or the number of steps per unit a value is
/// rounded to first, which absorbs the float noise of a position computed at another place:
/// `planeSteps` / `screenSteps` for the relative positions (and the sizes of the screen boxes),
/// `linearSteps` for the linear part of EffectCmd::xf.
struct CommandHashOrigin {
    Vec2 plane{};
    Vec2 screen{};
    float planeSteps = 0.0f;
    float screenSteps = 0.0f;
    float linearSteps = 0.0f;
};

/// T-721: the hash of command `index` of `list` as hashCommandList() computes it, except that its
/// positions are taken relative to `origin`: the translation of the transform of FillPath,
/// StrokePath, DrawImage, DrawGlyphs, PushClipRect, PushClipPath, DrawParticles and DrawBoxShadow,
/// the points of DrawImageQuad and DrawImageMesh and PushProjection's rootXf (minus `origin.plane`);
/// the bounds of PushLayer, PushMask, PushEffect and PushProjection and the translation of
/// EffectCmd::xf (minus `origin.screen`; an empty box, the "whole viewport" default, stays as it is).
/// Everything else -- the linear parts, the paints, the paths, the pools -- is hashed as it is. With
/// a default-constructed `origin` the result equals hashCommandList()'s for that command. An `index`
/// past the list gives the hash of nothing.
uint64_t hashCommandRelative(const CommandList& list, uint32_t index, const CommandHashOrigin& origin);

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

/// T-737 (Docs/tasks/T-737.md B): computeDamage() with the previous list's hashes supplied by the
/// caller and the current list's handed back. `prevHashes` must be exactly hashCommandList(prev)
/// (the caller vouches for it; nullptr hashes `prev` here, as computeDamage() does). `curHashes` is
/// left holding hashCommandList(cur) (capacity reused). The DamageList is computeDamage()'s.
/// T-753 (57 §57.3 item 4): a non-null `matchedOut` receives how many commands of `cur` the alignment
/// matched with an identical command of `prev` (all of them when the lists are identical).
void computeDamageWithHashes(const CommandList& prev, const std::vector<uint64_t>* prevHashes,
                             const CommandList& cur, Rect viewportDevice, DamageList& out,
                             std::vector<uint64_t>& curHashes, uint32_t* matchedOut = nullptr);

/// T-737: where `list`'s storage is -- the data pointer and the size of `commands` and of every
/// payload and pool hashCommandList() reads -- written to `out` (resized; capacity reused). Two equal
/// results of the same list mean nothing was added, removed, reallocated, moved or copied between
/// them; Context::render keeps the previous frame's hashes only while this stays equal (with the
/// list's frameSerial).
void commandListStorage(const CommandList& list, std::vector<uintptr_t>& out);

}  // namespace xt
