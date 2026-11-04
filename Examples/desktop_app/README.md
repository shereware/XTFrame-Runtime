# desktop_app

A small interactive `xt_shell::App` host: a `+1` button that updates a counter, a text input whose
`Change` event is echoed to another element, a checkbox that toggles a `dark` class on the
document root, and a `:hover` transition on the button. See `ui/app.xt` / `ui/app.xts` for the
document and stylesheet, and `main.cpp`'s `wireUi()` for the event listeners
(`Docs/design/08-events.md` §8.3, `Docs/design/13-api.md` §13.2-§13.3).

## Build & run

```
desktop_app [--headless] [--watch] [--frames N] [--width W] [--height H]
```

Without `--headless` this opens a real window and runs until it is closed (Escape, or the
window's close button). Click `+1` to increment the counter, type into the text field and press
Tab/Enter (or click elsewhere) to see the echo update, and check the checkbox to toggle dark mode.

With `--headless` -- or on any machine where `xt::shell::App` cannot create a window/GL context,
e.g. no display -- this falls back to driving a plain `xt::Context` for `--frames` update/render
iterations with no window at all. The fallback still exercises the same UI wiring by simulating a
button click and a checkbox toggle through `Context::pushInput()` (a `PointerDown`/`PointerUp`
pair at the target element's `boundingBox()` center -- exactly what a real pointer click looks
like to the event dispatcher) and a text entry through `ElementRef::setValue()`.

## Edit the `.xt`, save, see it

```
desktop_app --watch
```

With `--watch` the window keeps an eye on the UI's sources -- `ui/app.xt`, `ui/app.xts`,
`ui/app.xtflow` and any other `.xt` / `.xts` / `.xtcl` / `.xtflow` under `ui/` -- by polling their
modification times every 500 ms. Edit a style or a layer, save, and the window shows the change
within half a second: the example recompiles the UI with `xt::compile::compileDocument()` and swaps
it in with `Context::hotReload()`, which keeps port values and element states (13 §13.8.3), then
wires its listeners again. When the save does not compile, the diagnostics are printed to stderr in
the same `file:line:col: error: message` form as `xtframe-compile`, and the previous version stays
on screen until the next save that compiles.

A host gets the same behaviour by linking `xt_shell_watch` (`xt_shell` plus the compiler; a
shipped host links `xt_shell` alone and has no compiler) and installing a `SourceWatcher` on its
`xt::shell::App` after loading its UI:

```cpp
#include "xt/shell/source_watcher.h"

xt::shell::SourceWatcher watcher;  // declared before the App: it must outlive it
// ... create the App, load the UI as `handle` ...
watcher.setReloadedHandler([&](xt::DocumentHandle h) { /* re-wire listeners */ });
watcher.watch("ui/app.xt", handle, options);  // the CompileOptions it compiled with
xt::shell::installWatch(app, watcher);         // polled once per frame, before onFrame
```

`installWatch()` is a thin wrapper over the generic `App::setFramePoll()` hook.
`watcher.addPath(dir)` adds a directory outside the UI's own (a shared symbol library).
`--watch` applies to the window mode; the `--headless` run ignores it.

## Two ways to load the UI

This example compiles `ui/app.xt` to an in-memory `.xtir` blob at startup
(`xt::compile::compileDocument()`), so it never requires a separate build step. A host that wants
to ship pre-compiled UI instead runs the CLI once ahead of time:

```
xtframe-compile ui/app.xt -o app.xtir
```

and then loads the resulting file with `Context::loadDocumentFile("app.xtir")` /
`App::context().loadDocumentFile(...)` in place of this example's `compileUi()` step -- everything
else (event wiring, the main loop) stays the same.
