# XTFrame Runtime

XTFrame is an MIT-licensed C++20 library for UI documents and vector animation. This repository is
the **runtime**: the libraries a game engine or application links, the command-line tools a build
server needs, their tests and the public design chapters. The authoring editor is a separate
product and is not part of this repository.

> **Read-only snapshot.** This repository is exported from the XTFrame main repository by
> `tools/export_runtime.sh`; every commit is one snapshot (`Source-Commit:` trailer). Pull
> requests cannot be merged here directly; see [`CONTRIBUTING.md`](CONTRIBUTING.md).

- An offline compiler turns an HTML/CSS subset (`.xt` / `.xts`) and Lottie JSON into a compact
  binary intermediate representation (`.xtir`).
- The runtime core, `xt_core`, has **zero third-party dependencies**, no exceptions and no RTTI: it
  loads IR, runs layout, style and the timeline/animation system, and produces a backend-agnostic
  draw **command list**.
- A host engine renders the command list with its own pipeline, or uses one of the reference
  backends: a software rasterizer (`xt_sw`) and an OpenGL 3.3 backend (`xt_gl`). `xt_shell` is an
  SDL2 window and main loop for standalone desktop apps.

## Modules

```
core/      xt_core     IR loading, scene graph, properties, timeline, style, layout, events,
                       command-list output (no third-party dependencies)
compile/   xt_compile  .xt / .xts / Lottie JSON / SVG parsers -> IR
flow/      xt_flow     node-graph data logic that ships with a bundle (.xtflow)
bundle/    xt_bundle   UI Project -> Bundle packing and Bundle loading
text/      xt_text     reference FontProvider / TextShaper (FreeType, HarfBuzz)
sw/        xt_sw       software rasterizer backend
gpu/       xt_gpu      device-neutral GPU frame builder
gl/        xt_gl       OpenGL 3.3 core-profile backend
shell/     xt_shell    SDL2 window + main loop for standalone desktop apps
platforms/android/     Android viewer app (SW presentation)
tools/
  xtframe-compile      .xt / .xts / Lottie JSON -> .xtir
  xtframe-pack         UI Project -> Bundle
  xtframe-view         standalone player (window or headless, screenshots, scripted events)
  xtframe-gpu-probe    GPU frame builder diagnostics
tests/                 doctest unit tests, golden images, fuzz targets
examples/              engine_integration, desktop_app, lottie_player, demos, ui-project-starter
docs/design/           design chapters (the specification; Traditional Chinese)
```

`editor/symbols/` is the standard symbol library that `xtframe-pack` copies next to itself and the
symbol tests read; it is data, not editor code.

## Building

Requires CMake 3.24 or newer and a C++20 compiler. Third-party code is vendored under
`third_party/` or fetched at configure time with `FetchContent` at pinned versions (SDL2, FreeType,
HarfBuzz); nothing has to be installed system-wide.

Windows (Visual Studio 2022):

```
cmake --preset vs2022
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

Linux / macOS (Ninja):

```
cmake --preset ninja-gcc-debug        # or ninja-clang-debug, ninja-*-release
cmake --build --preset ninja-gcc-debug
ctest --preset ninja-gcc-debug
```

| Option | Default | Effect |
|---|---|---|
| `XT_BUILD_GL` | `ON` | OpenGL 3.3 backend (`xt_gl`) |
| `XT_BUILD_GPU` | `ON` | device-neutral GPU frame builder (`xt_gpu`; required by `xt_gl`) |
| `XT_BUILD_SHELL` | `ON` | SDL2 shell (`xt_shell`) |
| `XT_SHELL_GL` | `ON` | `xt_shell` presents through `xt_gl` (`OFF`: `xt_sw` + SDL_Renderer) |
| `XT_BUILD_TEXT` | `ON` | `xt_text` (fetches FreeType and HarfBuzz) |
| `XT_BUILD_TOOLS` | `ON` | the `xtframe-*` command-line tools |
| `XT_BUILD_TESTS` | `ON` | the test suites |
| `XT_BUILD_EXAMPLES` | `ON` | the `examples/` programs |
| `XT_BUILD_FUZZ` | `OFF` | libFuzzer targets (Clang only) |
| `XT_RUNTIME_OUTPUT_DIR` | empty | output directory of every `xtframe-*` executable |

`xt_core` alone needs none of the optional modules; every option adds a module on top of it.

## Embedding in a host engine

A host that owns its render loop needs `xt_core` (plus `xt_compile` to load source instead of
compiled `.xtir`). It drives `xt::Context` and walks the resulting `xt::CommandList` with its own
renderer:

```cpp
#include <xt/xt.h>

xt::ContextConfig config{};              // null shaper / assets -> built-in defaults
xt::Context ctx(config);
xt::Result<xt::DocumentHandle> doc = ctx.loadDocumentFile("ui/app.xtir");

ctx.setViewport(xt::Rect{0, 0, 1280, 720}, /*devicePixelRatio=*/1.0f);
ctx.update(dt);                          // seconds since the previous frame

xt::CommandList commands;
ctx.render(commands);
for (const xt::Command& cmd : commands.commands) {
    // switch (cmd.kind) { case xt::CmdKind::FillPath: ... }  -- issue your own draw calls
}
```

The host owns the pixel buffer, the clock (`Context::update(dt)`), input (`Context::pushInput()`),
assets (`AssetResolver`) and fonts (`TextShaper`). `Context::render()` can also fill an
`xt::DamageList` so only the changed rectangles are redrawn and uploaded.

- [`examples/engine_integration/`](examples/engine_integration/) is a complete, runnable host:
  it compiles or loads a document or a Bundle, walks every `xt::CmdKind`, and rasterizes the same
  list with `xt_sw` for a reference image.
- [`docs/design/23-host-integration.md`](docs/design/23-host-integration.md) is the host contract,
  [`docs/design/09-command-list.md`](docs/design/09-command-list.md) the command list,
  [`docs/design/22-bundle.md`](docs/design/22-bundle.md) the Bundle format,
  [`docs/design/02-ir-format.md`](docs/design/02-ir-format.md) the IR and
  [`docs/design/43-compatibility-and-versions.md`](docs/design/43-compatibility-and-versions.md)
  the compatibility and version promises. Start at
  [`docs/design/00-overview.md`](docs/design/00-overview.md).

## Command-line tools

```
xtframe-compile <input.json | input.xt> -o <out.xtir> [--root DIR] [--embed-images]
xtframe-pack    <project-dir | name.xtui | file.xt> [-o <out-dir>] [--ui <id>]... [--strict] [--no-thumbs]
xtframe-view    <file.xtir | file.xt | file.json | bundle | project> [--headless --frames N] [--screenshot PNG]
```

Run a tool without arguments to print its full usage (`xtframe-view --help` as well).

## License

MIT. See [`LICENSE`](LICENSE). Third-party components and their licenses are listed in
[`third_party/NOTICE.md`](third_party/NOTICE.md).

---

Exported from XTFrame 41a3a79246ad (2026-10-21, version 0.9.0).
