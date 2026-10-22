#pragma once

#include <json.hpp>
#include <string>
#include <string_view>

/// Private: T-262c's bounded JSON parse, shared by the compiler and Code/Editor/Core's readers of project
/// and user files (Code/Editor/Core includes Code/Runtime/compile/src privately). The same callback as
/// Code/Runtime/bundle/src/bundle_json.h's parseBounded(), which neither can include (neither links xt_bundle).

namespace xt::compile::detail {

/// T-262c: the deepest JSON nesting a reader accepts (the root object or array is level 1).
inline constexpr int kMaxJsonDepth = 32;

/// T-262c: `json::parse(text)` through nlohmann's callback parser, refusing every object or array
/// deeper than `maxDepth` before it is built (nlohmann's copy constructor and dump() recurse).
/// Returns a discarded value when `text` is not JSON (`error` empty) or nests deeper than
/// `maxDepth` (`error` = "nesting too deep"; the caller names the file). Never throws.
inline nlohmann::json parseBounded(std::string_view text, int maxDepth, std::string& error) {
    using nlohmann::json;
    error.clear();
    bool tooDeep = false;
    // `depth` is the number of enclosing containers, so the root starts at 0 and a container
    // starting at depth `maxDepth` would be level `maxDepth + 1`. Refusing it (returning false)
    // makes nlohmann skip building it and everything inside it.
    const json::parser_callback_t callback = [&tooDeep, maxDepth](int depth, json::parse_event_t event,
                                                                  json& /*parsed*/) {
        if ((event == json::parse_event_t::object_start || event == json::parse_event_t::array_start) &&
            depth >= maxDepth) {
            tooDeep = true;
            return false;
        }
        return true;
    };
    json root = json::parse(text.begin(), text.end(), callback, /*allow_exceptions=*/false);
    if (tooDeep) {
        error = "nesting too deep";
        return json(json::value_t::discarded);
    }
    return root;
}

}  // namespace xt::compile::detail
