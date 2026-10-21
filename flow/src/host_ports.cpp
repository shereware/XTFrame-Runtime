#include "xt/flow/host_ports.h"

#include <algorithm>
#include <set>
#include <utility>

#include "flow_internal.h"
#include "xt/document.h"

/// HostPorts: see xt/flow/host_ports.h. Every document-facing step -- endpoint resolution, input
/// application, output subscription and polling, the change-only filter, rebind's lost-port
/// warnings -- is the document endpoint layer in flow_internal.h / flow_bindings.cpp that
/// FlowRuntime uses too; this file only parses port names, type-checks host values like
/// validateGraph() type-checks an edge, and keeps the host's listener list. See docs/tasks/T-144.md.

namespace xt::flow {

namespace {

const char* portTypeName(PortType t) {
    switch (t) {
        case PortType::Pulse:
            return "pulse";
        case PortType::Bool:
            return "bool";
        case PortType::Number:
            return "number";
        case PortType::String:
            return "string";
    }
    return "?";
}

/// Splits "#<id>.<port>", "#document.<port>" or "document.<port>" into id ("document" for the
/// latter two) and port. False when malformed.
bool splitPortName(std::string_view name, std::string& id, std::string& port) {
    if (!name.empty() && name.front() == '#') {
        name.remove_prefix(1);
    } else if (name.substr(0, 9) != "document.") {
        return false;  // Element ports always carry the '#'.
    }
    size_t dot = name.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) {
        return false;
    }
    id = std::string(name.substr(0, dot));
    port = std::string(name.substr(dot + 1));
    return true;
}

}  // namespace

struct HostPorts::Impl {
    struct Entry {
        uint32_t id = 0;
        Endpoint endpoint;
        std::string key;  // externalKey() of `endpoint`.
        Listener listener;
    };

    DocEndpointState docs;
    bool bound = false;
    std::vector<PortDesc> portList;
    std::vector<Entry> entries;
    uint32_t nextId = 1;
    std::string lastError;
    // T-175: emit listeners (one Document port listener fans out to them), registered
    // converters and the instance id, all re-applied to the Document of every bind()/rebind().
    std::vector<std::pair<uint32_t, EmitListener>> emitListeners;
    uint32_t nextEmitId = 1;
    uint32_t portListener = 0;
    std::vector<std::pair<std::string, ConverterFn>> converters;
    uint32_t instanceId = 0;
    bool instanceIdSet = false;
    // T-394 (08.15): in-port listeners (id, port, callback), fed by the same port listener.
    struct InputListener {
        uint32_t id = 0;
        PortId port = kInvalidPort;
        std::function<void(const PortValue&)> fn;
    };
    std::vector<InputListener> inputListeners;
    uint32_t nextInputId = 1;

    /// Installs the Document-side state of the T-175 API on `docs.doc`.
    void attachPorts() {
        Document& doc = *docs.doc;
        portListener = doc.addPortListener([this](const PortEvent& ev) {
            if (ev.kind == PortEventKind::Changed && ev.fromDocument) {
                deliverInput(ev);  // T-394 (08.15).
                return;
            }
            if (ev.kind != PortEventKind::Emit) {
                return;
            }
            std::vector<std::pair<uint32_t, EmitListener>> copy = emitListeners;  // Listeners may edit.
            for (const auto& [id, fn] : copy) {
                fn(ev.port, ev.value, ev.instanceId);
            }
        });
        for (const auto& [name, fn] : converters) {
            doc.registerConverter(name, fn);
        }
        if (instanceIdSet) {
            doc.setInstanceId(instanceId);
        }
    }

    /// T-394 (08.15): a document write of an in-port, to its input listeners.
    void deliverInput(const PortEvent& ev) {
        if (inputListeners.empty() || docs.doc == nullptr) {
            return;
        }
        std::vector<InputListener> copy;
        for (const InputListener& l : inputListeners) {
            if (l.port == ev.port) {
                copy.push_back(l);  // Listeners may add or remove listeners.
            }
        }
        if (copy.empty()) {
            return;
        }
        const PortRecord info = docs.doc->portInfo(ev.port);
        if (info.dir != PortDir::In) {
            return;
        }
        PortValue v;
        const float number = ev.value.type == ValueType::Float ? ev.value.f : 0.0f;
        if (info.type == PortValueType::Bool) {
            v.type = PortType::Bool;
            v.b = number != 0.0f;
        } else if (info.type == PortValueType::Number || info.type == PortValueType::Entity) {
            v.type = PortType::Number;
            v.n = number;
        } else {
            v.type = PortType::String;
            v.s = docs.doc->portText(ev.port);
        }
        for (const InputListener& l : copy) {
            l.fn(v);
        }
    }

    Impl() {
        docs.output = [this](Endpoint::Owner owner, const std::string& elementId, const std::string& port,
                             const PortValue& v) { deliver(externalKey(owner, elementId, port), v); };
    }

    /// Calls every listener on `key`. Collected first: a listener may add or remove listeners.
    void deliver(const std::string& key, const PortValue& v) {
        std::vector<uint32_t> ids;
        for (const Entry& e : entries) {
            if (e.key == key) {
                ids.push_back(e.id);
            }
        }
        for (uint32_t id : ids) {
            for (const Entry& e : entries) {
                if (e.id == id) {
                    Listener fn = e.listener;  // A copy: the call may erase `e`.
                    fn(v);
                    break;
                }
            }
        }
    }

    Status fail(std::string message) {
        lastError = std::move(message);
        return Status::kInvalidArgument;
    }

    /// Resolves `name` against the bound Document in the requested direction.
    bool resolve(std::string_view name, bool isSource, Endpoint& out) {
        std::string id, port, err;
        if (!splitPortName(name, id, port)) {
            lastError = "malformed connection point '" + std::string(name) + "'";
            return false;
        }
        if (!resolveDocumentEndpoint(id, port, isSource, docs.doc, out, err)) {
            lastError = err;
            return false;
        }
        return true;
    }

    /// Registers and subscribes every listener's port on the (freshly bound) Document.
    void subscribeAll() {
        for (const Entry& e : entries) {
            registerDocumentEndpoint(docs, e.endpoint);
            subscribeDocumentOutput(docs, e.endpoint);
        }
    }
};

HostPorts::HostPorts() : m_impl(std::make_unique<Impl>()) {}

HostPorts::~HostPorts() {
    // Every listener captures the Impl, so they have to come off before it goes away.
    if (m_impl->bound && m_impl->portListener != 0) {
        m_impl->docs.doc->removePortListener(m_impl->portListener);
    }
    unbindDocumentEndpoints(m_impl->docs, /*removeListeners=*/true);
}

Status HostPorts::bind(Document& doc) {
    Impl& m = *m_impl;
    if (m.bound && m.portListener != 0) {
        m.docs.doc->removePortListener(m.portListener);  // The previous Document is still alive.
    }
    unbindDocumentEndpoints(m.docs, /*removeListeners=*/true);
    resetDocumentTreeState(m.docs);
    m.docs.doc = &doc;
    m.docs.clockAccum = 0;
    m.entries.clear();
    m.portList = connectionPoints(doc);
    m.bound = true;
    m.attachPorts();
    return Status::kOk;
}

Status HostPorts::rebind(Document& doc, std::vector<std::string>& warnings) {
    Impl& m = *m_impl;
    if (!m.bound) {
        return m.fail("rebind() before bind()");
    }
    // As FlowRuntime::rebind(): the previous Document is normally already destroyed, so its
    // listeners are forgotten rather than removed.
    unbindDocumentEndpoints(m.docs, /*removeListeners=*/false);
    resetDocumentTreeState(m.docs);
    m.docs.doc = &doc;
    m.portList = connectionPoints(doc);
    m.attachPorts();  // The previous Document's port listener died with it.

    std::set<std::string> warned;
    std::vector<Impl::Entry> kept;
    kept.reserve(m.entries.size());
    for (Impl::Entry& e : m.entries) {
        if (!documentEndpointLost(doc, e.endpoint, warnings, warned)) {
            kept.push_back(std::move(e));
        }
    }
    m.entries = std::move(kept);
    m.subscribeAll();
    return Status::kOk;
}

Status HostPorts::setInput(std::string_view port, const PortValue& value, std::string_view param) {
    Impl& m = *m_impl;
    if (!m.bound) {
        return m.fail("setInput() before bind()");
    }
    Endpoint sink;
    if (!m.resolve(port, /*isSource=*/false, sink)) {
        return Status::kInvalidArgument;
    }
    const std::string name(port);
    // validateGraph()'s rule for an edge into a document input: a pulse input accepts anything,
    // a value input needs exactly its own type.
    if (sink.type != PortType::Pulse && value.type != sink.type) {
        return m.fail("type mismatch on '" + name + "': expected " + portTypeName(sink.type) + ", got " +
                      portTypeName(value.type));
    }
    const std::string paramText(param);
    std::string err;
    if (!checkDocumentInputParam(sink.port, param.empty() ? nullptr : &paramText, "'" + name + "'", err)) {
        return m.fail(err);
    }
    registerDocumentEndpoint(m.docs, sink);
    applyDocumentInput(m.docs, sink, value, paramText);
    return Status::kOk;
}

Result<uint32_t> HostPorts::addOutputListener(std::string_view port, Listener listener) {
    Impl& m = *m_impl;
    if (!m.bound) {
        m.fail("addOutputListener() before bind()");
        return Result<uint32_t>::error(Status::kInvalidArgument);
    }
    Endpoint source;
    if (!m.resolve(port, /*isSource=*/true, source)) {
        return Result<uint32_t>::error(Status::kInvalidArgument);
    }
    Impl::Entry entry;
    entry.id = m.nextId++;
    entry.key = externalKey(source.owner, source.elementId, source.port);
    entry.endpoint = source;
    entry.listener = std::move(listener);
    m.entries.push_back(std::move(entry));
    registerDocumentEndpoint(m.docs, source);
    subscribeDocumentOutput(m.docs, source);
    return Result<uint32_t>::ok(m.entries.back().id);
}

void HostPorts::removeOutputListener(uint32_t id) {
    std::vector<Impl::Entry>& entries = m_impl->entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->id == id) {
            entries.erase(it);
            return;
        }
    }
}

void HostPorts::tick(float dt) {
    Impl& m = *m_impl;
    if (!m.bound) {
        return;
    }
    m.docs.clockAccum += dt;
    pollDocumentOutputs(m.docs);
}

std::span<const PortDesc> HostPorts::ports() const {
    return m_impl->bound ? std::span<const PortDesc>(m_impl->portList) : std::span<const PortDesc>{};
}

const std::string& HostPorts::lastError() const { return m_impl->lastError; }

// ---- T-175 declared ports ----------------------------------------------------------------------

PortId HostPorts::lookup(std::string_view name) const {
    return m_impl->bound ? m_impl->docs.doc->lookupPort(name) : kInvalidPort;
}

Status HostPorts::set(PortId port, const Value& value) {
    return m_impl->bound ? m_impl->docs.doc->setPort(port, value) : m_impl->fail("set() before bind()");
}

Status HostPorts::setText(PortId port, std::string_view text) {
    return m_impl->bound ? m_impl->docs.doc->setPortText(port, text)
                         : m_impl->fail("setText() before bind()");
}

Value HostPorts::get(PortId port) { return m_impl->bound ? m_impl->docs.doc->getPort(port) : Value{}; }

Result<Value> HostPorts::call(PortId port, std::span<const Value> args) {
    if (!m_impl->bound) {
        m_impl->fail("call() before bind()");
        return Result<Value>::error(Status::kInvalidArgument);
    }
    return m_impl->docs.doc->callPort(port, args);
}

Status HostPorts::setCallHandler(PortId port, PortCallHandler handler) {
    return m_impl->bound ? m_impl->docs.doc->setPortCallHandler(port, std::move(handler))
                         : m_impl->fail("setCallHandler() before bind()");
}

uint32_t HostPorts::addEmitListener(EmitListener listener) {
    if (!m_impl->bound) {
        m_impl->fail("addEmitListener() before bind()");
        return 0;
    }
    uint32_t id = m_impl->nextEmitId++;
    m_impl->emitListeners.emplace_back(id, std::move(listener));
    return id;
}

uint32_t HostPorts::addInputListener(PortId port, std::function<void(const PortValue&)> listener) {
    Impl& m = *m_impl;
    if (!m.bound) {
        m.fail("addInputListener() before bind()");
        return 0;
    }
    const PortRecord info = m.docs.doc->portInfo(port);
    if (info.id == kInvalidPort || info.dir != PortDir::In) {
        m.fail("addInputListener(): not a declared in-port");
        return 0;
    }
    const uint32_t id = m.nextInputId++;
    m.inputListeners.push_back(Impl::InputListener{id, port, std::move(listener)});
    return id;
}

void HostPorts::removeInputListener(uint32_t id) {
    auto& list = m_impl->inputListeners;
    list.erase(
        std::remove_if(list.begin(), list.end(), [id](const Impl::InputListener& l) { return l.id == id; }),
        list.end());
}

void HostPorts::removeEmitListener(uint32_t id) {
    auto& list = m_impl->emitListeners;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [id](const std::pair<uint32_t, EmitListener>& l) { return l.first == id; }),
               list.end());
}

Status HostPorts::listSet(PortId port, std::span<const Value> entries) {
    return m_impl->bound ? m_impl->docs.doc->listSet(port, entries) : m_impl->fail("listSet() before bind()");
}

Status HostPorts::listInsert(PortId port, uint32_t index, const Value& entry) {
    return m_impl->bound ? m_impl->docs.doc->listInsert(port, index, entry)
                         : m_impl->fail("listInsert() before bind()");
}

Status HostPorts::listRemove(PortId port, uint32_t index) {
    return m_impl->bound ? m_impl->docs.doc->listRemove(port, index)
                         : m_impl->fail("listRemove() before bind()");
}

Status HostPorts::listUpdate(PortId port, uint32_t index, const Value& entry) {
    return m_impl->bound ? m_impl->docs.doc->listUpdate(port, index, entry)
                         : m_impl->fail("listUpdate() before bind()");
}

Result<uint32_t> HostPorts::registerConverter(std::string_view name, ConverterFn fn) {
    if (!m_impl->bound) {
        m_impl->fail("registerConverter() before bind()");
        return Result<uint32_t>::error(Status::kInvalidArgument);
    }
    Result<uint32_t> op = m_impl->docs.doc->registerConverter(name, fn);
    if (op.ok()) {
        auto& list = m_impl->converters;
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const std::pair<std::string, ConverterFn>& c) { return c.first == name; });
        if (it != list.end()) {
            it->second = std::move(fn);
        } else {
            list.emplace_back(std::string(name), std::move(fn));
        }
    }
    return op;
}

std::vector<PortRecord> HostPorts::portTable() const {
    return m_impl->bound ? m_impl->docs.doc->ports() : std::vector<PortRecord>{};
}

Status HostPorts::setInstanceId(uint32_t id) {
    if (!m_impl->bound) {
        return m_impl->fail("setInstanceId() before bind()");
    }
    m_impl->instanceId = id;
    m_impl->instanceIdSet = true;
    m_impl->docs.doc->setInstanceId(id);
    return Status::kOk;
}

}  // namespace xt::flow
