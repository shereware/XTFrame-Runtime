#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/input.h"
#include "xt/property.h"
#include "xt/text_layout.h"
#include "xt/text_types.h"
#include "xt/types.h"
#include "xt/value.h"

/// Node kinds, per-node data structures (NodeCommon, *NodeData), layout/text support types.
/// See Docs/design/01-architecture.md §1.3-1.4, Docs/design/03-scene-and-properties.md §3.2-3.7,
/// Docs/design/06-layout.md §6.2, Docs/design/07-text.md §7.2, Docs/tasks/T-04.md

namespace xt {

/// Scene graph node kind. Free is the tombstone value used by NodeArena's free list.
enum class NodeKind : uint8_t {
    /// Tombstone value for a freed slot; see NodeArena's free list.
    Free = 0,
    /// A pure container node with no visual content of its own.
    Group = 1,
    /// A vector shape node (PathNodeData).
    Path = 2,
    /// A raster image node (ImageNodeData).
    Image = 3,
    /// A text run node (TextNodeData).
    Text = 4,
    /// A DOM-like element node (ElementData); see Docs/design/03-scene-and-properties.md §3.6.
    Element = 5,
    /// A masking node; see MaskMode and Docs/design/03-scene-and-properties.md §3.7.
    Mask = 6,
    /// A 3D camera node (IrCameraData); see Docs/design/19-3d-space.md. Enum value only in this
    /// task (T-42c); runtime camera semantics are implemented starting T-43.
    Camera = 7,
    /// External content (Docs/design/03-scene-and-properties.md §3.12.1, T-191): the child a
    /// `<video>` / `<sequence>` / `<external>` element gets at load time, carrying its
    /// ExternalNodeData (NodeArena::external(id)). Like Camera it is never drawn or hit-tested
    /// itself; the owning element draws the content as its image (object-fit, radius, effects).
    External = 8,
    /// A particle emitter (Docs/design/03-scene-and-properties.md §3.12.5, T-202): the child a
    /// `<particles>` element gets at load time, carrying its ParticleNodeData
    /// (NodeArena::particles(id)). Painted in the element's child order as one DrawParticles
    /// command in the element's coordinates; never hit-tested itself (the element's box is).
    Particles = 9,
    /// A shape modifier (Docs/design/31-vector-authoring.md §31.6.1, T-225): carries its
    /// ModifierNodeData (NodeArena::modifier(id)). Never drawn itself; the Painter applies it to
    /// the Path/Shape siblings that precede it in its parent group (collect-apply-draw).
    Modifier = 10,
    /// A parametric shape (§31.6.1, T-225): rect / ellipse / star whose path the Painter builds
    /// from ShapeNodeData::params every frame (NodeArena::shape(id)); painted like a Path.
    Shape = 11,
    /// A skeleton bone (Docs/design/32-rigging-and-deformation.md §32.2.1, T-243): a transform node
    /// (translate / rotate / scale) carrying BoneNodeData (NodeArena::bone(id)). Never drawn or
    /// hit-tested itself.
    Bone = 12,
    /// A constraint (§32.2.3, T-243) under a bone or any transform node, carrying
    /// ConstraintNodeData (NodeArena::constraint(id)). Never drawn.
    Constraint = 13,
    /// An image mesh (§32.2.4, T-243) carrying MeshNodeData (NodeArena::mesh(id)). Drawn by T-245;
    /// until then the Painter skips it.
    Mesh = 14,
};

/// See Docs/design/03-scene-and-properties.md §3.3.
enum class BlendMode : uint8_t {
    /// CSS `normal` (no blending).
    Normal = 0,
    /// CSS `multiply`.
    Multiply,
    /// CSS `screen`.
    Screen,
    /// CSS `overlay`.
    Overlay,
    /// CSS `darken`.
    Darken,
    /// CSS `lighten`.
    Lighten,
    /// CSS `color-dodge`.
    ColorDodge,
    /// CSS `color-burn`.
    ColorBurn,
    /// CSS `hard-light`.
    HardLight,
    /// CSS `soft-light`.
    SoftLight,
    /// CSS `difference`.
    Difference,
    /// CSS `exclusion`.
    Exclusion,
    /// CSS `hue`.
    Hue,
    /// CSS `saturation`.
    Saturation,
    /// CSS `color`.
    Color,
    /// CSS `luminosity`.
    Luminosity,
    /// Lottie's additive blend mode (no direct CSS equivalent).
    Add,
};

/// See Docs/design/03-scene-and-properties.md §3.7.
enum class MaskMode : uint8_t {
    /// The mask content's alpha determines visibility (Lottie track matte).
    Alpha = 0,
    /// Like Alpha, but visibility is inverted.
    AlphaInverted = 1,
    /// The mask content's luminance determines visibility (Lottie track matte).
    Luma = 2,
    /// Like Luma, but visibility is inverted.
    LumaInverted = 3,
    /// Union with the following masks (Lottie shape-mask mode "a").
    Add = 4,
    /// Subtract the following masks (Lottie shape-mask mode).
    Subtract = 5,
    /// Intersect with the following masks (Lottie shape-mask mode).
    Intersect = 6,
};

/// ElementData::stateBits bits. See Docs/design/03-scene-and-properties.md §3.6.
enum ElementState : uint16_t {
    /// CSS `:hover`; set by pointer move (T-32).
    kStateHover = 1 << 0,
    /// CSS `:active`; set while the pointer is pressed down on the element (T-32).
    kStateActive = 1 << 1,
    /// CSS `:focus`; set by Document::setFocus (08.5).
    kStateFocus = 1 << 2,
    /// CSS `:disabled`; settable through ElementRef::setState()/the `disabled` attribute.
    kStateDisabled = 1 << 3,
    /// CSS `:checked`; settable through ElementRef::setState()/the `checked` attribute.
    kStateChecked = 1 << 4,
};

/// T-390 / T-392 (27 §27.3.2, §27.3.5): the state names that mean a built-in ElementState in every
/// position (`:state(X)`, `bind:state.X`, `state(X, ...)`, `requires="states: X"`,
/// Document::setCustomState / hasCustomState): Disabled, Focused, Hover, Active and Checked. 0 for
/// any other name (a custom state).
constexpr uint16_t builtinStateAlias(std::string_view name) {
    return name == "Disabled"  ? static_cast<uint16_t>(kStateDisabled)
           : name == "Focused" ? static_cast<uint16_t>(kStateFocus)
           : name == "Hover"   ? static_cast<uint16_t>(kStateHover)
           : name == "Active"  ? static_cast<uint16_t>(kStateActive)
           : name == "Checked" ? static_cast<uint16_t>(kStateChecked)
                               : uint16_t{0};
}

/// NodeArena::dirty bits. See Docs/design/01-architecture.md §1.4.
enum DirtyBits : uint8_t {
    /// Needs its computed style re-resolved (cascade/selector match).
    kDirtyStyle = 1 << 0,
    /// Needs itself and its descendants re-laid out.
    kDirtyLayout = 1 << 1,
    /// Needs its world transform recomputed.
    kDirtyTransform = 1 << 2,
    /// A visual property changed without affecting layout.
    kDirtyPaint = 1 << 3,
    /// Its children were added or removed.
    kDirtyTree = 1 << 4,
};

/// See Docs/design/03-scene-and-properties.md §3.4.
enum class PathVerb : uint8_t {
    Move = 0,   ///< Starts a new subpath at an absolute point (1 point operand).
    Line = 1,   ///< A straight line segment to an absolute point (1 point operand).
    Cubic = 2,  ///< A cubic Bezier segment (3 point operands: ctrl1, ctrl2, end).
    Close = 3,  ///< Closes the current subpath back to its start (no operands).
};
/// Path fill rule (SVG/Canvas semantics).
enum class FillRule : uint8_t {
    NonZero = 0,  ///< Non-zero winding rule.
    EvenOdd = 1   ///< Even-odd winding rule.
};
/// Stroke end-cap style.
enum class LineCap : uint8_t {
    Butt = 0,   ///< Flat cap flush with the endpoint.
    Round = 1,  ///< Rounded cap centered on the endpoint.
    Square = 2  ///< Square cap extending half the stroke width past the endpoint.
};
/// Stroke corner-join style.
enum class LineJoin : uint8_t {
    Miter = 0,  ///< Sharp corner, clamped by Stroke::miterLimit.
    Round = 1,  ///< Rounded corner.
    Bevel = 2   ///< Flattened (cut-off) corner.
};
/// What a Paint (fill or stroke) renders: nothing, a flat color, or a gradient.
enum class PaintKind : uint8_t {
    None = 0,    ///< No paint (fill/stroke disabled).
    Solid = 1,   ///< Flat solid color.
    Linear = 2,  ///< Linear gradient.
    Radial = 3,  ///< Radial gradient.
    Conic = 4    ///< T-201: conic gradient (element backgrounds; PaintRef p0 = centre, p1 = 0deg ray).
};

// ValueType, LengthUnit, Length and GradientStop live in xt/value.h (included above), since
// xt/value.h's Value type needs them and must not depend on xt/node.h.

/// See Docs/design/03-scene-and-properties.md §3.4.
struct Paint {
    /// Whether this Paint is unused, a solid color or a gradient.
    PaintKind kind = PaintKind::None;
    /// Used when kind == Solid.
    Color color{};
    /// Linear: start point. Radial: center point.
    Vec2 p0{};
    /// Linear: end point. Radial: edge point (radius = |p1-p0|).
    Vec2 p1{};
    /// Radial only: Lottie's gradient highlight length ("h"), 0..1; read but ignored by the
    /// backend in this version.
    float highlightLength = 0;
    /// Radial only: Lottie's gradient highlight angle.
    float highlightAngle = 0;
    /// Index into PathNodeData::stops.
    uint16_t stopOffset = 0;
    /// Number of gradient stops in PathNodeData::stops.
    uint16_t stopCount = 0;
};

/// See Docs/design/03-scene-and-properties.md §3.4.
struct Stroke {
    /// Stroke width in node-local units.
    float width = 0;
    /// End-cap style.
    LineCap cap = LineCap::Butt;
    /// Corner-join style.
    LineJoin join = LineJoin::Miter;
    /// Miter-join clamp ratio.
    float miterLimit = 4;
    /// Index into PathNodeData::dashes.
    uint16_t dashOffset = 0;
    /// Number of dash lengths in PathNodeData::dashes; 0 = solid line.
    uint16_t dashCount = 0;
    /// Offset into the dash pattern.
    float dashPhase = 0;
    /// T-233 (31 §31.4.7): taper lengths at the start / end of each contour, as a 0..1 fraction of
    /// its arc length; the width ramps linearly from 0 to full over that length. 0 = no taper.
    /// Ignored on closed contours.
    float taperStart = 0;
    float taperEnd = 0;  ///< See taperStart.
    /// T-233: index into PathNodeData::widthProfile (or ShapeNodeData::widthProfile).
    uint16_t profileOffset = 0;
    /// T-233: number of width-profile points; 0 = constant width. Any tapered or profiled stroke is
    /// drawn by the Painter as a fill (strokeToFill with a width function).
    uint16_t profileCount = 0;
};

/// T-233 (31 §31.4.7): one point of a variable-width stroke profile: at arc-length fraction `t`
/// (0..1) the stroke width is multiplied by `w`; linear in between, held beyond the first / last point.
struct WidthPoint {
    float t = 0;  ///< Arc-length fraction, 0..1.
    float w = 1;  ///< Width multiplier.
};

/// Fields shared by every node kind. See Docs/design/03-scene-and-properties.md §3.3.
struct NodeCommon {
    // Animatable.
    /// 0..1, multiplied down into the subtree.
    float opacity = 1;
    /// Transform origin, in node-local coordinates.
    Vec2 anchor{};
    /// Translation, in node-local coordinates.
    Vec2 translate{};
    /// Scale factor; default (1, 1).
    Vec2 scale{1, 1};
    /// Degrees, clockwise.
    float rotation = 0;
    /// Degrees: x = shear angle (Lottie `sk`), y = shear-axis angle (Lottie `sa`); see
    /// Docs/design/11-lottie-import.md §11.3.1.
    Vec2 skew{};
    /// 0 = not drawn and not hit-tested (animatable through PropId 13).
    uint8_t visible = 1;
    // Not animatable.
    uint8_t blend = 0;     ///< BlendMode.
    uint8_t maskMode = 0;  ///< MaskMode; only used by Mask nodes.
    /// T-228: PropId 14 AutoOrient (0 none, 1 path).
    uint8_t autoOrient = 0;
    // Cached.
    /// Computed from the fields above (fixed order: T*R*Sk*S*T(-anchor)).
    Mat2x3 local{};
    /// parentWorld * local.
    Mat2x3 world{};
    /// Cumulative opacity through the ancestor chain.
    float worldOpacity = 1;
    // --- Docs/design/19-3d-space.md §19.3: 3D. ---
    float translateZ = 0;  ///< PropId 16.
    float anchorZ = 0;     ///< PropId 17.
    float rotationX = 0;   ///< PropId 18, degrees.
    float rotationY = 0;   ///< PropId 19, degrees.
    /// PropId 22, degrees (AE Orientation).
    Vec3 orientation{};
    /// 1 = this node participates in 3D transform/compositing (19.3).
    uint8_t is3D = 0;
    /// 1 = the node is hidden when its back face is toward the camera.
    uint8_t backfaceHidden = 0;
    uint8_t _pad3d[2] = {};  ///< Padding, unused.
    /// Cache (Docs/design/19-3d-space.md §19.5): 1 = this node is a 3D space root (owns a camera,
    /// or is an Element with perspectivePx > 0, or is a scene root Group whose subtree has an
    /// is3D node). Recomputed by updateWorldTransforms() only when kDirtyTree is set (or force).
    uint8_t isSpaceRoot = 0;
    /// Valid only when is3D == 1 (19.5): the node's transform relative to its 3D space root.
    Mat4 world3 = mat4Identity();
    // --- Docs/design/18-effects.md §18.2: effects. ---
    /// Offset/count into NodeArena::effects(). Read-only outside node_arena.cpp (T-270c): written
    /// only through NodeArena::assignEffects() / setEffectRange() / destroy() / compactEffects(),
    /// which keep the arena's live effect counts; never assign them directly, nor copy a whole
    /// NodeCommon onto another node (the copy would share the segment).
    uint32_t effectOffset = 0;  ///< Offset into NodeArena::effects().
    uint32_t effectCount = 0;   ///< Count of effects.
    // --- Docs/design/03-scene-and-properties.md §3.12.2 (T-174): Element-only tail fields. ---
    /// Trigger table: index into the IR's TRIG `tables()` + 1; 0 = none (27.2, 3.12.3).
    uint32_t triggerTable = 0;
    /// Custom states (`:state(<Name>)`, 27.1): bit i = the IR's STAT `names()[i]`.
    uint32_t customStates = 0;
    /// NodeFlags2 bits.
    uint32_t flags2 = 0;
    /// T-194 (03 §3.12.2, `nav:up/down/left/right="#id"`): explicit focus navigation targets, as
    /// the target id's StringId + 1 (0 = automatic). Indexed by NavDirection in navUp..navRight.
    uint32_t navUp = 0;
    uint32_t navDown = 0;   ///< See navUp.
    uint32_t navLeft = 0;   ///< See navUp.
    uint32_t navRight = 0;  ///< See navUp.
    /// T-228 (31 §31.7.2): the path tangent angle, degrees, added to `rotation` when composing the
    /// transform while `autoOrient` is 1. Written by the Animator; kept when the motion stops.
    float orientPathDeg = 0;
};

/// NodeCommon::flags2 bits (03 §3.12.2).
enum NodeFlags2 : uint32_t {
    /// `pointer-watch: true` (27.2.5, 08.9): hover/move only; presses pass through. Mirrored from
    /// ComputedStyle::pointerWatch by StyleEngine::resolve.
    kPointerWatch = 1u << 0,
    /// T-177 (27.3.4, 03 §3.12.4): a `<repeat>` template element. Template subtrees are kept out
    /// of the element tree (never styled, laid out, painted or hit) and only copied per list entry.
    kTemplate = 1u << 1,
    /// T-194 (08.9): `layer: overlay` -- the subtree paints after the root, hit tests first and is
    /// not clipped by its ancestors' `overflow`. Mirrored from ComputedStyle::layerOverlay.
    kLayerOverlay = 1u << 2,
    /// T-194 (27.7): the `draggable` attribute.
    kDraggable = 1u << 3,
    /// T-194 (27.7): the `drop-target` attribute.
    kDropTarget = 1u << 4,
    /// T-194 (27.7, modal): focus navigation and Tab never leave this element's subtree once the
    /// focus is inside it (Document::setFocusTrap).
    kFocusTrap = 1u << 5,
    /// T-201 (27.7): `draggable="axis-x"`: pointer drags along the element fire `drag.value`
    /// (0..1 across its box) and left / right navigation while it has the focus fires `nav.value`
    /// (-1 / +1) instead of moving the focus. Not a drag-and-drop source.
    kDragAxisX = 1u << 6,
    /// T-270f (internal): the node belongs to a recycled `<repeat>` instance waiting in its pool
    /// (alive, detached, not yet reused). Set on the instance's whole subtree when it is recycled,
    /// cleared when it is reused; every mutating ElementRef call treats such a node as removed.
    kPooled = 1u << 7,
    /// T-345 (25 §25.4.2 item 4): set by Animator::evaluate on a node whose transform, position
    /// or inset a running transition, an unfinished keyframe / `@timeline` track or a runtime
    /// override wrote this update; cleared (and the node marked kDirtyTransform) on the first
    /// update that writes none. The Painter skips pixel alignment under such a node.
    kInMotion = 1u << 8,
};

/// High bit tag for ComputedStyle::effectOffset (Docs/design/18-effects.md §18.4, Docs/tasks/
/// T-51b.md): when set, `effectOffset & ~kRuntimeEffectOffsetBit` indexes Document's runtime
/// effect declaration pool (Document::runtimeEffects()/runtimeEffectStops(), appended to by
/// ElementRef::setStyle's `effect:` declarations and ElementRef::setEffects()) instead of the
/// compiled IR's STYL `effects[]`/`effectStops[]` pools. Mirrors xt::kRuntimeStringBase's role for
/// StringId (xt/types.h).
inline constexpr uint32_t kRuntimeEffectOffsetBit = 0x80000000u;

/// See Docs/design/18-effects.md §18.2.
enum class EffectKind : uint8_t {
    None = 0,          ///< No effect (unused slot).
    DropShadow = 1,    ///< Drop shadow.
    Glow = 2,          ///< Outer/inner glow.
    Blur = 3,          ///< Gaussian blur.
    ColorAdjust = 4,   ///< Brightness/contrast/saturation/hue/opacity.
    GradientMask = 5,  ///< Gradient alpha mask.
    Shine = 6,         ///< Sweeping highlight.
    Outline = 7,       ///< Outline/stroke-from-alpha.
    BackdropBlur = 8,  ///< Blur of what lies below the node, masked by the node's own alpha.
    Custom = 128,      ///< HLSL fragment effect asset (0.3; rejected at load time in 0.2).
};

/// NodeKind::Camera-specific data (NodeArena::camera(id)). See Docs/design/19-3d-space.md §19.4.
/// The node's own translate/translateZ is its eye position; anchor/scale/skew are ignored.
struct CameraData {
    /// 0 = Perspective, 1 = Orthographic.
    uint8_t kind = 0;
    /// 1 = the camera looks at `target`; 0 = its orientation comes from the node's
    /// rotationX/rotationY/rotation (19.4).
    uint8_t useTarget = 1;
    uint8_t _pad[2] = {};  ///< Padding, unused.
    /// PropId 24: focal length in px; fovY = 2*atan((viewportH/2)/zoom).
    float zoom = 0;
    /// PropId 25: look-at target point, in the camera's 3D space root's coordinates.
    Vec3 target{};
    /// PropId 26: near clip plane.
    float nearZ = 1;
    /// PropId 27: far clip plane.
    float farZ = 100000;
};

/// Source of a NodeKind::External node (Docs/design/25-editor-blueprint.md §25.3.5b, T-191).
enum class ExternalSourceKind : uint8_t {
    /// `<video src>`: decoded and played by the host (IExternalTexture), never by the core.
    Video = 0,
    /// `<sequence src fps>`: an image sequence the core plays itself through AssetResolver.
    Sequence = 1,
    /// `<external source="scene-view:<camera>">`: a host scene view.
    SceneView = 2,
    /// `<external source="engine://<name>">`: an engine texture (T-150).
    Engine = 3,
    /// `<external source="host:<name>">`: any other host-provided texture.
    Host = 4,
};

/// Playback state of a video or sequence (IExternalTexture::setPlayback, T-191).
enum class MediaState : uint8_t {
    Stopped = 0,  ///< Not started (or stopped); time 0 unless seeked.
    Playing = 1,  ///< Time advances.
    Paused = 2,   ///< Time holds.
    Ended = 3,    ///< Played to its end without `loop`; `media(<id>).end` has fired.
};

/// NodeKind::External-specific data (NodeArena::external(id)); Docs/design/03 §3.12.1,
/// 13 §13.8.3, T-191. Filled at load time from the owning element's attributes; the playback
/// fields change at run time (Document's media API, IExternalTexture, `@timeline` sync).
struct ExternalNodeData {
    /// Which of the four sources this is.
    ExternalSourceKind source = ExternalSourceKind::Video;
    /// Playback state (video: as the host last reported, or the core last requested).
    MediaState state = MediaState::Stopped;
    /// `loop` attribute (or a later loop request): wrap at the end instead of ending.
    uint8_t loop = 0;
    /// `autoplay` attribute: play from the first update.
    uint8_t autoplay = 0;
    /// `sync` attribute: an enclosing `@timeline` group drives the clock (04 §4.10.2).
    uint8_t sync = 0;
    /// 1 while the host has a texture or CPU pixels bound to it (bindTexture / bindImage).
    uint8_t bound = 0;
    /// Painter: draw the placeholder (checkerboard + label) instead of the content. Set by the
    /// Document after layout while placeholders are enabled and nothing is bound.
    uint8_t showPlaceholder = 0;
    /// 1 until the first update has handled `autoplay`.
    uint8_t pendingAutoplay = 0;
    /// Video under a playing `@timeline` group: the host was asked to play (sync bookkeeping).
    uint8_t syncRunning = 0;
    uint8_t _pad = 0;  ///< Padding, unused.
    /// The owning element.
    NodeId element = kInvalidNode;
    /// Video / SceneView / Engine / Host: the `engine://` ASST entry the texture is bound to.
    /// A sequence from an atlas: the atlas image. Otherwise kInvalidAsset.
    AssetId asset = kInvalidAsset;
    /// A sequence from a folder or a `%0Nd` pattern: one image asset per frame, in order.
    std::vector<AssetId> frames;
    /// A sequence from an atlas: the frame size in atlas pixels (`frame-size="WxH"`), frames
    /// laid out row-major; (0, 0) otherwise.
    Vec2 frameSize{};
    /// A sequence from an atlas: the frame count (`frames`, default every whole cell).
    uint32_t atlasFrames = 0;
    /// Sequence: frames per second (`fps`, default 24).
    float fps = 24;
    /// `rate` attribute or the last rate request (> 0).
    float rate = 1;
    /// Current media time, seconds.
    float time = 0;
    /// Media duration in seconds; < 0 while unknown (a video the host has not reported).
    float duration = -1;
    /// `clip-start` / `clip-rate` of the owning element, copied at each sync step.
    float clipStart = 0;
    float clipRate = 1;
    /// Sequence: the frame shown (index into `frames`, or the atlas cell).
    uint32_t frame = 0;
    /// Sequence: the frame the preload window was last filled from (0xFFFFFFFF = never).
    uint32_t preloadedFrom = 0xFFFFFFFFu;
    /// The placeholder label, shaped after layout while showPlaceholder is set.
    std::vector<GlyphRun> label;
    /// Where `label`'s runs sit: the text origin in the element's local coordinates.
    Vec2 labelOrigin{};
    /// The label's color.
    Color labelColor{};
    /// The text and size `label` was shaped from (rebuild key).
    std::string labelText;
    float labelSize = 0;
};

/// `<particles shape>` (Docs/design/03-scene-and-properties.md §3.12.5, T-202): where new
/// particles start within the emitter box.
enum class ParticleShape : uint8_t {
    Point = 0,   ///< The box center.
    Rect = 1,    ///< Uniformly inside the box.
    Circle = 2,  ///< Uniformly inside the box's inscribed ellipse.
    Edge = 3,    ///< Uniformly along the box's perimeter.
};

/// `<particles blend>`: how a particle is composited (premultiplied source).
enum class ParticleBlend : uint8_t {
    Normal = 0,    ///< Source-over.
    Additive = 1,  ///< dst + src (GPU: ONE, ONE).
};

/// One live particle, in the owning element's local (border-box) coordinates.
struct Particle {
    Vec2 pos{};           ///< Center, px.
    Vec2 vel{};           ///< px/s.
    float age = 0;        ///< Seconds since birth.
    float life = 0;       ///< Lifetime, seconds; the particle dies when age >= life.
    float size0 = 0;      ///< Size at birth (px), taken from `size[0]` when emitted.
    float size1 = 0;      ///< Size at death (px), from `size[1]`.
    float rot = 0;        ///< Rotation, degrees.
    float rotVel = 0;     ///< Degrees per second (always 0: no attribute sets it yet).
    float colorSeed = 0;  ///< Uniform [0, 1) per particle, reserved for color variation.
};

/// NodeKind::Particles-specific data (NodeArena::particles(id)); Docs/design/03 §3.12.5, T-202.
/// The parameter fields mirror the owning element's particle PropIds (110-128) at every step;
/// `emitterShape`, `texture`, `blend` and `simulateInEditor` come from the element's attributes.
struct ParticleNodeData {
    ParticleShape emitterShape = ParticleShape::Point;
    ParticleBlend blend = ParticleBlend::Normal;
    /// `simulate-in-editor` (default true): simulate while the editor canvas is still.
    uint8_t simulateInEditor = 1;
    /// `autoplay` attribute: start emitting on the first update.
    uint8_t autoplay = 0;
    float rate = 10;  ///< Particles per second.
    float burst = 0;  ///< Particles emitted at once when emission starts.
    float lifetime[2] = {1, 1};
    float speed[2] = {100, 100};
    float direction = -90;  ///< Degrees; 0 = +x, -90 = up (y grows downward).
    float spread = 0;       ///< Full cone angle, degrees.
    float gravity = 0;      ///< px/s^2 along +y.
    float drag = 0;         ///< Velocity damping per second.
    float size[2] = {8, 8};
    /// Color at t = 0, 1/3, 2/3, 1 of the particle's life (straight alpha).
    Color color[4] = {Color{1, 1, 1, 1}, Color{1, 1, 1, 2.0f / 3.0f}, Color{1, 1, 1, 1.0f / 3.0f},
                      Color{1, 1, 1, 0}};
    float rotation[2] = {0, 0};
    /// Image drawn per particle (`texture` attribute); kInvalidAsset = a filled square.
    AssetId texture = kInvalidAsset;
    /// The part (0..1) of `texture` a particle samples: its bundle atlas cell, else the whole.
    Rect textureSubUv{0, 0, 1, 1};
    /// Live particle cap (`max`, default 256); nothing is emitted beyond it.
    uint32_t maxParticles = 256;

    // Run-time state.
    NodeId element = kInvalidNode;  ///< The owning `<particles>` element.
    uint8_t emitting = 0;           ///< Rate emission on (play / autoplay; stop turns it off).
    uint8_t burstPending = 0;       ///< Emit `burst` particles on the next step.
    uint8_t pendingAutoplay = 0;    ///< 1 until the first update has handled `autoplay`.
    uint8_t _pad = 0;               ///< Padding, unused.
    uint32_t seed = 0;              ///< Random stream seed (particleSeed of the element id).
    uint32_t emitSeq = 0;           ///< Particles emitted so far (random stream index).
    float accumulator = 0;          ///< Unsimulated seconds (< kParticleStep after a step).
    float emitCarry = 0;            ///< Fractional particles owed by `rate`.
    std::vector<Particle> particles;
};

/// Compiled visual effect instance; on-disk layout matches IrEffect (Docs/design/18-effects.md
/// §18.2), 80 bytes (4 + 4 + 32 + 32 + 8).
struct EffectInstance {
    /// Which built-in effect (or Custom) this slot holds.
    EffectKind kind = EffectKind::None;
    /// paramIndex 0; 0 = this effect is skipped.
    uint8_t enabled = 1;
    uint8_t _pad[2] = {};  ///< Padding, unused.
    /// Custom: the Shader asset; otherwise kInvalidAsset.
    AssetId shader = kInvalidAsset;
    /// paramIndex 1..8; meaning depends on `kind` (18.3's parameter table).
    float params[8] = {};
    /// paramIndex 16/17.
    Color colors[2]{};
    /// Offset/count into the owning pool's gradient stops (NodeArena::effectStops(),
    /// CommandList::stops, or the IR chunk's stop pool); used by GradientMask.
    uint32_t stopOffset = 0;  ///< Offset into the owning stop pool.
    uint32_t stopCount = 0;   ///< Count of stops.
};
static_assert(sizeof(EffectInstance) == 80, "EffectInstance must match IrEffect's 80-byte layout");

/// NodeKind::Bone-specific data (NodeArena::bone(id)); 32 §32.2.1, T-243. A child bone's
/// translate x is its parent's length (it sits at the parent's tip).
struct BoneNodeData {
    float length = 0;  ///< PropId BoneLength.
};

/// Constraint kinds (32 §32.2.3, T-243).
enum class ConstraintKind : uint8_t { Ik = 0, Transform = 1, Rotation = 2, Spring = 3 };

/// NodeKind::Constraint-specific data (NodeArena::constraint(id)); 32 §32.2.3, T-243. `params`
/// indices:
///   Ik        [strength, invert, chain].
///   Transform / Rotation: none (only `mix` and `space`).
///   Spring    [inertia, strength, damping, gravity].
struct ConstraintNodeData {
    ConstraintKind kind = ConstraintKind::Ik;  ///< Which constraint.
    uint8_t space = 0;                         ///< 0 = local, 1 = world (the skeleton's group space).
    uint8_t enabled = 1;                       ///< 0 = not applied.
    uint8_t _pad = 0;                          ///< Padding, unused.
    NodeId target = kInvalidNode;              ///< The target node (not animatable); none for Spring.
    float params[8] = {};                      ///< Kind-specific parameters (see above).
    float mix = 1;                             ///< PropId ConstraintMix.
};

/// One skin influence (32 §32.2.2, T-243): a bone and its inverse bind matrix (the inverse of the
/// bone's rest world x the skinned shape's own transform, in the skeleton's group space).
struct Tendon {
    NodeId bone = kInvalidNode;  ///< The bone.
    Mat2x3 inverseBind{};        ///< inverse(boneRestWorld) x shapeOwnTransform (T-243b).
};

/// One vertex weight set (32 §32.2.2): four 8-bit indices into SkinData::tendons (byte i of
/// `idx4`) and four 8-bit weights (byte i of `w4`, /255).
struct PointWeight {
    uint32_t idx4 = 0;  ///< Four 8-bit tendon indices.
    uint32_t w4 = 0;    ///< Four 8-bit weights (/255).
};

/// A Path's or Mesh's skin (32 §32.2.2, T-243).
struct SkinData {
    NodeId skeleton = kInvalidNode;    ///< The `<xt-skeleton>` group.
    std::vector<Tendon> tendons;       ///< One per bone the weights reference.
    std::vector<PointWeight> weights;  ///< Path: 3 per vertex (p, in, out); Mesh: 1 per vertex.
};

/// NodeKind::Mesh-specific data (NodeArena::mesh(id)); 32 §32.2.4, T-243. `positions` are in the
/// skeleton's group space; `uvs` are 0..1.
struct MeshNodeData {
    AssetId asset = kInvalidAsset;  ///< The image asset.
    std::vector<Vec2> positions;    ///< One per mesh vertex (pin).
    std::vector<Vec2> uvs;          ///< One per mesh vertex.
    std::vector<uint16_t> indices;  ///< Triangle list (3 per triangle).
    float opacity = 1;              ///< The mesh's own opacity.
    uint8_t smooth = 1;             ///< 1 = bilinear sampling.
    SkinData skin;                  ///< `skin.skeleton == kInvalidNode` = not skinned.
};

/// See Docs/design/03-scene-and-properties.md §3.4.
struct PathNodeData {
    /// One entry per path command; see PathVerb.
    std::vector<PathVerb> verbs;
    /// Command operands, node-local coordinates: 2 floats per Move/Line, 6 per Cubic, 0 per Close.
    std::vector<float> points;
    /// The fill rule used to determine the interior when filling.
    FillRule fillRule = FillRule::NonZero;
    /// The fill paint; PaintKind::None means no fill.
    Paint fill;
    /// The stroke paint; PaintKind::None means no stroke.
    Paint stroke;
    /// Stroke geometry parameters (width, cap, join, dash pattern).
    Stroke strokeStyle;
    /// Trim Path start/end, as a 0..1 fraction of arc length (see Docs/design/03-scene-and-properties.md
    /// §3.8).
    float trimStart = 0;
    /// Trim Path start/end, as a 0..1 fraction of arc length (§3.8).
    float trimEnd = 1;
    /// Trim Path offset, in whole turns (any real number).
    float trimOffset = 0;
    float fillOpacity = 1;    ///< PropId FillOpacity (03.9); multiplied into fill.color.a by the Painter.
    float strokeOpacity = 1;  ///< PropId StrokeOpacity (03.9); multiplied into stroke.color.a by the Painter.
    /// Gradient stop pool shared by fill and stroke.
    std::vector<GradientStop> stops;
    /// Dash pattern lengths, referenced by strokeStyle's dashOffset/dashCount.
    std::vector<float> dashes;
    /// T-233: width-profile points (sorted by t), referenced by strokeStyle's profileOffset/profileCount.
    std::vector<WidthPoint> widthProfile;
    /// T-243 (32 §32.2.2): vertex skinning; three PointWeights per vertex (p, in, out).
    /// `skin.weights.empty()` = not skinned.
    SkinData skin;
};

/// Shape modifier kinds (Docs/design/31-vector-authoring.md §31.6.1, T-225). Offset and Merge are
/// baked by the editor; the Painter ignores them (the node only keeps their parameters).
enum class ModifierKind : uint8_t {
    Trim = 0,
    Repeat = 1,
    Round = 2,
    Pucker = 3,
    ZigZag = 4,
    Twist = 5,
    Wiggle = 6,
    Offset = 7,
    Merge = 8,
};

/// NodeKind::Modifier-specific data (NodeArena::modifier(id)); §31.6.1, T-225. `params` indices:
///   Trim   [start, end, offset (degrees)]; mode 0 = parallel, 1 = serial.
///   Repeat [copies, offset, anchorX, anchorY, translateX, translateY, rotation (deg), scale];
///          mode bit 0 = order (0 below, 1 above); opacityA/opacityB = start/end opacity.
///   Round  [radius].
///   Pucker [amount (-100..100)].
///   ZigZag [size, ridges]; mode 0 = corner, 1 = smooth.
///   Twist  [angle (deg), centerX, centerY].
///   Wiggle [size, detail, seed, speed, correlation, temporalPhase (deg), spatialPhase (deg)];
///          mode 0 = corner, 1 = smooth.
struct ModifierNodeData {
    ModifierKind kind = ModifierKind::Trim;  ///< Which modifier.
    uint8_t mode = 0;                        ///< Kind-specific mode (see above).
    uint8_t enabled = 1;                     ///< 0 = the Painter skips this modifier.
    float params[8] = {};                    ///< Kind-specific parameters (see above).
    float opacityA = 1;                      ///< Repeat: opacity of copy 0.
    float opacityB = 1;                      ///< Repeat: opacity of the last copy.
};

/// Parametric shape kinds (§31.6.1, T-225).
enum class ShapeKind : uint8_t {
    Rect = 0,
    Ellipse = 1,
    Star = 2,
};

/// NodeKind::Shape-specific data (NodeArena::shape(id)); §31.6.1, T-225. `params` indices:
///   Rect    [x, y, w, h, rx, ry].
///   Ellipse [cx, cy, rx, ry].
///   Star    [cx, cy, points, r, r2, rotation (deg), round, round2]; r2 <= 0 = regular polygon.
/// The paint fields mean exactly what they mean on PathNodeData.
struct ShapeNodeData {
    ShapeKind kind = ShapeKind::Rect;       ///< Which shape.
    float params[12] = {};                  ///< Kind-specific parameters (see above).
    FillRule fillRule = FillRule::NonZero;  ///< Fill rule.
    Paint fill;                             ///< Fill paint; PaintKind::None means no fill.
    Paint stroke;                           ///< Stroke paint; PaintKind::None means no stroke.
    Stroke strokeStyle;                     ///< Stroke geometry parameters.
    float trimStart = 0;                    ///< Per-shape trim start (as PathNodeData::trimStart).
    float trimEnd = 1;                      ///< Per-shape trim end.
    float trimOffset = 0;                   ///< Per-shape trim offset, in turns.
    float fillOpacity = 1;                  ///< PropId FillOpacity.
    float strokeOpacity = 1;                ///< PropId StrokeOpacity.
    std::vector<GradientStop> stops;        ///< Gradient stop pool shared by fill and stroke.
    std::vector<float> dashes;              ///< Dash pattern lengths.
    std::vector<WidthPoint> widthProfile;   ///< T-233: width-profile points (as PathNodeData's).
};

/// See Docs/design/03-scene-and-properties.md §3.5.
struct ImageNodeData {
    /// The backing image asset.
    AssetId asset = kInvalidAsset;
    /// Draw size; 0 means use the asset's native size.
    float width = 0;
    /// Draw size; 0 means use the asset's native size.
    float height = 0;
};

// GlyphRun and TextLine are defined in xt/text_layout.h (included above). See
// Docs/design/07-text.md §7.2.

/// One rich-text run of a Text node (Docs/design/07-text.md §7.8, T-178), loaded from the IR's
/// `DOCT` run table (xt/ir_pod.h IrTextRun). Bytes outside every run use the parent element's
/// style.
struct TextStyleRun {
    /// UTF-8 byte range `[start, end)` into the Text node's string.
    uint32_t start = 0;
    uint32_t end = 0;  ///< One past the run's last byte.
    /// The style override: offset/count into the IR `DOCT` `inlineDecls[]` (color, font-weight,
    /// font-style, font-size, text-effect, letter-spacing).
    uint32_t declOffset = 0;
    uint32_t declCount = 0;  ///< Count of override declarations.
    /// 0 = no inline object; otherwise 1 + an index into TextNodeData::inlines.
    uint32_t inlineNode = 0;
    /// Index into the IR `DOCT` `textLinks()` when the run belongs to an `<a>`, else
    /// 0xFFFFFFFF.
    uint32_t linkIndex = 0xFFFFFFFFu;
};

/// Kinds of TextInline.
enum class TextInlineKind : uint8_t {
    Glyph = 1,  ///< `<glyph key>`: an input-device button glyph resolved by IGlyphProvider.
    Image = 2,  ///< `<img inline src>`.
};

/// One inline object (inline-block, baseline aligned) of a rich-text Text node
/// (Docs/design/07-text.md §7.8, 03 §3.12.1, T-178).
struct TextInline {
    /// Glyph or Image.
    TextInlineKind kind = TextInlineKind::Image;
    /// Glyph: the action name (`key` attribute); kInvalidString for an image.
    StringId action = kInvalidString;
    /// Image: the source asset. Glyph: the asset IGlyphProvider resolved the action to for the
    /// current input device, or kInvalidAsset (then the glyph is drawn as the text `[key]`).
    AssetId asset = kInvalidAsset;
    /// The `height` attribute in px; 0 = the run's font size.
    float height = 0;
    /// Layout output: the object's box relative to the Text node's top-left; `w == 0` when the
    /// object is not laid out as a box (an unresolved glyph drawn as text).
    Rect box{};
};

/// See Docs/design/06-layout.md §6.2.
struct Edges {
    /// Edge width, in logical pixels.
    float top = 0;
    /// Edge width, in logical pixels.
    float right = 0;
    /// Edge width, in logical pixels.
    float bottom = 0;
    /// Edge width, in logical pixels.
    float left = 0;
};

/// See Docs/design/06-layout.md §6.2.
struct LayoutBox {
    // Relative to the parent element's content box top-left (excludes parent scroll/transform).
    /// Border-box position.
    float x = 0;
    /// Border-box position.
    float y = 0;
    float width = 0;   ///< border-box size
    float height = 0;  ///< border-box size
    /// Margin widths outside the border box.
    Edges margin{};
    /// Padding widths inside the border box.
    Edges padding{};
    /// Border widths.
    Edges border{};
    // Derived (cached).
    /// Padding box relative to this box's own border-box top-left.
    Rect paddingBox() const {
        return Rect{border.left, border.top, width - border.left - border.right,
                    height - border.top - border.bottom};
    }
    /// Content box relative to this box's own border-box top-left.
    Rect contentBox() const {
        Rect pb = paddingBox();
        return Rect{pb.x + padding.left, pb.y + padding.top, pb.w - padding.left - padding.right,
                    pb.h - padding.top - padding.bottom};
    }
    // Text measurement cache.
    float intrinsicWidth = 0;   ///< Unwrapped content size (img/anim/text).
    float intrinsicHeight = 0;  ///< Unwrapped content size (img/anim/text).
    // Scroll.
    float scrollWidth = 0;   ///< Content overflow extent.
    float scrollHeight = 0;  ///< Content overflow extent.
};

/// See Docs/design/03-scene-and-properties.md §3.6.
struct TextNodeData {
    /// The source string (IR string table), used unless editIndex overrides it.
    StringId text = kInvalidString;
    /// When != kInvalidEditIndex (0xFFFFFFFF), this Text node's string is not `text` but the
    /// runtime, mutable value of Document's EditState pool entry at this index (an editable
    /// `input`'s text child, created by document_load.cpp). See Code/Runtime/core/src/text_edit.h.
    /// When != kInvalidEditIndex (0xFFFFFFFF), this Text node's string is not `text` but the
    /// runtime, mutable value of Document's EditState pool entry at this index (an editable
    /// `input`'s text child, created by document_load.cpp). See Code/Runtime/core/src/text_edit.h.
    uint32_t editIndex = 0xFFFFFFFFu;
    // Layout output, filled by LayoutEngine (see Docs/design/06-layout.md §6.8).
    /// Shaped glyph runs.
    std::vector<GlyphRun> runs;
    /// Line-break layout, one entry per wrapped line.
    std::vector<TextLine> lines;
    // T-30 measurement output: this text node's own box (§6.8 "節點盒尺寸") and its unwrapped
    // main-axis size, used for shrink-to-fit / flex basis.
    /// This text node's own box width (§6.8), the measurement output.
    float width = 0;
    /// This text node's own box height (§6.8), the measurement output.
    float height = 0;
    /// This text node's unwrapped main-axis size, used for shrink-to-fit / flex basis.
    float intrinsicMain = 0;
    /// Shaping cache (Docs/tasks/T-30.md goal 2): stays valid as long as every key field below
    /// matches the style/text being laid out; a changed `availMain` alone only re-runs
    /// layoutText(). See `shapedValid`.
    /// Shaping cache; valid as long as every key* field below matches the current style/text.
    ShapeResult shaped;
    /// 0 = `shaped` holds no valid cache entry yet; 1 = it does.
    uint8_t shapedValid = 0;
    /// The `text` value `shaped` was last computed for.
    StringId keyText = kInvalidString;
    /// The font `shaped` was last computed for.
    FontId keyFont = kInvalidFont;
    /// The font size `shaped` was last computed for.
    float keySize = 0;
    /// The letter-spacing `shaped` was last computed for.
    float keyLetterSpacing = 0;
    /// The text direction `shaped` was last computed for.
    TextDirection keyDirection = TextDirection::Ltr;
    /// The writing mode `shaped` was last computed for.
    WritingMode keyWritingMode = WritingMode::HorizontalTb;
    /// The text orientation `shaped` was last computed for.
    TextOrientation keyOrientation = TextOrientation::Mixed;
    /// The `lang` `shaped` was last computed for.
    StringId keyLang = kInvalidString;
    /// The `text-transform` `shaped` was last computed for (T-323; None for an editable input).
    TextTransform keyTransform = TextTransform::None;
    /// The ShapeParams::features `shaped` was last computed for (T-331, `font-variant-numeric`).
    uint8_t keyFeatures = 0;
    // U+2026's shaped advance/glyph for the cached (font, size) pair, shaped once per cache fill
    // (Docs/tasks/T-30.md "TextLayoutParams"). `ellipsisAdvance <= 0` means unavailable.
    /// The ellipsis glyph's shaped advance for the cached (font, size) pair; <= 0 means unavailable.
    float ellipsisAdvance = 0;
    /// U+2026's shaped glyph id for `ellipsisFont`, valid when `ellipsisAdvance > 0`.
    uint32_t ellipsisGlyph = 0;
    /// The font `ellipsisAdvance`/`ellipsisGlyph` were shaped with.
    FontId ellipsisFont = kInvalidFont;
    // --- T-178 (Docs/design/07-text.md §7.8): rich text. ---
    /// Rich-text runs, ascending `start`, non-overlapping; empty for plain text.
    std::vector<TextStyleRun> styleRuns;
    /// Inline objects referenced by TextStyleRun::inlineNode.
    std::vector<TextInline> inlines;
    /// Shaping cache companion of `shaped` for rich text: one entry per `shaped.runs[i]`.
    std::vector<TextRunInfo> shapedRunInfo;
    // --- T-395 (Docs/design/08-events.md §8.16): `<input placeholder>`. ---
    /// The placeholder laid out like the input's value, filled only while an editable input's
    /// value is empty (and it is not composing) and its `placeholder` attribute is non-empty;
    /// empty otherwise. Not part of `runs`/`lines`: no caret position, no hit test, no effect on
    /// the node's box or intrinsic size.
    std::vector<GlyphRun> placeholderRuns;
    /// Shaping cache of the placeholder string (valid while every placeholderKey* field matches).
    ShapeResult placeholderShaped;
    /// 0 = `placeholderShaped` holds no valid cache entry yet.
    uint8_t placeholderShapedValid = 0;
    /// The `placeholder` attribute value `placeholderShaped` was computed for.
    StringId placeholderKeyText = kInvalidString;
    /// The font `placeholderShaped` was computed for.
    FontId placeholderKeyFont = kInvalidFont;
    /// The font size `placeholderShaped` was computed for.
    float placeholderKeySize = 0;
    /// The letter-spacing `placeholderShaped` was computed for.
    float placeholderKeyLetterSpacing = 0;
    /// The direction `placeholderShaped` was computed for.
    TextDirection placeholderKeyDirection = TextDirection::Ltr;
    /// The writing mode `placeholderShaped` was computed for.
    WritingMode placeholderKeyWritingMode = WritingMode::HorizontalTb;
    /// The text orientation `placeholderShaped` was computed for.
    TextOrientation placeholderKeyOrientation = TextOrientation::Mixed;
    /// The `lang` `placeholderShaped` was computed for.
    StringId placeholderKeyLang = kInvalidString;
    /// The ShapeParams::features `placeholderShaped` was computed for.
    uint8_t placeholderKeyFeatures = 0;
};

// Non-animatable StyleProp enums (05.4). Numbering matches the compiler's xts_props.h/.cpp
// (Code/Runtime/compile/src/xts) exactly; core does not include Code/Runtime/compile/ headers, so these are redefined here
// (see Docs/tasks/T-24.md).
/// CSS `display`.
enum class Display : uint8_t {
    Block = 0,  ///< CSS `block`.
    Flex = 1,   ///< CSS `flex`.
    None = 2,   ///< CSS `none` (not rendered, not laid out).
    Grid = 3    ///< CSS `grid` (06 §6.15, T-338): the fixed-track subset.
};
/// CSS `position`.
enum class Position : uint8_t {
    Static = 0,    ///< CSS `static`.
    Relative = 1,  ///< CSS `relative`.
    Absolute = 2   ///< CSS `absolute`.
};
/// CSS `flex-direction`.
enum class FlexDirection : uint8_t {
    Row = 0,           ///< CSS `row`.
    Column = 1,        ///< CSS `column`.
    RowReverse = 2,    ///< CSS `row-reverse`.
    ColumnReverse = 3  ///< CSS `column-reverse`.
};
/// CSS `flex-wrap`.
enum class FlexWrap : uint8_t {
    NoWrap = 0,      ///< CSS `nowrap`.
    Wrap = 1,        ///< CSS `wrap`.
    WrapReverse = 2  ///< CSS `wrap-reverse` (05 §5.9.8, T-336): lines stack cross-end first.
};
/// CSS `justify-content`.
enum class Justify : uint8_t {
    FlexStart = 0,     ///< CSS `flex-start`.
    FlexEnd = 1,       ///< CSS `flex-end`.
    Center = 2,        ///< CSS `center`.
    SpaceBetween = 3,  ///< CSS `space-between`.
    SpaceAround = 4,   ///< CSS `space-around`.
    SpaceEvenly = 5,   ///< CSS `space-evenly`.
    /// CSS `stretch` / `normal` (06 §6.15, T-338), the initial value: `flex-start` in a flex
    /// container; a grid stretches its `auto` columns over the free space.
    Stretch = 6
};
/// CSS `align-items`.
enum class AlignItems : uint8_t {
    Stretch = 0,    ///< CSS `stretch`.
    FlexStart = 1,  ///< CSS `flex-start`.
    FlexEnd = 2,    ///< CSS `flex-end`.
    Center = 3      ///< CSS `center`.
};
/// CSS `align-self`.
enum class AlignSelf : uint8_t {
    Auto = 0,       ///< CSS `auto` (inherit the parent's align-items).
    Stretch = 1,    ///< CSS `stretch`.
    FlexStart = 2,  ///< CSS `flex-start`.
    FlexEnd = 3,    ///< CSS `flex-end`.
    Center = 4      ///< CSS `center`.
};
/// CSS `align-content`.
enum class AlignContent : uint8_t {
    FlexStart = 0,     ///< CSS `flex-start`.
    FlexEnd = 1,       ///< CSS `flex-end`.
    Center = 2,        ///< CSS `center`.
    SpaceBetween = 3,  ///< CSS `space-between`.
    SpaceAround = 4,   ///< CSS `space-around`.
    SpaceEvenly = 5,   ///< CSS `space-evenly`.
    Stretch = 6        ///< CSS `stretch`.
};
/// CSS `overflow`.
enum class Overflow : uint8_t {
    Visible = 0,  ///< CSS `visible`.
    Hidden = 1,   ///< CSS `hidden`.
    Scroll = 2,   ///< CSS `scroll`.
    Auto = 3      ///< CSS `auto` (05 §5.9.8, T-336): `scroll` once the content overflows.
};
/// CSS `visibility`.
enum class Visibility : uint8_t {
    Visible = 0,  ///< CSS `visible`.
    Hidden = 1    ///< CSS `hidden`.
};
/// CSS `pointer-events` (only the two values this project supports).
enum class PointerEvents : uint8_t {
    Auto = 0,  ///< CSS `auto` (participates in hit testing).
    None = 1   ///< CSS `none` (excluded from hit testing).
};
/// 05 §5.9.11 (T-386): in the encoded `hit-slop` declaration of an IR of minor <= 7 (StyleProp 276:
/// four f32 encoded like a Color, top / right / bottom / left), a slot <= -kHitSlopEmBias holds
/// `em` as -(kHitSlopEmBias + em); a slot in (-kHitSlopEmBias, 0) is a percentage -f (f = percent /
/// 100); a slot >= 0 is px. T-405: read only from such files; IR minor 8 writes the Length
/// longhands StyleProp 277-280 (02 §2.6). ComputedStyle::hitSlop holds the resolved values.
inline constexpr float kHitSlopEmBias = 65536.0f;
/// 05 §5.9.10 (T-376): `app-region`, the borderless-window drag region a host hit-test reads
/// (Document::appRegionAt).
enum class AppRegion : uint8_t {
    None = 0,   ///< `none` (the ancestors decide).
    Drag = 1,   ///< `drag` (the OS drags the window).
    NoDrag = 2  ///< `no-drag` (a normal client area inside a drag region).
};
/// CSS `font-style` (only the two values this project supports).
enum class FontStyle : uint8_t {
    Normal = 0,  ///< CSS `normal`.
    Italic = 1   ///< CSS `italic`.
};
/// CSS `object-fit`.
enum class ObjectFit : uint8_t {
    Fill = 0,     ///< CSS `fill` (stretch to fill the box).
    Contain = 1,  ///< CSS `contain` (scale to fit inside the box, preserving aspect ratio).
    Cover = 2     ///< CSS `cover` (scale to cover the box, preserving aspect ratio, may crop).
};

/// Which shape a CSS `background-gradient` is (Docs/design/05-style-and-css.md §5.5c, T-115).
/// Distinct from PaintKind (whose None/Solid members are meaningless for this property).
enum class GradientKind : uint8_t {
    None = 0,    ///< `background-gradient: none` (the initial value).
    Linear = 1,  ///< `linear-gradient(<angle>, <color-stop>+)`.
    Radial = 2,  ///< `radial-gradient(<len> <len> at <len> <len>, <color-stop>+)`.
    /// T-201 (05 §5.9.2): `conic-gradient([from] <angle> [at <len> <len>], <color-stop>+)`.
    Conic = 3
};

/// A resolved `background-gradient` declaration (05.5c, T-115), as StyleEngine::resolve decodes it
/// out of the IR's STYL `gradientPool[]` (or Document's runtime gradient declaration pool) into
/// ElementData. Lengths stay unresolved here (a `%` needs the layout box, which the cascade does
/// not have yet); Painter resolves them against the element's box, see painter_element.cpp.
struct ElementGradient {
    /// Linear, Radial, or None (this element has no gradient background).
    GradientKind kind = GradientKind::None;
    /// Linear: the gradient-line angle in degrees (CSS semantics: 0 = towards the top, then
    /// clockwise). Conic (T-201): the start angle, same convention.
    float angle = 0;
    /// Radial only: horizontal radius (`%` resolves against the box width).
    Length rx{};
    /// Radial only: vertical radius (`%` resolves against the box height).
    Length ry{};
    /// Radial / Conic: center x (`%` resolves against the box width).
    Length cx{};
    /// Radial / Conic: center y (`%` resolves against the box height).
    Length cy{};
    /// The color stops, in the order they were written.
    std::vector<GradientStop> stops;
};

/// 05 §5.9.8 (T-332): the basic shape of a `clip-path` declaration.
enum class ClipShapeKind : uint8_t {
    None = 0,     ///< `clip-path: none` (the initial value).
    Inset = 1,    ///< `inset(<t> [<r> [<b> [<l>]]] [round <radii>])`.
    Circle = 2,   ///< `circle(<r> [at <x> <y>])`.
    Ellipse = 3,  ///< `ellipse(<rx> <ry> [at <x> <y>])`.
    Polygon = 4,  ///< `polygon(<x> <y>, ...)`, filled even-odd.
};

/// T-332: a circle / ellipse radius written as `closest-side` or `farthest-side` is stored as a
/// Length of unit LengthUnit::Auto whose value is one of these two markers.
inline constexpr float kClipRadiusClosestSide = 0;
inline constexpr float kClipRadiusFarthestSide = 1;
/// T-332: ElementClipShape::params counts per kind (a polygon has 2 per vertex, at least 3
/// vertices and at most kMaxClipPolygonVertices).
inline constexpr uint32_t kClipInsetParamCount = 12;
inline constexpr uint32_t kClipCircleParamCount = 3;
inline constexpr uint32_t kClipEllipseParamCount = 4;
inline constexpr uint32_t kMaxClipPolygonVertices = 1024;

/// A `clip-path` basic shape (05 §5.9.8, T-332), as StyleEngine::resolve decodes it out of the
/// IR's `CLIP` chunk (or Document's runtime clip-shape pool) into ElementData. Lengths stay
/// unresolved here: a `%` is taken of the element's border box, which only layout knows; the
/// Painter (clip path), hit test (point-in-shape) and damage (bounding box) resolve them.
struct ElementClipShape {
    /// Which shape, or None for no clip.
    ClipShapeKind kind = ClipShapeKind::None;
    /// Inset: top, right, bottom, left insets, then the corner radii top-left x/y, top-right x/y,
    /// bottom-right x/y, bottom-left x/y (12 entries). Circle: r, cx, cy (3). Ellipse: rx, ry,
    /// cx, cy (4). Polygon: x0, y0, x1, y1, ... (2 per vertex). Px or %; a circle / ellipse
    /// radius may also be LengthUnit::Auto with a kClipRadius* marker value.
    std::vector<Length> params;
};

/// 06 §6.15 (T-338): the size kind of one `grid-template-columns` / `-rows` track.
enum class GridTrackKind : uint8_t {
    Px = 0,       ///< `<n>px`.
    Percent = 1,  ///< `<n>%` of the container's content box (indefinite: as `auto`).
    Fr = 2,       ///< `<n>fr`: a share of the space left after the other tracks and the gaps.
    Auto = 3,     ///< `auto`: the largest max-content size of the track's single-span items.
};

/// T-338: one grid track (after `repeat()` expansion).
struct GridTrack {
    GridTrackKind kind = GridTrackKind::Auto;
    float value = 0;  ///< Px / Percent / Fr: the number; Auto: 0.
};

/// T-338: one named `grid-template-areas` area -- 0-based lines, end exclusive.
struct GridArea {
    StringId name = kInvalidString;
    uint16_t rowStart = 0;
    uint16_t rowEnd = 0;
    uint16_t colStart = 0;
    uint16_t colEnd = 0;
};

/// T-338: a `grid-template-*` track list holds at most this many tracks (after `repeat()`), and an
/// area matrix at most this many rows / columns; a grid line number is at most kMaxGridTracks + 1.
inline constexpr uint32_t kMaxGridTracks = 1000;

/// T-338: the encoding of a `grid-column-start` / `-end` / `grid-row-start` / `-end` value (the IR
/// Enum payload of StyleProps 268-271): 0 = `auto`, n = line n (1-based), kGridLineSpan | n =
/// `span n`.
inline constexpr uint32_t kGridLineAuto = 0;
inline constexpr uint32_t kGridLineSpan = 0x10000u;

/// T-338: true for a valid grid line payload (auto, a line 1..kMaxGridTracks + 1, or a span
/// 1..kMaxGridTracks).
inline bool validGridLine(uint32_t v) {
    if (v == kGridLineAuto) {
        return true;
    }
    const uint32_t n = v & ~kGridLineSpan;
    return (v & kGridLineSpan) != 0 ? (n >= 1 && n <= kMaxGridTracks) : (v <= kMaxGridTracks + 1);
}

/// A grid container's resolved template (06 §6.15, T-338), decoded by StyleEngine::resolve from
/// the IR `GRID` chunk into ElementData. Empty for every non-grid element.
struct ElementGrid {
    std::vector<GridTrack> columns;  ///< `grid-template-columns`, `repeat()` expanded.
    std::vector<GridTrack> rows;     ///< `grid-template-rows`.
    std::vector<GridArea> areas;     ///< `grid-template-areas`' named areas.
    uint16_t areaRows = 0;           ///< The area matrix's row count (0 = no areas).
    uint16_t areaCols = 0;           ///< Its column count.
};

/// T-199: the `auto` marker of ComputedStyle::borderImageSlice (NaN; compared with std::isnan).
inline constexpr float kAutoSlice = std::numeric_limits<float>::quiet_NaN();

/// T-324 (05 §5.9.8): the `text-decoration-line` bits (the IR Enum payload of StyleProp 203).
inline constexpr uint8_t kTextDecorationUnderline = 1;
inline constexpr uint8_t kTextDecorationOverline = 2;
inline constexpr uint8_t kTextDecorationLineThrough = 4;
inline constexpr uint8_t kTextDecorationAll = 7;

/// T-327 (05 §5.9.8): `background-repeat` (the IR Enum payload of StyleProp 205). `stretch` is
/// XTFrame's own initial value -- the image stretched over the padding box, size and position
/// ignored -- so documents written before T-327 draw unchanged.
inline constexpr uint8_t kBackgroundRepeatStretch = 0;
inline constexpr uint8_t kBackgroundRepeatNone = 1;  ///< `no-repeat`.
inline constexpr uint8_t kBackgroundRepeatBoth = 2;  ///< `repeat`.
inline constexpr uint8_t kBackgroundRepeatX = 3;     ///< `repeat-x`.
inline constexpr uint8_t kBackgroundRepeatY = 4;     ///< `repeat-y`.

/// 05 §5.9.8 (T-333): `border-*-style` values (ComputedStyle::borderStyle). Solid is the initial
/// value, so a document that never declares a style draws as before.
inline constexpr uint8_t kBorderStyleNone = 0;
inline constexpr uint8_t kBorderStyleSolid = 1;
inline constexpr uint8_t kBorderStyleDashed = 2;
inline constexpr uint8_t kBorderStyleDotted = 3;
/// 05 §5.9.9 (T-345): `ui-scale` modes (ComputedStyle::uiScaleMode and the first f32 of StyleProp
/// 273's payload).
inline constexpr uint8_t kUiScaleAuto = 0;   ///< The Document's uiscale (the project policy).
inline constexpr uint8_t kUiScaleNone = 1;   ///< 1 x DeviceInfo::dpr (device pixels).
inline constexpr uint8_t kUiScaleClamp = 2;  ///< The uiscale clamped to [uiScaleMin, uiScaleMax].
/// 05 §5.9.9 (T-345): `media-override` values (ComputedStyle::mediaOverride, StyleProp 274).
inline constexpr uint8_t kMediaOverrideAuto = 0;  ///< `@media` rules switch `display` as usual.
inline constexpr uint8_t kMediaOverrideShow = 1;  ///< `@media` rules never change `display`.
inline constexpr uint8_t kMediaOverrideHide = 2;  ///< Always `display: none`.
/// T-327: the `background-size` keywords, as the value of a LengthUnit::Auto length in
/// base[BackgroundSizeX/Y] (xt/property.h); 0 = `auto`.
inline constexpr float kBackgroundSizeCover = 1.0f;
inline constexpr float kBackgroundSizeContain = 2.0f;

/// Element style property table (T-24). base[i] holds PropId i's current animatable value (03.9);
/// base[0..13] (the general PropIds) and the undefined 94/95 gap are reserved but unused for the
/// underlying storage: Element writes/reads Opacity/Anchor/Translate/... through NodeCommon /
/// ElementData::cssTranslate / ElementData::cssAnchor instead (see Docs/design/06-layout.md §6.7
/// and setProperty's Element special case); StyleEngine still stages those props' decoded values
/// in base[] transiently while resolving, before routing them through setProperty(). Defaults for
/// base[64..105] are filled in by initComputedStyleDefaults() (xt/property.h), called from
/// NodeArena::create(NodeKind::Element).
/// See Docs/design/05-style-and-css.md §5.7.
struct ComputedStyle {
    /// CSS `display`.
    Display display = Display::Block;
    /// CSS `position`.
    Position position = Position::Static;
    /// CSS `flex-direction`.
    FlexDirection flexDir = FlexDirection::Row;
    /// CSS `flex-wrap`.
    FlexWrap flexWrap = FlexWrap::NoWrap;
    /// CSS `justify-content`.
    Justify justify = Justify::Stretch;
    /// CSS `align-items`.
    AlignItems alignItems = AlignItems::Stretch;
    /// CSS `align-self`.
    AlignSelf alignSelf = AlignSelf::Auto;
    /// CSS `align-content`.
    AlignContent alignContent = AlignContent::Stretch;
    /// CSS `overflow-x` (05 §5.9.8, T-336; `overflow` sets both axes). The declared value: use
    /// usedOverflowX() for the CSS rule that a `visible` axis beside a non-visible one is `auto`.
    Overflow overflowX = Overflow::Visible;
    /// CSS `overflow-y`; see overflowX.
    Overflow overflowY = Overflow::Visible;
    /// CSS `z-index`.
    float zIndex = 0;
    /// CSS `visibility`.
    Visibility visibility = Visibility::Visible;
    /// CSS `pointer-events`.
    PointerEvents pointerEvents = PointerEvents::Auto;
    /// CSS `backface-visibility` (StyleProp 188); 1 = hidden. Defaults to 0 (visible); written by
    /// the cascade (T-47a) and copied to NodeCommon::backfaceHidden each resolve (19.3,
    /// Docs/tasks/T-43c.md).
    uint8_t backfaceHidden = 0;
    /// CSS-like `pointer-watch` (StyleProp 191, 27.2.5, T-174); 1 = true. Inherited; copied to
    /// NodeCommon::flags2's kPointerWatch each resolve.
    uint8_t pointerWatch = 0;
    /// T-194 (08.9): `focus-nav` (StyleProp 195): 0 auto, 1 explicit, 2 none. Inherited.
    uint8_t focusNav = 0;
    /// T-194 (08.9): `layer` (StyleProp 196): 0 normal, 1 overlay. Not inherited; copied to
    /// NodeCommon::flags2's kLayerOverlay each resolve.
    uint8_t layerOverlay = 0;
    /// Offset into the IR fontFamilies pool.
    uint32_t fontFamilyOffset = 0;
    /// Count of font families in the CSS font-family list.
    uint32_t fontFamilyCount = 0;
    /// CSS `font-weight`.
    uint16_t fontWeight = 400;
    /// CSS `font-style`.
    FontStyle fontStyle = FontStyle::Normal;
    /// CSS `text-align`.
    TextAlign textAlign = TextAlign::Start;
    /// CSS `white-space`.
    WhiteSpace whiteSpace = WhiteSpace::Normal;
    /// CSS `text-overflow`.
    TextOverflow textOverflow = TextOverflow::Clip;
    /// CSS `text-transform` (StyleProp 202, 05 §5.9.8, T-323); inherited.
    TextTransform textTransform = TextTransform::None;
    /// CSS `line-clamp` (StyleProp 256, 05 §5.9.8, T-331): the most lines this element's text
    /// takes; 0 = `none`. Not inherited.
    uint32_t lineClamp = 0;
    /// CSS `font-variant-numeric` (StyleProp 257, 05 §5.9.8, T-331): 0 normal, 1 tabular-nums.
    /// Inherited.
    uint8_t fontVariantNumeric = 0;
    // --- 06 §6.15 (T-338): grid. None inherited, none animatable. ---
    /// `grid-template-columns` / `-rows` (StyleProps 264 / 265): 0 = `none`, else 1 + the IR `GRID`
    /// chunk's track-list index. StyleEngine::resolve decodes them into ElementData::grid.
    uint32_t gridColumns = 0;
    uint32_t gridRows = 0;
    /// `grid-template-areas` (StyleProp 266): 0 = `none`, else 1 + the `GRID` area-list index.
    uint32_t gridAreas = 0;
    /// `justify-items` (StyleProp 267): Stretch (initial), FlexStart = `start`, FlexEnd = `end`,
    /// Center -- the AlignItems numbering.
    AlignItems justifyItems = AlignItems::Stretch;
    /// `grid-column-start`, `grid-column-end`, `grid-row-start`, `grid-row-end` (StyleProps
    /// 268-271), kGridLine* encoded.
    uint32_t gridLines[4] = {0, 0, 0, 0};
    /// `grid-area: <name>` (StyleProp 272): the area name, kInvalidString = `auto`.
    StringId gridAreaName = kInvalidString;
    // --- 05 §5.9.9 (T-345): the responsive exceptions. Both inherited, neither animatable. ---
    /// `ui-scale` (StyleProp 273): a kUiScale* value; for kUiScaleClamp the bounds below.
    uint8_t uiScaleMode = kUiScaleAuto;
    /// `media-override` (StyleProp 274): a kMediaOverride* value.
    uint8_t mediaOverride = kMediaOverrideAuto;
    /// `app-region` (StyleProp 275, 05 §5.9.10, T-376): inherited, not animatable.
    AppRegion appRegion = AppRegion::None;
    /// `hit-slop` (StyleProp 276, 05 §5.9.11, T-386): how far the hit rectangle reaches past the
    /// border box -- top, right, bottom, left. A value >= 0 is logical px (px / em resolved, ui-scale
    /// applied); a negative value -f is the fraction f of the element's own border-box width
    /// (right, left) or height (top, bottom), resolved by the hit test. Not inherited, not
    /// animatable; layout, paint and focus navigation ignore it.
    float hitSlop[4] = {0, 0, 0, 0};
    /// `ui-scale: clamp(uiScaleMin, uiScaleMax)`'s bounds (0 otherwise).
    float uiScaleMin = 0;
    float uiScaleMax = 0;
    /// The factor this element's px lengths were resolved with (StyleEngine::resolve): `auto` =
    /// the Document's uiscale, `none` = DeviceInfo::dpr, `clamp` = the uiscale clamped. Every
    /// px / em / rem length of base[] (and font-size, translate, transform-origin, effect radii,
    /// `calc()` terms) is already multiplied by it; `%`, `vw`/`vh`, `safe-*` and `uiscale` units
    /// are not. Keyframe and script values are multiplied by it when the Animator applies them.
    float uiScaleEff = 1;
    /// CSS `cursor`.
    Cursor cursor = Cursor::Default;
    /// CSS `object-fit`.
    ObjectFit objectFit = ObjectFit::Contain;
    /// CSS `direction`.
    TextDirection direction = TextDirection::Ltr;
    /// CSS `writing-mode`.
    WritingMode writingMode = WritingMode::HorizontalTb;
    /// CSS `text-orientation`.
    TextOrientation textOrientation = TextOrientation::Mixed;
    /// CSS `text-rendering` (StyleProp 189, Docs/design/23-host-integration.md §23.4, T-101).
    TextRendering textRendering = TextRendering::Auto;
    /// The resolved `lang` (for IME/shaping); kInvalidString when unset.
    StringId lang = kInvalidString;
    /// Offset into the IR transitions pool; 0,0 = none.
    uint32_t transitionOffset = 0;
    /// Count of transition entries.
    uint32_t transitionCount = 0;
    /// -> IR animations pool; count 0 = none.
    uint32_t animationOffset = 0;  ///< Offset into the IR animations pool.
    uint32_t animationCount = 0;   ///< Count of animation entries; 0 = none.
    /// CSS `effect` (StyleProp 187, Docs/design/18-effects.md §18.4, T-51b): the winning
    /// declaration's whole sequence, cascaded like `transition`/`animation` above (whole-sequence
    /// replace, not inherited; 0, 0 = `effect: none`, the initial value). Offset/count into the IR
    /// STYL `effects[]` pool, or -- when `effectOffset & kRuntimeEffectOffsetBit` is set -- into
    /// Document's runtime effect declaration pool at `effectOffset & ~kRuntimeEffectOffsetBit`
    /// (see kRuntimeEffectOffsetBit above). StyleEngine::resolve turns this into the node's actual
    /// effect stack via NodeArena::assignEffects() right before committing `next` (18.4's cascade
    /// paragraph).
    uint32_t effectOffset = 0;  ///< Offset into the IR/runtime effect pool (see above).
    uint32_t effectCount = 0;   ///< Count of effect entries; 0 = none.
    /// CSS `background-gradient` (StyleProp 190, Docs/design/05-style-and-css.md §5.5c, T-115):
    /// the winning declaration, cascaded like `effect` above (whole-value replace, not inherited,
    /// not animatable; count 0 = `none`, the initial value). Offset/count into the IR STYL
    /// `gradientPool[]`, or -- when `gradientOffset & kRuntimeEffectOffsetBit` is set -- into
    /// Document's runtime gradient declaration pool. StyleEngine::resolve decodes it into
    /// ElementData::backgroundGradient right before committing `next`.
    uint32_t gradientOffset = 0;  ///< Offset into the IR/runtime gradient pool (see above).
    uint32_t gradientCount = 0;   ///< 1 when a gradient is set, 0 for `none`.
    /// CSS `text-effect` (StyleProp 192, Docs/design/07-text.md §7.8, T-178): offset/count into
    /// the IR STYL `effects[]` pool (Outline = `outline <w> <color>`, Glow = `glow <r> <color>`,
    /// DropShadow = `shadow <dx> <dy> <r> <color>`); inherited; 0, 0 = `none`. Drawn at the glyph
    /// level by the Painter (not through the 18 effect stack).
    uint32_t textEffectOffset = 0;
    uint32_t textEffectCount = 0;  ///< Count of `text-effect` entries; 0 = none.
    /// CSS `background-image`, resolved to an asset; kInvalidAsset when unset.
    AssetId backgroundImage = kInvalidAsset;
    /// T-199 (26 §26.3.4): `border-image-source` (StyleProp 197) resolved to an asset;
    /// kInvalidAsset = `none`. When set, the nine-slice replaces the plain border.
    AssetId borderImage = kInvalidAsset;
    /// `border-image-slice` (StyleProp 198) as declared: L, R, T, B; >= 0 = source pixels, a
    /// negative value -f = the fraction f of the source width (L, R) / height (T, B); NaN = `auto`
    /// (the asset's manifest slice, see ElementData::borderImageSlicePx).
    float borderImageSlice[4] = {kAutoSlice, kAutoSlice, kAutoSlice, kAutoSlice};
    /// `border-image-repeat` (StyleProp 199): 0 stretch, 1 repeat, 2 round.
    uint8_t borderImageRepeat = 0;
    /// `border-image-fill` (StyleProp 200): 1 = the middle tile is drawn.
    uint8_t borderImageFill = 0;
    /// `outline-style` (StyleProp 201, 05 §5.9.8, T-325): 0 none, 1 solid. The ring's width,
    /// color and offset are base[OutlineWidth/OutlineColor/OutlineOffset].
    uint8_t outlineStyle = 0;
    /// `border-top/right/bottom/left-style` (StyleProps 258-261, 05 §5.9.8, T-333), indexed top,
    /// right, bottom, left: a kBorderStyle* value, initially solid.
    uint8_t borderStyle[4] = {kBorderStyleSolid, kBorderStyleSolid, kBorderStyleSolid, kBorderStyleSolid};
    /// T-333: bit i (1 top, 2 right, 4 bottom, 8 left) set = side i draws in its own
    /// base[BorderTopColor + i]; clear = it follows the single base[BorderColor] (the legacy
    /// `border-color`). Set by the cascade for the sides it declares after the last
    /// `border-color`, and by setProperty() of a side color (an animation or a script).
    uint8_t borderColorSides = 0;
    /// `text-decoration-line` (StyleProp 203, 05 §5.9.8, T-324) as declared on this element: a
    /// set of kTextDecoration* bits; 0 = `none`. Not inherited; the line's color and thickness
    /// are base[TextDecorationColor/TextDecorationThickness].
    uint8_t textDecorationLine = 0;
    /// `background-repeat` (StyleProp 205, 05 §5.9.8, T-327): a kBackgroundRepeat* value; 0 =
    /// `stretch` (the initial value). Size and position are base[BackgroundSize*/Position*].
    uint8_t backgroundRepeat = 0;
    /// `image-rendering` (StyleProp 206, T-327): 0 auto, 1 pixelated (nearest-neighbour sampling
    /// of the background image and of an `<img>` / `<sequence>`). Inherited.
    uint8_t imageRendering = 0;
    /// T-324: the decorations this element's text draws: its own textDecorationLine plus every
    /// ancestor's (CSS draws a decoration across all descendant text; the resolver passes the set
    /// down). The ancestors' share is `inheritedDecorations`, drawn in the nearest decorating
    /// ancestor's color/thickness below (snapshotted when this element was resolved).
    uint8_t textDecorations = 0;
    uint8_t inheritedDecorations = 0;  ///< The ancestors' part of textDecorations.
    /// T-324: color of the inherited decorations (the nearest decorating ancestor's
    /// base[TextDecorationColor]).
    Color inheritedDecorationColor{0, 0, 0, 1};
    /// T-324: thickness of the inherited decorations in px; < 0 = `auto`.
    float inheritedDecorationThickness = -1.0f;
    /// T-324: decoration metrics of resolvedFont in em (FontMetrics); all 0 without a shaper. 0
    /// offset / thickness = the font gave none (07 §7.2 fallbacks).
    float decoAscent = 0;
    float decoDescent = 0;
    float decoUnderlineOffset = 0;
    float decoUnderlineThickness = 0;
    /// `mix-blend-mode` (StyleProp 204, 18 §18.12, T-326): a BlendMode value (0 normal .. 16 add).
    /// StyleEngine copies it to NodeCommon::blend; the Painter then wraps the element in a
    /// PushLayer(opacity, blend) when it is not Normal (18 §18.1).
    uint8_t mixBlendMode = 0;
    /// `clip-path` (StyleProp 207, 05 §5.9.8, T-332): 0 = `none`; otherwise 1 + the index into
    /// the IR `CLIP` chunk's shapes, or -- when `clipShape & kRuntimeEffectOffsetBit` is set --
    /// the index into Document's runtime clip-shape pool (like gradientOffset above). Not
    /// inherited, not animatable. StyleEngine::resolve decodes it into ElementData::clipShape.
    uint32_t clipShape = 0;
    /// The animatable general/Element PropIds' current values; see the struct comment above.
    Value base[prop::kElementPropCount]{};
    /// The font resolved from fontFamilyOffset/Count, fontWeight and fontStyle.
    FontId resolvedFont = kInvalidFont;
    // Runtime helper: the animation declaration list last applied, used to diff against the new
    // one each resolve (see Docs/tasks/T-24.md step 5).
    uint32_t lastAnimationOffset = 0;  ///< Offset of the last-applied animation declaration list.
    uint32_t lastAnimationCount = 0;   ///< Count of the last-applied animation declaration list.
    /// `clip-start` (StyleProp 193, 28 §28.2, 04 §4.10.2, T-191): seconds of the parent timeline
    /// at which a `sync` clip's local time is 0. Not inherited; initial 0.
    float clipStart = 0;
    /// `clip-rate` (StyleProp 194): local clip seconds per parent timeline second. Not inherited;
    /// initial 1.
    float clipRate = 1;
};

/// 05 §5.9.8 (T-336): the used `overflow` of one axis -- a `visible` axis beside a non-`visible`
/// one behaves as `auto` (CSS Overflow 3 §3.1).
inline Overflow usedOverflow(Overflow axis, Overflow other) {
    return axis == Overflow::Visible && other != Overflow::Visible ? Overflow::Auto : axis;
}
/// usedOverflow() of the horizontal axis.
inline Overflow usedOverflowX(const ComputedStyle& style) {
    return usedOverflow(style.overflowX, style.overflowY);
}
/// usedOverflow() of the vertical axis.
inline Overflow usedOverflowY(const ComputedStyle& style) {
    return usedOverflow(style.overflowY, style.overflowX);
}

/// One runtime style declaration (T-35: ElementRef::setStyle). `prop` is either an animatable
/// PropId (0-105) or a non-animatable StyleProp number (160-186, Code/Runtime/core/src/style_decode.h) and
/// `value` its already-decoded value (Length values may still carry Em, resolved against the
/// element's own font-size when the cascade applies them, like an IR decl's). String-valued props
/// (`font-family`, `background-image`) carry an already-interned StringId in `value.sid`.
/// See Docs/design/05-style-and-css.md §5.7, Docs/tasks/T-35.md.
struct RuntimeStyleDecl {
    /// PropId (animatable) or StyleProp number (non-animatable); see the struct comment above.
    uint16_t prop = 0;
    /// Non-zero when this declaration carried `!important`.
    uint8_t important = 0;
    /// T-270e: non-zero once setStyle (or a style binding) warned that this element's value for
    /// `prop` names a font family or an image the compiled document does not have; kept across
    /// later values of the property so the warning comes once per element and property, and gone
    /// with the declarations when the element is removed or its `<repeat>` instance recycled.
    uint8_t undeclaredNameWarned = 0;
    /// The already-decoded value (unused when `varName` is set).
    Value value{};
    /// T-170 (Docs/design/05-style-and-css.md §5.9.2): the `--name` of a `var(--name[,
    /// fallback])` declaration, or kInvalidString. StyleEngine then resolves the value from the
    /// token tables each time the element's style is resolved.
    StringId varName = kInvalidString;
    /// T-170: the `var()` fallback, valid when `hasVarFallback` is non-zero.
    Value varFallback{};
    /// T-170: non-zero when `varFallback` holds a value.
    uint8_t hasVarFallback = 0;
};

/// See Docs/design/03-scene-and-properties.md §3.6.
struct ElementData {
    /// The tag name (e.g. "div", "img").
    StringId tag = kInvalidString;
    /// The `id` attribute value; kInvalidString when unset.
    StringId id = kInvalidString;
    /// T-177: on a `<repeat>` instance element, the template-relative id it was copied from (its
    /// `id` is the path `<repeat id>/<index>/<template id>`); `#<template id>` selectors match it
    /// too. kInvalidString everywhere else.
    StringId templateId = kInvalidString;
    std::vector<StringId> classes;  ///< Sorted, for binary search.
    uint16_t stateBits = 0;         ///< ElementState bits.
    // T-33: 1 when this element can take keyboard focus by its tag/attributes alone (`button`,
    // `input`, or any element carrying a `tabindex` attribute -- Docs/tasks/T-33.md "可 focus").
    // The dynamic half of the rule (disabled / display:none / visibility:hidden) is evaluated per
    // query in focus.cpp, not stored here.
    /// 1 when this element can take keyboard focus by its tag/attributes alone (button,
    /// input, or any element carrying a tabindex attribute). The dynamic half of the rule
    /// (disabled / display:none / visibility:hidden) is evaluated per query in focus.cpp, not
    /// stored here.
    uint8_t focusable = 0;
    /// The element's resolved style, produced by StyleEngine::resolve.
    ComputedStyle style{};
    /// The element's layout result, produced by LayoutEngine::layout.
    LayoutBox box{};
    /// The element's resolved `background-gradient` (05.5c, T-115), decoded by
    /// StyleEngine::resolve from ComputedStyle::gradientOffset/gradientCount. `kind == None` when
    /// the element has no gradient background.
    ElementGradient backgroundGradient{};
    /// The element's resolved `clip-path` shape (05 §5.9.8, T-332), decoded by
    /// StyleEngine::resolve from ComputedStyle::clipShape. `kind == None` when the element has no
    /// clip; the Painter pushes it as a PushClipPath around the element's own drawing and its
    /// children, hit testing rejects points outside it (08 §8.2).
    ElementClipShape clipShape{};
    /// The element's resolved grid template (06 §6.15, T-338), decoded by StyleEngine::resolve from
    /// ComputedStyle::gridColumns / gridRows / gridAreas; read by layout_grid.cpp.
    ElementGrid grid{};
    /// Write target of PropId 3/4/11 for Element nodes (section 6.7), px part.
    Vec2 cssTranslate{};
    /// T-211: percent part of PropId 3/4 (of the element's own border box), resolved by the §6.7
    /// fold after layout and added to cssTranslate. A Float/px write clears it (05 tail ruling).
    Vec2 cssTranslatePct{};
    /// Write target of PropId 1/2/10 for Element nodes; unit px or % per style.
    Vec2 cssAnchor{};
    /// transform-origin raw value, x component.
    Length originX{};
    Length originY{};  ///< transform-origin raw value
    // T-25: img/anim intrinsic size (Docs/design/06-layout.md §6.10), cached by LayoutEngine so a
    // later task (e.g. T-26 flex) can read it without recomputing (re-decoding data-xt-asset,
    // re-looking-up the scene chunk). (0, 0) / hasIntrinsic == 0 for every other tag.
    /// img/anim intrinsic size (Docs/design/06-layout.md section 6.10), cached by LayoutEngine.
    /// (0, 0) when hasIntrinsic == 0 (every non-img/anim tag).
    Vec2 intrinsic{};
    /// 1 when `intrinsic` holds a valid cached size, 0 otherwise (every non-img/anim tag).
    uint8_t hasIntrinsic = 0;
    // T-27: `img` source asset, resolved once from the `data-xt-asset` attribute at Document load
    // time (see document_load.cpp) so the Painter never re-parses it per frame. kInvalidAsset for
    // every element without that attribute (i.e. every non-`img` element).
    /// img source asset, resolved once from the data-xt-asset attribute at Document load
    /// time. kInvalidAsset for every element without that attribute.
    AssetId imageAsset = kInvalidAsset;
    /// If tag == "anim": instantiated scene, else kInvalidScene.
    SceneId scene = kInvalidScene;
    NodeId sceneRoot =
        kInvalidNode;  ///< root of instantiated scene subtree (Group node, child of this Element)
    // Docs/design/19-3d-space.md §19.4: when tag == "camera", the NodeKind::Camera child node
    // created for it at load time; kInvalidNode for every other element.
    /// When tag == "camera": the Camera child node created for it at load time (19.4).
    /// kInvalidNode for every other element.
    NodeId cameraNode = kInvalidNode;
    // §19.4/06.10 (19.10's revision to 06): CSS `perspective`, resolved to px by layout;
    // <= 0 means perspective: none. Computed in LayoutEngine::applyTransforms (T-47a).
    /// CSS `perspective`, resolved to px; <= 0 means `perspective: none` (06.10, 19.4).
    float perspectivePx = 0;
    /// CSS `perspective-origin`, resolved to px (06.10, 19.4).
    Vec2 perspectiveOriginPx{};
    /// Event listener linked list head (see section 08).
    uint32_t listenerHead = 0xFFFFFFFFu;
    /// Raw style attribute string (debug only).
    StringId inlineStyle = kInvalidString;
    /// Other attributes, name -> value.
    std::vector<std::pair<StringId, StringId>> attributes;
    // See Docs/design/02-ir-format.md §2.9 IrDocElement; parsed by StyleEngine::resolve (T-24).
    /// See Docs/design/02-ir-format.md section 2.9 IrDocElement; parsed by StyleEngine::resolve.
    uint32_t inlineDeclOffset = 0;
    /// Number of `IrDecl` entries at `inlineDeclOffset`.
    uint32_t inlineDeclCount = 0;
    // Index into Document's SceneInstance pool; only meaningful when scene != kInvalidScene.
    /// Index into Document's SceneInstance pool; only meaningful when scene != kInvalidScene.
    uint32_t sceneInstanceIndex = 0xFFFFFFFFu;
    // The autoplay-created Animator instance id for an `anim` element; 0 = none. See
    // Docs/tasks/T-18.md.
    /// The autoplay-created Animator instance id for an anim element; 0 = none.
    uint32_t defaultAnimation = 0;
    // T-24: true once StyleEngine::resolve has computed this element's style at least once (used
    // to suppress transitions on the very first resolve).
    /// True once StyleEngine::resolve has computed this element's style at least once (used
    /// to suppress transitions on the very first resolve).
    uint8_t styleResolvedOnce = 0;
    // T-24: Animator instance ids created for the element's current `animation` declaration list
    // (ComputedStyle::animationOffset/Count), in list order; stopped/replaced on the next diff.
    /// Animator instance ids created for the element's current animation declaration list
    /// (ComputedStyle::animationOffset/Count), in list order; stopped/replaced on the next diff.
    std::vector<uint32_t> cssAnimations;
    // T-35: the declarations set through ElementRef::setStyle(), at most one per prop, in the
    // order they were first set. Cascaded with inline specificity (1 << 24) right after the
    // element's IR inline decls, so a runtime declaration wins over the `style` attribute's.
    /// The declarations set through ElementRef::setStyle(), at most one per prop, in the
    /// order they were first set. Cascaded with inline specificity (1 << 24) right after the
    /// element's IR inline decls, so a runtime declaration wins over the style attribute's.
    std::vector<RuntimeStyleDecl> runtimeDecls;
    /// T-389 (27 §27.4.1): the last ElementRef::setStyle() text (64-bit hash and length) and its
    /// status, so the same text again is a no-op; 0 = none. Every other writer of runtimeDecls
    /// resets it.
    uint64_t setStyleHash = 0;
    uint32_t setStyleLength = 0;
    uint8_t setStyleStatus = 0;
    /// T-191 (03 §3.12.1): a `video` / `sequence` / `external` element's NodeKind::External
    /// child; kInvalidNode for every other element.
    NodeId externalNode = kInvalidNode;
    /// T-202 (03 §3.12.5): a `<particles>` element's NodeKind::Particles child; kInvalidNode for
    /// every other element.
    NodeId particlesNode = kInvalidNode;
    /// T-191: the part (0..1) of `imageAsset` drawn as the image -- an atlas sequence's current
    /// frame; the whole image everywhere else. object-fit applies within it. T-199: an asset
    /// packed into a bundle atlas (`atlas:<n>#<asset>`) starts at its atlas cell
    /// (AssetResolver::imageSubUv) at load time.
    Rect imageSubUv{0, 0, 1, 1};
    /// T-199: the atlas cell (0..1) of `style.backgroundImage`; the whole image when it is not
    /// atlased. Resolved by StyleEngine with the style.
    Rect backgroundSubUv{0, 0, 1, 1};
    /// T-327: `style.backgroundImage`'s size in pixels (its own cell when atlased), for
    /// `background-size` / `background-position`; (0, 0) when unknown. Resolved like
    /// backgroundSubUv.
    Vec2 backgroundImageSize{0, 0};
    /// T-199: the atlas cell (0..1) of `style.borderImage`, resolved like backgroundSubUv.
    Rect borderImageSubUv{0, 0, 1, 1};
    /// T-199: the source image's size in pixels (its own cell when atlased), for the nine-slice.
    Vec2 borderImageSize{0, 0};
    /// T-199: `style.borderImageSlice` resolved to source pixels (L, R, T, B): percentages against
    /// borderImageSize, `auto` from the resolver's manifest slice (AssetResolver::imageMeta), else 0.
    float borderImageSlicePx[4] = {0, 0, 0, 0};
    /// T-194 (08.9): bit p = local player p has the focus here (kStateFocus = any bit set);
    /// `:focus-player(p)` tests it.
    uint8_t focusPlayers = 0;
    /// T-336 (06 §6.9): a flex container's `order` cache, kept by LayoutEngine::layoutFlexChildren.
    /// `items` / `orders` are the in-flow items (document order) and their `order` values at the
    /// last sort, `permutation` the stable sort of them (indices into `items`). The sort runs again
    /// only when the items or an `order` change; all empty while every item has `order: 0`.
    struct FlexOrderCache {
        std::vector<NodeId> items;
        std::vector<float> orders;
        std::vector<uint32_t> permutation;
    } flexOrder;
};

/// T-336 (06 §6.5, 08 §8.4): the largest ScrollX of a laid-out box -- how far its in-flow content
/// reaches past the content box (0 when it does not overflow).
inline float maxScrollX(const LayoutBox& box) {
    const float extra = box.scrollWidth - box.contentBox().w;
    return extra > 0.0f ? extra : 0.0f;
}
/// maxScrollX() of the vertical axis.
inline float maxScrollY(const LayoutBox& box) {
    const float extra = box.scrollHeight - box.contentBox().h;
    return extra > 0.0f ? extra : 0.0f;
}
/// T-336: below this many px of overflow an `overflow: auto` axis counts as not overflowing.
inline constexpr float kOverflowEpsilon = 0.01f;
/// T-336 (05 §5.9.8): whether one axis is a live scroll axis -- `scroll`, or `auto` with content
/// overflowing on it. Its ScrollX / ScrollY offsets the children and the wheel moves it; `hidden`
/// and `visible` axes never apply an offset.
inline bool scrollsOnAxis(Overflow used, float maxScroll) {
    return used == Overflow::Scroll || (used == Overflow::Auto && maxScroll > kOverflowEpsilon);
}
/// T-336: true when the element clips its children to its padding box: an axis is `hidden`, or a
/// live scroll axis (scrollsOnAxis). The clip is always the whole padding box (CSS likewise).
inline bool clipsOverflow(const ElementData& data) {
    const Overflow x = usedOverflowX(data.style);
    const Overflow y = usedOverflowY(data.style);
    return x == Overflow::Hidden || y == Overflow::Hidden || scrollsOnAxis(x, maxScrollX(data.box)) ||
           scrollsOnAxis(y, maxScrollY(data.box));
}
/// T-336: the scroll offset the element applies to its children (0 on an axis that is not live).
inline Vec2 appliedScroll(const ElementData& data) {
    const bool x = scrollsOnAxis(usedOverflowX(data.style), maxScrollX(data.box));
    const bool y = scrollsOnAxis(usedOverflowY(data.style), maxScrollY(data.box));
    return Vec2{x ? data.style.base[prop::ScrollX].f : 0.0f, y ? data.style.base[prop::ScrollY].f : 0.0f};
}

}  // namespace xt
