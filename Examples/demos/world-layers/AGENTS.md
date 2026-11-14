# AGENTS.md

This directory is an XTFrame UI Project (Docs/design/30-ui-project.md) with one UI, `stage`: a
1280x720 screen built to be shown **layer by layer in 3D** (Docs/design/33-world-layers.md). Five
elements carry `world-layer: on` -- `#Grid`, `#Rings`, `#Panel`, `#Tags`, `#Fore` -- each at its own
depth (`translateZ` 30 / 90 / 160 / 230 / 300 px); `#Panel` is also turned (`rotateY(-22deg)`).
Everything else (`#Wall`, `#Caption`) is the base layer.

- In an ordinary (flat) view the switch is ignored and `#Stage`'s `perspective: 2000px` projects the
  same transforms into one picture.
- In world-layer output (`xtframe-view --layers` / `--layers-view`, a host's UI volume) the
  perspective is not used: every layer goes out as its own texture with its plane's transform, and
  the host's camera does the rest.

Three loops show what costs a redraw and what does not:

| Loop | What changes | Cost in world-layer output |
|---|---|---|
| `#RingDash` turns (`rotate`, 18 s) | content of the `#Rings` layer | that one layer is redrawn every frame |
| `#Tags` floats along z and flips (`transform: translateZ() rotateY()`, 6 s) | the layer's own transform | none: only its `world3` changes |
| `#Fore` pulses (`opacity`, 2.4 s) | the layer's own opacity | none: only its `opacity` changes |

Keep these properties when editing:

- The decorative layers carry `pointer-events: none`. A layer's box takes the pointer like any
  element, so a large layer in front would otherwise swallow every ray aimed at `#Engage`.
- `translateZ()` in a `@keyframes` stop is the layer's depth, not an offset: `tags-float` moves
  `#Tags` between 230 px (its resting depth in `stage.layout.xts`) and 280 px.
- `world-layer` is a style property (it goes in `stage.xts`, not in `stage.layout.xts`).
- Tests read this project: `Tests/unit/test_world_layers.cpp` expects the five ids above, their
  depths, the panel's 22 degree turn and `#Engage` at (32, 292) 376x56 inside `#Panel`.

## Layout

- `World Layers demo.xtui`: project identity, `spec` (desktop 1280x720 / 1920x1080, `logicalHeight`
  720) and the UI list.
- `stage/stage.xt`: the document; `stage/stage.layout.xts`: geometry and depths;
  `stage/stage.xts`: colours, the `world-layer` switches and the three loops.

## Look at it

```
bin/win_x64/Debug/xtframe-view.exe Examples/demos/world-layers/stage/stage.xt --width 1280 --height 720 --layers-view
bin/win_x64/Debug/xtframe-view.exe Examples/demos/world-layers/stage/stage.xt --width 1280 --height 720 --layers out/world-layers
```

The first opens a window on the layers standing in space (right-drag turns the camera, the wheel
changes the depth scale, the left button reaches the UI through the camera). The second writes
`layer-<order>-<id>.png`, `layers.json` and `preview.png` into `out/world-layers`.
