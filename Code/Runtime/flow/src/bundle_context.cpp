#include "xt/flow/bundle_context.h"

#include <algorithm>
#include <utility>

#include "flow_internal.h"

/// BundleContext (T-193; Docs/design/27-interaction.md §27.4, 13-api.md §13.8.3). See the header.

namespace xt::flow {

BundleContext::BundleContext(Context& context)
    : m_context(context), m_global(std::make_unique<FlowRuntime>()) {
    m_context.addListener(this);
}

BundleContext::~BundleContext() {
    m_context.removeListener(this);
    for (const std::unique_ptr<OwnerListener>& owner : m_owners) {
        owner->owner().removeListener(owner.get());
    }
    // The runtimes unbind from their Documents in their destructors (members, destroyed after
    // this body). T-260b: a registered UI whose Document was unloaded or reloaded meanwhile is
    // skipped there (liveDocument()), so the host need not removeUi() it first.
}

Status BundleContext::addUi(std::string_view uiId, DocumentHandle handle) {
    return addUi(uiId, m_context, handle);
}

Status BundleContext::addUi(std::string_view uiId, Context& owner, DocumentHandle handle) {
    if (uiId.empty() || owner.document(handle) == nullptr) {
        return Status::kInvalidArgument;
    }
    for (const Ui& ui : m_uis) {
        if (ui.context == &owner && ui.handle == handle) {
            return Status::kInvalidArgument;
        }
    }
    if (&owner != &m_context) {
        bool known = false;
        for (const std::unique_ptr<OwnerListener>& listener : m_owners) {
            known = known || &listener->owner() == &owner;
        }
        if (!known) {
            m_owners.push_back(std::make_unique<OwnerListener>(*this, owner));
            owner.addListener(m_owners.back().get());
        }
    }
    Ui ui;
    ui.id = std::string(uiId);
    ui.context = &owner;
    ui.handle = handle;
    m_uis.push_back(std::move(ui));
    return Status::kOk;
}

Status BundleContext::removeUi(std::string_view uiId) {
    bool any = false;
    for (size_t i = 0; i < m_uis.size();) {
        if (m_uis[i].id != uiId) {
            ++i;
            continue;
        }
        any = true;
        if (std::unique_ptr<FlowRuntime> flow = std::move(m_uis[i].flow)) {
            // Unbound now (only from a Document the Context still holds at the handle) and kept
            // until the next tick(): this call may come from one of its own callbacks.
            detachDocumentEndpoints(flow->m_impl->docs);
            flow->m_impl->pending = FlowRuntime::Impl::PendingLoad{};  // T-260c: nor loaded later.
            flow->setEnabled(false);
            m_retired.push_back(std::move(flow));
        }
        m_uis.erase(m_uis.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (!any) {
        return Status::kInvalidArgument;
    }
    m_global->m_impl->removeUi(uiId);
    return Status::kOk;
}

std::vector<std::string> BundleContext::uiIds() const {
    std::vector<std::string> ids;
    for (const Ui& ui : m_uis) {
        if (std::find(ids.begin(), ids.end(), ui.id) == ids.end()) {
            ids.push_back(ui.id);
        }
    }
    return ids;
}

std::vector<DocumentHandle> BundleContext::handles(std::string_view uiId) const {
    std::vector<DocumentHandle> out;
    for (const Ui& ui : m_uis) {
        if (ui.id == uiId && ui.context == &m_context) {
            out.push_back(ui.handle);
        }
    }
    return out;
}

std::vector<BundleContext::Instance> BundleContext::instances(std::string_view uiId) const {
    std::vector<Instance> out;
    for (const Ui& ui : m_uis) {
        if (ui.id == uiId) {
            out.push_back(Instance{ui.context, ui.handle});
        }
    }
    return out;
}

std::string_view BundleContext::uiOf(DocumentHandle handle) const {
    for (const Ui& ui : m_uis) {
        if (ui.handle == handle && ui.context == &m_context) {
            return ui.id;
        }
    }
    return {};
}

void BundleContext::enableFlow(bool on) {
    m_enabled = on;
    m_global->setEnabled(on);
    // T-260c: by index, re-reading the size (as tick()): no reference into m_uis across a call.
    for (size_t i = 0; i < m_uis.size(); ++i) {
        if (FlowRuntime* flow = m_uis[i].flow.get()) {
            flow->setEnabled(on);
        }
    }
}

Status BundleContext::loadGlobalFlow(std::string_view json, std::vector<std::string>& errors) {
    m_global->setEnabled(m_enabled);
    m_global->setHostEventListener(m_hostListener);
    return m_global->loadGlobal(json, *this, errors);
}

Status BundleContext::loadUiFlow(std::string_view uiId, std::string_view json,
                                 std::vector<std::string>& errors) {
    FlowGraph graph;
    if (FlowGraph::parse(json, graph, errors) != Status::kOk) {
        return Status::kInvalidArgument;
    }
    bool any = false;
    bool ok = true;
    // T-260c: by index, re-reading the size: load() sets the Document's ports, whose triggers can
    // reach a host callback that calls removeUi() / addUi(); `ui` is not used past that call.
    for (size_t i = 0; i < m_uis.size(); ++i) {
        Ui& ui = m_uis[i];
        if (ui.id != uiId) {
            continue;
        }
        any = true;
        Document* document = ui.context->document(ui.handle);
        if (document == nullptr) {
            ok = false;
            continue;
        }
        if (!ui.flow) {
            ui.flow = std::make_unique<FlowRuntime>();
        }
        FlowRuntime& flow = *ui.flow;
        // T-260b: the runtime knows the handle, so it re-resolves its Document at every use (and
        // load() below releases the previous one's listeners only if that one is still there).
        flow.m_impl->docs.context = ui.context;
        flow.m_impl->docs.handle = ui.handle;
        flow.setEnabled(m_enabled);
        flow.setHostEventListener(m_hostListener);
        if (flow.load(graph, *document, errors) != Status::kOk) {
            ok = false;
        }
    }
    if (!any) {
        errors.push_back("unknown UI '" + std::string(uiId) + "'");
        return Status::kInvalidArgument;
    }
    return ok ? Status::kOk : Status::kInvalidArgument;
}

FlowRuntime* BundleContext::uiFlowRuntime(DocumentHandle handle) {
    for (Ui& ui : m_uis) {
        if (ui.handle == handle && ui.context == &m_context) {
            return ui.flow.get();
        }
    }
    return nullptr;
}

void BundleContext::tick(float dt) {
    m_retired.clear();  // T-260b: removeUi()'s runtimes; no callback of theirs is running now.
    m_global->tick(dt);
    // T-260b: by index, re-reading the size: a callback may addUi() / removeUi() meanwhile.
    for (size_t i = 0; i < m_uis.size(); ++i) {
        if (FlowRuntime* flow = m_uis[i].flow.get()) {
            flow->tick(dt);
        }
    }
}

Status BundleContext::postSystemEvent(std::string_view kind, std::string_view value) {
    // T-200: through the Context, so every Document's `system.*` port gets it too; the Context
    // then calls onSystemEvent(), which reaches the flows.
    const Status status = m_context.postSystemEvent(kind, value);
    if (status == Status::kOk) {
        // T-356: the other Contexts' Documents; their OwnerListener ignores the callback.
        for (size_t i = 0; i < m_owners.size(); ++i) {
            (void)m_owners[i]->owner().postSystemEvent(kind, value);
        }
    }
    return status;
}

void BundleContext::onSystemEvent(std::string_view kind, std::string_view value) {
    if (m_global->postSystemEvent(kind, value) != Status::kOk) {
        return;
    }
    for (size_t i = 0; i < m_uis.size(); ++i) {  // T-260b: as tick().
        if (FlowRuntime* flow = m_uis[i].flow.get()) {
            flow->postSystemEvent(kind, value);
        }
    }
}

void BundleContext::setHostEventListener(FlowHostEventListener listener) {
    m_hostListener = std::move(listener);
    m_global->setHostEventListener(m_hostListener);
    for (size_t i = 0; i < m_uis.size(); ++i) {  // T-260c: as enableFlow().
        if (FlowRuntime* flow = m_uis[i].flow.get()) {
            flow->setHostEventListener(m_hostListener);
        }
    }
}

void BundleContext::onShown(DocumentHandle document) { lifecycleEvent(&m_context, document, true); }

void BundleContext::onHidden(DocumentHandle document) { lifecycleEvent(&m_context, document, false); }

void BundleContext::lifecycleEvent(const Context* owner, DocumentHandle document, bool shown) {
    m_global->m_impl->lifecycleEvent(owner, document, shown);
}

void BundleContext::OwnerListener::onShown(DocumentHandle document) {
    m_bundle.lifecycleEvent(&m_owner, document, true);
}

void BundleContext::OwnerListener::onHidden(DocumentHandle document) {
    m_bundle.lifecycleEvent(&m_owner, document, false);
}

}  // namespace xt::flow
