# AGENTS.md

This directory is an XTFrame UI Project (docs/design/30-ui-project.md). Read this file first.

## Layout

- `<name>.xtui` (here `XTFrame Game UI demo.xtui`): project identity (name, game, version), `spec` (platforms and resolutions,
  safe-area rules, input methods, languages, performance budget, target engine), the UI list
  (`uis`), the shared resources (`shared`) and the publish settings (`publish`).
- `ui/<name>/`: each UI's `.xt`, `.xts`, `.xtflow`, `.xtanim.json` and `.xteditor.json`.
- `shared/`: symbols, styles, `tokens.xts`, themes, images, fonts, animations, video, effects,
  string tables (`strings/<lang>.csv`) and motion recipes.
- `conventions/`: layout templates, naming rules, state and timeline conventions.
- `references/`: moodboards, reference screenshots (with licenses), decision records.
- `contracts/`: scenario contracts, tag dictionary references, `motion-spec.yaml`.
- `manifest/`: `assets.yaml` (asset list, measured sizes, approval status, provenance).

## Rules

- Use design tokens (`var(--...)`) instead of literal colors, sizes and durations.
- Stay inside `contracts/motion-spec.yaml`.
- Follow the layouts and naming in `conventions/`.
- Assets you create are `draft`; only a person marks them `approved`.
- A UI root fills the viewport; anchor key elements to the safe area (`safe-*` units, 25 §25.4.1).
  A fixed px root is lint `layout.fixedRoot`; the Editor Service's `repair.anchor` converts it.
- `references/`, `contracts/` and `shared/recipes/` never ship in the bundle.
