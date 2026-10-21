# Main menu: rebuild spec

Screenshots: `ref:screens/main-menu` (settled), `main-menu-intro-0-5s`, `main-menu-intro-1-27s-glitch`, `main-menu-hover-options`. Overview and shared rules: `npdo-ui-rebuild-overview.md`.

Entry screen. Full-bleed key art of the hospital at dusk in the rain; title top-left; a six-item list on the left; version bottom-left; key hints bottom-right. The scene stays alive (rain, fog, a flickering street lamp, a slow camera push) while the chrome stays quiet.

## Layers, bottom to top

| # | Layer | What | Geometry and style |
|---|---|---|---|
| 1 | menu-bg | `<img>` menu-bg.png | 0,0 1280 x 720, object-fit cover. Shipped art (not editor-only) |
| 2 | menu_lamp | `<img>` menu-lamp.png | left 412, top 82, 320 x 320; mix-blend-mode screen; opacity 0.8; pointer-events none. Sits on the street lamp |
| 3 | menu_fog | div, background menu-fog.png repeat-x, size 1280 x 320 | left 0, top 420, 2560 x 320; opacity 0.6; pointer-events none |
| 4 | menu-shade-left | full overlay | linear-gradient(90deg, #000000eb 0%, #000000b8 30%, transparent 62%) |
| 5 | menu-shade-bottom | full overlay | linear-gradient(0deg, #000000bf 0%, transparent 26%) |
| 6 | menu_grime | `<img>` menu-grime.png | 0,0 1280 x 720 cover; opacity 0.5; transform scale(1.15); pointer-events none |
| 7 | menu_rain | div, background menu-rain.png repeat, size 512 x 512 | left 0, top -512, 1280 x 1232; opacity 0.32; pointer-events none |
| 8 | menu-title | `<img>` menu-title.png | 462 x 154, object-fit contain; left safe + 12, top safe + 66 |
| 9 | menu-list | flex column, gap 6 | left safe + 32, top 41.11 % (about 296 px), width 300 |
| 10 | menu-version | "v1.0.0" | left safe, bottom safe; 12 px, letter-spacing 1, #a39e93 |
| 11 | menu-hints | flex row, gap 10, align center, justify flex-end | right safe, bottom safe, width 260. Keys W S + "Select" (margin-right 18, nowrap), key E + "Confirm" |

Root background `#050505`.

### Menu items

Six items, each 300 x 42, `position: relative`: New Game, Continue, Load Game, Options, Credits, Quit.

- **Band** (absolute 0,0 300 x 42): opacity 0 at rest; background menu-hl-N.png (N = item 1..6) stretched 100 % x 100 %, no repeat; border-left 2 px `#5c2320`; opacity transition 160 ms ease-out.
- **Label**: absolute left 26, height and line-height 42; 23 px, letter-spacing 2, colour `#d6d0c2` (New Game was imported a step brighter, `#f6f1e6`); colour transition 160 ms ease-out.
- **Hover**: band opacity 1, label `#f6f1e6`, plus the item's `hoverN` timeline:
  - band translateX -22 -> 0 over 180 ms ease-out (slides in from the left);
  - label flickers like a failing bulb, opacity 1 -> 0.35 (30 ms) -> 1 (60) -> 0.6 (100) -> 1 (140), and shifts translateX 0 -> 9 px by 180 ms ease-out.
  - Leave plays the same timeline in reverse.
- **Click**: `sound(ui_confirm)` (Quit: `ui_back`), then emit the item's port.
- Hover sound `ui_hover`.

### Key caps (shared look with settings)

22 x 22, line-height 20, centred, 12 px, border 1 px `#e8e2d4b3`, radius 3, text `#e8e2d4`. Hint text 14 px, letter-spacing 1, `#d6d0c2`.

## Motion

Started when the screen loads (and `intro` again whenever it is shown): `intro` once, `rain` and `fog` looping. When `intro` ends, `idle` loops.

- **intro**
  - menu-bg: opacity 0 -> 1, 0 to 1.2 s, ease-out.
  - menu-title: at 0.3 s opacity 0 and scale 1.07; opacity reaches 1 at 0.9 s (ease-out); scale settles to 1 at 1.2 s (ease-out). Then a glitch: 1.24 s opacity 0.45 and x -6; 1.29 s opacity 1 and x +5; 1.34 s opacity 0.7 and x -2; 1.39 s opacity 1 and x 0.
  - menu-list: opacity 0 -> 1 from 0.9 to 1.6 s, ease-out.
- **idle** (24 s loop, ease-in-out unless noted)
  - menu-bg: slow push, scale 1 -> 1.045 and x 0 -> -14 at 12 s, back at 24 s.
  - menu_lamp follows the push (x -17, y -5, scale 1.045 at 12 s) and flickers: opacity 0.8, then 0.25 / 0.8 / 0.4 / 0.8 at 6.45 / 6.52 / 6.6 / 6.7 s, then 0.1 / 0.8 / 0.3 / 0.8 at 17.05 / 17.12 / 17.24 / 17.32 s.
  - menu-shade-left: opacity 1 -> 0.93 by 1 s; at 17.05-17.32 s it flicks 1 / 0.93 / 1 / 0.93 in step with the lamp (the room darkens when the lamp dies); back to 1 at 24 s.
  - menu-shade-bottom: breathes 1 -> 0.82 at 6 s -> 1 at 12 s -> 0.82 at 18 s -> 1 at 24 s.
  - menu_grime: opacity 0.45 -> 0.7 at 8 s -> 0.45 at 16 s; drifts x -45 and y -30 at 12 s, back at 24 s (scale stays 1.15).
  - menu-title: scale 1.012 at 6 s and 18 s, 1 at 12 s and 24 s; a short flicker at 13.94-14.11 s (0.6 / 1 / 0.8 / 1).
- **rain**: menu_rain translateY 0 -> 512 over 0.5 s, linear, loop.
- **fog**: menu_fog translateX 0 -> -1280 over 64 s, linear, loop.

## Interface

- Ports (events out): `new_game`, `continue_game`, `load_game`, `open_options`, `open_credits`, `quit_game`.
- New Game and Continue open the HUD; Options opens settings; Load Game, Credits and Quit only emit.
- Key hints show W / S select and E confirm; keyboard / gamepad focus navigation was not built (hover only).
