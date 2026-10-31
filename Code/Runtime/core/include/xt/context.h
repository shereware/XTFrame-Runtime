#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/perf.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Context: owns every loaded Document, the shared clock, and the per-frame update/render entry
/// points. See Docs/design/13-api.md §13.1 (subset), Docs/tasks/T-18.md.

namespace xt {

class TextShaper;
class AssetResolver;
class Context;
class DispatchGuard;  // Code/Runtime/core/src/dispatch_guard.h (private; T-260a).

/// Borrowed dependencies a Context is constructed with. See Docs/design/13-api.md §13.1.
struct ContextConfig {
    /// Optional text shaper, borrowed for every loaded Document's lifetime; may be null.
    TextShaper* shaper = nullptr;
    /// Optional asset resolver, borrowed for every loaded Document's lifetime; may be null.
    AssetResolver* assets = nullptr;
};

/// One design token of a theme (Docs/design/05-style-and-css.md §5.9.2, 13-api.md §13.8.2,
/// T-170): `--name` and its value text as written in the theme's `.xts` file.
struct TokenDef {
    /// Token name, including its leading `--`.
    std::string name;
    /// Value text, e.g. `#1e90ff` or `8px`.
    std::string value;
};

/// T-178 (Docs/design/13-api.md §13.8.3): what IGlyphProvider::resolve() hands back -- the name
/// of an image asset in the document's IR asset table, exactly as compiled (an `img src` value,
/// e.g. `icons/pad_a.png` or `engine://pad_a`). An empty name, or one the document has no asset
/// for, leaves the glyph unresolved (drawn as the text `[key]`).
struct AssetRef {
    /// Asset name as it appears in the IR `ASST` table.
    std::string name;
};

/// T-178 (13.8.3): resolves an input action (`<glyph key="jump">`) to the button glyph image of
/// the given input device. Called on load, on setGlyphProvider() and whenever the input device
/// changes; never per frame.
class IGlyphProvider {
public:
    virtual ~IGlyphProvider() = default;
    /// The glyph image for `action` on `device`; an empty AssetRef when there is none.
    virtual AssetRef resolve(std::string_view action, InputKind device) = 0;
};

/// T-194 (Docs/design/08-events.md §8.9, 27 §27.7, 23 §23.10 item 6): how a surface (a loaded
/// Document) treats pointer input relative to the surfaces below it.
enum class SurfaceInput : uint8_t {
    /// Takes every pointer event and the keyboard / gamepad; surfaces below get neither (a modal).
    /// The default, which keeps the pre-T-194 behavior: the topmost surface takes all input.
    Block = 0,
    /// Takes a pointer event only where one of its elements (not its background root / body) is
    /// hit; elsewhere the event passes to the surface below. Keyboard focus may stay below.
    Pass = 1,
    /// Sees pointer moves (hover) but never presses, wheel or keyboard; everything passes through.
    Watch = 2,
};

/// T-194 (27.7): a drag that started in one surface was released over another surface
/// (ContextListener::onDragCross). The core does not move data between Documents; the host
/// decides what the drop means. The string views stay valid only for the duration of the call.
struct DragCross {
    /// The surface the drag started in and its dragged element.
    DocumentHandle sourceDocument = 0;
    NodeId source = kInvalidNode;
    std::string_view sourceId;          ///< The dragged element's id.
    uint32_t sourceItem = 0xFFFFFFFFu;  ///< Its `<repeat>` entry index (0xFFFFFFFF = none).
    /// The surface under the release point and its `drop-target` there (kInvalidNode = none).
    DocumentHandle targetDocument = 0;
    NodeId target = kInvalidNode;
    std::string_view targetId;          ///< The drop target's id.
    uint32_t targetItem = 0xFFFFFFFFu;  ///< Its `<repeat>` entry index (0xFFFFFFFF = none).
    /// The release point (viewport coordinates) and the player that dragged.
    Vec2 pos{};
    uint8_t player = 0;
};

/// T-200 (22.10): one theme of a BundleDelta (Context::defineTheme's arguments).
struct ThemeDef {
    std::string name;
    std::vector<TokenDef> tokens;
};

/// T-200 (22.10): one Document of a BundleDelta whose `.xtir` was recompiled.
struct IrReload {
    /// The loaded Document to replace.
    DocumentHandle handle = 0;
    /// The new IR; kept alive by the host for the new Document's lifetime unless `takeOwnership`.
    IrBlob blob{};
    bool takeOwnership = false;
};

/// T-200 (13.8.3 / 22.10 `hotReload(bundleDelta)`): what changed in a bundle, for
/// Context::hotReload(). Every field is optional; an empty delta changes nothing.
struct BundleDelta {
    /// Token values (manifest `tokens`), applied with setToken(): above the active theme, like any
    /// setToken() override. A value that is not a color, number or length is kInvalidArgument.
    std::optional<std::vector<TokenDef>> tokens;
    /// Themes (manifest `themes[]`), (re)defined with defineTheme().
    std::vector<ThemeDef> themes;
    /// The active theme, set with setTheme() after `themes` ("" = none).
    std::optional<std::string> theme;
    /// The string tables (manifest `strings`): setLocalizationProvider() with this provider (the
    /// same pointer again re-resolves every `@key` after its tables were reloaded).
    std::optional<ILocalizationProvider*> strings;
    /// The fonts changed (the host re-registered them with its TextShaper): every text is shaped
    /// and laid out again.
    bool fonts = false;
    /// The glyph sets (manifest `glyphs`): setGlyphProvider() with this provider.
    std::optional<IGlyphProvider*> glyphs;
    /// Symbol definitions changed. Symbols are expanded at compile time, so this alone changes
    /// nothing at run time: the affected Documents come recompiled in `ir`.
    bool symbols = false;
    /// Recompiled Documents (`.xtir`).
    std::vector<IrReload> ir;
};

/// T-200: what Context::hotReload() did with one IrReload.
struct HotReloadResult {
    DocumentHandle handle = 0;
    /// kOk: the new Document carries the old state over; kIncompatible: an id the old state
    /// refers to is gone, so the new Document starts fresh (a plain reloadDocument()); anything
    /// else: the load failed and the old Document stays.
    Status status = Status::kOk;
    /// kIncompatible only: what is gone (`port <name>`, `element <id>`, `state <id>:<Name>`,
    /// `timeline <scope>:<name>`).
    std::vector<std::string> missing;
};

/// T-200: Context::hotReload()'s per-Document results, in `BundleDelta::ir` order.
struct HotReloadReport {
    std::vector<HotReloadResult> documents;
};

/// Host callbacks of a Context (T-174; Docs/design/13-api.md §13.8.3, 27-interaction.md §27.2.4).
/// Every method has an empty default, so a host overrides only what it needs; new callbacks are
/// only ever appended.
class ContextListener {
public:
    virtual ~ContextListener() = default;
    /// A trigger's `sound` / `haptic` / `emit` action (HostEvent::document is set).
    virtual void onHostEvent(const HostEvent& /*event*/) {}
    /// requestShow() made a hidden surface visible again (`OnShown`).
    virtual void onShown(DocumentHandle /*document*/) {}
    /// A surface finished hiding (`OnHidden`): at once, or after its `then hide` outro.
    virtual void onHidden(DocumentHandle /*document*/) {}
    /// T-191 (13.8.3): a `<video>` playback request (play/pause/seek/loop/rate); the host decodes
    /// and reports back through the Document's IExternalTexture (bindTexture, setPlayback).
    virtual void onExternalRequest(const ExternalRequest& /*request*/) {}
    /// T-194 (27.7): a drag was released over a different surface than the one it started in.
    virtual void onDragCross(const DragCross& /*drop*/) {}
    /// T-200 (27.12, 23.10 item 4): Context::postSystemEvent() after the Documents' `system.*`
    /// ports were set. `kind` is the canonical name (loading, paused, resumed, disconnected,
    /// languageChanged, platform, inputDevice, back, reducedMotion); xt::flow::BundleContext
    /// forwards it to its flows.
    virtual void onSystemEvent(std::string_view /*kind*/, std::string_view /*value*/) {}
    /// T-561 (13.8.3): once per Context::update(), after input dispatch (refreshHover, queued
    /// input) and before every Document::update(). Writes made here (classes, styles, text, ports)
    /// land where xt_flow's pulse edges do: resolved this frame, transitions start at this frame's
    /// clock. Unload / reload requested here are deferred like in any other callback (13.9).
    virtual void onBeforeDocumentUpdate(Context& /*context*/) {}
};

/// Owns every loaded Document, the shared clock, and the per-frame update/render entry points
/// (load/unload, viewport, update, render, input queueing). See Docs/design/13-api.md §13.1.
class Context {
public:
    /// `config`'s pointers are borrowed and must outlive this Context.
    explicit Context(const ContextConfig& config);
    /// Destroys every remaining Document. T-263f: unlike unloadDocument(), it does not call
    /// AssetResolver::releaseSource() for them -- the resolver is normally destroyed with or
    /// right after its Context. A host that keeps one resolver (ContextConfig::assets) across
    /// several Contexts must unload this Context's Documents (unloadDocument() for each) before
    /// destroying it, or call resetAssetIds() on the resolver afterwards (FileAssetResolver);
    /// otherwise what the resolver keeps per IR -- the slots and decodes of those Documents'
    /// embedded images -- stays until the resolver is destroyed.
    ~Context();
    /// Not copyable.
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /// T-555: the shaper this Context was built with (ContextConfig::shaper); may be null.
    TextShaper* textShaper() const { return m_config.shaper; }

    /// `takeOwnership == false` requires the host to keep `blob` alive for as long as the returned
    /// Document lives.
    Result<DocumentHandle> loadDocument(IrBlob blob, bool takeOwnership);
    /// Reads the whole file into an owned buffer first, then loadDocument(..., /*takeOwnership=*/true).
    /// A missing/unreadable file returns Status::kIoError. `path` is UTF-8 (T-261e); a path that
    /// is not valid UTF-8 is treated as missing (T-261f).
    Result<DocumentHandle> loadDocumentFile(std::string_view path);
    /// Replaces `handle`'s Document with one loaded from `blob`, keeping the handle, this
    /// Context's viewport and clock, and the pending input queue. Document state (element runtime
    /// values, animations, focus/hover) is fully reset (Docs/design/22-bundle.md §22.5/§22.9).
    /// On a load failure the old Document stays and the error is returned; the handle stays valid
    /// either way. `takeOwnership == false` requires the host to keep `blob` alive for as long as
    /// the new Document lives, as in loadDocument().
    ///
    /// The old Document is destroyed, so EVERY `Document*`/`ElementRef`/listener id the host still
    /// holds for `handle` is invalidated: re-fetch document(handle) and re-resolve anything derived
    /// from it (xt::flow::FlowRuntime::rebind() does that for a bound flow graph).
    /// kInvalidArgument for an invalid or already-unloaded handle.
    Result<DocumentHandle> reloadDocument(DocumentHandle handle, IrBlob blob, bool takeOwnership);
    /// T-200 (13.8.3, 22.10): applies `delta` without rebuilding this Context. Tokens, themes,
    /// strings, fonts and glyphs go through the existing calls (setToken, defineTheme, setTheme,
    /// setLocalizationProvider, a reshape of every text, setGlyphProvider); no Document is
    /// reloaded for them. Each `ir` entry replaces its Document like reloadDocument() (the handle,
    /// viewport, clock and input queue stay; EVERY `Document*`/`ElementRef` the host holds for it
    /// is invalidated) but carries its state over, matched by id: every port value (by qualified
    /// name and type; struct fields and list entries by field name), every element's custom
    /// states (by element id and state name), each player's focus (by id), and every active
    /// timeline group's playhead and play state (by scope id and name); the Document clock
    /// continues. Restoring fires no port listener, port or state trigger, timeline event or
    /// transition (focus goes through setFocus(), so its Focus event runs). When
    /// something the old state refers to is gone (or a port changed type), nothing is carried
    /// over for that Document: it starts fresh and its result is kIncompatible. Returns kOk when
    /// every step succeeded, else the first failure (a failed load keeps the old Document;
    /// kIncompatible when a Document was reloaded fresh). `report` (may be null) receives one
    /// HotReloadResult per `ir` entry.
    Status hotReload(const BundleDelta& delta, HotReloadReport* report = nullptr);
    /// No-op for an invalid or already-unloaded handle. The handle is never reused afterwards.
    /// T-260a: called while this Context is dispatching (from a ContextListener or any other host
    /// callback the core is running), the Document is dead to the host at once -- document(handle)
    /// returns nullptr, every walk of the loaded Documents (update, input, render, fan-out) skips
    /// it, and it makes no further host callback, not even from the rest of the call running
    /// inside it -- but its memory is freed only once the outermost dispatching call returns.
    /// reloadDocument() / hotReload() load and check at once but install the new Document
    /// (destroying the old one) only then; until that install the old one stays live.
    void unloadDocument(DocumentHandle handle);
    /// Returns nullptr for an invalid or unloaded handle (T-260a: also from the moment an unload
    /// was queued during a dispatch).
    Document* document(DocumentHandle handle);
    /// T-260c: the load serial (Document::loadSerial()) of the Document document(handle) returns,
    /// 0 when that is nullptr (an invalid or unloaded handle, or an unload queued during a
    /// dispatch). A reload changes it once the new Document is installed (T-260a: at the end of
    /// the dispatch when called inside one). Code that keeps a handle checks the serial it saw
    /// against this one to tell "still the same Document" without comparing addresses, which a
    /// new Document may reuse.
    uint32_t documentSerial(DocumentHandle handle) const;

    /// Applied to every loaded Document's setViewport(); devicePixelRatio is only stored for
    /// render()'s CommandList::devicePixelRatio.
    void setViewport(Rect logical, float devicePixelRatio);
    /// Advances the shared clock by `dt` seconds and updates every loaded Document (style,
    /// animation, layout).
    void update(float dt);
    /// Clears `out`, writes this frame's viewport/devicePixelRatio into it, then paints every
    /// Document in load order.
    ///
    /// `damage` (Docs/design/23-host-integration.md §23.2) is filled with this frame's dirty
    /// rectangles in device pixels when it is non-null. Doing so needs the previous frame's
    /// command list, which this Context takes back out of `out` (swapping, never copying) before
    /// clearing it: pass the SAME CommandList object every frame and do not clear it yourself.
    /// A different list, an externally cleared one or one edited by hand is not an error and is
    /// never diffed against (T-263c: only the list this Context's previous render() produced,
    /// recognized by its CommandList::frameSerial and `structureChecked`, is a base): the frame
    /// then reports `full`. The first frame, a setViewport() and a
    /// reloadDocument() each force `full`, as does a frame that follows a `damage == nullptr`
    /// render (the previous list is not kept then).
    ///
    /// T-263e: what a host must do to keep partial damage -- this is enforced, not assumed. Hand
    /// back, every frame, the list the previous render() of this Context wrote, untouched: the
    /// same contents with its `frameSerial` and `structureChecked` as render() left them (the
    /// same object, or one it was moved / copied into). Anything that rewrites the list between
    /// two render() calls loses the base and every frame reports `full`: clearing it, editing a
    /// command or a pool, resetting either field, simplifying it in place
    /// (CommandListSimplifier: use the out-of-place form and draw its output), or alternating
    /// two lists. Reading it, drawing it and passing it to the backends changes nothing.
    void render(CommandList& out, DamageList* damage = nullptr);
    /// Current shared clock value (seconds), as advanced by update().
    float clock() const { return m_clock; }

    /// This Context's frame statistics recorder (Docs/design/21-perf-tracing.md §21.2). Disabled
    /// by default; `perf().setEnabled(true)` starts recording on the next update(). Every Document
    /// loaded by this Context reports its phase times and workload counts into it.
    PerfTracker& perf() { return m_perf; }
    /// Read-only access to this Context's frame statistics recorder.
    const PerfTracker& perf() const { return m_perf; }

    /// Queues `event` (copied) for later processing. A CompositionUpdate event's `composition`
    /// string_view is copied into an internal string pool immediately (the host is only required
    /// to keep it valid for the duration of this call); the queued copy's `compositionPoolIndex`
    /// is set to point at the pooled string and `composition` itself is repointed at the pooled
    /// storage so it stays valid after drainInput(). Queue-only in this task (T-20); dispatch into
    /// Document/Event is T-32's `update()`, which empties the pool once it has dispatched the
    /// batch (T-270a), so a pooled view is valid until the next update().
    /// T-393 (08.14): an ExternalDrop's `paths` are copied the same way (each string and the
    /// array of views), valid until the update() that dispatches it.
    void pushInput(const InputEvent& event);
    /// T-386 (Docs/design/08-events.md §8.11): queues an InputEvent::PointerCancel. At the next
    /// update() the Document holding the active element (a press in progress) sends it
    /// EventType::PointerCancel and clears the active state, without PointerUp / Click; the
    /// pointer capture is released. No effect when no press is in progress. T-390 (08.13): the
    /// queued cancel reaches every Document through Document::cancelPointer().
    void cancelPointer();
    /// T-388 (Docs/design/08-events.md §8.12): cancels the drag in progress in every surface, at
    /// once: the Dragging / DropHover states are cleared, the drop target gets DragLeave, the
    /// source DragCancel and then DragEnd (`Event::dropped` false); no Drop. The pointer press
    /// itself stays (its release dispatches PointerUp but no Click). No effect without a drag.
    void cancelDrag();
    /// Number of events currently queued (not yet drained).
    size_t pendingInputCount() const { return m_inputQueue.size(); }
    /// Moves the queued events into `out` (appended, `out` is not cleared first) and empties the
    /// queue. update() drains the queue itself (into an internal buffer); this is still exposed
    /// for tests and any host that wants to inspect/replay the queue directly.
    void drainInput(std::vector<InputEvent>& out);

    /// hoverNode's ComputedStyle::cursor in the foreground Document (Docs/design/08-events.md
    /// §8.7); Cursor::Default when there is no loaded Document or nothing is hovered.
    Cursor cursor() const;
    /// 08.8: true when the foreground Document's focus is a non-disabled `input[type=text]`, so
    /// the host should start platform text input (SDL_StartTextInput).
    bool wantsTextInput() const;
    // `@timeline` groups (Docs/design/13-api.md §13.8.2, 04-timeline.md §4.10.1, T-172). Each call
    // goes to the loaded Documents from the foreground one backwards and acts on the first that
    // has the group; `scopePath` is "" for a document's root timelines, otherwise the scope
    // element's id. See Document::playTimeline() and friends for the semantics.
    /// Plays a timeline group. kNotFound when no loaded Document has it (or the marker is
    /// unknown), kInvalidArgument for a non-positive rate.
    Status playTimeline(std::string_view scopePath, std::string_view name, const PlayOptions& options = {});
    /// Stops a timeline group (no End event). kNotFound when no loaded Document has it.
    Status stopTimeline(std::string_view scopePath, std::string_view name);
    /// Seeks a timeline group to `t` seconds. kNotFound when no loaded Document has it.
    Status seekTimeline(std::string_view scopePath, std::string_view name, float t);
    /// Registers `listener` for every TimelineEvent (End, Marker) of every Document this Context
    /// loads; 13.8.2's `timelineEvents(listener)`. Returns an id for removeTimelineListener().
    uint32_t addTimelineListener(TimelineListener listener);
    /// Removes a listener added by addTimelineListener(). No-op for an unknown id.
    void removeTimelineListener(uint32_t id);

    /// 08.8: the foreground Document's caret rectangle in viewport coordinates, for positioning
    /// the platform's IME candidate window (SDL_SetTextInputRect). An all-zero Rect when no
    /// Document is loaded or nothing editable has the focus. See Document::textInputRect().
    Rect textInputRect() const;

    /// T-170 (13.8.2): registers theme `name` -- a set of token values laid over every loaded
    /// Document's own `VARS` table while it is the active theme -- replacing any theme of that
    /// name. A token name must start with `--`; the value text is typed once here (color, number,
    /// length) and otherwise re-parsed per property. Redefining the active theme re-resolves the
    /// affected elements like setTheme(). kInvalidArgument for an empty theme name or a malformed
    /// token name.
    Status defineTheme(std::string_view name, std::span<const TokenDef> tokens);
    /// T-170 (13.8.2 `setTheme(name)`): makes `name` the active theme, or clears it for "" (every
    /// Document's own `VARS` values). Only elements that use `var()` are marked for re-resolve; no
    /// Document is recompiled or reloaded. kNotFound for a theme defineTheme() never registered.
    Status setTheme(std::string_view name);
    /// The active theme's name; empty when none is active.
    std::string_view theme() const { return m_activeTheme; }
    /// T-170 (13.8.2 tail addition): overrides token `name` (with its leading `--`) for every
    /// loaded Document, above the active theme. `value` must be a Float, Length, Color or Enum
    /// value (kInvalidArgument otherwise, or for a malformed name). Re-resolves like setTheme().
    Status setToken(std::string_view name, const Value& value);
    /// T-187 (13.8.5 tail addition): drops the setToken() override of `name`, so the token
    /// resolves from the active theme, the Document's `VARS` or the fallback again. Re-resolves
    /// like setToken(). kInvalidArgument for a malformed name, kNotFound when `name` has no
    /// override.
    Status clearToken(std::string_view name);
    /// T-187 (13.8.5 tail addition): drops every setToken() override (a no-op when there is none).
    void clearTokens();

    /// T-176 (13.8.2 `setDevice`, 25.4, 23.10 item 12): the screen every loaded Document (and every
    /// later one) is shown on -- aspect ratio, safe-area insets (fractions of the viewport), DPR,
    /// platform and input. `@media` is re-evaluated and the safe-area units follow; each Document
    /// also gets this Context's current viewport again, so one loaded after setViewport() lays out
    /// against it too. kInvalidArgument (nothing changes) for a negative or non-finite aspect, an
    /// inset outside [0, 0.5] or a non-positive DPR.
    Status setDevice(const DeviceInfo& device);
    /// T-379 (08.10): `false` turns the keyboard Enter / Space activation off in every Document
    /// (loaded now or later); `true` (the default) returns each Document to its own
    /// `<body key-activation>` declaration. Focus navigation is unaffected either way.
    void setKeyboardActivation(bool enabled) { m_keyActivation = enabled; }
    /// The last setKeyboardActivation() value (true until then).
    bool keyboardActivation() const { return m_keyActivation; }
    /// The DeviceInfo last accepted by setDevice() (a default DeviceInfo until then).
    const DeviceInfo& device() const { return m_device; }
    /// T-176 (13.8.2 `setUiScale`, tail addition): `1uiscale` = `scale` px in every loaded Document
    /// (default 1). kInvalidArgument for a non-positive or non-finite scale. T-345 (25 §25.4.2
    /// item 1): `scale` is the project rule's factor (bundle::uiScaleFor()); the factor the
    /// Documents get -- uiScale() -- is it clamped to setUiScaleLimits()' range, then raised to at
    /// least `dpiFloor` x DeviceInfo::dpr. A px length of an element whose `ui-scale` is `auto`
    /// resolves to px x this factor (05 §5.9.9).
    Status setUiScale(float scale);
    /// The final factor: setUiScale()'s value after setUiScaleLimits() (re-derived on a
    /// setDevice() that changes the DPR).
    float uiScale() const { return m_uiScale; }
    /// T-345: the factor last given to setUiScale(), before the limits.
    float uiScaleBase() const { return m_uiScaleBase; }
    /// T-345 (25 §25.4.2 item 1, tail addition): the project's `spec.uiscaleClamp: [clampMin,
    /// clampMax]` (clampMax infinity = no upper bound) and `spec.uiscaleDpiFloor`. Defaults 0,
    /// infinity, 0 (no limit). kInvalidArgument (nothing changes) for a negative or non-finite
    /// clampMin or dpiFloor, or a clampMax below clampMin or NaN.
    Status setUiScaleLimits(float clampMin, float clampMax, float dpiFloor);
    /// T-343 (Docs/design/23-host-integration.md §23.aa.1, tail addition): how the host binds the
    /// backend's output target. `Layer` (the default): a transparent layer of the host's own.
    /// `Direct`: the host's scene target itself -- the backends never clear it, `mix-blend-mode`
    /// and `backdrop-blur` read the scene, and the paint order is cached in bands (§23.aa.2).
    /// Takes effect from the next render(): the value goes out in `CommandList::hostTargetMode`,
    /// which every backend reads; a change invalidates the damage base (the whole surface is
    /// reported dirty once).
    void setHostTargetMode(HostTargetMode mode);
    /// The mode last set by setHostTargetMode() (Layer until then).
    HostTargetMode hostTargetMode() const { return m_hostTargetMode; }
    /// T-343 (§23.aa.4): the host says whether its scene is standing still (a pause menu over a
    /// frozen frame); false by default. While true, a Direct backend may also cache what the scene
    /// readers (blend modes, backdrop blurs) produced. Goes out in `CommandList::sceneStatic`.
    void setSceneStatic(bool isStatic) { m_sceneStatic = isStatic; }
    /// The value last set by setSceneStatic().
    bool sceneStatic() const { return m_sceneStatic; }
    /// T-174 (13.8.2): Document::setCustomState on `element`'s own Document.
    Status setCustomState(ElementRef element, std::string_view name, bool on);
    /// T-174 (13.8.3, 27.2.4): the surface lifecycle of one loaded Document (see
    /// Document::requestShow/requestHide). kInvalidArgument for an unknown handle.
    Status requestShow(DocumentHandle handle);
    Status requestHide(DocumentHandle handle);
    /// T-373 (17 §17.33.2): Document::setInitialVisibility() -- the surface's visibility without
    /// its `show` / `hide` triggers or OnShown / OnHidden; for a host before the first update().
    /// kInvalidArgument for an unknown handle.
    Status setVisible(DocumentHandle handle, bool visible);
    /// False for an unknown handle.
    bool isVisible(DocumentHandle handle) const;
    bool isHiding(DocumentHandle handle) const;
    /// T-175 (13.8.1, 27.3.1 multi-instance): the instance id `handle`'s host events and port
    /// events carry (Document::setInstanceId); kept across reloadDocument(). kInvalidArgument for
    /// an unknown handle.
    Status setInstanceId(DocumentHandle handle, uint32_t id);
    /// 0 for an unknown handle.
    uint32_t instanceId(DocumentHandle handle) const;
    /// T-174: registers a host listener (not owned; must outlive its registration). Adding the
    /// same pointer twice is a no-op.
    void addListener(ContextListener* listener);
    void removeListener(ContextListener* listener);

    /// T-179 (13.8.2, 07.8): the string table `@key` text resolves through, borrowed for every
    /// loaded Document (and every later one); null shows the keys themselves. Re-resolves every
    /// loaded Document's keys right away.
    void setLocalizationProvider(ILocalizationProvider* provider);
    /// The provider last given to setLocalizationProvider(), or nullptr.
    ILocalizationProvider* localizationProvider() const { return m_localization; }
    /// T-179 (13.8.2 `setLanguage`): the language every loaded Document (and every later one) is
    /// shown in: `@key` text is resolved again, `:lang()` matches it before the `lang` attribute,
    /// and text shaping re-picks the `@font-fallback` chain (every Document is relaid out).
    /// kInvalidArgument (nothing changes) for a tag holding anything but ASCII letters, digits,
    /// '-' and '_'; "" clears the language.
    Status setLanguage(std::string_view lang);
    /// The language last accepted by setLanguage(); empty until then.
    std::string_view language() const { return m_language; }
    /// T-178 (13.8.3): borrows `provider` (null = none; every `<glyph>` draws as `[key]`) for every
    /// loaded Document and every later one, and re-resolves their glyphs.
    void setGlyphProvider(IGlyphProvider* provider);
    /// T-191 (25.3.5b): every loaded Document (and every later one) draws an unbound `<video>` /
    /// `<external>` as a placeholder (Document::setExternalPlaceholders). For previewers without a
    /// decoder (xt_shell, the editor without Qt Multimedia); off by default.
    void setExternalPlaceholders(bool on);
    bool externalPlaceholders() const { return m_externalPlaceholders; }
    /// T-178 (13.8.3 `setInputDevice`): switches DeviceInfo::input -- setDevice() with only the
    /// input changed, so `@media (input: ...)` follows and every `<glyph>` is re-resolved.
    Status setInputDevice(InputKind input);

    // T-194: surface stacking and local players (Docs/design/08-events.md §8.9, 27 §27.7,
    // 23 §23.10 item 6).
    /// The stacking order of `handle`'s surface: higher `z` paints later and takes input first;
    /// equal `z` keeps load order (later on top). Default 0. kInvalidArgument for an unknown handle.
    Status setSurfaceZ(DocumentHandle handle, int z);
    /// 0 for an unknown handle.
    int surfaceZ(DocumentHandle handle) const;
    /// How `handle`'s surface takes input (SurfaceInput; default Block). A modal is a Block
    /// surface whose content is a focus trap (Document::setFocusTrap). kInvalidArgument for an
    /// unknown handle.
    Status setSurfaceInput(DocumentHandle handle, SurfaceInput mode);
    /// Block for an unknown handle.
    SurfaceInput surfaceInput(DocumentHandle handle) const;
    /// The input device of local player `player` (0..kMaxPlayers-1). Player 0's device is also
    /// the Context's input device (setInputDevice: `@media (input)`, `<glyph>`). kInvalidArgument
    /// for a player index out of range.
    Status setPlayerDevice(uint8_t player, InputKind device);
    /// Mouse for a player never set (or out of range).
    InputKind playerDevice(uint8_t player) const;

    /// T-200 (27.12, 23.10 item 4): a system event. Every loaded Document that has the implicit
    /// document-level port gets it -- `system.loading` / `system.paused` / `system.resumed` /
    /// `system.disconnected` (event ports: bindings, `on:port(system.paused)` triggers and port
    /// listeners fire) or `system.language` / `system.platform` / `system.inputDevice` (string ports
    /// set to `value`) -- then every ContextListener::onSystemEvent. `kind` is one of loading,
    /// paused, resumed, disconnected, languageChanged (or its alias language), platform,
    /// inputDevice, back (T-240: `system.back`, an event -- the Android back key),
    /// reducedMotion (T-330: `system.reducedMotion`, a string port; `value` reduce / true / 1 or
    /// no-preference / false / 0 also sets DeviceInfo::reducedMotion through setDevice(), so
    /// `@media (prefers-reduced-motion)` follows; any other value is kInvalidArgument);
    /// kInvalidArgument (nothing is sent) for anything else. A Document has such a
    /// port when its `.xt` references it (the compiler declares it implicitly).
    Status postSystemEvent(std::string_view kind, std::string_view value = {});

    /// T-200 (13.8.3 `setWorldAnchor`): Document::setWorldAnchor on `handle`'s Document (a PortId
    /// belongs to one Document). kInvalidArgument for an unknown handle.
    Status setWorldAnchor(DocumentHandle handle, PortId port, float x, float y, float depth = 0,
                          bool onScreen = true);

private:
    friend class DispatchGuard;           // T-260a: m_dispatchDepth, m_pendingDocOps, runPendingDocOps().
    friend class Document;                // T-397: m_dispatchDepth, m_surfaceRequestsPending.
    friend struct RuntimePoolTestAccess;  // T-270a: Tests/unit reads m_compositionPool's size.

    /// Loads `blob` as the replacement of `handle`'s Document with this Context's state applied
    /// (reloadDocument() and hotReload()); `out` is not installed yet.
    Status prepareReload(DocumentHandle handle, IrBlob blob, bool takeOwnership,
                         std::unique_ptr<Document>& out);
    /// Swaps `doc` into `handle`'s slot and destroys the old Document. T-260a: while dispatching,
    /// queued instead (`doc` is moved into the queue).
    void installReload(DocumentHandle handle, std::unique_ptr<Document>& doc);
    /// T-260a: runs the queued unloads / reload installs in the order they were queued (the
    /// outermost DispatchGuard's destructor). T-397: the queued surface show / hide requests of
    /// the live Documents are applied before them. A reload whose slot was unloaded first is dropped;
    /// T-260b: so is one with an unload of the same handle queued after it (never installed).
    void runPendingDocOps();
    /// T-260b: `handle`'s slot was freed: the pointer capture, hover and keyboard surfaces stop
    /// naming it and its surface z / input mode go back to the defaults.
    void forgetSurface(DocumentHandle handle);
    /// T-260a: the Document at `index` (= handle - 1), or nullptr for an index out of range, an
    /// empty slot or a Document whose unload is queued (Document::m_unloading). Every lookup and
    /// walk of m_documents goes through it, so an unloading Document is treated exactly like an
    /// empty slot; only its memory goes later.
    Document* liveAt(size_t index) const;
    /// T-200: hotReload() of one IrReload.
    HotReloadResult hotReloadDocument(const IrReload& reload);
    /// T-194: the visible surfaces, topmost first (z descending, then later loaded first).
    void surfaceOrder(std::vector<DocumentHandle>& out) const;
    /// T-194: routes one queued InputEvent through the surface stack (SurfaceInput).
    void routeInput(const InputEvent& event);
    /// T-194: the surface keyboard / gamepad input goes to (0 = none).
    DocumentHandle keyTarget() const;
    /// T-194: the topmost surface taking a press at `pos` (0 = none).
    DocumentHandle pressTarget(Vec2 pos) const;
    /// T-170: rebinds every loaded Document's token layers (active theme + overrides).
    void applyTokenLayers();
    /// T-170: the active theme's token map, or nullptr.
    const TokenMap* activeThemeTokens() const;

    /// The Document input/cursor targets: the last (by load order, i.e. highest handle) still-
    /// loaded Document (Docs/tasks/T-32.md "目前只有一份 Document 接收：多份時以載入順序最後者為
    /// 前景；第一版如此"). nullptr if none are loaded.
    Document* foregroundDocument() const;
    /// Routes `doc`'s TimelineEvents to m_timelineListeners (T-172).
    void bindTimelineEvents(Document& doc);
    void bindHostEvents(Document& doc, DocumentHandle handle);
    /// The Document (foreground first) that has the group, or nullptr.
    Document* documentWithTimeline(std::string_view scopePath, std::string_view name) const;

    ContextConfig m_config;
    std::vector<std::unique_ptr<Document>> m_documents;  // index = handle - 1; unload leaves nullptr.
    // T-260a: how many dispatching calls (DispatchGuard) are on the stack, and the unloads
    // (`doc` null) and reload installs (`doc` = the new Document) they deferred, in call order.
    // A member, so its capacity is kept.
    struct PendingDocOp {
        DocumentHandle handle = 0;
        std::unique_ptr<Document> doc;
    };
    uint32_t m_dispatchDepth = 0;
    std::vector<PendingDocOp> m_pendingDocOps;
    /// T-397 (08 §8.17): some Document queued a requestShow() / requestHide() while dispatching;
    /// runPendingDocOps() applies them first.
    bool m_surfaceRequestsPending = false;
    Rect m_viewport{};
    float m_devicePixelRatio = 1;
    float m_clock = 0;
    std::vector<InputEvent> m_inputQueue;
    // §23.2 step 1: the previous frame's command list, swapped out of the caller's `out` at the
    // top of render() so no copy is ever made. m_damageBaseValid is false whenever it does not
    // hold the frame that is currently on screen (first frame, viewport/dpr change, reload, or a
    // render() that was not asked for damage).
    CommandList m_prevList;
    bool m_damageBaseValid = false;
    // T-263c: CommandList::frameSerial of the list the last render() produced (0 = none yet). The
    // list the host hands back is the damage base only when it still carries this serial.
    uint32_t m_lastFrameSerial = 0;
    // std::deque so pushInput's stored string_view (repointed at the pooled string, see .cpp)
    // stays valid across further insertions -- unlike std::vector, a deque never relocates
    // existing elements when it grows.
    std::deque<std::string> m_compositionPool;
    // T-393 (08.14): the ExternalDrop paths pushInput copied: the strings, then the views the
    // queued event's `paths` spans (built after the strings, never resized afterwards). A deque,
    // so an entry never moves; emptied with m_compositionPool.
    struct PathCopy {
        std::vector<std::string> strings;
        std::vector<std::string_view> views;
    };
    std::deque<PathCopy> m_pathPool;
    // T-38 (Docs/design/01-architecture.md §1.7): update()'s drainInput() destination used to be a
    // local vector. Under MSVC's Debug STL (_ITERATOR_DEBUG_LEVEL == 2, the Debug default),
    // constructing any std::vector -- even one nothing is ever pushed into (m_inputQueue is
    // usually empty) -- allocates a 16-byte _Container_proxy for debug iterator tracking, so a
    // local vector "allocates" on every update() call purely from being constructed. A member is
    // constructed once (with the Context) instead; update() clears it before each drainInput().
    std::vector<InputEvent> m_updateEventsScratch;
    // Docs/design/21-perf-tracing.md §21.2: off by default, so an untouched Context pays one
    // branch per phase scope and nothing else.
    PerfTracker m_perf;
    // T-172: host timeline listeners (id, listener); ids start at 1 and are never reused.
    std::vector<std::pair<uint32_t, TimelineListener>> m_timelineListeners;
    uint32_t m_nextTimelineListener = 1;
    // T-170: registered themes (std::map, so a TokenMap's address survives later insertions), the
    // active theme's name ("" = none) and setToken()'s overrides.
    std::map<std::string, TokenMap, std::less<>> m_themes;
    std::string m_activeTheme;
    TokenMap m_tokenOverrides;
    // T-176: setDevice()/setUiScale() state, handed to every Document on load.
    DeviceInfo m_device{};
    bool m_keyActivation = true;  // T-379: setKeyboardActivation().
    float m_uiScale = 1;
    // T-345: setUiScale()'s value and setUiScaleLimits(); m_uiScale is derived from them.
    float m_uiScaleBase = 1;
    float m_uiScaleMin = 0;
    float m_uiScaleMax = std::numeric_limits<float>::infinity();
    float m_uiScaleDpiFloor = 0;
    /// T-345: re-derives m_uiScale from the base, the limits and the DPR and hands a changed value
    /// to every Document.
    void applyUiScale();
    HostTargetMode m_hostTargetMode = HostTargetMode::Layer;  // T-343
    bool m_sceneStatic = false;                               // T-343
    // T-174: host listeners, in registration order.
    std::vector<ContextListener*> m_listeners;
    // T-179: setLocalizationProvider()/setLanguage() state, handed to every Document on load.
    ILocalizationProvider* m_localization = nullptr;
    std::string m_language;
    // T-178: the borrowed glyph provider, handed to every Document on load.
    IGlyphProvider* m_glyphProvider = nullptr;
    // T-191: setExternalPlaceholders(); render()'s external damage scratch.
    bool m_externalPlaceholders = false;
    std::vector<Rect> m_externalDamageScratch;
    // T-194: per surface (index = handle - 1) stacking and input mode; the pointer's capture
    // surface (between a press and its release), the surface under the hover and the surface that
    // took the last press (it keeps the keyboard focus); the players' devices.
    struct SurfaceState {
        int z = 0;
        SurfaceInput input = SurfaceInput::Block;
    };
    std::vector<SurfaceState> m_surfaces;
    DocumentHandle m_pointerCapture = 0;
    DocumentHandle m_hoverSurface = 0;
    // T-393 (08.14): the surface an external (OS) drag is over (0 = none).
    DocumentHandle m_externalDragSurface = 0;
    DocumentHandle m_focusSurface = 0;
    mutable std::vector<DocumentHandle> m_orderScratch;
    InputKind m_playerDevices[kMaxPlayers] = {};
};

}  // namespace xt
