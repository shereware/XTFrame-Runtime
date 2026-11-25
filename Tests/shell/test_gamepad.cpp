#define SDL_MAIN_HANDLED  // doctest owns main(); SDL must not rename it.

#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/event.h"
#include "xt/ir.h"
#include "xt/shell/app.h"
#include "xt/shell/gamepad.h"

/// T-880 (Docs/design/60-input-channels.md §60.6, 08 §8.9): App opens SDL's game controllers by
/// default; WindowConfig::gamepad = false leaves the subsystem off. Without hardware, SDL's virtual
/// joystick driver stands in for a controller: its axes and buttons travel through SDL's game
/// controller layer, App's hot-plug and translation into the pad.* channels (the right stick and
/// the triggers included), and player slots follow WindowConfig::gamepadPlayers.

namespace xt::shell {
bool translateSdlGamepadEvent(const SDL_Event& event, InputEvent& out);  // sdl_input.cpp (no header).
}  // namespace xt::shell

using namespace xt;
using namespace xt::shell;

namespace {

WindowConfig hiddenConfig() {
    WindowConfig config;
    config.width = 160;
    config.height = 120;
    config.hidden = true;
    config.vsync = false;
    config.msaa = 0;
    config.liveResize = false;
    config.acceptFileDrop = false;
    return config;
}

void pump(App& app, int frames = 3) {
    for (int i = 0; i < frames; ++i) {
        app.runFrame([](float) {}, 1.0f / 60.0f, false);
    }
}

/// Attaches a virtual game controller (6 axes, 15 buttons) and opens it for the test; nullptr when
/// SDL cannot (the build has no virtual joystick driver).
SDL_Joystick* attachVirtualPad() {
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    if (index < 0) {
        return nullptr;
    }
    return SDL_JoystickOpen(index);
}

/// While alive: only the virtual controllers count. A real controller on the test machine would
/// take player slot 0 (RawInput ignores the device list, hence its own hint), and SDL reads the
/// joystick axes and buttons of an unfocused (hidden) window only with background events allowed.
struct VirtualOnly {
    VirtualOnly() {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x7880/0x0880");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    ~VirtualOnly() {
        SDL_ResetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
        SDL_ResetHint(SDL_HINT_JOYSTICK_RAWINPUT);
        SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    }
    VirtualOnly(const VirtualOnly&) = delete;
    VirtualOnly& operator=(const VirtualOnly&) = delete;
};

}  // namespace

TEST_CASE("T-880: App opens the game controller subsystem by default, not with gamepad = false") {
    {
        App app(hiddenConfig());
        REQUIRE(app.ok());
        CHECK(app.gamepadEnabled());
        CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0);
    }
    {
        WindowConfig config = hiddenConfig();
        config.gamepad = false;
        App app(config);
        REQUIRE(app.ok());
        CHECK_FALSE(app.gamepadEnabled());
        CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0);
        CHECK(SDL_WasInit(SDL_INIT_JOYSTICK) == 0);
    }
}

TEST_CASE("T-880: translateSdlGamepadEvent keeps all six axes (right stick, triggers) and the buttons") {
    const struct {
        Uint8 axis;
        Sint16 raw;
        GamepadAxis expected;
        float value;
    } axes[] = {
        {SDL_CONTROLLER_AXIS_LEFTX, 32767, GamepadAxis::LeftX, 1.0f},
        {SDL_CONTROLLER_AXIS_LEFTY, -32768, GamepadAxis::LeftY, -1.0f},
        {SDL_CONTROLLER_AXIS_RIGHTX, 16384, GamepadAxis::RightX, 16384.0f / 32767.0f},
        {SDL_CONTROLLER_AXIS_RIGHTY, -8192, GamepadAxis::RightY, -8192.0f / 32767.0f},
        {SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767, GamepadAxis::TriggerLeft, 1.0f},
        {SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 8192, GamepadAxis::TriggerRight, 8192.0f / 32767.0f},
    };
    for (const auto& a : axes) {
        SDL_Event event{};
        event.type = SDL_CONTROLLERAXISMOTION;
        event.caxis.axis = a.axis;
        event.caxis.value = a.raw;
        InputEvent out;
        REQUIRE(translateSdlGamepadEvent(event, out));
        CHECK(out.kind == InputEvent::GamepadAxisMotion);
        CHECK(out.gamepadAxis == a.expected);
        CHECK(out.axisValue == doctest::Approx(a.value));
    }
    SDL_Event button{};
    button.type = SDL_CONTROLLERBUTTONDOWN;
    button.cbutton.button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    InputEvent out;
    REQUIRE(translateSdlGamepadEvent(button, out));
    CHECK(out.kind == InputEvent::GamepadButtonDown);
    CHECK(out.gamepadButton == GamepadButton::RightShoulder);
}

TEST_CASE("T-880: a (virtual) game controller moves pad.* through App, right stick and triggers included") {
    const VirtualOnly virtualOnly;
    // T-884: dead zones off -- this case checks the raw conversion.
    WindowConfig config = hiddenConfig();
    config.gamepadDeadzoneLeft = 0.0f;
    config.gamepadDeadzoneRight = 0.0f;
    config.gamepadDeadzoneTrigger = 0.0f;
    App app(config);
    REQUIRE(app.ok());
    REQUIRE(app.gamepadEnabled());
    SDL_Joystick* pad = attachVirtualPad();
    REQUIRE_MESSAGE(pad != nullptr, SDL_GetError());
    pump(app);  // CONTROLLERDEVICEADDED -> App opens it (player 0).

    Context& ctx = app.context();
    const ChannelId lx = ctx.inputChannel("pad.lx");
    const ChannelId ly = ctx.inputChannel("pad.ly");
    const ChannelId rx = ctx.inputChannel("pad.rx");
    const ChannelId ry = ctx.inputChannel("pad.ry");
    const ChannelId lt = ctx.inputChannel("pad.lt");
    const ChannelId rt = ctx.inputChannel("pad.rt");
    const ChannelId a = ctx.inputChannel("pad.a");

    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTY, -16384);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX, 16384);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);  // Fully pressed.
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);     // Half way.
    SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_A, SDL_PRESSED);
    pump(app);

    CHECK(ctx.channelValue(lx, 0) == doctest::Approx(1.0f));
    CHECK(ctx.channelValue(ly, 0) == doctest::Approx(-16384.0f / 32767.0f));
    CHECK(ctx.channelValue(rx, 0) == doctest::Approx(16384.0f / 32767.0f));
    CHECK(ctx.channelValue(ry, 0) == doctest::Approx(-1.0f));
    CHECK(ctx.channelValue(lt, 0) == doctest::Approx(1.0f));
    CHECK(ctx.channelValue(rt, 0) == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(ctx.channelValue(a, 0) == doctest::Approx(1.0f));

    SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_A, SDL_RELEASED);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX, 0);
    pump(app);
    CHECK(ctx.channelValue(a, 0) == doctest::Approx(0.0f));
    CHECK(ctx.channelValue(rx, 0) == doctest::Approx(0.0f));

    // Hot unplug frees player slot 0 (the only one): the next controller plugged in gets it.
    const SDL_JoystickID instance = SDL_JoystickInstanceID(pad);
    SDL_JoystickClose(pad);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) == instance) {
            REQUIRE(SDL_JoystickDetachVirtual(i) == 0);
            break;
        }
    }
    pump(app);
    SDL_Joystick* next = attachVirtualPad();
    REQUIRE_MESSAGE(next != nullptr, SDL_GetError());
    pump(app);
    SDL_JoystickSetVirtualAxis(next, SDL_CONTROLLER_AXIS_RIGHTX, -32768);
    pump(app);
    CHECK(ctx.channelValue(rx, 0) == doctest::Approx(-1.0f));
    SDL_JoystickClose(next);
}

TEST_CASE("T-880: player slots follow gamepadPlayers (default 1: a second controller is ignored)") {
    for (const uint8_t players : {uint8_t{1}, uint8_t{2}}) {
        CAPTURE(players);
        const VirtualOnly virtualOnly;  // Per App: SDL_Quit clears the hints.
        WindowConfig config = hiddenConfig();
        if (players != 1) {
            config.gamepadPlayers = players;
        }
        App app(config);
        REQUIRE(app.ok());
        SDL_Joystick* first = attachVirtualPad();
        REQUIRE_MESSAGE(first != nullptr, SDL_GetError());
        pump(app);
        SDL_Joystick* second = attachVirtualPad();
        REQUIRE_MESSAGE(second != nullptr, SDL_GetError());
        pump(app);

        Context& ctx = app.context();
        const ChannelId rx = ctx.inputChannel("pad.rx");
        SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_RIGHTX, 32767);
        SDL_JoystickSetVirtualAxis(second, SDL_CONTROLLER_AXIS_RIGHTX, -32768);
        pump(app);
        CHECK(ctx.channelValue(rx, 0) == doctest::Approx(1.0f));
        CHECK(ctx.channelValue(rx, 1) == doctest::Approx(players == 2 ? -1.0f : 0.0f));

        SDL_JoystickClose(first);
        SDL_JoystickClose(second);
    }
}

// ---- T-884 (60 §60.6): the dead zones of the device-to-channel layer -------------------------

namespace {

Sint16 stickRaw(float v) { return static_cast<Sint16>(v >= 0 ? v * 32767.0f + 0.5f : v * 32767.0f - 0.5f); }
/// SDL's game controller layer maps a virtual trigger's -32768..32767 onto 0..32767.
Sint16 triggerRaw(float v) { return static_cast<Sint16>(v * 65535.0f - 32768.0f + 0.5f); }

void setStick(SDL_Joystick* pad, bool left, float x, float y) {
    SDL_JoystickSetVirtualAxis(pad, left ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX,
                               stickRaw(x));
    SDL_JoystickSetVirtualAxis(pad, left ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY,
                               stickRaw(y));
}

/// A Document that listens to the stick and trigger channels (so a held one keeps needsFrame()).
compile::CompileResult listeningDocument() {
    const std::string xt =
        "<xt><body id=\"Root\">"
        "<div id=\"L\" bind:opacity=\"input(pad.lx) | range(-1, 1, 0, 1)\"></div>"
        "<div id=\"M\" bind:opacity=\"input(pad.ly) | range(-1, 1, 0, 1)\"></div>"
        "<div id=\"T\" bind:opacity=\"input(pad.lt)\"></div>"
        "</body></xt>";
    compile::CompileOptions options;
    std::vector<compile::SourceFile> files{{"main.xt", xt}};
    return compile::compileDocumentFromStrings(files, "main.xt", options);
}

void detach(SDL_Joystick* pad) {
    const SDL_JoystickID instance = SDL_JoystickInstanceID(pad);
    SDL_JoystickClose(pad);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) == instance) {
            SDL_JoystickDetachVirtual(i);
            break;
        }
    }
}

constexpr float kEps = 0.002f;

}  // namespace

TEST_CASE("T-884: the circular rescaled stick dead zone and the trigger dead zone (pure functions)") {
    CHECK(applyStickDeadzone(0.2f, 0.1f, 0.24f).x == 0.0f);
    CHECK(applyStickDeadzone(0.2f, 0.1f, 0.24f).y == 0.0f);
    const Vec2 east = applyStickDeadzone(0.62f, 0.0f, 0.24f);
    CHECK(east.x == doctest::Approx(0.5f));
    CHECK(east.y == 0.0f);
    const float d45 = 0.62f * 0.70710678f;
    const Vec2 diag = applyStickDeadzone(d45, -d45, 0.24f);
    CHECK(diag.x == doctest::Approx(0.5f * 0.70710678f));
    CHECK(diag.y == doctest::Approx(-0.5f * 0.70710678f));
    const Vec2 corner = applyStickDeadzone(1.0f, 1.0f, 0.24f);  // Raw length 1.41: clamped to 1.
    CHECK(corner.x == doctest::Approx(0.70710678f));
    CHECK(corner.y == doctest::Approx(0.70710678f));
    CHECK(applyStickDeadzone(0.25f, 0.0f, 0.24f).x == doctest::Approx(0.01f / 0.76f));  // Continuous.
    const Vec2 off = applyStickDeadzone(1.0f, -0.5f, 0.0f);  // 0 = off: unchanged, not clamped.
    CHECK(off.x == 1.0f);
    CHECK(off.y == -0.5f);
    CHECK(applyTriggerDeadzone(0.1f, 0.12f) == 0.0f);
    CHECK(applyTriggerDeadzone(0.56f, 0.12f) == doctest::Approx(0.5f));
    CHECK(applyTriggerDeadzone(1.0f, 0.12f) == doctest::Approx(1.0f));
    CHECK(applyTriggerDeadzone(0.3f, 0.0f) == 0.3f);
}

TEST_CASE("T-884: App applies the default dead zones to a (virtual) controller") {
    const VirtualOnly virtualOnly;
    App app(hiddenConfig());
    REQUIRE(app.ok());
    const compile::CompileResult doc = listeningDocument();
    REQUIRE(doc.ok());
    Context& ctx = app.context();
    const DocumentHandle handle = ctx.loadDocument(IrBlob{doc.ir.data(), doc.ir.size()}, false).value();
    app.setCurrentDocument(handle);
    SDL_Joystick* pad = attachVirtualPad();
    REQUIRE_MESSAGE(pad != nullptr, SDL_GetError());
    pump(app);
    const ChannelId lx = ctx.inputChannel("pad.lx");
    const ChannelId ly = ctx.inputChannel("pad.ly");
    const ChannelId rx = ctx.inputChannel("pad.rx");
    const ChannelId ry = ctx.inputChannel("pad.ry");
    const ChannelId lt = ctx.inputChannel("pad.lt");

    SUBCASE("inside the circle: both axes 0") {
        setStick(pad, true, 0.2f, -0.12f);
        pump(app);
        CHECK(ctx.channelValue(lx, 0) == 0.0f);
        CHECK(ctx.channelValue(ly, 0) == 0.0f);
    }
    SUBCASE("outside: rescaled at three angles and clamped on the corner") {
        setStick(pad, true, 0.62f, 0.0f);  // East: (0.62 - 0.24) / 0.76 = 0.5.
        pump(app);
        CHECK(ctx.channelValue(lx, 0) == doctest::Approx(0.5f).epsilon(kEps));
        CHECK(ctx.channelValue(ly, 0) == 0.0f);
        setStick(pad, false, 0.0f, -0.63235f);  // Right stick north: (0.632 - 0.265) / 0.735 = 0.5.
        pump(app);
        CHECK(ctx.channelValue(rx, 0) == 0.0f);
        CHECK(ctx.channelValue(ry, 0) == doctest::Approx(-0.5f).epsilon(kEps));
        const float d45 = 0.62f * 0.70710678f;
        setStick(pad, true, d45, d45);  // Diagonal: length 0.5, direction kept.
        pump(app);
        CHECK(ctx.channelValue(lx, 0) == doctest::Approx(0.35355f).epsilon(kEps));
        CHECK(ctx.channelValue(ly, 0) == doctest::Approx(0.35355f).epsilon(kEps));
        setStick(pad, true, 1.0f, -1.0f);  // The corner: length 1.
        pump(app);
        CHECK(ctx.channelValue(lx, 0) == doctest::Approx(0.70711f).epsilon(kEps));
        CHECK(ctx.channelValue(ly, 0) == doctest::Approx(-0.70711f).epsilon(kEps));
    }
    SUBCASE("continuous: just outside the circle is close to 0") {
        setStick(pad, true, 0.26f, 0.0f);
        pump(app);
        CHECK(ctx.channelValue(lx, 0) > 0.0f);
        CHECK(ctx.channelValue(lx, 0) < 0.03f);
    }
    SUBCASE("back to rest with drift: both axes exactly 0 and needsFrame() turns false") {
        setStick(pad, true, 0.7f, -0.4f);
        pump(app);
        CHECK(ctx.channelValue(lx, 0) != 0.0f);
        CHECK(ctx.channelValue(ly, 0) != 0.0f);
        setStick(pad, true, 0.03f, -0.02f);  // A real stick never returns to exactly 0.
        pump(app, 6);
        CHECK(ctx.channelValue(lx, 0) == 0.0f);
        CHECK(ctx.channelValue(ly, 0) == 0.0f);
        CHECK_FALSE(ctx.needsFrame());
    }
    SUBCASE("triggers: one-dimensional") {
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, triggerRaw(0.1f));
        pump(app);
        CHECK(ctx.channelValue(lt, 0) == 0.0f);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, triggerRaw(0.56f));
        pump(app);
        CHECK(ctx.channelValue(lt, 0) == doctest::Approx(0.5f).epsilon(kEps));
    }
    SUBCASE("hot unplug: the axes still off 0 go back to 0") {
        setStick(pad, true, 1.0f, 0.0f);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, triggerRaw(1.0f));
        pump(app);
        REQUIRE(ctx.channelValue(lx, 0) == doctest::Approx(1.0f));
        REQUIRE(ctx.channelValue(lt, 0) == doctest::Approx(1.0f));
        detach(pad);
        pad = nullptr;
        pump(app, 6);
        CHECK(ctx.channelValue(lx, 0) == 0.0f);
        CHECK(ctx.channelValue(lt, 0) == 0.0f);
        CHECK_FALSE(ctx.needsFrame());
    }
    if (pad != nullptr) {
        SDL_JoystickClose(pad);
    }
}

TEST_CASE("T-884: WindowConfig dead zones of 0 give the raw conversion; injected axes bypass them") {
    const VirtualOnly virtualOnly;
    WindowConfig config = hiddenConfig();
    config.gamepadDeadzoneLeft = 0.0f;
    config.gamepadDeadzoneRight = 0.0f;
    config.gamepadDeadzoneTrigger = 0.0f;
    App app(config);
    REQUIRE(app.ok());
    SDL_Joystick* pad = attachVirtualPad();
    REQUIRE_MESSAGE(pad != nullptr, SDL_GetError());
    pump(app);
    Context& ctx = app.context();
    const ChannelId lx = ctx.inputChannel("pad.lx");
    const ChannelId ly = ctx.inputChannel("pad.ly");
    const ChannelId lt = ctx.inputChannel("pad.lt");
    setStick(pad, true, 0.1f, -0.05f);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, triggerRaw(0.05f));
    pump(app);
    CHECK(ctx.channelValue(lx, 0) == doctest::Approx(stickRaw(0.1f) / 32767.0f));
    CHECK(ctx.channelValue(ly, 0) == doctest::Approx(stickRaw(-0.05f) / 32767.0f));
    CHECK(ctx.channelValue(lt, 0) == doctest::Approx(0.05f).epsilon(kEps));
    SDL_JoystickClose(pad);
}

TEST_CASE("T-884: an axis pushed into the Context (as --event axis does) never meets the dead zone") {
    App app(hiddenConfig());
    REQUIRE(app.ok());
    InputEvent axis;
    axis.kind = InputEvent::GamepadAxisMotion;
    axis.gamepadAxis = GamepadAxis::LeftX;
    axis.axisValue = 0.1f;
    app.context().pushInput(axis);
    pump(app);
    CHECK(app.context().channelValue(app.context().inputChannel("pad.lx"), 0) == doctest::Approx(0.1f));
}
