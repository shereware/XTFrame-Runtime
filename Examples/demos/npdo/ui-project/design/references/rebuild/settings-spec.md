# Settings: rebuild spec

Screenshots: `ref:screens/settings-tab0` (Audio) to `settings-tab5` (Microphone), `settings-hover-tab` (Graphics hovered), `settings-hover-row` (Music row hovered). The key-art backdrop in the screenshots is preview only. Overview and shared rules: `npdo-ui-rebuild-overview.md`.

Opened from the main menu's Options. A vertical tab strip on the left, a 560 px content panel beside it, title top-left, location block top-right, footer bar with version and key hints. In the game it sits over whatever is behind it; the strip, panel and footer carry their own translucent dark surfaces.

## Layers

| Layer | What | Geometry and style |
|---|---|---|
| settings_bg | `<img>` menu-bg.png, **editor-only** | 0,0 1280 x 720 cover |
| settings_shade | full overlay, **editor-only** | linear-gradient(90deg, #000000e6 0%, #000000b3 45%, #00000059 100%) |
| settings-title | `<img>` menu-title.png | 300 x 100 contain; left safe + 8, top safe + 18 |
| settings-location | three lines: ST. REN HOSPITAL / TAIPEI, TAIWAN / 1991 - 1997 | right safe, top safe + 16, width 220, text-align right; 11 px, letter-spacing 2, line-height 17, `#bdb6a8`; border-right 1 px `#ffffff4d`, padding-right 12 |
| settings-tabs | flex column, gap 4, padding-top 14 | left safe + 4, bottom safe + 62, 186 x 470; linear-gradient(90deg, #0a0a0ad9 0%, #0a0a0a73 100%); border 1 px `#ffffff14` |
| settings-panel | six panels, one shown | left safe + 198, bottom safe + 62, 560 x 470; background `#0a0a0ae0`; border 1 px `#ffffff1f`; padding 24 / 28 |
| settings-footer | bar | left 0, right 0, bottom safe - 18 (flush to the screen bottom), height 52; background `#000000b3`; border-top 1 px `#ffffff14` |
| settings-version | "v1.0.0" in the footer | left safe, top 0, line-height 33; 12 px, letter-spacing 1, `#a39e93` |
| settings-hints | footer, flex row, gap 14, align center | right safe, top 1: [A] Apply, separator, [R] Reset to Defaults, separator, [B] Back |

Root background transparent.

### Tabs

Order top to bottom: Gameplay, Graphics, Audio, Controls, Accessibility, Microphone. Tab index (port `tab`, default 0): Audio 0, Gameplay 1, Graphics 2, Controls 3, Accessibility 4, Microphone 5.

- Tab 186 x 44. Label absolute left 22, line-height 44, 19 px, letter-spacing 1.5, `#bdb6a8`; selected label `#f6f1e6`.
- **Selected band** (shown when `tab` equals the tab's index): absolute 0,0 186 x 44, background tab-hl-N.png stretched (N = 1..6 in strip order), border-left 2 px `#5c2320`.
- **Hover band**: a second copy of the same texture, opacity 0, pointer-events none, plus the tab's `tabhover_*` timeline: band opacity 0 -> 0.6 (160 ms ease-out) and translateX -14 -> 0 (180 ms ease-out); label flickers opacity 1 / 0.35 / 1 / 0.6 / 1 at 0 / 30 / 60 / 100 / 140 ms and shifts x 0 -> 6 px by 180 ms. Leave reverses. No flat hover background.
- Click: `sound(ui_confirm)`, set `tab` to the index, emit `tab_<name>`. Hover sound `ui_hover`.

### Panel content (all panels share this layout)

- Heading 24 px, letter-spacing 2, `#f6f1e6`, weight 400. Description below (margin-top 6) 13 px, letter-spacing 0.5, `#a39e93`. Divider 504 x 1 `#ffffff1f`, margin-top 16, margin-bottom 4.
- **Row** 504 x 40, border-bottom 1 px `#ffffff14` (none on the last row), hover background `#ffffff0a` (140 ms ease-out). Label absolute left 6, line-height 40, 15 px, letter-spacing 1, `#d6d0c2`. Controls start at x 280 inside the row.
- **Slider**: track at left 280, top 19, 176 x 2, `#ffffff26`; fill height 2 `#d6d0c2`, width bound to the value (0-100 -> 0-100 %); knob 6 x 14 `#f6f1e6` at the fill's right end (right -3, top -6). Value at left 466, width 34, right-aligned, 14 px `#e8e2d4`, rounded. A transparent hit area 176 x 24 at (280, 8) is draggable on the x axis and writes the value (drag 0-1 -> 0-100, rounded).
- **Switch**: state text "On" / "Off" at left 280, 14 px `#e8e2d4`; switch at left 316, top 9, 42 x 22, radius 11, `#ffffff40`; knob 16 x 16 radius 8 `#f6f1e6`, at left 23 when on, left 3 when off. Click toggles the bool, `sound(ui_confirm)`.
- **Box** (dropdown or value): left 280, top 6, 220 x 28, border 1 px `#ffffff40` (hover `#efe8d8`, 140 ms), background `#00000066`; text left 12, line-height 26, 13 px `#e8e2d4`. Dropdowns carry a 12 x 12 chevron at right 10, top 7. Choice boxes in the placeholder tabs read "<   Value   >" and are static.
- **Stepper** (Dynamic Range): prev arrow 28 x 28 at left 280, value 160 px wide centred at left 310 (14 px `#f6f1e6`, letter-spacing 1), next arrow at left 472. Click emits `audio_mode_prev` / `audio_mode_next` (`ui_hover` sound).
- **Button** (Play Test Sound): a box 160 wide with centred text, emits `play_test_sound`.

### Panels

| Tab | Description line | Rows |
|---|---|---|
| Audio | Balance the hospital's silence against everything that breaks it. | Master Volume (slider, 80), Music (60), Sound Effects (75), Dialogue (90), Output Device (dropdown "Headphones (Realtek)", emits `open_output_device`), Dynamic Range (stepper "Night Mode"), Subtitles (switch, on, emits `toggle_subtitles`), Test Audio (button) |
| Gameplay | How the world treats you, and how much it forgives. | Difficulty (box "Standard"), Camera Shake (slider 60), Auto-Save (switch on), Tutorial Hints (switch on), Language (box "English") |
| Graphics | Resolution, light and the grain of old film. | Resolution (box "1920 x 1080"), Display Mode (box "Fullscreen"), Brightness (slider 50), VSync (switch on), Film Grain (switch off) |
| Controls | Mouse, keyboard and what each key does. | Mouse Sensitivity (slider 40), Invert Y Axis (switch off), Move Forward (box "W"), Interact ("E"), Flashlight ("F") |
| Accessibility | Make the dark easier to read. | Subtitle Size (box "Medium"), Colour Blind Mode (box "Off"), High Contrast (switch off), Text Size (slider 50) |
| Microphone | Input device and voice chat. | Input Device (box "Default Microphone"), Input Volume (slider 70), Voice Chat (switch on), Push to Talk (switch off) |

Audio is the designed tab; the other five are working placeholders (sliders and switches work, boxes are static).

### Footer hints

Hint chip: flex row, gap 10, height 32, padding 0 8, hover background `#ffffff14` (140 ms). Key caps as on the main menu; hint text 14 px, letter-spacing 1, `#d6d0c2`. Separators 1 x 22 `#ffffff33`.

- Apply: click or key A, emits `apply_settings`.
- Reset to Defaults: click or key R, writes every value back to its default (table below), emits `reset_defaults`.
- Back: click, key B or Escape, `sound(ui_back)`, emits `back` -> main menu.

## Values (ports, in, with defaults)

`master_volume` 80, `music_volume` 60, `sfx_volume` 75, `dialogue_volume` 90, `output_device` "Headphones (Realtek)", `audio_mode` "Night Mode", `subtitles` true, `gp_camera_shake` 60, `gp_autosave` true, `gp_hints` true, `gfx_brightness` 50, `gfx_vsync` true, `gfx_grain` false, `ctl_sensitivity` 40, `ctl_invert_y` false, `acc_contrast` false, `acc_text_size` 50, `mic_input_volume` 70, `mic_voice_chat` true, `mic_push_to_talk` false, `tab` 0.

Events out: `tab_gameplay`, `tab_graphics`, `tab_audio`, `tab_controls`, `tab_accessibility`, `tab_microphone`, `audio_mode_prev`, `audio_mode_next`, `open_output_device`, `toggle_subtitles`, `play_test_sound`, `apply_settings`, `reset_defaults`, `back`.

Not built: cycling Dynamic Range values, the Output Device list, real dropdowns in the placeholder tabs.

## Motion

`intro` when shown: tab strip opacity 0 -> 1 over 0-500 ms, panel 150-650 ms, ease-out. Hover responses as above (140-180 ms).
