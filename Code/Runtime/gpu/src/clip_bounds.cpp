#include "xt/gpu/clip_bounds.h"

#include <cmath>

#include "xt/box_shadow.h"  // commandBoundsInFrame (T-676)
#include "xt/damage.h"

namespace xt::gpu {

namespace {

bool finiteRect(const Rect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h);
}

}  // namespace

ClipContentBounds clipContentBounds(const CommandList& list, const Command& cmd, float dpr, Rect& out,
                                    TextShaper* shaper) {
    out = Rect{};
    bool hint = false;  // the box is a group's window: an empty one means "whole viewport"
    switch (cmd.kind) {
        case CmdKind::FillPath:
        case CmdKind::StrokePath:
        case CmdKind::DrawImage:
        case CmdKind::DrawGlyphs:
        case CmdKind::DrawImageQuad:
        case CmdKind::DrawParticles:
        case CmdKind::DrawImageMesh:
        case CmdKind::DrawBoxShadow:  // T-668
            break;
        case CmdKind::PushLayer:
        case CmdKind::PushMask:
        case CmdKind::PushEffect:
        case CmdKind::PushProjection:
            hint = true;
            break;
        case CmdKind::PushClipRect:
        case CmdKind::PushClipPath:
        case CmdKind::PopClip:
        case CmdKind::PopLayer:
        case CmdKind::EndMaskContent:
        case CmdKind::PopMask:
        case CmdKind::PopProjection:
        case CmdKind::PushTransform3D:
        case CmdKind::PopTransform3D:
        case CmdKind::PopEffect:
            return ClipContentBounds::Empty;
    }
    Rect r;
    if (cmd.kind != CmdKind::DrawGlyphs || !glyphRunInkBounds(list, list.glyphRuns[cmd.index], shaper, r)) {
        r = commandBounds(list, cmd);
    }
    if (!finiteRect(r) || !std::isfinite(dpr) || !(dpr > 0.0f)) {
        return ClipContentBounds::Unknown;
    }
    if (!(r.w > 0.0f) || !(r.h > 0.0f)) {
        // A zero-length stroke still draws its caps (commandBounds leaves that box unexpanded).
        const bool stroke = cmd.kind == CmdKind::StrokePath;
        return (hint || stroke) ? ClipContentBounds::Unknown : ClipContentBounds::Empty;
    }
    const float m = kClipContentMarginPx;
    out = Rect{r.x * dpr - m, r.y * dpr - m, r.w * dpr + 2.0f * m, r.h * dpr + 2.0f * m};
    if (!finiteRect(out)) {
        out = Rect{};
        return ClipContentBounds::Unknown;
    }
    return ClipContentBounds::Box;
}

ClipContentBounds clipContentBoundsInFrame(const CommandList& list, const Command& cmd, const Mat2x3& frame,
                                           Vec2 margin, Rect& out, TextShaper* shaper) {
    out = Rect{};
    bool hint = false;
    switch (cmd.kind) {
        case CmdKind::FillPath:
        case CmdKind::StrokePath:
        case CmdKind::DrawImage:
        case CmdKind::DrawGlyphs:
        case CmdKind::DrawImageQuad:
        case CmdKind::DrawParticles:
        case CmdKind::DrawImageMesh:
        case CmdKind::DrawBoxShadow:
            break;
        case CmdKind::PushLayer:
        case CmdKind::PushMask:
        case CmdKind::PushEffect:
        case CmdKind::PushProjection: {
            // An empty window means "the whole viewport": no box (as clipContentBounds).
            const Rect h = commandBounds(list, cmd);
            if (!finiteRect(h) || !(h.w > 0.0f) || !(h.h > 0.0f)) {
                return ClipContentBounds::Unknown;
            }
            hint = true;
            break;
        }
        case CmdKind::PushClipRect:
        case CmdKind::PushClipPath:
        case CmdKind::PopClip:
        case CmdKind::PopLayer:
        case CmdKind::EndMaskContent:
        case CmdKind::PopMask:
        case CmdKind::PopProjection:
        case CmdKind::PushTransform3D:
        case CmdKind::PopTransform3D:
        case CmdKind::PopEffect:
            return ClipContentBounds::Empty;
    }
    Rect r;
    bool done = false;
    if (cmd.kind == CmdKind::DrawGlyphs) {
        // glyphRunInkBounds maps its run-space box through the run's transform: give it the
        // run's transform relative to the frame.
        GlyphsCmd run = list.glyphRuns[cmd.index];
        run.xf = mul(frame, run.xf);
        done = glyphRunInkBounds(list, run, shaper, r);
    }
    if (!done) {
        bool empty = false;
        if (!commandBoundsInFrame(list, cmd, frame, r, empty)) {
            return ClipContentBounds::Unknown;
        }
        if (empty) {
            return hint ? ClipContentBounds::Unknown : ClipContentBounds::Empty;
        }
    }
    if (!finiteRect(r) || !std::isfinite(margin.x) || !std::isfinite(margin.y)) {
        return ClipContentBounds::Unknown;
    }
    if (!(r.w > 0.0f) || !(r.h > 0.0f)) {
        const bool stroke = cmd.kind == CmdKind::StrokePath;
        return (hint || stroke) ? ClipContentBounds::Unknown : ClipContentBounds::Empty;
    }
    out = Rect{r.x - margin.x, r.y - margin.y, r.w + 2.0f * margin.x, r.h + 2.0f * margin.y};
    if (!finiteRect(out)) {
        out = Rect{};
        return ClipContentBounds::Unknown;
    }
    return ClipContentBounds::Box;
}

}  // namespace xt::gpu
