# Broadsheet 2026: the standard symbol group

The preset group `broadsheet-2026` of the preset symbol library (`Assets/Symbols/`, see its README;
Docs/design/49-symbol-sets.md 49.2, 49.8): the symbols every UI Project can use without drawing
anything (Docs/design/30-ui-project.md 30.4): 39 symbols in five categories, one `<Name>.xt` symbol
library file each (the touch group in `mobile/`, see "Mobile"; the list-entry reference in `list/`,
see "List"), plus

- `group.json`: id, name, description, source set and version, token and theme files, thumbnail;
- `tokens.xts`: the default design tokens (the implied UI Project's token set);
- `tokens-night.xts`: the same token names with the dark "night desk" values (see "Visual style");
- `index.json`: name, category, description, contract, variants, state groups, ports, timelines,
  triggers, tokens (`{name, default}`), fonts, textures and notes of every symbol (the editor's UI
  Project panel reads it);
- `thumbnails/<Name>.png`: the thumbnail (see "Maintenance"); `thumbnails/group.png` the group's;
- `previews/<Name>.preview.xt` and `<Name>.preview.json` (`{width, height, frames, ports{}, events[]}`):
  the document each thumbnail is rendered from and the size, ports and events it is rendered with;
  `previews/night/` holds the four night-theme previews;
- `CHANGELOG.md`: this edition's changes. A released edition is frozen (49.8): a new look is a new
  group with the next year in its id.

The build copies the whole library next to `xtframe-editor` and `xtframe-pack`
(`<applicationDirPath>/symbols`, this group at `symbols/groups/broadsheet-2026`) and `cmake --install`
puts it in `bin/symbols`; `library.json`'s `default` names this group as the standard one.

## Visual style

The library is drawn as a newspaper: style name **Broadsheet**, v2 "real newsprint"
(Docs/design/47-symbol-visual-style.md 47.13). Grey-white paper (not cream) with a printed paper grain,
ink-black lines and type blocks, hatched tracks and disabled faces, square corners, thin and thick rules,
double rules, dashed "coupon" borders, letterpress edges on primary buttons, and a red outline with a slight
tilt (a rubber stamp) for focus; no blur shadows, gradients or rounded corners. Two theme files declare the
same token names: `tokens.xts` (theme "paper", the default) and `tokens-night.xts` (theme "night desk");
a document links exactly one of them. The tokens added by the style are `--font-display`, `--font-body`,
`--rule-thin`, `--rule-thick`, `--letter-display` and the five texture tokens `--tex-grain`, `--tex-hatch`,
`--tex-dots`, `--tex-rule-h` and `--tex-rule-v` (`--font-family` stays as an alias of `--font-body`;
`--color-shadow` is the letterpress edge colour, not a blur shadow). Noto Serif Display ExtraCondensed Black
and Bold (display) and Noto Serif (body) ship in `fonts/` (SIL OFL 1.1, see `fonts/README.md`); a project's
`shared/tokens.xts` declares them with `@font-face` (the templates and the starter do). `tokens.xts` and
`tokens-night.xts` declare them too (`src: "fonts/..."`, resolved relative to the token file), so linking
either file is enough; without the fonts the lists fall back to platform fonts.

### Textures

`textures/` holds ten small PNG files, all made by `Tools/Scripts/gen_symbol_textures.py` (deterministic;
CC0 like the rest of the library): `grain-paper.png` and `grain-night.png` (128x128 tileable paper grain
baked on the paper colour, +-6 per channel), `hatch-ink.png` (8x8, 45 degree hatch, ink at 18% alpha),
`dots-ink.png` (4x4 halftone dots, 35% alpha), `rule-double.png` (1x7: a 1px ink line, 3px clear, a 1px
ink line, centred in 7px) and `rule-double-v.png` (7x1). The ink textures are drawn in dark ink, so they
only show on the light "paper" theme; `hatch-night.png`, `dots-night.png`, `rule-double-night.png` and
`rule-double-v-night.png` are the same shapes in the light night ink (`#d9d3c4`), and `tokens-night.xts` points
`--tex-hatch`, `--tex-dots`, `--tex-rule-h` and `--tex-rule-v` at them. A token file names them with `url("textures/<file>.png")`
(`--tex-grain` points at the grain of its own theme); a symbol paints them with
`background-image: var(--tex-grain); background-repeat: repeat; background-size: <the image size>`, above the
`background-color` (a surface that must turn to ink on hover sets `background-image: none`). A project copies the
`textures/` directory into its `shared/` directory and declares the five tokens in its `shared/tokens.xts`
(`xtframe-pack` looks in `shared.assetRoot`).

## Licence

The whole library (symbols, tokens, index and thumbnails; not the fonts in `fonts/`, which keep their OFL) is dedicated to the public domain
under CC0-1.0 (`LICENSE`, draft pending review): use, change and ship it in any project,
including commercial ones, without attribution. The fonts stay under the SIL OFL 1.1.

## Using a symbol

Link the library file from the project's symbol directory and place instances:

```html
<head>
  <link rel="symbols" href="../../shared/symbols/Button.xt"/>
</head>
<body>
  <use symbol="Button" id="Play" variant:Kind="Secondary" variant:Size="L" slot:label="@menu_play"/>
</body>
```

`xtframe-pack` copies `<Name>.xt` from this library into the project's `shared.symbols` directory the
first time a UI links a missing one there (only the linked ones). A project file of the same name
is never replaced, so a project overrides a standard symbol by keeping its own copy.

The symbols draw only through tokens (`var(--color-...)`, `--font-*`, `--radius-*`, `--space-*`,
`--control-*`, `--motion-*`; see `tokens.xts` and each symbol's `tokens` in index.json). A project
defines the same names in its `shared/tokens.xts` and themes; nothing else changes.

## Conventions

- **Contract.** Every symbol declares `requires="states: ...; slots: ..."`; the compiler rejects
  a symbol whose definition lacks a rule for a required state or a required slot.
- **Structure.** One top element `#Box` inside the instance; the host addresses internal elements
  as `<instance>/<id>` (`Play/Box`).
- **Controlled components.** Value ports are `in`: the host (or a flow) owns the value and sets
  the port. User intent leaves through `event out` ports (`pressed`, `select`, `increase`, ...);
  an emit from a `<repeat>` entry carries the entry index. Toggle and Checkbox write their `value`
  port themselves (`set(value, true|false)` from the input's check / uncheck) and follow a host
  change of `value` through `bind:checked="value"`; text (TextField)
  stays local to the input element.
- **States.** Normal / Hover / Pressed / Focus / Disabled map to the pseudo-classes; custom states
  (Selected, Open, Low, ...) are bound from ports (`bind:state.<Name>`), exclusive ones through a
  state group. Disable an instance with `disabled="true"` on the `<use>` (or
  `setAttribute("disabled", "true")` on the instance root).
- **Bindings** that only move or scale bind transform components (`bind:scale-x` on the
  HealthBar fill, `bind:translate-x` on the CompassStrip), which never relayout; ProgressBar,
  Slider and Crosshair size their parts with width / left / height bindings.
- **Timelines** use the convention names `intro`, `outro`, `hover-in`, `hover-out`, `press`,
  `hit` (28.1.3) and stay inside the motion spec (transform / opacity, at most 500 ms). A
  trigger's `play()` / `stop()` / `seek()` reaches the timeline of its own instance (the host can
  still play one with `playTimeline("<instance id>", "<name>")`); `then` and
  `timeline(<name>).end` stay inside the instance.
- **Broadsheet review points** (47 §47.10, v2 47.13). A new symbol: (1) has no literal colours, and every
  token it reads exists in `tokens.xts` and is listed in `index.json`; (2) has no `border-radius`
  above 0 (except `--radius-full` for things that must be round), no shadow, gradient or blur;
  (3) sets buttons, tabs and titles in `--font-display`, upper case, with `--letter-display`, and all
  other text in `--font-body`; (4) uses only the two rule widths (`--rule-thin`, `--rule-thick`),
  focus as the red stamp outline (`outline: var(--focus-w) solid var(--color-focus)`, tilted `rotate(-1.5deg)` on the
  controls that can take it) and coupon borders as `dashed` only; textures only through the `--tex-*` tokens; (5) follows the state table
  of 47 §47.6 and the motion amounts of §47.7 (no scaling); (6) has been previewed once with
  `tokens.xts` and once with `tokens-night.xts`.
- **Sounds** are cue names (`ui_click`, `ui_toggle`, `ui_tick`, `ui_select`, `ui_open`,
  `ui_tab`, `ui_confirm`, `ui_back`, `ui_notify`) the host maps to its audio events.

## Symbols

| Symbol | Contract | Ports | Timelines |
|---|---|---|---|
| Button | states: Hover, Pressed, Focus, Disabled; slots: label; variants Kind = Primary / Secondary / Ghost; Size = S / M / L | `pressed: event out` | hover-in, hover-out, press |
| Toggle | states: Hover, Focus, Checked, Disabled; slots: label | `value: bool in` | - |
| Checkbox | states: Hover, Focus, Checked, Disabled; slots: label | `value: bool in` | - |
| RadioGroup | states: Hover, Focus, Selected, Disabled | `options: list<struct{label: string}> in`, `value: number in`, `select: event out` | - |
| Slider | states: Hover, Pressed, Focus, Disabled; slots: label | `value: number in` (set by dragging the track), `min: number in`, `max: number in`, `decrease: event out`, `increase: event out` | - |
| Stepper | states: Hover, Pressed, Focus, Disabled; slots: label | `value: number in`, `decrease: event out`, `increase: event out` | - |
| Dropdown | states: Hover, Focus, Open, Selected, Disabled; slots: label | `options: list<struct{label: string}> in`, `value: number in`, `current: string in`, `select: event out` | - |
| OptionCycler | states: Hover, Focus, Disabled; slots: label | `current: string in`, `prev: event out`, `next: event out` | - |
| TextField | states: Hover, Focus, Disabled; slots: label | `submit: event out` | - |
| Tabs | states: Hover, Focus, Tab1, Tab2, Tab3, Tab4; slots: tab1, tab2, tab3, tab4; group Tab: Tab1 / Tab2 / Tab3 / Tab4 | `value: number in`, `select: event out` | - |
| Tooltip | states: Hover, Focus; slots: anchor, tip | - | - |
| Modal | slots: title, body, actions | `dismiss: event out` | intro, outro |
| Toast | states: Hover | `toasts: list<struct{title: string, text: string}> in`, `dismiss: event out` | item-in, item-out |
| ProgressBar | states: Running, Complete; slots: label; group Progress: Running / Complete | `value: number in`, `value_max: number in` | - |
| CooldownRing | states: Ready, Cooling; slots: icon; group Cooldown: Ready / Cooling | `ready: bool in` | cooldown |
| HealthBar | states: Normal, Low; slots: label; group Level: Normal / Low | `hp: number in`, `hp_max: number in`, `hit: event in` | hit |
| Counter | slots: label, icon | `value: number in`, `gain: event in` | hit |
| Hotbar | states: Hover, Selected, Empty | `slots: list<struct{key: string, count: number, empty: bool}> in`, `selected: number in`, `select: event out` | - |
| Crosshair | states: Idle, Target; group Aim: Idle / Target | `spread: number in`, `on_target: bool in`, `hit: event in` | hit |
| DamageNumber | states: Crit | `amount: number in`, `crit: bool in`, `x: number in`, `y: number in`, `hit: event in` | pop (on the spawned DamagePop) |
| Minimap | slots: label | - | - |
| CompassStrip | slots: marker | `heading: number in` | - |
| Subtitle | states: Character, Narrator; group Voice: Character / Narrator | `speaker: string in`, `line: string in`, `narrator: bool in` | intro, outro |
| Notification | states: Info, Warning, Danger; slots: title, text; group Tone: Info / Warning / Danger | `tone: number in` | intro, outro |
| InputGlyph | slots: glyph, label | - | - |
| ButtonPrompt | slots: text | - | intro |
| Stamp | slots: text | - | - |
Forms and settings: Button, Toggle, Checkbox, RadioGroup, Slider, Stepper, Dropdown,
OptionCycler, TextField, Tabs, Tooltip, Modal, Toast, ProgressBar, CooldownRing. HUD: HealthBar,
Counter, Hotbar, Crosshair, DamageNumber, Minimap, CompassStrip, Subtitle, Notification, Stamp. Input
prompts: InputGlyph, ButtonPrompt. Mobile: see below. `index.json` has a one-line description and
the notes for each.

## Mobile

The `mobile/` subset (Docs/design/23-host-integration.md 23.11.2, category `mobile` in index.json)
is for phones and tablets. Link them like the others (`shared/symbols/<Name>.xt`; `xtframe-pack`
and the editor look in `mobile/` when the library root has no such file, and `xt-standard:<Name>`
resolves there too). Hit areas use the touch tokens `--touch-min` (44 px) and `--touch-lg`
(88 px); bars pad by the safe-area units (`1safe-top`, `1safe-bottom`).
`Examples/demos/mobile-hud/` is a portrait UI Project built from them.

| Symbol | Contract | Ports | Timelines |
|---|---|---|---|
| VirtualJoystick | states: Idle, Active; variant Mode = Fixed / Floating | `axis: vec2 out`, `dead: number in`, `active: bool out` | release |
| TouchButton | states: Hover, Pressed, Disabled, Cooling; slots: icon, label | `pressed: event out`, `held: bool out`, `repeat: event out`, `cooldown: number in` | press, repeat |
| DPad | states: Pressed, Disabled | `dir: event out`, `axis: vec2 out`, `direction: number out` | press |
| SwipeCard | states: Pressed; slots: content, next | `swipe: event out`, `direction: number out`, `offset: number in` | snap-back, fly-left, fly-right, intro |
| BottomSheet | states: Collapsed, Expanded; slots: title, content; group Sheet | `open: bool in` | expand, collapse |
| TabBar | states: Hover, Bar1-Bar5; slots: tab1-tab5; group TabBar; variant Count = 3 / 4 / 5 | `value: number in`, `select: event out` | tab-change |
| TopBar | slots: title, left, right | - | intro |
| PullIndicator | states: Pulling, Ready; slots: label; group Pull | `progress: number in` | spin |
| ToggleTouch | as Toggle | `value: bool in` | flip |
| SliderTouch | as Slider | as Slider | grab |
| PinchZoomView | states: Zoomed; slots: content | `scale: number in`, `min: number in`, `max: number in`, `pinch: event out`, `reset: event out` | zoom |

Toast (root library) is the mobile toast too.

## List

`list/ListItem.xt` (Docs/design/30-ui-project.md 30.13, category `list` in index.json) is the
reference list entry: copy it when you make a list-entry component. It shows each way data reaches
an entry once:

- **values -> in-ports** (the default): `title`, `subtitle`, `selected`. In a `<repeat>` template
  the instance takes them with `bind:<port>` (27-interaction.md 27.3.4), and every entry keeps its
  own copy (`Rows/3/Item.title` for the host);
- **free content -> the slot** `trailing`: a badge, an icon, a button; the slot content belongs to
  the using document, so it binds `item(...)` and document ports itself;
- **states -> `bind:state.<Name>`**: `Selected` follows `selected`; exclusive states go into a
  state group.

A click emits `activate`, a double click also `dblactivate`; from a `<repeat>` entry both carry the
entry index. Link it like the others (`shared/symbols/ListItem.xt`; `xtframe-pack`, the editor and
`xt-standard:ListItem` look in `list/` after the root and `mobile/`). The minimal list is its
preview, `previews/ListItem.preview.xt`:

```html
<div id="List" port:items="list&lt;struct{title: string, subtitle: string, badge: string, selected: bool}&gt; in">
  <repeat port="items" id="Rows">
    <use symbol="ListItem" id="Item" bind:title="item(title)" bind:subtitle="item(subtitle)"
         bind:selected="item(selected)">
      <slot name="trailing"><span id="Badge" bind:text="item(badge)"></span></slot>
    </use>
  </repeat>
</div>
```

| Symbol | Contract | Ports | Timelines |
|---|---|---|---|
| ListItem | states: Hover, Focus, Selected; slots: trailing | `title: string in`, `subtitle: string in`, `selected: bool in`, `activate: event out`, `dblactivate: event out` | - |

## Layout

Each symbol has a default size (a width on `#Box`, or its content size); an instance takes that
size inside flex rows and columns (a block shrinks to its max-content width there).

- `prop:` overrides only change visual and text properties; a UI stylesheet changes an instance's
  size through the internal element's path id, e.g. `#Play/Box { min-width: 240px; }`.

## Known engine gaps and the workarounds used

| Gap | Symbols | Workaround |
|---|---|---|
| A `<repeat>` row cannot clear its siblings' state or pass its index to `set()` | RadioGroup, Hotbar | Selection is bound from the `value` / `selected` port (`bind:state.Selected="index | eq(value)"`); a press emits `select` with the row's index. |
| Rich text and an `@key` cannot share one text node | ButtonPrompt | Localized prompts use InputGlyph (glyph + `@key` label). |
| `draggable` has only `axis-x` (no vertical or 2D drag value) | VirtualJoystick, SwipeCard, BottomSheet | The joystick writes `axis` = (x, 0); the sheet closes on the swipe(down) gesture instead of a drag. |
| Triggers have no pointer position | VirtualJoystick (Floating) | The floating base fades in where it is instead of moving under the finger. |
| The pinch gesture's scale is not an event value | PinchZoomView | `pinch` is emitted and the host writes the new `scale` (an `in` port). |
| A port has no default value and a symbol takes no custom attribute | VirtualJoystick (`dead` 0.15), TouchButton (`repeat-ms`) | The host sets `dead`; the repeat interval is the fixed 0.25 s `repeat` timeline. |
| No `extends` for symbols | ToggleTouch, SliderTouch | Copies of Toggle / Slider with touch sizes. |
| A bound value cannot drive a conic sweep | TouchButton | `cooldown` scales a vertical shade over the face. |

## Maintenance

- Tests: `Tests/symbols` (`Tools/Scripts/dev_check.sh -- symbols`): index consistency (token defaults,
  fonts and textures: `Tools/Scripts/check_symbols_index.sh`), strict compile of
  every preview (`previews/<Name>.preview.xt`), contract enforcement, no literal
  colors, one SW golden per state (the `normal` and `night` ones from `previews/`, the others in
  `Tests/symbols/manifest.txt` with `states/*.events`; `expected/*.png`; `XT_SYMBOLS_UPDATE=1` rewrites
  them), timelines playable, motion spec, and the starter UI Project (`Examples/ui-project-starter`)
  packing with `--strict`. The checks on the group itself are `xtframe-symbols lint Assets/Symbols`
  (codes in `xt/bundle/symbol_lint.h`; `--index-only` is `Tools/Scripts/check_symbols_index.sh`).
- Textures: `python Tools/Scripts/gen_symbol_textures.py` rewrites `textures/` (the output is byte-identical
  on every run); a project's copies are copied from there by hand.
- Thumbnails: `xtframe-symbols thumbnails Assets/Symbols/groups/broadsheet-2026 [--names A,B]` renders
  every `previews/<Name>.preview.json` (the pipeline of the normal-state golden, so a thumbnail is that
  golden pixel for pixel) into `thumbnails/<Name>.png`, then `thumbnails/group.png` (Button, Toggle, Slider and
  Modal in four 160 x 100 cells). A preview's `ports` become `port <name> <value>` lines, then its
  `events` run (`update <seconds>`, `placeholders`; DamageNumber fires its hits with `port` lines),
  then `frames` updates of 1/60 s.
- The skill's `references/tokens.json` is `xtframe-symbols tokens-json Assets/Symbols/groups/broadsheet-2026`
  (`Tools/Scripts/check_skill_extras.sh` compares it).
- A `<link>` resolves by its path only (a document may not link itself), so a preview or UI may
  share its file name with the library it links; the previews are named `<Name>.preview.xt` for
  clarity.
