#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/flow/flow_graph.h"

/// T-601 (Docs/design/53-flow-language.md §53.102 item 3; 24 §24.15.4): the static flow lint, on
/// the parsed graph alone (no Document): `flow.introByTimer`.

namespace xt::flow {

namespace {

/// 53 §53.102 item 3's message, word for word.
constexpr const char* kIntroByTimerMessage =
    "intro starts from an autostart timer; the first frames show the final state (use lifecycle.shown or "
    "on:show)";

/// A param read as the runtime reads a bool one (FlowRuntime::Impl::paramBool()).
bool boolParam(const NodeDesc& node, const char* name, bool fallback) {
    auto it = node.params.find(name);
    if (it == node.params.end()) {
        return fallback;
    }
    if (it->second.type == PortType::Bool) {
        return it->second.b;
    }
    if (it->second.type == PortType::Number) {
        return it->second.n != 0.0;
    }
    return fallback;
}

/// "<node>.<port>" -> (node, port); false for a document / UI endpoint or a malformed one.
bool splitNodeEndpoint(const std::string& text, std::string& node, std::string& port) {
    if (text.empty() || text[0] == '#' || text.compare(0, 3, "ui#") == 0) {
        return false;
    }
    const size_t dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) {
        return false;
    }
    node = text.substr(0, dot);
    port = text.substr(dot + 1);
    return true;
}

const NodeDesc* findNode(const FlowGraph& graph, const std::string& id) {
    for (const NodeDesc& node : graph.nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

/// The pulse outputs a pulse arriving at `port` of `node` passes on unchanged, or none: `merge`
/// (`in` -> `out`) and a pulse-typed `throttle` (`in` -> `out`, `passed`) are the pulse-only relays
/// of the node set (53 §53.102 item 3's "gate / once / sequence kinds").
std::vector<std::string> relayedOutputs(const NodeDesc& node, const std::string& port) {
    if (node.type == "merge" && port == "in") {
        return {"out"};
    }
    if (node.type == "throttle" && port == "in") {
        return {"out", "passed"};
    }
    return {};
}

/// True for an input that starts an intro or sets a state: `timeline.play` / `seek`, and any
/// input of a `state` node (`set` or one of its event inputs).
bool isIntroSink(const NodeDesc& node, const std::string& port) {
    return (node.type == "timeline" && (port == "play" || port == "seek")) || node.type == "state";
}

}  // namespace

void lintFlowGraph(const FlowGraph& graph, std::vector<FlowLintFinding>& out) {
    for (const NodeDesc& timer : graph.nodes) {
        const bool oneShot = boolParam(timer, "autostart", false) && !boolParam(timer, "repeat", true);
        if (timer.type != "timer" || !oneShot) {
            continue;
        }
        FlowLintFinding finding;
        // Breadth first from `<timer>.tick` over the edges leaving each pulse output reached.
        std::vector<std::pair<std::string, std::string>> frontier{{timer.id, "tick"}};
        std::set<std::pair<std::string, std::string>> seen(frontier.begin(), frontier.end());
        for (size_t i = 0; i < frontier.size(); ++i) {
            const std::string from = frontier[i].first + "." + frontier[i].second;
            for (const EdgeDesc& edge : graph.edges) {
                std::string nodeId;
                std::string port;
                if (edge.from != from || !splitNodeEndpoint(edge.to, nodeId, port)) {
                    continue;
                }
                const NodeDesc* node = findNode(graph, nodeId);
                if (node == nullptr) {
                    continue;
                }
                if (isIntroSink(*node, port)) {
                    finding.targets.push_back(edge.to);
                    if (node->type == "timeline" && finding.timeline.empty()) {
                        auto name = node->params.find("name");
                        if (name != node->params.end() && name->second.type == PortType::String) {
                            finding.timeline = name->second.s;
                        }
                    }
                    continue;
                }
                for (std::string& output : relayedOutputs(*node, port)) {
                    if (seen.emplace(nodeId, output).second) {
                        frontier.emplace_back(nodeId, std::move(output));
                    }
                }
            }
        }
        if (finding.targets.empty()) {
            continue;
        }
        finding.code = "flow.introByTimer";
        finding.nodeId = timer.id;
        finding.message = kIntroByTimerMessage;
        finding.fix = "drive " + finding.targets.front() + " from lifecycle.shown instead of " + timer.id;
        finding.fix += ".tick";
        if (!finding.timeline.empty()) {
            finding.fix += " (or on:show=\"play(" + finding.timeline + ")\" on the board)";
        }
        out.push_back(std::move(finding));
    }
}

}  // namespace xt::flow
