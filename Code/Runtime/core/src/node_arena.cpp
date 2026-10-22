#include "xt/node_arena.h"

#include <algorithm>
#include <utility>

#include "xt/assert.h"
#include "xt/property.h"

/// NodeArena implementation. See Docs/design/01-architecture.md §1.3-1.4, Docs/tasks/T-04.md

namespace xt {

void NodeArena::checkAlive(NodeId id) const {
    (void)id;
    XT_ASSERT(id < m_kind.size());
    XT_ASSERT(m_kind[id] != NodeKind::Free);
}

bool NodeArena::stopsLayoutPropagation(NodeId /*id*/) const { return false; }

uint32_t NodeArena::allocateData(NodeKind kind) {
    switch (kind) {
        case NodeKind::Path:
            if (!m_pathFreeList.empty()) {
                uint32_t idx = m_pathFreeList.back();
                m_pathFreeList.pop_back();
                return idx;
            }
            m_paths.push_back(PathNodeData{});
            return static_cast<uint32_t>(m_paths.size() - 1);
        case NodeKind::Image:
            if (!m_imageFreeList.empty()) {
                uint32_t idx = m_imageFreeList.back();
                m_imageFreeList.pop_back();
                return idx;
            }
            m_images.push_back(ImageNodeData{});
            return static_cast<uint32_t>(m_images.size() - 1);
        case NodeKind::Text:
            if (!m_textFreeList.empty()) {
                uint32_t idx = m_textFreeList.back();
                m_textFreeList.pop_back();
                return idx;
            }
            m_texts.push_back(TextNodeData{});
            return static_cast<uint32_t>(m_texts.size() - 1);
        case NodeKind::Element:
            if (!m_elementFreeList.empty()) {
                uint32_t idx = m_elementFreeList.back();
                m_elementFreeList.pop_back();
                return idx;
            }
            m_elements.push_back(ElementData{});
            return static_cast<uint32_t>(m_elements.size() - 1);
        case NodeKind::Camera:
            if (!m_cameraFreeList.empty()) {
                uint32_t idx = m_cameraFreeList.back();
                m_cameraFreeList.pop_back();
                return idx;
            }
            m_cameras.push_back(CameraData{});
            return static_cast<uint32_t>(m_cameras.size() - 1);
        case NodeKind::External:
            if (!m_externalFreeList.empty()) {
                uint32_t idx = m_externalFreeList.back();
                m_externalFreeList.pop_back();
                return idx;
            }
            m_externals.push_back(ExternalNodeData{});
            return static_cast<uint32_t>(m_externals.size() - 1);
        case NodeKind::Particles:
            if (!m_particleFreeList.empty()) {
                uint32_t idx = m_particleFreeList.back();
                m_particleFreeList.pop_back();
                return idx;
            }
            m_particles.push_back(ParticleNodeData{});
            return static_cast<uint32_t>(m_particles.size() - 1);
        case NodeKind::Modifier:
            if (!m_modifierFreeList.empty()) {
                uint32_t idx = m_modifierFreeList.back();
                m_modifierFreeList.pop_back();
                return idx;
            }
            m_modifiers.push_back(ModifierNodeData{});
            return static_cast<uint32_t>(m_modifiers.size() - 1);
        case NodeKind::Shape:
            if (!m_shapeFreeList.empty()) {
                uint32_t idx = m_shapeFreeList.back();
                m_shapeFreeList.pop_back();
                return idx;
            }
            m_shapes.push_back(ShapeNodeData{});
            return static_cast<uint32_t>(m_shapes.size() - 1);
        case NodeKind::Bone:  // T-243
            if (!m_boneFreeList.empty()) {
                uint32_t idx = m_boneFreeList.back();
                m_boneFreeList.pop_back();
                return idx;
            }
            m_bones.push_back(BoneNodeData{});
            return static_cast<uint32_t>(m_bones.size() - 1);
        case NodeKind::Constraint:  // T-243
            if (!m_constraintFreeList.empty()) {
                uint32_t idx = m_constraintFreeList.back();
                m_constraintFreeList.pop_back();
                return idx;
            }
            m_constraints.push_back(ConstraintNodeData{});
            return static_cast<uint32_t>(m_constraints.size() - 1);
        case NodeKind::Mesh:  // T-243
            if (!m_meshFreeList.empty()) {
                uint32_t idx = m_meshFreeList.back();
                m_meshFreeList.pop_back();
                return idx;
            }
            m_meshes.push_back(MeshNodeData{});
            return static_cast<uint32_t>(m_meshes.size() - 1);
        case NodeKind::Group:
        case NodeKind::Mask:
        case NodeKind::Free:
        default:
            return 0;
    }
}

void NodeArena::freeNodeData(NodeId id) {
    uint32_t idx = m_kindIndex[id];
    switch (m_kind[id]) {
        case NodeKind::Path:
            m_paths[idx] = PathNodeData{};
            m_pathFreeList.push_back(idx);
            break;
        case NodeKind::Image:
            m_images[idx] = ImageNodeData{};
            m_imageFreeList.push_back(idx);
            break;
        case NodeKind::Text:
            m_texts[idx] = TextNodeData{};
            m_textFreeList.push_back(idx);
            break;
        case NodeKind::Element:
            m_elements[idx] = ElementData{};
            m_elementFreeList.push_back(idx);
            break;
        case NodeKind::Camera:
            m_cameras[idx] = CameraData{};
            m_cameraFreeList.push_back(idx);
            break;
        case NodeKind::External:
            m_externals[idx] = ExternalNodeData{};
            m_externalFreeList.push_back(idx);
            break;
        case NodeKind::Particles:
            m_particles[idx] = ParticleNodeData{};
            m_particleFreeList.push_back(idx);
            break;
        case NodeKind::Modifier:
            m_modifiers[idx] = ModifierNodeData{};
            m_modifierFreeList.push_back(idx);
            break;
        case NodeKind::Shape:
            m_shapes[idx] = ShapeNodeData{};
            m_shapeFreeList.push_back(idx);
            break;
        case NodeKind::Bone:  // T-243
            m_bones[idx] = BoneNodeData{};
            m_boneFreeList.push_back(idx);
            break;
        case NodeKind::Constraint:  // T-243
            m_constraints[idx] = ConstraintNodeData{};
            m_constraintFreeList.push_back(idx);
            break;
        case NodeKind::Mesh:  // T-243
            m_meshes[idx] = MeshNodeData{};
            m_meshFreeList.push_back(idx);
            break;
        case NodeKind::Group:
        case NodeKind::Mask:
        case NodeKind::Free:
        default:
            break;
    }
}

void NodeArena::pushChildrenReversed(NodeId id) const {
    size_t start = m_childScratch.size();
    NodeId c = m_firstChild[id];
    while (c != kInvalidNode) {
        m_childScratch.push_back(c);
        c = m_nextSibling[c];
    }
    for (size_t i = m_childScratch.size(); i > start; --i) {
        m_stack.push_back(m_childScratch[i - 1]);
    }
    m_childScratch.resize(start);
}

void NodeArena::collectSubtreePostorder(NodeId root, std::vector<NodeId>& out) const {
    m_stack.clear();
    m_stack.push_back(root);
    out.clear();
    while (!m_stack.empty()) {
        NodeId n = m_stack.back();
        m_stack.pop_back();
        out.push_back(n);
        NodeId c = m_firstChild[n];
        while (c != kInvalidNode) {
            m_stack.push_back(c);
            c = m_nextSibling[c];
        }
    }
    std::reverse(out.begin(), out.end());
}

NodeId NodeArena::create(NodeKind kind) {
    XT_ASSERT(kind != NodeKind::Free);
    NodeId id;
    if (!m_freeList.empty()) {
        id = m_freeList.back();
        m_freeList.pop_back();
        ++m_generation[id];
    } else {
        id = static_cast<NodeId>(m_kind.size());
        m_kind.push_back(NodeKind::Free);
        m_parent.push_back(kInvalidNode);
        m_firstChild.push_back(kInvalidNode);
        m_nextSibling.push_back(kInvalidNode);
        m_lastChild.push_back(kInvalidNode);
        m_common.push_back(NodeCommon{});
        m_kindIndex.push_back(0);
        m_dirty.push_back(0);
        m_generation.push_back(0);
        m_name.push_back(kInvalidString);
    }
    m_kind[id] = kind;
    m_parent[id] = kInvalidNode;
    m_firstChild[id] = kInvalidNode;
    m_nextSibling[id] = kInvalidNode;
    m_lastChild[id] = kInvalidNode;
    m_common[id] = NodeCommon{};
    m_dirty[id] = 0;
    m_name[id] = kInvalidString;
    m_kindIndex[id] = allocateData(kind);
    if (kind == NodeKind::Element) {
        initComputedStyleDefaults(m_elements[m_kindIndex[id]].style);
    }
    ++m_alive;
    return id;
}

void NodeArena::destroy(NodeId id) {
    checkAlive(id);
    NodeId p = m_parent[id];
    if (p != kInvalidNode) {
        NodeId prev = prevSibling(id);
        NodeId next = m_nextSibling[id];
        if (prev == kInvalidNode) {
            m_firstChild[p] = next;
        } else {
            m_nextSibling[prev] = next;
        }
        if (m_lastChild[p] == id) {
            m_lastChild[p] = prev;
        }
        markDirty(p, static_cast<uint8_t>(kDirtyTree | kDirtyLayout));
    }
    collectSubtreePostorder(id, m_destroyBuf);
    for (NodeId n : m_destroyBuf) {
        if (m_common[n].effectCount != 0) {
            countEffects(n, false);  // T-270b: its segment becomes a hole.
        }
        freeNodeData(n);
        m_kind[n] = NodeKind::Free;
        m_parent[n] = kInvalidNode;
        m_firstChild[n] = kInvalidNode;
        m_nextSibling[n] = kInvalidNode;
        m_lastChild[n] = kInvalidNode;
        m_dirty[n] = 0;
        m_name[n] = kInvalidString;
        m_freeList.push_back(n);
        --m_alive;
    }
}

void NodeArena::appendChild(NodeId parent, NodeId child) {
    checkAlive(parent);
    checkAlive(child);
    XT_ASSERT(m_parent[child] == kInvalidNode);
    m_parent[child] = parent;
    m_nextSibling[child] = kInvalidNode;
    if (m_lastChild[parent] == kInvalidNode) {
        m_firstChild[parent] = child;
    } else {
        m_nextSibling[m_lastChild[parent]] = child;
    }
    m_lastChild[parent] = child;
    markDirty(parent, static_cast<uint8_t>(kDirtyTree | kDirtyLayout));
}

void NodeArena::insertBefore(NodeId parent, NodeId child, NodeId before) {
    if (before == kInvalidNode) {
        appendChild(parent, child);
        return;
    }
    checkAlive(parent);
    checkAlive(child);
    checkAlive(before);
    XT_ASSERT(m_parent[before] == parent);
    XT_ASSERT(m_parent[child] == kInvalidNode);
    m_parent[child] = parent;
    if (m_firstChild[parent] == before) {
        m_nextSibling[child] = before;
        m_firstChild[parent] = child;
    } else {
        NodeId prev = prevSibling(before);
        m_nextSibling[prev] = child;
        m_nextSibling[child] = before;
    }
    markDirty(parent, static_cast<uint8_t>(kDirtyTree | kDirtyLayout));
}

void NodeArena::detach(NodeId child) {
    checkAlive(child);
    NodeId p = m_parent[child];
    XT_ASSERT(p != kInvalidNode);
    NodeId prev = prevSibling(child);
    NodeId next = m_nextSibling[child];
    if (prev == kInvalidNode) {
        m_firstChild[p] = next;
    } else {
        m_nextSibling[prev] = next;
    }
    if (m_lastChild[p] == child) {
        m_lastChild[p] = prev;
    }
    m_parent[child] = kInvalidNode;
    m_nextSibling[child] = kInvalidNode;
    markDirty(p, static_cast<uint8_t>(kDirtyTree | kDirtyLayout));
}

NodeId NodeArena::parent(NodeId id) const {
    checkAlive(id);
    return m_parent[id];
}

NodeId NodeArena::firstChild(NodeId id) const {
    checkAlive(id);
    return m_firstChild[id];
}

NodeId NodeArena::lastChild(NodeId id) const {
    checkAlive(id);
    return m_lastChild[id];
}

NodeId NodeArena::nextSibling(NodeId id) const {
    checkAlive(id);
    return m_nextSibling[id];
}

NodeId NodeArena::prevSibling(NodeId id) const {
    checkAlive(id);
    NodeId p = m_parent[id];
    if (p == kInvalidNode) {
        return kInvalidNode;
    }
    NodeId prev = kInvalidNode;
    NodeId c = m_firstChild[p];
    while (c != id) {
        prev = c;
        c = m_nextSibling[c];
    }
    return prev;
}

uint32_t NodeArena::childCount(NodeId id) const {
    checkAlive(id);
    uint32_t count = 0;
    for (NodeId c = m_firstChild[id]; c != kInvalidNode; c = m_nextSibling[c]) {
        ++count;
    }
    return count;
}

bool NodeArena::isAncestorOf(NodeId ancestor, NodeId node) const {
    checkAlive(ancestor);
    checkAlive(node);
    for (NodeId p = m_parent[node]; p != kInvalidNode; p = m_parent[p]) {
        if (p == ancestor) {
            return true;
        }
    }
    return false;
}

NodeKind NodeArena::kind(NodeId id) const {
    if (id >= m_kind.size()) {
        return NodeKind::Free;
    }
    return m_kind[id];
}

bool NodeArena::alive(NodeId id) const {
    if (id >= m_kind.size()) {
        return false;
    }
    return m_kind[id] != NodeKind::Free;
}

uint32_t NodeArena::generation(NodeId id) const {
    checkAlive(id);
    return m_generation[id];
}

NodeCommon& NodeArena::common(NodeId id) {
    checkAlive(id);
    return m_common[id];
}

const NodeCommon& NodeArena::common(NodeId id) const {
    checkAlive(id);
    return m_common[id];
}

PathNodeData& NodeArena::path(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Path);
    return m_paths[m_kindIndex[id]];
}

const PathNodeData& NodeArena::path(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Path);
    return m_paths[m_kindIndex[id]];
}

ImageNodeData& NodeArena::image(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Image);
    return m_images[m_kindIndex[id]];
}

const ImageNodeData& NodeArena::image(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Image);
    return m_images[m_kindIndex[id]];
}

TextNodeData& NodeArena::text(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Text);
    return m_texts[m_kindIndex[id]];
}

const TextNodeData& NodeArena::text(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Text);
    return m_texts[m_kindIndex[id]];
}

ElementData& NodeArena::element(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Element);
    return m_elements[m_kindIndex[id]];
}

const ElementData& NodeArena::element(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Element);
    return m_elements[m_kindIndex[id]];
}

CameraData& NodeArena::camera(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Camera);
    return m_cameras[m_kindIndex[id]];
}

const CameraData& NodeArena::camera(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Camera);
    return m_cameras[m_kindIndex[id]];
}

ExternalNodeData& NodeArena::external(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::External);
    return m_externals[m_kindIndex[id]];
}

const ExternalNodeData& NodeArena::external(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::External);
    return m_externals[m_kindIndex[id]];
}

ParticleNodeData& NodeArena::particles(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Particles);
    return m_particles[m_kindIndex[id]];
}

const ParticleNodeData& NodeArena::particles(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Particles);
    return m_particles[m_kindIndex[id]];
}

ModifierNodeData& NodeArena::modifier(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Modifier);
    return m_modifiers[m_kindIndex[id]];
}

const ModifierNodeData& NodeArena::modifier(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Modifier);
    return m_modifiers[m_kindIndex[id]];
}

ShapeNodeData& NodeArena::shape(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Shape);
    return m_shapes[m_kindIndex[id]];
}

const ShapeNodeData& NodeArena::shape(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Shape);
    return m_shapes[m_kindIndex[id]];
}

BoneNodeData& NodeArena::bone(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Bone);
    return m_bones[m_kindIndex[id]];
}

const BoneNodeData& NodeArena::bone(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Bone);
    return m_bones[m_kindIndex[id]];
}

ConstraintNodeData& NodeArena::constraint(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Constraint);
    return m_constraints[m_kindIndex[id]];
}

const ConstraintNodeData& NodeArena::constraint(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Constraint);
    return m_constraints[m_kindIndex[id]];
}

MeshNodeData& NodeArena::mesh(NodeId id) {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Mesh);
    return m_meshes[m_kindIndex[id]];
}

const MeshNodeData& NodeArena::mesh(NodeId id) const {
    checkAlive(id);
    XT_ASSERT(m_kind[id] == NodeKind::Mesh);
    return m_meshes[m_kindIndex[id]];
}

std::vector<EffectInstance>& NodeArena::effects() { return m_effects; }

const std::vector<EffectInstance>& NodeArena::effects() const { return m_effects; }

std::vector<GradientStop>& NodeArena::effectStops() { return m_effectStops; }

const std::vector<GradientStop>& NodeArena::effectStops() const { return m_effectStops; }

void NodeArena::reserveEffects(uint32_t effectCount, uint32_t stopCount) {
    m_effects.reserve(effectCount);
    m_effectStops.reserve(stopCount);
}

Status NodeArena::assignEffects(NodeId node, std::span<const EffectInstance> effects,
                                std::span<const GradientStop> stops) {
    checkAlive(node);
    if (effects.size() > 16) {
        return Status::kInvalidArgument;
    }
    NodeCommon& common = m_common[node];
    countEffects(node, false);  // T-270b: counted again below, with whatever the node ends up with.

    bool sameSequence = static_cast<uint32_t>(effects.size()) == common.effectCount;
    if (sameSequence) {
        for (uint32_t i = 0; i < effects.size(); ++i) {
            if (m_effects[common.effectOffset + i].kind != effects[i].kind) {
                sameSequence = false;
                break;
            }
        }
    }

    size_t effectsCapBefore = m_effects.capacity();
    size_t stopsCapBefore = m_effectStops.capacity();

    if (sameSequence) {
        // In place: overwrite each slot's scalar fields; rebase its stops in place when the new
        // stop count matches the old one, else append that slot's stops instead (18.2).
        for (uint32_t i = 0; i < effects.size(); ++i) {
            EffectInstance dst = effects[i];  // stopOffset/stopCount rewritten below.
            XT_ASSERT(static_cast<uint64_t>(effects[i].stopOffset) + effects[i].stopCount <= stops.size());
            const GradientStop* srcStops = stops.data() + effects[i].stopOffset;
            uint32_t srcStopCount = effects[i].stopCount;
            EffectInstance& slot = m_effects[common.effectOffset + i];
            if (srcStopCount == slot.stopCount) {
                for (uint32_t s = 0; s < srcStopCount; ++s) {
                    m_effectStops[slot.stopOffset + s] = srcStops[s];
                }
                dst.stopOffset = slot.stopOffset;
            } else {
                uint32_t stopBase = static_cast<uint32_t>(m_effectStops.size());
                m_effectStops.insert(m_effectStops.end(), srcStops, srcStops + srcStopCount);
                dst.stopOffset = stopBase;
            }
            dst.stopCount = srcStopCount;
            slot = dst;
        }
    } else {
        // Append: a whole new segment at the pool's end; the old segment becomes a hole.
        uint32_t newOffset = static_cast<uint32_t>(m_effects.size());
        for (uint32_t i = 0; i < effects.size(); ++i) {
            EffectInstance dst = effects[i];
            XT_ASSERT(static_cast<uint64_t>(effects[i].stopOffset) + effects[i].stopCount <= stops.size());
            const GradientStop* srcStops = stops.data() + effects[i].stopOffset;
            uint32_t srcStopCount = effects[i].stopCount;
            uint32_t stopBase = static_cast<uint32_t>(m_effectStops.size());
            m_effectStops.insert(m_effectStops.end(), srcStops, srcStops + srcStopCount);
            dst.stopOffset = stopBase;
            dst.stopCount = srcStopCount;
            m_effects.push_back(dst);
        }
        common.effectOffset = newOffset;
        common.effectCount = static_cast<uint32_t>(effects.size());
    }
    countEffects(node, true);

    if (m_effects.capacity() > effectsCapBefore || m_effectStops.capacity() > stopsCapBefore) {
        m_effectPoolGrew = true;
    }
    return Status::kOk;
}

void NodeArena::setEffectRange(NodeId node, uint32_t offset, uint32_t count) {
    checkAlive(node);
    XT_ASSERT(static_cast<uint64_t>(offset) + count <= m_effects.size());
    countEffects(node, false);
    m_common[node].effectOffset = offset;
    m_common[node].effectCount = count;
    countEffects(node, true);
}

void NodeArena::countEffects(NodeId node, bool add) {
    const NodeCommon& common = m_common[node];
    size_t stops = 0;
    for (uint32_t i = 0; i < common.effectCount; ++i) {
        stops += m_effects[common.effectOffset + i].stopCount;
    }
    if (add) {
        m_liveEffects += common.effectCount;
        m_liveEffectStops += stops;
    } else {
        m_liveEffects -= common.effectCount;
        m_liveEffectStops -= stops;
    }
}

void NodeArena::liveEffectCounts(size_t& effects, size_t& stops) const {
    effects = m_liveEffects;
    stops = m_liveEffectStops;
#if !defined(NDEBUG)
    // T-270b: the incremental counts against the full scan they replace.
    size_t scanEffects = 0;
    size_t scanStops = 0;
    uint32_t n = capacity();
    for (uint32_t id = 0; id < n; ++id) {
        if (!alive(id) || m_common[id].effectCount == 0) {
            continue;
        }
        const NodeCommon& common = m_common[id];
        scanEffects += common.effectCount;
        for (uint32_t i = 0; i < common.effectCount; ++i) {
            scanStops += m_effects[common.effectOffset + i].stopCount;
        }
    }
    XT_ASSERT(scanEffects == effects && scanStops == stops);
#endif
}

void NodeArena::compactEffects() {
    std::vector<EffectInstance> newEffects;
    std::vector<GradientStop> newStops;
    newEffects.reserve(m_effects.size());
    newStops.reserve(m_effectStops.size());

    uint32_t n = capacity();
    for (uint32_t id = 0; id < n; ++id) {
        if (!alive(id) || m_common[id].effectCount == 0) {
            continue;
        }
        NodeCommon& common = m_common[id];
        uint32_t newOffset = static_cast<uint32_t>(newEffects.size());
        for (uint32_t i = 0; i < common.effectCount; ++i) {
            EffectInstance e = m_effects[common.effectOffset + i];
            uint32_t stopBase = static_cast<uint32_t>(newStops.size());
            for (uint32_t s = 0; s < e.stopCount; ++s) {
                newStops.push_back(m_effectStops[e.stopOffset + s]);
            }
            e.stopOffset = stopBase;
            newEffects.push_back(e);
        }
        common.effectOffset = newOffset;
    }

    m_effects = std::move(newEffects);
    m_effectStops = std::move(newStops);
    XT_ASSERT(m_effects.size() == m_liveEffects && m_effectStops.size() == m_liveEffectStops);
}

StringId NodeArena::name(NodeId id) const {
    checkAlive(id);
    return m_name[id];
}

void NodeArena::setName(NodeId id, StringId name) {
    checkAlive(id);
    m_name[id] = name;
}

uint8_t NodeArena::dirty(NodeId id) const {
    checkAlive(id);
    return m_dirty[id];
}

void NodeArena::markDirty(NodeId id, uint8_t bits) {
    checkAlive(id);
    if (bits & static_cast<uint8_t>(kDirtyPaint)) {
        m_dirty[id] |= static_cast<uint8_t>(kDirtyPaint);
    }
    if (bits & static_cast<uint8_t>(kDirtyTree)) {
        m_dirty[id] |= static_cast<uint8_t>(kDirtyTree);
    }
    if (bits & static_cast<uint8_t>(kDirtyStyle)) {
        // Upward propagation stops at the first ancestor that already carries the
        // bit: flags are only cleared in bulk at the end of a frame, so every
        // ancestor above it must already be marked (see 01-architecture.md 1.4).
        for (NodeId n = id; n != kInvalidNode; n = m_parent[n]) {
            if (n != id && (m_dirty[n] & static_cast<uint8_t>(kDirtyStyle))) break;
            m_dirty[n] |= static_cast<uint8_t>(kDirtyStyle);
        }
    }
    if (bits & static_cast<uint8_t>(kDirtyLayout)) {
        NodeId n = id;
        m_dirty[n] |= static_cast<uint8_t>(kDirtyLayout);
        while (!stopsLayoutPropagation(n) && m_parent[n] != kInvalidNode) {
            n = m_parent[n];
            if (m_dirty[n] & static_cast<uint8_t>(kDirtyLayout))
                break;  // same early-exit rule as kDirtyStyle
            m_dirty[n] |= static_cast<uint8_t>(kDirtyLayout);
        }
    }
    if (bits & static_cast<uint8_t>(kDirtyTransform)) {
        m_stack.clear();
        m_stack.push_back(id);
        while (!m_stack.empty()) {
            NodeId n = m_stack.back();
            m_stack.pop_back();
            m_dirty[n] |= static_cast<uint8_t>(kDirtyTransform);
            for (NodeId c = m_firstChild[n]; c != kInvalidNode; c = m_nextSibling[c]) {
                m_stack.push_back(c);
            }
        }
    }
}

void NodeArena::markDirtyAncestors(NodeId id, uint8_t bits, NodeId stopAt) {
    checkAlive(id);
    NodeId n = id;
    for (;;) {
        markDirty(n, bits);
        if (n == stopAt) {
            break;
        }
        NodeId p = m_parent[n];
        if (p == kInvalidNode) {
            break;
        }
        n = p;
    }
}

void NodeArena::clearDirty(NodeId id) {
    checkAlive(id);
    m_dirty[id] = 0;
}

void NodeArena::clearDirtyBits(NodeId id, uint8_t bits) {
    checkAlive(id);
    m_dirty[id] = static_cast<uint8_t>(m_dirty[id] & ~bits);
}

bool NodeArena::anyDirty(uint8_t bits) const {
    for (uint32_t i = 0; i < m_dirty.size(); ++i) {
        if ((m_dirty[i] & bits) != 0 && alive(NodeId{i})) {
            return true;
        }
    }
    return false;
}

void NodeArena::clearAllDirty() { std::fill(m_dirty.begin(), m_dirty.end(), uint8_t{0}); }

uint32_t NodeArena::capacity() const { return static_cast<uint32_t>(m_kind.size()); }

uint32_t NodeArena::aliveCount() const { return m_alive; }

const SkinnedGeometry* NodeArena::skinned(NodeId id) const {
    if (id >= m_skinSlot.size() || m_skinSlot[id] == 0xFFFFFFFFu || !alive(id)) {
        return nullptr;
    }
    const SkinnedGeometry& g = m_skinned[m_skinSlot[id]];
    return g.generation == m_generation[id] ? &g : nullptr;
}

SkinnedGeometry& NodeArena::skinnedSlot(NodeId id) {
    XT_ASSERT(alive(id));
    if (id >= m_skinSlot.size()) {
        m_skinSlot.resize(static_cast<size_t>(id) + 1, 0xFFFFFFFFu);
    }
    if (m_skinSlot[id] == 0xFFFFFFFFu) {
        m_skinSlot[id] = static_cast<uint32_t>(m_skinned.size());
        m_skinned.emplace_back();
        m_skinned.back().generation = m_generation[id];
    }
    SkinnedGeometry& g = m_skinned[m_skinSlot[id]];
    if (g.generation != m_generation[id]) {  // The slot was reused by another node.
        g = SkinnedGeometry{};
        g.generation = m_generation[id];
    }
    return g;
}

void NodeArena::invalidateSkinned(NodeId id) {
    if (id < m_skinSlot.size() && m_skinSlot[id] != 0xFFFFFFFFu) {
        m_skinned[m_skinSlot[id]].skinnedVersion = 0xFFFFFFFFu;
        m_skinned[m_skinSlot[id]].valid = 0;
    }
}

}  // namespace xt
