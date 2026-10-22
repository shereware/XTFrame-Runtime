#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fuzz_consume.h"
#include "xt/compile/xtfx.h"

/// libFuzzer target: feeds arbitrary bytes to the `.xtfx` custom-effect compiler
/// (xt::compile::compileXtfx: the header, then the HLSL subset translated to GLSL,
/// Docs/design/18-effects.md §18.7), and on success reads the parameters' effectParam() indices
/// and defaults back. See Docs/design/15-testing.md §15.5, Docs/tasks/T-267b.md.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // Docs/tasks/T-39.md "規則".
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInputSize) {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    xt::compile::XtfxResult result = xt::compile::compileXtfx(text);
    if (!result.ok) {
        return 0;
    }

    size_t sum = 0;
    for (const xt::compile::XtfxParam& param : result.header.params) {
        uint32_t index = 0;
        if (xt::compile::xtfxParamIndex(result.header.params, param.name, index)) {
            sum += index;
        }
    }
    float params[8] = {};
    float colors[2][4] = {};
    xt::compile::applyXtfxDefaults(result.header.params, params, colors);

    xt::fuzz::consume(sum + result.glsl.size() + static_cast<size_t>(params[0] != 0.0f));
    return 0;
}
