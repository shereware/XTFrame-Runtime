# The vendored nlohmann/json 3.11.3 (third_party/nlohmann/json.hpp, single header) under its own ABI
# namespace (docs/design/23-host-integration.md §23.ae, docs/tasks/T-396.md). A host engine may ship
# its own copy of nlohmann (often untagged, often built with JSON_NOEXCEPTION); every XTFrame target
# therefore sees nlohmann's types as nlohmann::xtframe_abi::*, so the two copies never share a
# mangled name and link side by side without an ODR clash. `nlohmann::json` still names XTFrame's
# copy in XTFrame code (the namespace is inline).
#
# 3.11.3 builds NLOHMANN_JSON_NAMESPACE_BEGIN / _END from its fixed `json_abi<tags>_v<version>`
# pieces and only honours NLOHMANN_JSON_NAMESPACE for qualified references, so defining that one
# macro alone opens one namespace and names another; all three macros are defined here.
#
# Every target that includes json.hpp links this INTERFACE target (never the include directory
# alone), so all of them agree on the namespace. Defines the INTERFACE target `xt_nlohmann`.
add_library(xt_nlohmann INTERFACE)
target_include_directories(xt_nlohmann SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party/nlohmann)
target_compile_definitions(xt_nlohmann INTERFACE
    "NLOHMANN_JSON_NAMESPACE=nlohmann::xtframe_abi"
    "NLOHMANN_JSON_NAMESPACE_BEGIN=namespace nlohmann { inline namespace xtframe_abi {"
    "NLOHMANN_JSON_NAMESPACE_END=} }")
