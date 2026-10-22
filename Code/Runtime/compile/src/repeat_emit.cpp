#include "repeat_emit.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

#include "bind_emit.h"
#include "xt/ir_pod.h"

/// Docs/design/27-interaction.md §27.3.4, 03 §3.12.4, 12 §12.7, Docs/tasks/T-177.md.
///
/// Not covered by the design/task sheet (conservative choices made here):
///   - `<repeat>` needs an `id` (the first segment of every instance path id `Item/<i>/...`) and
///     exactly one child element, the template root; text directly inside it is an error.
///   - A `port:` declaration written directly inside a template (every instance would share it) is
///     an error. T-380: a port a symbol instance in the template declares is the symbol's (one copy
///     per repeat instance). T-391: a `<repeat>` inside another repeat's template is allowed, but
///     not `virtualize` on it and not a spawn pool inside a template.
///   - `item-enter` / `item-exit` compile as the template root's `on:item-enter` /
///     `on:item-exit` triggers; writing those two events anywhere else is an error.
///   - `viewport` names an element by id (`#Scroll` or `Scroll`); an unknown id is an error.

namespace xt::compile {

namespace {

std::string trimCopyRepeatEmit(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

bool isItemEvent(std::string_view name) { return name == "on:item-enter" || name == "on:item-exit"; }

/// The symbol names of every `spawn(<symbol>, ...)` call in an action text, in order, once each.
std::vector<std::string> spawnedSymbols(std::string_view text) {
    std::vector<std::string> out;
    for (size_t at = text.find("spawn("); at != std::string_view::npos; at = text.find("spawn(", at + 6)) {
        if (at > 0) {
            const char before = text[at - 1];
            if (std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '-' ||
                before == '.') {
                continue;
            }
        }
        const size_t start = at + 6;
        size_t end = start;
        while (end < text.size() && text[end] != ',' && text[end] != ')') {
            ++end;
        }
        std::string name = trimCopyRepeatEmit(text.substr(start, end - start));
        if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') &&
            name.back() == name.front()) {
            name = name.substr(1, name.size() - 2);
        }
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    return out;
}

}  // namespace

void lowerSpawnPools(DomTree& tree, uint32_t& counter) {
    const size_t original = tree.nodes.size();
    for (uint32_t i = 0; i < original; ++i) {
        std::vector<std::string> symbols;
        int line = tree.nodes[i].line;
        int col = tree.nodes[i].col;
        for (const DomAttr& a : tree.nodes[i].attrs) {
            if (a.name.rfind("on:", 0) != 0) {
                continue;
            }
            for (std::string& name : spawnedSymbols(a.value)) {
                if (std::find(symbols.begin(), symbols.end(), name) == symbols.end()) {
                    symbols.push_back(std::move(name));
                    line = a.line;
                    col = a.col;
                }
            }
        }
        for (const std::string& symbol : symbols) {
            const std::string id = "spawn-" + std::to_string(counter++);
            DomNode pool;
            pool.tag = "repeat";
            pool.parent = i;
            pool.line = line;
            pool.col = col;
            pool.attrs.push_back(DomAttr{"id", id, line, col});
            pool.attrs.push_back(DomAttr{"spawn", symbol, line, col});
            pool.attrs.push_back(DomAttr{
                "style",
                "position: absolute; left: 0px; top: 0px; width: 0px; height: 0px; pointer-events: none",
                line, col});
            const uint32_t poolIndex = static_cast<uint32_t>(tree.nodes.size());
            DomNode use;
            use.tag = "use";
            use.parent = poolIndex;
            use.line = line;
            use.col = col;
            use.attrs.push_back(DomAttr{"symbol", symbol, line, col});
            use.attrs.push_back(DomAttr{"id", id + "-" + symbol, line, col});
            pool.children.push_back(poolIndex + 1);
            tree.nodes.push_back(std::move(pool));
            tree.nodes.push_back(std::move(use));
            tree.nodes[i].children.push_back(poolIndex);
        }
    }
}

bool prepareRepeats(DomTree& dom, std::string_view fileName, DiagnosticSink& sink,
                    std::vector<RepeatDecl>& decls, RepeatScopes& scopes,
                    const std::vector<uint32_t>& instanceRoots) {
    const std::string file(fileName);
    bool ok = true;
    scopes.repeatOf.assign(dom.nodes.size(), RepeatScopes::kNoRepeat);
    std::vector<uint8_t> isTemplateRoot(dom.nodes.size(), 0);
    for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
        if (dom.nodes[i].tag != "repeat") {
            continue;
        }
        DomNode& node = dom.nodes[i];
        auto fail = [&](const std::string& msg) {
            sink.error(file, "<repeat>: " + msg, node.line, node.col);
            ok = false;
        };
        // Nested inside another repeat's template? (T-391: allowed for a list repeat.)
        bool nested = false;
        for (uint32_t p = node.parent; p != 0xFFFFFFFFu && p < dom.nodes.size(); p = dom.nodes[p].parent) {
            nested = nested || dom.nodes[p].tag == "repeat";
        }
        if (nested && node.attr("spawn") != nullptr) {
            fail("spawn() inside a <repeat> template is not supported");
            continue;
        }
        RepeatDecl decl;
        decl.element = i;
        decl.line = node.line;
        decl.col = node.col;
        const DomAttr* port = node.attr("port");
        const DomAttr* id = node.attr("id");
        decl.spawn = node.attr("spawn") != nullptr;  // T-201: a spawn pool (lowerSpawnPools()).
        if (!decl.spawn && (port == nullptr || trimCopyRepeatEmit(port->value).empty())) {
            fail("missing port=\"<list port>\"");
            continue;
        }
        if (id == nullptr || trimCopyRepeatEmit(id->value).empty()) {
            fail("missing id (the instances' path ids start with it)");
            continue;
        }
        decl.port = decl.spawn ? std::string() : trimCopyRepeatEmit(port->value);
        if (const DomAttr* v = node.attr("virtualize")) {
            std::string value = trimCopyRepeatEmit(v->value);
            if (value != "" && value != "true" && value != "false") {
                fail("virtualize must be bare, true or false");
                continue;
            }
            decl.virtualize = value != "false";
            if (nested && decl.virtualize) {
                // T-391 (27 §27.3.4): only the outermost repeat virtualizes.
                fail(
                    "virtualize is only supported on the outermost <repeat>, not inside another repeat's "
                    "template");
                continue;
            }
        }
        if (const DomAttr* vp = node.attr("viewport")) {
            std::string value = trimCopyRepeatEmit(vp->value);
            if (!value.empty() && value[0] == '#') {
                value.erase(0, 1);
            }
            if (value.empty()) {
                fail("empty viewport");
                continue;
            }
            decl.viewport = value;
        }
        if (node.hasText && !trimCopyRepeatEmit(node.text).empty()) {
            fail("text inside <repeat>; the template must be one element");
            continue;
        }
        if (node.children.size() != 1) {
            fail("the template must be exactly one child element, got " +
                 std::to_string(node.children.size()));
            continue;
        }
        decl.templateRoot = node.children[0];
        isTemplateRoot[decl.templateRoot] = 1;
        DomNode& root = dom.nodes[decl.templateRoot];
        for (const char* key : {"item-enter", "item-exit"}) {
            const DomAttr* a = node.attr(key);
            if (a == nullptr) {
                continue;
            }
            const std::string event = std::string("on:") + key;
            if (root.attr(event) != nullptr) {
                sink.error(file, "<repeat " + std::string(key) + ">: the template root already has " + event,
                           a->line, a->col);
                ok = false;
                continue;
            }
            root.attrs.push_back(DomAttr{event, a->value, a->line, a->col});
        }
        decls.push_back(std::move(decl));
    }
    // Template membership: every descendant of a template root. Decls are in DOM order, so an
    // inner repeat (T-391) comes after the one around it and overwrites: the innermost wins.
    for (uint32_t r = 0; r < decls.size(); ++r) {
        for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
            for (uint32_t p = i; p != 0xFFFFFFFFu && p < dom.nodes.size(); p = dom.nodes[p].parent) {
                if (p == decls[r].templateRoot) {
                    scopes.repeatOf[i] = r;
                    break;
                }
                if (p == decls[r].element) {
                    break;
                }
            }
        }
    }
    // T-380 (27 §27.3.4): a port declared inside a symbol instance whose root is the template root
    // or below it belongs to the symbol (each repeat instance gets its own copy at run time).
    auto symbolOwned = [&](uint32_t node) {
        const uint32_t r = scopes.repeatOf[node];
        for (uint32_t p = dom.nodes[node].parent; p != 0xFFFFFFFFu && p < dom.nodes.size();
             p = dom.nodes[p].parent) {
            if (std::find(instanceRoots.begin(), instanceRoots.end(), p) != instanceRoots.end()) {
                return true;
            }
            if (p == decls[r].templateRoot) {
                break;
            }
        }
        return false;
    };
    for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
        for (const DomAttr& a : dom.nodes[i].attrs) {
            if (isItemEvent(a.name) && isTemplateRoot[i] == 0) {
                sink.error(file,
                           a.name + " is only valid on the template of a <repeat> (item-enter / item-exit)",
                           a.line, a.col);
                ok = false;
            }
            if (a.name.rfind("port:", 0) == 0 && scopes.repeatOf[i] != RepeatScopes::kNoRepeat &&
                !symbolOwned(i)) {
                sink.error(file,
                           a.name +
                               ": a port cannot be declared inside a <repeat> template; declare the port on "
                               "the symbol and bind it on <use> with bind:<port>=item(...)",
                           a.line, a.col);
                ok = false;
            }
        }
    }
    scopes.listPort.assign(decls.size(), -1);
    scopes.fields.assign(decls.size(), {});
    // T-391: the enclosing repeat of each repeat (the one whose template holds its element).
    scopes.parent.assign(decls.size(), RepeatScopes::kNoRepeat);
    for (uint32_t r = 0; r < decls.size(); ++r) {
        scopes.parent[r] = scopes.repeatOf[decls[r].element];
    }
    scopes.itemField.assign(decls.size(), -1);
    scopes.entryType.assign(decls.size(), 0);
    scopes.fieldOffset.assign(decls.size(), 0);
    return ok;
}

bool resolveRepeatPorts(const std::vector<RepeatDecl>& decls, const PortScopes& ports,
                        const PortBuilder& portTable, const IrWriter& writer, std::string_view fileName,
                        DiagnosticSink& sink, RepeatScopes& scopes) {
    const std::string file(fileName);
    bool ok = true;
    scopes.listPort.assign(decls.size(), -1);
    scopes.fields.assign(decls.size(), {});
    scopes.parent.resize(decls.size(), RepeatScopes::kNoRepeat);
    scopes.itemField.assign(decls.size(), -1);
    scopes.entryType.assign(decls.size(), 0);
    scopes.fieldOffset.assign(decls.size(), 0);
    auto setFields = [&](uint32_t r, uint8_t entryType, uint32_t offset, uint32_t count) {
        scopes.entryType[r] = entryType;
        scopes.fieldOffset[r] = offset;
        if (entryType != 0) {
            return;  // T-381: a list<number|string|...> has no entry fields (item() reads it whole).
        }
        for (uint32_t f = 0; f < count && offset + f < portTable.fields.size(); ++f) {
            scopes.fields[r].emplace_back(writer.stringAt(portTable.fields[offset + f].name));
        }
    };
    for (uint32_t r = 0; r < decls.size(); ++r) {
        const RepeatDecl& decl = decls[r];
        if (decl.spawn) {
            continue;  // T-201: a spawn pool has no list port.
        }
        if (decl.port.rfind("item(", 0) == 0 || decl.port.rfind("repeat.item(", 0) == 0) {
            // T-391 (27 §27.3.4): a list field of the enclosing repeat's entry.
            const size_t open = decl.port.find('(');
            std::string field =
                trimCopyRepeatEmit(std::string_view(decl.port).substr(open + 1, decl.port.size() - open - 1));
            if (!field.empty() && field.back() == ')') {
                field.pop_back();
            }
            field = trimCopyRepeatEmit(field);
            if (field.size() >= 2 && (field.front() == '"' || field.front() == '\'') &&
                field.back() == field.front()) {
                field = field.substr(1, field.size() - 2);
            }
            const uint32_t parent = r < scopes.parent.size() ? scopes.parent[r] : RepeatScopes::kNoRepeat;
            auto failItem = [&](const std::string& msg) {
                sink.error(file, "<repeat port=\"" + decl.port + "\">: " + msg, decl.line, decl.col);
                ok = false;
            };
            if (parent == RepeatScopes::kNoRepeat) {
                failItem("item(<field>) as a list is only valid inside another <repeat> template");
                continue;
            }
            if (!scopes.resolved(parent)) {
                continue;  // The enclosing repeat already failed.
            }
            const std::vector<std::string>& names = scopes.fields[parent];
            const auto it = std::find(names.begin(), names.end(), field);
            if (scopes.entryType[parent] != 0 || it == names.end()) {
                failItem("the enclosing repeat's entries have no field \"" + field + "\"");
                continue;
            }
            const uint32_t index = static_cast<uint32_t>(it - names.begin());
            const uint32_t at = scopes.fieldOffset[parent] + index;
            const IrPortNested* nested = nullptr;
            for (const IrPortNested& n : portTable.nested) {
                if (n.field == at) {
                    nested = &n;
                }
            }
            if (at >= portTable.fields.size() ||
                static_cast<PortValueType>(portTable.fields[at].type) != PortValueType::List ||
                nested == nullptr) {
                failItem("field \"" + field + "\" is not a list<...>");
                continue;
            }
            scopes.itemField[r] = static_cast<int>(index);
            setFields(r, nested->entryType, nested->fieldOffset, nested->fieldCount);
            continue;
        }
        int port = ports.resolve(decl.port, decl.element);
        if (port < 0 || static_cast<PortValueType>(ports.ports[static_cast<uint32_t>(port)].type) !=
                            PortValueType::List) {
            sink.error(file,
                       "<repeat port=\"" + decl.port + "\">: " +
                           (port < 0 ? std::string("no such port") : std::string("not a list<...> port")),
                       decl.line, decl.col);
            ok = false;
            continue;
        }
        scopes.listPort[r] = port;
        const IrPort& p = ports.ports[static_cast<uint32_t>(port)];
        setFields(r, p.entryType, p.fieldOffset, p.fieldCount);
    }
    return ok;
}

bool emitRepeats(const DomTree& dom, const std::vector<RepeatDecl>& decls, const RepeatScopes& scopes,
                 const TriggerBuilder& triggers, std::string_view fileName, DiagnosticSink& sink,
                 std::vector<IrRepeat>& out) {
    const std::string file(fileName);
    bool ok = true;
    for (uint32_t r = 0; r < decls.size(); ++r) {
        const RepeatDecl& decl = decls[r];
        if (!decl.spawn && !scopes.resolved(r)) {
            continue;  // resolveRepeatPorts() already reported it.
        }
        // T-391: a list field of the enclosing repeat's entry (listPort = the field index).
        const bool itemField = !decl.spawn && scopes.listPort[r] < 0;
        IrRepeat row{};
        row.element = decl.element;
        row.listPort = decl.spawn  ? kRepeatNoListPort
                       : itemField ? static_cast<uint32_t>(scopes.itemField[r])
                                   : static_cast<uint32_t>(scopes.listPort[r]);
        row.templateRoot = decl.templateRoot;
        row.flags =
            static_cast<uint8_t>((decl.virtualize ? kRepeatVirtualize : 0) | (decl.spawn ? kRepeatSpawn : 0) |
                                 (itemField ? kRepeatItemField : 0));
        row.viewportElement = kRepeatNoViewport;
        for (const IrTriggerTable& table : triggers.tables) {
            if (table.element != decl.templateRoot) {
                continue;
            }
            for (uint32_t k = 0; k < table.triggerCount; ++k) {
                const uint32_t index = table.triggerOffset + k;
                const TriggerEvent ev = static_cast<TriggerEvent>(triggers.triggers[index].event);
                if (ev == TriggerEvent::ItemEnter) {
                    row.enterTriggerActions = index + 1;
                } else if (ev == TriggerEvent::ItemExit) {
                    row.exitTriggerActions = index + 1;
                }
            }
        }
        if (!decl.viewport.empty()) {
            bool found = false;
            for (uint32_t i = 0; i < dom.nodes.size() && !found; ++i) {
                const DomAttr* id = dom.nodes[i].attr("id");
                if (id != nullptr && id->value == decl.viewport) {
                    row.viewportElement = i;
                    found = true;
                }
            }
            if (!found) {
                sink.error(file, "<repeat viewport=\"" + decl.viewport + "\">: no element with that id",
                           decl.line, decl.col);
                ok = false;
                continue;
            }
        }
        out.push_back(row);
    }
    return ok;
}

}  // namespace xt::compile
