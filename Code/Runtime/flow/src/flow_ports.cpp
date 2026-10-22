#include "xt/flow/flow_ports.h"

#include "flow_internal.h"
#include "xt/document.h"

/// connectionPoints(): see xt/flow/flow_ports.h's file comment. Implementation note: every port
/// name/type decision below is delegated to elementPortInfo()/animPortInfo()/docPortInfo()
/// (flow_internal.h, flow_bindings.cpp) -- the candidate name lists here only say which names to
/// probe, in Docs/design/17-editor.md §17.6's own table order; they do not themselves decide
/// validity or PortType.

namespace xt::flow {

namespace {

constexpr const char* kElementOutputs[] = {
    "Click", "PointerEnter", "PointerLeave", "Focus", "Blur", "AnimationEnd", "Change", "hover",
};
constexpr const char* kElementInputs[] = {
    "setText",     "setStyle", "setValue",    "addClass",      "removeClass",
    "toggleClass", "focus",    "setProperty", "playAnimation", "stopAnimation",
};
constexpr const char* kAnimOutputs[] = {"Marker", "localTime", "Finished"};
constexpr const char* kAnimInputs[] = {"play", "pause", "resume", "seek", "setSpeed"};
constexpr const char* kDocOutputs[] = {
    "clock",    "viewportWidth", "viewportHeight", "pointerX",
    "pointerY", "pointerNX",     "pointerNY",      "pointerDown",
};

void appendElementPorts(const std::string& owner, std::vector<PortDesc>& out) {
    for (const char* name : kElementOutputs) {
        PortType t{};
        if (elementPortInfo(name, /*isSource=*/true, t)) {
            out.push_back(PortDesc{owner, name, t, false});
        }
    }
    for (const char* name : kElementInputs) {
        PortType t{};
        if (elementPortInfo(name, /*isSource=*/false, t)) {
            out.push_back(PortDesc{owner, name, t, true});
        }
    }
}

void appendAnimPorts(const std::string& owner, std::vector<PortDesc>& out) {
    for (const char* name : kAnimOutputs) {
        PortType t{};
        if (animPortInfo(name, /*isSource=*/true, t)) {
            out.push_back(PortDesc{owner, name, t, false});
        }
    }
    for (const char* name : kAnimInputs) {
        PortType t{};
        if (animPortInfo(name, /*isSource=*/false, t)) {
            out.push_back(PortDesc{owner, name, t, true});
        }
    }
}

const char* declaredTypeName(PortValueType t) {
    switch (t) {
        case PortValueType::Number:
            return "number";
        case PortValueType::String:
            return "string";
        case PortValueType::Bool:
            return "bool";
        case PortValueType::Color:
            return "color";
        case PortValueType::Vec2:
            return "vec2";
        case PortValueType::Vec3:
            return "vec3";
        case PortValueType::Entity:
            return "entity";
        case PortValueType::Enum:
            return "enum";
        case PortValueType::Struct:
            return "struct";
        case PortValueType::List:
            return "list";
        case PortValueType::Texture:
            return "texture";
        case PortValueType::Event:
            return "event";
    }
    return "";
}

/// Preorder traversal (root()/firstChild()/nextSibling() only -- all const) collecting row-1/2
/// connection points for `element` and every descendant.
void visit(ElementRef element, std::vector<PortDesc>& out) {
    if (!element.valid()) {
        return;
    }
    std::string id(element.id());
    if (!id.empty()) {
        std::string owner = "#" + id;
        appendElementPorts(owner, out);
        if (element.tag() == "anim") {
            appendAnimPorts(owner, out);
        }
    }
    for (ElementRef child = element.firstChild(); child.valid(); child = child.nextSibling()) {
        visit(child, out);
    }
}

}  // namespace

std::vector<PortDesc> connectionPoints(const Document& doc) {
    std::vector<PortDesc> result;
    visit(doc.root(), result);
    for (const char* name : kDocOutputs) {
        PortType t{};
        if (docPortInfo(name, t)) {
            result.push_back(PortDesc{"document", name, t, false});
        }
    }
    // T-175: declared ports (PORT), connectable by the same "#<id>.<port>" spelling.
    for (const PortRecord& info : doc.ports()) {
        PortDesc d;
        d.owner = info.elementId.empty() ? std::string("document") : "#" + info.elementId;
        d.port = info.name;
        d.connectable = declaredPortFlowType(info.type, d.type);
        d.isInput = info.dir != PortDir::Out;
        d.declared = true;
        d.typeName = declaredTypeName(info.type);
        result.push_back(std::move(d));
    }
    return result;
}

}  // namespace xt::flow
