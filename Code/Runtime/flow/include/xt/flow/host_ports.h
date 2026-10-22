#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_ports.h"
#include "xt/types.h"
#include "xt/value.h"

/// HostPorts: host-side access to a Document's Docs/design/17-editor.md §17.6 connection points
/// without a FlowGraph -- the runtime side of a bundle manifest's `ports` contract
/// (Docs/design/22-bundle.md §22.3/§22.5). Input ports are driven and output ports observed by the
/// same code FlowRuntime uses for its document endpoints, so both behave identically. See
/// Docs/tasks/T-144.md.

namespace xt {
class Document;
}

namespace xt::flow {

/// Host-side access to a Document's §17.6 connection points without a FlowGraph.
class HostPorts {
public:
    HostPorts();
    /// Removes every Document listener this object registered (the bound Document must still be
    /// alive, see bind()).
    ~HostPorts();
    HostPorts(const HostPorts&) = delete;
    HostPorts& operator=(const HostPorts&) = delete;

    /// Binds to `doc` (must outlive this object or be replaced via rebind()). Lists ports via
    /// connectionPoints(doc). Clears listeners (and takes the previous Document's listeners off it,
    /// so that one must still be alive too), and restarts the `document.clock` port at 0.
    Status bind(Document& doc);
    /// After Context::reloadDocument(): re-resolves listeners by port name against `doc`; ports that
    /// vanished are reported in `warnings` (appended, one per distinct port, "#id.port: element not
    /// found" / "#id.port: not an <anim> element", as FlowRuntime::rebind()) and their listeners
    /// dropped. The previous Document is never touched (it is normally already destroyed). The
    /// clock keeps running. kInvalidArgument when bind() has never been called, kOk otherwise.
    Status rebind(Document& doc, std::vector<std::string>& warnings);

    /// Input ports (host -> UI). `port` is "#<id>.<port>" exactly as §17.6 / manifest `ports.in`.
    /// `param` is the edge parameter some ports take (`class` for add/remove/toggleClass, `prop`
    /// for setProperty); empty otherwise. Pulse ports ignore `value`. kInvalidArgument for an
    /// unknown port, a type mismatch (a value port's `value.type` must equal the port's PortType,
    /// exactly as a `.xtflow` edge -- no conversion), a missing/unknown `param`, or when not bound.
    Status setInput(std::string_view port, const PortValue& value, std::string_view param = {});

    /// Output ports (UI -> host). `port` is "#<id>.<port>" or, for the document's own ports,
    /// "document.<port>" (manifest `ports.out` spelling) or "#document.<port>" (`.xtflow`
    /// spelling). Pulse ports fire once per event. Value ports only fire when the value changes:
    /// the event-driven ones (`hover`, `Change`, `Marker`, the `document.pointer*` ports) as the
    /// Document dispatches the event (inside Context::update()), the polled ones (`clock`,
    /// `viewportWidth/Height`, `localTime`, and again the pointer ports) in tick(). Returns the
    /// listener id (never 0); kInvalidArgument for an unknown port or when not bound.
    using Listener = std::function<void(const PortValue&)>;
    Result<uint32_t> addOutputListener(std::string_view port, Listener listener);
    /// Unknown ids are ignored.
    void removeOutputListener(uint32_t id);

    /// Advances the `document.clock` port by `dt` seconds, then polls value-type outputs and
    /// delivers changes; event-type outputs are delivered as the Document dispatches them (inside
    /// Context::update()), regardless of tick(). No-op when not bound.
    void tick(float dt);

    /// The currently bound ports (same rows as connectionPoints()). Empty when not bound.
    std::span<const PortDesc> ports() const;

    /// Why the last call that returned kInvalidArgument failed (e.g. "type mismatch on
    /// '#title.setText': expected string, got number"). Not cleared by later successful calls.
    const std::string& lastError() const;

    // ---- T-175 (Docs/design/13-api.md §13.8.1, 27-interaction.md §27.3): declared ports -------
    // The `port:` connection points of the bound Document, by PortId. Every call forwards to the
    // Document's port API (xt/document.h: value encodings, statuses); kInvalidArgument (lookup:
    // kInvalidPort) when not bound.

    /// A declared port by name: `hp`, `[#]<element id>.<port>` (path ids included) or `<instance
    /// id>.<port>` (exposed). `ui#id.port` (another UI) is not resolved: this is one Document.
    PortId lookup(std::string_view name) const;
    /// Host -> UI value (any direction may be set).
    Status set(PortId port, const Value& value);
    /// The same from text, parsed by the port's type (Document::setPortText; a Struct port takes a
    /// JSON object, a `list<struct>` port a JSON array of objects, a `list<T>` port a JSON array of values).
    Status setText(PortId port, std::string_view text);
    /// The current value (Float 0 when unknown / not bound).
    Value get(PortId port);
    /// Calls the UI's `call` port: runs its `port(<name>)` triggers, returns their `emit` value.
    Result<Value> call(PortId port, std::span<const Value> args);
    /// The host function a UI-side `emit(<call port>)` invokes; its result becomes the value.
    Status setCallHandler(PortId port, PortCallHandler handler);
    /// UI -> host: every `emit(<port>)` of a declared port (the port, its value, the instance id).
    using EmitListener = std::function<void(PortId port, const Value& value, uint32_t instanceId)>;
    /// Returns the listener id (never 0); 0 when not bound. Kept across rebind().
    uint32_t addEmitListener(EmitListener listener);
    /// Unknown ids are ignored.
    void removeEmitListener(uint32_t id);
    /// T-394 (08.15, 13.8.1): in-port change notification -- `listener` is called with the new
    /// value whenever the document itself writes the in-port `port` (a trigger's `set()` or a
    /// binding, PortEvent::fromDocument); the host's own set() / setText() / list calls do not
    /// call it. The value: a Bool port as Bool, a Number / Entity port as Number, any other port
    /// as String (Document::portText). Returns the listener id (never 0); 0 when not bound or
    /// `port` is not a declared in-port. Kept across rebind() (by PortId).
    uint32_t addInputListener(PortId port, std::function<void(const PortValue&)> listener);
    /// Unknown ids are ignored.
    void removeInputListener(uint32_t id);
    /// List ports: values and ListChanged events only (`<repeat>` instances are T-177).
    Status listSet(PortId port, std::span<const Value> entries);
    Status listInsert(PortId port, uint32_t index, const Value& entry);
    Status listRemove(PortId port, uint32_t index);
    Status listUpdate(PortId port, uint32_t index, const Value& entry);
    /// A `host.<name>(...)` converter for the bound Document; returns its op number (0x80 + n).
    /// Registered converters are registered again on rebind().
    Result<uint32_t> registerConverter(std::string_view name, ConverterFn fn);
    /// The declared-port table (23.10 node generation, `xtframe-pack --gen-ports`). Empty when not
    /// bound.
    std::vector<PortRecord> portTable() const;
    /// 27.3.1 multi-instance: the id the Document's host / emit events carry. Kept across
    /// rebind().
    Status setInstanceId(uint32_t id);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::flow
