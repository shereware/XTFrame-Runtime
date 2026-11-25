# Fetches SDL2 (pinned tag, static) for the GL tests' hidden-window context and the future
# xt_shell. See Docs/tasks/T-19.md "Tools/CMake/xt_sdl2.cmake".
#
# Included once from the top-level CMakeLists.txt when XT_BUILD_GL OR XT_BUILD_SHELL. Defines the
# `SDL2::SDL2-static` (and `SDL2::SDL2main` on Windows) targets used by Tests/gl and shell.

include(${CMAKE_CURRENT_LIST_DIR}/xt_sdks.cmake)

# Minimal subsystems: video + events, plus the joystick / game controller drivers on desktop and
# Android (T-880, Docs/design/60-input-channels.md §60.6). Everything else that SDL2 can build
# (audio, haptic, sensors, power, locale and the shared library) is disabled.
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
elseif(WIN32 OR APPLE OR (UNIX AND NOT EMSCRIPTEN))
    # T-880 (60 §60.6): desktop game controllers. SDL_JOYSTICK brings the platform drivers --
    # Windows: RawInput, XInput and Windows.Gaming.Input (SDL_XINPUT; its default, set here so it
    # is visible) and DirectInput (SDL_DIRECTX, SDL's default); Linux: evdev, with libudev loaded at
    # run time when its header is found (SDL_LIBUDEV; without it SDL falls back to scanning
    # /dev/input and the configure step still succeeds); macOS: IOKit and, when CoreHaptics is
    # there, GameController (MFi). SDL_HIDAPI adds SDL's own Xbox / PlayStation / Switch drivers
    # (SDL_HIDAPI_JOYSTICK) and the virtual-joystick driver (SDL_VIRTUAL_JOYSTICK; the hardware-free
    # tests attach a virtual controller). No libusb (SDL_HIDAPI_LIBUSB OFF, SDL's default). Haptic
    # stays off: the `haptic` action is only logged by the desktop hosts, and controller rumble
    # (SDL_GameControllerRumble) does not need it.
    set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "" FORCE)
    set(SDL_VIRTUAL_JOYSTICK ON CACHE BOOL "" FORCE)
    if(WIN32)
        set(SDL_XINPUT ON CACHE BOOL "" FORCE)
    elseif(NOT APPLE)
        set(SDL_LIBUDEV ON CACHE BOOL "" FORCE)
    endif()
endif()

# Version: Code/SDKs/versions.cmake (sdl2); source: Code/SDKs/_fetched/sdl2 (Tools/CMake/xt_sdks.cmake).
xt_sdk_fetch(sdl2 SDL2)

# T-880: print the joystick drivers SDL's configure step selected (SDL sets them in its own
# directory scope), so the configure log shows what a build can read.
if(DEFINED sdl2_SOURCE_DIR)
    set(_xt_sdl_joy "")
    foreach(_xt_drv HIDAPI VIRTUAL RAWINPUT XINPUT WGI DINPUT LINUX IOKIT MFI ANDROID DUMMY)
        get_directory_property(_xt_on DIRECTORY "${sdl2_SOURCE_DIR}" DEFINITION SDL_JOYSTICK_${_xt_drv})
        if(_xt_on)
            string(TOLOWER "${_xt_drv}" _xt_drv)
            list(APPEND _xt_sdl_joy ${_xt_drv})
        endif()
    endforeach()
    get_directory_property(_xt_on DIRECTORY "${sdl2_SOURCE_DIR}" DEFINITION HAVE_LIBUDEV_H)
    if(_xt_on)
        list(APPEND _xt_sdl_joy udev)
    endif()
    list(JOIN _xt_sdl_joy " " _xt_sdl_joy)
    message(STATUS "xt_sdl2: joystick drivers: ${_xt_sdl_joy}")
endif()

# SDL2's static target does not carry every Apple framework it references (IOKit for display
# names / HID, ForceFeedback, GameController, CoreHaptics); add them so consumers link.
if(APPLE AND TARGET SDL2-static)
    target_link_libraries(SDL2-static INTERFACE
        "-framework IOKit" "-framework ForceFeedback" "-framework GameController"
        "-weak_framework CoreHaptics" "-framework AudioToolbox" "-framework CoreAudio"
        "-framework AVFoundation" "-framework CoreMedia")
endif()
