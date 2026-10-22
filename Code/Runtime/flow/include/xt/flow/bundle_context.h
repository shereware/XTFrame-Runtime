#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xt/context.h"
#include "xt/flow/flow_runtime.h"
#include "xt/types.h"

/// BundleContext (T-193; Docs/design/27-interaction.md §27.4, 13-api.md §13.8.3, 23-host-
/// integration.md §23.10 items 4 and 10): the host-side flow interface of a whole bundle. It maps
/// the bundle's UI ids to the Documents a Context has loaded for them (several per id for a
/// multi-instance UI, 27.3.1), owns the global flow runtime (`bundle.xtflow`) and the per-UI ones,
/// and switches all of them on and off together (`enableFlow`). xt_core cannot depend on xt_flow,
/// so 13.8.3's `enableFlow` / `flowRuntime` live here rather than on xt::Context.

namespace xt::flow {

/// See the file-level comment above.
class BundleContext : private ContextListener {
public:
    /// Registers itself as a ContextListener of `context` (for the UI lifecycle events); `context`
    /// must outlive this object.
    explicit BundleContext(Context& context);
    ~BundleContext() override;
    BundleContext(const BundleContext&) = delete;
    BundleContext& operator=(const BundleContext&) = delete;

    Context& context() { return m_context; }

    /// Registers `handle` (loaded in context()) as an instance of UI `uiId`; call once per
    /// instance. Register every UI before loading a flow that uses it. kInvalidArgument for an
    /// empty id, a handle the Context does not know, or one already registered.
    Status addUi(std::string_view uiId, DocumentHandle handle);
    /// T-356: registers `handle`, loaded in another Context `owner`, as an instance of UI `uiId`
    /// (a host that keeps some UIs out of the Context it renders, e.g. the editor's Demo space:
    /// the previewed UI in its preview Context, the project's other UIs in one it never renders).
    /// `owner` must outlive this object. Its `ui#<id>.shown/hidden` events reach the global flow;
    /// postSystemEvent() forwards to it, but a system event posted on it directly reaches no flow.
    /// handles() / uiOf() / uiFlowRuntime() see only context()'s instances; instances() sees
    /// every one. kInvalidArgument as the overload above; `owner` == context() is that overload.
    Status addUi(std::string_view uiId, Context& owner, DocumentHandle handle);
    /// T-260b: deregisters every instance of UI `uiId`: the global flow's endpoints on it are
    /// unbound (their listeners come off its Document only while the Context still holds that
    /// Document at the handle -- after an unloadDocument() or a reload they are just dropped) and
    /// it receives and sends nothing from then on; its own runtime (loadUiFlow) is unbound the
    /// same way, disabled, and destroyed at the next tick() (it may be the one calling back).
    /// Safe before or after Context::unloadDocument() of its handles, and from inside a callback
    /// (a ContextListener, a Document listener, a FlowHostEvent listener). The global flow keeps
    /// its edges to `ui#<uiId>` (they reach nothing); load it again to bind a replacement.
    /// kInvalidArgument when `uiId` is not registered.
    Status removeUi(std::string_view uiId);
    /// The distinct UI ids, in first-registration order.
    std::vector<std::string> uiIds() const;
    /// Every handle registered for `uiId` in context(), in registration order.
    std::vector<DocumentHandle> handles(std::string_view uiId) const;
    /// T-356: one registered instance: the Context that holds it and its handle there.
    struct Instance {
        Context* context = nullptr;
        DocumentHandle handle = 0;
    };
    /// T-356: every instance registered for `uiId` (any Context), in registration order.
    std::vector<Instance> instances(std::string_view uiId) const;
    /// The UI id `handle` was registered under; empty when it was not.
    std::string_view uiOf(DocumentHandle handle) const;

    /// 13.8.3 `enableFlow(bool)`: enables or disables the global and every per-UI runtime
    /// (FlowRuntime::setEnabled). Enabled by default.
    void enableFlow(bool on);
    bool flowEnabled() const { return m_enabled; }

    /// 13.8.3 `flowRuntime()`: the global flow runtime (nothing loaded until loadGlobalFlow()).
    FlowRuntime& flowRuntime() { return *m_global; }
    /// Parses and loads the global flow (FlowRuntime::loadGlobal() against this bundle).
    /// T-260c: called from a callback of the global flow's own propagation, it takes effect when
    /// that dispatch returns (FlowRuntime::load()); parse errors are still reported at once.
    Status loadGlobalFlow(std::string_view json, std::vector<std::string>& errors);
    /// Parses a UI's own `.xtflow` and loads it once per registered instance of `uiId` (each
    /// against its own Document). kInvalidArgument for an unregistered id or any parse / load
    /// error (every instance's errors are collected; the ones that loaded stay loaded).
    /// T-260c: an instance whose runtime is dispatching (the call comes from one of its callbacks)
    /// is loaded when that dispatch returns, as FlowRuntime::load(); one whose Document was
    /// reloaded or unloaded by then is left empty.
    Status loadUiFlow(std::string_view uiId, std::string_view json, std::vector<std::string>& errors);
    /// The per-UI runtime loaded for `handle`; nullptr when none.
    FlowRuntime* uiFlowRuntime(DocumentHandle handle);

    /// Ticks the global and every per-UI runtime by `dt` (a disabled runtime ignores it). Call
    /// after Context::update().
    void tick(float dt);
    /// Posts a system event through Context::postSystemEvent (T-200: every Document's implicit
    /// `system.*` port), which calls back onSystemEvent(): the global and every per-UI runtime
    /// (FlowRuntime::postSystemEvent). A Context::postSystemEvent() made by the host directly
    /// reaches the flows the same way. kInvalidArgument for an unknown kind. T-356: also posted
    /// to every other Context an addUi() named (their Documents' `system.*` ports only).
    Status postSystemEvent(std::string_view kind, std::string_view value = {});
    /// Forwards to every runtime this object owns, now and loaded later.
    void setHostEventListener(FlowHostEventListener listener);

private:
    void onShown(DocumentHandle document) override;
    void onHidden(DocumentHandle document) override;
    void onSystemEvent(std::string_view kind, std::string_view value) override;

    /// T-356: forwards the lifecycle events of a Context other than context().
    class OwnerListener : public ContextListener {
    public:
        OwnerListener(BundleContext& bundle, Context& owner) : m_bundle(bundle), m_owner(owner) {}
        Context& owner() { return m_owner; }
        void onShown(DocumentHandle document) override;
        void onHidden(DocumentHandle document) override;

    private:
        BundleContext& m_bundle;
        Context& m_owner;
    };
    void lifecycleEvent(const Context* owner, DocumentHandle document, bool shown);

    struct Ui {
        std::string id;
        Context* context = nullptr;  ///< T-356: where `handle` lives (context() by default).
        DocumentHandle handle = 0;
        std::unique_ptr<FlowRuntime> flow;  ///< Its own `.xtflow`, when loaded.
    };

    Context& m_context;
    std::vector<Ui> m_uis;
    std::vector<std::unique_ptr<OwnerListener>> m_owners;  ///< T-356: one per other Context.
    /// T-260b: per-UI runtimes removeUi() took out of m_uis; destroyed by the next tick().
    std::vector<std::unique_ptr<FlowRuntime>> m_retired;
    std::unique_ptr<FlowRuntime> m_global;
    FlowHostEventListener m_hostListener;
    bool m_enabled = true;
};

}  // namespace xt::flow
