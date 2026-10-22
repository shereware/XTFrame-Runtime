#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "fuzz_consume.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"

/// libFuzzer target: feeds arbitrary bytes to the `.xts` stylesheet parser
/// (xt::compile::parseXts, Code/Runtime/compile/src/xts) on its own, then, on success, computes every parsed
/// selector's specificity and re-parses the input as a bare declaration list. See
/// Docs/design/15-testing.md §15.5, Docs/tasks/T-267b.md.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // Docs/tasks/T-39.md "規則".
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInputSize) {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    xt::compile::DiagnosticSink sink;
    xt::compile::RuleSet rules;
    const bool ok = xt::compile::parseXts(text, "fuzz.xts", 1, sink, rules);

    uint32_t specificity = 0;
    for (const xt::compile::StyleRule& rule : rules.rules) {
        for (const xt::compile::Selector& sel : rule.selectors) {
            specificity ^= xt::compile::selectorSpecificity(sel);
        }
    }

    if (ok) {
        xt::compile::DiagnosticSink declSink;
        std::vector<xt::compile::StyleDecl> decls;
        (void)xt::compile::parseDeclarations(text, "fuzz.xts", 1, declSink, decls);
    }

    xt::fuzz::consume(specificity);
    return 0;
}
