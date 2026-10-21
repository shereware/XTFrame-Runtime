#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// xt_gpu: the device-neutral GPU frame (docs/design/10-backends.md §10.5, docs/tasks/T-146.md).
///
/// A `GpuFrame` is everything a GPU device needs to draw one CommandList: one vertex buffer, one
/// index buffer, a flat list of draw commands, the passes (target switches) that group them, a
/// float parameter pool and the transient (offscreen) targets the frame needs. It contains no
/// device handles of its own except texture handles the device itself issued through
/// `GpuDevice::createTexture`; transient targets are numbered per frame.
///
/// Conventions (every device implementation must follow them, see gpu_device.h for the rest):
///
/// - **Coordinates.** `Vertex::x/y` are *surface* pixels, origin top-left, y down, whatever the
///   pass's target. A device maps them to its own clip space with the pass's window
///   (`Pass::viewX/Y/W/H`, surface pixels): NDC.x = 2 (x - viewX) / viewW - 1,
///   NDC.y = 1 - 2 (y - viewY) / viewH (or the equivalent for the device's clip space).
/// - **Windows (T-151).** A transient target is exactly its window's size: its own pixel (0,0)
///   is the window's top-left surface pixel, and the pass that draws it sets the hardware
///   viewport to the whole transient. `DrawCmd::clip*` stays in surface pixels too; the device
///   intersects it with the window and translates it by the target's origin. A window never
///   leaves the surface. The surface target's passes always use the whole surface.
/// - **Homogeneous w.** `Vertex::w` is a projective divisor (docs/design/19-3d-space.md §19.7):
///   `x`/`y` are already multiplied by `w`, so the on-screen position is `(x / w, y / w)`. The
///   vertex shader must emit `clip = (ndc(x / w, y / w) * w, 0, w)` so the GPU's own perspective
///   divide reproduces that point while interpolating `u,v` and `rgba` perspective-correctly.
///   `w == 1` for every vertex of an unprojected command.
/// - **Color.** `Vertex::rgba` is premultiplied alpha, byte order r,g,b,a (`rgba & 0xFF` is red
///   on every platform: the builder packs it as `r | g << 8 | b << 16 | a << 24`). Every fragment
///   a shader writes is premultiplied, every texture the builder creates is premultiplied
///   (images are premultiplied on the CPU before upload; the R8 glyph atlas is coverage), and
///   every transient target holds premultiplied color. `Blend::SrcOver` is therefore
///   `(One, OneMinusSrcAlpha)` for both color and alpha.
/// - **Texture sampling.** Row 0 of a texture (or of a transient target read as a texture) is
///   its *top* row, matching `createTexture`'s row order and the y-down pixel coordinates above.
///   A device whose native render-target texel row 0 is the bottom (OpenGL) flips `v` when
///   sampling a transient target; it never flips uploaded textures, since it uploaded them
///   top-down itself.
/// - **Window commands** (`Shading::Composite`, `Shading::Effect`) are quads in surface pixels
///   whose `u,v` run from (0,0) at the quad's top-left to (1,1) at its bottom-right, addressing
///   the *whole* of `src` (and `aux`, which always shares `src`'s window): an Effect's quad is
///   its own target's window (every input shares it), a Composite's quad is `src`'s window. The
///   device draws the command's vertices like any other; effect shaders address their inputs
///   with those `u,v` (plus texel offsets from their parameters, which are 1 / window size).
/// - **Stencil.** 8 bits. Low nibble (0x0F): fill-rule winding scratch, 0 at rest. High nibble
///   (0xF0): the current path-clip depth (0..15). `DrawCmd::stencilRef` is always a *depth*
///   (0..15); the ops below spell out the exact test/write each `StencilOp` performs.

namespace xt::gpu {

/// A device-issued texture id (`GpuDevice::createTexture`). 0 means "no texture".
using TextureHandle = uint32_t;
/// A render target for this frame: 0 is the frame's output surface (the target the host bound
/// before `submit`); `i > 0` is `GpuFrame::transients[i - 1]`.
using TargetHandle = uint32_t;

/// The frame's output surface as a `TargetHandle`.
inline constexpr TargetHandle kSurfaceTarget = 0;

/// Pixel formats the builder asks the device for.
enum class TextureFormat : uint8_t {
    Rgba8 = 0,  ///< 4 bytes/texel, premultiplied color (images, gradient tables, color targets).
    R8 = 1,     ///< 1 byte/texel coverage (the glyph atlas).
    R32F = 2,   ///< 1 float/texel; transient targets only (Outline's signed distance field).
    /// T-199 (docs/design/26-layers-symbols-library.md §26.3.4, 23 §23.8): block-compressed
    /// images as `xtframe-pack --compress` writes them (DDS). `createTexture`'s `initial` is the
    /// level-0 blocks (`xt::ddsLevelSize` bytes, 4x4 blocks in row order, row 0 = top), with
    /// straight alpha -- the builder samples them with `Shading::TextureStraight`. A device that
    /// cannot upload the format returns 0 and the builder falls back to the decoded Rgba8 image.
    /// `updateTexture` is never called on them. Uploaded textures only, never transients.
    Bc1 = 3,  ///< 8 bytes per block (DXT1).
    Bc3 = 4,  ///< 16 bytes per block (DXT5).
    Bc7 = 5,  ///< 16 bytes per block (BPTC UNORM).
};

/// One vertex, 24 bytes. See the file comment for the meaning of each field; `u,v` depend on the
/// command's shading (`Shading`).
struct Vertex {
    float x, y, w;
    float u, v;
    uint32_t rgba;
};
static_assert(sizeof(Vertex) == 24, "Vertex must be 24 bytes");

/// What a draw command's fragment shader computes.
enum class Shading : uint8_t {
    /// Output = vertex `rgba`. `u,v` unused.
    Solid = 0,
    /// Gradient paint: `u,v` are the path-local position (the command's pre-transform space),
    /// `GradientParams` at `paramIndex` gives the gradient geometry and `texture` is a 1-D
    /// premultiplied-independent lookup table (`kGradientLutWidth` x 1 RGBA8, *straight* color)
    /// sampled at `t`. Output = (lut.rgb * a, a) with
    /// `a = lut.a * GradientParams::alpha * vertex.a` (vertex alpha = `rgba >> 24` / 255, the
    /// edge-fringe coverage of T-155; 255 everywhere else). Sample at
    /// `u = (t * (kGradientLutWidth - 1) + 0.5) / kGradientLutWidth`, linear filter. Vertex
    /// `rgb` unused.
    Gradient = 1,
    /// Textured quad: output = sample(`texture`, u, v) * vertex `rgba` (both premultiplied).
    Texture = 2,
    /// Glyph atlas quad: `texture` is R8 coverage; output = vertex `rgba` *
    /// pow(sample.r, `GlyphParams::gamma`).
    Glyph = 3,
    /// No color output at all (the winding/clip/clear stencil passes). The device disables color
    /// writes; the vertex shader still transforms positions.
    StencilOnly = 4,
    /// Layer composite: a quad over `src`'s window. `src` is the layer's color; `CompositeParams`
    /// gives the blend mode and opacity. With `blend == SrcOver` (Normal mode) the shader outputs
    /// `src * opacity` and the hardware blend does the rest; `aux == 0` then. With
    /// `blend == Replace` (every other mode) `aux` holds a copy of the destination taken before
    /// this composite and the shader computes the W3C compositing result itself:
    /// `as = src.a * opacity`, `ab = aux.a`, `Cs = src.rgb / src.a`, `Cb = aux.rgb / aux.a`,
    /// `out.rgb = (1 - ab) * as * Cs + ab * as * B(Cb, Cs) + (1 - as) * aux.rgb`,
    /// `out.a = as + ab * (1 - as)`, where `B` is the separable/non-separable blend function
    /// for `CompositeParams::mode` (docs/design/09-command-list.md's 17 BlendMode values; the
    /// formulas are the ones in gl/src/gl_shaders.h's `blendChannel`).
    Composite = 5,
    /// Effect pass over the target's window; `DrawCmd::program` selects the shader,
    /// `paramIndex` its parameters (the `*Params` structs below), `src`/`aux` its inputs (same
    /// window as the target). Semantics follow gl/src/gl_shaders.h's T-54 shaders one-to-one
    /// (docs/design/18-effects.md §18.5).
    Effect = 6,
    /// Textured quad over a texture that holds *straight* (non-premultiplied) alpha -- a host
    /// texture bound with `AssetResolver::setExternalTexture(..., straightAlpha = true)`
    /// (docs/tasks/T-150.md). `s = sample(texture, u, v)`; output = (s.rgb * s.a, s.a) * vertex
    /// `rgba`. Everything else as `Texture`.
    TextureStraight = 7,
    /// T-202: a `DrawKind::Particles` command whose particles sample `texture`: output =
    /// sample(`texture`, uv) * instance `rgba` (both premultiplied; a straight-alpha texture,
    /// `ParticleParams::straight != 0`, is premultiplied first). See `ParticleInstance`.
    ParticleTexture = 8,
    /// T-202: a `DrawKind::Particles` command without texture: output = instance `rgba`.
    ParticleSolid = 9,
};

/// Output blending. Sources are premultiplied.
enum class Blend : uint8_t {
    Replace = 0,   ///< No blending: the fragment overwrites the target.
    SrcOver = 1,   ///< out = src + dst * (1 - src.a), color and alpha.
    Additive = 2,  ///< T-202: out = src + dst, color and alpha (ONE, ONE; premultiplied source).
};

/// T-202: how a `DrawCmd` draws. `Triangles` is every command before T-202 (its index range);
/// `Particles` draws `instanceCount` instances of `GpuFrame::particleInstances` from
/// `firstInstance`, each a quad expanded by the device (two triangles, six vertices; the command
/// has no vertices or indices of its own).
enum class DrawKind : uint8_t {
    Triangles = 0,
    Particles = 1,
};

/// T-202 (docs/tasks/T-202.md item 5): one particle of a `DrawKind::Particles` command, 40 bytes.
/// The quad is `w` x `h` centered on (`x`, `y`) in the command's particle space, rotated by `rot`
/// radians (x' = x cos - y sin, y' = x sin + y cos, y down); `ParticleParams::xf` maps that space
/// to surface pixels. Corner (cu, cv) in {0, 1}^2 (0, 0 = top-left before rotation) has
/// `u,v = mix((u0, v0), (u1, v1), (cu, cv))`. `rgba` is premultiplied, packed like
/// `Vertex::rgba`.
struct ParticleInstance {
    float x, y;
    float w, h;
    float rot;
    uint32_t rgba;
    float u0, v0, u1, v1;
};
static_assert(sizeof(ParticleInstance) == 40, "ParticleInstance must be 40 bytes");

/// Stencil test/write for a command. `ref` below is `DrawCmd::stencilRef`, a clip depth 0..15.
enum class StencilOp : uint8_t {
    /// Stencil test disabled, nothing written.
    None = 0,
    /// NonZero winding pass. Test `(stencil & 0xF0) == ref << 4`; on pass front faces increment
    /// and back faces decrement (both wrapping), write mask 0x0F; on fail keep.
    /// Front/back follow the fan triangulation's signed area in the y-down pixel space: a
    /// counter-clockwise triangle *as seen with y up* is "front" (OpenGL's default), i.e. the
    /// device keeps its default front-face convention with the y-flip in the projection.
    WriteIncrWrap = 1,
    /// EvenOdd winding pass. Same test as WriteIncrWrap; on pass invert, write mask 0x0F.
    WriteInvert = 2,
    /// Cover test: pass iff `(stencil & 0x0F) != 0`; nothing written.
    TestNotEqualZero = 3,
    /// Clip test: pass iff `(stencil & 0xF0) == ref << 4`; nothing written.
    TestEqualRef = 4,
    /// Unconditional clear of the winding nibble: test always passes; write 0 with write mask
    /// 0x0F.
    ClearRect = 5,
    /// Push one path-clip level: pass iff `(stencil & 0x0F) != 0` (the clip shape's winding,
    /// accumulated by a preceding WriteIncrWrap/WriteInvert at depth `ref - 1`); on pass replace
    /// the whole byte with `ref << 4` (`ref` is the *new* depth), write mask 0xFF.
    ClipPush = 6,
    /// Pop one path-clip level: pass iff `(stencil & 0xF0) == ref << 4` (`ref` is the depth being
    /// popped); on pass decrement, write mask 0xF0 (the byte becomes `(ref - 1) << 4`).
    ClipPop = 7,
};

/// Texture sampling state.
enum class Sampler : uint8_t {
    PointClamp = 0,
    LinearClamp = 1,
};

/// Effect-pass fragment programs (`Shading::Effect`). Each maps to one of gl/src/gl_shaders.h's
/// T-54 shaders (named in the comments); `MaskApply` replaces that file's composite mask modes.
enum class EffectProgram : uint8_t {
    None = 0,
    GaussScalar = 1,   ///< kFxGaussScalarFrag; `GaussParams`; reads `src.r`, writes a scalar to all channels.
    GaussColor = 2,    ///< kFxGaussColorFrag; `GaussParams`; blurs all four channels of `src`.
    Dilate = 3,        ///< kFxDilateFrag; `DilateParams`; separable max filter over `src`.
    AlphaExtract = 4,  ///< kFxAlphaExtractFrag; `AlphaExtractParams`; writes `src.a` (or 1 - a).
    Sdf = 5,           ///< kFxSdfFrag; `SdfParams`; signed distance of `src.a >= 0.5` to an R32F target.
    ColorMatrix = 6,   ///< kFxColorMatrixFrag; `ColorMatrixParams`.
    GradientMask = 7,  ///< kFxGradientMaskFrag; `GradientMaskParams`; `texture` = the stop LUT (alpha used).
    Shine = 8,         ///< kFxShineFrag; `ShineParams`.
    ShadowGlow = 9,    ///< kFxShadowGlowFrag; `ShadowGlowParams`; `aux` = the scalar mask field.
    Outline = 10,      ///< kFxOutlineFrag; `OutlineParams`; `aux` = the R32F distance field.
    Backdrop = 11,     ///< kFxBackdropFrag; no params; `aux` = the blurred backdrop.
    MaskApply = 12,    ///< out = src * m(aux), `MaskApplyParams::mode` picks m (docs/tasks/T-146.md).
    /// T-203: a `.xtfx` custom effect (docs/design/18-effects.md §18.7); `CustomEffectParams`,
    /// whose `program` is the id `GpuDevice::prepareCustomEffect` returned.
    Custom = 13,
    /// T-343 (docs/design/18-effects.md §18.15): 2x2 box downsample into a target half `src`'s
    /// size; `DownsampleParams`. kFxDownsampleFrag.
    Downsample = 14,
    /// T-343: one Kawase blur pass (center + four diagonal taps); `KawaseParams`. kFxKawaseFrag.
    Kawase = 15,
    /// T-343: bilinear upsample of `src` (and `aux`, mixed by `LevelMixParams::t`) through a
    /// uv mapping; `LevelMixParams`. kFxLevelMixFrag.
    LevelMix = 16,
};

/// One draw. Vertices `[baseVertex, baseVertex + vertexCount)` and indices
/// `[firstIndex, firstIndex + indexCount)` of the frame; every index is relative to `baseVertex`
/// and less than `vertexCount`, so a device may upload each command's ranges separately.
/// Triangles are listed (three indices each). One command may hold any number of shapes (the
/// builder batches consecutive draws of identical state, T-155); a device must not assume a
/// fixed vertex count per command.
struct DrawCmd {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t baseVertex = 0;
    uint32_t vertexCount = 0;
    /// Device texture: `Texture`/`Glyph` sample it; `Gradient`/`GradientMask` use it as the LUT.
    TextureHandle texture = 0;
    /// Primary input target of `Composite`/`Effect` (read as a texture); 0 otherwise.
    TargetHandle src = 0;
    /// Secondary input target of `Composite` (backdrop copy) / `Effect` (see EffectProgram); 0 if unused.
    TargetHandle aux = 0;
    /// Start of this command's parameters in `GpuFrame::params` (see the `*Params` structs).
    uint32_t paramIndex = 0;
    Shading shading = Shading::Solid;
    Blend blend = Blend::SrcOver;
    StencilOp stencilOp = StencilOp::None;
    uint8_t stencilRef = 0;
    Sampler sampler = Sampler::LinearClamp;
    EffectProgram program = EffectProgram::None;
    uint8_t _pad[2] = {};
    /// Scissor rectangle in *surface* pixels (always set; the pass window when nothing clips).
    /// The device clips it to the pass window and translates it into the target's own pixels.
    int16_t clipX = 0, clipY = 0, clipW = 0, clipH = 0;
    /// T-202: `Particles` draws instances instead of the index range (see `DrawKind`).
    DrawKind kind = DrawKind::Triangles;
    uint8_t _pad2[3] = {};
    /// T-202, `DrawKind::Particles`: `GpuFrame::particleInstances[firstInstance,
    /// firstInstance + instanceCount)`, drawn with one instanced draw; `paramIndex` holds its
    /// `ParticleParams`.
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
};

/// A contiguous run of commands drawn into one target. Passes are executed in order.
struct Pass {
    TargetHandle target = 0;
    uint32_t firstCmd = 0;
    uint32_t cmdCount = 0;
    /// 1: clear the target's color to (0,0,0,0) and its stencil to 0 before the commands.
    uint8_t clear = 0;
    /// 1: before the commands (and after any clear), copy `copySource`'s color into `target`
    /// over the overlap of their two windows (a resolve when `copySource` is multisampled;
    /// stencil untouched). `copySource`'s window is the one of the last pass that drew it.
    uint8_t copy = 0;
    uint8_t _pad[2] = {};
    TargetHandle copySource = 0;
    /// The pass's *window* in surface pixels (T-151): the projection covers exactly this
    /// rectangle, a transient `target` is this size, vertices and scissors stay in surface
    /// pixels. 0 width/height means the whole target. The surface target (0) always gets
    /// (0, 0, targetWidth, targetHeight).
    int32_t viewX = 0, viewY = 0, viewW = 0, viewH = 0;
};

/// A transient (per-frame offscreen) target. The device keeps the content of transient `i` from
/// the pass that writes it to the last pass that reads it; the builder reuses a number once its
/// content is dead, so a device needs at most `transients.size()` targets per frame. The size
/// is the window of the passes that draw it (a multiple of 32 px and at least 64 x 64 unless the
/// surface is smaller, T-151); a reused number may sit at a different window each time.
///
/// T-343 (docs/design/18-effects.md §18.15): a *scaled* transient -- a pyramid level or a
/// downsampled blur stage -- is smaller than its window: `width`/`height` are the window's size
/// divided by a power of two (rounded up). The device still maps the pass window onto the whole
/// target (viewport = the target, projection = the window) and scales a command's surface-pixel
/// scissor by target / window; a window quad therefore covers the whole target and effect shaders
/// read their texel sizes from their parameters, never from the window.
struct TransientTargetDesc {
    uint16_t width = 0, height = 0;
    TextureFormat format = TextureFormat::Rgba8;
    /// 1: drawn with stencil ops (needs a stencil attachment); 0: fullscreen passes only.
    uint8_t needStencil = 0;
};

/// Effect result cache (T-162, docs/tasks/T-162.md): after pass `pass` has executed its
/// commands (and before the next pass), the device copies the whole content of transient `src`
/// -- its current window, `GpuFrame::transients[src - 1]`'s size, exactly the size the cached
/// target was acquired with -- into the persistent texture `texture`, which the builder obtained
/// from `GpuDevice::acquireCachedTarget`. The builder only stores effect outputs (never a
/// stencil target), but a device that multisamples every transient resolves first. The copy
/// keeps the *uploaded-texture* row order: a later frame samples `texture` through
/// `Shading::Texture` with the window quad's (0,0)-(1,1) u,v, so row 0 of `texture` must be
/// the window's top row (an OpenGL device flips while blitting, its render targets being
/// bottom-up).
struct CacheStore {
    uint32_t pass = 0;
    TargetHandle src = 0;
    TextureHandle texture = 0;
};

/// The output of `FrameBuilder::build`. The vectors keep their capacity across `clear()`.
struct GpuFrame {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<DrawCmd> cmds;
    std::vector<Pass> passes;
    std::vector<float> params;
    std::vector<TransientTargetDesc> transients;
    /// Size of the output surface (`FrameBuilder::resize`).
    int targetWidth = 0;
    int targetHeight = 0;
    /// T-162: effect results to keep for later frames, in pass order (see `CacheStore`). Empty
    /// on a device whose `acquireCachedTarget` returns 0.
    std::vector<CacheStore> cacheStores;
    /// T-202: the instances of every `DrawKind::Particles` command (see `ParticleInstance`).
    std::vector<ParticleInstance> particleInstances;

    /// Empties every vector (capacity retained).
    void clear();
};

// -------------------------------------------------------------------------------------------
// Parameter blocks. Each is a plain struct of floats; the builder appends it to
// `GpuFrame::params` with memcpy and a device reads it back the same way from
// `&params[cmd.paramIndex]`.
// -------------------------------------------------------------------------------------------

/// Width of the gradient lookup textures (`Shading::Gradient`, `EffectProgram::GradientMask`).
inline constexpr uint16_t kGradientLutWidth = 256;

/// `Shading::Gradient`. `kind` 2 = linear (t = dot(p - p0, p1 - p0) / |p1 - p0|^2), 3 = radial
/// (t = |((p - p0).x / rx, (p - p0).y / ry)|); t is clamped to [0, 1]. `|p1 - p0|^2 <= 0` means
/// t = 1. `p` is the interpolated vertex `u,v`. T-201: 4 = conic, t = the clockwise angle from
/// the ray p0 -> p1 to p - p0 over a full turn, in [0, 1) (xt::sw's rule).
struct GradientParams {
    float kind;
    float p0x, p0y, p1x, p1y;
    float rx, ry;
    float alpha;
};

/// `Shading::Glyph`: coverage gamma (docs/design/23-host-integration.md §23.4). T-178 (tail):
/// `effect` != 0 turns the quad into a glyph-level text effect (xt::GlyphEffect: 1 Dilate,
/// 2 Blur) of device radius `radius`; the shader then reads the atlas texel under the fragment
/// (`floor(uv * texSize)`) and its neighbours with `texelFetch`, treating texels outside the
/// glyph's own atlas rectangle `[rectX0, rectX1) x [rectY0, rectY1)` as 0, and outputs
/// vertex `rgba` * the effect coverage (no gamma). The quad is the glyph's quad outset by the
/// effect's kernel radius.
struct GlyphParams {
    float gamma;
    float effect = 0;
    float radius = 0;
    float rectX0 = 0, rectY0 = 0, rectX1 = 0, rectY1 = 0;
    float texSize = 0;
};

/// `Shading::Composite`: `mode` is a `BlendMode` value (0..16), `opacity` in [0, 1].
struct CompositeParams {
    float mode;
    float opacity;
};

/// `GaussScalar`/`GaussColor`: merged-tap separable Gaussian. Index 0 of `weights` is the center
/// sample; entries 1..tapCount are mirrored pairs at +-`offsets[i]` texels along `dir`.
/// `constEdge != 0`: taps outside [0,1]^2 read `outside` instead of clamping (GaussScalar only).
struct GaussParams {
    float texelW, texelH;
    float dirX, dirY;
    float tapCount;
    float constEdge;
    float outside;
    float weights[33];
    float offsets[33];
};

/// `Dilate`: max over `[-radius, radius]` texels along `dir`; `readAlpha != 0` reads `src.a`
/// (first pass, straight from a color source) else `src.r`; taps outside [0,1]^2 read `outside`.
struct DilateParams {
    float texelW, texelH;
    float dirX, dirY;
    float radius;
    float readAlpha;
    float outside;
};

/// `AlphaExtract`: writes `src.a` (`invert == 0`) or `1 - src.a` to every channel.
struct AlphaExtractParams {
    float invert;
};

/// `Sdf`: 8-direction search up to `searchRadius` texels; output = signed distance (negative
/// inside, `src.a >= 0.5` is inside; `searchRadius + 1` when no crossing is found).
struct SdfParams {
    float texelW, texelH;
    float searchRadius;
};

/// `ColorMatrix`: `straight = src.rgb / src.a`; `rgb = clamp(m * straight + offset)`;
/// `a = src.a * opacity`; output premultiplied. `m` is row-major 3x3 (row = output channel).
struct ColorMatrixParams {
    float m[9];
    float offset[3];
    float opacity;
};

/// `GradientMask`: `q = invXf * windowPx` (invXf = a,b,c,d,e,f of a Mat2x3 that already includes
/// the pass window's origin, `windowPx = (u * targetW, v * targetH)` with `targetW/H` the window
/// size), then t as for `GradientParams` except that radial uses
/// |q - p0| / |p1 - p0| and `|p1 - p0|^2 <= 0` means t = 0 (xt::sw's rule); `m = lut(t).a`
/// (inverted when `invert != 0`); output = src * m.
struct GradientMaskParams {
    float invXf[6];
    float p0x, p0y, p1x, p1y;
    float radial;
    float invert;
    float targetW, targetH;
};

/// `Shine`: `band = 1 - smoothstep(edge0, edge1, |dot(windowPx, dir) - center|)` with
/// `windowPx = (u * targetW, v * targetH)` (window size; `center` already accounts for the
/// window's origin); `straight = src.rgb / src.a`;
/// `rgb = clamp(straight + color * band * intensity * src.a)`; output = (rgb * src.a, src.a).
struct ShineParams {
    float dirX, dirY;
    float center, edge0, edge1;
    float r, g, b;
    float intensity;
    float targetW, targetH;
};

/// `ShadowGlow`: `raw = aux.r` sampled at `uv - (offsetU, offsetV)` (`maskOutside` outside
/// [0,1]^2); `g = clamp(raw * scale)`, times `src.a` when `effectAlphaMul != 0`;
/// `eff = (color * g * a, g * a)`; output = srcOver(top, bottom) with `eff` on top when
/// `lineOnTop != 0`, else `src` on top.
struct ShadowGlowParams {
    float offsetU, offsetV;
    float scale;
    float r, g, b, a;
    float effectAlphaMul;
    float lineOnTop;
    float maskOutside;
};

/// `Outline`: `sdf = aux.r`; `position` 0 outer / 1 center / 2 inner selects the mask formula of
/// docs/design/18-effects.md §18.3; line = (color * m * a, m * a); inner lines go on top of
/// `src`, outer/center under it.
struct OutlineParams {
    float radius;
    float softness;
    float position;
    float r, g, b, a;
};

/// `MaskApply`: `m` = aux.a (0 Alpha), 1 - aux.a (1 AlphaInverted), lum(aux.rgb) (2 Luma; aux is
/// premultiplied, so `lum(aux.rgb)` already includes its alpha), 1 - lum(aux.rgb) (3
/// LumaInverted), with lum = dot(rgb, (0.3, 0.59, 0.11)); output = src * m.
struct MaskApplyParams {
    float mode;
};

/// `Custom` (T-203): `program` = the device's custom program id (exact as a float below 2^24);
/// `time` = `CommandList::time` (`xtTime`); `layerW/H` = the pass window in surface pixels
/// (`XtInput.layerSize`, `texelSize` = its reciprocal); `params`/`colors` = the effect's
/// EffectInstance `params[8]` and `colors[2]` (RGBA), from which the device fills the parameter
/// uniforms per xt::customParamSlots().
struct CustomEffectParams {
    float program;
    float time;
    float layerW, layerH;
    float params[8];
    float colors[8];
};

/// T-202 `DrawKind::Particles`: `xf` = (a, b, c, d, e, f) maps particle space to surface pixels
/// (x' = a x + c y + e, y' = b x + d y + f); `straight != 0` when `texture` holds straight alpha.
struct ParticleParams {
    float xf[6];
    float straight = 0;
};

/// `Downsample` (T-343, docs/design/18-effects.md §18.15): the target is `dstW x dstH` texels,
/// `src` is `srcW x srcH` (`dst = ceil(src / 2)`). For the output texel `(x, y)` (top-down,
/// `floor(uv * dst size)`; an OpenGL device flips its fragment row to top-down first), output =
/// the mean of the four `src` texels `(2x, 2y)`, `(2x + 1, 2y)`, `(2x, 2y + 1)`, `(2x + 1,
/// 2y + 1)`, each clamped to `src`'s last column / row. Exact texel fetches, no filtering.
struct DownsampleParams {
    float srcW, srcH;
    float dstW, dstH;
};

/// `Kawase` (T-343): one Kawase blur pass over `src` into a target of the same size.
/// `out = (4 * s(uv) + s(uv + d1) + s(uv - d1) + s(uv + d2) + s(uv - d2)) / 8` with
/// `d1 = (offset * texelW, offset * texelH)`, `d2 = (offset * texelW, -offset * texelH)`, every
/// sample bilinear and clamped to the edge. `offset` is 1.5 texels in the builder's pyramid, so
/// each diagonal tap is exactly the mean of a 2 x 2 block.
struct KawaseParams {
    float texelW, texelH;
    float offset;
};

/// `LevelMix` (T-343): bilinear, edge-clamped upsample. `uvM = (u * scaleU + offU, v * scaleV +
/// offV)` maps the target's window quad into the source levels' own [0,1]^2 (the levels share one
/// window, `uvM` addresses the whole of each); output = `mix(sample(src, uvM), sample(aux, uvM),
/// t)`, or `sample(src, uvM)` alone when `aux == 0`. Sampling is the standard bilinear rule
/// (`p = uvM * size - 0.5`, the four nearest texels, clamped), the one xt::sw reproduces.
struct LevelMixParams {
    float scaleU, scaleV;
    float offU, offV;
    float t;
};

static_assert(sizeof(GradientParams) % sizeof(float) == 0);
static_assert(sizeof(ParticleParams) % sizeof(float) == 0);
static_assert(sizeof(GlyphParams) % sizeof(float) == 0);
static_assert(sizeof(CompositeParams) % sizeof(float) == 0);
static_assert(sizeof(GaussParams) % sizeof(float) == 0);
static_assert(sizeof(DilateParams) % sizeof(float) == 0);
static_assert(sizeof(AlphaExtractParams) % sizeof(float) == 0);
static_assert(sizeof(SdfParams) % sizeof(float) == 0);
static_assert(sizeof(ColorMatrixParams) % sizeof(float) == 0);
static_assert(sizeof(GradientMaskParams) % sizeof(float) == 0);
static_assert(sizeof(ShineParams) % sizeof(float) == 0);
static_assert(sizeof(ShadowGlowParams) % sizeof(float) == 0);
static_assert(sizeof(OutlineParams) % sizeof(float) == 0);
static_assert(sizeof(MaskApplyParams) % sizeof(float) == 0);
static_assert(sizeof(CustomEffectParams) % sizeof(float) == 0);
static_assert(sizeof(DownsampleParams) % sizeof(float) == 0);
static_assert(sizeof(KawaseParams) % sizeof(float) == 0);
static_assert(sizeof(LevelMixParams) % sizeof(float) == 0);

/// Debug/test serialization of a frame's structure (passes, commands, transients; no vertex
/// data), one line per pass/command. Stable across runs; used by tests/gpu's golden snapshots.
void dumpGpuFrame(const GpuFrame& frame, std::string& out);

}  // namespace xt::gpu
