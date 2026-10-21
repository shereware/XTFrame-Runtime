#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "fuzz_consume.h"
#include "fuzz_scratch.h"
#include "xt/import/dc.h"
#include "xt/import/figma.h"
#include "xt/import/import_node.h"
#include "xt/types.h"

/// libFuzzer target: xt_import's front ends. The first input byte selects one (`byte % 2`), the
/// rest is the file:
///   0  Figma: written as `<dump>/file.json`, xt::import::importFigma();
///   1  Claude Design: written as `<dump>/project/Board.dc.html` next to a fixed `canvas.json`
///      that lists it, xt::import::importDc().
/// Both entry points run their body through catchAll() (import/src/untrusted_input.h, T-261a), so
/// no exception may leave them: one that does terminates the process, which libFuzzer reports as
/// a finding. On success the result's UIs and diagnostics are counted. See
/// docs/design/15-testing.md §15.5, docs/tasks/T-267b.md.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // docs/tasks/T-39.md "規則".

constexpr const char* kCanvasJson =
    "{\n"
    "  \"name\": \"Fuzz\",\n"
    "  \"boards\": { \"Board.dc.html\": { \"x\": 0, \"y\": 0, \"w\": 1280, \"h\": 720, \"title\": \"fuzz\", "
    "\"page\": \"main\" } },\n"
    "  \"order\": [\"Board.dc.html\"],\n"
    "  \"pages\": [\"main\"],\n"
    "  \"designSystems\": [\"xtframe\"]\n"
    "}\n";

void keep(const xt::import::ImportResult& result) {
    xt::fuzz::consume(result.uis.size() + result.diagnostics.size() + result.bundleId.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > kMaxInputSize) {
        return 0;
    }
    const uint8_t* bytes = data + 1;
    const size_t length = size - 1;
    xt::import::ImportOptions options;
    xt::import::ImportResult result;

    if (data[0] % 2u == 0) {
        const std::filesystem::path& dir = xt::fuzz::scratchDir("import_figma");
        if (!xt::fuzz::writeScratchFile(dir / "file.json", bytes, length)) {
            return 0;
        }
        if (xt::import::importFigma(dir.string(), options, result) == xt::Status::kOk) {
            keep(result);
        }
        return 0;
    }

    const std::filesystem::path& dir = xt::fuzz::scratchDir("import_dc");
    static const bool canvasWritten = [&dir] {
        std::error_code ec;
        std::filesystem::create_directories(dir / "project", ec);
        return xt::fuzz::writeScratchText(dir / "project" / "canvas.json", kCanvasJson);
    }();
    if (!canvasWritten || !xt::fuzz::writeScratchFile(dir / "project" / "Board.dc.html", bytes, length)) {
        return 0;
    }
    if (xt::import::importDc(dir.string(), options, result) == xt::Status::kOk) {
        keep(result);
    }
    return 0;
}
