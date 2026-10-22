#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "binding_runtime.h"
#include "document_state.h"
#include "mini_css.h"
#include "timeline_group.h"
#include "xt/context.h"
#include "xt/document.h"

/// Context::hotReload (T-200; Docs/design/13-api.md §13.8.3, 22-bundle.md §22.10). The state a
/// recompiled Document carries over is matched by id: port values by qualified name, custom states
/// by element id and state name, focus by element id, timeline groups by scope id and name, input
/// values and scroll offsets by element id (T-398: Code/Runtime/core/src/document_state.cpp).

namespace xt {

Status Context::hotReload(const BundleDelta& delta, HotReloadReport* report) {
    Status result = Status::kOk;
    auto fail = [&result](Status status) {
        if (result == Status::kOk && status != Status::kOk) {
            result = status;
        }
    };
    // Context-level changes first, so a reloaded Document picks them up while it is prepared.
    if (delta.tokens) {
        for (const TokenDef& def : *delta.tokens) {
            Value value;
            fail(mini_css::classifyTokenText(def.value, value) ? setToken(def.name, value)
                                                               : Status::kInvalidArgument);
        }
    }
    for (const ThemeDef& theme : delta.themes) {
        fail(defineTheme(theme.name, theme.tokens));
    }
    if (delta.theme) {
        fail(setTheme(*delta.theme));
    }
    if (delta.strings) {
        setLocalizationProvider(*delta.strings);
        m_damageBaseValid = false;
    }
    if (delta.glyphs) {
        setGlyphProvider(*delta.glyphs);
    }
    if (delta.fonts) {
        // The host re-registered the fonts with its TextShaper: shape and lay every text out again.
        const size_t count = m_documents.size();  // T-260a: every m_documents walk is by index.
        for (size_t i = 0; i < count; ++i) {
            Document* doc = liveAt(i);
            if (doc == nullptr || doc->m_root == kInvalidNode) {
                continue;
            }
            NodeArena& arena = doc->m_arena;
            arena.traversePreorder(doc->m_root, [&arena](NodeId n) {
                if (arena.kind(n) == NodeKind::Element) {
                    arena.markDirty(n, static_cast<uint8_t>(kDirtyLayout));
                } else if (arena.kind(n) == NodeKind::Text) {
                    arena.text(n).shapedValid = 0;
                    arena.markDirty(n, static_cast<uint8_t>(kDirtyLayout));
                }
                return true;
            });
        }
        m_damageBaseValid = false;
    }
    for (const IrReload& reload : delta.ir) {
        HotReloadResult one = hotReloadDocument(reload);
        fail(one.status);
        if (report != nullptr) {
            report->documents.push_back(std::move(one));
        }
    }
    return result;
}

HotReloadResult Context::hotReloadDocument(const IrReload& reload) {
    HotReloadResult out;
    out.handle = reload.handle;
    std::unique_ptr<Document> fresh;
    out.status = prepareReload(reload.handle, reload.blob, reload.takeOwnership, fresh);
    if (out.status != Status::kOk) {
        return out;
    }
    Document& old = *m_documents[reload.handle - 1];
    Document& doc = *fresh;

    // 1. Snapshot the old state (T-398: the collect / check / apply code Document::snapshotState()
    // and restoreState() use; ports are adopted directly below, typed, instead of through text).
    DocumentState state;
    old.collectState(state, /*withPorts=*/false);

    // 2. Everything it refers to must exist in the new Document (input values and scroll offsets
    // of elements that are gone are simply not carried).
    doc.m_bindings->missingPorts(*old.m_bindings, out.missing);
    doc.missingState(state, out.missing);
    if (!out.missing.empty()) {
        out.status = Status::kIncompatible;  // Starts fresh, like reloadDocument().
        installReload(reload.handle, fresh);
        return out;
    }

    // 3. Carry it over: ports (bindings follow), then the exact custom states, input values,
    // focus, timelines and scroll offsets.
    doc.m_clock = old.m_clock;
    doc.m_bindings->adoptPorts(*old.m_bindings);
    doc.applyState(state, /*exact=*/false);
    installReload(reload.handle, fresh);
    return out;
}

}  // namespace xt
