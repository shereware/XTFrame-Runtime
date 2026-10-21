#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_consume.h"
#include "fuzz_scratch.h"
#include "null_asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/ir.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// libFuzzer target: the JSON files a bundle / UI Project / flow is read from. The first input
/// byte selects the format (`byte % 3`), the rest is the file:
///   0  bundle `manifest.json`: written into a scratch directory, xt::bundle::Bundle::open();
///   1  `.xtui` UI Project file: written as `fuzz.xtui`, xt::bundle::UiProject::load(), and every
///      UI's `doc`/`flow` resolved against the project (resolveProjectPath());
///   2  `.xtflow`: xt::flow::FlowGraph::parse(); on success the graph is loaded into a
///      FlowRuntime bound to XT_FUZZ_FLOW_DOC (the game-ui HUD, whose ids the seed flows use),
///      which then ticks twice.
/// See docs/design/15-testing.md §15.5, docs/tasks/T-267b.md.

#ifndef XT_FUZZ_FLOW_DOC
#error "XT_FUZZ_FLOW_DOC must name the .xt document a fuzzed .xtflow is loaded against"
#endif

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // docs/tasks/T-39.md "規則".

/// The flow document's compiled `.xtir`, compiled once per process; empty when it does not
/// compile (the flow is then only parsed).
const std::vector<uint8_t>& flowDocumentIr() {
    static const std::vector<uint8_t> ir = [] {
        xt::compile::CompileOptions options;
        options.emitMeta = false;
        xt::compile::CompileResult result = xt::compile::compileDocument(XT_FUZZ_FLOW_DOC, options);
        return result.ok() ? result.ir : std::vector<uint8_t>{};
    }();
    return ir;
}

void fuzzManifest(const uint8_t* data, size_t size) {
    const std::filesystem::path& dir = xt::fuzz::scratchDir("bundle_json_manifest");
    if (!xt::fuzz::writeScratchFile(dir / "manifest.json", data, size)) {
        return;
    }
    std::string error;
    xt::Result<std::unique_ptr<xt::bundle::Bundle>> bundle =
        xt::bundle::Bundle::open(dir.string(), nullptr, &error);
    if (!bundle.ok()) {
        return;
    }
    size_t total = bundle.value()->id().size();
    for (const xt::bundle::UiInfo& ui : bundle.value()->uis()) {
        total += ui.id.size() + ui.file.size();
    }
    xt::fuzz::consume(total);
}

void fuzzUiProject(const uint8_t* data, size_t size) {
    const std::filesystem::path& dir = xt::fuzz::scratchDir("bundle_json_xtui");
    const std::filesystem::path file = dir / "fuzz.xtui";
    if (!xt::fuzz::writeScratchFile(file, data, size)) {
        return;
    }
    std::string error;
    xt::Result<xt::bundle::UiProject> project = xt::bundle::UiProject::load(file.string(), &error);
    if (!project.ok()) {
        return;
    }
    size_t total = 0;
    for (const xt::bundle::ProjectUi& ui : project.value().uis) {
        total += xt::bundle::resolveProjectPath(project.value(), ui.doc).size();
        total += xt::bundle::resolveProjectPath(project.value(), ui.flow).size();
    }
    xt::fuzz::consume(total);
}

void fuzzFlow(const uint8_t* data, size_t size) {
    const std::string_view json(reinterpret_cast<const char*>(data), size);
    xt::flow::FlowGraph graph;
    std::vector<std::string> errors;
    if (xt::flow::FlowGraph::parse(json, graph, errors) != xt::Status::kOk) {
        return;
    }

    const std::vector<uint8_t>& ir = flowDocumentIr();
    if (ir.empty()) {
        return;
    }
    xt::NullTextShaper shaper;
    xt::fuzz::NullAssetResolver assets;
    xt::ContextConfig config;
    config.shaper = &shaper;
    config.assets = &assets;
    xt::Context ctx(config);
    xt::Result<xt::DocumentHandle> handle = ctx.loadDocument(xt::IrBlob{ir.data(), ir.size()}, false);
    if (!handle.ok()) {
        return;
    }
    xt::Document* doc = ctx.document(handle.value());
    if (doc == nullptr) {
        return;
    }
    ctx.setViewport(xt::Rect{0, 0, 1280, 720}, 1.0f);

    // Declared after ctx so it is destroyed first (it holds listeners on the Document).
    xt::flow::FlowRuntime runtime;
    if (runtime.load(graph, *doc, errors) != xt::Status::kOk) {
        return;
    }
    runtime.tick(1.0f / 60.0f);
    ctx.update(1.0f / 60.0f);
    runtime.tick(1.0f / 60.0f);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > kMaxInputSize) {
        return 0;
    }
    switch (data[0] % 3u) {
        case 0:
            fuzzManifest(data + 1, size - 1);
            break;
        case 1:
            fuzzUiProject(data + 1, size - 1);
            break;
        default:
            fuzzFlow(data + 1, size - 1);
            break;
    }
    return 0;
}
