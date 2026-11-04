#include <algorithm>
#include <string>
#include <vector>

#include "flow_internal.h"

/// T-580 (Docs/design/53-flow-language.md §53.101, 22-bundle.md §22.15): FlowGraph::hostInterface(),
/// a read-only reflection of what a graph shows a host -- its `emit` events, the declared ports it
/// writes and reads, and the `system` outputs it is wired to. It resolves the edges with the same
/// validator FlowGraph::parse() uses (no Document) and changes nothing at run time.

namespace xt::flow {

namespace {

bool sameRef(const FlowPortRef& a, const FlowPortRef& b) {
    return a.uiId == b.uiId && a.owner == b.owner && a.port == b.port;
}

FlowPortRef refOf(const Endpoint& ep) { return FlowPortRef{ep.uiId, ep.elementId, ep.port}; }

void addRef(std::vector<FlowPortRef>& list, const FlowPortRef& ref) {
    for (const FlowPortRef& existing : list) {
        if (sameRef(existing, ref)) {
            return;
        }
    }
    list.push_back(ref);
}

/// The edge (index into `edges`) into node `nodeIndex`'s input `port`, or -1.
int edgeInto(const std::vector<BoundEdge>& edges, int nodeIndex, const char* port) {
    for (size_t i = 0; i < edges.size(); ++i) {
        const Endpoint& to = edges[i].to;
        if (to.owner == Endpoint::Owner::Node && to.nodeIndex == nodeIndex && to.port == port) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// The type of the value `source` carries. A `filter` / `throttle` whose type parse() could not
/// infer passes its `in` edge's value through, so the walk follows it (bounded: the node graph is
/// acyclic, and a step limit guards a hand-built graph).
void sourceType(const std::vector<BoundEdge>& edges, Endpoint source, FlowEmitInfo& out) {
    for (size_t steps = 0; steps <= edges.size(); ++steps) {
        if (!source.anyType) {
            out.type = source.type;
            out.typeKnown = true;
            return;
        }
        if (source.owner == Endpoint::Owner::Port) {
            out.typeKnown = false;
            out.typeSource = refOf(source);
            return;
        }
        if (source.owner != Endpoint::Owner::Node) {
            break;
        }
        const int in = edgeInto(edges, source.nodeIndex, "in");
        if (in < 0) {
            break;
        }
        source = edges[static_cast<size_t>(in)].from;
    }
    out.type = PortType::String;  // Not expected after parse(); a value of some kind, as text.
    out.typeKnown = true;
}

}  // namespace

Status FlowGraph::hostInterface(FlowHostInterface& out) const {
    out = FlowHostInterface{};
    std::vector<std::string> errors;
    std::vector<BoundEdge> bound;
    if (validateGraph(*this, nullptr, errors, &bound) != Status::kOk) {
        return Status::kInvalidArgument;
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeType type;
        if (!parseNodeType(nodes[i].type, type) || type != NodeType::Emit) {
            continue;
        }
        const auto portIt = nodes[i].params.find("port");
        if (portIt == nodes[i].params.end() || portIt->second.s.empty()) {
            continue;
        }
        const std::string& name = portIt->second.s;
        if (std::any_of(out.emits.begin(), out.emits.end(),
                        [&](const FlowEmitInfo& e) { return e.name == name; })) {
            continue;  // The first `emit` node of a name decides its type.
        }
        FlowEmitInfo emit;
        emit.name = name;
        const int value = edgeInto(bound, static_cast<int>(i), "value");
        if (value >= 0) {
            sourceType(bound, bound[static_cast<size_t>(value)].from, emit);
        }
        out.emits.push_back(std::move(emit));
    }

    std::vector<std::string> wired;
    for (const BoundEdge& edge : bound) {
        if (edge.to.owner == Endpoint::Owner::Port) {
            addRef(out.writes, refOf(edge.to));
        }
        if (edge.from.owner == Endpoint::Owner::Port) {
            addRef(out.reads, refOf(edge.from));
        }
        if (edge.from.owner == Endpoint::Owner::Node && edge.from.nodeType == NodeType::System) {
            wired.push_back(edge.from.port);
        }
    }
    for (const std::string& name : outputPortOrder(NodeType::System)) {
        if (std::find(wired.begin(), wired.end(), name) != wired.end()) {
            out.system.push_back(name);
        }
    }
    return Status::kOk;
}

}  // namespace xt::flow
