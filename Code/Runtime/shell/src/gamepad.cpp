#include "xt/shell/gamepad.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace xt::shell {

namespace {

constexpr size_t kAxisCount = 6;

bool isTrigger(GamepadAxis axis) {
    return axis == GamepadAxis::TriggerLeft || axis == GamepadAxis::TriggerRight;
}

}  // namespace

/// T-194 (08.9): SDL game controller button / axis -> InputEvent (the player slot is set by the
/// caller). SDL's button and axis numbering equals GamepadButton / GamepadAxis. False for anything
/// else (or an unknown button / axis). T-881a: moved here from sdl_input.cpp (xt_gamepad).
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

Vec2 applyStickDeadzone(float x, float y, float d) {
    if (d <= 0.0f) {
        return Vec2{x, y};
    }
    const float m = std::sqrt(x * x + y * y);
    if (m <= d) {
        return Vec2{0.0f, 0.0f};
    }
    const float scaled = std::min((m - d) / (1.0f - d), 1.0f);  // A diagonal's raw length passes 1.
    return Vec2{x / m * scaled, y / m * scaled};
}

float applyTriggerDeadzone(float v, float d) {
    if (d <= 0.0f) {
        return v;
    }
    return v <= d ? 0.0f : std::min((v - d) / (1.0f - d), 1.0f);
}

GamepadInput::~GamepadInput() { shutdown(); }

bool GamepadInput::enable(uint8_t maxPlayers) {
    m_maxPlayers = maxPlayers == 0 || maxPlayers > kMaxPlayers ? kMaxPlayers : maxPlayers;
    if (!m_enabled) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            return false;
        }
        m_enabled = true;
    }
    // Controllers already connected; SDL also queues a CONTROLLERDEVICEADDED for each of them,
    // which addDevice() ignores once the instance is open.
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        addDevice(i);
    }
    return true;
}

void GamepadInput::shutdown() {
    for (Pad& pad : m_pads) {
        if (pad.controller != nullptr) {
            SDL_GameControllerClose(pad.controller);
        }
    }
    m_pads.clear();
    if (m_enabled) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        m_enabled = false;
    }
}

bool GamepadInput::isControllerEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
            return true;
        default:
            return false;
    }
}

std::vector<GamepadInput::Connected> GamepadInput::connected() const {
    std::vector<Connected> out;
    out.reserve(m_pads.size());
    for (const Pad& pad : m_pads) {
        out.push_back(Connected{pad.instance, pad.player});
    }
    return out;
}

bool GamepadInput::addDevice(int index) {
    if (!SDL_IsGameController(index)) {
        return false;
    }
    const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
    if (std::any_of(m_pads.begin(), m_pads.end(),
                    [instance](const Pad& p) { return p.instance == instance; })) {
        return false;  // Already open.
    }
    uint8_t player = 0;
    while (player < m_maxPlayers &&
           std::any_of(m_pads.begin(), m_pads.end(), [player](const Pad& p) { return p.player == player; })) {
        ++player;
    }
    if (player >= m_maxPlayers) {
        return false;  // Every player slot is taken.
    }
    SDL_GameController* controller = SDL_GameControllerOpen(index);
    if (controller == nullptr) {
        return false;
    }
    Pad pad;
    pad.controller = controller;
    pad.instance = instance;
    pad.player = player;
    m_pads.push_back(pad);
    return true;
}

void GamepadInput::pushAxis(Pad& pad, GamepadAxis axis, float value, Context& context) {
    const size_t i = static_cast<size_t>(axis);
    if (pad.sent[i] == value) {
        return;
    }
    pad.sent[i] = value;
    InputEvent input;
    input.kind = InputEvent::GamepadAxisMotion;
    input.gamepadAxis = axis;
    input.axisValue = value;
    input.player = pad.player;
    context.pushInput(input);
}

void GamepadInput::handleAxis(Pad& pad, GamepadAxis axis, float raw, Context& context) {
    pad.raw[static_cast<size_t>(axis)] = raw;
    if (isTrigger(axis)) {
        pushAxis(pad, axis, applyTriggerDeadzone(raw, m_deadzones.trigger), context);
        return;
    }
    const bool left = axis == GamepadAxis::LeftX || axis == GamepadAxis::LeftY;
    const float d = left ? m_deadzones.left : m_deadzones.right;
    if (d <= 0.0f) {
        pushAxis(pad, axis, raw, context);  // Off: the raw value of this axis alone.
        return;
    }
    // Circular: the stick's two axes are one vector, so one SDL axis event may move both outputs.
    const GamepadAxis ax = left ? GamepadAxis::LeftX : GamepadAxis::RightX;
    const GamepadAxis ay = left ? GamepadAxis::LeftY : GamepadAxis::RightY;
    const Vec2 out =
        applyStickDeadzone(pad.raw[static_cast<size_t>(ax)], pad.raw[static_cast<size_t>(ay)], d);
    pushAxis(pad, ax, out.x, context);
    pushAxis(pad, ay, out.y, context);
}

bool GamepadInput::handleEvent(const SDL_Event& event, Context& context) {
    if (!m_enabled || !isControllerEvent(event)) {
        return false;
    }
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            addDevice(event.cdevice.which);
            return true;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (size_t i = 0; i < m_pads.size(); ++i) {
                Pad& pad = m_pads[i];
                if (pad.instance != event.cdevice.which) {
                    continue;
                }
                // SDL recenters a removed joystick's axes itself; a channel must never stay at
                // the last value of a controller that is gone, so any axis left off 0 goes back.
                for (size_t a = 0; a < kAxisCount; ++a) {
                    pushAxis(pad, static_cast<GamepadAxis>(a), 0.0f, context);
                }
                SDL_GameControllerClose(pad.controller);
                m_pads.erase(m_pads.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
            return true;
        default: {
            const SDL_JoystickID which =
                event.type == SDL_CONTROLLERAXISMOTION ? event.caxis.which : event.cbutton.which;
            for (Pad& pad : m_pads) {
                if (pad.instance != which) {
                    continue;
                }
                InputEvent input;
                if (translateSdlGamepadEvent(event, input)) {
                    if (input.kind == InputEvent::GamepadAxisMotion) {
                        handleAxis(pad, input.gamepadAxis, input.axisValue, context);
                    } else {
                        input.player = pad.player;
                        context.pushInput(input);
                    }
                }
                break;
            }
            return true;
        }
    }
}

}  // namespace xt::shell
