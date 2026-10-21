#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "xt/node.h"
#include "xt/types.h"

/// CommandList: the single interface between the core and a rendering backend. See
/// docs/design/09-command-list.md §9.1, docs/tasks/T-10.md "command_list.h".

namespace xt {

class IrView;

/// See docs/design/09-command-list.md §9.1.
enum class CmdKind : uint8_t {
    FillPath = 0,         ///< Fill a path; payload in CommandList::fills.
    StrokePath = 1,       ///< Stroke a path; payload in CommandList::strokes.
    DrawImage = 2,        ///< Draw an image; payload in CommandList::images.
    DrawGlyphs = 3,       ///< Draw a glyph run; payload in CommandList::glyphRuns.
    PushClipRect = 4,     ///< Push a rectangular clip; payload in CommandList::clipRects.
    PushClipPath = 5,     ///< Push a path-shaped clip; payload in CommandList::clipPaths.
    PopClip = 6,          ///< Pop the most recently pushed clip (rect or path). No payload.
    PushLayer = 7,        ///< Begin an offscreen layer; payload in CommandList::layers.
    PopLayer = 8,         ///< End and composite the most recently pushed layer. No payload.
    PushMask = 9,         ///< Begin drawing a mask's content; payload in CommandList::masks.
    EndMaskContent = 10,  ///< End mask content and begin drawing the masked layer. No payload.
    PopMask = 11,         ///< End and composite the masked layer. No payload.
    // --- docs/design/19-3d-space.md §19.7: 3D projection/layer commands. ---
    PushProjection = 12,   ///< Enter a 3D space root; payload in CommandList::projections.
    PopProjection = 13,    ///< Leave the most recently entered 3D space root. No payload.
    PushTransform3D = 14,  ///< Set the current 3D layer's plane transform; payload in
                           ///< CommandList::transforms3.
    PopTransform3D = 15,   ///< Clear the current 3D layer's plane transform. No payload.
    DrawImageQuad = 16,    ///< Draw a perspective-warped image quad (19.8's flatten3D output; not
                           ///< produced by Painter); payload in CommandList::imageQuads.
    // --- docs/design/18-effects.md §18.5: effect commands. ---
    PushEffect = 17,  ///< Begin a node's effect stack in an offscreen layer; payload in
                      ///< CommandList::effectCmds.
    PopEffect = 18,   ///< Execute the effect stack and composite the result. No payload.
    // --- docs/design/03-scene-and-properties.md §3.12.5, 18 §18.12: particles (T-202). ---
    DrawParticles = 19,  ///< Instanced particle quads; payload in CommandList::particles.
    // --- docs/design/32-rigging-and-deformation.md §32.2.4: image meshes (T-245). ---
    DrawImageMesh = 20,  ///< Textured triangle mesh; payload in CommandList::imageMeshes.
};

/// Deepest nesting of stencil clips the GPU backends apply (docs/tasks/T-263a.md E8): the stencil
/// buffer keeps the clip depth in its high 4 bits. Only clips that are not an axis-aligned
/// rectangle in device pixels count (a path, a rounded rectangle, a rotated or tilted rectangle);
/// axis-aligned rectangles are scissor levels and nest without limit. A stencil clip beyond the
/// limit is reduced to its bounding box (its content is clipped to that axis-aligned box instead
/// of the shape, T-263b) and its PopClip still pairs with it. The software renderer has no limit
/// and applies every clip.
inline constexpr uint32_t kMaxClipDepth = 15;

/// One entry of CommandList::commands. `index` points into the payload vector matching `kind`
/// (e.g. FillPath -> CommandList::fills).
struct Command {
    /// Which command this entry is, and which payload vector `index` refers to.
    CmdKind kind;
    /// T-343: `flags[0]` is a bit set of the `kCommand*` flags below (the Painter writes it, a
    /// host-built list may); `flags[1]` and `flags[2]` are unused padding (0). Not part of any
    /// payload hash: the flags are derived from the payload.
    uint8_t flags[3];
    /// Index into the payload vector selected by `kind`.
    uint32_t index;
};

/// `Command::flags[0]` (T-343, docs/design/23-host-integration.md §23.aa.2): this PushLayer /
/// PushEffect opens a subtree that reads what lies below it on the *host scene* -- a
/// `mix-blend-mode` other than normal (`LayerCmd::blend != Normal`), or an effect stack whose first
/// entry is an enabled BackdropBlur. The Painter sets it on exactly those two commands; a backend
/// in `HostTargetMode::Direct` splits the paint order into bands at the outermost such subtrees
/// (`scanSceneRuns`). Nested marks (an S subtree inside another) change nothing.
inline constexpr uint8_t kCommandSceneRead = 0x01;

/// T-343 (23 §23.aa.1): how the host binds the backend's output target. Carried by
/// `CommandList::hostTargetMode` (Context::render copies Context::setHostTargetMode's value).
enum class HostTargetMode : uint8_t {
    /// The host binds a transparent layer of its own; the UI only sees itself (the default).
    Layer = 0,
    /// Target 0 is the host's scene: never cleared by the backend, read by blend modes and
    /// backdrop blurs, and the paint order is cached in bands (23 §23.aa.2).
    Direct = 1,
};

/// T-343: one run of the top-level paint order as `scanSceneRuns` splits it: commands
/// `[first, end)` are either a *band* (no scene reader; drawn into a cached band target in Direct
/// mode) or one scene-reading subtree (`sceneRead`: a PushLayer / PushEffect carrying
/// kCommandSceneRead at nesting depth 0, through its matching pop).
struct SceneRun {
    uint32_t first = 0;
    uint32_t end = 0;
    bool sceneRead = false;
};

/// Splits `list.commands` into runs (23 §23.aa.2): every maximal range of depth-0 commands between
/// scene-reading subtrees is one band (clip and projection commands included, so a band may leave
/// clips open for the runs after it); every depth-0 PushLayer / PushEffect whose `flags[0]` has
/// kCommandSceneRead is one scene run up to and including its matching pop. Depth 0 means outside
/// every Layer / Mask / Effect section. Only for a structurally valid list (`structureChecked` or
/// validateCommandList()). `out` is cleared first; an empty list yields no run.
struct CommandList;
void scanSceneRuns(const CommandList& list, std::vector<SceneRun>& out);

/// A range into CommandList::verbs / CommandList::points.
struct PathRef {
    /// Offset of the path's first verb in CommandList::verbs.
    uint32_t verbOffset;
    /// Number of verbs in CommandList::verbs.
    uint32_t verbCount;
    /// Offset of the path's first point in CommandList::points.
    uint32_t pointOffset;
    /// Number of points in CommandList::points.
    uint32_t pointCount;
};

/// A paint reference ready for the backend. `kind == PaintKind::None` means "do not fill / do not
/// stroke"; Painter never emits a command carrying such a PaintRef.
struct PaintRef {
    /// Solid color, gradient, or none.
    PaintKind kind;
    Color color;  ///< Solid color, or (for gradients) a (1,1,1,alpha) multiplier.
    /// Linear: gradient start point. Radial: center point. Unused for solid paint.
    Vec2 p0;
    /// Linear: gradient end point. Radial: center plus the horizontal radius (rx), i.e. `p1.x ==
    /// p0.x + rx`, `p1.y == p0.y`. Unused for solid paint.
    Vec2 p1;
    /// Radial only (T-116): center plus the vertical radius (ry), i.e. `p2.x == p0.x`,
    /// `p2.y == p0.y + ry`. `p2 == p0` means "no second axis was set" and the gradient is the
    /// pre-T-116 circle of radius `|p1 - p0|` (this can't collide with a genuine ellipse, since
    /// that would require `ry == 0`, a degenerate gradient no producer emits). Unused for Linear
    /// and solid paint.
    Vec2 p2;
    /// Offset of the first gradient stop in CommandList::stops.
    uint32_t stopOffset;
    /// Number of gradient stops in CommandList::stops.
    uint32_t stopCount;
};

/// Payload for `CmdKind::FillPath`.
struct FillCmd {
    /// Transform applied to the path before filling.
    Mat2x3 xf;
    /// Path geometry to fill.
    PathRef path;
    /// Fill paint.
    PaintRef paint;
    /// Winding rule used to determine the filled region.
    FillRule rule;
    /// Whether edges should be antialiased.
    uint8_t antialias;
};

/// Payload for `CmdKind::StrokePath`.
struct StrokeCmd {
    /// Transform applied to the path before stroking.
    Mat2x3 xf;
    /// Path geometry to stroke.
    PathRef path;
    /// Stroke paint.
    PaintRef paint;
    /// Stroke width in `xf`'s coordinate system.
    float width;
    /// Line cap style for open subpath ends.
    LineCap cap;
    /// Line join style for corners.
    LineJoin join;
    /// Miter limit for `LineJoin::Miter`.
    float miterLimit;
    /// Offset of the dash pattern in CommandList::dashes.
    uint32_t dashOffset;
    /// Number of entries in the dash pattern (0 = solid stroke).
    uint32_t dashCount;
    /// Starting phase into the dash pattern.
    float dashPhase;
};

/// Payload for `CmdKind::DrawImage`.
struct ImageCmd {
    /// Transform applied to `dst` before drawing.
    Mat2x3 xf;
    /// Image asset to draw.
    AssetId asset;
    /// Destination rectangle in `xf`'s coordinate system.
    Rect dst;
    Rect srcUv;  ///< 0..1
    /// Opacity multiplier, [0, 1].
    float opacity;
    /// Whether to use bilinear (rather than nearest-neighbor) sampling.
    uint8_t smooth;
    uint8_t _pad = 0;  ///< Padding, unused (0).
    /// T-263d: which entry of CommandList::sources `asset` belongs to (see CommandSource); 0 in a
    /// list that names no source.
    uint16_t source = 0;
    /// T-345 (25 §25.4.2 item 4): the drawing element's `ui-scale` factor; the backend asks the
    /// resolver for the variant at this x its dpr (`@2x` at >= 1.5, `@3x` at >= 2.5).
    float variantScale = 1.0f;
};

/// Glyph-level text effect of a DrawGlyphs command (docs/design/07-text.md §7.8, T-178). The
/// backend draws `GlyphsCmd::color` through the glyphs' coverage transformed as below instead of
/// the plain coverage; each glyph is processed on its own (effects of neighbouring glyphs are
/// composited source-over, not merged). Radii are in `xf` units and converted to device pixels by
/// the backend (capped at 16 device px). Only drawn where the glyph's coverage bitmap is available
/// (an axis-aligned, `text-rendering: auto`, unprojected run); elsewhere an effect command draws
/// nothing.
enum class GlyphEffect : uint8_t {
    /// Plain coverage (every command before T-178).
    None = 0,
    /// `outline <w>`: `m(p) = max over integer offsets d, |d| <= ceil(R), of cov(p + d) *
    /// clamp(R + 1 - |d|, 0, 1)` with R = the device radius (so a 1 px outline is opaque).
    Dilate = 1,
    /// `glow <r>` / `shadow ... <r>`: Gaussian blur of the coverage, sigma = R / 2, kernel
    /// radius ceil(3 sigma), weights normalized over the whole kernel (R = 0: plain coverage).
    Blur = 2,
};

/// Payload for `CmdKind::DrawGlyphs`.
struct GlyphsCmd {
    /// Transform applied to glyph positions before drawing.
    Mat2x3 xf;
    /// Font the glyphs belong to.
    FontId font;
    /// Font size in px.
    float size;
    /// Glyph fill color.
    Color color;
    /// CSS `text-rendering` of the element the run belongs to (T-101). `Auto` lets the backend
    /// take the small-size bitmap path with coverage gamma; `GeometricPrecision` forces the
    /// outline path with no gamma.
    TextRendering rendering;
    /// Offset of the run's first glyph in CommandList::glyphs / CommandList::glyphPos.
    uint32_t glyphOffset;
    /// Number of glyphs in the run.
    uint32_t glyphCount;
    /// T-178: glyph-level text effect (None for plain text).
    GlyphEffect effect;
    uint8_t _pad[3];  ///< Padding, unused (0).
    /// T-178: the effect's radius in `xf` units (outline width / blur radius), 0..8.
    float effectRadius;
};

/// Whether a backend may draw the run `cmd` from axis-aligned glyph bitmaps (the software
/// renderer's hinted-bitmap path, the GPU glyph atlas) instead of filling outlines, given
/// `deviceXf` = the device pixel ratio scale times `cmd.xf`: `text-rendering: auto`, and a
/// transform that is a positive uniform scale plus a translation. A rotated, skewed, mirrored or
/// non-uniformly scaled run has no bitmap that fits it. One definition for every backend
/// (docs/tasks/T-263b.md E3); a backend may add its own limits (the software renderer's 24 px).
inline bool glyphRunBitmapApplies(const GlyphsCmd& cmd, const Mat2x3& deviceXf) {
    return cmd.rendering == TextRendering::Auto && deviceXf.b == 0.0f && deviceXf.c == 0.0f &&
           deviceXf.a == deviceXf.d && deviceXf.a > 0.0f;
}

/// The size in device px the glyph bitmaps of `cmd` are rasterized at: `cmd.size` times the
/// run's scale (`deviceXf.a`, which glyphRunBitmapApplies() makes the whole scale), on the 1/4 px
/// grid the shaper's bitmap cache and the glyph atlas key on (§7.4).
inline float glyphRunBitmapSize(const GlyphsCmd& cmd, const Mat2x3& deviceXf) {
    const float sizeDevice = cmd.size * std::fabs(deviceXf.a);
    return std::floor(sizeDevice * 4.0f + 0.5f) / 4.0f;
}

/// Payload for `CmdKind::PushClipRect`.
struct ClipRectCmd {
    /// Transform applied to `rect`.
    Mat2x3 xf;
    Rect rect;  ///< in xf's coordinate system (may be rotated)
};

/// Payload for `CmdKind::PushClipPath`.
struct ClipPathCmd {
    /// Transform applied to the path.
    Mat2x3 xf;
    /// Path geometry to clip to.
    PathRef path;
    /// Winding rule used to determine the clipped region.
    FillRule rule;
};

/// Payload for `CmdKind::PushLayer`.
struct LayerCmd {
    /// Opacity multiplier applied when compositing the layer, [0, 1].
    float opacity;
    /// Blend mode used when compositing the layer.
    BlendMode blend;
    Rect boundsHint;  ///< world-space AABB; may be the full viewport
};

/// Payload for `CmdKind::PushMask`.
struct MaskCmd {
    /// How the mask content combines with the masked layer.
    MaskMode mode;
    Rect boundsHint;  ///< world-space AABB; may be the full viewport
};

/// Payload for `CmdKind::PushProjection`. See docs/design/19-3d-space.md §19.4 step 5, §19.7.
struct ProjectionCmd {
    /// The 3D space root's camera/CSS-perspective view-projection matrix (19.4 step 5).
    Mat4 viewProj;
    /// The root's own `world` (2D), mapping the projected point back into the outer mapping's
    /// space (19.7's `rootXf`).
    Mat2x3 rootXf;
    /// The root's reference box (19.4's `box`), in `rootXf`'s coordinate system.
    Rect boxRect;
    /// 0.3 reserved (19.6): 1 = the backend depth-tests this projection's content.
    uint8_t depthTest;
    uint8_t _pad[3];  ///< Padding, unused.
    /// Screen-space AABB of the projected subtree (19.6 step 1, T-105), clipped to the viewport;
    /// the whole viewport when the subtree cannot be bounded. Used by the damage list (23.2) and
    /// SW partial repaint (23.3).
    Rect boundsHint;
};

/// Payload for `CmdKind::PushTransform3D`. See docs/design/19-3d-space.md §19.5, §19.7.
struct Transform3DCmd {
    /// The current 3D layer's transform relative to its 3D space root (NodeCommon::world3).
    Mat4 world3;
};

/// Payload for `CmdKind::DrawImageQuad`. See docs/design/19-3d-space.md §19.7-19.8; produced by
/// CommandListSimplifier::simplify's flatten3D step, not by Painter.
struct ImageQuadCmd {
    /// Image asset to draw.
    AssetId asset;
    /// The four destination corners, in screen/root-box coordinates, clockwise from top-left.
    Vec2 p[4];
    Rect srcUv;  ///< 0..1
    /// Opacity multiplier, [0, 1].
    float opacity;
    /// Whether to use bilinear (rather than nearest-neighbor) sampling.
    uint8_t smooth;
    uint8_t _pad = 0;  ///< Padding, unused (0).
    /// T-263d: which entry of CommandList::sources `asset` belongs to (see CommandSource); 0 in a
    /// list that names no source. flatten3D copies it from the DrawImage it replaces.
    uint16_t source = 0;
    /// T-345: as ImageCmd::variantScale (flatten3D copies it too).
    float variantScale = 1.0f;
};

/// Payload for `CmdKind::PushEffect`. See docs/design/18-effects.md §18.5.
struct EffectCmd {
    /// Offset/count of this node's *enabled* effects, copied into CommandList::effects (disabled
    /// effects are not copied).
    uint32_t effectOffset, effectCount;
    /// The node's plane -> screen transform (includes dpr; 3D layers/descendants: the anchor's
    /// Jacobian, docs/design/19-3d-space.md §19.6).
    Mat2x3 xf;
    /// Content AABB (screen space) outset by the stacked effects' expansion, clamped to the
    /// viewport. Never the full viewport as a lazy default (unlike LayerCmd::boundsHint).
    Rect boundsHint;
    /// The unexpanded content AABB (screen space); Shine's band needs the un-outset content box.
    Rect contentBounds;
    /// T-263d: which entry of CommandList::sources the `shader` ids of this command's Custom
    /// EffectInstances belong to (see CommandSource); 0 in a list that names no source.
    uint16_t source = 0;
    uint16_t _pad = 0;  ///< Padding, unused (0).
};

/// One particle of a DrawParticles command (docs/design/18-effects.md §18.12, T-202): a `w` x `h`
/// quad centered on (`x`, `y`) in the command's `xf` space, rotated by `rot` radians about its
/// center (positive = clockwise on screen, y grows downward). Its color is `rgba`
/// (premultiplied) times the command's texture sampled over [`uv0`, `uv1`] (the quad's top-left
/// corner samples `uv0`, its bottom-right `uv1`), or `rgba` alone when the command has no texture.
struct ParticleInstance {
    float x, y;  ///< Center.
    float w, h;  ///< Size.
    float rot;   ///< Radians.
    Color rgba;  ///< Premultiplied color (the node's alpha folded in).
    Vec2 uv0;    ///< Texture coordinate of the quad's top-left corner.
    Vec2 uv1;    ///< Texture coordinate of the quad's bottom-right corner.
};

/// Payload for `CmdKind::DrawParticles` (T-202): `count` instances starting at
/// CommandList::particleInstances[instanceOffset], drawn in order.
struct ParticlesCmd {
    /// Transform applied to every instance (the emitter element's world transform).
    Mat2x3 xf;
    /// Image drawn per particle; kInvalidAsset = a solid square of the instance color.
    AssetId texture;
    /// Normal: source-over. Additive: dst + src (premultiplied).
    ParticleBlend blend;
    uint8_t _pad = 0;  ///< Padding, unused (0).
    /// T-263d: which entry of CommandList::sources `texture` belongs to (see CommandSource); 0 in
    /// a list that names no source.
    uint16_t source = 0;
    /// Offset of the first instance in CommandList::particleInstances.
    uint32_t instanceOffset;
    /// Number of instances (> 0; the Painter emits no empty command).
    uint32_t count;
};

/// One vertex of a DrawImageMesh command (docs/design/32-rigging-and-deformation.md §32.2.4,
/// T-245): `p` is already in screen/root-box coordinates (the same space as ImageQuadCmd::p, i.e.
/// the node's world transform applied); `uv` is the normalized texture coordinate (0..1).
struct MeshVertex {
    Vec2 p;   ///< Position (screen/root-box coordinates).
    Vec2 uv;  ///< Texture coordinate, 0..1.
};

/// Payload for `CmdKind::DrawImageMesh` (32 §32.2.4, T-245): `indexCount / 3` triangles whose
/// indices (CommandList::meshIndices[indexOffset..]) are relative to `vertexOffset` in
/// CommandList::meshVertices. Each triangle is texture-mapped affinely (no perspective correction).
struct ImageMeshCmd {
    /// Image asset to draw.
    AssetId asset;
    /// Offset/count of the mesh's vertices in CommandList::meshVertices.
    uint32_t vertexOffset, vertexCount;
    /// Offset/count of the mesh's triangle indices in CommandList::meshIndices (3 per triangle).
    uint32_t indexOffset, indexCount;
    /// Opacity multiplier, [0, 1].
    float opacity;
    /// Whether to use bilinear (rather than nearest-neighbor) sampling.
    uint8_t smooth;
    uint8_t _pad = 0;  ///< Padding, unused (0).
    /// T-263d: which entry of CommandList::sources `asset` belongs to (see CommandSource); 0 in a
    /// list that names no source.
    uint16_t source = 0;
};

/// One shader parameter declaration (0.3; defined now for the on-disk/CommandList layout).
struct ShaderParamDesc {
    uint32_t name;    ///< StringId.
    uint8_t type;     ///< 0 Float, 1 Vec2, 2 Color.
    uint8_t _pad[3];  ///< Padding, unused.
    float defaultValue[4];
    float minValue, maxValue;
};

/// A custom effect asset reference (T-203; docs/design/18-effects.md §18.5, §18.7). Painter
/// copies one per distinct `.xtfx` asset an enabled Custom effect of the frame uses (from
/// Document's SHDR-derived pools); `EffectInstance::shader` of a Custom effect finds it by
/// (`source`, `asset`), the source being its PushEffect's (T-263d). SW applies `fallback`; GL runs
/// `glsl` and falls back on a compile failure.
struct ShaderRef {
    AssetId asset;
    /// Offset/length into CommandList::shaderText (UTF-8, no NUL): the HLSL body as written.
    uint32_t sourceOffset, sourceLength;
    /// Offset/count into CommandList::shaderParams.
    uint32_t paramOffset, paramCount;
    float expandPx;
    /// kind == None means "do not apply" (used when SW/GL fail to run the custom shader).
    EffectInstance fallback;
    // -- T-203 (tail) --
    /// Offset/length into CommandList::shaderText: the GLSL 330 module (xt/compile/xtfx.h).
    uint32_t glslOffset, glslLength;
    /// Offset/length into CommandList::shaderText: the parameter names, NUL-separated and
    /// NUL-terminated, in `shaderParams` order (the GL uniform names; a CommandList has no
    /// string table, so `ShaderParamDesc::name` alone cannot name a uniform).
    uint32_t namesOffset, namesLength;
    /// T-263d: which entry of CommandList::sources `asset` belongs to (see CommandSource); 0 in a
    /// list that names no source. Not serialized by the baked format.
    uint16_t source = 0;
    uint16_t _pad = 0;  ///< Padding, unused (0).
};

/// T-203: where a custom effect's declared parameters live in its EffectInstance (and so which
/// `effectParam()` PropId animates each). In declaration order a Float takes `params[s]`, a Vec2
/// `params[s]`/`params[s + 1]` (`s` = the next free float slot) and a Color `colors[c]` (the next
/// free color); `outSlot[i]` receives `s` or `c`. Returns false when the table needs more than the 8
/// float slots or the 2 colors an EffectInstance has (the `.xtfx` header rejects that).
/// `types[i]` is ShaderParamDesc::type (0 Float, 1 Vec2, 2 Color).
inline bool customParamSlots(const uint8_t* types, size_t count, uint8_t* outSlot) {
    uint8_t nextFloat = 0;
    uint8_t nextColor = 0;
    for (size_t i = 0; i < count; ++i) {
        if (types[i] == 2) {
            if (nextColor >= 2) {
                return false;
            }
            outSlot[i] = nextColor++;
        } else {
            const uint8_t width = types[i] == 1 ? 2 : 1;
            if (nextFloat + width > 8) {
                return false;
            }
            outSlot[i] = nextFloat;
            nextFloat = static_cast<uint8_t>(nextFloat + width);
        }
    }
    return true;
}

/// One entry of CommandList::sources (docs/tasks/T-263d.md): the IR whose `AssetId`s the commands
/// stamped with this entry's index use. Every IR numbers its assets from 0, so a list painted from
/// several Documents (Context::render) names each of them here, and the asset-carrying commands
/// (ImageCmd, ImageQuadCmd, ParticlesCmd, ImageMeshCmd, EffectCmd, ShaderRef) say which one they
/// mean in their `source` field.
struct CommandSource {
    /// The Document's IR. Valid until the next Context::update() or any unload / reload of that
    /// Document: a host renders the list in the frame it was built.
    const IrView* ir;
    /// `ir->loadSerial()` when the list was built: what a backend caches the IR's textures under.
    uint32_t loadSerial;
};

/// The single interface between xt_core and a rendering backend. See
/// docs/design/09-command-list.md §9.1-9.2.
struct CommandList {
    /// The ordered command stream a backend replays.
    std::vector<Command> commands;
    /// `FillPath` payloads, indexed by `Command::index`.
    std::vector<FillCmd> fills;
    /// `StrokePath` payloads, indexed by `Command::index`.
    std::vector<StrokeCmd> strokes;
    /// `DrawImage` payloads, indexed by `Command::index`.
    std::vector<ImageCmd> images;
    /// `DrawGlyphs` payloads, indexed by `Command::index`.
    std::vector<GlyphsCmd> glyphRuns;
    /// `PushClipRect` payloads, indexed by `Command::index`.
    std::vector<ClipRectCmd> clipRects;
    /// `PushClipPath` payloads, indexed by `Command::index`.
    std::vector<ClipPathCmd> clipPaths;
    /// `PushLayer` payloads, indexed by `Command::index`.
    std::vector<LayerCmd> layers;
    /// `PushMask` payloads, indexed by `Command::index`.
    std::vector<MaskCmd> masks;
    /// `PushProjection` payloads, indexed by `Command::index`.
    std::vector<ProjectionCmd> projections;
    /// `PushTransform3D` payloads, indexed by `Command::index`.
    std::vector<Transform3DCmd> transforms3;
    /// `DrawImageQuad` payloads, indexed by `Command::index`.
    std::vector<ImageQuadCmd> imageQuads;
    /// `PushEffect` payloads, indexed by `Command::index`.
    std::vector<EffectCmd> effectCmds;
    /// Effect instance pool referenced by `EffectCmd::effectOffset`/`effectCount` (docs/design/
    /// 18-effects.md §18.2); GradientMask entries' `stopOffset`/`stopCount` index `stops` below.
    std::vector<EffectInstance> effects;
    /// Shader asset pool: one entry per `.xtfx` asset used by the frame's Custom effects (T-203).
    std::vector<ShaderRef> shaders;
    /// `ShaderRef::paramOffset`/`paramCount` pool.
    std::vector<ShaderParamDesc> shaderParams;
    /// `ShaderRef::sourceOffset`/`sourceLength` pool (UTF-8 source text, concatenated).
    std::vector<char> shaderText;
    // Payload pools.
    std::vector<PathVerb> verbs;      ///< Packed path verb pool referenced by every PathRef.
    std::vector<float> points;        ///< Path point pool referenced by every PathRef.
    std::vector<GradientStop> stops;  ///< Gradient stop pool referenced by every PaintRef.
    std::vector<uint32_t> glyphs;     ///< glyph id
    std::vector<Vec2> glyphPos;       ///< per-glyph baseline origin (xf coordinate system)
    std::vector<float> dashes;        ///< Dash pattern segment-length pool referenced by StrokeCmd.
    /// `DrawParticles` payloads, indexed by `Command::index` (T-202).
    std::vector<ParticlesCmd> particles;
    /// Instance pool referenced by ParticlesCmd::instanceOffset/count (T-202).
    std::vector<ParticleInstance> particleInstances;
    /// `DrawImageMesh` payloads, indexed by `Command::index` (T-245).
    std::vector<ImageMeshCmd> imageMeshes;
    /// Vertex pool referenced by ImageMeshCmd::vertexOffset/vertexCount (T-245).
    std::vector<MeshVertex> meshVertices;
    /// Triangle index pool referenced by ImageMeshCmd::indexOffset/indexCount (T-245); each index
    /// is relative to its command's `vertexOffset`.
    std::vector<uint16_t> meshIndices;
    Rect viewport;  ///< this frame's logical viewport
    /// Ratio of physical to logical pixels for this frame.
    float devicePixelRatio = 1;
    /// T-203 (tail): the Context clock (seconds) when this list was produced; custom effects read
    /// it as `xtTime` (docs/design/18-effects.md §18.7). Not serialized by the baked format.
    float time = 0;
    /// True when the library itself built this list (Context) or already checked it
    /// (BakedPlayer::frame, CommandListSimplifier): the backends then skip validateCommandList().
    /// A list a host fills by hand leaves it false and is checked on every render. Not serialized;
    /// clear() resets it.
    bool structureChecked = false;
    /// T-263c: which Context::render() call produced this list -- a process-wide count, never 0
    /// for such a list; 0 for a list a host fills by hand, one the simplifier rewrote, a baked
    /// frame. Context::render(out, &damage) takes the `out` it is handed as the damage base only
    /// when this is the serial of that Context's previous render() (and `structureChecked` is
    /// still set); a copy of the list carries it along. Not serialized; clear() resets it.
    /// T-263e: so a host that wants partial damage hands the list back to the next render()
    /// exactly as it got it and never writes this field (see Context::render).
    uint32_t frameSerial = 0;
    /// T-263d: the IRs this list's assets come from. Empty (a baked frame, a list a host fills by
    /// hand, Document::paint called directly): every command's assets belong to the IrView passed
    /// to the backend's render / build call. Not empty (Context::render appends one entry per
    /// Document it paints, in paint order): an asset command's `source` indexes it and the call's
    /// IrView is not used for assets (it may be null); an index outside the table fails
    /// validateCommandList(). The pointers are valid until the next Context::update() or any
    /// unload / reload of those Documents -- the list is rendered in the frame it was built, and a
    /// list kept longer (the previous frame's, as a damage base) is never read through them. Not
    /// serialized; clear() empties it (capacity retained).
    std::vector<CommandSource> sources;
    /// T-343 (23 §23.aa.1): the host target mode this list is drawn under; Context::render copies
    /// Context::hostTargetMode(). A backend in `Direct` leaves target 0 uncleared and caches the
    /// paint order in bands; `Layer` is every frame before T-343. Not serialized; clear() leaves it.
    HostTargetMode hostTargetMode = HostTargetMode::Layer;
    /// T-343 (23 §23.aa.4): the host says its scene is not changing (Context::setSceneStatic), so a
    /// Direct backend may also cache what the scene readers produced. Not serialized; clear()
    /// leaves it.
    bool sceneStatic = false;

    /// Clears every vector (capacity retained), `structureChecked` and `frameSerial`; leaves
    /// viewport/devicePixelRatio, `hostTargetMode` and `sceneStatic` untouched.
    void clear();
};

/// The IR the assets of a command stamped `source` belong to (docs/tasks/T-263d.md): `callIr` --
/// the IrView handed to the backend's render / build call -- when `list.sources` is empty,
/// otherwise the table's entry (nullptr for an index outside it; `callIr` is not used). Every
/// backend resolves an asset command's ids through this.
inline const IrView* sourceIr(const CommandList& list, uint16_t source, const IrView* callIr) {
    if (list.sources.empty()) {
        return callIr;
    }
    return source < list.sources.size() ? list.sources[source].ir : nullptr;
}

/// Debug serialization of a CommandList, one line per command. Used as the Painter tests' snapshot
/// format; see docs/tasks/T-10.md "command_list.h" for the exact per-kind line grammar. All
/// floating point values are formatted with "%.3f".
void dumpCommandList(const CommandList& list, std::string& out);

/// Structural check of a CommandList that may come from outside the Painter (a baked `.xtcl`
/// frame, a host) before anything indexes its pools (docs/tasks/T-263a.md E1). True when:
/// every `Command::index` is inside the payload vector its kind selects (and the kind is known);
/// every offset + count range (path verbs/points, paint and effect stops, dashes, glyphs and their
/// positions, effects, particle instances, mesh vertices/indices, shader text/params) lies inside
/// its pool, computed in 64 bits (an empty range is accepted at any offset); when `sources` is not
/// empty, the `source` of every asset command and of every ShaderRef indexes it; a path's verbs need
/// no more points than its range holds and use no unknown verb; PopClip / PopProjection never pop
/// an empty stack and PushTransform3D / PopTransform3D only occur inside a projection; Layer,
/// Mask and Effect sections nest properly with one another (PopLayer closes a PushLayer,
/// EndMaskContent follows its PushMask once, PopMask closes a mask after EndMaskContent,
/// PopEffect closes a PushEffect); and every stack is empty again at the end. Allocates nothing.
/// SwRenderer::render, gpu::FrameBuilder::build and CommandListSimplifier::simplify call it first
/// unless CommandList::structureChecked is set; BakedPlayer::frame calls it on every decoded frame.
bool validateCommandList(const CommandList& list);

}  // namespace xt
