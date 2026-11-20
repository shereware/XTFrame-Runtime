# Survey Showcase

A five-screen game UI (1280x720) built only from instances of the **survey-2026** symbol group.
Style: the survey-2026 symbol group (derived from the NPDO demo's look). All copy is generic placeholder
text; the backdrops are generated (`tools/gen_backdrop.py`, fixed seeds).

## Screens

| UI | What it shows |
|---|---|
| `title` | ScreenShade, FogBank, RainLayer, FilmGrain, TitleLockup, MenuItem x6, VersionTag, KeyHint x2 |
| `options` | TitleLockup, SiteCard, SettingsTab x4, four Panels with CyclerRow / SliderRow / ToggleRow / ActionRow / KeyBindRow, footer KeyHints |
| `load` | Panel with SaveSlot x4 (one empty), LoadingIndicator (shown by the `loading` port) |
| `hud` | ObjectiveBanner, LocationClock, VitalBar x2, NoiseMeter, InventorySlot x4, InteractPrompt, PickupNotice, SubtitleLine |
| `pause` | ScreenShade, ChapterCard, ConfirmDialog with two KeyHints (draws over the HUD) |

`shared/bundle.xtflow` holds a `screen` radio (0 title, 1 options, 2 load, 3 hud, 4 pause) and one
`lookup` per UI feeding its `active` in-port. The HUD is active on screens 3 and 4.

## Only survey-2026 symbols

The symbols in `shared/symbols/` are byte-identical copies of the group (registered with their source and
sha256 in `shared/symbols/index.json`). `shared/tokens.xts`, `shared/themes/defiance.xts`, `shared/fonts/` and
`shared/textures/` are copied from the group too. The screens contain no hand-drawn controls: only the backdrop
`<img>` and layout `div`s.

## Run

```
B=bin/linux_x64/Release
$B/xtframe-pack Examples/demos/survey-showcase -o /tmp/survey-bundle --strict
$B/xtframe-view --project Examples/demos/survey-showcase --ui title   # the global flow switches screens
python3 Examples/demos/survey-showcase/tools/gen_backdrop.py     # regenerate shared/img/backdrop*.png
```

Previews: `preview-<ui>.png` (640 px wide; the pause preview is composited over the HUD).
