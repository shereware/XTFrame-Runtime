#define SDL_MAIN_HANDLED  // doctest owns main(); SDL must not rename it.

#include "doctest.h"
#include "xt/context.h"
#include "xt/shell/gamepad.h"
#include "xt_platform_sdl2.h"

/// T-884 (Docs/design/60-input-channels.md §60.6): xtframe-view's --renderer window loop pumps
/// Code/Backends' PlatformSdl2, not xt_shell's App. The platform's event hook hands controller
/// events to xt_shell's GamepadInput -- the same hot plug, player slots and dead zones as App.
/// A virtual SDL controller stands in for hardware (see test_gamepad.cpp).

using namespace xt;

namespace {

SDL_Joystick* attachVirtual() {
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    return index < 0 ? nullptr : SDL_JoystickOpen(index);
}

}  // namespace

TEST_CASE("T-884: PlatformSdl2's event hook drives pad.* through xt_shell's GamepadInput") {
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x7880/0x0880");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    backend::PlatformSdl2 platform;
    backend::Sdl2Options options;
    options.width = 96;
    options.height = 80;
    options.hidden = true;
    REQUIRE_MESSAGE(platform.create(options), platform.lastError());
    {
        shell::GamepadInput gamepads;
        REQUIRE(gamepads.enable(1));
        platform.setEventHook([&gamepads](const SDL_Event& event, Context& context) {
            return gamepads.handleEvent(event, context);
        });
        Context ctx(ContextConfig{});
        SDL_Joystick* pad = attachVirtual();
        REQUIRE_MESSAGE(pad != nullptr, SDL_GetError());
        CHECK(platform.pumpEvents(ctx));
        ctx.update(0.016f);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, static_cast<Sint16>(0.62f * 32767.0f));
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_A, SDL_PRESSED);
        CHECK(platform.pumpEvents(ctx));
        ctx.update(0.016f);
        CHECK(ctx.channelValue(ctx.inputChannel("pad.lx"), 0) == doctest::Approx(0.5f).epsilon(0.002));
        CHECK(ctx.channelValue(ctx.inputChannel("pad.a"), 0) == doctest::Approx(1.0f));
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, 2000);  // Inside the dead zone.
        CHECK(platform.pumpEvents(ctx));
        ctx.update(0.016f);
        CHECK(ctx.channelValue(ctx.inputChannel("pad.lx"), 0) == 0.0f);
        SDL_JoystickClose(pad);
        platform.setEventHook({});
        gamepads.shutdown();  // Before the platform's SDL goes.
    }
    platform.destroy();
    SDL_ResetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
    SDL_ResetHint(SDL_HINT_JOYSTICK_RAWINPUT);
    SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
}
