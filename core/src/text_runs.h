#pragma once

#include <cstdint>

#include "xt/ir.h"
#include "xt/node.h"
#include "xt/types.h"

/// Rich-text run style resolution (docs/design/07-text.md §7.8, T-178), shared by the text layout
/// (layout_text.cpp) and the Painter's text branch (painter_text.cpp).

namespace xt {

/// The effective style of one rich-text run: the parent element's style with the run's override
/// declarations applied on top.
struct TextRunStyle {
    /// Glyph fill color.
    Color color{0, 0, 0, 1};
    /// CSS `font-weight`, 100..900.
    uint16_t fontWeight = 400;
    /// CSS `font-style: italic`.
    bool italic = false;
    /// Font size in px.
    float fontSize = 16;
    /// Letter spacing in px.
    float letterSpacing = 0;
    /// `text-effect`: offset/count into the IR STYL `effects[]` pool.
    uint32_t effectOffset = 0;
    uint32_t effectCount = 0;  ///< 0 = none.
    /// True when the run overrides `color` (the Painter then ignores the element's animated color).
    bool hasColor = false;
    /// T-324 (05 §5.9.8): the kTextDecoration* bits the run's own `text-decoration-line`
    /// overrides add (every such override counts: `<s><u>…</u></s>` draws both, and a run never
    /// removes the element's decorations, as in CSS). 0 = none.
    uint8_t decorationLine = 0;
    /// T-324: the run's `text-decoration-color`; valid when hasDecorationColor.
    Color decorationColor{0, 0, 0, 1};
    bool hasDecorationColor = false;
    /// T-331 (05 §5.9.8): ShapeParams::features for the run (the element's
    /// `font-variant-numeric`, or the run's own override).
    uint8_t features = 0;
};

/// Resolves `run`'s style (`run == nullptr` = the element's own style). `baseLetterSpacing` is the
/// element's already-resolved `letter-spacing` in px. Override lengths in `em` resolve against the
/// element's font size (`font-size`) or the run's font size (`letter-spacing`); `%` font sizes
/// against the element's font size.
void resolveTextRunStyle(const IrView& ir, const ComputedStyle& base, float baseLetterSpacing,
                         const TextStyleRun* run, TextRunStyle& out);

}  // namespace xt
