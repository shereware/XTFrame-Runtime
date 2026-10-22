#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "xt/ir_pod.h"
#include "xt/types.h"

/// Zero-copy reader for the `.xtir` binary format. See Docs/design/02-ir-format.md (whole chapter).
/// `IrView::open` validates the whole blob (structural + cross-reference checks) up front;
/// every accessor below is guaranteed already-bounds-checked against the original blob, so
/// callers never need to re-validate offsets/counts themselves.

namespace xt {

/// Non-owning view of an `.xtir` file's bytes. Caller keeps the memory alive for IrView's lifetime.
struct IrBlob {
    /// Pointer to the first byte of the `.xtir` file contents.
    const uint8_t* data = nullptr;
    /// Number of bytes at `data`.
    size_t size = 0;
};

/// Options controlling `IrView::open`'s validation pass.
struct IrReadOptions {
    /// When true, `open()` also verifies each chunk's CRC32 checksum, failing if any mismatch.
    bool verifyCrc = false;
};

/// `STRT` chunk: the deduplicated string table.
class StringChunk {
public:
    /// Returns the string for `id`, or an empty view if `id` is out of range.
    std::string_view get(StringId id) const;
    /// Returns the number of strings in the table.
    uint32_t count() const { return m_count; }

private:
    friend class IrView;
    uint32_t m_count = 0;
    const uint32_t* m_offsets = nullptr;
    const uint32_t* m_lengths = nullptr;
    const char* m_data = nullptr;
    size_t m_dataSize = 0;
};

/// `ASST` chunk: the asset table.
class AssetChunk {
public:
    /// Returns the asset table entries.
    std::span<const IrAsset> assets() const { return m_assets; }
    /// Returns the raw payload bytes referenced by `asset` (an entry from `assets()`).
    std::span<const uint8_t> data(const IrAsset& asset) const;

private:
    friend class IrView;
    std::span<const IrAsset> m_assets;
    const uint8_t* m_payload = nullptr;
    size_t m_payloadSize = 0;
};

/// `STYL` chunk: compiled style rules.
class StyleChunk {
public:
    /// Returns the compiled style rules (selector + declaration ranges).
    std::span<const IrRule> rules() const { return m_rules; }
    /// Returns the pool of selector parts referenced by rules.
    std::span<const IrSelPart> selectorParts() const { return m_selectorParts; }
    /// Returns the pool of property declarations referenced by rules.
    std::span<const IrDecl> decls() const { return m_decls; }
    /// Returns the pool of `@keyframes` references used by `animations()`.
    std::span<const IrKeyframesRef> keyframesRefs() const { return m_keyframesRefs; }
    /// Returns the pool of class-name string ids referenced by class selectors.
    std::span<const uint32_t> classIds() const { return m_classIds; }
    /// Returns the compiled `transition` specs.
    std::span<const IrTransitionSpec> transitions() const { return m_transitions; }
    /// Returns the compiled `animation` specs.
    std::span<const IrAnimationSpec> animations() const { return m_animations; }
    /// Returns the pool of `@font-face` family-name string ids.
    std::span<const uint32_t> fontFamilies() const { return m_fontFamilies; }
    /// Returns the compiled `@font-face` entries.
    std::span<const IrFontFace> fontFaces() const { return m_fontFaces; }
    /// Returns the compiled effect instances (referenced by `IrDecl::value` when `prop == 187`,
    /// ListRef; see 18-effects.md).
    std::span<const IrEffect> effects() const { return m_effects; }
    /// Returns the gradient-stop pool referenced by `effects()`' and `gradients()`'
    /// `stopOffset`/`stopCount`.
    std::span<const IrGradientStop> effectStops() const { return m_effectStops; }
    /// Returns the compiled `background-gradient` pool (referenced by `IrDecl::value` when
    /// `prop == 190`, ListRef; see Docs/design/05-style-and-css.md §5.5c). Empty for a `.xtir`
    /// written before IR minor 1 (T-115).
    std::span<const IrGradient> gradients() const { return m_gradients; }
    /// T-218: float pool of scene-target `d` declarations (IrDecl ListRef, see IrDecl::value).
    /// Empty for a `.xtir` written before IR minor 2.
    std::span<const float> floatPool() const { return m_floatPool; }
    /// T-335: the points of every LinearPoints easing in this file (STYL transitions / animations
    /// and TLIN keyframes; see irEasingPointOffset()). Empty for a `.xtir` written before IR
    /// minor 6.
    std::span<const EasingPoint> easingPoints() const { return m_easingPoints; }

private:
    friend class IrView;
    std::span<const IrRule> m_rules;
    std::span<const IrSelPart> m_selectorParts;
    std::span<const IrDecl> m_decls;
    std::span<const IrKeyframesRef> m_keyframesRefs;
    std::span<const uint32_t> m_classIds;
    std::span<const IrTransitionSpec> m_transitions;
    std::span<const IrAnimationSpec> m_animations;
    std::span<const uint32_t> m_fontFamilies;
    std::span<const IrFontFace> m_fontFaces;
    std::span<const IrEffect> m_effects;
    std::span<const IrGradientStop> m_effectStops;
    std::span<const IrGradient> m_gradients;
    std::span<const float> m_floatPool;
    std::span<const EasingPoint> m_easingPoints;
};

/// One `SCEN` chunk instance.
class SceneChunk {
public:
    /// Returns the scene's name string id.
    StringId name() const { return m_name; }
    /// Returns the scene's design width in logical pixels.
    float width() const { return m_width; }
    /// Returns the scene's design height in logical pixels.
    float height() const { return m_height; }
    /// Returns the scene graph nodes, in the order they must be instantiated.
    std::span<const IrSceneNode> nodes() const { return m_nodes; }
    /// Returns the pool of path data referenced by node geometry.
    std::span<const IrPathData> paths() const { return m_paths; }
    /// Returns the pool of embedded/referenced image data.
    std::span<const IrImageData> images() const { return m_images; }
    /// Returns the packed path verb byte pool referenced by `paths()`.
    std::span<const uint8_t> verbPool() const { return m_verbPool; }
    /// Returns the shared float value pool referenced by node properties.
    std::span<const float> floatPool() const { return m_floatPool; }
    /// Returns the shared gradient stop pool referenced by node paint properties and `effects()`.
    std::span<const IrGradientStop> stopPool() const { return m_stopPool; }
    /// Returns the pool of camera data referenced by `IrSceneNode::kindIndex` for kind == Camera.
    std::span<const IrCameraData> cameras() const { return m_cameras; }
    /// Returns the pool of effect instances referenced by `IrSceneNode::effectOffset`/`effectCount`.
    std::span<const IrEffect> effects() const { return m_effects; }
    /// T-225: the modifier data referenced by `IrSceneNode::kindIndex` for kind == Modifier
    /// (empty for an IR minor < 3 scene).
    std::span<const IrModifierData> modifiers() const { return m_modifiers; }
    /// T-225: the parametric-shape data referenced by `IrSceneNode::kindIndex` for kind == Shape
    /// (empty for an IR minor < 3 scene).
    std::span<const IrShapeData> shapes() const { return m_shapes; }
    /// T-243 (32 §32.2): the bone data referenced by `IrSceneNode::kindIndex` for kind == Bone
    /// (empty for an IR minor < 5 scene and a scene without bones).
    std::span<const IrBoneData> bones() const { return m_bones; }
    /// T-243: the constraint data referenced by `kindIndex` for kind == Constraint.
    std::span<const IrConstraintData> constraints() const { return m_constraints; }
    /// T-243: the image-mesh data referenced by `kindIndex` for kind == Mesh.
    std::span<const IrMeshData> meshes() const { return m_meshes; }
    /// T-243: the skins referenced by IrPathData::skinIndex / IrMeshData::skinIndex.
    std::span<const IrSkinData> skins() const { return m_skins; }
    /// T-243: the tendons referenced by IrSkinData::tendonOffset/tendonCount.
    std::span<const IrTendon> tendonPool() const { return m_tendonPool; }
    /// T-243: the vertex weights referenced by IrSkinData::weightOffset/weightCount.
    std::span<const IrPointWeight> weightPool() const { return m_weightPool; }
    /// T-243: mesh positions and uvs referenced by IrMeshData::posOffset/vertexCount.
    std::span<const Vec2> meshVec2Pool() const { return m_meshVec2Pool; }
    /// T-243: mesh triangle indices referenced by IrMeshData::indexOffset/indexCount.
    std::span<const uint16_t> meshIndexPool() const { return m_meshIndexPool; }

private:
    friend class IrView;
    StringId m_name = kInvalidString;
    float m_width = 0;
    float m_height = 0;
    std::span<const IrSceneNode> m_nodes;
    std::span<const IrPathData> m_paths;
    std::span<const IrImageData> m_images;
    std::span<const uint8_t> m_verbPool;
    std::span<const float> m_floatPool;
    std::span<const IrGradientStop> m_stopPool;
    std::span<const IrCameraData> m_cameras;
    std::span<const IrEffect> m_effects;
    std::span<const IrModifierData> m_modifiers;
    std::span<const IrShapeData> m_shapes;
    std::span<const IrBoneData> m_bones;              // T-243
    std::span<const IrConstraintData> m_constraints;  // T-243
    std::span<const IrMeshData> m_meshes;             // T-243
    std::span<const IrSkinData> m_skins;              // T-243
    std::span<const IrTendon> m_tendonPool;           // T-243
    std::span<const IrPointWeight> m_weightPool;      // T-243
    std::span<const Vec2> m_meshVec2Pool;             // T-243
    std::span<const uint16_t> m_meshIndexPool;        // T-243
    /// T-233: an IR minor < 4 file's paths()/shapes() records widened to the current layout (the
    /// new fields 0; T-243: IrPathData::skinIndex kIrNoSkin for any minor < 5 file); m_paths /
    /// m_shapes then point into these instead of into the blob.
    std::shared_ptr<const std::vector<IrPathData>> m_widenedPaths;
    std::shared_ptr<const std::vector<IrShapeData>> m_widenedShapes;
};

/// One `TLIN` chunk instance.
class TimelineChunk {
public:
    /// Returns the timeline's name string id.
    StringId name() const { return m_name; }
    /// Returns the timeline's total duration in seconds.
    float duration() const { return m_duration; }
    /// Returns the source frame rate used to author keyframe times, in frames per second.
    float frameRate() const { return m_frameRate; }
    /// Returns the property tracks that make up the timeline.
    std::span<const IrTrack> tracks() const { return m_tracks; }
    /// Returns the pool of keyframes referenced by `tracks()`.
    std::span<const IrKeyframe> keyframes() const { return m_keyframes; }
    /// Returns the shared float value pool referenced by keyframe values.
    std::span<const float> floatPool() const { return m_floatPool; }
    /// Returns the shared gradient stop pool referenced by keyframe values.
    std::span<const IrGradientStop> stopPool() const { return m_stopPool; }
    /// Returns the named markers placed on the timeline.
    std::span<const IrMarker> markers() const { return m_markers; }

private:
    friend class IrView;
    StringId m_name = kInvalidString;
    float m_duration = 0;
    float m_frameRate = 0;
    std::span<const IrTrack> m_tracks;
    std::span<const IrKeyframe> m_keyframes;
    std::span<const float> m_floatPool;
    std::span<const IrGradientStop> m_stopPool;
    std::span<const IrMarker> m_markers;
};

/// `DOCT` chunk: the document element tree.
class DocChunk {
public:
    /// Returns the document element tree, in depth-first order.
    std::span<const IrDocElement> elements() const { return m_elements; }
    /// Returns the pool of text node contents referenced by elements.
    std::span<const IrDocText> texts() const { return m_texts; }
    /// Returns the pool of element attributes referenced by elements.
    std::span<const IrAttr> attrPool() const { return m_attrPool; }
    /// Returns the pool of class-name string ids referenced by elements.
    std::span<const uint32_t> classPool() const { return m_classPool; }
    /// Returns the pool of inline `style="..."` declarations referenced by elements.
    std::span<const IrDecl> inlineDecls() const { return m_inlineDecls; }
    /// Returns the pool of document-level cameras referenced by `IrDocElement::cameraIndex`.
    std::span<const IrCameraData> cameras() const { return m_cameras; }
    /// T-178: rich-text runs (the chunk's optional tail; empty for older blobs).
    std::span<const IrTextRun> textRuns() const { return m_textRuns; }
    /// T-178: inline objects referenced by `IrTextRun::inlineObject`.
    std::span<const IrTextInline> textInlines() const { return m_textInlines; }
    /// T-178: links referenced by `IrTextRun::link`.
    std::span<const IrTextLink> textLinks() const { return m_textLinks; }

private:
    friend class IrView;
    std::span<const IrTextRun> m_textRuns;
    std::span<const IrTextInline> m_textInlines;
    std::span<const IrTextLink> m_textLinks;
    std::span<const IrDocElement> m_elements;
    std::span<const IrDocText> m_texts;
    std::span<const IrAttr> m_attrPool;
    std::span<const uint32_t> m_classPool;
    std::span<const IrDecl> m_inlineDecls;
    std::span<const IrCameraData> m_cameras;
};

/// Optional `SRCM` chunk: source-map entries for `DOCT`'s elements then texts.
class SourceMapChunk {
public:
    /// Returns whether the optional `SRCM` chunk was present in the blob.
    bool present() const { return m_present; }
    /// Returns the source file names (StringId), index 0 is always the main file.
    std::span<const uint32_t> fileNames() const { return m_fileNames; }
    /// Returns the entries, elements first then texts (`entries().size() == elementCount +
    /// textCount` when `present()`).
    std::span<const IrSourceEntry> entries() const { return m_entries; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const uint32_t> m_fileNames;
    std::span<const IrSourceEntry> m_entries;
};

/// One symbol instance of the optional `SYMB` chunk (T-171).
struct IrSymbolInstance {
    /// StringId of the symbol name (`<symbol name>`).
    StringId symbol = kInvalidString;
    /// DOCT element index of the instance root (the expanded `<use>`).
    uint32_t instanceElement = 0;
    /// The instance's internal elements that carry an id, in document order.
    std::span<const IrSymbolPath> paths;
};

/// Optional `SYMB` chunk: the symbol source table (Docs/design/26-layers-symbols-library.md
/// §26.2.2). Instances are in document order (an outer instance before the ones nested in it).
class SymbolChunk {
public:
    /// Returns whether the optional `SYMB` chunk was present in the blob.
    bool present() const { return m_present; }
    /// Returns the symbol instances.
    std::span<const IrSymbolInstance> instances() const { return m_instances; }

private:
    friend class IrView;
    bool m_present = false;
    std::vector<IrSymbolInstance> m_instances;
};

/// Optional `META` chunk.
class MetaChunk {
public:
    /// Returns whether the optional `META` chunk was present in the blob.
    bool present() const { return m_meta != nullptr; }
    /// Returns the metadata record, or `nullptr` if `present()` is false.
    const IrMeta* meta() const { return m_meta; }
    /// T-405 (IR minor 8, 43 §43.3): the project's behavior version (`spec.behavior`) from the
    /// payload's optional `u32 behavior` after IrMeta; 0 when the tail or the chunk is absent.
    uint32_t behavior() const { return m_behavior; }

private:
    friend class IrView;
    const IrMeta* m_meta = nullptr;
    uint32_t m_behavior = 0;
};

/// Optional `TGRP` chunk: `@timeline` groups (Docs/design/04-timeline.md §4.10.1, T-172). Every
/// group's marker/sound/member range is validated against the pools below by `IrView::open`.
class TimelineGroupChunk {
public:
    /// Returns whether the optional `TGRP` chunk was present in the blob.
    bool present() const { return m_present; }
    /// Returns the groups.
    std::span<const IrTimelineGroup> groups() const { return m_groups; }
    /// Returns the marker pool referenced by `IrTimelineGroup::markerOffset/markerCount`.
    std::span<const IrTimelineGroupMarker> markers() const { return m_markers; }
    /// Returns the sound-cue pool referenced by `IrTimelineGroup::soundOffset/soundCount`.
    std::span<const IrTimelineGroupSound> sounds() const { return m_sounds; }
    /// Returns the member pool referenced by `IrTimelineGroup::memberOffset/memberCount`.
    std::span<const IrTimelineGroupMember> members() const { return m_members; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrTimelineGroup> m_groups;
    std::span<const IrTimelineGroupMarker> m_markers;
    std::span<const IrTimelineGroupSound> m_sounds;
    std::span<const IrTimelineGroupMember> m_members;
};

/// Optional `VARS` chunk: the design-token table (Docs/design/05-style-and-css.md §5.9.2, 12.7,
/// T-170) -- `tokens[]` then `fallbacks[]`. Absent (both empty) for a document without tokens and
/// for every `.xtir` written before T-170.
class VarsChunk {
public:
    /// Returns whether the optional `VARS` chunk was present in the blob.
    bool present() const { return m_present; }
    /// The document's tokens (`--name` -> base value), unique by name.
    std::span<const IrVar> tokens() const { return m_tokens; }
    /// Fallback declarations referenced by a kDeclVarRef declaration's second value slot.
    std::span<const IrDecl> fallbacks() const { return m_fallbacks; }
    /// The token named by StringId `name`, or nullptr. Linear search.
    const IrVar* find(StringId name) const {
        for (const IrVar& var : m_tokens) {
            if (var.name == name) {
                return &var;
            }
        }
        return nullptr;
    }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrVar> m_tokens;
    std::span<const IrDecl> m_fallbacks;
};

/// Optional `CALC` chunk: the `calc()` programs a kDeclCalc declaration points at (T-176).
/// Absent (both empty) for a document without `calc()`.
class CalcChunk {
public:
    /// Returns whether the optional `CALC` chunk was present in the blob.
    bool present() const { return m_present; }
    /// The programs, indexed by a kDeclCalc declaration's value slot 0.
    std::span<const IrCalc> calcs() const { return m_calcs; }
    /// The RPN instruction pool every program slices into.
    std::span<const IrCalcOp> ops() const { return m_ops; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrCalc> m_calcs;
    std::span<const IrCalcOp> m_ops;
};

/// Optional `MDIA` chunk: `@media` conditions and the STYL rules they gate (T-176, 12.7's
/// `MEDIA`). Absent (all empty) for a document without `@media`.
class MediaChunk {
public:
    /// Returns whether the optional `MDIA` chunk was present in the blob.
    bool present() const { return m_present; }
    /// One entry per `@media` block.
    std::span<const IrMedia> media() const { return m_media; }
    /// The query pool IrMedia slices into.
    std::span<const IrMediaQuery> queries() const { return m_queries; }
    /// The condition pool IrMediaQuery slices into.
    std::span<const IrMediaCond> conds() const { return m_conds; }
    /// Rule -> media, ascending by rule.
    std::span<const IrMediaRule> rules() const { return m_rules; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrMedia> m_media;
    std::span<const IrMediaQuery> m_queries;
    std::span<const IrMediaCond> m_conds;
    std::span<const IrMediaRule> m_rules;
};

/// Optional `STAT` chunk: the custom-state table (Docs/design/27-interaction.md §27.1, T-174).
/// Absent (everything empty) for a document without `:state()`, `state-groups` or state triggers.
class CustomStateChunk {
public:
    /// Returns whether the optional `STAT` chunk was present in the blob.
    bool present() const { return m_present; }
    /// State names; custom state bit i is `names()[i]` (at most kMaxCustomStates).
    std::span<const uint32_t> names() const { return m_names; }
    /// Mutually exclusive groups.
    std::span<const IrStateGroup> groups() const { return m_groups; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const uint32_t> m_names;
    std::span<const IrStateGroup> m_groups;
};

/// Optional `TRIG` chunk: per-element trigger tables (Docs/design/03-scene-and-properties.md
/// §3.12.3, 27 §27.2, T-174). Every range is validated by `IrView::open`.
class TriggerChunk {
public:
    /// Returns whether the optional `TRIG` chunk was present in the blob.
    bool present() const { return m_present; }
    /// One table per element that has triggers.
    std::span<const IrTriggerTable> tables() const { return m_tables; }
    /// Trigger pool referenced by `IrTriggerTable::triggerOffset/triggerCount`.
    std::span<const IrTrigger> triggers() const { return m_triggers; }
    /// Action pool referenced by `IrTrigger::actionOffset/actionCount`.
    std::span<const IrTriggerAction> actions() const { return m_actions; }
    /// Condition pool referenced by If actions.
    std::span<const IrConverterStep> conditions() const { return m_conditions; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrTriggerTable> m_tables;
    std::span<const IrTrigger> m_triggers;
    std::span<const IrTriggerAction> m_actions;
    std::span<const IrConverterStep> m_conditions;
};

/// Optional `STRS` chunk: the localization table (T-179, 07.8, 12.7). Absent (all empty) for a
/// document with no `@key` text and no `@font-fallback`.
class LocalizationChunk {
public:
    /// Returns whether the optional `STRS` chunk was present in the blob.
    bool present() const { return m_present; }
    /// The localization keys (StringIds, without the leading `@`).
    std::span<const uint32_t> keys() const { return m_keys; }
    /// Empty, or one entry per DOCT `texts()`: 1-based index into keys(), 0 = not localized.
    std::span<const uint32_t> textKeys() const { return m_textKeys; }
    /// Empty, or one entry per DOCT `attrPool()`: 1-based index into keys(), 0 = not localized.
    std::span<const uint32_t> attrKeys() const { return m_attrKeys; }
    /// `@font-fallback` chains, in source order.
    std::span<const IrFontFallback> fallbacks() const { return m_fallbacks; }
    /// The family-name StringId pool IrFontFallback slices into.
    std::span<const uint32_t> fallbackFamilies() const { return m_fallbackFamilies; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const uint32_t> m_keys;
    std::span<const uint32_t> m_textKeys;
    std::span<const uint32_t> m_attrKeys;
    std::span<const IrFontFallback> m_fallbacks;
    std::span<const uint32_t> m_fallbackFamilies;
};

/// Optional `PORT` chunk: declared ports (Docs/design/27-interaction.md §27.3.1, 03 §3.12.3,
/// T-175). Every range and element index is validated by `IrView::open`.
class PortChunk {
public:
    /// Returns whether the optional `PORT` chunk was present in the blob.
    bool present() const { return m_present; }
    /// Declared ports; a port is referenced by its index here.
    std::span<const IrPort> ports() const { return m_ports; }
    /// Struct fields / enum value names referenced by `IrPort::fieldOffset/fieldCount`.
    std::span<const IrPortField> fields() const { return m_fields; }
    /// T-358: default values of the ports that declare one (empty before IR minor 7).
    std::span<const IrPortDefault> defaults() const { return m_defaults; }
    /// T-391: the entry descriptions of the struct fields of type `list<T>` (empty in older files).
    std::span<const IrPortNested> nested() const { return m_nested; }
    /// T-391: the IrPortNested of List field `field` (an index into fields()), or null.
    const IrPortNested* nestedOf(uint32_t field) const {
        for (const IrPortNested& n : m_nested) {
            if (n.field == field) {
                return &n;
            }
        }
        return nullptr;
    }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrPort> m_ports;
    std::span<const IrPortField> m_fields;
    std::span<const IrPortDefault> m_defaults;
    std::span<const IrPortNested> m_nested;
};

/// Optional `BIND` chunk: data bindings (27.3.2-27.3.3, 03 §3.12.3, T-175). Every range, port
/// index and element index is validated by `IrView::open`.
class BindingChunk {
public:
    /// Returns whether the optional `BIND` chunk was present in the blob.
    bool present() const { return m_present; }
    /// One entry per `bind:` attribute.
    std::span<const IrBinding> bindings() const { return m_bindings; }
    /// PORT indices referenced by `IrBinding::sourceOffset/sourceCount`.
    std::span<const uint32_t> sources() const { return m_sources; }
    /// Converter steps referenced by `IrBinding::stepOffset/stepCount`.
    std::span<const IrConverterStep> steps() const { return m_steps; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrBinding> m_bindings;
    std::span<const uint32_t> m_sources;
    std::span<const IrConverterStep> m_steps;
    /// T-243: an IR minor < 5 file's 24-byte records widened (sceneNode = kBindElementSelf);
    /// m_bindings then points into this instead of into the blob.
    std::shared_ptr<const std::vector<IrBinding>> m_widenedBindings;
};

/// Optional `SHDR` chunk: custom effect shaders (18 §18.7, T-203). Asset kinds, string ids,
/// text/param ranges and the fallback's kind are validated by `IrView::open`.
class ShaderChunk {
public:
    /// Returns whether the optional `SHDR` chunk was present in the blob.
    bool present() const { return m_present; }
    /// One entry per `.xtfx` asset.
    std::span<const IrShaderRef> shaders() const { return m_shaders; }
    /// Parameter pool (`IrShaderRef::paramOffset/paramCount`).
    std::span<const IrShaderParam> params() const { return m_params; }
    /// Text pool (`sourceOffset/sourceLength`, `glslOffset/glslLength`).
    std::string_view text() const { return std::string_view(m_text, m_textSize); }
    /// The entry whose `asset` is `asset`, or nullptr.
    const IrShaderRef* find(uint32_t asset) const {
        for (const IrShaderRef& s : m_shaders) {
            if (s.asset == asset) {
                return &s;
            }
        }
        return nullptr;
    }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrShaderRef> m_shaders;
    std::span<const IrShaderParam> m_params;
    const char* m_text = nullptr;
    size_t m_textSize = 0;
};

/// Optional `RPT ` chunk: `<repeat>` list templates (27 §27.3.4, 03 §3.12.4, T-177). Every
/// element, port and trigger index is validated by `IrView::open`.
class RepeatChunk {
public:
    /// Returns whether the optional `RPT ` chunk was present in the blob.
    bool present() const { return m_present; }
    /// One entry per `<repeat>`.
    std::span<const IrRepeat> repeats() const { return m_repeats; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrRepeat> m_repeats;
};

/// Optional `CLIP` chunk: the `clip-path` shape pool (05 §5.9.8, T-332). Kinds, parameter ranges
/// and units are validated by `IrView::open`; a `clip-path` declaration's Enum value is checked
/// against `shapes().size()`.
class ClipShapeChunk {
public:
    /// Returns whether the optional `CLIP` chunk was present in the blob.
    bool present() const { return m_present; }
    /// One entry per distinct `clip-path` shape.
    std::span<const IrClipShape> shapes() const { return m_shapes; }
    /// Parameter pool (`IrClipShape::paramOffset/paramCount`).
    std::span<const IrClipParam> params() const { return m_params; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrClipShape> m_shapes;
    std::span<const IrClipParam> m_params;
};

/// Optional `GRID` chunk: the grid track and area pools (06 §6.15, T-338). Ranges, kinds and area
/// bounds are validated by `IrView::open`; the template declarations' Enum values are checked
/// against `trackLists().size()` / `areaLists().size()`.
class GridChunk {
public:
    /// Returns whether the optional `GRID` chunk was present in the blob.
    bool present() const { return m_present; }
    /// One entry per `grid-template-columns` / `-rows` value.
    std::span<const IrGridList> trackLists() const { return m_trackLists; }
    /// The `gridTracks[]` pool.
    std::span<const IrGridTrack> tracks() const { return m_tracks; }
    /// One entry per `grid-template-areas` value.
    std::span<const IrGridList> areaLists() const { return m_areaLists; }
    /// The `gridAreas[]` pool.
    std::span<const IrGridArea> areas() const { return m_areas; }

private:
    friend class IrView;
    bool m_present = false;
    std::span<const IrGridList> m_trackLists;
    std::span<const IrGridTrack> m_tracks;
    std::span<const IrGridList> m_areaLists;
    std::span<const IrGridArea> m_areas;
};

/// Zero-copy index over a validated `.xtir` blob. Copyable (holds only pointers/spans + vectors
/// of chunk views).
class IrView {
public:
    /// Parses and validates `blob` as an `.xtir` file, returning an error `Status` (never
    /// asserting or crashing) on any structural, bounds, or cross-reference violation, including
    /// malformed/adversarial input.
    static Result<IrView> open(IrBlob blob, IrReadOptions options = {});

    /// Returns the original blob this view was opened from.
    IrBlob blob() const { return m_blob; }

    /// T-263c: which open() call produced this view -- a process-wide count starting at 1, taken
    /// by every successful open() and carried by copies of the view (0: a view that was never
    /// opened). Two views with the same serial index the same blob contents; a blob rebuilt at
    /// the same address and opened again gets a new serial, which is how the caches keyed by
    /// AssetId or by an embedded asset's address (gpu::FrameBuilder, FileAssetResolver) notice.
    uint32_t loadSerial() const { return m_loadSerial; }

    /// Returns the `STRT` string table.
    const StringChunk& strings() const { return m_strings; }
    /// Out-of-range id returns an empty string_view (never asserts: ids come from IR data that
    /// open() already validated for the fields that require validity, but this accessor stays
    /// defensive for ids that are allowed to be invalid, e.g. kInvalidString).
    std::string_view string(StringId id) const { return m_strings.get(id); }

    /// Returns the `ASST` asset table.
    const AssetChunk& assets() const { return m_assets; }
    /// Returns the `STYL` compiled style chunk.
    const StyleChunk& style() const { return m_style; }

    /// Returns the number of `SCEN` chunks in the blob.
    uint32_t sceneCount() const { return static_cast<uint32_t>(m_scenes.size()); }
    /// Returns the scene at `id`. `id` must be `< sceneCount()`.
    const SceneChunk& scene(SceneId id) const {
        XT_ASSERT(id < m_scenes.size());
        return m_scenes[id];
    }

    /// Returns the number of `TLIN` chunks in the blob.
    uint32_t timelineCount() const { return static_cast<uint32_t>(m_timelines.size()); }
    /// Returns the timeline at `id`. `id` must be `< timelineCount()`.
    const TimelineChunk& timeline(TimelineId id) const {
        XT_ASSERT(id < m_timelines.size());
        return m_timelines[id];
    }

    /// Returns the `DOCT` document tree chunk.
    const DocChunk& doc() const { return m_doc; }
    /// Returns the optional `META` chunk.
    const MetaChunk& meta() const { return m_meta; }
    /// T-405: the blob's IrHeader::minor (kIrFormatMinor for a default-constructed view). Readers
    /// branch on it where an older minor encodes a value differently (02 §2.2: a minor < 8 file's
    /// biased `hit-slop` StyleProp 276).
    uint16_t formatMinor() const { return m_formatMinor; }
    /// Returns the optional `SYMB` symbol source table (T-171); `present()` is false when the
    /// document instantiates no symbol.
    const SymbolChunk& symbols() const { return m_symbols; }
    /// Returns the optional `TGRP` chunk (`present() == false` when the blob has none).
    const TimelineGroupChunk& timelineGroups() const { return m_timelineGroups; }
    /// Returns the optional `VARS` token table (T-170).
    const VarsChunk& vars() const { return m_vars; }
    /// Returns the optional `CALC` program table (T-176).
    const CalcChunk& calcs() const { return m_calcs; }
    /// Returns the optional `MDIA` conditional-rule table (T-176).
    const MediaChunk& media() const { return m_media; }
    /// Returns the optional `STAT` custom-state table (T-174).
    const CustomStateChunk& customStates() const { return m_customStates; }
    /// Returns the optional `TRIG` trigger tables (T-174).
    const TriggerChunk& triggers() const { return m_triggers; }
    /// Returns the optional `STRS` localization table (T-179).
    const LocalizationChunk& localization() const { return m_localization; }
    /// Returns the optional `PORT` declared-port table (T-175).
    const PortChunk& ports() const { return m_ports; }
    /// Returns the optional `BIND` binding table (T-175).
    const BindingChunk& bindings() const { return m_bindings; }
    /// Returns the optional `RPT ` repeat table (T-177).
    const RepeatChunk& repeats() const { return m_repeats; }
    /// Returns the optional `SHDR` custom effect shader table (T-203).
    const ShaderChunk& shaders() const { return m_shaders; }
    /// Returns the optional `CLIP` clip-path shape pool (T-332).
    const ClipShapeChunk& clipShapes() const { return m_clipShapes; }
    /// Returns the optional `GRID` grid track / area pools (T-338).
    const GridChunk& grid() const { return m_grid; }

    /// Returns whether the optional `SRCM` chunk was present in the blob.
    bool hasSourceMap() const { return m_sourceMap.present(); }
    /// Returns the raw `SRCM` chunk (fileNames()/entries()), for callers that need the whole
    /// source map (e.g. `IrWriter::fromView`) rather than a single lookup.
    const SourceMapChunk& sourceMap() const { return m_sourceMap; }
    /// Returns the source entry for `doc().elements()[index]` (index < elementCount) or
    /// `doc().texts()[index - elementCount]` (elementCount <= index < elementCount + textCount).
    /// `index` must be in range and `hasSourceMap()` must be true.
    IrSourceEntry sourceOf(uint32_t index) const {
        XT_ASSERT(m_sourceMap.present());
        XT_ASSERT(index < m_sourceMap.entries().size());
        return m_sourceMap.entries()[index];
    }
    /// Returns the source file name for `fileId` (an `IrSourceEntry::fileId`). Out-of-range
    /// returns an empty view.
    std::string_view sourceFileName(uint32_t fileId) const {
        if (fileId >= m_sourceMap.fileNames().size()) {
            return {};
        }
        return string(m_sourceMap.fileNames()[fileId]);
    }

    /// Cross-reference validation (rule 5 in Docs/tasks/T-03.md); already run by open(). Exposed
    /// for tests.
    static Status validate(const IrView& view);

private:
    // Per-chunk payload parsers (structural bounds checks only, see Docs/tasks/T-03.md rules
    // 1-4). Declared as private members (rather than free functions) so they can write directly
    // into the chunk views' private fields; defined in Code/Runtime/core/src/ir_view.cpp.
    static bool parseStrings(const uint8_t* payload, size_t payloadSize, StringChunk& out);
    static bool parseAssets(const uint8_t* payload, size_t payloadSize, AssetChunk& out);
    static bool parseStyle(const uint8_t* payload, size_t payloadSize, StyleChunk& out);
    static bool parseScene(const uint8_t* payload, size_t payloadSize, uint16_t minor, SceneChunk& out);
    static bool parseTimeline(const uint8_t* payload, size_t payloadSize, TimelineChunk& out);
    static bool parseDoc(const uint8_t* payload, size_t payloadSize, DocChunk& out);
    static bool parseSourceMap(const uint8_t* payload, size_t payloadSize, SourceMapChunk& out);
    static bool parseMeta(const uint8_t* payload, size_t payloadSize, MetaChunk& out);
    static bool parseSymbols(const uint8_t* payload, size_t payloadSize, SymbolChunk& out);
    static bool parseTimelineGroups(const uint8_t* payload, size_t payloadSize, TimelineGroupChunk& out);
    static bool parseVars(const uint8_t* payload, size_t payloadSize, VarsChunk& out);
    static bool parseCalcs(const uint8_t* payload, size_t payloadSize, CalcChunk& out);
    static bool parseMedia(const uint8_t* payload, size_t payloadSize, MediaChunk& out);
    static bool parseCustomStates(const uint8_t* payload, size_t payloadSize, CustomStateChunk& out);
    static bool parseTriggers(const uint8_t* payload, size_t payloadSize, TriggerChunk& out);
    static bool parseLocalization(const uint8_t* payload, size_t payloadSize, LocalizationChunk& out);
    static bool parsePorts(const uint8_t* payload, size_t payloadSize, PortChunk& out);
    static bool parseBindings(const uint8_t* payload, size_t payloadSize, uint16_t minor, BindingChunk& out);
    static bool parseRepeats(const uint8_t* payload, size_t payloadSize, RepeatChunk& out);
    static bool parseShaders(const uint8_t* payload, size_t payloadSize, ShaderChunk& out);
    static bool parseClipShapes(const uint8_t* payload, size_t payloadSize, ClipShapeChunk& out);
    static bool parseGrid(const uint8_t* payload, size_t payloadSize, GridChunk& out);

    IrBlob m_blob;
    uint32_t m_loadSerial = 0;
    StringChunk m_strings;
    AssetChunk m_assets;
    StyleChunk m_style;
    std::vector<SceneChunk> m_scenes;
    std::vector<TimelineChunk> m_timelines;
    DocChunk m_doc;
    SourceMapChunk m_sourceMap;
    MetaChunk m_meta;
    uint16_t m_formatMinor = kIrFormatMinor;
    SymbolChunk m_symbols;
    TimelineGroupChunk m_timelineGroups;
    VarsChunk m_vars;
    CalcChunk m_calcs;
    MediaChunk m_media;
    CustomStateChunk m_customStates;
    TriggerChunk m_triggers;
    LocalizationChunk m_localization;
    PortChunk m_ports;
    BindingChunk m_bindings;
    RepeatChunk m_repeats;
    ShaderChunk m_shaders;
    ClipShapeChunk m_clipShapes;
    GridChunk m_grid;
};

}  // namespace xt
