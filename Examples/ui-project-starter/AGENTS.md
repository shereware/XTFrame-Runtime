# AGENTS.md

This directory is an XTFrame UI Project (docs/design/30-ui-project.md): a starter with three UIs
(`hud`, `main-menu`, `settings`) built only from the standard symbol library
(`editor/symbols`, 30.4). Read this file first.

## Layout

- `<name>.xtui` (here `Starter UI Project.xtui`): project identity, `spec` (1280x720 and 1920x1080, mouse and gamepad, English
  and Traditional Chinese, budget), the UI list (`uis`), the shared resources (`shared`) and the
  publish settings (`publish.outDir` = `out`).
- `ui/<name>/<name>.xt` + `.xts`: each UI. The `.xts` holds layout only; every color, size step
  and duration comes from a token.
- `shared/tokens.xts`: the base token set ("Daylight"). It is `shared.tokens`, so xtframe-pack and
  the editor load it before every UI; a UI must not `<link>` it again.
- `shared/themes/dark.xts`: the dark theme (token values only).
- `shared/strings/en.csv`, `shared/strings/zh-TW.csv`: the string tables; every visible text in
  the UIs is an `@key`.
- `shared/symbols/`: project symbols. A UI links a standard symbol as
  `<link rel="symbols" href="../../shared/symbols/<Name>.xt"/>`; when that file is missing,
  xtframe-pack copies it from the standard library (a project file of the same name overrides it).
- `conventions/layouts/{hud,main-menu,inventory}.xt`: layout templates (anchors from
  `docs/reference/ui-survey-2026-09`). Start a new screen from one of them.
- `contracts/motion-spec.yaml`: the motion spec (29.6). `references/`, `manifest/`: empty.

## Building

```
xtframe-pack examples/ui-project-starter            # writes out/ (bundle), copies used symbols
xtframe-view examples/ui-project-starter/out/ui/hud.xtir --root examples/ui-project-starter/out
```

## Rules

- Use tokens (`var(--...)`); literal colors are not allowed anywhere but `shared/tokens.xts` and
  the themes.
- Compose screens from standard symbols (`<use symbol>`); fill slots, pick variants, bind ports.
  `prop:` overrides may only change visual and text properties (26.2.1).
- Symbols are controlled components: value ports are `in` (the host or a flow sets them), user
  intent leaves through `event out` ports (`pressed`, `select`, `increase`, ...). See
  `editor/symbols/README.md` for every symbol's contract.
- Timeline names follow the convention `intro`, `outro`, `hover-in`, `hover-out`, `press`, `hit`.
- Stay inside `contracts/motion-spec.yaml`: animate transform and opacity, keep UI motion at or
  under 500 ms, one ambient loop per HUD.
- Keep floating elements inside the safe margin (`--safe-x`, `--safe-y`).
- Assets you create are `draft`; only a person marks them `approved`.
- A UI root fills the viewport; anchor key elements to the safe area (`safe-*` units, 25 §25.4.1).
  A fixed px root is lint `layout.fixedRoot`; the Editor Service's `repair.anchor` converts it.
- `references/`, `contracts/` and `shared/recipes/` never ship in the bundle.
