# Warning and exception/RTTI toggles shared by all XTFrame targets.
# See Docs/design/14-coding-standards.md section 14.1
#
# Uses the CMAKE_CXX_COMPILER_FRONTEND_VARIANT variable (CMake >= 3.14) rather than the
# $<CXX_COMPILER_FRONTEND_VARIANT:...> generator expression, which only exists in CMake >= 3.30
# (the WSL toolchain ships 3.28). clang-cl reports COMPILER_ID=Clang with FRONTEND_VARIANT=MSVC
# and therefore gets MSVC-style flags; regular clang/gcc get GNU-style flags.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR (MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT))
    set(XT_MSVC_FRONTEND ON)
else()
    set(XT_MSVC_FRONTEND OFF)
endif()

# xt_set_warnings(target): enable strict warnings-as-errors for `target`.
function(xt_set_warnings target)
    if(XT_MSVC_FRONTEND)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion)
        # Clang's -Wextra, unlike GCC/MSVC, additionally warns on a designated-initializer struct
        # literal that leaves later fields at their in-class defaults (Tools/Scripts/local_ci.sh's clang
        # step disables it the same way). First hit by XT_BUILD_FUZZ (Docs/tasks/T-39.md); T-402
        # extends it to every Clang build (the CI linux Clang leg and the sanitizers job).
        # Appended after -Wextra on the same target so it is not re-enabled by it.
        if(XT_BUILD_FUZZ OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
        endif()
        # T-503 (Docs/design/24-design-import.md §24.13): wasm32 is a 32-bit target, so iterator
        # arithmetic with a uint32_t index (`begin() + count`) converts to a 32-bit difference_type and
        # -Wsign-conversion fires in some 50 places that are fine on every 64-bit CI leg. The check
        # stays on for the native builds; the wasm build only drops this one warning.
        if(EMSCRIPTEN)
            target_compile_options(${target} PRIVATE -Wno-sign-conversion)
        endif()
    endif()
endfunction()

# xt_no_exceptions_rtti(target): disable C++ exceptions and RTTI for `target`.
function(xt_no_exceptions_rtti target)
    if(XT_MSVC_FRONTEND)
        target_compile_options(${target} PRIVATE /EHs-c- /GR-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    endif()
endfunction()

# xt_no_rtti(target): disable RTTI only; every non-editor test target calls it so it is built like
# xt_core (Docs/tasks/T-168.md point 1). A test class deriving from a core interface whose key
# function lives in xt_core (e.g. AssetResolver::imageView) otherwise references
# `typeinfo for xt::<Base>`, which the Itanium ABI (GCC/Clang) only emits next to that key function,
# i.e. never under xt_core's -fno-rtti. Exceptions stay on: doctest's REQUIRE relies on them.
function(xt_no_rtti target)
    if(XT_MSVC_FRONTEND)
        target_compile_options(${target} PRIVATE /GR-)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endfunction()

# xt_enable_exceptions(target): MSVC needs an explicit /EHsc once the default has been stripped.
function(xt_enable_exceptions target)
    if(XT_MSVC_FRONTEND)
        target_compile_options(${target} PRIVATE /EHsc)
    elseif(EMSCRIPTEN)
        # T-503: Emscripten compiles with exceptions off unless asked; native wasm exception
        # handling (-fwasm-exceptions, Node 17+) needs the flag at compile and link time.
        target_compile_options(${target} PRIVATE -fwasm-exceptions)
        target_link_options(${target} PRIVATE -fwasm-exceptions)
    endif()
endfunction()
