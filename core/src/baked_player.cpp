#include <cstring>
#include <utility>

#include "xt/baked.h"

/// BakedPlayer: `.xtcl` header/frame-index validation and per-frame decode. See
/// docs/design/09-command-list.md §9.6, docs/design/12-compiler.md §12.5, docs/tasks/T-37.md
/// "`.xtcl`".

namespace xt {

namespace {

/// Bounds-checked cursor over one frame's payload bytes. Any read past `end` sets `ok = false`
/// and leaves the destination untouched; every subsequent read then fails too (sticky), so a
/// truncated/malformed payload simply stops decoding early rather than reading out of bounds --
/// the most conservative choice for data this task's report lists as not independently validated
/// by load() (only the outer frame boundaries are).
struct Reader {
    const uint8_t* data;
    size_t pos;
    size_t end;
    bool ok = true;

    bool readBytes(void* dst, size_t n) {
        if (!ok || pos + n > end) {
            ok = false;
            return false;
        }
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }
    bool readU32(uint32_t& v) { return readBytes(&v, sizeof(v)); }
};

template <class T>
void readPackedVector(Reader& r, std::vector<T>& out) {
    using PackedT = decltype(toPacked(std::declval<const T&>()));
    out.clear();
    uint32_t count = 0;
    if (!r.readU32(count)) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PackedT packed{};
        if (!r.readBytes(&packed, sizeof(PackedT))) {
            break;
        }
        out.push_back(fromPacked(packed));
    }
}

/// `T` here is already a fixed-size, padding-free scalar (uint8_t/float/uint32_t) -- read as a
/// raw block rather than element by element.
template <class T>
void readRawVector(Reader& r, std::vector<T>& out) {
    out.clear();
    uint32_t count = 0;
    if (!r.readU32(count)) {
        return;
    }
    size_t need = static_cast<size_t>(count) * sizeof(T);
    if (!r.ok || r.pos + need > r.end) {
        r.ok = false;
        return;
    }
    out.resize(count);
    r.readBytes(out.data(), need);
}

/// `minor == 3` files (written before docs/tasks/T-116.md) store PaintRef without its `p2`
/// (radial second axis); this is that older on-disk layout.
#pragma pack(push, 1)
struct PackedPaintRefV3 {
    uint8_t kind;
    float r;
    float g;
    float b;
    float a;
    float p0x;
    float p0y;
    float p1x;
    float p1y;
    uint32_t stopOffset;
    uint32_t stopCount;
};
static_assert(sizeof(PackedPaintRefV3) == 41);

struct PackedFillCmdV3 {
    float xf[6];
    PackedPathRef path;
    PackedPaintRefV3 paint;
    uint8_t rule;
    uint8_t antialias;
};

struct PackedStrokeCmdV3 {
    float xf[6];
    PackedPathRef path;
    PackedPaintRefV3 paint;
    float width;
    uint8_t cap;
    uint8_t join;
    float miterLimit;
    uint32_t dashOffset;
    uint32_t dashCount;
    float dashPhase;
};
#pragma pack(pop)

/// Converts a `minor == 3` packed paint to a PaintRef, filling `p2` with `p0` (T-116's
/// pre-existing-circle rule for a paint that never had a second axis).
PaintRef fromPackedV3(const PackedPaintRefV3& v) {
    PaintRef p{};
    p.kind = static_cast<PaintKind>(v.kind);
    p.color = Color{v.r, v.g, v.b, v.a};
    p.p0 = Vec2{v.p0x, v.p0y};
    p.p1 = Vec2{v.p1x, v.p1y};
    p.p2 = p.p0;
    p.stopOffset = v.stopOffset;
    p.stopCount = v.stopCount;
    return p;
}

void readLegacyFills(Reader& r, std::vector<FillCmd>& out) {
    out.clear();
    uint32_t count = 0;
    if (!r.readU32(count)) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PackedFillCmdV3 packed{};
        if (!r.readBytes(&packed, sizeof(packed))) {
            break;
        }
        FillCmd fc{};
        std::memcpy(&fc.xf, packed.xf, sizeof(packed.xf));
        fc.path = fromPacked(packed.path);
        fc.paint = fromPackedV3(packed.paint);
        fc.rule = static_cast<FillRule>(packed.rule);
        fc.antialias = packed.antialias;
        out.push_back(fc);
    }
}

void readLegacyStrokes(Reader& r, std::vector<StrokeCmd>& out) {
    out.clear();
    uint32_t count = 0;
    if (!r.readU32(count)) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PackedStrokeCmdV3 packed{};
        if (!r.readBytes(&packed, sizeof(packed))) {
            break;
        }
        StrokeCmd sc{};
        std::memcpy(&sc.xf, packed.xf, sizeof(packed.xf));
        sc.path = fromPacked(packed.path);
        sc.paint = fromPackedV3(packed.paint);
        sc.width = packed.width;
        sc.cap = static_cast<LineCap>(packed.cap);
        sc.join = static_cast<LineJoin>(packed.join);
        sc.miterLimit = packed.miterLimit;
        sc.dashOffset = packed.dashOffset;
        sc.dashCount = packed.dashCount;
        sc.dashPhase = packed.dashPhase;
        out.push_back(sc);
    }
}

/// `minor == 2` files (written before docs/tasks/T-105.md) store ProjectionCmd without T-105's
/// `boundsHint`; this is that older on-disk layout.
#pragma pack(push, 1)
struct PackedProjectionCmdV2 {
    float viewProj[16];  ///< Row-major Mat4.
    float rootXf[6];     ///< Row-major Mat2x3 (a, b, c, d, e, f).
    float boxX;          ///< Root reference box x.
    float boxY;          ///< Root reference box y.
    float boxW;          ///< Root reference box width.
    float boxH;          ///< Root reference box height.
    uint8_t depthTest;   ///< 0.3-reserved depth-test flag.
};
#pragma pack(pop)
static_assert(sizeof(PackedProjectionCmdV2) == 105);

/// Reads the `minor == 2` projection layout, filling each `boundsHint` with `viewport` -- the
/// bound computeDamage() used for every PushProjection before T-105.
void readLegacyProjections(Reader& r, std::vector<ProjectionCmd>& out, Rect viewport) {
    out.clear();
    uint32_t count = 0;
    if (!r.readU32(count)) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PackedProjectionCmdV2 packed{};
        if (!r.readBytes(&packed, sizeof(packed))) {
            break;
        }
        PackedProjectionCmd wide{};
        std::memcpy(wide.viewProj, packed.viewProj, sizeof(packed.viewProj));
        std::memcpy(wide.rootXf, packed.rootXf, sizeof(packed.rootXf));
        wide.boxX = packed.boxX;
        wide.boxY = packed.boxY;
        wide.boxW = packed.boxW;
        wide.boxH = packed.boxH;
        wide.depthTest = packed.depthTest;
        wide.hintX = viewport.x;
        wide.hintY = viewport.y;
        wide.hintW = viewport.w;
        wide.hintH = viewport.h;
        out.push_back(fromPacked(wide));
    }
}

}  // namespace

Status BakedPlayer::load(IrBlob bytes, bool takeOwnership) {
    if (takeOwnership) {
        m_owned.assign(bytes.data, bytes.data + bytes.size);
        m_data = m_owned.data();
        m_size = m_owned.size();
    } else {
        m_owned.clear();
        m_data = bytes.data;
        m_size = bytes.size;
    }

    if (m_size < sizeof(BakedHeader)) {
        return Status::kInvalidIr;
    }
    BakedHeader header{};
    std::memcpy(&header, m_data, sizeof(header));
    if (header.magic[0] != 'X' || header.magic[1] != 'T' || header.magic[2] != 'C' ||
        header.magic[3] != 'L' || header.major != 1) {
        return Status::kInvalidIr;
    }

    // T-255: every frame record needs at least its 8-byte header, so a frameCount the file cannot
    // hold is rejected before it sizes the index vectors below.
    if (header.frameCount > (m_size - sizeof(BakedHeader)) / 8u) {
        return Status::kInvalidIr;
    }
    m_minor = header.minor;
    m_frameCount = header.frameCount;
    m_fps = header.fps;
    m_viewport = Rect{0, 0, header.width, header.height};
    m_dpr = header.dpr;
    m_frameOffsets.assign(m_frameCount, static_cast<size_t>(-1));
    m_framePayloadLengths.assign(m_frameCount, 0);

    size_t offset = sizeof(BakedHeader);
    while (offset < m_size) {
        if (offset + 8 > m_size) {
            return Status::kInvalidIr;  // Truncated frame header (frameIndex/payloadLength).
        }
        uint32_t frameIndex = 0;
        uint32_t payloadLength = 0;
        std::memcpy(&frameIndex, m_data + offset, 4);
        std::memcpy(&payloadLength, m_data + offset + 4, 4);
        offset += 8;
        if (offset + payloadLength > m_size) {
            return Status::kInvalidIr;  // Truncated payload.
        }
        if (frameIndex >= m_frameCount) {
            return Status::kInvalidIr;  // Frame index outside the header's declared frameCount.
        }
        m_frameOffsets[frameIndex] = offset;  // Last write for a given index wins.
        m_framePayloadLengths[frameIndex] = payloadLength;
        offset += payloadLength;
    }

    return Status::kOk;
}

const CommandList& BakedPlayer::frame(uint32_t index) {
    m_scratch.clear();
    m_scratch.viewport = m_viewport;
    m_scratch.devicePixelRatio = m_dpr;

    if (index >= m_frameCount || m_frameOffsets[index] == static_cast<size_t>(-1)) {
        return m_scratch;
    }

    Reader r{m_data, m_frameOffsets[index], m_frameOffsets[index] + m_framePayloadLengths[index], true};
    readPackedVector(r, m_scratch.commands);
    // `minor < 4` files (written before docs/tasks/T-116.md) store PaintRef without `p2`.
    if (m_minor >= 4) {
        readPackedVector(r, m_scratch.fills);
        readPackedVector(r, m_scratch.strokes);
    } else {
        readLegacyFills(r, m_scratch.fills);
        readLegacyStrokes(r, m_scratch.strokes);
    }
    readPackedVector(r, m_scratch.images);
    readPackedVector(r, m_scratch.glyphRuns);
    readPackedVector(r, m_scratch.clipRects);
    readPackedVector(r, m_scratch.clipPaths);
    readPackedVector(r, m_scratch.layers);
    readPackedVector(r, m_scratch.masks);
    readRawVector(r, m_scratch.verbs);
    readRawVector(r, m_scratch.points);
    readPackedVector(r, m_scratch.stops);
    readRawVector(r, m_scratch.glyphs);
    readPackedVector(r, m_scratch.glyphPos);
    readRawVector(r, m_scratch.dashes);
    // minor == 0 files (written before docs/tasks/T-49.md) end here; leave the three 3D vectors
    // empty (m_scratch.clear() above already did this) rather than reading past the payload.
    if (m_minor >= 1) {
        if (m_minor >= 3) {
            readPackedVector(r, m_scratch.projections);
        } else {
            readLegacyProjections(r, m_scratch.projections, m_viewport);
        }
        readPackedVector(r, m_scratch.transforms3);
        readPackedVector(r, m_scratch.imageQuads);
    }
    // minor == 1 files (written before docs/tasks/T-52.md) end here; leave the five effect
    // vectors empty (m_scratch.clear() above already did this).
    if (m_minor >= 2) {
        readPackedVector(r, m_scratch.effectCmds);
        readPackedVector(r, m_scratch.effects);
        readPackedVector(r, m_scratch.shaders);
        readPackedVector(r, m_scratch.shaderParams);
        readRawVector(r, m_scratch.shaderText);
    }
    // docs/tasks/T-263a.md E1: a corrupt or hand-made frame must not reach a backend that indexes
    // its pools; it reads as an empty frame, like an out-of-range index.
    if (!validateCommandList(m_scratch)) {
        m_scratch.clear();
    }
    m_scratch.structureChecked = true;  // Checked here (or empty): the backend need not repeat it.

    return m_scratch;
}

}  // namespace xt
