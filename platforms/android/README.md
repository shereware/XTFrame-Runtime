# XTFrame View for Android

A test app that runs an XTFrame bundle on an Android phone: the XTFrame runtime (`xt_core`,
`xt_compile`, `xt_sw`, `xt_text`, `xt_bundle`, `xt_flow`, `xt_shell`) built with the NDK and
statically linked into `libmain.so`, inside SDL2's Android shell (`SDLActivity`). It is the phone
counterpart of `xtframe-view`.

- Application id `org.xtframe.view`, name **XTFrame View**, minSdk 24, targetSdk 35.
- Rendering: the **SW presentation path** of `xt_shell` (`XT_SHELL_GL=OFF`). `xt_sw` renders each
  frame into a premultiplied RGBA buffer, redrawing only the damage rectangles; only those
  rectangles are uploaded to an `SDL_Renderer` streaming texture, which is then drawn to the
  screen. A frame where nothing changed renders and uploads nothing. (A GLES `xt_gl` is a later
  task.)
- Content: one directory bundle in the APK's `assets/bundle/`, packed at build time from
  `examples/demos/mobile-hud/` (or `examples/demos/game-ui/` while that project does not exist)
  with the host `xtframe-pack`. Every file (manifest, `.xtir`, images, fonts, flows) is read through
  `SDL_RWops`, so a bundle in the APK and one on the device's storage are read the same way.
- Input: touch (taps, multi-finger gestures: tap / hold / swipe / pinch), the back key (sent to
  the UI as the `system.back` event), the system keyboard for focused text inputs.
- Device: the viewport is the screen; uiscale follows the bundle's `spec.logicalHeight` and
  `spec.uiscaleRule` (`height`, `width` or `shortEdge`; `height` when absent); the safe area is the
  display cutout and system bars (`WindowInsets`, API 28+), re-read when the screen turns. A
  landscape UI turns the screen to landscape. Going to the background sends `system.paused`,
  coming back `system.resumed`.
- Flow host events (sound, haptic, spawn, emit) are only logged.

## Requirements

| What | Version | Where |
|------|---------|-------|
| Android SDK | platform 35, build-tools, platform-tools (adb) | `ANDROID_HOME` (default `C:/Android/Sdk`) |
| Android NDK | 27.3.13750724 | `$ANDROID_HOME/ndk/27.3.13750724` |
| CMake (SDK package) | 3.31.5 | installed by `tools/build_android.sh` through `sdkmanager` when missing |
| JDK | 17 or newer (21 tested) | `JAVA_HOME` (default `C:/Program Files/Microsoft/jdk-21.0.3.9-hotspot`) |
| Gradle | 8.10.2 (wrapper, downloaded on first run) | `~/.gradle` |
| Android Gradle Plugin | 8.7.3 (downloaded on first run) | `~/.gradle` |
| Host `xtframe-pack` | this repository's | `build/vs2022-editor/tools/Release/xtframe-pack.exe` (or set `XT_PACK`) |

SDL2 (2.30.9), FreeType (2.13.3) and HarfBuzz (10.1.0) are fetched by CMake (`cmake/xt_sdl2.cmake`,
`cmake/xt_text_deps.cmake`) and cross-compiled for each ABI; nothing prebuilt is checked in. The
Java files under `app/src/main/java/org/libsdl/app/` are SDL 2.30.9's own and must match the SDL tag
pinned in `cmake/xt_sdl2.cmake`.

## Build

```bash
# Once: the host packer.
cmake --build --preset vs2022-editor-release --target xtframe-pack
# The debug APK (arm64-v8a + x86_64).
bash tools/build_android.sh
```

The APK is `platforms/android/app/build/outputs/apk/debug/app-debug.apk`; the script prints its size
and the install command. `bash tools/build_android.sh --release` builds an unsigned arm64-only
release APK. Gradle properties `-PxtProject=<UI Project dir>` and `-PxtPack=<exe>` (after `--`)
choose another project or packer. `XT_CI_ANDROID=1 bash tools/local_ci.sh` adds this build to the
local CI.

## Install and run

```bash
adb install -r platforms/android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -S -n org.xtframe.view/.XTFrameActivity
adb logcat -s SDL/APP     # the app's log, including one xt-stats line per second
```

Or tap **XTFrame View** on the phone.

## Another bundle or UI

Push a bundle directory (the output of `xtframe-pack`) to the app's own external files directory,
which needs no storage permission, and start the app with the `XT_BUNDLE` extra:

```bash
xtframe-pack path/to/project -o out/mybundle --no-thumbs
adb push out/mybundle /sdcard/Android/data/org.xtframe.view/files/mybundle
adb shell am start -S -n org.xtframe.view/.XTFrameActivity \
    --es XT_BUNDLE /sdcard/Android/data/org.xtframe.view/files/mybundle --es XT_UI hud
```

Extras: `XT_BUNDLE` (bundle directory on the device; default the APK's `assets/bundle`), `XT_UI`
(UI id; default the manifest's first UI), `XT_STATS=0` (no frame-time log). `-S` restarts the app,
which is single-instance, so the extras take effect.

## Frame-time log

Every second the app logs, e.g.

```
xt-stats: 60 frames in 1.00 s, 2.10 ms/frame, 12 rendered (6.80 ms render, damage 8.4% of 1080x2400)
```

`ms/frame` is the whole frame on the app's thread (events, update, render, upload, the copy to
the screen; the vsync wait in present is included only when the frame waits for it); `rendered`
counts the frames that had damage, with their average SW render time and damaged area.

## Known limits

- Software rendering only (GLES `xt_gl` is a later task), so effect-heavy screens (blurs over large
  areas) cost CPU time on every frame they change.
- One UI at a time with its own flow; no global flow and no `ui#` switching between UIs.
- No sound, no haptics (logged only), no video, no `engine://` textures.
- No hot reload from the editor over Wi-Fi yet; push a new bundle and restart instead.
- Text uses the device fonts (`/system/fonts`, Roboto first) unless the bundle carries its own.
