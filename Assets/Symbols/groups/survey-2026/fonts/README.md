# Bundled fonts

Noto Serif (Regular 400, Bold 700, Italic 400) for the Survey style (`--font-display`, `--font-body`),
under the SIL Open Font License 1.1 (`OFL-NotoSerif.txt`). These are byte-identical copies of the files in
`Assets/Symbols/groups/broadsheet-2026/fonts/` (see the README there for the upstream source and the
static instances' SHA-256).

The fonts are not part of this group's CC0 dedication and stay under the OFL in every copy.
`tokens.xts` and `tokens-defiance.xts` declare them with `@font-face` (`src` relative to the token file).
A host that previews a group file directly must use the group directory as the asset root
(`xtframe-view --root Assets/Symbols/groups/survey-2026`).
