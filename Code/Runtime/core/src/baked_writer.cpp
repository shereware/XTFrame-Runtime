#include <cstring>
#include <string>
#include <utility>

#include "file_open_utf8.h"
#include "xt/baked.h"

/// BakedWriter and the CommandList element <-> packed POD conversion functions. See
/// Docs/design/09-command-list.md §9.6, Docs/design/12-compiler.md §12.5, Docs/tasks/T-37.md
/// "`.xtcl`".

namespace xt {

namespace {

void packXf(const Mat2x3& xf, float out[6]) {
    out[0] = xf.a;
    out[1] = xf.b;
    out[2] = xf.c;
    out[3] = xf.d;
    out[4] = xf.e;
    out[5] = xf.f;
}

Mat2x3 unpackXf(const float xf[6]) {
    Mat2x3 out{};
    out.a = xf[0];
    out.b = xf[1];
    out.c = xf[2];
    out.d = xf[3];
    out.e = xf[4];
    out.f = xf[5];
    return out;
}

}  // namespace

PackedPathRef toPacked(const PathRef& v) {
    PackedPathRef p{};
    p.verbOffset = v.verbOffset;
    p.verbCount = v.verbCount;
    p.pointOffset = v.pointOffset;
    p.pointCount = v.pointCount;
    return p;
}

PathRef fromPacked(const PackedPathRef& v) {
    PathRef p{};
    p.verbOffset = v.verbOffset;
    p.verbCount = v.verbCount;
    p.pointOffset = v.pointOffset;
    p.pointCount = v.pointCount;
    return p;
}

PackedPaintRef toPacked(const PaintRef& v) {
    PackedPaintRef p{};
    p.kind = static_cast<uint8_t>(v.kind);
    p.r = v.color.r;
    p.g = v.color.g;
    p.b = v.color.b;
    p.a = v.color.a;
    p.p0x = v.p0.x;
    p.p0y = v.p0.y;
    p.p1x = v.p1.x;
    p.p1y = v.p1.y;
    p.p2x = v.p2.x;
    p.p2y = v.p2.y;
    p.stopOffset = v.stopOffset;
    p.stopCount = v.stopCount;
    return p;
}

PaintRef fromPacked(const PackedPaintRef& v) {
    PaintRef p{};
    p.kind = static_cast<PaintKind>(v.kind);
    p.color = Color{v.r, v.g, v.b, v.a};
    p.p0 = Vec2{v.p0x, v.p0y};
    p.p1 = Vec2{v.p1x, v.p1y};
    p.p2 = Vec2{v.p2x, v.p2y};
    p.stopOffset = v.stopOffset;
    p.stopCount = v.stopCount;
    return p;
}

PackedCommand toPacked(const Command& v) {
    PackedCommand p{};
    p.kind = static_cast<uint8_t>(v.kind);
    p.index = v.index;
    return p;
}

Command fromPacked(const PackedCommand& v) {
    Command c{};
    c.kind = static_cast<CmdKind>(v.kind);
    c.index = v.index;
    return c;
}

PackedFillCmd toPacked(const FillCmd& v) {
    PackedFillCmd p{};
    packXf(v.xf, p.xf);
    p.path = toPacked(v.path);
    p.paint = toPacked(v.paint);
    p.rule = static_cast<uint8_t>(v.rule);
    p.antialias = v.antialias;
    return p;
}

FillCmd fromPacked(const PackedFillCmd& v) {
    FillCmd f{};
    f.xf = unpackXf(v.xf);
    f.path = fromPacked(v.path);
    f.paint = fromPacked(v.paint);
    f.rule = static_cast<FillRule>(v.rule);
    f.antialias = v.antialias;
    return f;
}

PackedStrokeCmd toPacked(const StrokeCmd& v) {
    PackedStrokeCmd p{};
    packXf(v.xf, p.xf);
    p.path = toPacked(v.path);
    p.paint = toPacked(v.paint);
    p.width = v.width;
    p.cap = static_cast<uint8_t>(v.cap);
    p.join = static_cast<uint8_t>(v.join);
    p.miterLimit = v.miterLimit;
    p.dashOffset = v.dashOffset;
    p.dashCount = v.dashCount;
    p.dashPhase = v.dashPhase;
    return p;
}

StrokeCmd fromPacked(const PackedStrokeCmd& v) {
    StrokeCmd s{};
    s.xf = unpackXf(v.xf);
    s.path = fromPacked(v.path);
    s.paint = fromPacked(v.paint);
    s.width = v.width;
    s.cap = static_cast<LineCap>(v.cap);
    s.join = static_cast<LineJoin>(v.join);
    s.miterLimit = v.miterLimit;
    s.dashOffset = v.dashOffset;
    s.dashCount = v.dashCount;
    s.dashPhase = v.dashPhase;
    return s;
}

PackedImageCmd toPacked(const ImageCmd& v) {
    PackedImageCmd p{};
    packXf(v.xf, p.xf);
    p.asset = v.asset;
    p.dstX = v.dst.x;
    p.dstY = v.dst.y;
    p.dstW = v.dst.w;
    p.dstH = v.dst.h;
    p.srcX = v.srcUv.x;
    p.srcY = v.srcUv.y;
    p.srcW = v.srcUv.w;
    p.srcH = v.srcUv.h;
    p.opacity = v.opacity;
    p.smooth = v.smooth;
    return p;
}

ImageCmd fromPacked(const PackedImageCmd& v) {
    ImageCmd i{};
    i.xf = unpackXf(v.xf);
    i.asset = v.asset;
    i.dst = Rect{v.dstX, v.dstY, v.dstW, v.dstH};
    i.srcUv = Rect{v.srcX, v.srcY, v.srcW, v.srcH};
    i.opacity = v.opacity;
    i.smooth = v.smooth;
    return i;
}

PackedGlyphsCmd toPacked(const GlyphsCmd& v) {
    PackedGlyphsCmd p{};
    packXf(v.xf, p.xf);
    p.font = v.font;
    p.size = v.size;
    p.r = v.color.r;
    p.g = v.color.g;
    p.b = v.color.b;
    p.a = v.color.a;
    p.glyphOffset = v.glyphOffset;
    p.glyphCount = v.glyphCount;
    return p;
}

GlyphsCmd fromPacked(const PackedGlyphsCmd& v) {
    GlyphsCmd g{};
    g.xf = unpackXf(v.xf);
    g.font = v.font;
    g.size = v.size;
    g.color = Color{v.r, v.g, v.b, v.a};
    g.glyphOffset = v.glyphOffset;
    g.glyphCount = v.glyphCount;
    return g;
}

PackedClipRectCmd toPacked(const ClipRectCmd& v) {
    PackedClipRectCmd p{};
    packXf(v.xf, p.xf);
    p.rectX = v.rect.x;
    p.rectY = v.rect.y;
    p.rectW = v.rect.w;
    p.rectH = v.rect.h;
    return p;
}

ClipRectCmd fromPacked(const PackedClipRectCmd& v) {
    ClipRectCmd c{};
    c.xf = unpackXf(v.xf);
    c.rect = Rect{v.rectX, v.rectY, v.rectW, v.rectH};
    return c;
}

PackedClipPathCmd toPacked(const ClipPathCmd& v) {
    PackedClipPathCmd p{};
    packXf(v.xf, p.xf);
    p.path = toPacked(v.path);
    p.rule = static_cast<uint8_t>(v.rule);
    return p;
}

ClipPathCmd fromPacked(const PackedClipPathCmd& v) {
    ClipPathCmd c{};
    c.xf = unpackXf(v.xf);
    c.path = fromPacked(v.path);
    c.rule = static_cast<FillRule>(v.rule);
    return c;
}

PackedLayerCmd toPacked(const LayerCmd& v) {
    PackedLayerCmd p{};
    p.opacity = v.opacity;
    p.blend = static_cast<uint8_t>(v.blend);
    p.boundsX = v.boundsHint.x;
    p.boundsY = v.boundsHint.y;
    p.boundsW = v.boundsHint.w;
    p.boundsH = v.boundsHint.h;
    return p;
}

LayerCmd fromPacked(const PackedLayerCmd& v) {
    LayerCmd l{};
    l.opacity = v.opacity;
    l.blend = static_cast<BlendMode>(v.blend);
    l.boundsHint = Rect{v.boundsX, v.boundsY, v.boundsW, v.boundsH};
    return l;
}

PackedMaskCmd toPacked(const MaskCmd& v) {
    PackedMaskCmd p{};
    p.mode = static_cast<uint8_t>(v.mode);
    p.boundsX = v.boundsHint.x;
    p.boundsY = v.boundsHint.y;
    p.boundsW = v.boundsHint.w;
    p.boundsH = v.boundsHint.h;
    return p;
}

MaskCmd fromPacked(const PackedMaskCmd& v) {
    MaskCmd m{};
    m.mode = static_cast<MaskMode>(v.mode);
    m.boundsHint = Rect{v.boundsX, v.boundsY, v.boundsW, v.boundsH};
    return m;
}

PackedGradientStop toPacked(const GradientStop& v) {
    PackedGradientStop p{};
    p.offset = v.offset;
    p.r = v.color.r;
    p.g = v.color.g;
    p.b = v.color.b;
    p.a = v.color.a;
    return p;
}

GradientStop fromPacked(const PackedGradientStop& v) {
    GradientStop g{};
    g.offset = v.offset;
    g.color = Color{v.r, v.g, v.b, v.a};
    return g;
}

PackedVec2 toPacked(const Vec2& v) {
    PackedVec2 p{};
    p.x = v.x;
    p.y = v.y;
    return p;
}

Vec2 fromPacked(const PackedVec2& v) { return Vec2{v.x, v.y}; }

PackedProjectionCmd toPacked(const ProjectionCmd& v) {
    PackedProjectionCmd p{};
    for (int i = 0; i < 16; ++i) {
        p.viewProj[i] = v.viewProj.m[i];
    }
    packXf(v.rootXf, p.rootXf);
    p.boxX = v.boxRect.x;
    p.boxY = v.boxRect.y;
    p.boxW = v.boxRect.w;
    p.boxH = v.boxRect.h;
    p.depthTest = v.depthTest;
    p.hintX = v.boundsHint.x;
    p.hintY = v.boundsHint.y;
    p.hintW = v.boundsHint.w;
    p.hintH = v.boundsHint.h;
    return p;
}

ProjectionCmd fromPacked(const PackedProjectionCmd& v) {
    ProjectionCmd p{};
    for (int i = 0; i < 16; ++i) {
        p.viewProj.m[i] = v.viewProj[i];
    }
    p.rootXf = unpackXf(v.rootXf);
    p.boxRect = Rect{v.boxX, v.boxY, v.boxW, v.boxH};
    p.depthTest = v.depthTest;
    p.boundsHint = Rect{v.hintX, v.hintY, v.hintW, v.hintH};
    return p;
}

PackedTransform3DCmd toPacked(const Transform3DCmd& v) {
    PackedTransform3DCmd p{};
    for (int i = 0; i < 16; ++i) {
        p.world3[i] = v.world3.m[i];
    }
    return p;
}

Transform3DCmd fromPacked(const PackedTransform3DCmd& v) {
    Transform3DCmd t{};
    for (int i = 0; i < 16; ++i) {
        t.world3.m[i] = v.world3[i];
    }
    return t;
}

PackedImageQuadCmd toPacked(const ImageQuadCmd& v) {
    PackedImageQuadCmd p{};
    p.asset = v.asset;
    p.p0x = v.p[0].x;
    p.p0y = v.p[0].y;
    p.p1x = v.p[1].x;
    p.p1y = v.p[1].y;
    p.p2x = v.p[2].x;
    p.p2y = v.p[2].y;
    p.p3x = v.p[3].x;
    p.p3y = v.p[3].y;
    p.srcX = v.srcUv.x;
    p.srcY = v.srcUv.y;
    p.srcW = v.srcUv.w;
    p.srcH = v.srcUv.h;
    p.opacity = v.opacity;
    p.smooth = v.smooth;
    return p;
}

ImageQuadCmd fromPacked(const PackedImageQuadCmd& v) {
    ImageQuadCmd q{};
    q.asset = v.asset;
    q.p[0] = Vec2{v.p0x, v.p0y};
    q.p[1] = Vec2{v.p1x, v.p1y};
    q.p[2] = Vec2{v.p2x, v.p2y};
    q.p[3] = Vec2{v.p3x, v.p3y};
    q.srcUv = Rect{v.srcX, v.srcY, v.srcW, v.srcH};
    q.opacity = v.opacity;
    q.smooth = v.smooth;
    return q;
}

PackedEffectInstance toPacked(const EffectInstance& v) {
    PackedEffectInstance p{};
    p.kind = static_cast<uint8_t>(v.kind);
    p.enabled = v.enabled;
    p.shader = v.shader;
    for (int i = 0; i < 8; ++i) {
        p.params[i] = v.params[i];
    }
    p.color0r = v.colors[0].r;
    p.color0g = v.colors[0].g;
    p.color0b = v.colors[0].b;
    p.color0a = v.colors[0].a;
    p.color1r = v.colors[1].r;
    p.color1g = v.colors[1].g;
    p.color1b = v.colors[1].b;
    p.color1a = v.colors[1].a;
    p.stopOffset = v.stopOffset;
    p.stopCount = v.stopCount;
    return p;
}

EffectInstance fromPacked(const PackedEffectInstance& v) {
    EffectInstance e{};
    e.kind = static_cast<EffectKind>(v.kind);
    e.enabled = v.enabled;
    e.shader = v.shader;
    for (int i = 0; i < 8; ++i) {
        e.params[i] = v.params[i];
    }
    e.colors[0] = Color{v.color0r, v.color0g, v.color0b, v.color0a};
    e.colors[1] = Color{v.color1r, v.color1g, v.color1b, v.color1a};
    e.stopOffset = v.stopOffset;
    e.stopCount = v.stopCount;
    return e;
}

PackedEffectCmd toPacked(const EffectCmd& v) {
    PackedEffectCmd p{};
    p.effectOffset = v.effectOffset;
    p.effectCount = v.effectCount;
    packXf(v.xf, p.xf);
    p.boundsX = v.boundsHint.x;
    p.boundsY = v.boundsHint.y;
    p.boundsW = v.boundsHint.w;
    p.boundsH = v.boundsHint.h;
    p.contentX = v.contentBounds.x;
    p.contentY = v.contentBounds.y;
    p.contentW = v.contentBounds.w;
    p.contentH = v.contentBounds.h;
    return p;
}

EffectCmd fromPacked(const PackedEffectCmd& v) {
    EffectCmd e{};
    e.effectOffset = v.effectOffset;
    e.effectCount = v.effectCount;
    e.xf = unpackXf(v.xf);
    e.boundsHint = Rect{v.boundsX, v.boundsY, v.boundsW, v.boundsH};
    e.contentBounds = Rect{v.contentX, v.contentY, v.contentW, v.contentH};
    return e;
}

PackedShaderParamDesc toPacked(const ShaderParamDesc& v) {
    PackedShaderParamDesc p{};
    p.name = v.name;
    p.type = v.type;
    for (int i = 0; i < 4; ++i) {
        p.defaultValue[i] = v.defaultValue[i];
    }
    p.minValue = v.minValue;
    p.maxValue = v.maxValue;
    return p;
}

ShaderParamDesc fromPacked(const PackedShaderParamDesc& v) {
    ShaderParamDesc s{};
    s.name = v.name;
    s.type = v.type;
    for (int i = 0; i < 4; ++i) {
        s.defaultValue[i] = v.defaultValue[i];
    }
    s.minValue = v.minValue;
    s.maxValue = v.maxValue;
    return s;
}

PackedShaderRef toPacked(const ShaderRef& v) {
    PackedShaderRef p{};
    p.asset = v.asset;
    p.sourceOffset = v.sourceOffset;
    p.sourceLength = v.sourceLength;
    p.paramOffset = v.paramOffset;
    p.paramCount = v.paramCount;
    p.expandPx = v.expandPx;
    p.fallback = toPacked(v.fallback);
    p.glslOffset = v.glslOffset;
    p.glslLength = v.glslLength;
    p.namesOffset = v.namesOffset;
    p.namesLength = v.namesLength;
    return p;
}

ShaderRef fromPacked(const PackedShaderRef& v) {
    ShaderRef s{};
    s.asset = v.asset;
    s.sourceOffset = v.sourceOffset;
    s.sourceLength = v.sourceLength;
    s.paramOffset = v.paramOffset;
    s.paramCount = v.paramCount;
    s.expandPx = v.expandPx;
    s.fallback = fromPacked(v.fallback);
    s.glslOffset = v.glslOffset;
    s.glslLength = v.glslLength;
    s.namesOffset = v.namesOffset;
    s.namesLength = v.namesLength;
    return s;
}

namespace {

void appendRaw(std::vector<uint8_t>& buf, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), bytes, bytes + size);
}

template <class T>
void appendPod(std::vector<uint8_t>& buf, const T& value) {
    appendRaw(buf, &value, sizeof(T));
}

/// Appends `u32 count` then each item's packed form, via `toPacked` (overload resolution picks
/// the matching one).
template <class T>
void appendPackedVector(std::vector<uint8_t>& buf, const std::vector<T>& items) {
    appendPod(buf, static_cast<uint32_t>(items.size()));
    for (const T& item : items) {
        appendPod(buf, toPacked(item));
    }
}

/// Appends `u32 count` then the raw bytes of `items` (already a fixed-size, padding-free scalar
/// type -- no packed mirror needed).
template <class T>
void appendRawVector(std::vector<uint8_t>& buf, const std::vector<T>& items) {
    appendPod(buf, static_cast<uint32_t>(items.size()));
    if (!items.empty()) {
        appendRaw(buf, items.data(), items.size() * sizeof(T));
    }
}

}  // namespace

BakedWriter::~BakedWriter() { close(); }

bool BakedWriter::open(std::string_view path, uint32_t frameCount, float fps, Rect viewport, float dpr) {
    if (m_file != nullptr) {
        close();
    }
    m_failed = false;
    m_lastError.clear();
    m_frameCount = frameCount;

    std::string pathStr(path);
    std::FILE* file = openFileUtf8(pathStr.c_str(), "wb");  // T-261e: `path` is UTF-8.
    if (file == nullptr) {
        return false;
    }
    m_file = file;

    BakedHeader header{};
    header.magic[0] = 'X';
    header.magic[1] = 'T';
    header.magic[2] = 'C';
    header.magic[3] = 'L';
    header.major = 1;
    header.minor = 4;
    header.frameCount = frameCount;
    header.fps = fps;
    header.width = viewport.w;
    header.height = viewport.h;
    header.dpr = dpr;

    if (std::fwrite(&header, sizeof(header), 1, m_file) != 1) {
        m_failed = true;
        return false;
    }
    return true;
}

bool BakedWriter::writeFrame(uint32_t index, const CommandList& list) {
    if (m_file == nullptr || m_failed) {
        return false;
    }
    // T-255b (A10): BakedPlayer::load rejects a frame whose index is not below the header's
    // frameCount, so the writer never writes one.
    if (index >= m_frameCount) {
        m_failed = true;
        m_lastError = "baked frame index " + std::to_string(index) + " is outside the frame count " +
                      std::to_string(m_frameCount);
        return false;
    }
    // T-263d: the format has no source table and a player's frame names none -- its asset ids are
    // the one IR's the player's host renders it with. A list painted from several Documents
    // cannot be written; one source (a Context with one Document) or none is written as before.
    if (list.sources.size() > 1) {
        m_failed = true;
        m_lastError = "baked frames cannot store a list with more than one source";
        return false;
    }
    // T-263f item 8: the format (minor 4) stores no particles / particleInstances pool and no
    // imageMeshes / meshVertices / meshIndices pool -- every other pool of a CommandList is
    // written below -- so a DrawParticles or DrawImageMesh command would index a pool the player
    // does not have: the frame fails validateCommandList() there and plays back empty. Until the
    // format stores them (a minor version of its own), the frame is refused at the first such
    // command and nothing of it is written.
    for (const Command& command : list.commands) {
        if (command.kind == CmdKind::DrawParticles || command.kind == CmdKind::DrawImageMesh) {
            m_failed = true;
            // T-263g item 11: the command as dumpCommandList() names it.
            m_lastError = std::string("baked frames cannot store ") +
                          (command.kind == CmdKind::DrawParticles ? "DrawParticles" : "DrawImageMesh") +
                          " commands yet";
            return false;
        }
    }

    std::vector<uint8_t> payload;
    appendPackedVector(payload, list.commands);
    appendPackedVector(payload, list.fills);
    appendPackedVector(payload, list.strokes);
    appendPackedVector(payload, list.images);
    // `.xtcl` minor 4 does not keep glyph effects (PackedGlyphsCmd has no `effect` /
    // `effectRadius`); they are added with particles / image meshes in the next minor.
    appendPackedVector(payload, list.glyphRuns);
    appendPackedVector(payload, list.clipRects);
    appendPackedVector(payload, list.clipPaths);
    appendPackedVector(payload, list.layers);
    appendPackedVector(payload, list.masks);
    appendRawVector(payload, list.verbs);
    appendRawVector(payload, list.points);
    appendPackedVector(payload, list.stops);
    appendRawVector(payload, list.glyphs);
    appendPackedVector(payload, list.glyphPos);
    appendRawVector(payload, list.dashes);
    appendPackedVector(payload, list.projections);
    appendPackedVector(payload, list.transforms3);
    appendPackedVector(payload, list.imageQuads);
    appendPackedVector(payload, list.effectCmds);
    appendPackedVector(payload, list.effects);
    appendPackedVector(payload, list.shaders);
    appendPackedVector(payload, list.shaderParams);
    appendRawVector(payload, list.shaderText);

    uint32_t payloadLength = static_cast<uint32_t>(payload.size());
    if (std::fwrite(&index, sizeof(index), 1, m_file) != 1 ||
        std::fwrite(&payloadLength, sizeof(payloadLength), 1, m_file) != 1 ||
        (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), m_file) != payload.size())) {
        m_failed = true;
        return false;
    }
    return true;
}

bool BakedWriter::close() {
    if (m_file == nullptr) {
        return !m_failed;
    }
    bool flushOk = std::fflush(m_file) == 0;
    bool closeOk = std::fclose(m_file) == 0;
    m_file = nullptr;
    bool ok = !m_failed && flushOk && closeOk;
    m_failed = false;
    return ok;
}

const char* BakedWriter::lastError() const { return m_lastError.c_str(); }

}  // namespace xt
