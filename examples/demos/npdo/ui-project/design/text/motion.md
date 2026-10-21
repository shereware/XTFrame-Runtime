---
id: motion
title: Motion
section: motion
status: approved
source:
  kind: ai:design-system
  ref: main-menu.xts, hud.xts, settings.xts timelines and transitions
approved:
  by: Bigspring
  at: 2026-10-16T04:02:23Z
---

# Motion

Only opacity is animated. All curves are ease-out.

## Entrance

Every UI plays an `intro` timeline on show. Main menu: background 0-1200 ms, title 300-1300 ms, list 900-1600 ms. HUD: five clusters staggered by 100-200 ms (objective 0, location 200, vitals 300, prompt 400, inventory 500 ms), each fading over about 500-800 ms. Settings: tab strip 0-500 ms, panel 150-650 ms. The motion lint expects a 40-80 ms stagger; the HUD uses 100-200 ms (see lint motion.entrance.stagger).

## Hover

Menu items and hint chips respond in 140-160 ms (opacity or colour), tabs and rows 140-160 ms (background colour).
