# Survey 2026: the dark game-UI symbol group

The preset group `survey-2026` of the preset symbol library (`Assets/Symbols/`, see its README;
Docs/design/55-survey-symbol-group.md). The style is warm monochrome chrome over scene
art, smoke-dark highlight bands with a thin blood-red edge, translucent dark panels and ruled form rows. Cream text on near-black, one red that only ever means something. The canvas is
1280x720 logical pixels. Style derived from the NPDO demo (Examples/demos/npdo). The group is not the library default (`library.json` keeps `broadsheet-2026`).

## Themes

Two token files declare the same names: `tokens.xts` (Dusk, the default) and `tokens-defiance.xts`
(Defiance: warmer, a red focus colour, quicker motion). A document links exactly one of them.

## Categories and symbols

Categories: `scene` (scene layers), `menu` (menus and flow), `form` (settings), `hud` (HUD), `overlay` (overlays).
Further symbols are added by later tasks; this table lists what the group holds now.

| Symbol | Category | Description |
|---|---|---|
| MenuItem | menu | Main-menu entry: a grime band slides in behind the label on hover, focus and selection; emits `pressed`. |
| Panel | form | Translucent settings panel: heading, one-line description, a rule and a body slot for rows. |
| SaveSlot | menu | Load-game entry: chapter, location and timestamp with the grime band for hover and selection; Empty variant; emits `pressed`. |
| ChapterCard | menu | Centred chapter title card: chapter number, title, a red rule and a place/date line; fades in and out. |
| LoadingIndicator | menu | Loading block: a label that breathes, a hairline progress bar for `progress` 0-100 and a tip line. |
| PickupNotice | overlay | Pickup/journal notice: a cream mark, a small-caps heading and the item name; slides in on show, out on hide. |
| ConfirmDialog | overlay | Modal confirm: strong scrim, scratched dark panel with title, rule, body and an actions slot for key hints; Escape emits `cancel`. |
| SubtitleLine | overlay | Subtitle line on a soft scrim: small-caps speaker over a centred line; Narrator variant drops the speaker and italicises. |
| SettingsTab | form | Settings category tab with the grime band for hover and selection; emits `pressed`. |
| SliderRow | form | Settings row with a hairline slider and its numeric value; drag or nav keys change `value`. |
| ToggleRow | form | Settings row with On/Off text and a pill switch bound to `on`; emits `toggled`. |
| CyclerRow | form | Settings row with a boxed `< value >` picker; arrows and Left/Right emit `prev`/`next`. |
| KeyBindRow | form | Settings row showing a bound key; `listening` swaps it for a pulsing prompt; click or Enter emits `rebind`. |
| ActionRow | form | Settings row with a boxed action button (e.g. Play Test Sound); emits `pressed`. |
| ScreenShade | scene | Full-bleed darkening over scene art: a vignette, a left fade for menus or a bottom fade for subtitles. |
| RainLayer | scene | Tiled rain streaks falling in a seamless 0.5 s loop; Light or Heavy. |
| FilmGrain | scene | Animated film grain overlay that jitters in a 0.5 s loop. |
| FogBank | scene | Low fog bank tiled across the bottom of a scene; Low or High. |
| BloodEdge | scene | Red damage edge whose opacity follows `level` (0-1); pulses at 0.6 and above. |
| TitleLockup | menu | Game title set in live type with a thin red slash and a tracked subtitle; fades in on show. |
| KeyHint | menu | Footer key hint: one or two keycaps and a label, optional divider before it; clickable, emits `pressed`. |
| VersionTag | menu | Small build/version caption for a screen corner. |
| SiteCard | menu | Right-aligned three-line site plaque (place, city, years) with a hairline rule. |
| ObjectiveBanner | hud | Top-left objective block: a rule, the `Objective:` label and the current objective; replays a fade when it changes. |
| LocationClock | hud | Top-right location name over a rule and a 24-hour clock. |
| VitalBar | hud | HUD vital: line icon, small-caps label and a thin bar for `value` 0-100; turns red below 30; `hit` flashes it. |
| NoiseMeter | hud | HUD noise meter: mic icon, label, a bar waveform and a level word, sage/amber/red for `level` 0/1/2. |
| InteractPrompt | hud | Interaction prompt: keycap and action label on a soft scrim, shown by `visible`. |
| InventorySlot | hud | Quick-slot cell with a number badge, a line-art item icon (Flashlight, Crowbar, Clipboard, Key or Empty) and a caption; `selected` frames it. |
| FocusDot | hud | Minimal centre aim dot; `target` adds a ring for something interactable. |

## Mixing with Broadsheet

Both preset groups can be used in one project and in one document. Symbol names never clash (55.7), and since
T-738 a state group that clashes with another symbol's group is kept inside its symbol instead of failing the
compile (56 56.4). Survey declares the whole core token vocabulary (47, "core vocabulary") with its own values,
and a project has one style (56 56.2): every symbol, whichever group it came from, reads the project's
`shared/tokens.xts`. So a Survey symbol dropped into a Broadsheet project draws with Broadsheet's values for the
shared names; apply the Survey style to the project to switch all of them. Survey's own extension tokens
(`--color-text-strong`, `--size-row`, `--tex-band`, `--tex-film-grain`, ...) do not clash with Broadsheet's names.

## Maintenance

- Index: `python3 Tools/Scripts/gen_symbols_index.py --dir Assets/Symbols/groups/survey-2026` writes the derived
  fields of `index.json` (tokens, fonts, textures, content hashes); `bash Tools/Scripts/check_symbols_index.sh`
  checks every group of the library.
- Thumbnails: `xtframe-symbols thumbnails Assets/Symbols/groups/survey-2026` renders `previews/` into `thumbnails/`;
  `xtframe-symbols lint Assets/Symbols/groups/survey-2026` must report 0 errors and 0 warnings.
- Textures: `python3 Tools/Scripts/gen_survey_textures.py` rewrites `textures/` (a development-time generator that
  needs Pillow; the PNGs are committed and no build or gate runs it; byte-identical on every run):
  it copies the four original textures (`band-dusk`, `band-short-dusk`, `rain`, `fog`), derives the two Defiance bands
  with a fixed warm tint, and generates `grain` and `scratch` with a fixed seed.

## Licence

Symbols, tokens, index, textures (four copied from the NPDO example and two tinted from them; their source images were made by the XTFrame author, who authorised their inclusion; grain and scratch are procedural) and thumbnails are dedicated to the public domain under CC0-1.0 (`LICENSE`, draft
pending review). The fonts in `fonts/` (Noto Serif) keep their SIL OFL 1.1.
