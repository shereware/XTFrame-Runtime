# Project-level golden corpus

Each directory here is one corpus project (T-401, `Docs/design/43-compatibility-and-versions.md`
§43.4), checked by `Tools/Scripts/corpus_check.py` (wrapped by `Tools/Scripts/corpus_check.sh`).

## project.json

| Key | Meaning |
|---|---|
| `source` | The UI Project directory, relative to the repository root. It is copied to the work directory and packed with `xtframe-pack --strict --fonts bundled`. |
| `dpr` | Device pixel ratio; only `1` is supported. |
| `fonts` | Optional, history (T-426 / T-429): no project here lists it since T-527d; see "History: system fonts". |
| `frames` | The frames each UI is rendered at. |
| `uis` | The UI ids with their viewport `width` / `height`. |
| `scripts` | Behaviour scripts under `scripts/`. |

## Fonts

Since T-527d (`Docs/design/43-compatibility-and-versions.md` §43.4) the checker packs every project
with `xtframe-pack --strict --fonts bundled`: the bundle carries the standard Noto fonts
(`Assets/Fonts`, `Docs/design/22-bundle.md` §22.10.3; `xtframe-pack`'s `--fonts-dir` default
`<exe dir>/fonts`, copied there by the build) and the renders use only those, never the machine's
system fonts. The `expected/` files are rendered on Windows (MSVC x64,
`Docs/design/43-compatibility-and-versions.md` §43.4), and the CTest entry `corpus_check` is
registered on every platform (Release / RelWithDebInfo). A platform whose renders differ shows an
inconsistency to fix, not a reason to widen the tolerances.

## History: system fonts

The two sections below describe the T-426 / T-429 machinery for corpora whose renders depend on
system fonts. No project in this directory uses it since T-527d; it stays in
`Tools/Scripts/corpus_check.py` and `Tools/Scripts/corpus_fonts.py` for external corpora.

### Missing fonts

Before the run the checker asks `xtframe-view --list-fonts` for the installed fonts once.
When a family a project lists in `fonts` is not installed (or, T-429, is a different build):

- default (strict): the project fails with
  `[FAIL] <name>: font '<family>' is not installed`, so a development machine knows its renders
  cannot match;
- `--skip-missing-fonts`: the project is reported as
  `[SKIP] <name>: font '<family>' is not installed` and does not count as a failure.

Until T-527d the expected images were rendered with the Windows system fonts and the CTest entry
was registered on Windows only (T-422).

### Font pinning

The same family can ship in different builds (Windows Server 2022 and Windows 11 have different
`bahnschrift.ttf` files), and a different build moves glyph edges, so a family name alone does not
guarantee the renders match (T-429). The `fonts` entries therefore carry the SHA-256 of the font
file the `expected/` images were rendered with:

- `xtframe-view --list-fonts` prints one `family<TAB>path` line per registered system font file
  (UTF-8, sorted); `--list-fonts=names` prints only the family names (the T-426 output).
- `Tools/Scripts/corpus_check.py` hashes the files of each listed family (stdlib `hashlib`); when a family
  has several files (Regular / Bold / Italic) any one matching is enough. No match is treated like a
  missing font: `[FAIL] <name>: font '<family>' is a different build (sha256 <12 hex>... expected
  <12 hex>...)`, or `[SKIP]` with the same text under `--skip-missing-fonts`.
- `Tools/Scripts/corpus_fonts.py --update [--project <name>]` rewrites the `fonts` of every (or the named)
  project to objects with this machine's hashes. It keeps a stored hash that still matches one of
  the family's files and otherwise pins the file with the shortest base name (the regular face in
  the Windows naming, e.g. `georgia.ttf`). `corpus_check.py --update` (`XT_CORPUS_UPDATE=1`) runs it
  for the selected projects before regenerating `expected/`, so the hashes always describe the
  machine that rendered the expected images.
