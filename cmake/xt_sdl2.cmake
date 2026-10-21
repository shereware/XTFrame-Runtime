# Fetches SDL2 (pinned tag, static) for the GL tests' hidden-window context and the future
# xt_shell. See docs/tasks/T-19.md "cmake/xt_sdl2.cmake".
#
# Included once from the top-level CMakeLists.txt when XT_BUILD_GL OR XT_BUILD_SHELL. Defines the
# `SDL2::SDL2-static` (and `SDL2::SDL2main` on Windows) targets used by tests/gl and shell.

include(FetchContent)

# Minimal subsystems: video + events only. Everything else that SDL2 can build (audio, joystick,
# haptic, sensors, render, and the shared library) is disabled -- xt only needs a window + GL
# context.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
# T-240: the SW presentation path (XT_SHELL_GL=OFF) presents through an SDL_Renderer.
if(XT_SHELL_GL)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
else()
    set(SDL_RENDER ON CACHE BOOL "" FORCE)
endif()
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_LOCALE OFF CACHE BOOL "" FORCE)
# T-387 (13 §13.9): App::openUrl calls SDL_OpenURL, which lives in SDL's misc subsystem.
set(SDL_MISC ON CACHE BOOL "" FORCE)
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_EVENTS ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
# T-240: SDL's Android Java shell (SDLActivity -> HIDDeviceManager) calls the HIDAPI natives at
# start-up unconditionally, so the Android build keeps joystick + HIDAPI (gamepads work too).
if(ANDROID)
    set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
    set(SDL_HAPTIC ON CACHE BOOL "" FORCE)  # SDL_android.c's joystick code calls the haptic hooks.
endif()

FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL2)

# SDL2's static target does not carry every Apple framework it references (IOKit for display
# names / HID, ForceFeedback, GameController, CoreHaptics); add them so consumers link.
if(APPLE AND TARGET SDL2-static)
    target_link_libraries(SDL2-static INTERFACE
        "-framework IOKit" "-framework ForceFeedback" "-framework GameController"
        "-weak_framework CoreHaptics" "-framework AudioToolbox" "-framework CoreAudio"
        "-framework AVFoundation" "-framework CoreMedia")
endif()
