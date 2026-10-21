#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "xt/input.h"
#include "xt/node.h"
#include "xt/types.h"
#include "xt/value.h"

/// Event type/struct and the listener callback signature. See docs/design/08-events.md §8.3,
/// docs/tasks/T-32.md. The listener pool (Listener, addListener/removeListener) and dispatch/
/// pointer-state-machine logic live in event_dispatcher.h/.cpp (core/src, not a public header).

namespace xt {

/// See docs/design/08-events.md §8.3.
enum class EventType : uint8_t {
    PointerDown,         ///< A pointer button was pressed on the hit element.
    PointerUp,           ///< A pointer button was released, dispatched to the active element.
    Click,               ///< PointerUp landed back on the active element (or a descendant); bubbles.
    PointerMove,         ///< The pointer moved; dispatched to the active element while dragging, else the hit
                         ///< element.
    PointerEnter,        ///< The pointer entered this element or an ancestor; does not bubble.
    PointerLeave,        ///< The pointer left this element or an ancestor; does not bubble.
    Wheel,               ///< A wheel/scroll input was received on the hit element.
    KeyDown,             ///< A key was pressed; dispatched to the focused element, or the root if none.
    KeyUp,               ///< A key was released; dispatched to the focused element, or the root if none.
    TextInput,           ///< Committed text input; dispatched to the focused element, or the root if none.
    Focus,               ///< This element became the focus target; does not bubble.
    Blur,                ///< This element lost focus; does not bubble.
    Change,              ///< A checkbox/input's value changed (e.g. toggled, or committed text input).
    CompositionStart,    ///< An IME composition session started.
    CompositionUpdate,   ///< The in-progress IME composition string changed.
    CompositionEnd,      ///< The IME composition session ended.
    AnimationStart,      ///< A CSS `animation` instance started playing on this element.
    AnimationEnd,        ///< A CSS `animation` instance finished (or an API-created one via the registered
                         ///< callback).
    AnimationIteration,  ///< A CSS `animation` instance completed one iteration and is repeating.
    TransitionEnd,       ///< A CSS transition on this element finished.
    Marker,              ///< A timeline marker was reached during an animation.
    TimelineEnd,         ///< A `@timeline` group played to its end (08.6/4.10.1, T-172); sent to its scope.
    TimelineMarker,      ///< A `@timeline` group crossed a named marker (08.6/4.10.1, T-172).
    MediaEnd,            ///< A `<video>`/`<sequence>` played to its end (13.8.3, T-191); sent to it.
    // --- T-194 (08.9, 27 §27.7): appended types. ---
    Back,        ///< Gamepad B / Circle: sent to the player's focus (else the surface), bubbles.
    NavBlocked,  ///< Focus navigation found no element in Event::direction; sent to the focus.
    DragStart,   ///< A drag began on Event::dragSource (the target); bubbles.
    DragOver,    ///< The drag entered the drop target Event::dropTarget (the target); bubbles.
    DragLeave,   ///< The drag left the drop target Event::dropTarget (the target); bubbles.
    Drop,        ///< The drag was released on the drop target Event::dropTarget; bubbles.
    Gesture,     ///< A touch gesture (Event::gesture) on the hit element; bubbles.
    // --- T-379 (08.10): appended type. ---
    /// The second Click of a double click (same target, presses <= 400 ms and <= 4 px apart, or
    /// InputEvent::clicks >= 2 from the host); dispatched right after that Click, bubbles.
    DoubleClick,
    // --- T-386 (08.11): appended type. ---
    /// The pointer press was taken away (window focus lost, the pointer left the window with a
    /// button held, or Context::cancelPointer()): sent to the active element, bubbles; the active
    /// state is cleared and no PointerUp / Click follows.
    PointerCancel,
    // --- T-388 (08.12): appended types. ---
    /// The drag was cancelled (Escape, a second pointer button, PointerCancel, gamepad B or
    /// Context::cancelDrag()): sent to Event::dragSource after the drag state is cleared, bubbles;
    /// no Drop follows. DragEnd comes right after it.
    DragCancel,
    /// The drag ended (dropped, released off every target, or cancelled): sent to
    /// Event::dragSource last, bubbles; Event::dropped tells whether a Drop was dispatched.
    DragEnd,
    // --- T-387 (08.12): appended types. ---
    /// A scroll container's ScrollX / ScrollY changed (wheel, or the ElementRef scroll API);
    /// target = the container, bubbles; Event::scrollPos = the new (scrollX, scrollY).
    Scroll,
    /// A PointerDown landed outside this element's subtree (light-dismiss): sent, without
    /// bubbling, to every visible element that has an OutsidePress listener or `on:outsidepress`
    /// trigger; target = that element, Event::pos = the press position.
    OutsidePress,
    // --- T-394 (08.15): appended type. ---
    /// A primary-button press on an element with an `on:pointerdrag` trigger or a PointerDrag
    /// listener moved at least its `drag-threshold` (default 4 px): sent, without bubbling, to that
    /// element on every pointer move until the release / PointerCancel (the `on:pointerdrag`
    /// trigger runs once per frame with the last one). No Drag* events, no ghost.
    PointerDrag,
    // --- T-397 (08.17): appended types. ---
    /// Sent right after Focus to the element that took the focus, but bubbling (web `focusin`).
    FocusIn,
    /// Sent right after Blur to the element that lost the focus, but bubbling (web `focusout`).
    FocusOut,
};

/// See docs/design/08-events.md §8.3. `target` is the originally hit element; `current` is the
/// element the dispatcher is currently invoking listeners on while bubbling from target to root.
struct Event {
    /// Which kind of event this is; determines which other fields are meaningful.
    EventType type = EventType::PointerMove;
    /// The originally hit element.
    NodeId target = kInvalidNode;
    /// The element the dispatcher is currently invoking listeners on while bubbling.
    NodeId current = kInvalidNode;
    Vec2 pos{};       ///< Viewport coordinates.
    Vec2 localPos{};  ///< `current`'s local (border-box) coordinates.
    /// Which pointer button triggered a Pointer*/Click event.
    PointerButton button = PointerButton::Left;
    /// Wheel scroll delta for a Wheel event, in notches (08.11): 1.0 = one notch (one line),
    /// fractions from touchpads / high-resolution wheels; positive y scrolls down.
    Vec2 wheelDelta{};
    /// Key involved in a KeyDown/KeyUp event.
    KeyCode key = KeyCode::Unknown;
    /// Bitmask of modifier keys (Modifiers) held during a pointer/wheel/key event (T-387: every
    /// event built from a pointer or wheel InputEvent carries InputEvent::modifiers).
    uint8_t modifiers = 0;
    /// TextInput: the committed text; CompositionUpdate: the composition string. A view, valid for
    /// the listener call only -- a CompositionUpdate's points into Context's composition pool,
    /// which the Context::update() that dispatched it empties when it is done (T-270a), so a
    /// listener that keeps the text must copy it (T-270b).
    std::string_view text;
    /// Animator instance id for an Animation*/TransitionEnd event.
    uint32_t animationId = 0;
    /// Marker name (interned) for a Marker event.
    StringId markerName = kInvalidString;
    /// Set by a listener to stop the event from bubbling further.
    bool stopPropagation = false;
    /// Set by a listener to suppress the event's default action.
    bool preventDefault = false;
    /// Timeline name (interned) for a TimelineEnd/TimelineMarker event.
    StringId timelineName = kInvalidString;
    // --- T-194 (08.9): tail additions. ---
    /// The local player whose input caused the event (0 for mouse / keyboard).
    uint8_t player = 0;
    /// NavBlocked: the requested direction; Gesture (Swipe): the stroke's direction.
    NavDirection direction = NavDirection::Up;
    /// Gesture: which gesture.
    GestureKind gesture = GestureKind::Tap;
    /// Gesture (Pinch): current finger distance / starting distance.
    float gestureScale = 1;
    /// Drag*/Drop: the dragged (`draggable`) element.
    NodeId dragSource = kInvalidNode;
    /// DragOver/DragLeave/Drop: the `drop-target` element.
    NodeId dropTarget = kInvalidNode;
    /// Drag*/Drop: the `<repeat>` entry index of dragSource (27.3.4), 0xFFFFFFFF when none.
    uint32_t sourceItem = 0xFFFFFFFFu;
    /// DragOver/DragLeave/Drop: the `<repeat>` entry index of dropTarget, 0xFFFFFFFF when none.
    uint32_t targetItem = 0xFFFFFFFFu;
    // --- T-383 (08.10): tail addition. ---
    /// Click: the press's number in its run of clicks (1 = first, 2 = second; the host's
    /// InputEvent::clicks when it gives one); keyboard / gamepad activation: 1. DoubleClick: 2
    /// (or the host's count). 0 for every other event.
    uint8_t clicks = 0;
    // --- T-388 (08.12): tail additions. ---
    /// DragOver/Drop: the pointer's position within dropTarget's border box, each axis 0..1
    /// (Document::fractionIn); (0, 0) for other events.
    Vec2 dropFraction{};
    /// DragEnd: true when the drag ended with a Drop.
    bool dropped = false;
    // --- T-387 (08.12): tail addition. ---
    /// Scroll: the container's new (scrollX, scrollY), px.
    Vec2 scrollPos{};
    // --- T-393 (08.14): tail addition. ---
    /// Drop of an external (OS) drag: the dropped files' paths; empty for every other event. Valid
    /// for the duration of the dispatch only (a listener keeping them copies them). An external
    /// drag's DragOver / DragLeave / Drop have dragSource == kInvalidNode.
    std::span<const std::string> externalPaths;
    // --- T-394 (08.15): tail additions. ---
    /// DragOver/Drop: the insertion index in the target's `<repeat>` list (target = a `<repeat>`
    /// instance, or the `<repeat>` element with the pointer over an entry): `targetItem +
    /// (dropFraction.y >= 0.5)`. Over the drag source's own row (a drop target itself) the source
    /// row is the target, `targetItem` = the source index. 0xFFFFFFFF for any other target or event.
    uint32_t dropIndex = 0xFFFFFFFFu;
    /// OutsidePress: set by a listener (or an `on:outsidepress` trigger with `consume()`) to take
    /// the press: the PointerDown is not dispatched to the hit element, and its PointerUp / Click
    /// are suppressed. Ignored for every other event.
    bool consume = false;
    // --- T-399 (08 §8.18): tail addition. ---
    /// DragLeave: true when the drag left the window or ended there (an external drag's
    /// ExternalDragLeave, a cancelled drag); false when the pointer moved
    /// on to another target (or off every target inside the window). False for every other event.
    bool leftWindow = false;
};

/// Signature of a callback registered to receive dispatched Events.
using EventListener = std::function<void(Event&)>;

/// Sentinel returned by Document::addListener() for an invalid target (docs/tasks/T-32.md;
/// addListener's own id space -- returned by EventDispatcher::addListener -- never produces this
/// value, since it is a plain, ever-growing vector index starting at 0).
inline constexpr uint32_t kInvalidListener = 0xFFFFFFFFu;

/// Kind of a HostEvent (T-174; docs/design/13-api.md §13.8.3, 27-interaction.md §27.2.3/27.6).
enum class HostEventKind : uint8_t {
    Sound = 0,     ///< `sound(<cue>[, volume])`.
    Haptic = 1,    ///< `haptic(<cue>)`.
    Emit = 2,      ///< `emit(<port>)`; T-175: HostEvent::value / port carry the port's value.
    Announce = 3,  ///< T-194 (27.7): the focus moved to an element with `aria-label`; `text` = it.
    Drop = 4,      ///< T-194 (27.7): a drop; `elementId` = source, `targetId` = drop target.
};

/// An out event a trigger action sends to the host (13.8.3). The string views stay valid only for
/// the duration of the callback.
struct HostEvent {
    /// Sound, Haptic or Emit.
    HostEventKind kind = HostEventKind::Sound;
    /// The cue (Sound/Haptic) or port (Emit) name.
    std::string_view name;
    /// Sound only: the volume argument (default 1).
    float volume = 1;
    /// The element whose trigger sent the event.
    NodeId element = kInvalidNode;
    /// That element's id ("" when it has none).
    std::string_view elementId;
    /// The sending Document's handle within its Context (0 when the Document is used directly).
    uint32_t document = 0;
    // --- T-175 (docs/design/13-api.md §13.8.1, 27 §27.3.1): tail additions. ---
    /// Emit only: the emitted port's current value (a Float 0 for an undeclared or `event` port).
    Value value{};
    /// Emit only: the emitted port's PortId (Document::lookupPort), 0 for an undeclared port.
    uint32_t port = 0;
    /// The sending Document's instance id (Document::setInstanceId, 27.3.1 multi-instance).
    uint32_t instanceId = 0;
    /// T-177 (27.3.4): sent from inside a `<repeat>` instance: its list entry index (the entry is
    /// that index of the repeat's list port); 0xFFFFFFFF otherwise.
    uint32_t itemIndex = 0xFFFFFFFFu;
    // --- T-194 (27.7): tail additions. ---
    /// Haptic only: the `haptic(<cue>, <strength>)` strength (default 1).
    float strength = 1;
    /// Announce only: the text to read aloud (the focused element's `aria-label`).
    std::string_view text;
    /// Drop only: the drop target element, its id and its `<repeat>` entry index (0xFFFFFFFF when
    /// none); `element` / `elementId` / `itemIndex` describe the dragged element.
    NodeId target = kInvalidNode;
    std::string_view targetId;               ///< See `target`.
    uint32_t targetItemIndex = 0xFFFFFFFFu;  ///< See `target`.
    /// The local player whose input caused the event (Announce, Drop, and trigger actions run
    /// for a player's input).
    uint8_t player = 0;
    // --- T-388 (08.12): tail addition. ---
    /// Drop only: Event::dropFraction (the release point within the drop target, 0..1 per axis).
    Vec2 dropFraction{};
    // --- T-393 (08.14): tail addition. ---
    /// Drop of an external (OS) drag only: the dropped files' paths (Event::externalPaths; then
    /// `element` is kInvalidNode). Valid for the duration of the callback.
    std::span<const std::string> paths;
};

/// Host callback for HostEvents (Document::setHostEventListener, Context's ContextListener).
using HostEventListener = std::function<void(const HostEvent&)>;

/// What the core asks the host to do with a `<video>` (docs/design/13-api.md §13.8.3, T-191).
enum class ExternalRequestKind : uint8_t {
    Play = 0,   ///< Start or resume playback from ExternalRequest::value seconds.
    Pause = 1,  ///< Hold (ExternalRequest::value = the current time).
    Seek = 2,   ///< Jump to ExternalRequest::value seconds.
    Loop = 3,   ///< ExternalRequest::value 1 = loop, 0 = play once.
    Rate = 4,   ///< ExternalRequest::value = the playback rate (> 0).
};

/// A playback request for a host-decoded `<video>` (ContextListener::onExternalRequest). The core
/// never decodes video: it asks, and the host answers through IExternalTexture::setPlayback. The
/// string views stay valid only for the duration of the callback.
struct ExternalRequest {
    /// What to do.
    ExternalRequestKind kind = ExternalRequestKind::Play;
    /// The `<video>` element.
    NodeId element = kInvalidNode;
    /// That element's id ("" when it has none).
    std::string_view elementId;
    /// The `src` attribute as written in the `.xt` file.
    std::string_view src;
    /// Play/Seek: seconds; Pause: the current time; Loop: 1/0; Rate: the rate.
    float value = 0;
    /// The sending Document's handle within its Context (0 when the Document is used directly).
    uint32_t document = 0;
    /// The sending Document's instance id (Document::setInstanceId).
    uint32_t instanceId = 0;
};

/// Host callback for ExternalRequests (Document::setExternalRequestListener).
using ExternalRequestListener = std::function<void(const ExternalRequest&)>;

/// The host side of an External node (docs/design/13-api.md §13.8.3, T-191); Document implements
/// it. `node` is the `<video>` / `<sequence>` / `<external>` element or its NodeKind::External
/// child. The binding goes through the element's `engine://` ASST entry (T-150), so a texture
/// handle is what the GPU backend samples (AssetResolver::ExternalTexture::texture: an
/// `xt::gpu::TextureHandle`, a GL texture name for xt_gl).
class IExternalTexture {
public:
    virtual ~IExternalTexture() = default;
    /// Binds `textureHandle` (`width` x `height` pixels) as the element's content; handle 0 only
    /// sets the intrinsic size. Every call marks the element's area damaged (a host that updated
    /// the pixels of the same texture calls it again). kInvalidArgument for a node that is not a
    /// video / external element (a `<sequence>` is played by the core) or a negative size;
    /// kUnsupported when the Document has no AssetResolver.
    virtual Status bindTexture(NodeId node, uint32_t textureHandle, int width, int height) = 0;
    /// Reports the host player's state and time (seconds). A video entering `Ended` fires its
    /// `media(<id>).end` triggers and EventType::MediaEnd, and resumes `play(<id>) then ...`
    /// chains. kInvalidArgument for a node that is not a video.
    virtual Status setPlayback(NodeId node, MediaState state, float time) = 0;
};

}  // namespace xt
