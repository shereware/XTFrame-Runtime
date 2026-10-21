#pragma once

#include <string>
#include <vector>

#include "xt/flow/flow_graph.h"

/// connectionPoints(): reflects docs/design/17-editor.md §17.6's connection-point table off a
/// live Document -- every element with an id (its row-1 ports, plus row-2 `<anim>` ports when its
/// tag is "anim") and the document itself (row-3 output ports). Used by the editor's Connection
/// Points and Flow panels (docs/tasks/T-65.md). Reuses flow_bindings.cpp's own
/// elementPortInfo()/animPortInfo()/docPortInfo() (flow_internal.h) for which port names are valid
/// and what PortType they carry, rather than re-deriving those rules.

namespace xt {
class Document;
}

namespace xt::flow {

/// One connection point. `owner` is "#<id>" for an element or `<anim>` element, or literally
/// "document" for the document itself. `isInput` distinguishes an input (call) port from an
/// output (event/value) port.
struct PortDesc {
    std::string owner;
    std::string port;
    PortType type = PortType::Pulse;
    bool isInput = false;
    // --- T-175 (docs/design/27-interaction.md §27.3.1/27.4): declared ports (`port:`). ---
    /// True for a port from the Document's PORT table (`owner` is "#<declaring element id>", or
    /// "document" for an element without an id; `isInput` = direction in/call).
    bool declared = false;
    /// The declared type as written (`number`, `enum`, `struct`, ...); empty for §17.6 ports.
    std::string typeName{};
    /// False for struct/list ports: listed, not connectable yet (`type` is then a placeholder).
    bool connectable = true;
};

/// Lists every §17.6 connection point currently reflected off `doc`: for each element with a
/// non-empty id (any tag, in document preorder), its row-1 ports, then (only if that element's
/// tag is "anim") its row-2 ports; finally, once, the document's row-3 output ports. T-175: then
/// every declared port (the IR's PORT table, in declaration order). Traverses
/// `doc`'s tree via root()/firstChild()/nextSibling() (all const) rather than Document::query(),
/// so this stays callable on a `const Document&`.
std::vector<PortDesc> connectionPoints(const Document& doc);

}  // namespace xt::flow
