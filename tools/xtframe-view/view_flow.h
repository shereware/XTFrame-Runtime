#pragma once

/// T-260c: xtframe-view's `--flow` loading and the flow half of R (reload), header-only so
/// tests/flow can run the same sequence the key runs (tools/xtframe-view/main.cpp).

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"

namespace xt::tools::view {

/// Reads `path` (a `.xtflow` file) and loads it against `handle`'s Document into `runtime`.
/// Diagnostics/errors are printed to stderr (one line per problem); returns false on failure.
inline bool loadFlow(xt::Context& ctx, xt::DocumentHandle handle, const std::string& path,
                     xt::flow::FlowRuntime& runtime) {
    std::ifstream in(xt::compile::pathFromUtf8(path), std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "xtframe-view: failed to read flow file '%s'\n", path.c_str());
        return false;
    }
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    xt::flow::FlowGraph graph;
    std::vector<std::string> errors;
    if (xt::flow::FlowGraph::parse(json, graph, errors) != xt::Status::kOk) {
        for (const std::string& e : errors) {
            std::fprintf(stderr, "xtframe-view: flow: %s\n", e.c_str());
        }
        return false;
    }

    xt::Document* doc = ctx.document(handle);
    if (doc == nullptr) {
        std::fprintf(stderr, "xtframe-view: flow: no document loaded\n");
        return false;
    }
    errors.clear();
    if (runtime.load(graph, *doc, errors) != xt::Status::kOk) {
        for (const std::string& e : errors) {
            std::fprintf(stderr, "xtframe-view: flow: %s\n", e.c_str());
        }
        return false;
    }
    return true;
}

/// R (reload) after the new Document was loaded at `handle` beside the one at `previous`: loads
/// the flow file at `flowPath` against the new one again when `hasFlow`, then unloads `previous`.
/// Returns whether a flow graph is loaded afterwards (false: the run goes on without one, the
/// error already printed by loadFlow()).
inline bool reloadFlow(xt::Context& ctx, xt::DocumentHandle previous, xt::DocumentHandle handle,
                       const std::string& flowPath, bool hasFlow, xt::flow::FlowRuntime& runtime) {
    // T-260c: off the previous Document first, whatever happens next: loadFlow() can fail before
    // it reaches FlowRuntime::load() (an unreadable or unparsable file), which would leave the
    // runtime's listeners on a Document that is unloaded below.
    runtime.unload();
    if (hasFlow) {
        hasFlow = loadFlow(ctx, handle, flowPath, runtime);
    }
    ctx.unloadDocument(previous);
    return hasFlow;
}

}  // namespace xt::tools::view
