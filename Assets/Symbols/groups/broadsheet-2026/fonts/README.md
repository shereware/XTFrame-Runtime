# Bundled fonts

Two font families for the Broadsheet style (Docs/design/47-symbol-visual-style.md 47.13.2), both under
the SIL Open Font License 1.1 (not CC0: the licence of this directory is the OFL files below):

| Family (CSS name) | Use | Files | Licence |
|---|---|---|---|
| Noto Serif Display ExtraCondensed | display text (`--font-display`) | `NotoSerifDisplay-ExtraCondensedBlack.ttf` (900), `NotoSerifDisplay-ExtraCondensedBold.ttf` (700) | `OFL-NotoSerifDisplay.txt` |
| Noto Serif | body text (`--font-body`, `--font-family`) | `NotoSerif-Regular.ttf` (400), `NotoSerif-Bold.ttf` (700), `NotoSerif-Italic.ttf` (italic 400) | `OFL-NotoSerif.txt` |

## Source

Upstream: https://github.com/google/fonts, commit `e7db6ca603fd4862010383ff17ffd6cc4ffac1c0`, directories `ofl/notoserifdisplay`
and `ofl/notoserif`. The OFL texts are the upstream `OFL.txt` files, renamed.

Upstream ships these families only as variable fonts (`wght` and `wdth` axes). The library bundles static
instances, made with fontTools (`fontTools.varLib.instancer.instantiateVariableFont`; no subsetting, no
other change; no `fvar` table is left). Upstream has no STAT table, so `updateFontNames` cannot be used: the `name` table of the
Noto Serif Display instances is set by hand to family "Noto Serif Display ExtraCondensed" (IDs 1, 2, 4, 6, in the
Windows and Macintosh records; IDs 16, 17, 25 removed; subfamily Black or Bold), and the Noto Serif instances get their
Regular / Bold / Italic subfamily the same way. The Black file keeps `usWeightClass` 900.

| Upstream file | SHA-256 | Instance |
|---|---|---|
| `ofl/notoserifdisplay/NotoSerifDisplay[wdth,wght].ttf` | `4b661adcf6a5d38bf5befdc4d976e8a7238a4aeb57e62c1c1a67775380a09df9` | wdth 62.5 with wght 900 and 700 |
| `ofl/notoserif/NotoSerif[wdth,wght].ttf` | `4d8e6761424656867019081a1a01336f3cb086982682698714054fc33f782713` | wdth 100 with wght 400 and 700 |
| `ofl/notoserif/NotoSerif-Italic[wdth,wght].ttf` | `e87acbc6c0efd0d9a20d6a8cbbda2b266c14be3a3a6f5af8ec9d7b2460570ad1` | wdth 100, wght 400 |
| `ofl/notoserifdisplay/OFL.txt` | `5e0da210fb04058a8c0087985d2d456b931c2579811a49655721d3cf0c36b6d6` | copied |
| `ofl/notoserif/OFL.txt` | `cee9892f9f0cc8fe882c9e9537ee6a89621d86ee7ceaf70b02e2b2b1c25c061a` | copied |

## Files in this directory (SHA-256)

```
8e955ed16738eaf3d09ebe337757d6ffd7a86c51bfd6bcd4409d552133b13f71  NotoSerif-Bold.ttf
ad6dafd7d900f978b747098e9411fb11db41ffa64efb4bd8cd98a5b253c7fe61  NotoSerif-Italic.ttf
d643ce7690e47debeededc795eb2d5db7a97e3005ef5bac35f552000023bbb9b  NotoSerif-Regular.ttf
95ae8b69a60f2e8a133cd57a0e3f47668720865eea958d3b9f47961214340fab  NotoSerifDisplay-ExtraCondensedBlack.ttf
ff3cc579778e4ff43e4ba75593868f9349c467ca183d8e19a4d3f58b776f7652  NotoSerifDisplay-ExtraCondensedBold.ttf
```

## Use

A project's `shared/tokens.xts` declares them before `:root` (the `src` is relative to the stylesheet's
own directory, 05 5.9.12; `xtframe-pack` copies the files into the bundle's `assets/`):

```css
@font-face { font-family: "Noto Serif Display ExtraCondensed"; src: "fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf"; font-weight: 900; }
```

Copy this directory's `.ttf` and `OFL-*.txt` files to the project's `shared/fonts/` (the templates,
the starter and the mobile HUD demo already carry a copy). `tokens.xts` and `tokens-night.xts` of the
library declare the same five `@font-face` rules against this directory. A host that previews a library
file directly must use the group directory as the asset root
(`xtframe-view --root Assets/Symbols/groups/broadsheet-2026`).
