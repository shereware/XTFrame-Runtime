#include "xt/bundle/project_runtime.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "xt/bundle/ui_project.h"
#include "xt/compile/utf8_path.h"

/// The whole-project flow runtime (T-356). See xt/bundle/project_runtime.h.

namespace xt::bundle {

using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace fs = std::filesystem;

namespace {

bool isFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool readText(const std::string& path, std::string& out) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// Compiles (`.xt`, `.json`) or reads (anything else, e.g. `.xtir`) `ui.doc` into `ctx`.
Status loadUi(const RuntimeUiSource& ui, const ProjectRuntimeOptions& options, Context& ctx,
              DocumentHandle& handle, std::vector<ProjectRuntimeProblem>& problems) {
    const fs::path path = pathFromUtf8(ui.doc);
    const fs::path ext = path.extension();
    Result<DocumentHandle> result{Result<DocumentHandle>::error(Status::kInternal)};
    if (ext == ".xt" || ext == ".json") {
        compile::CompileOptions compileOptions = options.compile;
        if (ext == ".xt") {
            compileOptions.xtDir = pathToUtf8(path.parent_path());
        }
        compile::CompileResult compiled = ext == ".json" ? compile::compileLottie(ui.doc, compileOptions)
                                                         : compile::compileDocument(ui.doc, compileOptions);
        if (options.onDiagnostic) {
            for (const compile::Diagnostic& d : compiled.diagnostics) {
                options.onDiagnostic(d);
            }
        }
        if (!compiled.ok()) {
            problems.push_back({ui.id, "failed to compile '" + ui.doc + "'"});
            return Status::kInvalidArgument;
        }
        result = ctx.loadDocument(IrBlob{compiled.ir.data(), compiled.ir.size()}, /*takeOwnership=*/true);
    } else {
        result = ctx.loadDocumentFile(ui.doc);
    }
    if (!result.ok()) {
        problems.push_back({ui.id, "failed to load '" + ui.doc + "' (" + statusName(result.status()) + ")"});
        return result.status();
    }
    handle = result.value();
    return Status::kOk;
}

}  // namespace

ProjectRuntimeSources projectRuntimeSources(const UiProject& project, std::string_view currentUiId) {
    ProjectRuntimeSources out;
    const fs::path root = pathFromUtf8(project.rootDir);
    for (const ProjectUi& ui : project.uis) {
        if (ui.id == currentUiId) {
            out.currentUi = ui.id;
            continue;
        }
        RuntimeUiSource source;
        source.id = ui.id;
        const fs::path doc = (root / pathFromUtf8(ui.doc)).lexically_normal();
        source.doc = pathToUtf8(doc);
        if (!ui.flow.empty()) {
            source.flow = pathToUtf8((root / pathFromUtf8(ui.flow)).lexically_normal());
        } else {
            const fs::path sibling = fs::path(doc).replace_extension(".xtflow");
            if (isFile(sibling)) {
                source.flow = pathToUtf8(sibling);
            }
        }
        out.others.push_back(std::move(source));
    }
    fs::path global = root / "bundle.xtflow";
    if (!isFile(global)) {
        global = root / "shared" / "bundle.xtflow";
    }
    if (isFile(global)) {
        out.globalFlow = pathToUtf8(global.lexically_normal());
    }
    return out;
}

void ProjectRuntime::reset() {
    bundle.reset();  // Unbinds from every Document first (they are still loaded).
    if (othersContext != nullptr) {
        for (const auto& [id, handle] : others) {
            othersContext->unloadDocument(handle);
        }
    }
    others.clear();
    othersContext = nullptr;
    globalFlowPath.clear();
}

Status loadProjectUis(const ProjectRuntimeSources& sources, const ProjectRuntimeOptions& options,
                      Context& ctx, ProjectRuntime& out, std::vector<ProjectRuntimeProblem>& problems) {
    out.reset();
    out.othersContext = &ctx;
    Status status = Status::kOk;
    for (const RuntimeUiSource& ui : sources.others) {
        DocumentHandle handle = 0;
        const Status loaded = loadUi(ui, options, ctx, handle, problems);
        if (loaded != Status::kOk) {
            status = loaded;
            continue;
        }
        if (options.othersStartHidden) {
            // T-373 (17 §17.33.2): hidden before its first frame, so its `show` waits for a real one.
            (void)ctx.setVisible(handle, false);
        }
        out.others.emplace_back(ui.id, handle);
    }
    return status;
}

Status startProjectFlows(const ProjectRuntimeSources& sources, const ProjectRuntimeOptions& options,
                         Context& currentCtx, DocumentHandle current, ProjectRuntime& out,
                         std::vector<ProjectRuntimeProblem>& problems) {
    out.bundle.reset();
    out.globalFlowPath.clear();
    out.bundle = std::make_unique<flow::BundleContext>(currentCtx);
    if (options.hostEventListener) {
        out.bundle->setHostEventListener(options.hostEventListener);
    }
    if (!sources.currentUi.empty() &&
        out.bundle->addUi(sources.currentUi, currentCtx, current) != Status::kOk) {
        problems.push_back({sources.currentUi, "the shown UI's Document is not loaded"});
    }
    Status status = Status::kOk;
    Context& others = out.othersContext != nullptr ? *out.othersContext : currentCtx;
    for (const auto& [id, handle] : out.others) {
        if (out.bundle->addUi(id, others, handle) != Status::kOk) {
            problems.push_back({id, "its Document is not loaded"});
            status = Status::kInvalidArgument;
        }
    }
    if (!options.loadFlows) {
        return status;
    }
    for (const auto& [id, handle] : out.others) {
        std::string flowPath;
        for (const RuntimeUiSource& ui : sources.others) {
            if (ui.id == id) {
                flowPath = ui.flow;
            }
        }
        std::string json;
        if (flowPath.empty() || !isFile(pathFromUtf8(flowPath)) || !readText(flowPath, json)) {
            continue;
        }
        std::vector<std::string> errors;
        if (out.bundle->loadUiFlow(id, json, errors) != Status::kOk) {
            for (const std::string& e : errors) {
                problems.push_back({id, e});
            }
            status = Status::kInvalidArgument;
        }
    }
    if (sources.globalFlow.empty()) {
        return status;
    }
    std::string json;
    if (!readText(sources.globalFlow, json)) {
        problems.push_back({std::string(), "failed to read flow file '" + sources.globalFlow + "'"});
        return Status::kIoError;
    }
    std::vector<std::string> errors;
    if (out.bundle->loadGlobalFlow(json, errors) != Status::kOk) {
        for (const std::string& e : errors) {
            problems.push_back({std::string(), e});
        }
        return Status::kInvalidArgument;
    }
    out.globalFlowPath = sources.globalFlow;
    return status;
}

Status loadProjectRuntime(const ProjectRuntimeSources& sources, const ProjectRuntimeOptions& options,
                          Context& currentCtx, DocumentHandle current, Context& othersCtx,
                          ProjectRuntime& out, std::vector<ProjectRuntimeProblem>& problems) {
    const Status loaded = loadProjectUis(sources, options, othersCtx, out, problems);
    const Status started = startProjectFlows(sources, options, currentCtx, current, out, problems);
    return loaded != Status::kOk ? loaded : started;
}

}  // namespace xt::bundle
