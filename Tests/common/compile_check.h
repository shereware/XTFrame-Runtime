// T-800: the compile step the tests share -- every Error diagnostic is a failed CHECK (with its
// message), then REQUIRE that the compile succeeded. Header-only; include after doctest.h is on the
// include path (every suite has it).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"

namespace xt::test {

/// Fails the test case when `result` has an Error diagnostic (each one CHECKed with its message
/// captured) or did not compile.
inline void requireCompiled(const compile::CompileResult& result) {
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
}

/// Compiles `text` as the in-memory file "main.xt" with `options` and returns its IR; the test
/// case fails as in requireCompiled() when it does not compile.
inline std::vector<uint8_t> compileMainOk(std::string_view text, const compile::CompileOptions& options) {
    const std::vector<compile::SourceFile> files{{"main.xt", text}};
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "main.xt", options);
    requireCompiled(result);
    return result.ir;
}

}  // namespace xt::test
