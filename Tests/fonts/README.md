# Test fonts

OFL-licensed subsets used by `xt_text_tests` (Docs/tasks/T-29.md) and the `ui_i18n_*`/`ui_text_*`
golden fixtures and `xt_text_tests` regression cases (Docs/tasks/T-30b.md). Each is < 200 KB.
See `LICENSE` for the SIL Open Font License text and per-font copyright.

| File | Source | Registered as |
|---|---|---|
| `NotoSans-Regular.subset.ttf` | Google Fonts `ofl/notosans/NotoSans[wdth,wght].ttf` | family "Noto Sans", weight 400 |
| `NotoSansSC-Regular.subset.ttf` | Google Fonts `ofl/notosanssc/NotoSansSC[wght].ttf` | family "Noto Sans SC", weight 400 |
| `NotoNaskhArabic-Regular.subset.ttf` | Google Fonts `ofl/notonaskharabic/NotoNaskhArabic[wght].ttf` | family "Noto Naskh Arabic", weight 400 |
| `NotoSansHebrew-Regular.subset.ttf` | Google Fonts `ofl/notosanshebrew/NotoSansHebrew[wdth,wght].ttf` | family "Noto Sans Hebrew", weight 400 |
| `NotoSansThai-Regular.subset.ttf` | Google Fonts `ofl/notosansthai/NotoSansThai[wdth,wght].ttf` | family "Noto Sans Thai", weight 400 |
| `NotoEmoji-Regular.subset.ttf` | Google Fonts `ofl/notoemoji/NotoEmoji[wght].ttf` (monochrome outline emoji, not the color "Noto Color Emoji") | family "Noto Emoji", weight 400 |

## How they were produced

Source fonts are Google Fonts' variable-font releases (single weight-400, default-width static
instances are not published separately for these families), so each was first pinned to a
static Regular instance with `fonttools varLib.instancer`, then subset to only the codepoints the
tests actually exercise with `fonttools subset` (`pyftsubset`). Requires `fonttools` (installed
locally with `uv`, in a temporary venv -- never globally):

```sh
uv venv .venv
source .venv/Scripts/activate   # .venv/bin/activate on Linux/macOS
uv pip install fonttools brotli
```

Download the source variable fonts:

```sh
curl -sSL -o NotoSans.ttf \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notosans/NotoSans%5Bwdth%2Cwght%5D.ttf"
curl -sSL -o NotoNaskhArabic.ttf \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notonaskharabic/NotoNaskhArabic%5Bwght%5D.ttf"
curl -sSL -o NotoSansSC.ttf \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
curl -sSL -o NotoSansHebrew.ttf \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanshebrew/NotoSansHebrew%5Bwdth%2Cwght%5D.ttf"
curl -sSL -o NotoSansThai.ttf \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notosansthai/NotoSansThai%5Bwdth%2Cwght%5D.ttf"
curl -sSL -o NotoEmoji.ttf \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
```

Pin each to a static weight-400 instance (Noto Sans and Noto Sans Hebrew/Thai also have a `wdth`
axis, pinned to 100 = Normal):

```sh
fonttools varLib.instancer -o NotoSans-Regular-full.ttf NotoSans.ttf wght=400 wdth=100
fonttools varLib.instancer -o NotoNaskhArabic-Regular-full.ttf NotoNaskhArabic.ttf wght=400
fonttools varLib.instancer -o NotoSansSC-Regular-full.ttf NotoSansSC.ttf wght=400
fonttools varLib.instancer -o NotoSansHebrew-Regular-full.ttf NotoSansHebrew.ttf wght=400 wdth=100
fonttools varLib.instancer -o NotoSansThai-Regular-full.ttf NotoSansThai.ttf wght=400 wdth=100
fonttools varLib.instancer -o NotoEmoji-Regular-full.ttf NotoEmoji.ttf wght=400
```

Subset each to the codepoints used by `Tests/text/test_ft_shaper.cpp` (Docs/tasks/T-29.md) and by
the `ui_i18n_*` golden fixtures / `Tests/text/test_i18n_regression.cpp` (Docs/tasks/T-30b.md;
`--layout-features='*'` keeps every OpenType GSUB/GPOS feature, needed for Arabic
joining/ligatures and Latin mark attachment (the "é"/"ä"/"ñ" combining-mark tests)):

```sh
# Latin: "Hello World", "abc def", the outline test's "A", "é"/"ä"/"ñ" (base + combining mark),
# ASCII digits (bidi/RTL-digit fixtures), parentheses (the bidi-nesting fixture), and the
# horizontal ellipsis U+2026 (text-overflow: ellipsis fixtures).
pyftsubset NotoSans-Regular-full.ttf \
  --output-file=NotoSans-Regular.subset.ttf \
  --unicodes="U+0020,U+0028-0029,U+0030-0039,U+0041-005A,U+0061-007A,U+0301,U+0303,U+0308,U+2026" \
  --layout-features='*' --glyph-names --notdef-outline --no-hinting \
  --recalc-bounds --recalc-average-width

# CJK: "日本"/"世界" (T-29) plus "這是一段測試換行"/"漢字語" (T-30b cjk_mixed_wrap/vertical fixtures).
pyftsubset NotoSansSC-Regular-full.ttf \
  --output-file=NotoSansSC-Regular.subset.ttf \
  --unicodes="U+4E00,U+4E16,U+5B57,U+63DB,U+662F,U+672C,U+65E5,U+6BB5,U+6E2C,U+6F22,U+754C,U+884C,U+8A66,U+8A9E,U+9019" \
  --layout-features='*' --glyph-names --notdef-outline --no-hinting \
  --recalc-bounds --recalc-average-width

# Arabic: "مرحبا" (T-29) plus the additional letters in "السلام عليكم" and a plain space (T-30b
# arabic_shaping/ellipsis_rtl/letter_spacing fixtures).
pyftsubset NotoNaskhArabic-Regular-full.ttf \
  --output-file=NotoNaskhArabic-Regular.subset.ttf \
  --unicodes="U+0020,U+0627,U+0628,U+062D,U+0631,U+0633,U+0639,U+0643,U+0644,U+0645,U+064A" \
  --layout-features='*' --glyph-names --notdef-outline --no-hinting \
  --recalc-bounds --recalc-average-width

# Hebrew: letters for "שלום" (fallback fixture), "מספר" (hebrew_rtl fixture) and "אבג"
# (bidi_nested fixture), plus a space and ASCII digits 0-9 (hebrew_rtl's embedded "123").
pyftsubset NotoSansHebrew-Regular-full.ttf \
  --output-file=NotoSansHebrew-Regular.subset.ttf \
  --unicodes="U+0020,U+0030-0039,U+05D0,U+05D1,U+05D2,U+05D5,U+05DC,U+05DD,U+05DE,U+05E1,U+05E4,U+05E8,U+05E9" \
  --layout-features='*' --glyph-names --notdef-outline --no-hinting \
  --recalc-bounds --recalc-average-width

# Thai: the exact codepoints of the thai_break fixture's unspaced phrase
# "ประเทศไทยมีวัฒนธรรมที่งดงาม" ("Thailand has a beautiful culture"), computed with
# `sorted(set(ord(c) for c in s))` so the subset carries no unused glyphs.
pyftsubset NotoSansThai-Regular-full.ttf \
  --output-file=NotoSansThai-Regular.subset.ttf \
  --unicodes="U+0E07,U+0E12,U+0E14,U+0E17,U+0E18,U+0E19,U+0E1B,U+0E21,U+0E22,U+0E23,U+0E27,U+0E28,U+0E30,U+0E31,U+0E32,U+0E35,U+0E40,U+0E44,U+0E48" \
  --layout-features='*' --glyph-names --notdef-outline --no-hinting \
  --recalc-bounds --recalc-average-width

# Emoji: only U+1F600 GRINNING FACE (the emoji fixture's "😀"). Noto Emoji is monochrome
# (outline glyphs, no COLR/CBDT), unlike "Noto Color Emoji", so it renders through the same
# glyph-outline/bitmap path as every other text font -- no separate color-glyph pipeline needed.
pyftsubset NotoEmoji-Regular-full.ttf \
  --output-file=NotoEmoji-Regular.subset.ttf \
  --unicodes="U+1F600" \
  --layout-features='*' --glyph-names --notdef-outline --no-hinting \
  --recalc-bounds --recalc-average-width
```

`fonttools varLib.instancer`'s STAT-driven name-table pruning mis-named the CJK and Hebrew
instances ("Noto Sans SC Thin" / "Noto Sans Hebrew Thin" instead of "Noto Sans SC" / "Noto Sans
Hebrew"); their `name` table IDs 1/2/4/6/16/17 were corrected in-place afterward with a short
`fontTools.ttLib` script setting them to the expected family ("Noto Sans SC" / "Noto Sans Hebrew")
/ subfamily ("Regular") names. The other fonts' instancer output already had the expected family
names and needed no such fix.

## Vertical (`vert`/`vrt2`) coverage

`NotoSansSC-Regular.subset.ttf`'s source font does not carry a `vert`/`vrt2` GSUB feature for the
subsetted codepoints (日/本/世/界/這/是/一/段/測/試/換/行/語/漢/字 have no vertical alternate glyphs
in Noto Sans SC -- unlike CJK punctuation such as brackets, plain ideographs render identically
upright or rotated). The `test_ft_shaper.cpp` and `test_i18n_regression.cpp` vertical-writing-mode
tests therefore only check the geometry contract (`yAdvance > 0`/`vertical`/`rotated` flags), not
the "`vert` feature present -> different glyph" branch of Docs/tasks/T-29.md's test list; see
"Uncovered cases" in the T-29 and T-30b task reports.

## Cross-family fallback (`ui_i18n_fallback`)

Docs/tasks/T-30b.md's `ui_i18n_fallback` fixture lists only the Latin family in `font-family` and
expects the shaper to still resolve the CJK and Hebrew portions of "Hello 世界 שלום" to their own
fonts. The `xt::Document` load path (`Code/Runtime/core/src/document_load.cpp`) only calls
`TextShaper::addFontMemory()` for `@font-face` assets; it never calls `FtTextShaper::addFallbackFont()`,
so a font not named in a node's own `font-family` is never a fallback candidate for that node
during golden-image rendering (`ui_i18n_fallback.xt`'s CJK/Hebrew glyphs render as `.notdef` boxes
there -- an existing, pre-T-30b limitation, not something this task's file list allows fixing since
that would mean editing `Code/Runtime/core/src/document_load.cpp`). `Tests/text/test_i18n_regression.cpp`
exercises the intended fallback behavior directly against `FtTextShaper`, registering the CJK and
Hebrew fonts with `addFallbackFont()` after `match()`/`shape()` is given only the Latin family, and
asserts on the resulting distinct `FontId`s -- decoupled from the golden-image pipeline, per
Docs/tasks/T-30b.md's "量化斷言...不看像素，看 ShapeResult / GlyphRun".

## Other T-30b deviations from the literal spec wording

- `ui_i18n_thai_break`: the vendored `Code/SDKs/libunibreak` has no Thai/Southeast-Asian word
  dictionary (`linebreak.c`'s `resolve_lb_class()` unconditionally maps line-break class `SA` to
  `AL`), so an unspaced Thai run gets zero UAX#14 `ALLOWBREAK` opportunities from
  `computeBreaks()` no matter what `lang` is set to. `layoutText()` still wraps such a run (per
  Docs/tasks/T-28.md's "a long word with no break opportunity forces a grapheme split"), but at a
  grapheme boundary, not a linguistic word boundary. `Tests/text/test_i18n_regression.cpp`
  therefore checks ">=2 lines, each split at a grapheme boundary" instead of the task table's
  literal "each break point has `breakAfter == 1`".
- `ui_i18n_arabic_shaping`: `NotoNaskhArabic-Regular.subset.ttf` decomposes some letters into a
  base+mark pair via its `ccmp` GSUB feature (already noted above for "مرحبا" in the T-29 vertical
  section); for "السلام عليكم" this makes glyph count *exceed* codepoint count instead of falling
  below it through ligature joining. `test_i18n_regression.cpp` checks the weaker
  "shaping is not a naive 1:1 codepoint->glyph mapping" (`!=` rather than `<`).
