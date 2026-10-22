// T-396: XTFrame side of the nlohmann coexistence check (see json_coexist.h).
#include "json_coexist.h"

#include <cstdio>
#include <string_view>
#include <type_traits>

#include "host_json/host_json.h"
// The public bundle header that exposes nlohmann::json (DesignSystem's unknown keys).
#include "xt/bundle/design_system.h"

namespace {

template <typename T>
const char* xtSignatureOf() {
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

}  // namespace

bool runJsonCoexistDemo() {
    // XTFrame's copy: the type DesignSystem::tokensExtra holds.
    xt::bundle::design::DesignSystem ds;
    ds.tokensExtra = nlohmann::json::parse(R"({"tokens":["color.bg","color.fg","space.s"]})", nullptr, false);
    const bool xtOk = ds.tokensExtra.is_object() && ds.tokensExtra.contains("tokens") &&
                      ds.tokensExtra["tokens"].size() == 3;

    // The engine's copy, in a library built without XTFrame's definitions.
    const bool hostOk = hostJsonArraySize(R"({"entities":[1,2,3,4]})", "entities") == 4;

    const std::string_view xtName = xtSignatureOf<nlohmann::json>();
    const std::string_view hostName = hostJsonTypeName();
    // Clang's __PRETTY_FUNCTION__ leaves inline namespaces out (it prints nlohmann::basic_json<>),
    // so the tag is also checked on the type itself: nlohmann::xtframe_abi::json only names the
    // same type when XTFrame's copy really lives in the xtframe_abi inline namespace.
    const bool xtTagged = xtName.find("nlohmann::xtframe_abi::") != std::string_view::npos ||
                          std::is_same_v<nlohmann::json, nlohmann::xtframe_abi::json>;
    const bool hostUntagged = hostName.find("nlohmann::") != std::string_view::npos &&
                              hostName.find("xtframe_abi") == std::string_view::npos;

    std::printf("xtframe json: %s (%s)\n", xtOk ? "parsed" : "FAILED",
                xtTagged ? "xtframe_abi" : "NOT TAGGED");
    std::printf("engine json:  %s (%s)\n", hostOk ? "parsed" : "FAILED",
                hostUntagged ? "untagged" : "TAGGED");
    std::printf("xtframe type: %.*s\n", static_cast<int>(xtName.size()), xtName.data());
    std::printf("engine type:  %.*s\n", static_cast<int>(hostName.size()), hostName.data());
    return xtOk && hostOk && xtTagged && hostUntagged;
}
