#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// SW (CPU) rasterizer backend. Stage 1 (T-11): FillPath (Solid paint, NonZero/EvenOdd) and
/// PushClipRect/PopClip. Stage 2 (T-12): gradients, StrokePath/dash, PushClipPath, Layer/Mask
/// (17 blend modes), DrawImage. See Docs/design/10-backends.md §10.2, Docs/tasks/T-11.md,
/// Docs/tasks/T-12.md.

namespace xt::sw {

/// An RGBA8 render target, row-major, y pointing down; non-premultiplied unless `premultiplied`.
struct SwTarget {
    /// Pointer to the first pixel (not owned; caller keeps it alive for the render calls).
    uint8_t* rgba = nullptr;
    /// Width in pixels.
    int width = 0;
    /// Height in pixels.
    int height = 0;
    /// Bytes between the start of consecutive rows.
    int strideBytes = 0;
    /// True when the host wants (and supplies) premultiplied RGBA8 in this buffer. The SW backend
    /// composites in premultiplied float internally (Docs/design/23-host-integration.md §23.5), so
    /// this simply drops the un-premultiply on every write into the target and the premultiply on
    /// every read back out of it -- the cheapest path for a host whose texture format is
    /// premultiplied. Default false keeps the pre-T-102 contract: non-premultiplied RGBA8.
    bool premultiplied = false;
    /// T-239 (item 5): the device-pixel position of this target's top-left pixel on the full
    /// canvas. The target is then a window onto the canvas: every command, damage rectangle and
    /// clearRects() rectangle stays in canvas device pixels and lands at (x - originX,
    /// y - originY) in the buffer, so a host can render only the visible part of a large canvas
    /// (a zoomed-in editor preview). 0/0 (the default) is the whole-canvas target of before.
    int originX = 0;
    int originY = 0;
};

/// CPU rasterizer for CommandList. Stage 3 (T-30): DrawGlyphs through TextShaper::glyphOutline
/// (Docs/design/10-backends.md §10.2.6). Supports every CmdKind.
class SwRenderer {
public:
    /// Constructs a renderer with no render target set; call setTarget() before render().
    SwRenderer();
    /// Destroys the renderer, releasing all Impl-owned resources.
    ~SwRenderer();
    /// Non-copyable (owns an opaque Impl).
    SwRenderer(const SwRenderer&) = delete;
    /// Non-copyable (owns an opaque Impl).
    SwRenderer& operator=(const SwRenderer&) = delete;

    /// Sets the render target. Resets the clip stack (the previous clip mask no longer applies to
    /// a target of possibly different size).
    void setTarget(const SwTarget& target);

    /// Fills the whole target with `color` (non-premultiplied, converted to RGBA8 by rounding).
    void clear(Color color);

    /// Fills only the pixels inside `rects` (canvas device px, i.e. before the target's origin is
    /// subtracted; snapped outwards to whole pixels and clamped to the target) with `color`; every pixel
    /// outside every rectangle keeps its value. A host calls this instead of clear() before a partial redraw
    /// -- render() never clears by itself, with or without damage (Docs/design/23-host-integration.md §23.3,
    /// Docs/tasks/T-99.md).
    void clearRects(Color color, std::span<const Rect> rects);

    /// Renders `list` into the current target. `shaper` supplies DrawGlyphs' glyph outlines
    /// (Docs/design/10-backends.md §10.2.6); null skips every DrawGlyphs command. `assets` resolves
    /// DrawImage's AssetId (may be null, in which case every DrawImage command is skipped). `ir` is the
    /// IrView that assets' AssetIds are relative to (required by AssetResolver::decodeImage's signature,
    /// Docs/design/10-backends.md §10.4); null has the same effect as `assets == nullptr` for DrawImage
    /// purposes. This parameter is additive (defaulted) so existing 3-argument call sites keep compiling
    /// unchanged. T-263d: a list that names its sources (`list.sources`, filled by Context::render)
    /// resolves each asset command against its own source's IR instead and does not use `ir` (it
    /// may be null); see xt::sourceIr().
    ///
    /// `perf` (Docs/design/21-perf-tracing.md §21.2) is likewise additive: when it is non-null and
    /// enabled, this call adds its own wall-clock duration to the current frame's `renderMs`, the
    /// time spent executing effect stacks (PopEffect) to `effectsMs`, and the time spent drawing
    /// glyph runs (DrawGlyphs) to `textMs`. Null (the default) measures nothing.
    /// `damage` (Docs/design/23-host-integration.md §23.3) restricts this call to the dirty
    /// rectangles of xt::Context::render(). Null, `full`, or an empty rectangle list renders the
    /// whole target exactly as before. Otherwise the union of `damage->rects` becomes a write mask
    /// on the *root* target: every pixel outside it keeps its value, and a FillPath / StrokePath /
    /// DrawImage / DrawGlyphs / DrawImageQuad command whose commandBounds() misses every rectangle
    /// is skipped entirely. Push*/Pop* commands always run (the stacks must stay balanced), and
    /// layer/mask/effect buffers are still allocated and computed over their full boundsHint (an
    /// effect reads its neighborhood); only their composite back onto the root is masked. The
    /// result inside the rectangles is byte-identical to a full render of the same list.
    ///
    /// `damage` is additive (defaulted) so existing call sites keep compiling unchanged.
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                const IrView* ir = nullptr, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);

    /// Draw commands (FillPath/StrokePath/DrawImage/DrawGlyphs/DrawImageQuad) the last render()
    /// call actually executed, and how many it skipped as outside the damage rectangles. Both are
    /// 0 before the first render(); a full render never skips anything (Docs/tasks/T-99.md test 2).
    uint32_t lastExecutedDrawCommands() const;
    /// See lastExecutedDrawCommands().
    uint32_t lastSkippedDrawCommands() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::sw
