#include <cstddef>
#include <cstdint>
#include <json.hpp>
#include <string>
#include <string_view>

#include "fuzz_consume.h"
#include "yaml_lite.h"

/// libFuzzer target: feeds arbitrary bytes to xt_bundle's YAML subset reader
/// (xt::bundle::detail::parseYamlLite, Code/Runtime/bundle/src/yaml_lite.h: `manifest/assets.yaml`,
/// `contracts/motion-spec.yaml`), and on success serializes the result and runs yamlScalar() over
/// the input's first line. See Docs/design/15-testing.md §15.5, Docs/tasks/T-267b.md.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // Docs/tasks/T-39.md "規則".
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInputSize) {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    nlohmann::json out;
    std::string error;
    if (!xt::bundle::detail::parseYamlLite(text, out, &error)) {
        return 0;
    }

    // `replace`: the reader does not promise valid UTF-8 strings, and dump()'s default (strict)
    // handler would throw on one, which is not a finding in the reader.
    const std::string dumped = out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const std::string scalar = xt::bundle::detail::yamlScalar(text.substr(0, text.find('\n')));

    xt::fuzz::consume(dumped.size() + scalar.size());
    return 0;
}
