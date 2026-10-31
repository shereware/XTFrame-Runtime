# T-199: the vendored BC1/BC3/BC7 block codecs (Code/SDKs/bc7enc, MIT / public domain), built
# once and linked by xt_sw (DDS decoding) and xt_bundle (`xtframe-pack --compress` encoding).
add_library(xt_bcn STATIC
    ${PROJECT_SOURCE_DIR}/Code/SDKs/bc7enc/bc7enc.cpp
    ${PROJECT_SOURCE_DIR}/Code/SDKs/bc7enc/bc7decomp.cpp
    ${PROJECT_SOURCE_DIR}/Code/SDKs/bc7enc/rgbcx.cpp
)
target_include_directories(xt_bcn SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/Code/SDKs/bc7enc)
# rgbcx's BC1 ordering table: the small variant (rgbcx_table4_small.h) is vendored, not the large one.
target_compile_definitions(xt_bcn PRIVATE RGBCX_USE_SMALLER_TABLES=1)
if(MSVC)
    target_compile_options(xt_bcn PRIVATE /w)
else()
    target_compile_options(xt_bcn PRIVATE -w)
endif()

add_library(xt_sw STATIC
    src/sw_renderer.cpp
    src/sw_raster.cpp
    src/sw_png.cpp
    src/sw_paint.cpp
    src/sw_blend.cpp
    src/sw_layer.cpp
    src/sw_effect_math.cpp
    src/sw_effects.cpp
    src/sw_image.cpp
    src/sw_dds.cpp
    src/sw_glyphs.cpp
    src/sw_simd_dispatch.cpp
)
# Docs/tasks/T-102.md step 3: one translation unit holds the AVX2 build of the shared kernel
# templates. Only reached after a run-time __cpuid check, so the rest of the library stays at the
# SSE2 baseline.
# T-529 (Docs/design/23-host-integration.md §23.5): the scalar, SSE2 and AVX2 tables must write
# byte-identical pixels, so this unit is built without FMA: no -mfma (no fused multiply-add
# instructions at all) and -ffp-contract=off (GCC's default `fast` contraction stays off as well);
# MSVC keeps /fp:precise under /arch:AVX2, which does not contract. AVX2 only changes the encoding
# (VEX) and allows 256-bit registers; a fused a*b+c rounds once instead of twice and changed the
# bytes of effect-heavy scenes for a ~2% speed-up over SSE2.
# T-240: x86 only (an ARM build, e.g. Android arm64, has no SSE2/AVX2 table; sw_simd_dispatch.cpp
# then never references it). Emscripten reports an x86 processor but has no AVX2 (T-503), so wasm32
# stays on the scalar/SSE2-baseline kernels as well.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86|X86)$" AND NOT EMSCRIPTEN)
    target_sources(xt_sw PRIVATE src/sw_simd_avx2.cpp)
    if(MSVC)
        set_source_files_properties(src/sw_simd_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:precise")
    else()
        set_source_files_properties(src/sw_simd_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-ffp-contract=off")
    endif()
endif()
target_link_libraries(xt_sw PUBLIC xt_core)
target_link_libraries(xt_sw PRIVATE xt_bcn)
target_include_directories(xt_sw PUBLIC include)
target_include_directories(xt_sw SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/stb)
# T-261e: Code/Runtime/core/src/file_open_utf8.h (openFileUtf8, the UTF-8 file open shared by the runtime layers).
target_include_directories(xt_sw PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/core/src)
xt_enable_exceptions(xt_sw)
xt_set_warnings(xt_sw)
