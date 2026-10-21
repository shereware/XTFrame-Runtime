// T-396 (docs/design/23-host-integration.md §23.ae): XTFrame's vendored nlohmann lives under its own
// ABI namespace, nlohmann::xtframe_abi (cmake/xt_nlohmann.cmake), so a host's own nlohmann copy
// never shares a mangled name with it. xt_tests is built without RTTI, so the runtime check reads
// the compiler's function signature string instead of typeid().name().
#include <json.hpp>
#include <string_view>
#include <type_traits>

#include "doctest.h"

namespace {

template <typename T>
std::string_view jsonAbiSignatureOf() {
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

}  // namespace

static_assert(std::is_same_v<nlohmann::json, nlohmann::xtframe_abi::json>,
              "nlohmann::json must be the xtframe_abi-tagged copy");
static_assert(std::is_same_v<nlohmann::ordered_json, nlohmann::xtframe_abi::ordered_json>,
              "nlohmann::ordered_json must be the xtframe_abi-tagged copy");

TEST_CASE("vendored nlohmann::json is in the xtframe_abi namespace") {
    const std::string_view sig = jsonAbiSignatureOf<nlohmann::json>();
    CHECK(sig.find("nlohmann::xtframe_abi::basic_json") != std::string_view::npos);
    // The library's default tag (json_abi_v3_11_3) must not appear: that is the name an untagged
    // host copy uses.
    CHECK(sig.find("json_abi") == std::string_view::npos);

    const nlohmann::json j = nlohmann::json::parse(R"({"a":[1,2,3]})");
    CHECK(j.at("a").size() == 3);
}
