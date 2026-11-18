// T-729 (Docs/design/10-backends.md §10.18 "B 定案", Docs/tasks/T-729.md "測試"): the frame builder's
// side of "a backdrop blur no longer feeds the element's own shadows". A device whose
// supportsBackdropMask() is false (the default) gets the frames of before, byte for byte: the
// snapshot golden/backdrop_mask_legacy.txt was written by the build before T-729 (frame dump, every
// parameter float, vertex and index hashes). No GPU needed: the pixels are checked in
// Tests/wgpu/test_wgpu_backdrop_mask.cpp and Tests/gl/test_gl_backdrop_mask.cpp.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"

using namespace xt;
using namespace xt::gpu;

namespace {

constexpr int kW = 320, kH = 200;

/// Hands out texture handles and cache targets; executes nothing. Reports no backdrop mask.
class LegacyDevice : public GpuDevice {
public:
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return true; }
    bool supportsAlphaChannelReads() const override { return true; }
    bool supportsDirectTarget() const override { return true; }
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}

private:
    TextureHandle m_next = 1;
};

void fillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    const float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    for (int i = 0; i < 4; ++i) {
        cl.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(pts[i * 2]);
        cl.points.push_back(pts[i * 2 + 1]);
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    FillCmd f{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

EffectInstance fx(EffectKind kind, std::initializer_list<float> params, Color color = Color{0, 0, 0, 1}) {
    EffectInstance e{};
    e.kind = kind;
    int i = 0;
    for (float p : params) {
        e.params[i++] = p;
    }
    e.colors[0] = color;
    e.enabled = 1;
    return e;
}

EffectInstance blurFx() { return fx(EffectKind::BackdropBlur, {12, 0.3f, 0.1f}); }
EffectInstance outerFx() { return fx(EffectKind::DropShadow, {0, 6, 12, 0, 1, 0}, Color{0, 0, 0, 0.5f}); }
EffectInstance insetFx() { return fx(EffectKind::DropShadow, {0, 2, 8, 0, 1, 1}, Color{0, 0, 0, 0.4f}); }
EffectInstance spreadFx() { return fx(EffectKind::DropShadow, {3, 3, 6, 2, 1, 0}, Color{0.1f, 0, 0, 0.6f}); }
EffectInstance glowFx(bool inner) {
    return fx(EffectKind::Glow, {10, 1.2f, inner ? 1.0f : 0.0f}, Color{1, 0.8f, 0.3f, 1});
}
EffectInstance adjustFx() { return fx(EffectKind::ColorAdjust, {0.1f, 0, -0.3f, 0, 1}); }

/// A panel: `effects` over a translucent white fill and a translucent triangle.
void panel(CommandList& cl, Rect box, std::initializer_list<EffectInstance> effects) {
    EffectCmd e{};
    e.effectOffset = static_cast<uint32_t>(cl.effects.size());
    for (const EffectInstance& f : effects) {
        cl.effects.push_back(f);
    }
    e.effectCount = static_cast<uint32_t>(effects.size());
    e.xf = Mat2x3{};
    e.boundsHint = Rect{box.x - 30, box.y - 30, box.w + 60, box.h + 60};
    e.contentBounds = box;
    cl.effectCmds.push_back(e);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)});
    fillRect(cl, box.x, box.y, box.w, box.h, Color{1, 1, 1, 0.3f});
    fillRect(cl, box.x + 10, box.y + 10, box.w * 0.5f, box.h * 0.5f, Color{0.2f, 0.9f, 0.6f, 0.6f});
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

/// Two columns of stripes, then the panel(s).
CommandList scene(std::initializer_list<EffectInstance> effects, bool direct = false,
                  bool twoPanels = false) {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    if (direct) {
        cl.hostTargetMode = HostTargetMode::Direct;
    }
    for (int i = 0; i < 10; ++i) {
        fillRect(cl, static_cast<float>(i) * 32.0f, 0, 32, kH,
                 Color{0.1f * static_cast<float>(i), 0.4f, 0.9f - 0.08f * static_cast<float>(i), 1});
    }
    panel(cl, Rect{60, 40, 120, 80}, effects);
    if (twoPanels) {
        panel(cl, Rect{150, 90, 120, 80}, effects);  // overlaps the first: reads its result
    }
    return cl;
}

uint64_t fnv(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

/// Everything a device receives: the structural dump, every parameter float's bits, vertex and index
/// hashes.
std::string fingerprint(const GpuFrame& frame) {
    std::string out;
    dumpGpuFrame(frame, out);
    char buf[64];
    for (size_t i = 0; i < frame.params.size(); ++i) {
        uint32_t bits = 0;
        std::memcpy(&bits, &frame.params[i], sizeof(bits));
        std::snprintf(buf, sizeof(buf), "%s%08x", i % 8 == 0 ? "\nparams " : " ", bits);
        out += buf;
    }
    std::snprintf(
        buf, sizeof(buf), "\nvertices %zu %016llx\n", frame.vertices.size(),
        static_cast<unsigned long long>(fnv(frame.vertices.data(), frame.vertices.size() * sizeof(Vertex))));
    out += buf;
    std::snprintf(buf, sizeof(buf), "indices %zu %016llx\n", frame.indices.size(),
                  static_cast<unsigned long long>(fnv(frame.indices.data(), frame.indices.size() * 4)));
    out += buf;
    return out;
}

std::string buildWith(GpuDevice& device, const CommandList& cl) {
    FrameBuilder builder;
    builder.init(device);
    builder.resize(kW, kH);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    std::string out = fingerprint(frame);
    builder.shutdown();
    return out;
}

struct Named {
    const char* name;
    CommandList cl;
    bool covered;  ///< BackdropBlur followed only by shadow-type effects
};

/// The stacks the new semantics cover (BackdropBlur, then shadow-type effects only) and some it
/// does not.
std::vector<Named> legacyScenes() {
    std::vector<Named> v;
    v.push_back({"blur outer inset", scene({blurFx(), outerFx(), insetFx()}), true});
    v.push_back({"blur outer", scene({blurFx(), outerFx()}), true});
    v.push_back({"blur glow", scene({blurFx(), glowFx(false)}), true});
    v.push_back({"blur inner glow", scene({blurFx(), glowFx(true)}), true});
    v.push_back({"blur spread shadow", scene({blurFx(), spreadFx()}), true});
    v.push_back({"blur outer inset, two overlapping panels",
                 scene({blurFx(), outerFx(), insetFx()}, false, true), true});
    v.push_back(
        {"direct: blur outer inset, two panels", scene({blurFx(), outerFx(), insetFx()}, true, true), true});
    v.push_back({"plain blur", scene({blurFx()}), false});
    v.push_back({"blur adjust", scene({blurFx(), adjustFx()}), false});
    v.push_back({"blur outer adjust", scene({blurFx(), outerFx(), adjustFx()}), false});
    v.push_back({"outer then blur", scene({outerFx(), blurFx()}), false});
    return v;
}

std::string legacyDump(GpuDevice& device) {
    std::string all;
    for (const Named& n : legacyScenes()) {
        all += "== ";
        all += n.name;
        all += "\n";
        all += buildWith(device, n.cl);
    }
    return all;
}

bool updateSnapshots() {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, "XT_GPU_GOLDEN_UPDATE") != 0 || v == nullptr) {
        return false;
    }
    const bool on = v[0] != '\0' && v[0] != '0';
    std::free(v);
    return on;
#else
    const char* v = std::getenv("XT_GPU_GOLDEN_UPDATE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
#endif
}

void checkLegacySnapshot(const std::string& actual) {
    const std::filesystem::path path = std::filesystem::path(XT_GPU_GOLDEN_DIR) / "backdrop_mask_legacy.txt";
    INFO("snapshot: ", path.string());
    if (updateSnapshots()) {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
        out << actual;
        MESSAGE("wrote snapshot ", path.string());
        return;
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string expected = ss.str();
    expected.erase(std::remove(expected.begin(), expected.end(), '\r'), expected.end());
    if (expected != actual) {
        const std::filesystem::path dir(XT_GPU_ACTUAL_DIR);
        std::filesystem::create_directories(dir);
        std::ofstream out(dir / "backdrop_mask_legacy.actual.txt", std::ios::binary);
        out << actual;
    }
    CHECK(expected == actual);
}

}  // namespace

TEST_CASE("gpu backdrop mask: a device without the capability gets the frames of before (T-729)") {
    LegacyDevice device;
    checkLegacySnapshot(legacyDump(device));
}

namespace {

/// LegacyDevice that runs EffectProgram::BackdropMasked.
class MaskDevice : public LegacyDevice {
public:
    bool supportsBackdropMask() const override { return true; }
};

struct Counted {
    size_t passes = 0;
    uint32_t copies = 0, backdrop = 0, masked = 0;
    bool shadowsFirst = true;   ///< every panel's shadow chain ends before its pyramid read
    bool maskIsContent = true;  ///< every BackdropMasked's mask is the panel's content target
};

/// The index of the last pass before `before` that draws `target` (-1: none).
int lastWriter(const GpuFrame& f, TargetHandle target, int before) {
    for (int q = before - 1; q >= 0; --q) {
        if (f.passes[static_cast<size_t>(q)].target == target) {
            return q;
        }
    }
    return -1;
}

/// The effect program of pass `p`'s first command (None when it has none).
EffectProgram programOf(const GpuFrame& f, int p) {
    if (p < 0 || f.passes[static_cast<size_t>(p)].cmdCount == 0) {
        return EffectProgram::None;
    }
    return f.cmds[f.passes[static_cast<size_t>(p)].firstCmd].program;
}

Counted count(GpuDevice& device, const CommandList& cl) {
    FrameBuilder builder;
    builder.init(device);
    builder.resize(kW, kH);
    GpuFrame f;
    builder.build(cl, nullptr, nullptr, nullptr, f);
    Counted n;
    n.passes = f.passes.size();
    for (size_t p = 0; p < f.passes.size(); ++p) {
        const Pass& pass = f.passes[p];
        n.copies += pass.copy != 0 ? 1u : 0u;
        for (uint32_t i = pass.firstCmd; i < pass.firstCmd + pass.cmdCount; ++i) {
            const DrawCmd& c = f.cmds[i];
            if (c.shading != Shading::Effect) {
                continue;
            }
            n.backdrop += c.program == EffectProgram::Backdrop ? 1u : 0u;
            if (c.program != EffectProgram::BackdropMasked) {
                continue;
            }
            ++n.masked;
            const int here = static_cast<int>(p);
            // R' (src) comes from a ShadowGlow pass; B (aux) from the colour adjustment of the pyramid
            // read (blurFx has a saturation); the shadow pass precedes the pyramid read.
            const int shadow = lastWriter(f, c.src, here);
            const int adjust = lastWriter(f, c.aux, here);
            const int read =
                adjust >= 0
                    ? lastWriter(f, f.cmds[f.passes[static_cast<size_t>(adjust)].firstCmd].src, adjust)
                    : -1;
            n.shadowsFirst = n.shadowsFirst && programOf(f, shadow) == EffectProgram::ShadowGlow &&
                             programOf(f, adjust) == EffectProgram::ColorMatrix &&
                             programOf(f, read) == EffectProgram::LevelMix && shadow < read;
            // S (mask): the panel's content target, cleared and drawn into before.
            const TargetHandle mask = backdropMaskTarget(f, c);
            const int content = lastWriter(f, mask, here);
            n.maskIsContent =
                n.maskIsContent && mask != 0 && mask != c.src && mask != c.aux && content >= 0 &&
                f.passes[static_cast<size_t>(content)].cmdCount > 0 &&
                f.cmds[f.passes[static_cast<size_t>(content)].firstCmd].shading != Shading::Effect;
        }
    }
    builder.shutdown();
    return n;
}

}  // namespace

TEST_CASE("gpu backdrop mask: other stacks build the same frames on a device with the capability (T-729)") {
    LegacyDevice legacy;
    MaskDevice mask;
    for (const Named& n : legacyScenes()) {
        if (n.covered) {
            continue;
        }
        CAPTURE(n.name);
        CHECK(buildWith(legacy, n.cl) == buildWith(mask, n.cl));
    }
}

TEST_CASE(
    "gpu backdrop mask: shadows run on the content first, one BackdropMasked composes, no extra pass "
    "(T-729)") {
    LegacyDevice legacy;
    MaskDevice mask;
    struct Case {
        const char* name;
        CommandList cl;
        uint32_t panels;
    };
    const Case cases[] = {
        {"outer inset", scene({blurFx(), outerFx(), insetFx()}), 1},
        {"glow", scene({blurFx(), glowFx(false)}), 1},
        {"inner glow", scene({blurFx(), glowFx(true)}), 1},
        {"spread shadow", scene({blurFx(), spreadFx()}), 1},
        {"two overlapping panels", scene({blurFx(), outerFx(), insetFx()}, false, true), 2},
        {"direct, two panels (shared pyramid)", scene({blurFx(), outerFx(), insetFx()}, true, true), 2},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const Counted before = count(legacy, c.cl);
        const Counted after = count(mask, c.cl);
        CHECK(before.masked == 0u);
        CHECK(before.backdrop == c.panels);
        CHECK(after.backdrop == 0u);
        CHECK(after.masked == c.panels);
        CHECK(after.passes == before.passes);
        CHECK(after.copies == before.copies);
        CHECK(after.shadowsFirst);
        CHECK(after.maskIsContent);
    }
}

TEST_CASE("gpu backdrop mask: an empty backdrop still takes the T-728 shortcut, the result is R' (T-729)") {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    fillRect(cl, 0, 0, kW, kH, Color{0.2f, 0.3f, 0.45f, 1});
    LayerCmd l{};
    l.opacity = 0.95f;
    l.blend = BlendMode::Normal;
    l.boundsHint = Rect{0, 0, kW, kH};
    cl.layers.push_back(l);
    cl.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
    panel(cl, Rect{100, 60, 120, 80}, {blurFx(), outerFx(), insetFx()});
    cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    LegacyDevice legacy;
    MaskDevice mask;
    const Counted n = count(mask, cl);
    CHECK(n.copies == 0u);
    CHECK(n.backdrop == 0u);
    CHECK(n.masked == 0u);
    CHECK(buildWith(legacy, cl) == buildWith(mask, cl));  // the shadows on the content: R' either way
}
