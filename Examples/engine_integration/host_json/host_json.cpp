// T-396: the engine side of the nlohmann coexistence check (see host_json.h). Built as its own
// library with the engine's copy and none of XTFrame's compile definitions.
#include "host_json.h"

#include <nlohmann/json.hpp>

namespace {

template <typename T>
const char* hostSignatureOf() {
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

}  // namespace

int hostJsonArraySize(const char* text, const char* key) {
    // allow_exceptions = false: the engine copy is built with JSON_NOEXCEPTION.
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return -1;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return -1;
    return static_cast<int>(it->size());
}

const char* hostJsonTypeName() { return hostSignatureOf<nlohmann::json>(); }
