#pragma once

#include "xt/context.h"

/// T-260a (Docs/tasks/T-260a.md): marks one entry point that may call host callbacks
/// synchronously -- Context::update(), input dispatch, and every public Document / ElementRef call
/// that runs triggers or listeners. While any is on the stack, Context::unloadDocument() and the
/// installation half of reloadDocument() / hotReload() are queued; the queue runs when the
/// outermost guard is destroyed. It must be the entry point's first local variable, and nothing
/// may touch a member after it is destroyed (the Document running the call may be gone by then).
/// A null Context (a Document loaded without one) makes it a no-op.

namespace xt {

class DispatchGuard {
public:
    explicit DispatchGuard(Context* context) : m_context(context) {
        if (m_context != nullptr) {
            ++m_context->m_dispatchDepth;
        }
    }
    ~DispatchGuard() {
        if (m_context != nullptr && --m_context->m_dispatchDepth == 0 &&
            (!m_context->m_pendingDocOps.empty() || m_context->m_surfaceRequestsPending)) {
            m_context->runPendingDocOps();
        }
    }
    DispatchGuard(const DispatchGuard&) = delete;
    DispatchGuard& operator=(const DispatchGuard&) = delete;

private:
    Context* m_context;
};

}  // namespace xt
