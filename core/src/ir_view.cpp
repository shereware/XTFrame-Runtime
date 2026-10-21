#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "crc32.h"
#include "path_verbs.h"
#include "property_accept.h"
#include "style_decode.h"
#include "xt/input.h"  // kMaxPlayers (T-194).
#include "xt/ir.h"
#include "xt/property.h"

/// See docs/design/02-ir-format.md (whole chapter) and docs/tasks/T-03.md "open validation rules".
/// Reads via reinterpret_cast to `#pragma pack(1)` structs (alignof == 1, so any byte offset is a
/// valid address for them) — chosen over memcpy so accessors stay truly zero-copy; this choice is
/// used consistently for every multi-byte read in this file.

namespace xt {

namespace {

constexpr size_t alignUp4(size_t v) { return (v + 3u) & ~static_cast<size_t>(3u); }

template <class T>
const T* ptrAt(const uint8_t* base, size_t offset) {
    return reinterpret_cast<const T*>(base + offset);
}

/// Bounds-checked forward cursor over one chunk's payload bytes.
class ByteCursor {
public:
    ByteCursor(const uint8_t* base, size_t size) : m_base(base), m_size(size) {}

    bool readU32(uint32_t& out) {
        if (!fits(4)) {
            return false;
        }
        out = *ptrAt<uint32_t>(m_base, m_pos);
        m_pos += 4;
        return true;
    }

    bool readF32(float& out) {
        if (!fits(4)) {
            return false;
        }
        out = *ptrAt<float>(m_base, m_pos);
        m_pos += 4;
        return true;
    }

    /// T-233: a counted array of `recordSize`-byte records (an older, shorter layout of T) copied
    /// into `out`, each widened to T with the missing trailing fields value-initialized (0).
    template <class T>
    bool readCountedWidened(size_t recordSize, std::vector<T>& out) {
        uint32_t count = 0;
        if (!readU32(count) || recordSize > sizeof(T)) {
            return false;
        }
        size_t bytes = static_cast<size_t>(count) * recordSize;
        if (!fits(bytes)) {
            return false;
        }
        out.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            T rec{};
            std::memcpy(&rec, m_base + m_pos + static_cast<size_t>(i) * recordSize, recordSize);
            out[i] = rec;
        }
        m_pos += bytes;
        return true;
    }

    template <class T>
    bool readArray(uint32_t count, std::span<const T>& out) {
        size_t bytes = static_cast<size_t>(count) * sizeof(T);
        if (!fits(bytes)) {
            return false;
        }
        out = std::span<const T>(ptrAt<T>(m_base, m_pos), count);
        m_pos += bytes;
        return true;
    }

    template <class T>
    bool readCounted(std::span<const T>& out) {
        uint32_t count = 0;
        if (!readU32(count)) {
            return false;
        }
        return readArray<T>(count, out);
    }

    bool alignTo4() {
        size_t aligned = alignUp4(m_pos);
        if (aligned > m_size) {
            return false;
        }
        m_pos = aligned;
        return true;
    }
    size_t pos() const { return m_pos; }
    size_t remaining() const { return m_size - m_pos; }

private:
    bool fits(size_t n) const { return n <= m_size - m_pos; }

    const uint8_t* m_base;
    size_t m_size;
    size_t m_pos = 0;
};

}  // namespace

bool IrView::parseStrings(const uint8_t* payload, size_t payloadSize, StringChunk& out) {
    ByteCursor cur(payload, payloadSize);
    uint32_t count = 0;
    if (!cur.readU32(count)) {
        return false;
    }
    std::span<const uint32_t> offsets;
    std::span<const uint32_t> lengths;
    if (!cur.readArray<uint32_t>(count, offsets)) {
        return false;
    }
    if (!cur.readArray<uint32_t>(count, lengths)) {
        return false;
    }
    const uint8_t* data = payload + cur.pos();
    size_t dataSize = cur.remaining();
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t end = static_cast<uint64_t>(offsets[i]) + static_cast<uint64_t>(lengths[i]) + 1u;
        if (end > dataSize) {
            return false;
        }
        if (data[offsets[i] + lengths[i]] != 0) {
            return false;
        }
    }
    (void)payloadSize;
    out.m_count = count;
    out.m_offsets = offsets.data();
    out.m_lengths = lengths.data();
    out.m_data = reinterpret_cast<const char*>(data);
    out.m_dataSize = dataSize;
    return true;
}

bool IrView::parseAssets(const uint8_t* payload, size_t payloadSize, AssetChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_assets)) {
        return false;
    }
    out.m_payload = payload;
    out.m_payloadSize = payloadSize;
    for (const IrAsset& a : out.m_assets) {
        if (a.embedded != 0) {
            uint64_t end = static_cast<uint64_t>(a.dataOffset) + static_cast<uint64_t>(a.dataLength);
            if (end > payloadSize) {
                return false;
            }
        }
    }
    return true;
}

bool IrView::parseStyle(const uint8_t* payload, size_t payloadSize, StyleChunk& s) {
    ByteCursor cur(payload, payloadSize);
    if (!(cur.readCounted(s.m_rules) && cur.readCounted(s.m_selectorParts) && cur.readCounted(s.m_decls) &&
          cur.readCounted(s.m_keyframesRefs) && cur.readCounted(s.m_classIds) &&
          cur.readCounted(s.m_transitions) && cur.readCounted(s.m_animations) &&
          cur.readCounted(s.m_fontFamilies) && cur.readCounted(s.m_fontFaces) &&
          cur.readCounted(s.m_effects) && cur.readCounted(s.m_effectStops))) {
        return false;
    }
    // `gradientPool[]` (T-115, IR minor 1): a file written by an older compiler ends right after
    // `effectStops[]`, and then simply has no gradients (05.5c's initial value is `none` anyway).
    // The payload is 4-byte padded, so a few trailing pad bytes are not a truncated count either.
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    if (!cur.readCounted(s.m_gradients)) {
        return false;
    }
    // `floatPool[]` (T-218, IR minor 2): absent from older files, like `gradientPool[]` above.
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    if (!cur.readCounted(s.m_floatPool)) {
        return false;
    }
    // `easingPoints[]` (T-335, IR minor 6): absent from older files, like the two above.
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    return cur.readCounted(s.m_easingPoints);
}

bool IrView::parseScene(const uint8_t* payload, size_t payloadSize, uint16_t minor, SceneChunk& scene) {
    // T-233 (IR minor 4): IrPathData/IrShapeData grew the stroke taper / width profile fields; an
    // older file's shorter records are widened into owned copies (the new fields 0). T-243 (IR
    // minor 5): IrPathData grew skinIndex, widened the same way (and set to kIrNoSkin).
    const bool widen = minor < 4;
    const bool widenPaths = minor < 5;
    ByteCursor cur(payload, payloadSize);
    uint32_t name = 0;
    if (!cur.readU32(name)) {
        return false;
    }
    scene.m_name = name;
    if (!cur.readF32(scene.m_width) || !cur.readF32(scene.m_height)) {
        return false;
    }
    if (!cur.readCounted(scene.m_nodes)) {
        return false;
    }
    if (widenPaths) {
        auto paths = std::make_shared<std::vector<IrPathData>>();
        if (!cur.readCountedWidened(widen ? kIrPathDataSizeV3 : kIrPathDataSizeV4, *paths)) {
            return false;
        }
        for (IrPathData& path : *paths) {
            path.skinIndex = kIrNoSkin;
        }
        scene.m_paths = std::span<const IrPathData>(*paths);
        scene.m_widenedPaths = std::move(paths);
    } else if (!cur.readCounted(scene.m_paths)) {
        return false;
    }
    if (!cur.readCounted(scene.m_images)) {
        return false;
    }
    if (!cur.readCounted(scene.m_cameras)) {
        return false;
    }
    if (!cur.readCounted(scene.m_verbPool)) {
        return false;
    }
    if (!cur.alignTo4()) {
        return false;
    }
    if (!cur.readCounted(scene.m_floatPool) || !cur.readCounted(scene.m_stopPool)) {
        return false;
    }
    if (!cur.readCounted(scene.m_effects)) {
        return false;
    }
    // T-225 (IR minor 3): optional `modifiers[]`, `shapes[]` tail; absent from older files (and
    // from a scene without Modifier/Shape nodes), like STYL's trailing arrays.
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    if (!cur.readCounted(scene.m_modifiers)) {
        return false;
    }
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    if (widen) {
        auto shapes = std::make_shared<std::vector<IrShapeData>>();
        if (!cur.readCountedWidened(kIrShapeDataSizeV3, *shapes)) {
            return false;
        }
        scene.m_shapes = std::span<const IrShapeData>(*shapes);
        scene.m_widenedShapes = std::move(shapes);
        return true;
    }
    if (!cur.readCounted(scene.m_shapes)) {
        return false;
    }
    // T-243 (IR minor 5): the optional rig tail, all eight arrays or none (a scene without bones,
    // constraints, meshes or skins omits it).
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    if (!cur.readCounted(scene.m_bones) || !cur.readCounted(scene.m_constraints) ||
        !cur.readCounted(scene.m_meshes) || !cur.readCounted(scene.m_skins) ||
        !cur.readCounted(scene.m_tendonPool) || !cur.readCounted(scene.m_weightPool) ||
        !cur.readCounted(scene.m_meshVec2Pool) || !cur.readCounted(scene.m_meshIndexPool)) {
        return false;
    }
    return true;
}

bool IrView::parseTimeline(const uint8_t* payload, size_t payloadSize, TimelineChunk& tl) {
    ByteCursor cur(payload, payloadSize);
    uint32_t name = 0;
    if (!cur.readU32(name)) {
        return false;
    }
    tl.m_name = name;
    if (!cur.readF32(tl.m_duration) || !cur.readF32(tl.m_frameRate)) {
        return false;
    }
    return cur.readCounted(tl.m_tracks) && cur.readCounted(tl.m_keyframes) &&
           cur.readCounted(tl.m_floatPool) && cur.readCounted(tl.m_stopPool) && cur.readCounted(tl.m_markers);
}

bool IrView::parseDoc(const uint8_t* payload, size_t payloadSize, DocChunk& doc) {
    ByteCursor cur(payload, payloadSize);
    if (!(cur.readCounted(doc.m_elements) && cur.readCounted(doc.m_texts) &&
          cur.readCounted(doc.m_attrPool) && cur.readCounted(doc.m_classPool) &&
          cur.readCounted(doc.m_inlineDecls) && cur.readCounted(doc.m_cameras))) {
        return false;
    }
    // T-178: the optional rich-text tail (absent in blobs written before it).
    if (cur.pos() >= payloadSize) {
        return true;
    }
    if (!(cur.readCounted(doc.m_textRuns) && cur.readCounted(doc.m_textInlines) &&
          cur.readCounted(doc.m_textLinks))) {
        return false;
    }
    const IrTextRun* prev = nullptr;
    for (const IrTextRun& run : doc.m_textRuns) {
        // T-255: runs are grouped by ascending text, ascending start within one (Document looks a
        // text's runs up by binary search).
        if (prev != nullptr &&
            (run.text < prev->text || (run.text == prev->text && run.start < prev->start))) {
            return false;
        }
        prev = &run;
        if (run.text >= doc.m_texts.size() || run.start > run.end ||
            (run.declCount > 0 && (run.declOffset > doc.m_inlineDecls.size() ||
                                   run.declCount > doc.m_inlineDecls.size() - run.declOffset)) ||
            (run.inlineObject != 0xFFFFFFFFu && run.inlineObject >= doc.m_textInlines.size()) ||
            (run.link != 0xFFFFFFFFu && run.link >= doc.m_textLinks.size())) {
            return false;
        }
    }
    return true;
}

bool IrView::parseSourceMap(const uint8_t* payload, size_t payloadSize, SourceMapChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_fileNames) || !cur.readCounted(out.m_entries)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseVars(const uint8_t* payload, size_t payloadSize, VarsChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_tokens) || !cur.readCounted(out.m_fallbacks)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseCalcs(const uint8_t* payload, size_t payloadSize, CalcChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_calcs) || !cur.readCounted(out.m_ops)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseCustomStates(const uint8_t* payload, size_t payloadSize, CustomStateChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_names) || !cur.readCounted(out.m_groups)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseMedia(const uint8_t* payload, size_t payloadSize, MediaChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_media) || !cur.readCounted(out.m_queries) || !cur.readCounted(out.m_conds) ||
        !cur.readCounted(out.m_rules)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseTriggers(const uint8_t* payload, size_t payloadSize, TriggerChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_tables) || !cur.readCounted(out.m_triggers) ||
        !cur.readCounted(out.m_actions) || !cur.readCounted(out.m_conditions)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseLocalization(const uint8_t* payload, size_t payloadSize, LocalizationChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_keys) || !cur.readCounted(out.m_textKeys) ||
        !cur.readCounted(out.m_attrKeys) || !cur.readCounted(out.m_fallbacks) ||
        !cur.readCounted(out.m_fallbackFamilies)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parsePorts(const uint8_t* payload, size_t payloadSize, PortChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_ports) || !cur.readCounted(out.m_fields)) {
        return false;
    }
    out.m_present = true;
    // `defaults[]` (T-358, IR minor 7): absent from older files (no defaults); the payload is
    // 4-byte padded, so a few trailing pad bytes are not a truncated count.
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    if (!cur.readCounted(out.m_defaults)) {
        return false;
    }
    // `nested[]` (T-391): absent from older files.
    if (cur.remaining() < sizeof(uint32_t)) {
        return true;
    }
    return cur.readCounted(out.m_nested);
}

bool IrView::parseBindings(const uint8_t* payload, size_t payloadSize, uint16_t minor, BindingChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (minor < 5) {
        // T-243 (IR minor 5): IrBinding grew sceneNode; an older file's 24-byte records are
        // widened into an owned copy (bindings on the element itself).
        auto bindings = std::make_shared<std::vector<IrBinding>>();
        if (!cur.readCountedWidened(kIrBindingSizeV4, *bindings)) {
            return false;
        }
        for (IrBinding& b : *bindings) {
            b.sceneNode = kBindElementSelf;
        }
        out.m_bindings = std::span<const IrBinding>(*bindings);
        out.m_widenedBindings = std::move(bindings);
    } else if (!cur.readCounted(out.m_bindings)) {
        return false;
    }
    if (!cur.readCounted(out.m_sources) || !cur.readCounted(out.m_steps)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseRepeats(const uint8_t* payload, size_t payloadSize, RepeatChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_repeats)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseShaders(const uint8_t* payload, size_t payloadSize, ShaderChunk& out) {
    ByteCursor cur(payload, payloadSize);
    std::span<const uint8_t> text;
    if (!cur.readCounted(out.m_shaders) || !cur.readCounted(out.m_params) || !cur.readCounted(text)) {
        return false;
    }
    out.m_text = reinterpret_cast<const char*>(text.data());
    out.m_textSize = text.size();
    out.m_present = true;
    return true;
}

bool IrView::parseClipShapes(const uint8_t* payload, size_t payloadSize, ClipShapeChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_shapes) || !cur.readCounted(out.m_params)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseGrid(const uint8_t* payload, size_t payloadSize, GridChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_trackLists) || !cur.readCounted(out.m_tracks) ||
        !cur.readCounted(out.m_areaLists) || !cur.readCounted(out.m_areas)) {
        return false;
    }
    out.m_present = true;
    return true;
}

bool IrView::parseMeta(const uint8_t* payload, size_t payloadSize, MetaChunk& meta) {
    if (payloadSize != sizeof(IrMeta)) {
        return false;
    }
    meta.m_meta = reinterpret_cast<const IrMeta*>(payload);
    return true;
}

bool IrView::parseSymbols(const uint8_t* payload, size_t payloadSize, SymbolChunk& out) {
    ByteCursor cur(payload, payloadSize);
    uint32_t count = 0;
    if (!cur.readU32(count)) {
        return false;
    }
    // Each instance record is at least 12 bytes; reject an impossible count before reserving.
    if (static_cast<uint64_t>(count) * 12u > cur.remaining()) {
        return false;
    }
    out.m_instances.clear();
    out.m_instances.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        IrSymbolInstance inst;
        uint32_t symbol = 0;
        if (!cur.readU32(symbol) || !cur.readU32(inst.instanceElement) || !cur.readCounted(inst.paths)) {
            return false;
        }
        inst.symbol = symbol;
        out.m_instances.push_back(inst);
    }
    out.m_present = true;
    return true;
}

bool IrView::parseTimelineGroups(const uint8_t* payload, size_t payloadSize, TimelineGroupChunk& out) {
    ByteCursor cur(payload, payloadSize);
    if (!cur.readCounted(out.m_groups) || !cur.readCounted(out.m_markers) || !cur.readCounted(out.m_sounds) ||
        !cur.readCounted(out.m_members)) {
        return false;
    }
    out.m_present = true;
    return true;
}

namespace {

bool validStringId(StringId id, uint32_t count) { return id < count || id == kInvalidString; }

bool inPool(uint64_t offset, uint64_t count, uint64_t poolSize) { return offset + count <= poolSize; }

// StyleProp ids used by IrDecl::value when valueType == ListRef: style_decode::styleprop (05 §5.4),
// the numbers style_engine.cpp decodes with (A14: one definition, not a copy).
namespace sp = style_decode::styleprop;

/// T-255: the properties style_engine.cpp decodes as a ListRef into a STYL pool by prop alone,
/// whatever the declaration's valueType says.
bool isPoolListProp(uint16_t prop) {
    return prop == sp::Transition || prop == sp::Animation || prop == sp::FontFamily || prop == sp::Effect ||
           prop == sp::BackgroundGradient || prop == sp::TextEffect;
}

/// T-255: a plain Length value's unit byte names a LengthUnit up to UiScale (Calc only comes from
/// a kDeclCalc declaration), or T-335's Rem.
bool validLengthUnit(uint8_t unit) {
    return unit <= static_cast<uint8_t>(LengthUnit::UiScale) || unit == static_cast<uint8_t>(LengthUnit::Rem);
}

/// T-335: a LinearPoints easing names at least two finite points of the STYL `easingPoints[]`
/// with non-decreasing inputs (evalEasing's binary search relies on the order), at an offset the
/// runtime Easing's u16 can hold. Other kinds carry no pool reference.
bool validateEasing(const IrEasing& e, std::span<const EasingPoint> pool) {
    if (e.kind != static_cast<uint8_t>(EasingKind::LinearPoints)) {
        return true;
    }
    const uint32_t offset = irEasingPointOffset(e);
    if (e.stepCount < 2 || offset > 0xFFFFu || !inPool(offset, e.stepCount, pool.size())) {
        return false;
    }
    std::span<const EasingPoint> pts = pool.subspan(offset, e.stepCount);
    for (size_t i = 0; i < pts.size(); ++i) {
        if (!std::isfinite(pts[i].in) || !std::isfinite(pts[i].out) || (i > 0 && pts[i].in < pts[i - 1].in)) {
            return false;
        }
    }
    return true;
}

bool validateDeclListRef(const IrDecl& decl, const StyleChunk& style) {
    if (static_cast<ValueType>(decl.valueType) != ValueType::ListRef) {
        // T-255: a pool-list property is read as a ListRef whatever its valueType, so it must be
        // one (or a var() reference, which is substituted before the value is read).
        return !isPoolListProp(decl.prop) || decl.valueType == kDeclVarRef;
    }
    uint32_t offset = irValueU32(decl.value, 0);
    uint32_t count = irValueU32(decl.value, 1);
    if (decl.prop == prop::PathData) {
        // T-218: a scene-target `d`: points and verbs, both in STYL's floatPool[]. T-255: its own
        // verbs (count 0 = the node's) must account for exactly its points.
        const uint32_t verbOffset = irValueU32(decl.value, 2);
        const uint32_t verbCount = irValueU32(decl.value, 3);
        return inPool(offset, count, style.floatPool().size()) && (count % 2) == 0 &&
               inPool(verbOffset, verbCount, style.floatPool().size()) &&
               (verbCount == 0 ||
                floatVerbsMatchPoints(style.floatPool().data() + verbOffset, verbCount, count));
    }
    if (decl.prop == prop::DashArray) {
        // T-236: a scene-target `stroke-dasharray`, in STYL's floatPool[] (count 0 = none).
        return inPool(offset, count, style.floatPool().size());
    }
    switch (decl.prop) {
        case sp::Transition:
            return inPool(offset, count, style.transitions().size());
        case sp::Animation:
            return inPool(offset, count, style.animations().size());
        case sp::FontFamily:
            return inPool(offset, count, style.fontFamilies().size());
        case sp::Effect:
        case sp::TextEffect:
            // T-255: style_engine.cpp resolves a sequence into a kMaxEffectSequence-entry array.
            return count <= kMaxEffectSequence && inPool(offset, count, style.effects().size());
        case sp::BackgroundGradient:
            return inPool(offset, count, style.gradients().size());
        default:
            return false;
    }
}

/// T-332 (05 §5.9.8): a `clip-path` declaration is an Enum (or a var() reference) whose value is
/// 0 (`none`) or 1 + a `CLIP` shape index.
bool validateDeclClipPath(const IrDecl& decl, const ClipShapeChunk& clips) {
    if (decl.prop != sp::ClipPath || decl.valueType == kDeclVarRef) {
        return true;
    }
    if (static_cast<ValueType>(decl.valueType) != ValueType::Enum) {
        return false;
    }
    const uint32_t value = irValueU32(decl.value, 0);
    return value == 0 || static_cast<uint64_t>(value) <= clips.shapes().size();
}

/// T-338 (06 §6.15): the grid StyleProps -- the template Enums index the `GRID` lists, the line
/// Enums are kGridLine* encodings, `justify-items` an AlignItems value, `grid-area` a string.
bool validateDeclGrid(const IrDecl& decl, const GridChunk& grid) {
    if (decl.prop < sp::GridTemplateColumns || decl.prop > sp::GridArea || decl.valueType == kDeclVarRef) {
        return true;
    }
    if (decl.prop == sp::GridArea) {
        return static_cast<ValueType>(decl.valueType) == ValueType::StringRef;
    }
    if (static_cast<ValueType>(decl.valueType) != ValueType::Enum) {
        return false;
    }
    const uint32_t value = irValueU32(decl.value, 0);
    switch (decl.prop) {
        case sp::GridTemplateColumns:
        case sp::GridTemplateRows:
            return value == 0 || static_cast<uint64_t>(value) <= grid.trackLists().size();
        case sp::GridTemplateAreas:
            return value == 0 || static_cast<uint64_t>(value) <= grid.areaLists().size();
        case sp::JustifyItems:
            return value <= static_cast<uint32_t>(AlignItems::Center);
        default:
            return validGridLine(value);
    }
}

/// T-338: every `GRID` list lies inside its pool and holds at most kMaxGridTracks entries; tracks
/// have a known kind and a finite, non-negative value; an area has a valid name and a non-empty
/// rectangle inside its list's matrix.
bool validateGrid(const GridChunk& grid, uint32_t stringCount) {
    for (const IrGridList& list : grid.trackLists()) {
        if (list.count == 0 || list.count > kMaxGridTracks ||
            !inPool(list.offset, list.count, grid.tracks().size())) {
            return false;
        }
    }
    for (const IrGridTrack& track : grid.tracks()) {
        if (track.kind > static_cast<uint8_t>(GridTrackKind::Auto) || !std::isfinite(track.value) ||
            track.value < 0.0f) {
            return false;
        }
    }
    for (const IrGridList& list : grid.areaLists()) {
        if (list.rows == 0 || list.cols == 0 || list.rows > kMaxGridTracks || list.cols > kMaxGridTracks ||
            !inPool(list.offset, list.count, grid.areas().size())) {
            return false;
        }
        for (uint32_t i = 0; i < list.count; ++i) {
            const IrGridArea& area = grid.areas()[list.offset + i];
            if (area.name >= stringCount || area.rowStart >= area.rowEnd || area.rowEnd > list.rows ||
                area.colStart >= area.colEnd || area.colEnd > list.cols) {
                return false;
            }
        }
    }
    return true;
}

/// T-332: every `CLIP` shape has a known kind, the parameter count of that kind, a range inside
/// the parameter pool and px / % units (Auto only as a circle / ellipse radius keyword).
bool validateClipShapes(const ClipShapeChunk& clips) {
    std::span<const IrClipParam> params = clips.params();
    for (const IrClipShape& shape : clips.shapes()) {
        uint32_t radiusSlots = 0;  // Leading parameters that may be an Auto keyword.
        switch (static_cast<ClipShapeKind>(shape.kind)) {
            case ClipShapeKind::Inset:
                if (shape.paramCount != kClipInsetParamCount) {
                    return false;
                }
                break;
            case ClipShapeKind::Circle:
                if (shape.paramCount != kClipCircleParamCount) {
                    return false;
                }
                radiusSlots = 1;
                break;
            case ClipShapeKind::Ellipse:
                if (shape.paramCount != kClipEllipseParamCount) {
                    return false;
                }
                radiusSlots = 2;
                break;
            case ClipShapeKind::Polygon:
                if (shape.paramCount < 6 || (shape.paramCount % 2) != 0 ||
                    shape.paramCount > 2u * kMaxClipPolygonVertices) {
                    return false;
                }
                break;
            default:
                return false;
        }
        if (!inPool(shape.paramOffset, shape.paramCount, params.size())) {
            return false;
        }
        for (uint32_t i = 0; i < shape.paramCount; ++i) {
            const IrClipParam& p = params[shape.paramOffset + i];
            if (!std::isfinite(p.value)) {
                return false;
            }
            const auto unit = static_cast<LengthUnit>(p.unit);
            if (unit == LengthUnit::Auto) {
                if (i >= radiusSlots ||
                    (p.value != kClipRadiusClosestSide && p.value != kClipRadiusFarthestSide)) {
                    return false;
                }
            } else if (unit != LengthUnit::Px && unit != LengthUnit::Percent) {
                return false;
            }
        }
    }
    return true;
}

bool validateDeclStringId(const IrDecl& decl, uint32_t stringCount) {
    if (static_cast<ValueType>(decl.valueType) != ValueType::StringRef) {
        return true;
    }
    return validStringId(irValueU32(decl.value, 0), stringCount);
}

/// T-170: a kDeclVarRef declaration names a valid token string and a fallback that exists.
bool validateDeclVarRef(const IrDecl& decl, uint32_t stringCount, uint64_t fallbackCount) {
    if (decl.valueType != kDeclVarRef) {
        return true;
    }
    uint32_t name = irValueU32(decl.value, 0);
    uint32_t fallback = irValueU32(decl.value, 1);
    return name < stringCount && (fallback == kNoVarFallback || fallback < fallbackCount);
}

/// T-176: a kDeclCalc declaration names a program that exists.
bool validateDeclCalc(const IrDecl& decl, uint64_t calcCount) {
    return decl.valueType != kDeclCalc || irValueU32(decl.value, 0) < calcCount;
}

/// T-255: a Length declaration's unit byte is a plain unit.
bool validateDeclLength(const IrDecl& decl) {
    return static_cast<ValueType>(decl.valueType) != ValueType::Length || validLengthUnit(decl.value[4]);
}

/// T-255b: the `font-size` cascade (style_engine.cpp) reads value[4] as a unit whatever the
/// declaration's type, so a `font-size` declaration is a Length, a calc() or a var() reference.
bool validateDeclFontSize(const IrDecl& decl) {
    return decl.prop != prop::FontSize || static_cast<ValueType>(decl.valueType) == ValueType::Length ||
           decl.valueType == kDeclCalc || decl.valueType == kDeclVarRef;
}

/// T-257b: the properties the element cascade (style_engine.cpp applyGeneralDecl) decodes as a
/// Length by property alone -- value[4] cast to a LengthUnit -- whatever the declaration's type:
/// transform-origin (AnchorX/Y, copied into ElementData::originX/Y and read by layout's
/// resolveLength), translate (TranslateX/Y), `line-height`, and every Length-typed property of the
/// layout and paint ranges (`width` ... `letter-spacing`, `background-color` ...
/// `text-decoration-thickness`:
/// sizes, insets, margins, paddings, borders, radii, `perspective`, `perspective-origin`, ...).
bool cascadeReadsLength(uint16_t p) {
    if (p == prop::AnchorX || p == prop::AnchorY || p == prop::TranslateX || p == prop::TranslateY ||
        p == prop::LineHeight) {
        return true;
    }
    if ((p >= prop::Width && p <= prop::LetterSpacing) || (p >= prop::BackgroundColor && p <= prop::Order)) {
        return propertyDefined(p) && propertyType(p) == ValueType::Length;
    }
    return false;
}

/// T-257b (fuzz_ir: a transform-origin `em` reaching layout): a declaration cascadeReadsLength()
/// names carries a unit the cascade can resolve. A Length's own unit is checked by
/// validateDeclLength(); any other plain value (a Float, as the compiler writes a scene-target
/// AnchorX) must leave the unit byte 0 (Px), the only unit it can mean -- a Float-typed
/// transform-origin with an `em` (or out-of-range) byte would otherwise reach
/// ElementData::originX/Y unresolved. calc() and var() declarations carry no unit byte.
bool validateDeclCascadeLength(const IrDecl& decl) {
    if (!cascadeReadsLength(decl.prop) || decl.valueType == kDeclCalc || decl.valueType == kDeclVarRef ||
        static_cast<ValueType>(decl.valueType) == ValueType::Length) {
        return true;
    }
    return decl.value[4] == static_cast<uint8_t>(LengthUnit::Px);
}

/// T-257b: document_load.cpp's autoplay rule for an `anim` element -- `autoplay` absent, empty or
/// "true" (the first such attribute wins, as findAttribute() does). Call after the element's
/// attribute slice has been checked against the pool.
bool animAutoplays(const IrView& view, const IrDocElement& el) {
    std::span<const IrAttr> attrs = view.doc().attrPool();
    for (uint32_t i = 0; i < el.attrCount; ++i) {
        const IrAttr& attr = attrs[el.attrOffset + i];
        if (view.string(attr.name) == "autoplay") {
            const std::string_view value = view.string(attr.value);
            return value.empty() || value == "true";
        }
    }
    return true;
}

/// T-176: every program slices inside ops[], and each op is well-formed (known kind, a
/// PushLength unit that is neither Auto nor Calc, a PushVar name that is a string).
bool validateCalcs(const CalcChunk& chunk, uint32_t stringCount) {
    const uint64_t opCount = chunk.ops().size();
    for (const IrCalc& calc : chunk.calcs()) {
        if (calc.opCount == 0 || static_cast<uint64_t>(calc.opOffset) + calc.opCount > opCount) {
            return false;
        }
    }
    for (const IrCalcOp& op : chunk.ops()) {
        if (op.op > static_cast<uint8_t>(CalcOpKind::Max)) {  // T-330: Min/Max.
            return false;
        }
        const auto kind = static_cast<CalcOpKind>(op.op);
        if (kind == CalcOpKind::PushLength &&
            (!validLengthUnit(op.unit) || op.unit == static_cast<uint8_t>(LengthUnit::Auto))) {
            return false;
        }
        if (kind == CalcOpKind::PushVar &&
            (op.name >= stringCount ||
             (op.hasFallback != 0 && op.unit != kCalcUnitNumber &&
              (!validLengthUnit(op.unit) || op.unit == static_cast<uint8_t>(LengthUnit::Auto))))) {
            return false;
        }
    }
    return true;
}

/// T-176: every range stays inside its pool, features are known, and the rule map is strictly
/// ascending over existing rules and media.
bool validateMedia(const MediaChunk& chunk, uint64_t ruleCount) {
    for (const IrMedia& media : chunk.media()) {
        if (static_cast<uint64_t>(media.queryOffset) + media.queryCount > chunk.queries().size()) {
            return false;
        }
    }
    for (const IrMediaQuery& query : chunk.queries()) {
        if (static_cast<uint64_t>(query.condOffset) + query.condCount > chunk.conds().size()) {
            return false;
        }
    }
    for (const IrMediaCond& cond : chunk.conds()) {
        if (cond.feature > static_cast<uint8_t>(MediaFeature::Resolution)) {  // T-330.
            return false;
        }
    }
    uint64_t next = 0;
    for (const IrMediaRule& entry : chunk.rules()) {
        if (entry.rule < next || entry.rule >= ruleCount || entry.media >= chunk.media().size()) {
            return false;
        }
        next = static_cast<uint64_t>(entry.rule) + 1u;
    }
    return true;
}

/// T-170: a VARS token carries a typed value (Float/Length/Color) or its text (StringRef).
bool validateVar(const IrVar& var, uint32_t stringCount) {
    if (var.name >= stringCount) {
        return false;
    }
    switch (static_cast<ValueType>(var.valueType)) {
        case ValueType::Float:
        case ValueType::Color:
            return true;
        case ValueType::Length:
            return validLengthUnit(var.value[4]);  // T-255

        case ValueType::StringRef:
            return irValueU32(var.value, 0) < stringCount;
        default:
            return false;
    }
}

bool validEffectKind(uint8_t kind) { return kind <= 8 || kind == 128; }

bool validateEffect(const IrEffect& fx, uint64_t stopPoolSize) {
    if (!validEffectKind(fx.kind)) {
        return false;
    }
    if (fx.enabled > 1) {
        return false;
    }
    return inPool(fx.stopOffset, fx.stopCount, stopPoolSize);
}

/// 05.5c (T-115): a `gradientPool[]` entry must carry a known GradientKind (never None, which is
/// encoded as a count-0 ListRef instead) and an in-range stop range.
bool validateGradient(const IrGradient& gradient, uint64_t stopPoolSize) {
    if (gradient.kind != static_cast<uint8_t>(GradientKind::Linear) &&
        gradient.kind != static_cast<uint8_t>(GradientKind::Radial) &&
        gradient.kind != static_cast<uint8_t>(GradientKind::Conic)) {  // T-201.
        return false;
    }
    // T-255b: each params[] unit is a plain LengthUnit, the rule a Length value's unit follows
    // (style_engine.cpp casts the byte straight to LengthUnit).
    for (uint8_t unit : gradient.units) {
        if (!validLengthUnit(unit)) {
            return false;
        }
    }
    return inPool(gradient.stopOffset, gradient.stopCount, stopPoolSize);
}

bool validateCamera(const IrCameraData& cam) { return cam.kind <= 1 && cam.useTarget <= 1; }

uint32_t pathVerbPoints(PathVerb verb) {
    switch (verb) {
        case PathVerb::Move:
            return 2;
        case PathVerb::Line:
            return 2;
        case PathVerb::Cubic:
            return 6;
        case PathVerb::Close:
            return 0;
    }
    return 0xFFFFFFFFu;  // unknown verb value; caller treats this as invalid
}

/// T-243 (32 §32.2.2): a path's vertex count, one per Move / Line / Cubic verb (Close has none).
uint32_t pathVertexCount(std::span<const uint8_t> verbs) {
    uint32_t n = 0;
    for (uint8_t v : verbs) {
        n += v != static_cast<uint8_t>(PathVerb::Close) ? 1u : 0u;
    }
    return n;
}

/// T-243 (32 §32.2): a scene's rig records -- constraint kinds and targets, skin skeletons, bones,
/// pools and weight indices, mesh assets, pools and triangle indices.
bool validateRig(const SceneChunk& scene, std::span<const IrSceneNode> nodes, size_t assetCount) {
    auto kindOf = [&](uint32_t index) { return static_cast<NodeKind>(nodes[index].kind); };
    for (const IrSceneNode& node : nodes) {
        if (static_cast<NodeKind>(node.kind) != NodeKind::Constraint) {
            continue;
        }
        const IrConstraintData& c = scene.constraints()[node.kindIndex];
        if (c.kind > static_cast<uint8_t>(ConstraintKind::Spring) || c.space > 1 || c.enabled > 1 ||
            (c.target != kIrNoTarget && c.target >= nodes.size())) {
            return false;
        }
    }
    for (const IrSkinData& skin : scene.skins()) {
        if (skin.skeleton >= nodes.size() || kindOf(skin.skeleton) != NodeKind::Group ||
            !inPool(skin.tendonOffset, skin.tendonCount, scene.tendonPool().size()) ||
            !inPool(skin.weightOffset, skin.weightCount, scene.weightPool().size())) {
            return false;
        }
        for (uint32_t i = 0; i < skin.tendonCount; ++i) {
            const uint32_t bone = scene.tendonPool()[skin.tendonOffset + i].bone;
            if (bone >= nodes.size() || kindOf(bone) != NodeKind::Bone) {
                return false;
            }
        }
        for (uint32_t i = 0; i < skin.weightCount; ++i) {
            const uint32_t idx4 = scene.weightPool()[skin.weightOffset + i].idx4;
            for (uint32_t k = 0; k < 4; ++k) {
                if (((idx4 >> (8 * k)) & 0xFFu) >= skin.tendonCount) {
                    return false;
                }
            }
        }
    }
    for (const IrMeshData& mesh : scene.meshes()) {
        if (mesh.asset >= assetCount || mesh.smooth > 1 ||
            !inPool(mesh.posOffset, uint64_t{mesh.vertexCount} * 2, scene.meshVec2Pool().size()) ||
            !inPool(mesh.indexOffset, mesh.indexCount, scene.meshIndexPool().size()) ||
            mesh.indexCount % 3 != 0) {
            return false;
        }
        for (uint32_t i = 0; i < mesh.indexCount; ++i) {
            if (scene.meshIndexPool()[mesh.indexOffset + i] >= mesh.vertexCount) {
                return false;
            }
        }
        if (mesh.skinIndex != kIrNoSkin && (mesh.skinIndex >= scene.skins().size() ||
                                            scene.skins()[mesh.skinIndex].weightCount != mesh.vertexCount)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view StringChunk::get(StringId id) const {
    if (id >= m_count) {
        return {};
    }
    return std::string_view(m_data + m_offsets[id], m_lengths[id]);
}

std::span<const uint8_t> AssetChunk::data(const IrAsset& asset) const {
    if (asset.embedded == 0) {
        return {};
    }
    return std::span<const uint8_t>(m_payload + asset.dataOffset, asset.dataLength);
}

Result<IrView> IrView::open(IrBlob blob, IrReadOptions options) {
    if (blob.data == nullptr || blob.size < sizeof(IrHeader)) {
        return Result<IrView>::error(Status::kInvalidIr);
    }
    const IrHeader* header = reinterpret_cast<const IrHeader*>(blob.data);
    if (std::memcmp(header->magic, "XTIR", 4) != 0) {
        return Result<IrView>::error(Status::kInvalidIr);
    }
    if (header->major != kIrFormatMajor) {
        return Result<IrView>::error(Status::kInvalidIr);
    }
    if (static_cast<uint64_t>(header->totalLength) != static_cast<uint64_t>(blob.size)) {
        return Result<IrView>::error(Status::kInvalidIr);
    }

    IrView view;
    view.m_blob = blob;

    enum Stage { S_STRT, S_ASST, S_STYL, S_SCEN, S_TLIN, S_DOCT_DONE, S_SRCM_DONE, S_META_DONE };
    Stage stage = S_STRT;

    size_t offset = sizeof(IrHeader);
    for (uint32_t i = 0; i < header->chunkCount; ++i) {
        if (offset % 4u != 0) {
            return Result<IrView>::error(Status::kInvalidIr);
        }
        if (offset + sizeof(IrChunkHeader) > blob.size) {
            return Result<IrView>::error(Status::kInvalidIr);
        }
        const IrChunkHeader* ch = reinterpret_cast<const IrChunkHeader*>(blob.data + offset);
        size_t payloadStart = offset + sizeof(IrChunkHeader);
        if (static_cast<uint64_t>(ch->payloadLength) > static_cast<uint64_t>(blob.size - payloadStart)) {
            return Result<IrView>::error(Status::kInvalidIr);
        }
        const uint8_t* payload = blob.data + payloadStart;
        size_t payloadLen = ch->payloadLength;

        if (options.verifyCrc) {
            if (crc32(payload, payloadLen) != ch->crc32) {
                return Result<IrView>::error(Status::kInvalidIr);
            }
        }

        uint32_t type = fourcc(ch->type[0], ch->type[1], ch->type[2], ch->type[3]);

        switch (type) {
            case kChunkSTRT:
                if (stage != S_STRT || !parseStrings(payload, payloadLen, view.m_strings)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                stage = S_ASST;
                break;
            case kChunkASST:
                if (stage != S_ASST || !parseAssets(payload, payloadLen, view.m_assets)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                stage = S_STYL;
                break;
            case kChunkSTYL:
                if (stage != S_STYL || !parseStyle(payload, payloadLen, view.m_style)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                stage = S_SCEN;
                break;
            case kChunkSCEN: {
                if (stage != S_SCEN) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                SceneChunk sc;
                if (!parseScene(payload, payloadLen, header->minor, sc)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                view.m_scenes.push_back(sc);
                break;
            }
            case kChunkTLIN: {
                if (stage != S_SCEN && stage != S_TLIN) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                TimelineChunk tc;
                if (!parseTimeline(payload, payloadLen, tc)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                view.m_timelines.push_back(tc);
                stage = S_TLIN;
                break;
            }
            case kChunkDOCT:
                if ((stage != S_SCEN && stage != S_TLIN) || !parseDoc(payload, payloadLen, view.m_doc)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                stage = S_DOCT_DONE;
                break;
            case kChunkSRCM:
                if (stage != S_DOCT_DONE || !parseSourceMap(payload, payloadLen, view.m_sourceMap)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                stage = S_SRCM_DONE;
                break;
            case kChunkMETA:
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE) ||
                    !parseMeta(payload, payloadLen, view.m_meta)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                stage = S_META_DONE;
                break;
            case kChunkSYMB:
                // T-171: optional, after DOCT (the writer puts it last); leaves `stage` alone so it
                // does not constrain the optional chunks around it.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_symbols.present() || !parseSymbols(payload, payloadLen, view.m_symbols)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkTGRP:
                // T-172: the `@timeline` group chunk comes after every other known chunk.
                // Like SYMB: leaves `stage` alone and may appear at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_timelineGroups.present() ||
                    !parseTimelineGroups(payload, payloadLen, view.m_timelineGroups)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkSTAT:
                // T-174: optional, after DOCT (the writer puts it near the end); at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_customStates.present() ||
                    !parseCustomStates(payload, payloadLen, view.m_customStates)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkTRIG:
                // T-174: optional, after DOCT; at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_triggers.present() || !parseTriggers(payload, payloadLen, view.m_triggers)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkSTRS:
                // T-179: like VARS -- accepted anywhere after STYL, at most once.
                if (stage < S_SCEN || view.m_localization.present() ||
                    !parseLocalization(payload, payloadLen, view.m_localization)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkPORT:
                // T-175: optional, after DOCT; at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_ports.present() || !parsePorts(payload, payloadLen, view.m_ports)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkBIND:
                // T-175: optional, after DOCT; at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_bindings.present() ||
                    !parseBindings(payload, payloadLen, header->minor, view.m_bindings)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkRPT:
                // T-177: optional, after DOCT; at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_repeats.present() || !parseRepeats(payload, payloadLen, view.m_repeats)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkSHDR:
                // T-203: optional, after DOCT; at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_shaders.present() || !parseShaders(payload, payloadLen, view.m_shaders)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkCLIP:
                // T-332: optional, after DOCT; at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_clipShapes.present() || !parseClipShapes(payload, payloadLen, view.m_clipShapes)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkGRID:
                // T-338: optional, after DOCT; at most once.
                if ((stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) ||
                    view.m_grid.present() || !parseGrid(payload, payloadLen, view.m_grid)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkVARS:
                // T-170: written last, but accepted anywhere after STYL; at most once.
                if (stage < S_SCEN || view.m_vars.present() || !parseVars(payload, payloadLen, view.m_vars)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkCALC:
                // T-176: like VARS -- accepted anywhere after STYL, at most once.
                if (stage < S_SCEN || view.m_calcs.present() ||
                    !parseCalcs(payload, payloadLen, view.m_calcs)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            case kChunkMDIA:
                // T-176: like VARS -- accepted anywhere after STYL, at most once.
                if (stage < S_SCEN || view.m_media.present() ||
                    !parseMedia(payload, payloadLen, view.m_media)) {
                    return Result<IrView>::error(Status::kInvalidIr);
                }
                break;
            default:
                // Unknown chunk type: skip for minor-version forward compatibility.
                break;
        }

        offset = payloadStart + payloadLen;
    }

    if (stage != S_DOCT_DONE && stage != S_SRCM_DONE && stage != S_META_DONE) {
        return Result<IrView>::error(
            Status::kInvalidIr);  // missing DOCT (also catches missing STRT/ASST/STYL)
    }
    if (offset != blob.size) {
        return Result<IrView>::error(Status::kInvalidIr);  // trailing bytes past the last chunk
    }

    Status vs = validate(view);
    if (vs != Status::kOk) {
        return Result<IrView>::error(vs);
    }
    // T-263c: see loadSerial(). Atomic because IR blobs may be opened on a loading thread; a
    // wrap after 2^32 opens skips 0, which means "never opened".
    static std::atomic<uint32_t> nextSerial{1};
    view.m_loadSerial = nextSerial.fetch_add(1, std::memory_order_relaxed);
    if (view.m_loadSerial == 0) {
        view.m_loadSerial = nextSerial.fetch_add(1, std::memory_order_relaxed);
    }
    return Result<IrView>::ok(std::move(view));
}

Status IrView::validate(const IrView& view) {
    uint32_t stringCount = view.m_strings.count();

    // -- ASST --
    for (const IrAsset& a : view.m_assets.assets()) {
        if (!validStringId(a.name, stringCount)) {
            return Status::kInvalidIr;
        }
    }

    // -- STYL --
    const StyleChunk& style = view.m_style;
    for (const IrRule& rule : style.rules()) {
        if (!inPool(rule.selOffset, rule.selCount, style.selectorParts().size())) {
            return Status::kInvalidIr;
        }
        // T-173: a chain starts with a compound, never with a condition part. T-218: a
        // scene-target part is only ever the chain's second part.
        if (rule.selCount != 0 && style.selectorParts()[rule.selOffset].combinator >= kIrSelCondition) {
            return Status::kInvalidIr;
        }
        for (uint32_t i = 2; i < rule.selCount; ++i) {
            if (style.selectorParts()[rule.selOffset + i].combinator == kIrSelSceneTarget) {
                return Status::kInvalidIr;
            }
        }
        if (!inPool(rule.declOffset, rule.declCount, style.decls().size())) {
            return Status::kInvalidIr;
        }
    }
    for (const IrSelPart& part : style.selectorParts()) {
        if (part.combinator > kIrSelSceneTarget) {
            return Status::kInvalidIr;
        }
        if (part.combinator == kIrSelSceneTarget) {
            // T-218: IrSceneTarget code in classCount; a StringId (or kInvalidString) in tag.
            const bool any = part.classCount == static_cast<uint8_t>(IrSceneTarget::Any);
            if (part.classCount > static_cast<uint8_t>(IrSceneTarget::Any) || part.classOffset != 0 ||
                part.stateMask != 0 || part.id != kInvalidString || !validStringId(part.tag, stringCount) ||
                any != (part.tag == kInvalidString)) {
                return Status::kInvalidIr;
            }
            continue;
        }
        if (part.combinator == kIrSelCondition) {
            // T-173: a condition part (xt/ir_pod.h's IrSelPart). NthChild carries two int32s in
            // tag/id; every other code carries StringIds (or kInvalidString) there.
            const uint8_t code = static_cast<uint8_t>(part.classCount & ~kIrSelCondNegated);
            if (code == 0 || code > kIrSelCondLast || part.classOffset != 0) {
                return Status::kInvalidIr;
            }
            if (code == static_cast<uint8_t>(IrSelCond::FocusPlayer) &&
                (part.tag >= kMaxPlayers || part.id != kInvalidString)) {
                return Status::kInvalidIr;  // T-194: `tag` is a player index.
            }
            if (code != static_cast<uint8_t>(IrSelCond::NthChild) &&
                code != static_cast<uint8_t>(IrSelCond::FocusPlayer) &&
                (!validStringId(part.tag, stringCount) || !validStringId(part.id, stringCount))) {
                return Status::kInvalidIr;
            }
            continue;
        }
        if (!validStringId(part.tag, stringCount) || !validStringId(part.id, stringCount)) {
            return Status::kInvalidIr;
        }
        if (!inPool(part.classOffset, part.classCount, style.classIds().size())) {
            return Status::kInvalidIr;
        }
    }
    for (uint32_t classId : style.classIds()) {
        if (!validStringId(classId, stringCount)) {
            return Status::kInvalidIr;
        }
    }
    for (uint32_t fam : style.fontFamilies()) {
        if (!validStringId(fam, stringCount)) {
            return Status::kInvalidIr;
        }
    }
    const uint64_t varFallbackCount = view.m_vars.fallbacks().size();
    const uint64_t calcCount = view.m_calcs.calcs().size();
    // -- CALC / MDIA (T-176) --
    if (!validateCalcs(view.m_calcs, stringCount) || !validateMedia(view.m_media, style.rules().size())) {
        return Status::kInvalidIr;
    }
    // -- CLIP (T-332) --
    if (!validateClipShapes(view.m_clipShapes)) {
        return Status::kInvalidIr;
    }
    // -- GRID (T-338) --
    if (!validateGrid(view.m_grid, stringCount)) {
        return Status::kInvalidIr;
    }
    for (const IrDecl& decl : style.decls()) {
        if (!validateDeclStringId(decl, stringCount) || !validateDeclListRef(decl, style) ||
            !validateDeclVarRef(decl, stringCount, varFallbackCount) || !validateDeclCalc(decl, calcCount) ||
            !validateDeclLength(decl) || !validateDeclFontSize(decl) || !validateDeclCascadeLength(decl) ||
            !validateDeclClipPath(decl, view.m_clipShapes) || !validateDeclGrid(decl, view.m_grid)) {
            return Status::kInvalidIr;
        }
    }
    // -- VARS (T-170) --
    for (const IrVar& var : view.m_vars.tokens()) {
        if (!validateVar(var, stringCount)) {
            return Status::kInvalidIr;
        }
    }
    for (const IrDecl& decl : view.m_vars.fallbacks()) {
        // A fallback is a plain value: never itself a reference.
        if (decl.valueType == kDeclVarRef || !validateDeclStringId(decl, stringCount) ||
            !validateDeclListRef(decl, style) || !validateDeclLength(decl) || !validateDeclFontSize(decl) ||
            !validateDeclCascadeLength(decl)) {
            return Status::kInvalidIr;
        }
    }
    for (const IrKeyframesRef& ref : style.keyframesRefs()) {
        if (!validStringId(ref.name, stringCount)) {
            return Status::kInvalidIr;
        }
        if (ref.timeline >= view.timelineCount()) {
            return Status::kInvalidIr;
        }
    }
    for (const IrAnimationSpec& anim : style.animations()) {
        if (anim.timeline >= view.timelineCount() || !validateEasing(anim.easing, style.easingPoints())) {
            return Status::kInvalidIr;
        }
    }
    for (const IrTransitionSpec& tr : style.transitions()) {
        if (!validateEasing(tr.easing, style.easingPoints())) {  // T-335.
            return Status::kInvalidIr;
        }
    }
    for (const IrFontFace& face : style.fontFaces()) {
        if (!validStringId(face.family, stringCount)) {
            return Status::kInvalidIr;
        }
        if (face.asset >= view.assets().assets().size()) {
            return Status::kInvalidIr;
        }
    }
    for (const IrEffect& fx : style.effects()) {
        if (!validateEffect(fx, style.effectStops().size())) {
            return Status::kInvalidIr;
        }
        // T-203: a Custom effect names an ASST entry (its `.xtfx` asset). The kind is not checked
        // here: an IR written before T-203 may carry a placeholder, and a Custom effect without a
        // SHDR entry is simply not applied (no ShaderRef reaches the backends).
        if (fx.kind == static_cast<uint8_t>(EffectKind::Custom) &&
            fx.shader >= view.assets().assets().size()) {
            return Status::kInvalidIr;
        }
    }
    for (const IrGradient& gradient : style.gradients()) {
        if (!validateGradient(gradient, style.effectStops().size())) {
            return Status::kInvalidIr;
        }
    }

    // -- SCEN --
    for (const SceneChunk& scene : view.m_scenes) {
        if (!validStringId(scene.name(), stringCount)) {
            return Status::kInvalidIr;
        }
        std::span<const IrSceneNode> nodes = scene.nodes();
        for (size_t idx = 0; idx < nodes.size(); ++idx) {
            const IrSceneNode& node = nodes[idx];
            if (idx == 0) {
                if (node.parent != 0xFFFFFFFFu) {
                    return Status::kInvalidIr;
                }
            } else if (node.parent >= idx) {
                return Status::kInvalidIr;
            }
            if (!validStringId(node.name, stringCount)) {
                return Status::kInvalidIr;
            }
            NodeKind kind = static_cast<NodeKind>(node.kind);
            // Scene nodes may only be Group/Path/Image/Mask/Camera (02.7), or (T-225) Modifier /
            // Shape; an out-of-range kind, blend or mask mode must be rejected here, never reach
            // instantiateScene's asserts.
            if (kind != NodeKind::Group && kind != NodeKind::Path && kind != NodeKind::Image &&
                kind != NodeKind::Mask && kind != NodeKind::Camera && kind != NodeKind::Modifier &&
                kind != NodeKind::Shape && kind != NodeKind::Bone && kind != NodeKind::Constraint &&
                kind != NodeKind::Mesh) {
                return Status::kInvalidIr;  // T-243: Bone / Constraint / Mesh.
            }
            if (node.blend > static_cast<uint8_t>(BlendMode::Add) ||
                node.maskMode > static_cast<uint8_t>(MaskMode::Intersect)) {
                return Status::kInvalidIr;
            }
            if (kind == NodeKind::Path) {
                if (node.kindIndex >= scene.paths().size()) {
                    return Status::kInvalidIr;
                }
            } else if (kind == NodeKind::Image) {
                if (node.kindIndex >= scene.images().size()) {
                    return Status::kInvalidIr;
                }
            } else if (kind == NodeKind::Camera) {
                if (node.kindIndex >= scene.cameras().size()) {
                    return Status::kInvalidIr;
                }
            } else if (kind == NodeKind::Modifier) {
                if (node.kindIndex >= scene.modifiers().size()) {
                    return Status::kInvalidIr;
                }
            } else if (kind == NodeKind::Shape) {
                if (node.kindIndex >= scene.shapes().size()) {
                    return Status::kInvalidIr;
                }
            } else if (kind == NodeKind::Bone) {  // T-243
                if (node.kindIndex >= scene.bones().size()) {
                    return Status::kInvalidIr;
                }
            } else if (kind == NodeKind::Constraint) {
                if (node.kindIndex >= scene.constraints().size()) {
                    return Status::kInvalidIr;
                }
            } else if (kind == NodeKind::Mesh) {
                if (node.kindIndex >= scene.meshes().size()) {
                    return Status::kInvalidIr;
                }
            }
            if (node.is3D > 1 || node.backfaceHidden > 1) {
                return Status::kInvalidIr;
            }
            if (node.effectCount > kMaxEffectSequence ||  // T-255
                !inPool(node.effectOffset, node.effectCount, scene.effects().size())) {
                return Status::kInvalidIr;
            }
        }
        for (const IrCameraData& cam : scene.cameras()) {
            if (!validateCamera(cam)) {
                return Status::kInvalidIr;
            }
        }
        for (const IrEffect& fx : scene.effects()) {
            if (!validateEffect(fx, scene.stopPool().size())) {
                return Status::kInvalidIr;
            }
            // T-255: a Custom scene effect names an ASST entry, as a STYL one does.
            if (fx.kind == static_cast<uint8_t>(EffectKind::Custom) &&
                fx.shader >= view.assets().assets().size()) {
                return Status::kInvalidIr;
            }
        }
        for (const IrPathData& path : scene.paths()) {
            if (path.fillRule > static_cast<uint8_t>(FillRule::EvenOdd) ||
                path.fillKind > static_cast<uint8_t>(PaintKind::Radial) ||
                path.strokeKind > static_cast<uint8_t>(PaintKind::Radial) ||
                path.cap > static_cast<uint8_t>(LineCap::Square) ||
                path.join > static_cast<uint8_t>(LineJoin::Bevel)) {
                return Status::kInvalidIr;
            }
            if (!inPool(path.verbOffset, path.verbCount, scene.verbPool().size())) {
                return Status::kInvalidIr;
            }
            if (!inPool(path.pointOffset, path.pointCount, scene.floatPool().size())) {
                return Status::kInvalidIr;
            }
            if (!inPool(path.fillStopOffset, path.fillStopCount, scene.stopPool().size())) {
                return Status::kInvalidIr;
            }
            if (!inPool(path.strokeStopOffset, path.strokeStopCount, scene.stopPool().size())) {
                return Status::kInvalidIr;
            }
            if (!inPool(path.dashOffset, path.dashCount, scene.floatPool().size())) {
                return Status::kInvalidIr;
            }
            if (!inPool(path.profileOffset, uint64_t{path.profileCount} * 2, scene.floatPool().size())) {
                return Status::kInvalidIr;  // T-233
            }
            if (path.fillStopCount > kMaxScenePaintListCount ||
                path.strokeStopCount > kMaxScenePaintListCount || path.dashCount > kMaxScenePaintListCount ||
                path.profileCount > kMaxScenePaintListCount) {
                return Status::kInvalidIr;  // T-255: the run-time fields are uint16.
            }
            uint64_t expectedPoints = 0;
            std::span<const uint8_t> verbs = scene.verbPool().subspan(path.verbOffset, path.verbCount);
            for (uint8_t v : verbs) {
                uint32_t n = pathVerbPoints(static_cast<PathVerb>(v));
                if (n == 0xFFFFFFFFu) {
                    return Status::kInvalidIr;
                }
                expectedPoints += n;
            }
            if (expectedPoints != path.pointCount) {
                return Status::kInvalidIr;
            }
            // T-243 (32 §32.2.2): a skinned path has 3 weights (p, in, out) per vertex.
            if (path.skinIndex != kIrNoSkin &&
                (path.skinIndex >= scene.skins().size() ||
                 scene.skins()[path.skinIndex].weightCount != uint64_t{pathVertexCount(verbs)} * 3)) {
                return Status::kInvalidIr;
            }
        }
        for (const IrModifierData& mod : scene.modifiers()) {  // T-225
            if (mod.kind > static_cast<uint8_t>(ModifierKind::Merge) || mod.enabled > 1) {
                return Status::kInvalidIr;
            }
        }
        for (const IrShapeData& shape : scene.shapes()) {  // T-225
            if (shape.kind > static_cast<uint8_t>(ShapeKind::Star) ||
                shape.fillRule > static_cast<uint8_t>(FillRule::EvenOdd) ||
                shape.fillKind > static_cast<uint8_t>(PaintKind::Radial) ||
                shape.strokeKind > static_cast<uint8_t>(PaintKind::Radial) ||
                shape.cap > static_cast<uint8_t>(LineCap::Square) ||
                shape.join > static_cast<uint8_t>(LineJoin::Bevel)) {
                return Status::kInvalidIr;
            }
            if (!inPool(shape.fillStopOffset, shape.fillStopCount, scene.stopPool().size()) ||
                !inPool(shape.strokeStopOffset, shape.strokeStopCount, scene.stopPool().size()) ||
                !inPool(shape.dashOffset, shape.dashCount, scene.floatPool().size()) ||
                !inPool(shape.profileOffset, uint64_t{shape.profileCount} * 2, scene.floatPool().size())) {
                return Status::kInvalidIr;
            }
            if (shape.fillStopCount > kMaxScenePaintListCount ||
                shape.strokeStopCount > kMaxScenePaintListCount ||
                shape.dashCount > kMaxScenePaintListCount || shape.profileCount > kMaxScenePaintListCount) {
                return Status::kInvalidIr;  // T-255: the run-time fields are uint16.
            }
        }
        for (const IrImageData& img : scene.images()) {
            if (img.asset >= view.assets().assets().size()) {
                return Status::kInvalidIr;
            }
        }
        if (!validateRig(scene, nodes, view.assets().assets().size())) {
            return Status::kInvalidIr;  // T-243
        }
    }

    // -- TLIN --
    for (size_t tlIndex = 0; tlIndex < view.m_timelines.size(); ++tlIndex) {
        const TimelineChunk& tl = view.m_timelines[tlIndex];
        // T-257b: TLIN i is SCEN i's timeline (Document::sceneTimeline()); a scene-local track
        // target is a node of that scene.
        const SceneChunk* tlScene = tlIndex < view.m_scenes.size() ? &view.m_scenes[tlIndex] : nullptr;
        if (!validStringId(tl.name(), stringCount)) {
            return Status::kInvalidIr;
        }
        for (const IrMarker& marker : tl.markers()) {
            if (!validStringId(marker.name, stringCount)) {
                return Status::kInvalidIr;
            }
        }
        for (const IrKeyframe& kf : tl.keyframes()) {
            if (!validateEasing(kf.easing, style.easingPoints())) {  // T-335: STYL's pool.
                return Status::kInvalidIr;
            }
        }
        for (const IrTrack& track : tl.tracks()) {
            if (track.target == kInvalidNode) {
                return Status::kInvalidIr;
            }
            if (track.kfCount < 1 || !inPool(track.kfOffset, track.kfCount, tl.keyframes().size())) {
                return Status::kInvalidIr;
            }
            std::span<const IrKeyframe> kfs = tl.keyframes().subspan(track.kfOffset, track.kfCount);
            ValueType type = static_cast<ValueType>(track.type);
            // T-257b (fuzz_ir: a TranslateX keyframe in `em` reaching setProperty): every keyframe
            // value is one its property takes (propertyAcceptsValue(), setProperty()'s own table).
            // A kSelfNode track animates the element that plays it (an `@keyframes`, `@timeline` or
            // repeat member); any other target is a scene node, which is never an Element.
            const NodeKind targetKind = track.target == kSelfNode ? NodeKind::Element : NodeKind::Group;
            if (!propertyAcceptsValue(targetKind, track.prop, type, LengthUnit::Px)) {
                return Status::kInvalidIr;
            }
            // T-257b (review of the fuzz_ir fixes): the track's property applies to its target's
            // kind (propertyApplies(), what Animator::evaluate writes through). kSelfNode is the
            // playing Element; a scene-local target is that scene node. A target past the scene's
            // nodes is skipped at run time (Animator::evaluate's localCount check).
            if (track.target == kSelfNode) {
                if (!propertyApplies(NodeKind::Element, track.prop)) {
                    return Status::kInvalidIr;
                }
            } else if (tlScene != nullptr && track.target < tlScene->nodes().size() &&
                       !propertyApplies(static_cast<NodeKind>(tlScene->nodes()[track.target].kind),
                                        track.prop)) {
                return Status::kInvalidIr;
            }
            for (size_t k = 0; k < kfs.size(); ++k) {
                // T-255: a non-finite time would make the keyframe search read past the track.
                if (!std::isfinite(kfs[k].time) || (k > 0 && !(kfs[k].time > kfs[k - 1].time))) {
                    return Status::kInvalidIr;
                }
                // T-255: a Length keyframe's unit (a neutral marker, slot 0 all ones, has none).
                // T-257b: and one the property takes (an element translate: px or % only).
                if (type == ValueType::Length && irValueU32(kfs[k].value, 0) != 0xFFFFFFFFu &&
                    (!validLengthUnit(kfs[k].value[4]) ||
                     !propertyAcceptsValue(targetKind, track.prop, type,
                                           static_cast<LengthUnit>(kfs[k].value[4])))) {
                    return Status::kInvalidIr;
                }
                if (type == ValueType::Path || type == ValueType::FloatArray) {
                    uint32_t off = irValueU32(kfs[k].value, 0);
                    uint32_t cnt = irValueU32(kfs[k].value, 1);
                    if (!inPool(off, cnt, tl.floatPool().size())) {
                        return Status::kInvalidIr;
                    }
                    // T-190: a Path keyframe's optional own verbs (slots 2/3, count 0 = none).
                    // T-255: they must account for exactly the keyframe's points.
                    uint32_t verbOff = irValueU32(kfs[k].value, 2);
                    uint32_t verbCnt = irValueU32(kfs[k].value, 3);
                    if (type == ValueType::Path && verbCnt != 0 &&
                        (!inPool(verbOff, verbCnt, tl.floatPool().size()) ||
                         !floatVerbsMatchPoints(tl.floatPool().data() + verbOff, verbCnt, cnt))) {
                        return Status::kInvalidIr;
                    }
                } else if (type == ValueType::Stops) {
                    uint32_t off = irValueU32(kfs[k].value, 0);
                    uint32_t cnt = irValueU32(kfs[k].value, 1);
                    if (!inPool(off, cnt, tl.stopPool().size())) {
                        return Status::kInvalidIr;
                    }
                }
            }
        }
    }

    // -- DOCT --
    const DocChunk& doc = view.m_doc;
    for (uint32_t classId : doc.classPool()) {
        if (!validStringId(classId, stringCount)) {
            return Status::kInvalidIr;
        }
    }
    const uint64_t assetCount = view.m_assets.assets().size();
    for (const IrAttr& attr : doc.attrPool()) {
        if (!validStringId(attr.name, stringCount) || !validStringId(attr.value, stringCount)) {
            return Status::kInvalidIr;
        }
        // T-255: an `img`'s `data-xt-asset` (parsed like document_load.cpp's parseAssetIdOr) names
        // an ASST entry; a value that is no number, or kInvalidAsset, means "no asset".
        if (view.m_strings.get(attr.name) == "data-xt-asset") {
            const std::string_view text = view.m_strings.get(attr.value);
            uint32_t id = 0;
            if (!text.empty() &&
                std::from_chars(text.data(), text.data() + text.size(), id).ec == std::errc{} &&
                id != kInvalidAsset && id >= assetCount) {
                return Status::kInvalidIr;
            }
        }
    }
    for (const IrDecl& decl : doc.inlineDecls()) {
        if (!validateDeclStringId(decl, stringCount) || !validateDeclListRef(decl, style) ||
            !validateDeclVarRef(decl, stringCount, varFallbackCount) || !validateDeclCalc(decl, calcCount) ||
            !validateDeclLength(decl) || !validateDeclFontSize(decl) || !validateDeclCascadeLength(decl) ||
            !validateDeclClipPath(decl, view.m_clipShapes) || !validateDeclGrid(decl, view.m_grid)) {
            return Status::kInvalidIr;
        }
    }
    std::span<const IrDocElement> elements = doc.elements();
    // T-255: each element's depth (root 0); layout and hit testing recurse once per level.
    std::vector<uint32_t> depths(elements.size(), 0);
    for (size_t idx = 0; idx < elements.size(); ++idx) {
        const IrDocElement& el = elements[idx];
        if (idx == 0) {
            if (el.parent != 0xFFFFFFFFu) {
                return Status::kInvalidIr;
            }
        } else if (el.parent >= idx) {
            return Status::kInvalidIr;
        } else {
            depths[idx] = depths[el.parent] + 1;
            if (depths[idx] >= kMaxElementDepth) {
                return Status::kInvalidIr;
            }
        }
        if (!validStringId(el.tag, stringCount) || !validStringId(el.id, stringCount)) {
            return Status::kInvalidIr;
        }
        if (!inPool(el.classOffset, el.classCount, doc.classPool().size())) {
            return Status::kInvalidIr;
        }
        if (!inPool(el.attrOffset, el.attrCount, doc.attrPool().size())) {
            return Status::kInvalidIr;
        }
        if (!inPool(el.inlineDeclOffset, el.inlineDeclCount, doc.inlineDecls().size())) {
            return Status::kInvalidIr;
        }
        if (el.scene != 0xFFFFFFFFu && el.scene >= view.sceneCount()) {
            return Status::kInvalidIr;
        }
        // T-257b (fuzz_ir: an `<anim>` autoplay naming a missing timeline): an autoplaying `anim`
        // element plays its scene's timeline -- Document::sceneTimeline(), TLIN index == scene
        // index -- at load (document_load.cpp step 4), so that timeline must exist. One with
        // `autoplay="false"` may lack it (the `sync` / timeline-group paths check at run time).
        if (el.scene != 0xFFFFFFFFu && el.scene >= view.timelineCount() &&
            view.m_strings.get(el.tag) == "anim" && animAutoplays(view, el)) {
            return Status::kInvalidIr;
        }
        if (el.firstTextChild != 0xFFFFFFFFu && el.firstTextChild >= doc.texts().size()) {
            return Status::kInvalidIr;
        }
        if (el.cameraIndex != 0xFFFFFFFFu && el.cameraIndex >= doc.cameras().size()) {
            return Status::kInvalidIr;
        }
    }
    for (const IrDocText& text : doc.texts()) {
        if (text.parent >= elements.size()) {
            return Status::kInvalidIr;
        }
        if (!validStringId(text.text, stringCount)) {
            return Status::kInvalidIr;
        }
    }
    for (const IrCameraData& cam : doc.cameras()) {
        if (!validateCamera(cam)) {
            return Status::kInvalidIr;
        }
    }

    // -- SRCM --
    if (view.m_sourceMap.present()) {
        for (uint32_t fileName : view.m_sourceMap.fileNames()) {
            if (!validStringId(fileName, stringCount)) {
                return Status::kInvalidIr;
            }
        }
        uint64_t expectedEntries =
            static_cast<uint64_t>(elements.size()) + static_cast<uint64_t>(doc.texts().size());
        if (static_cast<uint64_t>(view.m_sourceMap.entries().size()) != expectedEntries) {
            return Status::kInvalidIr;
        }
        for (const IrSourceEntry& entry : view.m_sourceMap.entries()) {
            if (entry.fileId >= view.m_sourceMap.fileNames().size()) {
                return Status::kInvalidIr;
            }
        }
    }

    // -- SYMB --
    for (const IrSymbolInstance& inst : view.m_symbols.instances()) {
        if (inst.symbol >= stringCount || inst.instanceElement >= elements.size()) {
            return Status::kInvalidIr;
        }
        for (const IrSymbolPath& p : inst.paths) {
            if (p.path >= stringCount || p.element >= elements.size()) {
                return Status::kInvalidIr;
            }
        }
    }

    // -- META --
    if (view.m_meta.present()) {
        const IrMeta& meta = *view.m_meta.meta();
        if (!validStringId(meta.sourceFile, stringCount) ||
            !validStringId(meta.compilerVersion, stringCount)) {
            return Status::kInvalidIr;
        }
    }

    // -- STAT (T-174) --
    const CustomStateChunk& states = view.m_customStates;
    if (states.names().size() > kMaxCustomStates) {
        return Status::kInvalidIr;
    }
    const uint64_t stateMaskAll =
        states.names().size() >= 32 ? 0xFFFFFFFFull : ((1ull << states.names().size()) - 1ull);
    for (uint32_t name : states.names()) {
        if (name >= stringCount) {
            return Status::kInvalidIr;
        }
    }
    for (const IrStateGroup& g : states.groups()) {
        if (g.name >= stringCount || (g.mask & ~stateMaskAll) != 0) {
            return Status::kInvalidIr;
        }
    }

    // -- PORT (T-175) --
    const PortChunk& portChunk = view.m_ports;
    const uint64_t portCount = portChunk.ports().size();
    for (const IrPort& port : portChunk.ports()) {
        if (port.name >= stringCount || port.type == 0 || port.type > kPortTypeLast ||
            port.dir > static_cast<uint8_t>(PortDir::Call) || port.element >= elements.size() ||
            (port.scope != kNoPortScope && port.scope >= elements.size()) ||
            !inPool(port.fieldOffset, port.fieldCount, portChunk.fields().size())) {
            return Status::kInvalidIr;
        }
        // T-381: only a List port has an entry type; a primitive one has no fields except an
        // Enum entry's value names.
        if (port.entryType != 0) {
            const PortValueType e = static_cast<PortValueType>(port.entryType);
            const bool primitive = e == PortValueType::Number || e == PortValueType::String ||
                                   e == PortValueType::Bool || e == PortValueType::Color ||
                                   e == PortValueType::Vec2 || e == PortValueType::Vec3 ||
                                   e == PortValueType::Enum;
            if (static_cast<PortValueType>(port.type) != PortValueType::List || !primitive ||
                (e != PortValueType::Enum && port.fieldCount != 0)) {
                return Status::kInvalidIr;
            }
        }
    }
    for (const IrPortField& field : portChunk.fields()) {
        if (field.name >= stringCount || field.type > kPortTypeLast) {
            return Status::kInvalidIr;
        }
    }
    // T-391: every List field has exactly one entry description; its range lies after the field
    // (finite nesting); a primitive entry has no fields except an Enum entry's value names.
    for (size_t k = 0; k < portChunk.nested().size(); ++k) {
        const IrPortNested& n = portChunk.nested()[k];
        if (n.field >= portChunk.fields().size() ||
            static_cast<PortValueType>(portChunk.fields()[n.field].type) != PortValueType::List ||
            !inPool(n.fieldOffset, n.fieldCount, portChunk.fields().size()) ||
            (n.fieldCount != 0 && n.fieldOffset <= n.field)) {
            return Status::kInvalidIr;
        }
        if (n.entryType != 0) {
            const PortValueType e = static_cast<PortValueType>(n.entryType);
            const bool primitive = e == PortValueType::Number || e == PortValueType::String ||
                                   e == PortValueType::Bool || e == PortValueType::Color ||
                                   e == PortValueType::Vec2 || e == PortValueType::Vec3 ||
                                   e == PortValueType::Enum;
            if (!primitive || (e != PortValueType::Enum && n.fieldCount != 0)) {
                return Status::kInvalidIr;
            }
        }
        for (size_t j = 0; j < k; ++j) {
            if (portChunk.nested()[j].field == n.field) {
                return Status::kInvalidIr;
            }
        }
    }
    for (uint32_t f = 0; f < portChunk.fields().size(); ++f) {
        if (static_cast<PortValueType>(portChunk.fields()[f].type) == PortValueType::List &&
            portChunk.nestedOf(f) == nullptr) {
            return Status::kInvalidIr;
        }
    }
    // T-358: a default names a declared port, carries that port's type, and only a type that may
    // have one; String / Enum values are StringIds (an Enum value must be one of its names).
    for (const IrPortDefault& d : portChunk.defaults()) {
        if (d.port >= portCount || d.type != portChunk.ports()[d.port].type) {
            return Status::kInvalidIr;
        }
        const IrPort& port = portChunk.ports()[d.port];
        uint32_t id = 0;
        std::memcpy(&id, d.value, sizeof(id));
        switch (static_cast<PortValueType>(d.type)) {
            case PortValueType::Number:
            case PortValueType::Bool:
            case PortValueType::Color:
            case PortValueType::Vec2:
            case PortValueType::Vec3:
                break;
            case PortValueType::String:
            case PortValueType::List:    // T-383: the JSON array text (parsed at load).
            case PortValueType::Struct:  // T-385: the JSON object text of a `<use>` literal.
                if (id >= stringCount) {
                    return Status::kInvalidIr;
                }
                break;
            case PortValueType::Enum: {
                bool found = false;
                for (uint32_t f = 0; f < port.fieldCount; ++f) {
                    found = found || portChunk.fields()[port.fieldOffset + f].name == id;
                }
                if (!found) {
                    return Status::kInvalidIr;
                }
                break;
            }
            default:
                return Status::kInvalidIr;
        }
    }
    // Converter steps (TRIG conditions and BIND chains): op, and String/Port argument references.
    auto validSteps = [&](std::span<const IrConverterStep> steps) {
        for (const IrConverterStep& step : steps) {
            if (step.op > kConverterOpLast && step.op != static_cast<uint8_t>(ConverterOp::Custom)) {
                return false;
            }
            for (uint32_t i = 0; i < 4; ++i) {
                uint32_t bits = 0;
                std::memcpy(&bits, &step.args[i], sizeof(bits));
                switch (converterArgKind(step, i)) {
                    case ConverterArg::Number:
                    case ConverterArg::Percent:
                    case ConverterArg::None:
                        break;
                    case ConverterArg::String:
                        if (bits >= stringCount) {
                            return false;
                        }
                        break;
                    case ConverterArg::Port:
                        if (bits >= portCount) {
                            return false;
                        }
                        break;
                    case ConverterArg::Item:       // T-177: a field name.
                    case ConverterArg::OuterItem:  // T-391: likewise.
                        if (bits >= stringCount) {
                            return false;
                        }
                        break;
                    case ConverterArg::Index:
                    case ConverterArg::Count:
                    case ConverterArg::OuterIndex:  // T-391
                        break;
                    default:
                        return false;
                }
            }
        }
        return true;
    };

    // -- TRIG (T-174) --
    const TriggerChunk& trig = view.m_triggers;
    if (!validSteps(trig.conditions())) {
        return Status::kInvalidIr;
    }
    for (const IrTriggerTable& t : trig.tables()) {
        if (t.element >= elements.size() ||
            !inPool(t.triggerOffset, t.triggerCount, trig.triggers().size())) {
            return Status::kInvalidIr;
        }
    }
    for (const IrTrigger& t : trig.triggers()) {
        if (!inPool(t.actionOffset, t.actionCount, trig.actions().size()) || t.headCount > t.actionCount) {
            return Status::kInvalidIr;
        }
        const TriggerEvent ev = static_cast<TriggerEvent>(t.event);
        const bool stringArg = ev == TriggerEvent::Port || ev == TriggerEvent::TimelineEnd ||
                               ev == TriggerEvent::TimelineMarker || ev == TriggerEvent::MediaEnd;
        const bool stateArg = ev == TriggerEvent::StateEnter || ev == TriggerEvent::StateExit;
        // T-175: a Port event's arg1 is a PORT index + 1 (0 / kInvalidString = none).
        // T-194: Gesture / Player / NavBlocked carry small codes, not StringIds.
        const bool codeArgs =
            ev == TriggerEvent::Gesture || ev == TriggerEvent::Player || ev == TriggerEvent::NavBlocked;
        if ((ev == TriggerEvent::Player && t.arg0 >= kMaxPlayers) || t.button > 3) {
            return Status::kInvalidIr;  // T-390: button = PointerButton + 1, 0 = any.
        }
        const bool portArg1Ok = ev == TriggerEvent::Port
                                    ? (t.arg1 == 0 || t.arg1 == kInvalidString || t.arg1 <= portCount)
                                    : (codeArgs || validStringId(t.arg1, stringCount));
        if ((stringArg && t.arg0 >= stringCount) || (stateArg && t.arg0 >= states.names().size()) ||
            !portArg1Ok) {
            return Status::kInvalidIr;
        }
        for (uint32_t a = 0; a < t.actionCount; ++a) {
            const IrTriggerAction& act = trig.actions()[t.actionOffset + a];
            if (act.next > t.actionCount || act.alt > t.actionCount) {
                return Status::kInvalidIr;
            }
            switch (static_cast<TriggerAction>(act.kind)) {
                case TriggerAction::Play:
                case TriggerAction::Seek:
                    // T-255: a Seek time (arg2) is finite.
                    if (act.arg0 >= stringCount || !validStringId(act.arg1, stringCount) ||
                        (static_cast<TriggerAction>(act.kind) == TriggerAction::Seek &&
                         !std::isfinite(act.arg2))) {
                        return Status::kInvalidIr;
                    }
                    break;
                case TriggerAction::Emit:
                    // T-175: arg1 = PORT index + 1, 0 = an undeclared port.
                    if (act.arg0 >= stringCount || act.arg1 > portCount) {
                        return Status::kInvalidIr;
                    }
                    break;
                case TriggerAction::Focus:  // T-392: kInvalidString = the trigger's element.
                    if (!validStringId(act.arg0, stringCount)) {
                        return Status::kInvalidIr;
                    }
                    break;
                case TriggerAction::Stop:
                case TriggerAction::Class:
                case TriggerAction::Sound:
                case TriggerAction::Haptic:
                    if (act.arg0 >= stringCount) {
                        return Status::kInvalidIr;
                    }
                    break;
                case TriggerAction::State:
                    // T-390: a built-in Disabled action carries no custom state bit; T-392: another
                    // built-in one carries its ElementState bit.
                    if (((act.flags & kStateBuiltin) == 0 && act.arg0 >= states.names().size()) ||
                        ((act.flags & kStateBuiltin) != 0 && act.arg0 != 0 && act.arg0 != kStateHover &&
                         act.arg0 != kStateActive && act.arg0 != kStateFocus && act.arg0 != kStateDisabled &&
                         act.arg0 != kStateChecked) ||
                        ((act.flags & kStateTarget) != 0 && act.arg1 >= stringCount)) {
                        return Status::kInvalidIr;
                    }
                    break;
                case TriggerAction::If:
                    // T-390: kIfTerms (a term list) or kIfActive, never both.
                    if ((act.flags != 0 && act.flags != kIfActive && act.flags != kIfTerms) ||
                        !inPool(act.arg0, act.arg1, trig.conditions().size())) {
                        return Status::kInvalidIr;
                    }
                    break;
                case TriggerAction::Spawn:
                    // T-201: the `at` id (Element) and the timeline name are StringIds.
                    if (act.flags > static_cast<uint8_t>(SpawnAt::Element) ||
                        (act.flags == static_cast<uint8_t>(SpawnAt::Element) && act.arg0 >= stringCount) ||
                        !validStringId(act.arg1, stringCount) || !(act.arg2 >= 0.0f)) {
                        return Status::kInvalidIr;
                    }
                    break;
                case TriggerAction::Set: {
                    // T-201: arg1 = PORT index + 1 (always declared); a String value is a StringId.
                    uint32_t bits = 0;
                    std::memcpy(&bits, &act.arg2, sizeof(bits));
                    // T-386: a pointer source's element id is a StringId too.
                    const bool idBits = act.flags == static_cast<uint8_t>(SetSource::String) ||
                                        (act.flags >= static_cast<uint8_t>(SetSource::PointerFractionX) &&
                                         act.flags <= static_cast<uint8_t>(SetSource::PointerLocalY));
                    if (act.arg1 == 0 || act.arg1 > portCount || act.flags > kSetSourceLast ||
                        (idBits && bits >= stringCount) ||
                        (act.extra != 0 && !inPool(act.arg0, act.extra, trig.conditions().size()))) {
                        return Status::kInvalidIr;
                    }
                    break;
                }
                default:
                    break;  // Show/Hide take no argument; an unknown kind is skipped at run time.
            }
        }
    }

    // -- STRS (T-179) --
    const LocalizationChunk& l10n = view.m_localization;
    if (l10n.present()) {
        for (uint32_t key : l10n.keys()) {
            if (key >= stringCount) {
                return Status::kInvalidIr;
            }
        }
        const uint64_t keyCount = l10n.keys().size();
        if ((!l10n.textKeys().empty() && l10n.textKeys().size() != doc.texts().size()) ||
            (!l10n.attrKeys().empty() && l10n.attrKeys().size() != doc.attrPool().size())) {
            return Status::kInvalidIr;
        }
        for (uint32_t k : l10n.textKeys()) {
            if (k > keyCount) {
                return Status::kInvalidIr;
            }
        }
        for (uint32_t k : l10n.attrKeys()) {
            if (k > keyCount) {
                return Status::kInvalidIr;
            }
        }
        for (const IrFontFallback& fb : l10n.fallbacks()) {
            if (fb.lang >= stringCount ||
                !inPool(fb.familyOffset, fb.familyCount, l10n.fallbackFamilies().size())) {
                return Status::kInvalidIr;
            }
        }
        for (uint32_t family : l10n.fallbackFamilies()) {
            if (family >= stringCount) {
                return Status::kInvalidIr;
            }
        }
    }

    // -- BIND (T-175) --
    const BindingChunk& binds = view.m_bindings;
    for (const IrBinding& b : binds.bindings()) {
        const BindTarget target = static_cast<BindTarget>(b.target);
        // T-243: a scene-node binding names a node of the scene its element owns.
        if (b.sceneNode != kBindElementSelf &&
            (b.element >= elements.size() || elements[b.element].scene >= view.m_scenes.size() ||
             b.sceneNode >= view.m_scenes[elements[b.element].scene].nodes().size())) {
            return Status::kInvalidIr;
        }
        if (b.element >= elements.size() || b.target == 0 ||
            b.target > static_cast<uint8_t>(BindTarget::BuiltinState) ||
            !inPool(b.sourceOffset, b.sourceCount, binds.sources().size()) ||
            !inPool(b.stepOffset, b.stepCount, binds.steps().size()) ||
            !validStringId(b.targetName, stringCount) ||
            (target == BindTarget::State && b.targetId >= states.names().size()) ||
            ((target == BindTarget::Prop || target == BindTarget::Class || target == BindTarget::Variant) &&
             b.targetName == kInvalidString) ||
            (target == BindTarget::WorldAnchor && (b.sourceCount != 1 || b.stepCount != 0))) {
            return Status::kInvalidIr;
        }
        // T-380: a Port target names an `in`, non-event port and sits on the element itself.
        if (target == BindTarget::Port &&
            (b.targetId >= portCount || b.sceneNode != kBindElementSelf ||
             static_cast<PortDir>(portChunk.ports()[b.targetId].dir) != PortDir::In ||
             static_cast<PortValueType>(portChunk.ports()[b.targetId].type) == PortValueType::Event)) {
            return Status::kInvalidIr;
        }
    }
    for (uint32_t source : binds.sources()) {
        if (source >= portCount) {
            return Status::kInvalidIr;
        }
    }
    if (!validSteps(binds.steps())) {
        return Status::kInvalidIr;
    }

    // -- RPT (T-177) --
    const RepeatChunk& rpt = view.m_repeats;
    // T-391: the row whose template contains row `i`'s `<repeat>` element (the innermost), or -1.
    auto enclosingRow = [&](size_t i) -> int64_t {
        uint32_t a = rpt.repeats()[i].element;
        for (size_t steps = 0; steps <= elements.size() && a < elements.size(); ++steps) {
            for (size_t k = 0; k < rpt.repeats().size(); ++k) {
                if (k != i && rpt.repeats()[k].templateRoot == a) {
                    return static_cast<int64_t>(k);
                }
            }
            a = elements[a].parent;
        }
        return -1;
    };
    // T-391: row `i`'s entry fields (range into fields[]) and entry type, through the enclosing
    // rows of a kRepeatItemField row; false when it does not resolve to a List.
    auto rowEntry = [&](size_t i, uint32_t& offset, uint32_t& count, uint8_t& entryType) {
        std::vector<size_t> chain;
        for (int64_t k = static_cast<int64_t>(i); k >= 0 && chain.size() <= rpt.repeats().size();) {
            chain.push_back(static_cast<size_t>(k));
            if ((rpt.repeats()[static_cast<size_t>(k)].flags & kRepeatItemField) == 0) {
                break;
            }
            k = enclosingRow(static_cast<size_t>(k));
            if (k < 0) {
                return false;
            }
        }
        if (chain.empty() || chain.size() > rpt.repeats().size()) {
            return false;
        }
        const IrRepeat& top = rpt.repeats()[chain.back()];
        if ((top.flags & kRepeatItemField) != 0 || top.listPort >= portCount ||
            static_cast<PortValueType>(portChunk.ports()[top.listPort].type) != PortValueType::List) {
            return false;
        }
        offset = portChunk.ports()[top.listPort].fieldOffset;
        count = portChunk.ports()[top.listPort].fieldCount;
        entryType = portChunk.ports()[top.listPort].entryType;
        for (size_t c = chain.size() - 1; c-- > 0;) {
            const uint32_t field = rpt.repeats()[chain[c]].listPort;
            if (entryType != 0 || field >= count) {
                return false;
            }
            const IrPortNested* n = portChunk.nestedOf(offset + field);
            if (n == nullptr) {
                return false;
            }
            offset = n->fieldOffset;
            count = n->fieldCount;
            entryType = n->entryType;
        }
        return true;
    };
    for (size_t i = 0; i < rpt.repeats().size(); ++i) {
        const IrRepeat& r = rpt.repeats()[i];
        // T-201: a spawn pool has no list port.
        const bool spawnPool = (r.flags & kRepeatSpawn) != 0 && r.listPort == kRepeatNoListPort;
        // T-391: a nested repeat over a list field of the enclosing repeat's entry.
        const bool itemField = (r.flags & kRepeatItemField) != 0;
        if (itemField) {
            uint32_t offset = 0;
            uint32_t count = 0;
            uint8_t entryType = 0;
            if (spawnPool || !rowEntry(i, offset, count, entryType)) {
                return Status::kInvalidIr;
            }
        }
        const bool portRow = !spawnPool && !itemField;
        if (r.element >= elements.size() || r.templateRoot >= elements.size() ||
            elements[r.templateRoot].parent != r.element || (portRow && r.listPort >= portCount) ||
            (portRow &&
             static_cast<PortValueType>(portChunk.ports()[r.listPort].type) != PortValueType::List) ||
            r.enterTriggerActions > trig.triggers().size() || r.exitTriggerActions > trig.triggers().size() ||
            (r.viewportElement != kRepeatNoViewport && r.viewportElement >= elements.size())) {
            return Status::kInvalidIr;
        }
        for (size_t k = 0; k < i; ++k) {
            if (rpt.repeats()[k].element == r.element) {
                return Status::kInvalidIr;
            }
        }
    }

    // -- SHDR (T-203) --
    const ShaderChunk& shdr = view.m_shaders;
    if (shdr.present()) {
        const size_t textSize = shdr.text().size();
        const std::span<const IrAsset> assetTable = view.m_assets.assets();
        for (size_t i = 0; i < shdr.shaders().size(); ++i) {
            const IrShaderRef& sr = shdr.shaders()[i];
            if (sr.asset >= assetTable.size() || assetTable[sr.asset].kind != 2 ||
                !inPool(sr.sourceOffset, sr.sourceLength, textSize) ||
                !inPool(sr.glslOffset, sr.glslLength, textSize) ||
                !inPool(sr.paramOffset, sr.paramCount, shdr.params().size()) || !(sr.expandPx >= 0.0f) ||
                !validEffectKind(sr.fallback.kind) ||
                sr.fallback.kind == static_cast<uint8_t>(EffectKind::Custom) || sr.fallback.stopCount != 0) {
                return Status::kInvalidIr;
            }
            for (size_t k = 0; k < i; ++k) {
                if (shdr.shaders()[k].asset == sr.asset) {
                    return Status::kInvalidIr;
                }
            }
        }
        for (const IrShaderParam& p : shdr.params()) {
            if (p.name >= stringCount || p.type > 2) {
                return Status::kInvalidIr;
            }
        }
    }

    // -- TGRP (T-172) --
    const TimelineGroupChunk& groups = view.m_timelineGroups;
    if (groups.present()) {
        for (const IrTimelineGroup& g : groups.groups()) {
            if (g.name >= stringCount || g.scopeElement >= elements.size() || !(g.duration >= 0.0f) ||
                !inPool(g.markerOffset, g.markerCount, groups.markers().size()) ||
                !inPool(g.soundOffset, g.soundCount, groups.sounds().size()) ||
                !inPool(g.memberOffset, g.memberCount, groups.members().size())) {
                return Status::kInvalidIr;
            }
        }
        for (const IrTimelineGroupMarker& m : groups.markers()) {
            if (m.name >= stringCount || !(m.time >= 0.0f)) {
                return Status::kInvalidIr;
            }
        }
        for (const IrTimelineGroupSound& snd : groups.sounds()) {
            if (snd.cue >= stringCount || !(snd.time >= 0.0f)) {
                return Status::kInvalidIr;
            }
        }
        for (const IrTimelineGroupMember& mem : groups.members()) {
            if (mem.element >= elements.size() || mem.timeline >= view.timelineCount() ||
                !(mem.delay >= 0.0f) || !(mem.duration >= 0.0f)) {
                return Status::kInvalidIr;
            }
        }
    }

    return Status::kOk;
}

}  // namespace xt
