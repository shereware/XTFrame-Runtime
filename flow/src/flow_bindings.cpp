#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>

#include "flow_internal.h"
#include "xt/charconv.h"
#include "xt/context.h"
#include "xt/event.h"
#include "xt/property.h"

/// The §17.6 connection-point port tables, the shared graph validator (used by both
/// FlowGraph::parse() and FlowRuntime::load(), see flow_internal.h) and the document endpoint
/// layer shared by FlowRuntime and HostPorts: binding connection points to the live Document
/// (addListener for outputs; ElementRef/Document calls for inputs). See docs/tasks/T-56.md,
/// docs/tasks/T-144.md.

namespace xt::flow {

bool parseNodeType(std::string_view name, NodeType& out) {
    if (name == "constant") {
        out = NodeType::Constant;
    } else if (name == "counter") {
        out = NodeType::Counter;
    } else if (name == "toggle") {
        out = NodeType::Toggle;
    } else if (name == "timer") {
        out = NodeType::Timer;
    } else if (name == "clock") {
        out = NodeType::Clock;
    } else if (name == "random") {
        out = NodeType::Random;
    } else if (name == "math") {
        out = NodeType::Math;
    } else if (name == "compare") {
        out = NodeType::Compare;
    } else if (name == "toString") {
        out = NodeType::ToString;
    } else if (name == "select") {
        out = NodeType::Select;
    } else if (name == "radio") {
        out = NodeType::Radio;
    } else if (name == "lookup") {
        out = NodeType::Lookup;
    } else if (name == "merge") {
        out = NodeType::Merge;
    } else if (name == "format") {
        out = NodeType::Format;
    } else if (name == "system") {
        out = NodeType::System;
    } else if (name == "sound") {
        out = NodeType::Sound;
    } else if (name == "haptic") {
        out = NodeType::Haptic;
    } else if (name == "spawn") {
        out = NodeType::Spawn;
    } else if (name == "emit") {
        out = NodeType::Emit;
    } else if (name == "timeline") {
        out = NodeType::Timeline;
    } else {
        return false;
    }
    return true;
}

const char* requiredStringParam(NodeType type) {
    switch (type) {
        case NodeType::Sound:
        case NodeType::Haptic:
            return "cue";
        case NodeType::Spawn:
            return "symbol";
        case NodeType::Emit:
            return "port";
        case NodeType::Timeline:
            return "name";
        default:
            return nullptr;
    }
}

uint32_t radioCount(const NodeDesc& node) {
    auto it = node.params.find("count");
    if (it == node.params.end() || it->second.type != PortType::Number) {
        return 2;
    }
    double n = it->second.n;
    if (n < 2 || n > 64) {
        return 2;
    }
    return static_cast<uint32_t>(n);
}

PortType lookupValueType(const NodeDesc& node) {
    auto it = node.arrayParams.find("values");
    if (it == node.arrayParams.end() || it->second.empty()) {
        return PortType::Number;
    }
    return it->second.front().type;
}

namespace {

/// True when `port` is `radio`'s "set<i>" for an `i` below `count` (T-123). "set" itself is the
/// separate number input and is handled by the caller. A leading zero ("set01") is rejected, so
/// every port has exactly one spelling.
bool isRadioSetPort(const std::string& port, uint32_t count) {
    if (port.size() < 4 || port.compare(0, 3, "set") != 0) {
        return false;
    }
    if (port[3] == '0' && port.size() != 4) {
        return false;
    }
    uint32_t index = 0;
    for (size_t i = 3; i < port.size(); ++i) {
        if (port[i] < '0' || port[i] > '9' || index > 64) {
            return false;
        }
        index = index * 10 + static_cast<uint32_t>(port[i] - '0');
    }
    return index < count;
}

}  // namespace

bool nodePortInfo(NodeType type, const NodeDesc& node, const std::string& port, bool isSource,
                  PortType& outType) {
    switch (type) {
        case NodeType::Constant:
            if (isSource && port == "out") {
                auto it = node.params.find("value");
                outType = (it != node.params.end()) ? it->second.type : PortType::Number;
                return true;
            }
            return false;
        case NodeType::Counter:
            if (!isSource) {
                if (port == "increment" || port == "decrement" || port == "reset") {
                    outType = PortType::Pulse;
                    return true;
                }
                if (port == "set") {  // T-123.
                    outType = PortType::Number;
                    return true;
                }
                return false;
            }
            if (port == "value") {
                outType = PortType::Number;
                return true;
            }
            return false;
        case NodeType::Toggle:
            if (!isSource) {
                if (port == "toggle" || port == "setTrue" || port == "setFalse") {
                    outType = PortType::Pulse;
                    return true;
                }
                return false;
            }
            if (port == "value") {
                outType = PortType::Bool;
                return true;
            }
            if (port == "changed") {
                outType = PortType::Pulse;
                return true;
            }
            return false;
        case NodeType::Timer:
            if (!isSource) {
                if (port == "start" || port == "stop") {
                    outType = PortType::Pulse;
                    return true;
                }
                return false;
            }
            if (port == "tick") {
                outType = PortType::Pulse;
                return true;
            }
            if (port == "count") {
                outType = PortType::Number;
                return true;
            }
            return false;
        case NodeType::Clock:
            if (isSource && port == "seconds") {
                outType = PortType::Number;
                return true;
            }
            return false;
        case NodeType::Random:
            if (!isSource) {
                if (port == "trigger") {
                    outType = PortType::Pulse;
                    return true;
                }
                return false;
            }
            if (port == "value") {
                outType = PortType::Number;
                return true;
            }
            return false;
        case NodeType::Math:
            if (!isSource) {
                if (port == "a" || port == "b") {
                    outType = PortType::Number;
                    return true;
                }
                return false;
            }
            if (port == "out") {
                outType = PortType::Number;
                return true;
            }
            return false;
        case NodeType::Compare:
            // "a"/"b" are flexible (Number or String, matched to each other, not statically
            // typed here); see isFlexibleSink() below. The placeholder type recorded is never
            // relied on for those two ports.
            if (!isSource) {
                if (port == "a" || port == "b") {
                    outType = PortType::Number;
                    return true;
                }
                return false;
            }
            if (port == "out") {
                outType = PortType::Bool;
                return true;
            }
            if (port == "true" || port == "false") {
                outType = PortType::Pulse;
                return true;
            }
            return false;
        case NodeType::ToString:
            // "in" is flexible (any non-pulse value type); see isFlexibleSink() below.
            if (!isSource) {
                if (port == "in") {
                    outType = PortType::String;
                    return true;
                }
                return false;
            }
            if (port == "out") {
                outType = PortType::String;
                return true;
            }
            return false;
        case NodeType::Select:
            // "a"/"b"/"out" all carry one and the same type, inferred from the edges that reach
            // "a"/"b" (T-123, see inferSelectTypes() below). The placeholder recorded here is
            // what a caller without that inference (nodeTypeTable(), FlowRuntime::load()'s
            // values() seeding) sees.
            if (!isSource) {
                if (port == "cond") {
                    outType = PortType::Bool;
                    return true;
                }
                if (port == "a" || port == "b") {
                    outType = PortType::Bool;
                    return true;
                }
                return false;
            }
            if (port == "out") {
                outType = PortType::Bool;
                return true;
            }
            return false;
        case NodeType::Radio:
            if (!isSource) {
                if (isRadioSetPort(port, radioCount(node))) {
                    outType = PortType::Pulse;
                    return true;
                }
                if (port == "set") {
                    outType = PortType::Number;
                    return true;
                }
                return false;
            }
            if (port == "index") {
                outType = PortType::Number;
                return true;
            }
            if (port == "changed") {
                outType = PortType::Pulse;
                return true;
            }
            return false;
        case NodeType::Lookup:
            if (!isSource) {
                if (port == "index") {
                    outType = PortType::Number;
                    return true;
                }
                return false;
            }
            if (port == "out") {
                outType = lookupValueType(node);
                return true;
            }
            return false;
        case NodeType::Merge:
            if (port == "in" && !isSource) {
                outType = PortType::Pulse;
                return true;
            }
            if (port == "out" && isSource) {
                outType = PortType::Pulse;
                return true;
            }
            return false;
        case NodeType::Format:
            // "a".."d" are flexible (any non-pulse value type); see isFlexibleSink() below.
            if (!isSource) {
                if (port == "a" || port == "b" || port == "c" || port == "d") {
                    outType = PortType::String;
                    return true;
                }
                return false;
            }
            if (port == "out") {
                outType = PortType::String;
                return true;
            }
            return false;
        case NodeType::System:
            // T-193 (27.4.1): driven by FlowRuntime::postSystemEvent(); outputs only.
            if (!isSource) {
                return false;
            }
            if (port == "loading" || port == "paused" || port == "resumed" || port == "disconnected" ||
                port == "back") {  // T-240: back.
                outType = PortType::Pulse;
                return true;
            }
            if (port == "languageChanged" || port == "platform" || port == "inputDevice" ||
                port == "reducedMotion") {  // T-330: reducedMotion.
                outType = PortType::String;
                return true;
            }
            return false;
        case NodeType::Sound:
        case NodeType::Haptic:
        case NodeType::Spawn:
            if (!isSource && port == "trigger") {
                outType = PortType::Pulse;
                return true;
            }
            return false;
        case NodeType::Emit:
            // "value" is flexible (any non-pulse value type); see isFlexibleSink() below.
            if (isSource) {
                return false;
            }
            if (port == "trigger") {
                outType = PortType::Pulse;
                return true;
            }
            if (port == "value") {
                outType = PortType::String;
                return true;
            }
            return false;
        case NodeType::Timeline:
            if (!isSource) {
                if (port == "play" || port == "stop") {
                    outType = PortType::Pulse;
                    return true;
                }
                if (port == "seek") {
                    outType = PortType::Number;
                    return true;
                }
                return false;
            }
            if (port == "end") {
                outType = PortType::Pulse;
                return true;
            }
            if (port == "marker") {
                outType = PortType::String;
                return true;
            }
            return false;
    }
    return false;
}

bool elementPortInfo(const std::string& port, bool isSource, PortType& outType) {
    if (isSource) {
        if (port == "Click" || port == "PointerEnter" || port == "PointerLeave" || port == "Focus" ||
            port == "Blur" || port == "AnimationEnd") {
            outType = PortType::Pulse;
            return true;
        }
        if (port == "Change") {
            outType = PortType::String;
            return true;
        }
        if (port == "hover") {
            outType = PortType::Bool;
            return true;
        }
        return false;
    }
    // T-123 adds `playAnimation` (a `@keyframes` name) and `stopAnimation` to §17.6's row 1.
    if (port == "setText" || port == "setStyle" || port == "setValue" || port == "playAnimation") {
        outType = PortType::String;
        return true;
    }
    if (port == "addClass" || port == "removeClass" || port == "toggleClass" || port == "focus" ||
        port == "stopAnimation") {
        outType = PortType::Pulse;
        return true;
    }
    if (port == "setProperty") {
        outType = PortType::Number;
        return true;
    }
    return false;
}

bool animPortInfo(const std::string& port, bool isSource, PortType& outType) {
    if (isSource) {
        if (port == "Marker") {
            outType = PortType::String;
            return true;
        }
        if (port == "localTime") {
            outType = PortType::Number;
            return true;
        }
        if (port == "Finished") {
            outType = PortType::Pulse;
            return true;
        }
        return false;
    }
    if (port == "play" || port == "pause" || port == "resume") {
        outType = PortType::Pulse;
        return true;
    }
    if (port == "seek" || port == "setSpeed") {
        outType = PortType::Number;
        return true;
    }
    return false;
}

bool docPortInfo(const std::string& port, PortType& outType) {
    if (port == "clock" || port == "viewportWidth" || port == "viewportHeight" || port == "pointerX" ||
        port == "pointerY" || port == "pointerNX" || port == "pointerNY") {
        outType = PortType::Number;
        return true;
    }
    if (port == "pointerDown") {
        outType = PortType::Bool;
        return true;
    }
    return false;
}

bool isPointerDocPort(const std::string& port) {
    return port == "pointerX" || port == "pointerY" || port == "pointerNX" || port == "pointerNY" ||
           port == "pointerDown";
}

bool isValueSignal(Endpoint::Owner owner, const std::string& port, PortType type) {
    switch (owner) {
        case Endpoint::Owner::Doc:
            return true;  // pointer*, viewport*, clock: all values.
        case Endpoint::Owner::Element:
            return port == "hover";
        case Endpoint::Owner::Anim:
            return port == "localTime";
        case Endpoint::Owner::Port:
            return type != PortType::Pulse;
        default:
            return false;
    }
}

std::vector<std::string> outputPortOrder(NodeType type) {
    switch (type) {
        case NodeType::Constant:
            return {"out"};
        case NodeType::Counter:
            return {"value"};
        case NodeType::Toggle:
            return {"value", "changed"};
        case NodeType::Timer:
            return {"tick", "count"};
        case NodeType::Clock:
            return {"seconds"};
        case NodeType::Random:
            return {"value"};
        case NodeType::Math:
            return {"out"};
        case NodeType::Compare:
            return {"out", "true", "false"};
        case NodeType::ToString:
            return {"out"};
        case NodeType::Select:
            return {"out"};
        case NodeType::Radio:
            return {"index", "changed"};
        case NodeType::Lookup:
        case NodeType::Merge:
        case NodeType::Format:
            return {"out"};
        case NodeType::System:
            return {"loading",  "paused",      "resumed", "disconnected", "languageChanged",
                    "platform", "inputDevice", "back",    "reducedMotion"};  // T-330.
        case NodeType::Timeline:
            return {"end", "marker"};
        case NodeType::Sound:
        case NodeType::Haptic:
        case NodeType::Spawn:
        case NodeType::Emit:
            return {};
    }
    return {};
}

bool splitUiEndpoint(const std::string& text, std::string& uiId, std::string& rest, bool& lifecycle,
                     std::string& err) {
    err.clear();
    lifecycle = false;
    if (text.compare(0, 3, "ui#") != 0) {
        return false;
    }
    const std::string body = text.substr(3);
    const size_t dot = body.find('.');
    const std::string tail = dot == std::string::npos ? std::string() : body.substr(dot + 1);
    if (dot == std::string::npos || dot == 0 || tail.empty()) {
        err =
            "malformed UI reference '" + text + "' (expected ui#<uiId>.<port> or ui#<uiId>.<element>.<port>)";
        return false;
    }
    uiId = body.substr(0, dot);
    const size_t dot2 = tail.find('.');
    if (dot2 == std::string::npos) {
        if (tail == "shown" || tail == "hidden") {
            lifecycle = true;
            rest = tail;
        } else {
            rest = "#document." + tail;
        }
        return true;
    }
    if (dot2 == 0 || dot2 + 1 >= tail.size()) {
        err =
            "malformed UI reference '" + text + "' (expected ui#<uiId>.<port> or ui#<uiId>.<element>.<port>)";
        return false;
    }
    rest = "#" + tail;
    return true;
}

namespace {

bool isFlexibleSink(const Endpoint& to) {
    if (to.owner != Endpoint::Owner::Node) {
        return false;
    }
    if (to.nodeType == NodeType::ToString && to.port == "in") {
        return true;
    }
    if (to.nodeType == NodeType::Compare && (to.port == "a" || to.port == "b")) {
        return true;
    }
    if (to.nodeType == NodeType::Format &&
        (to.port == "a" || to.port == "b" || to.port == "c" || to.port == "d")) {
        return true;
    }
    if (to.nodeType == NodeType::Emit && to.port == "value") {  // T-193.
        return true;
    }
    return false;
}

/// `pointerNX`/`pointerNY`: the pointer's offset from the viewport center in halves of the
/// viewport's extent, clamped to -1..1 (docs/design/17-editor.md §17.6). A degenerate (zero or
/// negative) extent has no center to speak of, so it reads as 0.
double normalizedPointer(double pos, double center, double extent) {
    double half = extent * 0.5;
    if (half <= 0) {
        return 0;
    }
    double n = (pos - center) / half;
    return n < -1.0 ? -1.0 : (n > 1.0 ? 1.0 : n);
}

/// One IrEasing rendered back into the 05.6 `easing` grammar mini_css parses, so that
/// `playAnimation` can hand ElementRef::setStyle the element's own declared timing function.
std::string easingText(const IrEasing& e) {
    char buf[96];
    switch (static_cast<EasingKind>(e.kind)) {
        case EasingKind::CubicBezier: {
            std::string text = "cubic-bezier(";
            const float points[4] = {e.x1, e.y1, e.x2, e.y2};
            for (int i = 0; i < 4; ++i) {
                text += i > 0 ? "," : "";
                text.append(buf, xt::formatFloat(buf, sizeof(buf), static_cast<double>(points[i]), 6));
            }
            return text + ")";
        }
        case EasingKind::Steps:
            std::snprintf(buf, sizeof(buf), "steps(%u,%s)", static_cast<unsigned>(e.stepCount),
                          e.stepsJumpEnd != 0 ? "end" : "start");
            return std::string(buf);
        case EasingKind::Hold:
            // 05.6's grammar has no `hold` keyword; `steps(1,start)` is the same curve.
            return "steps(1,start)";
        case EasingKind::Linear:
        case EasingKind::LinearPoints:  // T-335: the points live in the STYL pool; plain linear here
            break;
    }
    return "linear";
}

/// docs/design/17-editor.md §17.6 (T-123): the `animation` declaration text `playAnimation` sends
/// to ElementRef::setStyle for the `@keyframes` name `name` -- "<name> <duration> <easing>". The
/// duration and easing are taken from `element`'s own currently resolved `animation` declaration
/// for that same `@keyframes` (its ComputedStyle points at either the IR's STYL `animations[]`
/// pool or, after an earlier `playAnimation`, Document's runtime one), falling back to the first
/// compiled declaration of the same `@keyframes` anywhere in the stylesheet; "1s linear" when the
/// document declares no animation of that name at all.
std::string animationDeclarationFor(Document& doc, ElementRef element, const std::string& name) {
    uint32_t timeline = kInvalidTimeline;
    for (const IrKeyframesRef& ref : doc.ir().style().keyframesRefs()) {
        if (doc.string(ref.name) == name) {
            timeline = ref.timeline;
            break;
        }
    }
    if (timeline != kInvalidTimeline && element.valid()) {
        const ComputedStyle& style = doc.arena().element(element.node).style;
        std::span<const IrAnimationSpec> pool = doc.ir().style().animations();
        uint32_t offset = style.animationOffset;
        if ((offset & kRuntimeEffectOffsetBit) != 0) {
            offset &= ~kRuntimeEffectOffsetBit;
            pool = std::span<const IrAnimationSpec>(doc.runtimeAnimations());
        }
        const IrAnimationSpec* found = nullptr;
        if (static_cast<uint64_t>(offset) + style.animationCount <= pool.size()) {
            for (const IrAnimationSpec& spec : pool.subspan(offset, style.animationCount)) {
                if (spec.timeline == timeline) {
                    found = &spec;
                    break;
                }
            }
        }
        if (found == nullptr) {
            // The element is not currently declaring this `@keyframes` -- it never did, or an
            // earlier `stopAnimation` replaced its declaration with `animation: none`. Fall back
            // to the first compiled declaration of the same `@keyframes` anywhere in the
            // stylesheet, which for a rule written for this element is the same timing.
            for (const IrAnimationSpec& spec : doc.ir().style().animations()) {
                if (spec.timeline == timeline) {
                    found = &spec;
                    break;
                }
            }
        }
        if (found != nullptr) {
            char buf[64];
            const std::string seconds(
                buf, xt::formatFloat(buf, sizeof(buf), static_cast<double>(found->duration), 6));
            return name + " " + seconds + "s " + easingText(found->easing);
        }
    }
    return name + " 1s linear";
}

std::string sinkIdentity(const Endpoint& to) {
    // T-193: the same port of two UIs is two sinks.
    const std::string ui = to.uiId.empty() ? std::string() : to.uiId + "|";
    switch (to.owner) {
        case Endpoint::Owner::Node:
            return "N:" + std::to_string(to.nodeIndex) + ":" + to.port;
        case Endpoint::Owner::Element:
            return ui + "E:" + to.elementId + ":" + to.port;
        case Endpoint::Owner::Anim:
            return ui + "A:" + to.elementId + ":" + to.port;
        case Endpoint::Owner::Doc:
            return ui + "D:" + to.port;
        case Endpoint::Owner::Port:
            return ui + "P:" + to.elementId + ":" + to.port;
        case Endpoint::Owner::Lifecycle:
            return ui + "L:" + to.port;
    }
    return {};
}

/// Splits a node-port endpoint ("nodeId.port") into its two halves; false for a "#id.port"
/// connection point, a `ui#` reference (T-193) or a string with no dot.
bool splitNodePort(const std::string& text, std::string& id, std::string& port) {
    if (text.empty() || text[0] == '#' || text.compare(0, 3, "ui#") == 0) {
        return false;
    }
    size_t dot = text.find('.');
    if (dot == std::string::npos) {
        return false;
    }
    id = text.substr(0, dot);
    port = text.substr(dot + 1);
    return true;
}

/// The PortType an edge source carries, for inferSelectTypes()' own use: `selectTypes` supplies a
/// `select.out` whose type has already been inferred, and false means "not (yet) known".
bool sourceTypeForInference(const std::string& text, const FlowGraph& graph,
                            const std::map<std::string, int>& nodeIndexById,
                            const std::map<int, PortType>& selectTypes, PortType& outType) {
    std::string uiId, rest, uiErr;
    bool lifecycle = false;
    if (splitUiEndpoint(text, uiId, rest, lifecycle, uiErr)) {  // T-193.
        if (lifecycle) {
            outType = PortType::Pulse;
            return true;
        }
        return sourceTypeForInference(rest, graph, nodeIndexById, selectTypes, outType);
    }
    std::string id, port;
    if (!splitNodePort(text, id, port)) {
        if (text.size() < 2 || text[0] != '#') {
            return false;
        }
        size_t dot = text.find('.', 1);
        if (dot == std::string::npos) {
            return false;
        }
        std::string elementId = text.substr(1, dot - 1);
        std::string elementPort = text.substr(dot + 1);
        if (elementId == "document") {
            return docPortInfo(elementPort, outType);
        }
        return animPortInfo(elementPort, /*isSource=*/true, outType) ||
               elementPortInfo(elementPort, /*isSource=*/true, outType);
    }
    auto it = nodeIndexById.find(id);
    if (it == nodeIndexById.end()) {
        return false;
    }
    const NodeDesc& node = graph.nodes[static_cast<size_t>(it->second)];
    NodeType type;
    if (!parseNodeType(node.type, type)) {
        return false;
    }
    if (type == NodeType::Select && port == "out") {
        auto sel = selectTypes.find(it->second);
        if (sel == selectTypes.end()) {
            return false;
        }
        outType = sel->second;
        return true;
    }
    return nodePortInfo(type, node, port, /*isSource=*/true, outType);
}

/// docs/design/17-editor.md §17.6 (T-123): a `select`'s `a`/`b`/`out` all carry the type of the
/// edges reaching `a`/`b`, so it has to be inferred before any edge can be type-checked. Fills
/// `out` with one entry per `select` node whose type could be determined -- from `a`'s source
/// when that is known, else `b`'s -- iterating to a fixed point so a chain of selects feeding one
/// another resolves in any declaration order (the graph is acyclic, so this terminates). A
/// pulse-typed source is not used for inference: `select` has no pulse form, and leaving the node
/// untyped turns that into validateGraph()'s "select needs a and b".
void inferSelectTypes(const FlowGraph& graph, const std::map<std::string, int>& nodeIndexById,
                      std::map<int, PortType>& out) {
    for (size_t pass = 0; pass <= graph.nodes.size(); ++pass) {
        bool changed = false;
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            NodeType type;
            if (!parseNodeType(graph.nodes[i].type, type) || type != NodeType::Select) {
                continue;
            }
            int index = static_cast<int>(i);
            if (out.count(index) != 0) {
                continue;
            }
            for (const char* port : {"a", "b"}) {
                for (const EdgeDesc& e : graph.edges) {
                    std::string sinkId, sinkPort;
                    if (!splitNodePort(e.to, sinkId, sinkPort) || sinkId != graph.nodes[i].id ||
                        sinkPort != port) {
                        continue;
                    }
                    PortType t{};
                    if (sourceTypeForInference(e.from, graph, nodeIndexById, out, t) &&
                        t != PortType::Pulse) {
                        out[index] = t;
                        changed = true;
                        break;
                    }
                }
                if (out.count(index) != 0) {
                    break;
                }
            }
        }
        if (!changed) {
            return;
        }
    }
}

/// Resolves one edge endpoint string ("#id.port"/"#document.port" or "nodeId.port"). `doc` may be
/// nullptr (see validateGraph()'s doc comment). `selectTypes` is inferSelectTypes()' result: a
/// `select` port listed there reports the inferred type instead of nodePortInfo()'s placeholder.
bool resolveEndpoint(const std::string& text, bool isSource, const FlowGraph& graph,
                     const std::map<std::string, int>& nodeIndexById,
                     const std::map<int, PortType>& selectTypes, Document* doc,
                     const std::map<std::string, Document*>* uiDocs, Endpoint& out, std::string& err) {
    if (text.size() < 2) {
        err = "malformed endpoint '" + text + "'";
        return false;
    }
    // T-193 (27.4): `ui#<uiId>.<element>.<port>` / `ui#<uiId>.<port>` / `ui#<uiId>.shown|hidden`.
    std::string uiId, rest;
    bool lifecycle = false;
    if (splitUiEndpoint(text, uiId, rest, lifecycle, err)) {
        if (uiDocs == nullptr && doc != nullptr) {
            err = "'" + text + "': ui# references are only valid in the global flow (bundle.xtflow)";
            return false;
        }
        Document* target = nullptr;
        if (uiDocs != nullptr) {
            auto it = uiDocs->find(uiId);
            if (it == uiDocs->end()) {
                err = "unknown UI '" + uiId + "' in '" + text + "'";
                return false;
            }
            target = it->second;
        }
        if (lifecycle) {
            if (!isSource) {
                err = "'" + text + "' is a lifecycle output and cannot be an edge target";
                return false;
            }
            out = Endpoint{Endpoint::Owner::Lifecycle, -1, NodeType::Constant, "", rest, PortType::Pulse};
            out.uiId = uiId;
            return true;
        }
        const size_t restDot = rest.find('.', 1);
        if (!resolveDocumentEndpoint(rest.substr(1, restDot - 1), rest.substr(restDot + 1), isSource, target,
                                     out, err)) {
            return false;
        }
        out.uiId = uiId;
        return true;
    }
    if (!err.empty()) {
        return false;  // A malformed `ui#` reference.
    }
    if (text[0] == '#') {
        if (uiDocs != nullptr) {
            err = "'" + text + "': the global flow addresses a UI's ports as ui#<uiId>.<element>.<port>";
            return false;
        }
        size_t dot = text.find('.', 1);
        if (dot == std::string::npos) {
            err = "malformed connection point '" + text + "'";
            return false;
        }
        return resolveDocumentEndpoint(text.substr(1, dot - 1), text.substr(dot + 1), isSource, doc, out,
                                       err);
    }

    size_t dot = text.find('.');
    if (dot == std::string::npos) {
        err = "malformed node port reference '" + text + "'";
        return false;
    }
    std::string id = text.substr(0, dot);
    std::string port = text.substr(dot + 1);
    auto it = nodeIndexById.find(id);
    if (it == nodeIndexById.end()) {
        err = "unknown node id '" + id + "'";
        return false;
    }
    int idx = it->second;
    NodeType type;
    if (!parseNodeType(graph.nodes[static_cast<size_t>(idx)].type, type)) {
        err = "unknown node type '" + graph.nodes[static_cast<size_t>(idx)].type + "' for node '" + id + "'";
        return false;
    }
    PortType t{};
    if (!nodePortInfo(type, graph.nodes[static_cast<size_t>(idx)], port, isSource, t)) {
        err =
            "node '" + id + "' has no " + std::string(isSource ? "output" : "input") + " port '" + port + "'";
        return false;
    }
    if (type == NodeType::Select && (port == "a" || port == "b" || port == "out")) {
        auto sel = selectTypes.find(idx);
        if (sel != selectTypes.end()) {
            t = sel->second;
        }
    }
    out = Endpoint{Endpoint::Owner::Node, idx, type, "", port, t};
    return true;
}

}  // namespace

Status validateGraph(const FlowGraph& graph, Document* doc, std::vector<std::string>& errors,
                     std::vector<BoundEdge>* outEdges, const std::map<std::string, Document*>* uiDocs) {
    std::map<std::string, int> nodeIndexById;
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        nodeIndexById[graph.nodes[i].id] = static_cast<int>(i);
    }

    // T-193: a `timeline` node acts on its own UI's Document, or (global flow) on the UI its `ui`
    // param names.
    for (const NodeDesc& node : graph.nodes) {
        NodeType type;
        if (!parseNodeType(node.type, type) || type != NodeType::Timeline) {
            continue;
        }
        auto ui = node.params.find("ui");
        const bool hasUi = ui != node.params.end();
        if (uiDocs != nullptr) {
            if (!hasUi || ui->second.type != PortType::String || uiDocs->count(ui->second.s) == 0) {
                errors.push_back("node '" + node.id +
                                 "': a global timeline node needs a 'ui' param naming a UI");
            }
        } else if (doc != nullptr && hasUi) {
            errors.push_back("node '" + node.id + "': 'ui' is only valid in the global flow");
        }
    }

    // T-123: `select`'s own type comes from its incoming edges, so it is worked out up front and
    // every `select` that ends up untyped (or missing one of its two inputs) is an error.
    std::map<int, PortType> selectTypes;
    inferSelectTypes(graph, nodeIndexById, selectTypes);
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        NodeType type;
        if (!parseNodeType(graph.nodes[i].type, type) || type != NodeType::Select) {
            continue;
        }
        bool hasA = false, hasB = false;
        for (const EdgeDesc& e : graph.edges) {
            std::string sinkId, sinkPort;
            if (!splitNodePort(e.to, sinkId, sinkPort) || sinkId != graph.nodes[i].id) {
                continue;
            }
            hasA = hasA || sinkPort == "a";
            hasB = hasB || sinkPort == "b";
        }
        if (!hasA || !hasB || selectTypes.count(static_cast<int>(i)) == 0) {
            errors.push_back("node '" + graph.nodes[i].id + "': select needs a and b");
        }
    }

    std::map<std::string, int> valueSinkFanIn;
    std::vector<std::vector<int>> adj(graph.nodes.size());
    std::vector<int> indeg(graph.nodes.size(), 0);
    std::vector<BoundEdge> built;
    built.reserve(graph.edges.size());

    for (const EdgeDesc& e : graph.edges) {
        Endpoint from, to;
        std::string err;
        bool fromOk = resolveEndpoint(e.from, /*isSource=*/true, graph, nodeIndexById, selectTypes, doc,
                                      uiDocs, from, err);
        if (!fromOk) {
            errors.push_back(err);
        }
        bool toOk = resolveEndpoint(e.to, /*isSource=*/false, graph, nodeIndexById, selectTypes, doc, uiDocs,
                                    to, err);
        if (!toOk) {
            errors.push_back(err);
        }
        if (!fromOk || !toOk) {
            continue;
        }

        // A pulse sink fires whenever anything arrives, regardless of the source's payload type
        // (docs/design/17-editor.md §17.6's own sample graph wires a "Change" (string) output
        // into "#root.toggleClass" (pulse) this way -- resolving that contradiction between the
        // port-type table and the worked example in favor of the example; see the T-56 report).
        bool flexibleSink = isFlexibleSink(to);
        if (to.type == PortType::Pulse) {
            // Always OK.
        } else if (flexibleSink) {
            if (from.type == PortType::Pulse) {
                errors.push_back("cannot connect a pulse output to '" + e.to + "'");
                continue;
            }
        } else if (from.type != to.type && !from.anyType && !to.anyType) {
            errors.push_back("type mismatch on edge '" + e.from + "' -> '" + e.to + "'");
            continue;
        }

        // T-123: an element's `setStyle` is the one value input that takes several edges -- each
        // arrival merges its own declarations into the element's runtime style, so several
        // sources can drive different properties of the same element.
        // T-193 (27.3.1): `instance` narrows the edge to one instance of a multi-instance UI.
        int64_t instance = -1;
        if (auto it = e.params.find("instance"); it != e.params.end()) {
            const std::string& text = it->second;
            bool digits = !text.empty() && text.size() <= 10;
            for (char c : text) {
                digits = digits && c >= '0' && c <= '9';
            }
            if (!digits || std::strtoull(text.c_str(), nullptr, 10) > 0xFFFFFFFFull) {
                errors.push_back("edge '" + e.from + "' -> '" + e.to +
                                 "' has an invalid 'instance' (expected a non-negative integer)");
                continue;
            }
            instance = static_cast<int64_t>(std::strtoull(text.c_str(), nullptr, 10));
        }

        bool multiEdgeSink = to.owner == Endpoint::Owner::Element && to.port == "setStyle";
        if (to.type != PortType::Pulse && !multiEdgeSink) {
            // `setProperty` is one port per element but addresses a different target per `prop`
            // edge parameter, so RotationX and RotationY may each have their own source (§17.6).
            std::string identity = sinkIdentity(to);
            if (instance >= 0) {
                identity += "|i" + std::to_string(instance);
            }
            if (auto propIt = e.params.find("prop"); propIt != e.params.end()) {
                identity += "|" + propIt->second;
            }
            int& count = valueSinkFanIn[identity];
            ++count;
            if (count > 1) {
                errors.push_back("value input '" + e.to + "' has more than one incoming edge");
                continue;
            }
        }

        // A node's pulse-to-pulse self-edge is not a data dependency, so it is left out of the
        // topological graph rather than reported as a cycle (T-123; §17.6's `timer.tick ->
        // timer.stop` one-shot).
        bool pulseSelfEdge = from.owner == Endpoint::Owner::Node && to.owner == Endpoint::Owner::Node &&
                             from.nodeIndex == to.nodeIndex && from.type == PortType::Pulse &&
                             to.type == PortType::Pulse;
        if (from.owner == Endpoint::Owner::Node && to.owner == Endpoint::Owner::Node && !pulseSelfEdge) {
            adj[static_cast<size_t>(from.nodeIndex)].push_back(to.nodeIndex);
            ++indeg[static_cast<size_t>(to.nodeIndex)];
        }

        std::string param;
        if (to.owner != Endpoint::Owner::Node) {
            const char* paramName = documentInputParamName(to.port);
            const std::string* value = nullptr;
            if (paramName != nullptr) {
                if (auto it = e.params.find(paramName); it != e.params.end()) {
                    value = &it->second;
                }
            }
            std::string paramErr;
            if (!checkDocumentInputParam(to.port, value, "edge to '" + e.to + "'", paramErr)) {
                errors.push_back(paramErr);
                continue;
            }
            if (value != nullptr) {
                param = *value;
            }
        }

        BoundEdge bound{from, to, param};
        bound.instance = instance;
        built.push_back(std::move(bound));
    }

    std::vector<int> indegCopy = indeg;
    std::vector<int> queue;
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        if (indegCopy[i] == 0) {
            queue.push_back(static_cast<int>(i));
        }
    }
    size_t processed = 0;
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        int n = queue[qi];
        ++processed;
        for (int m : adj[static_cast<size_t>(n)]) {
            if (--indegCopy[static_cast<size_t>(m)] == 0) {
                queue.push_back(m);
            }
        }
    }
    if (processed != graph.nodes.size()) {
        errors.push_back("cycle detected in the node graph");
    }

    if (!errors.empty()) {
        return Status::kInvalidArgument;
    }
    if (outEdges != nullptr) {
        *outEdges = std::move(built);
    }
    return Status::kOk;
}

// ---- Document endpoint layer (flow_internal.h; shared by FlowRuntime and HostPorts) ------------

std::string externalKey(Endpoint::Owner owner, const std::string& elementId, const std::string& port) {
    char prefix = owner == Endpoint::Owner::Element     ? 'E'
                  : owner == Endpoint::Owner::Anim      ? 'A'
                  : owner == Endpoint::Owner::Port      ? 'P'
                  : owner == Endpoint::Owner::Lifecycle ? 'L'
                                                        : 'D';
    return std::string(1, prefix) + ":" + elementId + ":" + port;
}

bool declaredPortFlowType(PortValueType type, PortType& out) {
    switch (type) {
        case PortValueType::Number:
        case PortValueType::Entity:
            out = PortType::Number;
            return true;
        case PortValueType::Bool:
            out = PortType::Bool;
            return true;
        case PortValueType::Event:
            out = PortType::Pulse;
            return true;
        case PortValueType::Struct:
        case PortValueType::List:
            out = PortType::String;
            return false;
        default:
            out = PortType::String;
            return true;
    }
}

namespace {

/// The Document lookup name of the declared port "#<id>.<port>" ("document" = document level).
std::string declaredPortName(const std::string& id, const std::string& port) {
    return id == "document" ? port : id + "." + port;
}

bool isPortIdent(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) {
        return false;
    }
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

/// T-175: "#<id>.<port>" as a declared port. Without a Document every identifier is accepted
/// (typed at load); with one it must be a declared port of the right direction (`out` is a
/// source, `in` / `call` a sink) and not a struct / list. False with `err` set only when the
/// name is a declared port that cannot be used this way.
bool resolveDeclaredPort(const std::string& id, const std::string& port, bool isSource, Document* doc,
                         Endpoint& out, std::string& err) {
    if (doc == nullptr) {
        if (!isPortIdent(port)) {
            return false;
        }
        out = Endpoint{Endpoint::Owner::Port, -1, NodeType::Constant, id, port, PortType::String};
        out.anyType = true;
        return true;
    }
    PortId found = doc->lookupPort(declaredPortName(id, port));
    if (found == kInvalidPort) {
        return false;
    }
    PortRecord info = doc->portInfo(found);
    PortType t{};
    if (!declaredPortFlowType(info.type, t)) {
        err = "port '#" + id + "." + port + "' is a struct/list port (not connectable yet)";
        return false;
    }
    const bool source = info.dir == PortDir::Out;
    if (source != isSource) {
        err = "port '#" + id + "." + port + "' is " + (source ? "an output (out)" : "an input (in/call)") +
              " port";
        return false;
    }
    out = Endpoint{Endpoint::Owner::Port, -1, NodeType::Constant, id, port, t};
    return true;
}

/// The declared port behind a Port endpoint, as a flow PortValue of the endpoint's type.
PortValue declaredPortValue(Document& doc, PortId port, PortType type) {
    PortValue v = typedValue(type);
    const std::string text = doc.portText(port);
    if (type == PortType::Number) {
        // T-268: strtod(text, nullptr) through xt::fromChars: leading whitespace and one '+'
        // skipped as strtod skipped them, 0 when the text is not a number.
        size_t first = 0;
        while (first < text.size() && (text[first] == ' ' || (text[first] >= '\t' && text[first] <= '\r'))) {
            ++first;
        }
        if (first + 1 < text.size() && text[first] == '+' && text[first + 1] != '-') {
            ++first;
        }
        double n = 0;
        xt::fromChars(text.data() + first, text.data() + text.size(), n);
        v.n = n;
    } else if (type == PortType::Bool) {
        v.b = text == "true";
    } else if (type == PortType::String) {
        v.s = text;
    }
    return v;
}

}  // namespace

bool resolveDocumentEndpoint(const std::string& id, const std::string& port, bool isSource, Document* doc,
                             Endpoint& out, std::string& err) {
    if (id == "document") {
        PortType t;
        if (!docPortInfo(port, t)) {
            err.clear();
            if (resolveDeclaredPort(id, port, isSource, doc, out, err)) {
                return true;  // T-175: a document-level declared port.
            }
            if (err.empty()) {
                err = "unknown document port '" + port + "'";
            }
            return false;
        }
        if (!isSource) {
            err = "the document has no input port '" + port + "'";
            return false;
        }
        out = Endpoint{Endpoint::Owner::Doc, -1, NodeType::Constant, "", port, t};
        return true;
    }
    PortType t{};
    bool isAnimPort = animPortInfo(port, isSource, t);
    bool isElementPort = !isAnimPort && elementPortInfo(port, isSource, t);
    if (!isAnimPort && !isElementPort) {
        err.clear();
        if (resolveDeclaredPort(id, port, isSource, doc, out, err)) {
            return true;  // T-175: a port declared on (or exposed by) element `id`.
        }
        if (err.empty()) {
            err = "unknown " + std::string(isSource ? "output" : "input") + " port '" + port + "' on '#" +
                  id + "'";
        }
        return false;
    }
    if (doc != nullptr) {
        ElementRef ref = doc->byId(id);
        if (!ref.valid()) {
            err = "unknown element id '#" + id + "'";
            return false;
        }
        if (isAnimPort && ref.tag() != "anim") {
            err = "'#" + id + "' is not an <anim> element (port '" + port + "')";
            return false;
        }
    }
    out = Endpoint{
        isAnimPort ? Endpoint::Owner::Anim : Endpoint::Owner::Element, -1, NodeType::Constant, id, port, t};
    return true;
}

const char* documentInputParamName(const std::string& port) {
    if (port == "addClass" || port == "removeClass" || port == "toggleClass") {
        return "class";
    }
    if (port == "setProperty") {
        return "prop";
    }
    return nullptr;
}

bool checkDocumentInputParam(const std::string& port, const std::string* param, const std::string& subject,
                             std::string& err) {
    const char* name = documentInputParamName(port);
    if (name == nullptr) {
        return true;
    }
    if (param == nullptr || param->empty()) {
        err = subject + " is missing the '" + name + "' param";
        return false;
    }
    if (port == "setProperty" && propertyByName(*param) == prop::kInvalidProp) {
        err = subject + " names an unknown property '" + *param + "'";
        return false;
    }
    return true;
}

bool documentEndpointLost(Document& document, const Endpoint& ep, std::vector<std::string>& warnings,
                          std::set<std::string>& warned) {
    if (ep.owner == Endpoint::Owner::Port) {
        if (document.lookupPort(declaredPortName(ep.elementId, ep.port)) != kInvalidPort) {
            return false;
        }
        const std::string name = "#" + ep.elementId + "." + ep.port;
        if (warned.insert(name).second) {
            warnings.push_back(name + ": port not found");
        }
        return true;
    }
    if (ep.owner != Endpoint::Owner::Element && ep.owner != Endpoint::Owner::Anim) {
        return false;  // Node ports and the document's own ports survive any tree change.
    }
    ElementRef ref = document.byId(ep.elementId);
    const char* reason = nullptr;
    if (!ref.valid()) {
        reason = "element not found";
    } else if (ep.owner == Endpoint::Owner::Anim && ref.tag() != "anim") {
        // Same rule as resolveDocumentEndpoint(): a row-2 port needs an actual <anim> element.
        reason = "not an <anim> element";
    }
    if (reason == nullptr) {
        return false;
    }
    const std::string name = "#" + ep.elementId + "." + ep.port;
    if (warned.insert(name).second) {
        warnings.push_back(name + ": " + reason);
    }
    return true;
}

namespace {

/// Registers `<anim>` `elementId`'s "Finished" callback on Animator instance `instanceId`.
void registerFinishedCallback(DocEndpointState& state, const std::string& elementId, uint32_t instanceId) {
    if (instanceId == 0) {
        return;
    }
    // Document has no "unregister animation callback" API, so the callback outlives its owner on
    // paper; the weak liveness token makes a stale one a no-op instead of a use-after-free.
    std::weak_ptr<bool> alive = state.aliveToken;
    DocEndpointState* s = &state;
    state.doc->onAnimationFinished(instanceId, [s, elementId, alive]() {
        if (alive.expired()) {
            return;
        }
        emitDocumentOutput(*s, Endpoint::Owner::Anim, elementId, "Finished", typedValue(PortType::Pulse),
                           /*isPulse=*/true);
    });
}

void setHover(DocEndpointState& state, const std::string& elementId, bool value) {
    PortValue v = typedValue(PortType::Bool);
    v.b = value;
    emitDocumentOutput(state, Endpoint::Owner::Element, elementId, "hover", v, false);
}

void onChangeEvent(DocEndpointState& state, const std::string& elementId) {
    ElementRef& ref = state.elementRefs.at(elementId);
    if (!ref.valid()) {
        return;
    }
    std::string payload;
    if (ref.attribute("type") == "checkbox") {
        payload = ref.hasState(kStateChecked) ? "true" : "false";
    } else {
        payload = std::string(ref.value());
    }
    PortValue v = typedValue(PortType::String);
    v.s = std::move(payload);
    emitDocumentOutput(state, Endpoint::Owner::Element, elementId, "Change", v, false);
}

/// Reports every subscribed pointer port's current value (emitDocumentOutput() drops the
/// unchanged).
void pushPointerPorts(DocEndpointState& state) {
    for (const std::string& port : state.docOutputPorts) {
        if (isPointerDocPort(port)) {
            emitDocumentOutput(state, Endpoint::Owner::Doc, "", port, documentPortValue(state, port), false);
        }
    }
}

/// PointerMove/Down/Up on the document root: records the state and pushes the pointer ports.
/// `downChanged` distinguishes a move (which leaves pointerDown alone) from a button event.
void onPointerEvent(DocEndpointState& state, Vec2 pos, bool downChanged, bool down) {
    state.pointerSeen = true;
    state.pointerPos = pos;
    if (downChanged) {
        state.pointerDown = down;
    }
    // Reported at once; T-389: a FlowRuntime holds these value signals back and delivers the
    // frame's last one from its next tick() (27 §27.4.1), HostPorts passes them on as they come.
    pushPointerPorts(state);
}

/// The element event behind a pulse-type row-1 output port; false for any other port.
bool pulseEventType(const std::string& port, EventType& out) {
    if (port == "Click") {
        out = EventType::Click;
    } else if (port == "PointerEnter") {
        out = EventType::PointerEnter;
    } else if (port == "PointerLeave") {
        out = EventType::PointerLeave;
    } else if (port == "Focus") {
        out = EventType::Focus;
    } else if (port == "Blur") {
        out = EventType::Blur;
    } else if (port == "AnimationEnd") {
        out = EventType::AnimationEnd;
    } else {
        return false;
    }
    return true;
}

}  // namespace

void registerDocumentEndpoint(DocEndpointState& state, const Endpoint& ep) {
    if (ep.owner != Endpoint::Owner::Element && ep.owner != Endpoint::Owner::Anim) {
        return;
    }
    if (state.elementRefs.count(ep.elementId) != 0) {
        return;
    }
    ElementRef ref = state.doc->byId(ep.elementId);
    state.elementRefs[ep.elementId] = ref;
    if (ep.owner == Endpoint::Owner::Anim) {
        uint32_t inst = state.doc->defaultAnimation(ref);
        state.animInstance[ep.elementId] = inst;
        registerFinishedCallback(state, ep.elementId, inst);
    }
}

void applyDocumentInput(DocEndpointState& state, const Endpoint& sink, const PortValue& v,
                        const std::string& param) {
    Document& doc = *state.doc;
    switch (sink.owner) {
        case Endpoint::Owner::Node:
        case Endpoint::Owner::Doc:
        case Endpoint::Owner::Lifecycle:
            return;  // Not a document input port (the document has none, validated at load time).
        case Endpoint::Owner::Port: {
            // T-175: a declared `in` / `call` port; values go in through their text form.
            PortId port = doc.lookupPort(declaredPortName(sink.elementId, sink.port));
            if (port == kInvalidPort) {
                return;
            }
            PortRecord info = doc.portInfo(port);
            std::string text = v.type == PortType::Bool     ? (v.b ? "true" : "false")
                               : v.type == PortType::String ? v.s
                                                            : std::string();
            if (v.type == PortType::Number) {
                char buf[64];
                text.assign(buf, xt::formatFloat(buf, sizeof(buf), v.n, 17));
            }
            if (info.dir == PortDir::Call) {
                if (v.type != PortType::Pulse && info.type != PortValueType::Event) {
                    doc.setPortText(port, text);
                }
                doc.callPort(port, {});
                return;
            }
            doc.setPortText(port, info.type == PortValueType::Event ? std::string() : text);
            return;
        }
        case Endpoint::Owner::Element: {
            ElementRef& ref = state.elementRefs.at(sink.elementId);
            if (!ref.valid()) {
                return;
            }
            if (sink.port == "setText") {
                ref.setText(v.s);
            } else if (sink.port == "setStyle") {
                ref.setStyle(v.s);
            } else if (sink.port == "setValue") {
                ref.setValue(v.s);
            } else if (sink.port == "playAnimation") {
                // T-123: a CSS `animation` declaration is how the runtime plays a `@keyframes`
                // (there is no direct "play this timeline" API on an ordinary element).
                ref.setStyle("animation: " + animationDeclarationFor(doc, ref, v.s));
            } else if (sink.port == "stopAnimation") {
                ref.setStyle("animation: none");
            } else if (sink.port == "focus") {
                ref.focus();
            } else if (sink.port == "addClass") {
                ref.addClass(param);
            } else if (sink.port == "removeClass") {
                ref.removeClass(param);
            } else if (sink.port == "toggleClass") {
                ref.toggleClass(param);
            } else if (sink.port == "setProperty") {
                PropId prop = propertyByName(param);
                if (prop != prop::kInvalidProp) {
                    ref.setProperty(prop, Value::ofFloat(static_cast<float>(v.n)));
                }
            }
            return;
        }
        case Endpoint::Owner::Anim: {
            ElementRef& ref = state.elementRefs.at(sink.elementId);
            if (!ref.valid()) {
                return;
            }
            uint32_t& inst = state.animInstance[sink.elementId];
            if (sink.port == "play") {
                const SceneInstance* si = doc.sceneInstance(ref);
                if (si == nullptr) {
                    return;  // Not applicable (docs/tasks/T-56.md leaves this uncovered): no-op.
                }
                PlayParams params;
                params.timeline = doc.sceneTimeline(si->scene);
                params.localToArena = si->localToArena.data();
                params.localCount = static_cast<uint32_t>(si->localToArena.size());
                params.selfNode = ref.node;
                inst = doc.play(params);
                registerFinishedCallback(state, sink.elementId, inst);
            } else if (sink.port == "pause") {
                if (inst != 0) {
                    doc.pause(inst);
                }
            } else if (sink.port == "resume") {
                if (inst != 0) {
                    doc.resume(inst);
                }
            } else if (sink.port == "seek") {
                if (inst != 0) {
                    doc.seek(inst, static_cast<float>(v.n));
                }
            } else if (sink.port == "setSpeed") {
                if (inst != 0) {
                    doc.setSpeed(inst, static_cast<float>(v.n));
                }
            }
            return;
        }
    }
}

void subscribeDocumentOutput(DocEndpointState& state, const Endpoint& source) {
    // T-193: lifecycle outputs are driven by BundleContext, not by a Document listener.
    if (source.owner == Endpoint::Owner::Node || source.owner == Endpoint::Owner::Lifecycle) {
        return;
    }
    if (!state.subscribed.insert(externalKey(source.owner, source.elementId, source.port)).second) {
        return;
    }
    Document& doc = *state.doc;
    DocEndpointState* s = &state;

    if (source.owner == Endpoint::Owner::Port) {
        // T-175: a declared `out` port reports its value when it changes and on every emit().
        PortId port = doc.lookupPort(declaredPortName(source.elementId, source.port));
        if (port == kInvalidPort) {
            return;
        }
        const std::string id = source.elementId;
        const std::string name = source.port;
        const PortType type = source.type;
        // T-260c: the Document calls a copy of its listener list, so a listener removed by an
        // earlier one in the same emit (a flow load deferred to the end of that one's dispatch)
        // still runs: the liveness token tells it `s` is gone, as for the "finished" callbacks.
        std::weak_ptr<bool> alive = state.aliveToken;
        state.portListenerIds.push_back(
            doc.addPortListener([s, port, id, name, type, alive](const PortEvent& ev) {
                if (alive.expired() || ev.port != port || ev.kind == PortEventKind::ListChanged) {
                    return;
                }
                emitDocumentOutput(*s, Endpoint::Owner::Port, id, name,
                                   declaredPortValue(*s->doc, port, type), type == PortType::Pulse);
            }));
        return;
    }

    if (source.owner == Endpoint::Owner::Doc) {
        // Row-3 outputs are polled (pollDocumentOutputs()); the pointer ones are additionally
        // pushed from their event listeners.
        state.docOutputPorts.push_back(source.port);
        // T-77's #document.pointer* ports. PointerMove/Down/Up are dispatched to the hit (or, while
        // a button is held, the active) element and bubble to the root, so a listener on the
        // document root sees every pointer event the core dispatches; a move over nothing at all
        // (outside the root box) dispatches nothing, and the ports then keep the last known
        // position, which is what §17.6 asks for ("最後一次指標位置"). The values are also polled
        // (so the initial viewport-center reading appears even before the first event).
        ElementRef root = doc.root();
        if (isPointerDocPort(source.port) && !state.pointerSubscribed && root.valid()) {
            state.pointerSubscribed = true;
            state.listenerIds.push_back(doc.addListener(
                root, EventType::PointerMove, [s](Event& ev) { onPointerEvent(*s, ev.pos, false, false); }));
            state.listenerIds.push_back(doc.addListener(
                root, EventType::PointerDown, [s](Event& ev) { onPointerEvent(*s, ev.pos, true, true); }));
            state.listenerIds.push_back(doc.addListener(
                root, EventType::PointerUp, [s](Event& ev) { onPointerEvent(*s, ev.pos, true, false); }));
        }
        return;
    }

    if (source.owner == Endpoint::Owner::Anim && source.port == "localTime") {
        state.animLocalTimeElements.push_back(source.elementId);  // Polled.
        return;
    }
    ElementRef& ref = state.elementRefs.at(source.elementId);
    if (!ref.valid()) {
        return;
    }
    const std::string id = source.elementId;
    if (source.owner == Endpoint::Owner::Element) {
        if (source.port == "hover") {
            state.listenerIds.push_back(
                doc.addListener(ref, EventType::PointerEnter, [s, id](Event&) { setHover(*s, id, true); }));
            state.listenerIds.push_back(
                doc.addListener(ref, EventType::PointerLeave, [s, id](Event&) { setHover(*s, id, false); }));
        } else if (source.port == "Change") {
            state.listenerIds.push_back(
                doc.addListener(ref, EventType::Change, [s, id](Event&) { onChangeEvent(*s, id); }));
        } else {
            EventType et = EventType::Click;
            if (pulseEventType(source.port, et)) {
                std::string port = source.port;
                state.listenerIds.push_back(doc.addListener(ref, et, [s, id, port](Event&) {
                    emitDocumentOutput(*s, Endpoint::Owner::Element, id, port, typedValue(PortType::Pulse),
                                       /*isPulse=*/true);
                }));
            }
        }
    } else if (source.port == "Marker") {
        state.listenerIds.push_back(doc.addListener(ref, EventType::Marker, [s, id](Event& ev) {
            PortValue v = typedValue(PortType::String);
            v.s = std::string(s->doc->string(ev.markerName));
            emitDocumentOutput(*s, Endpoint::Owner::Anim, id, "Marker", v, false);
        }));
    }
    // An `<anim>`'s "Finished" is wired via onAnimationFinished (registerDocumentEndpoint() and
    // applyDocumentInput()'s "play").
}

void pollDocumentOutputs(DocEndpointState& state) {
    // T-260b: checked before every read -- the Document may be reloaded or unloaded under us,
    // also by a host callback that one of these outputs reaches (then nothing more is read until
    // the next load; BundleContext::removeUi leaves these lists in place for that reason).
    for (const std::string& port : state.docOutputPorts) {
        if (liveDocument(state) == nullptr) {
            return;
        }
        // documentPortValue() is the single source for every row-3 port's current value.
        emitDocumentOutput(state, Endpoint::Owner::Doc, "", port, documentPortValue(state, port), false);
    }
    for (const std::string& elementId : state.animLocalTimeElements) {
        if (liveDocument(state) == nullptr) {
            return;
        }
        auto it = state.animInstance.find(elementId);
        uint32_t inst = (it != state.animInstance.end()) ? it->second : 0;
        PortValue pv = typedValue(PortType::Number);
        pv.n = (inst != 0) ? static_cast<double>(state.doc->animationTime(inst)) : 0.0;
        emitDocumentOutput(state, Endpoint::Owner::Anim, elementId, "localTime", pv, false);
    }
}

PortValue documentPortValue(const DocEndpointState& state, const std::string& port) {
    if (port == "pointerDown") {
        PortValue v = typedValue(PortType::Bool);
        v.b = state.pointerDown;
        return v;
    }
    PortValue v = typedValue(PortType::Number);
    if (port == "clock") {
        v.n = static_cast<double>(state.clockAccum);
        return v;
    }
    // Everything below is expressed in the viewport box the document lays out into, which is the
    // root element's border box (the same source viewportWidth/Height already report).
    Rect box = state.doc->root().boundingBox();
    double centerX = static_cast<double>(box.x) + static_cast<double>(box.w) * 0.5;
    double centerY = static_cast<double>(box.y) + static_cast<double>(box.h) * 0.5;
    // Before the first pointer event the position reads as the viewport center (T-77's initial
    // values); it is resolved here rather than at bind time so that a document laid out later
    // still reports its real center.
    double px = state.pointerSeen ? static_cast<double>(state.pointerPos.x) : centerX;
    double py = state.pointerSeen ? static_cast<double>(state.pointerPos.y) : centerY;
    if (port == "viewportWidth") {
        v.n = static_cast<double>(box.w);
    } else if (port == "viewportHeight") {
        v.n = static_cast<double>(box.h);
    } else if (port == "pointerX") {
        v.n = px;
    } else if (port == "pointerY") {
        v.n = py;
    } else if (port == "pointerNX") {
        v.n = normalizedPointer(px, centerX, static_cast<double>(box.w));
    } else if (port == "pointerNY") {
        v.n = normalizedPointer(py, centerY, static_cast<double>(box.h));
    }
    return v;
}

void emitDocumentOutput(DocEndpointState& state, Endpoint::Owner owner, const std::string& elementId,
                        const std::string& port, const PortValue& v, bool isPulse) {
    if (!isPulse) {
        std::string key = externalKey(owner, elementId, port);
        auto it = state.lastOutputs.find(key);
        if (it != state.lastOutputs.end() && valuesEqual(it->second, v)) {
            return;
        }
        state.lastOutputs[key] = v;
    }
    if (state.output) {
        state.output(owner, elementId, port, v);
    }
}

Document* liveDocument(const DocEndpointState& state) {
    if (state.doc == nullptr || state.context == nullptr) {
        return state.doc;
    }
    // T-260c: by load serial, not by address (a later Document may reuse the old one's).
    return state.serial != 0 && state.context->documentSerial(state.handle) == state.serial ? state.doc
                                                                                            : nullptr;
}

void unbindDocumentEndpoints(DocEndpointState& state, bool removeListeners) {
    // T-260b: only from the Document the Context still holds at the handle; a reloaded or
    // unloaded one is gone (or going), so its listeners are only forgotten here.
    Document* doc = removeListeners ? liveDocument(state) : nullptr;
    if (doc != nullptr) {
        for (uint32_t id : state.listenerIds) {
            doc->removeListener(id);
        }
    }
    state.listenerIds.clear();
    if (doc != nullptr) {
        for (uint32_t id : state.portListenerIds) {
            doc->removePortListener(id);
        }
    }
    state.portListenerIds.clear();
    // A fresh token silences every "finished" callback registered against the old one, which
    // Document keeps until the instance finishes (see registerFinishedCallback()).
    state.aliveToken = std::make_shared<bool>(true);
    state.subscribed.clear();
    state.pointerSubscribed = false;
    state.docOutputPorts.clear();
    state.animLocalTimeElements.clear();
}

void detachDocumentEndpoints(DocEndpointState& state) {
    if (Document* doc = liveDocument(state)) {
        for (uint32_t id : state.listenerIds) {
            doc->removeListener(id);
        }
        for (uint32_t id : state.portListenerIds) {
            doc->removePortListener(id);
        }
    }
    state.listenerIds.clear();
    state.portListenerIds.clear();
    state.aliveToken = std::make_shared<bool>(true);  // Silences pending "finished" callbacks.
    state.doc = nullptr;
    state.context = nullptr;
    state.handle = 0;
    state.serial = 0;
}

void resetDocumentTreeState(DocEndpointState& state) {
    state.elementRefs.clear();
    state.animInstance.clear();
    state.lastOutputs.clear();
    state.pointerPos = Vec2{};
    state.pointerSeen = false;
    state.pointerDown = false;
}

// ---- FlowRuntime's use of the layer above -----------------------------------------------------

void FlowRuntime::Impl::bindDocumentEndpoints() {
    // T-193: an endpoint lives in the load() Document (uiId "") or in every instance of its UI.
    for (const BoundEdge& e : edges) {
        for (const Endpoint* ep : {&e.from, &e.to}) {
            for (DocEndpointState* state : statesFor(ep->uiId)) {
                registerDocumentEndpoint(*state, *ep);
            }
        }
    }
    // Element / `<anim>` outputs first, then the document's own (whose pointer listeners go on
    // the root last), each in edge order.
    for (const BoundEdge& e : edges) {
        if (e.from.owner == Endpoint::Owner::Element || e.from.owner == Endpoint::Owner::Anim ||
            e.from.owner == Endpoint::Owner::Port) {
            for (DocEndpointState* state : statesFor(e.from.uiId)) {
                subscribeDocumentOutput(*state, e.from);
            }
        }
    }
    for (const BoundEdge& e : edges) {
        if (e.from.owner == Endpoint::Owner::Doc) {
            for (DocEndpointState* state : statesFor(e.from.uiId)) {
                subscribeDocumentOutput(*state, e.from);
            }
        }
    }
    bindTimelineNodes();
}

void FlowRuntime::Impl::applyToSink(const BoundEdge& edge, const PortValue& v) {
    const Endpoint& sink = edge.to;
    if (sink.owner == Endpoint::Owner::Node) {
        // Route by the SINK's own port type, not the source's: a pulse sink fires whenever
        // anything arrives (see validateGraph()'s matching comment), even from a value-typed
        // source.
        deliverToNode(sink.nodeIndex, sink.port, v, sink.type == PortType::Pulse);
        return;
    }
    // T-193 (27.3.1): an `instance` edge only reaches that instance. T-260b: nor does anything
    // reach a Document that was reloaded or unloaded since the runtime bound it (liveDocument()).
    auto reaches = [&edge](const DocEndpointState& state) {
        const Document* doc = liveDocument(state);
        return doc != nullptr &&
               (edge.instance < 0 || static_cast<int64_t>(doc->instanceId()) == edge.instance);
    };
    if (sink.uiId.empty()) {
        if (reaches(docs)) {  // The common case: no allocation per delivery.
            applyDocumentInput(docs, sink, v, edge.param);
        }
        return;
    }
    for (UiDoc& ui : uiDocs) {
        if (ui.uiId == sink.uiId && reaches(*ui.state)) {
            applyDocumentInput(*ui.state, sink, v, edge.param);
        }
    }
}

std::vector<DocEndpointState*> FlowRuntime::Impl::statesFor(const std::string& uiId) {
    std::vector<DocEndpointState*> out;
    if (uiId.empty()) {
        if (liveDocument(docs) != nullptr) {  // T-260b.
            out.push_back(&docs);
        }
        return out;
    }
    for (UiDoc& ui : uiDocs) {
        if (ui.uiId == uiId && liveDocument(*ui.state) != nullptr) {
            out.push_back(ui.state.get());
        }
    }
    return out;
}

void FlowRuntime::Impl::bindTimelineNodes() {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].type != NodeType::Timeline) {
            continue;
        }
        const std::string name = paramStr(nodes[i], "name", std::string());
        const std::string scope = paramStr(nodes[i], "scope", std::string());
        const std::string uiId = nodes[i].uiId;
        const int index = static_cast<int>(i);
        for (DocEndpointState* state : statesFor(uiId)) {
            ElementRef root = state->doc->root();
            if (!root.valid()) {
                continue;
            }
            // TimelineEnd / TimelineMarker are sent to the group's scope element and bubble, so the
            // root sees every group's events; the name (and a named scope) picks this node's.
            auto handler = [this, state, index, name, scope, uiId](Event& ev) {
                if (std::string_view(state->doc->string(ev.timelineName)) != name) {
                    return;
                }
                if (!scope.empty() && ev.target != state->doc->byId(scope).node) {
                    return;
                }
                const std::string savedUi = currentUi;
                const uint32_t savedInstance = currentInstance;
                currentUi = uiId;
                currentInstance = state->doc->instanceId();
                if (ev.type == EventType::TimelineEnd) {
                    firePulseOutput(index, "end");
                } else {
                    PortValue marker = typedValue(PortType::String);
                    if (ev.markerName != kInvalidString) {
                        marker.s = std::string(state->doc->string(ev.markerName));
                    }
                    fireEventOutput(index, "marker", marker);
                }
                currentUi = savedUi;
                currentInstance = savedInstance;
            };
            state->listenerIds.push_back(state->doc->addListener(root, EventType::TimelineEnd, handler));
            state->listenerIds.push_back(state->doc->addListener(root, EventType::TimelineMarker, handler));
        }
    }
}

}  // namespace xt::flow
