#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/ir_pod.h"

/// T-587: helpers shared by the source format v2 tests (test_parse_xt_v2.cpp,
/// test_ports_section.cpp).

namespace xt::test::v2 {

/// The payload of the first chunk of `type` (a fourcc) in `ir`, or empty when there is none.
inline std::vector<uint8_t> chunkPayload(const std::vector<uint8_t>& ir, const char* type) {
    size_t pos = sizeof(IrHeader);
    while (pos + sizeof(IrChunkHeader) <= ir.size()) {
        IrChunkHeader header{};
        std::memcpy(&header, ir.data() + pos, sizeof(header));
        const size_t begin = pos + sizeof(header);
        if (std::memcmp(header.type, type, 4) == 0 && begin + header.payloadLength <= ir.size()) {
            return std::vector<uint8_t>(
                ir.begin() + static_cast<std::ptrdiff_t>(begin),
                ir.begin() + static_cast<std::ptrdiff_t>(begin + header.payloadLength));
        }
        pos = begin + header.payloadLength;
    }
    return {};
}

/// Compiles `files` (the first one is the main file) without META.
inline compile::CompileResult compileFiles(const std::vector<compile::SourceFile>& files,
                                           compile::CompileOptions options = {}) {
    options.emitMeta = false;
    return compile::compileDocumentFromStrings(files, files.front().name, options);
}

/// The diagnostic with `code`, or nullptr.
inline const compile::Diagnostic* findCode(const std::vector<compile::Diagnostic>& diagnostics,
                                           const std::string& code) {
    for (const compile::Diagnostic& d : diagnostics) {
        if (d.code == code) {
            return &d;
        }
    }
    return nullptr;
}

/// Fails the test case with every diagnostic when `result` did not compile.
inline void requireCompiled(const compile::CompileResult& result) {
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CAPTURE(d.line);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
}

}  // namespace xt::test::v2
