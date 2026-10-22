#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "xt/input.h"

/// SDL_Event -> InputEvent translation. See Docs/tasks/T-20.md "sdl_input.cpp",
/// Docs/design/08-events.md §8.1, §8.8.
///
/// `translateSdlEvent` and `translateSdlTextInput` have no header of their own (the task sheet
/// only lists `Code/Runtime/shell/src/sdl_input.cpp`, no `.h`); app.cpp forward-declares both with matching
/// signatures before use, same as any other internal-linkage-free function shared between two
/// translation units of one target.
///
/// Not covered by the task sheet (conservative choices made here):
///   - SDL_QUIT and SDL_TEXTEDITING are handled directly in app.cpp's event loop instead of here:
///     QUIT carries no InputEvent at all (app.cpp sets its own quit flag), and TEXTEDITING's
///     CompositionStart/End derivation needs state (has a composition started yet?) that a
///     single stateless `translateSdlEvent(event, dpr, out) -> bool` call cannot hold across
///     events, so app.cpp itself tracks `m_composing` and calls Context::pushInput up to twice per
///     TEXTEDITING event (Start+Update, or just Update, or End).
///   - SDL_TEXTINPUT can carry more than 7 UTF-8 bytes (InputEvent::utf8 is `char[8]`, 7 bytes +
///     NUL); `translateSdlTextInput` splits it into multiple TextInput events, each holding whole
///     UTF-8 codepoints only (never splitting a multi-byte codepoint across two events).
///   - `dpr` is accepted (per the task sheet's signature) but unused: Docs/tasks/T-20.md says SDL
///     mouse coordinates are already logical pixels, so no scaling is needed here.

namespace xt::shell {

namespace {

PointerButton mapButton(Uint8 sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_LEFT:
            return PointerButton::Left;
        case SDL_BUTTON_RIGHT:
            return PointerButton::Right;
        case SDL_BUTTON_MIDDLE:
            return PointerButton::Middle;
        default:
            return PointerButton::Left;
    }
}

}  // namespace

/// T-387 (08.12, 13 §13.9): SDL_Keymod bits -> InputEvent::modifiers bits (either Shift / Ctrl /
/// Alt / GUI key -> kShift / kCtrl / kAlt / kSuper). Shared with App::modifierState() (app.cpp
/// declares it, like translateSdlEvent).
uint8_t sdlModifiers(uint16_t sdlMod) {
    uint8_t mods = 0;
    if ((sdlMod & KMOD_SHIFT) != 0) {
        mods |= static_cast<uint8_t>(kShift);
    }
    if ((sdlMod & KMOD_CTRL) != 0) {
        mods |= static_cast<uint8_t>(kCtrl);
    }
    if ((sdlMod & KMOD_ALT) != 0) {
        mods |= static_cast<uint8_t>(kAlt);
    }
    if ((sdlMod & KMOD_GUI) != 0) {
        mods |= static_cast<uint8_t>(kSuper);
    }
    return mods;
}

namespace {

/// T-387 (08.12): the modifier keys held now, for a mouse / wheel event (SDL carries none).
uint8_t currentModifiers() { return sdlModifiers(static_cast<uint16_t>(SDL_GetModState())); }

/// Number of bytes in the UTF-8 codepoint starting with lead byte `c` (1-4); 1 for a continuation
/// or otherwise-invalid lead byte (treated as a lone byte so splitting always makes progress).
size_t utf8SeqLen(unsigned char c) {
    if ((c & 0x80u) == 0x00u) {
        return 1;
    }
    if ((c & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((c & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((c & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1;
}

/// SDL scancode -> KeyCode. SDL scancodes are USB HID usage ids, so the original keys (Unknown..Up,
/// which use HID numbering) pass through by value; the keys appended in T-143 use sequential
/// values and are mapped explicitly. Any other scancode becomes KeyCode::Unknown.
KeyCode mapScancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_SPACE) {
        return static_cast<KeyCode>(sc);
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (sc - SDL_SCANCODE_F1));
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Numpad1) + (sc - SDL_SCANCODE_KP_1));
    }
    switch (sc) {
        case SDL_SCANCODE_DELETE:
            return KeyCode::Delete;
        case SDL_SCANCODE_HOME:
            return KeyCode::Home;
        case SDL_SCANCODE_END:
            return KeyCode::End;
        case SDL_SCANCODE_RIGHT:
            return KeyCode::Right;
        case SDL_SCANCODE_LEFT:
            return KeyCode::Left;
        case SDL_SCANCODE_DOWN:
            return KeyCode::Down;
        case SDL_SCANCODE_UP:
            return KeyCode::Up;
        case SDL_SCANCODE_PAGEUP:
            return KeyCode::PageUp;
        case SDL_SCANCODE_PAGEDOWN:
            return KeyCode::PageDown;
        case SDL_SCANCODE_INSERT:
            return KeyCode::Insert;
        case SDL_SCANCODE_LSHIFT:
            return KeyCode::ShiftLeft;
        case SDL_SCANCODE_RSHIFT:
            return KeyCode::ShiftRight;
        case SDL_SCANCODE_LCTRL:
            return KeyCode::ControlLeft;
        case SDL_SCANCODE_RCTRL:
            return KeyCode::ControlRight;
        case SDL_SCANCODE_LALT:
            return KeyCode::AltLeft;
        case SDL_SCANCODE_RALT:
            return KeyCode::AltRight;
        case SDL_SCANCODE_LGUI:
            return KeyCode::MetaLeft;
        case SDL_SCANCODE_RGUI:
            return KeyCode::MetaRight;
        case SDL_SCANCODE_CAPSLOCK:
            return KeyCode::CapsLock;
        case SDL_SCANCODE_MINUS:
            return KeyCode::Minus;
        case SDL_SCANCODE_EQUALS:
            return KeyCode::Equal;
        case SDL_SCANCODE_LEFTBRACKET:
            return KeyCode::BracketLeft;
        case SDL_SCANCODE_RIGHTBRACKET:
            return KeyCode::BracketRight;
        case SDL_SCANCODE_BACKSLASH:
        case SDL_SCANCODE_NONUSHASH:
            return KeyCode::Backslash;
        case SDL_SCANCODE_SEMICOLON:
            return KeyCode::Semicolon;
        case SDL_SCANCODE_APOSTROPHE:
            return KeyCode::Quote;
        case SDL_SCANCODE_GRAVE:
            return KeyCode::Backquote;
        case SDL_SCANCODE_COMMA:
            return KeyCode::Comma;
        case SDL_SCANCODE_PERIOD:
            return KeyCode::Period;
        case SDL_SCANCODE_SLASH:
            return KeyCode::Slash;
        case SDL_SCANCODE_KP_0:
            return KeyCode::Numpad0;
        case SDL_SCANCODE_KP_PLUS:
            return KeyCode::NumpadAdd;
        case SDL_SCANCODE_KP_MINUS:
            return KeyCode::NumpadSubtract;
        case SDL_SCANCODE_KP_MULTIPLY:
            return KeyCode::NumpadMultiply;
        case SDL_SCANCODE_KP_DIVIDE:
            return KeyCode::NumpadDivide;
        case SDL_SCANCODE_KP_PERIOD:
            return KeyCode::NumpadDecimal;
        case SDL_SCANCODE_KP_ENTER:
            return KeyCode::NumpadEnter;
        default:
            return KeyCode::Unknown;
    }
}

}  // namespace

/// T-194 (08.9): SDL game controller button / axis -> InputEvent (the player slot is set by the
/// caller). SDL's button and axis numbering equals GamepadButton / GamepadAxis. False for anything
/// else (or an unknown button / axis).
bool translateSdlGamepadEvent(const SDL_Event& event, InputEvent& out) {
    switch (event.type) {
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            if (event.cbutton.button > SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                return false;
            }
            out = InputEvent{};
            out.kind = event.type == SDL_CONTROLLERBUTTONDOWN ? InputEvent::GamepadButtonDown
                                                              : InputEvent::GamepadButtonUp;
            out.gamepadButton = static_cast<GamepadButton>(event.cbutton.button);
            return true;
        case SDL_CONTROLLERAXISMOTION: {
            if (event.caxis.axis > SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                return false;
            }
            out = InputEvent{};
            out.kind = InputEvent::GamepadAxisMotion;
            out.gamepadAxis = static_cast<GamepadAxis>(event.caxis.axis);
            const float v = static_cast<float>(event.caxis.value) / 32767.0f;
            out.axisValue = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
            return true;
        }
        default:
            return false;
    }
}

bool translateSdlEvent(const SDL_Event& event, float dpr, InputEvent& out) {
    (void)dpr;
    switch (event.type) {
        case SDL_MOUSEMOTION:
            out = InputEvent{};
            out.kind = InputEvent::PointerMove;
            out.pos = Vec2{static_cast<float>(event.motion.x), static_cast<float>(event.motion.y)};
            out.modifiers = currentModifiers();  // T-387 (08.12).
            return true;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            out = InputEvent{};
            out.kind = event.type == SDL_MOUSEBUTTONDOWN ? InputEvent::PointerDown : InputEvent::PointerUp;
            out.pos = Vec2{static_cast<float>(event.button.x), static_cast<float>(event.button.y)};
            out.button = mapButton(event.button.button);
            out.clicks = event.button.clicks;    // T-379 (08.10): the host's double-click count.
            out.modifiers = currentModifiers();  // T-387 (08.12).
            return true;
        case SDL_MOUSEWHEEL: {
            out = InputEvent{};
            out.kind = InputEvent::Wheel;
            float x = event.wheel.preciseX;
            float y = event.wheel.preciseY;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                x = -x;
                y = -y;
            }
            // preciseY negated so positive y means "scroll down" (Docs/design/08-events.md §8.1).
            // T-386 (08.11): SDL's precise values are already notches (fractions from touchpads).
            out.wheelDelta = Vec2{x, -y};
            out.modifiers = currentModifiers();  // T-387 (08.12).
            return true;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            out = InputEvent{};
            out.kind = event.type == SDL_KEYDOWN ? InputEvent::KeyDown : InputEvent::KeyUp;
            out.key = mapScancode(event.key.keysym.scancode);
            out.modifiers = sdlModifiers(event.key.keysym.mod);
            out.repeat = event.key.repeat;
            return true;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                out = InputEvent{};
                out.kind = InputEvent::PointerLeave;
                return true;
            }
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                // T-386 (08.11): a press in progress is cancelled (no PointerUp / Click follows).
                out = InputEvent{};
                out.kind = InputEvent::PointerCancel;
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool translateSdlDrop(const SDL_Event& event, std::vector<std::string>& pending,
                      std::vector<std::string_view>& views, InputEvent& out) {
    // T-393 (08.14): SDL_DROPBEGIN starts a drop, each SDL_DROPFILE adds a path (SDL allocates
    // it; freed here), SDL_DROPTEXT is freed and ignored, SDL_DROPCOMPLETE ends it: one
    // ExternalDrop whose `paths` view `pending` through `views` (valid until either changes).
    switch (event.type) {
        case SDL_DROPBEGIN:
            pending.clear();
            return false;
        case SDL_DROPFILE:
        case SDL_DROPTEXT:
            if (event.drop.file != nullptr) {
                if (event.type == SDL_DROPFILE) {
                    pending.emplace_back(event.drop.file);
                }
                SDL_free(event.drop.file);
            }
            return false;
        case SDL_DROPCOMPLETE:
            if (pending.empty()) {
                return false;
            }
            views.assign(pending.begin(), pending.end());
            out = InputEvent{};
            out.kind = InputEvent::ExternalDrop;
            out.paths = views;
            out.modifiers = currentModifiers();
            return true;
        default:
            return false;
    }
}

size_t translateSdlTextInput(const SDL_Event& event, InputEvent* out, size_t maxOut) {
    if (event.type != SDL_TEXTINPUT || maxOut == 0) {
        return 0;
    }
    const char* text = event.text.text;
    size_t len = std::strlen(text);
    size_t produced = 0;
    size_t pos = 0;
    while (pos < len && produced < maxOut) {
        // Pack whole codepoints into each event while they still fit in 7 bytes.
        size_t chunkLen = 0;
        while (pos + chunkLen < len) {
            size_t seq = utf8SeqLen(static_cast<unsigned char>(text[pos + chunkLen]));
            if (pos + chunkLen + seq > len) {
                seq = 1;  // Truncated/invalid tail byte; consume it alone rather than overrun.
            }
            if (chunkLen + seq > 7) {
                break;
            }
            chunkLen += seq;
        }
        if (chunkLen == 0) {
            chunkLen = 1;  // A single codepoint longer than 7 bytes cannot happen in UTF-8 (max 4).
        }
        InputEvent& ev = out[produced++];
        ev = InputEvent{};
        ev.kind = InputEvent::TextInput;
        std::memcpy(ev.utf8, text + pos, chunkLen);
        ev.utf8[chunkLen] = '\0';
        ev.utf8Len = static_cast<uint8_t>(chunkLen);
        pos += chunkLen;
    }
    return produced;
}

}  // namespace xt::shell
