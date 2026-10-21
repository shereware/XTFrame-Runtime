#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/types.h"

/// `.xtcl` baked frame stream: packed on-disk mirrors of every CommandList element struct, plus
/// BakedWriter (encode) and BakedPlayer (decode). See docs/design/09-command-list.md §9.6,
/// docs/design/12-compiler.md §12.5, docs/design/19-3d-space.md §19.7/§19.8, docs/tasks/T-37.md
/// "`.xtcl`", docs/tasks/T-49.md.
///
/// File layout:
///   Header (28 bytes): char magic[4] = "XTCL"; u16 major = 1; u16 minor = 4; u32 frameCount;
///                       f32 fps; f32 width; f32 height; f32 dpr.
///   Per frame: u32 frameIndex; u32 payloadLength; then payloadLength bytes of payload.
///   Payload: for each CommandList vector, in this order --
///     commands, fills, strokes, images, glyphRuns, clipRects, clipPaths, layers, masks, verbs,
///     points, stops, glyphs, glyphPos, dashes, projections, transforms3, imageQuads, effectCmds,
///     effects, shaders, shaderParams, shaderText -- a u32 element count followed by that many
///     packed elements (raw bytes for verbs/points/glyphs/glyphPos/dashes/shaderText, which are
///     already POD/no-padding types).
///   Not stored (T-263f): particles, particleInstances, imageMeshes, meshVertices, meshIndices --
///   BakedWriter::writeFrame refuses a list with a DrawParticles or DrawImageMesh command.
///
/// `minor == 0` files (written before docs/tasks/T-49.md) end after `dashes`; BakedPlayer::load
/// accepts them and frame() leaves `projections`/`transforms3`/`imageQuads` empty for every frame
/// (19.7's "讀舊檔仍可"). `minor == 1` files (written before docs/tasks/T-52.md) end after
/// `imageQuads`; frame() then also leaves `effectCmds`/`effects`/`shaders`/`shaderParams`/
/// `shaderText` empty. `minor == 2` files (written before docs/tasks/T-105.md) store each
/// projection without its `boundsHint`; frame() reads that shorter layout and fills the field
/// with the viewport (the pre-T-105 damage bound). `minor == 3` files (written before
/// docs/tasks/T-116.md) store each PaintRef without its `p2` (radial second axis); frame() reads
/// that shorter layout and fills `p2` with `p0` (the pre-T-116 circle).
///
/// Note: docs/tasks/T-37.md's prose labels the header "24 bytes" but its own field list (magic +
/// major + minor + frameCount + fps + width + height + dpr) sums to 28 bytes; this implementation
/// follows the explicit field list (28 bytes) as the more authoritative of the two -- see the
/// T-37 report's "uncovered cases".
namespace xt {

#pragma pack(push, 1)

/// On-disk `.xtcl` file header (28 bytes, packed).
struct BakedHeader {
    char magic[4];        ///< File signature, always "XTCL".
    uint16_t major;       ///< Format major version.
    uint16_t minor;       ///< Format minor version.
    uint32_t frameCount;  ///< Number of frames stored in the file.
    float fps;            ///< Nominal playback rate the frames were baked at.
    float width;          ///< Viewport width (logical pixels) all frames were rendered at.
    float height;         ///< Viewport height (logical pixels) all frames were rendered at.
    float dpr;            ///< Device pixel ratio all frames were rendered at.
};
static_assert(sizeof(BakedHeader) == 28);

/// Packed mirror of PathRef: an offset/count pair into the shared verb and point arrays.
struct PackedPathRef {
    uint32_t verbOffset;   ///< See PathRef::verbOffset.
    uint32_t verbCount;    ///< See PathRef::verbCount.
    uint32_t pointOffset;  ///< See PathRef::pointOffset.
    uint32_t pointCount;   ///< See PathRef::pointCount.
};
static_assert(sizeof(PackedPathRef) == 16);

/// Packed mirror of PaintRef (solid color or gradient paint).
struct PackedPaintRef {
    uint8_t kind;         ///< PaintRef::kind (solid vs. gradient).
    float r;              ///< Solid color red, or gradient stop-independent tint red.
    float g;              ///< Solid color green, or gradient stop-independent tint green.
    float b;              ///< Solid color blue, or gradient stop-independent tint blue.
    float a;              ///< Solid color alpha, or gradient stop-independent tint alpha.
    float p0x;            ///< Gradient start (or center) point x.
    float p0y;            ///< Gradient start (or center) point y.
    float p1x;            ///< Gradient end (or edge) point x.
    float p1y;            ///< Gradient end (or edge) point y.
    float p2x;            ///< Radial only (T-116): center + vertical radius point x. p2x == p0x
                          ///< and p2y == p0y means "no second axis", i.e. the pre-T-116 circle.
    float p2y;            ///< Radial only (T-116): center + vertical radius point y.
    uint32_t stopOffset;  ///< Offset into the shared gradient-stop array.
    uint32_t stopCount;   ///< Count of gradient stops.
};
static_assert(sizeof(PackedPaintRef) == 49);

/// Packed mirror of Command: a command kind plus an index into the matching per-kind array.
struct PackedCommand {
    uint8_t kind;    ///< Command::kind (CmdKind).
    uint32_t index;  ///< Command::index.
};
static_assert(sizeof(PackedCommand) == 5);

/// Packed mirror of FillCmd.
struct PackedFillCmd {
    float xf[6];           ///< Row-major Mat2x3 (a, b, c, d, e, f).
    PackedPathRef path;    ///< Path to fill.
    PackedPaintRef paint;  ///< Fill paint.
    uint8_t rule;          ///< Fill rule (FillRule).
    uint8_t antialias;     ///< Nonzero if antialiasing is requested.
};

/// Packed mirror of StrokeCmd.
struct PackedStrokeCmd {
    float xf[6];           ///< Row-major Mat2x3 (a, b, c, d, e, f).
    PackedPathRef path;    ///< Path to stroke.
    PackedPaintRef paint;  ///< Stroke paint.
    float width;           ///< Stroke width.
    uint8_t cap;           ///< Line cap style (LineCap).
    uint8_t join;          ///< Line join style (LineJoin).
    float miterLimit;      ///< Miter join limit.
    uint32_t dashOffset;   ///< Offset into the shared dash-length array.
    uint32_t dashCount;    ///< Count of dash lengths.
    float dashPhase;       ///< Starting offset into the dash pattern.
};

/// Packed mirror of ImageCmd.
struct PackedImageCmd {
    float xf[6];     ///< Row-major Mat2x3 (a, b, c, d, e, f).
    uint32_t asset;  ///< Image AssetId.
    float dstX;      ///< Destination rectangle x, in local space.
    float dstY;      ///< Destination rectangle y, in local space.
    float dstW;      ///< Destination rectangle width, in local space.
    float dstH;      ///< Destination rectangle height, in local space.
    float srcX;      ///< Source rectangle x, in image pixels.
    float srcY;      ///< Source rectangle y, in image pixels.
    float srcW;      ///< Source rectangle width, in image pixels.
    float srcH;      ///< Source rectangle height, in image pixels.
    float opacity;   ///< Multiplied opacity in [0, 1].
    uint8_t smooth;  ///< Nonzero to request bilinear filtering.
};

/// Packed mirror of GlyphsCmd.
struct PackedGlyphsCmd {
    float xf[6];           ///< Row-major Mat2x3 (a, b, c, d, e, f).
    uint32_t font;         ///< FontId to draw with.
    float size;            ///< Font size in logical pixels.
    float r;               ///< Glyph fill color red.
    float g;               ///< Glyph fill color green.
    float b;               ///< Glyph fill color blue.
    float a;               ///< Glyph fill color alpha.
    uint32_t glyphOffset;  ///< Offset into the shared glyph/glyphPos arrays.
    uint32_t glyphCount;   ///< Count of glyphs.
};

/// Packed mirror of ClipRectCmd.
struct PackedClipRectCmd {
    float xf[6];  ///< Row-major Mat2x3 (a, b, c, d, e, f).
    float rectX;  ///< Clip rectangle x, in local space.
    float rectY;  ///< Clip rectangle y, in local space.
    float rectW;  ///< Clip rectangle width, in local space.
    float rectH;  ///< Clip rectangle height, in local space.
};

/// Packed mirror of ClipPathCmd.
struct PackedClipPathCmd {
    float xf[6];         ///< Row-major Mat2x3 (a, b, c, d, e, f).
    PackedPathRef path;  ///< Clip path.
    uint8_t rule;        ///< Fill rule used to interpret the clip path (FillRule).
};

/// Packed mirror of LayerCmd (push/pop of an offscreen compositing layer).
struct PackedLayerCmd {
    float opacity;  ///< Layer opacity in [0, 1].
    uint8_t blend;  ///< Blend mode applied when compositing the layer.
    float boundsX;  ///< Layer bounds x, in local space.
    float boundsY;  ///< Layer bounds y, in local space.
    float boundsW;  ///< Layer bounds width, in local space.
    float boundsH;  ///< Layer bounds height, in local space.
};

/// Packed mirror of MaskCmd (push/pop of a mask layer).
struct PackedMaskCmd {
    uint8_t mode;   ///< Mask mode (add/subtract/intersect/lighten).
    float boundsX;  ///< Mask bounds x, in local space.
    float boundsY;  ///< Mask bounds y, in local space.
    float boundsW;  ///< Mask bounds width, in local space.
    float boundsH;  ///< Mask bounds height, in local space.
};

/// Packed mirror of GradientStop.
struct PackedGradientStop {
    float offset;  ///< Position along the gradient in [0, 1].
    float r;       ///< Stop color red.
    float g;       ///< Stop color green.
    float b;       ///< Stop color blue.
    float a;       ///< Stop color alpha.
};
static_assert(sizeof(PackedGradientStop) == 20);

/// Packed mirror of Vec2.
struct PackedVec2 {
    float x;  ///< X coordinate.
    float y;  ///< Y coordinate.
};
static_assert(sizeof(PackedVec2) == 8);

/// Packed mirror of ProjectionCmd (docs/design/19-3d-space.md §19.7).
struct PackedProjectionCmd {
    float viewProj[16];  ///< Row-major Mat4.
    float rootXf[6];     ///< Row-major Mat2x3 (a, b, c, d, e, f).
    float boxX;          ///< Root reference box x.
    float boxY;          ///< Root reference box y.
    float boxW;          ///< Root reference box width.
    float boxH;          ///< Root reference box height.
    uint8_t depthTest;   ///< 0.3-reserved depth-test flag.
    float hintX;         ///< boundsHint x (minor >= 3).
    float hintY;         ///< boundsHint y (minor >= 3).
    float hintW;         ///< boundsHint width (minor >= 3).
    float hintH;         ///< boundsHint height (minor >= 3).
};

/// Packed mirror of Transform3DCmd (docs/design/19-3d-space.md §19.7).
struct PackedTransform3DCmd {
    float world3[16];  ///< Row-major Mat4.
};

/// Packed mirror of ImageQuadCmd (docs/design/19-3d-space.md §19.7-19.8).
struct PackedImageQuadCmd {
    uint32_t asset;  ///< Image AssetId.
    float p0x;       ///< Corner 0 (top-left) x.
    float p0y;       ///< Corner 0 (top-left) y.
    float p1x;       ///< Corner 1 (top-right) x.
    float p1y;       ///< Corner 1 (top-right) y.
    float p2x;       ///< Corner 2 (bottom-right) x.
    float p2y;       ///< Corner 2 (bottom-right) y.
    float p3x;       ///< Corner 3 (bottom-left) x.
    float p3y;       ///< Corner 3 (bottom-left) y.
    float srcX;      ///< Source rectangle x, in image pixels.
    float srcY;      ///< Source rectangle y, in image pixels.
    float srcW;      ///< Source rectangle width, in image pixels.
    float srcH;      ///< Source rectangle height, in image pixels.
    float opacity;   ///< Multiplied opacity in [0, 1].
    uint8_t smooth;  ///< Nonzero to request bilinear filtering.
};

/// Packed mirror of EffectInstance (docs/design/18-effects.md §18.2).
struct PackedEffectInstance {
    uint8_t kind;                              ///< EffectInstance::kind (EffectKind).
    uint8_t enabled;                           ///< EffectInstance::enabled.
    uint32_t shader;                           ///< EffectInstance::shader (AssetId).
    float params[8];                           ///< EffectInstance::params.
    float color0r, color0g, color0b, color0a;  ///< EffectInstance::colors[0].
    float color1r, color1g, color1b, color1a;  ///< EffectInstance::colors[1].
    uint32_t stopOffset;                       ///< EffectInstance::stopOffset.
    uint32_t stopCount;                        ///< EffectInstance::stopCount.
};

/// Packed mirror of EffectCmd (docs/design/18-effects.md §18.5).
struct PackedEffectCmd {
    uint32_t effectOffset;  ///< EffectCmd::effectOffset.
    uint32_t effectCount;   ///< EffectCmd::effectCount.
    float xf[6];            ///< Row-major Mat2x3 (a, b, c, d, e, f).
    float boundsX;          ///< EffectCmd::boundsHint x.
    float boundsY;          ///< EffectCmd::boundsHint y.
    float boundsW;          ///< EffectCmd::boundsHint width.
    float boundsH;          ///< EffectCmd::boundsHint height.
    float contentX;         ///< EffectCmd::contentBounds x.
    float contentY;         ///< EffectCmd::contentBounds y.
    float contentW;         ///< EffectCmd::contentBounds width.
    float contentH;         ///< EffectCmd::contentBounds height.
};

/// Packed mirror of ShaderParamDesc (0.3-reserved).
struct PackedShaderParamDesc {
    uint32_t name;          ///< ShaderParamDesc::name (StringId).
    uint8_t type;           ///< ShaderParamDesc::type.
    float defaultValue[4];  ///< ShaderParamDesc::defaultValue.
    float minValue;         ///< ShaderParamDesc::minValue.
    float maxValue;         ///< ShaderParamDesc::maxValue.
};

/// Packed mirror of ShaderRef (0.3-reserved; docs/design/18-effects.md §18.5/§18.7).
struct PackedShaderRef {
    uint32_t asset;                 ///< ShaderRef::asset (AssetId).
    uint32_t sourceOffset;          ///< ShaderRef::sourceOffset.
    uint32_t sourceLength;          ///< ShaderRef::sourceLength.
    uint32_t paramOffset;           ///< ShaderRef::paramOffset.
    uint32_t paramCount;            ///< ShaderRef::paramCount.
    float expandPx;                 ///< ShaderRef::expandPx.
    PackedEffectInstance fallback;  ///< ShaderRef::fallback.
    uint32_t glslOffset;            ///< ShaderRef::glslOffset (T-203).
    uint32_t glslLength;            ///< ShaderRef::glslLength (T-203).
    uint32_t namesOffset;           ///< ShaderRef::namesOffset (T-203).
    uint32_t namesLength;           ///< ShaderRef::namesLength (T-203).
};

#pragma pack(pop)

/// Converts a PathRef to its packed on-disk form.
PackedPathRef toPacked(const PathRef& v);
/// Converts a packed PathRef back to a PathRef.
PathRef fromPacked(const PackedPathRef& v);
/// Converts a PaintRef to its packed on-disk form.
PackedPaintRef toPacked(const PaintRef& v);
/// Converts a packed PaintRef back to a PaintRef.
PaintRef fromPacked(const PackedPaintRef& v);
/// Converts a Command to its packed on-disk form.
PackedCommand toPacked(const Command& v);
/// Converts a packed Command back to a Command.
Command fromPacked(const PackedCommand& v);
/// Converts a FillCmd to its packed on-disk form.
PackedFillCmd toPacked(const FillCmd& v);
/// Converts a packed FillCmd back to a FillCmd.
FillCmd fromPacked(const PackedFillCmd& v);
/// Converts a StrokeCmd to its packed on-disk form.
PackedStrokeCmd toPacked(const StrokeCmd& v);
/// Converts a packed StrokeCmd back to a StrokeCmd.
StrokeCmd fromPacked(const PackedStrokeCmd& v);
/// Converts an ImageCmd to its packed on-disk form.
PackedImageCmd toPacked(const ImageCmd& v);
/// Converts a packed ImageCmd back to an ImageCmd.
ImageCmd fromPacked(const PackedImageCmd& v);
/// Converts a GlyphsCmd to its packed on-disk form.
PackedGlyphsCmd toPacked(const GlyphsCmd& v);
/// Converts a packed GlyphsCmd back to a GlyphsCmd.
GlyphsCmd fromPacked(const PackedGlyphsCmd& v);
/// Converts a ClipRectCmd to its packed on-disk form.
PackedClipRectCmd toPacked(const ClipRectCmd& v);
/// Converts a packed ClipRectCmd back to a ClipRectCmd.
ClipRectCmd fromPacked(const PackedClipRectCmd& v);
/// Converts a ClipPathCmd to its packed on-disk form.
PackedClipPathCmd toPacked(const ClipPathCmd& v);
/// Converts a packed ClipPathCmd back to a ClipPathCmd.
ClipPathCmd fromPacked(const PackedClipPathCmd& v);
/// Converts a LayerCmd to its packed on-disk form.
PackedLayerCmd toPacked(const LayerCmd& v);
/// Converts a packed LayerCmd back to a LayerCmd.
LayerCmd fromPacked(const PackedLayerCmd& v);
/// Converts a MaskCmd to its packed on-disk form.
PackedMaskCmd toPacked(const MaskCmd& v);
/// Converts a packed MaskCmd back to a MaskCmd.
MaskCmd fromPacked(const PackedMaskCmd& v);
/// Converts a GradientStop to its packed on-disk form.
PackedGradientStop toPacked(const GradientStop& v);
/// Converts a packed GradientStop back to a GradientStop.
GradientStop fromPacked(const PackedGradientStop& v);
/// Converts a Vec2 to its packed on-disk form.
PackedVec2 toPacked(const Vec2& v);
/// Converts a packed Vec2 back to a Vec2.
Vec2 fromPacked(const PackedVec2& v);
/// Converts a ProjectionCmd to its packed on-disk form.
PackedProjectionCmd toPacked(const ProjectionCmd& v);
/// Converts a packed ProjectionCmd back to a ProjectionCmd.
ProjectionCmd fromPacked(const PackedProjectionCmd& v);
/// Converts a Transform3DCmd to its packed on-disk form.
PackedTransform3DCmd toPacked(const Transform3DCmd& v);
/// Converts a packed Transform3DCmd back to a Transform3DCmd.
Transform3DCmd fromPacked(const PackedTransform3DCmd& v);
/// Converts an ImageQuadCmd to its packed on-disk form.
PackedImageQuadCmd toPacked(const ImageQuadCmd& v);
/// Converts a packed ImageQuadCmd back to an ImageQuadCmd.
ImageQuadCmd fromPacked(const PackedImageQuadCmd& v);
/// Converts an EffectInstance to its packed on-disk form.
PackedEffectInstance toPacked(const EffectInstance& v);
/// Converts a packed EffectInstance back to an EffectInstance.
EffectInstance fromPacked(const PackedEffectInstance& v);
/// Converts an EffectCmd to its packed on-disk form.
PackedEffectCmd toPacked(const EffectCmd& v);
/// Converts a packed EffectCmd back to an EffectCmd.
EffectCmd fromPacked(const PackedEffectCmd& v);
/// Converts a ShaderParamDesc to its packed on-disk form.
PackedShaderParamDesc toPacked(const ShaderParamDesc& v);
/// Converts a packed ShaderParamDesc back to a ShaderParamDesc.
ShaderParamDesc fromPacked(const PackedShaderParamDesc& v);
/// Converts a ShaderRef to its packed on-disk form.
PackedShaderRef toPacked(const ShaderRef& v);
/// Converts a packed ShaderRef back to a ShaderRef.
ShaderRef fromPacked(const PackedShaderRef& v);

/// Writes a `.xtcl` file: open() writes the header (frameCount/fps/viewport/dpr fixed up front,
/// per docs/tasks/T-37.md), then one writeFrame() call per frame index in [0, frameCount), then
/// close(). Uses `<cstdio>` (xt_core has no exceptions, docs/design/14-coding-standards.md §14.1).
/// Format note (T-263g): `.xtcl` minor 4 does not keep glyph effects -- PackedGlyphsCmd has no
/// `effect` / `effectRadius`, so a text-shadow plays back as plain text; they are added together
/// with particles / image meshes in the next minor. A frame with them is written, not refused.
class BakedWriter {
public:
    /// Closes the file (if still open) without checking the result; call close() explicitly to
    /// observe write failures.
    ~BakedWriter();

    /// Opens `path` (UTF-8, T-261e) for writing and writes the 28-byte header. Returns false (and
    /// leaves the writer unopened) if the file could not be opened; a path that is not valid
    /// UTF-8 cannot (T-261f).
    bool open(std::string_view path, uint32_t frameCount, float fps, Rect viewport, float dpr);
    /// Serializes `list` and appends it as frame `index`. `index` is written as given (frames
    /// may be written out of order; the reader does not require ascending order); writing the
    /// same index twice is accepted (last write wins on read, since BakedPlayer indexes by byte
    /// offset in file order -- see BakedPlayer::load). T-255b: an `index` at or past the
    /// `frameCount` given to open(), which BakedPlayer::load rejects, is refused the same way as
    /// the cases below (false, nothing written, close() reports it).
    /// T-263d: `list.sources` is not written (a BakedPlayer frame names no source: its AssetIds are
    /// those of the one IR its host renders it with), so a list with more than one source -- a
    /// Context::render of several Documents -- is an error: false, and close() reports it.
    /// T-263f: the format has no pool for the payloads of DrawParticles (`particles`,
    /// `particleInstances`) and DrawImageMesh (`imageMeshes`, `meshVertices`, `meshIndices`), so a
    /// list with either command is refused the same way (false, nothing of the frame written,
    /// close() reports it) instead of written as a frame the player can only play back empty.
    bool writeFrame(uint32_t index, const CommandList& list);
    /// Flushes and closes the file. Returns false if any earlier write, or the close itself,
    /// failed. Safe to call more than once (a no-op after the first successful or failed close).
    bool close();
    /// T-263g item 11 (as gpu::FrameBuilder::lastError()): why the last writeFrame() refused the
    /// frame -- "baked frames cannot store <CommandName> commands yet" (the name as
    /// dumpCommandList() prints it) or "baked frames cannot store a list with more than one
    /// source" or (T-255b) "baked frame index <i> is outside the frame count <n>" -- and an empty
    /// string when it did not refuse one (a plain write error says
    /// nothing here). Cleared by open().
    const char* lastError() const;

private:
    std::FILE* m_file = nullptr;
    bool m_failed = false;
    uint32_t m_frameCount = 0;  // T-255b: the header's frameCount, the bound writeFrame() checks.
    std::string m_lastError;    // T-263g item 11
};

/// Reads a `.xtcl` blob written by BakedWriter. `load()` validates the header and every frame's
/// declared boundaries up front and builds a per-frame byte-offset index; `frame()` decodes one
/// frame on demand into an internal CommandList, reusing its buffers' capacity across calls.
class BakedPlayer {
public:
    /// `takeOwnership == false` requires the host to keep `bytes` alive for the player's whole
    /// lifetime. Returns Status::kInvalidIr for a truncated file, a bad magic/version, or a frame
    /// whose declared payloadLength runs past the end of the file.
    Status load(IrBlob bytes, bool takeOwnership);

    /// Number of frames in the file. Only valid after a successful load().
    uint32_t frameCount() const { return m_frameCount; }
    /// Nominal playback rate from the header. Only valid after a successful load().
    float fps() const { return m_fps; }
    /// Viewport (width/height as w/h, x/y always 0) from the header. Only valid after a
    /// successful load().
    Rect viewport() const { return m_viewport; }
    /// Device pixel ratio from the header. Only valid after a successful load().
    float dpr() const { return m_dpr; }

    /// Decodes frame `index` into an internal CommandList and returns it. An out-of-range index
    /// (>= frameCount(), or load() was never called successfully) returns a cleared, empty
    /// CommandList (viewport/devicePixelRatio from the header, same as any other frame); so does
    /// a frame whose decoded list fails validateCommandList() (docs/tasks/T-263a.md E1).
    const CommandList& frame(uint32_t index);

private:
    std::vector<uint8_t> m_owned;
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    uint32_t m_frameCount = 0;
    /// Header::minor (0, 1, 2 or 3); gates whether frame() reads the 3D vectors (>= 1), the
    /// effect vectors (>= 2) and ProjectionCmd::boundsHint (>= 3).
    uint16_t m_minor = 0;
    float m_fps = 0;
    Rect m_viewport{};
    float m_dpr = 1;
    // Byte offset (into m_data) of each frame's payload, and its declared length; built by
    // load(). frameOffsets.size() == m_frameCount.
    std::vector<size_t> m_frameOffsets;
    std::vector<uint32_t> m_framePayloadLengths;
    CommandList m_scratch;
};

}  // namespace xt
