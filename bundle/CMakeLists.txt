add_library(xt_bundle STATIC
    src/bundle_json.cpp
    src/project_file.cpp
    src/bundle.cpp
    src/pack.cpp
    src/manifest_writer.cpp
    src/localization.cpp
    src/ui_project.cpp
    src/assets_manifest.cpp
    src/yaml_lite.cpp
    src/gen_ports.cpp
    src/atlas_pack.cpp
    src/texture_compress.cpp
    src/font_subset.cpp
    src/symbol_contracts.cpp
    src/file_status.cpp
    src/color_math.cpp
    src/design_system.cpp
    src/design_io.cpp
    src/design_graph.cpp
    src/design_diff.cpp
    src/design_color.cpp
    src/design_audit.cpp
    src/design_doc.cpp
    src/design_export.cpp
    src/asset_meta.cpp
    src/asset_import.cpp
    src/asset_hygiene.cpp
    src/project_runtime.cpp
    src/project_edit.cpp
)
# pack() (T-92) compiles each UI (xt_compile), reflects its connection points (xt_flow) and
# renders its thumbnail with the software backend (xt_sw, always built).
target_link_libraries(xt_bundle PUBLIC xt_core xt_compile xt_flow xt_sw)
# T-199: BC1/BC3/BC7 block encoders for `xtframe-pack --compress` (third_party/bc7enc, built by sw/).
target_link_libraries(xt_bundle PRIVATE xt_bcn)
if(XT_BUILD_TEXT)
    # Thumbnails get real glyphs when the FreeType stack is in the build; otherwise pack() falls
    # back to NullTextShaper. See docs/tasks/T-92.md step 5.
    target_link_libraries(xt_bundle PRIVATE xt_text)
    target_compile_definitions(xt_bundle PRIVATE XT_BUNDLE_HAS_TEXT)
endif()
target_include_directories(xt_bundle PUBLIC include)
# T-305: PUBLIC because include/xt/bundle/design_system.h keeps unknown keys as nlohmann::json
# (T-396: the xtframe_abi-tagged copy, cmake/xt_nlohmann.cmake).
target_link_libraries(xt_bundle PUBLIC xt_nlohmann)
xt_enable_exceptions(xt_bundle)
xt_set_warnings(xt_bundle)
xt_unity_build(xt_bundle)  # T-361
