#pragma once

/// T-396 (docs/design/23-host-integration.md §23.ae): `engine_integration --json-coexist`. Parses
/// one JSON text with XTFrame's nlohmann (nlohmann::xtframe_abi, reached through the public bundle
/// header xt/bundle/design_system.h) and another with the engine's own copy (host_json/), prints
/// both type names and returns true when both parsed and the two types live in different
/// namespaces.
bool runJsonCoexistDemo();
