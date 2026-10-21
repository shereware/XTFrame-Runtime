#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/flow/bundle_context.h"
#include "xt/types.h"

/// The whole-project flow runtime (T-356; docs/design/17-editor.md §17.6.1, 27-interaction.md
/// §27.4, T-193's `xtframe-view --project`): every UI of a UI Project or Bundle besides the one a
/// host shows, compiled into Documents nobody has to render, a flow::BundleContext over all of
/// them, each other UI's own `.xtflow` and the global flow (`bundle.xtflow`). `xtframe-view
/// --project` and the editor's Demo space both load it through the functions below, so they see
/// the same UIs and the same global graph. No Qt; no rendering.

namespace xt::bundle {

struct UiProject;

/// One UI of a project, as loadProjectRuntime() reads it from disk.
struct RuntimeUiSource {
    std::string id;
    std::string doc;   ///< Absolute (or working-directory relative) `.xt`, `.json` or `.xtir` path.
    std::string flow;  ///< Its own `.xtflow`; empty when it has none.
};

/// What a project runtime loads.
struct ProjectRuntimeSources {
    std::string currentUi;                ///< The UI the host shows (its Document is the host's).
    std::vector<RuntimeUiSource> others;  ///< Every other UI, in project order.
    std::string globalFlow;               ///< The global flow file; empty when there is none.
};

/// The sources of `project` (loaded from disk, rootDir set) with `currentUiId` as the shown UI:
/// each other UI's document under the project root, its flow (the entry's `flow`, else the
/// `<doc stem>.xtflow` next to its document when that file exists -- T-139's sibling rule) and the
/// global flow (`<root>/bundle.xtflow`, else `<root>/shared/bundle.xtflow`, when one exists --
/// pack()'s rule). An unknown `currentUiId` lists every UI as another one.
ProjectRuntimeSources projectRuntimeSources(const UiProject& project, std::string_view currentUiId);

/// One problem loadProjectRuntime() met.
struct ProjectRuntimeProblem {
    std::string ui;       ///< The UI it concerns; empty for the global flow.
    std::string message;  ///< English, one line.
};

/// How loadProjectUis() compiles a `.xt` / `.json` document. `compile.xtDir` is set per
/// document (its own directory).
struct ProjectRuntimeOptions {
    compile::CompileOptions compile;
    /// Told every compile diagnostic (warnings included), before any problem is recorded.
    std::function<void(const compile::Diagnostic&)> onDiagnostic;
    /// false: neither the other UIs' own flows nor the global flow are loaded (`--no-flow`).
    bool loadFlows = true;
    /// Given to the BundleContext before any flow loads (BundleContext::setHostEventListener()).
    flow::FlowHostEventListener hostEventListener;
    /// T-373 (17 §17.33.2): loadProjectUis() hides each other UI as it loads (Context::setVisible(),
    /// before any update(): no `show` triggers fire), so only a flow / trigger `show` or a host's
    /// requestShow() makes one visible, and its `on:show` runs at that moment.
    bool othersStartHidden = true;
};

/// A loaded project runtime. Destroy (or reset()) it before the Contexts it names.
struct ProjectRuntime {
    std::unique_ptr<flow::BundleContext> bundle;
    /// The other UIs' Documents (in `othersContext`), in sources order: id and handle.
    std::vector<std::pair<std::string, DocumentHandle>> others;
    Context* othersContext = nullptr;
    /// The global flow file that was loaded; empty when none was.
    std::string globalFlowPath;

    ProjectRuntime() = default;
    ProjectRuntime(const ProjectRuntime&) = delete;
    ProjectRuntime& operator=(const ProjectRuntime&) = delete;
    ~ProjectRuntime() { reset(); }
    /// Drops the BundleContext, then unloads every other UI's Document from `othersContext`.
    void reset();
};

/// Step 1: drops whatever `out` held (reset()), then compiles (or reads, for `.xtir`) every
/// `sources.others` document and loads it into `ctx` (out.othersContext), hidden when
/// options.othersStartHidden is set. A UI that fails is skipped with a problem; kOk only when
/// every one loaded. A host that wants the shown UI to stay the foreground Document of a shared
/// Context calls this before loading that one.
Status loadProjectUis(const ProjectRuntimeSources& sources, const ProjectRuntimeOptions& options,
                      Context& ctx, ProjectRuntime& out, std::vector<ProjectRuntimeProblem>& problems);

/// Step 2: (re)builds out.bundle on `currentCtx` over the shown UI (`current`, registered as
/// `sources.currentUi` when that is not empty) and loadProjectUis()' Documents, then (with
/// options.loadFlows) loads each other UI's own flow (a missing or unreadable file is skipped)
/// and the global flow. kOk when every flow that exists loaded; out.bundle stays usable either
/// way. The shown UI's own flow is the host's business (the editor runs its edited graph).
Status startProjectFlows(const ProjectRuntimeSources& sources, const ProjectRuntimeOptions& options,
                         Context& currentCtx, DocumentHandle current, ProjectRuntime& out,
                         std::vector<ProjectRuntimeProblem>& problems);

/// loadProjectUis() into `othersCtx`, then startProjectFlows(): the whole runtime at once. kOk
/// when both returned kOk; whatever loaded stays loaded.
Status loadProjectRuntime(const ProjectRuntimeSources& sources, const ProjectRuntimeOptions& options,
                          Context& currentCtx, DocumentHandle current, Context& othersCtx,
                          ProjectRuntime& out, std::vector<ProjectRuntimeProblem>& problems);

}  // namespace xt::bundle
