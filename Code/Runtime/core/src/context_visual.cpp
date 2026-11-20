#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "command_list_append.h"
#include "damage_internal.h"
#include "env_flag.h"
#include "xt/assert.h"
#include "xt/context.h"
#include "xt/world_layer_output.h"

/// Context::render()'s idle-paint accounting (FrameStats::idlePaintMs), the visual-generation check
/// (Context::setVerifyVisualGeneration) and the static skip (Context::setStaticSkipEnabled) whose
/// verify mode that check is. See Docs/design/57-measurement.md §57.3 items 3-4,
/// Docs/design/23-host-integration.md §23.ag items 1-5, Docs/tasks/T-747.md, Docs/tasks/T-748.md.

namespace xt {

void Context::initVisualCheck() {
    // The test suites' switch (Tests/unit/test_main.cpp, golden, corpus): every Context checks, and a
    // mismatch fails the run at once with its diagnostic.
    if (envFlag("XT_VERIFY_VISUAL_GENERATION") == '1') {
        m_verifyVisual = true;
        m_verifyAbort = true;
    }
    // T-748 (23 §23.ag item 4): the host's way back to painting every frame.
    if (envFlag("XT_STATIC_SKIP") == '0') {
        m_staticSkip = false;
    }
}

Context::PaintOptions Context::currentPaintOptions() const {
    return PaintOptions{m_viewport, m_devicePixelRatio, m_hostTargetMode, m_worldLayerOutput,
                        m_boxShadowCommands};
}

void Context::beginVisualFrame() {
    ++m_renderFrame;
    if (m_renderFrame == 0) {
        m_renderFrame = 1;  // 0 means "never painted" in PaintState::frame.
        for (PaintState& s : m_paintStates) {
            s.frame = 0;
        }
    }
    m_frameIdlePaintMs = 0;
    // T-746 (23 §23.ah): a binding the host changed since the last update() (a new content version
    // between update and render) is noted now, so the paint it causes leaves the Document static again.
    for (const DocumentHandle handle : m_orderScratch) {
        m_documents[handle - 1]->checkExternalTextures();
    }
    // Idle only when the same surfaces paint in the same order as last time (a surface shown,
    // hidden, unloaded or restacked changes the list without any of their generations).
    m_frameAllIdle = m_orderScratch == m_prevPaintOrder;
    m_verifyMarks.clear();
    m_verifyDocs.clear();
}

bool Context::canSkipPaint(const Document& doc, DocumentHandle handle, uint16_t source, bool haveBase) const {
    // 23 §23.ag item 3: the host handed back the previous list untouched (`haveBase`: its serial and
    // structureChecked), the paint options are those of the previous render(), and this Document was
    // painted by it at the same place with the same load serial and visual generation, into commands
    // that change with nothing but writes.
    if (!m_staticSkip || !haveBase || handle == 0 || handle > m_paintStates.size() ||
        !(m_lastPaintOptions == currentPaintOptions())) {
        return false;
    }
    const PaintState& last = m_paintStates[handle - 1];
    return last.frame != 0 && last.frame + 1 == m_renderFrame && last.source == source &&
           last.loadSerial == doc.loadSerial() && last.generation == doc.visualGeneration() &&
           !last.clockDependent && last.end <= m_prevList.commands.size() &&
           last.layerEnd <= m_prevList.worldLayers.size() &&
           // T-746 (23 §23.ah): the external bindings (with their content versions) are the ones it
           // painted with -- also when the host changed one after the last update().
           last.externalSig == doc.externalTextureSignature();
}

bool Context::allPaintsSkip(bool haveBase) {
    // Every Document skips and they paint in the same order as last time: the previous list, as the
    // host handed it back, is this frame's list.
    bool all = haveBase && m_staticSkip && !m_verifyVisual && m_orderScratch == m_prevPaintOrder;
    for (size_t i = m_orderScratch.size(); all && i-- > 0;) {
        Document* doc = m_documents[m_orderScratch[i] - 1].get();
        xt::setWorldLayerOutput(*doc, m_worldLayerOutput);  // T-701 (a change moves the generation)
        all = canSkipPaint(*doc, m_orderScratch[i], static_cast<uint16_t>(m_orderScratch.size() - 1 - i),
                           haveBase);
    }
    return all;
}

void Context::reusePaint(DocumentHandle handle, CommandList& out) {
    PaintState& last = m_paintStates[handle - 1];
    const auto first = static_cast<uint32_t>(out.commands.size());
    const auto layerFirst = static_cast<uint32_t>(out.worldLayers.size());
    appendCommandRange(m_prevList, last.first, last.end, out);
    for (uint32_t i = last.layerFirst; i < last.layerEnd; ++i) {
        WorldLayerRange range = m_prevList.worldLayers[i];
        range.firstCommand = range.firstCommand - last.first + first;
        out.worldLayers.push_back(range);
    }
    out.culledOffscreen += last.culled;
    last.frame = m_renderFrame;
    last.first = first;
    last.end = static_cast<uint32_t>(out.commands.size());
    last.layerFirst = layerFirst;
    last.layerEnd = static_cast<uint32_t>(out.worldLayers.size());
    // Idle by definition: nothing was written and nothing was painted (idlePaintMs adds nothing).
    if (m_perf.enabled()) {
        ++m_perf.current().paintSkipped;
    }
}

void Context::paintTracked(Document& doc, DocumentHandle handle, CommandList& out, uint16_t source) {
    if (m_paintStates.size() < handle) {
        m_paintStates.resize(handle);
    }
    PaintState& last = m_paintStates[handle - 1];
    const uint64_t before = doc.visualGeneration();
    const bool clockSoFar = out.clockDependent;
    const bool paintedLast = last.frame != 0 && last.frame + 1 == m_renderFrame && last.source == source;
    out.clockDependent = false;
    const auto first = static_cast<uint32_t>(out.commands.size());
    const auto layerFirst = static_cast<uint32_t>(out.worldLayers.size());
    const uint32_t culledBefore = out.culledOffscreen;
    if (m_verifyVisual) {
        doc.m_painter.setNodeMarks(&m_verifyMarks);
    }
    const bool timing = m_perf.enabled();
    const auto start = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    doc.paintAs(out, source, m_boxShadowCommands);  // T-680
    float paintMs = 0;
    if (timing) {
        paintMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    doc.m_painter.setNodeMarks(nullptr);
    // 57 §57.3 item 4: nothing was written since this Document's last paint (paintAs itself may
    // still restyle a surface shown since the last update -- that moves the generation too) and
    // that paint showed nothing that changes with time alone.
    const bool idle = last.loadSerial == doc.loadSerial() && last.generation == before &&
                      doc.visualGeneration() == before && !last.clockDependent &&
                      last.externalSig == doc.externalTextureSignature();  // T-746
    if (idle) {
        m_frameIdlePaintMs += paintMs;
    }
    m_frameAllIdle = m_frameAllIdle && idle;
    if (m_verifyVisual) {
        // T-748: an idle Document painted at the same place last frame is compared with its own
        // commands of that frame (checkVisualFrame()).
        m_verifyDocs.push_back(VerifyDoc{handle, first, static_cast<uint32_t>(out.commands.size()),
                                         last.first, last.end, idle && paintedLast});
    }
    last.loadSerial = doc.loadSerial();
    last.generation = doc.visualGeneration();
    last.clockDependent = out.clockDependent;
    last.externalSig = doc.externalTextureSignature();  // T-746
    last.frame = m_renderFrame;
    last.source = source;
    last.first = first;
    last.end = static_cast<uint32_t>(out.commands.size());
    last.layerFirst = layerFirst;
    last.layerEnd = static_cast<uint32_t>(out.worldLayers.size());
    last.culled = out.culledOffscreen - culledBefore;
    out.clockDependent = clockSoFar || out.clockDependent;
}

void Context::checkVisualFrame(const CommandList& out, const std::vector<uint64_t>* knownHashes) {
    if (!m_verifyVisual) {
        m_verifyPrevValid = false;
        return;
    }
    // T-748: the damage diff hashed this list already when it ran.
    if (knownHashes == nullptr || knownHashes->size() != out.commands.size()) {
        hashCommandList(out, m_verifyHashes);
    } else {
        m_verifyHashes.assign(knownHashes->begin(), knownHashes->end());
    }
    uint32_t compared = 0;
    uint32_t mismatched = 0;
    for (const VerifyDoc& vd : m_verifyDocs) {
        if (!m_verifyPrevValid || !vd.compare || vd.prevEnd > m_verifyPrevHashes.size() ||
            vd.end > m_verifyHashes.size()) {
            continue;
        }
        ++compared;
        const uint32_t count = vd.end - vd.first;
        const uint32_t prevCount = vd.prevEnd - vd.prevFirst;
        uint32_t k = 0;
        while (k < count && k < prevCount &&
               m_verifyHashes[vd.first + k] == m_verifyPrevHashes[vd.prevFirst + k]) {
            ++k;
        }
        if (k == count && k == prevCount) {
            continue;
        }
        ++mismatched;
        if (!m_visualCheck.first.empty()) {
            continue;  // Only the first mismatch of the Context is described.
        }
        const size_t at = size_t{vd.first} + k;
        NodeId node = kInvalidNode;
        for (const auto& [mark, n] : m_verifyMarks) {
            if (mark <= at) {
                node = n;
            }
        }
        std::string id;
        const Document* doc = liveAt(vd.handle - 1);
        if (doc != nullptr && node != kInvalidNode && doc->arena().alive(node) &&
            doc->arena().kind(node) == NodeKind::Element && doc->arena().element(node).id != kInvalidString) {
            id = std::string(doc->string(doc->arena().element(node).id));
        }
        const int kind = k < count ? static_cast<int>(out.commands[at].kind) : -1;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "visual generation unchanged but the command list changed: command %zu of %zu "
                      "(kind %d, was %u commands), document %u, node %u%s%s",
                      at, m_verifyHashes.size(), kind, static_cast<unsigned>(prevCount),
                      static_cast<unsigned>(vd.handle), static_cast<unsigned>(node), id.empty() ? "" : " #",
                      id.c_str());
        m_visualCheck.first = buf;
        m_visualCheck.firstDocument = vd.handle;
        m_visualCheck.firstNode = node;
        m_visualCheck.firstKind = kind;
    }
    if (compared > 0) {
        ++m_visualCheck.frames;
    }
    if (mismatched > 0) {
        ++m_visualCheck.mismatches;
        if (m_perf.enabled()) {
            m_perf.current().staticSkipMismatch += mismatched;
        }
        if (m_verifyAbort) {
            detail::assertFail(m_visualCheck.first.c_str(), __FILE__, __LINE__);
        }
    }
    m_verifyHashes.swap(m_verifyPrevHashes);
    m_verifyPrevValid = true;
}

void Context::endVisualFrame(float damageMs) {
    // The damage diff of a frame where every Document was idle compared two equal lists.
    if (m_frameAllIdle) {
        m_frameIdlePaintMs += damageMs;
    }
    if (m_perf.enabled()) {
        m_perf.current().idlePaintMs += m_frameIdlePaintMs;
    }
    m_prevPaintOrder = m_orderScratch;
    m_lastPaintOptions = currentPaintOptions();
}

// T-755 (Docs/design/06-layout.md §6.x): the partial-layout switches of every Document.
void Context::applyPartialLayoutSettings(Document& doc) const {
    if (m_partialLayout >= 0) {
        doc.m_layoutEngine.setPartialLayout(m_partialLayout != 0);
    }
    if (m_verifyPartialLayout >= 0) {
        doc.m_layoutEngine.setVerifyPartial(m_verifyPartialLayout != 0);
    }
}

void Context::setPartialLayout(bool on) {
    m_partialLayout = on ? 1 : 0;
    forEachLiveDocument([&](Document* doc) { doc->m_layoutEngine.setPartialLayout(on); });
}

void Context::setVerifyPartialLayout(bool on) {
    m_verifyPartialLayout = on ? 1 : 0;
    forEachLiveDocument([&](Document* doc) { doc->m_layoutEngine.setVerifyPartial(on); });
}

const LayoutEngine* Context::layoutEngine(DocumentHandle handle) const {
    const Document* doc = handle == 0 ? nullptr : liveAt(handle - 1);
    return doc != nullptr ? &doc->m_layoutEngine : nullptr;
}

}  // namespace xt
