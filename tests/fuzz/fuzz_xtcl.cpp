#include <cstddef>
#include <cstdint>

#include "fuzz_consume.h"
#include "xt/baked.h"
#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/types.h"

/// libFuzzer target: feeds arbitrary bytes to xt::BakedPlayer (a baked `.xtcl` frame stream):
/// load -> read the header's size and frame count -> decode the first 3 frames. See
/// docs/design/15-testing.md §15.5, docs/tasks/T-267b.md.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // docs/tasks/T-39.md "規則".
constexpr uint32_t kFramesToDecode = 3;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInputSize) {
        return 0;
    }

    xt::BakedPlayer player;
    if (player.load(xt::IrBlob{data, size}, /*takeOwnership=*/false) != xt::Status::kOk) {
        return 0;
    }

    const xt::Rect viewport = player.viewport();
    const uint32_t frames = player.frameCount();
    size_t total = 0;
    for (uint32_t i = 0; i < frames && i < kFramesToDecode; ++i) {
        const xt::CommandList& list = player.frame(i);
        total += list.commands.size();
    }
    // Keeps the reads and the decode from being optimized away.
    xt::fuzz::consume(total + static_cast<size_t>(viewport.w > 0.0f) +
                      static_cast<size_t>(player.fps() > 0.0f));
    return 0;
}
