#pragma once

#include "xt/command_list.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// The device-pixel bounds the GPU frame builder's clip scan uses (Docs/design/10-backends.md
/// §10.11 ruling 5 final, Docs/tasks/T-663.md item 2): for every clip that would take a stencil
/// level at 1x, the union of these boxes over the commands up to its PopClip decides whether the
/// clip is needed at all, can be a scissor, or becomes a mask layer.

namespace xt::gpu {

/// How far (device px) every box is grown: the 1 px coverage fringe of convex fills, image quads
/// and glyphs, and the whole-pixel snapping of hinted glyphs, with room to spare.
inline constexpr float kClipContentMarginPx = 2.0f;

enum class ClipContentBounds : uint8_t {
    Empty,    ///< the command draws nothing by itself (Pop*, EndMaskContent, a clip, a degenerate shape)
    Box,      ///< `out` bounds every pixel the command (or the group it opens) can draw
    Unknown,  ///< no reliable box: the clip counts as touching everything
};

/// The conservative device-pixel box of `cmd` (world bounds times `dpr`, grown by
/// kClipContentMarginPx): fills and images their transformed geometry, strokes with the width,
/// caps and miters (xt::commandBounds), glyph runs their origins grown by the font size and any
/// glyph effect, particles and meshes their command box; PushLayer / PushMask / PushEffect /
/// PushProjection their window (`boundsHint`, already grown by the effects' reach). An empty hint
/// (the "whole viewport" default) or a non-finite value is Unknown.
/// T-664 (Docs/tasks/T-664.md item 5): with `shaper`, a glyph run's box is tightened by its font's
/// metrics (xt::glyphRunInkBounds, xt/damage.h; T-680 moved it to xt_core); without one, or when
/// the metrics are unusable, it stays the origins grown by the font size. xt::commandBounds (core,
/// SW damage) is unchanged. Only for a structurally valid list (like xt::commandBounds).
ClipContentBounds clipContentBounds(const CommandList& list, const Command& cmd, float dpr, Rect& out,
                                    TextShaper* shaper = nullptr);

/// T-676 (Docs/design/10-backends.md §10.11, the ruling at the end of the T-664 record): the box of
/// `cmd` in the frame `frame` maps root-box coordinates into -- a clip shape's own local coordinates
/// -- instead of the device: each draw command's own local box mapped through `frame` times its
/// transform (xt::commandBoundsInFrame; a glyph run's glyphRunInkBounds box under the same
/// relative transform), a group opener's window mapped through `frame`, then grown by `margin`
/// (kClipContentMarginPx device px expressed in frame units, per axis). Empty / Box / Unknown as
/// clipContentBounds; an empty group window is Unknown.
ClipContentBounds clipContentBoundsInFrame(const CommandList& list, const Command& cmd, const Mat2x3& frame,
                                           Vec2 margin, Rect& out, TextShaper* shaper = nullptr);

}  // namespace xt::gpu
