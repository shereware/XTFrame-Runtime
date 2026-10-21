# Fetches FreeType and HarfBuzz (pinned tags, static) for xt_text's FtTextShaper. See
# docs/tasks/T-29.md "第三方引入".
#
# Included once from the top-level CMakeLists.txt when XT_BUILD_TEXT. Defines the
# `freetype` and `harfbuzz` targets used by text/CMakeLists.txt. SheenBidi and libunibreak are
# vendored source (third_party/sheenbidi, third_party/libunibreak), not fetched here.

include(FetchContent)

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

FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG VER-2-13-3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(freetype)

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

FetchContent_Declare(
    harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG 10.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(harfbuzz)

# HarfBuzz's CMakeLists.txt defines the `harfbuzz` target only when HB_HAVE_FREETYPE can find a
# `freetype` target; FetchContent_MakeAvailable(freetype) above already created it in this same
# configure, so no extra wiring is needed here beyond making sure freetype comes first (it does).

# Third-party targets are not xt_text: their own warnings (including MSVC C4819, "file contains
# a character that cannot be represented in the current code page") are not our concern; do not
# add xt_set_warnings() to freetype/harfbuzz.
