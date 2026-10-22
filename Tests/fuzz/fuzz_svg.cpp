#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "fuzz_scratch.h"
#include "null_asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// libFuzzer target: the compiler's standalone SVG input (an `<img src="*.svg">` file, parsed by
/// xt::compile::parseSvg with the rig extras, Docs/design/12-compiler.md §12.2.1). The fuzz bytes
/// are written as `input.svg` next to a fixed `.xt` that references it, compiled through
/// compileDocument() (the SVG is read from disk there), and a successful compile runs through
/// fuzz_xt's load/update/render pipeline. See Docs/design/15-testing.md §15.5,
/// Docs/tasks/T-267b.md.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // Docs/tasks/T-39.md "規則".

constexpr const char* kMainXt =
    "<xt>\n"
    "  <head><style>#icon { width: 128px; height: 128px; object-fit: contain; }</style></head>\n"
    "  <body><img id=\"icon\" src=\"input.svg\"/></body>\n"
    "</xt>\n";

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInputSize) {
        return 0;
    }

    const std::filesystem::path& dir = xt::fuzz::scratchDir("svg");
    static const bool mainWritten = xt::fuzz::writeScratchText(dir / "main.xt", kMainXt);
    if (!mainWritten || !xt::fuzz::writeScratchFile(dir / "input.svg", data, size)) {
        return 0;
    }

    xt::compile::CompileOptions options;
    options.emitMeta = false;
    xt::compile::CompileResult result = xt::compile::compileDocument((dir / "main.xt").string(), options);
    if (!result.ok()) {
        return 0;
    }

    xt::IrBlob blob{result.ir.data(), result.ir.size()};
    xt::NullTextShaper shaper;
    xt::fuzz::NullAssetResolver assets;
    xt::ContextConfig config;
    config.shaper = &shaper;
    config.assets = &assets;
    xt::Context ctx(config);

    xt::Result<xt::DocumentHandle> handle = ctx.loadDocument(blob, /*takeOwnership=*/false);
    if (!handle.ok()) {
        return 0;
    }

    ctx.setViewport(xt::Rect{0, 0, 256, 256}, 1.0f);
    ctx.update(1.0f / 60.0f);

    xt::CommandList cmds;
    ctx.render(cmds);
    return 0;
}
