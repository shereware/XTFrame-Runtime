#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "golden_common.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// Lottie golden fixture loader/runner. See Docs/tasks/T-17.md "黃金圖 Lottie fixture 流程".
///
/// Not covered by Docs/tasks/T-17.md (conservative choices made here):
///   - Manifest lines starting with '#' (after trimming leading whitespace) are treated as
///     comments and skipped, as are blank lines; a line with too few fields is skipped with a
///     message to stdout rather than aborting the whole manifest load.
///   - `clearColor` hex parsing accepts a bare 6-digit "RRGGBB" string (no leading '#'), matching
///     the manifest format shown in the task sheet's table header ("預設 000000").

namespace xt::golden {

namespace {

namespace fs = std::filesystem;

xt::Color parseClearColorHex(const std::string& hex) {
    unsigned int value = 0;
    std::istringstream iss(hex);
    iss >> std::hex >> value;
    const float r = static_cast<float>((value >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((value >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(value & 0xFFu) / 255.0f;
    return xt::Color{r, g, b, 1.0f};
}

bool isBlankOrComment(const std::string& line) {
    const size_t start = line.find_first_not_of(" \t\r\n");
    return start == std::string::npos || line[start] == '#';
}

}  // namespace

std::vector<LottieFixture> loadLottieManifest(const std::string& manifestPath) {
    std::vector<LottieFixture> out;
    std::ifstream in(manifestPath);
    if (!in.good()) {
        report("xt_golden: could not open Lottie manifest '%s'\n", manifestPath.c_str());
        noteManifestError();
        return out;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (isBlankOrComment(line)) {
            continue;
        }
        std::istringstream iss(line);
        LottieFixture fx;
        if (!(iss >> fx.name >> fx.jsonPath >> fx.width >> fx.height >> fx.time)) {
            report("xt_golden: malformed Lottie manifest line: '%s'\n", line.c_str());
            noteManifestError();
            continue;
        }
        std::string clearHex;
        fx.clearColor = (iss >> clearHex) ? parseClearColorHex(clearHex) : xt::Color{0, 0, 0, 1};
        out.push_back(std::move(fx));
    }
    return out;
}

FrameBackend swFrameBackend() {
    return [](int width, int height, const xt::Color& clear, const xt::CommandList& cmds,
              xt::TextShaper* shaper, xt::AssetResolver* assets, const xt::IrView* ir) {
        std::vector<uint8_t> buf(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
        xt::sw::SwRenderer renderer;
        xt::sw::SwTarget target{buf.data(), width, height, width * 4};
        renderer.setTarget(target);
        renderer.clear(clear);
        renderer.render(cmds, shaper, assets, ir);
        return buf;
    };
}

std::vector<uint8_t> renderLottieFixture(const LottieFixture& fx, const std::string& lottieDir) {
    return renderLottieFixture(fx, lottieDir, swFrameBackend());
}

std::vector<uint8_t> renderLottieFixture(const LottieFixture& fx, const std::string& lottieDir,
                                         const FrameBackend& backend) {
    const fs::path jsonPath = fs::path(lottieDir) / fx.jsonPath;

    xt::compile::CompileOptions options;
    options.rootDir = jsonPath.parent_path().string();
    options.strict = false;

    xt::compile::CompileResult result = xt::compile::compileLottie(jsonPath.string(), options);
    for (const xt::compile::Diagnostic& d : result.diagnostics) {
        const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
        report("xt_golden: lottie fixture '%s': %s: %s\n", fx.name.c_str(), level, d.message.c_str());
    }
    if (!result.ok()) {
        report("xt_golden: lottie fixture '%s' failed to compile ('%s')\n", fx.name.c_str(),
               jsonPath.string().c_str());
        return {};
    }

    xt::FileAssetResolver resolver(options.rootDir, xt::sw::decodeImageBytes);
    xt::Context ctx(xt::ContextConfig{nullptr, &resolver});
    ctx.setViewport(xt::Rect{0, 0, static_cast<float>(fx.width), static_cast<float>(fx.height)}, 1.0f);

    xt::Result<xt::DocumentHandle> handle =
        ctx.loadDocument(xt::IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true);
    if (!handle.ok()) {
        report("xt_golden: lottie fixture '%s' failed to load (status=%s)\n", fx.name.c_str(),
               xt::statusName(handle.status()));
        return {};
    }

    xt::Document* doc = ctx.document(handle.value());
    if (doc == nullptr) {
        report("xt_golden: lottie fixture '%s': loaded document not found\n", fx.name.c_str());
        return {};
    }

    // Single update with dt == time: the Animator evaluates every track from the shared clock, so
    // one update() call from a fresh (clock == 0) Context is equivalent to seeking to `time`.
    ctx.update(fx.time);

    xt::CommandList cmds;
    ctx.render(cmds);
    return backend(fx.width, fx.height, fx.clearColor, cmds, nullptr, &resolver, &doc->ir());
}

}  // namespace xt::golden
