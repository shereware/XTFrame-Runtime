# Backends

Reference glue between the XTFrame runtime and a host environment, in the spirit of RmlUi's
`Backends/`: one **platform** file pair per windowing/input library, one **renderer** file pair per
graphics API, and one **backend** file per platform × renderer combination that wires the two
together for the samples. Every file is meant to be copied into a host project and edited there;
nothing in this directory is a library the runtime depends on.

Layer: R (runtime, public, MIT). Design: `Docs/design/45-repository-layout.md` §45.7,
`Docs/design/23-host-integration.md`.

## Naming

| Kind | File | Contents |
|---|---|---|
| common | `xt_backend.h` | the small interface the samples call: create/destroy, pump events, begin/end frame, present |
| platform | `xt_platform_<name>.h/.cpp` | window, input → `xt::InputEvent`, clock, DPI, clipboard, IME hooks |
| renderer | `xt_renderer_<name>.h/.cpp` | consumes `xt_gpu` frames (or `xt_gl` / `xt_sw` directly) on one graphics API |
| backend | `xt_backend_<platform>_<renderer>.cpp` | the combination; what `Examples/` link against |

Planned names (a row exists once its files do; `CMakeLists.txt` adds a backend only when every
file of the pair is present):

- platforms: `sdl2`, `win32`, `glfw`, `x11`, `wayland`, `android`
- renderers: `gl3`, `gles3`, `sw` (software, blit through the platform), `wgpu` (`xt_wgpu`, 10 §10.10), `d3d11`, `d3d12`, `vulkan`, `metal`
- backends: `sdl2_gl3`, `sdl2_sw`, `sdl2_wgpu`, `win32_gl3`, `win32_wgpu`, `win32_d3d11`, `glfw_gl3`, `android_gles3`, …

Present (T-439):

| File | Host API | Notes |
|---|---|---|
| `xt_platform_sdl2.*` | SDL 2.0.18+ | events, IME (TEXTEDITING), clipboard, DPI, GL context or native surface handles, window-framebuffer blit |
| `xt_platform_win32.*` | Win32 | message loop, per-monitor DPI v2, WM_CHAR text, clipboard, StretchDIBits blit; IME is a hook only |
| `xt_renderer_wgpu.*` | wgpu-native `webgpu.h` | `WgpuRenderer` on a `WGPUSurface` (Win32 / X11 / Wayland / Metal) or offscreen; `XT_WGPU_BACKEND` / `XT_WGPU_FALLBACK` |
| `xt_renderer_gl3.*` | OpenGL 3.3 core | `GlRenderer` into its own (4x) FBO, blitted to the window |
| `xt_renderer_sw.*` | none | `SwRenderer` into a premultiplied buffer (damage rectangles), presented by the platform |
| `xt_backend_sdl2_{wgpu,gl3,sw}.cpp`, `xt_backend_win32_wgpu.cpp` | | the combinations; `Examples/backends/minimal_sdl2_wgpu` |

Each part is its own static library (`xt_platform_<p>`, `xt_renderer_<r>`, built when its host API is
in the build), and `xt_backend_<p>_<r>` links the pair. An executable links one combination (each
defines `xt::backend::Backend`); `xtframe-view --renderer` links the part libraries instead.

The existing `Code/Runtime/shell/` (SDL2 + GL application shell as a linked library) and
`Examples/engine_integration/` (host-side JSON coexistence) stay where they are; the first backends
are extracted from them, not written from scratch.

## Rules

- A backend file includes only public `xt/...` headers and the host API; no `src/` includes.
- No new third-party dependency: a platform or renderer that needs a library (GLFW, Vulkan SDK)
  finds it through `third_party/versions.cmake` (T-430) and is skipped silently when absent.
- Each file carries the licence header and a top comment naming the host API version it targets.
