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
- renderers: `gl3`, `gles3`, `sw` (software, blit through the platform), `d3d11`, `d3d12`, `vulkan`, `metal`
- backends: `sdl2_gl3`, `sdl2_sw`, `win32_gl3`, `win32_d3d11`, `glfw_gl3`, `android_gles3`, …

The existing `Code/Runtime/shell/` (SDL2 + GL application shell as a linked library) and
`Examples/engine_integration/` (host-side JSON coexistence) stay where they are; the first backends
are extracted from them, not written from scratch.

## Rules

- A backend file includes only public `xt/...` headers and the host API; no `src/` includes.
- No new third-party dependency: a platform or renderer that needs a library (GLFW, Vulkan SDK)
  finds it through `third_party/versions.cmake` (T-430) and is skipped silently when absent.
- Each file carries the licence header and a top comment naming the host API version it targets.
