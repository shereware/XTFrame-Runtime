# Fetches FreeType and HarfBuzz (pinned tags, static) for xt_text's FtTextShaper. See
# Docs/tasks/T-29.md "第三方引入".
#
# Included once from the top-level CMakeLists.txt when XT_BUILD_TEXT. Defines the
# `freetype` and `harfbuzz` targets used by Code/Runtime/text/CMakeLists.txt. SheenBidi and libunibreak are
# vendored source (Code/SDKs/sheenbidi, Code/SDKs/libunibreak), not fetched here.

include(${CMAKE_CURRENT_LIST_DIR}/xt_sdks.cmake)

# ---- FreeType ---------------------------------------------------------------------------------
# No zlib/bzip2/png/harfbuzz/brotli: xt_text only needs outline decomposition and bitmap
# rendering of the fonts it is given; none of those optional codecs are needed, and disabling
# HarfBuzz support inside FreeType avoids a circular FreeType<->HarfBuzz build dependency (we
# build HarfBuzz with HB_HAVE_FREETYPE=ON separately, below).
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

# Version: Code/SDKs/versions.cmake (freetype); source: Code/SDKs/_fetched/freetype.
xt_sdk_fetch(freetype freetype)

# ---- HarfBuzz -----------------------------------------------------------------------------------
# Only FreeType integration is needed (hb_ft_font_create_referenced); everything else HarfBuzz can
# optionally use (Cairo, GDI, DirectWrite, CoreText, Uniscribe, ICU, Graphite2, glib, gobject) is
# off, and the subsetter (HB_BUILD_SUBSET) is unused at runtime so it is off too.
set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
set(HB_HAVE_DIRECTWRITE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_UNISCRIBE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GDI OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CAIRO OFF CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)

# Version: Code/SDKs/versions.cmake (harfbuzz); source: Code/SDKs/_fetched/harfbuzz.
xt_sdk_fetch(harfbuzz harfbuzz)

# HarfBuzz's CMakeLists.txt defines the `harfbuzz` target only when HB_HAVE_FREETYPE can find a
# `freetype` target; FetchContent_MakeAvailable(freetype) above already created it in this same
# configure, so no extra wiring is needed here beyond making sure freetype comes first (it does).

# Third-party targets are not xt_text: their own warnings (including MSVC C4819, "file contains
# a character that cannot be represented in the current code page") are not our concern; do not
# add xt_set_warnings() to freetype/harfbuzz.
