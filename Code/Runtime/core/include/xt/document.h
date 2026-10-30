#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xt/animator.h"
#include "xt/command_list.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/perf.h"
#include "xt/rig.h"
#include "xt/scene_instance.h"
#include "xt/shader_table.h"
#include "xt/style_engine.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Document: a loaded `.xtir` document (Element/Text node tree, instantiated `anim` scenes,
/// timelines and their Animator playback state) plus its per-frame update/paint entry points.
/// No style cascade, layout or events yet (see Docs/tasks/T-18.md). See Docs/design/13-api.md
/// §13.1-13.2 (subset), Docs/design/01-architecture.md §1.2, §1.5-1.8.

namespace xt {

/// Opaque handle to a loaded Document, returned by `Context::loadDocument*()`; 0 is invalid.
using DocumentHandle = uint32_t;

/// Text shaping backend (see xt/text_shaper.h); forward-declared here for Document's use.
class TextShaper;
/// Resolves IR asset references to image/font data (see xt/asset_resolver.h).
class AssetResolver;
/// Decoded RGBA8 image (see xt/asset_resolver.h).
struct ImageData;
/// Host input-glyph resolver (see xt/context.h, T-178).
class IGlyphProvider;
/// A compiled setStyle() text shape (see xt/style_template.h, T-477).
struct StyleTemplate;
/// Forward declaration; the class is defined below.
class Document;
/// The Context that loaded a Document (see xt/context.h).
class Context;
class EventDispatcher;    // Code/Runtime/core/src/event_dispatcher.h (private; Document owns one via pimpl).
struct EditState;         // Code/Runtime/core/src/text_edit.h (private; Document owns the pool, see m_editStates).
struct CompositionState;  // Code/Runtime/core/src/composition.h (private; Document owns one, see m_composition).
struct TimelineGroupSet;  // Code/Runtime/core/src/timeline_group.h (private; Document owns one, see m_timelineGroups).
class TriggerRuntime;     // Code/Runtime/core/src/trigger_runtime.h (private; Document owns one, see m_triggers).
class BindingRuntime;     // Code/Runtime/core/src/binding_runtime.h (private; Document owns one, see m_bindings).
class RepeatController;   // Code/Runtime/core/src/repeat_controller.h (private; Document owns one, see m_repeats).
struct DocumentState;     // Code/Runtime/core/src/document_state.h (private; T-398 snapshot / hotReload carry-over).
namespace rig {
struct RigState;  // Code/Runtime/core/src/rig_eval.h (private; Document owns the pool, see m_rigs).
}  // namespace rig

/// A declared port (Docs/design/27-interaction.md §27.3.1, 13-api.md §13.8.1, T-175): 1 + its
/// index in the IR's `PORT` table; kInvalidPort (0) = none.
using PortId = uint32_t;
/// No port.
inline constexpr PortId kInvalidPort = 0;

/// One row of a Document's port table (13.8.1 `portTable()`, 23.10 node generation).
struct PortRecord {
    /// The port's id.
    PortId id = kInvalidPort;
    /// Declared name (`hp`).
    std::string name;
    /// The name lookupPort() resolves: the declared name for a document-level port, else
    /// `<declaring element id>.<name>` (a path id inside a symbol instance).
    std::string qualifiedName;
    /// `<instance id>.<name>` for a port exposed by a symbol instance, else empty.
    std::string exposedName;
    /// Value type (ir_pod.h PortValueType).
    PortValueType type = PortValueType::Number;
    /// Direction.
    PortDir dir = PortDir::In;
    /// Exposed to its symbol instance's parent (`expose:false` clears it).
    bool exposed = true;
    /// True for a port declared outside any symbol instance.
    bool documentLevel = true;
    /// The declaring element's id ("" when it has none).
    std::string elementId;
    /// Struct / list entry fields (name, type), in declaration order.
    std::vector<std::pair<std::string, PortValueType>> fields;
    /// Enum value names, in declaration order (the Enum ordinal indexes this; also a
    /// `list<enum(...)>` entry's value names).
    std::vector<std::string> enumValues;
    /// T-381: a List port's entry type: 0 for `list<struct{...}>` (see `fields`), else the
    /// PortValueType of a `list<number|string|bool|color|vec2|vec3|enum(...)>` entry.
    PortValueType entryType = static_cast<PortValueType>(0);
    /// T-391 (27 §27.3.4): one record per entry of `fields` whose type is List, in field order:
    /// that list's entry type (`name` = the field name, `type` List, `entryType`, `fields`,
    /// `enumValues` and, recursively, `fieldLists`; the other members are unused).
    std::vector<PortRecord> fieldLists;
};

/// What a PortEvent reports.
enum class PortEventKind : uint8_t {
    Changed = 0,      ///< The port's value changed (set by the host or by the UI).
    Emit = 1,         ///< An `emit(<port>)` action (UI -> host), with the port's current value.
    ListChanged = 2,  ///< A list port changed (listSet/listInsert/listRemove/listUpdate).
};

/// A port notification (Document::addPortListener). `value` is only valid during the callback
/// (a Struct/List points into the port's storage).
struct PortEvent {
    /// What happened.
    PortEventKind kind = PortEventKind::Changed;
    /// The port.
    PortId port = kInvalidPort;
    /// The port's value after the change (Emit: at the emit).
    Value value{};
    /// ListChanged: the entry index the operation touched (0 for listSet).
    uint32_t index = 0;
    /// The Document's instance id (setInstanceId).
    uint32_t instanceId = 0;
    // --- T-177 (27.3.4): tail additions. ---
    /// Emit from inside a `<repeat>` instance: its list entry index; 0xFFFFFFFF otherwise.
    uint32_t itemIndex = 0xFFFFFFFFu;
    /// Emit from inside a `<repeat>` instance: that entry (a Struct, valid during the callback).
    Value item{};
    // --- T-394 (08.15): tail addition. ---
    /// Changed: true when the document itself wrote the port (a trigger's `set()` or a binding),
    /// false when the host did (setPort / setPortText / the list API). HostPorts::addInputListener
    /// reports only the former.
    bool fromDocument = false;
};

/// Port listener (Document::addPortListener).
using PortListener = std::function<void(const PortEvent&)>;
/// A host converter (13.8.1 registerConverter): the chain's current value and the converter's
/// arguments (numbers as Float, words and strings as StringRef, ports as their values).
using ConverterFn = std::function<Value(const Value& input, std::span<const Value> args)>;
/// A host function behind a `call` port: the UI side calls it with `emit(<port>)`, passing the
/// port's current value; the result becomes the port's new value.
using PortCallHandler = std::function<Value(std::span<const Value> args)>;

/// T-179 (Docs/design/13-api.md §13.8.2, 07-text.md §7.8): the host's string table for `@key`
/// text. Context::setLocalizationProvider() borrows one for every loaded Document.
class ILocalizationProvider {
public:
    virtual ~ILocalizationProvider() = default;
    /// The UTF-8 text of `key` (without its `@`) in `lang` (a BCP 47 tag, possibly empty). An
    /// empty result means "not translated": the key itself is shown. The returned view only has
    /// to stay valid until the next resolve() call; the caller copies it.
    virtual std::string_view resolve(std::string_view key, std::string_view lang) = 0;
};

/// T-211: the Document::warnings() entry (once per document) for a keyframe / transition / write
/// that addressed `effect.<n>.<param>` on an element with no n-th effect; the write was ignored.
inline constexpr std::string_view kWarnEffectMissing =
    "effect missing: a keyframe addressed an effect the element does not have (ignored)";

/// T-327 (05 §5.9.8): the Document::warnings() entry (once per document) when a
/// `background-repeat` needed more than 4096 tiles and was drawn stretched instead.
inline constexpr std::string_view kWarnBackgroundTiles =
    "background-repeat needs more than 4096 tiles; the background image was drawn stretched";

/// 06 §6.15 (T-338): the Document::warnings() entry (once per document) when a grid item was
/// placed past its grid's explicit tracks and clamped to the last track.
inline constexpr std::string_view kWarnGridPlacementClamped =
    "grid item placed past the explicit grid tracks; clamped to the last track";

/// T-387 (13 §13.9): where ElementRef::scrollIntoView() puts the child inside the scroll
/// container's visible area.
enum class ScrollAlign : uint8_t {
    Nearest,  ///< Scroll as little as possible: already fully visible = no change.
    Start,    ///< The child's top / left edge at the visible area's top / left.
    Center,   ///< The child's center at the visible area's center.
    End,      ///< The child's bottom / right edge at the visible area's bottom / right.
};

/// T-397 (08 §8.17): an editable `<input>`'s selection, as UTF-8 byte offsets into its value with
/// start <= end (start == end: a caret, no selection).
struct TextSelection {
    uint32_t start = 0;  ///< First selected byte.
    uint32_t end = 0;    ///< One past the last selected byte.
};

/// T-397 (08 §8.17): one `<repeat>` level around an element (Document::repeatChain()).
struct RepeatRef {
    /// The `<repeat>`'s id (a nested copy's path id, e.g. `Rows/2/Sub`); a view into the
    /// Document's strings, valid while the Document lives.
    std::string_view repeatId;
    /// The list entry index of the instance the element is in.
    uint32_t index = 0;
};

/// Lightweight, generation-checked handle to an Element node. See Docs/design/13-api.md §13.3.
struct ElementRef {
    /// The owning Document; nullptr for a default-constructed (invalid) ref.
    Document* doc = nullptr;
    /// The referenced node's id within doc's NodeArena.
    NodeId node = kInvalidNode;
    /// The node's arena generation at the time this ref was obtained; used by valid() to detect a
    /// node that was destroyed and whose slot was reused.
    uint32_t generation = 0;

    /// True when doc/node/generation still refer to a live node.
    bool valid() const;

    // Identity / attributes (Docs/design/13-api.md §13.3, T-35). Every accessor returns an empty
    // view (or a default-constructed value) when !valid().
    /// This element's tag name (e.g. "div", "img"); empty when !valid().
    std::string_view tag() const;
    /// This element's `id` attribute value; empty when unset or !valid().
    std::string_view id() const;
    /// The element's value for `name`, looked up among the attributes the IR carried plus any
    /// later setAttribute(). `id`/`class`/`style` are stored as their own fields and are NOT
    /// visible here (use id()/hasClass()); an absent attribute returns an empty view.
    std::string_view attribute(std::string_view name) const;
    /// 05.2/13.3's "attributes with a meaning": `id` (element id), `class` (the whole class list),
    /// `style` (setStyle), `disabled`/`checked` ("true" -> the matching ElementState), `value`
    /// (setValue on an editable `input`). Anything else is stored verbatim. Returns
    /// kUnsupported for `src` (assets are resolved at compile time; see Docs/tasks/T-35.md),
    /// kInvalidArgument when !valid() or when `style`'s text has invalid declarations (the valid
    /// ones still apply), else kOk.
    Status setAttribute(std::string_view name, std::string_view value);

    /// The text of this element's first Text child (its EditState value for an editable `input`).
    /// Empty when there is none. T-270b: the view is valid only until this element's text changes
    /// -- setText(), or a `bind:text` binding run by update() or a port write, both rewriting the
    /// same storage in place -- or the element is removed. Copy it to keep it across one of those
    /// or across frames.
    std::string_view text() const;
    /// Replaces that text (creating the Text child when the element has none), marking
    /// kDirtyLayout. Routed through setValue() for an editable `input`.
    void setText(std::string_view text);

    /// Parses `cssDeclarations` ("width: 200px; color: red") with core's runtime mini parser and
    /// stores the result as this element's runtime declaration list, which cascades exactly like
    /// the `style` attribute's (same specificity, applied after it). Replaces any previous
    /// declaration of the same property, keeps the others, and marks kDirtyStyle (05.8's
    /// propagation), so transitions trigger the same way they do for any other style change.
    /// Returns kUnsupported when the text contains a `transition`/`animation` declaration (those
    /// are compile-time only, 05.6; the declaration is ignored, the rest still applies),
    /// kInvalidArgument when a declaration could not be parsed (likewise ignored), else kOk.
    Status setStyle(std::string_view cssDeclarations);
    /// T-477 (27 §27.4.2 item 4): setStyle(`cssDeclarations`) for a text of `tmpl`'s shape (its
    /// literals around one number per hole): the declarations are written from the compiled
    /// template, without the parse, with the same result. kNotFound (nothing done) for a text of
    /// another shape -- call setStyle() then; otherwise setStyle()'s statuses.
    Status setStyleTemplate(const StyleTemplate& tmpl, std::string_view cssDeclarations);

    /// Writes `prop` directly (`sticky == false`: layer 0, so the next style resolve or animation
    /// frame overwrites it) or as an Animator override (`sticky == true`: layer 3, 04.7, which
    /// wins over everything until clearOverride()). No-op when `prop` does not apply to an Element
    /// or `value`'s type does not match the property's.
    void setProperty(PropId prop, const Value& value, bool sticky = false);
    /// Drops the Animator override and marks the element kDirtyStyle, so the next update()
    /// re-resolves the cascade and restores the property's styled value (the override's last
    /// value would otherwise simply stay put -- see the comment in element_ref.cpp).
    void clearOverride(PropId prop);
    /// The property's current final value (the arena's, i.e. after animations/overrides).
    Value property(PropId prop) const;

    /// Cascaded value for either an animatable PropId (0-108, delegates to `property()`) or a
    /// non-animatable StyleProp number (160-188, Code/Runtime/core/src/style_decode.h): read straight off this
    /// element's `ComputedStyle`. Only `ComputedStyle` fields that are themselves an `enum class`
    /// are supported for the StyleProp branch (encoded via `Value::ofEnum`); every other
    /// StyleProp -- and an unrecognized `propOrStyleProp` -- returns a default-constructed
    /// `Value{}` (`!valid()` too). Used by the editor's property panel and `Document::style()`'s
    /// cascade fallback (Docs/design/17-editor.md §17.7, Docs/tasks/T-59.md).
    Value computedStyleValue(uint16_t propOrStyleProp) const;

    // Effects (Docs/design/18-effects.md §18.4, Docs/tasks/T-51b.md). The node's current, final
    // effect stack (after the cascade -- see StyleEngine::resolve's effect cascade), not the
    // declaration that produced it.
    /// Number of effects in this element's current effect stack; 0 when !valid().
    uint32_t effectCount() const;
    /// A copy of effect stack slot `index`; a default-constructed (kind == None) EffectInstance
    /// when `index >= effectCount()` or !valid().
    EffectInstance effect(uint32_t index) const;
    /// Equivalent to `setProperty(effectParam(effectIndex, paramIndex), value, sticky = false)`
    /// followed by `markDirty(kDirtyPaint)`. No-op when !valid().
    void setEffectParam(uint32_t effectIndex, uint32_t paramIndex, const Value& value);
    /// Declares a new runtime `effect` sequence (mirrors setStyle()'s cascade mechanism: appended
    /// to Document's runtime effect pool and cascaded like any other `effect` declaration, with
    /// inline specificity). `stops`' entries are referenced by `effects`' `stopOffset`/`stopCount`
    /// (18.2's per-entry gradient-stop convention), not global positions. kInvalidArgument when
    /// `effects.size() > 16` (18.2's per-node effect maximum) or !valid(), else kOk.
    Status setEffects(std::span<const EffectInstance> effects, std::span<const GradientStop> stops);

    /// Border-box AABB in viewport coordinates (the four corners through the world transform).
    Rect boundingBox() const;
    /// This element's layout box (Docs/design/06-layout.md §6.2), all-zero when !valid().
    LayoutBox layoutBox() const;

    /// Moves the document focus to this element (Document::setFocus). T-397 (08 §8.17):
    /// kUnsupported, without touching the focus, for an element that is not laid out (`display:
    /// none` on it or an ancestor) -- show it first, or focus it from `on:show="focus()"`;
    /// kInvalidArgument when !valid(); else kOk. T-399 (08 §8.18): when a pending change may show
    /// it this frame (a port set since the last update(), a restyle of the hiding element), kOk
    /// and the focus moves in the next update() after style / layout -- or, still `display: none`
    /// then, stays where it was (reported once in Document::warnings()).
    Status focus();

    /// A named node inside this `anim` element's instantiated scene (e.g. a Lottie layer name), or
    /// kInvalidNode when this is not an `anim` element or the scene has no such node.
    NodeId sceneNode(std::string_view name) const;
    /// The autoplay-created Animator instance id, 0 when there is none.
    uint32_t defaultAnimation() const;

    /// Tree navigation. firstChild()/nextSibling() skip non-Element nodes (Text children and an
    /// `anim` element's instantiated scene subtree), so they only ever yield Elements.
    /// This element's parent Element; invalid for the root or when !valid().
    ElementRef parent() const;
    /// This element's first Element child; invalid when it has none.
    ElementRef firstChild() const;
    /// The next Element sibling; invalid when this is the last child.
    ElementRef nextSibling() const;

    // Minimal class/state API (T-24; full host API is T-35). Each mutator marks kDirtyStyle on
    // this element, its ancestors (automatic, see NodeArena::markDirty) and its descendants
    // (explicit; see Docs/design/05-style-and-css.md §5.8) so `:hover`-style descendant/child
    // selectors re-resolve too. No-ops (besides the false return, where applicable) when !valid().
    /// True when the element's class list contains `cls`.
    bool hasClass(std::string_view cls) const;
    /// Inserts `cls` into the element's sorted class list if absent. Returns true if it changed
    /// anything (i.e. the class was actually added).
    bool addClass(std::string_view cls);
    /// Removes `cls` from the element's class list if present. Returns true if it changed
    /// anything.
    bool removeClass(std::string_view cls);
    /// Adds `cls` when absent, removes it when present. Returns true if the class is set
    /// afterwards.
    bool toggleClass(std::string_view cls);

    /// True when every bit set in `state` is currently set on this element.
    bool hasState(ElementState state) const;
    /// Only kStateDisabled/kStateChecked may be set through this API (the others -- hover/active/
    /// focus -- are pointer/focus-driven, T-32/T-33). Returns true if it changed anything; false
    /// (no-op) for any other bit, including when !valid().
    bool setState(ElementState state, bool on);

    // Editable `input` value API (T-33; Docs/design/13-api.md, Docs/tasks/T-33.md "字串池").
    /// The element's runtime value. Empty for anything but an editable `input[type=text]`. The
    /// view points into the Document's EditState pool and is invalidated by the next setValue()
    /// or by any editing key/text event on that element.
    std::string_view value() const;
    /// Replaces the value, collapsing the caret to the end. Dispatches a (bubbling) Change and
    /// marks kDirtyLayout only when the value actually changed. No-op for a non-editable element.
    /// T-397 (08 §8.17): works while the element is not laid out (`display: none`) too.
    void setValue(std::string_view text);
    /// T-397 (08 §8.17): selects the whole value of this editable `<input>` (caret at the end).
    /// kUnsupported for any other element, kInvalidArgument when !valid(), else kOk.
    Status selectAll();
    /// T-397 (08 §8.17): selects bytes [`start`, `end`) of the value, both clamped to its length
    /// and moved back to a UTF-8 character start; `start` > `end` collapses to a caret at `end`.
    /// The caret goes to `end`. kUnsupported / kInvalidArgument as selectAll(), else kOk.
    Status setSelection(uint32_t start, uint32_t end);
    /// T-397 (08 §8.17): the current selection ({caret, caret} when nothing is selected); {0, 0}
    /// for a non-editable element or when !valid().
    TextSelection selection() const;

    // Scroll API (Docs/design/13-api.md §13.9, 08 §8.12, T-387). Only for an element with an
    // `overflow: scroll` / `auto` axis; every offset is clamped to [0, maxScroll()] (0 on an axis
    // that is neither), and a change dispatches one bubbling EventType::Scroll (`on:scroll`).
    // The maximum comes from the last layout, so call them after an update().
    /// Scrolls to (`x`, `y`) px. kUnsupported for an element with no scroll / auto axis,
    /// kInvalidArgument when !valid(), else kOk (also when the clamped offset did not change).
    Status scrollTo(float x, float y);
    /// scrollTo(scrollPos() + (`dx`, `dy`)).
    Status scrollBy(float dx, float dy);
    /// The current (ScrollX, ScrollY); zero when !valid().
    Vec2 scrollPos() const;
    /// The largest offset per axis (the layout box's content overflow on a scroll / auto axis, 0
    /// otherwise); zero when !valid().
    Vec2 maxScroll() const;
    /// Scrolls this container so `child` (a descendant) is visible per `align`, on each scroll /
    /// auto axis, from the layout boxes (transforms are ignored). kUnsupported for an element
    /// with no scroll / auto axis, kInvalidArgument when either ref is invalid or `child` is not
    /// a descendant of this element, else kOk.
    Status scrollIntoView(ElementRef child, ScrollAlign align = ScrollAlign::Nearest);
    /// T-394 (08.15): scrolls this container so entry `index` of the `<repeat>` with id `repeatId`
    /// (inside this container) is visible per `align`, vertically (the horizontal offset is kept
    /// for a virtualized repeat). Runs Document::layoutIfDirty() first, so an entry inserted this
    /// frame is reachable. A virtualized repeat uses the measured entry height x `index`; any other
    /// uses the entry's instance box (scrollIntoView(child)). kUnsupported without a scroll / auto
    /// axis; kInvalidArgument when !valid(), the repeat is unknown or not inside this element,
    /// `index` is past its list, or a virtualized entry height is not measured yet.
    Status scrollIntoView(std::string_view repeatId, uint32_t index,
                          ScrollAlign align = ScrollAlign::Nearest);
};

/// A loaded `.xtir` document instance; see the file-level comment above for scope.
class Document : public IExternalTexture {
public:
    /// Releases all resources owned by the document (arena, animator, engines, listeners, etc.).
    ~Document();
    /// Non-copyable.
    Document(const Document&) = delete;
    /// Non-copyable.
    Document& operator=(const Document&) = delete;

    /// Opens `blob` (IrView::open with verifyCrc = false), then builds the Element/Text tree,
    /// instantiates `anim` scenes and registers timelines. `takeOwnership == true` copies `blob`'s
    /// bytes into Document-owned storage; otherwise the caller keeps it alive for the Document's
    /// whole lifetime. On failure `out` is left untouched and a Status other than kOk is returned.
    static Status load(IrBlob blob, bool takeOwnership, TextShaper* shaper, AssetResolver* assets,
                       std::unique_ptr<Document>& out);

    /// The document's immutable IR view (the loaded blob's parsed structure).
    const IrView& ir() const { return m_ir; }
    /// T-260c: this Document's load serial -- ir().loadSerial(), the count IrView::open() takes
    /// once per opened blob (from 1; 0: never loaded). Every Document a Context installs
    /// (loadDocument(), reloadDocument(), hotReload()) was loaded from its own open(), so two
    /// Documents never share a serial, even when the second lives at the first one's address
    /// under the same handle. Context::documentSerial() is the per-handle lookup.
    uint32_t loadSerial() const { return m_ir.loadSerial(); }
    /// T-407 (43 §43.3): the project's behavior version, IR META `behavior` (0 without it). From
    /// kBehaviorFlowSignalCoalescing (xt/version.h) on, bindings, continuous-event triggers and a
    /// flow's value signals are coalesced per frame (27 §27.4.1); below it every event is handled
    /// at once, as before T-389.
    uint32_t behavior() const { return m_ir.meta().behavior(); }
    /// T-203 (Docs/design/18-effects.md §18.7, the host contract): the custom effect `asset`'s
    /// ShaderRef (HLSL `source`, GLSL, parameter table, `expandPx`, `fallback`), or nullptr when
    /// `asset` is not a `.xtfx` asset of this document. Its text ranges index shaderText(), its
    /// parameter range shaderParams().
    const ShaderRef* shaderRef(AssetId asset) const { return m_shaderTable.find(asset); }
    /// T-203: the parameter pool ShaderRef::paramOffset/paramCount index.
    std::span<const ShaderParamDesc> shaderParams() const { return m_shaderTable.params; }
    /// T-203: the text pool ShaderRef's source/glsl/names ranges index.
    std::string_view shaderText() const { return m_shaderTable.text; }
    /// The document's node arena (mutable access).
    NodeArena& arena() { return m_arena; }
    /// The document's node arena (read-only access).
    const NodeArena& arena() const { return m_arena; }

    /// The document root (DOCT elements[0]'s Element node).
    ElementRef root() const;
    /// T-210 (Docs/design/25-editor-blueprint.md §25.4.1 item 1, lint `layout.fixedRoot`): the UI
    /// root of a pre-contract UI -- `body`'s only element child, else `body` itself -- whose
    /// cascaded `width` or `height` is a px length. The child is checked before `body`. Invalid
    /// when neither is fixed (the root fills the viewport). Reads the resolved styles, so call it
    /// after an update(). T-481: the first of fixedRoots() (no board), or invalid.
    ElementRef fixedRoot() const;
    /// T-481 (25 §25.4.1 item 1 "multiple roots"): every fixed UI root -- the T-210 rules, but with
    /// several element children of `body`, each one whose px `width` and `height` cover the design
    /// board (design px; `>= board.x - 1`, `>= board.y - 1`), in document order; `{body}` when
    /// none and `body` is fixed. The core does not know the design size: without a (positive)
    /// board no child of several counts. Call it after an update().
    std::vector<ElementRef> fixedRoots(Vec2 board = Vec2{0, 0}) const;
    /// O(n) linear search; no match (or no element carries `id`) returns an invalid ref.
    ElementRef byId(std::string_view id) const;
    /// DOCT element index -> ElementRef; out-of-range returns an invalid ref.
    ElementRef elementAt(uint32_t docElementIndex) const;
    /// StringId -> text. Ids below kRuntimeStringBase come from the immutable IR string table,
    /// ids at or above it from this Document's runtime string pool (see intern()), ids at or above
    /// kRuntimeTextBase from the Text nodes' own runtime text (ElementRef::setText; such a view is
    /// valid until that node's text changes or the node is removed -- T-270b: so not across a
    /// setText(), an update() or a port write, which may run a `bind:text` binding; copy it to keep
    /// it. Such an id is no key for the text either: the slot is overwritten in place, and reused by
    /// another node once this one is removed). An unknown id (including kInvalidString) returns an
    /// empty view.
    std::string_view string(StringId id) const;
    /// Docs/design/13-api.md §13.2's runtime string pool (01.7's exception rule): returns `s`'s
    /// existing StringId when the IR string table or the runtime pool already holds it, otherwise
    /// appends it to the runtime pool and returns its new id (kRuntimeStringBase + index). The
    /// returned id stays valid for this Document's lifetime; the text it points at is stable
    /// (std::deque, never reallocating its elements).
    StringId intern(std::string_view s);
    /// Docs/design/18-effects.md §18.4/T-51b: appends `effects`/`stops` to this Document's
    /// runtime effect declaration pool (a separate, append-only pool from NodeArena::effects(),
    /// which holds each node's final assigned stack), rebasing each entry's stopOffset/stopCount
    /// from being relative to `stops` to indexing the pool's own stop storage -- mirrors
    /// NodeArena::assignEffects()'s rebasing contract. Returns the ListRef Value to cascade for
    /// it: `value.sid` is `kRuntimeEffectOffsetBit | (the appended sequence's start index)`
    /// (mirrors intern()'s kRuntimeStringBase-tagged StringId), `value.floatCount` is
    /// `effects.size()` (a ListRef Value has no second u32 slot the way an IrDecl's raw bytes do,
    /// so the count rides along in that otherwise-unused field; see RuntimeStyleDecl). The pool
    /// only grows -- 0.2 does not reclaim a sequence that stops being referenced by any
    /// runtimeDecls (a known limitation, like compactEffects() not touching this pool either) --
    /// so a returned offset never becomes invalid. ElementRef::setStyle's `effect:` declarations
    /// and ElementRef::setEffects() do not use it (T-270a): they write into one segment per element
    /// that is reused on every change and released with the element.
    Value declareRuntimeEffects(std::span<const EffectInstance> effects, std::span<const GradientStop> stops);
    /// The runtime effect declaration pool appended to by declareRuntimeEffects(); bound into
    /// StyleEngine at load time (document_load.cpp's StyleEngine::setRuntimeEffects call) so the
    /// cascade can resolve a runtime `effect` ListRef's high-bit-tagged offset (xt/node.h's
    /// kRuntimeEffectOffsetBit).
    const std::vector<EffectInstance>& runtimeEffects() const { return m_runtimeEffects; }
    /// The gradient-stop pool referenced by runtimeEffects()' GradientMask entries.
    const std::vector<GradientStop>& runtimeEffectStops() const { return m_runtimeEffectStops; }
    /// Docs/design/05-style-and-css.md §5.5c/T-115: appends `gradient` to this Document's runtime
    /// `background-gradient` declaration pool and returns the ListRef Value to cascade for it --
    /// `value.sid` is `kRuntimeEffectOffsetBit | index` and `value.floatCount` is 1 (0 for
    /// `GradientKind::None`, which needs no pool entry). The append-only pool mirrors
    /// declareRuntimeEffects()' (see there), including its "never reclaimed" limitation.
    /// ElementRef::setStyle's `background-gradient:` declarations do not use it (T-270c): they
    /// write into two slots per element, alternately, released with the element.
    Value declareRuntimeGradient(const ElementGradient& gradient);
    /// Docs/design/17-editor.md §17.6/T-123: appends `specs` to this Document's runtime
    /// `animation` declaration pool and returns the ListRef Value to cascade for it --
    /// `value.sid` is `kRuntimeEffectOffsetBit | (the appended sequence's start index)` and
    /// `value.floatCount` is `specs.size()` -- the same append-only, never-reclaimed pattern as
    /// declareRuntimeEffects() above. An empty `specs` (CSS `animation: none`) needs no pool
    /// entry and returns a count-0 ListRef. ElementRef::setStyle's `animation:` declarations --
    /// also how xt_flow's `playAnimation` element port plays a `@keyframes` -- do not use it
    /// (T-270c): like `background-gradient`, they write into two slots per element, alternately,
    /// so the same declaration made again still changes the offset and restarts the animation.
    Value declareRuntimeAnimations(std::span<const IrAnimationSpec> specs);
    /// The runtime `animation` declaration pool appended to by declareRuntimeAnimations(); bound
    /// into StyleEngine at load time (StyleEngine::setRuntimeAnimations).
    const std::vector<IrAnimationSpec>& runtimeAnimations() const { return m_runtimeAnimations; }
    /// Appends `message` to warnings(). Used by ElementRef::setStyle for an `animation` naming a
    /// `@keyframes` this document does not have (T-123).
    void addWarning(std::string message);
    /// The runtime `background-gradient` declaration pool appended to by declareRuntimeGradient();
    /// bound into StyleEngine at load time (StyleEngine::setRuntimeGradients).
    const std::vector<ElementGradient>& runtimeGradients() const { return m_runtimeGradients; }
    /// 05 §5.9.8 (T-332): the runtime `clip-path` shape pool written by ElementRef::setStyle's
    /// `clip-path:` declarations (two slots per element, like `background-gradient`); bound into
    /// StyleEngine at load time (StyleEngine::setRuntimeClipShapes).
    const std::vector<ElementClipShape>& runtimeClipShapes() const { return m_runtimeClipShapes; }

    /// T-170 (Docs/design/13-api.md §13.8.2): binds the token layers `var(--name)` declarations
    /// resolve against before this document's own `VARS` table -- `overrides` first, then
    /// `theme`; either may be null -- and marks every element that matched a `var()` declaration
    /// (plus its subtree) kDirtyStyle, so the next update() re-resolves them without recompiling
    /// or reloading anything. Context calls this on load, setTheme(), defineTheme() and
    /// setToken(); both maps must outlive this Document or be rebound.
    void setTokenLayers(const TokenMap* theme, const TokenMap* overrides);

    /// T-179 (13.8.2 `setLanguage`, 07.8): re-resolves every `@key` text node and attribute
    /// through `provider` (null, or an empty result, shows the key itself) for `lang`. A node whose
    /// text or attribute the host replaced since the last resolve (setText/setAttribute) keeps the
    /// host's value. When `lang` differs from language(), every element is also marked kDirtyStyle
    /// (`:lang()` prefers the document language) and every Text node drops its shaping cache and is
    /// marked kDirtyLayout (the `@font-fallback` chain follows the language). Context calls this on
    /// load, setLanguage() and setLocalizationProvider(); `provider` must outlive this Document or
    /// be rebound.
    void setLocalization(ILocalizationProvider* provider, std::string_view lang);
    /// The language last given to setLocalization(); empty until then.
    std::string_view language() const { return m_language; }
    /// T-179: the localization key of `element`'s text (without `@`), or an empty view when that
    /// text is not an `@key` or the host has since replaced it.
    std::string_view localizationKey(ElementRef element) const;

    /// Docs/design/05-style-and-css.md §5.3's selector subset -- `tag`, `.class`, `#id`, `*`,
    /// `:hover/:active/:focus/:disabled/:checked`, descendant (space) and child (`>`)
    /// combinators -- matched against every Element in the tree, in preorder. `out` is cleared
    /// first. Selector groups (`,`) and everything 05.3 lists as unsupported (attribute
    /// selectors, `:nth-*`, `::before`, `:not()`, `+`) return kInvalidArgument with an empty
    /// `out`.
    Status query(std::string_view selector, std::vector<ElementRef>& out);

    /// Creates a detached Element (parent == kInvalidNode) with `tag` interned, default style and
    /// tag-derived focusability. It takes part in nothing -- style, layout, hit testing -- until
    /// appendChild()/insertBefore() attaches it. `img`/`anim` tags get no asset and no scene
    /// (those are resolved at compile time; ElementData::scene stays kInvalidScene).
    ElementRef createElement(std::string_view tag);
    /// Appends `child` (which must be valid, detached and from this Document) to `parent`. Marks
    /// `parent` kDirtyTree|kDirtyLayout and the whole attached subtree kDirtyStyle (first resolve).
    /// No-op when the preconditions do not hold, and (T-255b) when the insert would put a node at
    /// depth kMaxElementDepth or deeper (the bound a loaded tree has).
    void appendChild(ElementRef parent, ElementRef child);
    /// Same, inserting before `before` (which must be a current child of `parent`; an invalid
    /// `before` behaves like appendChild).
    void insertBefore(ElementRef parent, ElementRef child, ElementRef before);
    /// Destroys `element` and its whole subtree after cleaning up everything the Document tracks
    /// per node: listeners, CSS/autoplay animation instances, transitions and overrides, EditState
    /// values, instantiated `anim` scenes, and the focus/hover/active state (with the Blur/Leave
    /// events that implies). Every ElementRef into the destroyed subtree is !valid() afterwards.
    void remove(ElementRef element);

    // Animation (see Docs/design/13-api.md §13.2).
    /// Starts a new Animator instance per `params`; returns its instance id.
    uint32_t play(const PlayParams& params);
    /// Stops and removes the Animator instance `id`. No-op for an unknown id.
    void stop(uint32_t id);
    /// Pauses the Animator instance `id` in place. No-op for an unknown id.
    void pause(uint32_t id);
    /// Resumes a previously paused Animator instance `id`. No-op for an unknown id.
    void resume(uint32_t id);
    /// Seeks the Animator instance `id` to local time `localT` (seconds). No-op for an unknown id.
    void seek(uint32_t id, float localT);
    /// Sets the Animator instance `id`'s playback speed multiplier. No-op for an unknown id.
    void setSpeed(uint32_t id, float speed);
    /// True while the Animator instance `id` is playing (not paused, stopped or finished).
    bool isPlaying(uint32_t id) const;
    /// Docs/tasks/T-56.md: the Animator instance `id`'s current local time (seconds); 0 for an
    /// invalid/unknown id (Animator::localTime's own contract).
    float animationTime(uint32_t id) const;
    /// Docs/tasks/T-70.md: the wall-clock length of one iteration of the Animator instance `id`'s
    /// timeline, in seconds; 0 for an unknown id. This is NOT the same domain as `seek()`/
    /// `animationTime()`'s `localT`: a compiled `@keyframes` timeline is normalized (duration 1)
    /// and StyleEngine plays it with `durationScale` = the declared `animation-duration`, so this
    /// returns that duration while `localT` stays in 0..1; an `<anim>` scene timeline is already in
    /// seconds and plays at `durationScale` 1, so the two agree there. Editor-only accessor (the
    /// timeline panel's playhead is in seconds, Docs/design/17-editor.md §17.5), like
    /// `animationsOf()`; not part of 13-api.md's public surface.
    float animationTimelineDuration(uint32_t id) const;
    /// Docs/tasks/T-70.md: the Animator instance `id`'s playback speed multiplier; negative for a
    /// `reverse`/`alternate-reverse` CSS animation, which runs its timeline backwards. 0 for an
    /// unknown id. Editor-only accessor, same rationale as `animationTimelineDuration()`.
    float animationSpeed(uint32_t id) const;
    /// Registers `callback` to run once when the Animator instance `id` finishes.
    void onAnimationFinished(uint32_t id, std::function<void()> callback);

    // `@timeline` groups (Docs/design/04-timeline.md §4.10.1-4.10.2, 13-api.md §13.8.2,
    // Docs/tasks/T-172.md). `scopePath` is "" for the document root's timelines, otherwise the
    // scope element's id. Group time is in seconds; the members follow it exactly (one shared
    // clock), and `<anim sync>` clips inside the scope are seeked with it.
    /// Plays the group from its start (its end when `reverse`, or `fromMarker`). Restarts a group
    /// that is already playing. kNotFound for an unknown group or marker, kInvalidArgument for a
    /// non-positive rate.
    Status playTimeline(std::string_view scopePath, std::string_view name, const PlayOptions& options = {});
    /// Stops the group and releases its members (the elements return to their styled values). No
    /// End event. kNotFound for an unknown group.
    Status stopTimeline(std::string_view scopePath, std::string_view name);
    /// Moves the group to time `t` (clamped to 0..duration), keeping it playing or paused as it
    /// was; a group that is not active is activated paused at `t`. Markers crossed in the play
    /// direction fire on the next update(). kNotFound for an unknown group.
    Status seekTimeline(std::string_view scopePath, std::string_view name, float t);
    /// The group's current time in seconds, or -1 for an unknown group.
    float timelineTime(std::string_view scopePath, std::string_view name) const;
    /// True while the group is playing (not stopped, ended or merely seeked).
    bool isTimelinePlaying(std::string_view scopePath, std::string_view name) const;
    /// Receives every End/Marker TimelineEvent of this Document (after the bubbling
    /// TimelineEnd/TimelineMarker event on the scope element). Context installs one to fan out to
    /// its own listeners; an empty function clears it.
    void setTimelineListener(TimelineListener listener);

    // Custom states (Docs/design/27-interaction.md §27.1, 13-api.md §13.8.2, T-174). `name` must be
    // one of the document's declared custom states (STAT); kNotFound otherwise, kInvalidArgument
    // for an invalid element. Turning a state on clears the other members of its state group; each
    // change fires the element's `state(<Name>).enter` / `.exit` triggers and re-resolves the
    // element's subtree style (`:state(<Name>)` selectors).
    // T-390 (27.3.2): the name `Disabled` is the built-in ElementState kStateDisabled here
    // (set / cleared / read, no enter / exit triggers), whether or not a custom state is declared.
    Status setCustomState(ElementRef element, std::string_view name, bool on);
    bool hasCustomState(ElementRef element, std::string_view name) const;
    /// T-359: fires `element`'s own triggers for `event` directly (no hit test, no bubbling), as if
    /// that input had reached it; the actions run before this returns. Only Click, Press, Release,
    /// Hover and Leave are allowed (kInvalidArgument otherwise, and for an invalid element). Used by
    /// tools that script input for an element with no hit area.
    Status fireTrigger(ElementRef element, TriggerEvent event);

    // Surface lifecycle (27.2.4, 13.8.3, T-174). The surface element is the root's `body` child
    // when there is one, else the root. requestHide() fires its `hide` triggers; when their
    // actions wait on a timeline (`play(outro) then hide`) the surface stays visible and
    // isHiding() until that chain's `hide` runs, otherwise it hides at once. requestShow() makes
    // it visible and fires `show` (also fired once by the first update()). A hidden surface is not
    // painted and takes no input.
    Status requestShow();
    Status requestHide();
    bool isVisible() const { return m_surfaceVisible; }
    bool isHiding() const { return m_surfaceHiding; }
    /// T-373 (17 §17.33.2): the surface's initial visibility, set without its lifecycle: no `show`
    /// / `hide` triggers, no OnShown / OnHidden, and a pending `then hide` is dropped. Meant for a
    /// host before the first update(): hidden then, that update() fires no `show` either, so
    /// requestShow() is the surface's first `show`.
    void setInitialVisibility(bool visible);
    /// T-374 (17 §17.33.3): where the surface's last hide came from: `port <name>` (a
    /// `port(<name>)` trigger's `hide()`), `trigger on:<event>` (another trigger's), or `host`
    /// (requestHide() outside a trigger chain, or setInitialVisibility(false)). Empty until the
    /// surface first hides.
    std::string_view lastHideSource() const { return m_lastHideSource; }
    /// Where `sound`/`haptic`/`emit` actions go (13.8.3); one listener, replaced by each call.
    void setHostEventListener(HostEventListener listener);
    /// Told when the surface finished showing (true, `OnShown`) or hiding (false, `OnHidden`).
    void setVisibilityListener(std::function<void(bool visible)> listener);

    // Ports and bindings (Docs/design/27-interaction.md §27.3, 13-api.md §13.8.1, T-175). Values
    // by port type: Number Float; String / Texture StringRef (this Document's StringId); Bool
    // Float 0/1 (Enum 0/1 is accepted); Color, Vec2, Vec3 as such; Entity Enum (a u32 id; Float
    // is accepted); Enum the value's ordinal (a StringRef value name is accepted); Struct a
    // Struct (fields matched by name, missing ones keep their value); List a List of Structs (T-381:
    // of T for a `list<T>` port);
    // Event anything (the value is ignored, `port(<name>)` fires). A change re-evaluates the
    // bindings that read the port (marking only the affected dirty bits) and fires the
    // `port(<name>)` triggers of the elements that declare it in their scope.
    /// A port by name: a document-level port's name, `[#]<element id>.<name>` (path ids inside
    /// symbol instances included) or `<instance id>.<name>` for an exposed port. `ui#id.port`
    /// (another UI) is not resolved here. kInvalidPort when nothing matches.
    PortId lookupPort(std::string_view name) const;
    /// Every declared port, in PORT order (13.8.1 `portTable()`).
    std::vector<PortRecord> ports() const;
    /// One port's row; a default PortRecord for an unknown id.
    PortRecord portInfo(PortId port) const;
    /// Sets a port (any direction). kNotFound for an unknown port, kInvalidArgument for a value
    /// of the wrong type, an unknown enum value or a struct field of the wrong type.
    Status setPort(PortId port, const Value& value);
    /// Sets a port from text, parsed by its type: a number, `true`/`false`/`1`/`0`, a CSS color,
    /// `x,y[,z]`, an enum value name, any text for String/Texture; a JSON object for a Struct port,
    /// a JSON array of objects for a `list<struct>` port and, T-381, a JSON array of values for a
    /// `list<T>` port (`["a","b"]`, `[1,2]`, `["#ff0000"]`, `[[1,2]]`, enum value names or
    /// ordinals); fields by name (T-177; 13 §13.8.1). kInvalidArgument when the text does not parse.
    Status setPortText(PortId port, std::string_view text);
    /// A port's current value (13.8.1 `get`); Float 0 for an unknown port. A String / Texture
    /// value is interned into this Document on the call.
    Value getPort(PortId port);
    /// A port's current value as text (numbers in the shortest form, Bool `true`/`false`, an Enum
    /// its value name, a Color `#rrggbbaa`); empty for Struct/List/Event and unknown ports.
    std::string portText(PortId port) const;
    /// 13.8.1 `call`: the host calls the UI's `call` port: `args[0]` (if any) becomes the port's
    /// value (bindings and port listeners follow it), the `port(<name>)` triggers of the port run
    /// once, and the value of the last `emit()` they performed is returned (Float 0 when none).
    /// kNotFound for an unknown port, kInvalidArgument for a port that is not `call` or a
    /// mistyped argument.
    Result<Value> callPort(PortId port, std::span<const Value> args);
    /// The host function an `emit(<port>)` on `call` port `port` invokes (UI -> host call); an
    /// empty function removes it. kNotFound for an unknown port, kInvalidArgument for a port that
    /// is not `call`.
    Status setPortCallHandler(PortId port, PortCallHandler handler);
    /// List ports (13.8.1): replace the whole list / insert before `index` (== size appends) /
    /// remove / replace one entry; entries are Struct Values or, T-381, for a `list<T>` port Values
    /// of T as setPort() takes them (an Enum entry its ordinal or value name; getPort() returns
    /// it as the value name's StringRef). Values and events only -- `<repeat>` instantiation is
    /// T-177. kNotFound for an unknown port, kInvalidArgument for a non-list port, an entry of the
    /// wrong type or an index out of range.
    Status listSet(PortId port, std::span<const Value> entries);
    Status listInsert(PortId port, uint32_t index, const Value& entry);
    Status listRemove(PortId port, uint32_t index);
    Status listUpdate(PortId port, uint32_t index, const Value& entry);
    /// 13.8.1 `registerConverter`: a `host.<name>(...)` converter; returns its op number (0x80 +
    /// registration order; re-registering a name replaces the function and keeps its number).
    /// Every binding that uses it is re-evaluated. kInvalidArgument for an empty name, an empty
    /// function or more than 128 converters.
    Result<uint32_t> registerConverter(std::string_view name, ConverterFn fn);
    /// T-200 (13.8.3 `setWorldAnchor`): the host projected a world position to viewport logical
    /// coordinates (`x`, `y`; `depth` is the view depth). Sets vec2 `port` to (x, y) or vec3 `port`
    /// to (x, y, depth); every element with `world-anchor="<port>"` is moved there by a run-time
    /// translate (its margin box's top-left on the point, `left`/`top` semantics, nothing else is
    /// relaid out) after each layout. `onScreen == false` turns the custom state `Offscreen` on for
    /// those elements (off again for true) when the document declares it. kNotFound for an unknown
    /// port, kInvalidArgument for a port that is not vec2/vec3 or a non-finite coordinate.
    Status setWorldAnchor(PortId port, float x, float y, float depth = 0, bool onScreen = true);
    /// Port change / emit / list notifications; returns an id for removePortListener() (never 0).
    uint32_t addPortListener(PortListener listener);
    /// No-op for an unknown id.
    void removePortListener(uint32_t id);

    // Document state snapshot (Docs/design/23-host-integration.md §23.ad.1, T-398): moving a
    // document to another Context = snapshotState() here, unload, load the same IR there,
    // restoreState(). Context::hotReload carries its state over with the same collect / apply code.
    /// The runtime state as JSON: every port value (Event ports excepted; repeat instance copies
    /// are rebuilt from their list), the custom states of every element with an id, the text and
    /// selection of every editable `input` with an id, each player's focused element id, the
    /// scroll position of every scrolled element with an id and every active `@timeline` group
    /// (scope id, name, position, rate, flags). Not const: a Struct / List port value is read
    /// through getPort(), which interns its strings.
    std::string snapshotState();
    /// Applies a snapshotState() text: after load and before the first update(), or at any time.
    /// Ports are set as setPortText() would (a change notifies listeners and fires the port's
    /// `port(<name>)` triggers, as a host write does); then custom states, input values and
    /// selections, focus, timeline groups (groups not in the snapshot are released) and scroll
    /// positions (a layout pass runs first, layoutIfDirty(), so the offsets clamp to the real
    /// content). The snapshot is authoritative for what it covers: id'd elements it does not list
    /// lose their custom states and scroll offset, players it lists no focus for lose theirs.
    /// Elements, ports, states and timelines this document does not have are skipped.
    /// kInvalidArgument when the text is not a snapshot (unparsable JSON, not an object, another
    /// `version`), with nothing applied.
    Status restoreState(std::string_view json);
    /// 27.3.1 multi-instance: the id this Document's host events carry (default 0).
    void setInstanceId(uint32_t id) { m_instanceId = id; }
    uint32_t instanceId() const { return m_instanceId; }
    /// Like hitTest() for a press (08.9): `pointer-watch` elements are skipped, so kInvalidNode
    /// means the press belongs to whatever is below the UI.
    NodeId hitTestPress(Vec2 pos) const;

    // External content (Docs/design/25-editor-blueprint.md §25.3.5b, 13-api.md §13.8.3, T-191).
    // `node` is a `<video>` / `<sequence>` / `<external>` element or its NodeKind::External child.
    // A `<sequence>` is played by the core (frames through the AssetResolver, a preload window of
    // setSequencePreload() frames ahead); a `<video>` only ever by the host: the media calls below
    // update the element's state and send ExternalRequests to setExternalRequestListener().
    /// IExternalTexture (see event.h).
    Status bindTexture(NodeId node, uint32_t textureHandle, int width, int height) override;
    /// IExternalTexture (see event.h).
    Status setPlayback(NodeId node, MediaState state, float time) override;
    /// Software renderers' counterpart of bindTexture(): borrows `pixels` (RGBA8, straight alpha;
    /// valid until the next bindImage()/bindTexture() of the node or the Document's end) as the
    /// element's content through AssetResolver::setExternalPixels; nullptr unbinds the pixels.
    /// Same errors as bindTexture().
    Status bindImage(NodeId node, const ImageData* pixels);
    /// Removes the element's texture binding (placeholder again when placeholders are on).
    /// kInvalidArgument like bindTexture().
    Status unbindExternal(NodeId node);
    /// The NodeKind::External child of `element` (or `element` itself when it is one);
    /// kInvalidNode for anything else.
    NodeId externalOf(NodeId element) const;
    /// The External data of `node` (element or child), or nullptr.
    const ExternalNodeData* externalData(NodeId node) const;
    /// Every `<video>` / `<sequence>` / `<external>` element, in document order.
    const std::vector<NodeId>& externalElements() const { return m_externalElements; }
    /// Plays a video or sequence from its current time (from 0 after it ended). `loop` also sets
    /// the loop flag. Video: a Play request. kInvalidArgument for any other node.
    Status playMedia(NodeId node, bool loop = false);
    /// Pauses (time holds). Video: a Pause request.
    Status pauseMedia(NodeId node);
    /// Stops: paused at time 0. Video: Pause + Seek(0) requests.
    Status stopMedia(NodeId node);
    /// Seeks to `seconds` (clamped to [0, duration] when the duration is known). Video: a Seek
    /// request.
    Status seekMedia(NodeId node, float seconds);
    /// Sets the playback rate (> 0; kInvalidArgument otherwise). Video: a Rate request.
    Status setMediaRate(NodeId node, float rate);
    /// Sets the loop flag. Video: a Loop request.
    Status setMediaLoop(NodeId node, bool loop);
    /// A video's duration in seconds as the host knows it (< 0 = unknown): clamps seeks, ends
    /// `sync` clips and shows in the placeholder. kInvalidArgument for any other node.
    Status setMediaDuration(NodeId node, float seconds);
    /// The host callback for video requests (Context routes it to ContextListener).
    void setExternalRequestListener(ExternalRequestListener listener);
    /// Draws unbound videos and external textures as a placeholder -- a checkerboard with the
    /// element's name (id, else src/source) and, for a video, its duration -- and gives them a
    /// 320x180 intrinsic size. Off by default (a game draws nothing until the host binds).
    void setExternalPlaceholders(bool on);
    bool externalPlaceholders() const { return m_externalPlaceholders; }
    /// How many frames a playing sequence decodes ahead of the one it shows, the shown frame
    /// included (default 8; 1 = only the shown frame).
    void setSequencePreload(uint32_t frames) { m_sequencePreload = frames == 0 ? 1 : frames; }
    uint32_t sequencePreload() const { return m_sequencePreload; }
    /// Context: appends the logical AABBs of the elements whose external binding changed since
    /// the last call, then forgets them.
    void takeExternalDamage(std::vector<Rect>& out);

    // Particles (Docs/design/03-scene-and-properties.md §3.12.5, T-202; particles.cpp). `node` is
    // a `<particles>` element or its NodeKind::Particles child. Emitters advance in update() after
    // layout, in fixed 1/120 s steps (xt/particles.h); play/stop only switch rate emission.
    /// The NodeKind::Particles child of `node` (or `node` itself when it is one); kInvalidNode
    /// for anything else.
    NodeId particlesOf(NodeId node) const;
    /// The emitter data of `node` (element or child), or nullptr.
    const ParticleNodeData* particleData(NodeId node) const;
    /// Every `<particles>` element, in document order.
    const std::vector<NodeId>& particleElements() const { return m_particleElements; }
    /// Starts emission (the `burst` goes out on the next step); a no-op while emitting.
    /// kInvalidArgument when `node` is not a particle emitter.
    Status playParticles(NodeId node);
    /// Stops emission; live particles finish their lives. kInvalidArgument like playParticles().
    Status stopParticles(NodeId node);
    /// Editor canvas at rest (Docs/tasks/T-202.md item 6): while on, emitters whose
    /// `simulate-in-editor` is false do not advance. Off by default (a game always simulates).
    void setParticleEditorMode(bool still) { m_particleEditorMode = still; }
    bool particleEditorMode() const { return m_particleEditorMode; }

    // `anim` element helpers.
    /// The autoplay-created instance id for `anim`; 0 if none (not autoplay, or not an `anim`
    /// element with an instantiated scene).
    uint32_t defaultAnimation(ElementRef anim) const;
    /// Docs/tasks/T-62.md: `element`'s CSS `animation` instance ids (`ElementData::cssAnimations`),
    /// in declaration order (the order they appear in the `animation` shorthand's comma list, per
    /// `applyAnimationDiff()`'s per-spec `play()` loop -- style_engine.cpp). Empty when `element` is
    /// invalid, not an Element, or currently has no `animation` declaration. Editor-only accessor
    /// (`xt::editor::PreviewSession`'s timeline playhead, Docs/design/17-editor.md §17.5/§17.7);
    /// intentionally omitted from 13-api.md's public surface (13.2 doesn't have it either) since it
    /// exposes Animator instance ids, which nothing outside the editor needs.
    std::vector<uint32_t> animationsOf(ElementRef element) const;
    /// `anim`'s instantiated scene instance, or nullptr when it has none.
    const SceneInstance* sceneInstance(ElementRef anim) const;
    /// Compiler convention: SCEN i <-> TLIN i. Returns kInvalidTimeline when TLIN i doesn't exist.
    TimelineId sceneTimeline(SceneId scene) const;

    // Per-frame, called by Context.
    /// Docs/design/01-architecture.md §1.5 step 3, restricted to this task's subset: Animator
    /// evaluation, world transform update, dirty-flag clear.
    void update(float clock);
    /// T-394 (08.15, 08 §8.17): runs the pending style, layout, virtualized `<repeat>` window and
    /// world-transform passes now, outside update(), so layout-dependent queries (boundingBox,
    /// maxScroll, scrollIntoView) see the changes made since the last frame (e.g. a `listInsert`).
    /// Animations, triggers and the clock do not advance; no-op without a root.
    void layoutIfDirty();
    /// Painter::paint(root), appended into `out` (does not clear it). T-263d: adds nothing to
    /// `out.sources` and leaves every asset command's `source` 0 -- the backend resolves this
    /// list's AssetIds against the IrView it is called with, ir(). (Context::render, which paints
    /// several Documents into one list, names each one's IR there instead.)
    void paint(CommandList& out);
    /// Sets the viewport `xt`'s box lays out into (Docs/design/06-layout.md §6.3) and marks the
    /// whole tree kDirtyLayout so the next update() relayouts from the root even if no element's
    /// own style changed.
    void setViewport(Rect logical);
    /// T-176 (13.8.2 `setDevice`): the screen this document is shown on. `@media` is re-evaluated
    /// (elements a rule that changed state matches are marked kDirtyStyle) and, when the resolved
    /// safe-area insets change, the whole tree is marked kDirtyLayout. Context forwards its own
    /// setDevice() here.
    void setDevice(const DeviceInfo& device);
    /// T-176 (13.8.2 `setUiScale`): `1uiscale` = `scale` px. Non-positive values are ignored.
    void setUiScale(float scale);
    /// The DeviceInfo last given to setDevice() (a default DeviceInfo until then).
    const DeviceInfo& device() const { return m_device; }
    /// The factor last given to setUiScale() (1 until then).
    float uiScale() const { return m_uiScale; }
    /// T-178 (13.8.3): the provider `<glyph key>` inline objects resolve through (borrowed; may
    /// be null = every glyph draws as the text `[key]`). Re-resolves every glyph against the
    /// current DeviceInfo::input; setDevice() re-resolves when the input kind changes. A glyph
    /// whose asset changed marks its text for re-shaping and relayout.
    void setGlyphProvider(IGlyphProvider* provider);
    /// T-176: what the device-relative units and `calc()` resolve against right now.
    const LengthEnv& lengthEnv() const { return m_lengthEnv; }
    /// Docs/design/18-effects.md §18.2: rebuilds the effect/effect-stop pools, reclaiming the
    /// holes left by NodeArena::assignEffects()'s append path. The host or editor may call it at a
    /// non-per-frame moment (e.g. after a batch of style edits); T-270a: update() also calls it at
    /// its end once more than half of a pool past 256 entries is holes.
    void compactEffects();

    /// The document's Animator (mutable access).
    Animator& animator() { return m_animator; }

    /// Docs/design/21-perf-tracing.md §21.2: binds the PerfTracker update()/paint() report their
    /// phase times and workload counts into. Context calls this for every Document it loads; a
    /// standalone Document (tests) keeps its own, permanently disabled tracker. `tracker` must
    /// outlive this Document; nullptr restores the built-in one.
    void setPerfTracker(PerfTracker* tracker) { m_perf = tracker != nullptr ? tracker : &m_ownPerf; }

    /// T-244 (Docs/design/32-rigging-and-deformation.md §32.2.3): zeroes every Spring constraint's
    /// simulation state, for an editor that scrubs to a frame and simulates forward from it.
    void resetRigSprings();

    /// T-243c (Docs/design/32-rigging-and-deformation.md §32.5): the number of skeletons
    /// (`<xt-skeleton>` Groups of the scene instances) this document evaluates.
    size_t rigCount() const;
    /// T-243c: a read-only copy of skeleton `index`'s pose as the most recent update() left it
    /// (FK, constraints and spring offsets; see xt/rig.h). An empty RigView when `index` >=
    /// rigCount(); before the first update() every bone's world is the identity.
    RigView rigAt(size_t index) const;

    // Events (Docs/design/08-events.md, Docs/tasks/T-32.md).
    /// Registers `listener` on `target` for `type`; returns kInvalidListener if `target` is
    /// !valid() or belongs to a different Document. See event_dispatcher.h's Listener for the
    /// pool/compaction rules.
    uint32_t addListener(ElementRef target, EventType type, EventListener listener);
    /// No-op for an id that was never returned by addListener() or was already removed.
    void removeListener(uint32_t id);
    /// Consumes one queued InputEvent (08.4's pointer state machine): hit test, hover/active
    /// state, PointerEnter/Leave/Move/Down/Up/Click/Wheel dispatch with bubbling. Called by
    /// Context::update(), once per drained InputEvent, before this Document's own update().
    void dispatchInput(const InputEvent& event);
    /// Topmost Element under `pos` (08.2); kInvalidNode if nothing hits (e.g. no root yet).
    NodeId hitTest(Vec2 pos) const;

    // 3D space (Docs/design/19-3d-space.md).
    /// Sets `node`'s NodeCommon::is3D (§19.3). Scene nodes only; an Element (whose is3D is
    /// derived at style-resolve time from its 3D PropIds) returns kInvalidArgument.
    Status setNode3D(NodeId node, bool on);
    /// `root`'s 3D space camera (§19.4): the first visible Camera child, or the Camera child of a
    /// `<camera>` Element child, in document order. kInvalidNode if `root` has none.
    NodeId cameraOf(NodeId root) const;
    /// Projects `localPoint` (in `node`'s own local coordinates) to screen/viewport coordinates,
    /// through node's 3D space root(s) (if any), per §19.5/§19.7. Falls back to
    /// `node.world * localPoint` outside any 3D space root's subtree, or when the projection's
    /// homogeneous divide fails (w <= 1e-6). Recomputed on every call (not cached).
    Vec2 nodeToScreen(NodeId node, Vec2 localPoint) const;
    /// Like nodeToScreen(), but `localPoint` carries a local Z coordinate (Docs/design/
    /// 19-3d-space.md §19.2's local3 point, before `node`'s own local3): for a 3D layer node
    /// (`is3D == 1`), `localPoint.z` is embedded into the innermost mapping-stack level (the one
    /// that applies `node`'s own `world3`) instead of always projecting z = 0. For a 2D node
    /// (`is3D == 0`), `localPoint.z` is ignored and this is equivalent to
    /// `nodeToScreen(node, {localPoint.x, localPoint.y})` (Docs/tasks/T-64.md).
    Vec2 nodeToScreen3(NodeId node, Vec3 localPoint) const;
    /// T-386 (Docs/design/08-events.md §8.11): `rootPos` (root / viewport coordinates, as
    /// Event::pos and hitTest() take) in `element`'s own border-box coordinates -- the inverse of
    /// its world transform, as the hit test computes it. (0, 0) for a dead or non-Element node or
    /// a singular transform.
    Vec2 localPosIn(NodeId element, Vec2 rootPos) const;
    /// T-386 (08.11): localPosIn() divided by `element`'s border-box size, each axis clamped to
    /// [0, 1]; (0, 0) when the element is not laid out (an axis of size 0 gives 0).
    Vec2 fractionIn(NodeId element, Vec2 rootPos) const;
    /// Generic (NodeId, PropId) property write: validates `prop` applies to `node`'s kind and that
    /// `value`'s type matches, then writes it and marks the property's dirty bits.
    /// kInvalidArgument on any validation failure (node not alive, prop inapplicable, type
    /// mismatch); otherwise kOk.
    Status setNodeProperty(NodeId node, PropId prop, const Value& value);
    /// Sets or clears arbitrary `ElementState` bits (node.h §3.6) on `ref` directly, without the
    /// pointer/focus dispatch (or the disabled/checked-only restriction) `ElementRef::setState()`
    /// has -- lets the editor's design-mode state toggle force `:hover`/`:active`/`:focus` for
    /// preview without actually simulating an event (Docs/design/17-editor.md §17.7,
    /// Docs/tasks/T-59.md). No-op for an invalid `ref` or an all-zero `stateBits`. Marks the same
    /// style-dirty subtree `ElementRef::setState()` does when a bit actually changes.
    void setElementState(ElementRef ref, uint16_t stateBits, bool on);
    /// The current hover element's ComputedStyle::cursor; Cursor::Default if nothing is hovered.
    /// An editable `input[type=text]` that specifies no `cursor` of its own reports Cursor::Text
    /// (Docs/tasks/T-33.md "cursor()").
    Cursor cursor() const;

    // Focus (Docs/design/08-events.md §8.5, Docs/tasks/T-33.md).
    /// The focused element; an invalid ref when nothing has the focus (the state after load()).
    ElementRef focus() const;
    /// Moves the focus: Blur + kStateFocus cleared on the old element, kStateFocus + Focus on the
    /// new one (neither event bubbles). An invalid ref (or one from another Document) clears the
    /// focus.
    void setFocus(ElementRef element);
    /// 08.8: true when the focused element is a non-disabled `input[type=text]`, i.e. when the
    /// host should have platform text input running. Stays true for the whole composition.
    bool wantsTextInput() const;
    /// 08.8: the caret rectangle in viewport coordinates, for positioning the platform's IME
    /// candidate window (SDL_SetTextInputRect). 1 logical pixel wide and one line high, taken
    /// at the composition caret while composing and at the ordinary caret otherwise, mapped
    /// through the text node's world transform (its AABB, for a rotated element). An all-zero
    /// Rect when nothing editable has the focus or its text has not been laid out yet.
    Rect textInputRect() const;
    /// 08.8's in-flight IME composition (never null; `active == false` when there is none).
    /// Exposed for the host's own composition rendering (preventDefault) and for tests.
    const CompositionState& compositionState() const;
    /// Non-fatal problems collected while loading (`@font-face` assets that could not be read or
    /// registered, Docs/tasks/T-30.md "字型資產") plus, from update(), a one-shot "effect pool
    /// grew" warning per NodeArena::assignEffects() reallocation (18.2, Docs/tasks/T-50.md;
    /// NodeArena itself keeps no warning list, see NodeArena::effectPoolGrew()).
    /// T-211: plus kWarnEffectMissing, once per document.
    const std::vector<std::string>& warnings() const { return m_warnings; }

    /// Docs/design/07-text.md §7.7: the logical UTF-8 byte offset of the grapheme boundary nearest
    /// `localPos` (in `text`'s parent element's content-box coordinates, i.e. the same space
    /// GlyphRun positions use) within the first Text child of `element`. Returns 0 when `element`
    /// is invalid, has no Text child, or that text has not been laid out.
    uint32_t hitTestText(ElementRef element, Vec2 localPos) const;

    /// T-178 (Docs/design/07-text.md §7.8, 08.9): the rich-text link under `pos` (viewport
    /// coordinates, like hitTest()): the index into `ir().doc().textLinks()` of the `<a>` run
    /// whose laid-out glyph run contains `pos`, within the text of the topmost Element hitTest()
    /// finds; 0xFFFFFFFF when there is none. Executing the link's `on:click` is the trigger
    /// table's job (T-174); this only reports the run-level hit.
    uint32_t hitTestTextLink(Vec2 pos) const;

    // Local players, focus navigation, drag and drop (Docs/design/08-events.md §8.9,
    // 27-interaction.md §27.7, T-194). Players are 0..kMaxPlayers-1; the mouse and keyboard are
    // player 0 unless the host stamps InputEvent::player.
    /// `player`'s focus; an invalid ref when that player focuses nothing.
    ElementRef focus(uint8_t player) const;
    /// Moves `player`'s focus (Focus / Blur carry Event::player). `:focus` matches while any
    /// player focuses an element, `:focus-player(<n>)` only for player n.
    void setFocus(ElementRef element, uint8_t player);
    /// Directional focus navigation for `player` (the d-pad / arrow-key default action): the
    /// element's `nav:<dir>` target, or the geometric best candidate under `focus-nav: auto`,
    /// inside the nearest focus trap. Dispatches NavBlocked (`nav.blocked`) and returns false when
    /// there is none.
    bool navigate(NavDirection dir, uint8_t player = 0);
    /// Makes `element` a focus trap (NodeFlags2::kFocusTrap, a modal): navigation and Tab from a
    /// focus inside it stay inside it. kInvalidArgument for an invalid element.
    Status setFocusTrap(ElementRef element, bool on);
    /// Held d-pad / stick repeat: the first repeat after `delay` seconds, then every `interval`
    /// (defaults 0.4 and 0.15). Non-positive values are ignored.
    void setNavRepeat(float delay, float interval);
    /// T-379 (08.10): the document's `<body key-activation="auto | none">` declaration: true for
    /// `auto` (the default: Enter / Space activate the focused element), false for `none`. The
    /// activation runs when this and Context::keyboardActivation() are both true; focus
    /// navigation never depends on it.
    bool keyboardActivation() const { return m_keyActivation; }
    /// The element being dragged (`draggable`); invalid when no drag is in progress.
    ElementRef dragSource() const;
    /// The `drop-target` under the drag; invalid when none.
    ElementRef dragOver() const;
    /// The drag ghost's offset from the source's laid-out position (zero without a drag).
    Vec2 dragGhostOffset() const;
    /// Cancels the drag in progress (T-388, 08.12): DragLeave on the target, DragCancel and
    /// DragEnd (dropped = false) on the source, no Drop. No effect without a drag.
    void cancelDrag();
    /// T-390 (08.13): cancels the pointer press in progress, at once: PointerCancel to the active
    /// element (bubbling), the active state cleared, no PointerUp / Click (T-386, 08.11). No
    /// effect when nothing is pressed. Context::cancelPointer() reaches every Document through it.
    void cancelPointer();
    /// The nearest `drop-target` ancestor-or-self of the topmost element under `pos` (excluding
    /// the drag source's subtree); invalid when none. Used for drops across surfaces.
    ElementRef dropTargetAt(Vec2 pos) const;
    /// T-393 (08.14): true when a `drop-target` element is under `pos` (dropTargetAt(pos) is
    /// valid); what a host answers an OS drag over its window with (copy / none).
    bool hasDropTargetAt(Vec2 pos) const { return dropTargetAt(pos).valid(); }
    /// The `<repeat>` entry index `element` belongs to (27.3.4), 0xFFFFFFFF when none.
    uint32_t itemIndexOf(ElementRef element) const;
    /// Like hitTest() / hitTestPress() but ignoring the root and the surface element (the full-
    /// screen background of a surface): what a `pass` surface lets through (Context::
    /// setSurfaceInput). kInvalidNode when only the background is under `pos`.
    NodeId contentHitTest(Vec2 pos, bool press) const;

    /// T-177 (Docs/design/27-interaction.md §27.3.4): the live instances of the `<repeat>` whose
    /// id is `repeatId`, in list order -- one per list entry (virtualized: only the instantiated
    /// window); instances playing their `item-exit` are not included. Each instance is the copy of
    /// the template root, id `<repeatId>/<index>/<template id>`. Empty for an unknown id. The span
    /// stays valid until the next call or list change.
    std::span<const ElementRef> repeatInstances(std::string_view repeatId);
    /// T-387 (13 §13.9): the list entry index of the `<repeat>` `repeatId` under `rootPos`
    /// (viewport coordinates, like hitTest()), 0xFFFFFFFF when there is none (unknown id, outside
    /// every instance, past the list end). Not virtualized: the live instance whose border box
    /// contains the point. Virtualized: computed from the item height and the scroll position
    /// (the entry need not be instantiated). Reads the last layout (call after an update()).
    uint32_t repeatIndexAt(std::string_view repeatId, Vec2 rootPos) const;
    /// T-397 (08 §8.17, XF-088): every `<repeat>` level `node` is inside, nearest first: the
    /// repeat's id and the list entry index of the instance holding `node` (Event::targetItem is
    /// the first entry's index). Empty when `node` is in no live instance; an instance playing its
    /// `item-exit` (here or further out) yields an empty chain too. Spawn pools are skipped.
    std::vector<RepeatRef> repeatChain(NodeId node) const;

    /// T-376 (23 §23.ab, 05 §5.9.10): the first `app-region` other than `none` on `node` and its
    /// ancestors (a borderless host's hit-test reads it for the element hitTest() returns);
    /// AppRegion::None when there is none, or for kInvalidNode / a dead node.
    AppRegion appRegionAt(NodeId node) const;

private:
    friend struct ElementRef;
    friend class Context;  // T-200: Context::hotReload carries the state over.
    friend class TriggerRuntime;
    friend class BindingRuntime;
    friend class RepeatController;
    friend struct DocumentTestAccess;     // T-260b: Tests/unit reads the private runtimes' counters.
    friend struct RuntimePoolTestAccess;  // T-270a: Tests/unit reads the runtime pools' sizes.

    Document();

    /// paint() with every asset command stamped `source` (T-263d): Context::render's entry, after
    /// it appended this Document's CommandSource to `out.sources` at that index.
    void paintAs(CommandList& out, uint16_t source);
    Status loadInternal(IrBlob blob, bool takeOwnership, TextShaper* shaper, AssetResolver* assets);
    void handleAnimationFinished(uint32_t id);
    void handleAnimationIteration(uint32_t id, uint32_t iteration);
    void handleAnimationMarker(uint32_t id, StringId marker);
    void handleTransitionEnd(NodeId node, PropId prop);
    /// Dispatches (bubbling) an Animation* event to `node`, if it is a live Element (08.6).
    void dispatchAnimationEvent(EventType type, NodeId node, uint32_t animationId, StringId marker);
    /// The EditState of `element`'s Text child, or nullptr when `element` is not editable. Used by
    /// ElementRef::value()/setValue() (ElementRef is a friend).
    EditState* editStateForElement(NodeId element);
    // T-398 (23 §23.ad.1, Code/Runtime/core/src/document_state.cpp): the state snapshotState() / restoreState()
    // and Context::hotReload carry. collectState() fills `out` (its port values only when
    // `withPorts`); missingState() lists what `state` refers to that this Document lacks (custom
    // states, focus, timelines -- hotReload's compatibility check); applyState() applies it (with
    // `exact`, as restoreState(): unlisted custom states / scroll offsets / focus are cleared).
    void collectState(DocumentState& out, bool withPorts);
    void missingState(const DocumentState& state, std::vector<std::string>& missing) const;
    void applyState(const DocumentState& state, bool exact);
    // `@timeline` group runtime (Code/Runtime/core/src/timeline_group.cpp, T-172).
    void loadTimelineGroups();
    int findTimelineGroup(std::string_view scopePath, std::string_view name) const;
    void activateTimelineGroup(size_t index);
    void releaseTimelineGroup(size_t index);
    void applyTimelineGroupTime(size_t index);
    void queueTimelineMarkers(size_t index, float from, float to, bool inclusiveFrom);
    void captureTimelineRestores(size_t index);
    void stepTimelineGroups(float clock);
    void flushTimelineEvents();
    void forgetTimelineNode(NodeId node);
    /// T-35 remove(): undoes everything this Document tracks for one node that is about to be
    /// destroyed (listeners, animation instances, transitions/overrides, EditState, `anim` scene
    /// instance). Does not touch the arena topology; the caller destroys the subtree afterwards.
    void cleanupNodeForRemoval(NodeId node);
    /// T-173: after a child of `parent` was added or removed, marks every Element child of
    /// `parent` and its subtree kDirtyStyle when the stylesheet uses structural pseudo-classes.
    void markSiblingsStyleDirty(NodeId parent);
    // Triggers (Code/Runtime/core/src/trigger_runtime.cpp, T-174).
    NodeId surfaceElement() const;
    void applyCustomState(NodeId node, uint32_t bit, uint8_t mode);
    /// T-390 / T-392 (27.3.2, 27.3.5): sets / clears / toggles (TriggerMode) the built-in
    /// ElementState `bit` of `node` -- setCustomState("Disabled|Focused|Hover|Active|Checked") and
    /// the `state(<that name>, ...)` action.
    void applyBuiltinState(NodeId node, uint16_t bit, uint8_t mode);
    void checkedChanged(NodeId node, bool checked);
    void showElement(NodeId node);
    void hideElement(NodeId node);
    void finishSurfaceHide();
    void finishPendingHides();
    /// T-349 (27.2.4): fires the surface's `show` (true) or `hide` (false) triggers on the
    /// surface element and, when it is `<body>`, on each of its direct Element children in
    /// document order, all in one trigger run.
    void fireSurfaceEvent(bool show);
    // T-397 (08 §8.17): requestShow() / requestHide() made while dispatching wait for the end of
    // the outermost dispatching call; trigger actions call applyShow() / applyHide() directly.
    static constexpr uint8_t kSurfaceRequestNone = 0;
    static constexpr uint8_t kSurfaceRequestShow = 1;
    static constexpr uint8_t kSurfaceRequestHide = 2;
    uint8_t m_pendingSurfaceRequest = kSurfaceRequestNone;
    /// Queues `request` when the Context is dispatching (true), else false (apply it now).
    bool deferSurfaceRequest(uint8_t request);
    /// Applies the queued request, if any (Context::runPendingDocOps()).
    void applyPendingSurfaceRequest();
    /// The surface show / hide itself (the bodies of requestShow() / requestHide()).
    Status applyShow();
    Status applyHide();
    /// T-349: the surface element, the root, or a direct Element child of `<body>`.
    bool isSurfaceMember(NodeId node) const;
    /// T-349: a `hide` chain of any surface member is still waiting (the surface's wait set).
    bool surfaceHideWaiting() const;
    /// Docs/design/19-3d-space.md §19.4: recomputes every `<camera>` element's omitted (NaN)
    /// position/target/zoom from its parent's border-box. Called from update(), right after
    /// layout.
    void refreshCameraDefaults();
    /// `root`'s reference box for camera projection (§19.4): an Element root's border-box, or a
    /// scene root Group's SceneChunk design size; all-zero for anything else.
    Rect spaceRootBox(NodeId root) const;
    /// `root`'s view-projection matrix (§19.4): its camera's, or the CSS/AE default.
    Mat4 viewProjFor(NodeId root) const;

    /// Which API created an Animator instance (Docs/tasks/T-32.md "動畫事件"): Css/Autoplay
    /// instances' onFinished/onIteration/onMarker fire a bubbling Document event on their element;
    /// Api-sourced ones (Document::play(), Docs/design/13-api.md §13.2) only invoke the
    /// onAnimationFinished callback (existing T-18 behavior), not an event.
    enum class AnimSource : uint8_t { Css, Autoplay, Api };
    struct AnimInstanceInfo {
        uint32_t id = 0;
        NodeId node = kInvalidNode;
        AnimSource source = AnimSource::Api;
    };

    std::vector<uint8_t> m_ownedBlob;  // Backing storage when the Document took ownership.
    IrView m_ir;
    NodeArena m_arena;
    Animator m_animator;
    StyleEngine m_styleEngine;
    LayoutEngine m_layoutEngine;
    Painter m_painter;
    /// T-203: the IR's SHDR chunk, unpacked at load (shaderRef()).
    ShaderTable m_shaderTable;
    std::unique_ptr<EventDispatcher> m_events;
    // Docs/design/08-events.md §8.5's one exception to §1.7's immutable-string rule: the runtime,
    // mutable value of every editable `input`, indexed by TextNodeData::editIndex. Held behind a
    // unique_ptr so EditState (Code/Runtime/core/src/text_edit.h, not a public header) stays incomplete here;
    // ~Document() is defined out-of-line in document.cpp, where it is complete.
    std::unique_ptr<std::vector<EditState>> m_editStates;
    // 08.8's single in-flight IME composition, held behind a unique_ptr for the same reason as
    // m_editStates (CompositionState lives in Code/Runtime/core/src/composition.h, not a public header).
    std::unique_ptr<CompositionState> m_composition;
    // `@timeline` groups (T-172), behind a unique_ptr like m_composition (the type lives in
    // Code/Runtime/core/src/timeline_group.h).
    std::unique_ptr<TimelineGroupSet> m_timelineGroups;
    // T-174: trigger tables and their pending `then` chains, behind a unique_ptr like the above.
    std::unique_ptr<TriggerRuntime> m_triggers;
    // T-175: port values and bindings, behind a unique_ptr like the above.
    std::unique_ptr<BindingRuntime> m_bindings;
    // T-177: `<repeat>` templates, instances and virtualization, behind a unique_ptr like the above.
    std::unique_ptr<RepeatController> m_repeats;
    // T-260a: the Context that loaded this Document (set by it on load / reload), whose dispatch
    // depth the DispatchGuard of every callback-running entry point counts; null when standalone.
    Context* m_context = nullptr;
    bool m_keyActivation = true;  // T-379: keyboardActivation().
    // T-260a: Context::unloadDocument() was called for this Document while a dispatch was running;
    // it is freed once the outermost dispatching call returns. From the moment this is set the
    // Document is dead to the host: every host callback it would emit is skipped.
    bool m_unloading = false;
    /// T-260a: sets m_unloading and silences the element listeners (EventDispatcher).
    void markUnloading();
    uint32_t m_instanceId = 0;
    /// T-374: lastHideSource().
    std::string m_lastHideSource;
    /// T-374: a direct child of `<body>` counts as shown for the member rule (27.2.4): not hidden
    /// or hiding by a `hide` action, `visibility` not hidden, `display` not none.
    bool surfaceChildShown(NodeId node) const;
    bool m_surfaceVisible = true;
    bool m_surfaceHiding = false;
    bool m_shownOnce = false;         // The first update() fires the surface's `show`.
    std::vector<NodeId> m_autofocus;  // T-194: `autofocus` elements, document order.
    /// T-194: focuses the first focusable `autofocus` element for player 0 when nothing has the
    /// focus (first show and requestShow()).
    void applyAutofocus();
    std::vector<NodeId> m_hidingElements;  // Elements whose `hide` waits on a `then hide` chain.
    std::vector<NodeId> m_hiddenElements;  // Elements hidden by a `hide` action.
    HostEventListener m_hostEventListener;
    std::function<void(bool)> m_visibilityListener;
    // T-191: external content (external_media.cpp).
    std::vector<NodeId> m_externalElements;  // `video`/`sequence`/`external` elements.
    std::vector<NodeId> m_pendingMediaEnds;  // Elements that ended since the last flush.
    std::vector<NodeId> m_externalDamaged;   // Elements whose binding changed (damage).
    ExternalRequestListener m_externalRequestListener;
    bool m_externalPlaceholders = false;
    uint32_t m_sequencePreload = 8;
    float m_mediaClock = 0;
    bool m_mediaClockValid = false;
    /// Creates `element`'s External child from its attributes (document_load.cpp).
    void loadExternal(NodeId element);
    /// The External child of `node` (element or child) when its source is in `mask`
    /// (bit = 1 << ExternalSourceKind), else kInvalidNode.
    NodeId externalFor(NodeId node, uint32_t mask) const;
    /// Advances playing sequences, handles autoplay; before layout (update()).
    void stepExternalMedia(float clock);
    /// Sets a sequence's time and shown frame (preloading ahead while `preload`).
    void setSequenceTime(NodeId ext, float time, bool preload);
    /// Placeholder flags and labels, after layout (update()).
    void refreshExternalPlaceholders();
    /// Dispatches MediaEnd / `media(<id>).end` for m_pendingMediaEnds.
    void flushMediaEvents();
    /// Sends one request for the video `ext` to the host.
    void sendExternalRequest(NodeId ext, ExternalRequestKind kind, float value);
    /// Marks a video/sequence ended (queues its events once).
    void endMedia(NodeId ext);
    /// `@timeline` sync (timeline_group.cpp): puts the clip `ext` at group time `t`.
    void syncExternalClip(NodeId ext, float t, bool groupPlaying);
    /// `@timeline` release: pauses a video the group started.
    void releaseExternalClip(NodeId ext);
    // T-202: particle emitters (particles.cpp).
    std::vector<NodeId> m_particleElements;  // `<particles>` elements, document order.
    bool m_particleEditorMode = false;
    float m_particleClock = 0;
    bool m_particleClockValid = false;
    /// Creates `element`'s Particles child from its attributes (document_load.cpp).
    void loadParticles(NodeId element);
    /// Mirrors the element PropIds and advances every emitter to `clock`; after layout (update()).
    void stepParticleNodes(float clock);
    std::vector<Timeline> m_timelines;
    std::vector<SceneInstance> m_instances;
    std::vector<NodeId> m_docElementNodes;  // DOCT element index -> Element NodeId.
    // instanceId -> (element, source). Appended to on every play() (public, CSS-diff and
    // autoplay); entries are erased once their instance finishes (ids are never reused, so an
    // erased id is simply never looked up again). A CSS animation instance that is stopped (not
    // finished) mid-flight -- e.g. replaced by a style re-resolve -- leaves its entry behind
    // permanently; see the T-32 report's "uncovered cases".
    std::vector<AnimInstanceInfo> m_animInstances;
    NodeId m_root = kInvalidNode;
    TextShaper* m_shaper = nullptr;
    AssetResolver* m_assets = nullptr;
    float m_clock = 0;
    Rect m_viewport{};
    // T-176: setDevice()/setUiScale() state and the LengthEnv derived from them + m_viewport.
    DeviceInfo m_device{};
    float m_uiScale = 1;
    LengthEnv m_lengthEnv{};
    // T-178: the glyph provider and the Text nodes that carry `<glyph>` inline objects.
    IGlyphProvider* m_glyphProvider = nullptr;
    std::vector<NodeId> m_glyphTextNodes;
    /// T-178: copies the IR run table entries of DOCT text `textIndex` into `textNode`.
    void loadTextRuns(uint32_t textIndex, NodeId textNode);
    /// T-178: resolves every `<glyph>` against m_glyphProvider and m_device.input.
    void resolveGlyphs();
    /// Rebuilds m_lengthEnv; marks the tree kDirtyLayout (and device-unit users kDirtyStyle) when
    /// a unit's px value changed; re-evaluates `@media`.
    void refreshLengthEnv(bool force);
    bool m_forceTransform = true;  // First frame, or since the last setViewport() call.
    /// T-397 (08 §8.17): update() skipped layout for a hidden surface since the last layout.
    bool m_layoutSkipped = false;
    /// T-397: after skipped layouts, marks the whole tree kDirtyLayout and forces the transforms.
    void restoreSkippedLayout();
    /// T-399 (08 §8.18): applies the pending ElementRef::focus() and validates the focus after the
    /// style / layout pass. True when a focus changed.
    bool validateFocusAfterLayout();
    bool m_warnedPendingFocus = false;  // T-399: the dropped pending focus is reported once.
    std::vector<std::pair<uint32_t, std::function<void()>>> m_finishedCallbacks;
    std::vector<std::string> m_warnings;
    bool m_warnedEffectMissing = false;    // T-211: kWarnEffectMissing is reported once.
    bool m_warnedBackgroundTiles = false;  // T-327: kWarnBackgroundTiles is reported once.
    bool m_warnedGridClamp = false;        // T-338: kWarnGridPlacementClamped is reported once.
    // T-179: every `@key` text node / attribute of the IR, and the StringId last shown there (a
    // different current value means the host replaced it, and it is left alone from then on).
    struct LocalizedSlot {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        uint32_t attrIndex = 0xFFFFFFFFu;  // Index into ElementData::attributes; 0xFFFFFFFF = text.
        StringId key = kInvalidString;
        StringId shown = kInvalidString;
    };
    std::vector<LocalizedSlot> m_localized;
    std::string m_language;
    // T-175: the provider `localize` binding steps resolve through (the last setLocalization()'s).
    ILocalizationProvider* m_l10nProvider = nullptr;
    /// The StringId currently at `slot`'s place, or kInvalidString when the node is gone.
    StringId localizedCurrent(const LocalizedSlot& slot) const;
    /// T-397 (08 §8.16): the shown StringIds of the `placeholder` `@key` slots still showing their
    /// key (unresolved), sorted; LayoutEngine skips those placeholders.
    std::vector<StringId> m_unresolvedPlaceholders;
    void refreshUnresolvedPlaceholders();
    // Runtime string pool (13.2's intern(), 01.7's exception rule). A deque so an interned
    // string_view stays valid as the pool grows. `m_irStringOrder` is the IR string table's ids
    // sorted by text, built on the first intern() call, so the "already in the IR?" lookup is a
    // binary search rather than the linear scan T-24's addClass used; `m_runtimeStringIndex`
    // (T-270a) is the pool's text -> id hash index (its keys view the deque's own strings).
    std::deque<std::string> m_runtimeStrings;
    std::unordered_map<std::string_view, StringId> m_runtimeStringIndex;
    /// intern()'s lookup half (T-270a): `s`'s StringId in the IR string table or the runtime pool,
    /// or kInvalidString when neither holds it. Never adds to the pool (query() uses it: a name no
    /// string carries cannot match any element).
    StringId findString(std::string_view s);
    std::vector<StringId> m_irStringOrder;
    bool m_irStringOrderBuilt = false;
    // T-270a (C6): node-owned runtime text (xt/types.h's kRuntimeTextBase). One slot per Text node
    // that ElementRef::setText wrote, overwritten on every change and put on the free list when the
    // node is removed; `m_runtimeTextOwners[slot]` is the Text node holding it (kInvalidNode when
    // free). A deque so one node's text never moves when another node's slot is added.
    std::deque<std::string> m_runtimeTexts;
    std::vector<NodeId> m_runtimeTextOwners;
    std::vector<uint32_t> m_freeRuntimeTexts;
    /// Stores `content` as `textNode`'s runtime text (its own slot, reused) and returns the
    /// kRuntimeTextBase-tagged StringId to put in TextNodeData::text.
    StringId setRuntimeText(NodeId textNode, std::string_view content);
    /// Releases `textNode`'s runtime text slot, if it owns one.
    void releaseRuntimeText(NodeId textNode);
    // Runtime effect declaration pool (18-effects.md §18.4, T-51b's declareRuntimeEffects()),
    // append-only like m_runtimeStrings above; see declareRuntimeEffects()'s doc comment.
    std::vector<EffectInstance> m_runtimeEffects;
    std::vector<GradientStop> m_runtimeEffectStops;
    // T-270a (C6): the segment of the two pools above each element's setStyle / setEffects
    // `effect` declaration occupies, reused while the new sequence fits it; released segments
    // (outgrown, or of a removed element) are taken again by a later declaration that fits.
    struct RuntimeEffectSegment {
        uint32_t effectOffset = 0;
        uint32_t effectCap = 0;
        uint32_t stopOffset = 0;
        uint32_t stopCap = 0;
    };
    std::unordered_map<NodeId, RuntimeEffectSegment> m_runtimeEffectSegments;
    // T-270a (B14): NodeArena's effects()/effectStops() sizes when update() last counted their
    // holes (it only counts again after a size change).
    size_t m_effectPoolChecked = 0;
    size_t m_stopPoolChecked = 0;
    std::vector<RuntimeEffectSegment> m_freeEffectSegments;
    /// declareRuntimeEffects() for `node`'s own `effect` declaration: writes into the node's
    /// segment instead of appending (see m_runtimeEffectSegments). Same returned Value.
    Value declareRuntimeEffectsFor(NodeId node, std::span<const EffectInstance> effects,
                                   std::span<const GradientStop> stops);
    /// Releases `node`'s runtime effect segment, if it has one. T-270d: a committed
    /// ComputedStyle::effectOffset/effectCount that points at it is reset to `none` at once.
    void releaseRuntimeEffects(NodeId node);
    // Runtime `background-gradient` declaration pool (05.5c, T-115's declareRuntimeGradient()),
    // append-only like m_runtimeEffects above -- except for setStyle's declarations (T-270c),
    // which use each element's two slots below.
    std::vector<ElementGradient> m_runtimeGradients;
    // Runtime `animation` declaration pool (17.6, T-123's declareRuntimeAnimations()), append-only
    // like m_runtimeGradients above -- except for setStyle's declarations (T-270c, slots below).
    std::vector<IrAnimationSpec> m_runtimeAnimations;
    // T-332: runtime `clip-path` shape pool (declareRuntimeClipShapeFor()), slots like
    // m_runtimeGradients.
    std::vector<ElementClipShape> m_runtimeClipShapes;
    // T-270c: a span of m_runtimeGradients / m_runtimeAnimations an element owns (cap 0 = none).
    struct RuntimeDeclSlot {
        uint32_t offset = 0;
        uint32_t cap = 0;
    };
    // Two slots per (element, property), written alternately: a declaration goes into the slot
    // the element's cascaded value does not reference, so its offset always changes -- which is
    // how StyleEngine sees a change (markNonAnimatableDirty repaints, applyAnimationDiff restarts)
    // -- and the pool holds at most two slots per element. Released with the element.
    struct RuntimeDeclSlots {
        RuntimeDeclSlot slot[2];
        uint8_t last = 1;  ///< The slot written last.
    };
    std::unordered_map<NodeId, RuntimeDeclSlots> m_runtimeGradientSlots;
    std::unordered_map<NodeId, RuntimeDeclSlots> m_runtimeClipShapeSlots;  // T-332.
    std::unordered_map<NodeId, RuntimeDeclSlots> m_runtimeAnimationSlots;
    std::vector<RuntimeDeclSlot> m_freeGradientSlots;
    std::vector<RuntimeDeclSlot> m_freeClipShapeSlots;  // T-332.
    std::vector<RuntimeDeclSlot> m_freeAnimationSlots;
    /// Picks the slot of `own` to write next (see RuntimeDeclSlots; `inUse` is the untagged pool
    /// offset the element's cascaded value references, UINT32_MAX when none) and makes it hold
    /// `need` entries: an outgrown slot goes to `freeList`, replaced by a released one that fits
    /// (first fit, never `inUse`) or a new one at `poolSize`'s end (`poolSize` grows; the caller
    /// resizes its pool to it). Returns the slot's offset.
    static uint32_t takeRuntimeDeclSlot(RuntimeDeclSlots& own, std::vector<RuntimeDeclSlot>& freeList,
                                        uint32_t inUse, uint32_t need, size_t& poolSize);
    /// declareRuntimeGradient() / declareRuntimeAnimations() for `node`'s own setStyle declaration:
    /// writes into one of the node's two slots instead of appending. Same returned Value.
    Value declareRuntimeGradientFor(NodeId node, const ElementGradient& gradient);
    Value declareRuntimeAnimationsFor(NodeId node, std::span<const IrAnimationSpec> specs);
    /// T-332: the same for `node`'s own setStyle `clip-path:` declaration; returns the Enum Value
    /// to cascade (0 for `none`, else kRuntimeEffectOffsetBit | the slot's offset).
    Value declareRuntimeClipShapeFor(NodeId node, const ElementClipShape& shape);
    /// Releases `node`'s gradient and animation slots, if it has any. T-270d: committed
    /// gradient / animation / last-applied animation offset-count pairs that point at a released
    /// slot are reset to `none` at once.
    void releaseRuntimeDeclSlots(NodeId node);
    // T-270c: element-owned m_runtimeTexts slots, one per (element, key), overwritten by the next
    // value and released with the element (cleanupNodeForRemoval, which a recycled `<repeat>`
    // instance goes through too). Keys: a style property number, for setStyle's other
    // text-valued declarations (`font-family`, `background-image`) whose text no string table
    // has; kNodeTextElementId, for a `<repeat>` instance element's id. Their owner entry is
    // kNodeTextOwner, so no Text node's setRuntimeText()/releaseRuntimeText() takes them.
    static constexpr NodeId kNodeTextOwner = kInvalidNode - 1;
    static constexpr uint16_t kNodeTextElementId = 0xFFFF;
    struct NodeText {
        uint16_t key = 0;
        uint32_t slot = 0;
    };
    std::unordered_map<NodeId, std::vector<NodeText>> m_nodeTexts;
    /// The StringId of `node`'s own text slot for `key`, now holding `content`.
    StringId setNodeText(NodeId node, uint16_t key, std::string_view content);
    /// Releases `node`'s text slots, if it has any; an element id held in one becomes unset.
    void releaseNodeTexts(NodeId node);
    // Scratch for remove()'s postorder subtree collection (NodeArena's own traversal buffers are
    // not re-entrant and remove() mutates the arena while walking).
    std::vector<NodeId> m_removeScratch;
    // 21.2: the tracker update()/paint() report into. m_ownPerf is the never-enabled fallback used
    // by a Document that no Context bound a tracker to, so the phase scopes never need a null
    // check (an unused PerfTracker allocates nothing -- its ring buffer is lazy).
    PerfTracker m_ownPerf;
    PerfTracker* m_perf = &m_ownPerf;

    // T-244 (32 §32.3, §32.8): one RigState per `<xt-skeleton>` of the scene instances, built at
    // load (behind a unique_ptr like m_editStates: rig::RigState is a private type). update()
    // evaluates them after the world transforms (dt = the clock difference), re-skins into
    // NodeArena's side table, and publishes the bones' `<id>.world` / `<id>.angle` ports.
    std::unique_ptr<std::vector<rig::RigState>> m_rigs;
    float m_rigClock = 0;
    bool m_rigClockValid = false;
    uint32_t m_rigBones = 0;       // Bones evaluated by the last update() (FrameStats::rigBones).
    uint32_t m_skinnedPoints = 0;  // Points skinned by the last update() (FrameStats::skinnedPoints).
    /// Builds m_rigs from the scene instances (document_load.cpp).
    void loadRigs();
    /// Evaluates every rig and refreshes the skinned geometry (update(), after the transforms).
    void evaluateRigs(float clock);
    /// Writes the bone output ports (update(), at its end).
    void publishBonePorts();
};

}  // namespace xt
