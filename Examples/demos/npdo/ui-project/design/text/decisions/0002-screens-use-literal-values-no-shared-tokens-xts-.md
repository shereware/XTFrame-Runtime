---
id: "0002"
title: Screens use literal values; no shared/tokens.xts yet
status: accepted
source:
  kind: ai:design-system
  ref: design.audit
approved:
  by: Bigspring
  at: 2026-10-16T04:02:23Z
---

Context: the three UIs were imported from the Claude Design board. shared/tokens.xts does not exist, so every colour, size and font is a literal in base.xts or the per-UI .xts (token coverage 0).
Decision: record the palette and type roles as knowledge only. Do not rewrite UI values in this pass.
Consequence: converting literals to variables is a separate restyle step the user can approve; see the report's restyle suggestions.
