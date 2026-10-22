# Third-party versions: the single source for every dependency of this tree
# (Docs/design/45-repository-layout.md §45.3, Docs/tasks/T-430.md items 7-10).
#
# One xt_sdk() call per dependency:
#   xt_sdk(<name> URL      <url>  <sha256> ...)   fetched archive (FetchContent, Tools/CMake/xt_sdks.cmake)
#   xt_sdk(<name> GIT      <repo> <tag>    ...)   fetched git tag (FetchContent, shallow)
#   xt_sdk(<name> VENDORED <dir>  <version> ...)  committed under Code/SDKs/<dir> (not fetched)
#   xt_sdk(<name> ASSET    <dir>  <version> ...)  committed data (listed for the notices only)
# followed by the notice columns TITLE, HOME, VERSION (as shown), LICENSE, USED_BY and PURPOSE,
# which Tools/Scripts/gen_third_party.py turns into Code/SDKs/NOTICE.md and Legal/THIRD-PARTY.md (rows in
# the order below).
#
# A fetched dependency's source lives in Code/SDKs/_fetched/<name>/ (git-ignored), shared by
# every build tree. Updating one = edit its line here; the next configure fetches the new version
# (the stamp `.xt-sdk` in that directory no longer matches). XT_SDK_OFFLINE=ON never downloads.
#
# Plain CMake syntax (no variables), read both by CMake and by Tools/Scripts/gen_third_party.py.

# ---- vendored --------------------------------------------------------------------------------
xt_sdk(doctest VENDORED doctest v2.4.11
    TITLE "doctest" HOME "https://github.com/doctest/doctest" VERSION "v2.4.11" LICENSE "MIT"
    USED_BY "`Tests/`" PURPOSE "Header-only unit test framework")
xt_sdk(stb VENDORED stb 5c205738c191bcb0abc65c4febfa9bd25ff35234
    TITLE "stb_image / stb_image_write" HOME "https://github.com/nothings/stb"
    VERSION "commit `5c20573`" LICENSE "Public Domain / MIT" USED_BY "`Code/Runtime/sw/`, `Examples/`"
    PURPOSE "PNG decode/encode for golden-image tests and reference output")
xt_sdk(nlohmann VENDORED nlohmann v3.11.3
    TITLE "nlohmann/json" HOME "https://github.com/nlohmann/json" VERSION "v3.11.3" LICENSE "MIT"
    USED_BY "`Code/Runtime/compile/`" PURPOSE "JSON parsing for the Lottie parser and `.xts`/`.xt` tooling")
xt_sdk(sheenbidi VENDORED sheenbidi v2.8
    TITLE "SheenBidi" HOME "https://github.com/Tehreer/SheenBidi" VERSION "v2.8"
    LICENSE "Apache License 2.0" USED_BY "`Code/Runtime/text/`" PURPOSE "UAX #9 bidirectional text analysis")
xt_sdk(libunibreak VENDORED libunibreak libunibreak_6_1
    TITLE "libunibreak" HOME "https://github.com/adah1972/libunibreak" VERSION "libunibreak_6_1"
    LICENSE "zlib" USED_BY "`Code/Runtime/text/`"
    PURPOSE "UAX #14 line breaking and grapheme cluster breaking")
xt_sdk(bc7enc VENDORED bc7enc b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    TITLE "bc7enc_rdo" TITLE_SUFFIX "(bc7enc, bc7decomp, rgbcx)"
    HOME "https://github.com/richgel999/bc7enc_rdo" VERSION "commit `b943862`"
    LICENSE "MIT / Public Domain (dual)" USED_BY "`Code/Runtime/bundle/`, `Code/Runtime/sw/`"
    PURPOSE "BC1/BC3/BC7 texture block encoding (`xtframe-pack --compress`) and software decoding of DDS assets")

# ---- fetched (FetchContent, Code/SDKs/_fetched/<name>/) ------------------------------------
xt_sdk(freetype GIT https://gitlab.freedesktop.org/freetype/freetype.git VER-2-13-3
    TITLE "FreeType" HOME "https://gitlab.freedesktop.org/freetype/freetype" VERSION "VER-2-13-3"
    LICENSE "FreeType License (BSD-style) / GPLv2 dual license" USED_BY "`Code/Runtime/text/`"
    PURPOSE "Font parsing, outline decomposition, bitmap rendering")
xt_sdk(harfbuzz GIT https://github.com/harfbuzz/harfbuzz.git 10.1.0
    TITLE "HarfBuzz" HOME "https://github.com/harfbuzz/harfbuzz" VERSION "10.1.0" LICENSE "MIT"
    USED_BY "`Code/Runtime/text/`" PURPOSE "Text shaping (glyph selection, positioning, OpenType features)")
xt_sdk(sdl2 GIT https://github.com/libsdl-org/SDL.git release-2.30.9
    TITLE "SDL2" HOME "https://github.com/libsdl-org/SDL" VERSION "release-2.30.9" LICENSE "zlib"
    USED_BY "`Code/Runtime/gl/` (tests), `Code/Runtime/shell/`" PURPOSE "Window, input, and GL context creation")
xt_sdk(clipper2 URL https://github.com/AngusJohnson/Clipper2/archive/refs/tags/Clipper2_1.5.4.tar.gz
    9d8a35a29d04cd1b7b45f542c0ba48015feece1210036ea9e4efaad3140af4b0
    TITLE "Clipper2" HOME "https://github.com/AngusJohnson/Clipper2"
    VERSION "Clipper2_1.5.4 (commit `ef88ee9`)" LICENSE "Boost Software License 1.0"
    USED_BY "`Code/Editor/Core/`"
    PURPOSE "Polygon boolean and offset operations for destructive vector path editing (`USINGZ` on)")
xt_sdk(httplib URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.58.0.tar.gz
    31932ce8b33f2905472a987dc1984b9d8a7d338083a066021349587cc5f1cfea
    TITLE "cpp-httplib" HOME "https://github.com/yhirose/cpp-httplib" VERSION "v0.58.0"
    LICENSE "MIT" USED_BY "`Code/Editor/Service/`"
    PURPOSE "Single-header HTTP server for the editor's MCP endpoint (loopback only)")
xt_sdk(miniz URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    TITLE "miniz" HOME "https://github.com/richgel999/miniz" VERSION "3.0.2" LICENSE "MIT"
    USED_BY "`Code/Editor/History/`"
    PURPOSE "zlib-compatible deflate/inflate for the history core's loose objects")

# ---- committed test data ---------------------------------------------------------------------
xt_sdk(noto_fonts ASSET Tests/fonts see-Tests/fonts/README.md
    TITLE "Noto Sans / Noto Sans SC / Noto Naskh Arabic / Noto Sans Hebrew / Noto Sans Thai / Noto Emoji"
    TITLE_SUFFIX "subsets" HOME "https://fonts.google.com/noto" VERSION "see `Tests/fonts/README.md`"
    LICENSE "SIL Open Font License 1.1" USED_BY "`Tests/fonts/`"
    PURPOSE "Test-only fonts for shaping, bidi, and golden-image fixtures")
