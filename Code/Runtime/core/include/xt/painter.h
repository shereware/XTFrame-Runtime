#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/project3d.h"
#include "xt/shader_table.h"
#include "xt/types.h"

/// World transform update and the Painter (Group/Path/Image/Mask/Element). See
/// Docs/design/09-command-list.md §9.3-9.4, Docs/design/01-architecture.md §1.5 step 4,
/// Docs/tasks/T-10.md, Docs/tasks/T-27.md, Docs/design/19-3d-space.md §19.5-19.6 (T-44b).

namespace xt {

struct EditState;         // Code/Runtime/core/src/text_edit.h (private); only ever used through a pointer here.
struct CompositionState;  // Code/Runtime/core/src/composition.h (private); likewise.

// Code/Runtime/core/src/layer_collect.h (private); only ever used as std::vector element types below, with
// Painter's ctor/dtor defined out-of-line (painter.cpp, where layer_collect.h is included) --
// same forward-declaration trick as the nested `Action` type just below.
namespace layer_collect {
struct LayerEntry;
struct Section;
}  // namespace layer_collect

// Code/Runtime/core/src/shape_modifiers.h (private, T-225); same forward-declaration trick.
namespace modifiers {
struct RepeatCopy;
}  // namespace modifiers

/// Recomputes NodeCommon::local/world/worldOpacity for root's subtree (root included), by a
/// non-recursive preorder traversal. `force == true` recomputes everything, ignoring dirty bits.
/// See world_transform.cpp for the exact dirty-bit rule.
void updateWorldTransforms(NodeArena& arena, NodeId root, bool force);

/// `node`'s children in Painter draw order (Docs/tasks/T-27.md rule 6): every child, in document
/// order, except that `node`'s Element-kind children are stable-sorted among themselves by
/// (z-index, document order) -- ties keep document order since the sort is stable and the
/// (z-index, node) pairs are collected in document order. Non-Element children (Text, an `anim`'s
/// instantiated scene root Group) keep their original position untouched. `node` need not be an
/// Element itself (Group/Mask children never carry z-index, so their child list is returned
/// as-is). Shared by Painter::pushPlan and hit_test.cpp's hitTest (Docs/tasks/T-32.md "命中測
/// 試"): the array's last entry is the topmost (last-painted) child, which is exactly the order a
/// z-index-aware hit test needs to test children in.
void sortedElementChildren(const NodeArena& arena, NodeId node, std::vector<NodeId>& out);

/// Walks a scene-node subtree (Group/Path/Image/Mask/Element/Text) and appends CommandList
/// commands for it. See Docs/design/09-command-list.md §9.3-9.4, Docs/tasks/T-30.md.
class Painter {
public:
    /// Constructs a Painter with empty scratch buffers.
    Painter();
    // Declared (and defined out-of-line, in painter.cpp) so the private nested Action type only
    // needs to be complete there: std::vector<Action>'s destructor/copy/move would otherwise be
    // implicitly instantiated wherever a Painter is constructed or destroyed, which requires
    // Action to be complete at that call site.
    /// Destroys the Painter and releases its scratch buffers.
    ~Painter();
    /// Not copyable.
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    /// Appends to `out` (does not clear it). `arena`'s NodeCommon::world/worldOpacity must already
    /// be up to date (see updateWorldTransforms).
    void paint(const NodeArena& arena, NodeId root, CommandList& out);

    /// The document clock the caret's 1-second blink phase is taken from (Docs/tasks/T-33.md
    /// "Painter"); Document::paint() sets it every frame. 0 (the default) means "always visible".
    void setClock(float clock) { m_clock = clock; }
    /// T-225 (Docs/design/31-vector-authoring.md §31.6.1): the document time, in seconds, that a
    /// Wiggle modifier's noise is sampled at (04's clock); Document::paint() sets it every frame.
    /// 0 (the default) is a valid time: the output depends only on it, so scrubbing is stable.
    void setTime(float time) { m_time = time; }
    /// T-225b (21.1 `modifierVertices`): on-curve points of the modified groups' output in the
    /// last paint() (a command-duplicating repeat counts each copy); reset by every paint().
    uint32_t modifierVertices() const { return m_modifierVertices; }
    /// T-327 (05 §5.9.8): true when the last paint() drew some element's `background-repeat`
    /// stretched because it needed more than 4096 tiles; reset by every paint().
    bool backgroundTilesCapped() const { return m_backgroundTilesCapped; }

    /// T-225: the paint (fill, stroke, stops, dashes) of a Path or Shape node; defined in
    /// painter.cpp, only used by the Painter's own emit functions.
    struct PaintSource;
    /// Borrows the Document's EditState pool, the source of the caret/selection offsets for an
    /// editable `input`'s Text node (TextNodeData::editIndex). nullptr (the default) means no
    /// element is editable, which is what a Document-less test Painter wants.
    void setEditStates(const std::vector<EditState>* editStates) { m_editStates = editStates; }
    /// Borrows the Document's CompositionState (T-33b): the composition underline, the IME
    /// clause highlight and the in-composition caret offset come from it. nullptr (the
    /// default) means "never composing".
    void setComposition(const CompositionState* composition) { m_composition = composition; }
    /// T-178: the IR the painted document came from (rich-text run styles and `text-effect`
    /// entries live in it). nullptr (the default) paints text runs in the element's style with no
    /// text effects.
    void setIr(const IrView* ir) { m_ir = ir; }
    /// T-203 (18.7): the document's custom effect shaders. Every enabled Custom effect a frame
    /// paints gets its ShaderRef copied into the CommandList (once per asset) and its boundsHint
    /// expanded by customEffectExpand(). nullptr (the default) = no shaders: Custom effects still
    /// reach the list, and backends find no ShaderRef for them (they draw the content unfiltered).
    void setShaders(const ShaderTable* shaders) { m_shaders = shaders; }
    /// T-263d: the index in CommandList::sources of the IR the painted nodes' assets belong to.
    /// paint() stamps every asset-carrying command it appends (DrawImage, DrawParticles,
    /// DrawImageMesh, PushEffect) and every ShaderRef it copies with it. 0 (the default) is right
    /// for a list that names no source; Context::render sets each Document's index.
    void setSource(uint16_t source) { m_source = source; }
    /// T-194 (27.7 `drag-ghost`): while `source` is being dragged, paint() draws a translucent
    /// copy of its subtree after everything else (overlays included), translated by `offset`.
    /// kInvalidNode (the default) draws no ghost.
    void setDragGhost(NodeId source, Vec2 offset) {
        m_ghostSource = source;
        m_ghostOffset = offset;
    }
    /// T-395 (08 §8.16): the `--placeholder-color` token's value, copied; nullptr (the default)
    /// draws an `<input placeholder>` in the element's `color` at 50 % of its alpha.
    void setPlaceholderColor(const Color* color) {
        m_hasPlaceholderColor = color != nullptr;
        m_placeholderColor = color != nullptr ? *color : Color{};
    }

private:
    // Per-node count of "drawable items" (a Path that fills or strokes, or an Image) in the
    // subtree rooted at that node (root included). Indexed by NodeId; recomputed at the start of
    // every paint() call (capacity retained across calls).
    std::vector<uint32_t> m_drawableCount;

    // Explicit work-item stack used to drive traversal without C++ recursion (capacity retained
    // across calls). Declared here (private state) rather than as a local, per the T-10 spec.
    struct Action;
    std::vector<Action> m_stack;

    // Scratch buffers for a Path's trimmed verbs/points, reused across emitPath() calls within one
    // paint() (and across calls) to avoid per-path allocation.
    std::vector<PathVerb> m_trimVerbsScratch;
    std::vector<float> m_trimPointsScratch;

    // T-233: scratch of emitVariableStroke() (flattened input, strokeToFill output).
    std::vector<Vec2> m_vsFlatPoints;
    std::vector<uint32_t> m_vsFlatStarts;
    std::vector<uint8_t> m_vsFlatClosed;
    std::vector<Vec2> m_vsOutPoints;
    std::vector<uint32_t> m_vsOutStarts;

    // Scratch buffers used by pushPlan() to build one Group/Mask/Element node's child action plan.
    // Each pushPlan() call fully consumes and pushes its plan onto m_stack before returning, so
    // reuse across calls (including calls nested via later m_stack pops) is safe.
    std::vector<NodeId> m_childScratch;
    std::vector<NodeId> m_maskRunScratch;
    std::vector<Action> m_planScratch;

    // T-27: scratch for one Element's background/border rounded-rect path, reused (cleared) across
    // emitElementBackground()/emitElementBorder() calls within and across paint().
    std::vector<PathVerb> m_elemVerbsScratch;
    std::vector<float> m_elemPointsScratch;

    // T-225 (31 §31.6.1): collect-apply-draw scratch of one modified group (emitModifiedGroup):
    // the collected paths' geometry (and a second buffer each modifier writes into before the
    // two swap), one ModPath per collected path, a pending command-duplicating repeat, the draw
    // order and the per-path PathRefs. Refilled per group; capacity kept across calls.
    struct ModPath;
    std::vector<PathVerb> m_modVerbs;
    std::vector<float> m_modPoints;
    std::vector<PathVerb> m_modVerbsNext;
    std::vector<float> m_modPointsNext;
    std::vector<ModPath> m_modPaths;
    std::vector<ModPath> m_modPathsNext;
    std::vector<modifiers::RepeatCopy> m_repeatCopies;
    std::vector<modifiers::RepeatCopy> m_pendingCopies;
    std::vector<uint32_t> m_modOrder;
    std::vector<PathRef> m_modRefs;
    // T-225: a Shape node's generated path (emitShape and the collector), before trim.
    std::vector<PathVerb> m_shapeVerbsScratch;
    std::vector<float> m_shapePointsScratch;
    // T-225: see setTime().
    float m_time = 0;
    // T-225b: see modifierVertices().
    uint32_t m_modifierVertices = 0;
    // T-327: see backgroundTilesCapped().
    bool m_backgroundTilesCapped = false;

    // T-33: caret blink phase and the editable-value pool (see setClock/setEditStates).
    float m_clock = 0;
    const std::vector<EditState>* m_editStates = nullptr;
    const CompositionState* m_composition = nullptr;
    // T-178: see setIr().
    const IrView* m_ir = nullptr;
    // T-203: see setShaders().
    const ShaderTable* m_shaders = nullptr;
    // T-263d: see setSource().
    uint16_t m_source = 0;
    // T-395: see setPlaceholderColor().
    bool m_hasPlaceholderColor = false;
    Color m_placeholderColor{};

    // T-44b (Docs/design/19-3d-space.md §19.5-19.6): per-node "subtree contains an is3D node"
    // cache, indexed by NodeId like m_drawableCount, recomputed every paint() call.
    std::vector<uint8_t> m_has3D;
    // Scratch for one sorted segment's collected 3D layers (flushSection()); cleared and refilled
    // per segment, capacity retained across segments and across paint() calls.
    std::vector<layer_collect::LayerEntry> m_layerScratch;
    // Scratch for one 3D space root's direct-children sectioning (emitRoot()); refilled per root.
    std::vector<layer_collect::Section> m_sectionScratch;
    // T-105: projectionBoundsHint()'s explicit walk stack -- each entry is a pending node plus the
    // plane (its nearest enclosing 3D layer's world3, identity on the root's own plane) that the
    // node's `world` is expressed in. Refilled per root.
    std::vector<std::pair<NodeId, Mat4>> m_boundsStack;
    // The mapping stack (19.7's "plane to screen" stack), one entry per currently-open
    // PushProjection; used by isBackface() while flushSection() decides a layer's draw order (not
    // by the deferred command emission, which happens later via m_stack -- see painter_3d.cpp).
    std::vector<project3d::Mapping> m_mappingStack;
    // The 3D layer currently being entered via an EnterNodeLayer action (painter_3d.cpp);
    // kInvalidNode outside of one (including for EnterNode2D actions -- see enterNode()'s new
    // is3D-skip branch, which only lets `n == m_currentLayer` through).
    NodeId m_currentLayer = kInvalidNode;
    // T-194 (08.9 `layer: overlay`): overlay subtree roots met during the main traversal, with
    // the alpha they inherit (their parent's world opacity), painted after the root in this order;
    // m_overlayEntering is the overlay root being entered by that pass (let through enterNode()).
    std::vector<std::pair<NodeId, float>> m_overlayQueue;
    NodeId m_overlayEntering = kInvalidNode;
    NodeId m_paintRoot = kInvalidNode;
    NodeId m_ghostSource = kInvalidNode;
    Vec2 m_ghostOffset{};

    /// Pops and executes m_stack until it is empty (paint()'s main loop).
    void runStack(const NodeArena& arena, CommandList& out);
    /// T-194: the overlay pass and the drag ghost, after the main traversal.
    void paintOverlays(const NodeArena& arena, CommandList& out);
    void paintGhost(const NodeArena& arena, CommandList& out);

    void computeDrawableCount(const NodeArena& arena, NodeId root);
    /// `elementChildrenOnly` drops the non-Element children from the plan (T-119: a
    /// `visibility: hidden` Element's scene/Text children, which have no ComputedStyle of
    /// their own and so never inherit the hidden state). `skipShapes` (T-225) drops the
    /// Path/Shape/Modifier children, which emitModifiedGroup() has already drawn.
    void pushPlan(const NodeArena& arena, NodeId group, float inheritedAlpha,
                  bool elementChildrenOnly = false, bool skipShapes = false);
    void enterNode(const NodeArena& arena, NodeId n, float inheritedAlpha, CommandList& out);
    void emitPath(const NodeArena& arena, NodeId n, float alpha, CommandList& out);
    // T-244 (32 §32.3): a skinned Path: its skinned points (NodeArena::skinned()) drawn with its
    // parent's world (its own transform counts as identity). False when the path is not skinned.
    bool emitSkinnedPath(const NodeArena& arena, NodeId n, float alpha, CommandList& out);
    // T-225: a Shape node outside a modified group: its generated path, drawn like emitPath().
    void emitShape(const NodeArena& arena, NodeId n, float alpha, CommandList& out);
    // T-225 (31 31.6.1): a Group with Modifier children -- collect its direct Path/Shape children
    // (per-path trim applied), apply each enabled Modifier in child order to everything collected
    // before it, draw the result (FillPath/StrokePath per path; a repeat with no later modifier
    // as repeated commands sharing one PathRef), then plan the other children (nested groups
    // etc.) after it, like pushPlan().
    void emitModifiedGroup(const NodeArena& arena, NodeId group, float alpha, CommandList& out);
    // T-225: the command half of emitPath(): the FillPath/StrokePath commands of `paint` over the
    // already-appended geometry `path`, at `xf`, with `alpha` folded into the paint.
    void emitPathCommands(const PathRef& path, const PaintSource& paint, const Mat2x3& xf, float alpha,
                          CommandList& out);
    // T-233 (31 31.4.7): a tapered / width-profiled stroke of `path`, converted by strokeToFill (width
    // function) into a NonZero FillPath with the stroke paint; backends never see it as a stroke.
    void emitVariableStroke(const PathRef& path, const PaintSource& paint, const Mat2x3& xf, float alpha,
                            CommandList& out);
    void emitImage(const NodeArena& arena, NodeId n, float alpha, CommandList& out);
    // T-245 (32 32.2.4): one DrawImageMesh of a Mesh node (positions x world, uvs, indices).
    void emitMesh(const NodeArena& arena, NodeId n, float alpha, CommandList& out);
    void paintElement(const NodeArena& arena, NodeId n, float alpha, CommandList& out);
    void paintText(const NodeArena& arena, NodeId n, float alpha, CommandList& out);

    // T-44b: defined in painter_3d.cpp. See Docs/design/19-3d-space.md §19.5-19.6.
    //
    // Enters a 3D space root (NodeCommon::isSpaceRoot): pushes PushProjection/the mapping-stack
    // entry immediately, emits root's own background/border/image (Element) without recursing
    // into its children via the normal pushPlan, then appends the root's children -- sectioned
    // into plain-2D and sorted-3D runs (layer_collect::collectSections) -- and a trailing
    // PopProjection to m_planScratch, finally pushing it (reversed) onto m_stack like pushPlan().
    void emitRoot(const NodeArena& arena, NodeId root, float inheritedAlpha, CommandList& out);
    // Appends one sorted segment's quanta (§19.6 step 3) to m_planScratch, in draw order: the
    // segment's own-plane 2D content first (EnterNode2D, one quantum per non-3D-layer child),
    // then its collected/sorted/backface-culled 3D layers (EnterNodeLayer, one quantum per
    // surviving layer, each wrapped in its own PushTransform3D{world3}/PopTransform3D pair).
    // `view` is the root's CameraSetup::view (19.4 step 2), for §19.6's depth formula.
    void flushSection(const NodeArena& arena, std::span<const NodeId> orderedChildren,
                      const layer_collect::Section& section, const Mat4& view, float alpha);
    // T-105 (Docs/design/19-3d-space.md §19.6 step 1): ProjectionCmd::boundsHint for the space
    // root `root` -- the screen AABB of every node in its subtree, each node's own shape corners
    // projected through m_mappingStack (whose top must already be this root's entry), outset by
    // the largest per-node effect expansion in the subtree and clipped to `viewport`. Falls back
    // to `viewport` when a corner fails to project (w <= 1e-6), when the subtree holds a nested
    // space root, or when no node in it has a shape.
    Rect projectionBoundsHint(const NodeArena& arena, NodeId root, Rect viewport);

    // T-52 (Docs/design/18-effects.md §18.1, §18.5): emits an immediate PushEffect command for
    // node `n` (which must have at least one enabled effect and non-empty drawable content --
    // enterNode() checks both before calling). Copies `n`'s enabled effects (and any GradientMask
    // stops) into `out.effects`/`out.stops`, computes EffectCmd::xf (c.world, or -- inside a 3D
    // space root's subtree -- project3d::jacobianAt) and boundsHint/contentBounds from
    // subtreeBounds(). Does not push the matching Action::Kind::PopEffect; the caller does that.
    void pushEffectCmd(const NodeArena& arena, NodeId n, CommandList& out);

    // T-133 (Docs/design/09-command-list.md's LayerCmd line): `n`'s subtreeBounds() mapped to
    // screen space -- `transformBounds(xf, ...)` outside any 3D space root, the AABB of the four
    // projected corners inside one (`xf` is then unused; every corner failing to project falls
    // back to `viewport`, the conservative choice §19.7 makes elsewhere). Not clamped to the
    // viewport; both callers (pushEffectCmd's contentBounds, enterNode's LayerCmd::boundsHint) do
    // their own clamping after their own expansion.
    Rect contentScreenBounds(const NodeArena& arena, NodeId n, const Mat2x3& xf, Rect viewport);
};

}  // namespace xt
