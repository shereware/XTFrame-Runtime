#include "xt/flow/flow_graph.h"

#include <cmath>
#include <json.hpp>
#include <set>

#include "flow_internal.h"

/// FlowGraph::parse(): JSON parsing plus every structural check that does not need a Document
/// (see flow_graph.h's doc comment). Edge/type/fan-in/cycle validation is shared with
/// FlowRuntime::load() via validateGraph() (flow_bindings.cpp). See docs/tasks/T-56.md.

namespace xt::flow {

using nlohmann::json;

namespace {

/// Converts one JSON scalar (bool/number/string) into a PortValue. False for anything else.
bool scalarFromJson(const json& val, PortValue& out) {
    if (val.is_boolean()) {
        out.type = PortType::Bool;
        out.b = val.get<bool>();
        return true;
    }
    if (val.is_number()) {
        out.type = PortType::Number;
        out.n = val.get<double>();
        return true;
    }
    if (val.is_string()) {
        out.type = PortType::String;
        out.s = val.get<std::string>();
        return true;
    }
    return false;
}

/// Converts a JSON object's values into NodeDesc params: scalars into `out`, arrays (T-123's
/// `lookup.values`) into `arrays`. Returns false (with an error pushed) for any entry whose JSON
/// type is neither, for an empty array, and for an array whose elements are not all one type.
bool paramsFromJson(const json& obj, const std::string& nodeId, std::map<std::string, PortValue>& out,
                    std::map<std::string, std::vector<PortValue>>& arrays, std::vector<std::string>& errors) {
    bool ok = true;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const json& val = it.value();
        if (val.is_array()) {
            std::vector<PortValue> items;
            bool itemsOk = !val.empty();
            for (const json& element : val) {
                PortValue v;
                if (!scalarFromJson(element, v) || (!items.empty() && items.front().type != v.type)) {
                    itemsOk = false;
                    break;
                }
                items.push_back(std::move(v));
            }
            if (!itemsOk) {
                errors.push_back("node '" + nodeId + "' param '" + it.key() +
                                 "' must be a non-empty array of same-typed values");
                ok = false;
                continue;
            }
            arrays[it.key()] = std::move(items);
            continue;
        }
        PortValue v;
        if (!scalarFromJson(val, v)) {
            errors.push_back("node '" + nodeId + "' param '" + it.key() + "' has an unsupported JSON type");
            ok = false;
            continue;
        }
        out[it.key()] = v;
    }
    return ok;
}

/// Assigns one PortValue param's JSON scalar (toJson()'s node/edge "params" entries) into
/// `slot`.
void assignParamValue(nlohmann::ordered_json& slot, const PortValue& v) {
    switch (v.type) {
        case PortType::Bool:
            slot = v.b;
            return;
        case PortType::Number:
            slot = v.n;
            return;
        case PortType::String:
            slot = v.s;
            return;
        case PortType::Pulse:
            // Params are never Pulse-typed (paramsFromJson() only ever produces Bool/Number/
            // String); kept for exhaustiveness. Conservative fallback: null.
            slot = nullptr;
            return;
    }
    slot = nullptr;
}

bool requireParam(const NodeDesc& node, const char* name, std::vector<std::string>& errors) {
    if (node.params.count(name) == 0) {
        errors.push_back("node '" + node.id + "' is missing required param '" + std::string(name) + "'");
        return false;
    }
    return true;
}

void checkOp(const NodeDesc& node, const std::set<std::string>& allowed, std::vector<std::string>& errors) {
    if (!requireParam(node, "op", errors)) {
        return;
    }
    const PortValue& opValue = node.params.at("op");
    if (opValue.type != PortType::String || allowed.count(opValue.s) == 0) {
        errors.push_back("node '" + node.id + "' has an invalid 'op'");
    }
}

}  // namespace

Status FlowGraph::parse(std::string_view jsonText, FlowGraph& out, std::vector<std::string>& errors) {
    // T-262c: no object or array deeper than 32 levels is built (nlohmann's copy constructor and
    // dump() recurse). xt_flow sits below xt_bundle, so this is the same callback as
    // bundle/src/bundle_json.h's parseBounded() rather than a call to it.
    constexpr int kMaxDepth = 32;
    bool tooDeep = false;
    const json::parser_callback_t callback = [&tooDeep](int depth, json::parse_event_t event,
                                                        json& /*parsed*/) {
        if ((event == json::parse_event_t::object_start || event == json::parse_event_t::array_start) &&
            depth >= kMaxDepth) {
            tooDeep = true;
            return false;
        }
        return true;
    };
    json root;
    try {
        root = json::parse(jsonText.begin(), jsonText.end(), callback);
    } catch (const json::exception& ex) {
        if (tooDeep) {
            errors.push_back(".xtflow: nesting too deep");
            return Status::kInvalidArgument;
        }
        errors.push_back(std::string("JSON parse error: ") + ex.what());
        return Status::kInvalidArgument;
    }
    if (tooDeep) {
        errors.push_back(".xtflow: nesting too deep");
        return Status::kInvalidArgument;
    }

    // T-406 (43 §43.2): missing = 1; a newer version is refused before anything else is read.
    if (root.is_object()) {
        if (const auto versionIt = root.find("version"); versionIt != root.end()) {
            const bool integer = versionIt->is_number_integer() || versionIt->is_number_unsigned();
            const long long version = integer ? versionIt->get<long long>() : 0;
            if (!integer || version < 1) {
                errors.push_back(".xtflow: 'version' is not a positive integer");
                return Status::kInvalidArgument;
            }
            if (version > kFlowFormatVersion) {
                errors.push_back(".xtflow format version " + std::to_string(version) +
                                 " is newer than this XTFrame supports (" +
                                 std::to_string(kFlowFormatVersion) + "); a newer XTFrame is needed");
                return Status::kUnsupported;
            }
        }
    }

    if (!root.is_object() || !root.contains("nodes") || !root["nodes"].is_array() ||
        !root.contains("edges") || !root["edges"].is_array()) {
        errors.push_back("malformed .xtflow document: expected an object with 'nodes' and 'edges' arrays");
        return Status::kInvalidArgument;
    }

    FlowGraph result;
    std::set<std::string> seenIds;

    for (const json& nodeJson : root["nodes"]) {
        if (!nodeJson.is_object() || !nodeJson.contains("id") || !nodeJson["id"].is_string() ||
            !nodeJson.contains("type") || !nodeJson["type"].is_string()) {
            errors.push_back("malformed node entry (missing 'id'/'type')");
            continue;
        }
        NodeDesc node;
        node.id = nodeJson["id"].get<std::string>();
        node.type = nodeJson["type"].get<std::string>();
        if (!seenIds.insert(node.id).second) {
            errors.push_back("duplicate node id '" + node.id + "'");
        }
        NodeType type;
        if (!parseNodeType(node.type, type)) {
            errors.push_back("unknown node type '" + node.type + "' for node '" + node.id + "'");
            continue;
        }
        if (nodeJson.contains("params")) {
            if (!nodeJson["params"].is_object()) {
                errors.push_back("node '" + node.id + "' 'params' must be an object");
            } else {
                paramsFromJson(nodeJson["params"], node.id, node.params, node.arrayParams, errors);
            }
        }
        switch (type) {
            case NodeType::Constant:
                requireParam(node, "value", errors);
                break;
            case NodeType::Timer:
                // T-258b (PR #269 review): the authored interval is kept as written (the editor
                // saves what it parsed); FlowRuntime::tick() runs one below 1 ms at 1 ms.
                requireParam(node, "interval", errors);
                break;
            case NodeType::Math:
                checkOp(node, {"add", "sub", "mul", "div", "min", "max", "clamp", "map", "abs", "round"},
                        errors);
                break;
            case NodeType::Compare:
                checkOp(node, {"eq", "ne", "lt", "le", "gt", "ge"}, errors);
                break;
            case NodeType::Radio:
                // §17.6 (T-123): `count` decides how many `set<i>` ports the node has, so it is
                // required and has to be a whole number in 2..64.
                if (requireParam(node, "count", errors)) {
                    const PortValue& count = node.params.at("count");
                    if (count.type != PortType::Number || count.n < 2 || count.n > 64 ||
                        count.n != std::floor(count.n)) {
                        errors.push_back("node '" + node.id + "' has an invalid 'count' (expected 2..64)");
                    }
                }
                break;
            case NodeType::Lookup:
                // §17.6 (T-123): `values` is the lookup table; its element type is `out`'s type,
                // so an empty or mixed-type array (rejected by paramsFromJson()) has none.
                if (node.arrayParams.count("values") == 0) {
                    errors.push_back("node '" + node.id + "' is missing required param 'values'");
                }
                break;
            default:
                break;
        }
        // T-193: the action / timeline nodes name what they act on with one string param.
        if (const char* name = requiredStringParam(type);
            name != nullptr && requireParam(node, name, errors)) {
            const PortValue& value = node.params.at(name);
            if (value.type != PortType::String || value.s.empty()) {
                errors.push_back("node '" + node.id + "' param '" + name + "' must be a non-empty string");
            }
        }
        // T-193: the canvas position (editor-only); a malformed one is dropped, never an error.
        if (auto pos = nodeJson.find("pos"); pos != nodeJson.end() && pos->is_array() && pos->size() == 2 &&
                                             (*pos)[0].is_number() && (*pos)[1].is_number()) {
            node.hasPos = true;
            node.posX = (*pos)[0].get<double>();
            node.posY = (*pos)[1].get<double>();
        }
        result.nodes.push_back(std::move(node));
    }

    for (const json& edgeJson : root["edges"]) {
        if (!edgeJson.is_object() || !edgeJson.contains("from") || !edgeJson["from"].is_string() ||
            !edgeJson.contains("to") || !edgeJson["to"].is_string()) {
            errors.push_back("malformed edge entry (missing 'from'/'to')");
            continue;
        }
        EdgeDesc edge;
        edge.from = edgeJson["from"].get<std::string>();
        edge.to = edgeJson["to"].get<std::string>();
        if (edgeJson.contains("params")) {
            if (!edgeJson["params"].is_object()) {
                errors.push_back("edge '" + edge.from + "' -> '" + edge.to + "' 'params' must be an object");
            } else {
                for (auto it = edgeJson["params"].begin(); it != edgeJson["params"].end(); ++it) {
                    if (!it.value().is_string()) {
                        errors.push_back("edge '" + edge.from + "' -> '" + edge.to + "' param '" + it.key() +
                                         "' must be a string");
                        continue;
                    }
                    edge.params[it.key()] = it.value().get<std::string>();
                }
            }
        }
        result.edges.push_back(std::move(edge));
    }

    // T-193: canvas annotation boxes (editor-only); malformed entries are dropped.
    if (auto groups = root.find("groups"); groups != root.end() && groups->is_array()) {
        for (const json& groupJson : *groups) {
            if (!groupJson.is_object()) {
                continue;
            }
            auto rect = groupJson.find("rect");
            if (rect == groupJson.end() || !rect->is_array() || rect->size() != 4) {
                continue;
            }
            bool numeric = true;
            for (const json& v : *rect) {
                numeric = numeric && v.is_number();
            }
            if (!numeric) {
                continue;
            }
            GroupDesc group;
            group.x = (*rect)[0].get<double>();
            group.y = (*rect)[1].get<double>();
            group.w = (*rect)[2].get<double>();
            group.h = (*rect)[3].get<double>();
            if (auto title = groupJson.find("title"); title != groupJson.end() && title->is_string()) {
                group.title = title->get<std::string>();
            }
            if (auto color = groupJson.find("color"); color != groupJson.end() && color->is_string()) {
                group.color = color->get<std::string>();
            }
            result.groups.push_back(std::move(group));
        }
    }

    if (!errors.empty()) {
        return Status::kInvalidArgument;
    }

    Status validated = validateGraph(result, nullptr, errors, nullptr);
    if (validated != Status::kOk) {
        return validated;
    }

    out = std::move(result);
    return Status::kOk;
}

std::string FlowGraph::toJson() const {
    using nlohmann::ordered_json;

    ordered_json root;
    root["version"] = kFlowFormatVersion;

    ordered_json nodesJson = ordered_json::array();
    for (const NodeDesc& n : nodes) {
        ordered_json nj;
        nj["id"] = n.id;
        nj["type"] = n.type;
        if (!n.params.empty() || !n.arrayParams.empty()) {
            ordered_json params;
            for (const auto& [key, value] : n.params) {
                assignParamValue(params[key], value);
            }
            for (const auto& [key, values] : n.arrayParams) {
                ordered_json items = ordered_json::array();
                for (const PortValue& value : values) {
                    ordered_json item;
                    assignParamValue(item, value);
                    items.push_back(std::move(item));
                }
                params[key] = std::move(items);
            }
            nj["params"] = std::move(params);
        }
        if (n.hasPos) {  // T-193: tail key, editor-only.
            nj["pos"] = ordered_json::array({n.posX, n.posY});
        }
        nodesJson.push_back(std::move(nj));
    }
    root["nodes"] = std::move(nodesJson);

    ordered_json edgesJson = ordered_json::array();
    for (const EdgeDesc& e : edges) {
        ordered_json ej;
        ej["from"] = e.from;
        ej["to"] = e.to;
        if (!e.params.empty()) {
            ordered_json params;
            for (const auto& [key, value] : e.params) {
                params[key] = value;
            }
            ej["params"] = std::move(params);
        }
        edgesJson.push_back(std::move(ej));
    }
    root["edges"] = std::move(edgesJson);

    // T-193: tail key, only when there is a box, so a graph without any keeps its old bytes.
    if (!groups.empty()) {
        ordered_json groupsJson = ordered_json::array();
        for (const GroupDesc& g : groups) {
            ordered_json gj;
            gj["title"] = g.title;
            gj["rect"] = ordered_json::array({g.x, g.y, g.w, g.h});
            if (!g.color.empty()) {
                gj["color"] = g.color;
            }
            groupsJson.push_back(std::move(gj));
        }
        root["groups"] = std::move(groupsJson);
    }

    return root.dump(2);
}

std::span<const NodeTypeDesc> nodeTypeTable() {
    struct Entry {
        NodeType type;
        const char* name;
        std::vector<std::string> params;
        std::vector<std::string> inputNames;
    };
    // clang-format off
    static const std::vector<Entry> kEntries = {
        {NodeType::Constant, "constant", {"value"}, {}},
        {NodeType::Counter,  "counter",  {"step", "initial", "min", "max"}, {"increment", "decrement", "reset", "set"}},
        {NodeType::Toggle,   "toggle",   {"initial"}, {"toggle", "setTrue", "setFalse"}},
        {NodeType::Timer,    "timer",    {"interval", "autostart", "repeat"}, {"start", "stop"}},
        {NodeType::Clock,    "clock",    {}, {}},
        {NodeType::Random,   "random",   {"min", "max", "integer"}, {"trigger"}},
        {NodeType::Math,     "math",     {"op", "lo", "hi", "inLo", "inHi", "outLo", "outHi"}, {"a", "b"}},
        {NodeType::Compare,  "compare",  {"op"}, {"a", "b"}},
        {NodeType::ToString, "toString", {"format", "decimals"}, {"in"}},
        {NodeType::Select,   "select",   {}, {"cond", "a", "b"}},
        {NodeType::Radio,    "radio",    {"count", "initial"}, {"set0", "set1", "set"}},
        {NodeType::Lookup,   "lookup",   {}, {"index"}},
        {NodeType::Merge,    "merge",    {}, {"in"}},
        {NodeType::Format,   "format",   {"format", "decimals"}, {"a", "b", "c", "d"}},
        // T-193 (docs/design/27-interaction.md §27.4.1).
        {NodeType::System,   "system",   {}, {}},
        {NodeType::Sound,    "sound",    {"cue", "volume"}, {"trigger"}},
        {NodeType::Haptic,   "haptic",   {"cue"}, {"trigger"}},
        {NodeType::Spawn,    "spawn",    {"symbol"}, {"trigger"}},
        {NodeType::Emit,     "emit",     {"port"}, {"trigger", "value"}},
        {NodeType::Timeline, "timeline", {"name", "scope", "ui", "loop", "rate"}, {"play", "stop", "seek"}},
    };
    // clang-format on

    static const std::vector<NodeTypeDesc> kTable = [] {
        std::vector<NodeTypeDesc> result;
        result.reserve(kEntries.size());
        for (const Entry& e : kEntries) {
            NodeTypeDesc desc;
            desc.type = e.name;
            desc.params = e.params;
            NodeDesc dummy;  // Empty params: nodePortInfo()'s only param-dependent case
                             // (Constant's "out" type mirroring its "value" param) falls back to
                             // PortType::Number, a reasonable static default for this table.
            dummy.id = e.name;
            dummy.type = e.name;
            for (const std::string& portName : e.inputNames) {
                PortType t{};
                if (nodePortInfo(e.type, dummy, portName, /*isSource=*/false, t)) {
                    desc.inputs.push_back(NodeTypePort{portName, t});
                }
            }
            for (const std::string& portName : outputPortOrder(e.type)) {
                PortType t{};
                if (nodePortInfo(e.type, dummy, portName, /*isSource=*/true, t)) {
                    desc.outputs.push_back(NodeTypePort{portName, t});
                }
            }
            result.push_back(std::move(desc));
        }
        return result;
    }();

    return kTable;
}

}  // namespace xt::flow
