#include "xt/document.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

#include "binding_runtime.h"
#include "camera.h"
#include "composition.h"
#include "dispatch_guard.h"
#include "event_dispatcher.h"
#include "repeat_controller.h"
#include "rig_eval.h"
#include "text_edit.h"
#include "timeline_group.h"
#include "trigger_runtime.h"
#include "xt/assert.h"
#include "xt/context.h"
#include "xt/project3d.h"
#include "xt/property.h"

/// Document's public API (everything except the load flow, see document_load.cpp). See
/// Docs/tasks/T-18.md, Docs/tasks/T-24.md (ElementRef class/state API), Docs/tasks/T-32.md
/// (events: hit test, listener pool, pointer state machine, animation event bridging),
/// Docs/tasks/T-33.md (focus, the EditState pool and the `value` API), Docs/tasks/T-35.md (the
/// runtime string pool, Document::query and the dynamic createElement/appendChild/insertBefore/
/// remove tree API), Docs/tasks/T-33b.md (the IME composition state, textInputRect).
/// ElementRef's own methods live in element_ref.cpp.

namespace xt {

Document::Document()
    : m_events(std::make_unique<EventDispatcher>()),
      m_editStates(std::make_unique<std::vector<EditState>>()),
      m_composition(std::make_unique<CompositionState>()),
      m_timelineGroups(std::make_unique<TimelineGroupSet>()),
      m_triggers(std::make_unique<TriggerRuntime>(*this)),
      m_bindings(std::make_unique<BindingRuntime>(*this)),
      m_repeats(std::make_unique<RepeatController>(*this)),
      m_rigs(std::make_unique<std::vector<rig::RigState>>()) {}
Document::~Document() = default;

Status Document::load(IrBlob blob, bool takeOwnership, TextShaper* shaper, AssetResolver* assets,
                      std::unique_ptr<Document>& out) {
    std::unique_ptr<Document> doc(new Document());
    Status status = doc->loadInternal(blob, takeOwnership, shaper, assets);
    if (status != Status::kOk) {
        return status;
    }
    out = std::move(doc);
    return Status::kOk;
}

ElementRef Document::root() const {
    if (m_root == kInvalidNode) {
        return ElementRef{};
    }
    return ElementRef{const_cast<Document*>(this), m_root, m_arena.generation(m_root)};
}

ElementRef Document::fixedRoot() const {
    const std::vector<ElementRef> roots = fixedRoots();
    return roots.empty() ? ElementRef{} : roots.front();
}

std::vector<ElementRef> Document::fixedRoots(Vec2 board) const {
    std::vector<ElementRef> roots;
    ElementRef body = root();
    if (body.valid() && body.tag() != "body") {
        ElementRef child = body.firstChild();
        while (child.valid() && child.tag() != "body") {
            child = child.nextSibling();
        }
        body = child;
    }
    if (!body.valid()) {
        return roots;
    }
    const auto isPx = [](const ElementRef& element, PropId prop) {
        const Value v = element.property(prop);
        return v.type == ValueType::Length && v.len.unit == LengthUnit::Px;
    };
    // T-481: a px length covering `extent` (the design board's edge, less 1 px).
    const auto covers = [](const ElementRef& element, PropId prop, float extent) {
        const Value v = element.property(prop);
        return extent > 0 && v.type == ValueType::Length && v.len.unit == LengthUnit::Px &&
               v.len.value >= extent - 1.0f;
    };
    const ElementRef first = body.firstChild();
    if (first.valid() && !first.nextSibling().valid()) {
        if (isPx(first, prop::Width) || isPx(first, prop::Height)) {
            roots.push_back(first);
        }
    } else {
        for (ElementRef child = first; child.valid(); child = child.nextSibling()) {
            if (covers(child, prop::Width, board.x) && covers(child, prop::Height, board.y)) {
                roots.push_back(child);
            }
        }
    }
    if (roots.empty() && (isPx(body, prop::Width) || isPx(body, prop::Height))) {
        roots.push_back(body);
    }
    return roots;
}

ElementRef Document::byId(std::string_view id) const {
    for (NodeId node : m_docElementNodes) {
        if (node == kInvalidNode || !m_arena.alive(node)) {
            continue;  // Removed by Document::remove().
        }
        const ElementData& data = m_arena.element(node);
        if (data.id == kInvalidString || (m_arena.common(node).flags2 & kTemplate) != 0) {
            continue;  // T-177: a `<repeat>` template element is not part of the document.
        }
        if (string(data.id) == id) {
            return ElementRef{const_cast<Document*>(this), node, m_arena.generation(node)};
        }
    }
    // T-177: `<repeat>` instance elements (`Item/3/Cell`).
    return m_repeats->byId(id);
}

ElementRef Document::elementAt(uint32_t docElementIndex) const {
    if (docElementIndex >= m_docElementNodes.size()) {
        return ElementRef{};
    }
    NodeId node = m_docElementNodes[docElementIndex];
    if (node == kInvalidNode || !m_arena.alive(node)) {
        return ElementRef{};
    }
    return ElementRef{const_cast<Document*>(this), node, m_arena.generation(node)};
}

std::string_view Document::string(StringId id) const {
    if (id == kInvalidString) {
        return std::string_view{};
    }
    if (id < kRuntimeStringBase) {
        return m_ir.string(id);
    }
    if (id >= kRuntimeTextBase) {
        uint32_t slot = id - kRuntimeTextBase;
        return slot < m_runtimeTexts.size() ? std::string_view(m_runtimeTexts[slot]) : std::string_view{};
    }
    uint32_t index = id - kRuntimeStringBase;
    if (index >= m_runtimeStrings.size()) {
        return std::string_view{};
    }
    return std::string_view(m_runtimeStrings[index]);
}

StringId Document::setRuntimeText(NodeId textNode, std::string_view content) {
    // T-270a (C6): the Text node's own slot, overwritten in place (assign keeps its capacity);
    // a node that has none yet (or whose id came from elsewhere, e.g. a `<repeat>` copy) takes a
    // released slot or a new one.
    const StringId current = m_arena.text(textNode).text;
    if (current >= kRuntimeTextBase && current != kInvalidString) {
        const uint32_t slot = current - kRuntimeTextBase;
        if (slot < m_runtimeTextOwners.size() && m_runtimeTextOwners[slot] == textNode) {
            m_runtimeTexts[slot].assign(content);
            return current;
        }
    }
    uint32_t slot = 0;
    if (!m_freeRuntimeTexts.empty()) {
        slot = m_freeRuntimeTexts.back();
        m_freeRuntimeTexts.pop_back();
        m_runtimeTexts[slot].assign(content);
    } else {
        slot = static_cast<uint32_t>(m_runtimeTexts.size());
        m_runtimeTexts.emplace_back(content);
        m_runtimeTextOwners.push_back(kInvalidNode);
    }
    m_runtimeTextOwners[slot] = textNode;
    return kRuntimeTextBase + slot;
}

void Document::releaseRuntimeText(NodeId textNode) {
    TextNodeData& data = m_arena.text(textNode);
    const StringId current = data.text;
    if (current < kRuntimeTextBase || current == kInvalidString) {
        return;
    }
    const uint32_t slot = current - kRuntimeTextBase;
    if (slot < m_runtimeTextOwners.size() && m_runtimeTextOwners[slot] == textNode) {
        m_runtimeTextOwners[slot] = kInvalidNode;
        m_runtimeTexts[slot].clear();
        m_freeRuntimeTexts.push_back(slot);
    }
    // T-270b: the node stops referring to the slot, which another node may take next. A recycled
    // `<repeat>` instance's Text node stays alive in the pool (RepeatController::resetFromTemplate
    // gives it its text again).
    data.text = kInvalidString;
    data.shapedValid = 0;
}

StringId Document::setNodeText(NodeId node, uint16_t key, std::string_view content) {
    std::vector<NodeText>& own = m_nodeTexts[node];
    for (const NodeText& t : own) {
        if (t.key == key) {
            m_runtimeTexts[t.slot].assign(content);
            return kRuntimeTextBase + t.slot;
        }
    }
    uint32_t slot = 0;
    if (!m_freeRuntimeTexts.empty()) {
        slot = m_freeRuntimeTexts.back();
        m_freeRuntimeTexts.pop_back();
        m_runtimeTexts[slot].assign(content);
    } else {
        slot = static_cast<uint32_t>(m_runtimeTexts.size());
        m_runtimeTexts.emplace_back(content);
        m_runtimeTextOwners.push_back(kInvalidNode);
    }
    m_runtimeTextOwners[slot] = kNodeTextOwner;
    own.push_back(NodeText{key, slot});
    return kRuntimeTextBase + slot;
}

void Document::releaseNodeTexts(NodeId node) {
    auto own = m_nodeTexts.find(node);
    if (own == m_nodeTexts.end()) {
        return;
    }
    StringId& id = m_arena.element(node).id;
    for (const NodeText& t : own->second) {
        if (id == kRuntimeTextBase + t.slot) {
            id = kInvalidString;  // A recycled `<repeat>` instance is renamed when it is reused.
        }
        m_runtimeTextOwners[t.slot] = kInvalidNode;
        m_runtimeTexts[t.slot].clear();
        m_freeRuntimeTexts.push_back(t.slot);
    }
    m_nodeTexts.erase(own);
}

StringId Document::intern(std::string_view s) {
    const StringId existing = findString(s);
    if (existing != kInvalidString) {
        return existing;
    }
    m_runtimeStrings.emplace_back(s);
    const StringId id = kRuntimeStringBase + static_cast<StringId>(m_runtimeStrings.size() - 1);
    m_runtimeStringIndex.emplace(std::string_view(m_runtimeStrings.back()), id);
    return id;
}

StringId Document::findString(std::string_view s) {
    // 1. Already in the IR string table? Build the by-text ordering on first use (T-24's addClass
    // did a linear scan per call; 13.2's "tag 必須存在於 IR 字串表，否則 intern 到執行時字串池").
    if (!m_irStringOrderBuilt) {
        uint32_t count = m_ir.strings().count();
        m_irStringOrder.reserve(count);
        for (StringId i = 0; i < count; ++i) {
            m_irStringOrder.push_back(i);
        }
        const IrView* ir = &m_ir;
        std::sort(m_irStringOrder.begin(), m_irStringOrder.end(),
                  [ir](StringId a, StringId b) { return ir->string(a) < ir->string(b); });
        m_irStringOrderBuilt = true;
    }
    const IrView* ir = &m_ir;
    auto it = std::lower_bound(m_irStringOrder.begin(), m_irStringOrder.end(), s,
                               [ir](StringId a, std::string_view text) { return ir->string(a) < text; });
    if (it != m_irStringOrder.end() && m_ir.string(*it) == s) {
        return *it;
    }
    // 2. Already interned at runtime? A hash index (T-270a): the pool holds every tag, id, class,
    // attribute and string port value the host or the bindings ever interned, so it can be large.
    auto found = m_runtimeStringIndex.find(s);
    return found != m_runtimeStringIndex.end() ? found->second : kInvalidString;
}

Value Document::declareRuntimeEffects(std::span<const EffectInstance> effects,
                                      std::span<const GradientStop> stops) {
    uint32_t startIndex = static_cast<uint32_t>(m_runtimeEffects.size());
    for (const EffectInstance& src : effects) {
        EffectInstance dst = src;
        uint32_t localOffset = src.stopOffset;
        uint32_t localCount = src.stopCount;
        uint32_t stopBase = static_cast<uint32_t>(m_runtimeEffectStops.size());
        for (uint32_t i = 0; i < localCount && localOffset + i < stops.size(); ++i) {
            m_runtimeEffectStops.push_back(stops[localOffset + i]);
        }
        dst.stopOffset = stopBase;
        dst.stopCount = static_cast<uint32_t>(m_runtimeEffectStops.size() - stopBase);
        m_runtimeEffects.push_back(dst);
    }
    Value v;
    v.type = ValueType::ListRef;
    v.sid = kRuntimeEffectOffsetBit | startIndex;
    v.floatCount = static_cast<uint32_t>(effects.size());
    return v;
}

Value Document::declareRuntimeEffectsFor(NodeId node, std::span<const EffectInstance> effects,
                                         std::span<const GradientStop> stops) {
    Value v;
    v.type = ValueType::ListRef;
    v.sid = kRuntimeEffectOffsetBit;
    v.floatCount = static_cast<uint32_t>(effects.size());
    if (effects.empty()) {
        return v;  // `effect: none`: no pool entry, like declareRuntimeGradient's `none`.
    }
    uint32_t stopTotal = 0;
    for (const EffectInstance& src : effects) {
        for (uint32_t i = 0; i < src.stopCount && src.stopOffset + i < stops.size(); ++i) {
            ++stopTotal;
        }
    }
    const uint32_t effectTotal = static_cast<uint32_t>(effects.size());
    auto fits = [&](const RuntimeEffectSegment& s) {
        return s.effectCap >= effectTotal && s.stopCap >= stopTotal;
    };
    // T-270a (C6): the node's own segment when the new sequence fits it; else a released segment
    // that fits (first fit), else a new one at the pool's end. The outgrown segment is released.
    RuntimeEffectSegment seg;
    auto own = m_runtimeEffectSegments.find(node);
    if (own != m_runtimeEffectSegments.end() && fits(own->second)) {
        seg = own->second;
    } else {
        if (own != m_runtimeEffectSegments.end()) {
            m_freeEffectSegments.push_back(own->second);
        }
        auto freeIt = std::find_if(m_freeEffectSegments.begin(), m_freeEffectSegments.end(), fits);
        if (freeIt != m_freeEffectSegments.end()) {
            seg = *freeIt;
            m_freeEffectSegments.erase(freeIt);
        } else {
            seg.effectOffset = static_cast<uint32_t>(m_runtimeEffects.size());
            seg.effectCap = effectTotal;
            seg.stopOffset = static_cast<uint32_t>(m_runtimeEffectStops.size());
            seg.stopCap = stopTotal;
            m_runtimeEffects.resize(m_runtimeEffects.size() + effectTotal);
            m_runtimeEffectStops.resize(m_runtimeEffectStops.size() + stopTotal);
        }
        m_runtimeEffectSegments[node] = seg;
    }
    uint32_t stopCursor = seg.stopOffset;
    for (uint32_t k = 0; k < effectTotal; ++k) {
        const EffectInstance& src = effects[k];
        EffectInstance dst = src;
        dst.stopOffset = stopCursor;
        for (uint32_t i = 0; i < src.stopCount && src.stopOffset + i < stops.size(); ++i) {
            m_runtimeEffectStops[stopCursor++] = stops[src.stopOffset + i];
        }
        dst.stopCount = stopCursor - dst.stopOffset;
        m_runtimeEffects[seg.effectOffset + k] = dst;
    }
    v.sid = kRuntimeEffectOffsetBit | seg.effectOffset;
    return v;
}

void Document::releaseRuntimeEffects(NodeId node) {
    auto own = m_runtimeEffectSegments.find(node);
    if (own == m_runtimeEffectSegments.end()) {
        return;
    }
    // T-270d: the committed style stops pointing at the segment the moment it is freed (another
    // element may take it next; a recycled `<repeat>` instance keeps its node and its style).
    ComputedStyle& style = m_arena.element(node).style;
    if (style.effectCount != 0 && (style.effectOffset & kRuntimeEffectOffsetBit) != 0 &&
        (style.effectOffset & ~kRuntimeEffectOffsetBit) == own->second.effectOffset) {
        style.effectOffset = 0;
        style.effectCount = 0;
    }
    m_freeEffectSegments.push_back(own->second);
    m_runtimeEffectSegments.erase(own);
}

Value Document::declareRuntimeGradient(const ElementGradient& gradient) {
    Value v;
    v.type = ValueType::ListRef;
    if (gradient.kind == GradientKind::None) {
        v.sid = kRuntimeEffectOffsetBit;
        v.floatCount = 0;  // 05.5c `none`: a ListRef with count 0, like `effect: none`.
        return v;
    }
    uint32_t index = static_cast<uint32_t>(m_runtimeGradients.size());
    m_runtimeGradients.push_back(gradient);
    v.sid = kRuntimeEffectOffsetBit | index;
    v.floatCount = 1;
    return v;
}

Value Document::declareRuntimeAnimations(std::span<const IrAnimationSpec> specs) {
    Value v;
    v.type = ValueType::ListRef;
    if (specs.empty()) {
        v.sid = kRuntimeEffectOffsetBit;
        v.floatCount = 0;  // `animation: none`: a ListRef with count 0, like `effect: none`.
        return v;
    }
    uint32_t startIndex = static_cast<uint32_t>(m_runtimeAnimations.size());
    for (const IrAnimationSpec& spec : specs) {
        m_runtimeAnimations.push_back(spec);
    }
    v.sid = kRuntimeEffectOffsetBit | startIndex;
    v.floatCount = static_cast<uint32_t>(specs.size());
    return v;
}

uint32_t Document::takeRuntimeDeclSlot(RuntimeDeclSlots& own, std::vector<RuntimeDeclSlot>& freeList,
                                       uint32_t inUse, uint32_t need, size_t& poolSize) {
    // T-270c: the slot written before the last one, unless the cascade still references it (two
    // declarations within one frame): then the last one, which nothing resolved yet.
    uint8_t k = static_cast<uint8_t>(own.last ^ 1u);
    if (own.slot[k].cap != 0 && own.slot[k].offset == inUse) {
        k = own.last;
    }
    own.last = k;
    RuntimeDeclSlot& slot = own.slot[k];
    if (slot.cap >= need) {
        return slot.offset;
    }
    if (slot.cap != 0) {
        freeList.push_back(slot);
    }
    auto fits = [need, inUse](const RuntimeDeclSlot& s) { return s.cap >= need && s.offset != inUse; };
    auto freeIt = std::find_if(freeList.begin(), freeList.end(), fits);
    if (freeIt != freeList.end()) {
        slot = *freeIt;
        freeList.erase(freeIt);
    } else {
        slot.offset = static_cast<uint32_t>(poolSize);
        slot.cap = need;
        poolSize += need;
    }
    return slot.offset;
}

Value Document::declareRuntimeGradientFor(NodeId node, const ElementGradient& gradient) {
    if (gradient.kind == GradientKind::None) {
        return declareRuntimeGradient(gradient);  // `none`: no pool entry.
    }
    const ComputedStyle& style = m_arena.element(node).style;
    const uint32_t inUse = style.gradientCount != 0 && (style.gradientOffset & kRuntimeEffectOffsetBit) != 0
                               ? style.gradientOffset & ~kRuntimeEffectOffsetBit
                               : UINT32_MAX;
    size_t poolSize = m_runtimeGradients.size();
    const uint32_t offset =
        takeRuntimeDeclSlot(m_runtimeGradientSlots[node], m_freeGradientSlots, inUse, 1, poolSize);
    m_runtimeGradients.resize(poolSize);
    m_runtimeGradients[offset] = gradient;  // Copy-assigned: the slot's stop vector keeps its capacity.
    Value v;
    v.type = ValueType::ListRef;
    v.sid = kRuntimeEffectOffsetBit | offset;
    v.floatCount = 1;
    return v;
}

Value Document::declareRuntimeClipShapeFor(NodeId node, const ElementClipShape& shape) {
    if (shape.kind == ClipShapeKind::None) {
        return Value::ofEnum(0);  // `none`: no pool entry.
    }
    const ComputedStyle& style = m_arena.element(node).style;
    const uint32_t inUse = (style.clipShape & kRuntimeEffectOffsetBit) != 0
                               ? style.clipShape & ~kRuntimeEffectOffsetBit
                               : UINT32_MAX;
    size_t poolSize = m_runtimeClipShapes.size();
    const uint32_t offset =
        takeRuntimeDeclSlot(m_runtimeClipShapeSlots[node], m_freeClipShapeSlots, inUse, 1, poolSize);
    m_runtimeClipShapes.resize(poolSize);
    m_runtimeClipShapes[offset] = shape;  // Copy-assigned: the slot's param vector keeps its capacity.
    return Value::ofEnum(kRuntimeEffectOffsetBit | offset);
}

Value Document::declareRuntimeAnimationsFor(NodeId node, std::span<const IrAnimationSpec> specs) {
    if (specs.empty()) {
        return declareRuntimeAnimations(specs);  // `animation: none`: no pool entry.
    }
    // The CSS animation diff (applyAnimationDiff) compares against the last played offset.
    const ComputedStyle& style = m_arena.element(node).style;
    const uint32_t inUse =
        style.lastAnimationCount != 0 && (style.lastAnimationOffset & kRuntimeEffectOffsetBit) != 0
            ? style.lastAnimationOffset & ~kRuntimeEffectOffsetBit
            : UINT32_MAX;
    size_t poolSize = m_runtimeAnimations.size();
    const uint32_t count = static_cast<uint32_t>(specs.size());
    const uint32_t offset =
        takeRuntimeDeclSlot(m_runtimeAnimationSlots[node], m_freeAnimationSlots, inUse, count, poolSize);
    m_runtimeAnimations.resize(poolSize);
    std::copy(specs.begin(), specs.end(), m_runtimeAnimations.begin() + offset);
    Value v;
    v.type = ValueType::ListRef;
    v.sid = kRuntimeEffectOffsetBit | offset;
    v.floatCount = count;
    return v;
}

void Document::releaseRuntimeDeclSlots(NodeId node) {
    // T-270d: a committed offset/count pair that points at a released slot is reset to `none`
    // (the runtime bit cleared, count 0) right here, whatever runs next.
    auto forget = [](const RuntimeDeclSlot& slot, uint32_t& offset, uint32_t& count) {
        if (count != 0 && (offset & kRuntimeEffectOffsetBit) != 0 &&
            (offset & ~kRuntimeEffectOffsetBit) == slot.offset) {
            offset = 0;
            count = 0;
        }
    };
    ComputedStyle& style = m_arena.element(node).style;
    auto release = [node](std::unordered_map<NodeId, RuntimeDeclSlots>& owners,
                          std::vector<RuntimeDeclSlot>& freeList, auto&& onSlot) {
        auto own = owners.find(node);
        if (own == owners.end()) {
            return;
        }
        for (const RuntimeDeclSlot& slot : own->second.slot) {
            if (slot.cap != 0) {
                onSlot(slot);
                freeList.push_back(slot);
            }
        }
        owners.erase(own);
    };
    release(m_runtimeGradientSlots, m_freeGradientSlots,
            [&](const RuntimeDeclSlot& slot) { forget(slot, style.gradientOffset, style.gradientCount); });
    release(m_runtimeClipShapeSlots, m_freeClipShapeSlots, [&](const RuntimeDeclSlot& slot) {  // T-332.
        if ((style.clipShape & kRuntimeEffectOffsetBit) != 0 &&
            (style.clipShape & ~kRuntimeEffectOffsetBit) == slot.offset) {
            style.clipShape = 0;
        }
    });
    release(m_runtimeAnimationSlots, m_freeAnimationSlots, [&](const RuntimeDeclSlot& slot) {
        forget(slot, style.animationOffset, style.animationCount);
        forget(slot, style.lastAnimationOffset, style.lastAnimationCount);
    });
}

void Document::addWarning(std::string message) { m_warnings.push_back(std::move(message)); }

void Document::setTokenLayers(const TokenMap* theme, const TokenMap* overrides) {
    m_styleEngine.setTokenLayers(theme, overrides);
    m_styleEngine.markTokenUsersDirty(m_arena);
}

StringId Document::localizedCurrent(const LocalizedSlot& slot) const {
    if (!m_arena.alive(slot.node) || m_arena.generation(slot.node) != slot.generation) {
        return kInvalidString;
    }
    if (slot.attrIndex == 0xFFFFFFFFu) {
        return m_arena.kind(slot.node) == NodeKind::Text ? m_arena.text(slot.node).text : kInvalidString;
    }
    if (m_arena.kind(slot.node) != NodeKind::Element) {
        return kInvalidString;
    }
    const ElementData& data = m_arena.element(slot.node);
    return slot.attrIndex < data.attributes.size() ? data.attributes[slot.attrIndex].second : kInvalidString;
}

void Document::setLocalization(ILocalizationProvider* provider, std::string_view lang) {
    DispatchGuard dispatch(m_context);  // T-260a: `localize` bindings may set states (triggers).
    m_l10nProvider = provider;          // T-175: `localize` binding steps.
    // 1. Every still-untouched `@key` place shows the provider's text, or the key itself.
    for (LocalizedSlot& slot : m_localized) {
        const StringId current = localizedCurrent(slot);
        if (current == kInvalidString || current != slot.shown) {
            continue;  // Gone, or the host replaced it (setText/setAttribute).
        }
        const std::string_view key = m_ir.string(slot.key);
        std::string_view text = provider != nullptr ? provider->resolve(key, lang) : std::string_view{};
        if (text.empty()) {
            text = key;
        }
        const StringId sid = intern(text);
        slot.shown = sid;
        if (sid == current) {
            continue;
        }
        if (slot.attrIndex == 0xFFFFFFFFu) {
            TextNodeData& data = m_arena.text(slot.node);
            data.text = sid;
            data.shapedValid = 0;
            m_arena.markDirty(slot.node, static_cast<uint8_t>(kDirtyLayout));
            const NodeId parent = m_arena.parent(slot.node);
            if (parent != kInvalidNode) {
                m_arena.markDirty(parent, static_cast<uint8_t>(kDirtyLayout));
            }
        } else {
            ElementData& element = m_arena.element(slot.node);
            element.attributes[slot.attrIndex].second = sid;
            m_arena.markDirty(slot.node, static_cast<uint8_t>(kDirtyStyle));  // `[attr]` selectors.
            if (m_ir.string(element.attributes[slot.attrIndex].first) == "placeholder") {
                m_arena.markDirty(slot.node, static_cast<uint8_t>(kDirtyLayout));  // T-395: relaid out.
            }
        }
    }

    refreshUnresolvedPlaceholders();  // T-397 (08 §8.16).

    // 2. A new language: `:lang()` and the `@font-fallback` chain may change for every element.
    if (lang == m_language) {
        m_bindings->relocalize();
        return;
    }
    m_language.assign(lang);
    m_bindings->relocalize();  // T-175: `localize` steps follow the language.
    m_styleEngine.setDocumentLanguage(m_language);
    m_layoutEngine.setLanguage(m_language);
    if (m_root == kInvalidNode) {
        return;
    }
    m_arena.traversePreorder(m_root, [this](NodeId n) {
        if (m_arena.kind(n) == NodeKind::Element) {
            m_arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
            m_arena.markDirty(n, static_cast<uint8_t>(kDirtyLayout));
        } else if (m_arena.kind(n) == NodeKind::Text) {
            m_arena.text(n).shapedValid = 0;
            m_arena.markDirty(n, static_cast<uint8_t>(kDirtyLayout));
        }
        return true;
    });
}

void Document::refreshUnresolvedPlaceholders() {
    // T-397 (08 §8.16): an untouched `placeholder="@key"` still showing the key itself (no
    // provider, or the provider has no text for it) is not drawn. Keyed by the shown StringId, so
    // a `<repeat>` instance's copy of the attribute is covered too.
    m_unresolvedPlaceholders.clear();
    for (const LocalizedSlot& slot : m_localized) {
        if (slot.attrIndex == 0xFFFFFFFFu) {
            continue;
        }
        const StringId current = localizedCurrent(slot);
        if (current == kInvalidString || current != slot.shown) {
            continue;
        }
        const ElementData& element = m_arena.element(slot.node);
        if (string(element.attributes[slot.attrIndex].first) == "placeholder" &&
            string(current) == m_ir.string(slot.key)) {
            m_unresolvedPlaceholders.push_back(current);
        }
    }
    std::sort(m_unresolvedPlaceholders.begin(), m_unresolvedPlaceholders.end());
    m_unresolvedPlaceholders.erase(
        std::unique(m_unresolvedPlaceholders.begin(), m_unresolvedPlaceholders.end()),
        m_unresolvedPlaceholders.end());
}

std::string_view Document::localizationKey(ElementRef element) const {
    if (!element.valid() || element.doc != this) {
        return std::string_view{};
    }
    for (const LocalizedSlot& slot : m_localized) {
        if (slot.attrIndex != 0xFFFFFFFFu || m_arena.parent(slot.node) != element.node) {
            continue;
        }
        const StringId current = localizedCurrent(slot);
        if (current != kInvalidString && current == slot.shown) {
            return m_ir.string(slot.key);
        }
    }
    return std::string_view{};
}

ElementRef Document::createElement(std::string_view tag) {
    NodeId node = m_arena.create(NodeKind::Element);
    ElementData& data = m_arena.element(node);
    data.tag = intern(tag);
    // T-33's static focusability half; `tabindex` can only arrive later, through setAttribute().
    data.focusable = (tag == "button" || tag == "input") ? 1 : 0;
    // Detached: no dirty bits until appendChild()/insertBefore() attaches it (StyleEngine::resolve
    // would otherwise resolve it as a parentless root).
    m_arena.clearDirty(node);
    return ElementRef{this, node, m_arena.generation(node)};
}

void Document::appendChild(ElementRef parent, ElementRef child) { insertBefore(parent, child, ElementRef{}); }

namespace {

/// T-255b: the depth of `node` below its tree's root (attached or detached; the root is 0).
uint32_t nodeDepth(const NodeArena& arena, NodeId node) {
    uint32_t depth = 0;
    for (NodeId p = arena.parent(node); p != kInvalidNode; p = arena.parent(p)) {
        ++depth;
    }
    return depth;
}

/// T-255b: the depth of the deepest node of `root`'s subtree below `root` (0 for a leaf), walked
/// without recursion.
uint32_t subtreeHeight(const NodeArena& arena, NodeId root) {
    uint32_t height = 0;
    uint32_t depth = 0;
    NodeId n = root;
    for (;;) {
        NodeId c = arena.firstChild(n);
        if (c != kInvalidNode) {
            n = c;
            height = std::max(height, ++depth);
            continue;
        }
        while (n != root && arena.nextSibling(n) == kInvalidNode) {
            n = arena.parent(n);
            --depth;
        }
        if (n == root) {
            return height;
        }
        n = arena.nextSibling(n);
    }
}

}  // namespace

void Document::insertBefore(ElementRef parent, ElementRef child, ElementRef before) {
    if (!parent.valid() || parent.doc != this || !child.valid() || child.doc != this) {
        return;
    }
    if (m_arena.parent(child.node) != kInvalidNode || child.node == parent.node) {
        return;  // 13.2: the child must be detached.
    }
    NodeId beforeNode = kInvalidNode;
    if (before.valid() && before.doc == this) {
        if (m_arena.parent(before.node) != parent.node) {
            return;
        }
        beforeNode = before.node;
    }
    // T-255b: layout, style and hit testing recurse once per level; no insert makes a tree as deep
    // as kMaxElementDepth (the bound IrView::validate puts on a loaded tree).
    if (uint64_t{nodeDepth(m_arena, parent.node)} + 1u + subtreeHeight(m_arena, child.node) >=
        kMaxElementDepth) {
        return;
    }
    m_arena.insertBefore(parent.node, child.node, beforeNode);  // Marks parent kDirtyTree|kDirtyLayout.
    m_arena.markDirty(child.node, static_cast<uint8_t>(kDirtyStyle));
    m_arena.markDirty(child.node, static_cast<uint8_t>(kDirtyLayout));
    m_arena.traversePreorder(child.node, [this](NodeId n) {
        m_arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
    markSiblingsStyleDirty(parent.node);
}

void Document::markSiblingsStyleDirty(NodeId parent) {
    // T-173 (05.9.2): a child added or removed shifts its siblings' `:nth-child`/`:first-child`/
    // `:last-child` positions; re-resolve every Element child of `parent` with its subtree (a
    // structural pseudo-class on an ancestor compound reaches the descendants too).
    if (!m_styleEngine.usesStructuralSelectors() || parent == kInvalidNode || !m_arena.alive(parent)) {
        return;
    }
    for (NodeId c = m_arena.firstChild(parent); c != kInvalidNode; c = m_arena.nextSibling(c)) {
        if (m_arena.kind(c) != NodeKind::Element) {
            continue;
        }
        m_arena.markDirty(c, static_cast<uint8_t>(kDirtyStyle));
        m_arena.traversePreorder(c, [this](NodeId n) {
            m_arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
            return true;
        });
    }
}

void Document::cleanupNodeForRemoval(NodeId node) {
    // Animator state keyed on this NodeId would otherwise outlive it and be applied to whatever
    // node reuses the slot (Animator::evaluate asserts on a Free slot).
    m_animator.clearNode(node);
    forgetTimelineNode(node);      // T-172: group members / sync clips on this node.
    m_triggers->forgetNode(node);  // T-174: waiting `then` chains of this node.
    m_hidingElements.erase(std::remove(m_hidingElements.begin(), m_hidingElements.end(), node),
                           m_hidingElements.end());
    m_hiddenElements.erase(std::remove(m_hiddenElements.begin(), m_hiddenElements.end(), node),
                           m_hiddenElements.end());
    for (size_t i = m_animInstances.size(); i > 0; --i) {
        AnimInstanceInfo& info = m_animInstances[i - 1];
        if (info.node != node) {
            continue;
        }
        m_animator.stop(info.id);
        m_animInstances.erase(m_animInstances.begin() + static_cast<ptrdiff_t>(i - 1));
    }
    if (m_arena.kind(node) == NodeKind::Text) {
        // T-33: the EditState slot is emptied but not reclaimed -- editIndex values are indices
        // into m_editStates and erasing would shift every later one (see the T-35 report).
        uint32_t index = m_arena.text(node).editIndex;
        if (index < m_editStates->size()) {
            (*m_editStates)[index] = EditState{};
        }
        releaseRuntimeText(node);  // T-270a: its setText() slot goes back to the free list.
        return;
    }
    if (m_arena.kind(node) != NodeKind::Element) {
        return;
    }
    m_events->removeListenersFor(m_arena, node);
    releaseRuntimeEffects(node);    // T-270a: its setStyle `effect` segment goes back to the pool.
    releaseRuntimeDeclSlots(node);  // T-270c: its gradient / animation slots too, and its own
    releaseNodeTexts(node);         // text slots (setStyle text values, a `<repeat>` instance id).
    ElementData& data = m_arena.element(node);
    // T-270e: nothing keeps pointing at the released slots (a recycled `<repeat>` instance stays
    // alive until its reuse); the capacity stays, so this allocates nothing.
    data.runtimeDecls.clear();
    data.setStyleHash = 0;  // T-389.
    for (uint32_t id : data.cssAnimations) {
        m_animator.stop(id);
    }
    data.cssAnimations.clear();
    if (data.defaultAnimation != 0) {
        m_animator.stop(data.defaultAnimation);
        data.defaultAnimation = 0;
    }
    if (data.scene != kInvalidScene && data.sceneInstanceIndex < m_instances.size()) {
        // Destroys the instantiated scene subtree (a child of this element) and resets the pool
        // entry; the slot itself stays (indices are stored in ElementData::sceneInstanceIndex).
        destroySceneInstance(m_arena, m_instances[data.sceneInstanceIndex]);
        data.scene = kInvalidScene;
        data.sceneRoot = kInvalidNode;
        data.sceneInstanceIndex = 0xFFFFFFFFu;
    }
}

void Document::remove(ElementRef element) {
    DispatchGuard dispatch(m_context);  // T-260a: Blur / Leave listeners run below.
    if (!element.valid() || element.doc != this) {
        return;
    }
    NodeId node = element.node;
    // Focus/hover/active first: their Blur/Leave listeners run while the subtree is still intact.
    m_events->clearForSubtreeRemoval(m_arena, node);
    if (!element.valid()) {
        return;  // T-260b: one of those listeners removed it already.
    }
    // T-260b: what keeps raw NodeIds into the subtree forgets them while it is still intact: the
    // `<repeat>` instances and repeats (their bindings, trigger nodes, pools) and the rigs whose
    // skeleton -- or skinned shape -- goes with it (B9).
    m_repeats->onSubtreeRemoved(node);
    if (!m_rigs->empty()) {
        auto inSubtree = [this, node](NodeId n) {
            return n != kInvalidNode && m_arena.alive(n) && (n == node || m_arena.isAncestorOf(node, n));
        };
        for (size_t i = m_rigs->size(); i > 0; --i) {
            rig::RigState& rs = (*m_rigs)[i - 1];
            if (inSubtree(rs.skeleton)) {
                m_rigs->erase(m_rigs->begin() + static_cast<ptrdiff_t>(i - 1));
                continue;
            }
            rs.skins.erase(
                std::remove_if(rs.skins.begin(), rs.skins.end(),
                               [&inSubtree](const rig::SkinTarget& t) { return inSubtree(t.node); }),
                rs.skins.end());
        }
    }

    // Collect the subtree into our own buffer: cleanupNodeForRemoval() destroys scene subtrees and
    // marks nodes dirty, both of which reuse NodeArena's (non-re-entrant) traversal scratch.
    m_removeScratch.clear();
    m_arena.traversePostorder(node, [this](NodeId n) { m_removeScratch.push_back(n); });
    for (NodeId n : m_removeScratch) {
        if (m_arena.alive(n)) {
            cleanupNodeForRemoval(n);
        }
    }
    if (node == m_root) {
        m_root = kInvalidNode;
    }
    for (NodeId& docNode : m_docElementNodes) {
        if (docNode != kInvalidNode && (docNode == node || m_arena.isAncestorOf(node, docNode))) {
            docNode = kInvalidNode;  // elementAt()/byId() must not hand out a destroyed node.
        }
    }
    NodeId parent = m_arena.parent(node);
    m_arena.destroy(node);
    markSiblingsStyleDirty(parent);  // T-173.
}

uint32_t Document::play(const PlayParams& params) {
    uint32_t id = m_animator.play(params, m_clock);
    if (id != 0 && params.selfNode != kInvalidNode) {
        m_animInstances.push_back(AnimInstanceInfo{id, params.selfNode, AnimSource::Api});
    }
    return id;
}

void Document::stop(uint32_t id) { m_animator.stop(id); }

void Document::pause(uint32_t id) { m_animator.pause(id, m_clock); }

void Document::resume(uint32_t id) { m_animator.resume(id, m_clock); }

void Document::seek(uint32_t id, float localT) { m_animator.seek(id, localT, m_clock); }

void Document::setSpeed(uint32_t id, float speed) { m_animator.setSpeed(id, speed, m_clock); }

bool Document::isPlaying(uint32_t id) const { return m_animator.isActive(id); }

float Document::animationTime(uint32_t id) const { return m_animator.localTime(id, m_clock); }

float Document::animationTimelineDuration(uint32_t id) const { return m_animator.segmentDuration(id); }

float Document::animationSpeed(uint32_t id) const { return m_animator.speedOf(id); }

void Document::markUnloading() {
    m_unloading = true;
    m_events->muteListeners();
}

void Document::onAnimationFinished(uint32_t id, std::function<void()> callback) {
    m_finishedCallbacks.emplace_back(id, std::move(callback));
}

void Document::handleAnimationFinished(uint32_t id) {
    for (size_t i = 0; i < m_finishedCallbacks.size(); ++i) {
        if (m_finishedCallbacks[i].first == id) {
            std::function<void()> callback = std::move(m_finishedCallbacks[i].second);
            m_finishedCallbacks.erase(m_finishedCallbacks.begin() + static_cast<ptrdiff_t>(i));
            if (callback && !m_unloading) {  // T-260a: an unloading Document calls no host.
                callback();
            }
            break;
        }
    }
    // 08.6: a CSS-`animation`- or autoplay-created instance also fires a bubbling AnimationEnd on
    // its element; an Api-sourced one (Docs/design/13-api.md §13.2) only uses the callback above.
    for (size_t i = 0; i < m_animInstances.size(); ++i) {
        if (m_animInstances[i].id == id) {
            AnimInstanceInfo info = m_animInstances[i];
            m_animInstances.erase(m_animInstances.begin() + static_cast<ptrdiff_t>(i));
            if (info.source != AnimSource::Api) {
                dispatchAnimationEvent(EventType::AnimationEnd, info.node, id, kInvalidString);
            }
            // §19.3: the finished instance may have been forcing is3D = 1 on a PropId 16-22
            // Track (Animator::evaluate); the instance is already gone from the Animator by the
            // time onFinished fires, so it no longer knows which props it touched -- re-resolving
            // style is cheap and idempotent, so mark unconditionally rather than tracking props
            // separately (Docs/tasks/T-43c.md).
            if (m_arena.alive(info.node) && m_arena.kind(info.node) == NodeKind::Element) {
                m_arena.markDirty(info.node, static_cast<uint8_t>(kDirtyStyle));
            }
            break;
        }
    }
}

void Document::handleAnimationIteration(uint32_t id, uint32_t iteration) {
    // 08.6 only carves out an Api-vs-CSS distinction for onFinished (see handleAnimationFinished);
    // AnimationIteration/Marker/TransitionEnd dispatch for every tracked source alike.
    (void)iteration;
    for (const AnimInstanceInfo& info : m_animInstances) {
        if (info.id == id) {
            dispatchAnimationEvent(EventType::AnimationIteration, info.node, id, kInvalidString);
            return;
        }
    }
}

void Document::handleAnimationMarker(uint32_t id, StringId marker) {
    for (const AnimInstanceInfo& info : m_animInstances) {
        if (info.id == id) {
            dispatchAnimationEvent(EventType::Marker, info.node, id, marker);
            return;
        }
    }
}

void Document::handleTransitionEnd(NodeId node, PropId prop) {
    // §19.3: a finished transition on an Element's PropId 16-22 may have been forcing is3D = 1
    // (Animator::evaluate) -- re-resolving style restores it from the base cascade value
    // (Docs/tasks/T-43c.md).
    if (prop >= prop::TranslateZ && prop <= prop::Orientation && m_arena.alive(node) &&
        m_arena.kind(node) == NodeKind::Element) {
        m_arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    }
    dispatchAnimationEvent(EventType::TransitionEnd, node, 0, kInvalidString);
}

void Document::dispatchAnimationEvent(EventType type, NodeId node, uint32_t animationId, StringId marker) {
    // T-260a: an unloading Document delivers no animation event (AnimationEnd / Iteration /
    // Marker / TransitionEnd) at all.
    if (m_unloading || !m_arena.alive(node) || m_arena.kind(node) != NodeKind::Element) {
        return;
    }
    Event ev;
    ev.type = type;
    ev.target = node;
    ev.animationId = animationId;
    ev.markerName = marker;
    m_events->dispatch(m_arena, ev);
}

uint32_t Document::defaultAnimation(ElementRef anim) const {
    if (!anim.valid() || anim.doc != this || m_arena.kind(anim.node) != NodeKind::Element) {
        return 0;
    }
    return m_arena.element(anim.node).defaultAnimation;
}

std::vector<uint32_t> Document::animationsOf(ElementRef element) const {
    if (!element.valid() || element.doc != this || m_arena.kind(element.node) != NodeKind::Element) {
        return {};
    }
    return m_arena.element(element.node).cssAnimations;
}

const SceneInstance* Document::sceneInstance(ElementRef anim) const {
    if (!anim.valid() || anim.doc != this || m_arena.kind(anim.node) != NodeKind::Element) {
        return nullptr;
    }
    const ElementData& data = m_arena.element(anim.node);
    if (data.scene == kInvalidScene || data.sceneInstanceIndex >= m_instances.size()) {
        return nullptr;
    }
    return &m_instances[data.sceneInstanceIndex];
}

TimelineId Document::sceneTimeline(SceneId scene) const {
    if (scene == kInvalidScene || scene >= m_ir.timelineCount()) {
        return kInvalidTimeline;
    }
    return static_cast<TimelineId>(scene);
}

uint32_t Document::addListener(ElementRef target, EventType type, EventListener listener) {
    if (!target.valid() || target.doc != this) {
        return kInvalidListener;
    }
    return m_events->addListener(m_arena, target.node, type, std::move(listener));
}

void Document::removeListener(uint32_t id) { m_events->removeListener(m_arena, id); }

void Document::dispatchInput(const InputEvent& event) {
    DispatchGuard dispatch(m_context);  // T-260a: listeners and triggers run below.
    if (m_root == kInvalidNode || !m_surfaceVisible) {
        return;  // T-174: a hidden surface takes no input.
    }
    // T-389 (27 §27.4.1): the continuous-event triggers held back so far (flushContinuous()) run
    // before any other input, so a drag's last move still sees the press it belongs to.
    if (event.kind != InputEvent::PointerMove && event.kind != InputEvent::Wheel &&
        event.kind != InputEvent::ExternalDragMove) {  // T-393: a move like PointerMove.
        m_triggers->flushContinuous();
    }
    // T-174: triggers fired by this event run once its default actions are done.
    m_triggers->beginDefer();
    m_events->setTime(m_clock);  // T-194: gamepad repeat / long press.
    // T-379 (08.10): the document's `key-activation`, unless the Context turned it off.
    m_events->setKeyboardActivation(m_keyActivation &&
                                    (m_context == nullptr || m_context->keyboardActivation()));
    m_events->handleInput(m_arena, m_root, event);
    m_triggers->endDefer();
}

NodeId Document::hitTest(Vec2 pos) const {
    if (m_root == kInvalidNode) {
        return kInvalidNode;
    }
    return xt::hitTest(m_arena, m_root, pos);
}

NodeId Document::hitTestPress(Vec2 pos) const {
    if (m_root == kInvalidNode) {
        return kInvalidNode;
    }
    return xt::hitTest(m_arena, m_root, pos, HitMode::Press);
}

Status Document::setNode3D(NodeId node, bool on) {
    if (!m_arena.alive(node)) {
        return Status::kInvalidArgument;
    }
    if (m_arena.kind(node) == NodeKind::Element) {
        return Status::kInvalidArgument;
    }
    NodeCommon& common = m_arena.common(node);
    bool was = common.is3D != 0;
    common.is3D = on ? 1 : 0;
    m_arena.markDirty(node, static_cast<uint8_t>(kDirtyTransform | kDirtyTree));
    if (was != on) {
        // §19.3: a descendant's is3D can flip an ancestor scene Group's isSpaceRoot (Docs/tasks/
        // T-43c.md "T-43b 遺留修正" 2) -- invalidate the cache up to the nearest existing root.
        m_arena.markDirtyAncestors(node, static_cast<uint8_t>(kDirtyTree),
                                   nearestSpaceRootOrDocRoot(m_arena, node));
    }
    return Status::kOk;
}

NodeId Document::cameraOf(NodeId root) const {
    if (!m_arena.alive(root)) {
        return kInvalidNode;
    }
    return findCameraChild(m_arena, root);
}

Rect Document::spaceRootBox(NodeId root) const {
    if (m_arena.kind(root) == NodeKind::Element) {
        const LayoutBox& box = m_arena.element(root).box;
        return Rect{0, 0, box.width, box.height};
    }
    if (m_arena.kind(root) == NodeKind::Group) {
        NodeId p = m_arena.parent(root);
        if (p != kInvalidNode && m_arena.kind(p) == NodeKind::Element) {
            const ElementData& pdata = m_arena.element(p);
            if (pdata.scene != kInvalidScene) {
                const SceneChunk& scene = m_ir.scene(pdata.scene);
                return Rect{0, 0, scene.width(), scene.height()};
            }
        }
    }
    return Rect{};
}

Mat4 Document::viewProjFor(NodeId root) const {
    NodeId cam = findCameraChild(m_arena, root);
    Rect box = spaceRootBox(root);
    if (cam != kInvalidNode) {
        return computeCameraViewProj(m_arena, cam, box).viewProj;
    }
    if (m_arena.kind(root) == NodeKind::Element) {
        const ElementData& data = m_arena.element(root);
        return cssPerspectiveViewProj(data.perspectivePx, data.perspectiveOriginPx).viewProj;
    }
    return defaultSceneViewProj(box.w, box.h).viewProj;
}

namespace {
/// The Mat4 ("M" in §19.7's `project(viewProj * M * ...)`) that `n`'s own local content sits on:
/// `n`'s own world3 when `n` itself is a (non-root) 3D layer, otherwise the nearest is3D
/// (non-root) ancestor's world3, or identity when `n` sits directly on a root's plane or `n` is
/// not inside any root.
Mat4 planeWorld3For(const NodeArena& arena, NodeId n) {
    const NodeCommon& self = arena.common(n);
    // A space root is never a 3D layer of its own (world_transform.cpp lets `asRoot` win over
    // `as3DLayer`, so its transform lives in `world`, not `world3`): in its *outer* context it
    // behaves like any plain 2D node, so its plane comes from the same ancestor walk rather than
    // being identity (T-79 item 3). Only a non-root 3D layer contributes its own world3.
    if (self.isSpaceRoot == 0 && self.is3D != 0) {
        return self.world3;
    }
    NodeId cur = arena.parent(n);
    while (cur != kInvalidNode) {
        const NodeCommon& c = arena.common(cur);
        if (c.isSpaceRoot != 0) {
            return mat4Identity();
        }
        if (c.is3D != 0) {
            return c.world3;
        }
        cur = arena.parent(cur);
    }
    return mat4Identity();
}
}  // namespace

Vec2 Document::localPosIn(NodeId element, Vec2 rootPos) const {
    if (!m_arena.alive(element) || m_arena.kind(element) != NodeKind::Element) {
        return Vec2{};
    }
    Mat2x3 inv{};
    if (!invert(m_arena.common(element).world, inv)) {
        return Vec2{};  // Singular transform (hit_test.cpp's tryEnter skips such a node too).
    }
    return apply(inv, rootPos);
}

Vec2 Document::fractionIn(NodeId element, Vec2 rootPos) const {
    if (!m_arena.alive(element) || m_arena.kind(element) != NodeKind::Element) {
        return Vec2{};
    }
    const LayoutBox& box = m_arena.element(element).box;
    const Vec2 local = localPosIn(element, rootPos);
    const auto axis = [](float v, float size) {
        return size > 0.0f ? std::clamp(v / size, 0.0f, 1.0f) : 0.0f;
    };
    return Vec2{axis(local.x, box.width), axis(local.y, box.height)};
}

Vec2 Document::nodeToScreen(NodeId node, Vec2 localPoint) const {
    if (!m_arena.alive(node)) {
        return localPoint;
    }
    Mat2x3 xf = m_arena.common(node).world;
    // Roots *containing* `node`, innermost first -- the walk starts at the parent, so `node` itself
    // is excluded even when it is a space root (T-79 item 3). A root's own box does not sit inside
    // the space it introduces: it lies on its parent's plane, and its `world` (already applied as
    // `xf` above) is exactly the `rootXf` its own projection would use, so including it here mapped
    // the point through that transform twice.
    // T-477: with no root on the way up nothing is built (an MSVC debug std::vector allocates even
    // when empty; the flow reads `#document.pointer*` through here every tick).
    bool anyRoot = false;
    for (NodeId cur = m_arena.parent(node); cur != kInvalidNode && !anyRoot; cur = m_arena.parent(cur)) {
        anyRoot = m_arena.common(cur).isSpaceRoot != 0;
    }
    if (!anyRoot) {
        return apply(xf, localPoint);  // §19.5: "不在根子樹內 -> world * p".
    }
    std::vector<NodeId> roots;
    for (NodeId cur = m_arena.parent(node); cur != kInvalidNode; cur = m_arena.parent(cur)) {
        if (m_arena.common(cur).isSpaceRoot != 0) {
            roots.push_back(cur);
        }
    }
    std::vector<project3d::Mapping> innermostFirst;
    innermostFirst.reserve(roots.size());
    NodeId content = node;
    for (NodeId r : roots) {
        project3d::Mapping m;
        m.hasProjection = true;
        m.rootXf = m_arena.common(r).world;
        m.planeWorld3 = planeWorld3For(m_arena, content);
        m.viewProj = viewProjFor(r);
        innermostFirst.push_back(m);
        content = r;
    }
    std::vector<project3d::Mapping> stack(innermostFirst.rbegin(), innermostFirst.rend());
    bool ok = false;
    Vec2 result = project3d::projectPoint(stack, xf, localPoint, ok);
    return ok ? result : apply(xf, localPoint);
}

Vec2 Document::nodeToScreen3(NodeId node, Vec3 localPoint) const {
    if (!m_arena.alive(node)) {
        return Vec2{localPoint.x, localPoint.y};
    }
    if (m_arena.common(node).is3D == 0) {
        // §19.3: a 2D node's local Z has no meaning; fall back to the plain 2D projection.
        return nodeToScreen(node, Vec2{localPoint.x, localPoint.y});
    }
    Mat2x3 xf = m_arena.common(node).world;
    std::vector<NodeId> roots;  // Excludes `node` itself; see nodeToScreen (T-79 item 3).
    for (NodeId cur = m_arena.parent(node); cur != kInvalidNode; cur = m_arena.parent(cur)) {
        if (m_arena.common(cur).isSpaceRoot != 0) {
            roots.push_back(cur);
        }
    }
    if (roots.empty()) {
        // is3D without a space root ancestor shouldn't happen (§19.3), but stay conservative.
        return apply(xf, Vec2{localPoint.x, localPoint.y});
    }
    std::vector<project3d::Mapping> innermostFirst;
    innermostFirst.reserve(roots.size());
    NodeId content = node;
    for (NodeId r : roots) {
        project3d::Mapping m;
        m.hasProjection = true;
        m.rootXf = m_arena.common(r).world;
        m.planeWorld3 = planeWorld3For(m_arena, content);
        m.viewProj = viewProjFor(r);
        innermostFirst.push_back(m);
        content = r;
    }
    // Innermost level: embed localPoint.z (this node's own local3 point) instead of the usual z=0
    // plane point (§19.2, §19.7).
    const project3d::Mapping& inner = innermostFirst.back();
    Vec2 q = apply(xf, Vec2{localPoint.x, localPoint.y});
    Mat4 innerM = mat4Mul(inner.viewProj, inner.planeWorld3);
    float w = 0;
    Vec3 r = mat4Project(innerM, Vec3{q.x, q.y, localPoint.z}, &w);
    if (w <= 1e-6f) {
        return apply(xf, Vec2{localPoint.x, localPoint.y});  // Same conservative fallback as nodeToScreen.
    }
    Vec2 rootPoint = apply(inner.rootXf, Vec2{r.x, r.y});
    if (innermostFirst.size() == 1) {
        return rootPoint;
    }
    // Remaining (outer) levels: ordinary 2D projection through project3d::projectPoint, outermost
    // first, since `rootPoint` is already flattened to the innermost root's own plane.
    std::vector<project3d::Mapping> outerStack(innermostFirst.rbegin(), innermostFirst.rend() - 1);
    bool ok = false;
    Vec2 result = project3d::projectPoint(outerStack, Mat2x3{}, rootPoint, ok);
    return ok ? result : rootPoint;
}

Status Document::setNodeProperty(NodeId node, PropId prop, const Value& value) {
    if (!m_arena.alive(node)) {
        return Status::kInvalidArgument;
    }
    if (!propertyApplies(m_arena.kind(node), prop) || value.type != propertyType(prop)) {
        return Status::kInvalidArgument;
    }
    xt::setProperty(m_arena, node, prop, value);
    return Status::kOk;
}

void Document::setElementState(ElementRef ref, uint16_t stateBits, bool on) {
    DispatchGuard dispatch(m_context);  // T-260a: check / uncheck triggers.
    if (!ref.valid() || stateBits == 0) {
        return;
    }
    NodeArena& arena = ref.doc->arena();
    ElementData& data = arena.element(ref.node);
    uint16_t before = data.stateBits;
    data.stateBits =
        on ? static_cast<uint16_t>(before | stateBits) : static_cast<uint16_t>(before & ~stateBits);
    if (data.stateBits == before) {
        return;
    }
    // 05.8's conservative rule for class/state/style changes (mirrors element_ref.cpp's
    // markStyleDirtySubtree(), private to that translation unit): this node's own kDirtyStyle bit
    // (ancestors picked up automatically by markDirty()), plus every descendant explicitly, since
    // a forced state can flip a descendant/child selector too.
    arena.markDirty(ref.node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(ref.node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
    const uint16_t checkedBit = static_cast<uint16_t>(kStateChecked);
    if (((before ^ data.stateBits) & checkedBit) != 0) {
        checkedChanged(ref.node, (data.stateBits & checkedBit) != 0);  // T-174: check / uncheck.
    }
}

void Document::refreshCameraDefaults() {
    for (NodeId node : m_docElementNodes) {
        if (node == kInvalidNode || !m_arena.alive(node)) {
            continue;
        }
        const ElementData& data = m_arena.element(node);
        if (data.cameraNode == kInvalidNode) {
            continue;
        }
        NodeId parent = m_arena.parent(node);
        if (parent == kInvalidNode || m_arena.kind(parent) != NodeKind::Element) {
            continue;
        }
        const LayoutBox& box = m_arena.element(parent).box;
        float w = box.width;
        float h = box.height;
        NodeCommon& camCommon = m_arena.common(data.cameraNode);
        CameraData& cam = m_arena.camera(data.cameraNode);
        bool changed = false;
        if (std::isnan(camCommon.translate.x)) {
            camCommon.translate = Vec2{w * 0.5f, h * 0.5f};
            camCommon.translateZ = w;
            changed = true;
        }
        if (std::isnan(cam.target.x)) {
            cam.target = Vec3{w * 0.5f, h * 0.5f, 0};
            changed = true;
        }
        if (std::isnan(cam.zoom)) {
            cam.zoom = w;
            changed = true;
        }
        if (changed) {
            m_arena.markDirty(data.cameraNode, static_cast<uint8_t>(kDirtyTransform));
        }
    }
}

Cursor Document::cursor() const {
    NodeId h = m_events->hoverNode();
    if (!m_arena.alive(h) || m_arena.kind(h) != NodeKind::Element) {
        return Cursor::Default;
    }
    Cursor styled = m_arena.element(h).style.cursor;
    if (styled == Cursor::Default && widgetKind(m_arena, m_ir, this, h) == WidgetKind::TextInput) {
        return Cursor::Text;  // Docs/tasks/T-33.md "cursor()": only when the style says nothing.
    }
    return styled;
}

ElementRef Document::focus() const {
    NodeId node = m_events->focusNode();
    if (node == kInvalidNode || !m_arena.alive(node)) {
        return ElementRef{};
    }
    return ElementRef{const_cast<Document*>(this), node, m_arena.generation(node)};
}

void Document::setFocus(ElementRef element) {
    DispatchGuard dispatch(m_context);  // T-260a.
    NodeId node = (element.valid() && element.doc == this) ? element.node : kInvalidNode;
    m_triggers->beginDefer();  // T-174: focus/blur triggers run after the change.
    m_events->setFocus(m_arena, node);
    m_triggers->endDefer();
}

bool Document::wantsTextInput() const { return m_events->wantsTextInput(m_arena); }

// -- T-194: players, navigation, drag and drop (08.9, 27.7) ---------------------------------------

ElementRef Document::focus(uint8_t player) const {
    NodeId node = m_events->focusNode(player);
    if (node == kInvalidNode || !m_arena.alive(node)) {
        return ElementRef{};
    }
    return ElementRef{const_cast<Document*>(this), node, m_arena.generation(node)};
}

void Document::setFocus(ElementRef element, uint8_t player) {
    DispatchGuard dispatch(m_context);  // T-260a.
    if (player >= kMaxPlayers) {
        return;
    }
    NodeId node = (element.valid() && element.doc == this) ? element.node : kInvalidNode;
    m_triggers->beginDefer();
    m_events->setTime(m_clock);
    m_events->setFocus(m_arena, node, player);
    m_triggers->endDefer();
}

bool Document::navigate(NavDirection dir, uint8_t player) {
    DispatchGuard dispatch(m_context);  // T-260a.
    if (m_root == kInvalidNode || player >= kMaxPlayers) {
        return false;
    }
    m_triggers->beginDefer();
    m_events->setTime(m_clock);
    const bool moved = m_events->navigate(m_arena, m_root, dir, player);
    m_triggers->endDefer();
    return moved;
}

Status Document::setFocusTrap(ElementRef element, bool on) {
    if (!element.valid() || element.doc != this || m_arena.kind(element.node) != NodeKind::Element) {
        return Status::kInvalidArgument;
    }
    uint32_t& flags = m_arena.common(element.node).flags2;
    flags = on ? (flags | kFocusTrap) : (flags & ~static_cast<uint32_t>(kFocusTrap));
    return Status::kOk;
}

void Document::setNavRepeat(float delay, float interval) {
    if (delay > 0.0f && interval > 0.0f) {
        m_events->setNavRepeat(delay, interval);
    }
}

ElementRef Document::dragSource() const {
    const NodeId node = m_events->dragSource();
    if (node == kInvalidNode || !m_arena.alive(node)) {
        return ElementRef{};
    }
    return ElementRef{const_cast<Document*>(this), node, m_arena.generation(node)};
}

ElementRef Document::dragOver() const {
    const NodeId node = m_events->dragOver();
    if (node == kInvalidNode || !m_arena.alive(node)) {
        return ElementRef{};
    }
    return ElementRef{const_cast<Document*>(this), node, m_arena.generation(node)};
}

Vec2 Document::dragGhostOffset() const { return m_events->dragGhostOffset(m_arena); }

void Document::cancelDrag() {
    DispatchGuard dispatch(m_context);  // T-260a.
    m_triggers->beginDefer();
    m_events->cancelDrag(m_arena);
    m_triggers->endDefer();
}

void Document::cancelPointer() {
    // T-390 (08.13): the same path as a queued InputEvent::PointerCancel (T-386).
    InputEvent cancel;
    cancel.kind = InputEvent::PointerCancel;
    dispatchInput(cancel);
}

ElementRef Document::dropTargetAt(Vec2 pos) const {
    if (m_root == kInvalidNode) {
        return ElementRef{};
    }
    const NodeId node = m_events->dropTargetOf(m_arena, xt::hitTest(m_arena, m_root, pos));
    if (node == kInvalidNode) {
        return ElementRef{};
    }
    return ElementRef{const_cast<Document*>(this), node, m_arena.generation(node)};
}

uint32_t Document::itemIndexOf(ElementRef element) const {
    if (!element.valid() || element.doc != this) {
        return 0xFFFFFFFFu;
    }
    return m_repeats->itemIndexOf(element.node, nullptr);
}

AppRegion Document::appRegionAt(NodeId node) const {
    for (NodeId n = node; n != kInvalidNode && m_arena.alive(n); n = m_arena.parent(n)) {
        if (m_arena.kind(n) == NodeKind::Element && m_arena.element(n).style.appRegion != AppRegion::None) {
            return m_arena.element(n).style.appRegion;
        }
    }
    return AppRegion::None;
}

NodeId Document::contentHitTest(Vec2 pos, bool press) const {
    if (m_root == kInvalidNode || !m_surfaceVisible) {
        return kInvalidNode;
    }
    const NodeId hit = xt::hitTest(m_arena, m_root, pos, press ? HitMode::Press : HitMode::Hover);
    if (hit == m_root || hit == surfaceElement()) {
        return kInvalidNode;
    }
    return hit;
}

void Document::applyAutofocus() {
    if (m_events->focusNode(0) != kInvalidNode) {
        return;
    }
    for (NodeId node : m_autofocus) {
        if (m_arena.alive(node) && isFocusable(m_arena, node)) {
            m_triggers->beginDefer();
            m_events->setFocus(m_arena, node, 0);
            m_triggers->endDefer();
            return;
        }
    }
}

const CompositionState& Document::compositionState() const { return *m_composition; }

Rect Document::textInputRect() const {
    // T-33b "textInputRect()": the caret rect in the Text node's own coordinates, through its
    // world transform, as a viewport-space AABB.
    if (!wantsTextInput()) {
        return Rect{};
    }
    NodeId element = m_events->focusNode();
    NodeId textNode = firstTextChild(m_arena, element);
    if (textNode == kInvalidNode) {
        return Rect{};
    }
    const TextNodeData& text = m_arena.text(textNode);
    uint32_t offset = 0;
    if (m_composition->active && m_composition->element == element) {
        offset = compositionCaret(*m_composition);
    } else {
        uint32_t index = text.editIndex;
        if (index >= m_editStates->size()) {
            return Rect{};
        }
        offset = (*m_editStates)[index].cursor;
    }
    Rect local{};
    if (!caretLocalRect(text, offset, local)) {
        return Rect{};
    }
    // Docs/design/19-3d-space.md §19.10's revision to 08 ("Context::textInputRect 改用
    // nodeToScreen"): the caret rect's four corners, each projected via nodeToScreen (a plain 2D
    // document leaves this byte-identical to the old transformBounds(world, local) call, since
    // nodeToScreen falls back to apply(world, p) outside any 3D root), then the AABB.
    Vec2 corners[4] = {
        Vec2{local.x, local.y},
        Vec2{local.x + local.w, local.y},
        Vec2{local.x + local.w, local.y + local.h},
        Vec2{local.x, local.y + local.h},
    };
    Vec2 mapped[4];
    for (int i = 0; i < 4; ++i) {
        mapped[i] = nodeToScreen(textNode, corners[i]);
    }
    float minX = mapped[0].x;
    float maxX = mapped[0].x;
    float minY = mapped[0].y;
    float maxY = mapped[0].y;
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, mapped[i].x);
        maxX = std::max(maxX, mapped[i].x);
        minY = std::min(minY, mapped[i].y);
        maxY = std::max(maxY, mapped[i].y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

EditState* Document::editStateForElement(NodeId element) {
    NodeId textNode = firstTextChild(m_arena, element);
    if (textNode == kInvalidNode) {
        return nullptr;
    }
    uint32_t index = m_arena.text(textNode).editIndex;
    if (index >= m_editStates->size()) {
        return nullptr;
    }
    return &(*m_editStates)[index];
}

uint32_t Document::hitTestText(ElementRef element, Vec2 localPos) const {
    if (!element.valid() || element.doc != this || m_arena.kind(element.node) != NodeKind::Element) {
        return 0;
    }
    return hitTestTextOffset(m_arena, element.node, localPos);
}

uint32_t Document::hitTestTextLink(Vec2 pos) const {
    NodeId element = hitTest(pos);
    if (element == kInvalidNode) {
        return 0xFFFFFFFFu;
    }
    NodeId textNode = firstTextChild(m_arena, element);
    if (textNode == kInvalidNode) {
        return 0xFFFFFFFFu;
    }
    const TextNodeData& data = m_arena.text(textNode);
    if (data.styleRuns.empty()) {
        return 0xFFFFFFFFu;
    }
    Mat2x3 inv{};
    if (!invert(m_arena.common(textNode).world, inv)) {
        return 0xFFFFFFFFu;
    }
    const Vec2 local = apply(inv, pos);
    for (const GlyphRun& run : data.runs) {
        if (run.tag >= data.styleRuns.size() || !contains(run.bounds, local)) {
            continue;
        }
        return data.styleRuns[run.tag].linkIndex;
    }
    return 0xFFFFFFFFu;
}

uint32_t hitTestTextOffset(const NodeArena& arena, NodeId element, Vec2 localPos) {
    NodeId textNode = firstTextChild(arena, element);
    if (textNode == kInvalidNode) {
        return 0;
    }
    const TextNodeData& data = arena.text(textNode);
    if (data.lines.empty() || data.runs.empty()) {
        return 0;
    }
    bool vertical = data.keyWritingMode != WritingMode::HorizontalTb;

    // 1. Line (column): the one whose rect contains the cross-axis coordinate, else the nearest.
    const TextLine* line = &data.lines[0];
    float bestLineDist = -1.0f;
    for (const TextLine& candidate : data.lines) {
        float lo = vertical ? candidate.rect.x : candidate.rect.y;
        float hi = lo + (vertical ? candidate.rect.w : candidate.rect.h);
        float pos = vertical ? localPos.x : localPos.y;
        float dist = pos < lo ? (lo - pos) : (pos > hi ? (pos - hi) : 0.0f);
        if (bestLineDist < 0.0f || dist < bestLineDist) {
            bestLineDist = dist;
            line = &candidate;
        }
    }
    if (line->runCount == 0) {
        return line->byteStart;
    }

    // 2. Run within that line: containing the main-axis coordinate, else the nearest.
    const GlyphRun* run = &data.runs[line->firstRun];
    float bestRunDist = -1.0f;
    for (uint32_t i = 0; i < line->runCount; ++i) {
        const GlyphRun& candidate = data.runs[line->firstRun + i];
        float lo = vertical ? candidate.bounds.y : candidate.bounds.x;
        float hi = lo + (vertical ? candidate.bounds.h : candidate.bounds.w);
        float pos = vertical ? localPos.y : localPos.x;
        float dist = pos < lo ? (lo - pos) : (pos > hi ? (pos - hi) : 0.0f);
        if (bestRunDist < 0.0f || dist < bestRunDist) {
            bestRunDist = dist;
            run = &candidate;
        }
    }
    if (run->positions.empty()) {
        return run->byteStart;
    }

    // 3. Nearest glyph boundary inside the run, in visual order: boundary k sits at the k-th
    // glyph's origin (the run's main-axis end for k == glyphCount).
    float pos = vertical ? localPos.y : localPos.x;
    uint32_t glyphCount = static_cast<uint32_t>(run->positions.size());
    uint32_t bestBoundary = 0;
    float bestDist = -1.0f;
    for (uint32_t k = 0; k <= glyphCount; ++k) {
        float boundary;
        if (k < glyphCount) {
            boundary = vertical ? run->positions[k].y : run->positions[k].x;
        } else {
            boundary = vertical ? (run->bounds.y + run->bounds.h) : (run->bounds.x + run->bounds.w);
        }
        float dist = boundary > pos ? (boundary - pos) : (pos - boundary);
        if (bestDist < 0.0f || dist < bestDist) {
            bestDist = dist;
            bestBoundary = k;
        }
    }

    // 4. Visual boundary -> logical byte offset. GlyphRun carries no per-glyph cluster array (only
    // the run's own byte range), so the boundary is mapped linearly across the run's bytes and then
    // snapped to the nearest grapheme boundary from the cached ShapeResult -- exact whenever the
    // run's glyphs are byte-uniform (ASCII, CJK), approximate otherwise. See the T-30 report.
    uint32_t logical = ((run->bidiLevel % 2) == 1) ? (glyphCount - bestBoundary) : bestBoundary;
    uint32_t byteSpan = run->byteEnd > run->byteStart ? (run->byteEnd - run->byteStart) : 0;
    uint32_t offset = run->byteStart;
    if (glyphCount > 0 && byteSpan > 0) {
        offset += static_cast<uint32_t>((static_cast<uint64_t>(byteSpan) * logical) / glyphCount);
    }
    const std::vector<uint8_t>& graphemeStart = data.shaped.graphemeStart;
    if (offset < graphemeStart.size() && graphemeStart[offset] == 0) {
        uint32_t back = offset;
        while (back > 0 && graphemeStart[back] == 0) {
            --back;
        }
        uint32_t forward = offset;
        while (forward < graphemeStart.size() && graphemeStart[forward] == 0) {
            ++forward;
        }
        offset = (offset - back) <= (forward - offset) ? back : forward;
    }
    return offset;
}

void Document::layoutIfDirty() {
    // T-394 (08.15, 08 §8.17): the style / layout / virtualized-repeat / world-transform part of
    // update(), so geometry queries (scrollIntoView(repeatId, index), boundingBox) see changes made
    // since the last frame. Dirty bits stay set; the next update() runs its passes as usual.
    if (m_root == kInvalidNode) {
        return;
    }
    DispatchGuard dispatch(m_context);  // After-layout bindings may notify port listeners.
    ScopedLengthEnv lengthEnvScope(&m_lengthEnv);
    restoreSkippedLayout();  // T-397: a hidden surface's frames cleared the layout dirty bits.
    m_styleEngine.resolve(m_arena, m_clock);
    m_layoutEngine.layout(m_arena, m_root, m_viewport);
    const bool bindingsChanged = m_bindings->afterLayout();
    const bool repeatsChanged = m_repeats->afterLayout();
    if (bindingsChanged || repeatsChanged) {
        m_styleEngine.resolve(m_arena, m_clock);
        m_layoutEngine.layout(m_arena, m_root, m_viewport);
    }
    updateWorldTransforms(m_arena, m_root, m_forceTransform);
}

void Document::restoreSkippedLayout() {
    // T-397 (08 §8.17): update() skipped layout while the surface was hidden and still cleared the
    // dirty bits every frame, so the whole tree is laid out (and transformed) again.
    if (!m_layoutSkipped || m_root == kInvalidNode) {
        return;
    }
    m_layoutSkipped = false;
    m_arena.traversePreorder(m_root, [this](NodeId n) {
        m_arena.markDirty(n, static_cast<uint8_t>(kDirtyLayout));
        return true;
    });
    m_forceTransform = true;
}

void Document::update(float clock) {
    // T-260a: triggers, listeners and animation events run below; an unload / reload a callback
    // asks for waits until this (or the outermost dispatching call) returns.
    DispatchGuard dispatch(m_context);
    // T-389 (27 §27.4.1): what the input since the last frame held back -- the continuous-event
    // triggers (once each, with the last event) and the bindings of every port set (once each) --
    // runs first, at the clock those inputs arrived at, so a `tween` starts where it did before.
    m_triggers->flushContinuous();
    m_bindings->flushDirty();
    m_clock = clock;
    // T-174 (27.2.4): the surface appears with its first frame, so its `show` triggers fire once.
    if (!m_shownOnce) {
        m_shownOnce = true;
        if (m_surfaceVisible) {
            fireSurfaceEvent(true);  // T-349: `<body>` and its direct children.
            applyAutofocus();        // T-194 (27.7): the surface's initial focus.
        }
    }
    // T-194 (08.9): held d-pad / stick repeats and the A long press of a gamepad drag.
    if (m_root != kInvalidNode && m_surfaceVisible) {
        m_triggers->beginDefer();
        m_events->tick(m_arena, m_root, clock);
        m_triggers->endDefer();
    }
    // T-175: `tween` / `delay` binding steps advance with the clock. T-389: tick() first evaluates
    // the bindings of the ports set since the flush above (the `show` triggers, the gamepad tick).
    m_bindings->tick();
    // T-176: device-relative units and `calc()` resolve against this document's LengthEnv in the
    // style (font-size) and layout passes below.
    ScopedLengthEnv lengthEnvScope(&m_lengthEnv);
    // Docs/design/21-perf-tracing.md §21.1: the style/animate/layout/transform phase boundaries.
    // Each scope adds to the frame's field, so both style passes below land in styleMs.
    uint32_t styled = 0;
    {
        PerfScope styleScope(*m_perf, &FrameStats::styleMs);
        styled = m_styleEngine.resolve(m_arena, clock);
    }
    {
        PerfScope animateScope(*m_perf, &FrameStats::animateMs);
        // T-172 (04.10.1): advance every playing `@timeline` group's shared clock and seek its
        // members before the Animator evaluates them; End/Marker events go out afterwards.
        stepTimelineGroups(clock);
        m_repeats->step(clock);    // T-177: timelines played for one `<repeat>` instance.
        stepExternalMedia(clock);  // T-191: sequences advance, autoplay starts.
        m_animator.evaluate(clock, m_arena);
    }
    flushTimelineEvents();
    m_repeats->flushEvents();
    flushMediaEvents();  // T-191: MediaEnd, `media(<id>).end`.
    // Animation callbacks (AnimationEnd/TransitionEnd listeners, is3D restore in 19.3) may mark
    // style dirty during evaluate(); clearAllDirty() below would drop those bits, so give the
    // style engine one extra pass in the same frame (bounded: evaluate() is not re-run).
    if (m_arena.anyDirty(static_cast<uint8_t>(kDirtyStyle))) {
        PerfScope styleScope(*m_perf, &FrameStats::styleMs);
        styled += m_styleEngine.resolve(m_arena, clock);
    }
    // T-399 (08 §8.18): an input shown again (itself or an ancestor no longer `display: none`)
    // shows its `bind:value` port's value, not the user's leftover edit.
    m_bindings->reapplyShownValues();
    // T-397 (08 §8.17): a hidden surface (not one playing its `then hide` outro) keeps its
    // triggers, timelines, flow and bindings running above but skips layout and everything that
    // reads it (after-layout bindings, virtualized repeats, particles, transforms, rigs); the
    // first visible frame lays the whole tree out again (restoreSkippedLayout()).
    const bool layoutThisFrame = m_root != kInvalidNode && m_surfaceVisible;
    if (m_root != kInvalidNode && !m_surfaceVisible) {
        m_layoutSkipped = true;
        m_particleClockValid = false;  // No catch-up burst when shown again.
        m_rigClockValid = false;
    }
    if (layoutThisFrame) {
        restoreSkippedLayout();
        {
            PerfScope layoutScope(*m_perf, &FrameStats::layoutMs);
            m_layoutEngine.layout(m_arena, m_root, m_viewport);
            refreshCameraDefaults();
        }
        // T-175 (27.3.2): `#Other.<prop>` bindings read the fresh layout; what they change is
        // resolved and laid out once more in this same frame. T-177: so do the virtualized
        // `<repeat>` windows (instances created, recycled, spacers resized).
        const bool bindingsChanged = m_bindings->afterLayout();
        const bool repeatsChanged = m_repeats->afterLayout();
        if (bindingsChanged || repeatsChanged) {
            {
                PerfScope styleScope(*m_perf, &FrameStats::styleMs);
                styled += m_styleEngine.resolve(m_arena, clock);
            }
            PerfScope layoutScope(*m_perf, &FrameStats::layoutMs);
            m_layoutEngine.layout(m_arena, m_root, m_viewport);
        }
        // T-399 (08 §8.18): the focus is validated against this frame's style and layout.
        if (validateFocusAfterLayout()) {
            {
                PerfScope styleScope(*m_perf, &FrameStats::styleMs);
                styled += m_styleEngine.resolve(m_arena, clock);
            }
            PerfScope layoutScope(*m_perf, &FrameStats::layoutMs);
            m_layoutEngine.layout(m_arena, m_root, m_viewport);
        }
        if (m_layoutEngine.gridPlacementClamped() && !m_warnedGridClamp) {
            m_warnedGridClamp = true;  // T-338 (06 §6.15).
            m_warnings.emplace_back(kWarnGridPlacementClamped);
        }
        refreshExternalPlaceholders();  // T-191: needs the content boxes.
        stepParticleNodes(clock);       // T-202: emitters fill their (laid out) boxes.
        PerfScope transformScope(*m_perf, &FrameStats::transformMs);
        updateWorldTransforms(m_arena, m_root, m_forceTransform);
        // T-244 (32 §32.3): the rigs, after the after-layout bindings (a bone's world-anchor) and
        // the world transforms (constraint targets, the skeleton groups), before the dirty bits
        // (a `d` morph re-skins) are cleared below.
        evaluateRigs(clock);
    } else if (validateFocusAfterLayout()) {
        PerfScope styleScope(*m_perf, &FrameStats::styleMs);  // Hidden surface: style only.
        styled += m_styleEngine.resolve(m_arena, clock);
    }
    if (m_perf->enabled()) {
        // 21.1 group 2/3. Accumulated (+=) so several Documents in one Context add up; the
        // command-list counters are filled once per frame by Context::render().
        FrameStats& stats = m_perf->current();
        stats.styledElements += styled;
        stats.laidOutElements += layoutThisFrame ? m_layoutEngine.lastLaidOutElements() : 0u;
        stats.activeAnimations += m_animator.activeInstanceCount();
        stats.activeTransitions += m_animator.transitionCount();
        stats.arenaNodes += m_arena.aliveCount();
        stats.rigBones += m_rigBones;            // T-244
        stats.skinnedPoints += m_skinnedPoints;  // T-244
        // T-270d: the runtime string pool and the unbounded-string guard's largest count.
        stats.runtimeStrings += static_cast<uint32_t>(m_runtimeStrings.size());
        stats.maxDistinctBindingStrings =
            std::max(stats.maxDistinctBindingStrings, m_bindings->maxDistinctStrings());
    }
    // T-552 (27 §27.4.1): what this frame laid out or moved under a still pointer is hover-judged at
    // the start of the next Context::update(), before its input and its bindings.
    if (m_events->pointerIn() && m_arena.anyDirty(static_cast<uint8_t>(kDirtyLayout | kDirtyTransform))) {
        m_events->markHoverStale();
    }
    m_arena.clearAllDirty();
    m_forceTransform = false;
    // T-244 (32 §32.8): the bones' output ports, after the dirty bits are cleared so that whatever
    // a port listener changes is picked up by the next update().
    publishBonePorts();

    // Docs/design/18-effects.md §18.2: NodeArena keeps no warning list of its own; surface the
    // "pool grew" event here, once per growth (Docs/tasks/T-50.md).
    if (m_arena.effectPoolGrew()) {
        m_warnings.push_back("effect pool grew");
        m_arena.clearEffectPoolGrew();
    }
    // T-270a (B14): between frames, compact the node effect pool once more than half of a pool
    // past 256 entries is holes. The live count is only taken when a pool's size changed since the
    // last look, so a frame that assigned no new segment pays nothing.
    {
        constexpr size_t kCompactMinEntries = 256;
        const size_t effectSize = m_arena.effects().size();
        const size_t stopSize = m_arena.effectStops().size();
        if ((effectSize > kCompactMinEntries || stopSize > kCompactMinEntries) &&
            (effectSize != m_effectPoolChecked || stopSize != m_stopPoolChecked)) {
            size_t liveEffects = 0;
            size_t liveStops = 0;
            m_arena.liveEffectCounts(liveEffects, liveStops);
            if ((effectSize > kCompactMinEntries && liveEffects * 2 < effectSize) ||
                (stopSize > kCompactMinEntries && liveStops * 2 < stopSize)) {
                m_arena.compactEffects();
            }
            m_effectPoolChecked = m_arena.effects().size();
            m_stopPoolChecked = m_arena.effectStops().size();
        }
    }
    // T-211: an effect-parameter write that found no such effect was ignored; say so once.
    if (m_arena.effectMissingWritten()) {
        m_arena.clearEffectMissingWritten();
        if (!m_warnedEffectMissing) {
            m_warnedEffectMissing = true;
            m_warnings.emplace_back(kWarnEffectMissing);
        }
    }
}

bool Document::validateFocusAfterLayout() {
    // Docs/tasks/T-33.md: "焦點元素被 display:none/移除 -> 下一次 update 清焦點（發 Blur）".
    // T-399 (08 §8.18): after the style / layout pass, so an element this frame's port change
    // shows keeps (or, pending from ElementRef::focus(), receives) the focus. True when the
    // focus changed (the caller resolves and lays out once more so `:focus`
    // shows this frame and the Focus / Blur triggers' changes are not dropped with the dirty bits).
    bool dropped = false;
    m_triggers->beginDefer();
    m_events->setTime(m_clock);
    bool changed = m_events->applyPendingFocus(m_arena, dropped);
    m_triggers->endDefer();
    changed = m_events->validateFocus(m_arena) || changed;
    if (dropped && !m_warnedPendingFocus) {
        m_warnedPendingFocus = true;
        m_warnings.emplace_back("focus(): the element was still display:none after layout; focus not moved");
    }
    return changed;
}

void Document::evaluateRigs(float clock) {
    m_rigBones = 0;
    m_skinnedPoints = 0;
    if (m_rigs->empty()) {
        return;
    }
    const float dt = m_rigClockValid ? std::max(0.0f, clock - m_rigClock) : 0.0f;
    m_rigClock = clock;
    m_rigClockValid = true;
    for (rig::RigState& rs : *m_rigs) {
        if (!m_arena.alive(rs.skeleton)) {
            continue;
        }
        m_rigBones += rig::evaluate(rs, m_arena, dt);
        m_skinnedPoints += rig::updateSkins(rs, m_arena);
    }
}

void Document::publishBonePorts() {
    for (const rig::RigState& rs : *m_rigs) {
        if (!rs.evaluated || !m_arena.alive(rs.skeleton)) {
            continue;
        }
        // T-260b (B9): a bone that is gone (or whose slot another node reuses) skips the rig;
        // Document::remove() normally drops the whole rig first.
        bool bonesAlive = true;
        for (NodeId bone : rs.bones) {
            bonesAlive = bonesAlive && m_arena.alive(bone) && m_arena.kind(bone) == NodeKind::Bone;
        }
        if (!bonesAlive) {
            continue;
        }
        const Mat2x3& group = m_arena.common(rs.skeleton).world;
        for (size_t i = 0; i < rs.bones.size(); ++i) {
            if (m_arena.name(rs.bones[i]) == kInvalidString) {
                continue;
            }
            const Mat2x3 screen = mul(group, rs.world[i]);
            const float angle = std::atan2(screen.b, screen.a) * 57.29577951308232f;
            m_bindings->publishBone(rs.bones[i], Vec2{screen.e, screen.f}, angle);
        }
    }
}

void Document::resetRigSprings() {
    for (rig::RigState& rs : *m_rigs) {
        rig::resetSprings(rs);
    }
}

size_t Document::rigCount() const { return m_rigs->size(); }

RigView Document::rigAt(size_t index) const {
    if (index >= m_rigs->size()) {
        return RigView{};
    }
    return rig::makeView((*m_rigs)[index], m_arena);
}

void Document::paint(CommandList& out) { paintAs(out, 0); }

void Document::paintAs(CommandList& out, uint16_t source) {
    PerfScope paintScope(*m_perf, &FrameStats::paintMs);
    ScopedLengthEnv lengthEnvScope(&m_lengthEnv);      // T-176: border-radius etc. in device units.
    if (m_root != kInvalidNode && m_surfaceVisible) {  // T-174: a hidden surface paints nothing.
        if (m_layoutSkipped) {
            // T-397 (08 §8.17): shown after the last update() (which skipped layout while it was
            // hidden): style, layout and transforms only -- no bindings, no host callback -- so
            // this frame is not painted from stale boxes. The next update() lays out as usual.
            restoreSkippedLayout();
            m_styleEngine.resolve(m_arena, m_clock);
            m_layoutEngine.layout(m_arena, m_root, m_viewport);
            updateWorldTransforms(m_arena, m_root, m_forceTransform);
        }
        // Docs/tasks/T-33.md "Painter": the caret blink phase and the EditState pool the caret /
        // selection geometry comes from are handed to the Painter just before it runs.
        m_painter.setClock(m_clock);
        m_painter.setTime(m_clock);  // T-225: wiggle modifiers sample the document time.
        m_painter.setEditStates(m_editStates.get());
        m_painter.setComposition(m_composition.get());
        m_painter.setIr(&m_ir);                // T-178: rich-text run styles and text effects.
        m_painter.setShaders(&m_shaderTable);  // T-203: custom effect shaders.
        m_painter.setSource(source);           // T-263d: whose AssetIds the commands carry.
        // T-194 (27.7): the drag ghost follows the pointer / the gamepad focus. T-394 (08.15): a
        // source with `drag-ghost="none"` hands the Painter no ghost.
        {
            const NodeId ghost = m_events->dragSource();
            const bool noGhost =
                ghost != kInvalidNode &&
                ElementRef{this, ghost, m_arena.generation(ghost)}.attribute("drag-ghost") == "none";
            m_painter.setDragGhost(noGhost ? kInvalidNode : ghost, m_events->dragGhostOffset(m_arena));
        }
        // T-395 (08 §8.16): `--placeholder-color`, through the current token layers.
        Color placeholderColor{};
        const bool hasPlaceholderColor = m_styleEngine.tokenColor("--placeholder-color", placeholderColor);
        m_painter.setPlaceholderColor(hasPlaceholderColor ? &placeholderColor : nullptr);
        m_painter.paint(m_arena, m_root, out);
        if (m_painter.backgroundTilesCapped() && !m_warnedBackgroundTiles) {
            m_warnedBackgroundTiles = true;  // T-327 (05 §5.9.8).
            m_warnings.emplace_back(kWarnBackgroundTiles);
        }
        if (m_perf->enabled()) {
            // T-225b (21.1): accumulated like the other per-Document counters.
            m_perf->current().modifierVertices += m_painter.modifierVertices();
        }
    }
}

void Document::setViewport(Rect logical) {
    m_viewport = logical;
    if (m_root != kInvalidNode) {
        // §6.3: any element dirty -> relayout from root. A viewport resize needs a relayout even
        // when no element's own style changed, so mark the whole tree explicitly rather than
        // relying on some element already carrying kDirtyLayout.
        m_arena.traversePreorder(m_root, [this](NodeId n) {
            m_arena.markDirty(n, static_cast<uint8_t>(kDirtyLayout));
            return true;
        });
    }
    m_forceTransform = true;
    refreshLengthEnv(/*force=*/false);
}

void Document::setDevice(const DeviceInfo& device) {
    const bool inputChanged = device.input != m_device.input;
    m_device = device;
    refreshLengthEnv(/*force=*/false);
    if (inputChanged) {
        resolveGlyphs();  // T-178 (13.8.3): glyphs follow the input device.
    }
}

void Document::setGlyphProvider(IGlyphProvider* provider) {
    m_glyphProvider = provider;
    resolveGlyphs();
}

void Document::resolveGlyphs() {
    std::span<const IrAsset> assets = m_ir.assets().assets();
    for (NodeId textNode : m_glyphTextNodes) {
        if (!m_arena.alive(textNode) || m_arena.kind(textNode) != NodeKind::Text) {
            continue;
        }
        TextNodeData& text = m_arena.text(textNode);
        bool changed = false;
        for (TextInline& object : text.inlines) {
            if (object.kind != TextInlineKind::Glyph) {
                continue;
            }
            AssetId asset = kInvalidAsset;
            if (m_glyphProvider != nullptr) {
                AssetRef ref = m_glyphProvider->resolve(m_ir.string(object.action), m_device.input);
                if (!ref.name.empty()) {
                    for (uint32_t i = 0; i < assets.size(); ++i) {
                        if (assets[i].kind == 0 /* Image */ && m_ir.string(assets[i].name) == ref.name) {
                            asset = i;
                            break;
                        }
                    }
                }
            }
            if (asset != object.asset) {
                object.asset = asset;
                changed = true;
            }
        }
        if (changed) {
            text.shapedValid = 0;  // The placeholder changes between an image box and `[key]`.
            NodeId parent = m_arena.parent(textNode);
            m_arena.markDirty(parent != kInvalidNode ? parent : textNode, static_cast<uint8_t>(kDirtyLayout));
        }
    }
}

void Document::setUiScale(float scale) {
    if (!(scale > 0.0f)) {
        return;
    }
    m_uiScale = scale;
    refreshLengthEnv(/*force=*/false);
}

void Document::refreshLengthEnv(bool force) {
    LengthEnv env;
    env.viewportW = m_viewport.w;
    env.viewportH = m_viewport.h;
    env.safePx = Insets{m_device.safeArea.top * m_viewport.h, m_device.safeArea.right * m_viewport.w,
                        m_device.safeArea.bottom * m_viewport.h, m_device.safeArea.left * m_viewport.w};
    env.uiScale = m_uiScale;
    env.calcs = m_ir.calcs().calcs();
    env.calcOps = m_ir.calcs().ops();
    env.runtimeCalcs = &m_styleEngine.runtimeCalcs();
    env.runtimeCalcOps = &m_styleEngine.runtimeCalcOps();
    const bool changed = force || env.viewportW != m_lengthEnv.viewportW ||
                         env.viewportH != m_lengthEnv.viewportH || env.safePx.top != m_lengthEnv.safePx.top ||
                         env.safePx.right != m_lengthEnv.safePx.right ||
                         env.safePx.bottom != m_lengthEnv.safePx.bottom ||
                         env.safePx.left != m_lengthEnv.safePx.left || env.uiScale != m_lengthEnv.uiScale;
    m_lengthEnv = env;
    // T-345 (05 §5.9.9): every px length is resolved with the element's `ui-scale` factor, which
    // follows uiscale and the DPR: a change restyles every element.
    const bool rescaled = m_styleEngine.setScaleInputs(m_uiScale, m_device.dpr);
    if (m_root == kInvalidNode) {
        return;
    }
    if (rescaled) {
        m_arena.traversePreorder(m_root, [this](NodeId n) {
            m_arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle | kDirtyLayout));
            return true;
        });
        m_forceTransform = true;
    }
    if (changed) {
        // §6.3: every length may depend on the device now; relayout from the root, and restyle the
        // elements whose font-size/translate turned a device unit into px at style time.
        m_arena.traversePreorder(m_root, [this](NodeId n) {
            m_arena.markDirty(n, static_cast<uint8_t>(kDirtyLayout));
            return true;
        });
        m_styleEngine.markDeviceUsersDirty(m_arena);
        m_forceTransform = true;
    }
    m_styleEngine.updateMedia(m_arena, m_lengthEnv, m_device);
}

void Document::compactEffects() { m_arena.compactEffects(); }

}  // namespace xt
