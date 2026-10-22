#include <cstddef>
#include <cstdint>

#include "null_asset_resolver.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// libFuzzer target: feeds arbitrary bytes through IrView::open, then (on success) through
/// Context::loadDocument, one setViewport/update/render frame. See Docs/design/15-testing.md
/// §15.5, Docs/tasks/T-39.md.

namespace {
// Docs/tasks/T-39.md "規則": 1 MiB input cap; run_fuzz.sh also passes -max_len=1048576, this is
// a defensive second check for corpus entries fed directly (e.g. via a debugger, not run_fuzz.sh).
constexpr size_t kMaxInputSize = 1u << 20;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInputSize) {
        return 0;
    }

    xt::IrBlob blob{data, size};
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
