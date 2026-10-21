---
id: "0003"
title: Traditional Chinese is the source language; add a CJK serif
status: accepted
source:
  kind: ai:design-system
  ref: ref key art
approved:
  by: Bigspring
  at: 2026-10-16T04:02:23Z
links:
  - rel: derived-from
    to: ref:key-art/af3f1d8f-b44b-402c-8232-d975d9c177a8
  - rel: derived-from
    to: ref:key-art/d5b9776d-3c07-4237-835b-f79ffc258b9c
---

Context: both key arts are set in Traditional Chinese (國有財產處置員, 聖仁醫院, 現地勘查紀錄, the four-line tagline, signage and the 1991-1997 blurb) with English as a secondary, widely tracked line. The UI is currently English only and uses Georgia, which has no CJK glyphs.
Decision (proposed): treat zh-TW as the source language and en as the translation. Add a Traditional Chinese serif role (cjk-serif) and a font fallback chain.
Consequence: spec.languages and spec.fontFallback must be set by a person; label columns (about 280 px) must be checked with Chinese strings; every screen string needs a zh-TW entry.
