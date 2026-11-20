#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "xt/dirty_cause.h"
#include "xt/thread_clock.h"

/// Built-in per-frame performance tracing. See Docs/design/21-perf-tracing.md §21.1-21.2.
///
/// Tracing is OFF by default: with `enabled() == false` a `PerfScope` costs exactly one branch on
/// construction and one on destruction, and never reads the clock. The core never prints anything;
/// presenting the numbers is the host's job (§21.3).

namespace xt {

/// One frame's measurements (§21.1). POD: every field is a float or a uint32_t, so a FrameStats can
/// be copied, zeroed and written to a CSV row without any special handling.
struct FrameStats {
    // --- Identity (§21.1: "frameIndex/timeSeconds 隨每筆記錄"). ---
    /// 0-based index of this frame since the last reset().
    uint32_t frameIndex = 0;
    /// The Context clock (seconds) at the frame's beginFrame().
    float timeSeconds = 0;

    // --- Times, in milliseconds (§21.1 group 1). ---
    /// The whole Context::update()/Document::update() pass.
    float updateMs = 0;
    /// Input drain + dispatch, inside update.
    float inputMs = 0;
    /// Style resolve, inside update.
    float styleMs = 0;
    /// Layout (including the camera-default refresh), inside update.
    float layoutMs = 0;
    /// Animator evaluation, inside update.
    float animateMs = 0;
    /// World transform update, inside update.
    float transformMs = 0;
    /// Document::paint(), i.e. building the command list.
    float paintMs = 0;
    /// The whole backend render() call (§21.2; GL reports CPU submission time -- it never calls
    /// glFinish, so the GPU may still be working when this stops).
    float renderMs = 0;
    /// Time spent inside the backend's PopEffect handling.
    float effectsMs = 0;
    /// Time spent inside the backend's DrawGlyphs handling.
    float textMs = 0;
    /// Dirty-rectangle computation inside Context::render() (Docs/design/23-host-integration.md
    /// §23.2); 0 on frames rendered without a DamageList.
    float damageMs = 0;
    /// The host's signal-graph tick (`xt::flow::FlowRuntime::tick()`), which runs between
    /// Context::update() and Context::render() and is filled by the host, not by the core
    /// (Docs/design/21-perf-tracing.md §21.1; added for Docs/tasks/T-124.md's measurements). 0 in
    /// a run without a flow graph.
    float flowMs = 0;
    /// T-475 (21 §21.6, 27 §27.4.2 item 6): the flow stages, filled by a FlowRuntime that
    /// FlowRuntime::setPerfTracker() gave this tracker. Exclusive times (a nested stage pauses the
    /// outer one), taken wherever the stage runs: inside FlowRuntime::tick() (a sub-phase of
    /// flowMs) and, for the outputs a Document delivers at once (events, and every output below
    /// behavior 1), inside the input dispatch (then part of inputMs).
    /// Delivering document outputs along their edges (deliverPendingSignals(), the delivery part
    /// of flushSignals(), an output delivered at once).
    float flowDeliverMs = 0;
    /// Node evaluation (settleNodes() / evaluateNode()).
    float flowEvalMs = 0;
    /// Writes into the Document (applyDocumentInput(): setText, setStyle, set(port), ...).
    float flowSinkMs = 0;
    /// pollDocumentOutputs(): the polled `#document.*` and `<anim>` `localTime` ports.
    float flowPollMs = 0;

    // --- SW backend breakdown (Docs/tasks/T-82.md step 1). Sub-phases of renderMs; the four `swFx*`
    // ones are also sub-phases of effectsMs. Only the SW backend fills these in; the GL backend
    // leaves them at 0, so a GL run simply shows zeros for the whole group.
    /// Path flattening, dashing, stroke-to-fill and 3D point projection.
    float swPathMs = 0;
    /// Rasterizer edge accumulation (reset + addLine).
    float swRasterMs = 0;
    /// Rasterizer sweep plus the per-pixel paint evaluation and blending it drives.
    float swSpanMs = 0;
    /// Sub-phase of swSpanMs: spans of a Solid paint (every glyph outline included).
    float swSpanSolidMs = 0;
    /// Sub-phase of swSpanMs: spans of a Linear/Radial gradient paint.
    float swSpanGradientMs = 0;
    /// DrawImage/DrawImageQuad per-pixel sampling and blending. A *sibling* of swSpanMs, not a
    /// sub-phase of it: image draws never run the rasterizer sweep swSpanMs covers, so folding
    /// them in would break the comparison with pre-T-128 numbers (Docs/tasks/T-128.md step 1).
    float swSpanImageMs = 0;
    /// Span time spent with a clip mask (or a partial-redraw write mask) active. This *overlaps*
    /// swSpanSolidMs/swSpanGradientMs: it splits the same time a second way, by clip state, so
    /// the three never sum to swSpanMs (Docs/tasks/T-128.md step 1).
    float swSpanClippedMs = 0;
    /// Clip mask rasterization and clip-stack combine.
    float swClipMs = 0;
    /// Layer/mask/effect offscreen buffer acquire, clear and composite.
    float swLayerMs = 0;
    /// Gaussian blur passes inside the effect stack.
    float swFxBlurMs = 0;
    /// Dilate (max filter) passes inside the effect stack.
    float swFxDilateMs = 0;
    /// Signed distance transform inside the effect stack.
    float swFxSdfMs = 0;
    /// Per-pixel effect composition loops (color matrix, gradient mask, shine, shadow compose).
    float swFxPixMs = 0;
    /// T-450 (21 §21.2): the editor's synchronous `documentChanged()` fan-out (the listeners that
    /// still run inside the edit). Filled by the editor host, not by the core; 0 elsewhere.
    float editorDocChangedMs = 0;
    /// T-450: the editor's deferred, coalesced panel refresh (run after the preview has drawn the
    /// edit). Filled by the editor host, not by the core; 0 elsewhere.
    float editorPanelsMs = 0;
    /// `xt::gpu::FrameBuilder::build` (Docs/design/10-backends.md §10.5): CommandList -> GpuFrame
    /// CPU time. 0 on frames rendered by another backend.
    float gpuBuildMs = 0;

    // --- Workload counts (§21.1 group 2). ---
    /// Elements whose style was re-resolved this frame.
    uint32_t styledElements = 0;
    /// Elements laid out this frame.
    uint32_t laidOutElements = 0;
    /// Animator instances still active at the end of update.
    uint32_t activeAnimations = 0;
    /// Transitions still active at the end of update.
    uint32_t activeTransitions = 0;
    /// CommandList::commands.size().
    uint32_t commands = 0;
    /// CommandList::layers.size() (PushLayer payloads).
    uint32_t layers = 0;
    /// CommandList::effectCmds.size() (PushEffect payloads).
    uint32_t effectLayers = 0;
    /// CommandList::projections.size() (PushProjection payloads).
    uint32_t projections = 0;
    /// CommandList::glyphs.size().
    uint32_t glyphs = 0;
    /// Number of path points in CommandList::points (which holds two floats per point).
    uint32_t pathPoints = 0;
    /// T-225b (Docs/design/31-vector-authoring.md §31.6.1): on-curve points of every shape
    /// modifier's output geometry this frame (a command-duplicating repeat counts each copy).
    uint32_t modifierVertices = 0;
    /// T-244 (Docs/design/32-rigging-and-deformation.md §32.3): points re-skinned this frame (a
    /// Path's p / in / out points and a Mesh's positions; 0 on a frame where nothing moved).
    uint32_t skinnedPoints = 0;
    /// T-244: skeleton bones evaluated this frame.
    uint32_t rigBones = 0;
    /// DrawImage + DrawImageQuad payload count.
    uint32_t imageDraws = 0;
    /// T-245 (Docs/design/32-rigging-and-deformation.md §32.3): DrawImageMesh payload count.
    uint32_t imageMeshes = 0;
    /// T-245 (32 §32.3 `meshTriangles/frame`): triangles of every DrawImageMesh command.
    uint32_t meshTriangles = 0;
    /// Glyphs the SW backend drew through the small-size bitmap path (T-101). GL leaves it at 0.
    uint32_t swGlyphBitmaps = 0;
    /// Glyphs the SW backend drew through the outline path (T-101). GL leaves it at 0.
    uint32_t swGlyphOutlines = 0;
    /// Coverage spans the SW backend painted this frame (Docs/tasks/T-128.md step 1): one per
    /// rasterizer sweep span that survived the destination-rectangle clip. GL leaves it at 0.
    uint32_t spansPerFrame = 0;
    /// Pixels those spans covered, summed (a pixel two spans touch counts twice). GL leaves it
    /// at 0.
    uint32_t spanPixelsPerFrame = 0;

    // --- Memory (§21.1 group 3). ---
    /// Live NodeArena nodes across every loaded Document.
    uint32_t arenaNodes = 0;
    /// Sum of size x sizeof(element) over every CommandList vector.
    uint32_t commandBytes = 0;
    /// T-270d: strings in the Documents' runtime string pools (Document::intern()), summed. The
    /// pool only grows, so its growth shows here.
    uint32_t runtimeStrings = 0;
    /// T-270d: the largest number of distinct new runtime strings any one binding or port has
    /// produced so far (the count the 1024-string guard warns past), over every Document.
    uint32_t maxDistinctBindingStrings = 0;

    // --- T-343 Direct target mode (Docs/design/23-host-integration.md §23.aa, 21 §21.1). Filled
    // by the backend that drew the frame (xt_gpu's FrameBuilder, xt_sw); 0 in Layer mode. ---
    /// Bands the paint order was split into (§23.aa.2); 0 when not in Direct mode.
    uint32_t bands = 0;
    /// Bands drawn again this frame (their damage was not empty); the others were pasted from
    /// their cached band targets.
    uint32_t bandRedraws = 0;
    /// Scene pixels copied for the scene readers this frame (region-limited backdrop copies,
    /// §23.aa.3), summed over every copy.
    uint32_t backdropCopyPx = 0;
    /// Levels of the shared backdrop blur pyramid built this frame (18 §18.15; 0 = no backdrop
    /// blur, or its result came from the cache).
    uint32_t pyramidLevels = 0;
    /// 1 when the band split was refused this frame (more than the band cap, or the band areas
    /// exceed twice the surface) and the whole UI was drawn directly; 0 otherwise.
    uint32_t directFallback = 0;

    /// T-389 (27 §27.4.1): ElementRef::setStyle() calls this frame (a flow / host / binding
    /// writing styles), counted while the tracker is enabled.
    uint32_t setStyleCalls = 0;

    // --- T-475 (21 §21.6): flow workload, counted by a FlowRuntime with this tracker
    // (FlowRuntime::setPerfTracker()) over the whole frame, the input dispatch included. ---
    /// Document outputs entering the flow (FlowRuntime's signalOutput() path).
    uint32_t flowSignals = 0;
    /// Of those, value signals that replaced a value already pending for the same port (coalesced
    /// away, 27 §27.4.1).
    uint32_t flowSignalsCoalesced = 0;
    /// evaluateNode() calls.
    uint32_t flowNodeEvals = 0;
    /// applyDocumentInput() calls (writes into a Document).
    uint32_t flowSinkWrites = 0;
    /// Edges fanOut() delivered along.
    uint32_t flowFanOutEdges = 0;
    /// T-477 (27 §27.4.2 item 4): the `set(port)` and `setStyle` sink writes that went in typed
    /// (Document::setPort() with a Value / ElementRef::setStyleTemplate()) and the ones that went in
    /// as text (setPortText() / setStyle()). The other sinks are counted in neither.
    uint32_t flowSinkTyped = 0;
    uint32_t flowSinkText = 0;
    /// Heap allocations inside FlowRuntime::tick() (xt::perf::AllocCounter), sink writes into the
    /// Document included except a `setStyle` that goes in as text (its CSS parse; T-478); 0 unless
    /// the executable replaces operator new with XT_PERF_ALLOC_COUNT (xt_perf_flow,
    /// xt_flow_alloc_tests).
    uint32_t flowAllocs = 0;

    /// T-717 (10 §10.16 W4): Animator instances whose tracks were evaluated this frame. Smaller than
    /// activeAnimations (the pool size) by the `@timeline` members held at their end stop, which
    /// are written once and then left alone, and by the instances still in their delay.
    uint32_t animEvaluated = 0;

    /// T-737 (Docs/tasks/T-737.md A item 6): the Painters' dashed / dotted border geometry caches,
    /// summed over the Documents painted this frame: sides drawn from the cache, sides computed (and
    /// stored), and the entries and bytes of path data held at the end of the paint.
    uint32_t borderDashHits = 0;
    uint32_t borderDashMisses = 0;
    uint32_t borderDashEntries = 0;
    uint32_t borderDashBytes = 0;

    /// T-754 (Docs/design/10-backends.md §10.20): effects and layers left out because their known
    /// bounds lie wholly outside the viewport -- by the Painter (CommandList::culledOffscreen) and
    /// by the SW backend (a known, empty hint in a list it was handed).
    uint32_t culledOffscreen = 0;

    /// T-747 (Docs/design/57-measurement.md §57.3 item 4): the paint + damage time of this frame's
    /// Context::render() spent on Documents whose visual generation had not moved since their last
    /// paint while that paint's list held nothing clock-dependent (CommandList::clockDependent): work
    /// that changed nothing. Paint still runs (23 §23.ag skips it later); a static screen shows its
    /// whole paint time here.
    float idlePaintMs = 0;
    /// T-747 (57 §57.3 item 2): this frame's invalidating writes, per DirtyCause (row) and kind
    /// (column: kDirtyKindStyle / Layout / Transform / Paint). One count per write and kind, however
    /// far the write propagates. Counted while the tracker is enabled; zeroed by beginFrame() (the top
    /// of Context::update()), so writes made between render() and the next update() land in the frame
    /// before.
    uint32_t causes[kDirtyCauseCount][kDirtyKindCount] = {};
    /// T-747: at detail level >= 1 (PerfTracker::setDetail), per cause, the last up to
    /// kDirtyCauseNodeSlots distinct nodes written (most recent first); kInvalidNode = empty slot
    /// (beginFrame() fills them so). Not written at detail level 0. Node ids of whichever Document
    /// wrote them.
    uint32_t causeNodes[kDirtyCauseCount][kDirtyCauseNodeSlots] = {};

    /// T-748 (Docs/design/23-host-integration.md §23.ag item 3): the Documents of this frame's
    /// Context::render() that met every static-skip condition (Context::setStaticSkipEnabled) --
    /// their previous commands were reused instead of painted. In the verify mode
    /// (Context::setVerifyStaticSkip) they are painted anyway and still counted here.
    uint32_t paintSkipped = 0;
    /// T-748 (23 §23.ag item 4): the Documents whose fresh paint differed from their previous one
    /// although their visual generation had not moved (the verify mode's finding: a write site that
    /// missed its DirtyCause hook). Always 0 outside the verify mode.
    uint32_t staticSkipMismatch = 0;
    /// T-776 (Docs/design/58-value-functions-and-drivers.md §58.5): timeline expression programs run
    /// this frame. 0 while every timeline with programs is paused, held or released and nothing they
    /// read changed.
    uint32_t exprEvaluated = 0;
    /// T-755 (Docs/design/06-layout.md §6.x item 3): the subtrees partial layout passes laid out
    /// again this frame (each layout root once per pass; laidOutElements counts their elements).
    uint32_t layoutRoots = 0;
    /// T-755: the full layout passes of this frame (a write with no layout boundary above it, a
    /// structural change above every boundary, a viewport or length-environment change...).
    uint32_t layoutFullPasses = 0;
    /// T-755 (06 §6.x item 5): partial layout passes whose result differed from the full pass the
    /// verify mode (LayoutEngine::setVerifyPartial) ran after them. Always 0 outside the verify mode.
    uint32_t partialLayoutMismatch = 0;

    // --- T-750 (Docs/design/57-measurement.md §57.2, §57.5): what FrameRecord v1 (xt/perf_record.h)
    // needs beyond the fields above. Not in the XT_FRAME_STATS_* lists (summary() does not average
    // them; makeFrameRecord() reads them). ---
    /// Thread CPU milliseconds of the top-level phases, taken over the same scopes as their wall
    /// times (PerfPhaseScope) while PerfTracker::setThreadCpuTiming(true); 0 otherwise. Like the wall
    /// times they nest as the scopes do: updateCpuMs includes inputCpuMs.
    float inputCpuMs = 0;
    float updateCpuMs = 0;
    float flowCpuMs = 0;
    float paintCpuMs = 0;
    float damageCpuMs = 0;
    /// gpuBuildMs's CPU time (the SW backend: its whole render() call).
    float buildCpuMs = 0;
    float submitCpuMs = 0;
    float waitCpuMs = 0;
    /// The backend's submission inside renderMs (xt_wgpu: WgpuDevice::submit; xt_gl: GlDevice::submit),
    /// and its split as the backend reports it (xt_wgpu: plan / upload / encode / queue; xt_gl: target
    /// acquisition / upload / draws + pool release, queue 0). 0 for the SW backend.
    float submitMs = 0;
    float submitPlanMs = 0;
    float submitUploadMs = 0;
    float submitEncodeMs = 0;
    float submitQueueMs = 0;
    /// Waits the host times around its own calls (the in-flight frame limit, a GPU / queue wait) with
    /// PerfPhaseScope(&FrameStats::waitMs, &FrameStats::waitCpuMs).
    float waitMs = 0;
    /// PerfTracker::markFrameStart() .. markFrameEnd(): the whole frame as the host measured it, wall
    /// and thread CPU (the CPU one only with setThreadCpuTiming); both 0 and frameMarked 0 when the
    /// host did not mark the frame.
    float frameWallMs = 0;
    float frameCpuMs = 0;
    uint32_t frameMarked = 0;
    /// The backend that drew the frame (PerfBackend: 0 none, 1 sw, 2 gl, 3 wgpu).
    uint32_t backend = 0;
    /// Backend counters of the frame (0 for a backend without them): render passes, draw calls,
    /// pipeline / program binds, geometry and uniform bytes uploaded (saturated at 2^32 - 1),
    /// transient targets created, 1 when the previous frame was submitted again without a build.
    uint32_t passes = 0;
    uint32_t drawCalls = 0;
    uint32_t pipelineBinds = 0;
    /// Texture bind groups created (xt_wgpu; 0 elsewhere).
    uint32_t bindGroups = 0;
    uint32_t uploadBytes = 0;
    uint32_t transientCreates = 0;
    uint32_t frameReused = 0;
    /// gpu::FrameBuilder's effect result cache and coverage atlas of the frame's build.
    uint32_t effectCacheHits = 0;
    uint32_t effectCacheMisses = 0;
    uint32_t coverageHits = 0;
    uint32_t coverageMisses = 0;
    /// Memory after the frame: the effect result cache's target bytes, the backend's transient
    /// target pool bytes (saturated at 2^32 - 1).
    uint32_t effectCacheBytes = 0;
    uint32_t transientPoolBytes = 0;

    // --- T-753 (Docs/design/57-measurement.md §57.3 item 4): work done that changed nothing. Counted
    // only at detail level >= 1 (PerfTracker::setDetail); 0 otherwise. ---
    /// `redundantWork`: elements re-styled this frame whose computed style equals, field by field, the
    /// one they had (a first resolve is not counted).
    uint32_t redundantStyled = 0;
    /// `redundantWork`: elements of the subtrees laid out this frame (each once, however many layout
    /// calls measured it) whose box -- position and size -- is the one it had before the pass.
    uint32_t redundantLaidOut = 0;
    /// `redundantWork`: commands of a painted list that the damage diff matched with an identical
    /// command of the previous list (Context::render with a DamageList; out of FrameStats::commands).
    uint32_t redundantCommands = 0;
    /// `repaintedUnchanged`: effect segments and layers gpu::FrameBuilder drew in full although the
    /// hash of their commands is one of the previous build's, by why the result was not reused: not
    /// cacheable (layers, backdrop readers, external textures, custom shaders, unknown bounds, a device
    /// without the cache, an identity stack), the cache key changed (clip, window, position or phase),
    /// the cache was full, or the key's first repeat (stored now; the cache keeps a result only on the
    /// second consecutive sighting).
    uint32_t repaintedUnchangedNotCacheable = 0;
    uint32_t repaintedUnchangedKeyChanged = 0;
    uint32_t repaintedUnchangedBudget = 0;
    uint32_t repaintedUnchangedFirstSight = 0;
};

/// T-750: FrameStats::backend.
enum class PerfBackend : uint32_t { None = 0, Sw = 1, Gl = 2, Wgpu = 3 };

/// T-750: `v` saturated into a uint32_t counter.
inline uint32_t saturateU32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }

/// T-747: the total count of `cause` in `stats.causes` over the four kinds.
inline uint32_t causeTotal(const FrameStats& stats, DirtyCause cause) {
    const uint32_t c = static_cast<uint32_t>(cause);
    uint32_t sum = 0;
    for (uint32_t k = 0; k < kDirtyKindCount; ++k) {
        sum += stats.causes[c][k];
    }
    return sum;
}

// clang-format off
/// Every float field of FrameStats, in declaration order, as `X(name)`. Lets a host iterate the
/// fields (CSV columns, a summary table) without repeating the list; not part of §21.2's API.
#define XT_FRAME_STATS_FLOAT_FIELDS(X)                                                                 \
    X(timeSeconds)                                                                                     \
    X(updateMs)                                                                                        \
    X(inputMs)                                                                                         \
    X(styleMs)                                                                                         \
    X(layoutMs) X(animateMs) X(transformMs) X(paintMs) X(renderMs) X(effectsMs) X(textMs) X(damageMs)  \
        X(flowMs) X(flowDeliverMs) X(flowEvalMs) X(flowSinkMs) X(flowPollMs) X(swPathMs) X(swRasterMs) X(swSpanMs) X(swSpanSolidMs) X(swSpanGradientMs)           \
            X(swSpanImageMs) X(swSpanClippedMs) X(swClipMs) X(swLayerMs) X(swFxBlurMs) X(swFxDilateMs) \
                X(swFxSdfMs) X(swFxPixMs) X(editorDocChangedMs) X(editorPanelsMs) X(idlePaintMs)

/// Every uint32_t workload/memory field of FrameStats (i.e. all but frameIndex), in declaration
/// order, as `X(name)`. Companion to XT_FRAME_STATS_FLOAT_FIELDS.
#define XT_FRAME_STATS_UINT_FIELDS(X)                                                                       \
    X(styledElements)                                                                                       \
    X(laidOutElements)                                                                                      \
    X(activeAnimations) X(activeTransitions) X(commands) X(layers) X(effectLayers) X(projections) X(glyphs) \
        X(pathPoints) X(modifierVertices) X(skinnedPoints) X(rigBones) X(imageDraws) X(imageMeshes)         \
            X(meshTriangles) X(swGlyphBitmaps) X(swGlyphOutlines) X(spansPerFrame) X(spanPixelsPerFrame)    \
                X(arenaNodes) X(commandBytes) X(runtimeStrings) X(maxDistinctBindingStrings)              \
                    X(bands) X(bandRedraws) X(backdropCopyPx) X(pyramidLevels) X(directFallback) X(setStyleCalls) \
                        X(flowSignals) X(flowSignalsCoalesced) X(flowNodeEvals) X(flowSinkWrites) \
                            X(flowFanOutEdges) X(flowSinkTyped) X(flowSinkText) X(flowAllocs) X(animEvaluated) \
                                X(borderDashHits) X(borderDashMisses) X(borderDashEntries) X(borderDashBytes) \
                                    X(culledOffscreen) X(paintSkipped) X(staticSkipMismatch) X(exprEvaluated) \
                                        X(layoutRoots) X(layoutFullPasses) X(partialLayoutMismatch) \
                                            X(redundantStyled) X(redundantLaidOut) X(redundantCommands) \
                                                X(repaintedUnchangedNotCacheable) X(repaintedUnchangedKeyChanged) \
                                                    X(repaintedUnchangedBudget) X(repaintedUnchangedFirstSight)
// clang-format on

/// T-753 (Docs/design/57-measurement.md §57.2 item 3): the second-level zones -- a fixed list, each under
/// one top-level phase. Timed by XT_PERF_ZONE while the tracker is enabled at detail level >= 2; their
/// totals are PerfTracker::zones() (FrameRecord has no columns for them). Exclusive: a zone opened inside
/// another pauses the outer one.
enum class PerfZone : uint8_t {
    PaintText,              ///< paint.text: a Text node's selection, glyphs, placeholder and caret
    PaintPaths,             ///< paint.paths: Path / Shape nodes and modified groups
    PaintBorders,           ///< paint.borders: an element's border, border image and outline
    PaintEffects,           ///< paint.effects: an effect node's PushEffect and box shadow candidate
    PaintImages,            ///< paint.images: an element's image, Image and Mesh nodes
    UpdateStyleMatch,       ///< update.style.match: an element's matching declarations, collected and sorted
    UpdateStyleCascade,     ///< update.style.cascade: the rest of its resolve, up to the commit
    UpdateLayoutFlex,       ///< update.layout.flex: a flex container's own layout
    UpdateLayoutGrid,       ///< update.layout.grid: a grid container's own layout
    UpdateLayoutTextShape,  ///< update.layout.textShape: a text node shaped (its shaping cache missed)
    BuildWalk,              ///< build.walk: gpu::FrameBuilder's walk over the commands
    BuildEffects,           ///< build.effects: an effect stack run
    BuildCache,             ///< build.cache: effect segments classified, keyed, pasted or stored
    Count
};
inline constexpr uint32_t kPerfZoneCount = static_cast<uint32_t>(PerfZone::Count);
/// "paint.text", "update.style.match", ...
const char* perfZoneName(PerfZone zone);
/// The same in snake case for CSV columns: "paint_text", "update_layout_text_shape", ...
const char* perfZoneCsvName(PerfZone zone);
/// The top-level phase the zone belongs to: "paint", "update" or "build".
const char* perfZonePhase(PerfZone zone);

/// T-753: wall milliseconds and calls of every zone, summed since the tracker's last reset().
struct PerfZoneTotals {
    double ms[kPerfZoneCount] = {};
    uint64_t count[kPerfZoneCount] = {};
};

/// avg/max over a window of frames, plus that window's average frame rate (§21.2).
struct PerfSummary {
    /// Per-field arithmetic mean over the window.
    FrameStats avg;
    /// Per-field maximum over the window.
    FrameStats max;
    /// Frames per second over the window, derived from FrameStats::timeSeconds (the Context clock).
    float fpsAvg = 0;
    /// Number of frames the summary covers (0 when there is no history).
    uint32_t frames = 0;
};

/// Per-Context frame statistics recorder (§21.2). Not thread safe: it is written from whatever
/// thread drives Context::update()/render() and the backend render() call.
class PerfTracker {
public:
    /// Number of frames history() keeps (§21.2's "環狀緩衝，最近 240 幀").
    static constexpr uint32_t kHistoryCapacity = 240;

    /// Turns tracing on or off. Turning it on allocates the ring buffer on the next beginFrame();
    /// turning it off keeps the history collected so far (use reset() to drop it).
    void setEnabled(bool on) {
        m_enabled = on;
        m_zoneTiming = m_enabled && m_detail >= 2;
    }
    /// True while frames are being recorded.
    bool enabled() const { return m_enabled; }
    /// T-747 (57 §57.3 item 2): how much detail the cause table records. 0 (the default): the
    /// counts only; >= 1: also FrameStats::causeNodes and (T-753) the redundant-work counts
    /// (FrameStats::redundant*, repaintedUnchanged*); >= 2: also the second-level zones (zones()).
    /// Negative values read as 0.
    void setDetail(int level) {
        m_detail = level < 0 ? 0 : level;
        m_zoneTiming = m_enabled && m_detail >= 2;
    }
    int detail() const { return m_detail; }
    /// T-753: enabled() with detail() >= 1 -- the redundant-work counts are taken.
    bool countsRedundantWork() const { return m_enabled && m_detail >= 1; }

    /// T-753 (57 §57.2 item 3): enabled() with detail() >= 2 -- XT_PERF_ZONE times its zone.
    bool zoneTiming() const { return m_zoneTiming; }
    /// T-753: every zone's wall milliseconds and calls since the last reset() (or resetZones()).
    const PerfZoneTotals& zones() const { return m_zones; }
    /// T-753: zeroes zones() (the open zones keep running and are counted when they close).
    void resetZones() { m_zones = PerfZoneTotals{}; }
    /// T-753: PerfZoneScope's halves. enterZone() pauses the open zone (its time so far is added) and
    /// opens `zone`; false (nothing opened) past kMaxZoneDepth nested zones. leaveZone() closes the
    /// innermost zone and resumes the one it paused.
    static constexpr uint32_t kMaxZoneDepth = 16;
    bool enterZone(PerfZone zone) {
        if (m_zoneDepth >= kMaxZoneDepth) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (m_zoneDepth > 0) {
            const uint32_t outer = m_zoneDepth - 1;
            m_zones.ms[static_cast<uint32_t>(m_zoneStack[outer])] +=
                std::chrono::duration<double, std::milli>(now - m_zoneStart[outer]).count();
        }
        m_zoneStack[m_zoneDepth] = zone;
        m_zoneStart[m_zoneDepth] = now;
        ++m_zoneDepth;
        return true;
    }
    void leaveZone() {
        if (m_zoneDepth == 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        --m_zoneDepth;
        const uint32_t z = static_cast<uint32_t>(m_zoneStack[m_zoneDepth]);
        m_zones.ms[z] += std::chrono::duration<double, std::milli>(now - m_zoneStart[m_zoneDepth]).count();
        ++m_zones.count[z];
        if (m_zoneDepth > 0) {
            m_zoneStart[m_zoneDepth - 1] = now;
        }
    }
    /// T-750 (57 §57.2 item 4): also record the thread CPU time of every top-level phase and of the
    /// marked frame (FrameStats::*CpuMs, frameCpuMs). Off by default; the first time it is turned on
    /// the tracker calibrates its ThreadClock (Windows: ~200 ms busy loop, once per tracker).
    void setThreadCpuTiming(bool on) {
        if (on && !m_clock.calibrated()) {
            m_clock.calibrate();
        }
        m_threadCpu = on;
    }
    bool threadCpuTiming() const { return m_threadCpu; }
    /// The tracker's thread clock (calibrated once setThreadCpuTiming(true) ran).
    const ThreadClock& threadClock() const { return m_clock; }

    /// T-750 (57 §57.2 item 2): the host marks its whole frame -- markFrameStart() before it starts
    /// the frame's work (before Context::update()), markFrameEnd() after the last of it (after the
    /// backend's submit and any wait). The start is held until the next beginFrame() opens the
    /// frame's record; markFrameEnd() writes FrameStats::frameWallMs / frameCpuMs / frameMarked into
    /// the current record. A frame without both marks keeps frameMarked 0. No-ops while disabled.
    void markFrameStart();
    void markFrameEnd();

    /// Opens a new frame (called at the top of Context::update()). No-op while disabled.
    void beginFrame(float timeSeconds);
    /// Closes the frame opened by the last beginFrame() (called at the end of Context::render()).
    /// Writes through current() that happen after endFrame() but before the next beginFrame() --
    /// which is exactly what a backend reporting renderMs after Context::render() returned does --
    /// still land in that same frame's record.
    void endFrame();

    /// The frame currently being filled. While disabled (or before the first beginFrame()) this is
    /// a scratch record that is never stored, so writing to it is always safe.
    FrameStats& current() { return m_frames.empty() ? m_sink : m_frames[m_newest]; }
    /// Read-only access to the frame currently being filled.
    const FrameStats& current() const { return m_frames.empty() ? m_sink : m_frames[m_newest]; }

    /// The recorded frames, oldest to newest, at most kHistoryCapacity of them (including the frame
    /// currently open). The returned span stays valid until the next call to history(), beginFrame()
    /// or reset().
    std::span<const FrameStats> history() const;

    /// avg/max over the last `lastN` recorded frames (fewer when the history is shorter).
    PerfSummary summary(uint32_t lastN = 120) const;

    /// Drops the whole history and restarts frameIndex at 0. Does not change enabled().
    void reset();

private:
    bool m_enabled = false;
    int m_detail = 0;  // T-747: setDetail().
    // T-753: the zones -- the switch (enabled && detail >= 2), the totals and the open zones.
    bool m_zoneTiming = false;
    PerfZoneTotals m_zones;
    uint32_t m_zoneDepth = 0;
    PerfZone m_zoneStack[kMaxZoneDepth] = {};
    std::chrono::steady_clock::time_point m_zoneStart[kMaxZoneDepth];
    // T-750: setThreadCpuTiming() and the frame marks (a pending start, then the open frame's).
    bool m_threadCpu = false;
    ThreadClock m_clock;
    bool m_pendingStart = false;
    std::chrono::steady_clock::time_point m_pendingWall;
    double m_pendingCpu = 0;
    bool m_frameStarted = false;
    std::chrono::steady_clock::time_point m_frameWall;
    double m_frameCpu = 0;
    // Ring buffer. m_frames is empty until the first beginFrame() while enabled, then sized to
    // kHistoryCapacity; m_count is how many slots hold a recorded frame, m_newest the newest one.
    std::vector<FrameStats> m_frames;
    uint32_t m_count = 0;
    uint32_t m_newest = 0;
    uint32_t m_nextFrameIndex = 0;
    // Written to by current() while disabled, so callers never need a null check.
    FrameStats m_sink;
    // Oldest-to-newest copy handed out by history(); mutable because history() is const.
    mutable std::vector<FrameStats> m_ordered;
};

/// RAII timer that adds the elapsed milliseconds to `*field` on destruction; a null `field` makes
/// both the constructor and the destructor a single branch and nothing else.
///
/// Not part of §21.2: the backends take their PerfTracker as an optional pointer parameter, so they
/// need a scope that can be switched off at the call site. PerfScope is implemented on top of it.
class PerfFieldScope {
public:
    /// `field` may be null, which disables the timer entirely.
    explicit PerfFieldScope(float* field) : m_field(field) {
        if (m_field == nullptr) {
            return;
        }
        m_start = std::chrono::steady_clock::now();
    }
    ~PerfFieldScope() {
        if (m_field == nullptr) {
            return;
        }
        std::chrono::duration<float, std::milli> elapsed = std::chrono::steady_clock::now() - m_start;
        *m_field += elapsed.count();
    }
    /// Not copyable.
    PerfFieldScope(const PerfFieldScope&) = delete;
    /// Not copyable.
    PerfFieldScope& operator=(const PerfFieldScope&) = delete;

private:
    float* m_field = nullptr;
    std::chrono::steady_clock::time_point m_start;
};

/// §21.2's phase scope: adds the elapsed milliseconds to `t.current().*field`. Costs one branch on
/// construction and one on destruction while `t` is disabled.
class PerfScope : public PerfFieldScope {
public:
    /// `t` must outlive the scope (it always does: it is the Context's tracker).
    PerfScope(PerfTracker& t, float FrameStats::* field)
        : PerfFieldScope(t.enabled() ? &(t.current().*field) : nullptr) {}
};

/// T-750 (57 §57.2): a top-level phase scope -- adds the elapsed wall milliseconds to
/// `t->current().*wall` and, while `t->threadCpuTiming()`, the thread CPU milliseconds to
/// `t->current().*cpu`. A null or disabled tracker costs one branch on construction and one on
/// destruction; an enabled tracker without CPU timing one more branch than PerfScope.
class PerfPhaseScope {
public:
    PerfPhaseScope(PerfTracker* t, float FrameStats::* wall, float FrameStats::* cpu) {
        if (t == nullptr || !t->enabled()) {
            return;
        }
        FrameStats& f = t->current();
        m_wall = &(f.*wall);
        if (t->threadCpuTiming()) {
            m_cpu = &(f.*cpu);
            m_clock = &t->threadClock();
            m_cpu0 = m_clock->nowMs();
        }
        m_start = std::chrono::steady_clock::now();
    }
    PerfPhaseScope(PerfTracker& t, float FrameStats::* wall, float FrameStats::* cpu)
        : PerfPhaseScope(&t, wall, cpu) {}
    ~PerfPhaseScope() {
        if (m_wall == nullptr) {
            return;
        }
        std::chrono::duration<float, std::milli> elapsed = std::chrono::steady_clock::now() - m_start;
        *m_wall += elapsed.count();
        if (m_cpu != nullptr) {
            *m_cpu += static_cast<float>(m_clock->nowMs() - m_cpu0);
        }
    }
    PerfPhaseScope(const PerfPhaseScope&) = delete;
    PerfPhaseScope& operator=(const PerfPhaseScope&) = delete;

private:
    float* m_wall = nullptr;
    float* m_cpu = nullptr;
    const ThreadClock* m_clock = nullptr;
    double m_cpu0 = 0;
    std::chrono::steady_clock::time_point m_start;
};

/// T-753 (57 §57.2 item 3): times one second-level zone (exclusively, see PerfZone) into
/// `t->zones()`. A null tracker, or one below detail level 2 or disabled, costs one test on construction
/// and one on destruction and never reads the clock. Use XT_PERF_ZONE.
class PerfZoneScope {
public:
    PerfZoneScope(PerfTracker* t, PerfZone zone) {
        if (t != nullptr && t->zoneTiming() && t->enterZone(zone)) {
            m_tracker = t;
        }
    }
    ~PerfZoneScope() {
        if (m_tracker != nullptr) {
            m_tracker->leaveZone();
        }
    }
    PerfZoneScope(const PerfZoneScope&) = delete;
    PerfZoneScope& operator=(const PerfZoneScope&) = delete;

private:
    PerfTracker* m_tracker = nullptr;
};

#define XT_PERF_ZONE_JOIN2(a, b) a##b
#define XT_PERF_ZONE_JOIN(a, b) XT_PERF_ZONE_JOIN2(a, b)
/// T-753: `XT_PERF_ZONE(tracker, PaintText);` -- a PerfZoneScope over the rest of the enclosing block.
/// `tracker` is a PerfTracker* (may be null); the zone is a PerfZone enumerator name.
#define XT_PERF_ZONE(tracker, zone) \
    ::xt::PerfZoneScope XT_PERF_ZONE_JOIN(xtPerfZone_, __LINE__)((tracker), ::xt::PerfZone::zone)

namespace perf {

/// T-475 (21 §21.6, 27 §27.4.2 item 6): a per-thread heap allocation count for tests. Nothing in
/// the libraries calls record(): an executable that defines XT_PERF_ALLOC_COUNT replaces the global
/// operator new and calls it there (Tests/perf/flow_alloc_hook.cpp, xt_perf_flow only), so in every
/// other program total() stays 0. FlowRuntime::tick() reports the difference as
/// FrameStats::flowAllocs.
class AllocCounter {
public:
    /// Counts one allocation on the calling thread. Never allocates, never throws.
    static void record() noexcept;
    /// Allocations recorded on the calling thread so far.
    static uint64_t total() noexcept;
};

}  // namespace perf

}  // namespace xt
