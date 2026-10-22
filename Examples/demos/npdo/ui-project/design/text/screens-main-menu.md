---
id: screens-main-menu
title: Main menu
section: screens
source:
  kind: ai:design-system
applies-to: [ui:main-menu]
links:
  - rel: evidence-for
    to: text:principles#quiet-until-needed
  - rel: derived-from
    to: ref:rebuild/main-menu-spec
  - rel: derived-from
    to: ref:screens/main-menu
---

# Main menu

Entry screen over the intro key art with the game title and a six-item list (New Game, Continue, Load Game, Options, Credits, Quit).

- Left and bottom shades keep the list readable over the art; rain, low fog, a flickering street lamp and edge grime keep the scene alive.
- Items fade in last (900-1600 ms) after a title glitch at about 1.25 s; hover slides in a textured smoky band with a muted blood-red edge and makes the label flicker.
- Ports: new_game, continue_game, load_game, open_options, open_credits, quit_game (events out).
- Key hints bottom right: W / S select, E confirm.

Full spec: `ref:rebuild/main-menu-spec`; screenshots `ref:screens/main-menu`, `ref:screens/main-menu-hover-options`.
