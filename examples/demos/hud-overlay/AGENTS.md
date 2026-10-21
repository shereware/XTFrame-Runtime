# AGENTS.md

This directory is an XTFrame UI Project (docs/design/30-ui-project.md) with one UI, `hud`: a
1920x1080 in-game HUD with no background, for checking how translucent panels, backdrop blur and
blend modes look over a game frame. The UI root paints nothing; the host composites it with
straight alpha (23 §23.z). `references/game-shot.png` is a made-up game frame
(`references/make_images.py`, standard library only) placed under the HUD as an `editor-only`
`<img>` (03 §3.13): the editor canvas shows it, the Demo space and the published bundle never
contain it, so do not give it ids that a trigger, a binding or a flow needs. `shared/minimap.png`
comes from the same script. The host drives the ports `hp`, `stamina`, `ammo` (numbers, 0-100 for
the bars) and `damage` (an event that flashes the vignette); on show the UI seeds them and loops
the 8 s `demo` timeline, whose markers `hit` (1 s) and `heal` (4.5 s) set the same ports, so the
HUD moves without a host. Keep every panel translucent and keep the root without a background;
`references/` never ships in the bundle.

## Layout

- `HUD Overlay demo.xtui`: project identity, `spec` (desktop 1920x1080 / 2560x1440 / 1280x720,
  `logicalHeight` 1080, `uiscaleRule` height), the UI list and the publish settings.
- `hud/hud.xt`, `hud/hud.xts`: the document and its stylesheet.
- `shared/tokens.xts`: the three tokens the stylesheet uses (each `var()` carries the same value as
  its fallback, so `hud/hud.xt` also renders right when opened on its own).
- `shared/minimap.png`: the minimap texture.
- `references/`: the editor-only reference frame and the script that draws both pictures.
