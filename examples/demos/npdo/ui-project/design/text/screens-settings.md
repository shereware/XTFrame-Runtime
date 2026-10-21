---
id: screens-settings
title: Settings
section: screens
source:
  kind: ai:design-system
applies-to: [ui:settings]
links:
  - rel: evidence-for
    to: text:principles#immediate-feedback
  - rel: derived-from
    to: ref:rebuild/settings-spec
  - rel: derived-from
    to: ref:screens/settings-tab0
---

# Settings

Tab strip (Gameplay, Graphics, Audio, Controls, Accessibility, Microphone) beside a 560 px content panel, footer with Apply (A), Reset to Defaults (R), Back (B / Escape). In the game it sits over whatever is behind it: the key-art background and its shade are `editor-only`, and the tab strip, panel and footer carry their own translucent dark surfaces.

- Every tab has a panel; Audio is the designed one, the others hold placeholder rows (sliders and switches work, choice boxes are static).
- The selected tab and the hovered tab use the same textured band as the main menu.
- Every value is a port with a default; Reset to Defaults writes the defaults back.

Full spec: `ref:rebuild/settings-spec`; screenshots `ref:screens/settings-tab0` to `ref:screens/settings-tab5`.
