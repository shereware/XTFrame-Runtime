#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "binding_runtime.h"
#include "event_dispatcher.h"
#include "repeat_controller.h"
#include "rig_eval.h"
#include "style_decode.h"
#include "text_edit.h"
#include "trigger_runtime.h"
#include "xt/assert.h"
#include "xt/asset_resolver.h"
#include "xt/charconv.h"
#include "xt/document.h"
#include "xt/text_shaper.h"

/// Document::loadInternal: DOCT -> Element/Text nodes; `anim` elements -> instantiateScene;
/// TLIN -> the Timeline pool. See Docs/tasks/T-18.md "載入流程".

namespace xt {

namespace {

/// Linear search of `data.attributes` by name; returns an empty view if not present.
std::string_view findAttribute(const IrView& ir, const ElementData& data, std::string_view name) {
    for (const auto& entry : data.attributes) {
        if (ir.string(entry.first) == name) {
            return ir.string(entry.second);
        }
    }
    return std::string_view{};
}

/// Parses `text` as a float; returns `fallback` when empty or malformed (never throws: core has no
/// exceptions).
float parseFloatOr(std::string_view text, float fallback) {
    if (text.empty()) {
        return fallback;
    }
    float value = fallback;
    std::from_chars_result result = xt::fromChars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return fallback;
    }
    return value;
}

/// Parses `text` as an unsigned decimal AssetId; returns kInvalidAsset on empty/malformed input
/// (never throws: core has no exceptions). Mirrors layout_engine.cpp's parseAssetIdOr (T-27:
/// resolved once here instead of per-frame in the Painter).
AssetId parseAssetIdOr(std::string_view text) {
    if (text.empty()) {
        return kInvalidAsset;
    }
    uint32_t value = 0;
    std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return kInvalidAsset;
    }
    return value;
}

}  // namespace

void Document::loadTextRuns(uint32_t textIndex, NodeId textNode) {
    // T-178 (07 §7.8): the IR run table is grouped by text in ascending order (the compiler emits
    // texts and their runs in the same element order), so the text's runs are one contiguous
    // range found by binary search.
    std::span<const IrTextRun> runs = m_ir.doc().textRuns();
    if (runs.empty()) {
        return;
    }
    auto first = std::lower_bound(runs.begin(), runs.end(), textIndex,
                                  [](const IrTextRun& run, uint32_t text) { return run.text < text; });
    std::span<const IrTextInline> inlines = m_ir.doc().textInlines();
    TextNodeData& data = m_arena.text(textNode);
    bool hasGlyph = false;
    for (auto it = first; it != runs.end() && it->text == textIndex; ++it) {
        TextStyleRun run;
        run.start = it->start;
        run.end = it->end;
        run.declOffset = it->declOffset;
        run.declCount = it->declCount;
        run.linkIndex = it->link;
        if (it->inlineObject != 0xFFFFFFFFu && it->inlineObject < inlines.size()) {
            const IrTextInline& src = inlines[it->inlineObject];
            TextInline object;
            object.kind = src.kind == 1 ? TextInlineKind::Glyph : TextInlineKind::Image;
            if (object.kind == TextInlineKind::Glyph) {
                object.action = src.ref;
                hasGlyph = true;
            } else {
                object.asset = src.ref < m_ir.assets().assets().size() ? src.ref : kInvalidAsset;
            }
            object.height = src.height;
            data.inlines.push_back(object);
            run.inlineNode = static_cast<uint32_t>(data.inlines.size());
        }
        data.styleRuns.push_back(run);
    }
    if (hasGlyph) {
        m_glyphTextNodes.push_back(textNode);
    }
}

void Document::loadRigs() {
    m_rigs->clear();
    for (const SceneInstance& inst : m_instances) {
        if (inst.scene == kInvalidScene || inst.root == kInvalidNode) {
            continue;
        }
        std::span<const IrSceneNode> nodes = m_ir.scene(inst.scene).nodes();
        const size_t count = std::min(nodes.size(), inst.localToArena.size());
        for (size_t i = 0; i < count; ++i) {
            const NodeId node = inst.localToArena[i];
            if (nodes[i].svgTag != static_cast<uint8_t>(IrSvgTag::XtSkeleton) || node == kInvalidNode ||
                !m_arena.alive(node)) {
                continue;
            }
            m_rigs->emplace_back();
            rig::buildRigState(m_arena, node, inst.localToArena, m_rigs->back());
        }
    }
}

Status Document::loadInternal(IrBlob blob, bool takeOwnership, TextShaper* shaper, AssetResolver* assets) {
    m_shaper = shaper;
    m_assets = assets;

    IrBlob viewBlob = blob;
    if (takeOwnership) {
        m_ownedBlob.assign(blob.data, blob.data + blob.size);
        viewBlob = IrBlob{m_ownedBlob.data(), m_ownedBlob.size()};
    }

    Result<IrView> opened = IrView::open(viewBlob, IrReadOptions{/*verifyCrc=*/false});
    if (!opened.ok()) {
        return opened.status();
    }
    m_ir = opened.value();
    m_shaderTable.build(m_ir);  // T-203 (18.7).

    // Docs/design/18-effects.md §18.2: size the effect/effect-stop pools up front (2x the
    // compiled total) so ordinary effect reassignment (StyleEngine re-resolve, setEffects()) does
    // not reallocate mid-frame (01.7's zero-alloc goal; Docs/tasks/T-50.md). SCEN's stopPool() is
    // shared by paint properties and effects (ir.h), so this slightly over-reserves stops rather
    // than under-reserving.
    {
        uint32_t totalEffects = static_cast<uint32_t>(m_ir.style().effects().size());
        uint32_t totalStops = static_cast<uint32_t>(m_ir.style().effectStops().size());
        for (uint32_t s = 0; s < m_ir.sceneCount(); ++s) {
            const SceneChunk& chunk = m_ir.scene(s);
            totalEffects += static_cast<uint32_t>(chunk.effects().size());
            totalStops += static_cast<uint32_t>(chunk.stopPool().size());
        }
        m_arena.reserveEffects(totalEffects * 2, totalStops * 2);
    }

    // Step 2 (Docs/tasks/T-18.md): timelines. Reserve to the exact count first so the pointers
    // Animator::registerTimeline() stores stay valid across the whole loop (Timeline is move-only
    // and Animator only borrows the pointer; a mid-loop reallocation would invalidate it).
    uint32_t timelineCount = m_ir.timelineCount();
    m_timelines.reserve(timelineCount);
    for (uint32_t i = 0; i < timelineCount; ++i) {
        m_timelines.emplace_back();
        Status status = timelineFromIr(m_ir.timeline(i), m_timelines.back());
        if (status != Status::kOk) {
            return status;
        }
        TimelineId id = m_animator.registerTimeline(&m_timelines.back());
        XT_ASSERT(id == i);
        (void)id;
    }
    // T-335: the `linear()` easing points every STYL / TLIN LinearPoints easing indexes.
    m_animator.setEasingPoints(m_ir.style().easingPoints());

    m_animator.onFinished = [this](uint32_t id) { this->handleAnimationFinished(id); };
    m_animator.onIteration = [this](uint32_t id, uint32_t iter) { this->handleAnimationIteration(id, iter); };
    m_animator.onMarker = [this](uint32_t id, StringId marker) { this->handleAnimationMarker(id, marker); };
    m_animator.onTransitionEnd = [this](NodeId node, PropId p) { this->handleTransitionEnd(node, p); };

    // Step 3: Element / Text nodes, depth-first preorder (matches DOCT layout: parent before child).
    const DocChunk& doc = m_ir.doc();
    std::span<const IrDocElement> elements = doc.elements();
    std::span<const uint32_t> classPool = doc.classPool();
    std::span<const IrAttr> attrPool = doc.attrPool();
    std::span<const IrDocText> texts = doc.texts();
    std::span<const IrDecl> inlineDecls = doc.inlineDecls();
    std::span<const IrCameraData> docCameras = doc.cameras();
    // T-179: `@key` references (STRS), each empty or parallel to texts[] / attrPool[].
    std::span<const uint32_t> l10nKeys = m_ir.localization().keys();
    std::span<const uint32_t> textKeys = m_ir.localization().textKeys();
    std::span<const uint32_t> attrKeys = m_ir.localization().attrKeys();
    m_docElementNodes.reserve(elements.size());

    for (uint32_t i = 0; i < elements.size(); ++i) {
        const IrDocElement& src = elements[i];
        NodeId node = m_arena.create(NodeKind::Element);
        ElementData& data = m_arena.element(node);

        data.tag = src.tag;
        data.id = src.id;

        data.classes.assign(classPool.begin() + src.classOffset,
                            classPool.begin() + src.classOffset + src.classCount);
        std::sort(data.classes.begin(), data.classes.end());

        data.attributes.reserve(src.attrCount);
        for (uint32_t a = 0; a < src.attrCount; ++a) {
            const IrAttr& attr = attrPool[src.attrOffset + a];
            data.attributes.emplace_back(attr.name, attr.value);
            const uint32_t attrIndex = src.attrOffset + a;
            if (attrIndex < attrKeys.size() && attrKeys[attrIndex] != 0) {
                LocalizedSlot slot;
                slot.node = node;
                slot.generation = m_arena.generation(node);
                slot.attrIndex = a;
                slot.key = l10nKeys[attrKeys[attrIndex] - 1];
                slot.shown = attr.value;
                m_localized.push_back(slot);
            }
            if (m_ir.string(attr.name) == "style") {
                data.inlineStyle = attr.value;
            }
        }

        data.inlineDeclOffset = src.inlineDeclOffset;
        data.inlineDeclCount = src.inlineDeclCount;

        // T-27: resolve `img`'s source asset once here (from `data-xt-asset`, the same attribute
        // LayoutEngine::intrinsicSize reads) instead of every Painter frame.
        data.imageAsset = parseAssetIdOr(findAttribute(m_ir, data, "data-xt-asset"));
        // T-199 (26.3.4): an asset packed into a bundle atlas draws its cell only.
        if (data.imageAsset != kInvalidAsset && m_assets != nullptr) {
            data.imageSubUv = m_assets->imageSubUv(data.imageAsset, m_ir);
        }

        // Docs/design/19-3d-space.md §19.4: a <camera> element gets a NodeKind::Camera child
        // carrying its CameraData; the element itself is never laid out/drawn/hit-tested
        // (layout_block.cpp/layout_flex.cpp/layout_absolute.cpp/hit_test.cpp check cameraNode).
        if (src.cameraIndex != 0xFFFFFFFFu) {
            XT_ASSERT(src.cameraIndex < docCameras.size());
            if (src.cameraIndex < docCameras.size()) {
                const IrCameraData& irCam = docCameras[src.cameraIndex];
                NodeId camNode = m_arena.create(NodeKind::Camera);
                CameraData& cam = m_arena.camera(camNode);
                cam.kind = irCam.kind;
                cam.useTarget = irCam.useTarget;
                // zoom/target may be NaN ("omitted"); refreshCameraDefaults() fills them in from
                // the parent border-box after every layout (19.4's "省略的屬性...重算預設值").
                cam.zoom = irCam.zoom;
                cam.target = irCam.target;
                cam.nearZ = std::isnan(irCam.nearZ) ? 1.0f : irCam.nearZ;
                cam.farZ = std::isnan(irCam.farZ) ? 100000.0f : irCam.farZ;
                // T-47a: irCam.position.x/y may be NaN ("position" attribute entirely omitted --
                // refreshCameraDefaults() recomputes translate/translateZ together from the
                // parent border-box every layout, 19.4); .z is never NaN when .x/.y aren't (the
                // compiler writes 0 for an omitted third component, "可省略 z -> 0").
                NodeCommon& camCommon = m_arena.common(camNode);
                camCommon.translate = Vec2{irCam.position.x, irCam.position.y};
                camCommon.translateZ = irCam.position.z;
                m_arena.appendChild(node, camNode);
                data.cameraNode = camNode;
            }
        }

        // T-191 (03 §3.12.1): external content gets its NodeKind::External child.
        if (data.tag != kInvalidString) {
            const std::string_view mediaTag = m_ir.string(data.tag);
            if (mediaTag == "video" || mediaTag == "sequence" || mediaTag == "external") {
                loadExternal(node);
            }
            if (mediaTag == "particles") {
                loadParticles(node);  // T-202 (03 §3.12.5).
            }
        }

        // T-33 (Docs/tasks/T-33.md "可 focus"): the static half of the focusability rule.
        std::string_view tag = data.tag == kInvalidString ? std::string_view{} : m_ir.string(data.tag);
        bool isInput = tag == "input";
        bool hasTabIndex = false;
        for (const auto& entry : data.attributes) {
            if (m_ir.string(entry.first) == "tabindex") {
                hasTabIndex = true;
                break;
            }
        }
        data.focusable = (tag == "button" || isInput || hasTabIndex) ? 1 : 0;

        // T-194 (05.9.1, 27.7, 08.9): `draggable`, `drop-target`, `autofocus` (bare = true) and
        // `nav:<dir>` (the compiler strips the `#`, so the value is the target id's StringId).
        // Draggable, drop-target and autofocus elements are focusable (gamepad drag and drop moves
        // the focus between them; autofocus is the surface's initial focus).
        for (const auto& entry : data.attributes) {
            const std::string_view name = m_ir.string(entry.first);
            const std::string_view value = m_ir.string(entry.second);
            const bool on = value != "false";
            NodeCommon& common = m_arena.common(node);
            if (name == "draggable" && value == "axis-x") {
                common.flags2 |= kDragAxisX;  // T-201: a slider track, not a drag source.
                data.focusable = 1;
            } else if (name == "draggable" && on) {
                common.flags2 |= kDraggable;
                data.focusable = 1;
            } else if (name == "drop-target" && on) {
                common.flags2 |= kDropTarget;
                data.focusable = 1;
            } else if (name == "key-activation" && tag == "body") {
                m_keyActivation = value != "none";  // T-379 (08.10).
            } else if (name == "focus-trap" && on) {
                common.flags2 |= kFocusTrap;  // T-201: the markup form of setFocusTrap (08.9).
            } else if (name == "autofocus" && on) {
                data.focusable = 1;
                m_autofocus.push_back(node);
            } else if (name == "nav:up") {
                common.navUp = entry.second + 1;
            } else if (name == "nav:down") {
                common.navDown = entry.second + 1;
            } else if (name == "nav:left") {
                common.navLeft = entry.second + 1;
            } else if (name == "nav:right") {
                common.navRight = entry.second + 1;
            }
        }

        // 05.2 (T-35 "附帶修正"): the `disabled`/`checked` attributes seed their ElementState bits.
        if (findAttribute(m_ir, data, "disabled") == "true") {
            data.stateBits = static_cast<uint16_t>(data.stateBits | static_cast<uint16_t>(kStateDisabled));
        }
        if (findAttribute(m_ir, data, "checked") == "true") {
            data.stateBits = static_cast<uint16_t>(data.stateBits | static_cast<uint16_t>(kStateChecked));
        }
        // T-385 (27 §27.3.2): `<use state="A B">` (compiled into `data-xt-state`) seeds the instance
        // root's custom states, without enter triggers.
        if (const std::string_view seeded = findAttribute(m_ir, data, "data-xt-state"); !seeded.empty()) {
            const std::span<const uint32_t> names = m_ir.customStates().names();
            size_t pos = 0;
            while (pos < seeded.size()) {
                const size_t end = std::min(seeded.find(' ', pos), seeded.size());
                const std::string_view word = seeded.substr(pos, end - pos);
                for (uint32_t bit = 0; !word.empty() && bit < names.size() && bit < kMaxCustomStates; ++bit) {
                    if (m_ir.string(names[bit]) == word) {
                        m_arena.common(node).customStates |= 1u << bit;
                        break;
                    }
                }
                pos = end + 1;
            }
        }
        // 08.8 (T-35 "附帶修正"): an `input` is always laid out horizontally; a `writing-mode` on
        // one is ignored (StyleEngine::resolve forces it back to horizontal-tb, whatever the
        // source) and, when the element declares it itself, reported as a load warning.
        if (isInput) {
            for (uint32_t d = 0; d < src.inlineDeclCount; ++d) {
                const IrDecl& decl = inlineDecls[src.inlineDeclOffset + d];
                if (decl.prop == style_decode::styleprop::WritingMode && irValueU32(decl.value, 0) != 0) {
                    m_warnings.push_back("input: writing-mode is not supported (08.8), ignored");
                    break;
                }
            }
        }

        if (i == 0) {
            XT_ASSERT(src.parent == 0xFFFFFFFFu);
            m_root = node;
        } else {
            XT_ASSERT(src.parent < m_docElementNodes.size());
            m_arena.appendChild(m_docElementNodes[src.parent], node);
        }
        m_docElementNodes.push_back(node);

        NodeId textNode = kInvalidNode;
        if (src.firstTextChild != 0xFFFFFFFFu) {
            const IrDocText& text = texts[src.firstTextChild];
            textNode = m_arena.create(NodeKind::Text);
            m_arena.text(textNode).text = text.text;
            m_arena.appendChild(node, textNode);
            if (src.firstTextChild < textKeys.size() && textKeys[src.firstTextChild] != 0 && !isInput) {
                LocalizedSlot slot;
                slot.node = textNode;
                slot.generation = m_arena.generation(textNode);
                slot.key = l10nKeys[textKeys[src.firstTextChild] - 1];
                slot.shown = text.text;
                m_localized.push_back(slot);
            }
            loadTextRuns(src.firstTextChild, textNode);  // T-178: rich-text runs.
        }

        // T-33: an editable `input` gets an EditState (initialized from its `value` attribute, or
        // from its text content when it has no such attribute) and, if the IR gave it no text
        // child, one created here to carry the laid-out value.
        if (isInput) {
            std::string_view type = findAttribute(m_ir, data, "type");
            if (type.empty() || type == "text") {
                if (textNode == kInvalidNode) {
                    textNode = m_arena.create(NodeKind::Text);
                    m_arena.appendChild(node, textNode);
                }
                EditState state;
                bool hasValueAttr = false;
                for (const auto& entry : data.attributes) {
                    if (m_ir.string(entry.first) == "value") {
                        state.value.assign(m_ir.string(entry.second));
                        hasValueAttr = true;
                        break;
                    }
                }
                if (!hasValueAttr && m_arena.text(textNode).text != kInvalidString) {
                    state.value.assign(m_ir.string(m_arena.text(textNode).text));
                }
                m_arena.text(textNode).editIndex = static_cast<uint32_t>(m_editStates->size());
                m_editStates->push_back(std::move(state));
            }
        }
    }

    // Step 4: `anim` elements -> instantiateScene + autoplay.
    for (uint32_t i = 0; i < elements.size(); ++i) {
        const IrDocElement& src = elements[i];
        if (src.scene == kInvalidScene) {
            continue;
        }
        NodeId elementNode = m_docElementNodes[i];
        ElementData& data = m_arena.element(elementNode);

        SceneInstance inst;
        Status status = instantiateScene(m_ir, src.scene, m_arena, elementNode, inst);
        XT_ASSERT(status == Status::kOk);
        (void)status;

        data.scene = src.scene;
        data.sceneRoot = inst.root;
        data.sceneInstanceIndex = static_cast<uint32_t>(m_instances.size());
        m_instances.push_back(std::move(inst));
        const SceneInstance& storedInst = m_instances.back();

        // T-110: an `<img src="*.svg">` scene is static (its timeline is empty), so autoplay and
        // the loop/segment/speed attributes have no meaning there.
        if (m_ir.string(data.tag) != "anim") {
            continue;
        }

        std::string_view autoplayAttr = findAttribute(m_ir, data, "autoplay");
        bool autoplay = autoplayAttr.empty() || autoplayAttr == "true";
        if (!autoplay) {
            continue;
        }

        std::string_view loopAttr = findAttribute(m_ir, data, "loop");
        LoopMode loop = LoopMode::Once;
        if (loopAttr == "true") {
            loop = LoopMode::Loop;
        } else if (loopAttr == "pingpong") {
            loop = LoopMode::PingPong;
        }

        float segStart = 0.0f;
        float segEnd = -1.0f;
        std::string_view segmentAttr = findAttribute(m_ir, data, "segment");
        size_t comma = segmentAttr.find(',');
        if (comma != std::string_view::npos) {
            segStart = parseFloatOr(segmentAttr.substr(0, comma), 0.0f);
            segEnd = parseFloatOr(segmentAttr.substr(comma + 1), -1.0f);
        }

        PlayParams params;
        params.timeline = sceneTimeline(src.scene);
        params.localToArena = storedInst.localToArena.data();
        params.localCount = static_cast<uint32_t>(storedInst.localToArena.size());
        params.selfNode = elementNode;
        params.loop = loop;
        params.iterations = (loop == LoopMode::Once) ? 1 : 0;
        params.speed = parseFloatOr(findAttribute(m_ir, data, "speed"), 1.0f);
        params.segStart = segStart;
        params.segEnd = segEnd;

        uint32_t instanceId = m_animator.play(params, m_clock);
        if (instanceId == 0) {
            continue;  // T-257b: no timeline for this scene (IrView::validate rejects that IR).
        }
        data.defaultAnimation = instanceId;
        m_animInstances.push_back(AnimInstanceInfo{instanceId, elementNode, AnimSource::Autoplay});
    }

    // Step 4b (Docs/tasks/T-30.md "字型資產"): every `@font-face` asset is handed to the shaper,
    // embedded bytes straight from the ASST chunk, external ones through AssetResolver::readBytes.
    // A failure is a load-time warning, never a load failure (the text then falls back to whatever
    // other font matches).
    if (m_shaper != nullptr) {
        const AssetChunk& assetChunk = m_ir.assets();
        std::span<const IrAsset> allAssets = assetChunk.assets();
        for (const IrFontFace& face : m_ir.style().fontFaces()) {
            std::string_view family = m_ir.string(face.family);
            if (face.asset >= allAssets.size()) {
                m_warnings.push_back("@font-face \"" + std::string(family) + "\": invalid asset id");
                continue;
            }
            const IrAsset& asset = allAssets[face.asset];
            std::string_view assetName = m_ir.string(asset.name);
            std::vector<uint8_t> bytes;
            if (asset.embedded != 0) {
                std::span<const uint8_t> data = assetChunk.data(asset);
                bytes.assign(data.begin(), data.end());
            } else if (m_assets == nullptr || !m_assets->readBytes(assetName, bytes)) {
                m_warnings.push_back("@font-face \"" + std::string(family) + "\": could not read \"" +
                                     std::string(assetName) + "\"");
                continue;
            }
            if (!m_shaper->addFontMemory(std::move(bytes))) {
                m_warnings.push_back("@font-face \"" + std::string(family) + "\": shaper rejected \"" +
                                     std::string(assetName) + "\"");
            }
        }
    }

    // Step 4c (T-172): `@timeline` groups (TGRP), idle until played.
    loadTimelineGroups();
    // T-244 (32 §32.3): one RigState per `<xt-skeleton>` of the instantiated scenes.
    loadRigs();

    // Step 4d (T-177): `<repeat>` templates (RPT) leave the tree before triggers and bindings bind.
    m_repeats->prepare();

    // Step 5 (Docs/tasks/T-24.md): every Element starts kDirtyStyle so the first update()/
    // StyleEngine::resolve() resolves the whole tree.
    m_styleEngine.init(m_ir, m_animator, m_shaper);
    m_styleEngine.setAssetResolver(m_assets);  // T-199 (26.3.4): atlas cells and manifest slices.
    m_styleEngine.onAnimationPlay = [this](uint32_t id, NodeId node) {
        m_animInstances.push_back(AnimInstanceInfo{id, node, AnimSource::Css});
    };
    // T-51b: bind the runtime effect declaration pool so the cascade can resolve a runtime
    // `effect` ListRef (see xt/node.h's kRuntimeEffectOffsetBit).
    m_styleEngine.setRuntimeEffects(&m_runtimeEffects, &m_runtimeEffectStops);
    // 05.5c (T-115): the same binding for runtime `background-gradient` declarations.
    m_styleEngine.setRuntimeGradients(&m_runtimeGradients);
    m_styleEngine.setRuntimeClipShapes(&m_runtimeClipShapes);  // T-332: runtime `clip-path`.
    // T-123: the same for a runtime `animation` declaration (ElementRef::setStyle / xt_flow's
    // `playAnimation` port).
    m_styleEngine.setRuntimeAnimations(&m_runtimeAnimations);
    // T-170: a runtime `var()` declaration's name is interned into the runtime string pool.
    m_styleEngine.setRuntimeStrings(&m_runtimeStrings);
    // T-270d: every text value the cascade reads goes through string() (runtime text slots too).
    m_styleEngine.setDocument(this);
    // T-218: scene-target rules (`#Icon/Blade { fill: ... }`) write into the scene instances.
    m_styleEngine.setSceneInstances(&m_instances);
    m_layoutEngine.init(m_ir, m_assets, m_shaper);
    // T-33: the IR (tag/attribute lookups) and the EditState pool the dispatcher edits, plus the
    // same pool as the layout-time string source for editable `input` Text nodes.
    m_events->init(&m_ir, m_editStates.get(), m_composition.get());
    // T-174: trigger tables (TRIG) bound to their elements; delivered events feed them.
    m_triggers->load();
    m_events->setTriggerSink(m_triggers.get());
    m_events->setDocument(this);          // T-270a: widgetKind() resolves runtime-interned tags too.
    m_events->setAutofocus(m_autofocus);  // T-194: the surface's initial focus (08.9).
    // T-175: port values and bindings (PORT/BIND); evaluates every binding once.
    m_bindings->load();
    m_layoutEngine.setEditStates(m_editStates.get());
    m_layoutEngine.setComposition(m_composition.get());
    m_layoutEngine.setRuntimeStrings(&m_runtimeStrings);
    m_layoutEngine.setRuntimeTexts(&m_runtimeTexts);  // T-270a: setText()'s node-owned text.
    // T-179: the `@font-fallback` chains text shaping appends per language.
    m_layoutEngine.setLocalization(&m_ir.localization());
    refreshUnresolvedPlaceholders();  // T-397: no provider yet, so every `@key` one is unresolved.
    m_layoutEngine.setUnresolvedPlaceholders(&m_unresolvedPlaceholders);
    for (NodeId node : m_docElementNodes) {
        m_arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
        m_arena.markDirty(node, static_cast<uint8_t>(kDirtyLayout));
    }
    // T-176: bind the `calc()` programs and record the initial `@media` state.
    refreshLengthEnv(/*force=*/true);
    // T-177: templates stay clean (detached); spacers and the initial instances.
    m_repeats->load();

    return Status::kOk;
}

}  // namespace xt
