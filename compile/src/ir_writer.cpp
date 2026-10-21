#include "xt/compile/ir_writer.h"

#include <cstddef>
#include <cstring>

/// See docs/design/02-ir-format.md §2.11 and docs/tasks/T-03.md "ir_writer.h finish() layout".
/// Declared here instead of including the private core/src/crc32.h (xt_core only exposes
/// core/include/xt as a PUBLIC include dir); the symbol is provided by xt_core, which xt_compile
/// links against.
namespace xt {
uint32_t crc32(const uint8_t* data, size_t size);
}  // namespace xt

namespace xt::compile {

namespace {

template <class T>
void appendPod(std::vector<uint8_t>& buf, const T& value) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

template <class T>
void appendArray(std::vector<uint8_t>& buf, const std::vector<T>& arr) {
    appendPod(buf, static_cast<uint32_t>(arr.size()));
    if (!arr.empty()) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(arr.data());
        buf.insert(buf.end(), p, p + arr.size() * sizeof(T));
    }
}

void alignTo4(std::vector<uint8_t>& buf) {
    while (buf.size() % 4u != 0) {
        buf.push_back(0);
    }
}

/// Appends one chunk (header + payload, payload zero-padded to a multiple of 4 bytes; the pad is
/// counted in payloadLength and covered by the CRC, so the next chunk header always starts at a
/// 4-byte-aligned offset).
void appendChunk(std::vector<uint8_t>& out, uint32_t type, std::vector<uint8_t> payload) {
    alignTo4(payload);
    IrChunkHeader header{};
    std::memcpy(header.type, &type, sizeof(header.type));
    header.payloadLength = static_cast<uint32_t>(payload.size());
    header.crc32 = crc32(payload.data(), payload.size());
    appendPod(out, header);
    out.insert(out.end(), payload.begin(), payload.end());
}

template <class T>
std::vector<T> toVector(std::span<const T> span) {
    return std::vector<T>(span.begin(), span.end());
}

}  // namespace

StringId IrWriter::intern(std::string_view s) {
    auto it = m_stringIndex.find(std::string(s));
    if (it != m_stringIndex.end()) {
        return it->second;
    }
    StringId id = static_cast<StringId>(m_strings.size());
    m_strings.emplace_back(s);
    m_stringIndex.emplace(m_strings.back(), id);
    return id;
}

std::string_view IrWriter::stringAt(StringId id) const {
    return id < m_strings.size() ? std::string_view(m_strings[id]) : std::string_view();
}

AssetId IrWriter::addAsset(AssetBuilder asset) {
    AssetId id = static_cast<AssetId>(m_assets.size());
    m_assets.push_back(std::move(asset));
    return id;
}

SceneId IrWriter::addScene(SceneBuilder scene) {
    SceneId id = static_cast<SceneId>(m_scenes.size());
    m_scenes.push_back(std::move(scene));
    return id;
}

TimelineId IrWriter::addTimeline(TimelineBuilder timeline) {
    TimelineId id = static_cast<TimelineId>(m_timelines.size());
    m_timelines.push_back(std::move(timeline));
    return id;
}

void IrWriter::setStyle(StyleBuilder style) { m_style = std::move(style); }

void IrWriter::setDoc(DocBuilder doc) { m_doc = std::move(doc); }

void IrWriter::setMeta(StringId sourceFile, StringId compilerVersion, uint64_t unixTime) {
    m_hasMeta = true;
    m_metaSourceFile = sourceFile;
    m_metaCompilerVersion = compilerVersion;
    m_metaUnixTime = unixTime;
}

uint32_t IrWriter::addSceneCamera(SceneBuilder& scene, IrCameraData camera) {
    uint32_t idx = static_cast<uint32_t>(scene.cameras.size());
    scene.cameras.push_back(camera);
    return idx;
}

uint32_t IrWriter::addSceneEffect(SceneBuilder& scene, IrEffect effect) {
    uint32_t idx = static_cast<uint32_t>(scene.effects.size());
    scene.effects.push_back(effect);
    return idx;
}

uint32_t IrWriter::addStyleEffect(StyleBuilder& style, IrEffect effect) {
    uint32_t idx = static_cast<uint32_t>(style.effects.size());
    style.effects.push_back(effect);
    return idx;
}

uint32_t IrWriter::addStyleEffectStop(StyleBuilder& style, IrGradientStop stop) {
    uint32_t idx = static_cast<uint32_t>(style.effectStops.size());
    style.effectStops.push_back(stop);
    return idx;
}

uint32_t IrWriter::addDocCamera(DocBuilder& doc, IrCameraData camera) {
    uint32_t idx = static_cast<uint32_t>(doc.cameras.size());
    doc.cameras.push_back(camera);
    return idx;
}

void IrWriter::setSourceMap(std::vector<StringId> files, std::vector<IrSourceEntry> entries) {
    m_hasSourceMap = true;
    m_srcmFiles = std::move(files);
    m_srcmEntries = std::move(entries);
}

void IrWriter::setSymbols(std::vector<SymbolInstanceBuilder> instances) { m_symbols = std::move(instances); }
void IrWriter::addVarToken(const IrVar& token) { m_varTokens.push_back(token); }

uint32_t IrWriter::addVarFallback(const IrDecl& fallback) {
    m_varFallbacks.push_back(fallback);
    return static_cast<uint32_t>(m_varFallbacks.size() - 1);
}

uint32_t IrWriter::addCalc(const std::vector<IrCalcOp>& ops) {
    IrCalc calc{};
    calc.opOffset = static_cast<uint32_t>(m_calcOps.size());
    calc.opCount = static_cast<uint32_t>(ops.size());
    m_calcOps.insert(m_calcOps.end(), ops.begin(), ops.end());
    m_calcs.push_back(calc);
    return static_cast<uint32_t>(m_calcs.size() - 1);
}

void IrWriter::setMedia(MediaBuilder media) { m_media = std::move(media); }

void IrWriter::setTimelineGroups(TimelineGroupBuilder groups) { m_timelineGroups = std::move(groups); }

void IrWriter::setCustomStates(CustomStateBuilder states) { m_customStates = std::move(states); }

void IrWriter::setTriggers(TriggerBuilder triggers) { m_triggers = std::move(triggers); }

void IrWriter::setLocalization(LocalizationBuilder localization) { m_localization = std::move(localization); }

void IrWriter::setPorts(PortBuilder ports) { m_ports = std::move(ports); }

void IrWriter::setBindings(BindingBuilder bindings) { m_bindings = std::move(bindings); }

void IrWriter::setRepeats(std::vector<IrRepeat> repeats) { m_repeats = std::move(repeats); }

uint32_t IrWriter::addShader(ShaderBuilder shader) {
    for (size_t i = 0; i < shader.params.size() && i < shader.paramNames.size(); ++i) {
        shader.params[i].name = intern(shader.paramNames[i]);
    }
    m_shaders.push_back(std::move(shader));
    return static_cast<uint32_t>(m_shaders.size() - 1);
}

uint32_t IrWriter::addClipShape(const ElementClipShape& shape) {
    m_clipShapes.push_back(shape);
    return static_cast<uint32_t>(m_clipShapes.size() - 1);
}

uint32_t IrWriter::addGridTrackList(std::span<const GridTrack> tracks) {
    IrGridList list{};
    list.offset = static_cast<uint32_t>(m_gridTracks.size());
    list.count = static_cast<uint32_t>(tracks.size());
    for (const GridTrack& track : tracks) {
        IrGridTrack ir{};
        ir.value = track.value;
        ir.kind = static_cast<uint8_t>(track.kind);
        m_gridTracks.push_back(ir);
    }
    m_gridTrackLists.push_back(list);
    return static_cast<uint32_t>(m_gridTrackLists.size() - 1);
}

uint32_t IrWriter::addGridAreaList(std::span<const GridArea> areas, uint16_t rows, uint16_t cols) {
    IrGridList list{};
    list.offset = static_cast<uint32_t>(m_gridAreas.size());
    list.count = static_cast<uint32_t>(areas.size());
    list.rows = rows;
    list.cols = cols;
    for (const GridArea& area : areas) {
        IrGridArea ir{};
        ir.name = area.name;
        ir.rowStart = area.rowStart;
        ir.rowEnd = area.rowEnd;
        ir.colStart = area.colStart;
        ir.colEnd = area.colEnd;
        m_gridAreas.push_back(ir);
    }
    m_gridAreaLists.push_back(list);
    return static_cast<uint32_t>(m_gridAreaLists.size() - 1);
}

const ShaderBuilder* IrWriter::shaderForAsset(AssetId asset) const {
    for (const ShaderBuilder& s : m_shaders) {
        if (s.asset == asset) {
            return &s;
        }
    }
    return nullptr;
}

std::vector<uint8_t> IrWriter::finish() const {
    // STRT
    std::vector<uint8_t> stringsPayload;
    {
        uint32_t count = static_cast<uint32_t>(m_strings.size());
        appendPod(stringsPayload, count);
        std::vector<uint32_t> offsets(count);
        std::vector<uint32_t> lengths(count);
        uint32_t cursor = 0;
        for (uint32_t i = 0; i < count; ++i) {
            offsets[i] = cursor;
            lengths[i] = static_cast<uint32_t>(m_strings[i].size());
            cursor += lengths[i] + 1;
        }
        for (uint32_t i = 0; i < count; ++i) {
            appendPod(stringsPayload, offsets[i]);
        }
        for (uint32_t i = 0; i < count; ++i) {
            appendPod(stringsPayload, lengths[i]);
        }
        for (uint32_t i = 0; i < count; ++i) {
            stringsPayload.insert(stringsPayload.end(), m_strings[i].begin(), m_strings[i].end());
            stringsPayload.push_back(0);
        }
    }

    // ASST
    std::vector<uint8_t> assetsPayload;
    {
        uint32_t count = static_cast<uint32_t>(m_assets.size());
        appendPod(assetsPayload, count);
        uint32_t base = 4u + count * static_cast<uint32_t>(sizeof(IrAsset));
        std::vector<IrAsset> entries;
        entries.reserve(count);
        std::vector<uint8_t> dataSection;
        for (const AssetBuilder& ab : m_assets) {
            IrAsset e{};
            e.name = ab.name;
            e.kind = ab.kind;
            e._pad = 0;
            if (!ab.embedded.empty()) {
                e.embedded = 1;
                e.dataOffset = base + static_cast<uint32_t>(dataSection.size());
                e.dataLength = static_cast<uint32_t>(ab.embedded.size());
                dataSection.insert(dataSection.end(), ab.embedded.begin(), ab.embedded.end());
            } else {
                e.embedded = 0;
                e.dataOffset = 0;
                e.dataLength = 0;
            }
            entries.push_back(e);
        }
        for (const IrAsset& e : entries) {
            appendPod(assetsPayload, e);
        }
        assetsPayload.insert(assetsPayload.end(), dataSection.begin(), dataSection.end());
    }

    // STYL
    std::vector<uint8_t> stylePayload;
    {
        appendArray(stylePayload, m_style.rules);
        appendArray(stylePayload, m_style.selectorParts);
        appendArray(stylePayload, m_style.decls);
        appendArray(stylePayload, m_style.keyframesRefs);
        appendArray(stylePayload, m_style.classIds);
        appendArray(stylePayload, m_style.transitions);
        appendArray(stylePayload, m_style.animations);
        appendArray(stylePayload, m_style.fontFamilies);
        appendArray(stylePayload, m_style.fontFaces);
        appendArray(stylePayload, m_style.effects);
        appendArray(stylePayload, m_style.effectStops);
        appendArray(stylePayload, m_style.gradients);
        appendArray(stylePayload, m_style.floatPool);     // T-218 (IR minor 2).
        appendArray(stylePayload, m_style.easingPoints);  // T-335 (IR minor 6).
    }

    // DOCT
    std::vector<uint8_t> docPayload;
    {
        appendArray(docPayload, m_doc.elements);
        appendArray(docPayload, m_doc.texts);
        appendArray(docPayload, m_doc.attrPool);
        appendArray(docPayload, m_doc.classPool);
        appendArray(docPayload, m_doc.inlineDecls);
        appendArray(docPayload, m_doc.cameras);
        // T-178: the rich-text tail; omitted entirely when the document has no runs, so a
        // document without rich text keeps its pre-T-178 bytes.
        if (!m_doc.textRuns.empty()) {
            appendArray(docPayload, m_doc.textRuns);
            appendArray(docPayload, m_doc.textInlines);
            appendArray(docPayload, m_doc.textLinks);
        }
    }

    std::vector<uint8_t> body;
    appendChunk(body, kChunkSTRT, std::move(stringsPayload));
    appendChunk(body, kChunkASST, std::move(assetsPayload));
    appendChunk(body, kChunkSTYL, std::move(stylePayload));

    for (const SceneBuilder& s : m_scenes) {
        std::vector<uint8_t> payload;
        appendPod(payload, static_cast<uint32_t>(s.name));
        appendPod(payload, s.width);
        appendPod(payload, s.height);
        appendArray(payload, s.nodes);
        appendArray(payload, s.paths);
        appendArray(payload, s.images);
        appendArray(payload, s.cameras);
        appendArray(payload, s.verbPool);
        alignTo4(payload);
        appendArray(payload, s.floatPool);
        appendArray(payload, s.stopPool);
        appendArray(payload, s.effects);
        // T-225 (IR minor 3): the `modifiers[]`/`shapes[]` tail, omitted when both are empty so a
        // scene without Modifier/Shape nodes keeps its pre-T-225 bytes. T-243 (IR minor 5): then
        // the rig tail, all eight arrays or none.
        const bool rig = !s.bones.empty() || !s.constraints.empty() || !s.meshes.empty() ||
                         !s.skins.empty() || !s.tendonPool.empty() || !s.weightPool.empty() ||
                         !s.meshVec2Pool.empty() || !s.meshIndexPool.empty();
        if (!s.modifiers.empty() || !s.shapes.empty() || rig) {
            appendArray(payload, s.modifiers);
            appendArray(payload, s.shapes);
        }
        if (rig) {
            appendArray(payload, s.bones);
            appendArray(payload, s.constraints);
            appendArray(payload, s.meshes);
            appendArray(payload, s.skins);
            appendArray(payload, s.tendonPool);
            appendArray(payload, s.weightPool);
            appendArray(payload, s.meshVec2Pool);
            appendArray(payload, s.meshIndexPool);
        }
        appendChunk(body, kChunkSCEN, std::move(payload));
    }

    for (const TimelineBuilder& t : m_timelines) {
        std::vector<uint8_t> payload;
        appendPod(payload, static_cast<uint32_t>(t.name));
        appendPod(payload, t.duration);
        appendPod(payload, t.frameRate);
        appendArray(payload, t.tracks);
        appendArray(payload, t.keyframes);
        appendArray(payload, t.floatPool);
        appendArray(payload, t.stopPool);
        appendArray(payload, t.markers);
        appendChunk(body, kChunkTLIN, std::move(payload));
    }

    appendChunk(body, kChunkDOCT, std::move(docPayload));

    if (m_hasSourceMap) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_srcmFiles);
        appendArray(payload, m_srcmEntries);
        appendChunk(body, kChunkSRCM, std::move(payload));
    }

    if (m_hasMeta) {
        std::vector<uint8_t> payload;
        IrMeta meta{};
        meta.sourceFile = m_metaSourceFile;
        meta.compilerVersion = m_metaCompilerVersion;
        meta.compileTimeUnix = m_metaUnixTime;
        appendPod(payload, meta);
        appendChunk(body, kChunkMETA, std::move(payload));
    }

    // SYMB (T-171): last, only when some symbol was instantiated.
    if (!m_symbols.empty()) {
        std::vector<uint8_t> payload;
        appendPod(payload, static_cast<uint32_t>(m_symbols.size()));
        for (const SymbolInstanceBuilder& inst : m_symbols) {
            appendPod(payload, static_cast<uint32_t>(inst.symbol));
            appendPod(payload, inst.instanceElement);
            appendArray(payload, inst.paths);
        }
        appendChunk(body, kChunkSYMB, std::move(payload));
    }

    // T-172: `@timeline` groups, after every other chunk (02 order + TGRP at the tail).
    bool hasTimelineGroups = !m_timelineGroups.groups.empty();
    if (hasTimelineGroups) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_timelineGroups.groups);
        appendArray(payload, m_timelineGroups.markers);
        appendArray(payload, m_timelineGroups.sounds);
        appendArray(payload, m_timelineGroups.members);
        appendChunk(body, kChunkTGRP, std::move(payload));
    }

    // T-170: the token table goes last, so a reader that predates it just skips one chunk.
    const bool hasVars = !m_varTokens.empty() || !m_varFallbacks.empty();
    if (hasVars) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_varTokens);
        appendArray(payload, m_varFallbacks);
        appendChunk(body, kChunkVARS, std::move(payload));
    }

    // T-176: `calc()` programs and `@media` rules, after VARS for the same reason.
    const bool hasCalcs = !m_calcs.empty();
    if (hasCalcs) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_calcs);
        appendArray(payload, m_calcOps);
        appendChunk(body, kChunkCALC, std::move(payload));
    }
    const bool hasMedia = !m_media.media.empty();
    if (hasMedia) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_media.media);
        appendArray(payload, m_media.queries);
        appendArray(payload, m_media.conds);
        appendArray(payload, m_media.rules);
        appendChunk(body, kChunkMDIA, std::move(payload));
    }
    // T-174: custom states, then trigger tables, after everything else.
    const bool hasStates = !m_customStates.names.empty();
    if (hasStates) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_customStates.names);
        appendArray(payload, m_customStates.groups);
        appendChunk(body, kChunkSTAT, std::move(payload));
    }
    const bool hasTriggers = !m_triggers.tables.empty();
    if (hasTriggers) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_triggers.tables);
        appendArray(payload, m_triggers.triggers);
        appendArray(payload, m_triggers.actions);
        appendArray(payload, m_triggers.conditions);
        appendChunk(body, kChunkTRIG, std::move(payload));
    }
    // T-179: the localization table, last for the same reason.
    const bool hasLocalization = !m_localization.keys.empty() || !m_localization.fallbacks.empty();
    if (hasLocalization) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_localization.keys);
        appendArray(payload, m_localization.textKeys);
        appendArray(payload, m_localization.attrKeys);
        appendArray(payload, m_localization.fallbacks);
        appendArray(payload, m_localization.fallbackFamilies);
        appendChunk(body, kChunkSTRS, std::move(payload));
    }
    // T-175: declared ports, then bindings.
    const bool hasPorts = !m_ports.ports.empty();
    if (hasPorts) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_ports.ports);
        appendArray(payload, m_ports.fields);
        // T-358 (IR minor 7): the `defaults[]` tail, omitted when no port has a default.
        if (!m_ports.defaults.empty() || !m_ports.nested.empty()) {
            appendArray(payload, m_ports.defaults);
        }
        // T-391: the `nested[]` tail (struct fields of type list<T>), omitted when there is none.
        if (!m_ports.nested.empty()) {
            appendArray(payload, m_ports.nested);
        }
        appendChunk(body, kChunkPORT, std::move(payload));
    }
    const bool hasBindings = !m_bindings.bindings.empty();
    if (hasBindings) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_bindings.bindings);
        appendArray(payload, m_bindings.sources);
        appendArray(payload, m_bindings.steps);
        appendChunk(body, kChunkBIND, std::move(payload));
    }
    // T-177: `<repeat>` templates, last.
    const bool hasRepeats = !m_repeats.empty();
    if (hasRepeats) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_repeats);
        appendChunk(body, kChunkRPT, std::move(payload));
    }
    // T-203: custom effect shaders, after RPT.
    const bool hasShaders = !m_shaders.empty();
    if (hasShaders) {
        std::vector<IrShaderRef> refs;
        std::vector<IrShaderParam> params;
        std::vector<uint8_t> text;
        for (const ShaderBuilder& sb : m_shaders) {
            IrShaderRef ref{};
            ref.asset = sb.asset;
            ref.sourceOffset = static_cast<uint32_t>(text.size());
            ref.sourceLength = static_cast<uint32_t>(sb.source.size());
            text.insert(text.end(), sb.source.begin(), sb.source.end());
            ref.glslOffset = static_cast<uint32_t>(text.size());
            ref.glslLength = static_cast<uint32_t>(sb.glsl.size());
            text.insert(text.end(), sb.glsl.begin(), sb.glsl.end());
            ref.paramOffset = static_cast<uint32_t>(params.size());
            ref.paramCount = static_cast<uint32_t>(sb.params.size());
            params.insert(params.end(), sb.params.begin(), sb.params.end());
            ref.expandPx = sb.expandPx;
            ref.fallback = sb.fallback;
            refs.push_back(ref);
        }
        std::vector<uint8_t> payload;
        appendArray(payload, refs);
        appendArray(payload, params);
        appendArray(payload, text);
        appendChunk(body, kChunkSHDR, std::move(payload));
    }
    // T-332: the `clip-path` shape pool, after SHDR.
    const bool hasClipShapes = !m_clipShapes.empty();
    if (hasClipShapes) {
        std::vector<IrClipShape> shapes;
        std::vector<IrClipParam> params;
        for (const ElementClipShape& shape : m_clipShapes) {
            IrClipShape ir{};
            ir.kind = static_cast<uint8_t>(shape.kind);
            ir.paramOffset = static_cast<uint32_t>(params.size());
            ir.paramCount = static_cast<uint32_t>(shape.params.size());
            for (const Length& len : shape.params) {
                IrClipParam p{};
                p.value = len.value;
                p.unit = static_cast<uint8_t>(len.unit);
                params.push_back(p);
            }
            shapes.push_back(ir);
        }
        std::vector<uint8_t> payload;
        appendArray(payload, shapes);
        appendArray(payload, params);
        appendChunk(body, kChunkCLIP, std::move(payload));
    }
    // T-338: the grid track / area pools, after CLIP.
    const bool hasGrid = !m_gridTrackLists.empty() || !m_gridAreaLists.empty();
    if (hasGrid) {
        std::vector<uint8_t> payload;
        appendArray(payload, m_gridTrackLists);
        appendArray(payload, m_gridTracks);
        appendArray(payload, m_gridAreaLists);
        appendArray(payload, m_gridAreas);
        appendChunk(body, kChunkGRID, std::move(payload));
    }

    uint32_t chunkCount =
        4u + static_cast<uint32_t>(m_scenes.size()) + static_cast<uint32_t>(m_timelines.size()) +
        (m_hasSourceMap ? 1u : 0u) + (m_hasMeta ? 1u : 0u) + (m_symbols.empty() ? 0u : 1u) +
        (hasTimelineGroups ? 1u : 0u) + (hasVars ? 1u : 0u) + (hasCalcs ? 1u : 0u) + (hasMedia ? 1u : 0u) +
        (hasStates ? 1u : 0u) + (hasTriggers ? 1u : 0u) + (hasLocalization ? 1u : 0u) + (hasPorts ? 1u : 0u) +
        (hasBindings ? 1u : 0u) + (hasRepeats ? 1u : 0u) + (hasShaders ? 1u : 0u) +
        (hasClipShapes ? 1u : 0u) + (hasGrid ? 1u : 0u);

    IrHeader header{};
    std::memcpy(header.magic, "XTIR", 4);
    header.major = kIrFormatMajor;
    header.minor =
        kIrFormatMinor;  // T-358: 7; T-335: 6; T-218: 2 (STYL floatPool[]); T-115: 1 (gradientPool[]).
    header.chunkCount = chunkCount;
    header.totalLength = static_cast<uint32_t>(sizeof(IrHeader) + body.size());

    std::vector<uint8_t> out;
    out.reserve(sizeof(IrHeader) + body.size());
    appendPod(out, header);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

IrWriter IrWriter::fromView(const IrView& view) {
    IrWriter writer;

    uint32_t stringCount = view.strings().count();
    for (uint32_t i = 0; i < stringCount; ++i) {
        writer.intern(view.string(i));
    }

    for (const IrAsset& asset : view.assets().assets()) {
        AssetBuilder ab;
        ab.name = asset.name;
        ab.kind = asset.kind;
        if (asset.embedded != 0) {
            std::span<const uint8_t> data = view.assets().data(asset);
            ab.embedded.assign(data.begin(), data.end());
        }
        writer.addAsset(std::move(ab));
    }

    {
        const StyleChunk& style = view.style();
        StyleBuilder sb;
        sb.rules = toVector(style.rules());
        sb.selectorParts = toVector(style.selectorParts());
        sb.decls = toVector(style.decls());
        sb.keyframesRefs = toVector(style.keyframesRefs());
        sb.classIds = toVector(style.classIds());
        sb.transitions = toVector(style.transitions());
        sb.animations = toVector(style.animations());
        sb.fontFamilies = toVector(style.fontFamilies());
        sb.fontFaces = toVector(style.fontFaces());
        sb.effects = toVector(style.effects());
        sb.effectStops = toVector(style.effectStops());
        sb.gradients = toVector(style.gradients());
        sb.floatPool = toVector(style.floatPool());
        sb.easingPoints = toVector(style.easingPoints());  // T-335.
        writer.setStyle(std::move(sb));
    }

    for (uint32_t i = 0; i < view.sceneCount(); ++i) {
        const SceneChunk& scene = view.scene(i);
        SceneBuilder scb;
        scb.name = scene.name();
        scb.width = scene.width();
        scb.height = scene.height();
        scb.nodes = toVector(scene.nodes());
        scb.paths = toVector(scene.paths());
        scb.images = toVector(scene.images());
        scb.cameras = toVector(scene.cameras());
        scb.verbPool = toVector(scene.verbPool());
        scb.floatPool = toVector(scene.floatPool());
        scb.stopPool = toVector(scene.stopPool());
        scb.effects = toVector(scene.effects());
        scb.modifiers = toVector(scene.modifiers());
        scb.shapes = toVector(scene.shapes());
        scb.bones = toVector(scene.bones());  // T-243
        scb.constraints = toVector(scene.constraints());
        scb.meshes = toVector(scene.meshes());
        scb.skins = toVector(scene.skins());
        scb.tendonPool = toVector(scene.tendonPool());
        scb.weightPool = toVector(scene.weightPool());
        scb.meshVec2Pool = toVector(scene.meshVec2Pool());
        scb.meshIndexPool = toVector(scene.meshIndexPool());
        writer.addScene(std::move(scb));
    }

    for (uint32_t i = 0; i < view.timelineCount(); ++i) {
        const TimelineChunk& tl = view.timeline(i);
        TimelineBuilder tb;
        tb.name = tl.name();
        tb.duration = tl.duration();
        tb.frameRate = tl.frameRate();
        tb.tracks = toVector(tl.tracks());
        tb.keyframes = toVector(tl.keyframes());
        tb.floatPool = toVector(tl.floatPool());
        tb.stopPool = toVector(tl.stopPool());
        tb.markers = toVector(tl.markers());
        writer.addTimeline(std::move(tb));
    }

    {
        const DocChunk& doc = view.doc();
        DocBuilder db;
        db.elements = toVector(doc.elements());
        db.texts = toVector(doc.texts());
        db.attrPool = toVector(doc.attrPool());
        db.classPool = toVector(doc.classPool());
        db.inlineDecls = toVector(doc.inlineDecls());
        db.cameras = toVector(doc.cameras());
        db.textRuns = toVector(doc.textRuns());
        db.textInlines = toVector(doc.textInlines());
        db.textLinks = toVector(doc.textLinks());
        writer.setDoc(std::move(db));
    }

    if (view.hasSourceMap()) {
        const SourceMapChunk& srcm = view.sourceMap();
        writer.setSourceMap(toVector(srcm.fileNames()), toVector(srcm.entries()));
    }

    if (view.meta().present()) {
        const IrMeta& meta = *view.meta().meta();
        writer.setMeta(meta.sourceFile, meta.compilerVersion, meta.compileTimeUnix);
    }

    {
        std::vector<SymbolInstanceBuilder> symbols;
        for (const IrSymbolInstance& inst : view.symbols().instances()) {
            SymbolInstanceBuilder sib;
            sib.symbol = inst.symbol;
            sib.instanceElement = inst.instanceElement;
            sib.paths = toVector(inst.paths);
            symbols.push_back(std::move(sib));
        }
        writer.setSymbols(std::move(symbols));
    }

    if (view.timelineGroups().present()) {
        const TimelineGroupChunk& tg = view.timelineGroups();
        TimelineGroupBuilder gb;
        gb.groups = toVector(tg.groups());
        gb.markers = toVector(tg.markers());
        gb.sounds = toVector(tg.sounds());
        gb.members = toVector(tg.members());
        writer.setTimelineGroups(std::move(gb));
    }

    for (const IrVar& token : view.vars().tokens()) {
        writer.addVarToken(token);
    }
    for (const IrDecl& fallback : view.vars().fallbacks()) {
        writer.addVarFallback(fallback);
    }

    for (const IrCalc& calc : view.calcs().calcs()) {
        std::span<const IrCalcOp> ops = view.calcs().ops().subspan(calc.opOffset, calc.opCount);
        writer.addCalc(std::vector<IrCalcOp>(ops.begin(), ops.end()));
    }
    if (view.media().present()) {
        MediaBuilder mb;
        mb.media.assign(view.media().media().begin(), view.media().media().end());
        mb.queries.assign(view.media().queries().begin(), view.media().queries().end());
        mb.conds.assign(view.media().conds().begin(), view.media().conds().end());
        mb.rules.assign(view.media().rules().begin(), view.media().rules().end());
        writer.setMedia(std::move(mb));
    }
    if (view.customStates().present()) {
        CustomStateBuilder sb;
        sb.names = toVector(view.customStates().names());
        sb.groups = toVector(view.customStates().groups());
        writer.setCustomStates(std::move(sb));
    }
    if (view.triggers().present()) {
        TriggerBuilder tb;
        tb.tables = toVector(view.triggers().tables());
        tb.triggers = toVector(view.triggers().triggers());
        tb.actions = toVector(view.triggers().actions());
        tb.conditions = toVector(view.triggers().conditions());
        writer.setTriggers(std::move(tb));
    }
    if (view.localization().present()) {
        const LocalizationChunk& l10n = view.localization();
        LocalizationBuilder lb;
        lb.keys = toVector(l10n.keys());
        lb.textKeys = toVector(l10n.textKeys());
        lb.attrKeys = toVector(l10n.attrKeys());
        lb.fallbacks = toVector(l10n.fallbacks());
        lb.fallbackFamilies = toVector(l10n.fallbackFamilies());
        writer.setLocalization(std::move(lb));
    }
    if (view.ports().present()) {
        PortBuilder pb;
        pb.ports = toVector(view.ports().ports());
        pb.fields = toVector(view.ports().fields());
        pb.defaults = toVector(view.ports().defaults());
        pb.nested = toVector(view.ports().nested());  // T-391
        writer.setPorts(std::move(pb));
    }
    if (view.bindings().present()) {
        BindingBuilder bb;
        bb.bindings = toVector(view.bindings().bindings());
        bb.sources = toVector(view.bindings().sources());
        bb.steps = toVector(view.bindings().steps());
        writer.setBindings(std::move(bb));
    }
    if (view.repeats().present()) {
        writer.setRepeats(toVector(view.repeats().repeats()));
    }
    if (view.shaders().present()) {
        const ShaderChunk& sc = view.shaders();
        for (const IrShaderRef& ref : sc.shaders()) {
            ShaderBuilder sb;
            sb.asset = ref.asset;
            sb.source = std::string(sc.text().substr(ref.sourceOffset, ref.sourceLength));
            sb.glsl = std::string(sc.text().substr(ref.glslOffset, ref.glslLength));
            for (uint32_t i = 0; i < ref.paramCount; ++i) {
                const IrShaderParam& p = sc.params()[ref.paramOffset + i];
                sb.params.push_back(p);
                sb.paramNames.emplace_back(view.string(p.name));
            }
            sb.expandPx = ref.expandPx;
            sb.fallback = ref.fallback;
            writer.addShader(std::move(sb));
        }
    }
    if (view.clipShapes().present()) {  // T-332.
        const ClipShapeChunk& cc = view.clipShapes();
        for (const IrClipShape& ref : cc.shapes()) {
            ElementClipShape shape;
            shape.kind = static_cast<ClipShapeKind>(ref.kind);
            for (uint32_t i = 0; i < ref.paramCount; ++i) {
                const IrClipParam& p = cc.params()[ref.paramOffset + i];
                shape.params.push_back(Length{p.value, static_cast<LengthUnit>(p.unit)});
            }
            writer.addClipShape(shape);
        }
    }
    if (view.grid().present()) {  // T-338: the pools are copied verbatim.
        const GridChunk& gc = view.grid();
        writer.m_gridTrackLists = toVector(gc.trackLists());
        writer.m_gridTracks = toVector(gc.tracks());
        writer.m_gridAreaLists = toVector(gc.areaLists());
        writer.m_gridAreas = toVector(gc.areas());
    }

    return writer;
}

}  // namespace xt::compile
