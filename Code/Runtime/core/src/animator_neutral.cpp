#include <algorithm>

#include "xt/animator.h"
#include "xt/node_arena.h"
#include "xt/property.h"

/// T-869 (Docs/design/12-compiler.md §12.3): the base value a Neutral keyframe stands for -- the
/// property's value without the animation -- and the cascade values a CSS animation ending
/// without a `forwards` fill leaves behind. evaluate() (animator.cpp) calls these.

namespace xt {

namespace {

/// Whether `track` starts or ends with a Neutral key (the compiler writes one at 0% / 100% for a
/// property the `@keyframes` block leaves out there).
bool trackHasNeutralEnd(const Timeline& tl, const Track& track) {
    if (track.kfCount == 0 || track.kfOffset > tl.keyframes.size() ||
        track.kfCount > tl.keyframes.size() - track.kfOffset) {
        return false;
    }
    return tl.keyframes[track.kfOffset].value.isNeutral() ||
           tl.keyframes[track.kfOffset + track.kfCount - 1].value.isNeutral();
}

/// The general properties an Element's cascade keeps in ComputedStyle::base while the Animator
/// writes them elsewhere (setProperty() stores them in NodeCommon / ElementData), so base[] holds
/// the value without any animation (05.7's commit, kGeneralAnimatableProps).
bool styleKeepsBase(PropId p) {
    switch (p) {
        case prop::Opacity:
        case prop::TranslateX:
        case prop::TranslateY:
        case prop::ScaleX:
        case prop::ScaleY:
        case prop::Rotation:
        case prop::SkewX:
        case prop::SkewY:
        case prop::TranslateZ:
        case prop::AnchorZ:
        case prop::RotationX:
        case prop::RotationY:
            return true;
        default:
            return false;
    }
}

/// A value a Neutral key can take the place of (no pool data that the next write may move).
bool plainValue(const Value& v) {
    switch (v.type) {
        case ValueType::Float:
        case ValueType::Vec2:
        case ValueType::Vec3:
        case ValueType::Color:
        case ValueType::Length:
        case ValueType::Enum:
            return !v.isNeutral();
        default:
            return false;
    }
}

}  // namespace

Value Animator::neutralBase(const Instance& inst, const Timeline& tl, const Track& track, NodeId node,
                            NodeArena& arena) {
    if (!trackHasNeutralEnd(tl, track)) {
        return Value::neutral();
    }
    const PropId p = track.prop;
    // 04 §4.7: a layer below this instance (a transition, an instance that sorts earlier) already
    // queued its value for this frame; that is the value without this instance.
    for (size_t i = m_pending.size(); i > 0; --i) {
        const Pending& q = m_pending[i - 1];
        if (q.node == node && q.prop == p) {
            return plainValue(q.value) ? q.value : Value::neutral();
        }
    }
    if (!arena.alive(node) || !propertyApplies(arena.kind(node), p)) {
        return Value::neutral();
    }
    if (arena.kind(node) == NodeKind::Element && styleKeepsBase(p)) {
        return arena.element(node).style.base[p];  // The cascade's value; restyles keep it current.
    }
    if (isEffectProp(p) && ((static_cast<uint32_t>(p) >> 8) & 0xFu) >= arena.common(node).effectCount) {
        return Value::neutral();  // No such effect: the write would be a no-op anyway (18.2).
    }
    const Value current = getProperty(arena, node, p);
    if (!plainValue(current)) {
        return Value::neutral();
    }
    const uint32_t trackIndex = static_cast<uint32_t>(&track - tl.tracks.data());
    NeutralBase* entry = nullptr;
    for (NeutralBase& nb : m_neutralBases) {
        if (nb.instance == inst.id && nb.track == trackIndex) {
            entry = &nb;
            break;
        }
    }
    if (entry == nullptr) {
        m_neutralBases.push_back(NeutralBase{.instance = inst.id,
                                             .track = trackIndex,
                                             .node = node,
                                             .prop = p,
                                             .base = current,
                                             .applied = current,
                                             .hasApplied = 0,
                                             .used = 0});
        entry = &m_neutralBases.back();
    } else if (entry->node != node || (entry->hasApplied != 0 && !valueEquals(current, entry->applied))) {
        // Someone other than this instance wrote it since (a restyle, a host): the new base.
        entry->node = node;
        entry->base = current;
    }
    entry->used = 1;
    return entry->base;
}

void Animator::neutralBasesApplied(const NodeArena& arena) {
    // What the node holds after this frame's writes; a different value next time means a restyle
    // or a host wrote it -- the new base value.
    for (NeutralBase& nb : m_neutralBases) {
        if (nb.used != 0 && arena.alive(nb.node)) {
            nb.applied = getProperty(arena, nb.node, nb.prop);
            nb.hasApplied = 1;
        }
        nb.used = 0;
    }
}

void Animator::dropNeutralBases(uint32_t instance) {
    // `instance` 0: those of every instance no longer in the pool.
    if (m_neutralBases.empty()) {
        return;
    }
    m_neutralBases.erase(std::remove_if(m_neutralBases.begin(), m_neutralBases.end(),
                                        [this, instance](const NeutralBase& nb) {
                                            return instance != 0 ? nb.instance == instance
                                                                 : findInstance(nb.instance) == nullptr;
                                        }),
                         m_neutralBases.end());
}

void Animator::restoreCascadeValues(const Instance& inst, NodeArena& arena) {
    const Timeline* tl = inst.css != 0 ? timeline(inst.timeline) : nullptr;
    if (tl == nullptr) {
        return;
    }
    for (const Track& track : tl->tracks) {
        const NodeId node = trackNode(inst, track);
        if (node == kInvalidNode || !arena.alive(node) || arena.kind(node) != NodeKind::Element ||
            !styleKeepsBase(track.prop)) {
            continue;
        }
        const Value v = arena.element(node).style.base[track.prop];
        // T-725: of these, TranslateZ / AnchorZ / RotationX / RotationY (Float) are 3D components;
        // a zero one leaves the Element on its plane.
        const bool is3DProp = track.prop >= prop::TranslateZ && track.prop <= prop::Orientation;
        const bool may3D = !is3DProp || v.type != ValueType::Float || v.f != 0.0f;
        // Layer 0 (04 §4.7): first in the queue, so whatever else writes it this frame wins.
        m_pending.insert(m_pending.begin(),
                         Pending{node, track.prop, v, false, may3D, DirtyCause::Animation});
        arena.common(node).flags2 |= kAnimWriteQueued;
    }
}

}  // namespace xt
