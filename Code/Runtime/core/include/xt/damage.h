#pragma once

#include <cstddef>
#include <vector>

#include "xt/command_list.h"
#include "xt/types.h"

/// DamageList: the per-frame dirty rectangles a host uploads to its texture. See
/// Docs/design/23-host-integration.md §23.2, Docs/tasks/T-98.md.

namespace xt {

/// Most rectangles a DamageList ever reports (§23.2); beyond this the smallest-growth pairs are
/// merged until only this many are left.
inline constexpr size_t kMaxDamageRects = 16;
/// Fraction of the viewport's area above which the merged rectangles are replaced by `full`
/// (§23.2 step 4).
inline constexpr float kFullDamageAreaRatio = 0.6f;

/// What changed on screen between the previous and the current frame, in device pixels (§23.2).
/// A default-constructed DamageList is `full`, i.e. "assume everything changed".
struct DamageList {
    /// Device-pixel rectangles, already clipped to the viewport, at most kMaxDamageRects of them.
    /// Empty while `full` is true.
    std::vector<Rect> rects;
    /// The whole viewport is dirty (first frame, viewport/dpr change, reload, or rectangles that
    /// merged into more than kFullDamageAreaRatio of the viewport).
    bool full = true;
    /// The device-pixel viewport this damage was computed against; what bounds() reports while
    /// `full` is true. Not in §23.2's declaration -- bounds() cannot answer "the viewport" without
    /// it (Docs/tasks/T-98.md left the storage unspecified).
    Rect viewport{};

    /// Drops every rectangle and clears `full` (the viewport is kept).
    void clear();
    /// Bounding box of every rectangle, or the viewport while `full` is true.
    Rect bounds() const;
};

/// World-space (i.e. CommandList, pre-devicePixelRatio) AABB of one command. Commands that draw
/// nothing by themselves (every Pop*, PushTransform3D) return an empty Rect. Public because a
/// backend doing partial redraw needs the *same* bounds the damage was computed from, and must not
/// grow a second implementation of them (Docs/tasks/T-99.md; moved here from
/// Code/Runtime/core/src/damage_internal.h).
/// Only for a structurally valid list (T-263c): `list.structureChecked`, or one
/// validateCommandList() accepted. The pools are indexed without range checks.
Rect commandBounds(const CommandList& list, const Command& cmd);

class TextShaper;

/// T-664, moved here by T-680 (Docs/design/10-backends.md "10.12 定案 2 補充 2" B.2) so the Painter's
/// box shadow test and xt::gpu's clip scan share one implementation: the conservative box of glyph
/// run `cmd` in its own (`cmd.xf`) space.
/// T-681 (Docs/design/10-backends.md §10.11, ruling 2 of the T-676 record): the union of every
/// glyph's own ink box -- the control-point box of its outline (the shaper's outline already holds
/// a synthetic bold), times the size, at the glyph's origin, united, for a run drawn from hinted
/// bitmaps at `dpr` (glyphRunUsesHintedBitmap), with its hinted bitmap's box (one device pixel more
/// left and right, half a pixel above and below, for the subpixel phase and the rounded
/// baseline); a glyph without an outline by its
/// bitmap's metrics at the run's size (grown by one run unit for the bitmap's pixel grid and
/// subpixel phase); a glyph the shaper gives neither for (no outline, no bitmap: drawn by no
/// backend) adds nothing -- grown by one device pixel (1 / (`dpr` times the smallest scale `cmd.xf`
/// applies)) and by twice the glyph effect radius on every side; a run with no ink at all (spaces)
/// gives an empty Rect (w = h = 0) and true. When the shaper's outline and
/// bitmap calls give nothing for any glyph, the T-664 estimate is used instead: vertically the
/// origins from the font's ascent above to its descent below, horizontally from half the size left
/// of the first origin to one and a half sizes right of the last. False (the caller keeps the
/// origins grown by the size) without a shaper or glyphs, for a non-finite size or `dpr`, a
/// degenerate transform, or when the estimate is needed and the metrics give no finite, positive
/// ascent + descent. `dpr` 1 (the default; the Painter paints for any ratio) makes the device pixel
/// one run unit at scale 1.
bool glyphRunInkBox(const CommandList& list, const GlyphsCmd& cmd, TextShaper* shaper, Rect& out,
                    float dpr = 1.0f);
/// glyphRunInkBox() transformed by `cmd.xf` (the axis-aligned box of its four corners).
bool glyphRunInkBounds(const CommandList& list, const GlyphsCmd& cmd, TextShaper* shaper, Rect& out,
                       float dpr = 1.0f);

}  // namespace xt
