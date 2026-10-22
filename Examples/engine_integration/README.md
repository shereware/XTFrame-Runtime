# engine_integration

Shows how a host engine integrates XTFrame directly, without `xt_shell` (no SDL2, no GL). This is
the pattern for embedding XTFrame's core (`xt_core`) inside an existing engine that already owns
its window, input and renderer.

## Which headers a host may include

From 0.3 on these headers are the host contract: within a major version they are added to, never
changed (`Docs/design/23-host-integration.md` §23.6). The core rows all come from the single
umbrella `#include "xt/xt.h"`; the non-core ones are included only by a host that uses them.

| Header | Contents |
|---|---|
| `xt/context.h` | `Context`, `ContextConfig`, `DocumentHandle`, `DamageList` |
| `xt/document.h`, `xt/element_ref.h` | `Document`'s public methods, `ElementRef`, event listeners |
| `xt/input.h` | `InputEvent`, `KeyCode`, `PointerButton`, `Cursor` |
| `xt/asset_resolver.h` | `AssetResolver`, `FileAssetResolver`, `ImageData` |
| `xt/text_shaper.h` | the `TextShaper` interface (a host supplying its own fonts implements `addFontMemory`) |
| `xt/command_list.h` | `CommandList` (for a host-written backend) |
| `xt/perf.h` | `PerfTracker`, `FrameStats` |
| `xt/sw/sw_renderer.h` | `SwRenderer`, `SwTarget` |
| `xt/bundle/bundle.h` | `Bundle`, `loadUi`, `reloadUi` |
| `xt/flow/flow_runtime.h` | `FlowRuntime` (for a host that runs flow graphs) |

None of these includes a private header (`*_internal.h`, anything under a module's `src/`) or
exposes a third-party type. `Tools/Scripts/check_public_headers.sh` enforces that, and that this example
stays on the list -- it includes `xt/xt.h` plus the three non-core headers above. Its four other
`xt/` includes (`xt/compile/compile.h`, `xt/sw/sw_png.h`, `xt/command_simplifier.h` and, under
`XT_BUILD_TEXT`, `xt/text/ft_text_shaper.h`) are example scaffolding, not part of the contract: a
host that ships pre-compiled `.xtir`, draws with its own backend and supplies its own shaper needs
none of them.

## Build & run

```
engine_integration --file <doc.xt|doc.xtir> [--headless] [--frames N] [--width W] [--height H] [--simplify]
```

Example (from the build directory):

```
engine_integration --file ../../Tests/xt/doc_basic.xt --headless --frames 10
```

`--headless` is accepted for command-line consistency with the other examples but has no effect
here -- this example never opens a window.

## Per-interval performance baseline (`--perf-every N`)

`--perf-every N` (either form) replaces the per-frame command tally with one line every N frames:

```
perf frame=<n> update=<ms> render=<ms> top=[sw:span 1.2, sw:layer 0.8, sw:path 0.3] damage=<%> ws=<MB>
```

`update`/`render` are the interval's average `Context::update()` and `SwRenderer::render()` times,
`top` lists the three largest SW render sub-phases (`FrameStats::sw*`, averaged), `damage` is the
share of the texture the host uploaded over the interval, and `ws` is the process working set
(`GetProcessMemoryInfo` on Windows, `/proc/self/statm` on Linux, 0 elsewhere). A host that sees its
frame time drift can run the same UI here: a flat `render`/`ws` column means the cost is fixed
and lives in the UI, a rising one means something accumulates per frame.

```
engine_integration --bundle ./my_bundle --ui hud --frames 3000 --width 1920 --height 1080 --perf-every 100
```

## Loading from a bundle (hot reload)

```
engine_integration --bundle <dir> --ui <id> [--watch] [--headless] [--frames N] [--width W] [--height H]
```

`--bundle` takes a published bundle directory (the output of `xtframe-pack`, see
[`Docs/design/22-bundle.md`](../../Docs/design/22-bundle.md) §22.3-§22.5) and replaces `--file`; the two
cannot be combined. The example then:

1. opens `<dir>/manifest.json` with `xt::bundle::Bundle::open()` and builds its `Context` with
   `ContextConfig.assets = &bundle.assets()` -- a packed UI references its images by their
   deduplicated `assets/<hash>.<ext>` name, so it must resolve them through the bundle;
2. loads the UI with `xt::bundle::loadUi(ctx, bundle, id)`;
3. runs the UI's `.xtflow` graph, when the manifest entry lists one, with `xt::flow::FlowRuntime`
   (`tick()` once per frame after `Context::update()`).

With `--watch`, `Bundle::reload()` is polled every 30 frames (a real host polls on a timer, ~0.5 s
is plenty). It returns the ids whose `.xtir` changed -- i.e. what the editor just re-packed -- and
for this UI the example calls `xt::bundle::reloadUi()` (same `DocumentHandle`, same viewport and
clock, a brand-new Document) followed by `FlowRuntime::rebind()`, printing
`reloaded <id> (<n> warnings)`; a warning names a connection point whose element the new document
no longer has. Everything the host cached from the old Document -- `Document*`, `ElementRef`,
`IrView*`, listener ids -- is invalid after a reload and has to be re-fetched, as the frame loop
in `main.cpp` does for `doc`/`irView`.

Example (from the build directory):

```
engine_integration --bundle ./my_bundle --ui hud --watch --frames 600
```

## Integration steps

The order is the one a host follows, and matches the header list in
[`Docs/design/23-host-integration.md`](../../Docs/design/23-host-integration.md) §23.6 -- those
headers are the frozen host contract, and this example includes nothing else.

1. **Texture + target** (`xt/sw/sw_renderer.h`). The *host* owns the pixels. This example's
   `HostTexture` stands in for the engine's texture object and keeps an RGBA8 buffer next to it;
   `xt::sw::SwTarget` simply points at that buffer, and `SwRenderer::setTarget()` is called once,
   not per frame -- a partial redraw has to find the previous frame's pixels still there.
2. **Load IR** (`xt/context.h`). `.xt` source is compiled at runtime with
   `xt::compile::compileDocument()` (Docs/design/12-compiler.md §12.2-§12.3) so a host never has to
   run a separate CLI step first; an already-compiled `.xtir` file is read directly with
   `Context::loadDocumentFile()`, and a published bundle with `xt::bundle::loadUi()`
   (`xt/bundle/bundle.h`). Either way the result is an opaque `DocumentHandle` owned by the
   `Context`.
3. **Viewport** (`xt/context.h`). `Context::setViewport(Rect logical, float devicePixelRatio)`
   tells every loaded Document how to lay out and where the Painter's coordinate space (in
   `CommandList::viewport`) sits. A viewport or dpr change makes the next frame fully dirty.
4. **Fonts / assets** (`xt/text_shaper.h`, `xt/asset_resolver.h`). A real host normally provides
   its own `xt::TextShaper` wrapping its engine's font/text system (Docs/design/07-text.md §7.2)
   and its own `xt::AssetResolver` wrapping its asset pipeline. This example stands in for both
   with the bundled FreeType shaper over the platform's system fonts and `xt::FileAssetResolver`
   with `xt::sw::decodeImageBytes` as the image decoder.
5. **Input** (`xt/input.h`). `Context::pushInput(const InputEvent&)` queues host input events; the
   next `Context::update()` drains and dispatches them into the foreground Document (hit testing,
   hover/active state, bubbling) before the Document's own per-frame update
   (Docs/design/08-events.md §8.1-§8.4). This example pushes a
   `PointerMove`/`PointerDown`/`PointerUp` sequence once, before the frame loop.
6. **update() / render()** (`xt/context.h`, `xt/command_list.h`). Each frame:
   `Context::update(dt)` advances animations/transitions and re-resolves style/layout as needed;
   `Context::render(CommandList&, DamageList*)` paints every loaded Document into the list and
   reports what changed on screen. A `.xtflow` graph, when the UI ships one, is ticked right after
   `update()` (`xt/flow/flow_runtime.h`).
7. **CommandList -> backend.** The engine's own backend (`MyBackend` in `main.cpp`) walks
   `list.commands` with a `switch` over every `xt::CmdKind` -- see
   [`Docs/design/09-command-list.md`](../../Docs/design/09-command-list.md) for the full command
   semantics, and its §9.5 table for the minimal subset (`FillPath`, `PushClipRect`/`PopClip`,
   `DrawGlyphs`, `DrawImage`) an integrator must support plus the allowed degradations for the
   rest. `xt::CommandListSimplifier` (enabled here with `--simplify`) performs exactly those
   degradations automatically, so an engine that cannot implement gradients/layers/masks/clip
   paths/strokes can run every document through it first and only ever see the minimal subset.
8. **Upload** (`xt/damage.h`). Only the dirty rectangles go to the texture -- see below.
9. **Profiling** (`xt/perf.h`). `Context::perf()` is off by default; this example turns it on and
   prints one `PerfSummary` line every 120 frames, the shape a real engine would feed into its own
   profiler HUD.

For a visible artifact, this example *also* rasterizes each frame's `CommandList` off-screen with
`xt::sw::SwRenderer` (a real engine would not normally also link `xt_sw`) and writes the last
frame -- read back from `HostTexture`, so a region the host failed to upload would show up as a
stale patch -- to `engine_integration_out.png` next to wherever the example is run from.

## Partial redraw

`Context::render(list, &damage)` fills an `xt::DamageList` (Docs/design/23-host-integration.md
§23.2): up to 16 device-pixel rectangles covering everything that changed since the previous frame,
or `full == true` for "assume everything changed" (the first frame, a viewport/dpr change, a
reload, or damage too fragmented to be worth tracking).

Two things follow from it, and `main.cpp`'s frame loop does both:

```cpp
// NOTE: `list` still holds the previous frame's commands -- that is what render() diffs
// against, and it clears the list itself. Do not clear it here.
ctx.render(list, &damage);
if (damage.full) {
    renderer.clear(clearColor);
    renderer.render(list, &shaper, &assets, irView, &ctx.perf());
    texture.uploadRegion(fullRect, pixels.data(), strideBytes);
} else {
    renderer.clearRects(clearColor, damage.rects);
    renderer.render(list, &shaper, &assets, irView, &ctx.perf(), &damage);
    for (const xt::Rect& r : damage.rects) {
        texture.uploadRegion(r, pixels.data(), strideBytes);
    }
}
```

- **Rasterize less.** `SwRenderer::render()`'s trailing `damage` argument turns the union of the
  rectangles into a write mask on the target: pixels outside it keep their value, and a draw
  command that lands entirely outside every rectangle is skipped. Layer, mask and effect buffers
  are still computed in full (a blur reads its neighborhood); only their composite back onto the
  target is masked. The result inside the rectangles is byte-identical to a full redraw, which is
  what `xt_golden`'s `partial_*` cases check. `SwRenderer::render()` never clears by itself, so the
  host clears first -- `clearRects()` for the partial path, `clear()` for a full one.
- **Upload less.** `HostTexture::uploadRegion()` stands in for the engine's sub-image upload
  (`glTexSubImage2D`, `UpdateSubresource`, ...). Every 120 frames the example prints
  `upload: avg <n>% of frame, <n> regions/frame` -- for a HUD where only a clock and an animated
  bar move, that stays well under half the texture.

A host that wants none of this simply passes no `DamageList` and keeps calling `clear()` +
`render(list, shaper, assets, ir)`; the GL backend ignores `damage` entirely (§23.3).
