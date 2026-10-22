# NPDO UI: rebuild overview

Snapshot of the first build of the three NPDO screens (main menu, settings, in-game HUD), taken on 2026-10-18 before they were deleted. It is the brief for rebuilding them. Per-screen specs: `main-menu-spec.md`, `settings-spec.md`, `hud-spec.md` (same reference group). Screenshots: `ref:screens/*` PNGs. Texture files: `ref:assets/*`.

What to keep is the **look and behaviour** shown here. The structure, layer names and wiring may be redone cleanly; the first build carried importer leftovers (hashed ids such as `n-3181d383`, `s-xxxxxx` classes, literal colours instead of tokens).

## Project frame

- UI Project `NPDO` (`npdo`), three UIs: `main-menu`, `settings`, `hud`, each 1280 x 720 logical, 16:9.
- Safe area 2.5 % (32 px left/right, 18 px top/bottom at 1280 x 720); console 5 %. Every anchored element sits inside it.
- One font: Georgia, 'Times New Roman', serif, weight 400. Text colour default `#e8e2d4` (cream).
- Root of each UI: 1280 x 720, `overflow: hidden`, `position: relative`. In the game the roots are transparent: background art and shades that exist only for previewing are marked `editor-only`.
- The design system (palette, type, principles, voice, motion, glossary, story bible) is in this `design/` folder and still applies. Literal colours used by the screens map to the palette swatches: `#050505` void, `#0a0a0a` ink, `#f6f1e6` cream-bright, `#e8e2d4` cream, `#efe8d8` cream-accent, `#d6d0c2` cream-mid, `#bdb6a8` cream-dim, `#a39e93` stone, `#8fbf8a` sage, `#5c2320` the muted blood marker (hover / selected edge).

## Shared assets (`shared/img`, copies in `ref:assets`)

| File | Size | Used for |
|---|---|---|
| menu-bg.png | 1672 x 941 | intro key art: main-menu background; settings preview backdrop (editor-only) |
| menu-title.png | 2172 x 724, alpha | NPDO logo with "National Property Disposal Officer" |
| hud-preview-bg.png | 1536 x 1024 | in-game corridor still, HUD preview backdrop only (editor-only) |
| menu-hl-1..6.png | 600 x 84 (2x of 300 x 42) | main-menu hover bands, one per item |
| tab-hl-1..6.png | 372 x 88 (2x of 186 x 44) | settings tab selected / hover bands, one per tab |
| menu-grime.png | 1280 x 720 | dark stains at the frame edges, text column clean |
| menu-rain.png | 512 x 512 | tileable rain streaks |
| menu-fog.png | 1280 x 320 | horizontally tileable low fog |
| menu-lamp.png | 320 x 320 | warm glow for the street lamp in the key art |

The bands, grime, rain, fog and lamp were generated from a black-and-white concrete stain texture sheet by `texture-recipe-tex2.txt` (a Python script, in `ref:assets`; the source sheet was a black-and-white concrete stain collage): smoky dark band with feathered edges, faint warm cracks and a dried-blood stain at the marker end, so the cream label gains contrast on hover instead of losing it.

## Screen flow (cross-UI)

| From | Action | To |
|---|---|---|
| (game start) | | main menu |
| main menu | New Game / Continue | HUD |
| main menu | Options | settings |
| settings | Back (B, Escape, click) | main menu |
| main menu | Load Game, Credits, Quit | events only, no screen yet |

- Only one screen is visible at a time. The screen that opens replays its `intro`.
- Each UI's own logic (defaults, tabs, sliders, toggles, its ambient loops) stays in that UI; only "which screen is shown" is cross-UI (global flow `shared/bundle.xtflow`).
- Sounds named by the triggers: `ui_hover`, `ui_confirm`, `ui_back` (cues, no audio files yet).

## Lessons from the first build (XTFrame, themixUI as of 2026-10-18)

These cost time last round; check them first.

1. **Demo did not show settings / HUD correctly and this was not solved.** The editor's Demo space (T-356/T-362) runs the whole project (all UIs + global flow) but draws **one** UI and follows the last UI whose surface received `show`. The first build switched screens with `setStyle visibility` on the root divs (children with their own visibility leak through, Demo does not follow), then with an `active` port and `<body on:port(active)="if eq(1) show() else hide()">`. That worked in `xtframe-view --project` but the user reported Demo still wrong. Verify each screen in the editor's Demo early, starting from the main menu, before building detail.
2. **Ports need default values** (`port:hp="number in = 64"`). Runtime, Demo and host start from the declared defaults; without them texts are empty and bars at 0, while the canvas shows the static placeholder text, so the two disagree.
3. `bind:visible` only toggles `visibility` (layout space kept, children with their own visibility still show). To remove a panel use a custom state with `display: none` (`bind:state.Off="tab | ne(2)"`) or `bind:display`.
4. `show` triggers fire on `<body>` and its direct children (T-349). Do not start the intro from `on:show` if you edit on the canvas: the canvas runs real time and every edit recompiles, so the intro replays and the screen blanks for a second. The first build started intros and ambient loops from each UI's flow (one-shot `timer` 0.05 s -> `timeline.play`), since flows run only in Demo and in the game.
5. `.xts` has no `filter` / `text-shadow` (use `effect:` / `text-effect:`); background images need a quoted relative URL `url("../shared/img/x.png")`.
6. Editor-only preview art: `editor-only` on the backdrop `<img>` and on any preview-only shade, root `background-color: transparent`, so the published UI is transparent.
7. The motion lint expects 40-80 ms stagger and duration tokens; the ambient loops (idle 24 s, fog 64 s) and the 150-300 ms staggers were deliberate and reported as violations.
