#include <vector>

#include "binding_runtime.h"
#include "event_dispatcher.h"
#include "repeat_controller.h"
#include "timeline_group.h"
#include "trigger_runtime.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/node_arena.h"

/// Context::needsFrame() and Context::documentNeedsUpdate() (Docs/design/23-host-integration.md §23.ag item
/// 6, Docs/tasks/T-749.md). Every source of Context::update() / Document::update() that moves on with time
/// alone is answered here by the state it keeps; whatever cannot be told apart counts as moving.

namespace xt {

namespace {

/// A `<particles>` element whose next step may move, spawn or retire a particle.
bool particlesMoving(const NodeArena& arena, NodeId element, bool editorMode) {
    if (!arena.alive(element) || arena.kind(element) != NodeKind::Element) {
        return false;
    }
    const NodeId node = arena.element(element).particlesNode;
    if (node == kInvalidNode) {
        return false;
    }
    const ParticleNodeData& x = arena.particles(node);
    if (editorMode && x.simulateInEditor == 0) {
        return false;  // The editor's canvas keeps this emitter still.
    }
    // Rate emission on with a rate that emits (NaN counts as emitting), a burst or autoplay waiting,
    // live particles moving.
    return x.pendingAutoplay != 0 || x.burstPending != 0 || (x.emitting != 0 && !(x.rate <= 0.0f)) ||
           !x.particles.empty();
}

/// A `<video>` / `<sequence>` / `<external>` element stepExternalMedia() or refreshExternalPlaceholders()
/// still has work for. `placeholders`: Document::setExternalPlaceholders() is on -- a host texture bound
/// through the AssetResolver (no Document call) turns the placeholder off only at an update, so such an
/// element counts as moving (conservative).
bool mediaMoving(const NodeArena& arena, NodeId element, bool haveAssets, bool placeholders) {
    if (!arena.alive(element) || arena.kind(element) != NodeKind::Element) {
        return false;
    }
    const NodeId node = arena.element(element).externalNode;
    if (node == kInvalidNode) {
        return false;
    }
    const ExternalNodeData& x = arena.external(node);
    if (x.pendingAutoplay != 0) {
        return true;
    }
    if (x.source != ExternalSourceKind::Sequence) {
        // A video's / external's frames come from the host: drawn, its texture counts as clock-dependent.
        return placeholders;
    }
    // An atlas whose cell count is still unknown is asked for its size at every step.
    const bool atlasUnknown =
        x.frames.empty() && x.atlasFrames == 0 && haveAssets && x.frameSize.x > 0.0f && x.frameSize.y > 0.0f;
    return x.state == MediaState::Playing || atlasUnknown;
}

}  // namespace

bool Context::documentNeedsUpdate(const Document& d) {
    if (d.m_unloading) {
        return false;
    }
    // The surface lifecycle: the first update's `show`, a show / hide notification made between frames.
    if (!d.m_shownOnce || d.m_pendingSurfaceNotify != 0) {
        return true;
    }
    // Writes not resolved yet (style, layout, transforms, paint, tree), held-back trigger chains and
    // port changes, `tween` / `delay` binding steps.
    if (d.m_arena.anyDirty(0xFFu) || d.m_forceTransform || d.m_triggers->hasQueued() ||
        d.m_bindings->needsTick()) {
        return true;
    }
    // Gamepad repeats, long presses, pointer-drag auto-scroll, a pending focus().
    if (d.m_events->needsTick()) {
        return true;
    }
    // Animations, transitions, overrides; `@timeline` groups; `<repeat>` instance timelines and
    // spawns; media sequences and their end events.
    if (d.m_animator.needsEvaluate(d.m_arena) || !d.m_timelineGroups->pending.empty() ||
        d.m_repeats->needsStep() || !d.m_pendingMediaEnds.empty()) {
        return true;
    }
    for (const TimelineGroupRuntime& g : d.m_timelineGroups->groups) {
        if (g.active && (g.playing || !g.restoresCaptured)) {
            return true;
        }
    }
    for (NodeId element : d.m_externalElements) {
        if (mediaMoving(d.m_arena, element, d.m_assets != nullptr, d.m_externalPlaceholders)) {
            return true;
        }
    }
    // Only a visible surface lays out and steps its particles and rigs (a hidden one restarts them
    // without a catch-up when shown, which moves the visual generation anyway).
    if (d.m_root == kInvalidNode || !d.m_surfaceVisible) {
        return false;
    }
    if (d.m_layoutSkipped || d.m_rigsMoving) {
        return true;
    }
    for (NodeId element : d.m_particleElements) {
        if (particlesMoving(d.m_arena, element, d.m_particleEditorMode)) {
            return true;
        }
    }
    return false;
}

bool Context::needsFrame() const {
    // Input and ray events queued, surface requests or document operations waiting.
    if (!m_inputQueue.empty() || !m_pointerRays.empty() || m_surfaceRequestsPending ||
        !m_pendingDocOps.empty()) {
        return true;
    }
    // A hover to re-judge against moved geometry (Context::update() runs it on the hovered surface).
    if (m_hoverSurface != 0) {
        const Document* doc = liveAt(m_hoverSurface - 1);
        if (doc != nullptr && doc->m_surfaceVisible && doc->m_events->hoverRefreshPending()) {
            return true;
        }
    }
    // Trim-on-hide: the update after everything was hidden releases the renderer's scratch.
    bool anyVisible = false;
    for (size_t i = 0; i < m_documents.size(); ++i) {
        const Document* doc = liveAt(i);
        if (doc == nullptr) {
            continue;
        }
        if (documentNeedsUpdate(*doc)) {
            return true;
        }
        anyVisible = anyVisible || doc->isVisible();
    }
    if (m_trimOnHide && m_renderScratchHeld && !anyVisible) {
        return true;
    }
    // A host listener (xt::flow::BundleContext: its flows' timers, clocks and queued signals).
    for (const ContextListener* listener : m_listeners) {
        if (listener->needsFrame(*this)) {
            return true;
        }
    }
    // The host has not rendered the current state: the static skip's conditions (canSkipPaint()) for
    // every Document the next render() paints -- painted by the last render() in the same order, with
    // the same load serial and visual generation, into commands that hold nothing clock-dependent,
    // under the same paint options -- T-746: and with the external bindings (content versions) it
    // painted with.
    if (m_renderFrame == 0 || !(m_lastPaintOptions == currentPaintOptions())) {
        return true;
    }
    surfaceOrder(m_orderScratch);
    if (m_orderScratch != m_prevPaintOrder) {
        return true;
    }
    const size_t count = m_orderScratch.size();
    for (size_t i = 0; i < count; ++i) {
        const DocumentHandle handle = m_orderScratch[i];
        const Document* doc = liveAt(handle - 1);
        if (doc == nullptr || handle > m_paintStates.size()) {
            return true;
        }
        const PaintState& last = m_paintStates[handle - 1];
        if (last.frame != m_renderFrame || last.source != static_cast<uint16_t>(count - 1 - i) ||
            last.loadSerial != doc->loadSerial() || last.generation != doc->visualGeneration() ||
            last.clockDependent || last.externalSig != doc->externalTextureSignature()) {  // T-746
            return true;
        }
    }
    return false;
}

}  // namespace xt
