# In-game HUD: rebuild spec

Screenshots: `ref:screens/hud` (defaults), `hud-variant-low-hp` (hp 22, noise High, no interact prompt). The corridor image is preview only. Overview and shared rules: `npdo-ui-rebuild-overview.md`.

Minimal survival HUD over the game view, transparent in the game. Five clusters in the corners and on the right edge; nothing in the centre.

## Layers

| Layer | What | Geometry and style |
|---|---|---|
| hud_preview_bg | `<img>` hud-preview-bg.png, **editor-only** | 0,0 1280 x 720 cover |
| hud_vignette | full overlay, **editor-only** | radial-gradient(70.71 % 70.71 % at 50 % 50 %, transparent 55 %, #00000099 100 %) |
| objective | top-left block | left safe + 40, top safe + 34, 280 x 52 |
| location | top-right block | right safe + 32, top safe + 34, width 260, text-align right |
| interact-prompt | right edge | left 100 % - safe - 154, top 58.33 %; flex row gap 10, align center; padding 4 12 4 4; background `#00000073` |
| vitals | bottom-left, flex column gap 2 | left safe + 28, bottom safe, width 300 |
| inventory | bottom-right, flex row gap 26 | right safe + 32, bottom safe + 22 |

Root background transparent.

### Objective

- Rule: 2 x 52 `#efe8d8` at the left edge.
- "Objective:" at left 16, line-height 22, 14 px, letter-spacing 2, `#d6d0c2`.
- Line 220 x 1 `#ffffff33` at (16, 25).
- Objective text at (16, 28), line-height 24, 16 px, letter-spacing 1.5, `#f6f1e6` (bound to `objective`).

### Location

- Name 14 px, letter-spacing 1.5, line-height 22, `#e8e2d4` (bound to `location`).
- Line 260 x 1 `#ffffff33`, margin 3 0 4.
- Clock 13 px, letter-spacing 2, line-height 20, `#bdb6a8`, 24-hour HH:MM (bound to `clock`).

### Interact prompt

Key cap 24 x 24, border 1 px `#e8e2d4`, radius 3, 13 px `#f6f1e6` ("E"); label 15 px, letter-spacing 1.5, `#f6f1e6` (bound to `interact_label`). Shown only when `interact_visible`.

### Vitals (three rows, each 300 x 34)

- Icon 20 x 20 at (0, 7), line icons stroked `#e8e2d4` 1.4: pulse line (health), running figure (stamina), microphone (noise).
- Label at left 34, line-height 34, 11 px, letter-spacing 2, uppercase, `#d6d0c2`: HEALTH, STAMINA, NOISE.
- Bar (health, stamina): track at (112, 14), 120 x 6, `#ffffff1a`, border 1 px `#ffffff26`, overflow hidden; fill height 4 `#e8e2d4`, width = value 0-100 -> 0-100 %.
- Noise: a waveform 120 x 20 at (112, 7) (sage `#8fbf8a` spikes, then a dashed rest line `#ffffff40`), value text at left 244, 13 px, letter-spacing 1, sage `#8fbf8a` (bound to `noise_label`).

### Inventory (four slots)

- Slot 62 x 70, border 1 px `#ffffff40`, background `#0000008c`. The active slot (slot 1) has background `#ffffff1a` and border 2 px `#efe8d8`, and its number badge is inverted (background `#efe8d8`, text `#0a0a0a`).
- Number badge 14 x 14 at (3, 3), 10 px, border 1 px `#e8e2d499`.
- Icon 36 x 36 at (13, 19), flat vector in the cream / stone ramp.
- Caption under the slot: left -12, top 76, width 86, centred, 11 px, letter-spacing 0.5, `#d6d0c2`.
- Placeholder items: 1 Flashlight, 2 Crowbar, 3 Clipboard, 4 East Wing Key. The story bible's items (Junk, Crowbar, Glock, MP5, a "memento") may replace them; Route 2 weapons would need an ammo readout.

## Values (ports, in, with defaults)

`objective` "Reach the Main Entrance", `location` "St. Ren Hospital Grounds", `clock` "17:42", `hp` 64, `stamina` 78, `noise_label` "Low", `interact_label` "Open Gate", `interact_visible` true. Defaults are needed so the HUD is complete before the game sends values.

## Motion

`intro` when shown, opacity 0 -> 1, ease-out: objective 0-800 ms, location 200-1000 ms, vitals 300-1100 ms, interact prompt 400-900 ms, inventory 500-1200 ms.
