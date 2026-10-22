#pragma once

#include "xt/command_list.h"
#include "xt/text_shaper.h"

/// CommandListSimplifier: degrades a CommandList to the engine-minimum command subset
/// (FillPath NonZero/Solid, PushClipRect/PopClip, DrawGlyphs, DrawImage), in place or into
/// another list. See Docs/design/09-command-list.md §9.5, Docs/tasks/T-37.md
/// "CommandListSimplifier".
///
/// Damage (T-263c, T-263e): a simplified list is never a damage base -- Context::render(out,
/// &damage) diffs only against the list it wrote itself in its previous call, untouched
/// (CommandList::frameSerial). A host that simplifies and wants partial damage therefore uses the
/// out-of-place form: `ctx.render(list, &damage); simplifier.simplify(list, simplified, options,
/// shaper);`, draws `simplified` and hands the same `list` back to the next render(). The
/// in-place form rewrites `list`, so the frame after it reports full damage, every frame.

namespace xt {

/// Per-rule toggles (Docs/design/09-command-list.md §9.5 table); `tolerance` feeds
/// flattenPath/strokeToFill/applyDash when `strokes` degrades a StrokePath.
struct SimplifyOptions {
    /// Flattens 3D projection content to the 2D engine-minimum subset (Docs/design/19-3d-space.md
    /// §19.8). Runs before every other rule below.
    bool flatten3D = true;
    /// Degrades StrokePath to FillPath via strokeToFill().
    bool strokes = true;
    /// Replaces gradient paints with the middle stop's solid color.
    bool gradients = true;
    /// Drops PushLayer/PopLayer, folding opacity into each contained paint instead.
    bool layers = true;
    /// Drops Mask commands, drawing the masked content unmasked.
    bool masks = true;
    /// Replaces ClipPath with a ClipRect covering its AABB.
    bool clipPaths = true;
    /// Converts DrawGlyphs to FillPath per glyph (needs a non-null `shaper`).
    bool glyphsToPaths = true;
    /// Drops PushEffect/PopEffect, drawing the wrapped content unfiltered (Docs/design/
    /// 18-effects.md §18.5's 09.5 minimal-subset rule). Runs after `flatten3D`, before the rules
    /// above.
    bool effects = true;
    /// Max chord error (px) used when flattening/stroking/dashing during simplification.
    float tolerance = 0.25f;
    /// Drops DrawParticles (T-202, Docs/design/18-effects.md §18.12: not in the engine-minimum
    /// subset); SimplifyStats::droppedParticles counts the particles removed. When false, the
    /// command passes through (its colors scaled by a dropped layer's opacity). Inside a 3D
    /// projection `flatten3D` drops it regardless.
    bool particles = true;
};

/// Counters returned by CommandListSimplifier::simplify(); Docs/design/19-3d-space.md §19.8.
struct SimplifyStats {
    /// Number of commands processed by the `flatten3D` rule (Docs/design/19-3d-space.md §19.8).
    uint32_t flattenedCommands = 0;
    /// Number of DrawGlyphs commands dropped because no shaper was available to convert them to
    /// paths (either `options.glyphsToPaths`'s existing rule, or `flatten3D`'s glyph-to-path step
    /// inside an active projection).
    uint32_t droppedGlyphRuns = 0;
    /// Number of enabled effects removed by `options.effects` (Docs/design/18-effects.md §18.5),
    /// summed across every dropped PushEffect's EffectCmd::effectCount.
    uint32_t droppedEffects = 0;
    /// Number of particles (instances) removed by `options.particles` or by `flatten3D` inside a
    /// projection (T-202), summed across every dropped DrawParticles.
    uint32_t droppedParticles = 0;
};

/// Degrades a CommandList to the engine-minimum subset per Docs/tasks/T-37.md's rule table. The
/// in-place form builds the simplified result into an internal scratch CommandList, then swaps it
/// with `list`; the scratch member keeps its buffers' capacity across calls so a steady-state
/// input shape does not reallocate after the first call (the out-of-place form reuses `out`'s
/// buffers the same way when the host keeps `out` across frames). A list that fails
/// validateCommandList() is not simplified: it is left as it is (out of place: `out` becomes a
/// copy of it) and zero stats are returned; the result of a valid one is marked
/// CommandList::structureChecked (T-263b), so a backend does not validate it again.
/// T-263e: the result keeps the input's viewport, devicePixelRatio, time and source table, and
/// -- when `options.effects == false` keeps the effects -- its shader pools (ShaderRef /
/// parameters / text: a kept custom effect still finds its shader, and reads xtTime; with the
/// effects dropped nothing refers to the pools and they are left empty); its frameSerial is 0.
class CommandListSimplifier {
public:
    /// In place. `shaper` is used both by `options.flatten3D` (glyph outlines inside an active
    /// projection) and `options.glyphsToPaths` (2D DrawGlyphs -> FillPath per glyph); null then
    /// removes every DrawGlyphs command that would otherwise need one instead of converting it.
    /// Ignored (may be null) when both `options.flatten3D` and `options.glyphsToPaths` are false.
    /// The rewritten `list` is not a damage base (see the top of this file). Same as the
    /// out-of-place form followed by a swap.
    SimplifyStats simplify(CommandList& list, const SimplifyOptions& options, TextShaper* shaper);
    /// T-263e: out of place. `in` is only read, so a list Context::render() wrote stays the
    /// damage base of the next render(); `out` is cleared (capacity retained) and receives the
    /// simplified list. `options` and `shaper` as above. `in` and `out` being the same list is
    /// the in-place form.
    SimplifyStats simplify(const CommandList& in, CommandList& out, const SimplifyOptions& options,
                           TextShaper* shaper);

private:
    CommandList m_scratch;
    /// Intermediate buffer for the `flatten3D` pass's output, consumed by the rest of simplify()'s
    /// rules. Kept as a member (not a local) so its buffers' capacity is also reused across calls.
    CommandList m_flatten3DScratch;
};

}  // namespace xt
