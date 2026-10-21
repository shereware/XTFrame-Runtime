#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "xt/node.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Binary POD structs for the `.xtir` format. Field order/size/semantics are the on-disk format;
/// see docs/design/02-ir-format.md (authoritative), docs/design/03-scene-and-properties.md §3.4
/// (PathVerb/FillRule/LineCap/LineJoin/PaintKind/BlendMode/MaskMode/ValueType/LengthUnit enum values)
/// and docs/design/04-timeline.md §4.1-4.4 (Easing/Keyframe/Track).
/// All structs below are byte-packed (`#pragma pack(push, 1)`), little-endian, and contain no
/// implicit padding (padding fields are explicit `_pad` members per the spec).
///
/// NodeKind/PathVerb/... live in xt/node.h and EasingKind lives in xt/timeline.h (both included
/// above); this header only defines the on-disk POD layouts.

namespace xt {

// -------------------------------------------------------------------------------------------
// fourcc / chunk type constants (see §2.3)
// -------------------------------------------------------------------------------------------

/// Encodes 4 bytes into a little-endian uint32 such that `a` is the byte at the lowest address
/// (matches the file's raw byte order for a char[4] chunk type field).
inline constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

/// The `.xtir` format's major version: what the writer stamps into IrHeader::major and the only
/// value IrView::open accepts. Hosts compare against it (e.g. xt_bundle's manifest `irMajor`).
inline constexpr uint16_t kIrFormatMajor = 2;

/// `STRT` chunk type: the string table. Required.
inline constexpr uint32_t kChunkSTRT = fourcc('S', 'T', 'R', 'T');
/// `ASST` chunk type: the asset table. Required (may have 0 entries).
inline constexpr uint32_t kChunkASST = fourcc('A', 'S', 'S', 'T');
/// `STYL` chunk type: the compiled style table. Required.
inline constexpr uint32_t kChunkSTYL = fourcc('S', 'T', 'Y', 'L');
/// `SCEN` chunk type: a scene template. Optional, may repeat.
inline constexpr uint32_t kChunkSCEN = fourcc('S', 'C', 'E', 'N');
/// `TLIN` chunk type: a timeline. Optional, may repeat.
inline constexpr uint32_t kChunkTLIN = fourcc('T', 'L', 'I', 'N');
/// `DOCT` chunk type: the document element tree. Required.
inline constexpr uint32_t kChunkDOCT = fourcc('D', 'O', 'C', 'T');
/// `SRCM` chunk type: the optional source map (element/text -> source file/line/col). Optional;
/// see §2.9a and 17-editor.md §17.7.
inline constexpr uint32_t kChunkSRCM = fourcc('S', 'R', 'C', 'M');
/// `META` chunk type: debug metadata. Optional.
inline constexpr uint32_t kChunkMETA = fourcc('M', 'E', 'T', 'A');
/// `SYMB` chunk type: the symbol source table (docs/design/26-layers-symbols-library.md §26.2.2,
/// 12-compiler.md §12.7, docs/tasks/T-171.md). Optional, written last, only when the document
/// instantiates at least one symbol. Payload: `u32 count`, then per instance `u32 symbolName`
/// (StringId), `u32 instanceElement` (DOCT element index), `u32 pathCount`, then `pathCount`
/// `IrSymbolPath` records.
inline constexpr uint32_t kChunkSYMB = fourcc('S', 'Y', 'M', 'B');
/// `TGRP` chunk type: `@timeline` groups (docs/design/04-timeline.md §4.10.1, 12-compiler.md
/// §12.7, T-172). Optional; written after every other chunk.
inline constexpr uint32_t kChunkTGRP = fourcc('T', 'G', 'R', 'P');
/// `VARS` chunk type: the design-token table (docs/design/05-style-and-css.md §5.9.2, 12.7,
/// T-170). Optional; written after every other chunk, so a reader that predates it skips it.
inline constexpr uint32_t kChunkVARS = fourcc('V', 'A', 'R', 'S');
/// `STAT` chunk type: the custom-state table (docs/design/27-interaction.md §27.1, 03 §3.12.2,
/// T-174): state names (bit i = names[i], at most 32) and state groups. Optional; written after
/// every other chunk. A `:state(<Name>)` selector is an IrSelCond::CustomState condition part.
inline constexpr uint32_t kChunkSTAT = fourcc('S', 'T', 'A', 'T');
/// `TRIG` chunk type: the trigger tables (27.2, 03 §3.12.3, 12 §12.7, T-174). Optional; written
/// after every other chunk.
inline constexpr uint32_t kChunkTRIG = fourcc('T', 'R', 'I', 'G');
/// `PORT` chunk type: declared ports (docs/design/27-interaction.md §27.3.1, 03 §3.12.3, T-175).
/// Optional; written after every other chunk.
inline constexpr uint32_t kChunkPORT = fourcc('P', 'O', 'R', 'T');
/// `BIND` chunk type: data bindings and their converter chains (27.3.2-27.3.3, 03 §3.12.3,
/// T-175). Optional; written after every other chunk.
inline constexpr uint32_t kChunkBIND = fourcc('B', 'I', 'N', 'D');
/// `RPT ` chunk type: `<repeat>` list templates (docs/design/27-interaction.md §27.3.4, 03 §3.12.4,
/// 12 §12.7, T-177). Optional; written after every other chunk.
inline constexpr uint32_t kChunkRPT = fourcc('R', 'P', 'T', ' ');

/// IrPort::type (27.3.1, 13.8.1).
enum class PortValueType : uint8_t {
    Number = 1,
    String = 2,
    Bool = 3,
    Color = 4,
    Vec2 = 5,
    Vec3 = 6,
    Entity = 7,
    Enum = 8,    ///< `enum(<a|b|c>)`; the value names are the port's fields.
    Struct = 9,  ///< `struct{f: type, ...}`.
    List = 10,   ///< `list<struct{...}>` (entry fields = the port's fields) or, T-381, `list<T>`.
    Texture = 11,
    Event = 12,  ///< Carries no value; setting it only fires `port(<name>)`.
};
/// Highest valid PortValueType code.
inline constexpr uint8_t kPortTypeLast = 12;

/// IrPort::dir (27.3.1): host -> UI, UI -> host, or a function call either way.
enum class PortDir : uint8_t { In = 0, Out = 1, Call = 2 };

/// IrPort::flags: the port is exposed to its symbol instance's parent (default; `expose:false`
/// clears it).
inline constexpr uint8_t kPortExpose = 1u;
/// IrPort::scope of a document-level port.
inline constexpr uint32_t kNoPortScope = 0xFFFFFFFFu;

/// IrBinding::target (27.3.2's left-hand sides).
enum class BindTarget : uint8_t {
    Prop = 1,     ///< A property (PropId or StyleProp in targetId, CSS name in targetName).
    Text = 2,     ///< The element's text.
    Src = 3,      ///< An `img` source (an asset name).
    Visible = 4,  ///< `visibility`.
    State = 5,    ///< `state.<Name>`: custom state bit targetId.
    Class = 6,    ///< `class.<name>`: class targetName.
    Variant = 7,  ///< `variant.<Prop>` on a symbol instance root: class prefix targetName.
    /// T-200 (13.8.3, 03 §3.12.2 `worldAnchorPort`): `world-anchor="<port>"`. The single source is
    /// a vec2/vec3 port holding viewport logical coordinates; the element is moved there with a
    /// run-time translate (always together with kBindAfterLayout). No converter steps.
    WorldAnchor = 8,
    /// T-204 (05 §5.9.1): `bind:checked` on an `<input type="checkbox">`: a truthy value sets
    /// ElementState Checked, a falsy one clears it. Neither fires Change nor `on:check` /
    /// `on:uncheck` (the value follows the port; the user's click still goes through triggers).
    Checked = 9,
    /// T-380 (27 §27.3.4): `bind:<port>` on a symbol instance root (`<use>`) naming an in-port that
    /// instance declares: the chain's result is written into port targetId (a PORT index; an `in`
    /// port that is not an event), as Document::setPort would. Inside a `<repeat>` template each
    /// repeat instance writes its own copy of the port.
    Port = 10,
    /// T-385 (27 §27.3.2): `bind:disabled` (and its alias `bind:state.Disabled`): a truthy value
    /// sets ElementState Disabled, a falsy one clears it (like the `disabled` attribute).
    Disabled = 11,
    /// T-385 (27 §27.3.2): `bind:display`: a falsy value writes the run-time override
    /// `display: none`, a truthy one removes it (the style sheet's value applies again). A string
    /// result that is not a bool spelling is written as the `display` value itself (the older
    /// `bind:display="flag | select(true: block, false: none)"` form).
    Display = 12,
    /// T-392 (27 §27.3.5): `bind:value` on an editable `<input>`: the result is written into the
    /// input's runtime value (ElementRef::value()) without a Change event; the user's edits do not
    /// write the port back.
    Value = 13,
    /// T-392 (27 §27.3.5): `bind:state.Focused|Hover|Active|Checked`: a truthy value sets the
    /// built-in ElementState bit targetId (kStateFocus / kStateHover / kStateActive /
    /// kStateChecked), a falsy one clears it; no event, no trigger (like Checked / Disabled).
    BuiltinState = 14,
};
/// Highest valid BindTarget code (02 §2.s; IrView::open rejects a larger one).
inline constexpr uint8_t kBindTargetLast = 14;
/// IrBinding::flags: the chain reads another element (`#Other.width`), so it is evaluated again
/// after layout.
inline constexpr uint8_t kBindAfterLayout = 1u;
/// IrBinding::flags: the chain has no input port (it starts at an element reference or a
/// constant format); sources[] only lists the ports its arguments read.
inline constexpr uint8_t kBindNoPrimary = 2u;
/// IrBinding::flags (T-177): the binding sits inside a `<repeat>` template and its chain starts
/// at the repeat entry (`item(<field>)`, `index`, `count`); always together with kBindNoPrimary.
/// The chain's input is the entry's Struct value of the instance the binding is copied into.
inline constexpr uint8_t kBindRepeatItem = 4u;

/// Most custom state names one document may declare (03 §3.12.2): NodeCommon::customStates is a
/// u32 bitset.
inline constexpr uint32_t kMaxCustomStates = 32;

/// T-255 (18 §18.2, "every node at most 16 effects"): the most entries one `effect` /
/// `text-effect` declaration or one scene node's effect range may name; IrView::open rejects more.
inline constexpr uint32_t kMaxEffectSequence = 16;
/// T-255: a scene path's or shape's fill / stroke stop counts, dash count and width-profile count
/// are kept in uint16 fields at run time; IrView::open rejects a larger count.
inline constexpr uint32_t kMaxScenePaintListCount = 0xFFFFu;
/// T-255: element tree depth limit (the root has depth 0). IrView::open rejects an element at this
/// depth or deeper, so layout and hit testing never recurse past it. T-255b: 256, measured safe
/// on a 512 KiB stack in a Debug build (Context::update recurses about 1.7 KB a level, hit testing
/// about 0.6 KB; a 256-deep chain updates in about 430 KB), so a small engine job thread or an
/// Android main thread holds it too. 512 overflowed a 512 KiB stack.
inline constexpr uint32_t kMaxElementDepth = 256;

/// IrTrigger::event (27.2.2), numbered in the table's order. T-194 filled the reserved 17-19
/// (drag), 21 (gesture) and 22 (player) and appended 26-28.
enum class TriggerEvent : uint8_t {
    None = 0,
    Hover = 1,            ///< Pointer entered the element's hit area (PointerEnter).
    Leave = 2,            ///< Pointer left it (PointerLeave).
    Press = 3,            ///< PointerDown on the element or a descendant.
    Release = 4,          ///< PointerUp.
    Click = 5,            ///< Click.
    Focus = 6,            ///< Focus (not bubbling).
    Blur = 7,             ///< Blur (not bubbling).
    Check = 8,            ///< ElementState Checked turned on.
    Uncheck = 9,          ///< ElementState Checked turned off.
    Show = 10,            ///< The element (or, on the root, the surface) became visible.
    Hide = 11,            ///< A hide request (27.2.4): the actions may delay it with `then hide`.
    Port = 12,            ///< `port(<name>)`; arg0 = port name, arg1 = PORT index + 1 (T-175; 0 or
                          ///< kInvalidString = no declared port).
    TimelineEnd = 13,     ///< `timeline(<name>).end`; arg0 = timeline name.
    TimelineMarker = 14,  ///< `timeline(<name>).marker(<m>)`; arg0 = timeline, arg1 = marker name.
    StateEnter = 15,      ///< `state(<Name>).enter`; arg0 = custom state bit index.
    StateExit = 16,       ///< `state(<Name>).exit`; arg0 = custom state bit index.
    DragStart = 17,       ///< T-194: `drag.start` on the dragged element (or an ancestor).
    DragOver = 18,        ///< T-194: `drag.over`: a drag entered this drop target.
    Drop = 19,            ///< T-194: `drop` / `drag.drop`: a drag was released on this drop target.
    Key = 20,             ///< `key(<key>)` on the focused element; arg0 = KeyCode.
    Gesture = 21,         ///< T-194: `gesture(<kind>)`; arg0 = GestureKind, arg1 = NavDirection of
                          ///< `gesture(swipe(<dir>))` (kInvalidString = any direction).
    Player = 22,          ///< T-194: `player(<n>).<event>`; arg0 = player (0-based), arg1 = the
                          ///< inner TriggerEvent (Press, Release, Click, Focus, Blur, Hover, Leave,
                          ///< Back, NavBlocked).
    ItemEnter = 23,       ///< T-177: a `<repeat>` instance was inserted (`item-enter`, on the
                          ///< template root).
    ItemExit = 24,        ///< T-177: a `<repeat>` instance's entry was removed (`item-exit`).
    MediaEnd = 25,        ///< T-191: `media(<id>).end`; arg0 = the `<video>`/`<sequence>` id.
    DragLeave = 26,       ///< T-194: `drag.leave`: a drag left this drop target.
    Back = 27,            ///< T-194: `back` (gamepad B / Circle; bubbles up to the surface).
    NavBlocked = 28,      ///< T-194: `nav.blocked[(<dir>)]`; arg0 = NavDirection (kInvalidString =
                          ///< any direction).
    DragValue = 29,       ///< T-201: `drag.value` on a `draggable="axis-x"` element while the pointer
                          ///< drags along it; the event value is the position 0..1 across its box.
    NavValue = 30,        ///< T-201: `nav.value` on a focused `draggable="axis-x"` element for a
                          ///< left / right navigation; the event value is -1 / +1.
    DoubleClick = 31,     ///< T-379 (08.10): `dblclick` (EventType::DoubleClick).
    PointerCancel = 32,   ///< T-386 (08.11): `pointercancel` (EventType::PointerCancel).
    /// T-386 (08.11): `pointermove` (EventType::PointerMove: the active element while a button is
    /// held, else the element under the pointer; bubbles).
    PointerMove = 33,
    DragCancel = 34,  ///< T-388 (08.12): `drag.cancel` (EventType::DragCancel, on the drag source).
    /// T-387 (08.12): `scroll` (EventType::Scroll: a scroll container scrolled; bubbles).
    Scroll = 35,
    /// T-387 (08.12): `outsidepress` (EventType::OutsidePress: a press outside the element's
    /// subtree while it is visible; not bubbling).
    OutsidePress = 36,
    /// T-392 (27 §27.3.5): `change` (EventType::Change on the element itself: an `<input>` value
    /// edit or setValue(), a checkbox toggle).
    Change = 37,
    /// T-392 (27 §27.3.5): `input`, every edit of an `<input>`. Core's Change already fires once
    /// per edit, so this fires on the same EventType::Change, right after `change`.
    Input = 38,
    /// T-394 (08.15): `pointerdrag` (EventType::PointerDrag: a primary-button press on the element
    /// moved past its `drag-threshold`; once per frame with the last move, until the release).
    PointerDrag = 39,
    /// T-397 (08.17): `focusin` (EventType::FocusIn: the element or a descendant took the focus;
    /// bubbles).
    FocusIn = 40,
    /// T-397 (08.17): `focusout` (EventType::FocusOut: the element or a descendant lost the focus;
    /// bubbles).
    FocusOut = 41,
};
/// Highest TriggerEvent code (02 §2.v). A larger code is not an error: the trigger never fires.
inline constexpr uint8_t kTriggerEventLast = 41;

/// IrTriggerAction::kind (27.2.3), numbered in the table's order.
enum class TriggerAction : uint8_t {
    None = 0,
    Play = 1,   ///< arg0 = timeline, arg1 = `from=` marker (or kInvalidString), flags = kPlay* bits.
    Stop = 2,   ///< arg0 = timeline name.
    Seek = 3,   ///< arg0 = timeline, arg1 = marker (or kInvalidString), arg2 = seconds.
    State = 4,  ///< arg0 = custom state bit index, flags = TriggerMode (| kStateTarget: arg1).
    Class = 5,  ///< arg0 = class name, flags = TriggerMode.
    Show = 6,   ///< Shows the trigger's element (the root: the surface).
    Hide = 7,   ///< Hides the trigger's element (the root: starts or completes the surface hide).
    Sound = 8,  ///< arg0 = cue name, arg2 = volume. Sent to the host (HostEvent).
    /// T-201: `set(<port>, <value>)`; arg1 = PORT index + 1, flags = SetSource, arg2 = the number
    /// (Number) or a StringId stored bit-for-bit (String); an EventValue source's converters are
    /// `extra` steps at arg0 in the TRIG `conditions[]` pool (0 steps = none, arg0 unused).
    Set = 9,
    Emit = 10,   ///< arg0 = port name, arg1 = PORT index + 1 (T-175; 0 = undeclared). HostEvent.
    If = 11,     ///< arg0/arg1 = condition range in `conditions[]`; next = true branch, alt = else.
    Focus = 12,  ///< arg0 = target element id (T-392: kInvalidString = the trigger's element).
    /// T-201: `spawn(<symbol>, at=<id|pointer|self>, lifetime=<t>[, timeline=<name>])`; `extra` =
    /// which spawn pool of the trigger's element (the n-th `<repeat>` child flagged kRepeatSpawn),
    /// flags = SpawnAt, arg0 = the `at` element's (path) id (SpawnAt::Element), arg1 = the timeline
    /// played on the new instance (kInvalidString = none), arg2 = lifetime in seconds.
    Spawn = 13,
    Haptic = 14,  ///< arg0 = cue name, arg2 = strength (T-194; default 1). Sent to the host.
    Layer = 15,   ///< T-194: `layer(overlay|normal)`; flags = 1 overlay, 0 normal.
    Remove = 16,  ///< T-177: `remove` (only in `item-exit`): destroys the exiting instance.
    /// T-394 (08.15): `consume()` (only in `on:outsidepress`): the press is taken (Event::consume);
    /// decided when the OutsidePress is dispatched, so it does nothing when the action runs.
    Consume = 17,
    /// T-399 (08 §8.18): `reset(value)`: re-applies the trigger element's `bind:value` (the port
    /// value replaces the user's edit). The compiler accepts it only on an element with `bind:value`.
    Reset = 18,
};
/// Highest TriggerAction code (02 §2.v). A larger code is not an error: the action is skipped.
inline constexpr uint8_t kTriggerActionLast = 18;

/// IrTriggerAction::flags of a Play action (27.2.3 `play(<tl>[, reverse|loop|once|hold])`).
inline constexpr uint8_t kPlayReverse = 1u;
inline constexpr uint8_t kPlayLoop = 2u;
/// `once`: release the members at the end instead of holding the last frame.
inline constexpr uint8_t kPlayOnce = 4u;
/// `hold` written explicitly: a re-trigger while the timeline plays does not restart it.
inline constexpr uint8_t kPlayHold = 8u;
/// T-191: on a Play/Stop/Seek action, arg0 names a `<video>`/`<sequence>` element id instead of
/// a timeline (only `loop` combines with it on Play; a Seek carries seconds in arg2). T-202: or a
/// `<particles>` element id (Play/Stop only, no options): emission on/off.
inline constexpr uint8_t kActionMedia = 16u;

/// IrTriggerAction::flags of a State/Class action.
enum class TriggerMode : uint8_t { On = 0, Off = 1, Toggle = 2 };
/// T-201: on a State action, OR-ed into the TriggerMode: arg1 is the StringId of the target
/// element's (path) id (`state(<Name>, <mode>, target=#<id>)`) instead of the trigger's element.
inline constexpr uint8_t kStateTarget = 0x80u;

/// IrTriggerAction::flags of a Spawn action (T-201): where the new instance's top-left goes.
enum class SpawnAt : uint8_t {
    Self = 0,     ///< The centre of the trigger's element.
    Pointer = 1,  ///< The last pointer position.
    Element = 2,  ///< The centre of the element whose id is arg0.
};

/// IrTriggerAction::flags of a Set action (T-201): where the value comes from.
enum class SetSource : uint8_t {
    Number = 0,      ///< arg2 (`true` / `false` are 1 / 0).
    String = 1,      ///< The StringId in arg2's bits.
    Toggle = 2,      ///< `toggle`: the negation of the (bool) port's current value.
    EventValue = 3,  ///< `event.value`: the event's value (a `port()` event: that port's value),
                     ///< through the converter steps at arg0 / `extra`.
    // T-386 (08.11): the firing pointer event's position relative to an element whose (path) id
    // is the StringId in arg2's bits (Document::fractionIn / localPosIn), through the converter
    // steps at arg0 / `extra` like EventValue. Only in Pointer* / Click triggers.
    PointerFractionX = 4,  ///< `pointer.fraction(#id).x`: 0..1 across the border box.
    PointerFractionY = 5,  ///< `pointer.fraction(#id).y`.
    PointerLocalX = 6,     ///< `pointer.local(#id).x`: border-box px.
    PointerLocalY = 7,     ///< `pointer.local(#id).y`.
    /// T-392 (27 §27.3.5): `value`: the trigger element's current text (an editable `<input>`'s
    /// value), through the converter steps at arg0 / `extra` like EventValue. Only in `change` /
    /// `input` triggers.
    ElementValue = 8,
    // T-411 (27 §27.2.3, IR 2.9): bind-source values, read when the action runs (after any `then`
    // wait), through the converter steps at arg0 / `extra` like EventValue. Number / String also
    // take converter steps since 2.9 (`5 | scale(2)`); before, `extra` was always 0 for them.
    /// `port(<name>)`: that port's current value; arg2's bits are the PORT index (in a `<repeat>`
    /// instance a template port means the instance's copy, as the set() target does).
    Port = 9,
    /// `item(<field>)` / `item()` / `repeat.index` / `repeat.count`: the chain's input is the list
    /// entry of the `<repeat>` instance holding the trigger's element, and its first step reads the
    /// field / index / count (as a kBindRepeatItem binding). Nothing is set outside a live instance.
    Item = 10,
};
/// Highest SetSource value.
inline constexpr uint8_t kSetSourceLast = 10;

/// T-386 (08.11): IrTriggerAction::flags of an If action. kIfActive: the condition is `active`
/// (the trigger's element has ElementState kStateActive: a press on it or inside it is in
/// progress) instead of the comparison chain at arg0 / arg1 (then an empty range).
inline constexpr uint8_t kIfActive = 1u;
/// T-390 (08.13): IrTriggerAction::flags of an If action. kIfTerms: arg0 / arg1 is a list of
/// terms that must all hold (`and`), instead of one comparison chain. Each term is a header step
/// followed by `argCount` comparison steps of its own: the header has op =
/// ConverterOp::Operands (never run as a converter), args[0] = IfTerm, args[1] = 1 when negated
/// (`not`), args[2] = the operand (IfTerm::Button: the PointerButton; IfTerm::State: the custom
/// state bit, or with args[3] = 1 the ElementState bit; IfTerm::Port: the PORT index, argKinds
/// marking it ConverterArg::Port). kIfTerms and kIfActive are never combined.
inline constexpr uint8_t kIfTerms = 2u;

/// T-390 (08.13): the kind of one kIfTerms term (stored as a float in the header's args[0]).
enum class IfTerm : uint8_t {
    Compare = 0,   ///< The event value (a port event: that port) through the comparison steps.
    Active = 1,    ///< `active` (as kIfActive); no comparison steps.
    Button = 2,    ///< `button(<b>)`: the firing pointer event's button; no comparison steps.
    State = 3,     ///< `state(<Name>)`: the trigger's element has the state; no comparison steps.
    Port = 4,      ///< `port(<name>) <op> <value>`: that port's value through the comparison steps.
    External = 5,  ///< T-393 (08.14) `external`: the firing drag event comes from an OS drag; no
                   ///< comparison steps.
    Dragging = 6,  ///< T-394 (08.15) `dragging`: the trigger's element is in a pointerdrag; no
                   ///< comparison steps.
};
/// Highest IfTerm value.
inline constexpr uint8_t kIfTermLast = 6;

/// T-390 (27.3.2): on a State action, OR-ed into the TriggerMode: the action sets a built-in
/// ElementState instead of a custom state. arg0 = 0 is kStateDisabled (`state(Disabled, ...)`,
/// T-390); T-392 (27.3.5): otherwise arg0 is the ElementState bit itself (`state(Focused|Hover|
/// Active|Checked, ...)`: kStateFocus / kStateHover / kStateActive / kStateChecked).
inline constexpr uint8_t kStateBuiltinDisabled = 0x40u;
/// T-392: the same flag under its general name.
inline constexpr uint8_t kStateBuiltin = kStateBuiltinDisabled;

/// IrConverterStep::op: the converter catalog of docs/design/27-interaction.md §27.3.3, numbered
/// in the catalog's order (T-174 fixed the comparisons at 7-13; T-175 the rest). An `if`
/// condition (TRIG) only ever uses the comparisons.
enum class ConverterOp : uint8_t {
    /// Not a converter: carries four more arguments of the preceding step (format, concat,
    /// select, step, a host converter). See IrConverterStep::argCount.
    Operands = 0,
    Range = 1,   ///< range(inMin, inMax, outMin, outMax); clamped; `%` outputs give a percent length.
    Clamp = 2,   ///< clamp(min, max).
    Round = 3,   ///< round([decimals = 0]).
    Abs = 4,     ///< abs.
    Scale = 5,   ///< scale(k): v * k.
    Offset = 6,  ///< offset(k): v + k.
    Lt = 7,
    Le = 8,
    Gt = 9,
    Ge = 10,
    Eq = 11,
    Ne = 12,
    Between = 13,    ///< args[0] <= v <= args[1].
    Not = 14,        ///< not.
    And = 15,        ///< and(x).
    Or = 16,         ///< or(x).
    Format = 17,     ///< format(pattern, a1, ...): `{0}` = the input, `{i}` = argument i.
    Thousands = 18,  ///< thousands: rounded, `,` every three digits.
    Fixed = 19,      ///< fixed(n): n decimals.
    Percent = 20,    ///< percent([decimals = 0]): v * 100 followed by `%`.
    Time = 21,       ///< time([pattern = mm:ss]): seconds -> `hh`/`h`/`mm`/`m`/`ss`/`s` fields.
    Upper = 22,      ///< upper.
    Lower = 23,      ///< lower.
    Select = 24,     ///< select(k1: v1, ..., default: d); arguments [default, k1, v1, ...].
    Step = 25,       ///< step(t1: v1, ...); arguments [default, t1, v1, ...], ascending t.
    LerpColor = 26,  ///< lerpColor(c1, c2): the input (0..1) mixes two colors.
    FromToken = 27,  ///< fromToken(name): `var(--name)` (`{0}` in the name = the input).
    Localize = 28,   ///< localize: `@key` resolution (T-179 provides the lookup).
    Concat = 29,     ///< concat(a1, ...): the input followed by every argument, as text.
    Glyph = 30,      ///< glyph([action]): the action's input glyph (IGlyphProvider, later task).
    Tween = 31,      ///< tween(seconds[, easing]): numeric changes interpolate (04 §4.10.4);
                     ///< arguments [seconds, EasingKind, x1|stepCount, y1|jumpEnd, x2, y2].
    Delay = 32,      ///< delay(seconds): changes pass through after that long.
    Count = 33,      ///< count: a list's entry count.
    Index = 34,      ///< index: the `<repeat>` entry index (T-177; 0 until then).
    Item = 35,       ///< item(field): a struct field.
    ElemProp = 36,   ///< `#Other.<prop>`: args[0] = element id, args[1] = property name (strings).
    // T-200: vector components. On a Vec2/Vec3 the component (z of a Vec2 and w of either are
    // 0); on a Color r/g/b/a (0..1); on a number x is the number itself, the others 0.
    X = 37,  ///< x (or r).
    Y = 38,  ///< y (or g).
    Z = 39,  ///< z (or b).
    W = 40,  ///< w (or a).
    /// T-200: `vec2(...)` / `vec3(...)`: a Vec2 (argCount 2) or Vec3 (argCount 3) of the arguments
    /// (Number or Port); an argument of kind None is the chain's input (`<port> | vec2(y)`).
    Vec = 41,
    /// Host converters (13.8.1 registerConverter) are numbered from here at run time; in the IR
    /// every host converter is this op with args[0] = its name (a String argument).
    Custom = 0x80,
};
/// Highest catalog op (ConverterOp::Vec, T-200).
inline constexpr uint8_t kConverterOpLast = 41;

/// Kind of one IrConverterStep argument, 4 bits per argument in IrConverterStep::argKinds
/// (T-175). Kind 0 keeps T-174's all-constant conditions valid.
enum class ConverterArg : uint8_t {
    Number = 0,   ///< The float itself.
    String = 1,   ///< A StringId, stored bit-for-bit in the float slot.
    Port = 2,     ///< A PORT table index, stored bit-for-bit in the float slot.
    Percent = 3,  ///< A percentage (20 for `20%`): a percent length for range() outputs, else
                  ///< relative to the `<port>_max` companion of the binding's (or event's) port.
    None = 4,     ///< Absent (select/step without `default:`).
    Item = 5,     ///< T-177: `item(<field>)` of the `<repeat>` entry (a StringId field name).
    Index = 6,    ///< T-177: the `<repeat>` entry index.
    Count = 7,    ///< T-177: the `<repeat>` list's entry count.
    /// T-391 (27 §27.3.4): `outer.item(<field>)`, a field of the enclosing `<repeat>`'s entry
    /// (a StringId field name), inside a nested repeat's template.
    OuterItem = 8,
    /// T-391: `outer.index`, the enclosing `<repeat>`'s entry index.
    OuterIndex = 9,
};

/// `IrDecl::valueType` of a `var(--name[, fallback])` reference (05.9.2, T-170). Not a
/// ValueType: it only ever appears in a style declaration, never in a runtime Value. The value
/// slots are u32 0 = the token's name StringId (`--name`), u32 1 = an index into the `VARS`
/// chunk's `fallbacks[]`, or kNoVarFallback.
inline constexpr uint8_t kDeclVarRef = 11;
/// "No fallback" marker for a kDeclVarRef declaration's second value slot.
inline constexpr uint32_t kNoVarFallback = 0xFFFFFFFFu;

/// `CALC` chunk type: `calc()` programs (docs/design/05-style-and-css.md §5.9.2, 06-layout.md
/// §6.4, T-176). Optional; written after every other chunk. Payload: `u32 calcCount`,
/// `IrCalc calcs[]`, `u32 opCount`, `IrCalcOp ops[]`.
inline constexpr uint32_t kChunkCALC = fourcc('C', 'A', 'L', 'C');
/// `MDIA` chunk type: `@media` conditional rules (05.9.2-5.9.3, 12.7's `MEDIA` block, T-176; a
/// fourcc has four letters). Optional; written after every other chunk. Payload: `u32 count`,
/// `IrMedia media[]`, `u32 count`, `IrMediaQuery queries[]`, `u32 count`, `IrMediaCond conds[]`,
/// `u32 count`, `IrMediaRule rules[]` (ascending by rule, each STYL rule at most once).
inline constexpr uint32_t kChunkMDIA = fourcc('M', 'D', 'I', 'A');

/// `STRS` chunk type: the localization table (docs/design/07-text.md §7.8, 12-compiler.md §12.7,
/// T-179). Optional; written after every other chunk, only when the document has an `@key` or an
/// `@font-fallback`. Payload: `u32 count, u32 keys[]` (StringId of each key, without its `@`);
/// `u32 count, u32 textKeys[]` (0 or one per DOCT `texts[]` entry: 1-based index into `keys[]`,
/// 0 = not localized); `u32 count, u32 attrKeys[]` (0 or one per DOCT `attrPool[]` entry, same
/// encoding); `u32 count, IrFontFallback fallbacks[]`; `u32 count, u32 fallbackFamilies[]`
/// (StringIds).
inline constexpr uint32_t kChunkSTRS = fourcc('S', 'T', 'R', 'S');

/// `IrDecl::valueType` of a `calc()` value (T-176). Like kDeclVarRef, never a runtime ValueType;
/// value slot u32 0 = the program's index in the `CALC` chunk. Only on Length-typed properties.
inline constexpr uint8_t kDeclCalc = 12;

/// One RPN instruction of a `calc()` program (IrCalcOp::op, T-176).
enum class CalcOpKind : uint8_t {
    PushLength = 0,  ///< Push `value` in LengthUnit `unit` (never Auto or Calc).
    PushNumber = 1,  ///< Push the unitless number `value`.
    /// Push token `name` (a StringId, with its leading `--`), looked up when the style is
    /// resolved; when `hasFallback`, `value`/`unit` is the fallback (unit kCalcUnitNumber = a
    /// number).
    PushVar = 2,
    Add = 3,  ///< a + b (both lengths or both numbers).
    Sub = 4,  ///< a - b (both lengths or both numbers).
    Mul = 5,  ///< a * b (at least one a number).
    Div = 6,  ///< a / b (b a non-zero number).
    /// T-330: the smaller of a and b (both lengths or both numbers). `min(a, b, c)` is a chain;
    /// `clamp(a, b, c)` is `max(a, min(b, c))`, in RPN `a b c Min Max`.
    Min = 7,
    Max = 8,  ///< T-330: the larger of a and b (both lengths or both numbers).
};

/// IrCalcOp::unit of a PushVar fallback that is a plain number.
inline constexpr uint8_t kCalcUnitNumber = 0xFF;

/// `@media` feature of one IrMediaCond (T-176).
enum class MediaFeature : uint8_t {
    MinWidth = 0,     ///< viewport width >= value (logical px).
    MaxWidth = 1,     ///< viewport width <= value.
    MinHeight = 2,    ///< viewport height >= value.
    MaxHeight = 3,    ///< viewport height <= value.
    MinAspect = 4,    ///< `(aspect-ratio >= value)`, value = width / height.
    MaxAspect = 5,    ///< `(aspect-ratio <= value)`.
    Orientation = 6,  ///< value 0 = landscape (width >= height), 1 = portrait.
    Platform = 7,     ///< value = DeviceKind (xt/layout.h).
    Input = 8,        ///< value = InputKind (xt/layout.h).
    /// T-330: `(prefers-reduced-motion)`, value 0 = no-preference, 1 = reduce
    /// (DeviceInfo::reducedMotion).
    ReducedMotion = 9,
    MinResolution = 10,  ///< T-330: DeviceInfo::dpr >= value (dppx).
    MaxResolution = 11,  ///< T-330: DeviceInfo::dpr <= value.
    Resolution = 12,     ///< T-330: DeviceInfo::dpr == value (1e-4 tolerance).
};

// Node/paint/value enums live in xt/node.h (shared with the runtime scene graph).
// EasingKind lives in xt/timeline.h (shared with the runtime timeline, included above).

#pragma pack(push, 1)

// -------------------------------------------------------------------------------------------
// §2.2 / §2.3 header + chunk header
// -------------------------------------------------------------------------------------------

/// T-218: the IR minor version this compiler writes (IrHeader::minor). 1 = T-115's STYL
/// `gradientPool[]`; 2 = T-218's STYL `floatPool[]`, kIrSelSceneTarget selector parts and
/// IrSceneNode::svgTag; 3 = T-225's SCEN `modifiers[]`/`shapes[]` tail (Modifier/Shape nodes);
/// 4 = T-233's stroke taper / width profile fields at the end of IrPathData and IrShapeData;
/// 5 = T-243's rig: SCEN `bones[]`...`meshIndexPool[]` tail, IrPathData::skinIndex and
/// IrBinding::sceneNode; 6 = T-335's STYL `easingPoints[]` (EasingKind::LinearPoints) and
/// LengthUnit::Rem; 7 = T-358's PORT `defaults[]` tail (port default values); 8 = T-405 (43
/// §43.5): the T-380..T-399 fields made formal (no layout change), `hit-slop` written as the four
/// Length longhands StyleProp 277-280 instead of the biased StyleProp 276 (read only from a minor
/// < 8 file), and the optional META `behavior` tail; 9 = T-411's SetSource Port / Item (27
/// §27.2.3) and converter steps on Number / String set() values.
inline constexpr uint16_t kIrFormatMinor = 9;

/// T-405 (02 §2.6): the highest StyleProp number an IR of this minor may carry (core/src/
/// style_decode.h and compile/src/xts/xts_props.h static_assert their last StyleProp against it).
/// A larger number is not an error: the declaration is ignored.
inline constexpr uint16_t kStylePropLast = 280;

/// `.xtir` file header (§2.2, 16 bytes).
struct IrHeader {
    /// File signature, always `"XTIR"`.
    char magic[4];
    /// Format major version (currently 2); a reader rejects a mismatching major version. 1.x files
    /// are no longer supported.
    uint16_t major;
    /// Format minor version (kIrFormatMinor; 0 was everything before T-115's STYL `gradientPool[]`,
    /// 1 everything before T-218's STYL `floatPool[]`).
    /// A newer minor version's unknown chunk types are ignored, and a chunk's trailing new arrays
    /// are treated as absent when the payload ends early (see IrView::parseStyle).
    uint16_t minor;
    /// Number of chunks that follow the header.
    uint32_t chunkCount;
    /// Total file length in bytes, including this header.
    uint32_t totalLength;
};
static_assert(sizeof(IrHeader) == 16);

/// Chunk header preceding every chunk's payload (§2.3, 12 bytes).
struct IrChunkHeader {
    /// Chunk type fourcc, e.g. `"STRT"`, `"ASST"`, `"STYL"`, `"SCEN"`, `"TLIN"`, `"DOCT"`, `"META"`.
    char type[4];
    /// Payload length in bytes (includes the 4-byte alignment padding at the payload's end).
    uint32_t payloadLength;
    /// CRC32 of the payload; verified only when `IrReadOptions::verifyCrc` is set.
    uint32_t crc32;
};
static_assert(sizeof(IrChunkHeader) == 12);

// -------------------------------------------------------------------------------------------
// §2.5 ASST
// -------------------------------------------------------------------------------------------

/// `ASST` chunk asset table entry (§2.5, 16 bytes).
struct IrAsset {
    /// StringId of the asset's logical name (e.g. `"images/logo.png"`).
    uint32_t name;
    /// Asset kind: 0 = Image, 1 = Font, 2 = Shader.
    uint8_t kind;
    /// 1 = bytes are embedded in this chunk (see dataOffset/dataLength), 0 = resolved by
    /// AssetResolver from `name`.
    uint8_t embedded;
    uint16_t _pad;  ///< Padding, unused.
    /// Byte offset of the embedded data, relative to the start of the ASST chunk payload (§2.1;
    /// IrWriter and AssetChunk::data use the same base). Valid only when `embedded` is set.
    uint32_t dataOffset;
    /// Byte length of the embedded data. Valid only when `embedded` is set.
    uint32_t dataLength;
};
static_assert(sizeof(IrAsset) == 16);

// -------------------------------------------------------------------------------------------
// §2.6 STYL
// -------------------------------------------------------------------------------------------

/// `STYL` chunk compiled style rule: a selector chain + a set of declarations + specificity
/// (§2.6, 20 bytes).
struct IrRule {
    /// Offset/count into `selectorParts[]`, ordered right-to-left (subject first, ancestors
    /// after).
    uint32_t selOffset;
    /// Number of `IrSelPart` entries at `selOffset`.
    uint32_t selCount;
    /// Offset/count into `decls[]`.
    uint32_t declOffset;
    /// Number of `IrDecl` entries at `declOffset`.
    uint32_t declCount;
    /// `(idCount << 16) | ((classCount + pseudoCount) << 8) | tagCount`. Source order is this
    /// rule's index in the `rules[]` array.
    uint32_t specificity;
};
static_assert(sizeof(IrRule) == 20);

/// One compound selector part of an `IrRule`'s selector chain (§2.6, 16 bytes).
///
/// T-173 (05.9.2) appends combinator value 3, kIrSelCondition: such a part is not a compound of its
/// own but one extra condition on the compound just before it in the chain (the subject, or the
/// last part with combinator 1/2), all of whose conditions must hold. In a condition part:
///   - `classCount` = the IrSelCond code, OR'd with kIrSelCondNegated for a `:not()` argument;
///   - `stateMask` = the ElementState bit(s) of IrSelCond::State, else 0;
///   - `tag` = the tag / class / attribute name / `:lang()` prefix StringId (Tag, Class, Attr*,
///     Lang), or `a` of `an+b` as an int32 bit pattern (NthChild), else kInvalidString;
///   - `id` = the id StringId (Id), the attribute value StringId (AttrEquals/Prefix/Suffix/
///     Substring), or `b` of `an+b` as an int32 bit pattern (NthChild), else kInvalidString;
///   - `classOffset` = 0.
///
/// T-218 (31 §31.5.1) appends combinator value 4, kIrSelSceneTarget: the rule addresses nodes of
/// the subject element's instantiated scene (an inline `<svg>`'s content: `#Icon/Blade`,
/// `#Icon/path`, `#Icon/*`) instead of the element itself. It is only ever `parts[1]`, right after
/// the subject compound (whose condition parts, if any, follow it). In a scene-target part:
///   - `classCount` = the IrSceneTarget code (0 name, 1 tag, 2 any);
///   - `tag` = the node name StringId (Name) or the SVG tag name StringId (Tag, one of
///     irSvgTagCode()'s names), kInvalidString for Any;
///   - `stateMask` = 0, `id` = kInvalidString, `classOffset` = 0.
/// Matching ignores the part; StyleEngine routes such a rule's declarations to the scene nodes.
struct IrSelPart {
    /// 0 = subject (first part), 1 = descendant combinator (space), 2 = child combinator (`>`),
    /// 3 = condition on the preceding compound (kIrSelCondition, T-173), 4 = scene target
    /// (kIrSelSceneTarget, T-218).
    uint8_t combinator;
    /// Number of class selectors, at `classOffset` in `classIds[]`.
    uint8_t classCount;
    /// Required ElementState bits (e.g. `:hover`).
    uint16_t stateMask;
    /// StringId of the tag selector, or kInvalidString (wildcard).
    uint32_t tag;
    /// StringId of the id selector, or kInvalidString.
    uint32_t id;
    /// Offset into `classIds[]`.
    uint32_t classOffset;
};
static_assert(sizeof(IrSelPart) == 16);

/// IrSelPart::combinator of a condition part (T-173; see IrSelPart).
inline constexpr uint8_t kIrSelCondition = 3;
/// Set in a condition part's `classCount` when it is a `:not()` argument.
inline constexpr uint8_t kIrSelCondNegated = 0x80;

/// IrSelPart::combinator of a scene-target part (T-218; see IrSelPart).
inline constexpr uint8_t kIrSelSceneTarget = 4;

/// `classCount` of a kIrSelSceneTarget part (T-218).
enum class IrSceneTarget : uint8_t {
    Name = 0,  ///< Every scene node whose name (`id` in the SVG) is `tag`.
    Tag = 1,   ///< Every scene node whose IrSceneNode::svgTag is irSvgTagCode(`tag`'s string).
    Any = 2,   ///< Every scene node except the scene root.
};

/// IrSceneNode::svgTag codes (T-218): the SVG element a parseSvg node came from; 0 = none (the
/// scene root, a `<use>` wrapper, `<a>`, a nested `<svg>`, or a non-SVG scene).
enum class IrSvgTag : uint8_t {
    None = 0,
    Path = 1,
    Rect = 2,
    Circle = 3,
    Ellipse = 4,
    Line = 5,
    Polygon = 6,
    Polyline = 7,
    G = 8,
    XtStar = 9,         ///< T-225: `<xt-star>`.
    Modifier = 10,      ///< T-225: any `<xt-trim>`...`<xt-merge>` modifier element (no tag name).
    XtSkeleton = 11,    ///< T-243: `<xt-skeleton>` (a Group).
    XtBone = 12,        ///< T-243: `<xt-bone>`.
    XtConstraint = 13,  ///< T-243: `<xt-constraint>`.
    XtMesh = 14,        ///< T-243: `<xt-mesh>`.
};

/// The IrSvgTag code of an SVG element name, IrSvgTag::None for any other name. T-236 (05
/// §5.9.7): `modifier` names every `<xt-*>` modifier element (IrSvgTag::Modifier).
inline IrSvgTag irSvgTagCode(std::string_view name) {
    constexpr std::string_view kNames[] = {
        "path", "rect",    "circle",   "ellipse",     "line",    "polygon",       "polyline",
        "g",    "xt-star", "modifier", "xt-skeleton", "xt-bone", "xt-constraint", "xt-mesh"};
    for (uint8_t i = 0; i < 14; ++i) {
        if (name == kNames[i]) {
            return static_cast<IrSvgTag>(i + 1);
        }
    }
    return IrSvgTag::None;
}

/// Condition codes of a kIrSelCondition part (T-173, 05.9.2); append-only.
enum class IrSelCond : uint8_t {
    Tag = 1,
    Id = 2,
    Class = 3,
    State = 4,
    Any = 5,  ///< `:not(*)`.
    AttrExists = 6,
    AttrEquals = 7,
    AttrPrefix = 8,
    AttrSuffix = 9,
    AttrSubstring = 10,
    NthChild = 11,
    FirstChild = 12,
    LastChild = 13,
    FocusWithin = 14,
    Lang = 15,
    /// T-174 (27.1): `:state(<Name>)`; `tag` = the state name's StringId (one of STAT's names).
    CustomState = 16,
    /// T-194 (08.9): `:focus-player(<n>)`; `tag` = the player index (0-based, not a StringId).
    FocusPlayer = 17,
};
/// Highest valid IrSelCond code.
inline constexpr uint8_t kIrSelCondLast = 17;

/// A single style declaration, shared format between `STYL`'s rules and `DOCT`'s inline
/// declarations (§2.6, 24 bytes).
struct IrDecl {
    /// StyleProp id (animatable PropIds and non-animatable style properties share one numbering
    /// space; see 05.4).
    uint16_t prop;
    /// ValueType this declaration's `value` is encoded as (Decl additionally allows StringRef = 8
    /// and ListRef = 9, style-only, never in a Keyframe).
    uint8_t valueType;
    /// 1 = `!important`.
    uint8_t important;
    uint32_t _pad;  ///< Padding, unused.
    /// Encoded per `valueType`: Float = f32 at 0; Vec2 = f32 x, f32 y; Color = f32 r,g,b,a;
    /// Length = f32 value + u8 unit; Enum/StringId = u32; ListRef = u32 offset, u32 count (into
    /// transitions[]/animations[]/fontFamilies[] depending on the property). T-218: a `d`
    /// (PropId PathData) ListRef is u32 pointOffset, u32 pointCount, u32 verbOffset, u32 verbCount
    /// into STYL's `floatPool[]` (verbs stored as floats, like a Path keyframe's).
    uint8_t value[16];
};
static_assert(sizeof(IrDecl) == 24);

/// One `VARS` token (05.9.2, 12.7, T-170, 24 bytes): `--name` and its base value. `valueType` is
/// ValueType::Float, Length, Color or StringRef (the value's source text, for anything the
/// compiler could not type on its own, e.g. an enum keyword); `value` uses IrDecl's encoding.
struct IrVar {
    /// StringId of the token name, including its leading `--`.
    uint32_t name;
    /// ValueType of `value` (Float, Length, Color or StringRef).
    uint8_t valueType;
    uint8_t _pad[3];  ///< Padding, unused.
    /// Encoded like IrDecl::value.
    uint8_t value[16];
};
static_assert(sizeof(IrVar) == 24);

/// Serialized easing function, shared by IrTransitionSpec, IrAnimationSpec and IrKeyframe
/// (§2.6/§2.8, 20 bytes).
struct IrEasing {
    /// EasingKind (xt/timeline.h).
    uint8_t kind;
    /// Steps easing's "jump-end" flag.
    uint8_t stepsJumpEnd;
    /// Steps easing's step count.
    uint16_t stepCount;
    /// Cubic-bezier control points (unused unless `kind` is a cubic-bezier kind).
    float x1;  ///< Cubic-bezier first control point x.
    float y1;  ///< Cubic-bezier first control point y.
    float x2;  ///< Cubic-bezier second control point x.
    float y2;  ///< Cubic-bezier second control point y.
};
static_assert(sizeof(IrEasing) == 20);

// T-335 (IR minor 6): a LinearPoints IrEasing names its points in the STYL chunk's
// `easingPoints[]` (EasingPoint {f32 in, f32 out}, 8 bytes, shared by STYL's transitions /
// animations and every TLIN keyframe): `stepCount` holds the point count, the four bytes of `x1`
// hold the u32 index of the first point (at most 0xFFFF), `stepsJumpEnd`, `y1`, `x2`, `y2` are 0.
static_assert(sizeof(EasingPoint) == 8);

/// T-335: the first point index of a LinearPoints IrEasing.
inline uint32_t irEasingPointOffset(const IrEasing& e) {
    uint32_t v;
    std::memcpy(&v, &e.x1, sizeof v);
    return v;
}

/// T-335: the runtime Easing of `e` (a LinearPoints one keeps its pool range, the rest as is).
inline Easing easingFromIr(const IrEasing& e) {
    Easing out{static_cast<EasingKind>(e.kind), e.stepsJumpEnd, e.stepCount, e.x1, e.y1, e.x2, e.y2};
    if (out.kind == EasingKind::LinearPoints) {
        out = Easing{};
        out.kind = EasingKind::LinearPoints;
        out.pointOffset = static_cast<uint16_t>(irEasingPointOffset(e));
        out.pointCount = e.stepCount;
    }
    return out;
}

/// T-335: encodes a LinearPoints IrEasing over `count` points from `offset`.
inline IrEasing irLinearPointsEasing(uint16_t offset, uint16_t count) {
    IrEasing e{};
    e.kind = static_cast<uint8_t>(EasingKind::LinearPoints);
    e.stepCount = count;
    const uint32_t v = offset;
    std::memcpy(&e.x1, &v, sizeof v);
    return e;
}

/// `STYL` chunk compiled `transition` spec (§2.6, 32 bytes).
struct IrTransitionSpec {
    /// PropId this transition applies to; 0xFFFF means "all".
    uint16_t prop;
    uint16_t _pad;  ///< Padding, unused.
    /// Transition duration, in seconds.
    float duration;
    /// Transition start delay, in seconds.
    float delay;
    /// Timing function.
    IrEasing easing;
};
static_assert(sizeof(IrTransitionSpec) == 32);

/// `STYL` chunk compiled `animation` spec (§2.6, 48 bytes).
struct IrAnimationSpec {
    /// TimelineId of the `TLIN` chunk to play.
    uint32_t timeline;
    /// Animation duration, in seconds.
    float duration;
    /// Animation start delay, in seconds.
    float delay;
    /// Default timing function (a keyframe's own easing may override it).
    IrEasing easing;
    /// Iteration count; 0 = infinite.
    uint16_t iterations;
    /// 0 = normal, 1 = reverse, 2 = alternate, 3 = alternate-reverse.
    uint8_t direction;
    /// FillMode.
    uint8_t fill;
    /// 0 = running, 1 = paused.
    uint8_t playState;
    uint8_t _pad[3];    ///< Padding, unused.
    uint32_t _pad2[2];  ///< Padding, unused.
};
static_assert(sizeof(IrAnimationSpec) == 48);

/// `STYL` chunk `@keyframes` name-to-timeline mapping, referenced by IrAnimationSpec via name
/// lookup (§2.6, 8 bytes).
struct IrKeyframesRef {
    /// StringId of the `@keyframes` name.
    uint32_t name;
    /// TimelineId of the compiled `TLIN` chunk.
    uint32_t timeline;
};
static_assert(sizeof(IrKeyframesRef) == 8);

/// `STYL` chunk `@font-face` declaration (§2.6, 12 bytes).
struct IrFontFace {
    /// StringId of the font family name.
    uint32_t family;
    /// AssetId of the font file (ASST kind = Font).
    uint32_t asset;
    /// Font weight, 100..900.
    uint16_t weight;
    /// 1 = italic.
    uint8_t italic;
    uint8_t _pad;  ///< Padding, unused.
};
static_assert(sizeof(IrFontFace) == 12);

/// Compiled visual effect instance, shared by `STYL::effects[]` and `SCEN::effects[]` (§2.6/§2.7,
/// 80 bytes). Layout matches docs/design/18-effects.md §18.2's EffectInstance.
struct IrEffect {
    /// EffectKind (18-effects.md); valid range {0..8, 128}.
    uint8_t kind;
    /// 1 = enabled.
    uint8_t enabled;
    uint8_t _pad[2];  ///< Padding, unused.
    /// AssetId of a Shader asset (ASST kind = Shader), or 0xFFFFFFFF when not used by `kind`.
    uint32_t shader;
    /// Effect-kind-specific numeric parameters.
    float params[8];
    /// Effect-kind-specific colors.
    Color colors[2];
    /// Offset/count into the owning chunk's stop pool (STYL's `effectStops[]` or SCEN's
    /// `stopPool[]`), for effect kinds that use a gradient (e.g. GradientMask).
    uint32_t stopOffset;  ///< Offset into the owning chunk's stop pool.
    uint32_t stopCount;   ///< Count of stops.
};
static_assert(sizeof(IrEffect) == 80);

/// Compiled `background-gradient` (§2.6's `gradientPool[]`, docs/design/05-style-and-css.md §5.5c,
/// T-115, 32 bytes). Referenced by an `IrDecl`'s `ListRef` value when `prop == 190`; the color
/// stops live in the same STYL `effectStops[]` pool `IrEffect` uses.
struct IrGradient {
    /// GradientKind (xt/node.h): 1 = linear, 2 = radial, 3 = conic (T-201; params[0] = start
    /// angle in degrees, params[2..3] = cx, cy) (0 never reaches the pool).
    uint8_t kind;
    /// LengthUnit of each `params[]` entry; `params[0]`'s unit is unused for a linear gradient
    /// (its angle is always stored in degrees).
    uint8_t units[4];
    uint8_t _pad[3];  ///< Padding, unused.
    /// Linear: `params[0]` is the gradient-line angle in degrees and the rest are unused. Radial:
    /// rx, ry, cx, cy, relative to the element's box.
    float params[4];
    /// Offset into the STYL `effectStops[]` pool.
    uint32_t stopOffset;
    /// Number of color stops.
    uint32_t stopCount;
};
static_assert(sizeof(IrGradient) == 32);

// -------------------------------------------------------------------------------------------
// §2.7 SCEN
// -------------------------------------------------------------------------------------------

/// `SCEN` chunk scene node, indexed by local NodeId; `nodes[0]` is the root Group. `nodes[]` is
/// depth-first preorder, parent before child; siblings' array order is draw order (earlier =
/// lower) (§2.7, 96 bytes).
struct IrSceneNode {
    /// NodeKind (Group/Path/Image/Mask/Camera).
    uint8_t kind;
    /// NodeCommon::visible.
    uint8_t visible;
    /// BlendMode.
    uint8_t blend;
    /// MaskMode (only meaningful for kind == Mask).
    uint8_t maskMode;
    /// Local NodeId of the parent; the root's parent is 0xFFFFFFFF.
    uint32_t parent;
    /// Index into `paths[]` for kind == Path, into `images[]` for kind == Image, into `cameras[]`
    /// for kind == Camera, into `modifiers[]` for kind == Modifier and into `shapes[]` for kind ==
    /// Shape (T-225), into `bones[]` / `constraints[]` / `meshes[]` for kind == Bone / Constraint /
    /// Mesh (T-243), otherwise 0.
    uint32_t kindIndex;
    /// NodeCommon::opacity.
    float opacity;
    /// NodeCommon transform: anchor point, translation and scale.
    Vec2 anchor;     ///< Transform anchor point.
    Vec2 translate;  ///< Transform translation.
    Vec2 scale;      ///< Transform scale.
    /// NodeCommon::rotation, degrees.
    float rotation;
    /// NodeCommon::skew.
    Vec2 skew;
    /// StringId of the node's Lottie layer name (queryable via the API).
    uint32_t name;
    /// 3D transform extensions (docs/design/19-3d-space.md); zero for a purely 2D node.
    float translateZ;  ///< Z translation.
    float anchorZ;     ///< Transform anchor point Z.
    float rotationX;   ///< Rotation around the X axis, degrees.
    float rotationY;   ///< Rotation around the Y axis, degrees.
    /// Additional free-axis orientation (19-3d-space.md); {0,0,0} when unused.
    Vec3 orientation;
    /// 1 = this node participates in 3D transform/compositing.
    uint8_t is3D;
    /// 1 = the node is hidden when its back face is toward the camera.
    uint8_t backfaceHidden;
    /// T-218: IrSvgTag of the SVG element this node came from (parseSvg); 0 otherwise.
    uint8_t svgTag;
    /// T-228: NodeCommon::autoOrient (Lottie `ao`); 0 for every other source (was padding).
    uint8_t autoOrient;
    /// Offset/count into the chunk's `effects[]` (18-effects.md); 0, 0 when the node has no
    /// effects.
    uint32_t effectOffset;  ///< Offset into the chunk's `effects[]`.
    uint32_t effectCount;   ///< Count of effects.
    // No trailing `_pad` here: the task sheet's field list summed to exactly 96 bytes without one
    // (an explicit trailing `uint32_t _pad[1]` in the sheet's listing would make this struct 100
    // bytes, contradicting the sheet's own `static_assert(... == 96)`; omitted per the mandated
    // size, see report).
};
static_assert(sizeof(IrSceneNode) == 96);

/// `SCEN`/`DOCT` chunk 3D camera data referenced by an `IrSceneNode`/`IrDocElement` of kind/tag
/// Camera (§2.7, 48 bytes). Layout per docs/design/19-3d-space.md §19.4, docs/tasks/T-47a.md "IR
/// 補欄位".
struct IrCameraData {
    /// Camera kind: 0 = perspective, 1 = orthographic.
    uint8_t kind;
    /// 1 = the camera looks at `target`; 0 = the camera looks along its own -z axis.
    uint8_t useTarget;
    uint8_t _pad[2];  ///< Padding, unused.
    /// Field-of-view zoom factor (perspective) or ortho scale.
    float zoom;
    /// Camera position (root coordinates); NaN when omitted (19.4's "省略的屬性" -- filled in by
    /// refreshCameraDefaults() every layout).
    Vec3 position;
    /// Look-at target point, valid when `useTarget` is set.
    Vec3 target;
    /// Near/far clip planes.
    float nearZ;     ///< Near clip plane.
    float farZ;      ///< Far clip plane.
    uint32_t _pad2;  ///< Padding, unused.
    uint32_t _pad3;  ///< Padding, unused (reserved; keeps the struct at 48 bytes, T-47a).
};
static_assert(sizeof(IrCameraData) == 48);

/// T-243: IrPathData::skinIndex / IrMeshData::skinIndex of an unskinned path / mesh.
inline constexpr uint32_t kIrNoSkin = 0xFFFFFFFFu;
/// T-243: IrConstraintData::target of a constraint without a target (Spring).
inline constexpr uint32_t kIrNoTarget = 0xFFFFFFFFu;

/// `SCEN` chunk vector path data (fill + stroke + geometry) referenced by an `IrSceneNode` of
/// kind Path (§2.7, 156 bytes; 152 in IR minor 4, 136 before).
struct IrPathData {
    /// Offset/count into the chunk's `verbPool`.
    uint32_t verbOffset;  ///< Offset into the chunk's `verbPool`.
    uint32_t verbCount;   ///< Count of verbs.
    /// Offset/count into the chunk's `floatPool`.
    uint32_t pointOffset;  ///< Offset into the chunk's `floatPool`.
    uint32_t pointCount;   ///< Count of points.
    /// FillRule.
    uint8_t fillRule;
    /// PaintKind of the fill and stroke, respectively.
    uint8_t fillKind;    ///< PaintKind of the fill.
    uint8_t strokeKind;  ///< PaintKind of the stroke.
    /// LineCap, LineJoin.
    uint8_t cap;      ///< LineCap.
    uint8_t join;     ///< LineJoin.
    uint8_t _pad[3];  ///< Padding, unused.
    /// Solid paint color (also the gradient's base color) for fill and stroke.
    Color fillColor;    ///< Solid paint color (also the gradient's base color) for the fill.
    Color strokeColor;  ///< Solid paint color (also the gradient's base color) for the stroke.
    /// Fill/stroke gradient start and end (or center/edge) points.
    Vec2 fillP0;    ///< Fill gradient start (or center) point.
    Vec2 fillP1;    ///< Fill gradient end (or edge) point.
    Vec2 strokeP0;  ///< Stroke gradient start (or center) point.
    Vec2 strokeP1;  ///< Stroke gradient end (or edge) point.
    /// Offset/count into the chunk's `stopPool`, for the fill and stroke gradients respectively.
    uint32_t fillStopOffset;    ///< Offset into the chunk's `stopPool`, for the fill gradient.
    uint32_t fillStopCount;     ///< Count of fill gradient stops.
    uint32_t strokeStopOffset;  ///< Offset into the chunk's `stopPool`, for the stroke gradient.
    uint32_t strokeStopCount;   ///< Count of stroke gradient stops.
    /// Stroke width and miter limit (default 4).
    float strokeWidth;  ///< Stroke width.
    float miterLimit;   ///< Miter join limit (default 4).
    /// Trim path start/end (fraction of path length) and phase offset.
    float trimStart;   ///< Trim path start, as a fraction of path length.
    float trimEnd;     ///< Trim path end, as a fraction of path length.
    float trimOffset;  ///< Trim path phase offset.
    /// Offset/count into the chunk's `floatPool`, for the stroke dash pattern.
    uint32_t dashOffset;  ///< Offset into the chunk's `floatPool`, for the stroke dash pattern.
    uint32_t dashCount;   ///< Count of dash lengths.
    /// Dash pattern phase offset.
    float dashPhase;
    /// T-233 (IR minor 4): stroke taper at the start / end of each contour (fraction of its arc
    /// length). An IR minor < 4 record is 136 bytes and reads these four fields as 0.
    float taperStart;
    float taperEnd;  ///< See taperStart.
    /// T-233: offset/count into the chunk's `floatPool` of the width profile, as `profileCount`
    /// (t, w) float pairs (so 2 * profileCount floats), sorted by t.
    uint32_t profileOffset;
    uint32_t profileCount;  ///< Number of (t, w) pairs.
    /// T-243 (IR minor 5, 32 §32.2.2): index into the chunk's `skins[]`, kIrNoSkin when the path
    /// is not skinned. An IR minor < 5 record reads it as kIrNoSkin. Initialized so that every
    /// `IrPathData{}` a writer builds is unskinned.
    uint32_t skinIndex = kIrNoSkin;
};
static_assert(sizeof(IrPathData) == 156);
/// T-233: the size of an IrPathData record in IR minor < 4 files.
inline constexpr size_t kIrPathDataSizeV3 = 136;
/// T-243: the size of an IrPathData record in IR minor 4 files.
inline constexpr size_t kIrPathDataSizeV4 = 152;

/// T-225 (31 §31.6.1): `SCEN` chunk modifier data referenced by an `IrSceneNode` of kind Modifier
/// (44 bytes); the fields are ModifierNodeData's (xt/node.h documents the params layout).
struct IrModifierData {
    uint8_t kind;     ///< ModifierKind.
    uint8_t mode;     ///< Kind-specific mode.
    uint8_t enabled;  ///< 0 = skipped by the Painter.
    uint8_t _pad;     ///< Padding, unused.
    float params[8];  ///< Kind-specific parameters.
    float opacityA;   ///< Repeat: opacity of copy 0.
    float opacityB;   ///< Repeat: opacity of the last copy.
};
static_assert(sizeof(IrModifierData) == 44);

/// T-225 (31 §31.6.1): `SCEN` chunk parametric-shape data referenced by an `IrSceneNode` of kind
/// Shape (188 bytes; 172 before IR minor 4): kind + params (ShapeNodeData's layout), then
/// IrPathData's paint fields in IrPathData's order (everything after its verb/point ranges).
struct IrShapeData {
    uint8_t kind;               ///< ShapeKind.
    uint8_t _pad0[3];           ///< Padding, unused.
    float params[12];           ///< Kind-specific parameters.
    uint8_t fillRule;           ///< FillRule.
    uint8_t fillKind;           ///< PaintKind of the fill.
    uint8_t strokeKind;         ///< PaintKind of the stroke.
    uint8_t cap;                ///< LineCap.
    uint8_t join;               ///< LineJoin.
    uint8_t _pad[3];            ///< Padding, unused.
    Color fillColor;            ///< Solid fill color (also the gradient's base color).
    Color strokeColor;          ///< Solid stroke color (also the gradient's base color).
    Vec2 fillP0;                ///< Fill gradient start (or center) point.
    Vec2 fillP1;                ///< Fill gradient end (or edge) point.
    Vec2 strokeP0;              ///< Stroke gradient start (or center) point.
    Vec2 strokeP1;              ///< Stroke gradient end (or edge) point.
    uint32_t fillStopOffset;    ///< Offset into the chunk's `stopPool`, for the fill gradient.
    uint32_t fillStopCount;     ///< Count of fill gradient stops.
    uint32_t strokeStopOffset;  ///< Offset into the chunk's `stopPool`, for the stroke gradient.
    uint32_t strokeStopCount;   ///< Count of stroke gradient stops.
    float strokeWidth;          ///< Stroke width.
    float miterLimit;           ///< Miter join limit (default 4).
    float trimStart;            ///< Per-shape trim start.
    float trimEnd;              ///< Per-shape trim end.
    float trimOffset;           ///< Per-shape trim offset, in turns.
    uint32_t dashOffset;        ///< Offset into the chunk's `floatPool`, for the stroke dash pattern.
    uint32_t dashCount;         ///< Count of dash lengths.
    float dashPhase;            ///< Dash pattern phase offset.
    float taperStart;           ///< T-233 (IR minor 4): as IrPathData::taperStart.
    float taperEnd;             ///< T-233: as IrPathData::taperEnd.
    uint32_t profileOffset;     ///< T-233: as IrPathData::profileOffset.
    uint32_t profileCount;      ///< T-233: as IrPathData::profileCount.
};
static_assert(sizeof(IrShapeData) == 188);
/// T-233: the size of an IrShapeData record in IR minor < 4 files.
inline constexpr size_t kIrShapeDataSizeV3 = 172;

// T-243 (IR minor 5, docs/design/32-rigging-and-deformation.md §32.2): the rig records of the SCEN
// tail. Node and bone references are scene node indices (local NodeIds).

/// `SCEN` chunk bone data referenced by an `IrSceneNode` of kind Bone (4 bytes).
struct IrBoneData {
    float length;  ///< BoneNodeData::length.
};
static_assert(sizeof(IrBoneData) == 4);

/// `SCEN` chunk constraint data referenced by an `IrSceneNode` of kind Constraint (44 bytes); the
/// fields are ConstraintNodeData's (xt/node.h documents the params layout).
struct IrConstraintData {
    uint8_t kind;     ///< ConstraintKind.
    uint8_t space;    ///< 0 = local, 1 = world (group space).
    uint8_t enabled;  ///< 0 = not applied.
    uint8_t _pad;     ///< Padding, unused.
    uint32_t target;  ///< Scene node index of the target, kIrNoTarget for none.
    float params[8];  ///< Kind-specific parameters.
    float mix;        ///< ConstraintNodeData::mix.
};
static_assert(sizeof(IrConstraintData) == 44);

/// `SCEN` chunk image-mesh data referenced by an `IrSceneNode` of kind Mesh (32 bytes). The
/// positions are `meshVec2Pool[posOffset, posOffset + vertexCount)`, the uvs the `vertexCount`
/// entries right after them; the triangle list is `meshIndexPool[indexOffset, + indexCount)`.
struct IrMeshData {
    uint32_t asset;                  ///< ASST index of the image.
    uint32_t posOffset;              ///< Offset into `meshVec2Pool` (positions, then uvs).
    uint32_t vertexCount;            ///< Number of mesh vertices.
    uint32_t indexOffset;            ///< Offset into `meshIndexPool`.
    uint32_t indexCount;             ///< Number of indices (3 per triangle).
    float opacity;                   ///< MeshNodeData::opacity.
    uint8_t smooth;                  ///< MeshNodeData::smooth.
    uint8_t _pad[3];                 ///< Padding, unused.
    uint32_t skinIndex = kIrNoSkin;  ///< Index into `skins[]`, kIrNoSkin when not skinned.
};
static_assert(sizeof(IrMeshData) == 32);

/// `SCEN` chunk skin referenced by IrPathData::skinIndex / IrMeshData::skinIndex (20 bytes).
struct IrSkinData {
    uint32_t skeleton;      ///< Scene node index of the `<xt-skeleton>` group.
    uint32_t tendonOffset;  ///< Offset into `tendonPool`.
    uint32_t tendonCount;   ///< Number of tendons.
    uint32_t weightOffset;  ///< Offset into `weightPool`.
    uint32_t weightCount;   ///< Path: 3 per vertex; Mesh: 1 per vertex.
};
static_assert(sizeof(IrSkinData) == 20);

/// `SCEN` chunk tendon (28 bytes): a bone and its inverse bind matrix (a, b, c, d, e, f).
struct IrTendon {
    uint32_t bone;         ///< Scene node index of the bone.
    float inverseBind[6];  ///< Mat2x3 a, b, c, d, e, f.
};
static_assert(sizeof(IrTendon) == 28);

/// `SCEN` chunk vertex weights (8 bytes); as PointWeight (xt/node.h).
struct IrPointWeight {
    uint32_t idx4;  ///< Four 8-bit indices into the skin's tendons.
    uint32_t w4;    ///< Four 8-bit weights (/255).
};
static_assert(sizeof(IrPointWeight) == 8);

/// `SCEN` chunk raster image data referenced by an `IrSceneNode` of kind Image (§2.7, 16 bytes).
struct IrImageData {
    /// AssetId of the image.
    uint32_t asset;
    /// Intrinsic image size.
    float width;    ///< Intrinsic image width.
    float height;   ///< Intrinsic image height.
    uint32_t _pad;  ///< Padding, unused.
};
static_assert(sizeof(IrImageData) == 16);

/// A single gradient color stop in a `SCEN`/`TLIN` chunk's shared `stopPool` (§2.7, 20 bytes).
struct IrGradientStop {
    /// Position along the gradient, in [0, 1].
    float offset;
    /// Stop color.
    Color color;
};
static_assert(sizeof(IrGradientStop) == 20);

// -------------------------------------------------------------------------------------------
// §2.8 TLIN
// -------------------------------------------------------------------------------------------

/// `TLIN` chunk property track (§2.8, 16 bytes).
struct IrTrack {
    /// Local NodeId of the animated scene node, or kSelfNode.
    uint32_t target;
    /// PropId this track animates.
    uint16_t prop;
    /// ValueType of the track's keyframe values.
    uint8_t type;
    uint8_t _pad;  ///< Padding, unused.
    /// Offset/count into the chunk's `keyframes[]`.
    uint32_t kfOffset;  ///< Offset into the chunk's `keyframes[]`.
    uint32_t kfCount;   ///< Count of keyframes belonging to this track.
};
static_assert(sizeof(IrTrack) == 16);

/// `TLIN` chunk keyframe (§2.8, 72 bytes).
struct IrKeyframe {
    /// Timeline-local time, seconds.
    float time;
    /// Timing function into the next keyframe.
    IrEasing easing;
    Vec3 tanOut;  ///< Outgoing spatial tangent, valid when `hasSpatial` is set (z = 0 for a Vec2
                  ///< track).
    Vec3 tanIn;   ///< Incoming spatial tangent, valid when `hasSpatial` is set (z = 0 for a Vec2
                  ///< track).
    /// 1 = `tanOut`/`tanIn` hold spatial interpolation handles.
    uint8_t hasSpatial;
    uint8_t _pad[3];  ///< Padding, unused.
    /// Same encoding as IrDecl::value; Path/Stops/FloatArray store a u32 offset + u32 count into
    /// the chunk's floatPool/stopPool instead of inline data.
    uint8_t value[16];
    uint32_t _pad2;  ///< Padding, unused.
};
static_assert(sizeof(IrKeyframe) == 72);

/// `TLIN` chunk named marker (§2.8, 12 bytes).
struct IrMarker {
    /// StringId of the marker name.
    uint32_t name;
    /// Marker start time, seconds.
    float start;
    /// Marker duration, seconds.
    float duration;
};
static_assert(sizeof(IrMarker) == 12);

// -------------------------------------------------------------------------------------------
// §2.9 DOCT
// -------------------------------------------------------------------------------------------

/// `DOCT` chunk document element, indexed by local ElementIndex; `elements[]` is depth-first
/// preorder (§2.9, 48 bytes).
struct IrDocElement {
    /// Element index of the parent; the root's parent is 0xFFFFFFFF.
    uint32_t parent;
    /// StringId of the tag name.
    uint32_t tag;
    /// StringId of the `id` attribute, or kInvalidString.
    uint32_t id;
    /// Offset/count into `classPool[]`.
    uint32_t classOffset;  ///< Offset into `classPool[]`.
    uint32_t classCount;   ///< Count of classes.
    /// Offset/count into `attrPool[]`.
    uint32_t attrOffset;  ///< Offset into `attrPool[]`.
    uint32_t attrCount;   ///< Count of attributes.
    /// Offset/count into `inlineDecls[]` (this element's `style="..."` declarations).
    uint32_t inlineDeclOffset;  ///< Offset into `inlineDecls[]`.
    uint32_t inlineDeclCount;   ///< Count of inline declarations.
    /// SceneId (a SCEN chunk's index) when `tag == "anim"`, otherwise 0xFFFFFFFF.
    uint32_t scene;
    /// Index into `texts[]` of this element's text child, or 0xFFFFFFFF (an element has at most
    /// one text child in this version; see 05.2).
    uint32_t firstTextChild;
    /// Index into `DOCT`'s own `cameras[]` when this element selects a document-level camera,
    /// otherwise 0xFFFFFFFF.
    uint32_t cameraIndex;
};
static_assert(sizeof(IrDocElement) == 48);

/// `DOCT` chunk text node content (§2.9, 8 bytes).
struct IrDocText {
    /// Element index of the owning element.
    uint32_t parent;
    /// StringId of the text content.
    uint32_t text;
};
static_assert(sizeof(IrDocText) == 8);

/// `DOCT` chunk element attribute (§2.9, 8 bytes).
struct IrAttr {
    /// StringId of the attribute name.
    uint32_t name;
    /// StringId of the attribute value.
    uint32_t value;
};
static_assert(sizeof(IrAttr) == 8);

/// T-178 (docs/design/07-text.md §7.8): one rich-text run of a `DOCT` text node (32 bytes). The
/// `DOCT` chunk ends, after `cameras[]`, with `u32 runCount, IrTextRun runs[]`,
/// `u32 inlineCount, IrTextInline inlines[]`, `u32 linkCount, IrTextLink links[]`; a reader that
/// predates T-178 stops after `cameras[]` and ignores the tail. Runs of one text are contiguous,
/// in ascending `start` order, and never overlap; bytes outside every run use the element's style.
struct IrTextRun {
    /// Index into `texts[]`.
    uint32_t text;
    /// UTF-8 byte range `[start, end)` of the run within the text's string.
    uint32_t start;
    uint32_t end;  ///< One past the run's last byte.
    /// Offset/count into `inlineDecls[]`: the run's style override (only `color`, `font-weight`,
    /// `font-style`, `font-size`, `text-effect` and `letter-spacing` are kept by the compiler).
    uint32_t declOffset;
    uint32_t declCount;  ///< Count of override declarations.
    /// Index into the tail's `inlines[]` when the run is an inline object (its text is a single
    /// U+FFFC), otherwise 0xFFFFFFFF.
    uint32_t inlineObject;
    /// Index into the tail's `links[]` when the run is (part of) an `<a>`, otherwise 0xFFFFFFFF.
    uint32_t link;
    uint32_t _pad;  ///< Padding, unused (0).
};
static_assert(sizeof(IrTextRun) == 32);

/// T-178: an inline object of a rich-text run (16 bytes). `kind` 1 = `<glyph key>` (`ref` =
/// StringId of the action name), 2 = `<img inline src>` (`ref` = AssetId). `height` is the
/// object's height in px, 0 = the run's font size.
struct IrTextInline {
    uint32_t kind;  ///< 1 = glyph, 2 = image.
    uint32_t ref;   ///< StringId (glyph action) or AssetId (image).
    float height;   ///< px; 0 = the run's font size.
    uint32_t _pad;  ///< Padding, unused (0).
};
static_assert(sizeof(IrTextInline) == 16);

/// T-178: a rich-text link (`<a on:click="...">`, 4 bytes): the `on:click` attribute's source
/// text, kept verbatim for the trigger table (T-174) to take over.
struct IrTextLink {
    uint32_t onClick;  ///< StringId of the raw `on:click` value, or kInvalidString.
};
static_assert(sizeof(IrTextLink) == 4);

// -------------------------------------------------------------------------------------------
// §2.9a SRCM
// -------------------------------------------------------------------------------------------

/// `SRCM` chunk source-map entry: one per `DOCT` element then, after those, one per `DOCT` text
/// node (§2.9a, 12 bytes). See 17-editor.md §17.7.
struct IrSourceEntry {
    /// Index into `SRCM::fileNames[]`; 0 is always the main `.xt` file.
    uint32_t fileId;
    /// 1-based source line.
    uint32_t line;
    /// 1-based source column.
    uint32_t col;
};
static_assert(sizeof(IrSourceEntry) == 12);

// -------------------------------------------------------------------------------------------
// SYMB (T-171)
// -------------------------------------------------------------------------------------------

/// One internal path of a symbol instance in the `SYMB` chunk (8 bytes): the expanded path id
/// (`Play/Bg`) and the DOCT element carrying it.
struct IrSymbolPath {
    /// StringId of the full path id (equal to that element's `id`).
    uint32_t path;
    /// Index into DOCT `elements[]`.
    uint32_t element;
};
static_assert(sizeof(IrSymbolPath) == 8);

// -------------------------------------------------------------------------------------------
// §2.10 META
// -------------------------------------------------------------------------------------------

/// `META` chunk debug metadata: source file, compiler version, compile time (§2.10, 16 bytes).
/// T-405 (IR minor 8, 43 §43.3): the payload may continue with `u32 behavior` (the project's
/// `spec.behavior`); absent = 0, and the writer omits it when it is 0 so that a 16-byte META stays
/// readable by a runtime of IR minor <= 7 (whose reader requires exactly 16 bytes). Bytes after
/// `behavior` are ignored (a later minor's tail).
struct IrMeta {
    /// StringId of the source file name.
    uint32_t sourceFile;
    /// StringId of the compiler version string.
    uint32_t compilerVersion;
    /// Unix timestamp (seconds) the IR was compiled.
    uint64_t compileTimeUnix;
};
static_assert(sizeof(IrMeta) == 16);

// -------------------------------------------------------------------------------------------
// TGRP (docs/design/04-timeline.md §4.10.1, 12-compiler.md §12.7, docs/tasks/T-172.md). Payload:
//   uint32 groupCount;  IrTimelineGroup groups[]
//   uint32 markerCount; IrTimelineGroupMarker markers[]
//   uint32 soundCount;  IrTimelineGroupSound sounds[]
//   uint32 memberCount; IrTimelineGroupMember members[]
// Each group references its own contiguous range of the three pools.
// -------------------------------------------------------------------------------------------

/// One `@timeline` group (36 bytes).
struct IrTimelineGroup {
    /// StringId of the timeline name.
    uint32_t name;
    /// DOCT element index of the scope element (0 = the document root).
    uint32_t scopeElement;
    /// Group duration, seconds.
    float duration;
    /// Range into the chunk's markers[].
    uint32_t markerOffset, markerCount;
    /// Range into the chunk's sounds[].
    uint32_t soundOffset, soundCount;
    /// Range into the chunk's members[].
    uint32_t memberOffset, memberCount;
};
static_assert(sizeof(IrTimelineGroup) == 36);

/// A named group marker (8 bytes).
struct IrTimelineGroupMarker {
    /// Group time, seconds.
    float time;
    /// StringId of the marker name.
    uint32_t name;
};
static_assert(sizeof(IrTimelineGroupMarker) == 8);

/// A sound cue on the group's sound track (8 bytes).
struct IrTimelineGroupSound {
    /// Group time, seconds.
    float time;
    /// StringId of the cue name.
    uint32_t cue;
};
static_assert(sizeof(IrTimelineGroupSound) == 8);

/// One member track: the element's compiled `@keyframes <timeline>__<path>` timeline (normalized
/// to duration 1, like every `@keyframes` timeline) played at group time `delay` for `duration`
/// seconds (16 bytes).
struct IrTimelineGroupMember {
    /// DOCT element index of the animated element.
    uint32_t element;
    /// TLIN index of the member timeline.
    uint32_t timeline;
    /// Group time at which the member track starts, seconds.
    float delay;
    /// Length of the member track in seconds (0 for a single-instant track).
    float duration;
};
static_assert(sizeof(IrTimelineGroupMember) == 16);

// -------------------------------------------------------------------------------------------
// CALC / MDIA (T-176)
// -------------------------------------------------------------------------------------------

/// One `calc()` program: a range of the chunk's ops[] (8 bytes).
struct IrCalc {
    /// Offset/count into the chunk's ops[].
    uint32_t opOffset, opCount;
};
static_assert(sizeof(IrCalc) == 8);

/// One RPN instruction (12 bytes).
struct IrCalcOp {
    /// CalcOpKind.
    uint8_t op;
    /// PushLength: the LengthUnit; PushVar: the fallback's LengthUnit or kCalcUnitNumber.
    uint8_t unit;
    /// PushVar: 1 when `value`/`unit` hold a fallback.
    uint8_t hasFallback;
    uint8_t _pad;  ///< Padding, unused.
    /// PushLength/PushNumber: the operand; PushVar: the fallback's value.
    float value;
    /// PushVar: StringId of the token name (with `--`); kInvalidString otherwise.
    uint32_t name;
};
static_assert(sizeof(IrCalcOp) == 12);

/// One `@media` query list: the rule applies when ANY of its queries holds (8 bytes).
struct IrMedia {
    /// Offset/count into the chunk's queries[].
    uint32_t queryOffset, queryCount;
};
static_assert(sizeof(IrMedia) == 8);

/// One query: holds when ALL of its conditions hold (8 bytes).
struct IrMediaQuery {
    /// Offset/count into the chunk's conds[].
    uint32_t condOffset, condCount;
};
static_assert(sizeof(IrMediaQuery) == 8);

/// One `(feature ...)` condition (8 bytes).
struct IrMediaCond {
    /// MediaFeature.
    uint8_t feature;
    uint8_t _pad[3];  ///< Padding, unused.
    /// Per MediaFeature.
    float value;
};
static_assert(sizeof(IrMediaCond) == 8);

/// A STYL rule that only applies while its `@media` holds (8 bytes).
struct IrMediaRule {
    /// Index into STYL rules[].
    uint32_t rule;
    /// Index into the chunk's media[].
    uint32_t media;
};
static_assert(sizeof(IrMediaRule) == 8);

// -------------------------------------------------------------------------------------------
// STAT (docs/design/27-interaction.md §27.1, 03 §3.12.2, T-174). Payload:
//   uint32 nameCount;  uint32 names[]              (StringId; custom state bit i = names[i])
//   uint32 groupCount; IrStateGroup groups[]
// -------------------------------------------------------------------------------------------

/// One mutually exclusive state group, `state-groups="<name>: a|b|c"` (8 bytes). Groups are
/// document-wide; a state belongs to at most one group.
struct IrStateGroup {
    /// StringId of the group name.
    uint32_t name;
    /// Custom state bits of the members.
    uint32_t mask;
};
static_assert(sizeof(IrStateGroup) == 8);

// -------------------------------------------------------------------------------------------
// TRIG (docs/design/03-scene-and-properties.md §3.12.3, 27 §27.2, 12 §12.7, T-174). Payload:
//   uint32 tableCount;     IrTriggerTable tables[]      (one per element with triggers)
//   uint32 triggerCount;   IrTrigger triggers[]
//   uint32 actionCount;    IrTriggerAction actions[]
//   uint32 conditionCount; IrConverterStep conditions[]
// An element's NodeCommon::triggerTable is its table index + 1 (0 = none).
// -------------------------------------------------------------------------------------------

/// One element's trigger table (12 bytes).
struct IrTriggerTable {
    /// DOCT element index.
    uint32_t element;
    /// Range into `triggers[]`.
    uint32_t triggerOffset, triggerCount;
};
static_assert(sizeof(IrTriggerTable) == 12);

/// One event -> action list entry (20 bytes). The actions are `actions[actionOffset ..
/// actionOffset + actionCount)`; the first `headCount` of them start when the event fires (the
/// `;`-separated statements), the rest are reached only through `next`/`alt`.
struct IrTrigger {
    /// TriggerEvent.
    uint8_t event;
    /// T-390 (08.13, formerly padding, always 0): the pointer button filter of `press(<b>)`,
    /// `release(<b>)`, `click(<b>)`, `dblclick(<b>)` and `pointermove(<b>)`: PointerButton + 1,
    /// 0 = any button (pointermove: the button held, 0 = with or without one).
    uint8_t button;
    uint8_t _pad[2];  ///< Padding, unused.
    /// Event argument (see TriggerEvent): a StringId, a state bit index or a KeyCode.
    uint32_t arg0;
    /// Second event argument (the marker name of TimelineMarker), else kInvalidString.
    uint32_t arg1;
    /// Range into `actions[]`.
    uint32_t actionOffset;
    uint16_t actionCount;
    /// Number of statements (the first actions of the range).
    uint16_t headCount;
};
static_assert(sizeof(IrTrigger) == 20);

/// One action (20 bytes); the fields follow 3.12.3's `{ kind, arg0, arg1, arg2, then }`, plus
/// `flags` and `alt` for the play/state modes and `if ... else`.
struct IrTriggerAction {
    /// TriggerAction.
    uint8_t kind;
    /// kPlay* bits (Play) or TriggerMode (State/Class).
    uint8_t flags;
    /// `then`: 1-based index (within the trigger's action range) of the action to run after this
    /// one completes, 0 = none. For If: the true branch.
    uint16_t next;
    /// Arguments; see TriggerAction.
    uint32_t arg0;
    uint32_t arg1;  ///< See TriggerAction.
    float arg2;     ///< See TriggerAction.
    /// If only: 1-based index of the else branch, 0 = none.
    uint16_t alt;
    /// T-201 (formerly padding, always 0): Set = the number of converter steps at arg0.
    uint16_t extra;
};
static_assert(sizeof(IrTriggerAction) == 20);

/// One converter step (20 bytes, 3.12.3's `{ op: u8, args: f32/StringId x4 }`): an `if`
/// condition (TRIG) or a binding chain (BIND, T-175).
struct IrConverterStep {
    /// ConverterOp.
    uint8_t op;
    /// T-175: number of arguments of the op, including those of the Operands steps that follow
    /// (0 in T-174 IR, whose comparisons have a fixed arity).
    uint8_t argCount;
    /// T-175: ConverterArg of each of this step's four arguments, 4 bits each (argument i in bits
    /// 4i..4i+3); 0 = every argument is a Number.
    uint16_t argKinds;
    /// Operands; see ConverterArg for the String/Port encodings.
    float args[4];
};
static_assert(sizeof(IrConverterStep) == 20);

// -------------------------------------------------------------------------------------------
// STRS (T-179)
// -------------------------------------------------------------------------------------------

/// One `@font-fallback <lang> { font-family: ... }` chain (12 bytes).
struct IrFontFallback {
    /// StringId of the BCP 47 language tag.
    uint32_t lang;
    /// Offset/count into the chunk's fallbackFamilies[].
    uint32_t familyOffset, familyCount;
};
static_assert(sizeof(IrFontFallback) == 12);

/// The ConverterArg of `step`'s argument `i` (0..3).
inline ConverterArg converterArgKind(const IrConverterStep& step, uint32_t i) {
    return static_cast<ConverterArg>((step.argKinds >> (4u * i)) & 0xFu);
}

// -------------------------------------------------------------------------------------------
// PORT (docs/design/27-interaction.md §27.3.1, 03 §3.12.3, 12 §12.7, T-175). Payload:
//   uint32 portCount;  IrPort ports[]
//   uint32 fieldCount; IrPortField fields[]   (struct/list fields, enum values)
//   uint32 defaultCount; IrPortDefault defaults[]   (T-358, IR minor 7; optional tail)
//   uint32 nestedCount; IrPortNested nested[]        (T-391; optional tail after defaults[])
// A port is referenced by its index in ports[].
// -------------------------------------------------------------------------------------------

/// One declared port, `port:<name>="<type> [in|out|call]"` (24 bytes).
struct IrPort {
    /// StringId of the declared name (`hp`).
    uint32_t name;
    /// PortValueType.
    uint8_t type;
    /// PortDir.
    uint8_t dir;
    /// kPortExpose.
    uint8_t flags;
    /// T-381 (27 §27.3.4): a List port's entry type: 0 for `list<struct{...}>`, else the
    /// PortValueType of a `list<number|string|bool|color|vec2|vec3|enum(...)>` entry (an Enum
    /// entry's value names are the port's fields; the other entry types have none). 0 for every
    /// other port type (formerly padding, so older IR reads as `list<struct>`).
    uint8_t entryType;
    /// DOCT element index of the declaring element.
    uint32_t element;
    /// DOCT element index of the innermost symbol instance root the declaring element is inside
    /// (the port's scope), or kNoPortScope for a document-level port.
    uint32_t scope;
    /// Range into `fields[]`: struct fields (Struct, List) or the value names (Enum).
    uint32_t fieldOffset, fieldCount;
};
static_assert(sizeof(IrPort) == 24);

/// One struct field or enum value name (8 bytes).
struct IrPortField {
    /// StringId of the field / value name.
    uint32_t name;
    /// PortValueType of a struct field; 0 for an enum value.
    uint8_t type;
    uint8_t _pad[3];  ///< Padding, unused.
};
static_assert(sizeof(IrPortField) == 8);

/// T-391 (27 §27.3.4): the entry description of a struct field of type `list<T>` (16 bytes). Every
/// IrPortField whose type is List has exactly one of these; its own fields (a struct entry's
/// fields, or an Enum entry's value names) are `fields[fieldOffset .. fieldOffset + fieldCount)`,
/// which lie after the field itself (fieldOffset > field), so nesting is finite.
struct IrPortNested {
    /// Index into the PORT chunk's fields[] of the List field described.
    uint32_t field;
    /// 0 for `list<struct{...}>`, else the primitive entry's PortValueType (IrPort::entryType).
    uint8_t entryType;
    uint8_t _pad[3];  ///< Padding, unused.
    /// Range into `fields[]`.
    uint32_t fieldOffset, fieldCount;
};
static_assert(sizeof(IrPortNested) == 16);

/// T-358 (02 §2.s.2, IR minor 7): the default value of one port (24 bytes). Only ports with a
/// declared default are listed. `value` by `type`: Number f32, Bool u8, Color u32 RGBA8, String
/// u32 StringId, Vec2 2 x f32, Vec3 3 x f32, Enum u32 value-name StringId, T-383: List u32
/// StringId of the JSON array text (parsed at load as Document::setPortText()), T-385: Struct u32
/// StringId of the JSON object text (a `<use>` literal value; parsed likewise); other types never
/// have a default. `type` equals the port's IrPort::type.
struct IrPortDefault {
    /// Index into the PORT chunk's ports[].
    uint32_t port;
    /// PortValueType (same as the port's).
    uint8_t type;
    uint8_t _pad[3];  ///< Padding, unused.
    /// The literal, laid out by `type`; unused bytes are 0.
    uint8_t value[16];
};
static_assert(sizeof(IrPortDefault) == 24);

// -------------------------------------------------------------------------------------------
// BIND (27.3.2, 03 §3.12.3, 12 §12.7, T-175). Payload:
//   uint32 bindingCount; IrBinding bindings[]
//   uint32 sourceCount;  uint32 sources[]          (PORT indices)
//   uint32 stepCount;    IrConverterStep steps[]
// -------------------------------------------------------------------------------------------

/// T-243: IrBinding::sceneNode of a binding on the element itself.
inline constexpr uint32_t kBindElementSelf = 0xFFFFFFFFu;

/// One binding (28 bytes; 24 before IR minor 5): `bind:<target>="<expr>"` on `element` or (T-243)
/// on a scene node of the scene `element` owns.
struct IrBinding {
    /// DOCT element index.
    uint32_t element;
    /// BindTarget.
    uint8_t target;
    /// kBindAfterLayout / kBindNoPrimary.
    uint8_t flags;
    /// Prop: the PropId or StyleProp number; State: the custom state bit.
    uint16_t targetId;
    /// Prop: the CSS property name; Class: the class name; Variant: the class prefix
    /// `v-<Prop>-`; else kInvalidString.
    uint32_t targetName;
    /// Range into `sources[]`: the chain's input port first (unless kBindNoPrimary), then every
    /// other port the chain reads (distinct).
    uint32_t sourceOffset;
    uint16_t sourceCount;
    /// Range into `steps[]`.
    uint16_t stepCount;
    uint32_t stepOffset;
    /// T-243 (IR minor 5, 32 §32.4): kBindElementSelf for the element itself; otherwise the index
    /// of a node of the scene the element owns (an inline `<svg>`'s content). An IR minor < 5
    /// record reads it as kBindElementSelf.
    uint32_t sceneNode = kBindElementSelf;
};
static_assert(sizeof(IrBinding) == 28);
/// T-243: the size of an IrBinding record in IR minor < 5 files.
inline constexpr size_t kIrBindingSizeV4 = 24;

// -------------------------------------------------------------------------------------------
// SHDR (docs/design/18-effects.md 18.5/18.7, T-203). Optional, after DOCT; at most once. Payload:
//   uint32 shaderCount; IrShaderRef shaders[]
//   uint32 paramCount;  IrShaderParam params[]
//   uint32 textLength;  uint8 text[]      // UTF-8; every source/glsl range slices into it
// One entry per `.xtfx` asset a `custom()` effect uses; `IrEffect::shader` (ASST kind 2) names the
// asset, this table carries what the backends need: the HLSL body (`source`, for a host that
// compiles HLSL itself), the GLSL 330 translation (`glsl`, for xt_gl), the parameter table, the
// expansion and the SW fallback.
// -------------------------------------------------------------------------------------------

/// `SHDR` chunk type (T-203).
inline constexpr uint32_t kChunkSHDR = fourcc('S', 'H', 'D', 'R');

/// One declared `.xtfx` parameter (32 bytes; same layout as ShaderParamDesc, command_list.h).
struct IrShaderParam {
    /// Parameter name (StringId): the uniform name and the `effect.<fx>.<param>` name.
    uint32_t name;
    /// 0 Float, 1 Vec2, 2 Color.
    uint8_t type;
    uint8_t _pad[3];  ///< Padding, unused.
    /// Default: [0] float, [0..1] vec2, RGBA color.
    float defaultValue[4];
    /// `[min, max]` from the header; both 0 when it gave none.
    float minValue, maxValue;
};
static_assert(sizeof(IrShaderParam) == 32);

/// One custom effect asset (112 bytes).
struct IrShaderRef {
    /// ASST index of the `.xtfx` asset (kind 2).
    uint32_t asset;
    /// The HLSL body, as written (a slice of `text[]`).
    uint32_t sourceOffset, sourceLength;
    /// The GLSL 330 module translated from it (a slice of `text[]`).
    uint32_t glslOffset, glslLength;
    /// Range into `params[]`.
    uint32_t paramOffset, paramCount;
    /// `expand:` in local px.
    float expandPx;
    /// `fallback:` (18.7): a built-in effect the SW backend (and a GL that failed to compile the
    /// shader) applies instead; `kind == None` = apply nothing.
    IrEffect fallback;
};
static_assert(sizeof(IrShaderRef) == 112);

// -------------------------------------------------------------------------------------------
// CLIP (docs/design/05-style-and-css.md §5.9.8, T-332). Optional, after DOCT; at most once.
// Payload:
//   uint32 shapeCount; IrClipShape shapes[]
//   uint32 paramCount; IrClipParam params[]
// The `clip-path` shape pool: a `clip-path` declaration (StyleProp 207) is an Enum whose value is
// 0 for `none`, else 1 + the index into `shapes[]`. A reader that does not know the chunk skips it
// (§2.1: unknown chunk types are ignored) and ignores the Enum -- the element is then unclipped.
// -------------------------------------------------------------------------------------------

/// `CLIP` chunk type (T-332).
inline constexpr uint32_t kChunkCLIP = fourcc('C', 'L', 'I', 'P');

/// One `clip-path` shape (12 bytes).
struct IrClipShape {
    /// ClipShapeKind (xt/node.h): 1 inset, 2 circle, 3 ellipse, 4 polygon (never 0 in the pool).
    uint8_t kind;
    uint8_t _pad[3];  ///< Padding, unused.
    /// Range into `params[]`; the count is fixed per kind (ElementClipShape::params' layout:
    /// inset 12, circle 3, ellipse 4) or, for a polygon, 2 per vertex (>= 6, even).
    uint32_t paramOffset, paramCount;
};
static_assert(sizeof(IrClipShape) == 12);

/// One shape parameter (8 bytes): a Length in IrDecl's encoding (f32 value + u8 unit).
struct IrClipParam {
    /// The length's value.
    float value;
    /// LengthUnit: Px or Percent; Auto (value kClipRadiusClosestSide / kClipRadiusFarthestSide)
    /// only for a circle's r or an ellipse's rx / ry.
    uint8_t unit;
    uint8_t _pad[3];  ///< Padding, unused.
};
static_assert(sizeof(IrClipParam) == 8);

// -------------------------------------------------------------------------------------------
// GRID (docs/design/06-layout.md §6.15, 05 §5.9.8, T-338). Optional, after DOCT; at most once.
// Payload:
//   uint32 trackListCount; IrGridList trackLists[]   (ranges into gridTracks[])
//   uint32 trackCount;     IrGridTrack gridTracks[]
//   uint32 areaListCount;  IrGridList areaLists[]    (ranges into gridAreas[], + matrix size)
//   uint32 areaCount;      IrGridArea gridAreas[]
// `grid-template-columns` / `-rows` (StyleProps 264 / 265) are Enums: 0 = `none`, else 1 + a
// trackLists[] index; `grid-template-areas` (266) likewise into areaLists[]. A reader that does not
// know the chunk skips it (§2.1) and ignores the Enums -- the grid then has no template.
// -------------------------------------------------------------------------------------------

/// `GRID` chunk type (T-338).
inline constexpr uint32_t kChunkGRID = fourcc('G', 'R', 'I', 'D');

/// One track list or area list (12 bytes): a range into gridTracks[] / gridAreas[]. For an area
/// list `rows` x `cols` is the `grid-template-areas` matrix size (`.` cells included); a track
/// list leaves both 0.
struct IrGridList {
    uint32_t offset, count;
    uint16_t rows, cols;
};
static_assert(sizeof(IrGridList) == 12);

/// One track (8 bytes): GridTrackKind (xt/node.h) + its number (px, %, fr; 0 for auto).
struct IrGridTrack {
    float value;
    uint8_t kind;
    uint8_t _pad[3];  ///< Padding, unused.
};
static_assert(sizeof(IrGridTrack) == 8);

/// One named area (12 bytes): the name's StringId and 0-based lines, end exclusive.
struct IrGridArea {
    uint32_t name;
    uint16_t rowStart, rowEnd, colStart, colEnd;
};
static_assert(sizeof(IrGridArea) == 12);

// -------------------------------------------------------------------------------------------
// RPT (docs/design/27-interaction.md §27.3.4, 03 §3.12.4, 12 §12.7, T-177). Payload:
//   uint32 repeatCount; IrRepeat repeats[]
// The template subtree (templateRoot and its descendants, contiguous in DOCT preorder) stays in
// DOCT; the runtime keeps it out of the tree (NodeFlags2 kTemplate) and copies it per list entry.
// BIND / TRIG records of template elements are template-relative: their element indices name
// template elements.
// -------------------------------------------------------------------------------------------

/// IrRepeat::flags: `virtualize`.
inline constexpr uint8_t kRepeatVirtualize = 1u;
/// IrRepeat::flags (T-201): a spawn pool (03 §3.12.3): no list port (listPort =
/// kRepeatNoListPort); instances come from `spawn()` actions and leave when their lifetime ends.
inline constexpr uint8_t kRepeatSpawn = 2u;
/// IrRepeat::flags (T-391, 27 §27.3.4): a `<repeat port="item(<field>)">` inside another
/// repeat's template: `listPort` is then the index of that list field among the enclosing repeat's
/// entry fields; every instance of the enclosing repeat expands its own entry's list.
inline constexpr uint8_t kRepeatItemField = 4u;
/// IrRepeat::listPort of a spawn pool.
inline constexpr uint32_t kRepeatNoListPort = 0xFFFFFFFFu;
/// IrRepeat::viewportElement without a `viewport` attribute (the nearest `overflow: scroll`
/// ancestor, else the document viewport).
inline constexpr uint32_t kRepeatNoViewport = 0xFFFFFFFFu;

/// One `<repeat port id [item-enter] [item-exit] [virtualize] [viewport]>` (28 bytes).
struct IrRepeat {
    /// DOCT element index of the `<repeat>` element (the instances' parent).
    uint32_t element;
    /// PORT index of the `list<struct{...}>` port (T-391: with kRepeatItemField, the field index in
    /// the enclosing repeat's entry).
    uint32_t listPort;
    /// DOCT element index of the template root (the only child of `element`).
    uint32_t templateRoot;
    /// TRIG trigger index + 1 of the template root's `item-enter` trigger (0 = none).
    uint32_t enterTriggerActions;
    /// TRIG trigger index + 1 of the template root's `item-exit` trigger (0 = none).
    uint32_t exitTriggerActions;
    /// kRepeatVirtualize, kRepeatSpawn, kRepeatItemField.
    uint8_t flags;
    uint8_t _pad[3];  ///< Padding, unused.
    /// DOCT element index of the `viewport` element, or kRepeatNoViewport.
    uint32_t viewportElement;
};
static_assert(sizeof(IrRepeat) == 28);

#pragma pack(pop)

// -------------------------------------------------------------------------------------------
// Decl/Keyframe `value[16]` accessors. `index` is a 4-byte slot index (0..3), never a byte
// offset; callers combine these for multi-component types (Vec2 = index 0,1; Color = 0,1,2,3;
// Length = f32 at index 0 plus the unit byte at value[4], read directly). Uses memcpy, no type
// punning.
// -------------------------------------------------------------------------------------------

/// Reads the f32 at the given 4-byte slot of a Decl/Keyframe `value[16]`.
inline float irValueF32(const uint8_t* value, int index) {
    float v;
    std::memcpy(&v, value + index * 4, sizeof(v));
    return v;
}

/// Reads the u32 at the given 4-byte slot of a Decl/Keyframe `value[16]`.
inline uint32_t irValueU32(const uint8_t* value, int index) {
    uint32_t v;
    std::memcpy(&v, value + index * 4, sizeof(v));
    return v;
}

/// Writes an f32 to the given 4-byte slot of a Decl/Keyframe `value[16]`.
inline void irSetValueF32(uint8_t* value, int index, float v) {
    std::memcpy(value + index * 4, &v, sizeof(v));
}

/// Writes a u32 to the given 4-byte slot of a Decl/Keyframe `value[16]`.
inline void irSetValueU32(uint8_t* value, int index, uint32_t v) {
    std::memcpy(value + index * 4, &v, sizeof(v));
}

}  // namespace xt
