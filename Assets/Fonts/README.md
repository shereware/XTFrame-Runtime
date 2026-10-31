# Standard fonts

The fonts XTFrame ships with its tools (`<exe dir>/fonts`, `Code/Tools/wasm/dist/fonts`, the browser
module's `xtframe-data.zip`), so that text renders the same on every machine and on platforms without
system fonts (Docs/design/22-bundle.md §22.10.3, T-527b). `xtframe-pack --fonts bundled` resolves
`font-family` names against them and copies the ones a UI uses into the bundle; nothing here is loaded
by the runtime from this directory.

| File | Family | Source |
|---|---|---|
| NotoSans-Regular.ttf, NotoSans-Bold.ttf, NotoSans-Italic.ttf, NotoSans-BoldItalic.ttf | Noto Sans | notofonts/notofonts.github.io `fonts/NotoSans/hinted/ttf`, 2026-10-29 |
| NotoSerif-Regular.ttf, NotoSerif-Bold.ttf, NotoSerif-Italic.ttf | Noto Serif | the same files as `Assets/Symbols/groups/broadsheet-2026/fonts/` (T-489), so serif projects without that group resolve too |
| NotoSansTC-Regular.ttf | Noto Sans TC | google/fonts `ofl/notosanstc/NotoSansTC[wght].ttf`, instanced to wght=400 with fontTools 4.68 (`varLib.instancer --update-name-table`), 2026-10-29 |

Licence: SIL Open Font License 1.1 (`OFL-NotoSans.txt`, `OFL-NotoSerif.txt`, `OFL-NotoSansTC.txt`). Noto Serif
Display (the headline face) stays with the symbol library group that uses it.
