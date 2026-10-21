#include <unistd.h>  // getpid; fuzz targets only build under clang on Linux/WSL (docs/tasks/T-39.md).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "null_asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// libFuzzer target: feeds arbitrary bytes to xt::compile::compileDocument (`.xt`), and on a
/// successful compile runs the resulting `.xtir` blob through fuzz_ir.cpp's load/update/render
/// pipeline. See docs/design/15-testing.md §15.5, docs/tasks/T-39.md.
///
/// Not covered by docs/tasks/T-39.md: compileDocument's signature takes a file path
/// (std::string_view xtPath), not the document text itself -- it reads the `.xt` file from disk.
/// The sheet's "compileDocument(string_view) -> success 再走 fuzz_ir 的流程" is read here as
/// "feed the fuzz bytes as the document's contents"; the only way to do that through the real
/// API is to write them to a scratch file first and pass that file's path.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // docs/tasks/T-39.md "規則".

/// One scratch file per process (libFuzzer runs a single process' worth of inputs sequentially
/// unless -jobs/-workers spawn several; each such process gets a distinct pid-suffixed path).
const std::string& scratchPath() {
    static const std::string path = [] {
        std::filesystem::path dir = std::filesystem::temp_directory_path();
        return (dir / ("xt_fuzz_xt_input_" + std::to_string(static_cast<unsigned long>(getpid())) + ".xt"))
            .string();
    }();
    return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInputSize) {
        return 0;
    }

    const std::string& path = scratchPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return 0;
        }
        if (size > 0) {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
    }

    xt::compile::CompileOptions options;
    xt::compile::CompileResult result = xt::compile::compileDocument(path, options);
    if (!result.ok()) {
        return 0;
    }

    xt::IrBlob blob{result.ir.data(), result.ir.size()};
    xt::Result<xt::IrView> view = xt::IrView::open(blob);
    if (!view.ok()) {
        return 0;
    }

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

    ctx.setViewport(xt::Rect{0, 0, 800, 600}, 1.0f);
    ctx.update(1.0f / 60.0f);

    xt::CommandList cmds;
    ctx.render(cmds);
    return 0;
}
