# CTest helper (T-714, Docs/design/22-bundle.md §22.17.1): the publish entry point `xtframe-pack`
# compresses images by default (`auto`) while development packs do not. Invoked as
#   cmake -DPACK=<xtframe-pack> [-DVIEW=<xtframe-view>] -DFIXTURE=<project dir> -DROOT=<scratch dir>
#         -P t714_publish_default.cmake
# The fixture (Tests/bundle/fixtures/t714_publish) has no `publish.images`, a 256 x 256 image and a
# 64 x 64 one. Checks:
#   1. no `--compress`: the large image becomes a DDS, the small one stays a PNG, and the run prints
#      the publish result line;
#   2. `--compress none` writes no DDS;
#   3. a copy whose `.xtui` says `"compress": "none"` packs the same assets and manifest (without
#      `builtAt`) as 2.;
#   4. (with VIEW) a copy whose `.xtui` says `"compress": "bc7"`, opened by `xtframe-view`: the
#      development pack encodes nothing -- the temp directory it packs into holds no encode cache
#      (`.xtframe-cache`) after the run.
# Not a C++ file: never run clang-format on it.
foreach(var PACK FIXTURE ROOT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "t714_publish_default.cmake: -D${var}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

function(t714_pack project out)
    execute_process(COMMAND "${PACK}" "${project}" -o "${out}" --no-thumbs ${ARGN}
                    RESULT_VARIABLE code OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    message("${stdout}${stderr}")
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "xtframe-pack ${project} exited ${code}")
    endif()
    set(t714_output "${stdout}${stderr}" PARENT_SCOPE)
endfunction()

function(t714_count dir pattern var)
    file(GLOB found "${dir}/assets/${pattern}")
    list(LENGTH found n)
    set(${var} ${n} PARENT_SCOPE)
endfunction()

# 1. The default: auto.
t714_pack("${FIXTURE}" "${ROOT}/auto")
t714_count("${ROOT}/auto" "*.dds" dds)
t714_count("${ROOT}/auto" "*.png" png)
if(NOT dds EQUAL 1 OR NOT png EQUAL 1)
    message(FATAL_ERROR "default pack: expected 1 DDS and 1 PNG under assets/, got ${dds} and ${png}")
endif()
if(NOT t714_output MATCHES "publish result: bundle [0-9.]+ [KM]B on disk; images: 1 compressed, 1 uncompressed, 0 in atlases; estimated display memory")
    message(FATAL_ERROR "default pack: no publish result line")
endif()
if(NOT t714_output MATCHES "compressed 1 image\\(s\\) to DDS \\(auto\\)")
    message(FATAL_ERROR "default pack: not packed with auto")
endif()

# 2. --compress none overrides it.
t714_pack("${FIXTURE}" "${ROOT}/cli-none" --compress none)
t714_count("${ROOT}/cli-none" "*.dds" dds)
if(NOT dds EQUAL 0)
    message(FATAL_ERROR "--compress none: ${dds} DDS file(s) under assets/")
endif()

# 3. `"compress": "none"` in the .xtui: the same bytes as 2.
file(COPY "${FIXTURE}/" DESTINATION "${ROOT}/none-src")
file(WRITE "${ROOT}/none-src/t714.xtui"
     "{ \"version\": 1, \"name\": \"T714\", \"id\": \"t714\", \"uis\": [ { \"id\": \"m\", \"doc\": \"m/m.xt\", \"size\": [400, 300] } ], \"publish\": { \"images\": { \"compress\": \"none\" } } }\n")
t714_pack("${ROOT}/none-src" "${ROOT}/xtui-none")
file(GLOB_RECURSE left RELATIVE "${ROOT}/cli-none" "${ROOT}/cli-none/assets/*")
file(GLOB_RECURSE right RELATIVE "${ROOT}/xtui-none" "${ROOT}/xtui-none/assets/*")
if(NOT left STREQUAL right)
    message(FATAL_ERROR ".xtui none vs --compress none: different assets (${left} / ${right})")
endif()
foreach(f IN LISTS left)
    file(SHA256 "${ROOT}/cli-none/${f}" a)
    file(SHA256 "${ROOT}/xtui-none/${f}" b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR ".xtui none vs --compress none: ${f} differs")
    endif()
endforeach()
foreach(side cli-none xtui-none)
    file(STRINGS "${ROOT}/${side}/manifest.json" lines)
    list(FILTER lines EXCLUDE REGEX "\"builtAt\"")
    set(manifest_${side} "${lines}")
endforeach()
if(NOT manifest_cli-none STREQUAL manifest_xtui-none)
    message(FATAL_ERROR ".xtui none vs --compress none: manifest.json differs")
endif()

# 4. xtframe-view's development pack encodes nothing, even for a project that says bc7.
if(DEFINED VIEW)
    file(COPY "${FIXTURE}/" DESTINATION "${ROOT}/bc7-src")
    file(WRITE "${ROOT}/bc7-src/t714.xtui"
         "{ \"version\": 1, \"name\": \"T714\", \"id\": \"t714\", \"uis\": [ { \"id\": \"m\", \"doc\": \"m/m.xt\", \"size\": [400, 300] } ], \"publish\": { \"images\": { \"compress\": \"bc7\" } } }\n")
    file(MAKE_DIRECTORY "${ROOT}/view-tmp")
    set(ENV{TMP} "${ROOT}/view-tmp")
    set(ENV{TEMP} "${ROOT}/view-tmp")
    set(ENV{TMPDIR} "${ROOT}/view-tmp")
    execute_process(COMMAND "${VIEW}" "${ROOT}/bc7-src" --headless --frames 1
                    RESULT_VARIABLE code OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    message("${stdout}${stderr}")
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "xtframe-view exited ${code}")
    endif()
    file(GLOB_RECURSE cache LIST_DIRECTORIES true "${ROOT}/view-tmp/*xtframe-cache*")
    if(cache)
        message(FATAL_ERROR "xtframe-view encoded images: ${cache}")
    endif()
endif()
file(REMOVE_RECURSE "${ROOT}")
