# libFuzzer targets (clang only), gated by XT_BUILD_FUZZ (see top-level CMakeLists.txt for the
# sanitizer/compiler-check plumbing). See Docs/design/15-testing.md §15.5, Docs/tasks/T-39.md,
# Docs/tasks/T-267b.md.
if(NOT XT_BUILD_FUZZ)
    return()
endif()

if(NOT TARGET xtframe-compile OR NOT TARGET xtframe-bake)
    message(FATAL_ERROR "XT_BUILD_FUZZ needs xtframe-compile and xtframe-bake (XT_BUILD_TOOLS=ON) to build the seed corpus")
endif()

set(XT_FUZZ_CORPUS_DIR "${CMAKE_CURRENT_BINARY_DIR}/corpus")
set(XT_FUZZ_SEEDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/seeds")
set_property(GLOBAL PROPERTY XT_FUZZ_CORPUS_OUTPUTS "")

# The selector prefixes of the multi-format targets (fuzz_image, fuzz_bundle_json, fuzz_import):
# a one-byte file holding the ASCII digit N, whose value modulo the target's format count is N.
set(XT_FUZZ_SELECTOR_DIR "${CMAKE_CURRENT_BINARY_DIR}/selector")
foreach(n 0 1 2)
    file(WRITE "${XT_FUZZ_SELECTOR_DIR}/${n}" "${n}")
endforeach()

# xt_fuzz_seed_name(out src): a corpus file name for `src` that two seeds with the same file name
# never share: its path relative to tests/ (or, outside tests/, to the repository root) with `/`
# and spaces replaced by `-` / `_` (Tests/xt/a.xt -> xt-a.xt).
function(xt_fuzz_seed_name out src)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}/Tests" "${src}")
    if(rel MATCHES "^\\.\\./")
        file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${src}")
    endif()
    string(REPLACE "/" "-" rel "${rel}")
    string(REPLACE " " "_" rel "${rel}")
    set(${out} "${rel}" PARENT_SCOPE)
endfunction()

# xt_fuzz_seed_copy(target src): copies `src` into corpus/<target>/.
function(xt_fuzz_seed_copy target src)
    xt_fuzz_seed_name(name "${src}")
    set(out "${XT_FUZZ_CORPUS_DIR}/${target}/${name}")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${XT_FUZZ_CORPUS_DIR}/${target}"
        COMMAND ${CMAKE_COMMAND} -E copy "${src}" "${out}"
        DEPENDS "${src}"
        COMMENT "fuzz corpus: ${target}/${name}"
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY XT_FUZZ_CORPUS_OUTPUTS "${out}")
endfunction()

# xt_fuzz_seed_selected(target selector src): corpus/<target>/<selector>-<name> = the selector
# byte followed by `src` (the fuzz targets are Linux-only, so `sh` does the concatenation).
function(xt_fuzz_seed_selected target selector src)
    xt_fuzz_seed_name(name "${src}")
    set(out "${XT_FUZZ_CORPUS_DIR}/${target}/${selector}-${name}")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${XT_FUZZ_CORPUS_DIR}/${target}"
        COMMAND sh -c "cat \"$1\" \"$2\" > \"$3\"" xt_fuzz_seed "${XT_FUZZ_SELECTOR_DIR}/${selector}" "${src}" "${out}"
        DEPENDS "${src}"
        COMMENT "fuzz corpus: ${target}/${selector}-${name}"
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY XT_FUZZ_CORPUS_OUTPUTS "${out}")
endfunction()

# xt_fuzz_seed_ir(src): corpus/ir/<name>.xtir, `src` compiled by xtframe-compile (an empty
# placeholder when it does not compile, see cmake/gen_ir_seed.cmake). Not checked in.
function(xt_fuzz_seed_ir src)
    xt_fuzz_seed_name(name "${src}")
    set(out "${XT_FUZZ_CORPUS_DIR}/ir/${name}.xtir")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${XT_FUZZ_CORPUS_DIR}/ir"
        COMMAND ${CMAKE_COMMAND} "-DXTFRAME_COMPILE=$<TARGET_FILE:xtframe-compile>" "-DSEED=${src}" "-DOUT=${out}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_ir_seed.cmake"
        DEPENDS xtframe-compile "${src}"
        COMMENT "fuzz corpus: ir/${name}.xtir"
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY XT_FUZZ_CORPUS_OUTPUTS "${out}")
endfunction()

# xt_fuzz_seed_xtcl(src): corpus/xtcl/<name>.xtcl, `src` compiled and then baked (3 frames at
# 10 fps) by xtframe-bake; an empty placeholder when either step fails (cmake/gen_xtcl_seed.cmake).
function(xt_fuzz_seed_xtcl src)
    xt_fuzz_seed_name(name "${src}")
    set(out "${XT_FUZZ_CORPUS_DIR}/xtcl/${name}.xtcl")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${XT_FUZZ_CORPUS_DIR}/xtcl"
        COMMAND ${CMAKE_COMMAND} "-DXTFRAME_COMPILE=$<TARGET_FILE:xtframe-compile>"
                "-DXTFRAME_BAKE=$<TARGET_FILE:xtframe-bake>" "-DSEED=${src}" "-DOUT=${out}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_xtcl_seed.cmake"
        DEPENDS xtframe-compile xtframe-bake "${src}"
        COMMENT "fuzz corpus: xtcl/${name}.xtcl"
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY XT_FUZZ_CORPUS_OUTPUTS "${out}")
endfunction()

# fuzz_xt / fuzz_ir: every `.xt` under Tests/xt, Tests/golden/ui and tests/fixtures (recursively)
# plus the hand-written Tests/fuzz/seeds/ir/*.xt (for an IR chunk kind the rest would miss), and
# the Lottie files under Tests/lottie (fuzz_lottie / fuzz_ir).
file(GLOB XT_FUZZ_XT_SEEDS "${PROJECT_SOURCE_DIR}/Tests/xt/*.xt")
file(GLOB_RECURSE XT_FUZZ_XT_SEEDS_GOLDEN "${PROJECT_SOURCE_DIR}/Tests/golden/ui/*.xt")
file(GLOB_RECURSE XT_FUZZ_XT_SEEDS_FIXTURES "${PROJECT_SOURCE_DIR}/Tests/fixtures/*.xt")
file(GLOB XT_FUZZ_XT_SEEDS_OWN "${XT_FUZZ_SEEDS_DIR}/ir/*.xt")
list(APPEND XT_FUZZ_XT_SEEDS ${XT_FUZZ_XT_SEEDS_GOLDEN} ${XT_FUZZ_XT_SEEDS_FIXTURES} ${XT_FUZZ_XT_SEEDS_OWN})
file(GLOB XT_FUZZ_LOTTIE_SEEDS "${PROJECT_SOURCE_DIR}/Tests/lottie/*.json")

foreach(seed IN LISTS XT_FUZZ_XT_SEEDS)
    xt_fuzz_seed_copy(xt "${seed}")
    xt_fuzz_seed_ir("${seed}")
endforeach()
foreach(seed IN LISTS XT_FUZZ_LOTTIE_SEEDS)
    xt_fuzz_seed_copy(lottie "${seed}")
    xt_fuzz_seed_ir("${seed}")
endforeach()

# fuzz_xtcl: baked from a Lottie animation, a custom shader effect and a 3D card flip.
foreach(seed "${PROJECT_SOURCE_DIR}/Tests/lottie/min_solid_anim.json"
             "${PROJECT_SOURCE_DIR}/Tests/golden/ui/ui_fx_custom.xt"
             "${PROJECT_SOURCE_DIR}/Tests/golden/ui/ui_3d_card_flip.xt")
    xt_fuzz_seed_xtcl("${seed}")
endforeach()

# fuzz_xts: every stylesheet under tests/.
file(GLOB_RECURSE XT_FUZZ_XTS_SEEDS "${PROJECT_SOURCE_DIR}/Tests/*.xts")
foreach(seed IN LISTS XT_FUZZ_XTS_SEEDS)
    xt_fuzz_seed_copy(xts "${seed}")
endforeach()

# fuzz_svg: the SVG files the compiler tests and goldens use, plus a rig sample (seeds/svg).
file(GLOB XT_FUZZ_SVG_SEEDS "${PROJECT_SOURCE_DIR}/Tests/xt/*.svg" "${PROJECT_SOURCE_DIR}/Tests/golden/ui/*.svg"
     "${XT_FUZZ_SEEDS_DIR}/svg/*.svg")
foreach(seed IN LISTS XT_FUZZ_SVG_SEEDS)
    xt_fuzz_seed_copy(svg "${seed}")
endforeach()

# fuzz_xtfx: the custom effects of the golden UIs.
file(GLOB XT_FUZZ_XTFX_SEEDS "${PROJECT_SOURCE_DIR}/Tests/golden/ui/*.xtfx")
foreach(seed IN LISTS XT_FUZZ_XTFX_SEEDS)
    xt_fuzz_seed_copy(xtfx "${seed}")
endforeach()

# fuzz_yaml: the shipped motion specs plus hand-written samples (seeds/yaml; no YAML file exists
# under tests/).
file(GLOB_RECURSE XT_FUZZ_YAML_SEEDS "${PROJECT_SOURCE_DIR}/Examples/*.yaml" "${XT_FUZZ_SEEDS_DIR}/yaml/*.yaml")
foreach(seed IN LISTS XT_FUZZ_YAML_SEEDS)
    xt_fuzz_seed_copy(yaml "${seed}")
endforeach()

# fuzz_image: 0 = DDS (hand-written, seeds/image; none under tests/), 1 = PNG.
file(GLOB XT_FUZZ_DDS_SEEDS "${XT_FUZZ_SEEDS_DIR}/image/*.dds")
foreach(seed IN LISTS XT_FUZZ_DDS_SEEDS)
    xt_fuzz_seed_selected(image 0 "${seed}")
endforeach()
foreach(seed "${PROJECT_SOURCE_DIR}/Tests/golden/ui/checker.png"
             "${PROJECT_SOURCE_DIR}/Tests/bundle/fixtures/sample/hud/img/a.png"
             "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/figma/hud/raster/3_8@2x.png")
    xt_fuzz_seed_selected(image 1 "${seed}")
endforeach()

# fuzz_bundle_json: 0 = manifest.json, 1 = UI Project files (.xtui and the legacy bundle.xtproj /
# ui-project.json), 2 = .xtflow; from tests/ and the shipped examples.
file(GLOB_RECURSE XT_FUZZ_MANIFEST_SEEDS "${PROJECT_SOURCE_DIR}/Tests/bundle/manifest.json")
file(GLOB_RECURSE XT_FUZZ_PROJECT_SEEDS "${PROJECT_SOURCE_DIR}/Tests/*.xtui" "${PROJECT_SOURCE_DIR}/Tests/bundle.xtproj"
     "${PROJECT_SOURCE_DIR}/Tests/ui-project.json" "${PROJECT_SOURCE_DIR}/Examples/*.xtui")
file(GLOB_RECURSE XT_FUZZ_FLOW_SEEDS "${PROJECT_SOURCE_DIR}/Tests/*.xtflow" "${PROJECT_SOURCE_DIR}/Examples/*.xtflow")
foreach(seed IN LISTS XT_FUZZ_MANIFEST_SEEDS)
    xt_fuzz_seed_selected(bundle_json 0 "${seed}")
endforeach()
foreach(seed IN LISTS XT_FUZZ_PROJECT_SEEDS)
    xt_fuzz_seed_selected(bundle_json 1 "${seed}")
endforeach()
foreach(seed IN LISTS XT_FUZZ_FLOW_SEEDS)
    xt_fuzz_seed_selected(bundle_json 2 "${seed}")
endforeach()

# fuzz_import: 0 = Figma file.json, 1 = Claude Design board (.dc.html).
file(GLOB_RECURSE XT_FUZZ_FIGMA_SEEDS "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/figma/file.json")
file(GLOB_RECURSE XT_FUZZ_DC_SEEDS "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/*.dc.html")
foreach(seed IN LISTS XT_FUZZ_FIGMA_SEEDS)
    xt_fuzz_seed_selected(import 0 "${seed}")
endforeach()
foreach(seed IN LISTS XT_FUZZ_DC_SEEDS)
    xt_fuzz_seed_selected(import 1 "${seed}")
endforeach()

get_property(XT_FUZZ_CORPUS_OUTPUTS GLOBAL PROPERTY XT_FUZZ_CORPUS_OUTPUTS)
add_custom_target(xt_fuzz_corpus DEPENDS ${XT_FUZZ_CORPUS_OUTPUTS})

# T-267b: every IR chunk type core knows must occur in some corpus/ir/*.xtir; fails the build and
# names the missing tags otherwise (cmake/check_corpus.cmake).
add_custom_target(xt_fuzz_corpus_check
    COMMAND ${CMAKE_COMMAND} "-DCORPUS_IR_DIR=${XT_FUZZ_CORPUS_DIR}/ir"
            "-DCHUNK_HEADER=${PROJECT_SOURCE_DIR}/Code/Runtime/core/include/xt/ir_pod.h"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_corpus.cmake"
    COMMENT "fuzz corpus: IR chunk coverage"
    VERBATIM)
add_dependencies(xt_fuzz_corpus_check xt_fuzz_corpus)

# xt_add_fuzz_target(name source libs...): an executable that is itself a libFuzzer driver
# (-fsanitize=fuzzer on top of the fuzzer-no-link,address,undefined the top-level CMakeLists.txt
# already added to every target under XT_BUILD_FUZZ).
function(xt_add_fuzz_target name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    xt_no_rtti(${name})
    xt_set_warnings(${name})
    add_dependencies(${name} xt_fuzz_corpus_check)
endfunction()

xt_add_fuzz_target(fuzz_ir fuzz_ir.cpp xt_core)
xt_add_fuzz_target(fuzz_xt fuzz_xt.cpp xt_core xt_compile)
xt_add_fuzz_target(fuzz_lottie fuzz_lottie.cpp xt_core xt_compile)

# T-267b targets.
xt_add_fuzz_target(fuzz_xtcl fuzz_xtcl.cpp xt_core)
xt_add_fuzz_target(fuzz_xts fuzz_xts.cpp xt_core xt_compile)
xt_add_fuzz_target(fuzz_svg fuzz_svg.cpp xt_core xt_compile)
xt_add_fuzz_target(fuzz_xtfx fuzz_xtfx.cpp xt_core xt_compile)
xt_add_fuzz_target(fuzz_yaml fuzz_yaml.cpp xt_bundle)
# yaml_lite.h is internal to xt_bundle (Code/Runtime/bundle/src) and includes nlohmann's json.hpp.
target_include_directories(fuzz_yaml PRIVATE "${PROJECT_SOURCE_DIR}/Code/Runtime/bundle/src")
target_link_libraries(fuzz_yaml PRIVATE xt_nlohmann)
xt_add_fuzz_target(fuzz_image fuzz_image.cpp xt_core xt_sw)
# sw_dds.h (decodeDdsBytes) is internal to xt_sw.
target_include_directories(fuzz_image PRIVATE "${PROJECT_SOURCE_DIR}/Code/Runtime/sw/src")
xt_add_fuzz_target(fuzz_bundle_json fuzz_bundle_json.cpp xt_core xt_compile xt_bundle xt_flow)
target_compile_definitions(fuzz_bundle_json
                           PRIVATE "XT_FUZZ_FLOW_DOC=\"${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/hud/hud.xt\"")
# T-400: xt_import is absent from the runtime export.
if(TARGET xt_import)
    xt_add_fuzz_target(fuzz_import fuzz_import.cpp xt_core xt_import)
endif()
# Not added (T-267b): the Editor Service JSON (Code/Editor/Service, xt_editor_service). editor/ is only
# added under XT_BUILD_EDITOR, which also configures Code/Editor/Qt and its Qt dependency; the fuzz
# configuration has no Qt, and splitting the build to change that is out of scope.
# T-430 (45 §45.4): the fuzz executables land in bin/<platform>_<arch>/<Config>/tests/ (the
# top-level default of XT_RUNTIME_OUTPUT_DIR); Tests/fuzz/run_fuzz.sh reads their directory here.
get_property(xt_fuzz_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT xt_fuzz_multi_config)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fuzz-bin-dir.txt" CONTENT "$<TARGET_FILE_DIR:fuzz_ir>\n")
endif()
