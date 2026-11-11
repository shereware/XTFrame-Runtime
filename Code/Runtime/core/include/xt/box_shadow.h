#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/command_list.h"
#include "xt/types.h"

/// Analytic box shadows (Docs/design/10-backends.md §10.12 item 2 and its "10.12 定案 2 的規格補充",
/// Docs/tasks/T-668.md): the coverage formula every backend evaluates for `CmdKind::DrawBoxShadow`
/// (xt::sw directly, the xt_gpu shaders as a WGSL and a GLSL transcription with the same
/// constants), and the Painter's six-condition test that decides when a node's `effect: shadow()`
/// becomes such a command instead of a PushEffect.

namespace xt {

class TextShaper;

// -------------------------------------------------------------------------------------------------
// The coverage formula.
// -------------------------------------------------------------------------------------------------

/// Fixed 16-point Gauss-Legendre quadrature on [-1, 1]: the 8 positive nodes (the other 8 are
/// their negatives) and their weights. Values from Abramowitz & Stegun, Handbook of Mathematical
/// Functions (1964), Table 25.4, n = 16. The GPU shaders (box_shadow.wgsl, gl_shaders.h
/// kBoxShadowFrag) repeat these digits.
inline constexpr int kBoxShadowQuadratureHalf = 8;
inline constexpr float kBoxShadowQuadratureNodes[kBoxShadowQuadratureHalf] = {
    0.0950125098376374f, 0.2816035507792589f, 0.4580167776572274f, 0.6178762444026438f,
    0.7554044083550030f, 0.8656312023878318f, 0.9445750230732326f, 0.9894009349916499f,
};
inline constexpr float kBoxShadowQuadratureWeights[kBoxShadowQuadratureHalf] = {
    0.1894506104550685f, 0.1826034150449236f, 0.1691565193950025f, 0.1495959888165767f,
    0.1246289712555339f, 0.0951585116824928f, 0.0622535239386479f, 0.0271524594117541f,
};

/// erf rational approximation, Abramowitz & Stegun 7.1.26 (maximum absolute error 1.5e-7):
/// erf(x) = 1 - (a1 t + a2 t^2 + a3 t^3 + a4 t^4 + a5 t^5) exp(-x^2), t = 1 / (1 + p x), x >= 0;
/// odd for x < 0.
inline constexpr float kBoxShadowErfP = 0.3275911f;
inline constexpr float kBoxShadowErfA1 = 0.254829592f;
inline constexpr float kBoxShadowErfA2 = -0.284496736f;
inline constexpr float kBoxShadowErfA3 = 1.421413741f;
inline constexpr float kBoxShadowErfA4 = -1.453152027f;
inline constexpr float kBoxShadowErfA5 = 1.061405429f;

/// The integration window and the drawn quad reach this many sigmas past the shape (§10.12: the
/// y integral runs over [p.y - 3 sigma, p.y + 3 sigma], the quad is R' outset by 3 sigma).
inline constexpr float kBoxShadowSigmaReach = 3.0f;

/// Docs/design/18-effects.md §18.3: radius-like effect parameters are capped at 64 device px
/// (effect_utils.cpp's kMaxEffectDevicePx, the backends' clampDevicePx64). A converted shadow keeps
/// that cap on its blur and its (non-negative) spread.
inline constexpr float kBoxShadowMaxDevicePx = 64.0f;

/// erf(x) by A&S 7.1.26 with the constants above.
float boxShadowErf(float x);

/// The shadow shape `R'` in the command's local coordinates: an axis-aligned rectangle
/// [x0, x1] x [y0, y1] with circular corner radii r[0..3] = top-left, top-right, bottom-right,
/// bottom-left (each at most half of the side lengths and no two neighbours longer than their side).
struct BoxShadowShape {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float r[4] = {0, 0, 0, 0};
};

/// `R'` of §10.12: `rect` grown by `spread` on every side (each radius + spread, at least 0), then
/// moved by `offset`. False when the result is empty (a negative spread larger than half a side) or
/// a value is not finite. The radii are clamped to the grown rectangle with the CSS overlap rule
/// (rounded_rect.cpp's clampRoundedRectRadii), which leaves radii that already fit unchanged.
bool boxShadowShape(const Rect& rect, const float radii[4], float spread, Vec2 offset, BoxShadowShape& out);

/// The horizontal extent [x0, x1] of `s` at height `y` (corners from the circle equation). False
/// when `y` lies outside [s.y0, s.y1].
bool boxShadowSpan(const BoxShadowShape& s, float y, float& x0, float& x1);

/// The §10.12 coverage at point `p` (local coordinates) for Gaussian `sigma` (local units, > 0):
/// `S(p) = ∫ G(y - p.y; sigma) · ½ [erf((x1(y) - p.x) / (sigma √2)) - erf((x0(y) - p.x) / (sigma √2))] dy`
/// over `[max(y0, p.y - 3 sigma), min(y1, p.y + 3 sigma)]` with the 16-point Gauss-Legendre rule;
/// 0 for an empty interval. Clamped to [0, 1].
float boxShadowCoverage(const BoxShadowShape& s, float px, float py, float sigma);

/// boxShadowCoverage() split at the point where `p.x` enters: everything that depends only on
/// `p.y` (the integration window, the 16 abscissas with their spans and weighted Gaussians) is
/// computed once by boxShadowRow() and boxShadowCoverageAt() finishes one point. A row of pixels
/// with the same local `y` (an axis-aligned transform) shares one setup; the arithmetic is exactly
/// boxShadowCoverage()'s, which is defined as these two calls.
struct BoxShadowRow {
    bool empty = true;
    float half = 0.0f;
    float sigma = 1.0f;
    float invErf = 0.0f;
    float wg[2 * kBoxShadowQuadratureHalf] = {};  ///< weight x Gaussian; 0 where the span is empty
    float x0[2 * kBoxShadowQuadratureHalf] = {};
    float x1[2 * kBoxShadowQuadratureHalf] = {};
};
void boxShadowRow(const BoxShadowShape& s, float py, float sigma, BoxShadowRow& row);
float boxShadowCoverageAt(const BoxShadowRow& row, float px);

/// True when a Gaussian of `sigmaDevice` (device px) is what the effect path treats as no blur at
/// all: xt::sw's three-box width `effect_math::boxBlurWidth(sigma)` is at most 1 (Docs/design/
/// 18-effects.md §18.6; the GPU path's gaussIsIdentity agrees). Such a shadow is the analytic
/// coverage of `R'` (the rasterizer / a fringed convex fill), not the formula.
bool boxShadowIsSharp(float sigmaDevice);

/// What a backend draws for one DrawBoxShadow under `deviceXf` (= device pixel ratio x cmd.xf):
/// the shape (T-680: and the extra signed terms whose shapes are not empty), the Gaussian in the
/// command's local units and in device px, and the premultiplied colour. `blur` and a positive `spread` are
/// capped at kBoxShadowMaxDevicePx device px (the uniform scale sqrt|det deviceXf| converts). False when
/// nothing is drawn (empty shape, a non-finite value, a singular transform, a colour with alpha <= 0).
struct BoxShadowDraw {
    BoxShadowShape shape;
    /// T-680: cmd.extra's shapes (grown and moved like `shape`), with their signs.
    uint32_t extraCount = 0;
    BoxShadowShape extra[2];
    float extraSign[2] = {0, 0};
    float sigmaLocal = 0.0f;
    float sigmaDevice = 0.0f;
    bool sharp = true;  ///< boxShadowIsSharp(sigmaDevice)
    Color premul{};     ///< cmd.color premultiplied
};
bool resolveBoxShadow(const BoxShadowCmd& cmd, const Mat2x3& deviceXf, BoxShadowDraw& out);

/// T-680: the signed sum of the terms' coverage at `p` (local), clamped to [0, 1]:
/// clamp(boxShadowCoverage(shape) + sum of extraSign x boxShadowCoverage(extra), 0, 1). With no
/// extra term it is boxShadowCoverage(shape) itself.
float boxShadowCoverageSum(const BoxShadowDraw& d, float px, float py, float sigma);

/// The local rectangle a DrawBoxShadow draws into: `R'` outset by 3 sigma (sigma = blur / 2, not
/// capped: an upper bound); T-680: the union over every term's shape. Empty when the first term's
/// shape is.
Rect boxShadowDrawRect(const BoxShadowCmd& cmd);

// -------------------------------------------------------------------------------------------------
// The Painter's conversion test (§10.12 "判定").
// -------------------------------------------------------------------------------------------------

/// Which of §10.12's six conditions failed first (None: the shadow converts).
enum class BoxShadowReject : uint8_t {
    None = 0,
    EffectChain = 1,      ///< (1) an enabled effect other than an outer drop shadow
    NodeLayer = 2,        ///< (2) opacity < 1, a blend mode, a 3D node, or a faded paint alpha
    Background = 3,       ///< (3) no opaque border-box background (or hidden, or a clip-path)
    ContentOverflow = 4,  ///< (4) content may reach past the border box
    Outline = 5,          ///< (5) an outline that is not solid and opaque (T-680)
    Params = 6,           ///< (6) a parameter is not finite or the blur is negative
};

/// The facts the test needs, gathered by the Painter from the node's computed style and from the
/// commands its subtree emitted. Plain data so the test is a pure function.
struct BoxShadowJudgeInput {
    /// The node's enabled effects, in stack order.
    std::span<const EffectInstance> effects;
    /// (2): the node's own opacity and blend mode; whether it is drawn inside a 3D space root or is
    /// a 3D layer; the alpha its paints are multiplied by (an ancestor's folded opacity).
    float opacity = 1.0f;
    uint8_t blend = 0;
    bool in3D = false;
    float paintAlpha = 1.0f;
    /// (3): the node is an Element whose background is drawn (not `visibility: hidden`) on its whole
    /// border box (no `clip-path`); the background colour's alpha; whether a background gradient is
    /// drawn and every one of its stops has alpha 1.
    bool isElement = false;
    bool backgroundDrawn = false;
    bool clipPath = false;
    float backgroundAlpha = 0.0f;
    bool gradientOpaque = false;
    /// The border box (local) and its four circular corner radii (top-left, top-right,
    /// bottom-right, bottom-left), as the background is painted.
    Rect borderBox{};
    float radii[4] = {0, 0, 0, 0};
    /// (4): the children are clipped to the padding box (an overflow clip is drawn); otherwise
    /// `contentKnown` and the conservative local bounds of everything the node's subtree draws
    /// (`contentEmpty`: nothing drawn at all). `skipContent` leaves condition 4 out (the Painter's
    /// early test, before the subtree has been emitted).
    bool overflowClips = false;
    bool skipContent = false;
    bool contentKnown = false;
    bool contentEmpty = false;
    Rect contentBounds{};
    /// (5): an outline ring is drawn. T-680: whether it is solid (`outline-style: solid`) and its
    /// colour's alpha.
    bool outline = false;
    bool outlineSolid = false;
    float outlineAlpha = 0.0f;
};

/// Tolerance (local px) of condition 4's "inside the border box": the subtree's bounds come back
/// through the inverse of the node's transform, which is not exact in float.
inline constexpr float kBoxShadowContentEpsilon = 1e-3f;

/// The six conditions of §10.12, in order; the first that fails is returned.
BoxShadowReject judgeBoxShadow(const BoxShadowJudgeInput& in);

/// The local bounds of command `cmd` of `list` in the frame `frame` maps root-box coordinates
/// into: each draw command's own geometry box under `frame * xf` (xt::commandBounds' boxes, glyph
/// runs and strokes grown as it grows them), a group opener's / quad's / mesh's root-box box
/// mapped through `frame`. `empty` is set for a command that draws nothing by itself. False when
/// the box is not finite. T-680 ("補充 2" B.2): with `shaper`, a glyph run's box is the font-metric
/// box of xt::glyphRunInkBox (xt/damage.h) where it is usable, else the origins grown by the size.
bool commandBoundsInFrame(const CommandList& list, const Command& cmd, const Mat2x3& frame, Rect& out,
                          bool& empty, TextShaper* shaper = nullptr);

}  // namespace xt
