---
id: voice
title: Voice
section: voice
links:
  - rel: derived-from
    to: ref:story/npdo-story-zh
---

# Voice

How the UI speaks: tone, capitalisation, numbers and localisation notes.

## Tone

Procedural and understated, like a government survey that has met something it cannot file. Short declarative lines ending in a full stop, one emphasised word at the end. Atmospheric lines are one sentence ("Balance the hospital's silence against everything that breaks it."); actions are plain verbs. This is the Route 1 register. Route 2 may break into defiant, darkly comic exclamation ("Rise up! Let your rage burn!"), but system UI (menus, settings, prompts) keeps the Route 1 voice in both routes.

## Item descriptions

First person, the officer speaking; wry and deadpan. The mundane is described with over-precise, almost clinical detail (the Glock as "a primer, a copper case, a cone of special alloy and smokeless powder"), and the punchline comes last. Wordplay on 處分 (disposal) is house style. An item's line can change with the route (Junk), so item text is per route. Puns that do not survive translation (嘴砲, "mouth cannon") carry a localisation note; see `ref:story/npdo-story-en`.

## Vocabulary

Official language over game language: survey (勘查) rather than explore, disposal (處分) rather than kill or clear, case, site, date, surveyor. Place and organisation names follow `text:glossary`.

## Capitalisation

English: Title Case for menu items, tabs and buttons (New Game, Load Game, Reset to Defaults). UPPERCASE for HUD labels and the location block. Sentence case for descriptions and prompts. Chinese has no case; use wide tracking for titles and labels instead.

## Numbers

Volumes are integers 0-100 (rounded). The HUD clock is 24-hour HH:MM. Versions read vMAJOR.MINOR.PATCH. Years use a spaced hyphen (1991 - 1997). Bars carry no unit.

## Localisation

The story bible is written in Traditional Chinese (`ref:story/npdo-story-zh`), so zh-TW is the source language and English is a translation (`ref:story/npdo-story-en`; decision:0003). The UI is English only today and spec.languages is empty. Georgia has no CJK glyphs; a Traditional Chinese serif is needed (type:cjk-serif). Check label columns (about 280 px) and the HUD clusters with Chinese strings.
