---
id: screens-hud
title: In-game HUD
section: screens
source:
  kind: ai:design-system
applies-to: [ui:hud]
links:
  - rel: evidence-for
    to: text:principles#readable-at-a-glance
  - rel: evidence-for
    to: text:principles#respect-the-safe-area
  - rel: evidence-for
    to: text:principles#warm-monochrome-one-red
  - rel: derived-from
    to: ref:rebuild/hud-spec
  - rel: derived-from
    to: ref:screens/hud
---

# In-game HUD

Minimal survival HUD over the game view. In the game it is transparent: the preview background and the corner vignette are `editor-only`.

- Top left: objective. Top right: location and clock. Right middle: interact prompt.
- Bottom left: health, stamina and noise meters. Bottom right: four inventory slots.
- Values: objective, location, clock, hp, stamina, noise_label, interact_label, interact_visible, all with defaults.

## Fit with the story

- The stamina and noise meters carry Route 1 (hold your breath, hide your fear): noise is the most important reading after health.
- The inventory placeholders (Flashlight, Crowbar, Clipboard, East Wing Key) only partly match the story bible's items (unlock items, Junk, Crowbar, Glock, MP5, a "memento"). Route 2 weapons will need an ammo or charge readout that the HUD does not have yet.
- Location names should use the two buildings: Surgical Building and Internal Medicine Building (`text:glossary`).

Full spec: `ref:rebuild/hud-spec`; screenshots `ref:screens/hud`, `ref:screens/hud-variant-low-hp`.
