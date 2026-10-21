#pragma once

#include <json.hpp>
#include <string>
#include <string_view>

/// A reader for the small YAML subset UI Project files use (T-183): `manifest/assets.yaml`
/// (docs/design/26-layers-symbols-library.md §26.3.3) and `contracts/motion-spec.yaml`
/// (docs/design/29-ai-endpoint.md §29.6). Internal to xt_bundle; not a general YAML parser.
///
/// Supported: `#` comments (at line start or after whitespace, outside quotes), block mappings and
/// block sequences nested by space indentation (a sequence may sit at its key's indentation),
/// `- key: value` items that open a mapping, single-line flow collections (`[a, b]`, `{k: v}`,
/// nested), double-quoted scalars (`\"`, `\`, `\n`, `\t` escapes), single-quoted scalars (`''`),
/// and plain scalars. Plain `true`/`false` read as booleans, `null`/`~`/empty as null, decimal
/// integers and floats as numbers; everything else (including `on`/`off`/`yes`/`no`) is a string.
///
/// Not supported (a parse error naming the line): tabs in indentation, anchors/aliases/tags,
/// block scalars (`|`, `>`), multi-line flow collections, multiple documents.

namespace xt::bundle::detail {

/// Parses `text` into `out` (an object, an array, or null for an empty file). Returns false and
/// sets `*error` (when non-null) to "line N: <reason>" on unsupported or malformed input.
bool parseYamlLite(std::string_view text, nlohmann::json& out, std::string* error = nullptr);

/// `text` as a YAML scalar: plain when it reads back as the same string, double-quoted otherwise.
std::string yamlScalar(std::string_view text);

/// T-305: writes `value` as block YAML that parseYamlLite() reads back to the same value, each line
/// indented by `indent` spaces more. Mappings are `key: value` lines in the object's own key order
/// (compose the document as an ordered_json to fix the order); a nested mapping or a sequence that
/// holds a mapping/sequence opens an indented block; a sequence of scalars is one flow line
/// `[a, b]`; a mapping inside a sequence is written `- key: v` with the rest of its keys below; an
/// empty mapping/sequence is `{}`/`[]`. Strings go through yamlScalar() (and are double-quoted in a
/// flow sequence when they hold `,[]{}`), numbers through nlohmann's shortest round-trip form,
/// null as `null`. Every line ends with '\n'.
std::string emitYamlLite(const nlohmann::ordered_json& value, int indent = 0);
/// The same for an unordered json (its keys come out alphabetically).
std::string emitYamlLite(const nlohmann::json& value, int indent = 0);

/// An unordered json as an ordered one (keys sorted, as json keeps them); used before emitting
/// YAML or ordered JSON (T-414: asset_meta.cpp and design_io.cpp had copies).
nlohmann::ordered_json toOrdered(const nlohmann::json& value);

}  // namespace xt::bundle::detail
