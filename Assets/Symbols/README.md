# Preset symbol library

This directory is XTFrame's **preset symbol library** (Docs/design/49-symbol-sets.md 49.2): a
library is any directory holding a `library.json`, and the editor mounts several of them at once
(the presets shipped here, the user's own library under the user data directory, a team's shared
directory, a project's `shared/symbol-library/`). The build copies this whole directory next to
`xtframe-editor`, `xtframe-pack` and `xtframe-design` (`<applicationDirPath>/symbols`) and
`cmake --install` puts it in `bin/symbols`.

## Layout

```
library.json                 version 1; id, name, readOnly, default (the group xt-standard:<Name>
                             resolves in), groups[] (the group ids, in panel order)
groups/<group-id>/
  group.json                 version 1; id, name, description, source {kind, set, setVersion},
                             tokens (the token file), themes[] (theme files), thumbnail
  <Name>.xt                  one symbol library file each (subdirectories mobile/ and list/ allowed)
  index.json                 every symbol: name, category, file, thumbnail, description, contract,
                             ports, timelines, triggers, tokens [{name, default}], fonts[], textures[]
  tokens.xts                 the group's token defaults (@font-face and --tex-* included)
  tokens-night.xts           a theme: the same token names with other values
  fonts/  textures/  thumbnails/
  previews/                  <Name>.preview.xt + <Name>.preview.json: what the thumbnails are rendered from
  README.md  LICENSE  CHANGELOG.md
```

`library.json` of the presets:

```json
{ "version": 1, "id": "preset", "name": "XTFrame presets", "readOnly": true,
  "default": "broadsheet-2026", "groups": ["broadsheet-2026"] }
```

- `readOnly`: the presets are never edited in place. The first editor start copies the default
  group into the user's own library (`<user data>/XTFrame/symbols/`), which is editable;
  "reload preset" restores a group from this copy (the old one goes to `.trash/`).
- `default`: the group every "standard symbol" path resolves in -- `xtframe-pack`'s and the
  editor's standard symbol directory and the `xt-standard:<Name>` link alias
  (`xt::bundle::defaultGroupDir()`). Nothing hard-codes a group name.
- A preset group is a year edition (49.8): `broadsheet-2026` is frozen once released; a new look is a
  new group (`broadsheet-2027`) that becomes the default, and the old one stays here.

`index.json` of a group lists each symbol's `tokens` as `{ "name": "--color-accent", "default":
"#1f1c18" }` (the default is the value in `tokens.xts`) and the `fonts` and `textures` files the
symbol needs (found from the `--font-*` and `--tex-*` tokens it reads: the `@font-face src` and
`url()` values of `tokens.xts`). `Tools/Scripts/gen_symbols_index.py` writes these fields and
`Tools/Scripts/check_symbols_index.sh` checks them.

## Groups

- `groups/broadsheet-2026/`: Broadsheet 2026, the newsprint set of chapter 47 (see its README).

## Licence

The presets (symbols, tokens, index and thumbnails) are dedicated to the public domain under
CC0-1.0 (`LICENSE`, draft pending review); the fonts in each group's `fonts/` keep their SIL OFL 1.1.
