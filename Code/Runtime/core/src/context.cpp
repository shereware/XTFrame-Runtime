#include "xt/context.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "damage_internal.h"
#include "dispatch_guard.h"
#include "event_dispatcher.h"  // T-552: Context::update() re-judges the hover (refreshHover)
#include "file_open_utf8.h"
#include "mini_css.h"
#include "xt/asset_resolver.h"

/// Context: Document ownership, clock and per-frame update/render dispatch. See
/// Docs/tasks/T-18.md. loadDocumentFile is Docs/tasks/T-18.md's carve-out of xt_core's
/// no-standard-I/O rule; since T-261e it reads through openFileUtf8() (`<cstdio>`, no exceptions)
/// so its UTF-8 path opens through the wide API on Windows.

namespace xt {

Context::Context(const ContextConfig& config) : m_config(config) {}

Context::~Context() = default;

namespace {

/// T-263e (Docs/tasks/T-263e.md must-fix 1): `doc`'s IR is about to be dropped; the resolver lets
/// go of what it keeps for that IR (AssetResolver::releaseSource). Called while `doc` is alive.
void releaseSourceOf(AssetResolver* assets, const Document* doc) {
    if (assets != nullptr && doc != nullptr) {
        assets->releaseSource(doc->ir());
    }
}

}  // namespace

Result<DocumentHandle> Context::loadDocument(IrBlob blob, bool takeOwnership) {
    std::unique_ptr<Document> doc;
    Status status = Document::load(blob, takeOwnership, m_config.shaper, m_config.assets, doc);
    if (status != Status::kOk) {
        return Result<DocumentHandle>::error(status);
    }
    // Docs/design/21-perf-tracing.md §21.2: "Document 經 Context 取得" its PerfTracker.
    doc->setPerfTracker(&m_perf);
    doc->m_context = this;  // T-260a: its dispatching calls defer unload / reload install.
    bindTimelineEvents(*doc);
    doc->setTokenLayers(activeThemeTokens(), &m_tokenOverrides);  // T-170.
    // 13 §13.8.14 ruling (T-236): a new Document starts from this Context's current viewport, so
    // a host that calls setViewport() before loadDocument() does not get a 0x0 viewport.
    doc->setViewport(m_viewport);
    doc->setDevice(m_device);  // T-176.
    doc->setUiScale(m_uiScale);
    doc->setLocalization(m_localization, m_language);  // T-179.
    doc->setGlyphProvider(m_glyphProvider);            // T-178.
    m_documents.push_back(std::move(doc));
    m_surfaces.resize(m_documents.size());  // T-194: z 0, Block.
    DocumentHandle handle = static_cast<DocumentHandle>(m_documents.size());
    bindHostEvents(*m_documents.back(), handle);  // T-174.
    return Result<DocumentHandle>::ok(handle);
}

Result<DocumentHandle> Context::loadDocumentFile(std::string_view path) {
    std::FILE* file = openFileUtf8(std::string(path).c_str(), "rb");
    if (file == nullptr) {
        return Result<DocumentHandle>::error(Status::kIoError);
    }
    long length = -1;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        length = std::ftell(file);
    }
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return Result<DocumentHandle>::error(Status::kIoError);
    }

    std::vector<uint8_t> data(static_cast<size_t>(length));
    const size_t read = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (read != data.size()) {
        return Result<DocumentHandle>::error(Status::kIoError);
    }
    return loadDocument(IrBlob{data.data(), data.size()}, /*takeOwnership=*/true);
}

Result<DocumentHandle> Context::reloadDocument(DocumentHandle handle, IrBlob blob, bool takeOwnership) {
    std::unique_ptr<Document> doc;
    const Status status = prepareReload(handle, blob, takeOwnership, doc);
    if (status != Status::kOk) {
        // The slot is untouched, so the previous Document keeps running (Docs/design/22-bundle.md
        // §22.5).
        return Result<DocumentHandle>::error(status);
    }
    installReload(handle, doc);
    return Result<DocumentHandle>::ok(handle);
}

Status Context::prepareReload(DocumentHandle handle, IrBlob blob, bool takeOwnership,
                              std::unique_ptr<Document>& out) {
    if (handle == 0 || liveAt(handle - 1) == nullptr) {  // T-260a: an unloading one is gone.
        return Status::kInvalidArgument;
    }
    std::unique_ptr<Document> doc;
    Status status = Document::load(blob, takeOwnership, m_config.shaper, m_config.assets, doc);
    if (status != Status::kOk) {
        return status;
    }
    doc->setPerfTracker(&m_perf);
    doc->m_context = this;  // T-260a.
    bindTimelineEvents(*doc);
    doc->setTokenLayers(activeThemeTokens(), &m_tokenOverrides);  // T-170.
    // The reloaded Document starts from this Context's current viewport, like setViewport() would
    // have given it; the clock and the input queue are Context state and are not touched at all.
    doc->setViewport(m_viewport);
    doc->setDevice(m_device);  // T-176.
    doc->setUiScale(m_uiScale);
    bindHostEvents(*doc, handle);                               // T-174.
    doc->setLocalization(m_localization, m_language);           // T-179.
    doc->setGlyphProvider(m_glyphProvider);                     // T-178.
    doc->setInstanceId(m_documents[handle - 1]->instanceId());  // T-175.
    out = std::move(doc);
    return Status::kOk;
}

void Context::installReload(DocumentHandle handle, std::unique_ptr<Document>& doc) {
    if (m_dispatchDepth > 0) {
        // T-260a: the old Document (or another one) is running a host callback; it is replaced
        // once the outermost dispatching call returns (runPendingDocOps).
        m_pendingDocOps.push_back(PendingDocOp{.handle = handle, .doc = std::move(doc)});
        return;
    }
    // Swap first, destroy after: the new Document is in place before the old one runs its
    // destructor (which is what invalidates every Document*/ElementRef the host still holds).
    m_documents[handle - 1].swap(doc);
    releaseSourceOf(m_config.assets, doc.get());  // T-263e: the old Document's IR goes with it.
    doc.reset();
    m_damageBaseValid = false;  // §23.2 step 5: a reload repaints everything.
}

void Context::runPendingDocOps() {
    // Counted as a dispatch itself: an unload reached from a destructor below is queued behind
    // and still run by this loop (index-based, the size re-read every round).
    ++m_dispatchDepth;
    // T-397 (08 §8.17): the surface show / hide requests made while dispatching, in handle order;
    // one a callback below makes is queued again and applied by the next round (bounded).
    for (int round = 0; m_surfaceRequestsPending && round < 8; ++round) {
        m_surfaceRequestsPending = false;
        for (size_t i = 0; i < m_documents.size(); ++i) {
            if (Document* doc = liveAt(i)) {
                doc->applyPendingSurfaceRequest();
            }
        }
    }
    m_surfaceRequestsPending = false;
    for (size_t i = 0; i < m_pendingDocOps.size(); ++i) {
        PendingDocOp op = std::move(m_pendingDocOps[i]);
        std::unique_ptr<Document>& slot = m_documents[op.handle - 1];
        if (!op.doc) {
            releaseSourceOf(m_config.assets, slot.get());  // T-263e.
            slot.reset();
            forgetSurface(op.handle);  // T-260b.
            continue;
        }
        // T-260b: a reload followed by an unload of the same handle is never installed, so the
        // new Document makes no host callback at all; the unload frees the old one in its turn.
        bool unloadedLater = false;
        for (size_t j = i + 1; j < m_pendingDocOps.size() && !unloadedLater; ++j) {
            unloadedLater = m_pendingDocOps[j].handle == op.handle && !m_pendingDocOps[j].doc;
        }
        if (slot && !unloadedLater) {
            slot.swap(op.doc);  // As installReload(): swap first, the old one dies with `op`.
            m_damageBaseValid = false;
        }
        // A reload of a slot unloaded before (or after) it: the new Document dies with `op`.
        // T-263e: whichever Document `op` holds now -- the old one after the swap, or the new one
        // that was never installed -- its IR is dropped here.
        releaseSourceOf(m_config.assets, op.doc.get());
    }
    m_pendingDocOps.clear();
    --m_dispatchDepth;
}

void Context::forgetSurface(DocumentHandle handle) {
    if (m_pointerCapture == handle) {
        m_pointerCapture = 0;
    }
    if (m_hoverSurface == handle) {
        m_hoverSurface = 0;
    }
    if (m_focusSurface == handle) {
        m_focusSurface = 0;
    }
    if (handle != 0 && handle <= m_surfaces.size()) {
        m_surfaces[handle - 1] = SurfaceState{};
    }
}

Status Context::postSystemEvent(std::string_view kind, std::string_view value) {
    DispatchGuard dispatch(this);  // T-260a: ports fire triggers and listeners, then onSystemEvent.
    // Canonical kind (27.12's order), the implicit port's name and whether it carries a string.
    struct SystemKind {
        std::string_view kind;
        std::string_view port;
        bool text;
    };
    static constexpr SystemKind kKinds[] = {
        {"loading", "system.loading", false},
        {"paused", "system.paused", false},
        {"resumed", "system.resumed", false},
        {"disconnected", "system.disconnected", false},
        {"languageChanged", "system.language", true},
        {"platform", "system.platform", true},
        {"inputDevice", "system.inputDevice", true},
        {"back", "system.back", false},  // T-240: the Android back key (13.8 tail addition).
        // T-330: the host's reduced-motion setting; also DeviceInfo::reducedMotion, so
        // `@media (prefers-reduced-motion)` follows it.
        {"reducedMotion", "system.reducedMotion", true},
    };
    const SystemKind* found = nullptr;
    for (const SystemKind& k : kKinds) {
        if (k.kind == kind || (kind == "language" && k.port == "system.language")) {
            found = &k;
            break;
        }
    }
    if (found == nullptr) {
        return Status::kInvalidArgument;
    }
    if (found->kind == "reducedMotion") {
        // T-330: "reduce" / "true" / "1" turn it on, "no-preference" / "false" / "0" off.
        const bool on = value == "reduce" || value == "true" || value == "1";
        if (!on && value != "no-preference" && value != "false" && value != "0") {
            return Status::kInvalidArgument;
        }
        if (on != m_device.reducedMotion) {
            DeviceInfo device = m_device;
            device.reducedMotion = on;
            (void)setDevice(device);  // Re-evaluates every document's `@media`.
        }
    }
    const size_t count = m_documents.size();  // T-260a: one a callback loads is not sent this event.
    for (size_t i = 0; i < count; ++i) {
        Document* doc = liveAt(i);
        if (doc == nullptr) {
            continue;
        }
        const PortId port = doc->lookupPort(found->port);
        if (port == kInvalidPort) {
            continue;
        }
        if (found->text) {
            (void)doc->setPortText(port, value);
        } else {
            (void)doc->setPort(port, Value::ofFloat(0));
        }
    }
    const std::vector<ContextListener*> listeners = m_listeners;  // Callbacks may edit the list.
    for (ContextListener* listener : listeners) {
        listener->onSystemEvent(found->kind, value);
    }
    return Status::kOk;
}

Status Context::setWorldAnchor(DocumentHandle handle, PortId port, float x, float y, float depth,
                               bool onScreen) {
    Document* doc = document(handle);
    return doc != nullptr ? doc->setWorldAnchor(port, x, y, depth, onScreen) : Status::kInvalidArgument;
}

void Context::unloadDocument(DocumentHandle handle) {
    if (handle == 0 || handle > m_documents.size()) {
        return;
    }
    if (m_dispatchDepth > 0) {
        // T-260a: a host callback is running inside the core (possibly inside this Document). It
        // is dead to the host from now on (marked: liveAt() skips it, its host callbacks stop);
        // its memory goes once the outermost dispatching call returns. Unloading it again is a
        // no-op (liveAt() already says it is gone).
        Document* doc = liveAt(handle - 1);
        if (doc == nullptr) {
            return;
        }
        doc->markUnloading();
        m_pendingDocOps.push_back(PendingDocOp{.handle = handle, .doc = nullptr});
        return;
    }
    releaseSourceOf(m_config.assets, m_documents[handle - 1].get());  // T-263e.
    m_documents[handle - 1].reset();
    forgetSurface(handle);  // T-260b.
}

Document* Context::liveAt(size_t index) const {
    if (index >= m_documents.size()) {
        return nullptr;
    }
    Document* doc = m_documents[index].get();
    return doc != nullptr && !doc->m_unloading ? doc : nullptr;
}

namespace {

/// T-170: `--` followed by at least one more character.
bool isTokenName(std::string_view name) { return name.size() > 2 && name.substr(0, 2) == "--"; }

}  // namespace

const TokenMap* Context::activeThemeTokens() const {
    if (m_activeTheme.empty()) {
        return nullptr;
    }
    auto it = m_themes.find(m_activeTheme);
    return it != m_themes.end() ? &it->second : nullptr;
}

void Context::applyTokenLayers() {
    const TokenMap* theme = activeThemeTokens();
    const size_t count = m_documents.size();  // T-260a: every m_documents walk is by index.
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setTokenLayers(theme, &m_tokenOverrides);
        }
    }
}

Status Context::defineTheme(std::string_view name, std::span<const TokenDef> tokens) {
    if (name.empty()) {
        return Status::kInvalidArgument;
    }
    TokenMap map;
    for (const TokenDef& def : tokens) {
        if (!isTokenName(def.name)) {
            return Status::kInvalidArgument;
        }
        TokenValue value;
        value.text = def.value;
        value.hasValue = mini_css::classifyTokenText(def.value, value.value);
        map[def.name] = std::move(value);  // A repeated name: the later one wins, as in `.xts`.
    }
    auto it = m_themes.find(name);
    if (it == m_themes.end()) {
        m_themes.emplace(std::string(name), std::move(map));
    } else {
        it->second = std::move(map);
    }
    if (m_activeTheme == name) {
        applyTokenLayers();
    }
    return Status::kOk;
}

Status Context::setTheme(std::string_view name) {
    if (!name.empty() && m_themes.find(name) == m_themes.end()) {
        return Status::kNotFound;
    }
    m_activeTheme.assign(name);
    applyTokenLayers();
    return Status::kOk;
}

Status Context::setToken(std::string_view name, const Value& value) {
    if (!isTokenName(name) || (value.type != ValueType::Float && value.type != ValueType::Length &&
                               value.type != ValueType::Color && value.type != ValueType::Enum)) {
        return Status::kInvalidArgument;
    }
    TokenValue& slot = m_tokenOverrides[std::string(name)];
    slot.value = value;
    slot.hasValue = true;
    slot.text.clear();
    applyTokenLayers();
    return Status::kOk;
}

Status Context::clearToken(std::string_view name) {
    if (!isTokenName(name)) {
        return Status::kInvalidArgument;
    }
    auto it = m_tokenOverrides.find(name);
    if (it == m_tokenOverrides.end()) {
        return Status::kNotFound;
    }
    m_tokenOverrides.erase(it);
    applyTokenLayers();
    return Status::kOk;
}

void Context::clearTokens() {
    if (m_tokenOverrides.empty()) {
        return;
    }
    m_tokenOverrides.clear();
    applyTokenLayers();
}

Document* Context::document(DocumentHandle handle) {
    if (handle == 0) {
        return nullptr;
    }
    return liveAt(handle - 1);  // T-260a: nullptr from the moment an unload was queued.
}

uint32_t Context::documentSerial(DocumentHandle handle) const {
    const Document* doc = handle != 0 ? liveAt(handle - 1) : nullptr;
    return doc != nullptr ? doc->loadSerial() : 0;
}

void Context::setViewport(Rect logical, float devicePixelRatio) {
    if (logical != m_viewport || devicePixelRatio != m_devicePixelRatio) {
        // §23.2 step 5: a viewport or dpr change makes every pixel dirty.
        m_damageBaseValid = false;
    }
    m_viewport = logical;
    m_devicePixelRatio = devicePixelRatio;
    const size_t count = m_documents.size();
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setViewport(logical);
        }
    }
}

Status Context::setDevice(const DeviceInfo& device) {
    DispatchGuard dispatch(this);  // T-260a: glyphs re-resolve through IGlyphProvider (host).
    auto validInset = [](float v) { return v >= 0.0f && v <= 0.5f; };
    if (!(device.aspect >= 0.0f) || !std::isfinite(device.aspect) || !(device.dpr > 0.0f) ||
        !std::isfinite(device.dpr) || !validInset(device.safeArea.top) ||
        !validInset(device.safeArea.right) || !validInset(device.safeArea.bottom) ||
        !validInset(device.safeArea.left)) {
        return Status::kInvalidArgument;
    }
    m_device = device;
    m_damageBaseValid = false;  // §23.2 step 5: like a viewport change, everything may move.
    const size_t count = m_documents.size();
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setViewport(m_viewport);
            doc->setDevice(m_device);
        }
    }
    applyUiScale();  // T-345: the DPI floor follows the DPR.
    return Status::kOk;
}

void Context::setHostTargetMode(HostTargetMode mode) {
    if (mode != m_hostTargetMode) {
        // The backends draw the whole surface differently under each mode: one full damage.
        m_damageBaseValid = false;
    }
    m_hostTargetMode = mode;
}

Status Context::setUiScale(float scale) {
    if (!(scale > 0.0f) || !std::isfinite(scale)) {
        return Status::kInvalidArgument;
    }
    m_uiScaleBase = scale;
    applyUiScale();
    return Status::kOk;
}

Status Context::setUiScaleLimits(float clampMin, float clampMax, float dpiFloor) {
    if (!(clampMin >= 0.0f) || !std::isfinite(clampMin) || !(clampMax >= clampMin) || !(dpiFloor >= 0.0f) ||
        !std::isfinite(dpiFloor)) {
        return Status::kInvalidArgument;
    }
    m_uiScaleMin = clampMin;
    m_uiScaleMax = clampMax;
    m_uiScaleDpiFloor = dpiFloor;
    applyUiScale();
    return Status::kOk;
}

void Context::applyUiScale() {
    // T-345 (25 §25.4.2 item 1): the rule's factor, clamped, then at least dpiFloor x dpr.
    float scale = std::min(std::max(m_uiScaleBase, m_uiScaleMin), m_uiScaleMax);
    scale = std::max(scale, m_uiScaleDpiFloor * m_device.dpr);
    if (!(scale > 0.0f) || !std::isfinite(scale)) {
        scale = m_uiScaleBase;  // A clamp to [0, 0]: keep the unlimited factor.
    }
    if (scale != m_uiScale) {
        m_damageBaseValid = false;
    }
    m_uiScale = scale;
    const size_t count = m_documents.size();
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setUiScale(m_uiScale);
        }
    }
}

void Context::setLocalizationProvider(ILocalizationProvider* provider) {
    DispatchGuard dispatch(this);  // T-260a: `localize` bindings may set states (triggers).
    m_localization = provider;
    const size_t count = m_documents.size();
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setLocalization(m_localization, m_language);
        }
    }
}

Status Context::setLanguage(std::string_view lang) {
    DispatchGuard dispatch(this);  // T-260a: as setLocalizationProvider().
    for (char c : lang) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        if (!ok) {
            return Status::kInvalidArgument;
        }
    }
    if (lang != m_language) {
        m_damageBaseValid = false;  // Every text may change.
    }
    m_language.assign(lang);
    const size_t count = m_documents.size();
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setLocalization(m_localization, m_language);
        }
    }
    return Status::kOk;
}

void Context::setGlyphProvider(IGlyphProvider* provider) {
    DispatchGuard dispatch(this);  // T-260a: every glyph re-resolves through `provider` (host).
    m_glyphProvider = provider;
    m_damageBaseValid = false;
    const size_t count = m_documents.size();
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setGlyphProvider(provider);
        }
    }
}

Status Context::setInputDevice(InputKind input) {
    DeviceInfo device = m_device;
    device.input = input;
    return setDevice(device);
}

Document* Context::foregroundDocument() const {
    for (size_t i = m_documents.size(); i-- > 0;) {
        if (Document* doc = liveAt(i)) {
            return doc;
        }
    }
    return nullptr;
}

namespace {

/// Docs/design/21-perf-tracing.md §21.1 group 2/3: the command-list-derived workload and memory
/// counters, taken once per frame after every Document has painted.
void fillCommandListStats(const CommandList& list, FrameStats& stats) {
    stats.commands = static_cast<uint32_t>(list.commands.size());
    stats.layers = static_cast<uint32_t>(list.layers.size());
    stats.effectLayers = static_cast<uint32_t>(list.effectCmds.size());
    stats.projections = static_cast<uint32_t>(list.projections.size());
    stats.glyphs = static_cast<uint32_t>(list.glyphs.size());
    // CommandList::points holds two floats per path point.
    stats.pathPoints = static_cast<uint32_t>(list.points.size() / 2);
    stats.imageDraws = static_cast<uint32_t>(list.images.size() + list.imageQuads.size());
    // T-245: DrawImageMesh commands and their triangles.
    stats.imageMeshes = static_cast<uint32_t>(list.imageMeshes.size());
    stats.meshTriangles = 0;
    for (const ImageMeshCmd& mesh : list.imageMeshes) {
        stats.meshTriangles += mesh.indexCount / 3;
    }

    size_t bytes = 0;
    bytes += list.commands.size() * sizeof(Command);
    bytes += list.fills.size() * sizeof(FillCmd);
    bytes += list.strokes.size() * sizeof(StrokeCmd);
    bytes += list.images.size() * sizeof(ImageCmd);
    bytes += list.glyphRuns.size() * sizeof(GlyphsCmd);
    bytes += list.clipRects.size() * sizeof(ClipRectCmd);
    bytes += list.clipPaths.size() * sizeof(ClipPathCmd);
    bytes += list.layers.size() * sizeof(LayerCmd);
    bytes += list.masks.size() * sizeof(MaskCmd);
    bytes += list.projections.size() * sizeof(ProjectionCmd);
    bytes += list.transforms3.size() * sizeof(Transform3DCmd);
    bytes += list.imageQuads.size() * sizeof(ImageQuadCmd);
    bytes += list.imageMeshes.size() * sizeof(ImageMeshCmd);
    bytes += list.meshVertices.size() * sizeof(MeshVertex);
    bytes += list.meshIndices.size() * sizeof(uint16_t);
    bytes += list.effectCmds.size() * sizeof(EffectCmd);
    bytes += list.effects.size() * sizeof(EffectInstance);
    bytes += list.shaders.size() * sizeof(ShaderRef);
    bytes += list.shaderParams.size() * sizeof(ShaderParamDesc);
    bytes += list.shaderText.size() * sizeof(char);
    bytes += list.verbs.size() * sizeof(PathVerb);
    bytes += list.points.size() * sizeof(float);
    bytes += list.stops.size() * sizeof(GradientStop);
    bytes += list.glyphs.size() * sizeof(uint32_t);
    bytes += list.glyphPos.size() * sizeof(Vec2);
    bytes += list.dashes.size() * sizeof(float);
    stats.commandBytes = static_cast<uint32_t>(bytes);
}

}  // namespace

void Context::update(float dt) {
    // T-260a: first, so it is destroyed last: an unload or reload install a callback asked for
    // runs after every Document has updated (nothing below may touch a member after it).
    DispatchGuard dispatch(this);
    // T-255b: a NaN / infinite dt reads as 0 and the clock never runs backwards.
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        dt = 0.0f;
    }
    m_clock += dt;
    // §21.2: the frame opens here and stays addressable until the next beginFrame, so a backend can
    // still report renderMs into it after render() has returned.
    m_perf.beginFrame(m_clock);
    PerfScope updateScope(m_perf, &FrameStats::updateMs);
    // T-260a: the Documents updated below are the ones loaded when this frame began: one a
    // callback loads (during input routing or another Document's update; m_documents may
    // reallocate) is updated from the next frame on.
    const size_t count = m_documents.size();

    // T-552 (27 §27.4.1, 2026-10-31 ruling): pointer and hover first, then bindings. The hovered
    // surface re-judges hover at the still pointer against the geometry the previous frame left (its
    // bindings, animations, layout): PointerEnter / PointerLeave and their triggers run here, before the
    // queued input and before this frame's Document::update() evaluates the bindings, which so see this
    // frame's hover. The geometry this frame's bindings produce is judged at the next update().
    if (m_hoverSurface != 0) {
        if (Document* doc = liveAt(m_hoverSurface - 1); doc != nullptr && doc->m_surfaceVisible) {
            PerfScope inputScope(m_perf, &FrameStats::inputMs);
            doc->m_events->refreshHover(doc->m_arena, doc->m_root);
        }
    }

    // Docs/tasks/T-32.md "1.5 步驟2": drain the queue, dispatch each event to the foreground
    // Document, then run every Document's own style/animation/layout update. m_updateEventsScratch
    // is a member (not a local) so this doesn't construct a new std::vector every call -- see its
    // declaration in context.h (T-38).
    {
        // 21.1 measures inputMs as a phase of update; the dispatch itself lives here rather than in
        // Document::update, so the scope does too.
        PerfScope inputScope(m_perf, &FrameStats::inputMs);
        m_updateEventsScratch.clear();
        drainInput(m_updateEventsScratch);
        // T-194: through the surface stack (Block / Pass / Watch, z order); with every surface at
        // the default (z 0, Block) this is the pre-T-194 "foreground Document takes all input".
        // T-260a: by index, each event copied out, the count taken now and the size re-checked: a
        // callback that re-enters update() (unsupported) clears and refills this scratch.
        const size_t eventCount = m_updateEventsScratch.size();
        for (size_t i = 0; i < eventCount && i < m_updateEventsScratch.size(); ++i) {
            const InputEvent event = m_updateEventsScratch[i];
            routeInput(event);
        }
        // T-270a (C12): the batch is dispatched (the dispatcher copies the composition text), so
        // its pooled strings are no longer referenced -- unless a listener queued another
        // CompositionUpdate meanwhile, in which case the pool is emptied on a later frame.
        const bool queuedComposition =
            std::any_of(m_inputQueue.begin(), m_inputQueue.end(),
                        [](const InputEvent& e) { return e.kind == InputEvent::CompositionUpdate; });
        if (!queuedComposition) {
            m_compositionPool.clear();
        }
        // T-393: likewise the copied ExternalDrop paths.
        const bool queuedDrop =
            std::any_of(m_inputQueue.begin(), m_inputQueue.end(),
                        [](const InputEvent& e) { return e.kind == InputEvent::ExternalDrop; });
        if (!queuedDrop) {
            m_pathPool.clear();
        }
    }

    // T-561 (13.8.3): after input dispatch, before the Documents resolve: a listener's writes land
    // in this frame like xt_flow's pulse edges. Unload / reload it asks for wait for `dispatch`.
    if (!m_listeners.empty()) {
        const std::vector<ContextListener*> listeners = m_listeners;  // Callbacks may edit the list.
        for (ContextListener* listener : listeners) {
            listener->onBeforeDocumentUpdate(*this);
        }
    }

    for (size_t i = 0; i < count; ++i) {  // By index (see `count` above).
        if (Document* doc = liveAt(i)) {
            doc->update(m_clock);
        }
    }
}

void Context::render(CommandList& out, DamageList* damage) {
    // T-260a: deliberately not a dispatching entry point (no DispatchGuard). Painting and the
    // damage diff call no host interface and no callback: Painter reads the arena and the IR only
    // (images are decoded later by the backend from the command list, not through AssetResolver
    // here), and takeExternalDamage() only projects rectangles. Unloading Documents are skipped
    // like every other walk (surfaceOrder() and liveAt()).
    bool haveBase = false;
    if (damage != nullptr) {
        // §23.2 step 1: `out` still holds the previous frame's commands (the caller kept it);
        // take it as the diff's base without copying, and hand the older buffer back for reuse.
        std::swap(out, m_prevList);
        // Docs/tasks/T-263c.md ruling 2: `out` was in the host's hands. It is the base only when
        // it is the list this Context produced last (its serial) and still says so itself
        // (`structureChecked`); a cleared, replaced or hand-edited list is not diffed -- nothing
        // may index it -- and the frame is reported fully damaged.
        haveBase = m_damageBaseValid && m_lastFrameSerial != 0 && m_prevList.structureChecked &&
                   m_prevList.frameSerial == m_lastFrameSerial;
        // T-263d: the base's source table names IRs as they were a frame ago (a Document may
        // have been unloaded or reloaded since). The diff compares commands, `source` indices
        // included, and never resolves an asset; with the table emptied nothing can.
        m_prevList.sources.clear();
    }
    out.clear();
    out.viewport = m_viewport;
    out.devicePixelRatio = m_devicePixelRatio;
    out.hostTargetMode = m_hostTargetMode;  // T-343
    out.sceneStatic = m_sceneStatic;
    // T-194: bottom surface first (z ascending, then load order).
    surfaceOrder(m_orderScratch);
    for (size_t i = m_orderScratch.size(); i-- > 0;) {
        // T-263d: one CommandSource per painted Document, in paint order (a single Document is
        // no special case); the Painter stamps the Document's asset commands with its index, so
        // a backend resolves each AssetId against the IR that numbered it.
        XT_ASSERT(out.sources.size() <= 0xFFFFu);
        if (out.sources.size() > 0xFFFFu) {
            break;  // `source` is 16 bits; nothing real paints 65537 surfaces in one frame.
        }
        Document* doc = m_documents[m_orderScratch[i] - 1].get();
        const uint16_t source = static_cast<uint16_t>(out.sources.size());
        out.sources.push_back(CommandSource{&doc->ir(), doc->ir().loadSerial()});
        doc->paintAs(out, source);
    }
    // T-263a: the Painter's own output; the backends need not check its structure every frame.
    // T-263b: Debug builds check it here, so a Painter bug that emits an unbalanced or
    // out-of-range list fails our own suites (no cost in Release).
    XT_ASSERT(validateCommandList(out));
    out.structureChecked = true;
    // T-263c: process-wide, so a list of another Context is never taken for this one's.
    static std::atomic<uint32_t> nextFrameSerial{1};
    out.frameSerial = nextFrameSerial.fetch_add(1, std::memory_order_relaxed);
    if (out.frameSerial == 0) {
        out.frameSerial = nextFrameSerial.fetch_add(1, std::memory_order_relaxed);
    }
    m_lastFrameSerial = out.frameSerial;
    if (!haveBase) {
        // T-191: nothing to report the external rebinds into; drop them.
        m_externalDamageScratch.clear();
        const size_t count = m_documents.size();
        for (size_t i = 0; i < count; ++i) {
            if (Document* doc = liveAt(i)) {
                doc->takeExternalDamage(m_externalDamageScratch);
            }
        }
    }
    if (damage != nullptr) {
        PerfFieldScope damageScope(m_perf.enabled() ? &m_perf.current().damageMs : nullptr);
        damage->clear();
        Rect device{m_viewport.x * m_devicePixelRatio, m_viewport.y * m_devicePixelRatio,
                    m_viewport.w * m_devicePixelRatio, m_viewport.h * m_devicePixelRatio};
        damage->viewport = device;
        if (haveBase) {
            computeDamage(m_prevList, out, device, *damage);
            // T-191: a host that rebound (or refilled) an external texture changed pixels the
            // command diff cannot see.
            m_externalDamageScratch.clear();
            const size_t count = m_documents.size();
            for (size_t i = 0; i < count; ++i) {
                if (Document* doc = liveAt(i)) {
                    doc->takeExternalDamage(m_externalDamageScratch);
                }
            }
            for (const Rect& r : m_externalDamageScratch) {
                if (damage->full) {
                    break;
                }
                Rect d{r.x * m_devicePixelRatio, r.y * m_devicePixelRatio, r.w * m_devicePixelRatio,
                       r.h * m_devicePixelRatio};
                const float x0 = std::max(d.x, device.x);
                const float y0 = std::max(d.y, device.y);
                const float x1 = std::min(d.x + d.w, device.x + device.w);
                const float y1 = std::min(d.y + d.h, device.y + device.h);
                if (x1 <= x0 || y1 <= y0) {
                    continue;
                }
                damage->rects.push_back(Rect{x0, y0, x1 - x0, y1 - y0});
                if (damage->rects.size() > kMaxDamageRects) {
                    damage->rects.clear();
                    damage->full = true;
                }
            }
        } else {
            // §23.2 step 5: first frame, viewport/dpr change, reload, no previous list kept, or
            // (T-263c) a list that is not this Context's previous one.
            damage->full = true;
        }
        m_damageBaseValid = true;
    } else {
        // Nothing tracks the frame on screen any more; the next damaged frame starts over.
        m_damageBaseValid = false;
        m_prevList.clear();
    }
    if (m_perf.enabled()) {
        fillCommandListStats(out, m_perf.current());
    }
    m_perf.endFrame();
}

void Context::pushInput(const InputEvent& event) {
    InputEvent queued = event;
    if (queued.kind == InputEvent::CompositionUpdate) {
        m_compositionPool.emplace_back(queued.composition);
        queued.compositionPoolIndex = static_cast<uint32_t>(m_compositionPool.size() - 1);
        queued.composition = m_compositionPool.back();
    }
    if (queued.kind == InputEvent::ExternalDrop) {
        // T-393 (08.14): the host's views are valid during this call only.
        PathCopy& copy = m_pathPool.emplace_back();
        copy.strings.assign(queued.paths.begin(), queued.paths.end());
        copy.views.assign(copy.strings.begin(), copy.strings.end());
        queued.paths = copy.views;
    } else {
        queued.paths = {};
    }
    m_inputQueue.push_back(queued);
}

void Context::cancelPointer() {
    InputEvent event;
    event.kind = InputEvent::PointerCancel;
    pushInput(event);
}

void Context::cancelDrag() {
    // T-388 (08.12): one drag per Document; liveAt() skips a surface a listener unloaded.
    for (size_t i = 0; i < m_documents.size(); ++i) {
        if (Document* doc = liveAt(i)) {
            doc->cancelDrag();
        }
    }
}

void Context::drainInput(std::vector<InputEvent>& out) {
    for (InputEvent& event : m_inputQueue) {
        out.push_back(event);
    }
    m_inputQueue.clear();
}

void Context::bindTimelineEvents(Document& doc) {
    // T-260a: each fan-out below stops as soon as a listener unloaded `doc` (it owns the lambda,
    // so the pointer is valid while the lambda runs).
    Document* source = &doc;
    doc.setTimelineListener([this, source](const TimelineEvent& event) {
        // Index-based: a listener may add or remove listeners while being called.
        for (size_t i = 0; i < m_timelineListeners.size() && !source->m_unloading; ++i) {
            TimelineListener listener = m_timelineListeners[i].second;
            if (listener) {
                listener(event);
            }
        }
    });
}

Document* Context::documentWithTimeline(std::string_view scopePath, std::string_view name) const {
    for (size_t i = m_documents.size(); i-- > 0;) {
        Document* doc = liveAt(i);
        if (doc != nullptr && doc->timelineTime(scopePath, name) >= 0.0f) {
            return doc;
        }
    }
    return nullptr;
}

Status Context::playTimeline(std::string_view scopePath, std::string_view name, const PlayOptions& options) {
    Document* doc = documentWithTimeline(scopePath, name);
    return doc != nullptr ? doc->playTimeline(scopePath, name, options) : Status::kNotFound;
}

Status Context::stopTimeline(std::string_view scopePath, std::string_view name) {
    Document* doc = documentWithTimeline(scopePath, name);
    return doc != nullptr ? doc->stopTimeline(scopePath, name) : Status::kNotFound;
}

Status Context::seekTimeline(std::string_view scopePath, std::string_view name, float t) {
    Document* doc = documentWithTimeline(scopePath, name);
    return doc != nullptr ? doc->seekTimeline(scopePath, name, t) : Status::kNotFound;
}

uint32_t Context::addTimelineListener(TimelineListener listener) {
    uint32_t id = m_nextTimelineListener++;
    m_timelineListeners.emplace_back(id, std::move(listener));
    return id;
}

void Context::removeTimelineListener(uint32_t id) {
    for (size_t i = 0; i < m_timelineListeners.size(); ++i) {
        if (m_timelineListeners[i].first == id) {
            m_timelineListeners.erase(m_timelineListeners.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void Context::bindHostEvents(Document& doc, DocumentHandle handle) {
    // T-260a: each fan-out below stops as soon as a listener unloaded `doc` (see
    // bindTimelineEvents).
    Document* source = &doc;
    doc.setHostEventListener([this, handle, source](const HostEvent& event) {
        HostEvent copy = event;
        copy.document = handle;
        std::vector<ContextListener*> listeners = m_listeners;  // A callback may (un)register.
        for (ContextListener* listener : listeners) {
            if (source->m_unloading) {
                break;
            }
            listener->onHostEvent(copy);
        }
    });
    doc.setVisibilityListener([this, handle, source](bool visible) {
        std::vector<ContextListener*> listeners = m_listeners;
        for (ContextListener* listener : listeners) {
            if (source->m_unloading) {
                break;
            }
            if (visible) {
                listener->onShown(handle);
            } else {
                listener->onHidden(handle);
            }
        }
    });
    // T-191 (13.8.3): video playback requests.
    doc.setExternalRequestListener([this, handle, source](const ExternalRequest& request) {
        ExternalRequest copy = request;
        copy.document = handle;
        std::vector<ContextListener*> listeners = m_listeners;
        for (ContextListener* listener : listeners) {
            if (source->m_unloading) {
                break;
            }
            listener->onExternalRequest(copy);
        }
    });
    doc.setExternalPlaceholders(m_externalPlaceholders);
}

void Context::setExternalPlaceholders(bool on) {
    m_externalPlaceholders = on;
    const size_t count = m_documents.size();
    for (size_t i = 0; i < count; ++i) {
        if (Document* doc = liveAt(i)) {
            doc->setExternalPlaceholders(on);
        }
    }
}

Status Context::setCustomState(ElementRef element, std::string_view name, bool on) {
    if (!element.valid()) {
        return Status::kInvalidArgument;
    }
    return element.doc->setCustomState(element, name, on);
}

Status Context::requestShow(DocumentHandle handle) {
    Document* doc = document(handle);
    return doc != nullptr ? doc->requestShow() : Status::kInvalidArgument;
}

Status Context::requestHide(DocumentHandle handle) {
    Document* doc = document(handle);
    return doc != nullptr ? doc->requestHide() : Status::kInvalidArgument;
}

Status Context::setVisible(DocumentHandle handle, bool visible) {
    Document* doc = document(handle);
    if (doc == nullptr) {
        return Status::kInvalidArgument;
    }
    doc->setInitialVisibility(visible);
    return Status::kOk;
}

bool Context::isVisible(DocumentHandle handle) const {
    if (handle == 0 || liveAt(handle - 1) == nullptr) {
        return false;
    }
    return m_documents[handle - 1]->isVisible();
}

Status Context::setInstanceId(DocumentHandle handle, uint32_t id) {
    Document* doc = document(handle);
    if (doc == nullptr) {
        return Status::kInvalidArgument;
    }
    doc->setInstanceId(id);
    return Status::kOk;
}

uint32_t Context::instanceId(DocumentHandle handle) const {
    if (handle == 0 || liveAt(handle - 1) == nullptr) {
        return 0;
    }
    return m_documents[handle - 1]->instanceId();
}

bool Context::isHiding(DocumentHandle handle) const {
    if (handle == 0 || liveAt(handle - 1) == nullptr) {
        return false;
    }
    return m_documents[handle - 1]->isHiding();
}

void Context::addListener(ContextListener* listener) {
    if (listener != nullptr &&
        std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end()) {
        m_listeners.push_back(listener);
    }
}

void Context::removeListener(ContextListener* listener) {
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
}

bool Context::wantsTextInput() const {
    const DocumentHandle h = keyTarget();  // T-194: the surface keyboard input goes to.
    return h != 0 && m_documents[h - 1]->wantsTextInput();
}

Rect Context::textInputRect() const {
    const DocumentHandle h = keyTarget();
    return h != 0 ? m_documents[h - 1]->textInputRect() : Rect{};
}

Cursor Context::cursor() const {
    // T-194: the surface under the pointer, else the keyboard surface.
    DocumentHandle h = m_hoverSurface;
    if (h == 0 || liveAt(h - 1) == nullptr) {
        h = keyTarget();
    }
    return h != 0 ? m_documents[h - 1]->cursor() : Cursor::Default;
}

// -- T-194: surface stacking, input routing, players ----------------------------------------------

Status Context::setSurfaceZ(DocumentHandle handle, int z) {
    if (handle == 0 || liveAt(handle - 1) == nullptr) {
        return Status::kInvalidArgument;
    }
    m_surfaces[handle - 1].z = z;
    m_damageBaseValid = false;  // The paint order changed.
    return Status::kOk;
}

int Context::surfaceZ(DocumentHandle handle) const {
    return handle == 0 || handle > m_surfaces.size() ? 0 : m_surfaces[handle - 1].z;
}

Status Context::setSurfaceInput(DocumentHandle handle, SurfaceInput mode) {
    if (handle == 0 || liveAt(handle - 1) == nullptr ||
        static_cast<uint8_t>(mode) > static_cast<uint8_t>(SurfaceInput::Watch)) {
        return Status::kInvalidArgument;
    }
    m_surfaces[handle - 1].input = mode;
    return Status::kOk;
}

SurfaceInput Context::surfaceInput(DocumentHandle handle) const {
    return handle == 0 || handle > m_surfaces.size() ? SurfaceInput::Block : m_surfaces[handle - 1].input;
}

Status Context::setPlayerDevice(uint8_t player, InputKind device) {
    if (player >= kMaxPlayers) {
        return Status::kInvalidArgument;
    }
    m_playerDevices[player] = device;
    if (player == 0 && m_device.input != device) {
        return setInputDevice(device);
    }
    return Status::kOk;
}

InputKind Context::playerDevice(uint8_t player) const {
    return player < kMaxPlayers ? m_playerDevices[player] : InputKind::Mouse;
}

void Context::surfaceOrder(std::vector<DocumentHandle>& out) const {
    out.clear();
    for (size_t i = 0; i < m_documents.size(); ++i) {
        if (liveAt(i) != nullptr && m_documents[i]->isVisible()) {
            out.push_back(static_cast<DocumentHandle>(i + 1));
        }
    }
    // Insertion sort (short list, no allocation): z descending, then handle descending.
    for (size_t i = 1; i < out.size(); ++i) {
        const DocumentHandle item = out[i];
        size_t j = i;
        while (j > 0) {
            const DocumentHandle prev = out[j - 1];
            const int zi = m_surfaces[item - 1].z;
            const int zp = m_surfaces[prev - 1].z;
            if (zp > zi || (zp == zi && prev > item)) {
                break;
            }
            out[j] = prev;
            --j;
        }
        out[j] = item;
    }
}

DocumentHandle Context::keyTarget() const {
    surfaceOrder(m_orderScratch);
    // Candidates: every non-Watch surface down to (and including) the topmost Block one.
    DocumentHandle first = 0;
    for (DocumentHandle h : m_orderScratch) {
        const SurfaceInput mode = m_surfaces[h - 1].input;
        if (mode == SurfaceInput::Watch) {
            continue;
        }
        if (first == 0) {
            first = h;
        }
        if (h == m_focusSurface) {
            return h;  // The surface that took the last press keeps the keyboard.
        }
        if (mode == SurfaceInput::Block) {
            break;
        }
    }
    return first;
}

DocumentHandle Context::pressTarget(Vec2 pos) const {
    surfaceOrder(m_orderScratch);
    for (DocumentHandle h : m_orderScratch) {
        const SurfaceInput mode = m_surfaces[h - 1].input;
        if (mode == SurfaceInput::Watch) {
            continue;
        }
        if (mode == SurfaceInput::Block ||
            m_documents[h - 1]->contentHitTest(pos, /*press=*/true) != kInvalidNode) {
            return h;
        }
    }
    return 0;
}

void Context::routeInput(const InputEvent& event) {
    // T-260a: a surface whose unload a callback queued is gone here too, even halfway through.
    auto live = [this](DocumentHandle h) { return h != 0 && liveAt(h - 1) != nullptr; };
    switch (event.kind) {
        case InputEvent::PointerMove: {
            DocumentHandle target = live(m_pointerCapture) ? m_pointerCapture : 0;
            DocumentHandle watched[8] = {};
            size_t watchedCount = 0;
            surfaceOrder(m_orderScratch);
            const std::vector<DocumentHandle> order = m_orderScratch;  // Dispatch may re-sort.
            for (DocumentHandle h : order) {
                if (!live(h)) {
                    continue;  // Unloaded by a callback of a surface above.
                }
                const SurfaceInput mode = m_surfaces[h - 1].input;
                if (mode == SurfaceInput::Watch) {
                    m_documents[h - 1]->dispatchInput(event);
                    if (watchedCount < 8) {
                        watched[watchedCount++] = h;
                    }
                    continue;
                }
                if (target != 0) {
                    break;  // A captured pointer: surfaces below see nothing.
                }
                if (mode == SurfaceInput::Block ||
                    m_documents[h - 1]->contentHitTest(event.pos, /*press=*/false) != kInvalidNode) {
                    target = h;
                    break;
                }
            }
            if (live(target)) {
                m_documents[target - 1]->dispatchInput(event);
            }
            const DocumentHandle previous = m_hoverSurface;
            bool previousWatched = false;
            for (size_t i = 0; i < watchedCount; ++i) {
                previousWatched = previousWatched || watched[i] == previous;
            }
            if (previous != target && !previousWatched && live(previous)) {
                InputEvent leave;
                leave.kind = InputEvent::PointerLeave;
                leave.player = event.player;
                m_documents[previous - 1]->dispatchInput(leave);
            }
            m_hoverSurface = target;
            return;
        }
        case InputEvent::PointerLeave:
        case InputEvent::PointerCancel: {
            // T-386 (08.11): a cancel reaches every surface (only one with an active element
            // reacts) and releases the pointer capture, so the next press is routed afresh.
            if (event.kind == InputEvent::PointerCancel) {
                m_pointerCapture = 0;
            }
            const size_t count = m_documents.size();
            for (size_t i = 0; i < count; ++i) {
                if (Document* doc = liveAt(i)) {
                    if (event.kind == InputEvent::PointerCancel) {
                        doc->cancelPointer();  // T-390 (08.13).
                    } else {
                        doc->dispatchInput(event);
                    }
                }
            }
            if (event.kind == InputEvent::PointerLeave) {
                m_hoverSurface = 0;
            }
            return;
        }
        case InputEvent::ExternalDragEnter:
        case InputEvent::ExternalDragMove:
        case InputEvent::ExternalDrop: {
            // T-393 (08.14): the surface a press at `pos` would reach; one the drag moved off
            // gets ExternalDragLeave first.
            const DocumentHandle target = pressTarget(event.pos);
            const DocumentHandle previous = m_externalDragSurface;
            m_externalDragSurface = event.kind == InputEvent::ExternalDrop ? 0 : target;
            if (previous != target && live(previous)) {
                InputEvent leave;
                leave.kind = InputEvent::ExternalDragLeave;
                leave.pos = event.pos;
                leave.player = event.player;
                m_documents[previous - 1]->dispatchInput(leave);
            }
            if (live(target)) {
                m_documents[target - 1]->dispatchInput(event);
            }
            return;
        }
        case InputEvent::ExternalDragLeave: {
            const DocumentHandle previous = m_externalDragSurface;
            m_externalDragSurface = 0;
            if (live(previous)) {
                m_documents[previous - 1]->dispatchInput(event);
            }
            return;
        }
        case InputEvent::PointerDown:
        case InputEvent::Wheel:
        case InputEvent::Gesture: {
            const DocumentHandle target = pressTarget(event.pos);
            if (event.kind == InputEvent::PointerDown) {
                m_pointerCapture = target;
                if (target != 0) {
                    m_focusSurface = target;
                }
            }
            if (live(target)) {
                m_documents[target - 1]->dispatchInput(event);
            }
            return;
        }
        case InputEvent::PointerUp: {
            const DocumentHandle target = live(m_pointerCapture) ? m_pointerCapture : pressTarget(event.pos);
            m_pointerCapture = 0;
            if (!live(target)) {
                return;
            }
            Document& source = *m_documents[target - 1];
            // A drag released outside every drop target of its own surface: does another surface
            // take it? (27.7: the host forwards cross-surface drops.)
            const ElementRef dragged = source.dragSource();
            if (dragged.valid() && !source.dragOver().valid() && !m_listeners.empty()) {
                surfaceOrder(m_orderScratch);
                for (DocumentHandle h : m_orderScratch) {
                    if (h == target || m_surfaces[h - 1].input == SurfaceInput::Watch) {
                        continue;
                    }
                    Document& other = *m_documents[h - 1];
                    if (m_surfaces[h - 1].input == SurfaceInput::Pass &&
                        other.contentHitTest(event.pos, /*press=*/false) == kInvalidNode) {
                        continue;
                    }
                    DragCross cross;
                    cross.sourceDocument = target;
                    cross.source = dragged.node;
                    const StringId sid = source.arena().element(dragged.node).id;
                    cross.sourceId = sid != kInvalidString ? source.string(sid) : std::string_view{};
                    cross.sourceItem = source.itemIndexOf(dragged);
                    cross.targetDocument = h;
                    const ElementRef drop = other.dropTargetAt(event.pos);
                    if (drop.valid()) {
                        cross.target = drop.node;
                        const StringId tid = other.arena().element(drop.node).id;
                        cross.targetId = tid != kInvalidString ? other.string(tid) : std::string_view{};
                        cross.targetItem = other.itemIndexOf(drop);
                    }
                    cross.pos = event.pos;
                    cross.player = event.player;
                    std::vector<ContextListener*> listeners = m_listeners;
                    for (ContextListener* listener : listeners) {
                        if (source.m_unloading || other.m_unloading) {
                            break;  // T-260a: an earlier listener unloaded one of the two.
                        }
                        listener->onDragCross(cross);
                    }
                    break;
                }
            }
            if (live(target)) {
                m_documents[target - 1]->dispatchInput(event);
            }
            return;
        }
        default: {
            const DocumentHandle target = keyTarget();
            if (live(target)) {
                m_documents[target - 1]->dispatchInput(event);
            }
            return;
        }
    }
}

}  // namespace xt
