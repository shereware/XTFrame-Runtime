add_executable(xtframe-compile
    compile/main.cpp
)
# xt_bundle (T-263f): the standard-symbols note reads the enclosing UI Project (UiProject::load).
target_link_libraries(xtframe-compile PRIVATE xt_core xt_compile xt_bundle)
xt_enable_exceptions(xtframe-compile)
xt_set_warnings(xtframe-compile)

# T-400: xtframe-bake, xtframe-design and xtframe-import are guarded by their directories, which the
# runtime export (Tools/Scripts/export_runtime.sh) leaves out; their tests below check `if(TARGET ...)`.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bake/main.cpp")
add_executable(xtframe-bake
    bake/main.cpp
)
# xt_compile (T-261f): only its header-only xt/compile/utf8_path.h (the UTF-8 argument paths).
target_link_libraries(xtframe-bake PRIVATE xt_core xt_compile)
xt_enable_exceptions(xtframe-bake)
xt_set_warnings(xtframe-bake)
endif()

add_executable(xtframe-pack
    pack/main.cpp
)
target_link_libraries(xtframe-pack PRIVATE xt_core xt_bundle)
xt_enable_exceptions(xtframe-pack)
xt_set_warnings(xtframe-pack)
# T-198 (Docs/design/30-ui-project.md 30.4): the standard symbol library next to the executable,
# where xtframe-pack looks for it (`<executable dir>/symbols`).
add_custom_command(TARGET xtframe-pack POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:xtframe-pack>/symbols"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/Assets/Symbols"
        "$<TARGET_FILE_DIR:xtframe-pack>/symbols"
    COMMENT "Copying the standard symbol library for xtframe-pack"
    VERBATIM
)

# T-309 (Docs/design/35-design-system.md §35.7.3): the design system on the command line. Runtime
# layer: xt_bundle only. The standard symbol library goes next to the executable as for xtframe-pack
# (the DESIGN.md Components section reads its index.json).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/design/main.cpp")
add_executable(xtframe-design
    design/main.cpp
    design/md_html.cpp
)
target_link_libraries(xtframe-design PRIVATE xt_core xt_bundle)
xt_enable_exceptions(xtframe-design)
xt_set_warnings(xtframe-design)
add_custom_command(TARGET xtframe-design POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:xtframe-design>/symbols"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/Assets/Symbols"
        "$<TARGET_FILE_DIR:xtframe-design>/symbols"
    COMMENT "Copying the standard symbol library for xtframe-design"
    VERBATIM
)

endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/import/main.cpp")
add_executable(xtframe-import
    import/main.cpp
)
target_link_libraries(xtframe-import PRIVATE xt_core xt_import)
xt_enable_exceptions(xtframe-import)
xt_set_warnings(xtframe-import)
endif()

# T-151: offline GPU-frame shape probe (FrameBuilder over a counting device, no GPU).
if(XT_BUILD_GPU AND XT_BUILD_TEXT)
    add_executable(xtframe-gpu-probe
        gpu-probe/main.cpp
    )
    target_link_libraries(xtframe-gpu-probe PRIVATE xt_core xt_bundle xt_gpu xt_sw xt_text)
    # T-159: `--gl` runs the frames through the real GlRenderer (hidden SDL2 window) and prints
    # the device's GL call counts; needs xt_gl's private gl_loader.h for the offscreen target.
    if(XT_BUILD_GL)
        target_compile_definitions(xtframe-gpu-probe PRIVATE XT_PROBE_GL)
        target_include_directories(xtframe-gpu-probe PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/gl/src)
        target_link_libraries(xtframe-gpu-probe PRIVATE xt_gl SDL2::SDL2-static)
        if(WIN32)
            # SDL2main; psapi for the process memory the T-163 tail report prints.
            target_link_libraries(xtframe-gpu-probe PRIVATE SDL2::SDL2main psapi)
        endif()
    endif()
    xt_enable_exceptions(xtframe-gpu-probe)
    xt_set_warnings(xtframe-gpu-probe)
endif()

if(XT_BUILD_SHELL)
    add_executable(xtframe-view
        view/main.cpp
    )
    # xt_bundle (T-135, T-213): Bundle and UI Project inputs are read through xt_bundle (a UI
    # Project is packed with xt::bundle::pack() before it is opened).
    target_link_libraries(xtframe-view PRIVATE xt_core xt_compile xt_sw xt_shell xt_flow xt_bundle)
    # main.cpp's --bind-texture upload uses xt_gl's private gl_loader.h directly, same pattern as
    # Code/Runtime/shell/CMakeLists.txt for app.cpp/sdl_input.cpp. T-240: not on the SW presentation path.
    if(XT_SHELL_GL)
        target_include_directories(xtframe-view PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/gl/src)
    endif()
    xt_enable_exceptions(xtframe-view)
    xt_set_warnings(xtframe-view)
endif()

if(XT_BUILD_TESTS)
    # T-267a: every CLI test runs through Tests/cmake/expect_exit.cmake, which checks the exact exit
    # code and the output (PASS_REGULAR_EXPRESSION made CTest ignore the exit code; WILL_FAIL took
    # any non-zero code, a crash included).
    include("${PROJECT_SOURCE_DIR}/Tests/cmake/expect_exit.cmake")
    xt_add_expect_exit_test(xtframe-compile_ok CODE 0 OUTPUT "wrote "
        COMMAND $<TARGET_FILE:xtframe-compile> "${PROJECT_SOURCE_DIR}/Tests/lottie/min_solid_anim.json"
                -o "${CMAKE_BINARY_DIR}/xtframe-compile-test-out/min_solid_anim.xtir")
    set_tests_properties(xtframe-compile_ok PROPERTIES FIXTURES_SETUP xtframe_bake_input)

    # A missing input is a compile error (1), not a usage error.
    xt_add_expect_exit_test(xtframe-compile_missing_input CODE 1 REJECT "wrote "
        COMMAND $<TARGET_FILE:xtframe-compile> "${CMAKE_CURRENT_SOURCE_DIR}/compile/does-not-exist.json"
                -o "${CMAKE_BINARY_DIR}/xtframe-compile-test-out/does-not-exist.xtir")

    xt_add_expect_exit_test(xtframe-compile_missing_output_flag CODE 2 OUTPUT "usage: xtframe-compile"
        COMMAND $<TARGET_FILE:xtframe-compile> "${PROJECT_SOURCE_DIR}/Tests/lottie/min_solid_anim.json")

    # T-420: --version prints three lines to stdout and exits 0, whatever else is given.
    xt_add_expect_exit_test(xtframe-compile_version CODE 0
        OUTPUT "^xtframe-compile [0-9]+\\.[0-9]+\\.[0-9]+[^a-z]+ir-format 2\\.[0-9]+[^a-z]+behavior [0-9]+"
        COMMAND $<TARGET_FILE:xtframe-compile> --version --bogus)

    # T-23: `.xt` input via compileDocument.
    xt_add_expect_exit_test(xtframe-compile_xt_ok CODE 0 OUTPUT "wrote "
        COMMAND $<TARGET_FILE:xtframe-compile> "${PROJECT_SOURCE_DIR}/Tests/xt/doc_basic.xt"
                -o "${CMAKE_BINARY_DIR}/xtframe-compile-test-out/doc_basic.xtir")

    # T-37: `xtframe-bake`, run against xtframe-compile_ok's output .xtir.
    if(TARGET xtframe-bake)
    xt_add_expect_exit_test(xtframe-bake_ok CODE 0 OUTPUT "wrote "
        COMMAND $<TARGET_FILE:xtframe-bake> "${CMAKE_BINARY_DIR}/xtframe-compile-test-out/min_solid_anim.xtir"
                -o "${CMAKE_BINARY_DIR}/xtframe-bake-test-out/min_solid_anim.xtcl" --fps 10)
    set_tests_properties(xtframe-bake_ok PROPERTIES FIXTURES_REQUIRED xtframe_bake_input)

    xt_add_expect_exit_test(xtframe-bake_missing_fps CODE 2 OUTPUT "usage: xtframe-bake"
        COMMAND $<TARGET_FILE:xtframe-bake> "${CMAKE_BINARY_DIR}/xtframe-compile-test-out/min_solid_anim.xtir"
                -o "${CMAKE_BINARY_DIR}/xtframe-bake-test-out/min_solid_anim_nofps.xtcl")
    set_tests_properties(xtframe-bake_missing_fps PROPERTIES FIXTURES_REQUIRED xtframe_bake_input)
    endif()

    # T-92: `xtframe-pack` against Tests/bundle/fixtures/sample (--no-thumbs keeps it quick), and an
    # unknown `--ui` id, which must exit 2.
    xt_add_expect_exit_test(xtframe-pack_ok CODE 0 OUTPUT "wrote 2 UI"
        COMMAND $<TARGET_FILE:xtframe-pack> "${PROJECT_SOURCE_DIR}/Tests/bundle/fixtures/sample"
                -o "${CMAKE_BINARY_DIR}/xtframe-pack-test-out/sample" --no-thumbs)

    xt_add_expect_exit_test(xtframe-pack_unknown_ui CODE 2 OUTPUT "no UI with id 'nope'"
        COMMAND $<TARGET_FILE:xtframe-pack> "${PROJECT_SOURCE_DIR}/Tests/bundle/fixtures/sample"
                -o "${CMAKE_BINARY_DIR}/xtframe-pack-test-out/sample-ui" --ui nope --no-thumbs)

    # T-420: --version prints three lines to stdout and exits 0, whatever else is given.
    xt_add_expect_exit_test(xtframe-pack_version CODE 0
        OUTPUT "^xtframe-pack [0-9]+\\.[0-9]+\\.[0-9]+[^a-z]+ir-format 2\\.[0-9]+[^a-z]+behavior [0-9]+"
        COMMAND $<TARGET_FILE:xtframe-pack> --upgrade --version)

    # T-274 (Docs/RELEASE-MAP.md §6.1): xtframe-pack is runtime layer and no longer bakes vectors.
    # `--bake-vectors` is a moved option (exit 2, pointing at xtframe-editor); an unbaked
    # <xt-offset> / <xt-merge> packs with the compiler's `vector.unbakedModifier` warning plus the
    # bake hint, and fails under --strict.
    set(T274_EXPECT "${PROJECT_SOURCE_DIR}/Tests/cmake/expect_exit.cmake")
    set(T274_UNBAKED "${PROJECT_SOURCE_DIR}/Tests/bundle/fixtures/unbaked_modifier/main.xt")
    add_test(NAME xtframe-pack_bake_vectors_moved_to_editor
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=2 "-DEXPECT_OUTPUT=moved to the editor"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-pack> "${T274_UNBAKED}"
                     -o "${CMAKE_BINARY_DIR}/xtframe-pack-test-out/t274-moved" --no-thumbs --bake-vectors)
    add_test(NAME t274_bundle_pack_unbaked_warns
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=0
                     "-DEXPECT_OUTPUT=warning: vector\\.unbakedModifier: <xt-offset>[^\n]*\\(bake it: save the project in the editor, or xtframe-editor --bake-vectors\\)"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-pack> "${T274_UNBAKED}"
                     -o "${CMAKE_BINARY_DIR}/xtframe-pack-test-out/t274-unbaked" --no-thumbs)
    add_test(NAME t274_bundle_pack_unbaked_strict
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=1 "-DEXPECT_OUTPUT=vector\\.unbakedModifier"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-pack> "${T274_UNBAKED}"
                     -o "${CMAKE_BINARY_DIR}/xtframe-pack-test-out/t274-strict" --no-thumbs --strict)

    # T-273d: a <video src> outside the project packs (exit 0) with one warning naming the path as
    # the document writes it; the video file itself is neither needed nor copied. (`.` stands for
    # the message's `"` and `;`, the latter a list separator here.)
    xt_add_expect_exit_test(t273d_bundle_pack_outside_video_warns CODE 0
        OUTPUT "video .\\.\\./outside_video_clips/intro\\.webm. is outside the project. a bundle does not carry video files, so the published UI names it by this path"
        COMMAND $<TARGET_FILE:xtframe-pack> "${PROJECT_SOURCE_DIR}/Tests/bundle/fixtures/outside_video/main.xt"
                -o "${CMAKE_BINARY_DIR}/xtframe-pack-test-out/t273d-video" --no-thumbs)

    # T-261f item 1/2: the tools read their arguments as UTF-8, so a path named in a script the ANSI
    # code page cannot spell (kana, Hangul) works too (Tests/cmake/t261f_utf8_args.cmake).
    set(T261F_SCRIPT "${PROJECT_SOURCE_DIR}/Tests/cmake/t261f_utf8_args.cmake")
    set(T261F_WORK "${CMAKE_BINARY_DIR}/t261f-utf8-args")
    add_test(NAME t261f_bundle_wmain_compile
             COMMAND ${CMAKE_COMMAND} -DCASE=compile "-DTOOL=$<TARGET_FILE:xtframe-compile>"
                     "-DWORK=${T261F_WORK}" "-DSOURCE=${PROJECT_SOURCE_DIR}" -P "${T261F_SCRIPT}")
    if(TARGET xtframe-bake)
    add_test(NAME t261f_bundle_wmain_bake
             COMMAND ${CMAKE_COMMAND} -DCASE=bake "-DTOOL=$<TARGET_FILE:xtframe-bake>"
                     "-DCOMPILE=$<TARGET_FILE:xtframe-compile>" "-DWORK=${T261F_WORK}"
                     "-DSOURCE=${PROJECT_SOURCE_DIR}" -P "${T261F_SCRIPT}")
    endif()
    if(TARGET xtframe-gpu-probe)
        add_test(NAME t261f_bundle_wmain_gpu_probe
                 COMMAND ${CMAKE_COMMAND} -DCASE=gpu_probe "-DTOOL=$<TARGET_FILE:xtframe-gpu-probe>"
                         "-DPACK=$<TARGET_FILE:xtframe-pack>" "-DWORK=${T261F_WORK}"
                         "-DSOURCE=${PROJECT_SOURCE_DIR}" -P "${T261F_SCRIPT}")
    endif()

    # T-263f item 12: a UI Project whose standard symbols were never fetched (they are not checked
    # in). xtframe-compile and xtframe-view still fail on the missing file, with the same exit
    # code, and say once per run what fetches the symbols and where to (the fixture's symbol
    # directory is shared/lib, read from its .xtui). Neither tool writes into the project. The
    # names carry "bundle" so that Tools/Scripts/dev_check.sh's `ctest -R "editor|flow|bundle"` runs them,
    # like t274_bundle_pack_* above. (`.` stands for the note's `;`, a list separator here.)
    set(T263F_EXPECT "${PROJECT_SOURCE_DIR}/Tests/cmake/expect_exit.cmake")
    set(T263F_PROJECT "${PROJECT_SOURCE_DIR}/Tests/bundle/fixtures/missing_standard_symbols")
    set(T263F_NOTE "error: failed to read file .*note: the project's standard symbols are not checked in. open the project in xtframe-editor once, or run xtframe-pack on it, to fetch them into [^\n]*missing_standard_symbols/shared/lib\n")
    set(T263F_TWICE "note: the project's standard symbols.*note: the project's standard symbols")
    add_test(NAME t263f_bundle_project_compile_symbols_note
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=1 "-DEXPECT_OUTPUT=${T263F_NOTE}"
                     "-DREJECT_OUTPUT=${T263F_TWICE}" -P "${T263F_EXPECT}" --
                     $<TARGET_FILE:xtframe-compile> "${T263F_PROJECT}/ui/main/main.xt"
                     -o "${CMAKE_BINARY_DIR}/t263f-test-out/main.xtir")
    if(XT_BUILD_SHELL)
        # The document by itself, and the whole project (packed into a scratch Bundle first: the
        # diagnostics of both UIs, one note).
        add_test(NAME t263f_bundle_project_view_symbols_note
                 COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=1 "-DEXPECT_OUTPUT=${T263F_NOTE}"
                         "-DREJECT_OUTPUT=${T263F_TWICE}" -P "${T263F_EXPECT}" --
                         $<TARGET_FILE:xtframe-view> "${T263F_PROJECT}/ui/main/main.xt" --headless --frames 1)
        add_test(NAME t263f_bundle_project_view_packed_symbols_note
                 COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=1 "-DEXPECT_OUTPUT=${T263F_NOTE}"
                         "-DREJECT_OUTPUT=${T263F_TWICE}" -P "${T263F_EXPECT}" --
                         $<TARGET_FILE:xtframe-view> "${T263F_PROJECT}" --headless --frames 1)
    endif()

    # T-263f item 8: a document with <particles> cannot be baked (the .xtcl format stores no
    # particle pool): xtframe-bake stops with exit code 1 and leaves no output file behind.
    if(TARGET xtframe-bake)
    set(T263F_XTIR "${CMAKE_BINARY_DIR}/t263f-test-out/ui_particles.xtir")
    set(T263F_XTCL "${CMAKE_BINARY_DIR}/t263f-test-out/ui_particles.xtcl")
    add_test(NAME t263f_bundle_bake_particles_compile
             COMMAND xtframe-compile "${PROJECT_SOURCE_DIR}/Tests/golden/ui/ui_particles.xt" -o "${T263F_XTIR}")
    set_tests_properties(t263f_bundle_bake_particles_compile PROPERTIES
                         PASS_REGULAR_EXPRESSION "wrote " FIXTURES_SETUP t263f_particles_xtir)
    # T-263g item 11: the tool prints `<output file>: <BakedWriter::lastError()>`.
    add_test(NAME t263f_bundle_bake_particles_refused
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=1
                     "-DEXPECT_OUTPUT=ui_particles\\.xtcl: baked frames cannot store DrawParticles commands yet"
                     "-DREJECT_OUTPUT=wrote " -P "${T263F_EXPECT}" --
                     $<TARGET_FILE:xtframe-bake> "${T263F_XTIR}" -o "${T263F_XTCL}" --fps 10)
    set_tests_properties(t263f_bundle_bake_particles_refused PROPERTIES
                         FIXTURES_REQUIRED t263f_particles_xtir FIXTURES_SETUP t263f_particles_baked)
    # `cmake -E md5sum` fails on a file that does not exist: the refused bake left none.
    add_test(NAME t263f_bundle_bake_particles_no_file COMMAND ${CMAKE_COMMAND} -E md5sum "${T263F_XTCL}")
    set_tests_properties(t263f_bundle_bake_particles_no_file PROPERTIES
                         WILL_FAIL TRUE FIXTURES_REQUIRED t263f_particles_baked)
    endif()

    if(TARGET xtframe-import)
    # T-107: `xtframe-import figma` against the wireframe fixture. Since T-118 the fixture has a second
    # page (`Overlays`) whose top-level frame has the same name, so two UIs are imported.
    xt_add_expect_exit_test(xtframe-import_figma_wireframe CODE 0 OUTPUT "imported 2 UIs"
        COMMAND $<TARGET_FILE:xtframe-import> figma "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/figma/wireframe"
                -o "${CMAKE_BINARY_DIR}/xtframe-import-test-out/wireframe" --force)
    set_tests_properties(xtframe-import_figma_wireframe PROPERTIES FIXTURES_SETUP xtframe_import_reimport)

    # T-112: the same dump imported into the same bundle again. Nothing changed on either side, so
    # the re-import merge must be silent: exit 0 and no conflict (Docs/design/24-design-import.md
    # §24.4 point 5).
    xt_add_expect_exit_test(xtframe-import_figma_reimport CODE 0 OUTPUT "0 conflicts"
        COMMAND $<TARGET_FILE:xtframe-import> figma "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/figma/wireframe"
                -o "${CMAKE_BINARY_DIR}/xtframe-import-test-out/wireframe")
    set_tests_properties(xtframe-import_figma_reimport PROPERTIES FIXTURES_REQUIRED xtframe_import_reimport)

    xt_add_expect_exit_test(xtframe-import_missing_mode CODE 2 OUTPUT "usage: xtframe-import"
        COMMAND $<TARGET_FILE:xtframe-import> "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/figma/wireframe")

    # T-267a (review L20): `dc` has been implemented since T-113; this used to be
    # xtframe-import_dc_not_implemented (any non-zero exit passed). A Figma dump fed to `dc` has no
    # project/canvas.json: the import fails (exit 1) with that status and imports no UI.
    xt_add_expect_exit_test(xtframe-import_dc_figma_dump_rejected CODE 1
        OUTPUT "import failed \\(kNotFound\\)" REJECT "imported [0-9]"
        COMMAND $<TARGET_FILE:xtframe-import> dc "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/figma/wireframe"
                -o "${CMAKE_BINARY_DIR}/xtframe-import-test-out/dc")
    endif()

    # T-56: `xtframe-view --flow` against Examples/desktop_app's ui/app.xt + ui/app.xtflow, headless
    # (no display in CI) for a few frames.
    if(XT_BUILD_SHELL)
        set(XT_VIEW $<TARGET_FILE:xtframe-view>)
        xt_add_expect_exit_test(xtframe-view_flow CODE 0 OUTPUT "headless run complete"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Examples/desktop_app/ui/app.xt"
                    --flow "${PROJECT_SOURCE_DIR}/Examples/desktop_app/ui/app.xtflow"
                    --headless --frames 3)

        # T-125: scripted interaction against Examples/demos/game-ui's HUD. `#fire.Click` on
        # frame 5 decrements the `ammo` counter node once, and its toString feeds `#ammo`'s text.
        # T-240 item 8: on the SW presentation path (XT_SHELL_GL=OFF) a windowed frame (hidden
        # window: streaming texture, damage upload) is byte-identical to the headless SW render.
        # The GL comparison (tolerance 2 except antialiased edges) is a manual check, see
        # Code/Runtime/platforms/android/README.md.
        if(NOT XT_SHELL_GL)
            foreach(xt_sw_ui hud/hud pause/pause)
                get_filename_component(xt_sw_name "${xt_sw_ui}" NAME)
                set(xt_sw_src "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/${xt_sw_ui}.xt")
                if(xt_sw_name STREQUAL "pause")
                    set(xt_sw_src "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/menus/pause.xt")
                endif()
                set(xt_sw_out "${CMAKE_BINARY_DIR}/xtframe-view-sw-test-out")
                file(MAKE_DIRECTORY "${xt_sw_out}")
                add_test(NAME xtframe-view_sw_window_${xt_sw_name}
                         COMMAND xtframe-view "${xt_sw_src}" --no-flow --hidden
                                 --screenshot "${xt_sw_out}/${xt_sw_name}_window.png")
                add_test(NAME xtframe-view_sw_headless_${xt_sw_name}
                         COMMAND xtframe-view "${xt_sw_src}" --no-flow --headless --frames 1
                                 --screenshot "${xt_sw_out}/${xt_sw_name}_headless.png")
                set_tests_properties(xtframe-view_sw_window_${xt_sw_name} xtframe-view_sw_headless_${xt_sw_name}
                                     PROPERTIES FIXTURES_SETUP xt_sw_present_${xt_sw_name})
                add_test(NAME xtframe-view_sw_compare_${xt_sw_name}
                         COMMAND ${CMAKE_COMMAND} -E compare_files "${xt_sw_out}/${xt_sw_name}_window.png"
                                 "${xt_sw_out}/${xt_sw_name}_headless.png")
                set_tests_properties(xtframe-view_sw_compare_${xt_sw_name}
                                     PROPERTIES FIXTURES_REQUIRED xt_sw_present_${xt_sw_name})
            endforeach()
        endif()

        xt_add_expect_exit_test(xtframe-view_event_click CODE 0 OUTPUT "text=29"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/hud/hud.xt"
                    --flow "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/hud/hud.xtflow"
                    --headless --frames 10 --event "5 #fire.Click" --print-text ammo)

        # T-359: a click scripted on a zero-area element fires its triggers directly, with a warning.
        xt_add_expect_exit_test(xtframe-view_event_click_zero_area CODE 0
            OUTPUT "text=7.*warning: #zero has no hit area. dispatching click directly"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Tests/xt/view_zero_click.xt"
                    --headless --frames 6 --event "3 #zero.Click" --print-text label)

        # T-392 (XF-078, 27 §27.3.2 XF-073): --event parse errors name the spec, the reason and the
        # element form; --print-text names a missing id; --strict prints the runtime warnings.
        xt_add_expect_exit_test(xtframe-view_event_parse_error CODE 2
            OUTPUT "cannot parse --event .5 box.Click.: the target .box.Click. does not start with .#. .expected "
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Tests/xt/view_zero_viewport.xt"
                    --headless --frames 1 --event "5 box.Click")
        xt_add_expect_exit_test(xtframe-view_print_text_missing CODE 0 OUTPUT "--print-text: no element 'nosuch'"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Tests/xt/view_zero_viewport.xt"
                    --headless --frames 1 --print-text nosuch)
        xt_add_expect_exit_test(xtframe-view_strict_zero_viewport CODE 0
            OUTPUT "warning: virtualized repeat .Item. has a zero-height viewport"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Tests/xt/view_zero_viewport.xt"
                    --headless --frames 3 --strict --port "items=[{\"name\":\"a\"},{\"name\":\"b\"}]")

        xt_add_expect_exit_test(xtframe-view_dump_flow_values CODE 0 OUTPUT "ammo\\.value = "
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/hud/hud.xt"
                    --flow "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/hud/hud.xtflow"
                    --headless --frames 3 --dump-flow-values)

        # --screenshot works under --headless (software buffer, no GL); the relative path resolves
        # against the working directory, which WORKING_DIRECTORY pins here.
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/xtframe-view-test-out")
        xt_add_expect_exit_test(xtframe-view_headless_screenshot CODE 0 OUTPUT "wrote screenshot "
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/xtframe-view-test-out"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/hud/hud.xt"
                    --headless --frames 2 --width 320 --height 200 --screenshot rel.png)

        xt_add_expect_exit_test(xtframe-view_help CODE 0 OUTPUT "usage: xtframe-view" COMMAND ${XT_VIEW} --help)
        # T-420: --version prints three lines to stdout and exits 0, whatever else is given.
        xt_add_expect_exit_test(xtframe-view_version CODE 0
            OUTPUT "^xtframe-view [0-9]+\\.[0-9]+\\.[0-9]+[^a-z]+ir-format 2\\.[0-9]+[^a-z]+behavior [0-9]+"
            COMMAND ${XT_VIEW} --width nope --version)

        # T-261f item 2: a document, a screenshot and a Bundle in a directory the ANSI code page
        # cannot spell.
        add_test(NAME t261f_bundle_wmain_view
                 COMMAND ${CMAKE_COMMAND} -DCASE=view "-DTOOL=${XT_VIEW}" "-DPACK=$<TARGET_FILE:xtframe-pack>"
                         "-DWORK=${T261F_WORK}" "-DSOURCE=${PROJECT_SOURCE_DIR}" -P "${T261F_SCRIPT}")

        # An unknown option is a usage error: exit 2.
        xt_add_expect_exit_test(xtframe-view_unknown_option CODE 2 OUTPUT "unknown option '--bogus'"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/hud/hud.xt" --bogus)

        if(TARGET xtframe-import)
        # T-135: flow auto-load. The fixture is a real imported bundle, built here by xtframe-import
        # so the test exercises exactly what a user gets out of the importer. The importer's own
        # `difficulty-select.xtflow` is then overwritten with the v2 board (the one whose
        # `#diff-veteran.PointerEnter` writes "VETERAN" into `#diff-tab`).
        add_test(NAME t135_fixture_import
                 COMMAND xtframe-import dc "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/dc/difficulty-select"
                         -o "${CMAKE_BINARY_DIR}/t135-ds" --force)
        set_tests_properties(t135_fixture_import PROPERTIES FIXTURES_SETUP t135_import)

        add_test(NAME t135_fixture_flow
                 COMMAND ${CMAKE_COMMAND} -E copy
                         "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/dc/difficulty-select/difficulty-select.v2.xtflow"
                         "${CMAKE_BINARY_DIR}/t135-ds/difficulty-select/difficulty-select.xtflow")
        set_tests_properties(t135_fixture_flow PROPERTIES
                             FIXTURES_SETUP t135_bundle
                             FIXTURES_REQUIRED t135_import)

        # The `.xt` is opened with no --flow at all: the sibling `.xtflow` has to be found, loaded
        # and ticked, or `#diff-tab` never says VETERAN.
        xt_add_expect_exit_test(t135_flow_auto_load CODE 0 OUTPUT "text=VETERAN"
            COMMAND ${XT_VIEW} "${CMAKE_BINARY_DIR}/t135-ds/difficulty-select/difficulty-select.xt"
                    --headless --frames 6 --event "2 #diff-veteran.PointerEnter"
                    --print-text diff-tab)
        set_tests_properties(t135_flow_auto_load PROPERTIES FIXTURES_REQUIRED t135_bundle)

        # ... and --no-flow turns exactly that off again (the text is still printed, just not VETERAN).
        xt_add_expect_exit_test(t135_no_flow CODE 0 OUTPUT "text=" REJECT "text=VETERAN"
            COMMAND ${XT_VIEW} "${CMAKE_BINARY_DIR}/t135-ds/difficulty-select/difficulty-select.xt"
                    --headless --frames 6 --event "2 #diff-veteran.PointerEnter"
                    --print-text diff-tab --no-flow)
        set_tests_properties(t135_no_flow PROPERTIES FIXTURES_REQUIRED t135_bundle)

        # The imported UI Project as the input (T-214: `<name>.xtui`, the canvas has no name so it is
        # "Imported Canvas"; the importer no longer writes bundle.xtproj): it is packed, the single
        # UI is picked, and the flow the manifest lists is loaded (the announcement line proves it).
        xt_add_expect_exit_test(t135_xtproj_input CODE 0 OUTPUT "loaded flow"
            COMMAND ${XT_VIEW} "${CMAKE_BINARY_DIR}/t135-ds/Imported Canvas.xtui" --headless --frames 2)
        set_tests_properties(t135_xtproj_input PROPERTIES FIXTURES_REQUIRED t135_bundle)

        # T-138: opening an imported UI Project (T-214: through `HUD Basic.xtui`, i.e. packed into
        # a Bundle first) resolves its assets, so the screenshot is byte-identical to opening the
        # UI's `.xt` directly. hud-basic, not the
        # sheet's authored-hud: only hud-basic's board carries images (one SVG, one PNG).
        add_test(NAME t138_fixture_import
                 COMMAND xtframe-import dc "${PROJECT_SOURCE_DIR}/Tests/import/fixtures/dc/hud-basic"
                         -o "${CMAKE_BINARY_DIR}/t138-hud" --force)
        set_tests_properties(t138_fixture_import PROPERTIES FIXTURES_SETUP t138_import)

        xt_add_expect_exit_test(t138_shot_bundle CODE 0 OUTPUT "wrote screenshot "
            COMMAND ${XT_VIEW} "${CMAKE_BINARY_DIR}/t138-hud/HUD Basic.xtui"
                    --headless --screenshot "${CMAKE_BINARY_DIR}/t138-hud/a.png")
        xt_add_expect_exit_test(t138_shot_xt CODE 0 OUTPUT "wrote screenshot "
            COMMAND ${XT_VIEW} "${CMAKE_BINARY_DIR}/t138-hud/hud-basic/hud-basic.xt"
                    --headless --no-flow --screenshot "${CMAKE_BINARY_DIR}/t138-hud/b.png")
        set_tests_properties(t138_shot_bundle t138_shot_xt PROPERTIES
                             FIXTURES_REQUIRED t138_import
                             FIXTURES_SETUP t138_shots)

        add_test(NAME t138_shots_identical
                 COMMAND ${CMAKE_COMMAND} -E compare_files
                         "${CMAKE_BINARY_DIR}/t138-hud/a.png" "${CMAKE_BINARY_DIR}/t138-hud/b.png")
        set_tests_properties(t138_shots_identical PROPERTIES FIXTURES_REQUIRED t138_shots)
        endif()

        # T-213 (Docs/design/22-bundle.md §22.11.2): every input form, headless, against the game-ui
        # demo's HUD. `#fire.Click` on frame 5 must still count the ammo down to 29, which proves
        # the Bundle's flow was loaded too.
        set(T213_DEMO "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui")
        set(T213_BUNDLE "${CMAKE_BINARY_DIR}/t213-view/game-ui")
        add_test(NAME t213_view_fixture_pack
                 COMMAND xtframe-pack "${T213_DEMO}" -o "${T213_BUNDLE}" --no-thumbs)
        set_tests_properties(t213_view_fixture_pack PROPERTIES FIXTURES_SETUP t213_bundle)
        set(T213_CLICK --headless --frames 10 --event "5 #fire.Click" --print-text ammo)
        xt_add_expect_exit_test(t213_view_project_dir CODE 0 OUTPUT "text=29"
            COMMAND ${XT_VIEW} "${T213_DEMO}" --ui hud ${T213_CLICK})
        xt_add_expect_exit_test(t213_view_ui_project_json CODE 0 OUTPUT "text=29"
            COMMAND ${XT_VIEW} "${T213_DEMO}/XTFrame Game UI demo.xtui" --ui hud ${T213_CLICK})
        xt_add_expect_exit_test(t213_view_bundle_dir CODE 0 OUTPUT "text=29"
            COMMAND ${XT_VIEW} "${T213_BUNDLE}" --ui hud ${T213_CLICK})
        xt_add_expect_exit_test(t213_view_manifest CODE 0 OUTPUT "text=29"
            COMMAND ${XT_VIEW} "${T213_BUNDLE}/manifest.json" --ui hud ${T213_CLICK})
        xt_add_expect_exit_test(t213_view_project_alias CODE 0 OUTPUT "text=29"
            COMMAND ${XT_VIEW} --project "${T213_DEMO}" --ui hud ${T213_CLICK})
        set_tests_properties(t213_view_bundle_dir t213_view_manifest PROPERTIES
                             FIXTURES_REQUIRED t213_bundle)
        # A compiled `.xtir` of that Bundle (asset root = the Bundle), and an old `bundle.xtproj`
        # (upgraded in memory, packed like a UI Project).
        xt_add_expect_exit_test(t213_view_xtir CODE 0 OUTPUT "headless run complete"
            COMMAND ${XT_VIEW} "${T213_BUNDLE}/ui/hud.xtir" --root "${T213_BUNDLE}" --headless --frames 2)
        set_tests_properties(t213_view_xtir PROPERTIES FIXTURES_REQUIRED t213_bundle)
        xt_add_expect_exit_test(t213_view_xtproj CODE 0 OUTPUT "headless run complete"
            COMMAND ${XT_VIEW} "${PROJECT_SOURCE_DIR}/Tests/bundle/fixtures/sample/bundle.xtproj"
                    --ui hud --no-flow --headless --frames 2)
        # Several UIs and no --ui: listed, exit 2.
        xt_add_expect_exit_test(t213_view_needs_ui CODE 2 OUTPUT "pass --ui <id>"
            COMMAND ${XT_VIEW} "${T213_DEMO}" --headless --frames 1)
    endif()
endif()
