#pragma once

#include <cstdint>
#include <vector>

#include "xt/context.h"
#include "xt/input.h"

// SDL2 is a real third-party dependency of xt_gamepad / xt_shell (see Tools/CMake/xt_sdl2.cmake).
#include <SDL.h>

/// T-884 (Docs/design/60-input-channels.md §60.6): SDL game controllers -> the pad.* channels of a
/// Context. Opens the controllers (and hot-plugged ones) in player slots, translates their buttons
/// and axes and applies the dead zones on the way -- the device-to-channel layer the dead zone
/// belongs to (the core takes what it is given). App owns one; a host with an event loop of its
/// own (xtframe-view's --renderer window, Code/Backends' PlatformSdl2 event hook) uses one directly.
/// T-881a: built as the library xt_gamepad (SDL2 and xt_core only, no window), which xt_shell links
/// and the editor preview links without xt_shell.

namespace xt::shell {

/// The dead zones (fraction of full deflection, 0 = off). Defaults are XInput's recommendations.
struct GamepadDeadzones {
    float left = 0.24f;     ///< left stick, circular (radius)
    float right = 0.265f;   ///< right stick, circular (radius)
    float trigger = 0.12f;  ///< each trigger, one-dimensional
};

/// T-194: an SDL controller button / axis event -> InputEvent, raw (no dead zone, player 0); false
/// for any other event. GamepadInput::handleEvent() uses it.
bool translateSdlGamepadEvent(const SDL_Event& event, InputEvent& out);

/// The circular, rescaled stick dead zone: (x, y) of length m <= d gives (0, 0); otherwise
/// (x, y) / m * (m - d) / (1 - d), its length clamped to 1. `d` <= 0 returns (x, y) unchanged.
Vec2 applyStickDeadzone(float x, float y, float d);
/// The trigger dead zone: v <= d gives 0; otherwise (v - d) / (1 - d) (at most 1). `d` <= 0
/// returns v unchanged.
float applyTriggerDeadzone(float v, float d);

class GamepadInput {
public:
    GamepadInput() = default;
    /// shutdown().
    ~GamepadInput();
    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

    /// Starts SDL's game controller subsystem (SDL itself must be initialised) and opens the
    /// controllers already connected, in player slots 0, 1, ... up to `maxPlayers` (1..kMaxPlayers,
    /// 0 = kMaxPlayers). Calling it again only changes the slot limit. False when SDL cannot.
    bool enable(uint8_t maxPlayers = kMaxPlayers);
    bool enabled() const { return m_enabled; }
    /// Closes the controllers and stops the subsystem enable() started. Must run before SDL_Quit.
    void shutdown();

    void setDeadzones(const GamepadDeadzones& deadzones) { m_deadzones = deadzones; }
    const GamepadDeadzones& deadzones() const { return m_deadzones; }

    /// A controller event (hot plug, button, axis) into `context`: true when `event` is one (it is
    /// consumed), false for anything else or before enable(). Axis values pass the dead zones; a
    /// stick event may push two GamepadAxisMotion events (both axes of the stick, each only when
    /// its output changed). An unplugged controller's axes that are not 0 are pushed back to 0.
    bool handleEvent(const SDL_Event& event, Context& context);
    /// True for the SDL event types handleEvent() consumes.
    static bool isControllerEvent(const SDL_Event& event);

    /// T-881b: one open controller -- its SDL instance id and player slot.
    struct Connected {
        int32_t instance = -1;
        uint8_t player = 0;
    };
    /// The controllers open now, in the order they were opened.
    std::vector<Connected> connected() const;

private:
    struct Pad {
        SDL_GameController* controller = nullptr;
        int32_t instance = -1;
        uint8_t player = 0;
        float raw[6] = {};   ///< last raw value per GamepadAxis
        float sent[6] = {};  ///< last value pushed per GamepadAxis
    };
    bool addDevice(int index);
    void pushAxis(Pad& pad, GamepadAxis axis, float value, Context& context);
    void handleAxis(Pad& pad, GamepadAxis axis, float raw, Context& context);

    std::vector<Pad> m_pads;
    GamepadDeadzones m_deadzones;
    bool m_enabled = false;
    uint8_t m_maxPlayers = kMaxPlayers;
};

}  // namespace xt::shell
