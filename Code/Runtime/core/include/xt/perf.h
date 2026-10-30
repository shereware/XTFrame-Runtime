#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

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
};

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
                X(swFxSdfMs) X(swFxPixMs) X(editorDocChangedMs) X(editorPanelsMs)

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
                            X(flowFanOutEdges) X(flowSinkTyped) X(flowSinkText) X(flowAllocs)
// clang-format on

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
    void setEnabled(bool on) { m_enabled = on; }
    /// True while frames are being recorded.
    bool enabled() const { return m_enabled; }

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
