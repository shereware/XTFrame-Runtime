# XT_COVERAGE (Docs/tasks/T-404.md): source-based coverage instrumentation for every target added
# after this file is included. Clang: -fprofile-instr-generate -fcoverage-mapping (read back with
# llvm-profdata + llvm-cov, see Tools/Scripts/coverage.sh). GCC: --coverage (gcov). Other compilers: error.
# Only for measurement builds (the `ninja-coverage` preset); never for shipping binaries.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
    # Like XT_BUILD_FUZZ in xt_warnings.cmake: clang's -Wextra also flags designated initializers
    # that leave later fields at their defaults. Kept a warning here (-Wno-error=, which the
    # per-target -Wextra cannot turn back into an error) so the measurement build links.
    add_compile_options(-Wno-error=missing-field-initializers)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(--coverage)
    add_link_options(--coverage)
else()
    message(FATAL_ERROR "XT_COVERAGE=ON needs clang or gcc (got ${CMAKE_CXX_COMPILER_ID})")
endif()
