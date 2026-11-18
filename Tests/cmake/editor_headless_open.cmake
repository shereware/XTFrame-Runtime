# CTest helper (T-712): `xtframe-editor --headless` on a scratch copy of a UI Project, opened as a
# lone `.xt`, as its `.xtui` or as its directory, must run its frames, print `done`, exit 0 -- and
# leave the copy exactly as it was (no sidecar, no standard symbols copied in, nothing else written
# next to the project). Invoked as
#   cmake -DEDITOR=<xtframe-editor> -DSOURCE=<ui project dir> -DWORK=<scratch dir> -DOPEN=<path in WORK>
#         -P editor_headless_open.cmake
# OPEN is relative to WORK; "." opens the directory itself. The copy drops `shared/symbols/*.xt` and
# `out/` first, so a project whose UIs link standard symbols is one the windowed editor's open would
# write into (EditorSession::openLoadedProject(), bundle::pullStandardSymbols()).
foreach(var EDITOR SOURCE WORK OPEN)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "editor_headless_open.cmake: -D${var}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${SOURCE}/" DESTINATION "${WORK}")
file(GLOB xt_symbols "${WORK}/shared/symbols/*.xt")
if(xt_symbols)
    file(REMOVE ${xt_symbols})
endif()
file(REMOVE_RECURSE "${WORK}/out")

# Every file and directory under WORK, with the SHA-256 of each file.
function(xt_snapshot out)
    file(GLOB_RECURSE entries LIST_DIRECTORIES true RELATIVE "${WORK}" "${WORK}/*")
    list(SORT entries)
    set(lines "")
    foreach(entry IN LISTS entries)
        if(IS_DIRECTORY "${WORK}/${entry}")
            string(APPEND lines "dir  ${entry}\n")
        else()
            file(SHA256 "${WORK}/${entry}" hash)
            string(APPEND lines "file ${entry} ${hash}\n")
        endif()
    endforeach()
    set(${out} "${lines}" PARENT_SCOPE)
endfunction()

xt_snapshot(xt_before)
if(OPEN STREQUAL ".")
    set(xt_target "${WORK}")
else()
    set(xt_target "${WORK}/${OPEN}")
endif()
execute_process(COMMAND "${EDITOR}" "${xt_target}" --headless --frames 3
                RESULT_VARIABLE xt_code OUTPUT_VARIABLE xt_out ERROR_VARIABLE xt_err)
set(xt_all "${xt_out}${xt_err}")
message("${xt_all}")
if(NOT "${xt_code}" STREQUAL "0")
    message(FATAL_ERROR "expected exit code 0, got ${xt_code}")
endif()
if(NOT xt_all MATCHES "(^|\n)done\r?\n")
    message(FATAL_ERROR "no 'done' line")
endif()
xt_snapshot(xt_after)
if(NOT xt_before STREQUAL xt_after)
    message("before:\n${xt_before}")
    message("after:\n${xt_after}")
    message(FATAL_ERROR "the headless run changed the project directory")
endif()
file(REMOVE_RECURSE "${WORK}")
