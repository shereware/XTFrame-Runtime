# lottie_player

Plays a Lottie (bodymovin) `.json` file -- compiled at runtime with
`xt::compile::compileLottie()` -- or an already-compiled `.xtir`, through `xt_shell::App`.
Playback is driven entirely through the public animation API on the document's `defaultAnimation()`
instance (`Document::play/pause/resume/seek`, Docs/design/13-api.md §13.2); this example never
touches the Animator directly beyond reading `localTime()`/`timeline()` for the title bar.

## Build & run

```
lottie_player <file.json|file.xtir> [--headless] [--frames N] [--width W] [--height H]
```

Example:

```
lottie_player ../../Tests/lottie/min_solid_anim.json
```

## Interactive controls

| Key | Action |
|---|---|
| Space | Pause / resume |
| Left / Right | Step one frame back / forward (`seek(localT +/- 1/frameRate)`) |
| R | Reload: recompile/reread the file into a fresh Document, replacing the old one |
| Escape | Quit |

The window title shows the current frame index, time and pause state.

## Headless mode

`--headless --frames N` renders `N` frames off-screen with `xt_sw::SwRenderer` and no window at
all -- useful for CI or any host without a display. `App` creation failing for any other reason
(no display, no GL 3.3, ...) falls back to the same off-screen path automatically, so this example
always exits 0 on a headless machine even without `--headless`.
