#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node.h"  // ElementClipShape (T-332), GridTrack / GridArea (T-338).
#include "xt/types.h"

/// `.xtir` writer, used only by `xt_compile`. See docs/design/02-ir-format.md §2.11 and
/// docs/tasks/T-03.md "ir_writer.h". `IrWriter::finish()` produces the exact on-disk layout
/// described in §2 (byte-identical on repeated calls, and byte-identical when rebuilt from an
/// `IrView` via `fromView()` -- verified by tests/unit/test_ir_roundtrip.cpp).

namespace xt::compile {

/// Builder for one Scene chunk (SCEN), passed to `IrWriter::addScene()`.
struct SceneBuilder {
    /// Scene name (`StringId`).
    StringId name = kInvalidString;
    /// Design width (Lottie's `w`).
    float width = 0;
    /// Design height (Lottie's `h`).
    float height = 0;
    /// Scene nodes in depth-first pre-order; `nodes[0]` is the root Group.
    std::vector<IrSceneNode> nodes;
    /// Path data referenced by `IrSceneNode::kindIndex` for Path nodes.
    std::vector<IrPathData> paths;
    /// Image data referenced by `IrSceneNode::kindIndex` for Image nodes.
    std::vector<IrImageData> images;
    /// Packed path-verb byte pool referenced by `IrPathData::verbOffset`/`verbCount`.
    std::vector<uint8_t> verbPool;
    /// Float pool referenced by `IrPathData`'s point/dash offsets.
    std::vector<float> floatPool;
    /// Gradient-stop pool referenced by `IrPathData`'s fill/stroke stop offsets, and by
    /// `effects[]`' `stopOffset`/`stopCount`.
    std::vector<IrGradientStop> stopPool;
    /// Camera data referenced by `IrSceneNode::kindIndex` for kind == Camera.
    std::vector<IrCameraData> cameras;
    /// Effect instances referenced by `IrSceneNode::effectOffset`/`effectCount`.
    std::vector<IrEffect> effects;
    /// T-225: modifier data referenced by `IrSceneNode::kindIndex` for kind == Modifier.
    std::vector<IrModifierData> modifiers;
    /// T-225: parametric-shape data referenced by `IrSceneNode::kindIndex` for kind == Shape.
    std::vector<IrShapeData> shapes;
    /// T-243 (32 §32.2): bone data referenced by `IrSceneNode::kindIndex` for kind == Bone.
    std::vector<IrBoneData> bones;
    /// T-243: constraint data referenced by `kindIndex` for kind == Constraint.
    std::vector<IrConstraintData> constraints;
    /// T-243: image-mesh data referenced by `kindIndex` for kind == Mesh.
    std::vector<IrMeshData> meshes;
    /// T-243: skins referenced by IrPathData::skinIndex / IrMeshData::skinIndex.
    std::vector<IrSkinData> skins;
    /// T-243: tendons referenced by IrSkinData::tendonOffset/tendonCount.
    std::vector<IrTendon> tendonPool;
    /// T-243: vertex weights referenced by IrSkinData::weightOffset/weightCount.
    std::vector<IrPointWeight> weightPool;
    /// T-243: mesh positions, each mesh's uvs right after its positions.
    std::vector<Vec2> meshVec2Pool;
    /// T-243: mesh triangle indices.
    std::vector<uint16_t> meshIndexPool;
};

/// Builder for one Timeline chunk (TLIN), passed to `IrWriter::addTimeline()`.
struct TimelineBuilder {
    /// Timeline name (`StringId`).
    StringId name = kInvalidString;
    /// Timeline duration, in seconds.
    float duration = 0;
    /// Frame rate the timeline was authored at.
    float frameRate = 0;
    /// Animated tracks, one per animated node/property pair.
    std::vector<IrTrack> tracks;
    /// Keyframe pool referenced by `IrTrack::kfOffset`/`kfCount`.
    std::vector<IrKeyframe> keyframes;
    /// Float pool referenced by keyframe value arrays (e.g. path/float-array values).
    std::vector<float> floatPool;
    /// Gradient-stop pool referenced by keyframe stop-array values.
    std::vector<IrGradientStop> stopPool;
    /// Named time markers.
    std::vector<IrMarker> markers;
};

/// Builder for the single STYL chunk, passed to `IrWriter::setStyle()`.
struct StyleBuilder {
    /// Style rules, already expanded from selector groups into one rule per selector.
    std::vector<IrRule> rules;
    /// Selector-part pool referenced by `IrRule::selOffset`/`selCount`.
    std::vector<IrSelPart> selectorParts;
    /// Declaration pool referenced by `IrRule::declOffset`/`declCount`.
    std::vector<IrDecl> decls;
    /// `@keyframes name -> TimelineId` mappings, resolved for `animation` declarations.
    std::vector<IrKeyframesRef> keyframesRefs;
    /// Class-name (`StringId`) pool referenced by `IrSelPart::classOffset`.
    std::vector<uint32_t> classIds;
    /// Transition-spec pool referenced by `transition` declarations' `ListRef` values.
    std::vector<IrTransitionSpec> transitions;
    /// Animation-spec pool referenced by `animation` declarations' `ListRef` values.
    std::vector<IrAnimationSpec> animations;
    /// `font-family` string (`StringId`) pool referenced by `font-family` declarations.
    std::vector<uint32_t> fontFamilies;
    /// `@font-face` declarations.
    std::vector<IrFontFace> fontFaces;
    /// Effect instances referenced by an `IrDecl`'s `ListRef` value when `prop == 187`.
    std::vector<IrEffect> effects;
    /// Gradient-stop pool referenced by `effects[]`' and `gradients[]`' `stopOffset`/`stopCount`.
    std::vector<IrGradientStop> effectStops;
    /// `background-gradient` values referenced by an `IrDecl`'s `ListRef` value when `prop == 190`
    /// (docs/design/05-style-and-css.md §5.5c, T-115).
    std::vector<IrGradient> gradients;
    /// T-218: points and verbs (as floats) of scene-target `d` declarations (IrDecl ListRef).
    std::vector<float> floatPool;
    /// T-335 (IR minor 6): the points of every LinearPoints easing (STYL transitions / animations
    /// and TLIN keyframes; see irLinearPointsEasing()).
    std::vector<EasingPoint> easingPoints;
};

/// Builder for the single DOCT chunk, passed to `IrWriter::setDoc()`.
struct DocBuilder {
    /// Document elements in depth-first pre-order; `elements[0]` is the `<xt>` root.
    std::vector<IrDocElement> elements;
    /// Text nodes referenced by `IrDocElement::firstTextChild`.
    std::vector<IrDocText> texts;
    /// Attribute pool referenced by `IrDocElement::attrOffset`/`attrCount`.
    std::vector<IrAttr> attrPool;
    /// Class-name (`StringId`) pool referenced by `IrDocElement::classOffset`/`classCount`.
    std::vector<uint32_t> classPool;
    /// Inline-style declaration pool referenced by `IrDocElement::inlineDeclOffset`/`inlineDeclCount`.
    std::vector<IrDecl> inlineDecls;
    /// Document-level cameras referenced by `IrDocElement::cameraIndex`.
    std::vector<IrCameraData> cameras;
    /// T-178: rich-text runs (DOCT tail), grouped by text, ascending `start`.
    std::vector<IrTextRun> textRuns;
    /// T-178: inline objects referenced by `IrTextRun::inlineObject`.
    std::vector<IrTextInline> textInlines;
    /// T-178: links referenced by `IrTextRun::link`.
    std::vector<IrTextLink> textLinks;
};

/// `embedded` empty means the asset is resolved externally by name (`IrAsset::embedded = 0`).
/// Accumulates the optional `TGRP` chunk (docs/design/04-timeline.md §4.10.1, T-172): each group
/// references its own contiguous range of the three pools.
struct TimelineGroupBuilder {
    /// The groups.
    std::vector<IrTimelineGroup> groups;
    /// Marker pool.
    std::vector<IrTimelineGroupMarker> markers;
    /// Sound-cue pool.
    std::vector<IrTimelineGroupSound> sounds;
    /// Member pool.
    std::vector<IrTimelineGroupMember> members;
};

/// Accumulates the optional `MDIA` chunk (T-176): `@media` blocks, their queries and conditions,
/// and the STYL rules each block gates (`rules` ascending by rule index).
struct MediaBuilder {
    /// One per `@media` block.
    std::vector<IrMedia> media;
    /// Query pool.
    std::vector<IrMediaQuery> queries;
    /// Condition pool.
    std::vector<IrMediaCond> conds;
    /// Rule -> media map.
    std::vector<IrMediaRule> rules;
};

/// Accumulates the optional `STAT` chunk (docs/design/27-interaction.md §27.1, T-174).
struct CustomStateBuilder {
    /// State names (StringId); bit i = names[i].
    std::vector<uint32_t> names;
    /// Mutually exclusive groups.
    std::vector<IrStateGroup> groups;
};

/// Accumulates the optional `TRIG` chunk (03 §3.12.3, T-174).
struct TriggerBuilder {
    /// One table per element with triggers.
    std::vector<IrTriggerTable> tables;
    /// Trigger pool.
    std::vector<IrTrigger> triggers;
    /// Action pool.
    std::vector<IrTriggerAction> actions;
    /// Condition pool.
    std::vector<IrConverterStep> conditions;
};

/// Accumulates the optional `STRS` chunk (T-179, 07.8, 12.7): localization keys, the per-text and
/// per-attribute key references (1-based into `keys`, 0 = none; each either empty or exactly as
/// long as DOCT's `texts`/`attrPool`), and the `@font-fallback` chains.
struct LocalizationBuilder {
    /// Key StringIds, without the leading `@`.
    std::vector<uint32_t> keys;
    /// Per DOCT text: 1-based key index, 0 = not localized.
    std::vector<uint32_t> textKeys;
    /// Per DOCT attribute: 1-based key index, 0 = not localized.
    std::vector<uint32_t> attrKeys;
    /// `@font-fallback` chains.
    std::vector<IrFontFallback> fallbacks;
    /// Family-name StringId pool the chains slice into.
    std::vector<uint32_t> fallbackFamilies;
};

/// Accumulates the optional `PORT` chunk (27 §27.3.1, 03 §3.12.3, T-175).
struct PortBuilder {
    /// Declared ports; a port is referenced by its index.
    std::vector<IrPort> ports;
    /// Struct fields / enum value names.
    std::vector<IrPortField> fields;
    /// T-358: default values (only ports that declare one).
    std::vector<IrPortDefault> defaults;
    /// T-391: entry descriptions of the struct fields of type list<T>.
    std::vector<IrPortNested> nested;
};

/// Accumulates the optional `BIND` chunk (27 §27.3.2-27.3.3, 03 §3.12.3, T-175).
struct BindingBuilder {
    /// One entry per `bind:` attribute.
    std::vector<IrBinding> bindings;
    /// PORT indices.
    std::vector<uint32_t> sources;
    /// Converter chains.
    std::vector<IrConverterStep> steps;
};

/// One `SHDR` entry (T-203, docs/design/18-effects.md §18.7): a `.xtfx` asset's compiled form,
/// passed to `IrWriter::addShader()`. `name`/`paramNames` are compiler-only metadata (the
/// `effect.<name>.<param>` addressing and the editor need them; the chunk stores StringIds).
struct ShaderBuilder {
    /// ASST index of the `.xtfx` asset (kind 2).
    AssetId asset = kInvalidAsset;
    /// The HLSL body as written.
    std::string source;
    /// The GLSL 330 module translated from it.
    std::string glsl;
    /// Declared parameters; `IrShaderParam::name` is interned by `addShader()`.
    std::vector<IrShaderParam> params;
    /// `expand:` in local px.
    float expandPx = 0;
    /// `fallback:` (`kind == None` = none).
    IrEffect fallback{};
    /// Compiler-only: the header's `name:` (empty = the asset file's stem).
    std::string name;
    /// Compiler-only: `params[i]`'s name, parallel to `params`.
    std::vector<std::string> paramNames;
};

struct AssetBuilder {
    /// Logical asset name (`StringId`), e.g. `"images/logo.png"`.
    StringId name = kInvalidString;
    /// 0 = Image, 1 = Font, 2 = Shader, 3 = AtlasImage (T-199).
    uint8_t kind = 0;
    /// Raw asset bytes to embed; empty means resolved externally by name.
    std::vector<uint8_t> embedded;
    /// Compiler-only metadata, never serialized: the reference as written in the source, before
    /// `CompileOptions::assetNameRewrite` (empty means "same as `name`").
    std::string sourceName;
    /// Compiler-only metadata, never serialized: the resolved path on disk, empty when unknown.
    std::string resolvedPath;
    /// Compiler-only metadata, never serialized (T-273b, `CompileOptions::publishAssetNames`):
    /// the document references this asset by `name`, not by `sourceName`.
    bool referencedByName = false;
};

/// One `SYMB` instance record (T-171), passed to `IrWriter::setSymbols()`.
struct SymbolInstanceBuilder {
    /// Symbol name (`StringId`).
    StringId symbol = kInvalidString;
    /// DOCT element index of the instance root.
    uint32_t instanceElement = 0;
    /// Internal elements carrying an id: full path id + element index.
    std::vector<IrSymbolPath> paths;
};

/// Accumulates the scenes, timelines, style and document data compiled from source, and
/// serializes them to an `.xtir` blob.
class IrWriter {
public:
    /// Deduplicating string intern: returns the existing id if `s` was interned before.
    StringId intern(std::string_view s);
    /// Returns the string interned under `id`, or an empty view for `kInvalidString`.
    std::string_view stringAt(StringId id) const;
    /// Adds an asset and returns its id.
    AssetId addAsset(AssetBuilder asset);
    /// Every asset added so far, in AssetId order (the order ASST serializes them in).
    const std::vector<AssetBuilder>& assets() const { return m_assets; }
    /// Adds a scene (emitted as a SCEN chunk) and returns its id.
    SceneId addScene(SceneBuilder scene);
    /// Adds a timeline (emitted as a TLIN chunk) and returns its id.
    TimelineId addTimeline(TimelineBuilder timeline);
    /// Sets the single STYL chunk's contents, replacing any previous value.
    void setStyle(StyleBuilder style);
    /// Sets the single DOCT chunk's contents, replacing any previous value.
    void setDoc(DocBuilder doc);
    /// Calling this makes finish() emit a META chunk.
    void setMeta(StringId sourceFile, StringId compilerVersion, uint64_t unixTime);

    /// Appends `camera` to `scene.cameras`, returning its index (the value an `IrSceneNode` of
    /// kind Camera stores in `kindIndex`).
    static uint32_t addSceneCamera(SceneBuilder& scene, IrCameraData camera);
    /// Appends `effect` to `scene.effects`, returning its index (used to compute an
    /// `IrSceneNode`'s `effectOffset`).
    static uint32_t addSceneEffect(SceneBuilder& scene, IrEffect effect);
    /// Appends `effect` to `style.effects`, returning its index.
    static uint32_t addStyleEffect(StyleBuilder& style, IrEffect effect);
    /// Appends `stop` to `style.effectStops`, returning its index (used to compute an
    /// `IrEffect`'s `stopOffset`).
    static uint32_t addStyleEffectStop(StyleBuilder& style, IrGradientStop stop);
    /// Appends `camera` to `doc.cameras`, returning its index (the value an `IrDocElement` stores
    /// in `cameraIndex`).
    static uint32_t addDocCamera(DocBuilder& doc, IrCameraData camera);

    /// Sets the optional `SRCM` source map: `files` is the interned StringId of each source file
    /// (index 0 must be the main `.xt` file), `entries` is one entry per `DocBuilder::elements`
    /// then, after those, one per `DocBuilder::texts`. Calling this makes finish() emit an `SRCM`
    /// chunk.
    void setSourceMap(std::vector<StringId> files, std::vector<IrSourceEntry> entries);

    /// Sets the optional `SYMB` symbol source table (T-171). finish() emits the chunk (last) only
    /// when `instances` is non-empty.
    void setSymbols(std::vector<SymbolInstanceBuilder> instances);
    /// T-170 (docs/design/12-compiler.md §12.7): appends one design token to the `VARS` chunk.
    /// finish() emits `VARS` (after every other chunk) only when a token or a fallback exists.
    void addVarToken(const IrVar& token);
    /// Appends a `var()` fallback declaration to `VARS`' `fallbacks[]`, returning its index (the
    /// second value slot of a kDeclVarRef declaration).
    uint32_t addVarFallback(const IrDecl& fallback);
    /// The tokens added so far, in VARS order.
    const std::vector<IrVar>& varTokens() const { return m_varTokens; }
    /// The fallbacks added so far, in VARS order.
    const std::vector<IrDecl>& varFallbacks() const { return m_varFallbacks; }

    /// T-176: appends one `calc()` program to the `CALC` chunk, returning its index (a kDeclCalc
    /// declaration's value slot 0). finish() emits `CALC` only when a program exists.
    uint32_t addCalc(const std::vector<IrCalcOp>& ops);
    /// The programs added so far, and their op pool.
    const std::vector<IrCalc>& calcs() const { return m_calcs; }
    const std::vector<IrCalcOp>& calcOps() const { return m_calcOps; }
    /// T-176: sets the optional `MDIA` chunk; an empty `media.media` writes no chunk.
    void setMedia(MediaBuilder media);

    /// Sets the optional `TGRP` chunk (T-172); an empty `groups.groups` writes no chunk.
    void setTimelineGroups(TimelineGroupBuilder groups);
    /// T-174: sets the optional `STAT` chunk; written only when it has a name.
    void setCustomStates(CustomStateBuilder states);
    /// T-174: sets the optional `TRIG` chunk; written only when it has a table.
    void setTriggers(TriggerBuilder triggers);
    /// T-179: sets the optional `STRS` chunk; written only when it has a key or a fallback chain.
    void setLocalization(LocalizationBuilder localization);
    /// T-175: sets the optional `PORT` chunk; written only when it has a port.
    void setPorts(PortBuilder ports);
    /// T-175: sets the optional `BIND` chunk; written only when it has a binding.
    void setBindings(BindingBuilder bindings);
    /// T-177: sets the optional `RPT ` chunk; written only when it has a repeat.
    void setRepeats(std::vector<IrRepeat> repeats);
    /// T-203: appends one `SHDR` entry (its `params[i].name` are interned from `paramNames[i]`
    /// here); the chunk is written only when it has an entry. Returns the entry's index.
    uint32_t addShader(ShaderBuilder shader);
    /// T-203: every `SHDR` entry, in `addShader()` order.
    const std::vector<ShaderBuilder>& shaders() const { return m_shaders; }
    /// T-203: the `SHDR` entry for `asset`, or nullptr.
    const ShaderBuilder* shaderForAsset(AssetId asset) const;
    /// T-332 (05 §5.9.8): appends one `clip-path` shape to the optional `CLIP` chunk (written only
    /// when it has an entry), returning its index; a `clip-path` declaration's Enum value is that
    /// index + 1. `shape.kind` must not be None.
    uint32_t addClipShape(const ElementClipShape& shape);
    /// T-332: every `CLIP` entry, in `addClipShape()` order.
    const std::vector<ElementClipShape>& clipShapes() const { return m_clipShapes; }
    /// T-338 (06 §6.15): appends one `grid-template-columns` / `-rows` track list (1 to
    /// kMaxGridTracks tracks) to the optional `GRID` chunk (written only when it has an entry),
    /// returning its index; the declaration's Enum value is that index + 1.
    uint32_t addGridTrackList(std::span<const GridTrack> tracks);
    /// T-338: appends one `grid-template-areas` value -- its named areas (names already interned)
    /// and its `rows` x `cols` matrix size -- returning its index (Enum value = index + 1).
    uint32_t addGridAreaList(std::span<const GridArea> areas, uint16_t rows, uint16_t cols);
    /// T-338: the `GRID` lists and pools, in add order.
    const std::vector<IrGridList>& gridTrackLists() const { return m_gridTrackLists; }
    const std::vector<IrGridTrack>& gridTracks() const { return m_gridTracks; }
    const std::vector<IrGridList>& gridAreaLists() const { return m_gridAreaLists; }
    const std::vector<IrGridArea>& gridAreas() const { return m_gridAreas; }

    /// Serializes to the `.xtir` byte layout described in docs/design/02-ir-format.md. Callable
    /// repeatedly; produces identical bytes each time.
    std::vector<uint8_t> finish() const;

    /// Rebuilds a writer from an already-open IrView, reading every chunk's data back into
    /// builders (used by the round-trip test).
    static IrWriter fromView(const IrView& view);

private:
    std::vector<std::string> m_strings;
    std::unordered_map<std::string, StringId> m_stringIndex;
    std::vector<AssetBuilder> m_assets;
    std::vector<SceneBuilder> m_scenes;
    std::vector<TimelineBuilder> m_timelines;
    StyleBuilder m_style;
    DocBuilder m_doc;
    bool m_hasMeta = false;
    StringId m_metaSourceFile = kInvalidString;
    StringId m_metaCompilerVersion = kInvalidString;
    uint64_t m_metaUnixTime = 0;
    bool m_hasSourceMap = false;
    std::vector<StringId> m_srcmFiles;
    std::vector<IrSourceEntry> m_srcmEntries;
    std::vector<SymbolInstanceBuilder> m_symbols;
    TimelineGroupBuilder m_timelineGroups;
    CustomStateBuilder m_customStates;
    TriggerBuilder m_triggers;
    PortBuilder m_ports;
    BindingBuilder m_bindings;
    std::vector<IrRepeat> m_repeats;
    std::vector<ShaderBuilder> m_shaders;
    std::vector<ElementClipShape> m_clipShapes;  // T-332.
    std::vector<IrGridList> m_gridTrackLists;    // T-338.
    std::vector<IrGridTrack> m_gridTracks;
    std::vector<IrGridList> m_gridAreaLists;
    std::vector<IrGridArea> m_gridAreas;
    std::vector<IrVar> m_varTokens;
    std::vector<IrDecl> m_varFallbacks;
    std::vector<IrCalc> m_calcs;
    std::vector<IrCalcOp> m_calcOps;
    MediaBuilder m_media;
    LocalizationBuilder m_localization;
};

}  // namespace xt::compile
