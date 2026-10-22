# Standard symbol library

The symbols every UI Project can use without drawing anything (Docs/design/30-ui-project.md
30.4): 38 symbols in five groups, one `<Name>.xt` symbol library file each (the touch group in
`mobile/`, see "Mobile"; the list-entry reference in `list/`, see "List"), plus

- `tokens.xts`: the default design tokens (the implied UI Project's token set);
- `index.json`: name, category, description, contract, variants, state groups, ports, timelines,
  triggers, tokens and notes of every symbol (the editor's UI Project panel reads it);
- `<Name>.png`: the thumbnail (see "Maintenance").

The build copies this directory next to `xtframe-editor` and `xtframe-pack`
(`<applicationDirPath>/symbols`) and `cmake --install` puts it in `bin/symbols`.

## Licence

The whole library (symbols, tokens, index and thumbnails) is dedicated to the public domain
under CC0-1.0 (`LICENSE`, draft pending review): use, change and ship it in any project,
including commercial ones, without attribution.

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
Forms and settings: Button, Toggle, Checkbox, RadioGroup, Slider, Stepper, Dropdown,
OptionCycler, TextField, Tabs, Tooltip, Modal, Toast, ProgressBar, CooldownRing. HUD: HealthBar,
Counter, Hotbar, Crosshair, DamageNumber, Minimap, CompassStrip, Subtitle, Notification. Input
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
preview, `Tests/symbols/fixtures/ListItem.preview.xt`:

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

- Tests: `Tests/symbols` (`Tools/Scripts/dev_check.sh -- symbols`): index consistency, strict compile of
  every preview (`Tests/symbols/fixtures/<Name>.preview.xt`), contract enforcement, no literal
  colors, one SW golden per state (`Tests/symbols/manifest.txt`, `states/*.events`,
  `expected/*.png`; `XT_SYMBOLS_UPDATE=1` rewrites them), timelines playable, motion spec, and the
  starter UI Project (`Examples/ui-project-starter`) packing with `--strict`.
- Thumbnails: the preview rendered with `xtframe-view`, e.g.
  `xtframe-view Tests/symbols/fixtures/Slider.preview.xt --headless --frames 30 --width 300
  --height 110 --port Volume.min=0 --port Volume.max=100 --port Volume.value=35 --screenshot
  Assets/Symbols/Slider.png` (the size and the ports of each preview are those of its `normal`
  state in `Tests/symbols/manifest.txt` and `states/<Name>-normal.events`). DamageNumber fires
  its hits with `--event "1 port Dmg.hit 1"` (after `amount` / `x` / `y` / `crit`) and
  `--frames 10`, since a spawned number lives 0.45 s.
- A `<link>` resolves by its path only (a document may not link itself), so a preview or UI may
  share its file name with the library it links; the previews are named `<Name>.preview.xt` for
  clarity.
