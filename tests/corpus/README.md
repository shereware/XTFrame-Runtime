# Project-level golden corpus

Each directory here is one corpus project (T-401, `docs/design/43-compatibility-and-versions.md`
§43.4), checked by `tools/corpus_check.py` (wrapped by `tools/corpus_check.sh`).

## project.json

| Key | Meaning |
|---|---|
| `source` | The UI Project directory, relative to the repository root. It is copied to the work directory and packed with `xtframe-pack --strict`. |
| `dpr` | Device pixel ratio; only `1` is supported. |
| `fonts` | Optional (T-426). The font families the `expected/` renders depend on, e.g. `["Georgia"]`: the family the renderer actually resolves for the first choice of the project's `.xts` `font-family` (a family name as `xtframe-view --list-fonts` prints it: `Bahnschrift SemiCondensed` is a named instance of the variable font whose family is `Bahnschrift`). |
| `frames` | The frames each UI is rendered at. |
| `uis` | The UI ids with their viewport `width` / `height`. |
| `scripts` | Behaviour scripts under `scripts/`. |

## Missing fonts

Before the run the checker asks `xtframe-view --list-fonts` for the installed font families once.
When a family a project lists in `fonts` is not installed:

- default (strict, `tools/dev_check.sh` suite `corpus`): the project fails with
  `[FAIL] <name>: font '<family>' is not installed`, so a development machine knows its renders
  cannot match;
- `--skip-missing-fonts` (the CTest entry `corpus_check`, i.e. CI runners): the project is reported as
  `[SKIP] <name>: font '<family>' is not installed` and does not count as a failure.

The expected images are rendered with the Windows system fonts, so the CTest entry is registered on
Windows only (T-422).
