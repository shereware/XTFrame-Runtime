#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "xt/types.h"

/// Host input event structure. See docs/design/08-events.md §8.1 (InputEvent, PointerButton,
/// KeyCode, Modifiers), §8.8 (Composition events), docs/tasks/T-20.md "input.h". `Context` only
/// queues these (pushInput); dispatch into Document/Event happens in T-32.

namespace xt {

/// Identifies a mouse/pointer button in a PointerDown/PointerUp event.
enum class PointerButton : uint8_t {
    Left = 0,   ///< Primary/left button.
    Right = 1,  ///< Secondary/right button.
    Middle = 2  ///< Middle/wheel button.
};

/// Unknown..Up are USB HID usage table values (see docs/tasks/T-20.md "input.h"); the keys after Up
/// (docs/tasks/T-143.md) are appended with sequential values and do not follow HID numbering.
enum class KeyCode : uint16_t {
    Unknown = 0,     ///< No mapped key.
    KeyA = 4,        ///< 'A' key.
    KeyB = 5,        ///< 'B' key.
    KeyC = 6,        ///< 'C' key.
    KeyD = 7,        ///< 'D' key.
    KeyE = 8,        ///< 'E' key.
    KeyF = 9,        ///< 'F' key.
    KeyG = 10,       ///< 'G' key.
    KeyH = 11,       ///< 'H' key.
    KeyI = 12,       ///< 'I' key.
    KeyJ = 13,       ///< 'J' key.
    KeyK = 14,       ///< 'K' key.
    KeyL = 15,       ///< 'L' key.
    KeyM = 16,       ///< 'M' key.
    KeyN = 17,       ///< 'N' key.
    KeyO = 18,       ///< 'O' key.
    KeyP = 19,       ///< 'P' key.
    KeyQ = 20,       ///< 'Q' key.
    KeyR = 21,       ///< 'R' key.
    KeyS = 22,       ///< 'S' key.
    KeyT = 23,       ///< 'T' key.
    KeyU = 24,       ///< 'U' key.
    KeyV = 25,       ///< 'V' key.
    KeyW = 26,       ///< 'W' key.
    KeyX = 27,       ///< 'X' key.
    KeyY = 28,       ///< 'Y' key.
    KeyZ = 29,       ///< 'Z' key.
    Key1 = 30,       ///< '1' key.
    Key2 = 31,       ///< '2' key.
    Key3 = 32,       ///< '3' key.
    Key4 = 33,       ///< '4' key.
    Key5 = 34,       ///< '5' key.
    Key6 = 35,       ///< '6' key.
    Key7 = 36,       ///< '7' key.
    Key8 = 37,       ///< '8' key.
    Key9 = 38,       ///< '9' key.
    Key0 = 39,       ///< '0' key.
    Enter = 40,      ///< Enter/Return key.
    Escape = 41,     ///< Escape key.
    Backspace = 42,  ///< Backspace key.
    Tab = 43,        ///< Tab key.
    Space = 44,      ///< Space bar.
    Delete = 76,     ///< Forward Delete key.
    Home = 74,       ///< Home key.
    End = 77,        ///< End key.
    Right = 79,      ///< Right arrow key.
    Left = 80,       ///< Left arrow key.
    Down = 81,       ///< Down arrow key.
    Up = 82,         ///< Up arrow key.
    // T-143: appended keys (W3C KeyboardEvent.code names). These values are sequential after Up
    // and are NOT HID usage ids; hosts map them explicitly (see shell/src/sdl_input.cpp).
    F1 = 83,               ///< F1 function key.
    F2 = 84,               ///< F2 function key.
    F3 = 85,               ///< F3 function key.
    F4 = 86,               ///< F4 function key.
    F5 = 87,               ///< F5 function key.
    F6 = 88,               ///< F6 function key.
    F7 = 89,               ///< F7 function key.
    F8 = 90,               ///< F8 function key.
    F9 = 91,               ///< F9 function key.
    F10 = 92,              ///< F10 function key.
    F11 = 93,              ///< F11 function key.
    F12 = 94,              ///< F12 function key.
    PageUp = 95,           ///< Page Up key.
    PageDown = 96,         ///< Page Down key.
    Insert = 97,           ///< Insert key.
    ShiftLeft = 98,        ///< Left Shift key.
    ShiftRight = 99,       ///< Right Shift key.
    ControlLeft = 100,     ///< Left Control key.
    ControlRight = 101,    ///< Right Control key.
    AltLeft = 102,         ///< Left Alt/Option key.
    AltRight = 103,        ///< Right Alt/Option (AltGr) key.
    MetaLeft = 104,        ///< Left Meta/Windows/Command key.
    MetaRight = 105,       ///< Right Meta/Windows/Command key.
    CapsLock = 106,        ///< Caps Lock key.
    Minus = 107,           ///< '-' key.
    Equal = 108,           ///< '=' key.
    BracketLeft = 109,     ///< '[' key.
    BracketRight = 110,    ///< ']' key.
    Backslash = 111,       ///< '\' key.
    Semicolon = 112,       ///< ';' key.
    Quote = 113,           ///< Apostrophe/quote key.
    Backquote = 114,       ///< '`' key.
    Comma = 115,           ///< ',' key.
    Period = 116,          ///< '.' key.
    Slash = 117,           ///< '/' key.
    Numpad0 = 118,         ///< Numpad '0' key.
    Numpad1 = 119,         ///< Numpad '1' key.
    Numpad2 = 120,         ///< Numpad '2' key.
    Numpad3 = 121,         ///< Numpad '3' key.
    Numpad4 = 122,         ///< Numpad '4' key.
    Numpad5 = 123,         ///< Numpad '5' key.
    Numpad6 = 124,         ///< Numpad '6' key.
    Numpad7 = 125,         ///< Numpad '7' key.
    Numpad8 = 126,         ///< Numpad '8' key.
    Numpad9 = 127,         ///< Numpad '9' key.
    NumpadAdd = 128,       ///< Numpad '+' key.
    NumpadSubtract = 129,  ///< Numpad '-' key.
    NumpadMultiply = 130,  ///< Numpad '*' key.
    NumpadDivide = 131,    ///< Numpad '/' key.
    NumpadDecimal = 132,   ///< Numpad '.' key.
    NumpadEnter = 133,     ///< Numpad Enter key.
};

/// Bitmask flags for InputEvent::modifiers / Event::modifiers.
enum Modifiers : uint8_t {
    kShift = 1,  ///< Shift key held.
    kCtrl = 2,   ///< Control key held.
    kAlt = 4,    ///< Alt/Option key held.
    kSuper = 8   ///< Super/Windows/Command key held.
};

/// T-194 (08.9): a standard gamepad button (SDL GameController layout; A = bottom face button,
/// Cross on PlayStation).
enum class GamepadButton : uint8_t {
    A = 0,               ///< Bottom face button (Cross): press / click.
    B = 1,               ///< Right face button (Circle): `back`.
    X = 2,               ///< Left face button (Square).
    Y = 3,               ///< Top face button (Triangle).
    Back = 4,            ///< View / Select / Share.
    Guide = 5,           ///< Home / PS button.
    Start = 6,           ///< Menu / Options.
    LeftStick = 7,       ///< Left stick click.
    RightStick = 8,      ///< Right stick click.
    LeftShoulder = 9,    ///< LB / L1: focus the previous element (tab-prev).
    RightShoulder = 10,  ///< RB / R1: focus the next element (tab-next).
    DpadUp = 11,         ///< D-pad up: focus navigation.
    DpadDown = 12,       ///< D-pad down: focus navigation.
    DpadLeft = 13,       ///< D-pad left: focus navigation.
    DpadRight = 14,      ///< D-pad right: focus navigation.
};

/// T-194 (08.9): a gamepad axis. Sticks report -1..1 (positive = right / down), triggers 0..1.
enum class GamepadAxis : uint8_t {
    LeftX = 0,         ///< Left stick horizontal: focus navigation.
    LeftY = 1,         ///< Left stick vertical: focus navigation.
    RightX = 2,        ///< Right stick horizontal.
    RightY = 3,        ///< Right stick vertical.
    TriggerLeft = 4,   ///< Left trigger.
    TriggerRight = 5,  ///< Right trigger.
};

/// T-194 (08.9): a screen direction (focus navigation, swipe).
enum class NavDirection : uint8_t {
    Up = 0,     ///< Towards the top of the viewport.
    Down = 1,   ///< Towards the bottom of the viewport.
    Left = 2,   ///< Towards the left of the viewport.
    Right = 3,  ///< Towards the right of the viewport.
};

/// T-194 (27.2.2 `gesture(...)`): a recognized touch gesture.
enum class GestureKind : uint8_t {
    Tap = 0,        ///< One short touch.
    DoubleTap = 1,  ///< Two taps in quick succession at nearly the same place.
    Swipe = 2,      ///< A fast one-finger stroke; InputEvent::direction holds its direction.
    Pinch = 3,      ///< Two fingers moving apart or together; InputEvent::gestureScale holds the ratio.
    Hold = 4,       ///< One finger held still (long press).
};

/// Host-facing cursor shape (docs/design/08-events.md §8.7); Context::cursor() is a placeholder in
/// this task and always returns Cursor::Default (T-32 computes it from the hover node's style).
enum class Cursor : uint8_t {
    Default = 0,  ///< Platform default arrow cursor.
    Pointer,      ///< Hand/link cursor.
    Text,         ///< I-beam text-caret cursor.
    Move          ///< Move/drag cursor.
};

/// One raw input event as pushed by the host via Context::pushInput().
struct InputEvent {
    /// Discriminates which fields below are meaningful.
    enum Kind : uint8_t {
        PointerMove,        ///< Pointer moved; `pos` valid.
        PointerDown,        ///< A pointer button was pressed; `pos`/`button` valid.
        PointerUp,          ///< A pointer button was released; `pos`/`button` valid.
        Wheel,              ///< Wheel/scroll input; `pos`/`wheelDelta` valid.
        KeyDown,            ///< A key was pressed; `key`/`modifiers`/`repeat` valid.
        KeyUp,              ///< A key was released; `key`/`modifiers` valid.
        TextInput,          ///< Committed text input; `utf8`/`utf8Len` valid.
        PointerLeave,       ///< The pointer left the viewport.
        CompositionStart,   ///< An IME composition session began.
        CompositionUpdate,  ///< IME composition text changed; `composition`/`compositionCursor`/
                            ///< `compositionSelLen` valid.
        CompositionEnd,     ///< The IME composition session ended (committed or cancelled).
        // --- T-194 (docs/design/08-events.md §8.9, 27 §27.7): appended kinds. ---
        GamepadButtonDown,  ///< A gamepad button was pressed; `gamepadButton`/`player` valid.
        GamepadButtonUp,    ///< A gamepad button was released; `gamepadButton`/`player` valid.
        GamepadAxisMotion,  ///< A gamepad axis moved; `gamepadAxis`/`axisValue`/`player` valid.
        Gesture,            ///< A recognized touch gesture (GestureRecognizer); `pos`/`gesture`/
                            ///< `direction`/`gestureScale`/`player` valid.
        // --- T-386 (docs/design/08-events.md §8.11): appended kind. ---
        PointerCancel,  ///< The host took the pointer press away (focus lost, capture lost): the
                        ///< active element gets EventType::PointerCancel, no PointerUp / Click.
        // --- T-393 (docs/design/08-events.md §8.14): appended kinds (external file drop). ---
        ExternalDragEnter,  ///< An OS drag (files from outside) entered the viewport; `pos` valid.
        ExternalDragMove,   ///< The OS drag moved over the viewport; `pos` valid.
        ExternalDragLeave,  ///< The OS drag left the viewport or was cancelled.
        ExternalDrop        ///< The OS drag was released; `pos` / `paths` valid.
    };
    /// Which kind of input event this is; determines which fields are valid.
    Kind kind = PointerMove;

    Vec2 pos{};                                  ///< Logical pixels, viewport coordinates.
    PointerButton button = PointerButton::Left;  ///< Button for PointerDown/PointerUp.
    /// Wheel: in notches (08.11), 1.0 = one notch (one line); touchpads and high-resolution
    /// wheels give fractions; a pixel-based host divides by its line height (40 logical px
    /// suggested) before pushing. Positive y scrolls down.
    Vec2 wheelDelta{};
    KeyCode key = KeyCode::Unknown;  ///< Key for KeyDown/KeyUp.
    uint8_t modifiers = 0;           ///< Modifiers bitmask (see Modifiers) active when this event occurred.
    uint8_t repeat = 0;              ///< Non-zero for an auto-repeated KeyDown.
    char utf8[8] = {};               ///< TextInput.
    uint8_t utf8Len = 0;             ///< Number of valid bytes in `utf8`.
    /// CompositionUpdate: current composition string. The host guarantees this view stays valid
    /// only for the duration of the pushInput() call; Context::pushInput copies it into an internal
    /// string pool immediately (see compositionPoolIndex below). That copy lives until the end of
    /// the Context::update() that dispatches the event (T-270a): the Event's `text` a listener
    /// sees is a view of it, so a listener keeping the text copies it (T-270b).
    std::string_view composition;
    uint32_t compositionCursor = 0;  ///< Byte offset within `composition`.
    /// Byte length of the selection within `composition`, from compositionCursor.
    uint32_t compositionSelLen = 0;
    uint32_t pointerId = 0;  ///< First version only uses 0.

    /// Set by Context::pushInput() for CompositionUpdate events: index into Context's internal
    /// std::string pool where `composition` was copied. Not part of docs/design/08-events.md's
    /// InputEvent (added by this task, see docs/tasks/T-20.md "Context 新增" note on
    /// pushInput). Unused (0) for every other Kind.
    uint32_t compositionPoolIndex = 0;

    // --- T-194 (docs/design/08-events.md §8.9): tail additions. ---
    /// GamepadButtonDown/Up: which button.
    GamepadButton gamepadButton = GamepadButton::A;
    /// GamepadAxisMotion: which axis.
    GamepadAxis gamepadAxis = GamepadAxis::LeftX;
    /// GamepadAxisMotion: the axis position (-1..1 for a stick, 0..1 for a trigger).
    float axisValue = 0;
    /// Which local player (0..kMaxPlayers-1) produced the event. Keyboard and mouse input is
    /// player 0 unless the host says otherwise.
    uint8_t player = 0;
    /// Gesture: which gesture.
    GestureKind gesture = GestureKind::Tap;
    /// Gesture (Swipe): the stroke's direction.
    NavDirection direction = NavDirection::Up;
    /// Gesture (Pinch): current finger distance / starting distance (> 1 = spreading).
    float gestureScale = 1;
    // --- T-379 (docs/design/08-events.md 8.10): tail addition. ---
    /// PointerDown/PointerUp: the host's click count (SDL's `button.clicks`); >= 2 on the release
    /// that completes a double click. 0 = unknown: the core detects double clicks itself.
    uint8_t clicks = 0;
    // --- T-393 (08.14): tail addition. ---
    /// ExternalDrop: the dropped files' paths (UTF-8). The views (and the array) only need to stay
    /// valid for the duration of the pushInput() call: Context::pushInput copies them, and the
    /// queued event points at that copy until the update() that dispatches it.
    std::span<const std::string_view> paths;
};

/// T-194 (08.9): most local players one Document tracks a focus for.
inline constexpr uint8_t kMaxPlayers = 8;

}  // namespace xt
