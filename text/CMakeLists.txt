# xt_text: FreeType + HarfBuzz + SheenBidi + libunibreak based TextShaper (FtTextShaper) and
# GlyphAtlas. See docs/tasks/T-29.md, docs/design/07-text.md §7.3-7.4.

# ---- Vendored third-party static libs (C) ------------------------------------------------------
# SheenBidi: upstream ships its own single-translation-unit amalgamation entry point
# (Source/SheenBidi.c, compiled with SB_CONFIG_UNITY defined -- see third_party/sheenbidi).
add_library(sheenbidi STATIC ${PROJECT_SOURCE_DIR}/third_party/sheenbidi/Source/SheenBidi.c)
target_include_directories(sheenbidi SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/sheenbidi/Headers)
target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
set_target_properties(sheenbidi PROPERTIES C_STANDARD 99 LINKER_LANGUAGE C)

# libunibreak: only the line-break and grapheme-break translation units are compiled; the *data.c
# files are #included by their def.c counterparts (not separate translation units) -- see
# third_party/libunibreak/VERSION.txt.
add_library(unibreak STATIC
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/linebreak.c
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/linebreakdata.c
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/linebreakdef.c
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/graphemebreak.c
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/unibreakbase.c
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/unibreakdef.c
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/emojidef.c
    ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src/eastasianwidthdef.c
)
target_include_directories(unibreak SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/libunibreak/src)
set_target_properties(unibreak PROPERTIES C_STANDARD 99 LINKER_LANGUAGE C)
# T-402 (XT_SANITIZE): libunibreak calls its ub_get_next_char_* readers through a get_next_char_t
# pointer whose first parameter is `const void*`; vendored code, left unchanged, so Clang's
# -fsanitize=function (part of `undefined` for C since Clang 17) is off for this target only.
if(XT_SANITIZE AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(unibreak PRIVATE -fno-sanitize=function)
endif()

# Third-party targets are not xt_text: their own warnings (e.g. MSVC C4819, "file contains a
# character that cannot be represented in the current code page") are not our concern. No
# xt_set_warnings() on sheenbidi/unibreak/freetype/harfbuzz.
if(MSVC)
    target_compile_options(sheenbidi PRIVATE /w)
    target_compile_options(unibreak PRIVATE /w)
endif()

# ---- xt_text -------------------------------------------------------------------------------
add_library(xt_text STATIC
    src/font_name_table.cpp
    src/ft_fonts.cpp
    src/ft_bidi.cpp
    src/ft_breaks.cpp
    src/ft_script.cpp
    src/ft_glyphs.cpp
    src/ft_text_shaper.cpp
    src/glyph_atlas.cpp
)
target_include_directories(xt_text PUBLIC include)
# T-261e: core/src/file_open_utf8.h (openFileUtf8, the UTF-8 file open shared by the runtime layers).
target_include_directories(xt_text PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
target_link_libraries(xt_text PUBLIC xt_core)
xt_enable_exceptions(xt_text)  # already sets MSVC's /EHsc.
target_link_libraries(xt_text PRIVATE freetype harfbuzz sheenbidi unibreak)
# FreeType's and HarfBuzz's own public headers, included as SYSTEM so their warnings never count
# against xt_text's -Werror/-WX (docs/tasks/T-29.md "third-party headers included as SYSTEM").
target_include_directories(xt_text SYSTEM PRIVATE
    $<TARGET_PROPERTY:freetype,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:harfbuzz,INTERFACE_INCLUDE_DIRECTORIES>
)
xt_set_warnings(xt_text)
