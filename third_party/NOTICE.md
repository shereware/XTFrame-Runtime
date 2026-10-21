# Third-party notices

XTFrame is MIT-licensed (see `LICENSE`). It vendors or fetches the third-party components listed
below. None of them are relicensed or modified beyond what each entry notes; see the linked
`VERSION.txt` / `LICENSE` files for exact provenance and full license text.

| Name | Version | License | Used by | Purpose |
|---|---|---|---|---|
| [doctest](https://github.com/doctest/doctest) | v2.4.11 | MIT | `tests/` | Header-only unit test framework |
| [stb_image / stb_image_write](https://github.com/nothings/stb) | commit `5c20573` | Public Domain / MIT | `sw/`, `examples/` | PNG decode/encode for golden-image tests and reference output |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | MIT | `compile/` | JSON parsing for the Lottie parser and `.xts`/`.xt` tooling |
| [SheenBidi](https://github.com/Tehreer/SheenBidi) | v2.8 | Apache License 2.0 | `text/` | UAX #9 bidirectional text analysis |
| [libunibreak](https://github.com/adah1972/libunibreak) | libunibreak_6_1 | zlib | `text/` | UAX #14 line breaking and grapheme cluster breaking |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (bc7enc, bc7decomp, rgbcx) | commit `b943862` | MIT / Public Domain (dual) | `bundle/`, `sw/` | BC1/BC3/BC7 texture block encoding (`xtframe-pack --compress`) and software decoding of DDS assets |
| [FreeType](https://gitlab.freedesktop.org/freetype/freetype) | VER-2-13-3 | FreeType License (BSD-style) / GPLv2 dual license | `text/` | Font parsing, outline decomposition, bitmap rendering |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | 10.1.0 | MIT | `text/` | Text shaping (glyph selection, positioning, OpenType features) |
| [SDL2](https://github.com/libsdl-org/SDL) | release-2.30.9 | zlib | `gl/` (tests), `shell/` | Window, input, and GL context creation |
| [Clipper2](https://github.com/AngusJohnson/Clipper2) | Clipper2_1.5.4 (commit `ef88ee9`) | Boost Software License 1.0 | `editor/core/` | Polygon boolean and offset operations for destructive vector path editing (`USINGZ` on) |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.58.0 | MIT | `editor/service/` | Single-header HTTP server for the editor's MCP endpoint (loopback only) |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | MIT | `editor/history/` | zlib-compatible deflate/inflate for the history core's loose objects |
| [Noto Sans / Noto Sans SC / Noto Naskh Arabic / Noto Sans Hebrew / Noto Sans Thai / Noto Emoji](https://fonts.google.com/noto) subsets | see `tests/fonts/README.md` | SIL Open Font License 1.1 | `tests/fonts/` | Test-only fonts for shaping, bidi, and golden-image fixtures |

## Vendored vs. fetched

- **Vendored** (source committed under `third_party/`, unmodified): doctest, stb, nlohmann/json,
  SheenBidi, libunibreak, bc7enc_rdo (bc7enc/bc7decomp/rgbcx). See `third_party/README.md` and each subdirectory's `VERSION.txt` for
  exact commit/tag and source URL.
- **Fetched at configure time** via CMake `FetchContent` (pinned tags, built from source, never
  installed system-wide): FreeType and HarfBuzz (`cmake/xt_text_deps.cmake`), SDL2
  (`cmake/xt_sdl2.cmake`), Clipper2 (`cmake/xt_clipper2.cmake`; only `Clipper2Lib`'s engine, offset
  and rectclip sources are compiled), cpp-httplib (`cmake/xt_httplib.cmake`, header only) and miniz
  (`cmake/xt_miniz.cmake`, only `miniz.c`).
- **Test-only assets** (not linked into any library or tool): the Noto font subsets under
  `tests/fonts/`, OFL-licensed; see `tests/fonts/README.md` for how they were produced and their
  per-file registered family names.

## License texts

- MIT, zlib, Apache License 2.0 and Boost Software License 1.0 texts are reproduced in each vendored component's own
  `LICENSE` file (`third_party/<name>/LICENSE`) or, for FreeType/HarfBuzz/SDL2/Clipper2/cpp-httplib/miniz, in the sources
  fetched by CMake at `build/_deps/<name>-src/`.
- The SIL Open Font License 1.1 text for the Noto fonts is at `tests/fonts/LICENSE`.
- XTFrame's own MIT license is at the repository root `LICENSE`.

None of the above imposes copyleft obligations on XTFrame itself: FreeType is used under its own
permissive FreeType License option (not GPLv2), and every other dependency is MIT, zlib, Apache
2.0, Boost 1.0, Public Domain, or OFL.
