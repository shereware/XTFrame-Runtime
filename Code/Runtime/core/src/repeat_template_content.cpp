#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "repeat_controller.h"
#include "xt/assert.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/scene_instance.h"

/// T-624: what a `<repeat>` instance copies of its template besides the elements and their text
/// (repeat_controller.cpp): the scene of an inline `<svg>` / `<img src="*.svg">` (its own scene
/// instance, so it paints and its scene-node bindings target the copy's nodes) and the `@key`
/// places of the template (T-179: resolved when the instance is made and on every
/// setLocalization(), with the document's rule that a place the host replaced keeps its value).

namespace xt {

void RepeatController::resolveLocalizedSlot(Document& doc, Document::LocalizedSlot& slot,
                                            ILocalizationProvider* provider, std::string_view lang) {
    NodeArena& arena = doc.m_arena;
    const StringId current = doc.localizedCurrent(slot);
    if (current == kInvalidString || current != slot.shown) {
        return;  // Gone, or the host replaced it (setText/setAttribute).
    }
    const std::string_view key = doc.m_ir.string(slot.key);
    std::string_view text = provider != nullptr ? provider->resolve(key, lang) : std::string_view{};
    if (text.empty()) {
        text = key;
    }
    const StringId sid = doc.intern(text);
    slot.shown = sid;
    if (sid == current) {
        return;
    }
    if (slot.attrIndex == 0xFFFFFFFFu) {
        TextNodeData& data = arena.text(slot.node);
        data.text = sid;
        data.shapedValid = 0;
        arena.markDirty(slot.node, static_cast<uint8_t>(kDirtyLayout));
        const NodeId parent = arena.parent(slot.node);
        if (parent != kInvalidNode) {
            arena.markDirty(parent, static_cast<uint8_t>(kDirtyLayout));
        }
    } else {
        ElementData& element = arena.element(slot.node);
        element.attributes[slot.attrIndex].second = sid;
        arena.markDirty(slot.node, static_cast<uint8_t>(kDirtyStyle));  // `[attr]` selectors.
        if (doc.m_ir.string(element.attributes[slot.attrIndex].first) == "placeholder") {
            arena.markDirty(slot.node, static_cast<uint8_t>(kDirtyLayout));  // T-395: relaid out.
        }
    }
}

void RepeatController::instantiateScene(NodeId element, SceneId scene) {
    SceneInstance inst;
    Status status = xt::instantiateScene(m_doc.m_ir, scene, m_doc.m_arena, element, inst);
    XT_ASSERT(status == Status::kOk);
    (void)status;
    // A slot a removed element's scene left (destroySceneInstance() reset it) is reused, so
    // instances made and destroyed over and over do not grow the table.
    std::vector<SceneInstance>& table = m_doc.m_instances;
    uint32_t slot = static_cast<uint32_t>(table.size());
    for (uint32_t s = 0; s < table.size(); ++s) {
        if (table[s].scene == kInvalidScene && table[s].root == kInvalidNode) {
            slot = s;
            break;
        }
    }
    ElementData& data = m_doc.m_arena.element(element);
    data.scene = scene;
    data.sceneRoot = inst.root;
    data.sceneInstanceIndex = slot;
    if (slot == table.size()) {
        table.push_back(std::move(inst));
    } else {
        table[slot] = std::move(inst);
    }
}

void RepeatController::copyTemplateContent(Repeat& r, Instance& inst) {
    NodeArena& arena = m_doc.m_arena;
    const IrView& ir = m_doc.m_ir;
    std::span<const IrDocElement> elements = ir.doc().elements();
    std::span<const uint32_t> l10nKeys = ir.localization().keys();
    std::span<const uint32_t> textKeys = ir.localization().textKeys();
    std::span<const uint32_t> attrKeys = ir.localization().attrKeys();
    for (size_t k = 0; k < r.tpl.size(); ++k) {
        const IrDocElement& src = elements[r.tpl[k]];
        const NodeId n = inst.nodes[k];
        if (n == kInvalidNode || !arena.alive(n)) {
            continue;
        }
        const std::string_view tag =
            arena.element(n).tag != kInvalidString ? ir.string(arena.element(n).tag) : std::string_view{};
        // An `anim` copy stays a plain element (its timeline would need its own autoplay).
        if (src.scene != kInvalidScene && tag != "anim") {
            instantiateScene(n, src.scene);
        }
        for (uint32_t a = 0; a < src.attrCount; ++a) {
            const uint32_t attrIndex = src.attrOffset + a;
            if (attrIndex >= attrKeys.size() || attrKeys[attrIndex] == 0 ||
                a >= arena.element(n).attributes.size()) {
                continue;
            }
            Document::LocalizedSlot slot;
            slot.node = n;
            slot.generation = arena.generation(n);
            slot.attrIndex = a;
            slot.key = l10nKeys[attrKeys[attrIndex] - 1];
            inst.l10n.push_back(slot);
        }
        if (src.firstTextChild < textKeys.size() && textKeys[src.firstTextChild] != 0 && tag != "input") {
            NodeId t = arena.firstChild(n);
            while (t != kInvalidNode && arena.kind(t) != NodeKind::Text) {
                t = arena.nextSibling(t);
            }
            if (t != kInvalidNode) {
                Document::LocalizedSlot slot;
                slot.node = t;
                slot.generation = arena.generation(t);
                slot.key = l10nKeys[textKeys[src.firstTextChild] - 1];
                inst.l10n.push_back(slot);
            }
        }
    }
    relocalizeInstance(inst);
}

void RepeatController::relocalizeInstance(Instance& inst) {
    // The places hold the template's values (just copied, or reset from the pool): untouched.
    for (Document::LocalizedSlot& slot : inst.l10n) {
        slot.shown = m_doc.localizedCurrent(slot);
        resolveLocalizedSlot(m_doc, slot, m_doc.m_l10nProvider, m_doc.m_language);
    }
}

void RepeatController::relocalize(ILocalizationProvider* provider, std::string_view lang) {
    for (Repeat& r : m_repeats) {
        for (std::vector<Instance>* list : {&r.live, &r.exiting, &r.pool}) {
            for (Instance& inst : *list) {
                for (Document::LocalizedSlot& slot : inst.l10n) {
                    resolveLocalizedSlot(m_doc, slot, provider, lang);
                }
            }
        }
    }
}

std::string_view RepeatController::localizationKeyOf(NodeId element) const {
    const NodeArena& arena = m_doc.m_arena;
    for (const Repeat& r : m_repeats) {
        for (const std::vector<Instance>* list : {&r.live, &r.exiting}) {
            for (const Instance& inst : *list) {
                for (const Document::LocalizedSlot& slot : inst.l10n) {
                    if (slot.attrIndex != 0xFFFFFFFFu || !arena.alive(slot.node) ||
                        arena.parent(slot.node) != element) {
                        continue;
                    }
                    const StringId current = m_doc.localizedCurrent(slot);
                    if (current != kInvalidString && current == slot.shown) {
                        return m_doc.m_ir.string(slot.key);
                    }
                }
            }
        }
    }
    return std::string_view{};
}

}  // namespace xt
