#include "xt/scene_instance.h"

#include "xt/assert.h"

/// See Docs/tasks/T-08.md.

namespace xt {

namespace {

template <class Src, class Dst>
void copyPaint(const SceneChunk& chunk, const Src& src, Dst& dst);

void copyPath(const IrView& ir, const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena,
              NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.paths().size());
    const IrPathData& src = chunk.paths()[node.kindIndex];
    std::span<const uint8_t> verbPool = chunk.verbPool();
    std::span<const float> floatPool = chunk.floatPool();
    (void)ir;

    PathNodeData& dst = arena.path(id);

    XT_ASSERT(static_cast<uint64_t>(src.verbOffset) + src.verbCount <= verbPool.size());
    dst.verbs.resize(src.verbCount);
    for (uint32_t i = 0; i < src.verbCount; ++i) {
        dst.verbs[i] = static_cast<PathVerb>(verbPool[src.verbOffset + i]);
    }

    XT_ASSERT(static_cast<uint64_t>(src.pointOffset) + src.pointCount <= floatPool.size());
    dst.points.assign(floatPool.data() + src.pointOffset,
                      floatPool.data() + src.pointOffset + src.pointCount);

    copyPaint(chunk, src, dst);
}

/// The paint half of an IrPathData / IrShapeData (T-225: same field names) into a PathNodeData /
/// ShapeNodeData.
template <class Src, class Dst>
void copyPaint(const SceneChunk& chunk, const Src& src, Dst& dst) {
    std::span<const float> floatPool = chunk.floatPool();
    std::span<const IrGradientStop> stopPool = chunk.stopPool();

    dst.fillRule = static_cast<FillRule>(src.fillRule);

    dst.fill.kind = static_cast<PaintKind>(src.fillKind);
    dst.fill.color = src.fillColor;
    dst.fill.p0 = src.fillP0;
    dst.fill.p1 = src.fillP1;

    dst.stroke.kind = static_cast<PaintKind>(src.strokeKind);
    dst.stroke.color = src.strokeColor;
    dst.stroke.p0 = src.strokeP0;
    dst.stroke.p1 = src.strokeP1;

    // Fill stops first, then stroke stops, both copied into this node's local `stops` vector;
    // stopOffset/stopCount are rewritten to point into it (Docs/tasks/T-08.md rule 3).
    XT_ASSERT(static_cast<uint64_t>(src.fillStopOffset) + src.fillStopCount <= stopPool.size());
    XT_ASSERT(static_cast<uint64_t>(src.strokeStopOffset) + src.strokeStopCount <= stopPool.size());
    dst.stops.clear();
    dst.stops.reserve(static_cast<size_t>(src.fillStopCount) + src.strokeStopCount);
    for (uint32_t i = 0; i < src.fillStopCount; ++i) {
        const IrGradientStop& s = stopPool[src.fillStopOffset + i];
        dst.stops.push_back(GradientStop{s.offset, s.color});
    }
    for (uint32_t i = 0; i < src.strokeStopCount; ++i) {
        const IrGradientStop& s = stopPool[src.strokeStopOffset + i];
        dst.stops.push_back(GradientStop{s.offset, s.color});
    }
    dst.fill.stopOffset = 0;
    dst.fill.stopCount = static_cast<uint16_t>(src.fillStopCount);
    dst.stroke.stopOffset = static_cast<uint16_t>(src.fillStopCount);
    dst.stroke.stopCount = static_cast<uint16_t>(src.strokeStopCount);

    dst.strokeStyle.width = src.strokeWidth;
    dst.strokeStyle.cap = static_cast<LineCap>(src.cap);
    dst.strokeStyle.join = static_cast<LineJoin>(src.join);
    dst.strokeStyle.miterLimit = src.miterLimit;

    XT_ASSERT(static_cast<uint64_t>(src.dashOffset) + src.dashCount <= floatPool.size());
    dst.dashes.assign(floatPool.data() + src.dashOffset, floatPool.data() + src.dashOffset + src.dashCount);
    dst.strokeStyle.dashOffset = 0;
    dst.strokeStyle.dashCount = static_cast<uint16_t>(src.dashCount);
    dst.strokeStyle.dashPhase = src.dashPhase;

    // T-233: taper and width profile ((t, w) float pairs in the floatPool).
    dst.strokeStyle.taperStart = src.taperStart;
    dst.strokeStyle.taperEnd = src.taperEnd;
    XT_ASSERT(static_cast<uint64_t>(src.profileOffset) + uint64_t{src.profileCount} * 2 <= floatPool.size());
    dst.widthProfile.resize(src.profileCount);
    for (uint32_t i = 0; i < src.profileCount; ++i) {
        dst.widthProfile[i] =
            WidthPoint{floatPool[src.profileOffset + 2 * i], floatPool[src.profileOffset + 2 * i + 1]};
    }
    dst.strokeStyle.profileOffset = 0;
    dst.strokeStyle.profileCount = static_cast<uint16_t>(src.profileCount);

    dst.trimStart = src.trimStart;
    dst.trimEnd = src.trimEnd;
    dst.trimOffset = src.trimOffset;
}

/// T-225: a Modifier node's IrModifierData.
void copyModifier(const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena, NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.modifiers().size());
    const IrModifierData& src = chunk.modifiers()[node.kindIndex];
    ModifierNodeData& dst = arena.modifier(id);
    dst.kind = static_cast<ModifierKind>(src.kind);
    dst.mode = src.mode;
    dst.enabled = src.enabled;
    for (int i = 0; i < 8; ++i) {
        dst.params[i] = src.params[i];
    }
    dst.opacityA = src.opacityA;
    dst.opacityB = src.opacityB;
}

/// T-225: a Shape node's IrShapeData.
void copyShape(const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena, NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.shapes().size());
    const IrShapeData& src = chunk.shapes()[node.kindIndex];
    ShapeNodeData& dst = arena.shape(id);
    dst.kind = static_cast<ShapeKind>(src.kind);
    for (int i = 0; i < 12; ++i) {
        dst.params[i] = src.params[i];
    }
    copyPaint(chunk, src, dst);
}

/// T-243 (32 §32.2.2): skin `skinIndex` of `chunk` into `dst`, its node indices mapped through
/// `localToArena` (complete: called once every node exists).
void copySkin(const SceneChunk& chunk, uint32_t skinIndex, const std::vector<NodeId>& localToArena,
              SkinData& dst) {
    XT_ASSERT(skinIndex < chunk.skins().size());
    const IrSkinData& src = chunk.skins()[skinIndex];
    XT_ASSERT(src.skeleton < localToArena.size());
    dst.skeleton = localToArena[src.skeleton];
    dst.tendons.resize(src.tendonCount);
    for (uint32_t i = 0; i < src.tendonCount; ++i) {
        const IrTendon& t = chunk.tendonPool()[src.tendonOffset + i];
        XT_ASSERT(t.bone < localToArena.size());
        dst.tendons[i].bone = localToArena[t.bone];
        dst.tendons[i].inverseBind = Mat2x3{t.inverseBind[0], t.inverseBind[1], t.inverseBind[2],
                                            t.inverseBind[3], t.inverseBind[4], t.inverseBind[5]};
    }
    dst.weights.resize(src.weightCount);
    for (uint32_t i = 0; i < src.weightCount; ++i) {
        const IrPointWeight& w = chunk.weightPool()[src.weightOffset + i];
        dst.weights[i] = PointWeight{w.idx4, w.w4};
    }
}

/// T-243: a Bone node's IrBoneData.
void copyBone(const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena, NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.bones().size());
    arena.bone(id).length = chunk.bones()[node.kindIndex].length;
}

/// T-243: a Constraint node's IrConstraintData, but for its target (needs every node; see
/// resolveRig()).
void copyConstraint(const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena, NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.constraints().size());
    const IrConstraintData& src = chunk.constraints()[node.kindIndex];
    ConstraintNodeData& dst = arena.constraint(id);
    dst.kind = static_cast<ConstraintKind>(src.kind);
    dst.space = src.space;
    dst.enabled = src.enabled;
    for (int i = 0; i < 8; ++i) {
        dst.params[i] = src.params[i];
    }
    dst.mix = src.mix;
}

/// T-243: a Mesh node's IrMeshData (positions, uvs, indices), but for its skin (see resolveRig()).
void copyMesh(const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena, NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.meshes().size());
    const IrMeshData& src = chunk.meshes()[node.kindIndex];
    MeshNodeData& dst = arena.mesh(id);
    dst.asset = src.asset;
    std::span<const Vec2> pool = chunk.meshVec2Pool();
    XT_ASSERT(static_cast<uint64_t>(src.posOffset) + uint64_t{src.vertexCount} * 2 <= pool.size());
    dst.positions.assign(pool.begin() + src.posOffset, pool.begin() + src.posOffset + src.vertexCount);
    dst.uvs.assign(
        pool.begin() + src.posOffset + src.vertexCount,
        pool.begin() + src.posOffset + static_cast<std::ptrdiff_t>(2 * static_cast<size_t>(src.vertexCount)));
    std::span<const uint16_t> indices = chunk.meshIndexPool();
    XT_ASSERT(static_cast<uint64_t>(src.indexOffset) + src.indexCount <= indices.size());
    dst.indices.assign(indices.begin() + src.indexOffset, indices.begin() + src.indexOffset + src.indexCount);
    dst.opacity = src.opacity;
    dst.smooth = src.smooth;
}

/// T-243: the node references of the rig -- constraint targets and the skins of Path / Mesh
/// nodes -- once `out.localToArena` covers every node of the scene.
void resolveRig(const SceneChunk& chunk, NodeArena& arena, const SceneInstance& out) {
    std::span<const IrSceneNode> nodes = chunk.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        const IrSceneNode& node = nodes[i];
        const NodeId id = out.localToArena[i];
        switch (static_cast<NodeKind>(node.kind)) {
            case NodeKind::Constraint: {
                const uint32_t target = chunk.constraints()[node.kindIndex].target;
                arena.constraint(id).target = target != kIrNoTarget && target < out.localToArena.size()
                                                  ? out.localToArena[target]
                                                  : kInvalidNode;
                break;
            }
            case NodeKind::Path: {
                const uint32_t skin = chunk.paths()[node.kindIndex].skinIndex;
                if (skin != kIrNoSkin) {
                    copySkin(chunk, skin, out.localToArena, arena.path(id).skin);
                }
                break;
            }
            case NodeKind::Mesh: {
                const uint32_t skin = chunk.meshes()[node.kindIndex].skinIndex;
                if (skin != kIrNoSkin) {
                    copySkin(chunk, skin, out.localToArena, arena.mesh(id).skin);
                }
                break;
            }
            default:
                break;
        }
    }
}

void copyImage(const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena, NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.images().size());
    const IrImageData& src = chunk.images()[node.kindIndex];
    ImageNodeData& dst = arena.image(id);
    dst.asset = src.asset;
    dst.width = src.width;
    dst.height = src.height;
}

void copyCamera(const SceneChunk& chunk, const IrSceneNode& node, NodeArena& arena, NodeId id) {
    XT_ASSERT(node.kindIndex < chunk.cameras().size());
    const IrCameraData& src = chunk.cameras()[node.kindIndex];
    CameraData& dst = arena.camera(id);
    dst.kind = src.kind;
    dst.useTarget = src.useTarget;
    dst.zoom = src.zoom;
    dst.target = src.target;
    dst.nearZ = src.nearZ;
    dst.farZ = src.farZ;
}

/// Copies every scene effect (Docs/design/18-effects.md §18.2) into the arena's global effect
/// pool once per instantiateScene() call, rebasing GradientMask stop offsets into
/// NodeArena::effectStops(). Returns the base offset to add to each IrSceneNode::effectOffset
/// when writing NodeCommon::effectOffset (§18.2's pool only grows, so this call's effects are
/// simply appended after whatever another instance already put there).
uint32_t copySceneEffects(const SceneChunk& chunk, NodeArena& arena) {
    uint32_t base = static_cast<uint32_t>(arena.effects().size());
    std::span<const IrEffect> srcEffects = chunk.effects();
    std::span<const IrGradientStop> stopPool = chunk.stopPool();
    for (const IrEffect& src : srcEffects) {
        EffectInstance fx{};
        fx.kind = static_cast<EffectKind>(src.kind);
        fx.enabled = src.enabled;
        fx.shader = src.shader;
        for (int i = 0; i < 8; ++i) {
            fx.params[i] = src.params[i];
        }
        fx.colors[0] = src.colors[0];
        fx.colors[1] = src.colors[1];

        XT_ASSERT(static_cast<uint64_t>(src.stopOffset) + src.stopCount <= stopPool.size());
        uint32_t stopBase = static_cast<uint32_t>(arena.effectStops().size());
        for (uint32_t i = 0; i < src.stopCount; ++i) {
            const IrGradientStop& s = stopPool[src.stopOffset + i];
            arena.effectStops().push_back(GradientStop{s.offset, s.color});
        }
        fx.stopOffset = stopBase;
        fx.stopCount = src.stopCount;

        arena.effects().push_back(fx);
    }
    return base;
}

}  // namespace

Status instantiateScene(const IrView& ir, SceneId scene, NodeArena& arena, NodeId parent,
                        SceneInstance& out) {
    XT_ASSERT(arena.alive(parent));
    const SceneChunk& chunk = ir.scene(scene);

    out = SceneInstance{};
    out.scene = scene;
    out.width = chunk.width();
    out.height = chunk.height();
    out.name = chunk.name();
    out.localToArena.clear();
    out.localToArena.reserve(chunk.nodes().size());

    // 18.2's effect pool only grows; copy this scene's effects once and rebase every node's
    // effectOffset by this call's base (see copySceneEffects()).
    uint32_t effectsBase = copySceneEffects(chunk, arena);

    std::span<const IrSceneNode> nodes = chunk.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        const IrSceneNode& node = nodes[i];
        NodeKind kind = static_cast<NodeKind>(node.kind);
        XT_ASSERT(kind == NodeKind::Group || kind == NodeKind::Path || kind == NodeKind::Image ||
                  kind == NodeKind::Mask || kind == NodeKind::Camera || kind == NodeKind::Modifier ||
                  kind == NodeKind::Shape || kind == NodeKind::Bone || kind == NodeKind::Constraint ||
                  kind == NodeKind::Mesh);

        NodeId id = arena.create(kind);

        NodeCommon& common = arena.common(id);
        common.opacity = node.opacity;
        common.anchor = node.anchor;
        common.translate = node.translate;
        common.scale = node.scale;
        common.rotation = node.rotation;
        common.skew = node.skew;
        common.visible = node.visible;
        common.blend = node.blend;
        common.maskMode = node.maskMode;
        common.autoOrient = node.autoOrient != 0 ? 1 : 0;  // T-228 (Lottie `ao`).
        common.local = composeTRSS(node.translate, node.rotation, node.skew, node.scale, node.anchor);
        common.world = common.local;
        // 19.3's 3D fields; is3D/backfaceHidden come straight from the IR for a scene node (they
        // are runtime-derived only for Element, see Docs/design/19-3d-space.md §19.3).
        common.translateZ = node.translateZ;
        common.anchorZ = node.anchorZ;
        common.rotationX = node.rotationX;
        common.rotationY = node.rotationY;
        common.orientation = node.orientation;
        common.is3D = node.is3D;
        common.backfaceHidden = node.backfaceHidden;
        // 18.2's per-node effect range, rebased into NodeArena::effects() (T-270b: through
        // setEffectRange(), which keeps the arena's live effect counts).
        arena.setEffectRange(id, effectsBase + node.effectOffset, node.effectCount);

        arena.setName(id, node.name);

        if (kind == NodeKind::Path) {
            copyPath(ir, chunk, node, arena, id);
        } else if (kind == NodeKind::Image) {
            copyImage(chunk, node, arena, id);
        } else if (kind == NodeKind::Camera) {
            copyCamera(chunk, node, arena, id);
        } else if (kind == NodeKind::Modifier) {
            copyModifier(chunk, node, arena, id);
        } else if (kind == NodeKind::Shape) {
            copyShape(chunk, node, arena, id);
        } else if (kind == NodeKind::Bone) {
            copyBone(chunk, node, arena, id);  // T-243
        } else if (kind == NodeKind::Constraint) {
            copyConstraint(chunk, node, arena, id);
        } else if (kind == NodeKind::Mesh) {
            copyMesh(chunk, node, arena, id);
        }

        if (node.parent == 0xFFFFFFFFu) {
            XT_ASSERT(i == 0);
            arena.appendChild(parent, id);
        } else {
            XT_ASSERT(node.parent < out.localToArena.size());
            arena.appendChild(out.localToArena[node.parent], id);
        }

        out.localToArena.push_back(id);
    }

    // T-243: constraint targets and skins reference nodes anywhere in the scene (a skeleton may
    // follow the shapes it deforms), so they are resolved once every node exists.
    resolveRig(chunk, arena, out);

    out.root = out.localToArena.empty() ? kInvalidNode : out.localToArena[0];
    out.base = out.root;
    return Status::kOk;
}

void destroySceneInstance(NodeArena& arena, SceneInstance& inst) {
    if (inst.root != kInvalidNode) {
        arena.destroy(inst.root);
    }
    inst = SceneInstance{};
}

NodeId resolveTrackTarget(const SceneInstance& inst, NodeId localTarget) {
    if (localTarget == kSelfNode) {
        return kSelfNode;
    }
    if (localTarget >= inst.localToArena.size()) {
        return kInvalidNode;
    }
    return inst.localToArena[localTarget];
}

}  // namespace xt
