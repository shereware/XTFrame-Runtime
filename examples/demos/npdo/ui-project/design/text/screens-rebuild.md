---
id: screens-rebuild
title: Rebuild brief
section: screens
source:
  kind: ai:design-system
  ref: ref:rebuild/npdo-ui-rebuild-overview
links:
  - rel: derived-from
    to: ref:rebuild/npdo-ui-rebuild-overview
---

# Rebuild brief

The first build of the three screens was captured on 2026-10-18 and then removed, to be rebuilt from this design document. Rebuild from these references; the look and behaviour are the target, the old structure is not.

| What | Reference |
|---|---|
| Project frame, assets, screen flow, lessons learned | `ref:rebuild/npdo-ui-rebuild-overview` |
| Main menu (layers, item hover, intro, ambient idle / rain / fog) | `ref:rebuild/main-menu-spec` |
| Settings (tab strip, six panels, row controls, footer, values) | `ref:rebuild/settings-spec` |
| In-game HUD (five clusters, values, intro) | `ref:rebuild/hud-spec` |
| Screenshots | `ref:screens/main-menu`, `main-menu-intro-0-5s`, `main-menu-intro-1-27s-glitch`, `main-menu-hover-options`, `settings-tab0` to `settings-tab5`, `settings-hover-tab`, `settings-hover-row`, `hud`, `hud-variant-low-hp` |
| Textures (copies of `shared/img`) and their recipe | `ref:assets/*`, `ref:assets/texture-recipe-tex2` |

## Order

1. Main menu first, then settings, then the HUD; wire the screen flow (main menu -> settings / HUD, settings Back -> main menu) as soon as two screens exist.
2. After each screen, check it in the editor's Demo space starting from the main menu, not only on the canvas: the first build looked right on the canvas but not in Demo.
3. Preview-only backdrops are `editor-only`; the published UIs are transparent except the main menu's own key art.
