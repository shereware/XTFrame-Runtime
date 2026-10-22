#include "repeat_controller.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

#include "binding_runtime.h"
#include "event_dispatcher.h"
#include "text_edit.h"
#include "timeline_group.h"
#include "trigger_runtime.h"
#include "xt/node_arena.h"
#include "xt/property.h"

/// Docs/design/27-interaction.md §27.3.4, 03 §3.12.4, 13 §13.8.9, Docs/tasks/T-177.md.
///
/// Not covered by the design/task sheet (conservative choices made here):
///   - Instances are children of the `<repeat>` element (which lays out like a `div`), in list
///     order; the template subtree itself is detached (kTemplate) and never styled or drawn.
///   - An instance element's id is `<repeat id>/<index>/<template id>` (no id when the template
///     element has none; inside a symbol instance the template id loses the instance prefix the
///     repeat id already carries, T-378); it is renamed whenever its entry index changes. `#<template id>`
///     selectors keep matching it (ElementData::templateId). Document::byId() finds instance
///     elements by their path ids.
///   - Instances are keyed by index, not by entry identity: `listSet` keeps the instances of the
///     indices that still exist and re-points their bindings (so a reordered list re-binds in
///     place and the child order always follows the list), removes the surplus without
///     `item-exit` and creates the missing ones without `item-enter`. `listInsert` creates the new
///     instance and fires its `item-enter`; `listRemove` fires `item-exit` on the removed one
///     (without an `item-exit`, or while it has no `remove`, it goes at once); `listUpdate`
///     re-evaluates that instance's bindings only. After an insert or remove every live instance's
///     bindings are re-evaluated (their `index` / `count` may have changed).
///   - An exiting instance keeps its place in the tree, is not part of repeatInstances() and its
///     bindings stop updating; a later list change never reuses it.
///   - A timeline played from inside an instance (`item-enter="play(item-in)"`, `on:hover` in the
///     template) that animates template elements plays a copy for that instance only; members
///     outside the template are ignored there, and markers / sounds of such a copy are not fired.
///   - Virtualization: the item height is the first instance's margin-box height, measured once
///     after layout; entries are laid out one after another from the repeat's content top (a
///     vertical list). The window is the visible range plus one visible count before and after;
///     a spacer `div` before and after the instances (`height` = the skipped entries' height,
///     `flex-shrink: 0`) keeps the scroll length. Until an item has been measured only entry 0 is
///     instantiated; a zero-height item instantiates every entry. Recycled instances wait detached
///     in a pool (at most 3 x the visible count; the rest are destroyed) and are reset to the
///     template's classes, attributes and states before reuse. The viewport without `viewport=`
///     is the nearest `overflow: scroll` ancestor, else the document viewport.
///   - `anim` and `<camera>` elements inside a template are copied as plain elements (their scene
///     or camera is not instantiated); `@key` text inside a template shows the template's text at
///     copy time and does not follow setLanguage().
///   - T-380 (27 §27.3.4): every instance gets its own copy of each port a symbol declares inside
///     the template (BindingRuntime::addInstancePort); its bindings, `port()` triggers and
///     `set()` / `emit()` actions use the copies. A pooled instance's copies go back to the
///     declared defaults; destroying the instance releases them.
///   - T-391 (27 §27.3.4): a `<repeat>` inside another repeat's template is a `proto` record; every
///     instance of the enclosing repeat gets a live copy of it (createInnerRepeats(), released
///     with the instance; a pooled instance releases them and makes them again when reused). A
///     copy over `item(<field>)` reads its own nested list slot (BindingRuntime::addNestedList),
///     refreshed from the enclosing entry each time the enclosing instance is evaluated (index
///     keyed, as `listSet`); a copy over a document list port expands that port. Path ids nest:
///     `Rows/2/Sub/0/Cell`. Nested copies never virtualize.

namespace xt {

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// -- Load ---------------------------------------------------------------------------------------

void RepeatController::prepare() {
    const IrView& ir = m_doc.m_ir;
    std::span<const IrDocElement> elements = ir.doc().elements();
    std::span<const IrRepeat> rows = ir.repeats().repeats();
    m_templateOf.assign(elements.size(), kNoItem);
    m_tplPos.assign(elements.size(), kNoItem);
    m_repeats.clear();
    m_repeats.resize(rows.size());
    NodeArena& arena = m_doc.m_arena;
    // The templates: each root and every later element whose parent is in it (preorder). T-391: in
    // ascending template-root order, so a nested repeat's template (inside the enclosing one's)
    // is marked last and its elements belong to the innermost template only.
    std::vector<uint32_t> order(rows.size());
    for (uint32_t ri = 0; ri < rows.size(); ++ri) {
        order[ri] = ri;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&rows](uint32_t a, uint32_t b) { return rows[a].templateRoot < rows[b].templateRoot; });
    for (uint32_t ri : order) {
        const uint32_t root = rows[ri].templateRoot;
        m_templateOf[root] = ri;
        for (uint32_t i = root + 1; i < elements.size(); ++i) {
            const uint32_t p = elements[i].parent;
            if (p >= elements.size() || (p != root && m_templateOf[p] != ri)) {
                break;
            }
            m_templateOf[i] = ri;
        }
    }
    for (uint32_t ri = 0; ri < rows.size(); ++ri) {
        Repeat& r = m_repeats[ri];
        r.ir = &rows[ri];
        r.self = ri;
        r.tplIndex = ri;
        r.element = m_doc.m_docElementNodes[r.ir->element];
        r.id = arena.element(r.element).id;
        r.virtualize = (r.ir->flags & kRepeatVirtualize) != 0;
        r.spawn = (r.ir->flags & kRepeatSpawn) != 0;
        if (r.spawn) {
            r.poolCap = kMaxSpawnInstances;
        }
        // T-391: a repeat inside another template only describes the copies its instances make.
        r.proto = r.ir->element < m_templateOf.size() && m_templateOf[r.ir->element] != kNoItem;
        r.dead = r.proto;
        r.listPort = (r.ir->flags & kRepeatItemField) != 0 ? kRepeatNoListPort : r.ir->listPort;
        if (r.proto) {
            r.virtualize = false;
        }
        {
            const std::string rid(r.id != kInvalidString ? m_doc.string(r.id) : std::string_view{});
            const size_t slash = rid.rfind('/');
            r.stripPrefix = slash == std::string::npos ? std::string() : rid.substr(0, slash + 1);
        }
        for (uint32_t i = r.ir->templateRoot; i < elements.size(); ++i) {
            if (i > r.ir->templateRoot && m_templateOf[i] == kNoItem) {
                break;  // Past the preorder range of the template (and of everything nested in it).
            }
            if (m_templateOf[i] == ri) {
                m_tplPos[i] = static_cast<uint32_t>(r.tpl.size());
                r.tpl.push_back(i);
            }
        }
        for (uint32_t element : r.tpl) {
            NodeId node = m_doc.m_docElementNodes[element];
            arena.common(node).flags2 |= kTemplate;
        }
        NodeId rootNode = m_doc.m_docElementNodes[r.ir->templateRoot];
        if (arena.parent(rootNode) != kInvalidNode) {
            arena.detach(rootNode);
        }
        std::span<const IrBinding> binds = ir.bindings().bindings();
        for (uint32_t b = 0; b < binds.size(); ++b) {
            if (binds[b].element < m_templateOf.size() && m_templateOf[binds[b].element] == ri) {
                r.tplBindings.push_back(b);
            }
        }
        // T-380 (27 §27.3.4): ports a symbol declares inside the template; one copy per instance.
        std::span<const IrPort> ports = ir.ports().ports();
        for (uint32_t p = 0; p < ports.size(); ++p) {
            if (ports[p].element < m_templateOf.size() && m_templateOf[ports[p].element] == ri) {
                r.tplPorts.push_back(p);
            }
        }
        const TriggerChunk& tc = ir.triggers();
        if (r.ir->exitTriggerActions != 0 && r.ir->exitTriggerActions <= tc.triggers().size()) {
            const IrTrigger& t = tc.triggers()[r.ir->exitTriggerActions - 1];
            for (uint32_t a = 0; a < t.actionCount; ++a) {
                r.exitRemoves = r.exitRemoves || tc.actions()[t.actionOffset + a].kind ==
                                                     static_cast<uint8_t>(TriggerAction::Remove);
            }
        }
    }
}

void RepeatController::load() {
    NodeArena& arena = m_doc.m_arena;
    const size_t rows = m_repeats.size();  // T-391: sync() below adds nested copies after these.
    for (size_t ri = 0; ri < rows; ++ri) {
        Repeat& r = m_repeats[ri];
        // Load marked every DOCT element dirty; a detached template must not be resolved as a root.
        NodeId root = m_doc.m_docElementNodes[r.ir->templateRoot];
        std::vector<NodeId> nodes;
        arena.traversePreorder(root, [&nodes](NodeId n) {
            nodes.push_back(n);
            return true;
        });
        for (NodeId n : nodes) {
            arena.clearDirty(n);
        }
        if (r.proto) {
            continue;  // T-391: copies are made per instance of the enclosing repeat.
        }
        if (r.virtualize) {
            ElementRef parent{&m_doc, r.element, arena.generation(r.element)};
            ElementRef before = m_doc.createElement("div");
            ElementRef after = m_doc.createElement("div");
            before.setStyle("flex-shrink: 0");
            after.setStyle("flex-shrink: 0");
            m_doc.appendChild(parent, before);
            m_doc.appendChild(parent, after);
            r.spacerBefore = before.node;
            r.spacerAfter = after.node;
            setSpacers(r);
        }
        if (!r.spawn) {
            sync(r, 0, r.virtualize ? 1 : count(r));
        }
    }
}

// -- Instances ----------------------------------------------------------------------------------

uint32_t RepeatController::count(const Repeat& r) const {
    if (r.spawn || r.free || r.listPort == kRepeatNoListPort) {
        return 0;  // T-201: a spawn pool has no list.
    }
    return m_doc.m_bindings->entryCount(r.listPort);
}

RepeatController::Repeat* RepeatController::repeatOfElement(NodeId element) {
    for (Repeat& r : m_repeats) {
        if (r.element == element) {
            return &r;
        }
    }
    return nullptr;
}

bool RepeatController::findInstance(NodeId node, Repeat*& repeat, Instance*& instance, bool& exiting) {
    const NodeArena& arena = m_doc.m_arena;
    if (m_repeats.empty() || node == kInvalidNode || !arena.alive(node)) {
        return false;
    }
    for (NodeId n = node; n != kInvalidNode;) {
        NodeId p = arena.parent(n);
        if (p == kInvalidNode) {
            return false;
        }
        if (Repeat* r = repeatOfElement(p)) {
            for (Instance& inst : r->live) {
                if (inst.nodes[0] == n) {
                    repeat = r;
                    instance = &inst;
                    exiting = false;
                    return true;
                }
            }
            for (Instance& inst : r->exiting) {
                if (inst.nodes[0] == n) {
                    repeat = r;
                    instance = &inst;
                    exiting = true;
                    return true;
                }
            }
        }
        n = p;
    }
    return false;
}

void RepeatController::renameInstance(Repeat& r, Instance& inst) {
    NodeArena& arena = m_doc.m_arena;
    const std::string rid(m_doc.string(r.id));  // Owned: setNodeText() below may grow the pools.
    const std::string prefix = rid + "/" + std::to_string(inst.index) + "/";
    // T-378 (27 §27.11): inside a symbol instance both the repeat id and the template ids carry
    // the instance prefix (`CList/CRow`, `CList/Track/Add`); the template-relative part drops it
    // so the path reads `CList/CRow/0/Track/Add`. A top-level repeat has no prefix.
    // T-391: a nested copy's id carries the enclosing path (`Rows/2/Sub`); the prefix the template
    // ids carry is the one of the RPT row's own id (stripPrefix).
    const std::string& instancePrefix = r.stripPrefix;
    for (NodeId n : inst.nodes) {
        if (!arena.alive(n)) {
            continue;
        }
        ElementData& d = arena.element(n);
        if (d.templateId == kInvalidString) {
            continue;
        }
        // T-270c: the id lives in the node's own text slot (overwritten when the instance is
        // pointed at another entry or reused from the pool), not in the runtime string pool; a
        // text some string table already has keeps that id (byId() compares the text either way).
        std::string_view relative = m_doc.string(d.templateId);
        if (!instancePrefix.empty() && relative.size() > instancePrefix.size() &&
            relative.substr(0, instancePrefix.size()) == std::string_view(instancePrefix)) {
            relative.remove_prefix(instancePrefix.size());
        }
        const std::string text = prefix + std::string(relative);
        if (d.id != kInvalidString && m_doc.string(d.id) == text) {
            continue;
        }
        const StringId known = m_doc.findString(text);
        d.id = known != kInvalidString ? known : m_doc.setNodeText(n, Document::kNodeTextElementId, text);
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
    }
}

RepeatController::Instance RepeatController::createInstance(Repeat& r, uint32_t index) {
    NodeArena& arena = m_doc.m_arena;
    std::span<const IrDocElement> elements = m_doc.m_ir.doc().elements();
    Instance inst;
    inst.index = index;
    inst.nodes.assign(r.tpl.size(), kInvalidNode);
    for (size_t k = 0; k < r.tpl.size(); ++k) {
        const uint32_t di = r.tpl[k];
        const NodeId src = m_doc.m_docElementNodes[di];
        const NodeId n = arena.create(NodeKind::Element);
        {
            const ElementData& s = arena.element(src);
            ElementData& d = arena.element(n);
            d.tag = s.tag;
            d.templateId = s.id;
            d.classes = s.classes;
            d.stateBits = s.stateBits;
            d.focusable = s.focusable;
            d.attributes = s.attributes;
            d.inlineStyle = s.inlineStyle;
            d.inlineDeclOffset = s.inlineDeclOffset;
            d.inlineDeclCount = s.inlineDeclCount;
            d.imageAsset = s.imageAsset;
        }
        arena.common(n).triggerTable = arena.common(src).triggerTable;
        arena.common(n).customStates = arena.common(src).customStates;  // T-385: `<use state>` seeds.
        {
            // T-194: the drag attributes and explicit navigation targets travel with the copy.
            const NodeCommon& sc = arena.common(src);
            NodeCommon& dc = arena.common(n);
            dc.flags2 |= sc.flags2 & (kDraggable | kDropTarget | kFocusTrap | kDragAxisX);
            dc.navUp = sc.navUp;
            dc.navDown = sc.navDown;
            dc.navLeft = sc.navLeft;
            dc.navRight = sc.navRight;
        }
        for (NodeId c = arena.firstChild(src); c != kInvalidNode; c = arena.nextSibling(c)) {
            if (arena.kind(c) != NodeKind::Text) {
                continue;
            }
            const NodeId t = arena.create(NodeKind::Text);
            const TextNodeData& ts = arena.text(c);
            TextNodeData& td = arena.text(t);
            td.text = ts.text;
            if (ts.text >= kRuntimeTextBase && ts.text != kInvalidString) {
                td.text = m_doc.setRuntimeText(t, m_doc.string(ts.text));  // T-270a: its own slot.
            }
            td.styleRuns = ts.styleRuns;
            td.inlines = ts.inlines;
            if (ts.editIndex != kInvalidEditIndex && ts.editIndex < m_doc.m_editStates->size()) {
                EditState state = (*m_doc.m_editStates)[ts.editIndex];
                td.editIndex = static_cast<uint32_t>(m_doc.m_editStates->size());
                m_doc.m_editStates->push_back(std::move(state));
            }
            bool glyph = false;
            for (const TextInline& object : td.inlines) {
                glyph = glyph || object.kind == TextInlineKind::Glyph;
            }
            if (glyph) {
                m_doc.m_glyphTextNodes.push_back(t);
            }
            arena.appendChild(n, t);
        }
        if (k > 0) {
            arena.appendChild(inst.nodes[m_tplPos[elements[di].parent]], n);
        }
        inst.nodes[k] = n;
    }
    renameInstance(r, inst);
    // T-202 (03 §3.12.5): a `<particles>` copy gets its own emitter, with a per-instance stream.
    for (NodeId n : inst.nodes) {
        const ElementData& d = arena.element(n);
        if (d.tag != kInvalidString && m_doc.m_ir.string(d.tag) == "particles") {
            m_doc.loadParticles(n);
            const NodeId child = arena.element(n).particlesNode;
            if (child != kInvalidNode) {
                arena.particles(child).seed += (index + 1u) * 0x85EBCA6Bu;
            }
        }
    }
    // T-380: the instance's own copies of the template's ports (after renameInstance(), so their
    // names carry the instance's path ids), then the bindings reading / targeting them.
    std::span<const IrPort> ports = m_doc.m_ir.ports().ports();
    const uint32_t ri = m_templateOf[r.ir->templateRoot];
    for (uint32_t tp : r.tplPorts) {
        const IrPort& p = ports[tp];
        const NodeId element = inst.nodes[m_tplPos[p.element]];
        const NodeId scope = p.scope < m_templateOf.size() && m_templateOf[p.scope] == ri
                                 ? inst.nodes[m_tplPos[p.scope]]
                                 : kInvalidNode;
        const uint32_t copy = m_doc.m_bindings->addInstancePort(tp, element, scope);
        if (copy != BindingRuntime::kNoPort) {
            inst.ports.emplace_back(tp, copy);
        }
    }
    m_doc.m_bindings->linkInstancePorts(inst.ports);
    std::span<const IrBinding> binds = m_doc.m_ir.bindings().bindings();
    for (uint32_t irb : r.tplBindings) {
        const NodeId node = inst.nodes[m_tplPos[binds[irb].element]];
        const uint32_t b = m_doc.m_bindings->addInstanceBinding(irb, node, r.listPort, index, inst.ports);
        if (r.parentRepeat != kNoItem) {
            m_doc.m_bindings->setInstanceOuter(b, r.outerPort, r.outerItem);  // T-391: `outer.*`.
        }
        inst.bindings.push_back(b);
    }
    createInnerRepeats(r, inst);  // T-391
    return inst;
}

void RepeatController::createInnerRepeats(Repeat& r, Instance& inst) {
    const uint32_t self = r.self;
    for (uint32_t pi = 0; pi < m_repeats.size(); ++pi) {
        if (!m_repeats[pi].proto || m_repeats[pi].ir == nullptr) {
            continue;
        }
        const uint32_t element = m_repeats[pi].ir->element;
        if (element >= m_templateOf.size() || m_templateOf[element] != m_repeats[self].tplIndex ||
            m_tplPos[element] >= inst.nodes.size()) {
            continue;
        }
        uint32_t di = static_cast<uint32_t>(m_repeats.size());
        for (uint32_t k = 0; k < m_repeats.size(); ++k) {
            if (m_repeats[k].free) {
                di = k;
                break;
            }
        }
        if (di == m_repeats.size()) {
            m_repeats.emplace_back();  // A deque: `r` and the other records stay where they are.
        }
        const Repeat& proto = m_repeats[pi];
        Repeat copy;
        copy.ir = proto.ir;
        copy.self = di;
        copy.tplIndex = pi;
        copy.tpl = proto.tpl;
        copy.tplBindings = proto.tplBindings;
        copy.tplPorts = proto.tplPorts;
        copy.exitRemoves = proto.exitRemoves;
        copy.stripPrefix = proto.stripPrefix;
        copy.element = inst.nodes[m_tplPos[element]];
        copy.id = m_doc.m_arena.element(copy.element).id;
        copy.parentRepeat = self;
        copy.outerPort = m_repeats[self].listPort;
        copy.outerItem = inst.index;
        if ((proto.ir->flags & kRepeatItemField) != 0) {
            copy.listPort = m_doc.m_bindings->addNestedList(m_repeats[self].listPort, proto.ir->listPort,
                                                            inst.index, m_repeats[self].element);
            copy.hiddenList = copy.listPort != BindingRuntime::kNoPort;
            if (!copy.hiddenList) {
                copy.listPort = kRepeatNoListPort;
            }
        } else {
            // T-399 (08 §8.18, XF-098): a list port declared on a template element (a symbol's
            // `port:rows="list<...> in"` written by `bind:rows="item(rows)"`) is read through this
            // instance's copy (T-380), or an enclosing instance's; a document port stays shared.
            copy.listPort = proto.ir->listPort;
            bool mapped = false;
            for (const auto& [tplPort, own] : inst.ports) {
                if (tplPort == proto.ir->listPort) {
                    copy.listPort = own;
                    mapped = true;
                    break;
                }
            }
            if (!mapped) {
                copy.listPort = instancePortOf(m_repeats[self].element, proto.ir->listPort);
            }
        }
        m_repeats[di] = std::move(copy);
        inst.inner.push_back(di);
    }
}

void RepeatController::refreshInner(uint32_t ri) {
    if (ri >= m_repeats.size() || m_repeats[ri].free || m_repeats[ri].dead || m_repeats[ri].frozen) {
        return;
    }
    Repeat& d = m_repeats[ri];
    if (d.hiddenList) {
        const bool changed = m_doc.m_bindings->refreshNestedList(d.listPort);
        if (changed || !d.synced) {
            d.synced = true;
            onListChanged(d.listPort, RepeatListOp::Set, 0);
            return;
        }
    } else if (!d.synced) {
        d.synced = true;
        sync(d, 0, count(d));
        return;
    }
    // The list did not change: the bindings reading the enclosing entry may have.
    const size_t start = m_evalScratch.size();
    for (const Instance& inst : m_repeats[ri].live) {
        for (uint32_t b : inst.bindings) {
            if (m_doc.m_bindings->usesOuter(b)) {
                m_evalScratch.push_back(b);
            }
        }
    }
    const size_t end = m_evalScratch.size();
    for (size_t i = start; i < end; ++i) {
        m_doc.m_bindings->evaluateBinding(m_evalScratch[i], /*initial=*/false);
    }
    m_evalScratch.resize(start);
}

void RepeatController::releaseRepeat(uint32_t ri, bool destroyNodes) {
    if (ri >= m_repeats.size() || m_repeats[ri].free) {
        return;
    }
    m_repeats[ri].dead = true;  // First: a re-entrant Document::remove() skips it.
    std::vector<Instance> gone;
    for (std::vector<Instance>* list : {&m_repeats[ri].live, &m_repeats[ri].exiting, &m_repeats[ri].pool}) {
        for (Instance& inst : *list) {
            gone.push_back(std::move(inst));
        }
        list->clear();
    }
    NodeArena& arena = m_doc.m_arena;
    for (Instance& inst : gone) {
        const NodeId root = inst.nodes.empty() ? kInvalidNode : inst.nodes[0];
        stopPlaybacks(root);
        releaseInstance(inst);
        if (destroyNodes && root != kInvalidNode && arena.alive(root)) {
            m_doc.remove(ElementRef{&m_doc, root, arena.generation(root)});
        }
    }
    if (m_repeats[ri].hiddenList) {
        m_doc.m_bindings->removeNestedList(m_repeats[ri].listPort);
    }
    Repeat freed;
    freed.self = ri;
    freed.free = true;
    freed.dead = true;
    m_repeats[ri] = std::move(freed);
}

void RepeatController::repointInner(Instance& inst, uint32_t index) {
    for (uint32_t di : inst.inner) {
        if (di >= m_repeats.size() || m_repeats[di].free) {
            continue;
        }
        Repeat& d = m_repeats[di];
        d.outerItem = index;
        if (d.hiddenList) {
            m_doc.m_bindings->setNestedItem(d.listPort, index);
        }
        for (std::vector<Instance>* list : {&d.live, &d.exiting, &d.pool}) {
            for (const Instance& x : *list) {
                for (uint32_t b : x.bindings) {
                    m_doc.m_bindings->setInstanceOuter(b, d.outerPort, index);
                }
            }
        }
    }
    refreshInnerIds(inst);
}

void RepeatController::freezeInner(Instance& inst) {
    for (uint32_t di : inst.inner) {
        if (di >= m_repeats.size() || m_repeats[di].free) {
            continue;
        }
        Repeat& d = m_repeats[di];
        d.frozen = true;
        if (d.hiddenList) {
            m_doc.m_bindings->setNestedItem(d.listPort, kNoItem);
        }
        for (Instance& x : d.live) {
            freezeInner(x);
        }
    }
}

void RepeatController::refreshInnerIds(Instance& inst) {
    NodeArena& arena = m_doc.m_arena;
    for (uint32_t di : inst.inner) {
        if (di >= m_repeats.size() || m_repeats[di].free) {
            continue;
        }
        Repeat& d = m_repeats[di];
        if (d.element != kInvalidNode && arena.alive(d.element)) {
            d.id = arena.element(d.element).id;
        }
        for (std::vector<Instance>* list : {&d.live, &d.exiting, &d.pool}) {
            for (Instance& x : *list) {
                if (x.index != kNoItem) {
                    renameInstance(d, x);
                }
                refreshInnerIds(x);
            }
        }
    }
}

NodeId RepeatController::anchorFor(const Repeat& r, uint32_t index) const {
    // T-260b: only a live child of the repeat can anchor (defensive: Document::remove() already
    // drops the instances it destroys, see onSubtreeRemoved()).
    const NodeArena& arena = m_doc.m_arena;
    auto attached = [&arena, &r](NodeId n) {
        return n != kInvalidNode && arena.alive(n) && arena.parent(n) == r.element;
    };
    for (const Instance& inst : r.live) {
        if (inst.index != kNoItem && inst.index > index && attached(inst.nodes[0])) {
            return inst.nodes[0];
        }
    }
    return attached(r.spacerAfter) ? r.spacerAfter : kInvalidNode;
}

void RepeatController::attachInstance(Repeat& r, Instance& inst) {
    NodeArena& arena = m_doc.m_arena;
    const NodeId root = inst.nodes[0];
    if (r.dead || !arena.alive(r.element) || !arena.alive(root)) {
        return;  // T-260b: defensive (a dead repeat never gets here through onListChanged()).
    }
    arena.insertBefore(r.element, root, anchorFor(r, inst.index));  // Marks the repeat dirty.
    arena.markDirty(root, static_cast<uint8_t>(kDirtyStyle | kDirtyLayout));
    arena.traversePreorder(root, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
    m_doc.markSiblingsStyleDirty(r.element);
    for (NodeId n : inst.nodes) {
        if (arena.common(n).triggerTable != 0) {
            m_doc.m_triggers->bindNode(n);
            m_doc.m_bindings->bindTriggerNode(n, inst.ports);  // T-380: on the instance's port copies.
        }
    }
}

void RepeatController::pointInstance(Repeat& r, Instance& inst, uint32_t index) {
    if (inst.index == index) {
        return;
    }
    inst.index = index;
    renameInstance(r, inst);
    for (uint32_t b : inst.bindings) {
        m_doc.m_bindings->setInstanceItem(b, index);
    }
    repointInner(inst, index);  // T-391
}

void RepeatController::evaluateInstance(const Instance& inst, bool initial) {
    // T-260b: a binding may reach a host callback that changes this list, which moves or drops
    // `inst`; its binding ids are copied first and `inst` is not touched again.
    const std::vector<uint32_t> inner = inst.inner;  // T-391: copied for the same reason.
    const size_t start = m_evalScratch.size();
    m_evalScratch.insert(m_evalScratch.end(), inst.bindings.begin(), inst.bindings.end());
    const size_t end = m_evalScratch.size();
    for (size_t i = start; i < end; ++i) {
        m_doc.m_bindings->evaluateBinding(m_evalScratch[i], initial);
    }
    m_evalScratch.resize(start);
    for (uint32_t di : inner) {
        refreshInner(di);  // T-391: the nested repeats follow the (new) entry.
    }
}

size_t RepeatController::liveIndexOf(const Repeat& r, NodeId root) const {
    for (size_t i = 0; i < r.live.size(); ++i) {
        if (r.live[i].nodes[0] == root) {
            return i;
        }
    }
    return r.live.size();
}

void RepeatController::resetFromTemplate(Repeat& r, Instance& inst) {
    NodeArena& arena = m_doc.m_arena;
    if (!inst.nodes.empty() && arena.alive(inst.nodes[0])) {
        // T-270f: out of the pool, so the ElementRef mutators accept the subtree again.
        arena.traversePreorder(inst.nodes[0], [&arena](NodeId n) {
            arena.common(n).flags2 &= ~static_cast<uint32_t>(kPooled);
            return true;
        });
    }
    for (size_t k = 0; k < r.tpl.size(); ++k) {
        const NodeId n = inst.nodes[k];
        const NodeId src = m_doc.m_docElementNodes[r.tpl[k]];
        if (!arena.alive(n)) {
            continue;
        }
        {
            const ElementData& s = arena.element(src);
            ElementData& d = arena.element(n);
            d.classes = s.classes;
            d.stateBits = s.stateBits;
            d.attributes = s.attributes;
            d.inlineStyle = s.inlineStyle;
            d.runtimeDecls.clear();
            d.setStyleHash = 0;  // T-389.
            d.styleResolvedOnce = 0;
        }
        arena.common(n).customStates = arena.common(src).customStates;  // T-385: `<use state>` seeds.
        // T-270b: the text comes from the template again, createInstance()'s way: a runtime text
        // is copied into this node's own slot (the one it held was released when it was recycled).
        NodeId c = arena.firstChild(src);
        NodeId t = arena.firstChild(n);
        for (;;) {
            while (c != kInvalidNode && arena.kind(c) != NodeKind::Text) {
                c = arena.nextSibling(c);
            }
            while (t != kInvalidNode && arena.kind(t) != NodeKind::Text) {
                t = arena.nextSibling(t);
            }
            if (c == kInvalidNode || t == kInvalidNode) {
                break;
            }
            const StringId text = arena.text(c).text;
            TextNodeData& td = arena.text(t);
            td.text = text;
            if (text >= kRuntimeTextBase && text != kInvalidString) {
                td.text = m_doc.setRuntimeText(t, m_doc.string(text));
            }
            td.shapedValid = 0;
            arena.markDirty(t, static_cast<uint8_t>(kDirtyLayout));
            c = arena.nextSibling(c);
            t = arena.nextSibling(t);
        }
    }
}

void RepeatController::releaseInstance(Instance& inst) {
    for (uint32_t b : inst.bindings) {
        m_doc.m_bindings->removeBinding(b);
    }
    inst.bindings.clear();
    for (NodeId n : inst.nodes) {
        m_doc.m_bindings->forgetTriggerNode(n);
    }
    for (const auto& [tpl, copy] : inst.ports) {
        m_doc.m_bindings->removeInstancePort(copy);  // T-380
    }
    inst.ports.clear();
    const std::vector<uint32_t> inner = std::move(inst.inner);  // T-391
    inst.inner.clear();
    for (uint32_t di : inner) {
        releaseRepeat(di, /*destroyNodes=*/false);  // Their nodes go with this instance's.
    }
}

void RepeatController::destroyInstance(Repeat& r, Instance& inst) {
    (void)r;
    NodeArena& arena = m_doc.m_arena;
    const NodeId root = inst.nodes[0];
    stopPlaybacks(root);
    releaseInstance(inst);
    if (arena.alive(root)) {
        m_doc.remove(ElementRef{&m_doc, root, arena.generation(root)});
    }
}

void RepeatController::recycleInstance(Repeat& r, Instance inst) {
    NodeArena& arena = m_doc.m_arena;
    const NodeId root = inst.nodes[0];
    if ((!r.virtualize && !r.spawn) || r.pool.size() >= r.poolCap || !arena.alive(root)) {
        destroyInstance(r, inst);
        return;
    }
    stopPlaybacks(root);
    {
        // T-391: the nested repeats (and their instances' nodes) go; reuse makes them again.
        const std::vector<uint32_t> inner = std::move(inst.inner);
        inst.inner.clear();
        for (uint32_t di : inner) {
            releaseRepeat(di, /*destroyNodes=*/true);
        }
    }
    for (uint32_t b : inst.bindings) {
        m_doc.m_bindings->setInstanceItem(b, kNoItem);
    }
    for (NodeId n : inst.nodes) {
        m_doc.m_bindings->forgetTriggerNode(n);
    }
    for (const auto& [tpl, copy] : inst.ports) {
        m_doc.m_bindings->resetInstancePort(copy);  // T-380: back to the declared default.
    }
    // What Document::remove() drops per node (focus/hover, listeners, animations, waiters), but
    // the nodes stay alive, detached, for reuse.
    m_doc.m_events->clearForSubtreeRemoval(arena, root);
    std::vector<NodeId> nodes;
    arena.traversePostorder(root, [&nodes](NodeId n) { nodes.push_back(n); });
    for (NodeId n : nodes) {
        m_doc.cleanupNodeForRemoval(n);
        arena.common(n).flags2 |= kPooled;  // T-270f: refused by every mutating ElementRef call.
    }
    if (arena.parent(root) != kInvalidNode) {
        arena.detach(root);
    }
    m_doc.markSiblingsStyleDirty(r.element);
    inst.index = kNoItem;
    r.pool.push_back(std::move(inst));
}

bool RepeatController::sync(Repeat& r, uint32_t begin, uint32_t end) {
    if (r.dead) {
        return false;  // T-260b: a callback above removed the `<repeat>` element.
    }
    const uint32_t n = count(r);
    end = std::min(end, n);
    begin = std::min(begin, end);
    bool changed = false;
    // Instances outside [begin, end) go (no item-exit: the entry still exists or went with a set).
    for (size_t i = 0; i < r.live.size();) {
        if (r.live[i].index >= begin && r.live[i].index < end) {
            ++i;
            continue;
        }
        Instance gone = std::move(r.live[i]);
        r.live.erase(r.live.begin() + static_cast<std::ptrdiff_t>(i));
        recycleInstance(r, std::move(gone));
        changed = true;
    }
    for (uint32_t index = begin; index < end; ++index) {
        auto it = std::lower_bound(r.live.begin(), r.live.end(), index,
                                   [](const Instance& inst, uint32_t v) { return inst.index < v; });
        if (it != r.live.end() && it->index == index) {
            continue;
        }
        Instance inst;
        if (!r.pool.empty()) {
            inst = std::move(r.pool.back());
            r.pool.pop_back();
            resetFromTemplate(r, inst);
            inst.index = kNoItem;
            pointInstance(r, inst, index);
            createInnerRepeats(r, inst);  // T-391: released when it was pooled.
        } else {
            inst = createInstance(r, index);
        }
        it = std::lower_bound(r.live.begin(), r.live.end(), index,
                              [](const Instance& i, uint32_t v) { return i.index < v; });
        it = r.live.insert(it, std::move(inst));
        const NodeId root = it->nodes[0];
        attachInstance(r, *it);
        // Evaluate through a fresh lookup: the bindings may change the list (re-entrancy).
        const size_t at = liveIndexOf(r, root);
        if (at < r.live.size()) {
            evaluateInstance(r.live[at], /*initial=*/true);
        }
        changed = true;
        if (r.dead) {
            break;  // T-260b: that evaluation's callback removed the `<repeat>` element.
        }
    }
    return changed;
}

bool RepeatController::setSpacers(Repeat& r) {
    if (!r.virtualize) {
        return false;
    }
    const uint32_t n = count(r);
    const float h = r.itemHeight > 0 ? r.itemHeight : 0.0f;
    const uint32_t b = std::min(r.windowEnd, n);
    const uint32_t a = std::min(r.windowBegin, b);
    const float before = static_cast<float>(a) * h;
    const float after = static_cast<float>(n - b) * h;
    bool changed = false;
    NodeArena& arena = m_doc.m_arena;
    auto apply = [&](NodeId spacer, float value, float& last) {
        if (spacer == kInvalidNode || !arena.alive(spacer) || value == last) {
            return;
        }
        last = value;
        ElementRef{&m_doc, spacer, arena.generation(spacer)}.setStyle("height: " + std::to_string(value) +
                                                                      "px");
        changed = true;
    };
    apply(r.spacerBefore, before, r.spacerBeforeH);
    apply(r.spacerAfter, after, r.spacerAfterH);
    return changed;
}

// -- List changes -------------------------------------------------------------------------------

void RepeatController::onListChanged(uint32_t port, RepeatListOp op, uint32_t index) {
    for (size_t ri = 0; ri < m_repeats.size(); ++ri) {
        if (m_repeats[ri].listPort != port || m_repeats[ri].dead || m_repeats[ri].spawn ||
            m_repeats[ri].frozen) {
            continue;  // T-260b: a repeat whose element the host removed ignores its list.
        }
        Repeat* r = &m_repeats[ri];
        const uint32_t n = count(*r);
        // T-260b (C10): an instance binding can reach a host callback that changes this same list
        // (a nested onListChanged() inserts into or erases from `live`). Every walk that evaluates
        // goes by index, looks the repeat up again after each evaluation and re-checks the bound.
        auto evaluateLive = [this, ri, &r](auto&& wanted) {
            for (size_t i = 0;; ++i) {
                r = &m_repeats[ri];
                if (i >= r->live.size()) {
                    break;
                }
                if (wanted(r->live[i])) {
                    evaluateInstance(r->live[i], /*initial=*/false);
                }
            }
            r = &m_repeats[ri];
        };
        switch (op) {
            case RepeatListOp::Update:
                for (size_t i = 0; i < r->live.size(); ++i) {
                    if (r->live[i].index == index) {
                        evaluateInstance(r->live[i], /*initial=*/false);
                        break;
                    }
                }
                break;
            case RepeatListOp::Set:
                evaluateLive([n](const Instance& inst) { return inst.index < n; });
                sync(*r, r->virtualize ? r->windowBegin : 0, r->virtualize ? std::max(r->windowEnd, 1u) : n);
                break;
            case RepeatListOp::Insert: {
                for (Instance& inst : r->live) {
                    if (inst.index >= index) {
                        pointInstance(*r, inst, inst.index + 1);
                    }
                }
                const bool inWindow = !r->virtualize || (index >= r->windowBegin && index <= r->windowEnd);
                if (r->virtualize && inWindow) {
                    r->windowEnd = std::max(r->windowEnd, index + 1);
                }
                sync(*r, r->virtualize ? r->windowBegin : 0, r->virtualize ? r->windowEnd : n);
                r = &m_repeats[ri];
                NodeId entered = kInvalidNode;
                for (const Instance& inst : r->live) {
                    if (inst.index == index) {
                        entered = inst.nodes[0];
                    }
                }
                evaluateLive([entered](const Instance& inst) { return inst.nodes[0] != entered; });
                if (entered != kInvalidNode) {
                    m_doc.m_triggers->fire(entered, TriggerEvent::ItemEnter);
                }
                break;
            }
            case RepeatListOp::Remove: {
                NodeId exitRoot = kInvalidNode;
                for (size_t i = 0; i < r->live.size(); ++i) {
                    if (r->live[i].index != index) {
                        continue;
                    }
                    Instance gone = std::move(r->live[i]);
                    r->live.erase(r->live.begin() + static_cast<std::ptrdiff_t>(i));
                    if (r->ir->exitTriggerActions != 0 && m_doc.m_arena.alive(gone.nodes[0])) {
                        for (uint32_t b : gone.bindings) {
                            m_doc.m_bindings->setInstanceItem(b, kNoItem);  // Frozen.
                        }
                        gone.index = kNoItem;
                        freezeInner(gone);  // T-391
                        exitRoot = gone.nodes[0];
                        r->exiting.push_back(std::move(gone));
                    } else {
                        recycleInstance(*r, std::move(gone));
                    }
                    break;
                }
                for (Instance& inst : r->live) {
                    if (inst.index > index) {
                        pointInstance(*r, inst, inst.index - 1);
                    }
                }
                sync(*r, r->virtualize ? r->windowBegin : 0, r->virtualize ? r->windowEnd : n);
                r = &m_repeats[ri];
                evaluateLive([](const Instance&) { return true; });
                if (exitRoot != kInvalidNode) {
                    m_doc.m_triggers->fire(exitRoot, TriggerEvent::ItemExit);
                    r = &m_repeats[ri];
                    if (!r->exitRemoves) {
                        removeAction(exitRoot);  // No `then remove`: it goes at once.
                    }
                }
                break;
            }
        }
        setSpacers(m_repeats[ri]);
    }
}

void RepeatController::removeAction(NodeId node) {
    for (Repeat& r : m_repeats) {
        for (size_t i = 0; i < r.exiting.size(); ++i) {
            Instance& inst = r.exiting[i];
            if (inst.nodes[0] != node && !m_doc.m_arena.isAncestorOf(inst.nodes[0], node)) {
                continue;
            }
            Instance gone = std::move(inst);
            r.exiting.erase(r.exiting.begin() + static_cast<std::ptrdiff_t>(i));
            destroyInstance(r, gone);
            return;
        }
    }
}

void RepeatController::onSubtreeRemoved(NodeId root) {
    NodeArena& arena = m_doc.m_arena;
    if (m_repeats.empty() || root == kInvalidNode || !arena.alive(root)) {
        return;
    }
    auto inSubtree = [&arena, root](NodeId n) {
        return n != kInvalidNode && arena.alive(n) && (n == root || arena.isAncestorOf(root, n));
    };
    for (Repeat& r : m_repeats) {
        if (r.dead) {
            continue;
        }
        const bool repeatGone = inSubtree(r.element);
        if (repeatGone) {
            r.dead = true;  // First: a nested Document::remove() below skips this repeat.
        }
        // The instances whose root goes with the subtree (all of them when the repeat goes): their
        // nodes are destroyed by Document::remove(), the rest is dropped here.
        for (std::vector<Instance>* list : {&r.live, &r.exiting}) {
            for (size_t i = 0; i < list->size();) {
                if (!repeatGone && !inSubtree((*list)[i].nodes[0])) {
                    ++i;
                    continue;
                }
                Instance gone = std::move((*list)[i]);
                list->erase(list->begin() + static_cast<std::ptrdiff_t>(i));
                stopPlaybacks(gone.nodes[0]);
                releaseInstance(gone);
            }
        }
        if (!repeatGone) {
            continue;
        }
        // The pooled instances are detached (not in the subtree): destroyed here.
        while (!r.pool.empty()) {
            Instance gone = std::move(r.pool.back());
            r.pool.pop_back();
            destroyInstance(r, gone);
        }
        r.element = kInvalidNode;
        r.spacerBefore = kInvalidNode;
        r.spacerAfter = kInvalidNode;
    }
}

// -- Virtualization -----------------------------------------------------------------------------

NodeId RepeatController::viewportOf(const Repeat& r) const {
    const NodeArena& arena = m_doc.m_arena;
    if (r.ir->viewportElement != kRepeatNoViewport) {
        NodeId vp = m_doc.m_docElementNodes[r.ir->viewportElement];
        return vp != kInvalidNode && arena.alive(vp) ? vp : kInvalidNode;
    }
    for (NodeId p = arena.parent(r.element); p != kInvalidNode; p = arena.parent(p)) {
        // T-336: the nearest vertical scroll container (`overflow-y` scroll or auto).
        if (arena.kind(p) == NodeKind::Element &&
            (usedOverflowY(arena.element(p).style) == Overflow::Scroll ||
             usedOverflowY(arena.element(p).style) == Overflow::Auto)) {
            return p;
        }
    }
    return kInvalidNode;
}

bool RepeatController::afterLayout() {
    bool changed = false;
    NodeArena& arena = m_doc.m_arena;
    for (size_t ri = 0; ri < m_repeats.size(); ++ri) {  // T-391: sync() may add nested copies.
        Repeat& r = m_repeats[ri];
        if (!r.virtualize || r.dead || !arena.alive(r.element)) {
            continue;
        }
        const uint32_t n = count(r);
        if (r.itemHeight <= 0 && !r.live.empty() && arena.alive(r.live.front().nodes[0])) {
            const LayoutBox& box = arena.element(r.live.front().nodes[0]).box;
            const float h = box.height + box.margin.top + box.margin.bottom;
            if (h > 0) {
                r.itemHeight = h;
            }
        }
        uint32_t a = 0;
        uint32_t b = std::min(n, 1u);
        uint32_t visible = 1;
        if (r.itemHeight > 0) {
            const NodeId vp = viewportOf(r);
            float vpH = m_doc.m_viewport.h;
            float scrollY = 0;
            if (vp != kInvalidNode) {
                const ElementData& vd = arena.element(vp);
                vpH = vd.box.paddingBox().h;
                scrollY = appliedScroll(vd).y;  // T-336: 0 unless the vertical axis scrolls.
            }
            if (!r.viewportChecked) {
                // T-392 (27 §27.3.2, XF-073): once, after the first layout that measured an entry.
                r.viewportChecked = true;
                if (vp != kInvalidNode && vpH <= 0.0f) {
                    std::string name;
                    if (r.id != kInvalidString) {
                        name = std::string(m_doc.string(r.id));
                    } else if (r.listPort < m_doc.m_ir.ports().ports().size()) {
                        name = std::string(m_doc.m_ir.string(m_doc.m_ir.ports().ports()[r.listPort].name));
                    }
                    m_doc.addWarning("virtualized repeat \"" + name +
                                     "\" has a zero-height viewport; give the scroll container a height");
                }
            }
            // The repeat's content top in the viewport's content coordinates.
            float top = arena.element(r.element).box.contentBox().y;
            for (NodeId c = r.element; c != kInvalidNode && c != vp;) {
                top += arena.element(c).box.y;
                const NodeId p = arena.parent(c);
                if (p == kInvalidNode || p == vp || arena.kind(p) != NodeKind::Element) {
                    break;
                }
                top += arena.element(p).box.contentBox().y;
                c = p;
            }
            const float h = r.itemHeight;
            const double firstF = std::floor((scrollY - top) / h);
            const double lastF = std::ceil((scrollY + vpH - top) / h);
            const uint32_t first = static_cast<uint32_t>(std::clamp(firstF, 0.0, static_cast<double>(n)));
            const uint32_t last =
                std::max(first, static_cast<uint32_t>(std::clamp(lastF, 0.0, static_cast<double>(n))));
            visible = std::max(1u, static_cast<uint32_t>(std::ceil(std::max(vpH, 0.0f) / h)));
            a = first > visible ? first - visible : 0;
            b = std::min(n, last + visible);
        } else if (!r.live.empty()) {
            b = n;  // Zero-height entries: every entry is "visible".
            visible = std::max(1u, n);
        }
        r.windowBegin = a;
        r.windowEnd = b;
        r.poolCap = visible * 3;
        changed = sync(r, a, b) || changed;
        changed = setSpacers(r) || changed;
        while (r.pool.size() > r.poolCap) {
            Instance gone = std::move(r.pool.back());
            r.pool.pop_back();
            destroyInstance(r, gone);
        }
    }
    return changed;
}

// -- Instance timelines -------------------------------------------------------------------------

void RepeatController::applyPlayback(Playback& pb) {
    for (size_t i = 0; i < pb.ids.size(); ++i) {
        if (pb.ids[i] == 0) {
            continue;
        }
        const IrTimelineGroupMember& m = pb.members[i];
        float u;
        if (m.duration > 0) {
            u = clampf((pb.t - m.delay) / m.duration, 0.0f, 1.0f);
        } else {
            u = pb.t >= m.delay ? 1.0f : 0.0f;
        }
        m_doc.m_animator.seek(pb.ids[i], u, m_doc.m_clock);
    }
}

void RepeatController::releasePlayback(Playback& pb) {
    NodeArena& arena = m_doc.m_arena;
    for (size_t i = 0; i < pb.ids.size(); ++i) {
        if (pb.ids[i] == 0) {
            continue;
        }
        m_doc.m_animator.stop(pb.ids[i]);
        const NodeId node = pb.nodes[i];
        if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
            continue;
        }
        // As Document::releaseTimelineGroup: put the styled values back.
        const ElementData& data = arena.element(node);
        if (const Timeline* tl = m_doc.m_animator.timeline(pb.members[i].timeline)) {
            for (const Track& track : tl->tracks) {
                if (track.prop < prop::kElementPropCount && data.styleResolvedOnce != 0) {
                    setProperty(arena, node, track.prop, data.style.base[track.prop]);
                }
            }
        }
        arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    }
    pb.ids.clear();
    pb.playing = false;
}

void RepeatController::stopPlaybacks(NodeId scope) {
    for (size_t i = 0; i < m_playbacks.size();) {
        if (m_playbacks[i].scope != scope) {
            ++i;
            continue;
        }
        releasePlayback(m_playbacks[i]);
        m_playbacks.erase(m_playbacks.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

RepeatPlay RepeatController::play(NodeId node, StringId name, const PlayOptions& options, bool keepPlaying,
                                  NodeId& scope) {
    Repeat* r = nullptr;
    Instance* inst = nullptr;
    bool exiting = false;
    if (!findInstance(node, r, inst, exiting)) {
        return RepeatPlay::NotInstance;
    }
    const uint32_t ri = r->tplIndex;  // T-391: a nested copy plays its RPT row's template.
    // T-201: the group of that name animating this template, whatever its scope (the root, or a
    // symbol instance around or inside the repeat: T-190 scopes a symbol's groups per instance).
    int found = -1;
    const std::vector<TimelineGroupRuntime>& groups = m_doc.m_timelineGroups->groups;
    for (size_t gi = 0; gi < groups.size() && found < 0; ++gi) {
        if (groups[gi].name != name) {
            continue;
        }
        for (const IrTimelineGroupMember& m : groups[gi].members) {
            if (m.element < m_templateOf.size() && m_templateOf[m.element] == ri) {
                found = static_cast<int>(gi);
                break;
            }
        }
    }
    if (found < 0) {
        return RepeatPlay::NotInstance;
    }
    const size_t group = static_cast<size_t>(found);
    const TimelineGroupRuntime& g = m_doc.m_timelineGroups->groups[group];
    Playback pb;
    for (const IrTimelineGroupMember& m : g.members) {
        if (m.element < m_templateOf.size() && m_templateOf[m.element] == ri) {
            pb.members.push_back(m);
            pb.nodes.push_back(inst->nodes[m_tplPos[m.element]]);
        }
    }
    if (pb.members.empty()) {
        return RepeatPlay::NotInstance;
    }
    scope = inst->nodes[0];
    Playback* existing = nullptr;
    for (Playback& p : m_playbacks) {
        if (p.scope == scope && p.group == group) {
            existing = &p;
            break;
        }
    }
    if (existing != nullptr && keepPlaying && existing->playing) {
        return RepeatPlay::KeptPlaying;
    }
    float start = options.reverse ? g.duration : 0.0f;
    if (!options.fromMarker.empty()) {
        bool markerFound = false;
        for (const IrTimelineGroupMarker& mk : g.markers) {
            if (m_doc.m_ir.string(mk.name) == options.fromMarker) {
                start = clampf(mk.time, 0.0f, g.duration);
                markerFound = true;
                break;
            }
        }
        if (!markerFound) {
            return RepeatPlay::Failed;
        }
    }
    if (existing != nullptr) {
        releasePlayback(*existing);
        m_playbacks.erase(m_playbacks.begin() + (existing - m_playbacks.data()));
    }
    NodeArena& arena = m_doc.m_arena;
    pb.scope = scope;
    pb.scopeGeneration = arena.generation(scope);
    pb.group = group;
    pb.name = name;
    pb.duration = g.duration;
    pb.t = start;
    pb.lastClock = m_doc.m_clock;
    pb.rate = options.rate > 0 ? options.rate : 1.0f;
    pb.reverse = options.reverse;
    pb.loop = options.loop && !options.once;
    pb.hold = options.hold;
    pb.playing = true;
    pb.ids.assign(pb.members.size(), 0);
    for (size_t i = 0; i < pb.members.size(); ++i) {
        const IrTimelineGroupMember& m = pb.members[i];
        if (!arena.alive(pb.nodes[i]) || m_doc.m_animator.timeline(m.timeline) == nullptr) {
            continue;
        }
        PlayParams params;
        params.timeline = m.timeline;
        params.selfNode = pb.nodes[i];
        params.durationScale = m.duration > 0 ? m.duration : 1.0f;
        params.fill = FillMode::Both;
        params.loop = LoopMode::Once;
        params.iterations = 1;
        params.group = 1;
        const uint32_t id = m_doc.m_animator.play(params, m_doc.m_clock);
        m_doc.m_animator.pause(id, m_doc.m_clock);
        pb.ids[i] = id;
    }
    applyPlayback(pb);
    m_playbacks.push_back(std::move(pb));
    return RepeatPlay::Played;
}

bool RepeatController::stop(NodeId node, StringId name, NodeId& scope) {
    Repeat* r = nullptr;
    Instance* inst = nullptr;
    bool exiting = false;
    if (!findInstance(node, r, inst, exiting)) {
        return false;
    }
    for (size_t i = 0; i < m_playbacks.size(); ++i) {
        if (m_playbacks[i].scope == inst->nodes[0] && m_playbacks[i].name == name) {
            scope = inst->nodes[0];
            releasePlayback(m_playbacks[i]);
            m_playbacks.erase(m_playbacks.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool RepeatController::seek(NodeId node, StringId name, float t) {
    Repeat* r = nullptr;
    Instance* inst = nullptr;
    bool exiting = false;
    if (!findInstance(node, r, inst, exiting)) {
        return false;
    }
    for (Playback& pb : m_playbacks) {
        if (pb.scope == inst->nodes[0] && pb.name == name) {
            // T-255b: a NaN / infinite time reads as 0 (clampf passes NaN through).
            pb.t = clampf(std::isfinite(t) ? t : 0.0f, 0.0f, pb.duration);
            pb.lastClock = m_doc.m_clock;
            applyPlayback(pb);
            return true;
        }
    }
    return false;
}

// -- Spawn pools (T-201) --------------------------------------------------------------------------

bool RepeatController::spawn(NodeId node, uint32_t pool, Vec2 at, float lifetime, StringId timeline) {
    NodeArena& arena = m_doc.m_arena;
    if (node == kInvalidNode || !arena.alive(node)) {
        return false;
    }
    Repeat* r = nullptr;
    uint32_t seen = 0;
    for (NodeId c = arena.firstChild(node); c != kInvalidNode && r == nullptr; c = arena.nextSibling(c)) {
        Repeat* candidate = repeatOfElement(c);
        if (candidate != nullptr && candidate->spawn && seen++ == pool) {
            r = candidate;
        }
    }
    if (r == nullptr) {
        return false;
    }
    const uint32_t ri = r->self;
    if (r->live.size() >= kMaxSpawnInstances) {
        const NodeId oldest = r->live.front().nodes[0];
        Instance gone = std::move(r->live.front());
        r->live.erase(r->live.begin());
        recycleInstance(*r, std::move(gone));
        m_spawned.erase(std::remove_if(m_spawned.begin(), m_spawned.end(),
                                       [oldest](const Spawned& s) { return s.root == oldest; }),
                        m_spawned.end());
    }
    const uint32_t index = m_spawnSerial++;
    Instance inst;
    if (!r->pool.empty()) {
        inst = std::move(r->pool.back());
        r->pool.pop_back();
        resetFromTemplate(*r, inst);
        inst.index = kNoItem;
        pointInstance(*r, inst, index);
    } else {
        inst = createInstance(*r, index);
    }
    r->live.push_back(std::move(inst));
    const NodeId root = r->live.back().nodes[0];
    attachInstance(*r, r->live.back());
    const Rect origin = ElementRef{&m_doc, r->element, arena.generation(r->element)}.boundingBox();
    ElementRef{&m_doc, root, arena.generation(root)}.setStyle(
        "position: absolute; left: " + std::to_string(at.x - origin.x) +
        "px; top: " + std::to_string(at.y - origin.y) + "px; pointer-events: none");
    // Evaluated once through a fresh lookup (a binding may spawn again), then frozen: a spawned
    // instance keeps the values it was spawned with. T-260b: looked up again after the
    // evaluation, which may have spawned (or removed) and so moved `live`.
    r = &m_repeats[ri];
    if (size_t k = liveIndexOf(*r, root); k < r->live.size()) {
        evaluateInstance(r->live[k], /*initial=*/true);
        r = &m_repeats[ri];
        k = liveIndexOf(*r, root);
        if (k < r->live.size()) {
            for (uint32_t b : r->live[k].bindings) {
                m_doc.m_bindings->setInstanceItem(b, kNoItem);
            }
        }
    }
    m_spawned.push_back(Spawned{root, arena.generation(root), m_doc.m_clock + std::max(0.0f, lifetime), ri});
    if (timeline != kInvalidString) {
        PlayOptions options;
        options.hold = true;
        NodeId scope = kInvalidNode;
        play(root, timeline, options, false, scope);
    }
    return true;
}

void RepeatController::expireSpawned(float clock) {
    const NodeArena& arena = m_doc.m_arena;
    for (size_t i = 0; i < m_spawned.size();) {
        const Spawned s = m_spawned[i];
        const bool alive = arena.alive(s.root) && arena.generation(s.root) == s.generation;
        if (alive && clock < s.expires) {
            ++i;
            continue;
        }
        m_spawned.erase(m_spawned.begin() + static_cast<std::ptrdiff_t>(i));
        if (s.repeat >= m_repeats.size()) {
            continue;
        }
        Repeat& r = m_repeats[s.repeat];
        for (size_t k = 0; k < r.live.size(); ++k) {
            if (r.live[k].nodes[0] == s.root) {
                Instance gone = std::move(r.live[k]);
                r.live.erase(r.live.begin() + static_cast<std::ptrdiff_t>(k));
                if (alive) {
                    recycleInstance(r, std::move(gone));
                } else {
                    releaseInstance(gone);  // T-260b (C14): its bindings and trigger nodes go too.
                }
                break;
            }
        }
    }
}

void RepeatController::step(float clock) {
    if (!m_spawned.empty()) {
        expireSpawned(clock);
    }
    const NodeArena& arena = m_doc.m_arena;
    for (size_t i = 0; i < m_playbacks.size();) {
        Playback& pb = m_playbacks[i];
        if (!arena.alive(pb.scope) || arena.generation(pb.scope) != pb.scopeGeneration) {
            releasePlayback(pb);
            m_playbacks.erase(m_playbacks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!pb.playing) {
            ++i;
            continue;
        }
        float dt = (clock - pb.lastClock) * pb.rate;
        pb.lastClock = clock;
        dt = dt < 0 ? 0 : dt;
        bool ended = false;
        if (!pb.reverse) {
            float to = pb.t + dt;
            if (pb.loop && pb.duration > 0) {
                to = std::fmod(to, pb.duration);
            } else if (to >= pb.duration) {
                to = pb.duration;
                ended = true;
            }
            pb.t = to;
        } else {
            float to = pb.t - dt;
            if (pb.loop && pb.duration > 0) {
                while (to < 0) {
                    to += pb.duration;
                }
            } else if (to <= 0) {
                to = 0;
                ended = true;
            }
            pb.t = to;
        }
        applyPlayback(pb);
        if (ended) {
            pb.playing = false;
            m_pendingEnds.emplace_back(pb.scope, pb.name);
            if (!pb.hold) {
                releasePlayback(pb);
                m_playbacks.erase(m_playbacks.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }
}

void RepeatController::flushEvents() {
    if (m_pendingEnds.empty()) {
        return;
    }
    std::vector<std::pair<NodeId, StringId>> batch;
    batch.swap(m_pendingEnds);
    for (const auto& [scope, name] : batch) {
        m_doc.m_triggers->onInstanceTimelineEnd(scope, name);
    }
}

// -- Lookups ------------------------------------------------------------------------------------

ElementRef RepeatController::byId(std::string_view id) const {
    const NodeArena& arena = m_doc.m_arena;
    for (const Repeat& r : m_repeats) {
        if (r.proto || r.free || r.frozen || r.id == kInvalidString) {
            continue;  // T-391
        }
        std::string_view rid = m_doc.string(r.id);
        if (id.size() <= rid.size() || id.substr(0, rid.size()) != rid || id[rid.size()] != '/') {
            continue;
        }
        for (const std::vector<Instance>* list : {&r.live, &r.exiting}) {
            for (const Instance& inst : *list) {
                for (NodeId n : inst.nodes) {
                    if (arena.alive(n) && arena.element(n).id != kInvalidString &&
                        m_doc.string(arena.element(n).id) == id) {
                        return ElementRef{&m_doc, n, arena.generation(n)};
                    }
                }
            }
        }
    }
    return ElementRef{};
}

std::span<const ElementRef> RepeatController::instances(std::string_view repeatId) {
    const NodeArena& arena = m_doc.m_arena;
    for (Repeat& r : m_repeats) {
        if (r.proto || r.free || r.frozen || r.id == kInvalidString || m_doc.string(r.id) != repeatId) {
            continue;  // T-391: a nested copy answers to its path id (`Rows/2/Sub`).
        }
        r.refs.clear();
        for (const Instance& inst : r.live) {
            if (arena.alive(inst.nodes[0])) {
                r.refs.push_back(ElementRef{&m_doc, inst.nodes[0], arena.generation(inst.nodes[0])});
            }
        }
        return std::span<const ElementRef>(r.refs.data(), r.refs.size());
    }
    return {};
}

uint32_t RepeatController::itemIndexOf(NodeId node, uint32_t* listPort, bool outermost) const {
    Repeat* r = nullptr;
    Instance* inst = nullptr;
    bool exiting = false;
    auto* self = const_cast<RepeatController*>(this);
    if (!self->findInstance(node, r, inst, exiting) || exiting) {
        return kNoItem;
    }
    // T-391: from a nested copy's instance out to the enclosing repeats' instances.
    for (uint32_t guard = 0; outermost && r->parentRepeat != kNoItem && guard < 64; ++guard) {
        Repeat* outerRepeat = nullptr;
        Instance* outerInst = nullptr;
        if (!self->findInstance(r->element, outerRepeat, outerInst, exiting) || exiting) {
            return kNoItem;
        }
        r = outerRepeat;
        inst = outerInst;
    }
    if (listPort != nullptr) {
        *listPort = r->listPort;
    }
    return inst->index;
}

void RepeatController::chainOf(NodeId node, std::vector<RepeatRef>& out) const {
    out.clear();
    Repeat* r = nullptr;
    Instance* inst = nullptr;
    bool exiting = false;
    auto* self = const_cast<RepeatController*>(this);  // findInstance() changes nothing.
    NodeId at = node;
    for (uint32_t guard = 0; guard < 64 && self->findInstance(at, r, inst, exiting); ++guard) {
        if (exiting) {
            out.clear();  // As itemIndexOf(): an exiting instance has no current entry.
            return;
        }
        if (!r->spawn && r->id != kInvalidString) {
            out.push_back(RepeatRef{m_doc.string(r->id), inst->index});
        }
        at = r->element;  // T-391: on to the enclosing repeat's instance.
    }
}

uint32_t RepeatController::instancePortOf(NodeId node, uint32_t port) const {
    Repeat* r = nullptr;
    Instance* inst = nullptr;
    bool exiting = false;
    auto* self = const_cast<RepeatController*>(this);
    // T-391: the innermost instance first, then the enclosing ones (a port declared in an outer
    // template, used from inside a nested repeat).
    for (NodeId at = node; self->findInstance(at, r, inst, exiting);) {
        for (const auto& [tpl, copy] : inst->ports) {
            if (tpl == port) {
                return copy;
            }
        }
        if (r->parentRepeat == kNoItem) {
            break;
        }
        at = r->element;
    }
    return port;
}

uint32_t RepeatController::indexAt(std::string_view repeatId, Vec2 rootPos) const {
    // T-387 (13 §13.9): `rootPos` in a node's own border-box coordinates (its world transform
    // inverted; false for a singular one).
    const NodeArena& arena = m_doc.m_arena;
    auto toLocal = [&arena](NodeId n, Vec2 pos, Vec2& out) {
        Mat2x3 inv{};
        if (!invert(arena.common(n).world, inv)) {
            return false;
        }
        out = apply(inv, pos);
        return true;
    };
    for (const Repeat& r : m_repeats) {
        if (r.spawn || r.dead || m_doc.string(r.id) != repeatId || !arena.alive(r.element)) {
            continue;
        }
        if (r.virtualize && r.itemHeight > 0) {
            // The repeat element holds every entry at itemHeight apart (the spacers stand in for
            // the ones not instantiated); its world transform already carries the viewport scroll.
            Vec2 local{};
            if (!toLocal(r.element, rootPos, local)) {
                return kNoItem;
            }
            const LayoutBox& box = arena.element(r.element).box;
            const float y = local.y - box.contentBox().y;
            if (local.x < 0 || local.x > box.width || y < 0) {
                return kNoItem;
            }
            const double index = std::floor(static_cast<double>(y) / r.itemHeight);
            return index < static_cast<double>(count(r)) ? static_cast<uint32_t>(index) : kNoItem;
        }
        for (const Instance& inst : r.live) {
            const NodeId root = inst.nodes[0];
            Vec2 local{};
            if (!arena.alive(root) || !toLocal(root, rootPos, local)) {
                continue;
            }
            const LayoutBox& box = arena.element(root).box;
            if (local.x >= 0 && local.y >= 0 && local.x < box.width && local.y < box.height) {
                return inst.index;
            }
        }
        return kNoItem;
    }
    return kNoItem;
}

bool RepeatController::itemSpan(std::string_view repeatId, uint32_t index, ItemSpan& out) const {
    // T-394 (08.15): ElementRef::scrollIntoView(repeatId, index) and the `<repeat>` drop index.
    const NodeArena& arena = m_doc.m_arena;
    for (const Repeat& r : m_repeats) {
        if (r.spawn || r.dead || m_doc.string(r.id) != repeatId || !arena.alive(r.element)) {
            continue;
        }
        if (index >= count(r)) {
            return false;
        }
        out = ItemSpan{};
        out.element = r.element;
        for (const Instance& inst : r.live) {
            if (inst.index == index && arena.alive(inst.nodes[0])) {
                out.instance = inst.nodes[0];
                break;
            }
        }
        if (r.virtualize) {
            if (!(r.itemHeight > 0)) {
                return false;
            }
            // As indexAt(): every entry sits itemHeight apart from the repeat's content top (the
            // spacers stand in for the ones not instantiated).
            out.virtualized = true;
            out.top = arena.element(r.element).box.contentBox().y + static_cast<float>(index) * r.itemHeight;
            out.height = r.itemHeight;
            return true;
        }
        return out.instance != kInvalidNode;
    }
    return false;
}

// -- Document ------------------------------------------------------------------------------------

std::span<const ElementRef> Document::repeatInstances(std::string_view repeatId) {
    return m_repeats->instances(repeatId);
}

uint32_t Document::repeatIndexAt(std::string_view repeatId, Vec2 rootPos) const {
    return m_repeats->indexAt(repeatId, rootPos);
}

std::vector<RepeatRef> Document::repeatChain(NodeId node) const {
    std::vector<RepeatRef> out;
    m_repeats->chainOf(node, out);
    return out;
}

}  // namespace xt
