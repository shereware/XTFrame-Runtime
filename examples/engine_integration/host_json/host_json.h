#pragma once

/// T-396 (docs/design/23-host-integration.md §23.ae): stands in for a host engine's own JSON code.
/// The engine ships its own nlohmann copy without XTFrame's ABI tag (host_json/nlohmann/json.hpp,
/// built with JSON_NOEXCEPTION and without C++ exceptions, as engines often are) in a library that
/// knows nothing of XTFrame. No nlohmann type crosses this header, as in a real engine module.

/// Parses `text` with the engine's nlohmann copy and returns the size of the array under `key`,
/// or -1 when `text` is not JSON or `key` is not an array.
int hostJsonArraySize(const char* text, const char* key);

/// The compiler's spelling of the engine copy's `nlohmann::json` type (it names the namespace).
const char* hostJsonTypeName();
