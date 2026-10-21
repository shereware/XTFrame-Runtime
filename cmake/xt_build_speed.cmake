# Build-time settings (docs/tasks/T-361.md). None of them changes what is compiled into a binary:
# they only change how many translation units the compiler sees and how the linker writes symbols.
#
# XT_UNITY_BUILD: CMake unity builds for the large E/R libraries. The vs2022-editor preset turns it
# on; the clang syntax check and the WSL GCC build of tools/local_ci.sh keep it OFF (the default),
# so every .cpp is still compiled on its own there and a missing include still shows up.
option(XT_UNITY_BUILD "Unity builds for the large libraries (batches of 16 .cpp)" OFF)

# xt_unity_build(<target>): unity build for <target> when XT_UNITY_BUILD is ON.
function(xt_unity_build target)
    if(XT_UNITY_BUILD)
        set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 16)
        # A batch of 16 files exceeds the default COFF section limit (C1128).
        if(XT_MSVC_FRONTEND)
            target_compile_options(${target} PRIVATE /bigobj)
        endif()
    endif()
endfunction()

# XT_PCH: precompiled headers for xt_core, xt_editor_service, xt_editor_qt and their test
# executables. ON in the vs2022-editor preset, OFF elsewhere for the same reason as XT_UNITY_BUILD.
option(XT_PCH "Precompiled headers for the large libraries and their tests" OFF)

# xt_precompile(<target> <header>...): precompiled headers for <target> when XT_PCH is ON.
function(xt_precompile target)
    if(XT_PCH)
        target_precompile_headers(${target} PRIVATE ${ARGN})
    endif()
endfunction()

# xt_precompile_like(<target> <from>): <target> gets its own precompiled header made of the same
# headers as <from>'s when XT_PCH is ON. (REUSE_FROM is not usable: MSVC warns C4651 when the test
# target lacks one of the library's private definitions, and /WX makes that an error.)
function(xt_precompile_like target from)
    if(XT_PCH)
        get_target_property(_headers ${from} PRECOMPILE_HEADERS)
        if(_headers)
            target_precompile_headers(${target} PRIVATE ${_headers})
        endif()
    endif()
endfunction()

# xt_fast_debug_link(<target>): Debug links of the big executables (the editor and the Qt test
# binaries) write a FASTLINK PDB (the type records stay in the object files' PDBs instead of being
# merged) and keep incremental linking (CMake's Debug default, stated here so it is not lost).
# Each target already has its own compile PDB (the generator's per-target vc<ver>.pdb), so /Zi
# does not make targets wait on one shared PDB.
function(xt_fast_debug_link target)
    if(XT_MSVC_FRONTEND)
        target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:/DEBUG:FASTLINK> $<$<CONFIG:Debug>:/INCREMENTAL>)
    endif()
endfunction()
